#pragma once
#include <math.hpp>
#include <widget/Widget.hpp>


namespace rack {
namespace assistant {


struct Controller;


/** Creates the docked assistant chat panel, initially hidden. Called once by the Scene constructor, which adds it as a child. */
widget::Widget* createPanel();
/** The controller of the live panel, or NULL if no panel exists. */
Controller* getController();
bool isPanelVisible();
/** Shows or hides the panel. Showing focuses the input field. */
void setPanelVisible(bool visible);
void togglePanel();
/** Starts a new chat (cancels a running request). */
void newChat();
/** Opens the modal settings dialog. */
void openSettings();
/** Called by Scene::step. If the panel is visible, places it at the right side of `area` (in Scene coordinates) and returns the width it takes. Returns 0 if hidden. */
float layoutPanel(math::Rect area);


} // namespace assistant
} // namespace rack
