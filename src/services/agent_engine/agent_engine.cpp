#include "services/agent_engine/agent_engine.hpp"

#include "services/agent_engine/agent_engine_text.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace chronicles::agent_engine {
namespace {

constexpr std::string_view kMemoryStoredMessage = "Memory stored.";
constexpr std::size_t kMaximumBreakdownSteps = kMinutesPerDay / kMinimumScheduleIntervalMinutes;
// Turns kept per conversation for model context; older turns are dropped.
constexpr std::size_t kMaximumKeptTurns = 2 * kMaximumConversationTurns;

void require_text(std::string_view text, std::size_t maximum_bytes, std::string_view field) {
  if (text.size() > maximum_bytes || !is_clean_utf8(text)) {
    throw std::invalid_argument(std::string(field) + " is too long or not clean UTF-8");
  }
}

void require_id(std::string_view id, std::string_view field) {
  if (id.empty()) {
    throw std::invalid_argument(std::string(field) + " is required");
  }
  require_text(id, kMaximumIdBytes, field);
}

void require_list(const std::vector<std::string> &entries, std::size_t maximum_bytes,
                  std::string_view field) {
  if (entries.size() > kMaximumListEntries) {
    throw std::invalid_argument(std::string(field) + " has too many entries");
  }
  for (const auto &entry : entries) {
    require_text(entry, maximum_bytes, field);
  }
}

int bounded_turn_limit(int requested) {
  if (requested <= 0) {
    return kDefaultConversationTurns;
  }
  return std::min(requested, kMaximumConversationTurns);
}

// Keyword overlap between a candidate and the words describing what the agent wants to do.
std::size_t relevance(std::string_view candidate, const std::vector<std::string> &goal_terms) {
  const auto candidate_terms = keyword_tokens(candidate);
  std::size_t overlap = 0;
  for (const auto &term : goal_terms) {
    if (std::ranges::find(candidate_terms, term) != candidate_terms.end()) {
      ++overlap;
    }
  }
  return overlap;
}

// The first candidate with the highest positive relevance, or nullptr.
const std::string *most_relevant(const std::vector<std::string> &candidates,
                                 const std::vector<std::string> &goal_terms) {
  const std::string *best = nullptr;
  std::size_t best_overlap = 0;
  for (const auto &candidate : candidates) {
    const auto overlap = relevance(candidate, goal_terms);
    if (overlap > best_overlap) {
      best_overlap = overlap;
      best = &candidate;
    }
  }
  return best;
}

std::vector<std::string> history_lines(const std::vector<ConversationTurn> &turns) {
  std::vector<std::string> lines;
  lines.reserve(turns.size());
  for (const auto &turn : turns) {
    lines.push_back(turn.speaker_name + ": " + turn.text);
  }
  return lines;
}

std::string block_task(const ScheduleBlock &block) {
  if (block.location.empty()) {
    return block.task;
  }
  return block.task + " at " + block.location;
}

// End of a block: a block ending exactly at midnight reads "24:00" rather than "00:00".
std::string format_block_end(int minute) {
  if (minute == kMinutesPerDay) {
    return "24:00";
  }
  return format_minute_of_day(minute);
}

} // namespace

std::optional<int> parse_minute_of_day(std::string_view text) {
  text = trim(text);
  const auto time_separator = text.find('T');
  if (time_separator != std::string_view::npos) {
    text = text.substr(time_separator + 1);
  }
  if (text.size() < 5 || text[2] != ':') {
    return std::nullopt;
  }

  int hours{};
  int minutes{};
  const auto hours_end = text.data() + 2;
  const auto minutes_end = text.data() + 5;
  if (std::from_chars(text.data(), hours_end, hours).ptr != hours_end ||
      std::from_chars(text.data() + 3, minutes_end, minutes).ptr != minutes_end) {
    return std::nullopt;
  }
  if (hours < 0 || hours > 24 || minutes < 0 || minutes > 59 || (hours == 24 && minutes != 0)) {
    return std::nullopt;
  }
  return hours * 60 + minutes;
}

std::string format_minute_of_day(int minute) {
  const int wrapped = ((minute % kMinutesPerDay) + kMinutesPerDay) % kMinutesPerDay;
  char text[6]{};
  std::snprintf(text, sizeof(text), "%02d:%02d", wrapped / 60, wrapped % 60);
  return text;
}

AgentEngine::AgentEngine(std::vector<AgentProfile> agents, KnowledgeIndex knowledge,
                         std::shared_ptr<AgentMemoryStore> memories,
                         std::unique_ptr<DialogueModel> model)
    : agents_(std::move(agents)), knowledge_(std::move(knowledge)), memories_(std::move(memories)),
      model_(std::move(model)) {
  if (!memories_ || !model_) {
    throw std::invalid_argument("The agent engine needs a memory store and a dialogue model");
  }
  for (std::size_t index = 0; index < agents_.size(); ++index) {
    if (!agent_index_.emplace(agents_[index].id, index).second) {
      throw std::invalid_argument("Duplicate agent id " + agents_[index].id);
    }
  }
}

const AgentProfile &AgentEngine::profile(const std::string &agent_id) const {
  const auto found = agent_index_.find(agent_id);
  if (found == agent_index_.end()) {
    throw UnknownAgent("Unknown agent id");
  }
  return agents_[found->second];
}

void AgentEngine::require_agent(const std::string &agent_id) const {
  require_id(agent_id, "agent_id");
  static_cast<void>(profile(agent_id));
}

AgentDirectory AgentEngine::directory() const {
  AgentDirectory directory;
  directory.stored = agents_;

  std::lock_guard lock(mutex_);
  for (const auto &agent : agents_) {
    if (loaded_.contains(agent.id)) {
      directory.loaded.push_back(agent);
    }
  }
  return directory;
}

AgentProfile AgentEngine::load(const std::string &agent_id) {
  require_id(agent_id, "agent_id");
  const auto &agent = profile(agent_id);

  std::lock_guard lock(mutex_);
  loaded_.insert(agent.id);
  return agent;
}

AgentEngine::Conversation &AgentEngine::touch_conversation(const ConversationKey &key) {
  auto &conversation = conversations_[key];
  conversation.last_use = ++use_counter_;

  if (conversations_.size() > kMaximumConversations) {
    const auto oldest =
        std::ranges::min_element(conversations_, [](const auto &left, const auto &right) {
          return left.second.last_use < right.second.last_use;
        });
    // The conversation just touched has the newest use count, so it is never the one dropped.
    conversations_.erase(oldest);
  }
  return conversations_.at(key);
}

ConverseResult AgentEngine::converse(const ConverseInput &input) {
  require_id(input.self_id, "self_id");
  require_id(input.partner_id, "partner_id");
  require_text(input.statement, kMaximumTextBytes, "statement");
  require_text(input.dialogue_situation, kMaximumTextBytes, "dialogue_situation");
  require_text(input.relationship, kMaximumShortTextBytes, "relationship");
  require_list(input.world_knowledge, kMaximumTextBytes, "world_knowledge");
  const auto &speaker = profile(input.self_id);
  const auto &partner = profile(input.partner_id);
  const auto turn_limit = bounded_turn_limit(input.max_conversation_turn);
  const ConversationKey key{speaker.id, partner.id};

  std::vector<ConversationTurn> history;
  int earlier_replies = 0;
  {
    std::lock_guard lock(mutex_);
    if (input.start_conversation) {
      conversations_.erase(key);
    }
    const auto &conversation = touch_conversation(key);
    history = conversation.turns;
    earlier_replies = conversation.replies;
  }

  const auto statement = std::string(trim(input.statement));
  const bool closing = input.end_conversation_forcefully || is_farewell_statement(statement) ||
                       earlier_replies + 1 >= turn_limit;
  const auto memories = memories_->recent(speaker.id, kRecalledMemories);

  // The model may call a remote endpoint, so it runs without the engine lock.
  const ReplyRequest request{speaker,
                             partner,
                             history,
                             statement,
                             input.dialogue_situation,
                             input.relationship,
                             input.world_knowledge,
                             memories,
                             input.start_conversation || history.empty(),
                             closing};
  ConverseResult result;
  result.response = model_->reply(request);
  result.conversation_end = closing;

  std::vector<ConversationTurn> finished_turns;
  {
    std::lock_guard lock(mutex_);
    auto &conversation = touch_conversation(key);
    if (!statement.empty()) {
      conversation.turns.push_back({partner.name, statement});
    }
    conversation.turns.push_back({speaker.name, result.response});
    ++conversation.replies;
    if (conversation.turns.size() > kMaximumKeptTurns) {
      const auto excess = conversation.turns.size() - kMaximumKeptTurns;
      conversation.turns.erase(conversation.turns.begin(),
                               conversation.turns.begin() + static_cast<std::ptrdiff_t>(excess));
    }
    if (closing) {
      finished_turns = std::move(conversation.turns);
      conversations_.erase(key);
    }
  }

  if (closing) {
    result.conversation_summary = model_->summarize(speaker, history_lines(finished_turns));
  }
  return result;
}

std::string AgentEngine::summarize(const std::string &agent_id,
                                   const std::vector<std::string> &history) {
  require_id(agent_id, "agent_id");
  require_list(history, kMaximumTextBytes, "conversation_history");
  return model_->summarize(profile(agent_id), history);
}

MemoryResult AgentEngine::add_memory(const std::string &agent_id, const std::string &description,
                                     const std::string &event_datetime) {
  require_id(agent_id, "agent_id");
  const auto &agent = profile(agent_id);
  const auto event = std::string(trim(description));
  if (event.empty()) {
    throw std::invalid_argument("event_description is required");
  }

  const auto rating = model_->rate_memory(agent, event);
  AgentMemory memory;
  memory.agent_id = agent.id;
  memory.description = event;
  memory.event_datetime = event_datetime;
  memory.priority = rating.priority;
  memory.reason = rating.reason;
  // validate_memory (inside add) rejects oversized or unclean text before anything is stored.
  memories_->add(memory);

  return {std::string(kMemoryStoredMessage), memory.priority, memory.reason};
}

std::vector<std::string> AgentEngine::player_responses(const PlayerResponseInput &input) {
  require_id(input.agent_id, "agent_id");
  require_id(input.player_id, "player_id");
  require_text(input.dialogue_situation, kMaximumTextBytes, "dialogue_situation");
  require_list(input.world_knowledge, kMaximumTextBytes, "world_knowledge");
  const auto &first = profile(input.agent_id);
  const auto &second = profile(input.player_id);

  // The request names both agents; whichever is a player agent speaks the options. The
  // conversation key is (speaking agent, player agent), as in ConverseReq self/partner.
  const bool first_is_player = first.is_player || !second.is_player;
  const auto &player = first_is_player ? first : second;
  const auto &partner = first_is_player ? second : first;

  std::vector<ConversationTurn> history;
  {
    std::lock_guard lock(mutex_);
    const auto found = conversations_.find({partner.id, player.id});
    if (found != conversations_.end()) {
      history = found->second.turns;
    }
  }

  const PlayerOptionsRequest request{player, partner, history, input.world_knowledge,
                                     input.dialogue_situation};
  return model_->player_options(request);
}

std::vector<KnowledgeHit> AgentEngine::search_knowledge(const std::string &query, int top_k) const {
  require_text(query, kMaximumTextBytes, "query");
  if (top_k < 0) {
    throw std::invalid_argument("top_k must not be negative");
  }
  const int limit =
      top_k == 0 ? kDefaultKnowledgeResults : std::min(top_k, kMaximumKnowledgeResults);
  return knowledge_.search(query, static_cast<std::size_t>(limit));
}

std::string AgentEngine::time_and_status(const std::string &reference_time) const {
  require_text(reference_time, kMaximumDateTimeBytes, "reference_time");
  const auto minute = parse_minute_of_day(reference_time);
  if (!minute) {
    throw std::invalid_argument("reference_time must contain HH:MM");
  }

  std::string status;
  for (const auto &agent : agents_) {
    if (agent.is_player) {
      continue;
    }

    const auto block = std::ranges::find_if(agent.schedule, [&](const ScheduleBlock &entry) {
      return *minute >= entry.start_minute && *minute < entry.end_minute;
    });
    std::string sentence = agent.name + " is ";
    if (block != agent.schedule.end()) {
      sentence += block_task(*block);
    } else if (!agent.residence.empty()) {
      sentence += "resting at " + agent.residence;
    } else {
      sentence += "resting";
    }

    if (!status.empty()) {
      status += " ";
    }
    status += sentence + ".";
  }
  return status;
}

LocationChoice AgentEngine::select_location(const LocationInput &input) const {
  require_id(input.agent_id, "agent_id");
  require_text(input.current_task, kMaximumTextBytes, "current_task");
  require_text(input.residence, kMaximumShortTextBytes, "residence");
  require_text(input.current_location, kMaximumShortTextBytes, "current_location");
  require_list(input.locations, kMaximumShortTextBytes, "locations");
  require_list(input.neighbor_area_locations, kMaximumShortTextBytes, "neighbor_area_locations");
  const auto &agent = profile(input.agent_id);

  std::vector<std::string> candidates = input.locations;
  for (const auto &location : input.neighbor_area_locations) {
    if (std::ranges::find(candidates, location) == candidates.end()) {
      candidates.push_back(location);
    }
  }

  const auto task_terms = keyword_tokens(input.current_task);
  if (const auto *best = most_relevant(candidates, task_terms)) {
    return {*best, "It is the place that fits the task \"" + input.current_task + "\"."};
  }

  const auto residence = input.residence.empty() ? agent.residence : input.residence;
  const bool resting = has_keyword(input.current_task, "rest") ||
                       has_keyword(input.current_task, "sleep") ||
                       has_keyword(input.current_task, "home");
  if (resting && !residence.empty()) {
    return {residence, "The task is to rest, so " + agent.name + " goes home."};
  }
  if (!input.current_location.empty()) {
    return {input.current_location, "No other place fits better, so " + agent.name + " stays."};
  }
  if (!candidates.empty()) {
    return {candidates.front(), "It is the first place available."};
  }
  if (!residence.empty()) {
    return {residence, "No place was offered, so " + agent.name + " goes home."};
  }
  throw std::invalid_argument("No location was offered");
}

std::string AgentEngine::select_action(const ActionInput &input) const {
  require_agent(input.agent_id);
  require_text(input.task, kMaximumTextBytes, "task");
  require_text(input.location, kMaximumShortTextBytes, "location");
  require_list(input.interactables, kMaximumShortTextBytes, "interactables");
  require_list(input.game_commands, kMaximumShortTextBytes, "game_commands");

  auto goal_terms = keyword_tokens(input.task);
  for (auto &term : keyword_tokens(input.location)) {
    goal_terms.push_back(std::move(term));
  }

  if (const auto *command = most_relevant(input.game_commands, goal_terms)) {
    return *command;
  }
  if (const auto *interactable = most_relevant(input.interactables, goal_terms)) {
    return *interactable;
  }
  if (!input.game_commands.empty()) {
    return input.game_commands.front();
  }
  if (!input.interactables.empty()) {
    return input.interactables.front();
  }
  throw std::invalid_argument("No action was offered");
}

std::vector<PlanStep> AgentEngine::breakdown_schedule(const ScheduleInput &input) const {
  require_agent(input.agent_id);
  require_text(input.schedule, kMaximumShortTextBytes, "schedule");
  require_text(input.location, kMaximumShortTextBytes, "location");
  require_text(input.type, kMaximumShortTextBytes, "type");
  require_list(input.interactables, kMaximumShortTextBytes, "interactables");

  const auto separator = input.schedule.find_first_of("-~");
  if (separator == std::string::npos) {
    throw std::invalid_argument("schedule must be HH:MM-HH:MM");
  }
  const auto start = parse_minute_of_day(std::string_view(input.schedule).substr(0, separator));
  auto end = parse_minute_of_day(std::string_view(input.schedule).substr(separator + 1));
  if (!start || !end) {
    throw std::invalid_argument("schedule must be HH:MM-HH:MM");
  }
  if (*end <= *start) {
    *end += kMinutesPerDay;
  }

  const int interval = input.interval == 0 ? kDefaultScheduleIntervalMinutes : input.interval;
  if (interval < kMinimumScheduleIntervalMinutes || interval > kMinutesPerDay) {
    throw std::invalid_argument("interval must be between 5 and 1440 minutes");
  }

  const auto task = input.type.empty() ? std::string("routine work") : input.type;
  const auto described_task = input.location.empty() ? task : task + " at " + input.location;
  std::vector<PlanStep> steps;
  for (int minute = *start; minute < *end && steps.size() < kMaximumBreakdownSteps;
       minute += interval) {
    PlanStep step;
    step.start = format_minute_of_day(minute);
    step.end = format_block_end(std::min(minute + interval, *end));
    step.task = described_task;
    if (!input.interactables.empty()) {
      step.interactables.push_back(input.interactables[steps.size() % input.interactables.size()]);
    }
    steps.push_back(std::move(step));
  }
  return steps;
}

std::vector<PlanStep> AgentEngine::make_plan(const std::string &agent_id) const {
  require_id(agent_id, "agent_id");
  const auto &agent = profile(agent_id);

  std::vector<PlanStep> steps;
  for (const auto &block : agent.schedule) {
    PlanStep step;
    step.start = format_minute_of_day(block.start_minute);
    step.end = format_block_end(block.end_minute);
    step.task = block_task(block);
    steps.push_back(std::move(step));
  }
  return steps;
}

std::size_t AgentEngine::agent_count() const {
  return agents_.size();
}

std::size_t AgentEngine::knowledge_count() const {
  return knowledge_.size();
}

} // namespace chronicles::agent_engine
