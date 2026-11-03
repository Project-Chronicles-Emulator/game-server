#pragma once

// Channel event status and tribute offerings for the world transport.
//
// Evidence (build 40367e24...):
// - Opcodes from protocol/opcodes.json: PC2S 20062 ChannelEventTributeCountReq (empty body),
//   20081 ChannelEventItemTributeReq {item_cms_id, count, target_unit_id, packet_seq} and
//   20082 ChannelTriggerEntryReq {map_id, pos, body_yaw}; PS2C 117 ChangeChannelEventStatusNoti
//   {status_type} and 118 ChannelEventTributeCountNoti {count, target_count}.
// - Official capture P2: interaction 312 on a quest doodad (phase 2, cut scene 301006) was
//   followed in the same server batch by ChangeChannelEventStatusNoti Success, then the cut
//   scene and the Zone_01_POI_004 visit. Interaction, dialogue and map conditions test
//   CheckChannelEventStatus Success on the caster, so the status gates later content.
// - Doodad 49054 (Tribute property, item 100194) offers interaction 401, which
//   InteractionUseOpenUi maps to OpenSharedOfferingForChannelEvent; ItemsUpdateNoti has a
//   Reason_Tribute; EErrorCode Tribute_LimitReached/InvalidTarget/TargetTooFar/InvalidItem.
// - Channel objectives, tasks, tribute targets and rewards are server-only data: the client
//   ChannelEvent table holds IDs and reward links only. The tribute target, offering range,
//   audience and in-memory lifetime used here are emulator choices (Candidate).
//
// Placements and map bindings come from tools/build_channel_event_data.py.

#include "protocol/gameplay_packet.hpp"
#include "persistence/character_state_types.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chronicles::channel_event {

inline constexpr std::uint16_t kTributeCountRequest = 20062;
inline constexpr std::uint16_t kItemTributeRequest = 20081;
inline constexpr std::uint16_t kChannelTriggerEntryRequest = 20082;
inline constexpr std::uint16_t kChangeChannelEventStatusNotification = 117;
inline constexpr std::uint16_t kTributeCountNotification = 118;
inline constexpr std::uint16_t kCommonErrorResponse = 2;
inline constexpr std::uint16_t kItemsUpdateNotification = 169;

// Emulator choices; the official values are server-only data.
inline constexpr int kTributeTarget = 10;
inline constexpr double kTributeRangeMm = 10000;
inline constexpr int kMaximumTributePerRequest = 100;

// x5.cms.EX5ChannelEventStatusType.
enum class EventStatus {
  none = 0,
  fail = 1,
  success = 2,
  fail_timeout = 3,
  raid_success = 4,
  channel_close = 5,
};

const char *status_name(EventStatus status);

struct ChannelEventBinding {
  int map_id{};
  int channel_event_id{};
  std::optional<int> reward_id;
};

struct TributeAltar {
  int map_id{};
  std::uint64_t unit_id{};
  int doodad_cms_id{};
  int item_cms_id{};
  std::array<int, 3> position_mm{};
};

// Immutable bindings and placements built by tools/build_channel_event_data.py.
class ChannelEventData {
public:
  static std::shared_ptr<const ChannelEventData> load(const std::filesystem::path &path);

  explicit ChannelEventData(const nlohmann::json &document);

  const ChannelEventBinding *binding(int map_id) const;

  const TributeAltar *altar(int map_id, std::uint64_t unit_id) const;

  // The closest altar on the position's map within `range_mm`, if any.
  const TributeAltar *nearest_altar(const WorldPosition &position, double range_mm) const;

  std::size_t altar_count() const;

  std::size_t binding_count() const;

private:
  std::unordered_map<int, ChannelEventBinding> bindings_;
  std::map<std::pair<int, std::uint64_t>, TributeAltar> altars_;
};

struct TributeProgress {
  int count{};
  int target{};
};

enum class TributeCommitStatus { committed, limit_reached, storage_conflict };

struct TributeCommit {
  TributeCommitStatus status{TributeCommitStatus::storage_conflict};
  TributeProgress progress;
  bool completed_event{};
};

// Shared, in-memory channel state: one event status per map and one offering ledger per altar.
// It resets with the server process. Thread-safe; every method takes one internal lock.
class ChannelEventBoard {
public:
  explicit ChannelEventBoard(int tribute_target = kTributeTarget);

  TributeProgress tribute_progress(int map_id, std::uint64_t altar_unit_id) const;

  // Serializes the capacity check, item commit and ledger publication against other offerings
  // and resets. The callback commits only the player's inventory; it must not re-enter this
  // board or acquire the protocol mutex. False or an exception leaves count/status unchanged.
  // Database durability covers inventory only: this in-memory board is not crash-atomic with it.
  TributeCommit commit_tribute(int map_id, std::uint64_t altar_unit_id, int count,
                               const std::function<bool()> &commit_items);

  EventStatus status(int map_id) const;

  // Returns true when the status changed.
  bool set_status(int map_id, EventStatus status);

  // Clears the map's status and every altar ledger on it.
  void reset(int map_id);

private:
  int tribute_target_;
  mutable std::mutex mutex_;
  std::unordered_map<int, EventStatus> statuses_;
  std::map<std::pair<int, std::uint64_t>, int> offerings_;
};

// A validated ChannelEventItemTributeReq. The caller serializes its item commit through the board.
struct TributePlan {
  // EErrorCode name when the request is refused.
  std::optional<std::string> error;
  const TributeAltar *altar{};
  int count{};
};

TributePlan plan_tribute(const ChannelEventData &data, const nlohmann::json &request,
                         const WorldPosition &position, const WorldProgress &progress);

// Progress after removing the offered items; `plan` must have no error.
WorldProgress consume_tribute(const WorldProgress &before, const TributePlan &plan);

GameplayPacket tribute_count_packet(const TributeProgress &progress);

GameplayPacket status_packet(EventStatus status);

// ItemsUpdateNoti with Reason_Tribute for the committed stack.
GameplayPacket tribute_items_packet(const TributePlan &plan, const WorldProgress &after,
                                    const nlohmann::json &request, std::uint32_t sequence);

GameplayPacket common_error(std::string_view code, std::uint32_t sequence);

// True when the request's `pos` (WorldPos millimeters) lies within `range_mm` of the server's
// position. Raid and channel entry requests report the client position; the server keeps its own.
bool reported_position_near(const nlohmann::json &request, const WorldPosition &server_position,
                            double range_mm);

} // namespace chronicles::channel_event
