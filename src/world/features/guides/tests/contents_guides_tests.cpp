#include "world/features/guides/contents_guides.hpp"

#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

namespace chronicles::guides {
namespace {

using Json = nlohmann::json;

Json condition(const char *kind, int group = 1) {
  return {{"group", group}, {"operator", "AND"}, {"negated", false}, {"kind", kind}};
}

Json profile() {
  auto entered = condition("TriggerVolumeEnter");
  entered["volume_id"] = "tutorial-volume";
  auto left = condition("TriggerVolumeLeave");
  left["volume_id"] = "tutorial-volume";
  auto quest = condition("Quest");
  quest.update({{"quest_id", 42}, {"requirement", "QuestComplete"}});
  auto interaction = condition("Interaction");
  interaction["interaction_id"] = 10;
  auto unsupported = condition("Unsupported");
  unsupported["negated"] = true;
  auto scene = condition("CutSceneEnd");
  scene["cut_scene_id"] = 1523;
  auto weapon = condition("CurrentWeaponType");
  weapon["weapon_type"] = "OneHandedSword";
  auto item = condition("OwnItem");
  item.update({{"item_id", 200}, {"count", 1}, {"grade", "EX5ItemGrade_Normal"}});
  return {{"format_version", 1},
          {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
          {"guides", Json::array({
                         {{"id", 101}, {"type", "Text"}, {"start", Json::array({entered})},
                          {"end", Json::array({left})}},
                         {{"id", 102}, {"type", "Text"}, {"start", Json::array({quest})},
                          {"end", Json::array()}},
                         {{"id", 103}, {"type", "Text"}, {"start", Json::array({interaction})},
                          {"end", Json::array()}},
                         {{"id", 104}, {"type", "Text"}, {"start", Json::array({unsupported})},
                          {"end", Json::array()}},
                         {{"id", 105}, {"type", "ControlTip"},
                          {"start", Json::array({scene, weapon})}, {"end", Json::array()}},
                         {{"id", 106}, {"type", "Text"}, {"start", Json::array({item})},
                          {"end", Json::array()}},
                     })},
          {"maps", Json::array({{{"map_id", 8000},
                                 {"volumes", Json::array({{{"id", "tutorial-volume"},
                                                           {"center", {0, 0, 0}},
                                                           {"radius", 1000}}})}}})}};
}

class GuideTest : public testing::Test {
protected:
  std::shared_ptr<const Catalog> catalog = std::make_shared<Catalog>(profile());
  std::shared_ptr<const std::map<int, ItemDescription>> items =
      std::make_shared<const std::map<int, ItemDescription>>(
          std::map<int, ItemDescription>{{100, {"OneHandedSword", "Normal"}},
                                         {200, {"None", "Normal"}}});
  Session session{catalog, [](int) -> const Json * { return nullptr; }, items};
  WorldProgress progress;
  Character character;
  WorldPosition outside{8000, 3000, 0, 0, 0};
  WorldPosition inside{8000, 0, 0, 0, 0};
};

TEST_F(GuideTest, EnterAndLeaveUseLocalizedGuideIdsAndClearOnMapChange) {
  EXPECT_TRUE(session.observe(outside, progress, character, {}).empty());
  const auto entered = session.observe(inside, {}, {}, {});
  ASSERT_EQ(entered.size(), 1U);
  EXPECT_EQ(entered[0].message.at("contents_guide_cms_id"), 101);
  EXPECT_TRUE(entered[0].message.at("start_trigger").get<bool>());
  EXPECT_TRUE(session.observe(inside, {}, {}, {}).empty());
  const auto left = session.observe(outside, {}, {}, {});
  ASSERT_EQ(left.size(), 1U);
  EXPECT_FALSE(left[0].message.at("start_trigger").get<bool>());
  ASSERT_EQ(session.observe(inside, {}, {}, {}).size(), 1U);
  const auto map_change = session.observe({9012, 0, 0, 0, 0}, {}, {}, {});
  ASSERT_EQ(map_change.size(), 1U);
  EXPECT_FALSE(map_change[0].message.at("start_trigger").get<bool>());

  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : entered) {
    const auto decoded = protocol.decode(protocol.encode(packet.opcode, packet.message));
    EXPECT_EQ(decoded.at("status"), "decoded");
    EXPECT_EQ(decoded.at("message").at("contents_guide_cms_id"), 101);
  }
}

TEST_F(GuideTest, KnownExitStopsGuideWhenAnAlternativeIsUnsupported) {
  auto document = profile();
  auto &end_conditions = document["guides"][0]["end"];
  end_conditions[0]["operator"] = "OR";
  auto unsupported = condition("Unsupported");
  unsupported["operator"] = "OR";
  unsupported["negated"] = true;
  end_conditions.push_back(unsupported);

  auto mixed_catalog = std::make_shared<Catalog>(std::move(document));
  Session mixed_session(mixed_catalog, [](int) -> const Json * { return nullptr; }, items);
  EXPECT_TRUE(mixed_session.observe(outside, progress, character, {}).empty());
  const auto entered = mixed_session.observe(inside, {}, {}, {});
  ASSERT_EQ(entered.size(), 1U);
  EXPECT_TRUE(entered.front().message.at("start_trigger").get<bool>());
  const auto left = mixed_session.observe(outside, {}, {}, {});
  ASSERT_EQ(left.size(), 1U);
  EXPECT_FALSE(left.front().message.at("start_trigger").get<bool>());
  const auto reentered = mixed_session.observe(inside, {}, {}, {});
  ASSERT_EQ(reentered.size(), 1U);
  EXPECT_TRUE(reentered.front().message.at("start_trigger").get<bool>());
}

TEST_F(GuideTest, QuestCompletionAppearsOnceButHistoricalCompletionDoesNotReplay) {
  QuestProgress completed;
  completed.complete = true;
  progress.quests[42] = completed;
  EXPECT_TRUE(session.observe(outside, progress, character, {}).empty());
  EXPECT_TRUE(session.observe(inside, {}, {}, {}).size() == 1U);

  Session fresh(catalog, [](int) -> const Json * { return nullptr; }, items);
  WorldProgress before;
  EXPECT_TRUE(fresh.observe(outside, before, character, {}).empty());
  const auto changed = fresh.observe(outside, progress, {}, {});
  ASSERT_EQ(changed.size(), 1U);
  EXPECT_EQ(changed[0].message.at("contents_guide_cms_id"), 102);
  EXPECT_TRUE(fresh.observe(outside, progress, {}, {}).empty());
}

TEST_F(GuideTest, OnlySuccessfulInteractionsTriggerAndUnsupportedConditionsFailClosed) {
  EXPECT_TRUE(session.observe(outside, progress, character, {}).empty());
  const GameplayPacket failed{163, {{"interaction_id", 10}, {"ec", "BadRequest"}}};
  EXPECT_TRUE(session.observe(outside, {}, {}, {failed}).empty());
  const GameplayPacket success{163, {{"interaction_id", 10}}};
  const auto notifications = session.observe(outside, {}, {}, {success});
  ASSERT_EQ(notifications.size(), 1U);
  EXPECT_EQ(notifications[0].message.at("contents_guide_cms_id"), 103);
  EXPECT_TRUE(session.observe(outside, {}, {}, {success}).empty());
}

TEST_F(GuideTest, CompletedCutsceneShowsOnlyTheEquippedWeaponsGuide) {
  character.items.push_back({1, 100, 1, 0, 1, 10000, 0});
  EXPECT_TRUE(session.observe(outside, progress, character, {}).empty());
  EXPECT_TRUE(session.observe(outside, {}, {}, {}, 100).empty());
  const auto shown = session.observe(outside, {}, {}, {}, 1523);
  ASSERT_EQ(shown.size(), 1U);
  EXPECT_EQ(shown.front().message.at("contents_guide_cms_id"), 105);
  EXPECT_TRUE(session.observe(outside, {}, {}, {}, 1523).empty());
}

TEST_F(GuideTest, AcquiringAnItemShowsItsGuideWithoutReplayingExistingItems) {
  EXPECT_TRUE(session.observe(outside, progress, character, {}).empty());
  progress.stackables[200] = 1;
  const auto shown = session.observe(outside, progress, {}, {});
  ASSERT_EQ(shown.size(), 1U);
  EXPECT_EQ(shown.front().message.at("contents_guide_cms_id"), 106);
  EXPECT_TRUE(session.observe(outside, progress, {}, {}).empty());

  Session reconnect(catalog, [](int) -> const Json * { return nullptr; }, items);
  EXPECT_TRUE(reconnect.observe(outside, progress, character, {}).empty());
}

} // namespace
} // namespace chronicles::guides
