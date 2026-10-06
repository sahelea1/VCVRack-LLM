#pragma once
// Private helpers shared by Tools.cpp, PatchTools.cpp and CatalogTools.cpp.
// Not part of the public API. All of this runs on the UI thread.
#include <string>
#include <vector>
#include <memory>
#include <stdint.h>

#include <jansson.h>

#include <math.hpp>
#include <engine/Module.hpp>
#include <engine/ParamQuantity.hpp>
#include <app/ModuleWidget.hpp>
#include <app/CableWidget.hpp>
#include <assistant/Tools.hpp>


namespace rack {
namespace assistant {
namespace helpers {


// ---- JSON ----------------------------------------------------------------------------

struct JsonDeleter {
	void operator()(json_t* j) const {
		if (j)
			json_decref(j);
	}
};
typedef std::unique_ptr<json_t, JsonDeleter> JsonPtr;

/** Replaces invalid UTF-8 byte sequences with '?', so jansson accepts and dumps the text. */
std::string sanitizeUtf8(const std::string& s);
/** Never returns NULL. */
json_t* jStr(const std::string& s);
/** Rounds to 6 significant digits so float32 values print as 0.1 instead of 0.10000000149.
Non-finite numbers become null (JSON has no inf/NaN). */
json_t* jNum(double v);
json_t* jInt(int64_t v);
json_t* jBool(bool b);
void setStr(json_t* obj, const char* key, const std::string& v);
void setNum(json_t* obj, const char* key, double v);
void setInt(json_t* obj, const char* key, int64_t v);
void setBool(json_t* obj, const char* key, bool v);
/** Steals v. */
void setJson(json_t* obj, const char* key, json_t* v);
/** Steals v. */
void appendJson(json_t* arr, json_t* v);
/** {"hp": hp, "row": row} */
json_t* posJson(int hp, int row);

std::string lowercase(const std::string& s);
std::string trim(const std::string& s);
/** Compact JSON text of a value (jansson flags used for tool results). */
std::string dumpCompact(json_t* j);


// ---- Module / port / param lookup ----------------------------------------------------

struct ModuleRef {
	engine::Module* module = NULL;
	app::ModuleWidget* mw = NULL;
};

/** Finds the engine module and its widget by id. */
bool findModule(int64_t id, ModuleRef* out, std::string* err);
/** Reads the module id from args[key] (required, int or numeric string) and finds it. */
bool moduleArg(json_t* args, const char* key, ModuleRef* out, std::string* err);
/** Model display name, e.g. "VCO". */
std::string moduleName(const ModuleRef& m);
/** "'VCO' (Fundamental VCO, id 123)" */
std::string moduleDescription(const ModuleRef& m);

/** Name of a port, "#N" if the module did not configure it. Index must be valid. */
std::string portName(engine::Module* module, bool input, int idx);
/** Label of a param ("#N" if unnamed). Index must be valid and have a ParamQuantity. */
std::string paramName(engine::Module* module, int idx);
/** Param quantity or NULL (bounds-checked). */
engine::ParamQuantity* paramQuantity(engine::Module* module, int idx);

/** Resolves a port given as integer index or case-insensitive name. On failure fills err
with a message listing the available "id: name" pairs. Checks the index against the module's
port count and that a PortWidget exists. */
bool resolvePort(const ModuleRef& m, bool input, json_t* idJ, int* out, std::string* err);
/** Same for params (index or label). Requires a ParamQuantity. */
bool resolveParam(const ModuleRef& m, json_t* idJ, int* out, std::string* err);

/** "Display" text of a param: value string plus unit. */
std::string displayString(engine::ParamQuantity* pq);


// ---- Grid / placement ----------------------------------------------------------------

struct GridPos {
	int hp = 0;
	int row = 0;
};

math::Vec gridToPx(int hp, int row);
GridPos gridPosOf(app::ModuleWidget* mw);
int widthHp(app::ModuleWidget* mw);

/** Snapshot of the occupied slots in the rack (optionally ignoring one module). */
struct Occupancy {
	struct Entry {
		int hp, row, width;
		math::Rect px;
	};
	std::vector<Entry> entries;
	explicit Occupancy(const app::ModuleWidget* ignore = NULL);
	bool isFree(int hp, int row, int width) const;
	/** First free hp >= hp (rightOnly) or nearest free hp (prefers right on ties). */
	int findFreeHp(int hp, int row, int width, bool rightOnly) const;
	/** Rightmost edge (hp + width) of modules in the row, or `none` if the row is empty. */
	int rowRightEdge(int row, int none) const;
};

/** Parses {"hp": int, "row": int} (row defaults to 0). */
bool parsePosition(json_t* posJ, int* hp, int* row, std::string* err);


// ---- History / cables ----------------------------------------------------------------

/** Mutating tools need ctx.undo. */
bool requireUndo(ToolContext& ctx, std::string* err);
/** Number of complete cables attached to any port of the module. */
int countModuleCables(const ModuleRef& m);
/** Complete cables whose input is the given port widget. */
std::vector<app::CableWidget*> cablesOnInput(app::PortWidget* pw);
/** Removes the module with undo actions (cables, then the module). Returns the number of
removed cables. Deletes the widget. */
int removeModuleWithHistory(app::ModuleWidget* mw, ToolContext& ctx);

/** Short name of a cable end for summaries, e.g. "VCO Sawtooth". */
std::string endLabel(engine::Module* module, bool input, int idx);


} // namespace helpers
} // namespace assistant
} // namespace rack
