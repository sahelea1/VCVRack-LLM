#pragma once


namespace rack {
namespace assistant {


/** Called once per frame from Scene::step() (UI thread).
If the environment variable RACK_ASSISTANT_SELFTEST is "tools", "mock" or "all", waits ~30 frames,
runs the selected in-app scenarios synchronously, logs "[assistant selftest] PASS/FAIL <name>",
writes assistant-selftest-result.json to the user directory and closes the window.
Does nothing (and costs one counter check per frame) otherwise.
*/
void sceneStepHook();


} // namespace assistant
} // namespace rack
