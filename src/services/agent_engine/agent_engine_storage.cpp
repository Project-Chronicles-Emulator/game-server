#include "services/agent_engine/agent_engine_storage.hpp"

#include "services/agent_engine/agent_engine_text.hpp"

#include <pqxx/pqxx>

#include <algorithm>
#include <stdexcept>

namespace chronicles::agent_engine {

void validate_memory(const AgentMemory &memory) {
  if (memory.agent_id.empty() || memory.agent_id.size() > kMaximumIdBytes) {
    throw std::invalid_argument("Agent memory requires an agent id");
  }
  if (trim(memory.description).empty() || memory.description.size() > kMaximumTextBytes) {
    throw std::invalid_argument("Agent memory description is empty or too long");
  }
  if (memory.event_datetime.size() > kMaximumDateTimeBytes ||
      memory.reason.size() > kMaximumReasonBytes) {
    throw std::invalid_argument("Agent memory metadata is too long");
  }
  if (memory.priority < kMinimumMemoryPriority || memory.priority > kMaximumMemoryPriority) {
    throw std::invalid_argument("Agent memory priority is outside 1..10");
  }
  if (!is_clean_utf8(memory.agent_id) || !is_clean_utf8(memory.description) ||
      !is_clean_utf8(memory.event_datetime) || !is_clean_utf8(memory.reason)) {
    throw std::invalid_argument("Agent memory text must be clean UTF-8");
  }
}

void InMemoryAgentMemoryStore::add(const AgentMemory &memory) {
  validate_memory(memory);

  std::lock_guard lock(mutex_);
  auto &memories = memories_[memory.agent_id];
  memories.push_back(memory);
  if (memories.size() > kMaximumMemoriesPerAgent) {
    const auto excess = memories.size() - kMaximumMemoriesPerAgent;
    memories.erase(memories.begin(), memories.begin() + static_cast<std::ptrdiff_t>(excess));
  }
}

std::vector<AgentMemory> InMemoryAgentMemoryStore::recent(const std::string &agent_id,
                                                          std::size_t limit) {
  std::lock_guard lock(mutex_);
  const auto found = memories_.find(agent_id);
  if (found == memories_.end()) {
    return {};
  }

  const auto &memories = found->second;
  const auto count = std::min(limit, memories.size());
  return {memories.rbegin(), memories.rbegin() + static_cast<std::ptrdiff_t>(count)};
}

PostgresAgentMemoryStore::PostgresAgentMemoryStore(std::string connection)
    : connection_(std::move(connection)) {}

void PostgresAgentMemoryStore::add(const AgentMemory &memory) {
  validate_memory(memory);

  pqxx::connection connection(connection_);
  pqxx::work transaction(connection);
  transaction.exec("INSERT INTO emulator_agent_memories"
                   "(agent_id,description,event_datetime,priority,reason) "
                   "VALUES ($1,$2,$3,$4,$5)",
                   pqxx::params{memory.agent_id, memory.description, memory.event_datetime,
                                memory.priority, memory.reason});
  // Retention keeps the newest rows; ids are generated in insertion order.
  transaction.exec("DELETE FROM emulator_agent_memories WHERE agent_id=$1 AND id NOT IN ("
                   "SELECT id FROM emulator_agent_memories WHERE agent_id=$1 "
                   "ORDER BY id DESC LIMIT $2)",
                   pqxx::params{memory.agent_id, static_cast<long long>(kMaximumMemoriesPerAgent)});
  transaction.commit();
}

std::vector<AgentMemory> PostgresAgentMemoryStore::recent(const std::string &agent_id,
                                                          std::size_t limit) {
  const auto bounded_limit = std::min(limit, kMaximumMemoriesPerAgent);

  pqxx::connection connection(connection_);
  pqxx::read_transaction transaction(connection);
  const auto rows = transaction.exec(
      "SELECT description,event_datetime,priority,reason FROM emulator_agent_memories "
      "WHERE agent_id=$1 ORDER BY id DESC LIMIT $2",
      pqxx::params{agent_id, static_cast<long long>(bounded_limit)});

  std::vector<AgentMemory> memories;
  memories.reserve(rows.size());
  for (const auto &row : rows) {
    AgentMemory memory;
    memory.agent_id = agent_id;
    memory.description = row[0].as<std::string>();
    memory.event_datetime = row[1].as<std::string>();
    memory.priority = row[2].as<int>();
    memory.reason = row[3].as<std::string>();
    memories.push_back(std::move(memory));
  }
  return memories;
}

} // namespace chronicles::agent_engine
