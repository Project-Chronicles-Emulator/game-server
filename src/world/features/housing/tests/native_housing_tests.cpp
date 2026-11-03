#include "auth/auth.hpp"
#include "testing/company_test_support.hpp"
#include "testing/memory_repositories.hpp"
#include "services/front/native_ws.hpp"
#include "testing/housing_test_support.hpp"
#include "world/features/company/company.hpp"
#include "world/features/housing/native_housing.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

TEST(NativeHousing, CharacterSelectionRestoresOwnedSiteAndHousingChannelContext) {
  using namespace housing;
  using namespace housing::testing_support;
  MemoryRepositories store;
  Config config;
  AuthService auth(store.character_repositories(), config);
  ASSERT_TRUE(auth.create_account("housing_entry", "local-housing-test-password"));
  const auto token = auth.login("housing_entry", "local-housing-test-password").access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  ASSERT_EQ(auth.enter(token, enter).error_code(), x5::ec::Success);
  x5::pb::ws::front::CharCreateReq create;
  create.set_slot(0);
  create.set_char_name("HousingEntry");
  create.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto created = auth.create(token, create);
  ASSERT_EQ(created.ec(), x5::ec::Success);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(created.char_().char_id());
  ASSERT_EQ(auth.select(token, select).ec(), x5::ec::Success);
  const auto character = auth.selected_character(token);
  ASSERT_TRUE(character);

  const HousingActor actor{character->account_id, character->id, 1, character->name,
                           WorldPosition{8000, 0, 0, 0, 0}};
  const auto before =
      store.character_state->load_world_progress(actor.account_id, actor.character_id, 1);
  auto after = before;
  ++after.revision;
  after.stackables[kPermitItem] = 60;
  ASSERT_TRUE(store.character_state->save_world_progress(actor.account_id, actor.character_id, 1,
                                                         before, after));
  auto service = std::make_shared<HousingService>(
      housing_catalog(), std::make_shared<MemoryHousingStore>(store.character_state), 1,
      [] { return 638000000000000000LL; });
  ASSERT_EQ(service->contract(actor, SiteType::small, kZoneKey, ContractType::monthly, nullptr).ec,
            x5::ec::Success);
  ASSERT_TRUE(store.characters->save_world_position(actor.account_id, actor.character_id, 1,
                                         WorldPosition{kHousingMapId, 1000, 2000, 3000, 0}));
  auto hub = std::make_shared<HousingHub>(service);
  NativeWsDispatcher dispatcher(auth, {}, hub);
  const auto frame = native_ws::encode_frame(native_ws::kCharacterSelectRequest, select);
  const auto wire = dispatcher.handle(NativeWsChannel::front, token, frame);
  const auto frames = native_ws::decode_frames(wire);
  ASSERT_EQ(frames.size(), 1U);
  x5::pb::frontwebsocket::CharSelectRes response;
  ASSERT_TRUE(response.ParseFromArray(frames[0].payload.data(),
                                      static_cast<int>(frames[0].payload.size())));
  ASSERT_EQ(response.ec(), x5::ec::Success);
  const auto &entry = response.character();
  EXPECT_EQ(entry.housing_site().lot_number(), kSmallLot);
  EXPECT_EQ(entry.housing_site().channel(), 1);
  EXPECT_EQ(entry.map_id(), kHousingMapId);
  EXPECT_EQ(entry.entry().housing_channel_id(), 1);
  EXPECT_GT(entry.entry().housing_version(), 0);
  EXPECT_FALSE(entry.entry().housing_visit());

  // Proto3 can omit a zero/unset channel while reopening the board. A known live housing
  // location supplies that context; explicit channel requests remain untouched.
  hub->attach(actor, 1, static_cast<std::uint64_t>(actor.character_id), {});
  hub->set_location(actor.character_id, 1, HousingLocation{kZoneKey, 1});
  x5::pb::ws::front::HousingSiteAreaBoardGetReq board_request;
  const auto board_frame =
      native_ws::encode_frame(native_housing::kAreaBoardRequest, board_request);
  const auto board_wire = dispatcher.handle(NativeWsChannel::front, token, board_frame);
  const auto board_frames = native_ws::decode_frames(board_wire);
  ASSERT_EQ(board_frames.size(), 1U);
  x5::pb::frontwebsocket::HousingSiteAreaBoardGetRes board;
  ASSERT_TRUE(board.ParseFromArray(board_frames[0].payload.data(),
                                   static_cast<int>(board_frames[0].payload.size())));
  ASSERT_EQ(board.ec(), x5::ec::Success);
  ASSERT_EQ(board.residents_size(), 1);
  EXPECT_EQ(board.residents(0).lot_number(), kSmallLot);

  board_request.set_channel(999);
  const auto invalid_wire =
      dispatcher.handle(NativeWsChannel::front, token,
                        native_ws::encode_frame(native_housing::kAreaBoardRequest, board_request));
  const auto invalid_frames = native_ws::decode_frames(invalid_wire);
  ASSERT_TRUE(board.ParseFromArray(invalid_frames[0].payload.data(),
                                   static_cast<int>(invalid_frames[0].payload.size())));
  EXPECT_EQ(board.ec(), x5::ec::Housing_ChannelNotFound);
}

// The housing NPC (Nivhen) opens its housing window from the public world with an empty
// WSC2F_HousingSiteAreaBoardGetReq. These tests drive the front requests through the
// dispatcher for a selected character that stands outside any housing area.
class NativeHousingFrontTest : public testing::Test {
protected:
  static constexpr int kWorldId = 1;
  static constexpr int kPublicMapId = 8000;
  static constexpr std::int64_t kNowTicks = 638'000'000'000'000'000;

  void SetUp() override {
    ASSERT_TRUE(auth.create_account("housing_board", "local-housing-test-password"));
    token = auth.login("housing_board", "local-housing-test-password").access_token();
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(kWorldId);
    ASSERT_EQ(auth.enter(token, enter).error_code(), x5::ec::Success);

    x5::pb::ws::front::CharCreateReq create;
    create.set_slot(0);
    create.set_char_name("BoardReader");
    create.set_char_class(x5::cms::EX5CharacterClass_Fight);
    const auto created = auth.create(token, create);
    ASSERT_EQ(created.ec(), x5::ec::Success);

    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(created.char_().char_id());
    ASSERT_EQ(auth.select(token, select).ec(), x5::ec::Success);
    const auto character = auth.selected_character(token);
    ASSERT_TRUE(character);
    reader = housing::HousingActor{character->account_id, character->id, kWorldId,
                                   character->name,
                                   WorldPosition{kPublicMapId, 1000, 2000, 3000, 0.0F}};

    service = std::make_shared<housing::HousingService>(
        housing::testing_support::housing_catalog(),
        std::make_shared<housing::MemoryHousingStore>(store.character_state), kWorldId,
        [] { return kNowTicks; });
    hub = std::make_shared<housing::HousingHub>(service);
    dispatcher =
        std::make_unique<NativeWsDispatcher>(auth, std::shared_ptr<GuildService>{}, hub);
  }

  // A funded resident with its own account that rents one small lot. The test catalog has two
  // small lots, so the third small rental opens channel 2.
  housing::ContractResult rent_small_lot(const std::string &name) {
    Character created;
    created.account_id = next_account_id++;
    created.name = name;
    const auto character = store.characters->create(created);
    const housing::HousingActor resident{created.account_id, character.id, kWorldId, name,
                                         WorldPosition{kPublicMapId, 0, 0, 0, 0.0F}};

    const auto before = store.character_state->load_world_progress(
        resident.account_id, resident.character_id, kWorldId);
    auto funded = before;
    funded.revision = before.revision + 1;
    funded.stackables[housing::testing_support::kPermitItem] = 60;
    EXPECT_TRUE(store.character_state->save_world_progress(
        resident.account_id, resident.character_id, kWorldId, before, funded));

    return service->contract(resident, housing::SiteType::small,
                             housing::testing_support::kZoneKey, housing::ContractType::monthly,
                             nullptr);
  }

  x5::pb::frontwebsocket::HousingSiteAreaBoardGetRes
  read_board(const x5::pb::ws::front::HousingSiteAreaBoardGetReq &request) {
    const auto request_frame = native_ws::encode_frame(native_housing::kAreaBoardRequest, request);
    const auto wire = dispatcher->handle(NativeWsChannel::front, token, request_frame);
    const auto frames = native_ws::decode_frames(wire);

    x5::pb::frontwebsocket::HousingSiteAreaBoardGetRes response;
    EXPECT_EQ(frames.size(), 1U);
    if (frames.size() != 1U) {
      return response;
    }
    EXPECT_EQ(frames[0].packet_id, native_housing::kAreaBoardResponse);
    EXPECT_TRUE(response.ParseFromArray(frames[0].payload.data(),
                                        static_cast<int>(frames[0].payload.size())));
    return response;
  }

  x5::pb::frontwebsocket::HousingSiteAreaBoardGetRes read_board_without_channel() {
    return read_board(x5::pb::ws::front::HousingSiteAreaBoardGetReq{});
  }

  x5::pb::frontwebsocket::HousingSiteAreaBoardGetRes read_board_channel(int channel) {
    x5::pb::ws::front::HousingSiteAreaBoardGetReq request;
    request.set_channel(channel);
    return read_board(request);
  }

  MemoryRepositories store;
  Config config;
  AuthService auth{store.character_repositories(), config};
  std::string token;
  housing::HousingActor reader;
  std::shared_ptr<housing::HousingService> service;
  std::shared_ptr<housing::HousingHub> hub;
  std::unique_ptr<NativeWsDispatcher> dispatcher;
  std::int64_t next_account_id = 9000;
};

TEST_F(NativeHousingFrontTest, PublicWorldBoardWithoutChannelReadsEmptyPrimaryChannel) {
  ASSERT_FALSE(hub->location_of(reader.character_id).has_value());

  const auto board = read_board_without_channel();

  EXPECT_EQ(board.ec(), x5::ec::Success);
  EXPECT_EQ(board.residents_size(), 0);
}

TEST_F(NativeHousingFrontTest, PublicWorldBoardWithoutChannelListsPrimaryChannelResidents) {
  const auto first = rent_small_lot("FirstResident");
  const auto second = rent_small_lot("SecondResident");
  ASSERT_EQ(first.ec, x5::ec::Success);
  ASSERT_EQ(second.ec, x5::ec::Success);
  ASSERT_EQ(first.channel, housing::kPrimaryPersonalChannel);
  ASSERT_EQ(second.channel, housing::kPrimaryPersonalChannel);

  const auto board = read_board_without_channel();

  ASSERT_EQ(board.ec(), x5::ec::Success);
  ASSERT_EQ(board.residents_size(), 2);
  EXPECT_EQ(board.residents(0).lot_number(), housing::testing_support::kSmallLot);
  EXPECT_EQ(board.residents(0).name(), "FirstResident");
  EXPECT_EQ(board.residents(1).lot_number(), housing::testing_support::kSecondSmallLot);
  EXPECT_EQ(board.residents(1).name(), "SecondResident");
}

TEST_F(NativeHousingFrontTest, LiveHousingChannelTakesPrecedenceOverPrimaryChannel) {
  ASSERT_EQ(rent_small_lot("FirstResident").ec, x5::ec::Success);
  ASSERT_EQ(rent_small_lot("SecondResident").ec, x5::ec::Success);
  const auto overflow = rent_small_lot("OverflowResident");
  ASSERT_EQ(overflow.ec, x5::ec::Success);
  ASSERT_EQ(overflow.channel, 2);

  hub->attach(reader, 1, static_cast<std::uint64_t>(reader.character_id), {});
  hub->set_location(reader.character_id, 1,
                    housing::HousingLocation{housing::testing_support::kZoneKey, 2});

  const auto board = read_board_without_channel();

  ASSERT_EQ(board.ec(), x5::ec::Success);
  ASSERT_EQ(board.residents_size(), 1);
  EXPECT_EQ(board.residents(0).name(), "OverflowResident");

  // An explicit channel still wins over the live location.
  const auto primary = read_board_channel(housing::kPrimaryPersonalChannel);
  ASSERT_EQ(primary.ec(), x5::ec::Success);
  EXPECT_EQ(primary.residents_size(), 2);
}

TEST_F(NativeHousingFrontTest, ExplicitInvalidChannelsAreStillRejected) {
  ASSERT_EQ(rent_small_lot("FirstResident").ec, x5::ec::Success);

  EXPECT_EQ(read_board_channel(999).ec(), x5::ec::Housing_ChannelNotFound);
  EXPECT_EQ(read_board_channel(-1).ec(), x5::ec::Housing_ChannelNotFound);
  // Channel 2 opens only when the primary channel has no free lot of the rented size.
  EXPECT_EQ(read_board_channel(2).ec(), x5::ec::Housing_ChannelNotFound);
}

TEST_F(NativeHousingFrontTest, BoardReadsLeavePendingTravelAndContractsUnchanged) {
  ASSERT_EQ(rent_small_lot("FirstResident").ec, x5::ec::Success);
  ASSERT_EQ(rent_small_lot("SecondResident").ec, x5::ec::Success);
  const auto overflow = rent_small_lot("OverflowResident");
  ASSERT_EQ(overflow.ec, x5::ec::Success);
  const housing::SiteKey overflow_key{housing::testing_support::kZoneKey, overflow.channel,
                                      overflow.lot_number};
  const auto site_before = service->site(overflow_key);
  const auto channel_before =
      service->channel_record(housing::testing_support::kZoneKey, overflow.channel);
  ASSERT_TRUE(site_before.has_value());
  ASSERT_TRUE(channel_before.has_value());

  // A pending visit to channel 2 that the next housing-map entry must still consume.
  const auto visit = service->visit(reader, housing::testing_support::kZoneKey, 2);
  ASSERT_EQ(visit.ec, x5::ec::Success);

  for (int attempt = 0; attempt < 3; ++attempt) {
    EXPECT_EQ(read_board_without_channel().ec(), x5::ec::Success);
    EXPECT_EQ(read_board_channel(2).ec(), x5::ec::Success);
    EXPECT_EQ(read_board_channel(999).ec(), x5::ec::Housing_ChannelNotFound);
  }

  const auto site_after = service->site(overflow_key);
  const auto channel_after =
      service->channel_record(housing::testing_support::kZoneKey, overflow.channel);
  ASSERT_TRUE(site_after.has_value());
  ASSERT_TRUE(channel_after.has_value());
  EXPECT_EQ(site_after->version, site_before->version);
  EXPECT_EQ(site_after->expiry_epoch_ticks, site_before->expiry_epoch_ticks);
  EXPECT_EQ(site_after->owner_character_id, site_before->owner_character_id);
  EXPECT_EQ(channel_after->version, channel_before->version);
  EXPECT_FALSE(service->personal_site(reader.character_id).has_value());

  const auto destination = service->take_destination(reader.character_id,
                                                     housing::testing_support::kHousingMapId);
  ASSERT_TRUE(destination.has_value());
  EXPECT_EQ(destination->channel, 2);
  ASSERT_TRUE(destination->origin.has_value());
  EXPECT_EQ(destination->origin->map_id, kPublicMapId);
}

TEST_F(NativeHousingFrontTest, PersonalContractUsesTheRequesterCompanyPosition) {
  auto companies = std::make_shared<CompanyService>(std::make_shared<MemoryCompanyStore>(),
                                                    testing_support::company_rules(), kWorldId);
  testing_support::seat_in_company(*companies, reader.character_id, 2,
                                   testing_support::kBlueSalt);
  NativeWsDispatcher with_companies(auth, std::shared_ptr<GuildService>{}, hub, {}, companies);

  const auto before =
      store.character_state->load_world_progress(reader.account_id, reader.character_id, kWorldId);
  auto funded = before;
  funded.revision = before.revision + 1;
  funded.stackables[housing::testing_support::kPermitItem] = 60;
  ASSERT_TRUE(store.character_state->save_world_progress(reader.account_id, reader.character_id,
                                                         kWorldId, before, funded));

  x5::pb::ws::front::HousingSiteContractReq request;
  request.set_type(x5::cms::EX5HousingSiteType_Small);
  request.set_zone_key(static_cast<x5::cms::EX5ZoneKey>(housing::testing_support::kZoneKey));
  request.set_contract_type(x5::cms::EX5HousingContractType_MonthlyContract);
  const auto contract = [&] {
    const auto wire = with_companies.handle(
        NativeWsChannel::front, token,
        native_ws::encode_frame(native_housing::kSiteContractRequest, request));
    const auto frames = native_ws::decode_frames(wire);
    x5::pb::frontwebsocket::HousingSiteContractRes response;
    EXPECT_EQ(frames.size(), 1U);
    if (frames.size() == 1U) {
      EXPECT_EQ(frames[0].packet_id, native_housing::kSiteContractResponse);
      EXPECT_TRUE(response.ParseFromArray(frames[0].payload.data(),
                                          static_cast<int>(frames[0].payload.size())));
    }
    return response;
  };

  EXPECT_EQ(contract().ec(), x5::ec::Housing_InsufficientCompanyPosition);
  EXPECT_FALSE(service->personal_site(reader.character_id).has_value());

  testing_support::seat_in_company(*companies, reader.character_id, 3,
                                   testing_support::kBlueSalt);
  const auto rented = contract();
  ASSERT_EQ(rented.ec(), x5::ec::Success);
  EXPECT_EQ(rented.lot_number(), housing::testing_support::kSmallLot);
  EXPECT_EQ(rented.channel(), housing::kPrimaryPersonalChannel);
  EXPECT_TRUE(service->personal_site(reader.character_id).has_value());
}

} // namespace
} // namespace chronicles
