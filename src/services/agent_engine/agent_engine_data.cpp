// Startup data for the agent engine: agent personas and world knowledge.

#include "services/agent_engine/agent_engine.hpp"

#include "services/agent_engine/agent_engine_text.hpp"
#include "data/client_catalog.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <map>

namespace chronicles::agent_engine {
namespace {

constexpr std::string_view kAgentsFile = "agents.json";
constexpr std::string_view kKnowledgeFile = "knowledge.json";
constexpr std::size_t kMaximumAgents = 256;
constexpr std::size_t kMaximumKnowledgeEntries = 4096;
constexpr std::size_t kMaximumScheduleBlocks = 48;

std::string required_string(const nlohmann::json &object, std::string_view field,
                            std::size_t maximum_bytes) {
  const auto value = object.at(std::string(field)).get<std::string>();
  if (trim(value).empty() || value.size() > maximum_bytes || !is_clean_utf8(value)) {
    throw std::invalid_argument("Agent data field " + std::string(field) + " is invalid");
  }
  return value;
}

std::string optional_string(const nlohmann::json &object, std::string_view field,
                            std::size_t maximum_bytes) {
  const auto key = std::string(field);
  if (!object.contains(key)) {
    return {};
  }
  return required_string(object, field, maximum_bytes);
}

// "[Housemate] Seren" -> "Seren": the bracketed role tag is not part of the name.
std::string display_name(std::string_view npc_name) {
  auto name = trim(npc_name);
  if (name.starts_with('[')) {
    const auto tag_end = name.find(']');
    if (tag_end != std::string_view::npos) {
      name = trim(name.substr(tag_end + 1));
    }
  }
  return std::string(name);
}

std::optional<nlohmann::json> read_json_file(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }
  try {
    return nlohmann::json::parse(input);
  } catch (const nlohmann::json::exception &) {
    throw std::invalid_argument("Agent engine file " + path.filename().string() +
                                " is not valid JSON");
  }
}

ScheduleBlock parse_schedule_block(const nlohmann::json &entry) {
  ScheduleBlock block;
  const auto start = parse_minute_of_day(required_string(entry, "start", kMaximumDateTimeBytes));
  const auto end = parse_minute_of_day(required_string(entry, "end", kMaximumDateTimeBytes));
  if (!start || !end || *end <= *start) {
    throw std::invalid_argument("Agent schedule blocks need HH:MM start < end");
  }
  block.start_minute = *start;
  block.end_minute = *end;
  block.task = required_string(entry, "task", kMaximumShortTextBytes);
  block.location = optional_string(entry, "location", kMaximumShortTextBytes);
  return block;
}

// Applies one agents.json entry over an existing (or new) profile with the same id.
void apply_agent_override(const nlohmann::json &entry, AgentProfile &agent) {
  if (entry.contains("name")) {
    agent.name = required_string(entry, "name", kMaximumShortTextBytes);
  }
  if (entry.contains("is_player")) {
    agent.is_player = entry.at("is_player").get<bool>();
  }
  if (entry.contains("summary")) {
    agent.summary = required_string(entry, "summary", kMaximumTextBytes);
  }
  if (entry.contains("residence")) {
    agent.residence = required_string(entry, "residence", kMaximumShortTextBytes);
  }
  if (entry.contains("schedule")) {
    const auto &blocks = entry.at("schedule");
    if (!blocks.is_array() || blocks.size() > kMaximumScheduleBlocks) {
      throw std::invalid_argument("Agent schedule must be an array of at most 48 blocks");
    }
    agent.schedule.clear();
    for (const auto &block : blocks) {
      agent.schedule.push_back(parse_schedule_block(block));
    }
  }
}

void add_catalog_agents(ClientCatalog &catalog, std::map<std::string, AgentProfile> &agents) {
  if (catalog.has_table("AIChatNpcAgent") && catalog.has_table("Npc")) {
    for (const auto &row : catalog.rows("AIChatNpcAgent")) {
      const auto npc_id = row.at("npcCmsId").get<std::string>();
      AgentProfile agent;
      agent.id = row.at("agentId").get<std::string>();
      const auto npc = catalog.find_row("Npc", "id", npc_id);
      agent.name = npc ? display_name(npc->value("name", std::string{})) : std::string{};
      if (agent.name.empty()) {
        agent.name = "NPC " + npc_id;
      }
      agents[agent.id] = std::move(agent);
    }
  }

  if (catalog.has_table("AIChatPlayerAgent")) {
    for (const auto &row : catalog.rows("AIChatPlayerAgent")) {
      AgentProfile agent;
      agent.id = row.at("agentId").get<std::string>();
      agent.is_player = true;
      agent.name = row.value("characterRace", std::string{}) + " " +
                   row.value("characterGender", std::string{}) + " " +
                   row.value("characterClass", std::string{});
      agent.name = std::string(trim(agent.name));
      if (agent.name.empty()) {
        agent.name = "Player";
      }
      agents[agent.id] = std::move(agent);
    }
  }
}

void add_file_agents(const std::filesystem::path &data_directory,
                     std::map<std::string, AgentProfile> &agents) {
  const auto document = read_json_file(data_directory / kAgentsFile);
  if (!document) {
    return;
  }

  const auto &entries = document->at("agents");
  if (!entries.is_array() || entries.size() > kMaximumAgents) {
    throw std::invalid_argument("agents.json needs an \"agents\" array of at most 256 entries");
  }
  for (const auto &entry : entries) {
    const auto id = required_string(entry, "id", kMaximumIdBytes);
    auto &agent = agents[id];
    agent.id = id;
    apply_agent_override(entry, agent);
    if (agent.name.empty()) {
      throw std::invalid_argument("Agent " + id + " has no name");
    }
  }
}

std::vector<std::string> file_knowledge(const std::filesystem::path &data_directory) {
  const auto document = read_json_file(data_directory / kKnowledgeFile);
  if (!document) {
    return {};
  }

  const auto &entries = document->at("entries");
  if (!entries.is_array() || entries.size() > kMaximumKnowledgeEntries) {
    throw std::invalid_argument("knowledge.json needs an \"entries\" array");
  }
  std::vector<std::string> knowledge;
  for (const auto &entry : entries) {
    knowledge.push_back(entry.get<std::string>());
  }
  return knowledge;
}

} // namespace

AgentEngineData load_agent_engine_data(ClientCatalog *catalog,
                                       const std::filesystem::path &data_directory) {
  std::map<std::string, AgentProfile> agents_by_id;
  try {
    if (catalog) {
      add_catalog_agents(*catalog, agents_by_id);
    }
    add_file_agents(data_directory, agents_by_id);
  } catch (const nlohmann::json::exception &) {
    throw std::invalid_argument("Agent engine data has a missing or mistyped field");
  }

  AgentEngineData data;
  for (auto &[id, agent] : agents_by_id) {
    if (id.empty() || id.size() > kMaximumIdBytes || !is_clean_utf8(id)) {
      throw std::invalid_argument("Agent ids must be short clean text");
    }
    data.agents.push_back(std::move(agent));
  }

  try {
    data.knowledge = file_knowledge(data_directory);
  } catch (const nlohmann::json::exception &) {
    throw std::invalid_argument("knowledge.json entries must be strings");
  }

  // Personas double as world knowledge, so "who is Seren?" finds her description.
  for (const auto &agent : data.agents) {
    if (!agent.summary.empty()) {
      data.knowledge.push_back(agent.name + ": " + agent.summary);
    }
    if (!agent.residence.empty()) {
      data.knowledge.push_back(agent.name + " lives at " + agent.residence + ".");
    }
  }
  return data;
}

} // namespace chronicles::agent_engine
