#include "world/features/mail/mail.hpp"

#include "common/currency.hpp"
#include "persistence/item_wire.hpp"
#include "data/shop_catalog.hpp"
#include "world/core/progress_packets.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace chronicles {
namespace {

constexpr std::int64_t kEpochTicksPerSecond = 10'000'000;
constexpr std::int64_t kMaximumRetentionSeconds = 365 * 24 * 60 * 60;

void validate_keys(const nlohmann::json &object, const std::set<std::string> &allowed) {
  if (!object.is_object()) {
    throw std::invalid_argument("Mail input must be an object");
  }
  for (const auto &[key, value] : object.items()) {
    if (!allowed.contains(key)) {
      throw std::invalid_argument("Unknown mail field: " + key);
    }
  }
}

std::int64_t integer(const nlohmann::json &object, const char *key, std::int64_t minimum,
                     std::int64_t maximum) {
  const auto &value = object.at(key);
  if (!value.is_number_integer() || value.is_boolean()) {
    throw std::invalid_argument(std::string("Mail field must be an integer: ") + key);
  }
  if (value.is_number_unsigned() &&
      value.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum)) {
    throw std::invalid_argument(std::string("Mail field is out of range: ") + key);
  }
  const auto parsed = value.get<std::int64_t>();
  if (parsed < minimum || parsed > maximum) {
    throw std::invalid_argument(std::string("Mail field is out of range: ") + key);
  }
  return parsed;
}

} // namespace

void validate_mail_catalog(const MailRecord &mail, const ShopCatalog &catalog) {
  validate_mail_delivery(mail);
  std::size_t instance_count{};
  for (const auto &attachment : mail.attachments) {
    if (attachment.kind == MailAttachmentKind::money) {
      continue;
    }
    const auto *definition = catalog.item(attachment.cms_id);
    if (definition == nullptr || definition->storage == ShopItemStorage::unsupported ||
        attachment.count > catalog.maximum_stack()) {
      throw std::invalid_argument("Unsupported mail item or attachment quantity");
    }
    if (definition->storage != ShopItemStorage::instance && attachment.grade) {
      throw std::invalid_argument("Only equipment attachments may override their grade");
    }
    if (definition->storage == ShopItemStorage::instance) {
      instance_count += static_cast<std::size_t>(attachment.count);
    }
  }
  if (instance_count > 64) {
    throw std::invalid_argument("One mail can grant at most 64 equipment instances");
  }
}

MailClaimPlan plan_mail_claim(const MailRecord &mail, const Character &character,
                              const WorldProgress &progress, const ShopCatalog &catalog,
                              std::int64_t now_epoch_ticks) {
  // Validation of immutable delivery content also applies to loaded mail. Its state fields
  // can already contain a read receipt, which is unrelated to attachment eligibility.
  auto delivery = mail;
  delivery.confirmed_epoch_ticks = 0;
  delivery.claimed_epoch_ticks = 0;
  delivery.claimed_character_id = 0;
  delivery.deleted = false;
  validate_mail_catalog(delivery, catalog);
  if (character.account_id != mail.account_id || character.world_id != mail.world_id ||
      (mail.character_id != 0 && mail.character_id != character.id) || mail.deleted ||
      mail.claimed_epoch_ticks != 0 || now_epoch_ticks < mail.sent_epoch_ticks ||
      now_epoch_ticks >= mail.expiry_epoch_ticks ||
      now_epoch_ticks >= mail.attachment_expiry_epoch_ticks ||
      progress.revision == std::numeric_limits<std::int64_t>::max()) {
    throw std::invalid_argument("Mail is not available for this character");
  }

  MailClaimPlan plan;
  plan.before = progress;
  plan.after = progress;
  ++plan.after.revision;
  std::set<int> occupied_slots;
  for (const auto &item : character.items) {
    occupied_slots.insert(item.inventory_slot);
  }
  for (const auto &attachment : mail.attachments) {
    if (attachment.kind == MailAttachmentKind::money) {
      auto &balance = attachment.cms_id == 0 ? plan.after.gold : plan.after.hersander_token;
      if (balance > kMoneyLimit - attachment.count) {
        throw std::invalid_argument("Mail would exceed the currency balance limit");
      }
      balance += attachment.count;
      continue;
    }
    const auto &definition = *catalog.item(attachment.cms_id);
    if (definition.storage == ShopItemStorage::stackable) {
      if (!plan.after.stackables.contains(attachment.cms_id) &&
          plan.after.stackables.size() >= kMaximumStackableKinds) {
        throw std::invalid_argument("Inventory has no free stackable kind");
      }
      auto &count = plan.after.stackables[attachment.cms_id];
      if (count > catalog.maximum_stack() - attachment.count) {
        throw std::invalid_argument("Mail would exceed the item stack limit");
      }
      count += attachment.count;
    } else if (definition.storage == ShopItemStorage::housing) {
      if (!plan.after.housing_items.contains(attachment.cms_id) &&
          plan.after.housing_items.size() >= kMaximumHousingItemKinds) {
        throw std::invalid_argument("Housing inventory has no free item kind");
      }
      auto &count = plan.after.housing_items[attachment.cms_id].quantity;
      if (count > std::numeric_limits<int>::max() - attachment.count) {
        throw std::invalid_argument("Mail would overflow housing item quantity");
      }
      count += attachment.count;
    } else {
      for (int index = 0; index < attachment.count; ++index) {
        int slot{};
        while (slot <= kMaximumInventorySlot && occupied_slots.contains(slot)) {
          ++slot;
        }
        if (slot > kMaximumInventorySlot) {
          throw std::invalid_argument("Inventory has no free equipment slot");
        }
        occupied_slots.insert(slot);
        CharacterItem item;
        item.cms_id = attachment.cms_id;
        item.inventory_slot = slot;
        item.durability = definition.max_durability;
        item.grade = attachment.grade.value_or(definition.grade);
        item.created_epoch_ticks = now_epoch_ticks;
        plan.granted_items.push_back(item);
      }
    }
  }
  return plan;
}

nlohmann::json mail_content_json(const MailRecord &mail) {
  nlohmann::json content{{"id", std::to_string(mail.id)},
                         {"type", mail.type},
                         {"title", mail.title},
                         {"text", mail.text},
                         {"sent_epochtime", std::to_string(mail.sent_epoch_ticks)},
                         {"expiry_epochtime", std::to_string(mail.expiry_epoch_ticks)},
                         {"attachments", nlohmann::json::array()}};
  if (mail.confirmed_epoch_ticks != 0) {
    content["confirm_epochtime"] = std::to_string(mail.confirmed_epoch_ticks);
  }
  if (mail.claimed_epoch_ticks != 0) {
    content["attachment_completion_epochtime"] = std::to_string(mail.claimed_epoch_ticks);
  }
  content["attachment_expiry_epochtime"] = std::to_string(mail.attachment_expiry_epoch_ticks);
  for (const auto &attachment : mail.attachments) {
    nlohmann::json value;
    if (attachment.kind == MailAttachmentKind::money) {
      value["money"] = {{"type", attachment.cms_id}, {"amount", attachment.count}};
    } else {
      value["item"] = {{"cms_id", attachment.cms_id}, {"count", attachment.count}};
      if (attachment.grade) {
        value["item"]["grade"] = *attachment.grade;
      }
    }
    content["attachments"].push_back(std::move(value));
  }
  return content;
}

std::vector<GameplayPacket> mail_claim_packets(const MailRecord &mail, const MailClaimPlan &plan) {
  using Json = nlohmann::json;
  const auto source_field = mail.character_id == 0 ? "account_mail_id" : "char_mail_id";
  Json update{{"system", {{source_field, std::to_string(mail.id)}}},
              {"packet_seq", Json::object()}};
  for (const auto &item : plan.granted_items) {
    if (item.id <= 0) {
      throw std::invalid_argument("Mail packet requires committed item identities");
    }
    update["items"].push_back(item_instance_json(item));
  }
  for (const auto &[cms_id, quantity] : plan.after.stackables) {
    const auto previous = plan.before.stackables.find(cms_id);
    if (previous == plan.before.stackables.end() || previous->second != quantity) {
      update["stackables"].push_back({{"item_cms_id", cms_id}, {"quantity", quantity}});
    }
  }
  for (const auto &[cms_id, record] : plan.after.housing_items) {
    const auto previous = plan.before.housing_items.find(cms_id);
    if (previous == plan.before.housing_items.end() || previous->second != record) {
      update["housing_items"].push_back(housing_item_count_json(cms_id, record));
    }
  }
  const auto money_change = [&](int type, std::int64_t before, std::int64_t after) {
    if (after != before) {
      update["money_changes"].push_back({{"money_type", type},
                                         {"change", std::to_string(after - before)},
                                         {"result", std::to_string(after)}});
    }
  };
  money_change(0, plan.before.gold, plan.after.gold);
  money_change(1, plan.before.hersander_token, plan.after.hersander_token);
  return {{169, std::move(update)}};
}

MailRecord parse_mail_delivery(const nlohmann::json &request, std::int64_t now_epoch_ticks) {
  validate_keys(request,
                {"account_id", "character_id", "world_id", "delivery_key", "type", "title", "text",
                 "retention_seconds", "attachment_retention_seconds", "attachments"});
  MailRecord mail;
  mail.account_id = integer(request, "account_id", 1, std::numeric_limits<std::int64_t>::max());
  if (request.contains("character_id")) {
    mail.character_id =
        integer(request, "character_id", 0, std::numeric_limits<std::int64_t>::max());
  }
  mail.world_id =
      static_cast<int>(integer(request, "world_id", 1, std::numeric_limits<int>::max()));
  mail.delivery_key = request.at("delivery_key").get<std::string>();
  mail.title = request.at("title").get<std::string>();
  mail.text = request.value("text", std::string{});
  if (request.contains("type")) {
    mail.type = static_cast<int>(integer(request, "type", 0, 1));
  }
  const auto retention = integer(request, "retention_seconds", 1, kMaximumRetentionSeconds);
  const auto attachment_retention =
      request.contains("attachment_retention_seconds")
          ? integer(request, "attachment_retention_seconds", 1, retention)
          : retention;
  if (now_epoch_ticks <= 0 || now_epoch_ticks > std::numeric_limits<std::int64_t>::max() -
                                                    retention * kEpochTicksPerSecond) {
    throw std::invalid_argument("Mail timestamp is out of range");
  }
  mail.sent_epoch_ticks = now_epoch_ticks;
  mail.expiry_epoch_ticks = now_epoch_ticks + retention * kEpochTicksPerSecond;
  mail.attachment_expiry_epoch_ticks =
      now_epoch_ticks + attachment_retention * kEpochTicksPerSecond;
  const auto attachments = request.value("attachments", nlohmann::json::array());
  if (!attachments.is_array() || attachments.size() > kMaximumMailAttachments) {
    throw std::invalid_argument("Too many mail attachments");
  }
  for (const auto &value : attachments) {
    validate_keys(value, {"kind", "cms_id", "count", "grade"});
    MailAttachment attachment;
    const auto kind = value.at("kind").get<std::string>();
    if (kind == "money") {
      attachment.kind = MailAttachmentKind::money;
    } else if (kind != "item") {
      throw std::invalid_argument("Mail attachment kind must be item or money");
    }
    const int minimum_id = attachment.kind == MailAttachmentKind::money ? 0 : 1;
    attachment.cms_id =
        static_cast<int>(integer(value, "cms_id", minimum_id, std::numeric_limits<int>::max()));
    attachment.count =
        static_cast<int>(integer(value, "count", 1, std::numeric_limits<int>::max()));
    if (value.contains("grade")) {
      attachment.grade = static_cast<int>(integer(value, "grade", 0, 5));
    }
    mail.attachments.push_back(attachment);
  }
  validate_mail_delivery(mail);
  return mail;
}

} // namespace chronicles
