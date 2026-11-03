#include "world/features/mail/mail.hpp"
#include "data/shop_catalog.hpp"
#include "testing/memory_repositories.hpp"
#include "world/features/mail/mail_repository.hpp"
#include "world/features/mail/tutorial_completion_mail.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

constexpr std::int64_t kNow = 1'000'000'000;
constexpr std::int64_t kLater = kNow + 5'000'000;
constexpr int kHealthPotion = 40101101;
constexpr int kStaminaPotion = 40301201;

ShopCatalog catalog_with_rewards() {
  ShopCatalogTables tables;
  tables.items = {
      {{"id", std::to_string(kHealthPotion)}, {"category", "Consumable"}, {"grade", "Normal"}},
      {{"id", std::to_string(kStaminaPotion)}, {"category", "Consumable"}, {"grade", "Rare"}}};
  tables.game_variables = {{{"name", "ItemMaxStack"}, {"value", "999"}},
                           {{"name", "ResetBaseDateTime"}, {"value", "2025-01-01 00:00:00"}}};
  tables.money = {{{"type", "Gold"}, {"limitUserMax", "9000000000000000"}}};
  return ShopCatalog(tables);
}

Character create_character(const CharacterRepositories &store,
                           const std::string &account_name, int slot) {
  if (!store.accounts->account(account_name)) {
    Account account;
    account.name = account_name;
    account.salt = "test-salt";
    account.hash = "test-hash";
    EXPECT_TRUE(store.accounts->add_account(account));
  }

  Character character;
  character.account_id = store.accounts->account(account_name)->id;
  character.name = "Tut" + std::to_string(slot) + account_name.substr(account_name.size() - 3);
  character.slot = slot;
  return store.characters->create(character);
}

TEST(TutorialCompletionMail, BuildsAnAccountMailWithTheFreeSupplies) {
  const auto mail = build_tutorial_completion_mail(7, 1, kNow);

  EXPECT_EQ(mail.account_id, 7);
  EXPECT_EQ(mail.character_id, 0);
  EXPECT_EQ(mail.world_id, 1);
  EXPECT_EQ(mail.delivery_key, kTutorialCompletionMailKey);
  EXPECT_EQ(mail.sent_epoch_ticks, kNow);
  EXPECT_GT(mail.attachment_expiry_epoch_ticks, kNow);

  ASSERT_EQ(mail.attachments.size(), 2U);
  EXPECT_EQ(mail.attachments[0].kind, MailAttachmentKind::item);
  EXPECT_EQ(mail.attachments[0].cms_id, kHealthPotion);
  EXPECT_EQ(mail.attachments[1].cms_id, kStaminaPotion);
}

TEST(TutorialCompletionMail, RewardsEachAccountOnceAcrossItsCharacters) {
  MemoryRepositories store;
  MemoryMailRepository mail{store.database};
  const auto definitions = catalog_with_rewards();
  const auto first = create_character(store.character_repositories(), "tutorial_mail_one", 0);
  const auto sibling = create_character(store.character_repositories(), "tutorial_mail_one", 1);
  const auto other_account =
      create_character(store.character_repositories(), "tutorial_mail_two", 0);

  const auto delivered =
      deliver_tutorial_completion_mail(mail, definitions, first.account_id, 1, kNow);
  ASSERT_TRUE(delivered.has_value());

  // A second character of the same account finishing later neither duplicates the reward nor
  // reports a new mail to notify.
  EXPECT_FALSE(
      deliver_tutorial_completion_mail(mail, definitions, sibling.account_id, 1, kLater));
  EXPECT_EQ(mail.mailbox(first.account_id, first.id, 1, kLater).size(), 1U);
  EXPECT_EQ(mail.mailbox(sibling.account_id, sibling.id, 1, kLater).size(), 1U);

  const auto other_delivery =
      deliver_tutorial_completion_mail(mail, definitions, other_account.account_id, 1, kNow);
  ASSERT_TRUE(other_delivery.has_value());
  EXPECT_NE(other_delivery->id, delivered->id);
}

TEST(TutorialCompletionMail, RemainsClaimableByTheFirstCharacterThatClaims) {
  MemoryRepositories store;
  MemoryMailRepository mail{store.database};
  const auto definitions = catalog_with_rewards();
  const auto character = create_character(store.character_repositories(), "tutorial_mail_claim", 0);
  const auto delivered =
      deliver_tutorial_completion_mail(mail, definitions, character.account_id, 1, kNow);
  ASSERT_TRUE(delivered.has_value());

  const auto before =
      store.character_state->load_world_progress(character.account_id, character.id, 1);
  auto plan = plan_mail_claim(*delivered, character, before, definitions, kLater);
  ASSERT_EQ(mail.claim_mail(character.account_id, character.id, 1, *delivered, kLater,
                            plan.before, plan.after, plan.granted_items),
            MailClaimStatus::claimed);

  const auto after =
      store.character_state->load_world_progress(character.account_id, character.id, 1);
  EXPECT_EQ(after.stackables.at(kHealthPotion), 20);
  EXPECT_EQ(after.stackables.at(kStaminaPotion), 10);

  // A claimed and even deleted reward is not issued again by a later tutorial completion.
  EXPECT_FALSE(
      deliver_tutorial_completion_mail(mail, definitions, character.account_id, 1, kLater));
}

TEST(TutorialCompletionMail, RefusesRewardsMissingFromTheItemCatalog) {
  MemoryRepositories store;
  MemoryMailRepository mail{store.database};
  const auto character = create_character(store.character_repositories(), "tutorial_mail_nocat", 0);

  ShopCatalogTables tables;
  tables.items = {
      {{"id", std::to_string(kHealthPotion)}, {"category", "Consumable"}, {"grade", "Normal"}}};
  tables.game_variables = {{{"name", "ItemMaxStack"}, {"value", "999"}},
                           {{"name", "ResetBaseDateTime"}, {"value", "2025-01-01 00:00:00"}}};
  tables.money = {{{"type", "Gold"}, {"limitUserMax", "9000000000000000"}}};
  const ShopCatalog incomplete(tables);

  EXPECT_THROW(
      deliver_tutorial_completion_mail(mail, incomplete, character.account_id, 1, kNow),
      std::invalid_argument);
  EXPECT_TRUE(mail.mailbox(character.account_id, character.id, 1, kLater).empty());
}

TEST(TutorialCompletionMail, ReturnsNothingForAnUnknownAccount) {
  MemoryRepositories store;
  MemoryMailRepository mail{store.database};
  const auto definitions = catalog_with_rewards();

  EXPECT_FALSE(deliver_tutorial_completion_mail(mail, definitions, 9999, 1, kNow));
}

} // namespace
} // namespace chronicles
