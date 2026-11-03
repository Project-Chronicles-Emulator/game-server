#pragma once

// HTTP routes of the agent engine (src/services/agent_engine/agent_engine.hpp).
//
// Client routes, all relative to the hard-coded base URL (static analysis; docs/agent-engine.md):
//   GET  {base}                            reachability check (any 2xx)
//   GET  {base}/agents/list                AgentListRes
//   POST {base}/agent/{id}/load            AddAgentRes (no request body)
//   POST {base}/converse                   ConverseReq -> ConverseRes
//   POST {base}/summarize-conversation     SummarizeConversationReq -> SummarizeConversationRes
//   POST {base}/add-memory                 AddMemoryReq -> AddMemoryRes
//   POST {base}/generate-player-response   GeneratePlayerResponseReq -> GeneratePlayerResponseRes
//   POST {base}/search/knowledge-hybrid    SearchKnowledgeReq -> SearchKnowledgeRes
// Bodies are protobuf JSON with proto field names. The client parses responses strictly, so
// they contain only schema fields. It reads them only for HTTP 200-206.
//
// Emulator-defined routes for DTOs without a client caller in this build:
//   POST {base}/time-and-status, /select-location, /select-action, /breakdown-schedule
//   POST {base}/agent/{id}/make-plan       MakePlanRes (no request schema was recovered)

#include "services/agent_engine/agent_engine.hpp"
#include "auth/native_auth.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace chronicles::agent_engine {

// Large enough for 64 knowledge strings of kMaximumTextBytes plus JSON overhead.
inline constexpr std::size_t kMaximumRequestBodyBytes = 192 * 1024;

class AgentEngineRest {
public:
  explicit AgentEngineRest(AgentEngine &engine);

  NativeHttpResponse handle(std::string_view method, std::string_view target,
                            std::string_view content_type, const std::string &body);

private:
  NativeHttpResponse dispatch(std::string_view method, std::string_view path,
                              std::string_view content_type, const std::string &body);

  AgentEngine &engine_;
};

} // namespace chronicles::agent_engine
