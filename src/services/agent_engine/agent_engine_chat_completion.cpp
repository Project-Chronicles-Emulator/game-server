// OpenAI-compatible chat-completions backend for the agent engine (agent_engine_model.hpp).
// Prompts and generated text are player conversation content: they are never logged.

#include "services/agent_engine/agent_engine_model.hpp"

#include "services/agent_engine/agent_engine_text.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <charconv>
#include <chrono>
#include <future>
#include <stdexcept>

namespace chronicles::agent_engine {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

constexpr std::string_view kHttpScheme = "http://";
constexpr std::string_view kDefaultPort = "80";
constexpr std::string_view kDefaultPathPrefix = "/v1";
constexpr std::string_view kCompletionsPath = "/chat/completions";
constexpr std::size_t kMaximumResponseBytes = 256 * 1024;
constexpr double kTemperature = 0.7;
constexpr int kReplyTokens = 160;
constexpr int kOptionTokens = 120;
constexpr int kSummaryTokens = 220;
constexpr int kRatingTokens = 80;

bool valid_port(std::string_view port) {
  unsigned value{};
  const auto parsed = std::from_chars(port.data(), port.data() + port.size(), value);
  return !port.empty() && parsed.ec == std::errc{} && parsed.ptr == port.data() + port.size() &&
         value > 0 && value <= 65535;
}

// Runs `io` until the operation behind `future` completes or the deadline passes. Synchronous
// Beast calls ignore stream timeouts, so every step is asynchronous with one shared deadline.
template <class Result>
Result run_until_ready(asio::io_context &io, std::future<Result> future,
                       std::chrono::steady_clock::time_point deadline) {
  io.restart();
  io.run_until(deadline);
  if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    throw std::runtime_error("Agent model request timed out");
  }
  return future.get();
}

std::string persona_prompt(const AgentProfile &agent) {
  std::string prompt = "You are " + agent.name + ", a character in a fantasy MMORPG world.";
  if (!agent.summary.empty()) {
    prompt += " " + agent.summary;
  }
  if (!agent.residence.empty()) {
    prompt += " You live at " + agent.residence + ".";
  }
  prompt += " Stay in character, answer in one or two short sentences and never mention being "
            "an AI.";
  return prompt;
}

std::string bullet_list(std::span<const std::string> lines) {
  std::string text;
  for (const auto &line : lines) {
    text += "- " + line + "\n";
  }
  return text;
}

// One option per line; list markers such as "1." or "-" are removed.
std::vector<std::string> parse_option_lines(std::string_view text) {
  std::vector<std::string> options;
  std::size_t start = 0;
  while (start <= text.size() && options.size() < kMaximumPlayerOptions) {
    const auto end = text.find('\n', start);
    auto line = trim(text.substr(start, end == std::string_view::npos ? text.npos : end - start));
    const auto marker_end = line.find_first_not_of("-*0123456789.) ");
    line = marker_end == std::string_view::npos ? std::string_view{} : line.substr(marker_end);
    auto option = clean_generated_line(line, kMaximumOptionBytes);
    if (!option.empty()) {
      options.push_back(std::move(option));
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return options;
}

} // namespace

ChatCompletionEndpoint parse_chat_completion_url(std::string_view url) {
  if (!url.starts_with(kHttpScheme)) {
    throw std::invalid_argument("The agent model URL must start with http://");
  }

  auto rest = url.substr(kHttpScheme.size());
  const auto path_start = rest.find('/');
  const auto authority = rest.substr(0, path_start);
  auto prefix = path_start == std::string_view::npos ? std::string_view{} : rest.substr(path_start);
  while (prefix.ends_with('/')) {
    prefix.remove_suffix(1);
  }
  if (prefix.find_first_of("?#") != std::string_view::npos) {
    throw std::invalid_argument("The agent model URL must not contain a query or fragment");
  }

  ChatCompletionEndpoint endpoint;
  const auto port_separator = authority.rfind(':');
  endpoint.host = std::string(authority.substr(0, port_separator));
  endpoint.port = port_separator == std::string_view::npos
                      ? std::string(kDefaultPort)
                      : std::string(authority.substr(port_separator + 1));
  if (endpoint.host.empty() || !valid_port(endpoint.port)) {
    throw std::invalid_argument("The agent model URL needs a host and a valid port");
  }
  endpoint.target =
      std::string(prefix.empty() ? kDefaultPathPrefix : prefix) + std::string(kCompletionsPath);
  return endpoint;
}

ChatCompletionModel::ChatCompletionModel(ChatCompletionConfig config,
                                         std::unique_ptr<DialogueModel> fallback)
    : config_(std::move(config)), fallback_(std::move(fallback)) {
  if (!fallback_) {
    throw std::invalid_argument("The chat-completion model needs a fallback model");
  }
  if (config_.model.empty()) {
    throw std::invalid_argument("The chat-completion model needs a model name");
  }
}

std::string ChatCompletionModel::complete(const std::vector<Message> &messages,
                                          int maximum_tokens) {
  nlohmann::json request_body{{"model", config_.model},
                              {"temperature", kTemperature},
                              {"max_tokens", maximum_tokens},
                              {"stream", false},
                              {"messages", nlohmann::json::array()}};
  for (const auto &message : messages) {
    request_body["messages"].push_back({{"role", message.role}, {"content", message.content}});
  }

  try {
    // One deadline covers resolution, connection, the request and the response.
    const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
    asio::io_context io;
    tcp::resolver resolver(io);
    beast::tcp_stream stream(io);

    const auto endpoints = run_until_ready(
        io, resolver.async_resolve(config_.endpoint.host, config_.endpoint.port, asio::use_future),
        deadline);
    run_until_ready(io, stream.async_connect(endpoints, asio::use_future), deadline);

    http::request<http::string_body> request{http::verb::post, config_.endpoint.target, 11};
    request.set(http::field::host, config_.endpoint.host);
    request.set(http::field::content_type, "application/json");
    if (!config_.api_key.empty()) {
      request.set(http::field::authorization, "Bearer " + config_.api_key);
    }
    request.body() = request_body.dump();
    request.prepare_payload();
    run_until_ready(io, http::async_write(stream, request, asio::use_future), deadline);

    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(kMaximumResponseBytes);
    run_until_ready(io, http::async_read(stream, buffer, parser, asio::use_future), deadline);

    beast::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);

    const auto &response = parser.get();
    if (response.result() != http::status::ok) {
      spdlog::warn("Agent model endpoint answered HTTP {}; using the scripted model",
                   response.result_int());
      return {};
    }

    const auto document = nlohmann::json::parse(response.body());
    const auto &content = document.at("choices").at(0).at("message").at("content");
    return content.is_string() ? content.get<std::string>() : std::string{};
  } catch (const std::exception &) {
    // The exception text may quote the response; report only that the request failed.
    spdlog::warn("Agent model request failed; using the scripted model");
    return {};
  }
}

std::string ChatCompletionModel::reply(const ReplyRequest &request) {
  std::string context = "You are talking with " + request.partner.name + ".";
  if (!request.relationship.empty()) {
    context += " Your relationship: " + std::string(request.relationship) + ".";
  }
  if (!request.dialogue_situation.empty()) {
    context += " Their current situation: " + std::string(request.dialogue_situation);
  }
  if (!request.memories.empty()) {
    context += "\nThings you remember:\n";
    for (const auto &memory : request.memories) {
      context += "- " + memory.description + "\n";
    }
  }
  if (!request.world_knowledge.empty()) {
    context += "\nRelevant world knowledge:\n" + bullet_list(request.world_knowledge);
  }
  if (request.closing) {
    context += "\nThis is the end of the conversation: say goodbye.";
  }

  std::vector<Message> messages{{"system", persona_prompt(request.speaker) + "\n" + context}};
  for (const auto &turn : request.history) {
    const bool own_line = turn.speaker_name == request.speaker.name;
    messages.push_back({own_line ? "assistant" : "user", turn.text});
  }
  const auto statement = trim(request.statement);
  messages.push_back(
      {"user", statement.empty() ? std::string("(greets you)") : std::string(statement)});

  auto line = clean_generated_line(complete(messages, kReplyTokens), kMaximumReplyBytes);
  if (line.empty()) {
    return fallback_->reply(request);
  }
  return line;
}

std::vector<std::string> ChatCompletionModel::player_options(const PlayerOptionsRequest &request) {
  std::string prompt = "Suggest three short things " + request.player.name + " could say next to " +
                       request.partner.name + ". Write one per line, without numbering.";
  if (!request.dialogue_situation.empty()) {
    prompt += " Situation: " + std::string(request.dialogue_situation);
  }
  std::string transcript;
  for (const auto &turn : request.history) {
    transcript += turn.speaker_name + ": " + turn.text + "\n";
  }
  if (!request.world_knowledge.empty()) {
    transcript += "World knowledge:\n" + bullet_list(request.world_knowledge);
  }

  const std::vector<Message> messages{{"system", prompt}, {"user", transcript}};
  auto options = parse_option_lines(complete(messages, kOptionTokens));
  if (options.size() < kMinimumPlayerOptions) {
    return fallback_->player_options(request);
  }
  return options;
}

std::string ChatCompletionModel::summarize(const AgentProfile &agent,
                                           std::span<const std::string> history) {
  const std::vector<Message> messages{
      {"system", "Summarize this conversation in two sentences from the point of view of " +
                     agent.name + ", in the third person."},
      {"user", bullet_list(history)}};
  auto summary = clean_generated_line(complete(messages, kSummaryTokens), kMaximumSummaryBytes);
  if (summary.empty()) {
    return fallback_->summarize(agent, history);
  }
  return summary;
}

MemoryRating ChatCompletionModel::rate_memory(const AgentProfile &agent, std::string_view event) {
  const std::vector<Message> messages{
      {"system", "Rate how important this event is for " + agent.name +
                     " to remember, from 1 (mundane) to 10 (life-changing). Answer only with "
                     "JSON: {\"priority\": <integer>, \"reason\": \"<one sentence>\"}."},
      {"user", std::string(event)}};
  const auto text = complete(messages, kRatingTokens);

  try {
    const auto first = text.find('{');
    const auto last = text.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last < first) {
      return fallback_->rate_memory(agent, event);
    }

    const auto document = nlohmann::json::parse(text.substr(first, last - first + 1));
    MemoryRating rating;
    rating.priority = document.at("priority").get<int>();
    rating.reason =
        clean_generated_line(document.at("reason").get<std::string>(), kMaximumReasonBytes);
    if (rating.priority < kMinimumMemoryPriority || rating.priority > kMaximumMemoryPriority ||
        rating.reason.empty()) {
      return fallback_->rate_memory(agent, event);
    }
    return rating;
  } catch (const std::exception &) {
    return fallback_->rate_memory(agent, event);
  }
}

} // namespace chronicles::agent_engine
