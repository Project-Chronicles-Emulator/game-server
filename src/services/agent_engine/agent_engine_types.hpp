#pragma once

// Records shared by the agent engine (the emulator's implementation of the client's AI chat
// REST service; docs/agent-engine.md).
//
// The client names agents by the string form of the agentId columns in AIChatNpcAgent.csv
// (house mate Seren, NPC 35, is agent "2") and AIChatPlayerAgent.csv (Fight/Nuian/Male is
// agent "0"). Requests carry no credential, so state is owned by agent ids, not characters.

#include <cstddef>
#include <string>
#include <vector>

namespace chronicles::agent_engine {

// Request bounds. The client limits turns with AIChatMaxTurn (10) and knowledge with
// AIChatSearchKnowledgeTopK (5); the larger bounds below only reject hostile requests.
inline constexpr std::size_t kMaximumIdBytes = 64;
inline constexpr std::size_t kMaximumTextBytes = 2048;
inline constexpr std::size_t kMaximumShortTextBytes = 256;
inline constexpr std::size_t kMaximumDateTimeBytes = 64;
inline constexpr std::size_t kMaximumReasonBytes = 512;
inline constexpr std::size_t kMaximumListEntries = 64;
inline constexpr std::size_t kMaximumMemoriesPerAgent = 512;
inline constexpr int kMinimumMemoryPriority = 1;
inline constexpr int kMaximumMemoryPriority = 10;
inline constexpr int kMinutesPerDay = 24 * 60;

// One block of an agent's daily routine, in minutes after midnight of the game day.
struct ScheduleBlock {
  int start_minute{};
  int end_minute{};
  std::string task;
  std::string location;
};

struct AgentProfile {
  std::string id;
  std::string name;
  bool is_player{};
  // Persona text returned as AddAgentRes.summary and used by the dialogue model.
  std::string summary;
  std::string residence;
  std::vector<ScheduleBlock> schedule;
};

struct AgentMemory {
  std::string agent_id;
  std::string description;
  std::string event_datetime;
  int priority{kMinimumMemoryPriority};
  std::string reason;
};

struct ConversationTurn {
  std::string speaker_name;
  std::string text;
};

} // namespace chronicles::agent_engine
