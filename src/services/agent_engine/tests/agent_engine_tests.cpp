#include "services/agent_engine/agent_engine.hpp"
#include "services/agent_engine/agent_engine_rest.hpp"
#include "services/agent_engine/agent_engine_text.hpp"
#include "data/client_catalog.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace chronicles::agent_engine {
namespace {

const std::filesystem::path kProjectRoot{CHRONICLES_PROJECT_ROOT};
const std::filesystem::path kDataDirectory = kProjectRoot / "config" / "agent-engine";
const std::filesystem::path kCatalogDirectory = kProjectRoot / "data" / "generated" / "catalog";

AgentProfile npc_agent() {
  AgentProfile agent;
  agent.id = "2";
  agent.name = "Seren";
  agent.summary = "A cheerful house mate who tends the garden.";
  agent.residence = "the family home";
  agent.schedule = {{360, 480, "making breakfast", "the kitchen"},
                    {480, 720, "tending the garden", "the garden"}};
  return agent;
}

AgentProfile player_agent() {
  AgentProfile agent;
  agent.id = "0";
  agent.name = "Traveler";
  agent.is_player = true;
  return agent;
}

struct Fixture {
  std::shared_ptr<InMemoryAgentMemoryStore> memories = std::make_shared<InMemoryAgentMemoryStore>();
  AgentEngine engine{{npc_agent(), player_agent()},
                     KnowledgeIndex({"Village markets sell produce and crafted goods.",
                                     "Trade packs earn more when sold far from home.",
                                     "Seren: A cheerful house mate who tends the garden."}),
                     memories,
                     std::make_unique<ScriptedDialogueModel>()};
  AgentEngineRest rest{engine};
};

ConverseInput converse_input(std::string statement, bool start = false) {
  ConverseInput input;
  input.self_id = "2";
  input.partner_id = "0";
  input.start_conversation = start;
  input.statement = std::move(statement);
  input.max_conversation_turn = 10;
  return input;
}

nlohmann::json post_json(AgentEngineRest &rest, std::string_view path, const nlohmann::json &body,
                         unsigned expected_status = 200) {
  const auto response = rest.handle("POST", path, "application/json", body.dump());
  EXPECT_EQ(response.status, expected_status) << path << " " << response.body;
  EXPECT_EQ(response.content_type, "application/json");
  return nlohmann::json::parse(response.body);
}

std::set<std::string> keys_of(const nlohmann::json &object) {
  std::set<std::string> keys;
  for (const auto &[key, value] : object.items()) {
    keys.insert(key);
  }
  return keys;
}

TEST(AgentEngineText, TruncationKeepsWholeCodePoints) {
  const std::string korean = "\xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94";
  for (std::size_t limit = 0; limit <= korean.size(); ++limit) {
    const auto truncated = truncate_utf8(korean, limit);
    EXPECT_LE(truncated.size(), limit);
    EXPECT_TRUE(is_clean_utf8(truncated)) << limit;
  }
  EXPECT_EQ(truncate_utf8("short", 10), "short");
  EXPECT_EQ(truncate_utf8("a longer sentence", 10), "a longe...");
}

TEST(AgentEngineText, KeywordsFoldAsciiAndKeepQuestionWords) {
  EXPECT_EQ(keyword_tokens("Who is SEREN's friend?"),
            (std::vector<std::string>{"seren", "friend"}));
  EXPECT_TRUE(has_keyword("Who are you?", "who"));
  EXPECT_FALSE(is_clean_utf8(std::string("bad\x01text")));
  EXPECT_FALSE(is_clean_utf8(std::string("\xC3\x28")));
}

TEST(AgentEngineKnowledge, RanksRelevantFactsWithinTheClientThreshold) {
  const KnowledgeIndex index({"Village markets sell produce and crafted goods.",
                              "Trade packs earn more when sold far from home.",
                              "Monsters roam the wilds."});

  const auto hits = index.search("Where can I sell my trade packs?", 5);
  ASSERT_FALSE(hits.empty());
  EXPECT_EQ(hits.front().text, "Trade packs earn more when sold far from home.");
  // AIChatSearchKnowledgeDistanceThreshold: the client discards results above 0.7.
  EXPECT_LE(hits.front().distance, 0.7);
  for (std::size_t index_in_hits = 1; index_in_hits < hits.size(); ++index_in_hits) {
    EXPECT_GE(hits[index_in_hits].distance, hits[index_in_hits - 1].distance);
  }

  EXPECT_TRUE(index.search("", 5).empty());
  EXPECT_TRUE(index.search("trade", 0).empty());
}

TEST(AgentEngineKnowledge, ResultFormatSplitsLikeTheClient) {
  const auto text = format_knowledge_result({"Markets sell goods.", 0.25});
  EXPECT_EQ(text, "Markets sell goods., Distance:0.2500");
  const auto separator = text.find(kDistanceSeparator);
  ASSERT_NE(separator, std::string::npos);
  EXPECT_EQ(text.find(kDistanceSeparator, separator + 1), std::string::npos);
}

TEST(AgentEngineKnowledge, RejectsEntriesTheClientCannotSplit) {
  EXPECT_THROW(KnowledgeIndex({"bad, Distance:0.1"}), std::invalid_argument);
  EXPECT_THROW(KnowledgeIndex({"   "}), std::invalid_argument);
}

TEST(AgentEngineData, LoadsCatalogAgentsWithPersonaOverrides) {
  if (!std::filesystem::is_regular_file(kCatalogDirectory / "manifest.json")) {
    GTEST_SKIP() << "Exported client catalog is not present";
  }
  ClientCatalog catalog(kCatalogDirectory);
  const auto data = load_agent_engine_data(&catalog, kDataDirectory);

  const auto seren = std::ranges::find(data.agents, std::string("2"), &AgentProfile::id);
  ASSERT_NE(seren, data.agents.end());
  EXPECT_EQ(seren->name, "Seren");
  EXPECT_FALSE(seren->is_player);
  EXPECT_FALSE(seren->schedule.empty());

  const auto player = std::ranges::find(data.agents, std::string("0"), &AgentProfile::id);
  ASSERT_NE(player, data.agents.end());
  EXPECT_TRUE(player->is_player);
  EXPECT_FALSE(data.knowledge.empty());
  EXPECT_NO_THROW(KnowledgeIndex{data.knowledge});
}

TEST(AgentEngineData, CatalogWithoutOverridesUsesNpcNames) {
  if (!std::filesystem::is_regular_file(kCatalogDirectory / "manifest.json")) {
    GTEST_SKIP() << "Exported client catalog is not present";
  }
  ClientCatalog catalog(kCatalogDirectory);
  const auto data = load_agent_engine_data(&catalog, kProjectRoot / "missing-agent-engine-dir");

  const auto seren = std::ranges::find(data.agents, std::string("2"), &AgentProfile::id);
  ASSERT_NE(seren, data.agents.end());
  // Npc 35 is "[Housemate] Seren"; the role tag is not part of the name.
  EXPECT_EQ(seren->name, "Seren");
}

TEST(AgentEngineConversation, GreetsRepliesAndEndsAtTheTurnLimit) {
  Fixture fixture;
  auto opening = converse_input("", true);
  opening.max_conversation_turn = 3;
  const auto first = fixture.engine.converse(opening);
  EXPECT_NE(first.response.find("Traveler"), std::string::npos);
  EXPECT_FALSE(first.conversation_end);

  auto second_input = converse_input("Who are you?");
  second_input.max_conversation_turn = 3;
  const auto second = fixture.engine.converse(second_input);
  EXPECT_NE(second.response.find("Seren"), std::string::npos);
  EXPECT_FALSE(second.conversation_end);

  auto third_input = converse_input("Tell me about markets.");
  third_input.max_conversation_turn = 3;
  third_input.world_knowledge = {"Village markets sell produce and crafted goods."};
  const auto third = fixture.engine.converse(third_input);
  EXPECT_TRUE(third.conversation_end);
  EXPECT_FALSE(third.conversation_summary.empty());
}

TEST(AgentEngineConversation, FarewellAndForcedEndCloseTheConversation) {
  Fixture fixture;
  fixture.engine.converse(converse_input("", true));
  const auto farewell = fixture.engine.converse(converse_input("I should get going. Goodbye."));
  EXPECT_TRUE(farewell.conversation_end);

  fixture.engine.converse(converse_input("", true));
  auto forced = converse_input("Hmm.");
  forced.end_conversation_forcefully = true;
  EXPECT_TRUE(fixture.engine.converse(forced).conversation_end);
}

TEST(AgentEngineConversation, StoredMemoriesInformLaterReplies) {
  Fixture fixture;
  const auto stored = fixture.engine.add_memory(
      "2", "Traveler promised to bring Seren a rare flower for the garden.", "1000-03-01T06:00");
  EXPECT_GE(stored.priority, kMinimumMemoryPriority);
  EXPECT_LE(stored.priority, kMaximumMemoryPriority);
  EXPECT_FALSE(stored.reason.empty());

  fixture.engine.converse(converse_input("", true));
  const auto reply = fixture.engine.converse(converse_input("Did I promise you a flower?"));
  EXPECT_NE(reply.response.find("rare flower"), std::string::npos);
}

TEST(AgentEngineConversation, PlayerOptionsIncludeAWayOut) {
  Fixture fixture;
  fixture.engine.converse(converse_input("", true));

  PlayerResponseInput input;
  input.agent_id = "0";
  input.player_id = "2";
  const auto options = fixture.engine.player_responses(input);
  ASSERT_GE(options.size(), kMinimumPlayerOptions);
  ASSERT_LE(options.size(), kMaximumPlayerOptions);
  EXPECT_TRUE(is_farewell_statement(options.back()));
}

TEST(AgentEngineConversation, UnknownAgentsAndOversizedTextAreRejected) {
  Fixture fixture;
  auto unknown = converse_input("hello", true);
  unknown.self_id = "99";
  EXPECT_THROW(fixture.engine.converse(unknown), UnknownAgent);

  auto oversized = converse_input(std::string(kMaximumTextBytes + 1, 'a'));
  EXPECT_THROW(fixture.engine.converse(oversized), std::invalid_argument);
}

TEST(AgentEngineMemoryStore, KeepsTheNewestMemoriesPerAgent) {
  InMemoryAgentMemoryStore store;
  for (std::size_t index = 0; index < kMaximumMemoriesPerAgent + 3; ++index) {
    store.add({"2", "event " + std::to_string(index), "", 3, "reason"});
  }
  const auto recent = store.recent("2", kMaximumMemoriesPerAgent + 10);
  ASSERT_EQ(recent.size(), kMaximumMemoriesPerAgent);
  EXPECT_EQ(recent.front().description, "event " + std::to_string(kMaximumMemoriesPerAgent + 2));
  EXPECT_THROW(store.add({"2", "event", "", 11, "reason"}), std::invalid_argument);
  EXPECT_THROW(store.add({"", "event", "", 3, "reason"}), std::invalid_argument);
}

TEST(AgentEngineRest, ClientFlowUsesTheRecoveredJsonContract) {
  Fixture fixture;

  const auto health = fixture.rest.handle("GET", "", "", "");
  EXPECT_EQ(health.status, 200u);

  const auto listed = fixture.rest.handle("GET", "/agents/list", "", "");
  ASSERT_EQ(listed.status, 200u);
  auto list = nlohmann::json::parse(listed.body);
  EXPECT_EQ(keys_of(list), (std::set<std::string>{"status", "current_agents", "agents_in_db"}));
  EXPECT_TRUE(list.at("current_agents").empty());
  EXPECT_EQ(list.at("agents_in_db").size(), 2u);

  // The client posts the load request without a body.
  const auto loaded = fixture.rest.handle("POST", "/agent/2/load", "application/json", "");
  ASSERT_EQ(loaded.status, 200u);
  const auto agent = nlohmann::json::parse(loaded.body);
  EXPECT_EQ(keys_of(agent), (std::set<std::string>{"status", "name", "summary", "is_player"}));
  EXPECT_EQ(agent.at("name"), "Seren");

  list = nlohmann::json::parse(fixture.rest.handle("GET", "/agents/list", "", "").body);
  ASSERT_EQ(list.at("current_agents").size(), 1u);
  EXPECT_EQ(list.at("current_agents")[0].at("id"), "2");

  const auto search = post_json(fixture.rest, "/search/knowledge-hybrid",
                                {{"query", "markets produce"}, {"top_k", 5}});
  ASSERT_FALSE(search.at("query_result").empty());
  EXPECT_NE(search.at("query_result")[0].get<std::string>().find(", Distance:"), std::string::npos);

  // Field order and names as AAC_AgentEngine_SendConverseReq serializes them.
  const auto converse = post_json(fixture.rest, "/converse",
                                  {{"self_id", "2"},
                                   {"partner_id", "0"},
                                   {"start_conversation", true},
                                   {"statement", "Hello!"},
                                   {"dialogue_situation", ""},
                                   {"relationship", ""},
                                   {"max_conversation_turn", 10},
                                   {"end_conversation_forcefully", false},
                                   {"world_knowledge", nlohmann::json::array()}});
  EXPECT_EQ(keys_of(converse), (std::set<std::string>{"status", "response", "conversation_end",
                                                      "conversation_summary"}));
  EXPECT_FALSE(converse.at("response").get<std::string>().empty());

  const auto options =
      post_json(fixture.rest, "/generate-player-response", {{"agent_id", "0"}, {"player_id", "2"}});
  EXPECT_GE(options.at("response").size(), kMinimumPlayerOptions);

  const auto summary =
      post_json(fixture.rest, "/summarize-conversation",
                {{"agent_id", "2"},
                 {"conversation_history", {"Traveler: Hello!", "Seren: Hello, Traveler."}}});
  EXPECT_FALSE(summary.at("summary").get<std::string>().empty());

  const auto memory = post_json(fixture.rest, "/add-memory",
                                {{"agent_id", "2"},
                                 {"event_description", summary.at("summary")},
                                 {"event_datetime", "1000-03-01T06:00"}});
  EXPECT_EQ(keys_of(memory), (std::set<std::string>{"status", "message", "priority", "reason"}));
  EXPECT_GE(memory.at("priority").get<int>(), kMinimumMemoryPriority);
}

TEST(AgentEngineRest, RejectsMalformedRequests) {
  Fixture fixture;
  EXPECT_EQ(fixture.rest.handle("GET", "/converse", "", "").status, 405u);
  EXPECT_EQ(fixture.rest.handle("POST", "/converse", "text/plain", "{}").status, 415u);
  EXPECT_EQ(fixture.rest.handle("POST", "/converse", "application/json", "{not json").status, 400u);
  EXPECT_EQ(fixture.rest.handle("POST", "/agent/99/load", "application/json", "").status, 404u);
  EXPECT_EQ(fixture.rest.handle("POST", "/agent/2/delete", "application/json", "").status, 404u);
  EXPECT_EQ(fixture.rest.handle("POST", "/agent/..%2F/load", "application/json", "").status, 404u);
  EXPECT_EQ(fixture.rest.handle("GET", "/unknown", "", "").status, 404u);
  const std::string oversized(kMaximumRequestBodyBytes + 1, ' ');
  EXPECT_EQ(fixture.rest.handle("POST", "/converse", "application/json", oversized).status, 413u);
  post_json(fixture.rest, "/converse", {{"self_id", "2"}, {"partner_id", "0"}, {"statement", 7}},
            400);
}

TEST(AgentEngineRest, EmulatorDefinedPlanningRoutes) {
  Fixture fixture;

  const auto status =
      post_json(fixture.rest, "/time-and-status", {{"reference_time", "1000-03-01T09:30"}});
  EXPECT_EQ(status.at("agent_status"), "Seren is tending the garden at the garden.");

  const auto location = post_json(fixture.rest, "/select-location",
                                  {{"agent_id", "2"},
                                   {"current_task", "buy vegetables at the market"},
                                   {"locations", {"the kitchen", "the village market"}},
                                   {"current_location", "the kitchen"}});
  EXPECT_EQ(location.at("selected_location"), "the village market");

  const auto action = post_json(fixture.rest, "/select-action",
                                {{"agent_id", "2"},
                                 {"task", "water the garden"},
                                 {"interactables", {"stove", "watering can"}},
                                 {"game_commands", {"Sit", "Water"}}});
  EXPECT_EQ(action.at("action"), "Water");

  const auto breakdown = post_json(fixture.rest, "/breakdown-schedule",
                                   {{"agent_id", "2"},
                                    {"schedule", "23:00-01:00"},
                                    {"type", "night watch"},
                                    {"interactables", {"lantern"}},
                                    {"interval", 30}});
  const auto &steps = breakdown.at("schedule_breakdown");
  ASSERT_EQ(steps.size(), 4u);
  EXPECT_EQ(steps[0].at("start"), "23:00");
  EXPECT_EQ(steps[3].at("end"), "01:00");
  EXPECT_EQ(steps[1].at("selected_interactable")[0], "lantern");

  const auto plan = fixture.rest.handle("POST", "/agent/2/make-plan", "application/json", "");
  ASSERT_EQ(plan.status, 200u);
  EXPECT_EQ(nlohmann::json::parse(plan.body).at("plan").size(), 2u);

  post_json(fixture.rest, "/breakdown-schedule",
            {{"agent_id", "2"}, {"schedule", "morning"}, {"interval", 30}}, 400);
  post_json(fixture.rest, "/time-and-status", {{"reference_time", "noon"}}, 400);
}

TEST(AgentEngineModel, ChatCompletionUrlParsing) {
  const auto local = parse_chat_completion_url("http://127.0.0.1:11434");
  EXPECT_EQ(local.host, "127.0.0.1");
  EXPECT_EQ(local.port, "11434");
  EXPECT_EQ(local.target, "/v1/chat/completions");

  const auto prefixed = parse_chat_completion_url("http://llm.local:8000/api/v1/");
  EXPECT_EQ(prefixed.target, "/api/v1/chat/completions");

  EXPECT_THROW(parse_chat_completion_url("https://api.example.com"), std::invalid_argument);
  EXPECT_THROW(parse_chat_completion_url("http://:80"), std::invalid_argument);
  EXPECT_THROW(parse_chat_completion_url("http://host:99999"), std::invalid_argument);
}

TEST(AgentEngineModel, UnreachableChatCompletionFallsBackToScriptedText) {
  ChatCompletionConfig config;
  // Port 9 (discard) is closed on a development machine, so the connection fails quickly.
  config.endpoint = parse_chat_completion_url("http://127.0.0.1:9");
  config.model = "test-model";
  config.timeout = std::chrono::milliseconds(1500);
  ChatCompletionModel model(std::move(config), std::make_unique<ScriptedDialogueModel>());
  ScriptedDialogueModel scripted;

  const auto speaker = npc_agent();
  const auto partner = player_agent();
  const ReplyRequest request{speaker, partner, {}, "Who are you?", "", "", {}, {}, false, false};
  EXPECT_EQ(model.reply(request), scripted.reply(request));
  EXPECT_EQ(model.rate_memory(speaker, "A promise").priority,
            scripted.rate_memory(speaker, "A promise").priority);
}

TEST(AgentEngineSchedule, MinuteParsingAcceptsClientDateTimes) {
  EXPECT_EQ(parse_minute_of_day("1000-03-01T06:00"), 360);
  EXPECT_EQ(parse_minute_of_day("23:59"), 1439);
  EXPECT_EQ(parse_minute_of_day("24:00"), 1440);
  EXPECT_FALSE(parse_minute_of_day("24:30"));
  EXPECT_FALSE(parse_minute_of_day("6:00"));
  EXPECT_EQ(format_minute_of_day(1440), "00:00");
  EXPECT_EQ(format_minute_of_day(1500), "01:00");
}

} // namespace
} // namespace chronicles::agent_engine
