#include "services/agent_engine/agent_engine_rest.hpp"

#include "p_ae2c.rest.pb.h"
#include "p_c2ae.rest.pb.h"

#include <google/protobuf/util/json_util.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <functional>
#include <optional>

namespace chronicles::agent_engine {
namespace {

namespace rest = x5::pb::rest;

constexpr std::string_view kJsonContentType = "application/json";
// Status text of successful responses. The client does not read it; any value parses.
constexpr std::string_view kSuccessStatus = "success";
constexpr std::string_view kAgentPrefix = "/agent/";
constexpr std::string_view kLoadAction = "load";
constexpr std::string_view kMakePlanAction = "make-plan";

// A request the adapter refuses before it reaches the engine.
struct RequestRejected {
  unsigned status{};
  std::string_view code;
};

NativeHttpResponse error_response(unsigned status, std::string_view code) {
  // Every *Res message has field 1 "status", so this body also parses as the expected type.
  NativeHttpResponse response;
  response.status = status;
  response.content_type = std::string(kJsonContentType);
  response.body = "{\"status\":\"" + std::string(code) + "\"}";
  return response;
}

NativeHttpResponse json_response(const google::protobuf::Message &message) {
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  options.always_print_fields_with_no_presence = true;

  NativeHttpResponse response;
  response.content_type = std::string(kJsonContentType);
  const auto status = google::protobuf::util::MessageToJsonString(message, &response.body, options);
  if (!status.ok()) {
    return error_response(500, "serialization_failed");
  }
  return response;
}

template <class Message>
Message parse_body(std::string_view content_type, const std::string &body) {
  if (!content_type.starts_with(kJsonContentType)) {
    throw RequestRejected{415, "json_required"};
  }

  // Unknown fields are ignored so a later client build adding fields still parses.
  google::protobuf::util::JsonParseOptions options;
  options.ignore_unknown_fields = true;
  Message message;
  const auto status = google::protobuf::util::JsonStringToMessage(body, &message, options);
  if (!status.ok()) {
    throw RequestRejected{400, "invalid_json"};
  }
  return message;
}

std::vector<std::string> to_vector(const google::protobuf::RepeatedPtrField<std::string> &field) {
  if (static_cast<std::size_t>(field.size()) > kMaximumListEntries) {
    throw std::invalid_argument("A list field has too many entries");
  }
  return {field.begin(), field.end()};
}

int bounded_int(std::uint64_t value) {
  constexpr std::uint64_t kLimit = 1'000'000;
  return static_cast<int>(std::min(value, kLimit));
}

void write_agent(const AgentProfile &profile, rest::Agent &agent) {
  agent.set_id(profile.id);
  agent.set_name(profile.name);
  agent.set_is_player(profile.is_player);
}

void write_plan(const PlanStep &step, rest::Plan &plan) {
  plan.set_start(step.start);
  plan.set_end(step.end);
  plan.set_task(step.task);
  for (const auto &interactable : step.interactables) {
    plan.add_selected_interactable(interactable);
  }
}

// "/agent/{id}/{action}" -> {id, action}. Ids use the characters of catalog agent ids.
std::optional<std::pair<std::string, std::string>> agent_route(std::string_view path) {
  if (!path.starts_with(kAgentPrefix)) {
    return std::nullopt;
  }
  const auto rest_of_path = path.substr(kAgentPrefix.size());
  const auto separator = rest_of_path.find('/');
  if (separator == std::string_view::npos || separator == 0 || separator > kMaximumIdBytes) {
    return std::nullopt;
  }

  const auto id = rest_of_path.substr(0, separator);
  const bool valid_id = std::ranges::all_of(id, [](char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') || character == '-' || character == '_';
  });
  if (!valid_id) {
    return std::nullopt;
  }
  return std::pair{std::string(id), std::string(rest_of_path.substr(separator + 1))};
}

NativeHttpResponse agent_list(AgentEngine &engine) {
  const auto directory = engine.directory();
  rest::AgentListRes response;
  response.set_status(std::string(kSuccessStatus));
  for (const auto &agent : directory.loaded) {
    write_agent(agent, *response.add_current_agents());
  }
  for (const auto &agent : directory.stored) {
    write_agent(agent, *response.add_agents_in_db());
  }
  return json_response(response);
}

NativeHttpResponse load_agent(AgentEngine &engine, const std::string &agent_id) {
  const auto agent = engine.load(agent_id);
  rest::AddAgentRes response;
  response.set_status(std::string(kSuccessStatus));
  response.set_name(agent.name);
  response.set_summary(agent.summary);
  response.set_is_player(agent.is_player);
  return json_response(response);
}

NativeHttpResponse make_plan(AgentEngine &engine, const std::string &agent_id) {
  rest::MakePlanRes response;
  response.set_status(std::string(kSuccessStatus));
  for (const auto &step : engine.make_plan(agent_id)) {
    write_plan(step, *response.add_plan());
  }
  return json_response(response);
}

NativeHttpResponse converse(AgentEngine &engine, const rest::ConverseReq &request) {
  ConverseInput input;
  input.self_id = request.self_id();
  input.partner_id = request.partner_id();
  input.start_conversation = request.start_conversation();
  input.statement = request.statement();
  input.dialogue_situation = request.dialogue_situation();
  input.relationship = request.relationship();
  input.max_conversation_turn = bounded_int(request.max_conversation_turn());
  input.end_conversation_forcefully = request.end_conversation_forcefully();
  input.world_knowledge = to_vector(request.world_knowledge());
  const auto result = engine.converse(input);

  rest::ConverseRes response;
  response.set_status(std::string(kSuccessStatus));
  response.set_response(result.response);
  response.set_conversation_end(result.conversation_end);
  response.set_conversation_summary(result.conversation_summary);
  return json_response(response);
}

NativeHttpResponse summarize(AgentEngine &engine, const rest::SummarizeConversationReq &request) {
  rest::SummarizeConversationRes response;
  response.set_status(std::string(kSuccessStatus));
  response.set_summary(
      engine.summarize(request.agent_id(), to_vector(request.conversation_history())));
  return json_response(response);
}

NativeHttpResponse add_memory(AgentEngine &engine, const rest::AddMemoryReq &request) {
  const auto result =
      engine.add_memory(request.agent_id(), request.event_description(), request.event_datetime());
  rest::AddMemoryRes response;
  response.set_status(std::string(kSuccessStatus));
  response.set_message(result.message);
  response.set_priority(result.priority);
  response.set_reason(result.reason);
  return json_response(response);
}

NativeHttpResponse player_response(AgentEngine &engine,
                                   const rest::GeneratePlayerResponseReq &request) {
  PlayerResponseInput input;
  input.agent_id = request.agent_id();
  input.player_id = request.player_id();
  input.world_knowledge = to_vector(request.world_knowledge());
  input.dialogue_situation = request.dialogue_situation();

  rest::GeneratePlayerResponseRes response;
  response.set_status(std::string(kSuccessStatus));
  for (const auto &line : engine.player_responses(input)) {
    response.add_response(line);
  }
  return json_response(response);
}

NativeHttpResponse search_knowledge(AgentEngine &engine, const rest::SearchKnowledgeReq &request) {
  rest::SearchKnowledgeRes response;
  response.set_status(std::string(kSuccessStatus));
  for (const auto &hit : engine.search_knowledge(request.query(), request.top_k())) {
    response.add_query_result(format_knowledge_result(hit));
  }
  return json_response(response);
}

NativeHttpResponse time_and_status(AgentEngine &engine, const rest::TimeAndStatusReq &request) {
  rest::TimeAndStatusRes response;
  response.set_status(std::string(kSuccessStatus));
  response.set_agent_status(engine.time_and_status(request.reference_time()));
  return json_response(response);
}

NativeHttpResponse select_location(AgentEngine &engine, const rest::SelectLocationReq &request) {
  LocationInput input;
  input.agent_id = request.agent_id();
  input.current_task = request.current_task();
  input.locations = to_vector(request.locations());
  input.residence = request.residence();
  input.current_location = request.current_location();
  input.neighbor_area_locations = to_vector(request.neighbor_area_locations());
  const auto choice = engine.select_location(input);

  rest::SelectLocationRes response;
  response.set_status(std::string(kSuccessStatus));
  response.set_selected_location(choice.location);
  response.set_reason(choice.reason);
  return json_response(response);
}

NativeHttpResponse select_action(AgentEngine &engine, const rest::SelectActionReq &request) {
  ActionInput input;
  input.agent_id = request.agent_id();
  input.task = request.task();
  input.location = request.location();
  input.interactables = to_vector(request.interactables());
  input.game_commands = to_vector(request.game_commands());

  rest::SelectActionRes response;
  response.set_status(std::string(kSuccessStatus));
  response.set_action(engine.select_action(input));
  return json_response(response);
}

NativeHttpResponse breakdown_schedule(AgentEngine &engine,
                                      const rest::BreakdownScheduleReq &request) {
  ScheduleInput input;
  input.agent_id = request.agent_id();
  input.schedule = request.schedule();
  input.location = request.location();
  input.type = request.type();
  input.interactables = to_vector(request.interactables());
  input.interval = request.interval();

  rest::BreakdownScheduleRes response;
  response.set_status(std::string(kSuccessStatus));
  for (const auto &step : engine.breakdown_schedule(input)) {
    write_plan(step, *response.add_schedule_breakdown());
  }
  return json_response(response);
}

// POST routes whose body is one recovered request message.
template <class Request>
std::function<NativeHttpResponse(AgentEngine &, std::string_view, const std::string &)>
json_route(NativeHttpResponse (*handler)(AgentEngine &, const Request &)) {
  return [handler](AgentEngine &engine, std::string_view content_type, const std::string &body) {
    return handler(engine, parse_body<Request>(content_type, body));
  };
}

struct JsonRoute {
  std::string_view path;
  std::function<NativeHttpResponse(AgentEngine &, std::string_view, const std::string &)> handle;
};

const std::array<JsonRoute, 9> &json_routes() {
  static const std::array<JsonRoute, 9> routes{{
      {"/converse", json_route(&converse)},
      {"/summarize-conversation", json_route(&summarize)},
      {"/add-memory", json_route(&add_memory)},
      {"/generate-player-response", json_route(&player_response)},
      {"/search/knowledge-hybrid", json_route(&search_knowledge)},
      {"/time-and-status", json_route(&time_and_status)},
      {"/select-location", json_route(&select_location)},
      {"/select-action", json_route(&select_action)},
      {"/breakdown-schedule", json_route(&breakdown_schedule)},
  }};
  return routes;
}

} // namespace

AgentEngineRest::AgentEngineRest(AgentEngine &engine) : engine_(engine) {}

NativeHttpResponse AgentEngineRest::handle(std::string_view method, std::string_view target,
                                           std::string_view content_type, const std::string &body) {
  const auto path = target.substr(0, target.find('?'));
  if (body.size() > kMaximumRequestBodyBytes) {
    return error_response(413, "body_too_large");
  }

  NativeHttpResponse response;
  try {
    response = dispatch(method, path, content_type, body);
  } catch (const RequestRejected &rejected) {
    response = error_response(rejected.status, rejected.code);
  } catch (const UnknownAgent &) {
    response = error_response(404, "unknown_agent");
  } catch (const std::invalid_argument &) {
    response = error_response(400, "invalid_request");
  } catch (const std::exception &) {
    spdlog::error("Agent engine request failed");
    response = error_response(500, "service_error");
  }

  // Route and status only: request bodies are player conversation text.
  std::string logged_path(path.substr(0, 64));
  if (path.starts_with(kAgentPrefix)) {
    logged_path = "/agent/{id}" + std::string(path.substr(path.rfind('/')).substr(0, 32));
  }
  spdlog::info("Agent engine {} {} -> {}", method, logged_path, response.status);
  return response;
}

NativeHttpResponse AgentEngineRest::dispatch(std::string_view method, std::string_view path,
                                             std::string_view content_type,
                                             const std::string &body) {
  // The client's reachability check requests the base URL itself.
  if (path.empty() || path == "/") {
    if (method != "GET") {
      return error_response(405, "method_not_allowed");
    }
    NativeHttpResponse response;
    response.content_type = std::string(kJsonContentType);
    response.body = "{\"status\":\"ok\"}";
    return response;
  }

  if (path == "/agents/list") {
    if (method != "GET") {
      return error_response(405, "method_not_allowed");
    }
    return agent_list(engine_);
  }

  if (const auto route = agent_route(path)) {
    const auto &[agent_id, action] = *route;
    if (action != kLoadAction && action != kMakePlanAction) {
      return error_response(404, "route_not_found");
    }
    if (method != "POST") {
      return error_response(405, "method_not_allowed");
    }
    return action == kLoadAction ? load_agent(engine_, agent_id) : make_plan(engine_, agent_id);
  }

  const auto &routes = json_routes();
  const auto route = std::ranges::find(routes, path, &JsonRoute::path);
  if (route == routes.end()) {
    return error_response(404, "route_not_found");
  }
  if (method != "POST") {
    return error_response(405, "method_not_allowed");
  }
  return route->handle(engine_, content_type, body);
}

} // namespace chronicles::agent_engine
