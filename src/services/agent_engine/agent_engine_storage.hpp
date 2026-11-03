#pragma once

// Persistence of agent memories written by POST /add-memory.

#include "services/agent_engine/agent_engine_types.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles::agent_engine {

// Throws std::invalid_argument when a memory is outside the persisted bounds.
void validate_memory(const AgentMemory &memory);

class AgentMemoryStore {
public:
  virtual ~AgentMemoryStore() = default;

  // Stores one memory and drops the agent's oldest rows beyond kMaximumMemoriesPerAgent.
  virtual void add(const AgentMemory &memory) = 0;

  // The agent's newest memories first, at most `limit`.
  virtual std::vector<AgentMemory> recent(const std::string &agent_id, std::size_t limit) = 0;
};

// Disposable memories for --storage memory and tests.
class InMemoryAgentMemoryStore final : public AgentMemoryStore {
public:
  void add(const AgentMemory &memory) override;

  std::vector<AgentMemory> recent(const std::string &agent_id, std::size_t limit) override;

private:
  std::mutex mutex_;
  // Oldest first per agent.
  std::unordered_map<std::string, std::vector<AgentMemory>> memories_;
};

// emulator_agent_memories (schema version 26).
class PostgresAgentMemoryStore final : public AgentMemoryStore {
public:
  explicit PostgresAgentMemoryStore(std::string connection);

  void add(const AgentMemory &memory) override;

  std::vector<AgentMemory> recent(const std::string &agent_id, std::size_t limit) override;

private:
  std::string connection_;
};

} // namespace chronicles::agent_engine
