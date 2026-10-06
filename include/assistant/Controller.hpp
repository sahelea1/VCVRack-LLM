#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <assistant/Types.hpp>
#include <assistant/Config.hpp>
#include <assistant/LlmClient.hpp>


namespace rack {
namespace assistant {


/** One entry of the visible chat transcript (separate from the API conversation). */
struct ChatEntry {
	enum Kind {
		USER,
		ASSISTANT,
		/** Compact list of tool actions of one model round */
		ACTIONS,
		ERROR,
		INFO,
		/** Destructive tool call waiting for (or answered by) the user */
		CONFIRM,
	};
	struct Action {
		std::string text;
		bool ok = true;
		bool readOnly = false;
		/** Text without the repeat suffix; identical consecutive actions are merged ("text ×3"). */
		std::string key;
		int repeat = 1;
	};
	enum ConfirmState {
		PENDING,
		ALLOWED,
		DENIED,
		CANCELLED,
	};

	/** Unique, increasing. Stable for the lifetime of the entry. */
	uint64_t id = 0;
	Kind kind = INFO;
	/** USER/ASSISTANT/ERROR/INFO text, or the CONFIRM question. */
	std::string text;
	/** ASSISTANT: optional reasoning text, shown collapsed. */
	std::string reasoning;
	/** ACTIONS: one item per executed tool call. */
	std::vector<Action> actions;
	/** CONFIRM only. */
	ConfirmState confirmState = PENDING;
};


/** Runs the agent loop: sends the conversation to the LLM on a worker thread, executes tool calls on the UI thread, asks for confirmation of destructive calls, and keeps the transcript.

All methods must be called on the UI thread.
The loop is a state machine advanced by step(), which must be called every frame.
Each run (one send()) records all patch changes in one history::ComplexAction, so a single undo reverts the whole run.
*/
struct Controller {
	enum State {
		IDLE,
		WAITING_HTTP,
		EXECUTING_TOOLS,
		WAITING_CONFIRM,
	};

	struct Internal;
	Internal* internal;

	Controller();
	/** Cancels and joins all worker threads. */
	~Controller();

	/** Advances the state machine. Call every frame. */
	void step();
	/** Starts a run with the given user text.
	If `attachSelection` is true and modules are selected, a compact description of them is appended to the user message.
	Returns false if busy or if the text is empty after trimming.
	*/
	bool send(const std::string& text, bool attachSelection);
	/** Cancels the current run. No-op when idle. */
	void cancel();
	/** Cancels the current run and clears the conversation and transcript. */
	void newChat();
	/** Answers a CONFIRM entry. Ignored if the entry is not pending. */
	void confirm(uint64_t entryId, bool allow);

	State getState() const;
	bool isBusy() const;
	/** Short status line, e.g. "Thinking… 3s", "Running tools…", "Waiting for your confirmation". Empty or last-run usage when idle. */
	std::string getStatusText() const;
	const std::vector<ChatEntry>& getEntries() const;
	/** Incremented on every transcript change. */
	uint64_t getRevision() const;
	/** API conversation without the system message. */
	const std::vector<ChatMessage>& getMessages() const;

	/** True if the newest undo step in APP->history is the most recent run's whole change set (nothing else was done since, and the run was not split into several undo steps by edits made while it was running). False while a run is in progress. */
	bool canUndoLastRun() const;
	/** Undoes the most recent run's changes with one history undo step, if canUndoLastRun(). */
	void undoLastRun();

	const Config& getConfig() const;
	/** Applies and saves the config to assistant.json. Returns false and sets `error` on save failure (the config is still applied). */
	bool setConfig(const Config& c, std::string* error = NULL);
	/** Reloads assistant.json from disk. */
	void reloadConfig();

	/** Testing hook: replaces client creation, e.g. to force the mock client. Pass an empty function to restore the default. */
	void setClientFactory(std::function<std::shared_ptr<LlmClient>(const ClientOptions&)> factory);
};


} // namespace assistant
} // namespace rack
