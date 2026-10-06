#pragma once
#include <string>
#include <vector>
#include <functional>
#include <stdint.h>

#include <jansson.h>

#include <history.hpp>


namespace rack {
namespace assistant {


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
/** Accepts JSON integer, real, or numeric string. Rejects non-finite values. */
bool argFloat(json_t* args, const char* key, float* out);
/** Accepts JSON strings only. */
bool argString(json_t* args, const char* key, std::string* out);
/** Accepts JSON booleans and the strings "true"/"false" (case-insensitive). */
bool argBool(json_t* args, const char* key, bool* out);
/** Steals obj, sets "ok":true, returns compact text. A NULL obj yields {"ok":true}. */
std::string okJson(json_t* obj);
/** {"ok":false,"error":msg} */
std::string errorJson(const std::string& msg);
ToolResult errorResult(const std::string& msg);


} // namespace assistant
} // namespace rack
