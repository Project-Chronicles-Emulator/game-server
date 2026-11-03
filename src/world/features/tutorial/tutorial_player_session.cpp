#include "world/features/tutorial/tutorial_player_session.hpp"

#include "auth/auth.hpp"
#include "common/clock_ticks.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/inventory/inventory_items.hpp"
#include "world/features/inventory/utility_bag.hpp"

#include <iterator>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace chronicles::tutorial {
namespace {

constexpr std::uint16_t kTimeSyncRequest = 20001;
constexpr std::uint16_t kCutSceneEndRequest = 20055;
constexpr std::uint16_t kCommonErrorResponse = 2;
using world_request::kChangeSkillStageRequest;
using world_request::kExecuteSpawnProjectileRequest;
using world_request::kMapLoadCompleteRequest;
using world_request::kReviveRequest;
using world_request::kSkillEffectReportRequest;
using world_request::kStartSkillRequest;

} // namespace

TutorialRewardPersistence
tutorial_reward_persistence(AuthService &auth, const Handoff &handoff,
                            std::function<void(const Character &)> items_changed) {
  TutorialRewardPersistence persistence;
  persistence.load_character = [&auth, handoff] {
    return auth.world_character(handoff);
  };
  persistence.commit = [&auth, handoff](const WorldProgress &before, const WorldProgress &after,
                                        const ItemTrade &trade) {
    return auth.commit_world_item_trade(handoff, before, after, trade);
  };
  persistence.items_changed = std::move(items_changed);
  return persistence;
}

TutorialSession::InstanceCommitResult
commit_tutorial_instances(TutorialRewardPersistence &persistence, const WorldProgress &before,
                          const WorldProgress &after, const std::vector<CharacterItem> &granted) {
  using Status = TutorialSession::InstanceCommitStatus;
  TutorialSession::InstanceCommitResult result;

  const auto character = persistence.load_character();
  if (!character) {
    result.status = Status::conflict;
    return result;
  }
  const auto free_slots = lowest_free_inventory_slots(*character, granted.size());
  if (!free_slots) {
    result.status = Status::inventory_full;
    return result;
  }

  std::vector<CharacterItem> rows = granted;
  const auto created_epoch_ticks = unix_epoch_ticks_now();
  for (std::size_t index = 0; index < rows.size(); ++index) {
    rows[index].inventory_slot = (*free_slots)[index];
    rows[index].created_epoch_ticks = created_epoch_ticks;
  }

  auto stored = persistence.commit(before, after, ItemTrade{rows, {}, {}});
  if (!stored) {
    result.status = Status::conflict;
    return result;
  }

  // Keep the cached world snapshot's item list equal to the committed inventory.
  Character live = *character;
  live.items.insert(live.items.end(), stored->begin(), stored->end());
  persistence.items_changed(live);

  result.status = Status::committed;
  result.stored = std::move(*stored);
  return result;
}

TutorialPlayerSession::TutorialPlayerSession(const TutorialFeature &feature,
                                             TutorialSessionContext &context)
    : feature_(feature), context_(context) {
}

void TutorialPlayerSession::initialize(std::int64_t wire_unit_id, WorldProgress progress,
                                       TutorialSession::Commit commit,
                                       std::shared_ptr<const Progression> progression,
                                       std::shared_ptr<const TutorialData> followup_data) {
  session_ = std::make_unique<TutorialSession>(followup_data ? followup_data : feature_.data(),
                                               wire_unit_id, std::move(progress), std::move(commit),
                                               std::move(progression));
}

void TutorialPlayerSession::bind_instance_rewards(TutorialRewardPersistence persistence) {
  session_->set_instance_commit([persistence = std::move(persistence)](
                                    const WorldProgress &before, const WorldProgress &after,
                                    const std::vector<CharacterItem> &granted) mutable {
    return commit_tutorial_instances(persistence, before, after, granted);
  });
}

bool TutorialPlayerSession::has_instance() const noexcept {
  return static_cast<bool>(session_);
}

TutorialSession *TutorialPlayerSession::current_instance() noexcept {
  return session_.get();
}

const TutorialSession *TutorialPlayerSession::current_instance() const noexcept {
  return session_.get();
}

void TutorialPlayerSession::reset_instance() {
  session_.reset();
}

void TutorialPlayerSession::reset_bootstrap_flags() {
  snapshot_sent_ = false;
  sectors_sent_ = false;
}

void TutorialPlayerSession::mark_bootstrap_sent() {
  snapshot_sent_ = true;
  sectors_sent_ = true;
}

std::vector<GameplayPacket> TutorialPlayerSession::bootstrap_packets() {
  auto packets = session_->snapshot();
  auto sectors = session_->sectors();
  packets.insert(packets.end(), std::make_move_iterator(sectors.begin()),
                 std::make_move_iterator(sectors.end()));
  mark_bootstrap_sent();
  return packets;
}

std::optional<TutorialFrameResult>
TutorialPlayerSession::handle_frame(const WorldGameplayRequest *gameplay_request,
                                    const std::optional<WorldPosition> &gameplay_position) {
  if (!session_ || !gameplay_request) {
    return std::nullopt;
  }

  TutorialFrameResult result;
  auto &packets = result.packets;
  if (gameplay_request->opcode == kTimeSyncRequest && !snapshot_sent_) {
    packets = session_->snapshot();
    snapshot_sent_ = true;
  }
  if (gameplay_request->opcode == kMapLoadCompleteRequest && gameplay_position && !sectors_sent_) {
    packets = session_->sectors();
    sectors_sent_ = true;
  }
  if (gameplay_position) {
    std::optional<int> ending_cut_scene;
    if (gameplay_request->opcode == kCutSceneEndRequest &&
        gameplay_request->message.contains("cut_scene_id") &&
        gameplay_request->message.at("cut_scene_id").is_number_integer()) {
      const int id = gameplay_request->message.at("cut_scene_id");
      if (session_->has_cut_scene(id)) {
        ending_cut_scene = id;
      }
    }
    auto timed_packets = session_->advance(context_.tutorial_monotonic_ticks(), *gameplay_position);
    packets.insert(packets.end(), std::make_move_iterator(timed_packets.begin()),
                   std::make_move_iterator(timed_packets.end()));
    if (context_.tutorial_has_combat()) {
      auto combat_packets =
          context_.tutorial_advance_combat(context_.tutorial_monotonic_ticks(), *gameplay_position);
      packets.insert(packets.end(), std::make_move_iterator(combat_packets.begin()),
                     std::make_move_iterator(combat_packets.end()));
    }
    std::vector<GameplayPacket> replies;
    const bool combat_request =
        context_.tutorial_has_combat() && context_.tutorial_combat_handles(*gameplay_request);
    const bool conflicting_start = context_.tutorial_has_combat() &&
                                   gameplay_request->opcode == kStartSkillRequest &&
                                   ((combat_request && session_->busy()) ||
                                    (!combat_request && context_.tutorial_combat_busy()));
    if (conflicting_start) {
      const auto &start = gameplay_request->message.at("start_skill");
      replies.push_back({13,
                         {{"ec", "Skill_UsingSkill"},
                          {"skill_id", start.at("skill_id")},
                          {"instance_id", start.at("instance_id")}}});
    } else if (gameplay_request->opcode == kUtilityBagSetRequestOpcode &&
               context_.tutorial_player_in_combat()) {
      replies.push_back({kCommonErrorResponse,
                         {{"ec", "UtilityBag_InCombat"},
                          {"packet_seq", {{"seq", std::to_string(gameplay_request->sequence)}}}}});
    } else if (gameplay_request->opcode == kReviveRequest) {
      replies = context_.tutorial_revive_at_shrine(*gameplay_request);
    } else if (combat_request) {
      replies = context_.tutorial_receive_combat(*gameplay_request, *gameplay_position,
                                                 context_.tutorial_monotonic_ticks());
    } else if (context_.tutorial_has_loot() &&
               context_.tutorial_loot_handles(gameplay_request->opcode)) {
      // Corpse looting commits to storage here, outside the shared codec mutex.
      replies = context_.tutorial_receive_loot(gameplay_request->message, *gameplay_position,
                                               context_.tutorial_monotonic_ticks());
    } else {
      replies = session_->receive(gameplay_request->opcode, gameplay_request->message,
                                  gameplay_request->sequence, *gameplay_position,
                                  context_.tutorial_monotonic_ticks());
    }
    if (ending_cut_scene && !session_->has_cut_scene(*ending_cut_scene)) {
      result.completed_cut_scene = ending_cut_scene;
    }

    if (gameplay_request->opcode == kStartSkillRequest ||
        gameplay_request->opcode == kSkillEffectReportRequest ||
        gameplay_request->opcode == kExecuteSpawnProjectileRequest ||
        gameplay_request->opcode == kChangeSkillStageRequest) {
      int skill_id = 0;
      int stage = -1;
      if (gameplay_request->message.contains("start_skill")) {
        const auto &start = gameplay_request->message.at("start_skill");
        skill_id = start.value("skill_id", 0);
        stage = start.value("stage_index", -1);
      } else if (gameplay_request->message.contains("skill_instance_id")) {
        stage = gameplay_request->message.value("stage_index", -1);
      }
      std::string error_code;
      for (const auto &reply : replies) {
        if ((reply.opcode == 13 || reply.opcode == 2) && reply.message.contains("ec")) {
          error_code = reply.message.at("ec").get<std::string>();
        }
      }
      spdlog::info("Native world skill request opcode={} skill_id={} stage={} result_ec={}",
                   gameplay_request->opcode, skill_id, stage,
                   error_code.empty() ? "none" : error_code);
    }

    packets.insert(packets.end(), std::make_move_iterator(replies.begin()),
                   std::make_move_iterator(replies.end()));
    if (context_.tutorial_has_combat()) {
      auto combat_packets =
          context_.tutorial_advance_combat(context_.tutorial_monotonic_ticks(), *gameplay_position);
      packets.insert(packets.end(), std::make_move_iterator(combat_packets.begin()),
                     std::make_move_iterator(combat_packets.end()));
    }
    if (context_.tutorial_has_loot()) {
      auto corpse_packets = context_.tutorial_advance_loot(context_.tutorial_monotonic_ticks());
      packets.insert(packets.end(), std::make_move_iterator(corpse_packets.begin()),
                     std::make_move_iterator(corpse_packets.end()));
    }
  }
  context_.tutorial_apply_level_change(session_->progress(), packets);
  context_.tutorial_append_encumbrance_changes(packets, false);
  context_.tutorial_persist_player_death(packets);
  context_.tutorial_collect_activity_rewards(packets);

  // A finished cutscene may relocate the player: a teleport inside this map or a
  // map change to its end point. Services are swapped before the next deadline.
  auto &finished_teleport = result.finished_teleport;
  auto &map_change = result.map_change;
  if (session_) {
    finished_teleport = session_->take_completed_teleport();
    if (const auto move = session_->take_map_move()) {
      map_change = leave_map(*move);
    }
  }
  return result;
}

std::optional<MailRecord> TutorialPlayerSession::deliver_completion_mail() {
  if (!context_.tutorial_authenticated() || !context_.tutorial_has_mail_repository() ||
      !context_.tutorial_has_shop_catalog()) {
    return std::nullopt;
  }
  return context_.tutorial_deliver_completion_mail(unix_epoch_ticks_now());
}

TutorialMapChange TutorialPlayerSession::leave_map(const TutorialMapMove &move) {
  TutorialMapChange change{move.spawn, {}, std::nullopt};
  if (!context_.tutorial_authenticated()) {
    throw std::logic_error("A tutorial map move requires an authenticated character");
  }
  for (const auto &[unit_id, npc] : session_->data().npcs) {
    if (npc.value("initially_active", true)) {
      change.removed_unit_ids.push_back(std::to_string(unit_id));
    }
  }

  // The combat observer refers to the private loot domain. Tutorial completion releases
  // combat (and movement state) first; generic map departure has its own inverse sequence.
  context_.tutorial_end_combat();
  context_.tutorial_reset_loot();
  sectors_sent_ = false;

  const auto followup = feature_.data()->followup(move.map_id);
  if (followup) {
    session_->enter_map(followup);
  } else {
    // Candidate: the capture targets public channel 9011. This server hosts the loaded
    // NPC population's map; the channels share this endpoint's ArcheWorld coordinates.
    if (const auto population_map = context_.tutorial_population_map()) {
      change.spawn.map_id = *population_map;
    }
    if (context_.tutorial_has_party_identity()) {
      context_.tutorial_party_graduation(session_->progress());
    }
    session_.reset();
    change.completion_mail = deliver_completion_mail();
    if (const auto notice = context_.tutorial_graduation_notice()) {
      context_.tutorial_gm_reply(*notice);
    }
    context_.tutorial_set_population_map(context_.tutorial_population_map()
                                             ? std::optional<int>(change.spawn.map_id)
                                             : std::nullopt);
    if (const auto character = context_.tutorial_stored_character()) {
      context_.tutorial_create_gathering(*character);
      context_.tutorial_create_companions(*character);
      context_.tutorial_create_quests(character->id);
    }
  }

  if (!context_.tutorial_save_world_position(change.spawn)) {
    spdlog::warn("Tutorial map move position was not persisted map_id={}", change.spawn.map_id);
  }
  spdlog::info("Tutorial cutscene map move connection={} map_id={}",
               context_.tutorial_connection_id(), change.spawn.map_id);
  return change;
}

std::optional<std::int64_t> TutorialPlayerSession::next_action_tick() const {
  return session_ ? session_->next_action_tick() : std::nullopt;
}

std::vector<GameplayPacket> TutorialPlayerSession::advance(std::int64_t ticks,
                                                           const WorldPosition &position) {
  return session_->advance(ticks, position);
}

} // namespace chronicles::tutorial
