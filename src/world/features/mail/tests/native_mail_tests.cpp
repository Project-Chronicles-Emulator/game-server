#include "world/session/tests/native_world_test_support.hpp"

#include "world/features/events/ranking_repository.hpp"
#include "world/features/fog/fog_repository.hpp"
#include "world/features/mail/mail_repository.hpp"
#include "world/features/market/market_repository.hpp"


#include <future>

namespace chronicles {
namespace {

using namespace native_testing;

TEST(NativeWorld, MailDeliveryAndConcurrentClaimsUseOwnerOnlyNativePacketsAndOneReceipt) {
  constexpr int kMaterial = 9701;
  constexpr int kEquipment = 9702;
  constexpr int kHousingItem = 9703;
  constexpr std::size_t kClaimAttempts = 8;
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  const auto selected_owner =
      select_world_test_character(auth, "mail_tls_owner", "MailTlsOwner", 1);
  const auto selected_observer =
      select_world_test_character(auth, "mail_tls_observer", "MailTlsObserver", 3);
  const auto owner_account = repositories.accounts->account("mail_tls_owner");
  ASSERT_TRUE(owner_account.has_value());
  const auto owner = repositories.characters->characters(owner_account->id, 1).front();
  const auto before =
      repositories.character_state->load_world_progress(owner.account_id, owner.id, 1);

  TemporaryWorldCatalog catalog;
  catalog.write_tables({{"Item",
                         {{{"id", std::to_string(kMaterial)}, {"category", "CraftMaterial"}},
                          {{"id", std::to_string(kEquipment)},
                           {"category", "Equipment"},
                           {"grade", "Rare"},
                           {"maxDurability", "10000"}},
                          {{"id", std::to_string(kHousingItem)}, {"category", "HousingObject"}}}},
                        {"Shop", {}}});
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, catalog.open());
  const auto mail = std::make_shared<MemoryMailRepository>(repositories.database);
  server.listener().set_repositories(
      {mail, std::make_shared<MemoryMarketRepository>(repositories.database),
       std::make_shared<MemoryRankingRepository>(repositories.database),
       std::make_shared<MemoryFogRepository>(repositories.database)});
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  WorldTestClient owner_client(server.port());
  enter_shared_world(owner_client, protocol, selected_owner.packet_key());
  WorldTestClient observer_client(server.port());
  enter_shared_world(observer_client, protocol, selected_observer.packet_key());
  ASSERT_EQ(owner_client.read().opcode(), 10);
  ASSERT_EQ(observer_client.read().opcode(), 10);
  owner_client.send(protocol.encode(20086, nlohmann::json::object(), 2));
  ASSERT_EQ(read_until_opcode(owner_client, 183).opcode(), 183);
  observer_client.send(protocol.encode(20086, nlohmann::json::object(), 2));
  ASSERT_EQ(read_until_opcode(observer_client, 183).opcode(), 183);
  auto after_entry = before;
  ++after_entry.revision;
  EXPECT_EQ(repositories.character_state->load_world_progress(owner.account_id, owner.id, 1),
            after_entry);

  using EpochTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
  const auto now =
      std::chrono::duration_cast<EpochTicks>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  MailRecord draft;
  draft.account_id = owner.account_id;
  draft.character_id = owner.id;
  draft.world_id = 1;
  draft.delivery_key = "native-tls-reward-once";
  draft.title = "Server reward fixture";
  draft.text = "Claim these fixture attachments.";
  draft.sent_epoch_ticks = now;
  draft.expiry_epoch_ticks = now + 3600LL * 10000000;
  draft.attachment_expiry_epoch_ticks = draft.expiry_epoch_ticks;
  draft.attachments = {{MailAttachmentKind::money, 0, 25, {}},
                       {MailAttachmentKind::money, 1, 7, {}},
                       {MailAttachmentKind::item, kMaterial, 3, {}},
                       {MailAttachmentKind::item, kEquipment, 1, 4},
                       {MailAttachmentKind::item, kHousingItem, 2, {}}};
  const auto delivered = mail->deliver_mail(draft);
  ASSERT_TRUE(delivered.has_value());
  ASSERT_EQ(server.listener().notify_mail(*delivered), 1U);
  const auto notification = protocol.decode(read_until_opcode(owner_client, 174)).at("message");
  EXPECT_EQ(notification.at("char_mail").at("id"), std::to_string(delivered->id));
  EXPECT_EQ(notification.at("char_mail").at("attachments").size(), 5U);

  std::vector<std::future<MailClaimStatus>> results;
  for (std::size_t attempt = 0; attempt < kClaimAttempts; ++attempt) {
    auto completion = std::make_shared<std::promise<MailClaimStatus>>();
    results.push_back(completion->get_future());
    ASSERT_TRUE(
        server.listener().claim_mail(owner, delivered->id, [completion](MailClaimStatus status) {
          completion->set_value(status);
        }));
  }
  unsigned claimed{};
  unsigned already_claimed{};
  for (auto &result : results) {
    ASSERT_EQ(result.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    const auto status = result.get();
    claimed += status == MailClaimStatus::claimed ? 1U : 0U;
    already_claimed += status == MailClaimStatus::already_claimed ? 1U : 0U;
  }
  EXPECT_EQ(claimed, 1U);
  EXPECT_EQ(already_claimed, kClaimAttempts - 1);

  const auto reward = protocol.decode(read_until_opcode(owner_client, 169)).at("message");
  EXPECT_EQ(reward.at("system").at("char_mail_id"), std::to_string(delivered->id));
  const auto gold = std::ranges::find_if(reward.at("money_changes"), [](const auto &change) {
    return change.value("money_type", "EX5MoneyType_Gold") == "EX5MoneyType_Gold";
  });
  ASSERT_NE(gold, reward.at("money_changes").end());
  EXPECT_EQ(gold->at("change"), "25");
  EXPECT_EQ(gold->at("result"), std::to_string(before.gold + 25));
  EXPECT_EQ(reward.at("stackables").at(0).at("quantity"), 3);
  EXPECT_EQ(reward.at("items").at(0).at("item_cms_id"), kEquipment);
  EXPECT_EQ(reward.at("housing_items").at(0).at("quantity"), 2);

  // A pong bounds each socket's queue after every claim callback completed.
  // Neither retries nor a nearby observer may receive another grant or another owner's mail.
  for (auto *client : {&owner_client, &observer_client}) {
    client->send(protocol.encode(20003, {{"ping_cycles64", "1"}, {"tick", "1"}}, 3));
    for (unsigned count = 0; count < 100; ++count) {
      const auto frame = client->read();
      EXPECT_NE(frame.opcode(), 169);
      EXPECT_NE(frame.opcode(), 174);
      if (frame.opcode() == 4) {
        break;
      }
      ASSERT_LT(count, 99U);
    }
  }

  const auto after =
      repositories.character_state->load_world_progress(owner.account_id, owner.id, 1);
  EXPECT_EQ(after.revision, after_entry.revision + 1);
  EXPECT_EQ(after.gold, before.gold + 25);
  EXPECT_EQ(after.hersander_token, before.hersander_token + 7);
  EXPECT_EQ(after.stackables.at(kMaterial), 3);
  EXPECT_EQ(after.housing_items.at(kHousingItem).quantity, 2);
  const auto persisted_owner = repositories.characters->characters(owner.account_id, 1).front();
  EXPECT_EQ(persisted_owner.items.size(), owner.items.size() + 1);
  const auto claimed_item = std::ranges::find_if(
      persisted_owner.items, [](const CharacterItem &item) { return item.cms_id == kEquipment; });
  ASSERT_NE(claimed_item, persisted_owner.items.end());
  EXPECT_GT(claimed_item->id, 0);
  EXPECT_EQ(claimed_item->grade, 4);
  EXPECT_EQ(claimed_item->durability, 10000);
  const auto mailbox = mail->mailbox(owner.account_id, owner.id, 1, now + 1);
  ASSERT_EQ(mailbox.size(), 1U);
  EXPECT_GT(mailbox.front().claimed_epoch_ticks, 0);
  EXPECT_EQ(mailbox.front().claimed_character_id, owner.id);
}

} // namespace
} // namespace chronicles
