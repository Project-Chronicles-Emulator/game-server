#pragma once

// Internal helpers shared by the WorldQuestBook translation units (world_quests.cpp and
// quest_job_boards.cpp). Not part of the module's public interface.

#include "world/core/unit_registry.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/quests/quest_rules.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace chronicles::quest_book_detail {

enum QuestOpcode : std::uint16_t {
  kCommonError = 2,
  kQuestUpdate = 27,
  kQuestAccepted = 28,
  kQuestAbandoned = 29,
  kJobBoardResponse = 30,
  kRegisterMateNpc = 58,
  kChangedMateIntimacy = 59,
  kWorldQuests = 136,
  kDialogueStarted = 161,
  kDialogueEnded = 162,
  kUnitInteracted = 164,
  kItemsUpdated = 169,
  kAcceptQuest = world_request::kAcceptQuestRequest,
  kAbandonQuest = world_request::kAbandonQuestRequest,
  kClaimQuestReward = world_request::kClaimQuestRewardRequest,
  kProgressQuest = world_request::kProgressQuestRequest,
  kJobBoardRequest = world_request::kQuestJobBoardRequest,
  kStartDialogue = 20065,
  kEndDialogue = 20066,
};

// A unit id sent as a decimal string or number.
inline std::uint64_t parse_unit_id(const nlohmann::json &value) {
  const auto text =
      value.is_string() ? value.get<std::string>() : std::to_string(value.get<std::uint64_t>());
  const auto parsed = parse_unit_id_text(text);
  quest_rules::require(parsed.has_value(), "BadRequest");
  return *parsed;
}

// The JobBoard acceptance rule's board id, when the quest is a job board quest.
inline std::optional<int> job_board_of(const nlohmann::json &definition) {
  for (const auto &rule : definition.at("accept")) {
    if (rule.at("kind") == "JobBoard") {
      return rule.at("board_id").get<int>();
    }
  }
  return std::nullopt;
}

} // namespace chronicles::quest_book_detail
