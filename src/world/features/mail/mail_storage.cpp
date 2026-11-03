#include "world/features/mail/mail_repository.hpp"

#include "common/currency.hpp"
#include "persistence/memory_database.hpp"
#include "persistence/postgres/character_rows.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <set>
#include <stdexcept>

namespace chronicles {
namespace {

constexpr const char *kMailColumns =
    "id,account_id,COALESCE(character_id,0) AS character_id,world_id,delivery_key,type,title,text,"
    "sent_epoch_ticks,expiry_epoch_ticks,attachment_expiry_epoch_ticks,confirmed_epoch_ticks,"
    "claimed_epoch_ticks,claimed_character_id,deleted,attachments";

bool owns(const Character &character, std::int64_t account_id, std::int64_t character_id,
          int world_id) {
  return character.id == character_id && character.account_id == account_id &&
         character.world_id == world_id;
}

bool addressed(const MailRecord &mail, std::int64_t account_id, std::int64_t character_id,
               int world_id) {
  return mail.account_id == account_id && mail.world_id == world_id &&
         (mail.character_id == 0 || mail.character_id == character_id);
}

bool same_delivery(const MailRecord &left, const MailRecord &right) {
  return left.account_id == right.account_id && left.character_id == right.character_id &&
         left.world_id == right.world_id && left.delivery_key == right.delivery_key &&
         left.type == right.type && left.title == right.title && left.text == right.text &&
         left.attachments == right.attachments &&
         left.expiry_epoch_ticks - left.sent_epoch_ticks ==
             right.expiry_epoch_ticks - right.sent_epoch_ticks &&
         left.attachment_expiry_epoch_ticks - left.sent_epoch_ticks ==
             right.attachment_expiry_epoch_ticks - right.sent_epoch_ticks;
}

bool removable(const MailRecord &mail, std::int64_t now) {
  return mail.attachments.empty() || mail.claimed_epoch_ticks != 0 ||
         now >= mail.attachment_expiry_epoch_ticks;
}

bool valid_batch(const std::vector<std::int64_t> &ids, std::int64_t now) {
  if (now <= 0 || ids.empty() || ids.size() > kMaximumMailBatch) {
    return false;
  }
  const std::set<std::int64_t> unique(ids.begin(), ids.end());
  return unique.size() == ids.size() && *unique.begin() > 0;
}

void validate_grants(const std::vector<CharacterItem> &items) {
  if (items.size() > 64) {
    throw std::invalid_argument("Mail grants too many equipment instances");
  }
  std::set<int> slots;
  for (const auto &item : items) {
    validate_character_item(item);
    if (item.id != 0 || item.equip_slot || item.quantity != 1 ||
        !slots.insert(item.inventory_slot).second) {
      throw std::invalid_argument("Invalid new mail equipment instance");
    }
  }
}

void validate_claim_delta(const MailRecord &mail, const WorldProgress &before,
                          const WorldProgress &after,
                          const std::vector<CharacterItem> &granted_items) {
  auto expected = before;
  ++expected.revision;
  std::vector<bool> consumed(granted_items.size());
  for (const auto &attachment : mail.attachments) {
    if (attachment.count <= 0) {
      throw std::invalid_argument("Stored mail has an invalid attachment quantity");
    }
    if (attachment.kind == MailAttachmentKind::money) {
      if (attachment.cms_id != 0 && attachment.cms_id != 1) {
        throw std::invalid_argument("Stored mail has an invalid currency");
      }
      auto &balance = attachment.cms_id == 0 ? expected.gold : expected.hersander_token;
      if (balance > kMoneyLimit - attachment.count) {
        throw std::invalid_argument("Mail claim exceeds currency limit");
      }
      balance += attachment.count;
      continue;
    }
    int instances{};
    for (std::size_t index = 0; index < granted_items.size() && instances < attachment.count;
         ++index) {
      const auto &item = granted_items[index];
      if (!consumed[index] && item.cms_id == attachment.cms_id &&
          (!attachment.grade || item.grade == *attachment.grade)) {
        consumed[index] = true;
        ++instances;
      }
    }
    if (instances == attachment.count) {
      continue;
    }
    if (instances != 0 || attachment.grade) {
      throw std::invalid_argument("Mail equipment grant does not match its attachment");
    }
    const auto stack = after.stackables.find(attachment.cms_id);
    const auto housing = after.housing_items.find(attachment.cms_id);
    if (stack != after.stackables.end() &&
        (!before.stackables.contains(attachment.cms_id) ||
         stack->second != before.stackables.at(attachment.cms_id))) {
      auto &quantity = expected.stackables[attachment.cms_id];
      if (quantity > std::numeric_limits<int>::max() - attachment.count) {
        throw std::invalid_argument("Mail stackable quantity overflows");
      }
      quantity += attachment.count;
    } else if (housing != after.housing_items.end()) {
      auto &quantity = expected.housing_items[attachment.cms_id].quantity;
      if (quantity > std::numeric_limits<int>::max() - attachment.count) {
        throw std::invalid_argument("Mail housing quantity overflows");
      }
      quantity += attachment.count;
    } else {
      throw std::invalid_argument("Mail item grant does not match its attachment");
    }
  }
  if (std::ranges::any_of(consumed, [](bool value) { return !value; }) || expected != after) {
    throw std::invalid_argument("Mail claim contains unrelated progress or item changes");
  }
}

MailClaimStatus eligibility(const MailRecord &mail, const MailRecord &expected, std::int64_t now) {
  if (mail.deleted || now < mail.sent_epoch_ticks) {
    return MailClaimStatus::not_found;
  }
  if (mail.claimed_epoch_ticks != 0) {
    return MailClaimStatus::already_claimed;
  }
  if (now >= mail.expiry_epoch_ticks || now >= mail.attachment_expiry_epoch_ticks) {
    return MailClaimStatus::expired;
  }
  if (!same_delivery(mail, expected) || mail.sent_epoch_ticks != expected.sent_epoch_ticks) {
    return MailClaimStatus::conflict;
  }
  return MailClaimStatus::claimed;
}

nlohmann::json attachments_json(const std::vector<MailAttachment> &attachments) {
  auto result = nlohmann::json::array();
  for (const auto &attachment : attachments) {
    nlohmann::json value{{"kind", static_cast<int>(attachment.kind)},
                         {"cms_id", attachment.cms_id},
                         {"count", attachment.count}};
    if (attachment.grade) {
      value["grade"] = *attachment.grade;
    }
    result.push_back(std::move(value));
  }
  return result;
}

MailRecord read_mail(const pqxx::row &row) {
  MailRecord mail;
  mail.id = row["id"].as<std::int64_t>();
  mail.account_id = row["account_id"].as<std::int64_t>();
  mail.character_id = row["character_id"].as<std::int64_t>();
  mail.world_id = row["world_id"].as<int>();
  mail.delivery_key = row["delivery_key"].as<std::string>();
  mail.type = row["type"].as<int>();
  mail.title = row["title"].as<std::string>();
  mail.text = row["text"].as<std::string>();
  mail.sent_epoch_ticks = row["sent_epoch_ticks"].as<std::int64_t>();
  mail.expiry_epoch_ticks = row["expiry_epoch_ticks"].as<std::int64_t>();
  mail.attachment_expiry_epoch_ticks = row["attachment_expiry_epoch_ticks"].as<std::int64_t>();
  mail.confirmed_epoch_ticks = row["confirmed_epoch_ticks"].as<std::int64_t>();
  mail.claimed_epoch_ticks = row["claimed_epoch_ticks"].as<std::int64_t>();
  mail.claimed_character_id = row["claimed_character_id"].as<std::int64_t>();
  mail.deleted = row["deleted"].as<bool>();
  const auto attachments = nlohmann::json::parse(row["attachments"].as<std::string>());
  if (!attachments.is_array() || attachments.size() > kMaximumMailAttachments) {
    throw std::runtime_error("Stored mail attachment collection is invalid");
  }
  for (const auto &value : attachments) {
    MailAttachment attachment;
    const auto kind = value.at("kind").get<int>();
    if (kind < 0 || kind > 1) {
      throw std::runtime_error("Stored mail attachment kind is invalid");
    }
    attachment.kind = static_cast<MailAttachmentKind>(kind);
    attachment.cms_id = value.at("cms_id").get<int>();
    attachment.count = value.at("count").get<int>();
    if (value.contains("grade")) {
      attachment.grade = value.at("grade").get<int>();
    }
    mail.attachments.push_back(attachment);
  }
  return mail;
}

bool owned_character(pqxx::transaction_base &transaction, std::int64_t account_id,
                     std::int64_t character_id, int world_id) {
  return !transaction
              .exec("SELECT id FROM emulator_characters "
                    "WHERE account_id=$1 AND id=$2 AND world_id=$3",
                    pqxx::params{account_id, character_id, world_id})
              .empty();
}

} // namespace

MemoryMailRepository::MemoryMailRepository(std::shared_ptr<MemoryDatabase> database)
    : database_(std::move(database)) {
  // Reproduce the former in-memory deletion cleanup: the character's recipient mail
  // follows its removal. The callback runs with database_->mutex held.
  MemoryDatabase::CharacterDeletionHook hook;
  hook.erase_rows = [this](std::int64_t character_id) {
    // Match PostgreSQL's recipient FK cascade. Account mail (character_id == 0) and
    // its claim receipts survive even when the character that claimed it is deleted.
    std::erase_if(mails_, [character_id](const auto &entry) {
      return entry.second.character_id == character_id;
    });
  };
  std::lock_guard lock(database_->mutex);
  deletion_hook_index_ = database_->deletion_hooks.size();
  database_->deletion_hooks.push_back(std::move(hook));
}

MemoryMailRepository::~MemoryMailRepository() {
  // The database can outlive this repository through the shared accessor; neutralize the
  // hook so its callback never touches the mail table after it is gone.
  std::lock_guard lock(database_->mutex);
  if (deletion_hook_index_ < database_->deletion_hooks.size()) {
    database_->deletion_hooks[deletion_hook_index_].erase_rows = nullptr;
  }
}

std::optional<MailRecord> MemoryMailRepository::deliver_mail(MailRecord mail) {
  validate_mail_delivery(mail);
  std::lock_guard lock(database_->mutex);
  const bool account_missing = std::ranges::none_of(
      database_->accounts, [&](const Account &account) { return account.id == mail.account_id; });
  const bool character_missing =
      mail.character_id != 0 &&
      std::ranges::none_of(database_->characters, [&](const Character &character) {
        return owns(character, mail.account_id, mail.character_id, mail.world_id);
      });
  if (account_missing || character_missing) {
    return std::nullopt;
  }
  std::size_t live_count{};
  for (const auto &[id, existing] : mails_) {
    if (existing.account_id != mail.account_id || existing.world_id != mail.world_id) {
      continue;
    }
    if (existing.delivery_key == mail.delivery_key) {
      if (!same_delivery(existing, mail)) {
        throw std::invalid_argument("Mail delivery key was reused with different content");
      }
      return existing;
    }
    if (!existing.deleted && existing.expiry_epoch_ticks > mail.sent_epoch_ticks) {
      ++live_count;
    }
  }
  if (live_count >= kMaximumMailboxEntries) {
    return std::nullopt;
  }
  mail.id = next_mail_id_++;
  mails_.emplace(mail.id, mail);
  return mail;
}

std::vector<MailRecord>
MemoryMailRepository::mailbox(std::int64_t account_id, std::int64_t character_id, int world_id,
                              std::int64_t now_epoch_ticks) {
  std::lock_guard lock(database_->mutex);
  if (std::ranges::none_of(database_->characters, [&](const Character &character) {
        return owns(character, account_id, character_id, world_id);
      })) {
    return {};
  }
  std::vector<MailRecord> result;
  for (auto entry = mails_.rbegin(); entry != mails_.rend(); ++entry) {
    const auto &mail = entry->second;
    if (addressed(mail, account_id, character_id, world_id) && !mail.deleted &&
        mail.sent_epoch_ticks <= now_epoch_ticks && mail.expiry_epoch_ticks > now_epoch_ticks) {
      result.push_back(mail);
    }
  }
  return result;
}

bool MemoryMailRepository::change_mail(std::int64_t account_id, std::int64_t character_id,
                                       int world_id,
                                       const std::vector<std::int64_t> &mail_ids, bool remove,
                                       std::int64_t now_epoch_ticks) {
  if (!valid_batch(mail_ids, now_epoch_ticks)) {
    return false;
  }
  std::lock_guard lock(database_->mutex);
  if (std::ranges::none_of(database_->characters, [&](const Character &character) {
        return owns(character, account_id, character_id, world_id);
      })) {
    return false;
  }
  for (const auto id : mail_ids) {
    const auto found = mails_.find(id);
    if (found == mails_.end() || !addressed(found->second, account_id, character_id, world_id) ||
        (remove && !removable(found->second, now_epoch_ticks))) {
      return false;
    }
  }
  for (const auto id : mail_ids) {
    auto &mail = mails_.at(id);
    if (remove) {
      mail.deleted = true;
    } else if (mail.confirmed_epoch_ticks == 0) {
      mail.confirmed_epoch_ticks = now_epoch_ticks;
    }
  }
  return true;
}

MailClaimStatus MemoryMailRepository::claim_mail(std::int64_t account_id, std::int64_t character_id,
                                        int world_id, const MailRecord &expected_mail,
                                        std::int64_t now_epoch_ticks, const WorldProgress &before,
                                        const WorldProgress &after,
                                        std::vector<CharacterItem> &granted_items) {
  std::lock_guard lock(database_->mutex);
  const auto character =
      detail::find_owned_character(*database_, account_id, character_id, world_id);
  const auto found = mails_.find(expected_mail.id);
  if (character == database_->characters.end() || found == mails_.end() ||
      !addressed(found->second, account_id, character_id, world_id)) {
    return MailClaimStatus::not_found;
  }
  const auto status = eligibility(found->second, expected_mail, now_epoch_ticks);
  if (status != MailClaimStatus::claimed) {
    return status;
  }
  detail::validate_world_progress_transition(before, after);
  validate_grants(granted_items);
  validate_claim_delta(found->second, before, after, granted_items);
  auto &stored_progress = database_->world_progress[character_id];
  if (stored_progress.revision != before.revision || stored_progress.dead != before.dead) {
    return MailClaimStatus::conflict;
  }
  auto next_items = character->items;
  auto next_item_id = database_->next_item_id;
  auto committed = granted_items;
  for (auto &item : committed) {
    if (std::ranges::any_of(next_items, [&](const CharacterItem &existing) {
          return existing.inventory_slot == item.inventory_slot;
        })) {
      return MailClaimStatus::conflict;
    }
    item.id = next_item_id++;
    next_items.push_back(item);
  }
  detail::commit_progress_and_items(*database_, *character, after, std::move(next_items),
                                    next_item_id);
  found->second.claimed_epoch_ticks = now_epoch_ticks;
  found->second.claimed_character_id = character_id;
  granted_items = std::move(committed);
  return MailClaimStatus::claimed;
}

std::optional<MailRecord> PostgresMailRepository::deliver_mail(MailRecord mail) {
  validate_mail_delivery(mail);
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  // Serialize delivery keys and mailbox capacity per recipient account, across processes.
  if (transaction
          .exec("SELECT id FROM emulator_accounts WHERE id=$1 FOR UPDATE",
                pqxx::params{mail.account_id})
          .empty() ||
      (mail.character_id != 0 &&
       !owned_character(transaction, mail.account_id, mail.character_id, mail.world_id))) {
    return std::nullopt;
  }
  const auto existing = transaction.exec(
      std::string("SELECT ") + kMailColumns +
          " FROM emulator_mail WHERE account_id=$1 AND world_id=$2 AND delivery_key=$3",
      pqxx::params{mail.account_id, mail.world_id, mail.delivery_key});
  if (!existing.empty()) {
    auto result = read_mail(existing.front());
    if (!same_delivery(result, mail)) {
      throw std::invalid_argument("Mail delivery key was reused with different content");
    }
    return result;
  }
  const auto count = transaction
                         .exec("SELECT COUNT(*) FROM emulator_mail WHERE account_id=$1 "
                               "AND world_id=$2 AND NOT deleted AND expiry_epoch_ticks>$3",
                               pqxx::params{mail.account_id, mail.world_id, mail.sent_epoch_ticks})
                         .front()[0]
                         .as<int>();
  if (count >= static_cast<int>(kMaximumMailboxEntries)) {
    return std::nullopt;
  }
  const auto result = transaction.exec(
      "INSERT INTO emulator_mail(account_id,character_id,world_id,delivery_key,type,title,text,"
      "sent_epoch_ticks,expiry_epoch_ticks,attachment_expiry_epoch_ticks,attachments) "
      "VALUES($1,NULLIF($2,0),$3,$4,$5,$6,$7,$8,$9,$10,$11::jsonb) RETURNING id",
      pqxx::params{mail.account_id, mail.character_id, mail.world_id, mail.delivery_key, mail.type,
                   mail.title, mail.text, mail.sent_epoch_ticks, mail.expiry_epoch_ticks,
                   mail.attachment_expiry_epoch_ticks, attachments_json(mail.attachments).dump()});
  mail.id = result.front()[0].as<std::int64_t>();
  transaction.commit();
  return mail;
}

std::vector<MailRecord>
PostgresMailRepository::mailbox(std::int64_t account_id, std::int64_t character_id, int world_id,
                                std::int64_t now_epoch_ticks) {
  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  if (!owned_character(transaction, account_id, character_id, world_id)) {
    return {};
  }
  const auto rows = transaction.exec(
      std::string("SELECT ") + kMailColumns +
          " FROM emulator_mail WHERE account_id=$1 AND (character_id IS NULL OR character_id=$2) "
          "AND world_id=$3 AND NOT deleted AND sent_epoch_ticks<=$4 AND expiry_epoch_ticks>$4 "
          "ORDER BY id DESC LIMIT 1000",
      pqxx::params{account_id, character_id, world_id, now_epoch_ticks});
  std::vector<MailRecord> result;
  for (const auto &row : rows) {
    result.push_back(read_mail(row));
  }
  return result;
}

bool PostgresMailRepository::change_mail(std::int64_t account_id, std::int64_t character_id,
                                         int world_id,
                                         const std::vector<std::int64_t> &mail_ids, bool remove,
                                         std::int64_t now_epoch_ticks) {
  if (!valid_batch(mail_ids, now_epoch_ticks)) {
    return false;
  }
  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  if (!owned_character(transaction, account_id, character_id, world_id)) {
    return false;
  }
  // Stable lock order prevents two overlapping batches from deadlocking.
  auto sorted_ids = mail_ids;
  std::ranges::sort(sorted_ids);
  for (const auto id : sorted_ids) {
    const auto row = transaction.exec(std::string("SELECT ") + kMailColumns +
                                          " FROM emulator_mail WHERE id=$1 FOR UPDATE",
                                      pqxx::params{id});
    if (row.empty()) {
      return false;
    }
    const auto mail = read_mail(row.front());
    if (!addressed(mail, account_id, character_id, world_id) ||
        (remove && !removable(mail, now_epoch_ticks))) {
      return false;
    }
  }
  for (const auto id : sorted_ids) {
    if (remove) {
      transaction.exec("UPDATE emulator_mail SET deleted=true WHERE id=$1", pqxx::params{id});
    } else {
      transaction.exec("UPDATE emulator_mail SET confirmed_epoch_ticks=$2 "
                       "WHERE id=$1 AND confirmed_epoch_ticks=0",
                       pqxx::params{id, now_epoch_ticks});
    }
  }
  transaction.commit();
  return true;
}

MailClaimStatus
PostgresMailRepository::claim_mail(std::int64_t account_id, std::int64_t character_id,
                                   int world_id, const MailRecord &expected_mail,
                                   std::int64_t now_epoch_ticks, const WorldProgress &before,
                                   const WorldProgress &after,
                                   std::vector<CharacterItem> &granted_items) {
  try {
    pqxx::connection connection(connection_);
    pqxx::work transaction(connection);
    // All gameplay transactions lock the character before dependent rows.
    if (transaction
            .exec("SELECT id FROM emulator_characters WHERE account_id=$1 "
                  "AND id=$2 AND world_id=$3 FOR UPDATE",
                  pqxx::params{account_id, character_id, world_id})
            .empty()) {
      return MailClaimStatus::not_found;
    }
    const auto rows = transaction.exec(std::string("SELECT ") + kMailColumns +
                                           " FROM emulator_mail WHERE id=$1 FOR UPDATE",
                                       pqxx::params{expected_mail.id});
    if (rows.empty()) {
      return MailClaimStatus::not_found;
    }
    const auto mail = read_mail(rows.front());
    if (!addressed(mail, account_id, character_id, world_id)) {
      return MailClaimStatus::not_found;
    }
    const auto status = eligibility(mail, expected_mail, now_epoch_ticks);
    if (status != MailClaimStatus::claimed) {
      return status;
    }
    detail::validate_world_progress_transition(before, after);
    validate_grants(granted_items);
    validate_claim_delta(mail, before, after, granted_items);
    if (!detail::write_world_progress_transition(transaction, account_id, character_id, world_id,
                                                 before, after)) {
      return MailClaimStatus::conflict;
    }
    auto committed = granted_items;
    for (auto &item : committed) {
      const auto inserted = transaction.exec(
          "INSERT INTO emulator_character_items(character_id,item_cms_id,inventory_slot,"
          "equip_slot,quantity,durability,grade,created_epoch_ticks,polish_step) "
          "VALUES($1,$2,$3,NULL,$4,$5,$6,$7,$8) RETURNING id",
          pqxx::params{character_id, item.cms_id, item.inventory_slot, item.quantity,
                       item.durability, item.grade, item.created_epoch_ticks, item.polish_step});
      item.id = inserted.front()[0].as<std::int64_t>();
    }
    transaction.exec("UPDATE emulator_mail SET claimed_epoch_ticks=$2,claimed_character_id=$3 "
                     "WHERE id=$1",
                     pqxx::params{mail.id, now_epoch_ticks, character_id});
    transaction.commit();
    granted_items = std::move(committed);
    return MailClaimStatus::claimed;
  } catch (const pqxx::unique_violation &) {
    return MailClaimStatus::conflict;
  }
}

} // namespace chronicles
