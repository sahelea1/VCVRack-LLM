#include <assistant/Controller.hpp>
#include <assistant/PatchEvents.hpp>
#include <assistant/Protocol.hpp>
#include <assistant/SystemPrompt.hpp>
#include <assistant/Tools.hpp>
#include "ToolHelpers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <app/Scene.hpp>
#include <app/RackWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <asset.hpp>
#include <context.hpp>
#include <history.hpp>
#include <logger.hpp>
#include <plugin/Model.hpp>
#include <plugin/Plugin.hpp>
#include <string.hpp>
#include <system.hpp>


namespace rack {
namespace assistant {


/** Tool result content sent to the model is cut to this many bytes. */
static const size_t MAX_TOOL_RESULT_BYTES = 24000;
/** Maximum number of tool calls executed per frame. */
static const int MAX_TOOLS_PER_STEP = 4;
/** Maximum number of selected modules described in the attachment. */
static const size_t MAX_SELECTION_LISTED = 40;
/** The run stops after this many consecutive tool rounds in which every call failed. */
static const int MAX_FAILED_ROUNDS = 3;
/** Maximum length of an error shown in the ACTIONS entry. */
static const size_t MAX_ACTION_ERROR_BYTES = 80;


/** One HTTP request running on a worker thread. Shared between the controller and the worker; the
controller always joins `thread` before dropping the last reference (the worker only holds a raw pointer). */
struct PendingRequest {
	std::atomic<bool> cancel;
	std::atomic<bool> done;
	ChatResponse response;
	std::thread thread;

	PendingRequest() : cancel(false), done(false) {}
};


typedef std::shared_ptr<PendingRequest> PendingRequestPtr;


/** The undo action of a run. Its destructor flips a shared flag, so the controller can tell whether the history still owns the action
without dereferencing a possibly freed pointer (history clear, redo-stack truncation, 500-action cap). This also rules out ABA problems
when the allocator hands out the same address for a new action. */
struct RunAction : history::ComplexAction {
	std::shared_ptr<bool> alive;

	RunAction() : alive(std::make_shared<bool>(true)) {
		name = "assistant changes";
	}
	~RunAction() {
		*alive = false;
	}
};


static uint64_t patchGeneration = 0;

void notifyPatchCleared() {
	patchGeneration++;
}

uint64_t getPatchGeneration() {
	return patchGeneration;
}


static void joinRequest(const PendingRequestPtr& p) {
	if (p && p->thread.joinable())
		p->thread.join();
}


struct Controller::Internal {
	Config config;
	/** True if the last setConfig() could not be saved; then the in-memory config must not be replaced by the file. */
	bool configUnsaved = false;
	std::string lastConfigWarning;
	std::function<std::shared_ptr<LlmClient>(const ClientOptions&)> clientFactory;

	State state = IDLE;
	std::vector<ChatEntry> entries;
	std::vector<ChatMessage> messages;
	uint64_t nextEntryId = 1;
	uint64_t revision = 0;

	PendingRequestPtr pending;
	std::vector<PendingRequestPtr> retired;
	std::chrono::steady_clock::time_point requestStart;

	// ---- Current run ----
	/** Undo action that tool calls append to. Created lazily by the first tool call of a run.
	It is pushed to APP->history right after the first call that changed something, so history stays in chronological order
	even if the user edits the patch while the run is waiting for the model or for a confirmation.
	While `undoInHistory` is true the history owns it: it must not be dereferenced unless undoIsTop() holds. */
	RunAction* undo = NULL;
	bool undoInHistory = false;
	/** Liveness flag of `undo` (valid while `undo` is not NULL) */
	std::shared_ptr<bool> undoAlive;
	/** The most recent action of the last run that was pushed to the history, for undoLastRun(). Reset when a run starts. */
	RunAction* lastAction = NULL;
	std::shared_ptr<bool> lastAlive;
	/** Number of RunActions the current/last run pushed to the history. More than one means the run was split by an edit of someone else
	(or its action was pushed out of the history), so one undo step cannot revert all of its changes and undoLastRun() is not offered. */
	int runActionsPushed = 0;
	/** Number of actions in `undo` before the current tool call */
	size_t undoActionsBefore = 0;
	/** getPatchGeneration() at the start of the run */
	uint64_t runPatchGeneration = 0;
	ToolContext ctx;
	int round = 0;
	/** Consecutive tool rounds in which every call failed (the model is stuck in a loop) */
	int failedRounds = 0;
	/** Some call of the current round succeeded */
	bool roundHadSuccess = false;
	/** System prompt + environment block, fixed for the whole run */
	std::string systemText;
	/** Non-empty: reasoning effort override for this run only */
	std::string effortOverride;
	/** Endpoint/model/reasoning settings that effortOverride was learned for */
	std::string effortOverrideKey;
	bool retriedWithNone = false;
	int requestCount = 0;
	int64_t totalTokens = 0;
	bool tokensKnown = false;
	double totalCost = 0.0;
	bool costKnown = false;
	/** Tool calls of the current model response */
	std::vector<ToolCall> calls;
	size_t nextCall = 0;
	/** ACTIONS entry being filled (0: create a new one on the next action) */
	uint64_t actionsEntryId = 0;
	/** CONFIRM entry the state machine waits for */
	uint64_t confirmEntryId = 0;
	/** Decision for calls[nextCall]: 0 = not asked yet, 1 = allowed, 2 = denied */
	int confirmDecision = 0;
	/** Index into `messages` where the previous request's trimmed context started */
	size_t lastTrimStart = 0;

	std::string lastRunSummary;

	~Internal() {
		if (pending)
			pending->cancel = true;
		for (PendingRequestPtr& p : retired)
			p->cancel = true;
		joinRequest(pending);
		for (PendingRequestPtr& p : retired)
			joinRequest(p);
		// An action that was already pushed belongs to the history (APP may already be gone); an unpushed one is empty
		if (!undoInHistory)
			delete undo;
	}

	// ---- Undo bookkeeping ----

	/** True if `action` is still owned by the history and is its newest undo step (the next undo() reverts it).
	With `noRedo`, there must also be no redo stack behind it: appending to the action is then equivalent to pushing a new action right now. */
	static bool isTopAction(const RunAction* action, const std::shared_ptr<bool>& alive, bool noRedo) {
		if (!action || !alive || !*alive || !APP || !APP->history)
			return false;
		history::State* h = APP->history;
		if (h->actionIndex <= 0 || h->actionIndex > (int) h->actions.size() || h->actions[h->actionIndex - 1] != action)
			return false;
		return !noRedo || h->actionIndex == (int) h->actions.size();
	}

	bool undoIsTop() const {
		return undoInHistory && isTopAction(undo, undoAlive, true);
	}

	/** Forgets `undo` without touching it if the history owns it. */
	void releaseUndo() {
		if (undo && !undoInHistory)
			delete undo;
		undo = NULL;
		undoInHistory = false;
		undoAlive.reset();
	}

	/** Drops `undo` as soon as the history was changed by someone else (user edit, undo, redo, clear). The next tool call then starts a new action. */
	void validateUndo() {
		if (undo && undoInHistory && !undoIsTop()) {
			undo = NULL;
			undoInHistory = false;
			undoAlive.reset();
		}
	}

	/** Prepares ctx.undo for one tool call. */
	void beginUndo() {
		validateUndo();
		if (!undo) {
			undo = new RunAction;
			undoAlive = undo->alive;
			undoInHistory = false;
		}
		undoActionsBefore = undo->actions.size();
		ctx.undo = undo;
	}

	/** Pushes the action of a call that changed something. Appending to the pushed top action keeps one undo step per run. */
	void endUndo() {
		if (!undo)
			return;
		history::State* h = (APP && APP->history) ? APP->history : NULL;
		if (!undoInHistory) {
			if (!undo->isEmpty() && h) {
				h->push(undo);
				undoInHistory = true;
				lastAction = undo;
				lastAlive = undoAlive;
				runActionsPushed++;
			}
		}
		else if (h && undo->actions.size() != undoActionsBefore) {
			// The patch changed at an already-saved history position: the file on disk no longer matches it
			if (h->savedIndex == h->actionIndex)
				h->savedIndex = -1;
		}
		ctx.undo = NULL;
	}

	bool canUndoLast() const {
		return state == IDLE && lastAction && runActionsPushed == 1 && runPatchGeneration == getPatchGeneration() && isTopAction(lastAction, lastAlive, false);
	}

	// ---- Transcript ----

	ChatEntry& addEntry(ChatEntry::Kind kind, const std::string& text) {
		ChatEntry e;
		e.id = nextEntryId++;
		e.kind = kind;
		e.text = text;
		entries.push_back(e);
		revision++;
		return entries.back();
	}

	ChatEntry* findEntry(uint64_t id) {
		for (size_t i = entries.size(); i > 0; i--) {
			if (entries[i - 1].id == id)
				return &entries[i - 1];
		}
		return NULL;
	}

	void addAction(const std::string& text, bool ok, bool readOnly) {
		ChatEntry* e = actionsEntryId ? findEntry(actionsEntryId) : NULL;
		if (!e) {
			e = &addEntry(ChatEntry::ACTIONS, "");
			actionsEntryId = e->id;
		}
		if (!e->actions.empty()) {
			// Merge identical consecutive actions ("text ×3") so a model repeating a failing call does not flood the chat
			ChatEntry::Action& last = e->actions.back();
			if (last.ok == ok && last.readOnly == readOnly && last.key == text) {
				last.repeat++;
				last.text = text + string::f(" \xc3\x97%d", last.repeat);
				revision++;
				return;
			}
		}
		ChatEntry::Action a;
		a.text = text;
		a.key = text;
		a.ok = ok;
		a.readOnly = readOnly;
		e->actions.push_back(a);
		revision++;
	}

	// ---- Config ----

	/** Reloads assistant.json. A broken file keeps the current config. */
	void reloadConfigFromDisk() {
		if (configUnsaved)
			return;
		std::vector<std::string> warnings;
		Config c = loadConfig(configPath(), &warnings);
		bool broken = false;
		std::string joined;
		for (const std::string& w : warnings) {
			if (w.find("Could not parse") == 0 || w.find("Could not read") == 0 || w.find("Error while loading") == 0)
				broken = true;
			if (!joined.empty())
				joined += " ";
			joined += w;
		}
		if (!broken)
			config = c;
		if (joined != lastConfigWarning) {
			lastConfigWarning = joined;
			if (!joined.empty()) {
				WARN("Assistant: %s", joined.c_str());
				addEntry(ChatEntry::INFO, "assistant.json: " + truncateUtf8(joined, 400) + (broken ? " Keeping the previous settings." : ""));
			}
		}
	}

	std::string effectiveEffort() const {
		return effortOverride.empty() ? config.reasoningEffort : effortOverride;
	}

	// ---- Helpers ----

	static std::string selectionBlock(int* countOut) {
		*countOut = 0;
		if (!APP || !APP->scene || !APP->scene->rack)
			return "";
		app::RackWidget* rack = APP->scene->rack;
		if (!rack->hasSelection())
			return "";
		std::vector<app::ModuleWidget*> mws;
		for (app::ModuleWidget* mw : rack->getSelected()) {
			if (mw && mw->module && mw->model)
				mws.push_back(mw);
		}
		if (mws.empty())
			return "";
		std::sort(mws.begin(), mws.end(), [](app::ModuleWidget* a, app::ModuleWidget* b) {
			return a->module->id < b->module->id;
		});
		std::string s = "\n\n[Selected modules]";
		size_t n = std::min(mws.size(), MAX_SELECTION_LISTED);
		for (size_t i = 0; i < n; i++) {
			app::ModuleWidget* mw = mws[i];
			helpers::GridPos p = helpers::gridPosOf(mw);
			s += string::f("\n- id %lld: %s/%s \"%s\" at HP %d, row %d", (long long) mw->module->id, mw->model->plugin->slug.c_str(), mw->model->slug.c_str(), mw->model->name.c_str(), p.hp, p.row);
		}
		if (mws.size() > n)
			s += string::f("\n- ... and %d more", (int) (mws.size() - n));
		*countOut = (int) mws.size();
		return s;
	}

	static size_t messageChars(const ChatMessage& m) {
		size_t n = m.content.size() + m.toolCallId.size() + m.reasoningDetailsJson.size();
		for (const ToolCall& tc : m.toolCalls)
			n += tc.id.size() + tc.name.size() + tc.arguments.size();
		return n;
	}

	/** System message + the conversation, trimmed to maxContextChars by dropping the oldest whole turns. */
	std::vector<ChatMessage> buildRequestMessages() {
		std::vector<ChatMessage> out;
		ChatMessage sys;
		sys.role = ChatMessage::SYSTEM;
		sys.content = systemText;
		out.push_back(sys);

		size_t total = systemText.size();
		for (const ChatMessage& m : messages)
			total += messageChars(m);

		size_t start = 0;
		size_t limit = config.maxContextChars > 0 ? (size_t) config.maxContextChars : 0;
		while (total > limit) {
			// Next turn start after `start`
			size_t next = messages.size();
			for (size_t i = start + 1; i < messages.size(); i++) {
				if (messages[i].role == ChatMessage::USER) {
					next = i;
					break;
				}
			}
			// Always keep the latest turn
			if (next >= messages.size())
				break;
			for (size_t i = start; i < next; i++)
				total -= messageChars(messages[i]);
			start = next;
		}
		if (start > lastTrimStart) {
			lastTrimStart = start;
			addEntry(ChatEntry::INFO, "Older messages were left out of the request to stay within max_context_chars.");
		}
		for (size_t i = start; i < messages.size(); i++)
			out.push_back(messages[i]);
		return out;
	}

	// ---- Run lifecycle ----

	bool send(const std::string& rawText, bool attachSelection) {
		if (state != IDLE)
			return false;
		std::string text = helpers::trim(rawText);
		if (text.empty())
			return false;

		reloadConfigFromDisk();

		// Fresh run state
		systemText = loadSystemPrompt() + "\n\n" + environmentBlock();
		// The "none" fallback learned from a 400 stays for the following runs as long as endpoint, model
		// and reasoning settings are unchanged (otherwise every message would cost a failing request first)
		std::string overrideKey = config.baseUrl + "|" + config.model + "|" + config.reasoningEffort + "|" + config.reasoningParamStyle;
		if (overrideKey != effortOverrideKey) {
			effortOverride.clear();
			effortOverrideKey = overrideKey;
		}
		retriedWithNone = false;
		requestCount = 0;
		totalTokens = 0;
		tokensKnown = false;
		totalCost = 0.0;
		costKnown = false;
		round = 0;
		failedRounds = 0;
		roundHadSuccess = false;
		calls.clear();
		nextCall = 0;
		actionsEntryId = 0;
		confirmEntryId = 0;
		confirmDecision = 0;
		lastRunSummary.clear();

		std::string content = text;
		int selected = 0;
		if (attachSelection)
			content += selectionBlock(&selected);

		ChatEntry& e = addEntry(ChatEntry::USER, text);
		if (selected > 0)
			e.text += string::f("\n(+%d selected module%s attached)", selected, selected == 1 ? "" : "s");

		ChatMessage um;
		um.role = ChatMessage::USER;
		um.content = content;
		messages.push_back(um);

		releaseUndo();
		lastAction = NULL;
		lastAlive.reset();
		runActionsPushed = 0;
		ctx = ToolContext();
		runPatchGeneration = getPatchGeneration();

		startHttp();
		return true;
	}

	void startHttp() {
		ChatRequest req;
		req.messages = buildRequestMessages();
		req.toolsJson = defaultRegistry().toolsJson();

		Config runConfig = config;
		if (!effortOverride.empty())
			runConfig.reasoningEffort = effortOverride;
		std::shared_ptr<LlmClient> client;
		try {
			ClientOptions opts = makeClientOptions(runConfig);
			client = clientFactory ? clientFactory(opts) : createClient(opts);
		}
		catch (const std::exception& ex) {
			addEntry(ChatEntry::ERROR, std::string("Could not create the LLM client: ") + ex.what());
			endRun();
			return;
		}
		if (!client) {
			addEntry(ChatEntry::ERROR, "Could not create the LLM client.");
			endRun();
			return;
		}

		PendingRequestPtr p = std::make_shared<PendingRequest>();
		PendingRequest* raw = p.get();
		try {
			p->thread = std::thread([raw, client, req]() {
				system::setThreadName("Assistant");
				ChatResponse resp;
				try {
					resp = client->complete(req, raw->cancel);
				}
				catch (const std::exception& ex) {
					resp = ChatResponse();
					resp.ok = false;
					resp.error.kind = LlmError::OTHER;
					resp.error.message = std::string("Unexpected error: ") + ex.what();
				}
				catch (...) {
					resp = ChatResponse();
					resp.ok = false;
					resp.error.kind = LlmError::OTHER;
					resp.error.message = "Unexpected error.";
				}
				raw->response = std::move(resp);
				raw->done.store(true, std::memory_order_release);
			});
		}
		catch (const std::exception& ex) {
			addEntry(ChatEntry::ERROR, std::string("Could not start the request thread: ") + ex.what());
			endRun();
			return;
		}
		pending = p;
		requestStart = std::chrono::steady_clock::now();
		state = WAITING_HTTP;
	}

	void endRun() {
		state = IDLE;
		calls.clear();
		nextCall = 0;
		actionsEntryId = 0;
		confirmEntryId = 0;
		confirmDecision = 0;
		// The run's changes are already in the history (see endUndo)
		releaseUndo();
		ctx = ToolContext();

		lastRunSummary.clear();
		if (requestCount > 0) {
			std::string s = string::f("Last run: %d request%s", requestCount, requestCount == 1 ? "" : "s");
			if (tokensKnown)
				s += string::f(" · %lld tokens", (long long) totalTokens);
			if (costKnown)
				s += string::f(" · $%.4f", totalCost);
			lastRunSummary = s;
		}
		revision++;
	}

	void reapRetired() {
		for (size_t i = 0; i < retired.size();) {
			if (retired[i]->done.load(std::memory_order_acquire)) {
				joinRequest(retired[i]);
				retired.erase(retired.begin() + i);
			}
			else {
				i++;
			}
		}
	}

	// ---- Responses ----

	bool shouldRetryWithNone(const ChatResponse& r) const {
		if (retriedWithNone || r.error.kind != LlmError::BAD_REQUEST)
			return false;
		std::string effort = effectiveEffort();
		if (effort == "off" || effort == "none")
			return false;
		std::string hay = helpers::lowercase(r.error.detail + " " + r.error.message);
		return hay.find("reasoning_effort") != std::string::npos && hay.find("none") != std::string::npos;
	}

	void onResponse(ChatResponse& r) {
		if (!r.ok) {
			if (shouldRetryWithNone(r)) {
				retriedWithNone = true;
				effortOverride = "none";
				addEntry(ChatEntry::INFO, "The provider does not support reasoning together with tools for this model; retried with reasoning effort 'none'. Change it in Settings to skip the extra request.");
				startHttp();
				return;
			}
			addEntry(ChatEntry::ERROR, r.error.message.empty() ? std::string("The request failed.") : r.error.message);
			endRun();
			return;
		}

		requestCount++;
		const Usage& u = r.usage;
		if (u.totalTokens >= 0) {
			totalTokens += u.totalTokens;
			tokensKnown = true;
		}
		else if (u.promptTokens >= 0 || u.completionTokens >= 0) {
			totalTokens += std::max(u.promptTokens, 0) + std::max(u.completionTokens, 0);
			tokensKnown = true;
		}
		if (u.cost >= 0.0) {
			totalCost += u.cost;
			costKnown = true;
		}

		ChatMessage msg = r.message;
		msg.role = ChatMessage::ASSISTANT;
		// Every tool call needs a unique id so that the tool results can refer to it
		for (size_t i = 0; i < msg.toolCalls.size(); i++) {
			if (msg.toolCalls[i].id.empty())
				msg.toolCalls[i].id = string::f("call_%d_%d", round, (int) i);
		}
		bool hasText = !helpers::trim(msg.content).empty();
		bool hasCalls = !msg.toolCalls.empty();

		if (!hasText && !hasCalls) {
			// An empty assistant message would only confuse later requests; do not keep it
			addEntry(ChatEntry::INFO, r.finishReason == "length" ? "The response was cut off (max_tokens)." : "The model returned an empty reply.");
			endRun();
			return;
		}

		messages.push_back(msg);
		if (hasText) {
			ChatEntry& e = addEntry(ChatEntry::ASSISTANT, msg.content);
			e.reasoning = msg.reasoning;
		}
		if (!hasCalls) {
			if (r.finishReason == "length")
				addEntry(ChatEntry::INFO, "The response was cut off (max_tokens).");
			endRun();
			return;
		}

		calls = msg.toolCalls;
		nextCall = 0;
		actionsEntryId = 0;
		confirmDecision = 0;
		state = EXECUTING_TOOLS;
	}

	// ---- Tools ----

	void appendToolMessage(const ToolCall& tc, const std::string& content) {
		ChatMessage tm;
		tm.role = ChatMessage::TOOL;
		tm.toolCallId = tc.id;
		tm.content = content;
		messages.push_back(tm);
	}

	/** Short error text of a failed tool result for the ACTIONS entry. */
	static std::string shortError(const ToolResult& r) {
		std::string err;
		json_error_t jerr;
		json_t* j = json_loads(r.content.c_str(), 0, &jerr);
		if (j) {
			json_t* e = json_is_object(j) ? json_object_get(j, "error") : NULL;
			if (e && json_is_string(e))
				err = json_string_value(e);
			json_decref(j);
		}
		if (err.empty())
			err = r.summary.empty() ? "failed" : r.summary;
		for (size_t i = 0; i < err.size(); i++) {
			if (err[i] == '\n' || err[i] == '\r' || err[i] == '\t')
				err[i] = ' ';
		}
		return truncateUtf8(err, MAX_ACTION_ERROR_BYTES);
	}

	void runCall(const ToolCall& tc, bool denied) {
		if (denied) {
			appendToolMessage(tc, errorJson("The user declined this action."));
			revision++;
			return;
		}
		DEBUG("Assistant: tool call %s %s", tc.name.c_str(), truncateUtf8(tc.arguments, 300).c_str());
		beginUndo();
		ToolResult r = defaultRegistry().execute(tc.name, tc.arguments, ctx);
		endUndo();
		std::string content = r.content;
		if (content.size() > MAX_TOOL_RESULT_BYTES)
			content = truncateUtf8(content, MAX_TOOL_RESULT_BYTES) + string::f("\n[Result truncated: it was longer than %d bytes.]", (int) MAX_TOOL_RESULT_BYTES);
		appendToolMessage(tc, content);
		if (r.ok)
			roundHadSuccess = true;
		if (!r.ok) {
			// Tool arguments never contain secrets; DEBUG level is only visible in dev mode (-d)
			DEBUG("Assistant: tool %s failed: %s (arguments: %s)", tc.name.c_str(), shortError(r).c_str(), truncateUtf8(tc.arguments, 300).c_str());
			addAction("✗ " + tc.name + ": " + shortError(r), false, false);
		}
		else if (!r.summary.empty())
			addAction(r.summary, true, r.readOnly);
		else
			revision++;
	}

	void stepTools() {
		int executed = 0;
		while (state == EXECUTING_TOOLS && nextCall < calls.size() && executed < MAX_TOOLS_PER_STEP) {
			const ToolCall tc = calls[nextCall];
			bool denied = false;
			if (confirmDecision == 0 && config.confirmDestructive) {
				std::string question = defaultRegistry().confirmationFor(tc.name, tc.arguments);
				if (!question.empty()) {
					ChatEntry& e = addEntry(ChatEntry::CONFIRM, question);
					confirmEntryId = e.id;
					// Actions after the answer go into a new ACTIONS entry below the question
					actionsEntryId = 0;
					state = WAITING_CONFIRM;
					return;
				}
			}
			else if (confirmDecision == 2) {
				denied = true;
			}
			confirmDecision = 0;
			runCall(tc, denied);
			nextCall++;
			executed++;
		}

		if (state == EXECUTING_TOOLS && nextCall >= calls.size()) {
			calls.clear();
			nextCall = 0;
			actionsEntryId = 0;
			round++;
			failedRounds = roundHadSuccess ? 0 : failedRounds + 1;
			roundHadSuccess = false;
			if (round >= config.maxToolRounds) {
				addEntry(ChatEntry::INFO, string::f("Stopped after %d tool rounds (max_tool_rounds).", round));
				endRun();
			}
			else if (failedRounds >= MAX_FAILED_ROUNDS) {
				// Do not burn tokens while the model repeats failing calls
				addEntry(ChatEntry::INFO, string::f("Stopped: the last %d tool rounds failed. Rephrase the request or try again.", failedRounds));
				endRun();
			}
			else {
				startHttp();
			}
		}
	}

	// ---- Cancel ----

	void cancelRun(const char* toolError = "Cancelled by the user.", const char* info = "Cancelled.") {
		if (state == IDLE)
			return;
		if (state == WAITING_HTTP) {
			if (pending) {
				pending->cancel = true;
				retired.push_back(pending);
				pending.reset();
			}
		}
		else {
			// Keep the API history valid: every tool call gets a result
			for (size_t i = nextCall; i < calls.size(); i++)
				appendToolMessage(calls[i], errorJson(toolError));
			if (state == WAITING_CONFIRM) {
				ChatEntry* e = findEntry(confirmEntryId);
				if (e && e->confirmState == ChatEntry::PENDING)
					e->confirmState = ChatEntry::CANCELLED;
			}
		}
		addEntry(ChatEntry::INFO, info);
		endRun();
	}

	/** The patch was replaced (New, Open, Revert) during the run: the history was cleared and the model's view of the patch is stale. */
	void cancelForPatchChange() {
		// The history deleted the run's action when it was cleared
		if (undoInHistory) {
			undo = NULL;
			undoInHistory = false;
			undoAlive.reset();
		}
		lastAction = NULL;
		lastAlive.reset();
		runActionsPushed = 0;
		cancelRun("Cancelled: the patch was replaced by the user.", "Cancelled: the patch was replaced.");
	}
};


// ---- Controller ----------------------------------------------------------------------

Controller::Controller() {
	internal = new Internal;
	std::vector<std::string> warnings;
	internal->config = loadConfig(configPath(), &warnings);
	for (const std::string& w : warnings)
		WARN("Assistant: %s", w.c_str());
}

Controller::~Controller() {
	delete internal;
}

void Controller::step() {
	Internal& d = *internal;
	d.reapRetired();

	if (d.state != IDLE) {
		if (getPatchGeneration() != d.runPatchGeneration)
			d.cancelForPatchChange();
		else
			d.validateUndo();
	}

	switch (d.state) {
		case IDLE:
			break;
		case WAITING_HTTP: {
			if (d.pending && d.pending->done.load(std::memory_order_acquire)) {
				PendingRequestPtr p = d.pending;
				d.pending.reset();
				joinRequest(p);
				ChatResponse r = std::move(p->response);
				d.onResponse(r);
			}
		} break;
		case EXECUTING_TOOLS:
			d.stepTools();
			break;
		case WAITING_CONFIRM:
			break;
	}
}

bool Controller::send(const std::string& text, bool attachSelection) {
	return internal->send(text, attachSelection);
}

void Controller::cancel() {
	internal->cancelRun();
}

void Controller::newChat() {
	Internal& d = *internal;
	d.cancelRun();
	d.entries.clear();
	d.messages.clear();
	d.lastRunSummary.clear();
	d.lastTrimStart = 0;
	d.lastAction = NULL;
	d.lastAlive.reset();
	d.runActionsPushed = 0;
	d.revision++;
}

bool Controller::canUndoLastRun() const {
	return internal->canUndoLast();
}

void Controller::undoLastRun() {
	Internal& d = *internal;
	if (!d.canUndoLast())
		return;
	d.lastAction = NULL;
	d.lastAlive.reset();
	APP->history->undo();
	d.revision++;
}

void Controller::confirm(uint64_t entryId, bool allow) {
	Internal& d = *internal;
	if (d.state != WAITING_CONFIRM || entryId != d.confirmEntryId)
		return;
	// The patch was replaced since the question was asked: the next step() cancels the run, nothing may run before
	if (getPatchGeneration() != d.runPatchGeneration)
		return;
	ChatEntry* e = d.findEntry(entryId);
	if (!e || e->kind != ChatEntry::CONFIRM || e->confirmState != ChatEntry::PENDING)
		return;
	e->confirmState = allow ? ChatEntry::ALLOWED : ChatEntry::DENIED;
	d.confirmDecision = allow ? 1 : 2;
	d.confirmEntryId = 0;
	d.state = EXECUTING_TOOLS;
	d.revision++;
}

Controller::State Controller::getState() const {
	return internal->state;
}

bool Controller::isBusy() const {
	return internal->state != IDLE;
}

std::string Controller::getStatusText() const {
	const Internal& d = *internal;
	switch (d.state) {
		case IDLE: {
			// The learned effort fallback stays active across runs and chats, so keep it visible
			std::string s = d.lastRunSummary;
			if (!d.effortOverride.empty())
				s += std::string(s.empty() ? "" : " · ") + "effort " + d.effortOverride + " (provider fallback)";
			return s;
		}
		case WAITING_HTTP: {
			auto elapsed = std::chrono::steady_clock::now() - d.requestStart;
			int sec = (int) std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
			return string::f("Thinking… %ds", sec);
		}
		case EXECUTING_TOOLS:
			return "Running tools…";
		case WAITING_CONFIRM:
			return "Waiting for your confirmation";
	}
	return "";
}

const std::vector<ChatEntry>& Controller::getEntries() const {
	return internal->entries;
}

uint64_t Controller::getRevision() const {
	return internal->revision;
}

const std::vector<ChatMessage>& Controller::getMessages() const {
	return internal->messages;
}

const Config& Controller::getConfig() const {
	return internal->config;
}

bool Controller::setConfig(const Config& c, std::string* error) {
	Internal& d = *internal;
	// Round trip through JSON to clamp values and drop invalid headers like loading the file does
	json_t* j = configToJson(c);
	d.config = configFromJson(j);
	json_decref(j);
	std::string err;
	bool ok = saveConfig(d.config, configPath(), &err);
	d.configUnsaved = !ok;
	if (!ok) {
		WARN("Assistant: could not save %s: %s", configPath().c_str(), err.c_str());
		if (error)
			*error = err;
	}
	d.revision++;
	return ok;
}

void Controller::reloadConfig() {
	Internal& d = *internal;
	d.configUnsaved = false;
	d.reloadConfigFromDisk();
}

void Controller::setClientFactory(std::function<std::shared_ptr<LlmClient>(const ClientOptions&)> factory) {
	internal->clientFactory = factory;
}


} // namespace assistant
} // namespace rack
