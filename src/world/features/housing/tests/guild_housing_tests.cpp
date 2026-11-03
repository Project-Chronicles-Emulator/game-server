#include "testing/company_test_support.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/housing/guild_housing.hpp"
#include "world/features/housing/housing_service.hpp"
#include "testing/memory_repositories.hpp"
#include "testing/housing_test_support.hpp"
#include "world/features/housing/housing_world.hpp"
#include "world/features/housing/native_housing.hpp"
#include "world/features/chat/native_chat.hpp"
#include "world/features/guild/native_guild.hpp"
#include "services/front/native_ws.hpp"
#include "p_c2f.ws.guild.pb.h"
#include "p_f2c.ws.guild.pb.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles {
namespace {

namespace wire = x5::pb::guild;
using housing::testing_support::kPermitItem;
using housing::testing_support::kSmallLot;
using housing::testing_support::kZoneKey;

constexpr int kWorldId = 1;
constexpr std::int64_t kStartTicks = 638'000'000'000'000'000;
// Guild area rent in the test catalog: 3000 permits and a one-time 5000-token unlock.
constexpr int kGuildRentPermits = 3000;
constexpr std::int64_t kGuildUnlockTokens = 5000;

struct Member {
  std::int64_t account_id{};
  std::int64_t char_id{};
  std::string name;
};

// One delivered front frame, decoded lazily by the test.
struct SentFrame {
  std::int64_t char_id{};
  std::string frame;
};

// Memory housing storage whose commits can be made to fail like a lost database connection.
class InterruptibleHousingStore final : public housing::HousingStore {
public:
  explicit InterruptibleHousingStore(
      std::shared_ptr<CharacterStateRepository> character_state)
      : rows_(std::move(character_state)) {}

  housing::HousingSnapshot load(int world_id) override {
    return rows_.load(world_id);
  }

  WorldProgress load_progress(std::int64_t account_id, std::int64_t character_id,
                              int world_id) override {
    return rows_.load_progress(account_id, character_id, world_id);
  }

  bool commit(const housing::HousingCommit &commit) override {
    if (unavailable) {
      throw std::runtime_error("housing storage unavailable");
    }
    return rows_.commit(commit);
  }

  bool unavailable{};

private:
  housing::MemoryHousingStore rows_;
};

class GuildHousingTest : public testing::Test {
protected:
  GuildHousingTest() {
    guilds_ = std::make_shared<GuildService>(std::make_shared<MemoryGuildStore>(), GuildRules{},
                                             kWorldId);
    housing_store_ =
        std::make_shared<InterruptibleHousingStore>(repositories_.character_state);
    housing_ = std::make_shared<housing::HousingService>(
        housing::testing_support::housing_catalog(), housing_store_, kWorldId,
        [this] { return now_; }, housing::HousingPolicy{},
        std::make_shared<LegacyGuildServiceDirectory>(guilds_));
    coordinator_ = std::make_shared<GuildHousingCoordinator>(housing_, guilds_);
    coordinator_->set_front_delivery([this](std::int64_t char_id, const std::string &frame) {
      sent_.push_back({char_id, frame});
    });
    guilds_->subscribe([this](const std::vector<GuildEvent> &events) {
      // Off to model a cleanup lost after the guild change committed (a storage failure,
      // a session timeout or a server stop before the queued cleanup ran).
      if (deliver_guild_events_) {
        coordinator_->on_guild_events(events);
      }
    });

    master_ = create_member("Builder");
    resident_ = create_member("Resident");
    GuildCreateRequest create;
    create.name = "Builders";
    create.join_type = 1;
    const auto created = guilds_->create(actor(master_), create);
    if (created.error != x5::ec::Success) {
      throw std::runtime_error("Cannot create test guild");
    }
    guild_id_ = created.guild.id;
    if (guilds_->join(actor(resident_), guild_id_).error != x5::ec::Success) {
      throw std::runtime_error("Cannot join test guild");
    }
  }

  Member create_member(const std::string &name) {
    Character created;
    created.account_id = 100 + static_cast<std::int64_t>(members_created_++);
    created.name = name;
    const auto character = repositories_.characters->create(created);
    return Member{created.account_id, character.id, name};
  }

  GuildActor actor(const Member &member, int company_position = 3) const {
    GuildActor result;
    result.world_id = kWorldId;
    result.char_id = member.char_id;
    result.account_id = member.account_id;
    result.name = member.name;
    result.level = 10;
    result.company_type = testing_support::kAntiqus;
    result.company_position = company_position;
    return result;
  }

  static GuildHousingRequester requester(const Member &member) {
    return GuildHousingRequester{member.account_id, member.char_id, kWorldId, member.name};
  }

  void give_rent(const Member &member) {
    const auto before = repositories_.character_state->load_world_progress(
        member.account_id, member.char_id, kWorldId);
    auto after = before;
    after.revision = before.revision + 1;
    after.stackables[kPermitItem] += kGuildRentPermits;
    after.hersander_token += kGuildUnlockTokens;
    ASSERT_TRUE(repositories_.character_state->save_world_progress(
        member.account_id, member.char_id, kWorldId, before, after));
  }

  std::string send(const Member &member, std::uint16_t packet_id,
                   const google::protobuf::Message &message) {
    const auto encoded = native_ws::encode_frame(packet_id, message);
    const auto frames = native_ws::decode_frames(encoded);
    return coordinator_->handle(requester(member), frames.front());
  }

  template <class Message>
  static Message decode(const std::string &frame, std::uint16_t packet_id) {
    const auto frames = native_ws::decode_frames(frame);
    if (frames.size() != 1 || frames.front().packet_id != packet_id) {
      throw std::runtime_error("Unexpected guild housing frame");
    }
    Message message;
    const auto payload = frames.front().payload;
    if (!message.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
      throw std::runtime_error("Malformed guild housing frame");
    }
    return message;
  }

  // Frames delivered to `char_id` with `packet_id`, in delivery order.
  std::vector<std::string> sent_to(std::int64_t char_id, std::uint16_t packet_id) const {
    std::vector<std::string> frames;
    for (const auto &sent : sent_) {
      const auto decoded = native_ws::decode_frames(sent.frame);
      if (sent.char_id == char_id && decoded.front().packet_id == packet_id) {
        frames.push_back(sent.frame);
      }
    }
    return frames;
  }

  wire::GuildHousingAreaContractRes contract_area() {
    give_rent(master_);
    wire::GuildHousingAreaContractReq request;
    request.set_guild_id(guild_id_);
    request.set_zone_key(static_cast<x5::cms::EX5ZoneKey>(kZoneKey));
    request.set_contract_type(x5::cms::EX5HousingContractType_MonthlyContract);
    request.set_area_name("Harbor");
    EXPECT_TRUE(send(master_, guild_housing::kAreaContractRequest, request).empty());

    const auto responses = sent_to(master_.char_id, guild_housing::kAreaContractResponse);
    if (responses.size() != 1) {
      throw std::runtime_error("Missing guild area contract response");
    }
    return decode<wire::GuildHousingAreaContractRes>(responses.front(),
                                                     guild_housing::kAreaContractResponse);
  }

  wire::GuildHousingSiteAllowMemberRes allow_resident() {
    wire::GuildHousingSiteAllowMemberReq request;
    request.set_guild_id(guild_id_);
    auto *lot = request.add_allow_lots();
    lot->set_lot_number(kSmallLot);
    lot->set_char_id(resident_.char_id);
    return decode<wire::GuildHousingSiteAllowMemberRes>(
        send(master_, guild_housing::kSiteAllowMemberRequest, request),
        guild_housing::kSiteAllowMemberResponse);
  }

  std::int64_t now_{kStartTicks};
  std::size_t members_created_{};
  MemoryRepositories repositories_;
  std::shared_ptr<GuildService> guilds_;
  std::shared_ptr<InterruptibleHousingStore> housing_store_;
  std::shared_ptr<housing::HousingService> housing_;
  std::shared_ptr<GuildHousingCoordinator> coordinator_;
  std::vector<SentFrame> sent_;
  Member master_;
  Member resident_;
  std::int64_t guild_id_{};
  bool deliver_guild_events_{true};
};

TEST_F(GuildHousingTest, DirectoryMapsGradesAndCompanyPositions) {
  LegacyGuildServiceDirectory directory(guilds_);
  const auto membership = directory.membership(master_.char_id);
  ASSERT_TRUE(membership.has_value());
  EXPECT_EQ(membership->guild_id, guild_id_);
  EXPECT_EQ(membership->grade, housing::GuildGrade::master);
  EXPECT_EQ(membership->max_member_count, 10);
  EXPECT_FALSE(directory.membership(999).has_value());

  const auto members = directory.members(guild_id_);
  ASSERT_EQ(members.size(), 2u);
  EXPECT_EQ(members[1].grade, housing::GuildGrade::member);
  EXPECT_EQ(members[1].account_id, resident_.account_id);
  EXPECT_EQ(members[1].company_position_level, 3);

  EXPECT_EQ(housing_guild_grade(GuildGrade::sub_master), housing::GuildGrade::sub_master);
  EXPECT_EQ(guild_grade_from_housing(housing::GuildGrade::sub_master), GuildGrade::sub_master);
  EXPECT_EQ(guild_grade_from_housing(housing::GuildGrade::member), GuildGrade::member);
}

TEST_F(GuildHousingTest, MasterContractsAnAreaAndMembersAreNotified) {
  wire::GuildHousingAreaContractReq refused;
  refused.set_guild_id(guild_id_);
  refused.set_zone_key(static_cast<x5::cms::EX5ZoneKey>(kZoneKey));
  refused.set_contract_type(x5::cms::EX5HousingContractType_MonthlyContract);
  refused.set_area_name("Harbor");
  EXPECT_TRUE(send(resident_, guild_housing::kAreaContractRequest, refused).empty());
  const auto refusals = sent_to(resident_.char_id, guild_housing::kAreaContractResponse);
  ASSERT_EQ(refusals.size(), 1u);
  EXPECT_EQ(decode<wire::GuildHousingAreaContractRes>(refusals.front(),
                                                      guild_housing::kAreaContractResponse)
                .ec(),
            x5::ec::Guild_NoPermission);

  const auto contracted = contract_area();
  ASSERT_EQ(contracted.ec(), x5::ec::Success);
  EXPECT_EQ(contracted.area_name(), "Harbor");
  EXPECT_EQ(static_cast<int>(contracted.zone_key()), kZoneKey);
  EXPECT_GT(contracted.expiry_epochtime(), now_);

  const auto notices = sent_to(resident_.char_id, guild_housing::kAreaContractNotification);
  ASSERT_EQ(notices.size(), 1u);
  const auto notice = decode<wire::GuildHousingAreaContractNoti>(
      notices.front(), guild_housing::kAreaContractNotification);
  EXPECT_EQ(notice.guild_id(), guild_id_);
  EXPECT_EQ(notice.guild_channel(), contracted.guild_channel());
  EXPECT_TRUE(sent_to(master_.char_id, guild_housing::kAreaContractNotification).empty());

  const auto area = coordinator_->housing_area(guild_id_);
  ASSERT_TRUE(area.has_value());
  EXPECT_EQ(area->area_name(), "Harbor");
  EXPECT_EQ(area->housing_channel(), contracted.guild_channel());

  // The extension window has not opened yet.
  wire::GuildHousingAreaContractExtensionReq extend;
  extend.set_guild_id(guild_id_);
  extend.set_contract_type(x5::cms::EX5HousingContractType_MonthlyContract);
  EXPECT_TRUE(send(master_, guild_housing::kAreaContractExtensionRequest, extend).empty());
  const auto extensions =
      sent_to(master_.char_id, guild_housing::kAreaContractExtensionResponse);
  ASSERT_EQ(extensions.size(), 1u);
  EXPECT_NE(decode<wire::GuildHousingAreaContractExtensionRes>(
                extensions.front(), guild_housing::kAreaContractExtensionResponse)
                .ec(),
            x5::ec::Success);
}

TEST_F(GuildHousingTest, ServerGuildDirectoryReportsRosterCompanyPositions) {
  // The server wires GuildServiceDirectory into HousingService; guild_allow reads each
  // resident's company position from it.
  const GuildServiceDirectory directory(guilds_, repositories_.characters);
  guilds_->refresh_member(actor(resident_, 4));
  guilds_->refresh_member(actor(master_, 5));

  const auto members = directory.members(guild_id_);

  ASSERT_EQ(members.size(), 2u);
  std::map<housing::CharacterId, int> positions;
  for (const auto &member : members) {
    positions[member.character_id] = member.company_position_level;
  }
  EXPECT_EQ(positions.at(resident_.char_id), 4);
  EXPECT_EQ(positions.at(master_.char_id), 5);
}

TEST_F(GuildHousingTest, ResidentsNeedTheCompanyPositionOfTheLotSize) {
  ASSERT_EQ(contract_area().ec(), x5::ec::Success);

  guilds_->refresh_member(actor(resident_, 2));
  EXPECT_EQ(allow_resident().ec(), x5::ec::Housing_InsufficientCompanyPosition);

  guilds_->refresh_member(actor(resident_, 3));
  const auto allowed = allow_resident();
  ASSERT_EQ(allowed.ec(), x5::ec::Success);
  ASSERT_EQ(allowed.residents_size(), 1);
  EXPECT_EQ(allowed.residents(0).char_id(), resident_.char_id);
  EXPECT_EQ(allowed.residents(0).grade(), x5::cms::EX5GuildMemberGrade_Member);
  EXPECT_TRUE(housing_->guild_site_of(resident_.char_id).has_value());

  wire::GuildHousingSiteAreaBoardGetReq board;
  board.set_guild_id(guild_id_);
  const auto listed = decode<wire::GuildHousingSiteAreaBoardGetRes>(
      send(resident_, guild_housing::kSiteAreaBoardGetRequest, board),
      guild_housing::kSiteAreaBoardGetResponse);
  ASSERT_EQ(listed.ec(), x5::ec::Success);
  ASSERT_EQ(listed.residents_size(), 1);
  EXPECT_EQ(listed.residents(0).name(), "Resident");

  wire::GuildHousingSiteAssignmentReq assignment;
  assignment.set_guild_id(guild_id_);
  const auto assigned = decode<wire::GuildHousingSiteAssignmentRes>(
      send(master_, guild_housing::kSiteAssignmentRequest, assignment),
      guild_housing::kSiteAssignmentResponse);
  ASSERT_EQ(assigned.exist_housing_site_char_ids_size(), 1);
  EXPECT_EQ(assigned.exist_housing_site_char_ids(0), resident_.char_id);
}

TEST_F(GuildHousingTest, LeavingMembersLoseTheirSiteAndDisbandingReleasesTheArea) {
  ASSERT_EQ(contract_area().ec(), x5::ec::Success);
  ASSERT_EQ(allow_resident().ec(), x5::ec::Success);
  sent_.clear();

  ASSERT_EQ(guilds_->leave(actor(resident_), guild_id_).error, x5::ec::Success);
  EXPECT_FALSE(housing_->guild_site_of(resident_.char_id).has_value());
  EXPECT_EQ(sent_to(master_.char_id, guild_housing::kAreaUpdateNotification).size(), 1u);

  ASSERT_EQ(guilds_->gm_disband(guild_id_), x5::ec::Success);
  EXPECT_FALSE(housing_->guild_area(guild_id_).has_value());
  EXPECT_FALSE(coordinator_->housing_area(guild_id_).has_value());
}

// Regression: guild changes commit before their housing cleanup runs. In 0.2.5 a cleanup that
// failed, timed out or was still queued at a stop was dropped for good, leaving a departed
// member's guild site and a disbanded guild's area behind.
TEST_F(GuildHousingTest, ReconciliationReleasesHousingWhoseGuildCleanupWasLost) {
  ASSERT_EQ(contract_area().ec(), x5::ec::Success);
  ASSERT_EQ(allow_resident().ec(), x5::ec::Success);
  housing::HousingHub hub(housing_);

  // Current members keep everything.
  EXPECT_EQ(hub.reconcile_guild_housing(), 0u);
  EXPECT_TRUE(housing_->guild_site_of(resident_.char_id).has_value());
  EXPECT_TRUE(housing_->guild_area(guild_id_).has_value());

  deliver_guild_events_ = false;
  ASSERT_EQ(guilds_->leave(actor(resident_), guild_id_).error, x5::ec::Success);
  ASSERT_TRUE(housing_->guild_site_of(resident_.char_id).has_value());
  EXPECT_EQ(hub.reconcile_guild_housing(), 0u);
  EXPECT_FALSE(housing_->guild_site_of(resident_.char_id).has_value());
  const auto area = housing_->guild_area(guild_id_);
  ASSERT_TRUE(area.has_value());
  EXPECT_EQ(area->residents.at(housing::testing_support::kSmallLot).character_id, 0);

  ASSERT_EQ(guilds_->gm_disband(guild_id_), x5::ec::Success);
  ASSERT_TRUE(housing_->guild_area(guild_id_).has_value());
  EXPECT_EQ(hub.reconcile_guild_housing(), 0u);
  EXPECT_FALSE(housing_->guild_area(guild_id_).has_value());

  // Repeating the sweep, or a late delivery of the lost events, changes nothing.
  EXPECT_EQ(hub.reconcile_guild_housing(), 0u);
  EXPECT_TRUE(hub.guild_member_left(guild_id_, resident_.char_id));
  EXPECT_TRUE(hub.guild_disbanded(guild_id_));
  EXPECT_TRUE(housing_->stale_guild_housing().empty());
}

// Regression: in 0.2.5 an exception from a cleanup on the housing event thread escaped an
// asio::thread_pool handler, which calls std::terminate and stops the server.
TEST_F(GuildHousingTest, StorageFailuresDuringGuildCleanupAreContainedAndRetried) {
  ASSERT_EQ(contract_area().ec(), x5::ec::Success);
  ASSERT_EQ(allow_resident().ec(), x5::ec::Success);
  housing::HousingHub hub(housing_);
  deliver_guild_events_ = false;
  ASSERT_EQ(guilds_->leave(actor(resident_), guild_id_).error, x5::ec::Success);
  ASSERT_EQ(guilds_->gm_disband(guild_id_), x5::ec::Success);

  housing_store_->unavailable = true;
  bool released_site = true;
  bool released_area = true;
  EXPECT_NO_THROW(released_site = hub.guild_member_left(guild_id_, resident_.char_id));
  EXPECT_NO_THROW(released_area = hub.guild_disbanded(guild_id_));
  EXPECT_FALSE(released_site);
  EXPECT_FALSE(released_area);
  // The guild is gone, so the sweep releases the departed site together with the area.
  EXPECT_EQ(hub.reconcile_guild_housing(), 1u);
  EXPECT_TRUE(housing_->guild_site_of(resident_.char_id).has_value());
  EXPECT_TRUE(housing_->guild_area(guild_id_).has_value());

  housing_store_->unavailable = false;
  EXPECT_EQ(hub.reconcile_guild_housing(), 0u);
  EXPECT_FALSE(housing_->guild_site_of(resident_.char_id).has_value());
  EXPECT_FALSE(housing_->guild_area(guild_id_).has_value());
}

TEST_F(GuildHousingTest, MalformedOrUnknownRequestsThrow) {
  const std::string malformed{"\x05\x00\x42\x4e\x80", 5};
  const auto frames = native_ws::decode_frames(malformed);
  EXPECT_THROW(coordinator_->handle(requester(master_), frames.front()), std::invalid_argument);

  wire::GuildHousingSiteAreaBoardGetReq board;
  EXPECT_THROW(send(master_, 20039, board), std::invalid_argument);
  EXPECT_THROW(guild_housing::response_id(20033), std::invalid_argument);
  EXPECT_EQ(guild_housing::response_id(guild_housing::kSiteAssignmentRequest),
            guild_housing::kSiteAssignmentResponse);
}

TEST(GuildHousingFront, RequestsRouteOnlyWhenGuildHousingIsEnabled) {
  MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories(), Config{});
  auto guilds =
      std::make_shared<GuildService>(std::make_shared<MemoryGuildStore>(), GuildRules{}, kWorldId);
  auto housing = std::make_shared<housing::HousingService>(
      housing::testing_support::housing_catalog(),
      std::make_shared<housing::MemoryHousingStore>(repositories.character_state),
      kWorldId,
      [] { return kStartTicks; }, housing::HousingPolicy{},
      std::make_shared<LegacyGuildServiceDirectory>(guilds));
  auto coordinator = std::make_shared<GuildHousingCoordinator>(housing, guilds);
  NativeChatService chat(auth);

  NativeGuildFront without_housing(auth, *guilds, chat);
  EXPECT_FALSE(without_housing.handles(guild_housing::kAreaContractRequest));
  EXPECT_TRUE(without_housing.handles(native_guild::kJoinRequest));

  NativeGuildFront with_housing(auth, *guilds, chat, nullptr, coordinator);
  for (std::uint16_t packet_id = guild_housing::kAreaContractRequest;
       packet_id <= guild_housing::kAreaContractExtensionRequest; ++packet_id) {
    EXPECT_TRUE(with_housing.handles(packet_id)) << packet_id;
  }
  EXPECT_FALSE(with_housing.handles(guild_housing::kAreaContractExtensionRequest + 1));
}

} // namespace
} // namespace chronicles
