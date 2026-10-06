#pragma once
#include <string>


namespace rack {
namespace assistant {


/** Built-in default system prompt. */
std::string defaultSystemPrompt();
/** asset::user("assistant-system-prompt.md") */
std::string systemPromptPath();
/** Reads the user-editable prompt file. Creates it with the default prompt if missing. Falls back to the default prompt on any error or if the file is empty. */
std::string loadSystemPrompt();
/** Runtime context appended to the system prompt: Rack version and installed plugins with model counts. UI thread only. */
std::string environmentBlock();


} // namespace assistant
} // namespace rack
