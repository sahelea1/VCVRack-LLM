#pragma once


namespace rack {
namespace assistant {


struct Controller;


/** Opens the modal assistant settings dialog on top of the scene. The dialog edits `controller`'s config and saves it with Controller::setConfig(). Does nothing if a dialog is already open. */
void showSettingsDialog(Controller* controller);


} // namespace assistant
} // namespace rack
