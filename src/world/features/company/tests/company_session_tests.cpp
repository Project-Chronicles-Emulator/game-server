#include "testing/company_test_support.hpp"
#include "world/features/company/company_feature.hpp"

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>

namespace chronicles::company {
namespace {

class CompanyContext final : public CompanySessionContext {
public:
  std::optional<CompanyCharacter> company_character() const override {
    return authenticated ? std::optional<CompanyCharacter>({1, 17}) : std::nullopt;
  }

  std::optional<std::int64_t> company_selected_character_id() const override {
    return selected ? std::optional<std::int64_t>(17) : std::nullopt;
  }

  const WorldProgress *company_tutorial_progress() const override {
    return tutorial ? &progress : nullptr;
  }

  std::optional<WorldProgress> company_current_progress() const override {
    return authenticated ? std::optional<WorldProgress>(progress) : std::nullopt;
  }

  std::optional<int> company_dialogue_npc() const override {
    return npc;
  }

  bool company_has_guild_membership(std::int64_t character_id) const override {
    EXPECT_EQ(character_id, 17);
    return guild_member;
  }

  void company_publish_activity_experience(std::int64_t character_id,
                                           std::int64_t experience) override {
    EXPECT_EQ(character_id, 17);
    experience_awards.push_back(experience);
    if (on_experience) {
      on_experience();
    }
  }

  void company_publish_membership_changed() override {
    ++membership_refreshes;
    if (on_membership) {
      on_membership();
    }
  }

  void company_publish_progress_committed(const WorldProgress &committed) override {
    progress = committed;
    ++adoptions;
  }

  bool authenticated{true};
  bool selected{true};
  bool tutorial{true};
  bool guild_member{};
  std::optional<int> npc{145};
  WorldProgress progress;
  std::vector<std::int64_t> experience_awards;
  int membership_refreshes{};
  int adoptions{};
  std::function<void()> on_experience;
  std::function<void()> on_membership;
};

std::shared_ptr<const CompanyDialogueIndex> dialogues() {
  return CompanyDialogueIndex::from_rows(
      {{{"id", "1401"},
        {"sequenceOrder", "1"},
        {"selectOrder", "1"},
        {"playerTaskCase", "CompanyJoin"}},
       {{"id", "1401"},
        {"sequenceOrder", "1"},
        {"selectOrder", "2"},
        {"playerTaskCase", "CompanyChange"}},
       {{"id", "1401"},
        {"sequenceOrder", "1"},
        {"selectOrder", "3"},
        {"playerTaskCase", "PioneerPermitRegist"}}},
      {{{"id", "1401"}, {"speakerName", "Npc"}, {"speakerNameValue", "145"}}}, {},
      {{{"id", "145"}, {"company", "Antiqus"}}});
}

WorldGameplayRequest dialogue_request(int selection) {
  return {company_world::kDialogueFunctionRequest,
          41,
          {{"dialogue_cms_id", 1401}, {"sequence_order", 1}, {"select_order", selection}}};
}

std::shared_ptr<CompanyService> service(std::shared_ptr<CompanyStore> store = {}) {
  if (!store) {
    store = std::make_shared<MemoryCompanyStore>();
  }
  auto rules = testing_support::company_rules();
  rules.companies.at(testing_support::kAntiqus).positions.front().pioneer_permit_money_value = 10;
  return std::make_shared<CompanyService>(store, rules, 1, [] {
    return 1'700'000'000LL * kEpochTicksPerSecond;
  });
}

TEST(CompanySession, ActivityPublishesGuildExperienceBeforeContributionAndRefreshAfterPacket) {
  auto companies = service();
  ASSERT_EQ(companies->join(17, testing_support::kAntiqus).error, x5::ec::Success);
  CompanyFeature feature(companies, dialogues());
  CompanyContext context;
  auto session = feature.create_session(context);
  session->on_select_character(context.progress);
  std::vector<GameplayPacket> packets;
  std::vector<std::string> order;
  context.on_experience = [&] {
    order.push_back("guild experience");
    EXPECT_EQ(companies->status(17).record.contribution, 0);
    EXPECT_TRUE(packets.empty());
  };
  context.on_membership = [&] {
    order.push_back("guild roster");
    EXPECT_EQ(companies->status(17).record.contribution, 1);
    ASSERT_EQ(packets.size(), 1U);
    EXPECT_EQ(packets[0].opcode, company_world::kCompanyUpdateNotification);
  };
  context.progress.experience = 20;
  session->collect_activity_rewards(packets);
  EXPECT_EQ(order, (std::vector<std::string>{"guild experience", "guild roster"}));
  EXPECT_EQ(context.experience_awards, (std::vector<std::int64_t>{20}));
  session->collect_activity_rewards(packets);
  EXPECT_EQ(packets.size(), 1U);
}

TEST(CompanySession, ActivityCursorAdvancesBeforeSubscriberFailureAndIsNotReplayed) {
  CompanyFeature feature(service(), {});
  CompanyContext context;
  auto session = feature.create_session(context);
  session->on_select_character(context.progress);
  context.progress.experience = 20;
  context.on_experience = [] {
    throw std::runtime_error("guild write failed");
  };
  std::vector<GameplayPacket> packets;
  EXPECT_THROW(session->collect_activity_rewards(packets), std::runtime_error);
  context.on_experience = {};
  session->collect_activity_rewards(packets);
  EXPECT_EQ(context.experience_awards, (std::vector<std::int64_t>{20}));
  EXPECT_TRUE(packets.empty());
}

TEST(CompanySession, ExperienceDecreaseResetsTheCursorBeforeLaterAwards) {
  CompanyFeature feature({}, {});
  CompanyContext context;
  context.progress.experience = 100;
  auto session = feature.create_session(context);
  session->on_select_character(context.progress);
  std::vector<GameplayPacket> packets;
  context.progress.experience = 80;
  session->collect_activity_rewards(packets);
  EXPECT_TRUE(context.experience_awards.empty());
  context.progress.experience = 100;
  session->collect_activity_rewards(packets);
  EXPECT_EQ(context.experience_awards, (std::vector<std::int64_t>{20}));
}

TEST(CompanySession, NoActivityCursorOrTutorialProducesNoAward) {
  CompanyFeature feature(service(), {});
  CompanyContext context;
  auto session = feature.create_session(context);
  std::vector<GameplayPacket> packets;
  context.progress.experience = 20;
  session->collect_activity_rewards(packets);
  EXPECT_TRUE(context.experience_awards.empty());
  session->on_select_character(context.progress);
  context.tutorial = false;
  context.progress.experience = 40;
  session->collect_activity_rewards(packets);
  EXPECT_TRUE(context.experience_awards.empty());
}

TEST(CompanySession, DialogueJoinCommitsMembershipBeforeRefreshingGuildRoster) {
  auto companies = service();
  CompanyFeature feature(companies, dialogues());
  CompanyContext context;
  context.on_membership = [&] {
    EXPECT_EQ(companies->status(17).record.company_type, testing_support::kAntiqus);
  };
  auto session = feature.create_session(context);
  const auto packets = session->dialogue(dialogue_request(1));
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets[0].opcode, company_world::kJoinCompanyResponse);
  EXPECT_EQ(context.membership_refreshes, 1);
}

TEST(CompanySession, GuildMembershipRefusesCompanyChangeBeforeMutatingTheService) {
  auto companies = service();
  ASSERT_EQ(companies->join(17, testing_support::kAntiqus).error, x5::ec::Success);
  CompanyFeature feature(companies, dialogues());
  CompanyContext context;
  context.guild_member = true;
  auto session = feature.create_session(context);
  const auto packets = session->dialogue(dialogue_request(2));
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets[0].message, company_error_packet(41, x5::ec::Company_CannotLeave).message);
  EXPECT_EQ(context.membership_refreshes, 0);
}

TEST(CompanySession, InvalidDialogueSpeakerAndAbsentCapabilitiesRetainOriginalRefusal) {
  CompanyFeature feature(service(), dialogues());
  CompanyContext context;
  context.npc = 999;
  auto session = feature.create_session(context);
  const auto invalid = session->dialogue(dialogue_request(1));
  ASSERT_EQ(invalid.size(), 1U);
  EXPECT_EQ(invalid[0].opcode, company_world::kCommonErrorResponse);
  context.authenticated = false;
  EXPECT_EQ(session->dialogue(dialogue_request(1))[0].message,
            company_error_packet(41, x5::ec::Dialouge_InvalidSelectionData).message);
}

class PermitStore final : public CompanyStore {
public:
  std::vector<CompanyRecord> load(int) override {
    return {};
  }

  void save(const CompanyRecord &record) override {
    records[record.char_id] = record;
  }

  bool commit_permit(std::int64_t account_id, int world_id, const CompanyRecord &before_company,
                     const CompanyRecord &after_company, const WorldProgress &before_progress,
                     const WorldProgress &after_progress) override {
    EXPECT_EQ(account_id, 1);
    EXPECT_EQ(world_id, 1);
    ++commits;
    if (fail || records.at(before_company.char_id) != before_company ||
        progress != before_progress) {
      return false;
    }
    records[after_company.char_id] = after_company;
    progress = after_progress;
    return true;
  }

  bool fail{};
  int commits{};
  std::map<std::int64_t, CompanyRecord> records;
  WorldProgress progress;
};

TEST(CompanySession, PermitAdoptsOnlyTheAtomicCommitAndKeepsInventoryBeforePermitResponse) {
  auto store = std::make_shared<PermitStore>();
  auto companies = service(store);
  ASSERT_EQ(companies->join(17, testing_support::kAntiqus).error, x5::ec::Success);
  CompanyFeature feature(companies, dialogues());
  CompanyContext context;
  context.progress.gold = 25;
  store->progress = context.progress;
  auto session = feature.create_session(context);
  store->fail = true;
  const auto refused = session->dialogue(dialogue_request(3));
  EXPECT_EQ(refused[0].opcode, company_world::kCommonErrorResponse);
  EXPECT_EQ(context.adoptions, 0);
  EXPECT_EQ(context.progress.gold, 25);

  store->fail = false;
  const auto paid = session->dialogue(dialogue_request(3));
  ASSERT_EQ(paid.size(), 2U);
  EXPECT_EQ(paid[0].opcode, 169);
  EXPECT_EQ(paid[1].opcode, company_world::kRegisterPioneerPermitResponse);
  EXPECT_EQ(context.adoptions, 1);
  EXPECT_EQ(context.progress.gold, 15);
  EXPECT_EQ(context.progress, store->progress);
  EXPECT_EQ(store->commits, 2);
  EXPECT_TRUE(companies->status(17).permit_valid);
}

// Devicas-style permit desk: the speaker and the selection name no company, so the permit is
// issued for the character's own company and charged like any other registration.
TEST(CompanySession, PermitDeskWithoutACompanyRegistersTheCharactersOwnCompany) {
  auto store = std::make_shared<PermitStore>();
  auto companies = service(store);
  ASSERT_EQ(companies->join(17, testing_support::kAntiqus).error, x5::ec::Success);
  const auto permit_desk = CompanyDialogueIndex::from_rows(
      {{{"id", "10500"},
        {"sequenceOrder", "1"},
        {"selectOrder", "2"},
        {"playerTaskCase", "PioneerPermitRegist"}}},
      {{{"id", "10500"}, {"speakerName", "Npc"}, {"speakerNameValue", "1001175"}}}, {},
      {{{"id", "1001175"}, {"company", "None"}}});
  CompanyFeature feature(companies, permit_desk);
  CompanyContext context;
  context.npc = 1001175;
  // Devicas stands on the shared world, where no tutorial instance exists; the charge
  // commits from stored progress.
  context.tutorial = false;
  context.progress.gold = 25;
  store->progress = context.progress;
  auto session = feature.create_session(context);

  const auto paid = session->dialogue({company_world::kDialogueFunctionRequest,
                                       42,
                                       {{"dialogue_cms_id", 10500},
                                        {"sequence_order", 1},
                                        {"select_order", 2}}});

  ASSERT_EQ(paid.size(), 2U);
  EXPECT_EQ(paid[1].opcode, company_world::kRegisterPioneerPermitResponse);
  EXPECT_EQ(context.progress.gold, 15);
  EXPECT_TRUE(companies->status(17).permit_valid);
}

} // namespace
} // namespace chronicles::company
