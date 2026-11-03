// Quest job boards (QuestJobBoard, QuestJobBoardCategory, QuestPickGroup).
//
// Evidence: the client's board widget sends PC2S_QuestJobBoardReq{quest_job_board_cms_id,
// doodad_unit_id, refresh_using_cost} and expects PS2C_QuestJobBoardRes{jobs (board -> quest
// ids), condition_not_met_ids}. Accepting and turning in a job send AcceptQuestReq.unit and
// QuestRewardReq.unit_id naming the board doodad (AAC_QuestJobBoard_SendAcceptForBoardUnit,
// AAC_QuestJobBoard_SendRewardForBoardUnit; dumps/ida_research/all-quests-20261030).
// GameVariable QuestJobBoardPickOnceCount (3) and the daily QuestJobBoardRefreshSchedule* rows
// define the offered slots and refresh period.
//
// CUSTOM (Candidate): the official selection algorithm is not captured. Each slot (`order`)
// uses the highest category row the character qualifies for (company type and position level,
// or guild level) and draws one quest from its pick group with a deterministic hash of the
// character, board, period and paid refresh count, so the offer is stable until the next period
// or refresh. A paid refresh costs QuestJobBoard.resetCost Gold.

#include "world/features/quests/quest_book_detail.hpp"
#include "world/features/quests/world_quests.hpp"

#include <algorithm>
#include <limits>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using namespace quest_book_detail;
using quest_rules::require;

// SplitMix64 finalizer: spreads the selection key so neighbouring periods pick unrelated quests.
std::uint64_t mix(std::uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

// The level a category row asks for, or nullopt when the character does not qualify.
std::optional<int> slot_qualification(const Json &slot, const QuestWorldView &view) {
  if (slot.at("is_guild").get<bool>()) {
    const int required = slot.at("guild_level");
    if (!view.guild_level || *view.guild_level < required) {
      return std::nullopt;
    }
    return required;
  }

  const int company_type = slot.at("company_type");
  if (company_type == 0) {
    return 0;
  }
  const int required = slot.at("company_level");
  if (!view.company || view.company->company_type != company_type ||
      view.company->position_level < required) {
    return std::nullopt;
  }
  return required;
}

} // namespace

std::int64_t WorldQuestBook::job_board_period(const QuestWorldView &view) const {
  const auto &schedule = catalog_->job_board_schedule();
  if (schedule.period_ms <= 0 || view.now_epoch_ms <= schedule.start_epoch_ms) {
    return 0;
  }
  return (view.now_epoch_ms - schedule.start_epoch_ms) / schedule.period_ms;
}

bool WorldQuestBook::job_board_nearby(int board_id, std::uint64_t unit_id,
                                      const QuestWorldView &view) const {
  const auto *board = catalog_->job_board(board_id);
  return board && placement_nearby(unit_id, board->at("doodads"), view);
}

std::vector<int> WorldQuestBook::job_board_offers(int board_id, const WorldProgress &state,
                                                  const QuestWorldView &view) const {
  std::vector<int> offers;
  const auto *board = catalog_->job_board(board_id);
  if (!board) {
    return offers;
  }

  const auto period = job_board_period(view);
  const auto record = state.job_boards.find(board_id);
  const int refreshes =
      record != state.job_boards.end() && record->second.day == period ? record->second.refreshes
                                                                       : 0;
  const int pick_count = catalog_->job_board_schedule().pick_count;
  for (int order = 1; order <= pick_count; ++order) {
    const Json *chosen = nullptr;
    int chosen_level = -1;
    for (const auto &slot : board->at("slots")) {
      if (slot.at("order").get<int>() != order) {
        continue;
      }
      const auto level = slot_qualification(slot, view);
      if (level && *level > chosen_level) {
        chosen = &slot;
        chosen_level = *level;
      }
    }
    if (!chosen) {
      continue;
    }

    // Prefer quests not already offered by an earlier slot of this board.
    std::vector<int> pool;
    for (const auto &quest_id : chosen->at("quest_ids")) {
      if (std::ranges::find(offers, quest_id.get<int>()) == offers.end()) {
        pool.push_back(quest_id.get<int>());
      }
    }
    if (pool.empty()) {
      continue;
    }
    auto key = mix(static_cast<std::uint64_t>(player_unit_id_));
    key = mix(key ^ static_cast<std::uint64_t>(board_id));
    key = mix(key ^ static_cast<std::uint64_t>(period));
    key = mix(key ^ static_cast<std::uint64_t>(refreshes));
    key = mix(key ^ static_cast<std::uint64_t>(order));
    offers.push_back(pool.at(key % pool.size()));
  }
  return offers;
}

void WorldQuestBook::renew_job_board(int board_id, WorldProgress &next,
                                     std::vector<GameplayPacket> &packets,
                                     const QuestWorldView &view) const {
  const auto period = job_board_period(view);
  const auto found = next.job_boards.find(board_id);
  if (found != next.job_boards.end() && found->second.day >= period) {
    return;
  }
  require(found != next.job_boards.end() || next.job_boards.size() < kMaximumJobBoards,
          "BadRequest");

  // Jobs completed in an earlier period become available again: the record restarts as an
  // abandoned tombstone and the client removes its completed entry.
  for (auto &[quest_id, quest] : next.quests) {
    const auto *definition = catalog_->quest(quest_id);
    if (!definition || job_board_of(*definition) != board_id || !quest.complete ||
        quest.abandoned) {
      continue;
    }
    QuestProgress tombstone;
    tombstone.attempt = quest.attempt + 1;
    tombstone.success_count = quest.success_count;
    tombstone.abandoned = true;
    for (const auto &[sequence, value] : quest.objectives) {
      tombstone.objectives.emplace(sequence, 0);
    }
    quest = tombstone;
    packets.push_back({kQuestAbandoned, {{"quest_cms_id", quest_id}, {"force", true}}});
  }
  next.job_boards.insert_or_assign(board_id, JobBoardRecord{period, 0});
}

void WorldQuestBook::job_board_request(const Json &request, WorldProgress &next,
                                       const QuestWorldView &view,
                                       std::vector<GameplayPacket> &packets) const {
  const int board_id = request.at("quest_job_board_cms_id");
  const auto *board = catalog_->job_board(board_id);
  require(board != nullptr, "BadRequest");
  require(job_board_nearby(board_id, parse_unit_id(request.at("doodad_unit_id")), view),
          "TooFar");

  renew_job_board(board_id, next, packets, view);
  if (request.value("refresh_using_cost", false)) {
    auto &record = next.job_boards.at(board_id);
    require(record.refreshes < std::numeric_limits<int>::max(), "BadRequest");
    Json update{{"stackables", Json::array()}, {"quest_job_board", Json::object()}};
    const auto cost = board->at("reset_cost").get<std::int64_t>();
    if (cost > 0) {
      quest_rules::change_money(next, MoneyType::Gold, -cost, update, "MoneyNotEnough");
      packets.push_back({kItemsUpdated, std::move(update)});
    }
    ++record.refreshes;
  }

  const auto offers = job_board_offers(board_id, next, view);
  Json condition_not_met = Json::array();
  for (const int quest_id : offers) {
    if (!conditions_pass(*catalog_->quest(quest_id), next, view)) {
      condition_not_met.push_back(quest_id);
    }
  }
  Json jobs = Json::object();
  jobs[std::to_string(board_id)] = {{"quest_cms_ids", offers}};
  packets.push_back({kJobBoardResponse,
                     {{"jobs", std::move(jobs)},
                      {"condition_not_met_ids", std::move(condition_not_met)}}});
}

} // namespace chronicles
