#include "services/agent_engine/agent_engine_model.hpp"

#include "services/agent_engine/agent_engine_text.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

namespace chronicles::agent_engine {
namespace {

constexpr std::size_t kSituationHintBytes = 160;
constexpr std::size_t kQuotedLineBytes = 160;
constexpr std::size_t kMaximumSummaryTopics = 3;
constexpr int kBaseMemoryPriority = 2;

constexpr std::array<std::string_view, 3> kFarewellWords{"bye", "goodbye", "farewell"};
// Multi-word goodbyes, matched against the ASCII-lowercased statement.
constexpr std::array<std::string_view, 6> kFarewellPhrases{"see you", "got to go", "have to go",
                                                           "must go", "get going", "take care"};
constexpr std::array<std::string_view, 5> kGreetingWords{"hello", "hi", "hey", "greetings",
                                                         "morning"};
constexpr std::array<std::string_view, 3> kThanksWords{"thanks", "thank", "grateful"};
constexpr std::array<std::string_view, 5> kHomeWords{"home", "house", "live", "living",
                                                     "residence"};

// Event words that make a memory worth keeping, with the priority each adds.
constexpr std::array<std::pair<std::string_view, int>, 14> kSignificantWords{{
    {"death", 4},
    {"promise", 3},
    {"danger", 3},
    {"attack", 3},
    {"love", 3},
    {"secret", 3},
    {"war", 3},
    {"quest", 2},
    {"gift", 2},
    {"family", 2},
    {"friend", 2},
    {"treasure", 2},
    {"home", 1},
    {"help", 1},
}};

bool mentions_any(std::string_view text, std::span<const std::string_view> words) {
  return std::ranges::any_of(words, [&](std::string_view word) { return has_keyword(text, word); });
}

bool asks_identity(std::string_view statement) {
  const bool who_are_you = has_keyword(statement, "who") && has_keyword(statement, "you");
  const bool your_name = has_keyword(statement, "your") && has_keyword(statement, "name");
  return who_are_you || your_name;
}

// How many of `terms` appear among the keywords of `text`.
std::size_t shared_keywords(const std::vector<std::string> &terms, std::string_view text) {
  const auto text_terms = keyword_tokens(text);
  return static_cast<std::size_t>(std::ranges::count_if(terms, [&](const std::string &term) {
    return std::ranges::find(text_terms, term) != text_terms.end();
  }));
}

// The memory sharing the most keywords with the statement, if any shares one.
const AgentMemory *best_matching_memory(std::string_view statement,
                                        std::span<const AgentMemory> memories) {
  const auto statement_terms = keyword_tokens(statement);
  const AgentMemory *best = nullptr;
  std::size_t best_overlap = 0;
  for (const auto &memory : memories) {
    const auto overlap = shared_keywords(statement_terms, memory.description);
    if (overlap > best_overlap) {
      best_overlap = overlap;
      best = &memory;
    }
  }
  return best;
}

// The sentence of `text` sharing the most keywords with `statement` (the first on ties).
std::string most_relevant_sentence(std::string_view text, std::string_view statement) {
  const auto statement_terms = keyword_tokens(statement);
  std::string_view best;
  std::size_t best_overlap = 0;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find_first_of(".!?", start);
    const auto length = end == std::string_view::npos ? text.size() - start : end - start + 1;
    const auto sentence = trim(text.substr(start, length));
    const auto overlap = shared_keywords(statement_terms, sentence);
    if (!sentence.empty() && (best.empty() || overlap > best_overlap)) {
      best = sentence;
      best_overlap = overlap;
    }
    start += length;
  }
  return truncate_utf8(best, kQuotedLineBytes);
}

std::string greeting_line(const ReplyRequest &request) {
  std::string line = "Hello, " + request.partner.name + ". ";
  const auto situation = first_sentence(request.dialogue_situation, kSituationHintBytes);
  if (situation.empty()) {
    line += "What is on your mind?";
  } else {
    line += "I hear you have your hands full: " + situation;
  }
  return line;
}

std::string farewell_line(const ReplyRequest &request) {
  return "Take care, " + request.partner.name + ". Come and talk to me again soon.";
}

std::string identity_line(const AgentProfile &speaker) {
  std::string line = "I am " + speaker.name + ".";
  const auto persona = first_sentence(speaker.summary, kSituationHintBytes);
  if (!persona.empty()) {
    line += " " + persona;
  }
  return line;
}

std::string home_line(const AgentProfile &speaker) {
  if (speaker.residence.empty()) {
    return "I have not settled anywhere for long. Wherever the road takes me, I suppose.";
  }
  return "I live at " + speaker.residence + ". It is quiet there, and that suits me.";
}

// Lines used when nothing more specific applies, rotated by turn so repeats are rarer.
std::string fallback_line(const ReplyRequest &request) {
  const std::array<std::string, 3> lines{
      "Tell me more, " + request.partner.name + ".",
      std::string("I have been thinking about that as well."),
      std::string("Hmm. I am not sure I know much about that."),
  };
  return lines[request.history.size() % lines.size()];
}

std::string speaker_of(std::string_view history_line) {
  const auto separator = history_line.find(':');
  if (separator == std::string_view::npos) {
    return {};
  }
  return std::string(trim(history_line.substr(0, separator)));
}

std::string text_of(std::string_view history_line) {
  const auto separator = history_line.find(':');
  if (separator == std::string_view::npos) {
    return std::string(trim(history_line));
  }
  return std::string(trim(history_line.substr(separator + 1)));
}

std::string join_names(const std::vector<std::string> &names) {
  std::string joined;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (index > 0) {
      joined += index + 1 == names.size() ? " and " : ", ";
    }
    joined += names[index];
  }
  return joined;
}

// Most frequent keywords of the conversation, excluding participant names.
std::vector<std::string> conversation_topics(std::span<const std::string> history,
                                             const std::vector<std::string> &participants) {
  std::vector<std::string> excluded;
  for (const auto &participant : participants) {
    for (auto &token : keyword_tokens(participant)) {
      excluded.push_back(std::move(token));
    }
  }

  std::map<std::string, int> counts;
  std::vector<std::string> first_seen;
  for (const auto &line : history) {
    for (auto &token : keyword_tokens(text_of(line))) {
      if (std::ranges::find(excluded, token) != excluded.end()) {
        continue;
      }
      if (counts[token]++ == 0) {
        first_seen.push_back(token);
      }
    }
  }

  std::ranges::stable_sort(first_seen, [&](const std::string &left, const std::string &right) {
    return counts[left] > counts[right];
  });
  if (first_seen.size() > kMaximumSummaryTopics) {
    first_seen.resize(kMaximumSummaryTopics);
  }
  return first_seen;
}

} // namespace

bool is_farewell_statement(std::string_view statement) {
  const auto lowered = ascii_lower(statement);
  const bool phrase = std::ranges::any_of(kFarewellPhrases, [&](std::string_view farewell) {
    return lowered.find(farewell) != std::string::npos;
  });
  return phrase || mentions_any(statement, kFarewellWords);
}

std::string clean_generated_line(std::string_view text, std::size_t maximum_bytes) {
  std::string line;
  line.reserve(text.size());
  for (const char character : text) {
    const bool line_break = character == '\n' || character == '\r' || character == '\t';
    line.push_back(line_break ? ' ' : character);
  }
  const auto trimmed = trim(line);
  if (trimmed.empty() || !is_clean_utf8(trimmed)) {
    return {};
  }
  return truncate_utf8(trimmed, maximum_bytes);
}

std::string ScriptedDialogueModel::reply(const ReplyRequest &request) {
  const auto statement = trim(request.statement);
  std::string line;
  if (request.closing) {
    line = farewell_line(request);
  } else if (statement.empty() || (request.opening && mentions_any(statement, kGreetingWords))) {
    line = greeting_line(request);
  } else if (mentions_any(statement, kGreetingWords)) {
    line = "Hello again, " + request.partner.name + ".";
  } else if (mentions_any(statement, kThanksWords)) {
    line = "You are welcome, " + request.partner.name + ".";
  } else if (asks_identity(statement)) {
    line = identity_line(request.speaker);
  } else if (mentions_any(statement, kHomeWords)) {
    line = home_line(request.speaker);
  } else if (const auto *memory = best_matching_memory(statement, request.memories)) {
    line = "I remember this: " + most_relevant_sentence(memory->description, statement);
  } else if (!request.world_knowledge.empty()) {
    line = "From what I know, " + first_sentence(request.world_knowledge.front(), kQuotedLineBytes);
  } else {
    line = fallback_line(request);
  }
  return truncate_utf8(line, kMaximumReplyBytes);
}

std::vector<std::string>
ScriptedDialogueModel::player_options(const PlayerOptionsRequest &request) {
  std::vector<std::string> options;
  if (request.history.size() < 2) {
    options.push_back("How have you been, " + request.partner.name + "?");
  }
  options.push_back(request.world_knowledge.empty() ? "Tell me more about that."
                                                    : "What else do you know about that?");
  if (!request.partner.residence.empty() && options.size() < 2) {
    options.push_back("How is life at " + request.partner.residence + "?");
  }
  if (options.size() < 2) {
    options.push_back("Who are you, really?");
  }
  options.push_back("I should get going. Goodbye.");

  for (auto &option : options) {
    option = truncate_utf8(option, kMaximumOptionBytes);
  }
  return options;
}

std::string ScriptedDialogueModel::summarize(const AgentProfile &agent,
                                             std::span<const std::string> history) {
  if (history.empty()) {
    return agent.name + " had a brief encounter in which nothing was said.";
  }

  std::vector<std::string> others;
  for (const auto &line : history) {
    auto speaker = speaker_of(line);
    if (!speaker.empty() && speaker != agent.name &&
        std::ranges::find(others, speaker) == others.end()) {
      others.push_back(std::move(speaker));
    }
  }

  std::string summary = agent.name + " talked with " +
                        (others.empty() ? std::string("someone") : join_names(others)) + ".";
  std::vector<std::string> participants = others;
  participants.push_back(agent.name);
  const auto topics = conversation_topics(history, participants);
  if (!topics.empty()) {
    summary += " They talked about " + join_names(topics) + ".";
  }

  const auto &last = history.back();
  const auto last_speaker = speaker_of(last);
  const auto last_text = truncate_utf8(text_of(last), kQuotedLineBytes);
  if (!last_text.empty()) {
    summary += " It ended with " + (last_speaker.empty() ? std::string("someone") : last_speaker) +
               " saying \"" + last_text + "\".";
  }
  return truncate_utf8(summary, kMaximumSummaryBytes);
}

MemoryRating ScriptedDialogueModel::rate_memory(const AgentProfile &, std::string_view event) {
  int priority = kBaseMemoryPriority;
  std::vector<std::string> reasons;
  for (const auto &[word, weight] : kSignificantWords) {
    if (has_keyword(event, word)) {
      priority += weight;
      reasons.emplace_back(word);
    }
  }

  MemoryRating rating;
  rating.priority = std::clamp(priority, kMinimumMemoryPriority, kMaximumMemoryPriority);
  rating.reason = reasons.empty() ? "Routine exchange without lasting consequences."
                                  : "Mentions " + join_names(reasons) + ".";
  return rating;
}

} // namespace chronicles::agent_engine
