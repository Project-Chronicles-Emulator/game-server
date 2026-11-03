# Agent engine (AI chat REST service)

The client contains an AI chat feature: a house mate can hold a free-form conversation with the
player, suggest replies, summarize the conversation and remember it. The client talks to a
separate "agent engine" REST service for every step. The emulator implements that service in
`src/services/agent_engine/agent_engine*.cpp` and the launcher points the client at it.

Static evidence for the matching client build is in
`dumps/ida_research/ai-rest-20261026/FINDINGS.md` (37 annotated functions). No original-client
session has exercised these routes yet; see "Verification" below.

## Client contract

- **Base URL.** The front REST object stores the hard-coded base URL
  `http://modena.xlgames.corp:38011` (an internal XLGAMES host) in an FString. Every request
  appends its route to that FString. Without a redirect the feature cannot reach any server.
- **Transport.** HTTP with `Content-Type: application/json` and no `Authorization` header.
  Bodies are protobuf JSON with proto field names (`x5.pb.rest.*Req` / `*Res` in
  `p_c2ae.rest.proto` / `p_ae2c.rest.proto`). The client reads responses only for HTTP 200–206
  and parses them strictly: an unknown field makes it ignore the response.
- **Routes.**

  | Route | Body → response |
  | --- | --- |
  | `GET {base}` | reachability check, any 2xx |
  | `GET {base}/agents/list` | `AgentListRes` (`current_agents`, `agents_in_db`) |
  | `POST {base}/agent/{id}/load` | no body → `AddAgentRes` |
  | `POST {base}/converse` | `ConverseReq` → `ConverseRes` |
  | `POST {base}/summarize-conversation` | `SummarizeConversationReq` → `SummarizeConversationRes` |
  | `POST {base}/add-memory` | `AddMemoryReq` → `AddMemoryRes` |
  | `POST {base}/generate-player-response` | `GeneratePlayerResponseReq` → `GeneratePlayerResponseRes` |
  | `POST {base}/search/knowledge-hybrid` | `SearchKnowledgeReq` → `SearchKnowledgeRes` |

- **Agents.** `AIChatNpcAgent` maps NPC 35 (house mate Seren) to agent `"2"`;
  `AIChatPlayerAgent` maps only Fight/Nuian/Male characters to agent `"0"`. The client ignores a
  conversation reply whose speaking agent was not listed or loaded, and it cannot start AI chat
  for other class/race/gender combinations because this build has no row for them.
- **Knowledge results** must look like `text, Distance:0.1234`. The client keeps results with
  distance ≤ 0.7 (`AIChatSearchKnowledgeDistanceThreshold`) and passes their text to
  `/converse` and `/generate-player-response` as `world_knowledge`.

## Emulator behaviour

- **Agents** come from the client catalog (`AIChatNpcAgent` with the NPC name without its
  `[Housemate]` tag, and `AIChatPlayerAgent`) plus `config/agent-engine/agents.json`, which
  overrides or adds personas, residences and daily schedules by id.
- **Knowledge** comes from `config/agent-engine/knowledge.json` plus one entry per persona.
  Search scores documents by IDF-weighted keyword coverage plus character-trigram overlap and
  reports `distance = 1 - score`. The official ranking model is unknown.
- **Conversations** are keyed by (speaking agent, partner agent). `start_conversation` resets
  the conversation. A conversation ends after `max_conversation_turn` replies (10 when 0, at most
  50), on `end_conversation_forcefully`, or when the player says goodbye; the final reply carries
  `conversation_summary`.
- **Memories** written by `/add-memory` are rated 1–10 with a reason and persisted per agent
  (PostgreSQL schema version 26, newest 512 per agent; memory storage keeps them only while the
  server runs). Replies recall the agent's five newest memories.
- **Text generation.** The default `ScriptedDialogueModel` is deterministic: it greets, answers
  identity/home/thanks questions from the persona, quotes matching memories or supplied
  knowledge, offers two to four reply options ending with a goodbye, and builds extractive
  summaries. `--agent-engine-model-url http://HOST:PORT[/v1] --agent-engine-model NAME`
  switches to an OpenAI-compatible chat-completions endpoint (for example a local Ollama or
  llama.cpp server); `CHRONICLES_AGENT_MODEL_API_KEY` supplies an optional bearer key. Any
  model failure, timeout (20 s) or unusable answer falls back to the scripted model. Only plain
  HTTP endpoints are supported; use a local proxy for HTTPS providers. Conversation text is never
  logged.
- **Emulator-defined routes.** The recovered schemas also contain DTOs that no client code uses
  in this build. They are served on routes the emulator chose:
  `POST /time-and-status` (`reference_time` `HH:MM` or `YYYY-MM-DDTHH:MM` → one status sentence
  per scheduled agent), `POST /select-location`, `POST /select-action`,
  `POST /breakdown-schedule` (`schedule` `HH:MM-HH:MM`, `interval` minutes, 5–1440, default 60)
  and `POST /agent/{id}/make-plan` (the agent's configured schedule; no request schema exists).
  Their behaviour is a keyword-matching planner, not a recovered official algorithm.

## Running it

```powershell
chronicles-server --storage memory --port 8080 --client-data-dir data/generated/catalog `
  --agent-engine-port 38011
```

- `--agent-engine-port PORT` enables the listener on the server's `--bind-address`.
- `--agent-engine-data DIR` replaces `config/agent-engine` (personas and knowledge).
- Only loopback peers are served unless `--agent-engine-allow-remote true` is given. The client
  sends no credential, so remote access lets anyone on the network read personas and write agent
  memories; enable it only on a trusted network.

The launcher starts its local server with `--agent-engine-port` from the `agent_engine_port`
setting (default 38011; 0 disables) and, while `agent_engine_redirect` is on, rewrites the
client's base-URL FString to `http://<server host>:<port>`. It scans writable memory for heap
FStrings holding exactly the official URL, patches them in place (the replacement must not be
longer than the official 32 characters), keeps an original-byte backup in
`out/launcher/agent-engine-backup-<pid>.json` and restores them with the redirect Restore action
or on exit when `restore_on_exit` is set. The literal in the executable image is never changed.
Distribution profiles turn the redirect off because a hosted agent engine is loopback-only by
default.

## Verification

- `chronicles-agent-engine-tests` (GoogleTest): text bounds, knowledge ranking and the client's
  distance format, catalog/persona loading, conversation turns and endings, memory recall and
  retention, the JSON contract of every route (exact response field sets), rejection paths,
  emulator-defined planning routes, model URL parsing and chat-completion fallback.
- `integration.agent_engine` (`tools/agent_engine_smoke_test.py`): starts `chronicles-server`
  and replays the client's route order over HTTP, including the distance filter and memory
  recall.
- `chronicles-launcher-tests` `AgentEngineRedirect.*`: base-URL discovery, in-place patch,
  repeated pass, restore and refusal of oversized addresses against an in-memory process image.

These are synthetic checks. The original client has not yet been observed reaching the emulator's
agent engine, so in-game rendering of replies, suggested options and memory lines is unverified.
