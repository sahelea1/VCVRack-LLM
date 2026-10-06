# Rack Assistant — Architecture & Implementation Spec

Built-in LLM chat assistant for this VCV Rack 2 fork. The user chats with it in a docked
side panel; it can read and modify the current patch through tools (add/remove/move
modules, set params, connect/disconnect cables, save) and explain/build patches.
Backend: any OpenAI-compatible `POST {base_url}/chat/completions` endpoint.

**Status: this document reflects the implementation** (steps 1-3 plus review fixes). Where code and
text disagree, the code in `include/assistant/` wins. User-facing usage, config table and
troubleshooting live in [README.md](README.md); they are not repeated here.

---------------------------------------------------------------------------------------

## 0. Ground rules

- C++11 only (`-std=c++11`): no `std::make_unique`, `std::optional`, `string_view`,
  generic lambdas. Mark overrides with `override` (`-Wsuggest-override`).
- Namespace: `rack::assistant`. Code and comments in English, Rack code style
  (tabs, `camelCase` methods, `PascalCase` types, braces on same line).
- New public headers: `include/assistant/*.hpp`. New sources: `src/assistant/*.cpp`
  (picked up automatically by the `src/*/*.cpp` glob; do NOT nest deeper).
- Do NOT change the layout of existing public structs (plugin ABI). Extra state for
  `Scene` goes into `Scene::Internal` (defined in `src/app/Scene.cpp`).
- Secrets: never write an API key into source, defaults, tests, logs, commits. Never log
  request bodies, headers or keys. Only log URL, model, byte counts, status, duration.
- Threading (most important rule): **only the UI/main thread touches `APP->scene`,
  `APP->engine` mutations, `APP->history`, `APP->patch`, widgets, `settings::`, NanoVG.**
  Worker threads do HTTP + JSON parsing on plain value types only and never use `APP`.
- All engine/widget pointers are re-resolved by id on every tool call. No `Module*`,
  `ModuleWidget*`, `CableWidget*` is stored across frames.
- Every failure path returns a clear error (tool result / chat error entry). Never crash,
  never `assert` on model input. Wrap tool execution in `try/catch (std::exception&)`.

## 1. Files

```
include/assistant/
  Types.hpp           value types: ToolCall, ChatMessage, Usage, LlmError, ChatRequest, ChatResponse
  Config.hpp          Config, load/save, API key resolution, masking
  Protocol.hpp        request body builder, response parser, HTTP error mapping
  LlmClient.hpp       LlmClient interface, ClientOptions, factories (HTTP + mock)
  Tools.hpp           ToolResult, ToolContext, Tool, ToolRegistry, arg helpers
  Controller.hpp      Controller (agent loop state machine, conversation, transcript)
  SystemPrompt.hpp    default system prompt + user-editable prompt file
  Panel.hpp           docked chat panel public API (create/toggle/layout)
  SettingsDialog.hpp  settings dialog
  SelfTest.hpp        in-app self test (env RACK_ASSISTANT_SELFTEST)
  PatchEvents.hpp     notifyPatchCleared() / getPatchGeneration() (see section 8)
src/assistant/
  Config.cpp  Protocol.cpp  HttpLlmClient.cpp  MockLlmClient.cpp
  Tools.cpp (registry + arg helpers + registration)  PatchTools.cpp  CatalogTools.cpp
  Controller.cpp  SystemPrompt.cpp  Panel.cpp  SettingsDialog.cpp  SelfTest.cpp
  ToolHelpers.hpp/.cpp  (private: JSON/ModuleRef/port+param resolution/grid/undo helpers for the tools)
  UiCommon.hpp          (private: fonts, colors, text measuring shared by Panel + SettingsDialog)
tests/assistant/
  test.hpp (tiny assertion framework)  test_main.cpp  test_protocol.cpp
  test_config.cpp  test_mock.cpp  test_args.cpp  test_http.cpp  test_live.cpp  run_selftest.sh
docs/assistant/ARCHITECTURE.md (this file), docs/assistant/README.md (user guide)
```

Touched existing files (minimal diffs):
- `src/app/Scene.cpp` — create the panel (hidden) in the constructor, dock it in
  `step()` (shrinks `rackScroll`), Ctrl+L toggles it in `onHoverKey`, call
  `assistant::sceneStepHook()` (SelfTest.hpp) once per frame (self-test trigger).
- `src/app/MenuBar.cpp` — new top-level "Assistant" menu.
- `src/patch.cpp` — `patch::Manager::clear()` calls `assistant::notifyPatchCleared()` (section 8).
- `Makefile` — `test-assistant` target (Linux; builds `build/assistant_test` against libRack.so);
  `RACK_VERSION` falls back to the newest `### 2.x.y` in CHANGELOG.md when there are no git tags.
- `.gitignore` — `/assistant.json`, `/assistant-system-prompt.md`, `/assistant-mock.json`,
  `/assistant-selftest*` (dev mode puts the user dir in the repo root!).

## 2. Threading model

```
UI thread (Window::run -> Scene::step -> ... -> Panel::step -> Controller::step)
  ├─ owns Controller state machine, conversation, transcript, ComplexAction of the run
  ├─ executes ALL tools (engine/rack/history/patch access)
  └─ starts one worker std::thread per HTTP request
Worker thread ("Assistant")
  └─ LlmClient::complete(request copy, cancel flag) -> ChatResponse (plain values)
     writes result into shared PendingRequest, sets done=true (release)
Controller::step() polls done (acquire), consumes the response, continues the loop.
```

- No blocking waits on the UI thread except joining a worker that already reported
  `done` (immediate) and the destructor (cancels first; curl aborts within ~1 s via
  XFERINFO callback).
- Cancel: sets the request's `std::atomic<bool> cancel`, the controller immediately
  forgets the request (moves it to `retired`), joins it later once `done`.
- The agent loop itself is a UI-thread state machine (no promise/future ping-pong, no
  deadlock risk); confirmations simply pause the state machine.
- `Panel::step()` runs every frame even when the panel is hidden (Rack steps hidden
  children), so runs continue in the background.

## 3. Types (`include/assistant/Types.hpp`)

```cpp
namespace rack { namespace assistant {

struct ToolCall {
	std::string id;
	std::string name;
	/** Raw JSON text as sent by the model. May be invalid JSON. */
	std::string arguments;
};

struct ChatMessage {
	enum Role { SYSTEM, USER, ASSISTANT, TOOL };
	Role role = USER;
	std::string content;
	/** Assistant message whose content was null/absent (tool-calls-only). Serialized as null. */
	bool contentNull = false;
	std::vector<ToolCall> toolCalls;      // ASSISTANT only
	std::string toolCallId;               // TOOL only
	/** Display-only reasoning text (never sent back). */
	std::string reasoning;
	/** Raw JSON array text of provider "reasoning_details"; echoed back verbatim on later
	requests when reasoning_param_style == "openrouter". Empty if none. */
	std::string reasoningDetailsJson;
};

struct Usage {
	int promptTokens = -1, completionTokens = -1, totalTokens = -1, reasoningTokens = -1;
	double cost = -1.0;   // OpenRouter "usage.cost" if present
};

struct LlmError {
	enum Kind { NONE, CANCELLED, CONFIG, NETWORK, TIMEOUT, AUTH, PAYMENT, FORBIDDEN,
		NOT_FOUND, RATE_LIMIT, SERVER, BAD_REQUEST, BAD_RESPONSE, OTHER };
	Kind kind = NONE;
	int httpStatus = 0;
	/** User-facing message incl. hint. Never contains the API key. */
	std::string message;
	/** Provider error text (truncated, may be empty). */
	std::string detail;
};

struct ChatResponse {
	bool ok = false;
	ChatMessage message;        // role ASSISTANT when ok
	std::string finishReason;
	std::string model;          // model reported by provider
	Usage usage;
	LlmError error;             // kind != NONE when !ok
};

struct ChatRequest {
	std::vector<ChatMessage> messages;   // system message first
	/** JSON array text of tool definitions (OpenAI format) or empty for no tools. */
	std::string toolsJson;
};

}}
```

## 4. Config (`include/assistant/Config.hpp`, file `asset::user("assistant.json")`)

```cpp
struct Config {
	std::string baseUrl = "https://openrouter.ai/api/v1";
	std::string model = "openai/gpt-5.6-terra";
	std::string reasoningEffort = "medium";        // off|none|minimal|low|medium|high
	std::string reasoningParamStyle = "openrouter"; // openrouter|openai
	int maxTokens = 0;                // <= 0: unset, not sent
	bool hasTemperature = false;      // false: not sent
	float temperature = 1.f;
	std::vector<std::pair<std::string, std::string>> extraHeaders;  // default below
	bool confirmDestructive = true;
	int maxToolRounds = 15;           // clamp 1..100
	double timeoutSec = 180.0;        // total request timeout, clamp 10..1800
	std::string caBundle;             // optional CA file path for TLS
	bool mock = false;                // use MockLlmClient (no network, no cost)
	std::string apiKey;               // only from file; never logged
	float panelWidth = 400.f;         // UI, clamp 280..1200
	bool attachSelection = true;      // UI toggle default
	int maxContextChars = 200000;     // conversation trimming threshold
};

Config defaultConfig();
std::string configPath();                       // asset::user("assistant.json")
/** Type-checked parse; invalid values keep defaults and add a warning string. */
Config configFromJson(json_t* rootJ, std::vector<std::string>* warnings = NULL);
/** New reference. Writes "api_key" only if non-empty. */
json_t* configToJson(const Config& c);
/** Missing file: returns defaults and writes a defaults file (without key). Bad JSON:
returns defaults + warning (file untouched). Never throws. */
Config loadConfig(const std::string& path, std::vector<std::string>* warnings = NULL);
/** Atomic write (path.tmp + rename), chmod 0600 on POSIX. */
bool saveConfig(const Config& c, const std::string& path, std::string* error = NULL);

struct ResolvedKey {
	std::string key;
	/** "RACK_ASSISTANT_API_KEY" | "OPENROUTER_API_KEY" | "assistant.json" | "" (none) */
	std::string source;
};
/** Order: env RACK_ASSISTANT_API_KEY, env OPENROUTER_API_KEY, config.apiKey. Empty env vars are skipped. */
ResolvedKey resolveApiKey(const Config& c);
/** "sk-or-v1…abcd" style: first 8 chars + "…" + last 4; keys < 16 chars -> "••••". */
std::string maskKey(const std::string& key);
bool isValidReasoningEffort(const std::string& s);
bool isValidReasoningParamStyle(const std::string& s);
```

JSON (snake_case, all optional):
```json
{
  "base_url": "https://openrouter.ai/api/v1",
  "model": "openai/gpt-5.6-terra",
  "reasoning_effort": "medium",
  "reasoning_param_style": "openrouter",
  "max_tokens": null,
  "temperature": null,
  "extra_headers": {"HTTP-Referer": "https://github.com/sahelea1/vcvrack-llm", "X-Title": "VCV Rack Assistant"},
  "confirm_destructive": true,
  "max_tool_rounds": 15,
  "timeout_sec": 180,
  "ca_bundle": "",
  "mock": false,
  "api_key": "",
  "panel_width": 400,
  "attach_selection": true,
  "max_context_chars": 200000
}
```
- `max_tokens`: null / missing / 0 / "" → unset. `temperature`: null / missing / "" → unset.
- `extra_headers`: object of string→string. Header names/values containing CR/LF are dropped
  with a warning.
- The defaults file written on first run omits `api_key`.

## 5. Protocol (`include/assistant/Protocol.hpp`)

```cpp
/** "{base}/chat/completions"; trims trailing '/', doesn't double-append if base already ends with it. */
std::string chatCompletionsUrl(const std::string& baseUrl);
/** Compact JSON body for the request. */
std::string buildRequestBody(const Config& c, const ChatRequest& req);
/** New reference, OpenAI chat format. includeReasoningDetails: echo reasoning_details. */
json_t* messageToJson(const ChatMessage& m, bool includeReasoningDetails);
/** Inverse of messageToJson (for future persistence). */
bool messageFromJson(json_t* j, ChatMessage* out);
/** Parse a chat.completions response (status 2xx or not). */
ChatResponse parseResponse(long httpStatus, const std::string& body);
/** Friendly error for a non-2xx status; extracts provider message from body. */
LlmError httpError(long httpStatus, const std::string& body);
/** UTF-8-safe truncation adding "…" when cut. */
std::string truncateUtf8(const std::string& s, size_t maxBytes);
```

Request body:
- `model`, `messages`; `tools` + `"tool_choice": "auto"` only if `toolsJson` non-empty.
- Reasoning: `off` → no reasoning parameter at all (for non-reasoning models).
  `none` → explicit effort "none" in the configured style (required by OpenAI's own API for gpt-5.6-* when tools are
  used on /v1/chat/completions: "Function tools with reasoning_effort are not supported …
  set reasoning_effort to 'none'"; OpenRouter accepts reasoning + tools). Otherwise style `openrouter` → `"reasoning": {"effort": E}`; style `openai` →
  `"reasoning_effort": E`.
- `max_tokens` when set: style `openrouter` → `"max_tokens"`; style `openai` →
  `"max_completion_tokens"` (OpenAI reasoning models reject `max_tokens`).
- `temperature` only when `hasTemperature`.
- Messages: system/user `{role, content}`; assistant `{role:"assistant", content: string|null,
  tool_calls:[{id, type:"function", function:{name, arguments}}]}` (omit `tool_calls` if
  none; plus `reasoning_details` (parsed JSON) when non-empty and style is openrouter);
  tool `{role:"tool", tool_call_id, content}`.

Response parsing (robust):
- Non-2xx → `httpError`. Body not JSON → `BAD_RESPONSE` ("The server returned invalid JSON").
- Top-level `"error"` object even with 200 → error (use `error.code` if it is an HTTP-like
  int, else map to OTHER) with `error.message` as detail.
- `choices` missing/empty → `BAD_RESPONSE`. `choices[0].error` → error.
- `message.content`: string → content; null/missing → `contentNull=true`; array → concat
  `text` of parts whose `type` is `text`/`output_text`.
- `message.tool_calls`: each `{id, function:{name, arguments}}`; missing id → left empty
  (the controller assigns `"call_<round>_<index>"`, unique across rounds); `arguments` string kept verbatim, object/array → dumped compact,
  missing → `"{}"`. Entries without a function name are kept with empty name (tool
  execution then reports the error). Legacy `message.function_call` → one tool call.
- Reasoning: `message.reasoning` or `message.reasoning_content` (string) → `reasoning`;
  `message.reasoning_details` (array) → dumped into `reasoningDetailsJson`, and if
  `reasoning` is empty derive it from entries' `text` / `summary` strings.
- `finish_reason`, top-level `model`, `usage.{prompt_tokens, completion_tokens,
  total_tokens, completion_tokens_details.reasoning_tokens, cost}`.

HTTP error mapping (`message` texts, English, user-facing):
| status | kind | message |
|---|---|---|
| 400 | BAD_REQUEST | "The provider rejected the request (400): <detail>. Check model parameters (temperature, max_tokens, reasoning) in the assistant settings." (if the detail says the model is invalid/not found: "... Check 'model' in the assistant settings.") |
| 401 | AUTH | "Authentication failed (401). Check your API key (RACK_ASSISTANT_API_KEY / OPENROUTER_API_KEY or the assistant settings)." |
| 402 | PAYMENT | "Insufficient credits (402). Top up your account or pick a cheaper model." |
| 403 | FORBIDDEN | "Access denied (403): <detail>" |
| 404 | NOT_FOUND | "Model or endpoint not found (404)[: <detail>]. Check 'model' and 'base_url' in the assistant settings." |
| 408 | TIMEOUT | "The provider timed out (408). Try again." |
| 413 | BAD_REQUEST | "Request too large (413). Start a new chat to shorten the conversation." |
| 429 | RATE_LIMIT | "Rate limited (429). Wait a moment and try again." |
| 5xx | SERVER | "The provider had a server error (<status>). Try again later." |
| other | OTHER | "Unexpected HTTP status <status>: <detail>" |
`detail` = provider `error.message` (or `error` string, or raw body) truncated to 300 bytes.

## 6. LLM clients (`include/assistant/LlmClient.hpp`)

```cpp
struct LlmClient {
	virtual ~LlmClient() {}
	/** Blocking; runs on a worker thread. Must not touch APP/widgets/settings.
	Returns within ~1 s after `cancel` becomes true with error.kind == CANCELLED. */
	virtual ChatResponse complete(const ChatRequest& req, const std::atomic<bool>& cancel) = 0;
	// Phase 2 (streaming): add a virtual completeStream(req, cancel, onDelta) whose default
	// implementation calls complete(). Do not implement now.
};

/** Everything a client needs, resolved on the UI thread (settings/asset access). */
struct ClientOptions {
	Config config;
	std::string apiKey;          // resolved key (may be empty)
	bool verifyTls = true;       // snapshot of settings::verifyHttpsCerts
	std::string caBundlePath;    // config.caBundle, else env RACK_ASSISTANT_CA_BUNDLE, else asset::system("cacert.pem")
	std::string userAgent;       // "VCV Rack <version> Assistant"
	std::string mockScriptPath;  // asset::user("assistant-mock.json")
};
/** UI thread only. */
ClientOptions makeClientOptions(const Config& c);
/** Mock if options.config.mock or base_url starts with "mock:"; else HTTP. */
std::shared_ptr<LlmClient> createClient(const ClientOptions& o);
std::shared_ptr<LlmClient> createHttpClient(const ClientOptions& o);
std::shared_ptr<LlmClient> createMockClient(const ClientOptions& o);
```

HttpLlmClient (libcurl directly; `#define CURL_STATICLIB` before `<curl/curl.h>`; global
init already done by `network::init()`):
- One easy handle per request. Options: URL, POST body (`POSTFIELDS` + `POSTFIELDSIZE_LARGE`),
  headers `Content-Type: application/json`, `Accept: application/json`,
  `Authorization: Bearer <key>` (only if key non-empty), extra headers; `USERAGENT`,
  `NOSIGNAL 1`, `CONNECTTIMEOUT 30`, `TIMEOUT timeoutSec`, `CAINFO caBundlePath`,
  `SSL_VERIFYPEER verifyTls`, `FOLLOWLOCATION 0`, `NOPROGRESS 0` + `XFERINFOFUNCTION`
  returning 1 when cancel is set. No `ACCEPT_ENCODING` (curl built without zlib).
  Proxy env vars (`HTTPS_PROXY`) are honored by libcurl defaults.
- Empty key and host is not localhost/127.0.0.1/[::1] → return CONFIG error
  "No API key configured. Set RACK_ASSISTANT_API_KEY or OPENROUTER_API_KEY, or enter a key in the assistant settings."
  (local servers like Ollama/LM Studio work without a key).
- A non-empty key with a plain `http://` URL whose host is not local → CONFIG error "Refusing to send
  the API key over plain http:// to a non-local host. Use an https:// base URL." (nothing is sent).
- Transport errors: `CURLE_ABORTED_BY_CALLBACK` → CANCELLED "Cancelled.";
  `CURLE_OPERATION_TIMEDOUT` → TIMEOUT "The request timed out after N s.";
  resolve/connect errors → NETWORK "Could not connect to <host>: <curl error>.";
  SSL/cert errors → NETWORK "TLS error: <curl error>. If you are behind a proxy, set 'ca_bundle'.";
  others → NETWORK with curl error text.
- Logs: `INFO("Assistant: POST %s (model %s, %zu bytes)")` and
  `INFO("Assistant: HTTP %ld, %zu bytes, %.1f s")`. Nothing else.

MockLlmClient (deterministic, no network; ~250 ms simulated latency, cancel-aware in
≤20 ms steps):
- If `mockScriptPath` exists and holds `{"responses":[<chat completion JSON>, ...]}`,
  respond with them in order (index = number of ASSISTANT messages in the request,
  clamped to last) via `parseResponse(200, dump)`.
- Otherwise built-in scenarios keyed on the last USER message (lowercase) and `round` =
  number of ASSISTANT messages after that USER message:
  - contains `/mock-error <code>` → `httpError(code, '{"error":{"message":"mock error"}}')`.
  - contains `/mock-badargs` → r0: tool call `add_module` with arguments `{not json`;
    r1: text "The tool reported invalid arguments, as expected.".
  - contains `/mock-delete` → r0: `get_patch`; r1: `remove_module` with the first module id
    found in the get_patch tool result; r2: text summary.
  - contains `/mock-loop` → always a `get_patch` tool call (tests max_tool_rounds).
  - contains `/mock-build`, `build`, `bau`, `demo` or `acid` → demo build:
    r0: `get_patch {}` + `search_modules {"query":"VCO"}` (two calls, content null);
    r1: `add_module Fundamental/VCO` + `add_module Fundamental/VCF`;
    r2: `get_module_info` for both new module ids (parsed from the tool results' `module_id`);
    r3: `connect` VCO output whose name contains "saw" (else 0) → VCF input whose name
        contains "audio" or "in" (else 0), and `set_param` on the VCF param whose name contains
        "cutoff" or "freq" with `value` 0.4;
    r4: final text "Mock: built VCO → VCF. Try turning Cutoff and Resonance."
    If a needed id cannot be found, reply with a text explaining what failed.
  - anything else → text "Mock reply: <first 200 chars of user text>".
- Tool call ids: `"mock_<round>_<index>"`.

## 7. Tools (`include/assistant/Tools.hpp`)

```cpp
struct ToolResult {
	bool ok = true;
	/** JSON object text sent to the model as the tool message content. Always has "ok".
	Errors: {"ok":false,"error":"..."} */
	std::string content;
	/** Compact human-readable line for the chat UI ("VCF added", "Cutoff → 1.2 kHz",
	"VCO Saw → VCF Audio"). Empty: not shown. */
	std::string summary;
	/** True if the patch was changed. */
	bool mutated = false;
	/** True for read-only tools (UI shows them dimmed). */
	bool readOnly = false;
};

struct ToolContext {
	/** Undo actions of the current agent run. Never NULL while a run executes tools. */
	history::ComplexAction* undo = NULL;
	/** Module ids added during this run, in order (placement heuristic). */
	std::vector<int64_t> addedModuleIds;
	/** Set by save_patch; reset to 0 mutations by save, incremented per mutation. */
	bool savedDuringRun = false;
	int mutationsSinceSave = 0;
};

struct Tool {
	std::string name;
	std::string description;
	/** JSON Schema text ({"type":"object",...}) for the arguments. */
	std::string parametersSchema;
	/** NULL = never destructive. Otherwise returns a confirmation question for this call,
	or "" if this particular call is not destructive. UI thread. */
	std::function<std::string(json_t* args)> confirmation;
	/** UI thread only. args is a JSON object (never NULL). */
	std::function<ToolResult(json_t* args, ToolContext& ctx)> run;
};

struct ToolRegistry {
	void add(const Tool& t);
	const Tool* find(const std::string& name) const;
	const std::vector<Tool>& all() const;
	/** OpenAI "tools" array JSON text: [{"type":"function","function":{name,description,parameters}}] */
	std::string toolsJson() const;
	/** Parses args (empty -> {}), rejects unknown tool / invalid JSON / non-object with a
	clear error result, catches all exceptions, updates ctx.mutationsSinceSave. UI thread. */
	ToolResult execute(const std::string& name, const std::string& argsJson, ToolContext& ctx) const;
	/** Confirmation question if this call is destructive, else "" (also "" for invalid args). */
	std::string confirmationFor(const std::string& name, const std::string& argsJson) const;
private:
	std::vector<Tool> tools;
};

/** Registry with all built-in tools (built lazily, UI thread). */
ToolRegistry& defaultRegistry();
void registerPatchTools(ToolRegistry& r);    // PatchTools.cpp
void registerCatalogTools(ToolRegistry& r);  // CatalogTools.cpp

// Argument helpers (unit-tested; no APP access)
/** Accepts JSON integer, integral real, or decimal string. */
bool argInt64(json_t* args, const char* key, int64_t* out);
bool argFloat(json_t* args, const char* key, float* out);
bool argString(json_t* args, const char* key, std::string* out);
bool argBool(json_t* args, const char* key, bool* out);
std::string okJson(json_t* obj);              // steals obj, sets "ok":true, returns compact text
std::string errorJson(const std::string& msg); // {"ok":false,"error":msg}
ToolResult errorResult(const std::string& msg);
```

Common conventions:
- Module ids are JSON integers (int64, < 2^53). Accept numeric strings too.
- Port/param ids: integer index, or a string matching the port/param name
  case-insensitively (exact match; if ambiguous/unknown → error listing available
  `id: name` pairs, truncated to ~40 entries).
- Positions are grid units `{"hp": int, "row": int}` (`ModuleWidget::getGridPosition()`;
  1 HP = 15 px, row = 380 px).
- Errors are actionable: e.g. "Module id 123 not found. Call get_patch to see current module ids."
- Every mutation pushes the matching `history::*` action into `ctx.undo`.

### Read tools
**get_patch** `{include_all_params?: bool=false, module_ids?: [int]}`
→ `{ok, path: string|null, unsaved, module_count, cable_count, selected_module_ids:[],
modules:[{id, plugin, model, name, pos:{hp,row}, width_hp, bypassed, params:[{id, name,
value, display}]}], cables:[{id, color, from:{module, output, name}, to:{module, input, name}}]}`.
Params: only those differing from default (|v-def| > 1e-6) unless `include_all_params`.
`display` = `getDisplayValueString()` + unit. Modules sorted by row, then hp.
Summary: "Read patch (N modules)".

**search_modules** `{query: string, tag?: string, limit?: int=12 (1..40)}`
→ `{ok, total_matches, results:[{plugin, model, name, brand, tags:[...], description}]}`.
Own `fuzzysearch::Database<plugin::Model*>` built lazily with the Browser's weights and
fields (brand, plugin name, model name, description, tag aliases); visibility filter
as `Browser::isModelVisible` (hidden, disabled, whitelist). Also append case-insensitive
substring matches on model slug/name not already present. Unknown tag → error listing
valid tag names. Empty query + tag → all visible models with that tag. Never dumps the
whole catalog (limit enforced). Summary: `Searched "VCO" (8)`.

**get_module_info** `{module_id: int}` or `{plugin: string, model: string}`
→ `{ok, plugin, model, name, description, tags, width_hp, module_id?, pos?,
params:[{id, name, min, max, default, unit, snap, options?:[labels], min_display,
max_display, default_display, value?, display?}], inputs:[{id, name, description?,
connected?}], outputs:[...same]}`.
Model-level info is computed once per `Model*` from a temporary `model->createModule()`
(never added to the engine; set `params[i].value` directly to compute display strings;
`delete` afterwards) plus `model->createModuleWidget(NULL)` for the width (deleted
afterwards); cached as JSON text. Instance info = model info + current values/display +
connection flags + id/pos. `SwitchQuantity` → `options`. Summary: "Inspected VCF".

### Mutating tools
**add_module** `{plugin, model, position?: {hp, row}, near_module_id?: int}`
→ `{ok, module_id, plugin, model, name, pos:{hp,row}, width_hp, note?}`.
`plugin::getModel(p, m)` exact (no fallback); unknown → error suggesting `search_modules`;
hidden model → error. Create: `createModule()` → `engine->addModule` →
`createModuleWidget(module)` → position → `rack->addModule(mw)` (catch Exception → clean
up) → `mw->loadTemplate()` (try/catch) → `history::ModuleAdd` (setModule after add).
Placement (no overlap, same-row intervals in HP): explicit `position` → that slot if free,
else nearest free slot in that row (prefer right) with a note; `near_module_id` → first
free slot right of it; else right of the last module added in this run; else right of the
rightmost selected module; else right of the rightmost module in row 0; empty patch →
(0,0). Summary: "VCF added".

**remove_module** `{module_id}` — destructive. Confirmation: "Remove module 'VCO'
(Fundamental, HP 10, row 0) and its 2 cables?" (plugin slug and grid position; module ids are
long random numbers and mean nothing to the user). Run: `mw->appendDisconnectActions(ctx.undo)`,
`history::ModuleRemove` (setModule before removal), `rack->removeModule(mw)`, `delete mw`,
`rack->updateExpanders()`. → `{ok, removed_module_id, removed_cables}`. Summary: "VCO removed".

**move_module** `{module_id, position:{hp,row}}` → collision-free (nearest free slot in
row, ignoring itself), `history::ModuleMove` (pixel old/new pos), `updateExpanders()`.
→ `{ok, module_id, pos, note?}`. Summary: "VCO moved to HP 24, row 1".

**set_param** `{module_id, param_id: int|string, value?: number, display_value?: number}`
(one of value/display_value; if both are sent, a 0 in one field is treated as a placeholder for the other, otherwise display_value wins, and the result carries a note; with both 0 the raw value is used and a note warns when raw 0 does not display as 0). Validates range; `ParamQuantity` required. old =
`params[id].getValue()`; `pq->setImmediateValue(v)` or `pq->setDisplayValue(dv)` (clamps +
snaps); new = `pq->getImmediateValue()`; push `history::ParamChange` if changed.
→ `{ok, module_id, param_id, name, old_value, value, display, min, max, clamped}`
(clamped = requested raw value was outside [min,max]). Summary: "Cutoff → 1.2 kHz".

**connect** `{from_module, output_id: int|string, to_module, input_id: int|string,
color?: "#rrggbb", replace?: bool=false}`. Validates modules (engine + widget) and port
ranges BEFORE creating anything. Same cable exists → ok with note "already connected".
Input already connected → error naming the existing cable id unless `replace` (then those
cables are removed with `history::CableRemove`). Create: `new engine::Cable` →
`engine->addCable` → `new CableWidget; cw->setCable(c); cw->color = color` →
`rack->addCable(cw)` → `history::CableAdd`. Color: hex via `color::fromHexString`, else
`rack->getNextCableColor()`. → `{ok, cable_id, from:{module,output,name}, to:{module,input,name},
replaced_cable_ids:[]}`. Summary: "VCO Saw → VCF Audio".

**disconnect** `{cable_id}` or `{to_module, input_id}` → `history::CableRemove` (setCable
before removal), `rack->removeCable(cw)`, `delete cw`. → `{ok, removed_cable_ids:[...]}`.
Summary: "Disconnected VCO Saw → VCF Audio". Not destructive (undoable, spec).

**save_patch** `{path?: string}` — empty → current `APP->patch->path` (untitled → error asking
for a path). Only relative paths are accepted: absolute paths and any `..` component are rejected with an
error; the file is stored under `asset::user("patches")/<path>` (dirs created); ".vcv" is
appended unless the name already ends in .vcv. The deepest existing part of the target is canonicalized
(symlinks resolved) and must stay under the canonical patches folder, otherwise the call fails. Confirmation only if the target exists and is not the current patch file:
"Overwrite existing file <path>?". Run: `APP->patch->save(path)` (catch), `patch->path = path`,
`patch->pushRecentPath(path)`, `ctx.savedDuringRun = true; ctx.mutationsSinceSave = 0`.
→ `{ok, path}`. Summary: "Saved my-acid.vcv".

**clear_patch** `{}` — destructive. Confirmation "Remove all N modules and M cables from
the patch?". Removes every module like remove_module (undoable; never `patch->clear()`).
Summary: "Cleared patch (N modules)".

## 8. Controller (`include/assistant/Controller.hpp`)

```cpp
struct ChatEntry {
	enum Kind { USER, ASSISTANT, ACTIONS, ERROR, INFO, CONFIRM };
	struct Action { std::string text; bool ok = true; bool readOnly = false; };
	enum ConfirmState { PENDING, ALLOWED, DENIED, CANCELLED };
	uint64_t id = 0;              // unique, increasing
	Kind kind = INFO;
	std::string text;             // USER/ASSISTANT/ERROR/INFO text; CONFIRM question
	std::string reasoning;        // ASSISTANT: optional reasoning (collapsed in UI)
	std::vector<Action> actions;  // ACTIONS
	ConfirmState confirmState = PENDING;  // CONFIRM
};

struct Controller {
	enum State { IDLE, WAITING_HTTP, EXECUTING_TOOLS, WAITING_CONFIRM };
	Controller();
	~Controller();                                   // cancels + joins all workers
	/** Drive the state machine. UI thread, every frame. */
	void step();
	/** Starts a run. false if busy or text empty (after trim). */
	bool send(const std::string& text, bool attachSelection);
	void cancel();                                   // no-op when idle
	void newChat();                                  // cancels, clears conversation + transcript
	void confirm(uint64_t entryId, bool allow);
	State getState() const;
	bool isBusy() const;                             // state != IDLE
	/** e.g. "Thinking… 3s", "Running tools…", "Waiting for your confirmation", idle: "" or last usage. */
	std::string getStatusText() const;
	const std::vector<ChatEntry>& getEntries() const;
	/** Increments on every transcript change (UI rebuild trigger). */
	uint64_t getRevision() const;
	const std::vector<ChatMessage>& getMessages() const;  // API conversation (no system msg)
	const Config& getConfig() const;
	void setConfig(const Config& c);                 // saves assistant.json
	void reloadConfig();
	/** Testing hook: replace client creation (e.g. force mock). */
	void setClientFactory(std::function<std::shared_ptr<LlmClient>(const ClientOptions&)> f);
};
```

Run lifecycle (UI thread):
1. `send`: reload config (from file) and system prompt (create default file if missing);
   build user content = text + (if attachSelection and selection non-empty) a block
   `"\n\n[Selected modules]\n- id <id>: <plugin> <model> \"<name>\" at HP <hp>, row <row>\n..."`;
   append USER entry (text only + "(+N selected modules attached)") and USER message;
   round = 0; record the patch generation; the undo action (`RunAction`) is created lazily by the first mutating call (see below); start HTTP.
2. Start HTTP: request = [system message (prompt file + environment block: Rack version,
   installed plugins with model counts)] + conversation trimmed to `maxContextChars` (drop
   oldest whole turns; a turn starts at a USER message; always keep the latest turn) +
   `defaultRegistry().toolsJson()`. Worker thread runs `client->complete`.
3. Response OK: append ASSISTANT message (verbatim incl. tool calls / reasoning details).
   If content non-empty → ASSISTANT entry (with reasoning). If `finish_reason == "length"`
   and no tool calls → INFO "The response was cut off (max_tokens)." No tool calls → end run.
   Else queue tool calls → EXECUTING_TOOLS with a fresh ACTIONS entry.
4. EXECUTING_TOOLS (per frame, process calls in order): if
   `config.confirmDestructive` and `confirmationFor()` non-empty → CONFIRM entry, state
   WAITING_CONFIRM. Allow → execute; Deny → tool result
   `{"ok":false,"error":"The user declined this action."}`. Execute via
   `defaultRegistry().execute()`; append TOOL message (content truncated to 24 000 bytes with
   a note); add summary to the ACTIONS entry (error results show "✗ <tool>: <short error>").
   All done → round++; round >= maxToolRounds → INFO "Stopped after N tool rounds
   (max_tool_rounds)." and end run; else start HTTP.
5. Response error → ERROR entry with `error.message`; end run.
6. Cancel: HTTP → retire worker, INFO "Cancelled."; tools/confirm → remaining tool calls
   get `{"ok":false,"error":"Cancelled by the user."}` results (keeps the API history valid),
   pending CONFIRM entries → CANCELLED; end run.
7. End run: state IDLE, per-run bookkeeping reset, status line "Last run: N requests · tokens · $cost".
   The run's changes are already in the history (see below), nothing is pushed here.
- **Eager undo push.** The run's undo action is a `RunAction : history::ComplexAction` with a
  `shared_ptr<bool> alive` flag that its destructor clears (the history may delete it any time).
  The first tool call that changed something pushes it to `APP->history` immediately; later calls
  append to it, so the whole run stays one undo step and Ctrl+Z works even while the run is still
  going. Before every call and every frame the controller checks (`isTopAction`, using `alive`)
  that the action is still the newest undo step with no redo stack; if the user edited, undid or
  redid in the meantime, the action is dropped and the next mutation starts a new one (the run is
  then split into several undo steps). Appending to an action sitting at the saved index clears
  `savedIndex`, so the patch shows as modified.
- `bool canUndoLastRun()` is true when idle, the last run pushed exactly one action, the patch
  generation is unchanged and that action is still the top undo step. `void undoLastRun()` then
  calls `APP->history->undo()` once. The panel shows an "Undo changes" button in the status row
  while `canUndoLastRun()`.
- **PatchEvents** (`PatchEvents.hpp`): `patch::Manager::clear()` (File > New/Open/Revert,
  templates, autosave restore) calls `notifyPatchCleared()`, which bumps `getPatchGeneration()`.
  A run records the generation at start; when it changes, `step()` cancels the run
  ("Cancelled: the patch was replaced..."), forgets the action (the history deleted it) and
  `confirm()` is ignored, so no stale tool call runs on the new patch.
- Confirm auto-show: if the panel is hidden while the state is WAITING_CONFIRM with a pending
  CONFIRM entry, the panel shows itself again (without taking keyboard focus) so the run cannot
  stall unnoticed.
- Reasoning `none` retry: if a response is a 400 whose text mentions `reasoning_effort` and
  `none` (OpenAI direct with tools) and the effort was not already off/none, the run restarts once
  with an effort override of `none` and an INFO entry tells the user to change the setting.
- Worker: `struct PendingRequest { std::atomic<bool> cancel, done; ChatResponse response;
  std::thread thread; }` held by `std::shared_ptr`; thread name "Assistant"; catches all
  exceptions → OTHER error. Retired requests are joined once `done`.
- The controller never stores engine/widget pointers.

## 9. System prompt (`include/assistant/SystemPrompt.hpp`)

```cpp
std::string defaultSystemPrompt();
std::string systemPromptPath();      // asset::user("assistant-system-prompt.md")
/** Reads the file; creates it with the default if missing; falls back to default on error. */
std::string loadSystemPrompt();
/** Environment block appended at runtime: Rack version, installed plugins + model counts. UI thread. */
std::string environmentBlock();
```
Default prompt content (English, model answers in the user's language): role in a modular
software synth with tools; always start with get_patch / search_modules / get_module_info,
never guess slugs, ids or port numbers, only use installed modules; patch needs an audio
output module (search "audio" in Core) to be audible; cables go from outputs to inputs;
1V/oct; after changes briefly explain what was built and which knobs are worth tweaking
live; answer concisely in the user's language, plain text (no tables, minimal markdown);
destructive actions may require user confirmation; domain knowledge: VCO/VCF/VCA,
envelopes, LFO, clock, sequencer, CV/gate, 1V/oct; Tekno/Tribe/Freetek sound design:
kick from sine + pitch envelope + distortion, 303-style acid (saw/square → resonant LP,
accent, slide), stabs and hoovers, breaks/samples, dub delay, hard distortion, tempo
roughly 150–190 BPM depending on substyle.

## 10. UI

**Panel** (`include/assistant/Panel.hpp`):
```cpp
widget::Widget* createPanel();        // hidden; called by Scene constructor
bool isPanelVisible();
void setPanelVisible(bool visible);   // focuses the input when shown
void togglePanel();
void newChat();
void openSettings();                  // opens SettingsDialog
/** Scene::step hook: if visible, places the panel at the right side of `area` and returns
the width taken; else 0. */
float layoutPanel(math::Rect area);
```
`void sceneStepHook();` (per-frame self-test trigger) lives in `include/assistant/SelfTest.hpp`
so that the self test exists before the panel does.
- `Panel : widget::OpaqueWidget` owns the `Controller` (`std::unique_ptr`); a file-static
  pointer to the single live panel serves the free functions (cleared in the destructor).
- Layout (top→bottom): header (title "Assistant", dim model label, buttons "New",
  "Settings", "×"), message list (`ui::ScrollWidget`), status line (busy spinner text),
  input (multiline `ui::TextField`, auto-grows 2–8 lines), bottom row ("Selection: N"
  toggle button, "Send"/"Cancel" button). Background `bndMenuBackground` + left border,
  colors from `bndGetTheme()`.
- Messages: USER (textField theme inner color), ASSISTANT (menu theme inner color), ACTIONS
  (small dim single paragraph joined with " · ", errors in red), ERROR (red tint), INFO
  (dim italic-like), CONFIRM (question + "Allow"/"Deny" buttons, state label afterwards).
  ASSISTANT with reasoning: clickable "▸ Reasoning" toggles a dim reasoning paragraph.
  Right-click on a message → menu "Copy text" (clipboard via glfwSetClipboardString).
  Word-wrapped with `nvgTextBox`/`nvgTextBoxBounds` (uiFont, 13 px). Rebuild on revision
  change; keep scroll pinned to bottom when it was at the bottom.
- Input: Enter sends, Shift+Enter newline, Escape unfocuses. While busy Enter does nothing
  (Send becomes Cancel). Keys handled by the field are consumed.
- Left edge (5 px) drag resizes the panel; width saved to config on release.
- Docking in `Scene::step`: after the existing size code, `w = assistant::layoutPanel(
  Rect(rackScroll->box.pos, rackScroll->box.size))`; `rackScroll->box.size.x -= w`.
- Shortcut: Ctrl+L (`RACK_MOD_CTRL`) in `Scene::onHoverKey` (first block) toggles.

**Menu** ("Assistant" button in `MenuBar` before Help): "Show assistant" check item with
"Ctrl+L", "New chat", "Settings…", separator, "Open user folder" (system::openDirectory
(asset::user(""))).

**SettingsDialog** (`ui::MenuOverlay` modal, Esc/click outside closes, centered panel ~560 px
wide): rows label + field: Base URL, Model, Reasoning effort (ChoiceButton menu: off,
none, minimal, low, medium, high), Parameter style (openrouter, openai), Max tokens (blank =
unset), Temperature (blank = unset), API key (`ui::PasswordField`; blank keeps the stored
key; info line shows source + masked key, e.g. "Using env OPENROUTER_API_KEY
(sk-or-v1…abcd) — overrides the stored key"; "Clear stored key" button), Extra headers
(multiline "Name: value" per line), Max tool rounds, Timeout (s), CA bundle, checkboxes
Confirm destructive actions / Mock mode (no network). Buttons: Save (validates; errors in
red; saves via `Controller::setConfig`), Cancel. Tab cycles fields (prevField/nextField).
Key containment: every focusable widget consumes all key presses it does not handle (and Escape
closes the dialog), so no shortcut of `Scene::onHoverKey` (undo, save, delete selection, ...)
fires behind the modal. The API key field is a `PasswordField` that blocks Ctrl+C/Ctrl+X and has
no context menu (the key cannot be copied out); the typed text is zeroed when the dialog closes.

## 11. Tests

- `make test-assistant` (Linux): builds `build/assistant_test` from `tests/assistant/*.cpp`
  linked against `libRack.so` (rpath `$ORIGIN/..`), runs it; exit code ≠ 0 on failure.
  Covers: request body (reasoning styles, off, max_tokens variants, temperature, tools,
  null content, reasoning_details echo), response parsing (text only, null content + tool
  calls, multiple tool calls, content arrays, object arguments, missing ids, legacy
  function_call, reasoning fields, usage, 200-with-error, missing choices, invalid JSON,
  each HTTP status class), config (defaults, type-checked parsing, invalid values, extra
  headers CR/LF, save/load roundtrip + 0600, key precedence via setenv/unsetenv, masking),
  arg helpers, mock client scenarios (no APP needed). `test_http.cpp` runs `HttpLlmClient` against
  an in-process fake server on 127.0.0.1 (request shape, no auth header for localhost, status
  mapping, invalid JSON, cancel/timeout/refused, missing or malformed key). `test_live.cpp` holds
  opt-in tests against a real provider (`--live`, env `RACK_ASSISTANT_*`, see README); they never
  print keys or bodies. `make test-assistant ASSISTANT_TEST_ARGS="--live --filter x"`.
- In-app self test: `RACK_ASSISTANT_SELFTEST=tools|mock|all ./Rack -d -u <tmp>` (script
  `tests/assistant/run_selftest.sh` creates a temp user dir with a `plugins` symlink, runs
  under Xvfb if no display). Runs on the UI thread after ~30 frames: tools scenario against
  a fresh patch (add/connect/set_param clamping/invalid ids/ports/move overlap/get_patch/
  search/get_module_info/disconnect/remove/save/clear, single undo restores, redo
  re-applies, confirmation detection) and mock controller scenarios (build, delete with
  allow/deny, badargs, error 402, cancel, loop limit). Logs `[assistant selftest] PASS/FAIL
  <name>`, writes `assistant-selftest-result.json` to the user dir, then closes the window;
  the script exits non-zero on failure.

## 12. Phase 2 hooks (not implemented now)

- Streaming: `LlmClient::completeStream` + SSE parsing in HttpLlmClient; Controller
  appends deltas to the current ASSISTANT entry (transcript entries are already mutable).
- MIDI mapping by chat: new tools in a separate registration function (e.g.
  `registerMidiTools`) operating on Core MIDI-Map via the same ToolContext/undo mechanism.
- Patch recipes: system prompt snippets / a `recipes/` folder exposed through a tool.
- Chat history in the patch: `messageToJson`/`messageFromJson` already exist; the
  controller can serialize its conversation into `patch::Manager` JSON later.
- Screenshots to multimodal models: `ChatMessage.content` would become parts; Protocol
  already parses content arrays.

## 13. Decisions taken (open questions resolved autonomously)

1. `reasoning_effort: off` omits every reasoning parameter (for non-reasoning models).
2. Confirmations are buttons in the chat (Allow/Deny), not typed answers.
3. Saving onto the current patch file is not destructive; overwriting another existing file is.
4. `disconnect` is not destructive (undoable); remove_module, clear_patch, overwriting files are.
5. API key precedence exactly as specified (env RACK_ASSISTANT_API_KEY, env
   OPENROUTER_API_KEY, file). Env keys are shown read-only (masked) in settings.
6. UI strings are English like the rest of Rack; the model answers in the user's language.
7. The panel is docked on the right (non-modal) so the rack stays usable; Ctrl+L toggles.
8. The OpenAI `max_completion_tokens` naming is used for `reasoning_param_style: openai`.
9. Extra effort value `none` (verified live): OpenAI direct + tools needs it. The controller
   retries a run once with `none` when a 400 says reasoning_effort is unsupported with tools,
   and tells the user to change the setting.
10. The undo action is pushed eagerly and re-validated each frame (section 8) instead of once at
    the end of the run, so edits made during a run cannot corrupt the history.
