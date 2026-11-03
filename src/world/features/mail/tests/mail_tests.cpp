#include "world/features/mail/mail.hpp"
#include "world/features/mail/mail_repository.hpp"
#include "world/features/mail/mail_rest.hpp"
#include "protocol/protocol.hpp"
#include "data/shop_catalog.hpp"
#include "persistence/memory_database.hpp"
#include "persistence/memory_repositories.hpp"
#include "persistence/postgres_repositories.hpp"
#include "testing/memory_repositories.hpp"

#include "p_c2f.rest.pb.h"
#include "p_f2c.rest.pb.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>

namespace chronicles {
namespace {

constexpr std::int64_t kNow = 1'000'000'000;
constexpr int kMaterial = 701;
constexpr int kEquipment = 702;
constexpr int kHousing = 703;

ShopCatalog catalog() {
  ShopCatalogTables tables;
  tables.items = {
      {{"id", "701"}, {"category", "CraftMaterial"}, {"grade", "Normal"}},
      {{"id", "702"}, {"category", "Equipment"}, {"grade", "Rare"}, {"maxDurability", "10000"}},
      {{"id", "703"}, {"category", "HousingObject"}, {"grade", "Normal"}}};
  tables.game_variables = {{{"name", "ItemMaxStack"}, {"value", "999"}},
                           {{"name", "ResetBaseDateTime"}, {"value", "2025-01-01 00:00:00"}}};
  tables.money = {{{"type", "Gold"}, {"limitUserMax", "9000000000000000"}}};
  return ShopCatalog(tables);
}

Character create_owner(const CharacterRepositories &store, const std::string &suffix,
                       int slot = 0) {
  const auto account_name = "mail" + suffix;
  if (!store.accounts->account(account_name)) {
    Account account;
    account.name = account_name;
    account.salt = "test-salt";
    account.hash = "test-hash";
    if (!store.accounts->add_account(account)) {
      throw std::runtime_error("Could not create mail test account");
    }
  }
  Character character;
  character.account_id = store.accounts->account(account_name)->id;
  character.name =
      "Ml" + suffix.substr(suffix.size() > 8 ? suffix.size() - 8 : 0) + std::to_string(slot);
  character.slot = slot;
  return store.characters->create(character);
}

MailRecord delivery(const Character &character, const std::string &key = "reward") {
  MailRecord mail;
  mail.account_id = character.account_id;
  mail.character_id = character.id;
  mail.world_id = character.world_id;
  mail.delivery_key = key;
  mail.title = "Server reward";
  mail.text = "Your attachments are ready.";
  mail.sent_epoch_ticks = kNow;
  mail.expiry_epoch_ticks = kNow + 1000;
  mail.attachment_expiry_epoch_ticks = kNow + 500;
  mail.attachments = {{MailAttachmentKind::item, kMaterial, 3, {}},
                      {MailAttachmentKind::item, kEquipment, 1, 4},
                      {MailAttachmentKind::item, kHousing, 2, {}},
                      {MailAttachmentKind::money, 0, 25, {}},
                      {MailAttachmentKind::money, 1, 7, {}}};
  return mail;
}

void exercise_atomic_claim(const CharacterRepositories &store, MailRepository &mail,
                           const std::string &suffix) {
  const auto definitions = catalog();
  const auto owner = create_owner(store, suffix);
  const auto stranger = create_owner(store, suffix + "x");
  const auto delivered = mail.deliver_mail(delivery(owner));
  ASSERT_TRUE(delivered);
  EXPECT_EQ(mail.deliver_mail(delivery(owner))->id, delivered->id);
  auto changed_delivery = delivery(owner);
  changed_delivery.text = "Different content";
  EXPECT_THROW(mail.deliver_mail(changed_delivery), std::invalid_argument);
  EXPECT_TRUE(mail.mailbox(stranger.account_id, stranger.id, 1, kNow + 1).empty());
  EXPECT_FALSE(
      mail.change_mail(owner.account_id, stranger.id, 1, {delivered->id}, false, kNow + 1));
  EXPECT_FALSE(mail.change_mail(owner.account_id, owner.id, 1, {delivered->id}, true, kNow + 1));
  ASSERT_TRUE(mail.change_mail(owner.account_id, owner.id, 1, {delivered->id}, false, kNow + 1));
  EXPECT_EQ(mail.mailbox(owner.account_id, owner.id, 1, kNow + 1).front().confirmed_epoch_ticks,
            kNow + 1);

  const auto before = store.character_state->load_world_progress(owner.account_id, owner.id, 1);
  auto plan = plan_mail_claim(*delivered, owner, before, definitions, kNow + 2);
  auto stale_after = before;
  ++stale_after.revision;
  ASSERT_TRUE(store.character_state->save_world_progress(owner.account_id, owner.id, 1, before,
                                                         stale_after));
  EXPECT_EQ(mail.claim_mail(owner.account_id, owner.id, 1, *delivered, kNow + 2, plan.before,
                            plan.after, plan.granted_items),
            MailClaimStatus::conflict);
  EXPECT_EQ(store.character_state->load_world_progress(owner.account_id, owner.id, 1).gold, 0);
  EXPECT_EQ(mail.mailbox(owner.account_id, owner.id, 1, kNow + 2).front().claimed_epoch_ticks, 0);

  const auto current = store.character_state->load_world_progress(owner.account_id, owner.id, 1);
  plan = plan_mail_claim(*delivered, owner, current, definitions, kNow + 3);
  auto fabricated = plan.after;
  fabricated.gold += 1;
  EXPECT_THROW(mail.claim_mail(owner.account_id, owner.id, 1, *delivered, kNow + 3, plan.before,
                               fabricated, plan.granted_items),
               std::invalid_argument);
  ASSERT_EQ(mail.claim_mail(owner.account_id, owner.id, 1, *delivered, kNow + 3, plan.before,
                            plan.after, plan.granted_items),
            MailClaimStatus::claimed);
  EXPECT_GT(plan.granted_items.front().id, 0);
  EXPECT_EQ(mail.claim_mail(owner.account_id, owner.id, 1, *delivered, kNow + 4, plan.before,
                            plan.after, plan.granted_items),
            MailClaimStatus::already_claimed);
  const auto after = store.character_state->load_world_progress(owner.account_id, owner.id, 1);
  EXPECT_EQ(after.gold, 25);
  EXPECT_EQ(after.hersander_token, 7);
  EXPECT_EQ(after.stackables.at(kMaterial), 3);
  EXPECT_EQ(after.housing_items.at(kHousing).quantity, 2);
  EXPECT_EQ(after.revision, current.revision + 1);
  const auto reloaded = store.characters->character_by_id(owner.id);
  ASSERT_TRUE(reloaded);
  EXPECT_EQ(reloaded->items.size(), owner.items.size() + 1);

  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto packets = mail_claim_packets(*delivered, plan);
  ASSERT_EQ(packets.size(), 1U);
  const auto decoded =
      protocol.decode(protocol.encode(packets.front().opcode, packets.front().message));
  EXPECT_EQ(decoded.at("message").at("system").at("char_mail_id"), std::to_string(delivered->id));
  EXPECT_EQ(decoded.at("message").at("money_changes").at(1).at("money_type"),
            "EX5MoneyType_HersanderToken");
  EXPECT_EQ(decoded.at("message").at("housing_items").at(0).at("quantity"), 2);
  EXPECT_EQ(decoded.at("message").at("items").at(0).at("grade"), "EX5ItemGrade_Legendary");
  EXPECT_TRUE(mail.change_mail(owner.account_id, owner.id, 1, {delivered->id}, true, kNow + 4));
  EXPECT_TRUE(mail.mailbox(owner.account_id, owner.id, 1, kNow + 4).empty());
  EXPECT_EQ(mail.deliver_mail(delivery(owner))->id, delivered->id);
}

TEST(Mail, MemoryAtomicOwnershipRevisionReceiptAndRewardPacket) {
  MemoryRepositories store;
  MemoryMailRepository mail{store.database};
  exercise_atomic_claim(store.character_repositories(), mail, "memory");
}

TEST(Mail, PostgresAtomicOwnershipRevisionReceiptAndRewardPacket) {
  const auto *database = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!database) {
    GTEST_SKIP() << "Set CHRONICLES_TUTORIAL_TEST_DATABASE_URL to a disposable database";
  }
  auto store = make_postgres_character_repositories(database);
  PostgresMailRepository mail{database};
  const auto migration_path = std::filesystem::path(CHRONICLES_PROTOCOL_DIR).parent_path() /
                              "migrations/001_accounts_characters.sql";
  std::ifstream stream(migration_path, std::ios::binary);
  const std::string migration{std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>()};
  ASSERT_FALSE(migration.empty());
  migrate_postgres(database, migration);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  exercise_atomic_claim(store, mail, suffix);
}

TEST(Mail, AccountMailCanBeClaimedOnceAcrossCharactersAndExpiryIsExclusive) {
  MemoryRepositories store;
  MemoryMailRepository mail{store.database};
  const auto owner = create_owner(store.character_repositories(), "account");
  const auto sibling = create_owner(store.character_repositories(), "account", 1);
  auto draft = delivery(owner);
  draft.character_id = 0;
  const auto record = *mail.deliver_mail(draft);
  const auto definitions = catalog();
  auto plan = plan_mail_claim(record, owner, {}, definitions, kNow + 1);
  ASSERT_EQ(mail.claim_mail(owner.account_id, owner.id, 1, record, kNow + 1, plan.before,
                            plan.after, plan.granted_items),
            MailClaimStatus::claimed);
  auto sibling_plan = plan_mail_claim(record, sibling, {}, definitions, kNow + 1);
  EXPECT_EQ(mail.claim_mail(sibling.account_id, sibling.id, 1, record, kNow + 1,
                            sibling_plan.before, sibling_plan.after,
                            sibling_plan.granted_items),
            MailClaimStatus::already_claimed);
  EXPECT_EQ(store.character_state->load_world_progress(sibling.account_id, sibling.id, 1).gold, 0);
  const auto expiring = *mail.deliver_mail(delivery(owner, "expiring"));
  EXPECT_THROW(plan_mail_claim(expiring, sibling, {}, definitions, kNow + 1),
               std::invalid_argument);
  EXPECT_THROW(
      plan_mail_claim(expiring, *store.characters->character_by_id(owner.id),
                      store.character_state->load_world_progress(owner.account_id, owner.id, 1),
                      definitions, expiring.attachment_expiry_epoch_ticks),
      std::invalid_argument);
}

TEST(Mail, RefusesCapacityUnknownItemsAndMixedBatchWithoutLosingAttachments) {
  MemoryRepositories store;
  MemoryMailRepository mail{store.database};
  auto owner = create_owner(store.character_repositories(), "capacity");
  const auto record = *mail.deliver_mail(delivery(owner));
  const auto definitions = catalog();
  auto progress = WorldProgress{};
  progress.stackables[kMaterial] = 998;
  EXPECT_THROW(plan_mail_claim(record, owner, progress, definitions, kNow + 1),
               std::invalid_argument);
  progress.stackables.clear();
  for (int slot = 0; slot <= kMaximumInventorySlot; ++slot) {
    CharacterItem item;
    item.cms_id = kEquipment;
    item.inventory_slot = slot;
    owner.items.push_back(item);
  }
  EXPECT_THROW(plan_mail_claim(record, owner, progress, definitions, kNow + 1),
               std::invalid_argument);
  auto unknown = delivery(owner, "unknown");
  unknown.attachments.front().cms_id = 123456;
  EXPECT_THROW(validate_mail_catalog(unknown, definitions), std::invalid_argument);
  EXPECT_FALSE(
      mail.change_mail(owner.account_id, owner.id, 1, {record.id, 123456}, false, kNow + 1));
  EXPECT_EQ(mail.mailbox(owner.account_id, owner.id, 1, kNow + 1).front().confirmed_epoch_ticks,
            0);
  EXPECT_FALSE(
      mail.change_mail(owner.account_id, owner.id, 1, {record.id, record.id}, false, kNow + 1));
  auto expired_plan = plan_mail_claim(record, *store.characters->character_by_id(owner.id), {},
                                      definitions, kNow + 1);
  EXPECT_EQ(mail.claim_mail(owner.account_id, owner.id, 1, record,
                            record.attachment_expiry_epoch_ticks, expired_plan.before,
                            expired_plan.after, expired_plan.granted_items),
            MailClaimStatus::expired);
  EXPECT_TRUE(mail.change_mail(owner.account_id, owner.id, 1, {record.id}, true,
                               record.attachment_expiry_epoch_ticks));
}

TEST(Mail, ConcurrentAccountClaimsGrantExactlyOnce) {
  MemoryRepositories store;
  MemoryMailRepository mail{store.database};
  const auto owner = create_owner(store.character_repositories(), "race");
  const auto sibling = create_owner(store.character_repositories(), "race", 1);
  auto draft = delivery(owner);
  draft.character_id = 0;
  const auto record = *mail.deliver_mail(draft);
  const auto definitions = catalog();
  auto owner_plan = plan_mail_claim(record, owner, {}, definitions, kNow + 1);
  auto sibling_plan = plan_mail_claim(record, sibling, {}, definitions, kNow + 1);
  MailClaimStatus first{};
  MailClaimStatus second{};
  std::thread first_thread([&] {
    first = mail.claim_mail(owner.account_id, owner.id, 1, record, kNow + 1, owner_plan.before,
                            owner_plan.after, owner_plan.granted_items);
  });
  std::thread second_thread([&] {
    second = mail.claim_mail(sibling.account_id, sibling.id, 1, record, kNow + 1,
                             sibling_plan.before, sibling_plan.after, sibling_plan.granted_items);
  });
  first_thread.join();
  second_thread.join();
  EXPECT_TRUE((first == MailClaimStatus::claimed && second == MailClaimStatus::already_claimed) ||
              (second == MailClaimStatus::claimed && first == MailClaimStatus::already_claimed));
  EXPECT_EQ(store.character_state->load_world_progress(owner.account_id, owner.id, 1).gold +
                store.character_state->load_world_progress(sibling.account_id, sibling.id, 1).gold,
            25);
}

TEST(Mail, CharacterDeletionReleasesMailboxCapacityAndPreservesAccountClaimReceipts) {
  MemoryRepositories store;
  MemoryMailRepository mail{store.database};
  const auto owner = create_owner(store.character_repositories(), "delete");
  const auto sibling = create_owner(store.character_repositories(), "delete", 1);
  auto account_draft = delivery(owner, "account-reward");
  account_draft.character_id = 0;
  const auto account_mail = *mail.deliver_mail(account_draft);
  const auto definitions = catalog();
  auto plan = plan_mail_claim(account_mail, owner, {}, definitions, kNow + 1);
  ASSERT_EQ(mail.claim_mail(owner.account_id, owner.id, 1, account_mail, kNow + 1, plan.before,
                            plan.after, plan.granted_items),
            MailClaimStatus::claimed);
  for (std::size_t index = 1; index < kMaximumMailboxEntries; ++index) {
    ASSERT_TRUE(mail.deliver_mail(delivery(owner, "personal-" + std::to_string(index))));
  }
  EXPECT_FALSE(mail.deliver_mail(delivery(sibling, "capacity-check")));
  ASSERT_TRUE(store.characters->delete_character(owner.id));
  EXPECT_TRUE(mail.mailbox(owner.account_id, owner.id, 1, kNow + 2).empty());
  const auto remaining = mail.mailbox(sibling.account_id, sibling.id, 1, kNow + 2);
  ASSERT_EQ(remaining.size(), 1U);
  EXPECT_EQ(remaining.front().id, account_mail.id);
  EXPECT_EQ(remaining.front().claimed_character_id, owner.id);
  auto sibling_plan = plan_mail_claim(account_mail, sibling, {}, definitions, kNow + 2);
  EXPECT_EQ(mail.claim_mail(sibling.account_id, sibling.id, 1, account_mail, kNow + 2,
                            sibling_plan.before, sibling_plan.after, sibling_plan.granted_items),
            MailClaimStatus::already_claimed);
  EXPECT_EQ(store.character_state->load_world_progress(sibling.account_id, sibling.id, 1).gold, 0);
  EXPECT_TRUE(mail.deliver_mail(delivery(sibling, "capacity-check")));
  // Character-scoped keys cascade with their recipient, as they do in PostgreSQL.
  EXPECT_TRUE(mail.deliver_mail(delivery(sibling, "personal-1")));
}

TEST(Mail, MemoryRepositoryForgetsDeletedCharacterMail) {
  // Bare repositories over one shared database, wired like create_repositories builds them:
  // the mail repository's deletion hook erases a deleted character's recipient mail.
  const auto database = std::make_shared<MemoryDatabase>();
  MemoryAccountRepository accounts{database};
  MemoryCharacterRepository characters{database};
  MemoryMailRepository mail{database};

  Account account;
  account.name = "mail-forget";
  ASSERT_TRUE(accounts.add_account(account));
  const auto stored_account = *accounts.account("mail-forget");

  Character owner_draft;
  owner_draft.account_id = stored_account.id;
  owner_draft.world_id = 1;
  owner_draft.name = "MailForgetOwner";
  const auto owner = characters.create(owner_draft);
  Character sibling_draft;
  sibling_draft.account_id = stored_account.id;
  sibling_draft.world_id = 1;
  sibling_draft.name = "MailForgetSibling";
  sibling_draft.slot = 1;
  const auto sibling = characters.create(sibling_draft);

  const auto delivered = mail.deliver_mail(delivery(owner));
  ASSERT_TRUE(delivered);
  ASSERT_TRUE(characters.delete_character(owner.id));

  // The second character's mailbox never lists the erased recipient mail, and its freed
  // character-scoped delivery key can be reused by a new delivery.
  EXPECT_TRUE(mail.mailbox(stored_account.id, sibling.id, 1, kNow + 1).empty());
  const auto redelivered = mail.deliver_mail(delivery(sibling));
  ASSERT_TRUE(redelivered);
  EXPECT_NE(redelivered->id, delivered->id);
}

TEST(Mail, OperatorParserRejectsAmbiguousTargetsAndWireRoundTripsAttachments) {
  const nlohmann::json request{
      {"account_id", 1},
      {"character_id", 2},
      {"world_id", 1},
      {"delivery_key", "campaign-1"},
      {"title", "Reward"},
      {"retention_seconds", 60},
      {"attachments", {{{"kind", "money"}, {"cms_id", 0}, {"count", 25}}}}};
  const auto mail = parse_mail_delivery(request, kNow);
  EXPECT_EQ(mail.attachments.front().cms_id, 0);
  auto invalid = request;
  invalid["character_id"] = -1;
  EXPECT_THROW(parse_mail_delivery(invalid, kNow), std::invalid_argument);
  invalid = request;
  invalid["ignored_field"] = true;
  EXPECT_THROW(parse_mail_delivery(invalid, kNow), std::invalid_argument);
  auto invalid_kind = mail;
  invalid_kind.attachments.front().kind = static_cast<MailAttachmentKind>(42);
  EXPECT_THROW(validate_mail_delivery(invalid_kind), std::invalid_argument);
  x5::pb::MailContent message;
  parse_json(mail_content_json(mail), message);
  ASSERT_EQ(message.attachments_size(), 1);
  EXPECT_EQ(message.attachments(0).money().amount(), 25);
  EXPECT_FALSE(message.has_attachment_completion_epochtime());
}

TEST(Mail, RestUsesSelectedOwnershipAndClaimCompletionIsAsynchronousAndAtMostOnce) {
  MemoryRepositories store;
  AuthService auth(store.character_repositories());
  ASSERT_TRUE(auth.create_account("mailrest", "test-password-123"));
  const auto token = auth.login("mailrest", "test-password-123").access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(auth.enter(token, enter).error_code(), x5::ec::Success);
  Character character;
  character.account_id = store.accounts->account("mailrest")->id;
  character.name = "MailRest";
  character = store.characters->create(character);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(character.id);
  ASSERT_EQ(auth.select(token, select).ec(), x5::ec::Success);
  MemoryMailRepository mail_repository{store.database};
  const auto mail = *mail_repository.deliver_mail(delivery(character));
  MailRest::ClaimReply pending;
  MailRest rest(
      auth, mail_repository,
      [&](const Character &selected, std::int64_t id, MailRest::ClaimReply reply) {
        EXPECT_EQ(selected.id, character.id);
        EXPECT_EQ(id, mail.id);
        pending = std::move(reply);
      },
      [] { return kNow + 1; });
  const auto authorization = "Bearer " + token;
  x5::pb::rest::MailboxGetReq page;
  page.set_count(10);
  const auto listed = rest.handle("PUT", "/api/front/MailboxGetReq", authorization,
                                  "application/protobuf", page.SerializeAsString());
  ASSERT_EQ(listed.status, 200);
  x5::pb::rest::MailboxGetRes contents;
  ASSERT_TRUE(contents.ParseFromString(listed.body));
  ASSERT_EQ(contents.contents_size(), 1);
  EXPECT_EQ(contents.contents(0).id(), mail.id);
  EXPECT_EQ(rest.handle("PUT", "/api/front/MailboxGetReq", "Bearer invalid", "application/protobuf",
                        page.SerializeAsString())
                .status,
            401);
  int completed{};
  const auto path = "/api/front/MailboxTakeAttachmentReq/" + std::to_string(mail.id);
  rest.handle_async("POST", path, authorization, "", "", [&](NativeHttpResponse response) {
    EXPECT_EQ(response.status, 204);
    ++completed;
  });
  ASSERT_TRUE(pending);
  EXPECT_EQ(completed, 0);
  pending(MailClaimStatus::claimed);
  pending(MailClaimStatus::claimed);
  EXPECT_EQ(completed, 1);

  MailRest throwing(
      auth, mail_repository,
      [](const Character &, std::int64_t, MailRest::ClaimReply) {
        throw std::runtime_error("Private storage diagnostic");
      },
      [] { return kNow + 1; });
  throwing.handle_async("POST", path, authorization, "", "", [&](NativeHttpResponse response) {
    EXPECT_EQ(response.status, 500);
    EXPECT_EQ(response.body.find("Private"), std::string::npos);
  });
}

} // namespace
} // namespace chronicles
