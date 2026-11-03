#pragma once

// The emulator's agent engine: the AI chat backend that the client calls over REST.
//
// Client contract (static analysis of the matching build; docs/agent-engine.md and
// dumps/ida_research/ai-rest-20261026): the front REST client sends JSON bodies (protobuf JSON
// with proto field names) to a hard-coded base URL, http://modena.xlgames.corp:38011, without an
// Authorization header. The launcher rewrites that base URL to this service. Conversations start
// from a house mate dialogue option (DialogueSelection AIConverse/ManualAIConverse). The client
// then loads agents, searches world knowledge, converses, asks for suggested player replies,
// summarizes the finished conversation and stores the summary as an agent memory.
//
// Five operations (time-and-status, select-location, select-action, breakdown-schedule and
// make-plan) have recovered DTOs but no client caller in this build; their routes are
// emulator-defined.

#include "services/agent_engine/agent_engine_knowledge.hpp"
#include "services/agent_engine/agent_engine_model.hpp"
#include "services/agent_engine/agent_engine_storage.hpp"
#include "services/agent_engine/agent_engine_types.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chronicles {
class ClientCatalog;
}

namespace chronicles::agent_engine {

// GameVariable AIChatMaxTurn and AIChatSearchKnowledgeTopK; used when a request sends 0.
inline constexpr int kDefaultConversationTurns = 10;
inline constexpr int kMaximumConversationTurns = 50;
inline constexpr int kDefaultKnowledgeResults = 5;
inline constexpr int kMaximumKnowledgeResults = 20;
// Open conversations kept in memory; the least recently used one is dropped beyond this.
inline constexpr std::size_t kMaximumConversations = 256;
// Memories recalled for each reply.
inline constexpr std::size_t kRecalledMemories = 5;
// Breakdown steps are bounded to one day at the shortest interval below.
inline constexpr int kMinimumScheduleIntervalMinutes = 5;
inline constexpr int kDefaultScheduleIntervalMinutes = 60;

class UnknownAgent : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct AgentDirectory {
  // Agents a client has loaded since the service started (AgentListRes.current_agents).
  std::vector<AgentProfile> loaded;
  // Every configured agent (AgentListRes.agents_in_db).
  std::vector<AgentProfile> stored;
};

struct ConverseInput {
  std::string self_id;
  std::string partner_id;
  bool start_conversation{};
  std::string statement;
  std::string dialogue_situation;
  std::string relationship;
  int max_conversation_turn{};
  bool end_conversation_forcefully{};
  std::vector<std::string> world_knowledge;
};

struct ConverseResult {
  std::string response;
  bool conversation_end{};
  // Set only when the conversation ended with this reply.
  std::string conversation_summary;
};

struct PlayerResponseInput {
  std::string agent_id;
  std::string player_id;
  std::vector<std::string> world_knowledge;
  std::string dialogue_situation;
};

struct MemoryResult {
  std::string message;
  int priority{};
  std::string reason;
};

struct LocationInput {
  std::string agent_id;
  std::string current_task;
  std::vector<std::string> locations;
  std::string residence;
  std::string current_location;
  std::vector<std::string> neighbor_area_locations;
};

struct LocationChoice {
  std::string location;
  std::string reason;
};

struct ActionInput {
  std::string agent_id;
  std::string task;
  std::string location;
  std::vector<std::string> interactables;
  std::vector<std::string> game_commands;
};

struct ScheduleInput {
  std::string agent_id;
  // "HH:MM-HH:MM"; an end before the start continues past midnight.
  std::string schedule;
  std::string location;
  // What the agent does in this block, for example "cooking".
  std::string type;
  std::vector<std::string> interactables;
  // Minutes per step; 0 selects kDefaultScheduleIntervalMinutes.
  int interval{};
};

struct PlanStep {
  std::string start;
  std::string end;
  std::string task;
  std::vector<std::string> interactables;
};

// Request validation failures throw std::invalid_argument; unknown agent ids throw UnknownAgent.
class AgentEngine {
public:
  AgentEngine(std::vector<AgentProfile> agents, KnowledgeIndex knowledge,
              std::shared_ptr<AgentMemoryStore> memories, std::unique_ptr<DialogueModel> model);

  AgentDirectory directory() const;

  // POST /agent/{id}/load: marks the agent loaded and returns it.
  AgentProfile load(const std::string &agent_id);

  ConverseResult converse(const ConverseInput &input);

  std::string summarize(const std::string &agent_id, const std::vector<std::string> &history);

  MemoryResult add_memory(const std::string &agent_id, const std::string &description,
                          const std::string &event_datetime);

  std::vector<std::string> player_responses(const PlayerResponseInput &input);

  // top_k 0 selects kDefaultKnowledgeResults.
  std::vector<KnowledgeHit> search_knowledge(const std::string &query, int top_k) const;

  // "HH:MM" or "YYYY-MM-DDTHH:MM" reference time; one status sentence per scheduled agent.
  std::string time_and_status(const std::string &reference_time) const;

  LocationChoice select_location(const LocationInput &input) const;

  std::string select_action(const ActionInput &input) const;

  std::vector<PlanStep> breakdown_schedule(const ScheduleInput &input) const;

  // The agent's configured daily routine as plan steps.
  std::vector<PlanStep> make_plan(const std::string &agent_id) const;

  std::size_t agent_count() const;

  std::size_t knowledge_count() const;

private:
  using ConversationKey = std::pair<std::string, std::string>;

  struct Conversation {
    std::vector<ConversationTurn> turns;
    int replies{};
    std::uint64_t last_use{};
  };

  const AgentProfile &profile(const std::string &agent_id) const;

  // Validates the id and throws UnknownAgent when no agent has it.
  void require_agent(const std::string &agent_id) const;

  // Requires mutex_. Creates the conversation when absent and evicts the least recently used.
  Conversation &touch_conversation(const ConversationKey &key);

  std::vector<AgentProfile> agents_;
  std::unordered_map<std::string, std::size_t> agent_index_;
  KnowledgeIndex knowledge_;
  std::shared_ptr<AgentMemoryStore> memories_;
  std::unique_ptr<DialogueModel> model_;

  mutable std::mutex mutex_;
  std::set<std::string> loaded_;
  std::map<ConversationKey, Conversation> conversations_;
  std::uint64_t use_counter_{};
};

struct AgentEngineData {
  std::vector<AgentProfile> agents;
  std::vector<std::string> knowledge;
};

// Agents come from the client catalog (AIChatNpcAgent with the Npc name, AIChatPlayerAgent)
// and from <data_directory>/agents.json, whose entries override or add agents by id. World
// knowledge comes from <data_directory>/knowledge.json plus one entry per persona. A missing
// catalog or file contributes nothing; malformed content throws std::invalid_argument.
AgentEngineData load_agent_engine_data(ClientCatalog *catalog,
                                       const std::filesystem::path &data_directory);

// Minutes after midnight for "HH:MM", or the time part of "YYYY-MM-DDTHH:MM[:SS]".
std::optional<int> parse_minute_of_day(std::string_view text);

// "HH:MM" for a minute count; values wrap at midnight.
std::string format_minute_of_day(int minute);

} // namespace chronicles::agent_engine
