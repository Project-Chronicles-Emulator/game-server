#pragma once

// Text generation behind the agent engine's conversational routes.
//
// The official service used a language model whose prompts and weights are not part of the
// client. ScriptedDialogueModel is the emulator's deterministic default: it answers from the
// agent persona, retrieved memories and the world knowledge the client attaches. An operator
// can instead point ChatCompletionModel at an OpenAI-compatible chat-completions endpoint
// (for example a local Ollama or llama.cpp server); any failure falls back to the scripted model.

#include "services/agent_engine/agent_engine_types.hpp"

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles::agent_engine {

// Generated text bounds. The client shows replies in the chat log and option buttons.
inline constexpr std::size_t kMaximumReplyBytes = 512;
inline constexpr std::size_t kMaximumOptionBytes = 160;
inline constexpr std::size_t kMaximumSummaryBytes = 1024;
inline constexpr std::size_t kMinimumPlayerOptions = 2;
inline constexpr std::size_t kMaximumPlayerOptions = 4;

struct ReplyRequest {
  const AgentProfile &speaker;
  const AgentProfile &partner;
  // Earlier turns of this conversation, oldest first; excludes `statement`.
  std::span<const ConversationTurn> history;
  std::string_view statement;
  std::string_view dialogue_situation;
  std::string_view relationship;
  std::span<const std::string> world_knowledge;
  std::span<const AgentMemory> memories;
  bool opening{};
  // The engine ends the conversation after this reply.
  bool closing{};
};

struct PlayerOptionsRequest {
  const AgentProfile &player;
  const AgentProfile &partner;
  std::span<const ConversationTurn> history;
  std::span<const std::string> world_knowledge;
  std::string_view dialogue_situation;
};

struct MemoryRating {
  int priority{kMinimumMemoryPriority};
  std::string reason;
};

class DialogueModel {
public:
  virtual ~DialogueModel() = default;

  // One line the speaker says next. Never empty.
  virtual std::string reply(const ReplyRequest &request) = 0;

  // Two to four lines the player may choose to say next.
  virtual std::vector<std::string> player_options(const PlayerOptionsRequest &request) = 0;

  // A short third-person summary of `history` ("Name: text" lines) from `agent`'s view.
  virtual std::string summarize(const AgentProfile &agent,
                                std::span<const std::string> history) = 0;

  // How important an event is for `agent` to remember, 1 (mundane) to 10 (life-changing).
  virtual MemoryRating rate_memory(const AgentProfile &agent, std::string_view event) = 0;
};

// True when a player statement says goodbye ("bye", "farewell", "see you", ...). The engine
// then lets the agent answer once more and ends the conversation.
bool is_farewell_statement(std::string_view statement);

// Bounded single-paragraph form of generated text: newlines become spaces and the result is
// cut to `maximum_bytes`. Returns empty text when nothing printable remains.
std::string clean_generated_line(std::string_view text, std::size_t maximum_bytes);

// Deterministic replies from persona, memories and supplied knowledge. Equal input always
// produces equal output, which keeps tests and support transcripts reproducible.
class ScriptedDialogueModel final : public DialogueModel {
public:
  std::string reply(const ReplyRequest &request) override;

  std::vector<std::string> player_options(const PlayerOptionsRequest &request) override;

  std::string summarize(const AgentProfile &agent, std::span<const std::string> history) override;

  MemoryRating rate_memory(const AgentProfile &agent, std::string_view event) override;
};

struct ChatCompletionEndpoint {
  std::string host;
  std::string port;
  // Path of the chat-completions resource, for example "/v1/chat/completions".
  std::string target;
};

// Accepts "http://host[:port][/prefix]"; the chat-completions path is appended to the prefix
// ("/v1" when the URL has no path). Only plain HTTP is supported: point remote HTTPS providers
// at a local proxy. Throws std::invalid_argument for other forms.
ChatCompletionEndpoint parse_chat_completion_url(std::string_view url);

struct ChatCompletionConfig {
  ChatCompletionEndpoint endpoint;
  std::string model;
  // Sent as "Authorization: Bearer <key>" when non-empty. Never logged.
  std::string api_key;
  std::chrono::milliseconds timeout{std::chrono::seconds(20)};
};

// Sends one chat-completions request per operation and validates the returned text. The
// fallback answers whenever the endpoint fails, times out or returns unusable text.
class ChatCompletionModel final : public DialogueModel {
public:
  ChatCompletionModel(ChatCompletionConfig config, std::unique_ptr<DialogueModel> fallback);

  std::string reply(const ReplyRequest &request) override;

  std::vector<std::string> player_options(const PlayerOptionsRequest &request) override;

  std::string summarize(const AgentProfile &agent, std::span<const std::string> history) override;

  MemoryRating rate_memory(const AgentProfile &agent, std::string_view event) override;

private:
  struct Message {
    std::string role;
    std::string content;
  };

  // The assistant text, or empty on any transport, status or format failure.
  std::string complete(const std::vector<Message> &messages, int maximum_tokens);

  ChatCompletionConfig config_;
  std::unique_ptr<DialogueModel> fallback_;
};

} // namespace chronicles::agent_engine
