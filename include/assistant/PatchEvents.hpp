#pragma once
#include <cstdint>


namespace rack {
namespace assistant {


/** Called by patch::Manager::clear() (File > New, Open, Revert, template, autosave restore).
The patch and the undo history are replaced, so a running agent run must not continue on the new patch. UI thread. */
void notifyPatchCleared();
/** Incremented by every notifyPatchCleared(). */
uint64_t getPatchGeneration();


} // namespace assistant
} // namespace rack
