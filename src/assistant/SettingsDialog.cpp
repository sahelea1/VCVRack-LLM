#include <assistant/SettingsDialog.hpp>
#include <assistant/Controller.hpp>
#include "UiCommon.hpp"

#include <functional>
#include <string>
#include <vector>
#include <utility>
#include <cstdlib>
#include <cerrno>

#include <widget/Widget.hpp>
#include <widget/OpaqueWidget.hpp>
#include <app/Scene.hpp>
#include <ui/MenuOverlay.hpp>
#include <ui/ScrollWidget.hpp>
#include <ui/Button.hpp>
#include <ui/ChoiceButton.hpp>
#include <ui/TextField.hpp>
#include <window/Window.hpp>
#include <context.hpp>
#include <helpers.hpp>
#include <system.hpp>
#include <string.hpp>


namespace rack {
namespace assistant {


namespace {


using namespace uic;


const float DIALOG_WIDTH = 560.f;
const float DIALOG_PAD = 16.f;
const float TITLE_HEIGHT = 38.f;
const float LABEL_WIDTH = 140.f;
const float COLUMN_GAP = 8.f;
const float ROW_GAP = 8.f;
const float SCROLLBAR_SPACE = 14.f;
const float FONT_HINT = 11.5f;

const char* const EFFORTS[] = {"off", "none", "minimal", "low", "medium", "high"};
const char* const STYLES[] = {"openrouter", "openai"};


/** Plain text label, optionally word-wrapped. An alpha-0 color means the theme text color. */
struct FormLabel : widget::Widget {
	std::string text;
	float fontSize = FONT_BODY;
	NVGcolor color = nvgRGBA(0, 0, 0, 0);
	bool wrap = false;

	NVGcolor getColor() {
		if (color.a > 0.f)
			return color;
		return textColor();
	}

	/** Sets the width and, if wrapping, the height that fits the text. */
	void fit(float width) {
		box.size.x = width;
		if (wrap)
			box.size.y = measureText(text, fontSize, LINE_BODY, width);
	}

	void draw(const DrawArgs& args) override {
		if (text.empty())
			return;
		NVGcontext* vg = args.vg;
		nvgSave(vg);
		setFont(vg, fontSize, LINE_BODY);
		nvgFillColor(vg, getColor());
		if (wrap) {
			nvgTextBox(vg, 0.f, 0.f, box.size.x, text.c_str(), NULL);
		}
		else {
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgText(vg, 0.f, box.size.y / 2.f, text.c_str(), NULL);
		}
		nvgRestore(vg);
	}
};


/** Wraps a widget of the modal dialog so that no key can reach the Scene behind it.
Scene::onHoverKey runs its shortcuts (undo, save, open, new, quit, delete selection, ...) for every key that the focused widget does not consume, even behind a modal overlay.
Therefore a widget that can have keyboard focus consumes every key press and repeat it does not handle itself. Escape closes the dialog. */
template <class T>
struct Guarded : T {
	/** Returns false if Escape is not for the dialog (a menu is open above it) */
	std::function<bool()> onEscape;

	void onSelectKey(const widget::Widget::SelectKeyEvent& e) override {
		bool down = e.action == GLFW_PRESS || e.action == GLFW_REPEAT;
		if (down && e.isKeyCommand(GLFW_KEY_ESCAPE)) {
			if (!onEscape || onEscape()) {
				e.consume(this);
				return;
			}
			// A menu above the dialog gets the key
			return;
		}
		T::onSelectKey(e);
		if (down && !e.isConsumed())
			e.consume(this);
	}
};


struct CallbackButtonBase : ui::Button {
	std::function<void()> callback;
	void onAction(const ActionEvent& e) override {
		if (callback)
			callback();
	}
};

typedef Guarded<CallbackButtonBase> CallbackButton;


struct CheckBoxBase : ui::Button {
	bool checked = false;

	void draw(const DrawArgs& args) override {
		BNDwidgetState state = BND_DEFAULT;
		if (checked)
			state = BND_ACTIVE;
		else if (APP->event->getHoveredWidget() == this)
			state = BND_HOVER;
		bndOptionButton(args.vg, 0.0, 0.0, box.size.x, box.size.y, state, text.c_str());
	}

	void onAction(const ActionEvent& e) override {
		checked = !checked;
	}
};

typedef Guarded<CheckBoxBase> CheckBox;


/** A choice among fixed strings, shown as a dropdown. */
struct ChoiceFieldBase : ui::ChoiceButton {
	std::vector<std::string> options;
	std::string value;

	void step() override {
		text = value;
		ui::ChoiceButton::step();
	}

	void onAction(const ActionEvent& e) override {
		ui::Menu* menu = createMenu();
		menu->box.pos = getAbsoluteOffset(math::Vec(0.f, box.size.y));
		menu->box.size.x = box.size.x;
		WeakPtr<ChoiceFieldBase> self = this;
		for (const std::string& option : options) {
			menu->addChild(createCheckMenuItem(option, "",
				[=]() {return self && self->value == option;},
				[=]() {
					if (self)
						self->value = option;
				}
			));
		}
	}
};

typedef Guarded<ChoiceFieldBase> ChoiceField;


/** Single-line field for the API key. Never allows copying the key out. */
struct KeyFieldBase : ui::PasswordField {
	void onSelectKey(const SelectKeyEvent& e) override {
		if (e.action == GLFW_PRESS || e.action == GLFW_REPEAT) {
			if (e.isKeyCommand(GLFW_KEY_C, RACK_MOD_CTRL) || e.isKeyCommand(GLFW_KEY_X, RACK_MOD_CTRL)) {
				e.consume(this);
				return;
			}
		}
		ui::PasswordField::onSelectKey(e);
	}

	void onButton(const ButtonEvent& e) override {
		// No context menu, which would offer Copy
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			e.consume(this);
			return;
		}
		ui::PasswordField::onButton(e);
	}
};


typedef Guarded<ui::TextField> DialogField;
typedef Guarded<uic::MultilineField> DialogMultilineField;
typedef Guarded<KeyFieldBase> KeyField;


bool parseInt(const std::string& s, long* out) {
	if (s.empty())
		return false;
	errno = 0;
	char* end = NULL;
	long v = strtol(s.c_str(), &end, 10);
	if (errno != 0 || !end || *end != '\0')
		return false;
	*out = v;
	return true;
}

bool parseDouble(const std::string& s, double* out) {
	if (s.empty())
		return false;
	errno = 0;
	char* end = NULL;
	double v = strtod(s.c_str(), &end);
	if (errno != 0 || !end || *end != '\0' || !std::isfinite(v))
		return false;
	*out = v;
	return true;
}


struct SettingsOverlay;
SettingsOverlay* gOverlay = NULL;


struct Dialog : widget::OpaqueWidget {
	Controller* controller;
	Config original;

	// Pending edits that are not stored in a field
	bool clearStoredKey = false;

	ui::ScrollWidget* scroll;
	widget::Widget* form;
	FormLabel* title;
	FormLabel* errorLabel;
	CallbackButton* saveButton;
	CallbackButton* cancelButton;
	CallbackButton* clearKeyButton;
	FormLabel* keyInfo;

	ui::TextField* baseUrl;
	ui::TextField* model;
	ChoiceField* effort;
	ChoiceField* style;
	ui::TextField* maxTokens;
	ui::TextField* temperature;
	KeyField* apiKey;
	uic::MultilineField* headers;
	ui::TextField* maxRounds;
	ui::TextField* timeout;
	ui::TextField* caBundle;
	CheckBox* confirmDestructive;
	CheckBox* mock;

	struct Row {
		FormLabel* label;
		widget::Widget* control;
	};
	std::vector<Row> rows;

	std::function<void()> onClose;

	Dialog(Controller* controller) : controller(controller) {
		original = controller->getConfig();
		const Config& c = original;

		title = new FormLabel;
		title->text = "Assistant settings";
		title->fontSize = 15.f;
		addChild(title);

		scroll = new ui::ScrollWidget;
		scroll->hideScrollbars = false;
		addChild(scroll);
		form = new widget::Widget;
		scroll->container->addChild(form);

		baseUrl = addText("Base URL", c.baseUrl);
		model = addText("Model", c.model);

		effort = guarded(new ChoiceField);
		effort->options.assign(EFFORTS, EFFORTS + sizeof(EFFORTS) / sizeof(EFFORTS[0]));
		effort->value = c.reasoningEffort;
		addRow("Reasoning effort", effort);

		style = guarded(new ChoiceField);
		style->options.assign(STYLES, STYLES + sizeof(STYLES) / sizeof(STYLES[0]));
		style->value = c.reasoningParamStyle;
		addRow("Parameter style", style);

		maxTokens = addText("Max tokens", c.maxTokens > 0 ? string::f("%d", c.maxTokens) : "");
		maxTokens->placeholder = "unset";
		temperature = addText("Temperature", c.hasTemperature ? string::f("%g", temperatureToDouble(c.temperature)) : "");
		temperature->placeholder = "unset";

		apiKey = guarded(new KeyField);
		apiKey->placeholder = "blank keeps the stored key";
		addRow("API key", apiKey);

		keyInfo = new FormLabel;
		keyInfo->fontSize = FONT_HINT;
		keyInfo->color = dimColor(0.7f);
		keyInfo->wrap = true;
		form->addChild(keyInfo);

		clearKeyButton = guarded(new CallbackButton);
		clearKeyButton->text = "Clear stored key";
		clearKeyButton->box.size.x = std::ceil(measureWidth(clearKeyButton->text, FONT_BODY)) + 18.f;
		clearKeyButton->callback = [this]() {
			clearStoredKey = true;
		};
		form->addChild(clearKeyButton);

		headers = guarded(new DialogMultilineField);
		headers->minLines = 3;
		headers->maxLines = 6;
		headers->placeholder = "Name: value (one per line)";
		std::string h;
		for (const std::pair<std::string, std::string>& kv : c.extraHeaders)
			h += kv.first + ": " + kv.second + "\n";
		if (!h.empty())
			h.pop_back();
		headers->text = h;
		headers->cursor = headers->selection = 0;
		addRow("Extra headers", headers);

		maxRounds = addText("Max tool rounds", string::f("%d", c.maxToolRounds));
		timeout = addText("Timeout (s)", string::f("%g", c.timeoutSec));
		caBundle = addText("CA bundle path", c.caBundle);
		caBundle->placeholder = "optional";

		confirmDestructive = guarded(new CheckBox);
		confirmDestructive->text = "Confirm destructive actions";
		confirmDestructive->checked = c.confirmDestructive;
		addRow("", confirmDestructive);

		mock = guarded(new CheckBox);
		mock->text = "Mock mode (no network)";
		mock->checked = c.mock;
		addRow("", mock);

		// Tab order
		std::vector<ui::TextField*> fields = {baseUrl, model, maxTokens, temperature, apiKey, headers, maxRounds, timeout, caBundle};
		for (size_t i = 0; i < fields.size(); i++) {
			fields[i]->nextField = fields[(i + 1) % fields.size()];
			fields[i]->prevField = fields[(i + fields.size() - 1) % fields.size()];
		}

		errorLabel = new FormLabel;
		errorLabel->fontSize = FONT_BODY;
		errorLabel->color = errorColor();
		errorLabel->wrap = true;
		addChild(errorLabel);

		saveButton = guarded(new CallbackButton);
		saveButton->text = "Save";
		saveButton->box.size.x = 90.f;
		saveButton->callback = [this]() {
			save();
		};
		addChild(saveButton);

		cancelButton = guarded(new CallbackButton);
		cancelButton->text = "Cancel";
		cancelButton->box.size.x = 90.f;
		cancelButton->callback = [this]() {
			close();
		};
		addChild(cancelButton);
	}

	~Dialog() {
		// Do not leave the typed key in memory longer than needed
		if (apiKey)
			apiKey->text.assign(apiKey->text.size(), '\0');
	}

	template <class W>
	W* guarded(W* w) {
		w->onEscape = [this]() {
			return escape();
		};
		return w;
	}

	ui::TextField* addText(const std::string& label, const std::string& value) {
		ui::TextField* f = guarded(new DialogField);
		f->text = value;
		f->cursor = f->selection = 0;
		addRow(label, f);
		return f;
	}

	void addRow(const std::string& label, widget::Widget* control) {
		FormLabel* l = new FormLabel;
		l->text = label;
		form->addChild(l);
		form->addChild(control);
		Row r;
		r.label = l;
		r.control = control;
		rows.push_back(r);
	}

	void close() {
		if (onClose)
			onClose();
	}

	/** True if no other overlay (e.g. a dropdown menu) is above the dialog */
	bool isTopmost() {
		return parent && APP->scene && !APP->scene->children.empty() && APP->scene->children.back() == parent;
	}

	/** Escape: closes the dialog unless a menu above it should get the key first. Returns true if the key was handled. */
	bool escape() {
		if (!isTopmost())
			return false;
		close();
		return true;
	}

	std::string getKeyInfo() {
		Config c = original;
		if (clearStoredKey)
			c.apiKey.clear();
		ResolvedKey rk = resolveApiKey(c);
		std::string s;
		if (rk.source.empty()) {
			s = "No API key set.";
		}
		else if (rk.source == "assistant.json") {
			s = "Stored key: " + maskKey(rk.key);
		}
		else {
			s = "Using env " + rk.source + " (" + maskKey(rk.key) + ").";
			if (!c.apiKey.empty())
				s += " It overrides the stored key.";
		}
		if (clearStoredKey && !original.apiKey.empty())
			s += " The stored key will be removed on Save.";
		if (!string::trim(apiKey->text).empty())
			s += " The new key will be stored on Save.";
		return s;
	}

	void setError(const std::string& s) {
		errorLabel->text = s;
	}

	/** Validates the fields. Returns false and sets the error text if invalid. */
	bool collect(Config& c) {
		c = original;

		std::string url = string::trim(baseUrl->text);
		if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
			setError("Base URL must start with http:// or https://.");
			return false;
		}
		c.baseUrl = url;

		std::string m = string::trim(model->text);
		if (m.empty()) {
			setError("Model must not be empty.");
			return false;
		}
		c.model = m;

		if (!isValidReasoningEffort(effort->value)) {
			setError("Invalid reasoning effort.");
			return false;
		}
		c.reasoningEffort = effort->value;
		if (!isValidReasoningParamStyle(style->value)) {
			setError("Invalid parameter style.");
			return false;
		}
		c.reasoningParamStyle = style->value;

		std::string s = string::trim(maxTokens->text);
		if (s.empty()) {
			c.maxTokens = 0;
		}
		else {
			long v;
			if (!parseInt(s, &v) || v < 1 || v > 10000000) {
				setError("Max tokens must be a whole number of at least 1, or blank.");
				return false;
			}
			c.maxTokens = (int) v;
		}

		s = string::trim(temperature->text);
		if (s.empty()) {
			c.hasTemperature = false;
		}
		else {
			double v;
			if (!parseDouble(s, &v) || v < 0.0 || v > 2.0) {
				setError("Temperature must be a number between 0 and 2, or blank.");
				return false;
			}
			c.hasTemperature = true;
			c.temperature = (float) v;
		}

		// Headers
		c.extraHeaders.clear();
		std::vector<std::string> lines = string::split(headers->text, "\n");
		for (size_t i = 0; i < lines.size(); i++) {
			std::string line = string::trim(lines[i]);
			if (line.empty())
				continue;
			size_t colon = line.find(':');
			std::string name = colon == std::string::npos ? "" : string::trim(line.substr(0, colon));
			std::string value = colon == std::string::npos ? "" : string::trim(line.substr(colon + 1));
			bool valid = !name.empty();
			for (char ch : name) {
				if (ch == ' ' || ch == '\t' || ch == '\r')
					valid = false;
			}
			if (!valid) {
				setError(string::f("Extra headers, line %d: expected \"Name: value\".", (int) i + 1));
				return false;
			}
			c.extraHeaders.push_back(std::make_pair(name, value));
		}

		long rounds;
		if (!parseInt(string::trim(maxRounds->text), &rounds) || rounds < 1 || rounds > 100) {
			setError("Max tool rounds must be a whole number between 1 and 100.");
			return false;
		}
		c.maxToolRounds = (int) rounds;

		double to;
		if (!parseDouble(string::trim(timeout->text), &to) || to < 10.0 || to > 1800.0) {
			setError("Timeout must be a number of seconds between 10 and 1800.");
			return false;
		}
		c.timeoutSec = to;

		std::string ca = string::trim(caBundle->text);
		if (!ca.empty() && !system::isFile(ca)) {
			setError("CA bundle file not found.");
			return false;
		}
		c.caBundle = ca;

		c.confirmDestructive = confirmDestructive->checked;
		c.mock = mock->checked;

		// Key: blank keeps the stored one
		std::string key = string::trim(apiKey->text);
		if (!key.empty())
			c.apiKey = key;
		else if (clearStoredKey)
			c.apiKey.clear();
		return true;
	}

	void save() {
		Config c;
		if (!collect(c))
			return;
		std::string error;
		// Re-read the config so that changes made while the dialog was open (panel width, selection toggle) are kept
		c.panelWidth = controller->getConfig().panelWidth;
		c.attachSelection = controller->getConfig().attachSelection;
		c.maxContextChars = controller->getConfig().maxContextChars;
		if (!controller->setConfig(c, &error)) {
			setError("Settings applied, but saving assistant.json failed: " + error);
			return;
		}
		close();
	}

	/** Positions all rows. Returns the height of the form. */
	float layoutForm(float width) {
		const float fieldX = LABEL_WIDTH + COLUMN_GAP;
		const float fieldW = width - fieldX;
		float y = 0.f;
		for (Row& row : rows) {
			float h = BND_WIDGET_HEIGHT;
			if (row.control == headers) {
				headers->box.size.x = fieldW;
				h = headers->desiredHeight(fieldW);
				headers->updateScroll();
			}
			row.label->box.pos = math::Vec(0.f, y);
			row.label->box.size = math::Vec(LABEL_WIDTH, BND_WIDGET_HEIGHT);
			row.control->box.pos = math::Vec(fieldX, y);
			row.control->box.size.x = fieldW;
			row.control->box.size.y = h;
			y += h + ROW_GAP;

			if (row.control == apiKey) {
				keyInfo->text = getKeyInfo();
				keyInfo->box.pos = math::Vec(fieldX, y - 2.f);
				keyInfo->fit(fieldW);
				y += keyInfo->box.size.y + 4.f;
				bool showClear = !original.apiKey.empty() && !clearStoredKey;
				clearKeyButton->setVisible(showClear);
				if (showClear) {
					clearKeyButton->box.pos = math::Vec(fieldX, y);
					y += BND_WIDGET_HEIGHT + ROW_GAP;
				}
			}
		}
		return std::max(y - ROW_GAP, 0.f);
	}

	void layoutDialog(math::Vec available) {
		float formW = DIALOG_WIDTH - 2 * DIALOG_PAD - SCROLLBAR_SPACE;
		float formH = layoutForm(formW);
		form->box.pos = math::Vec(0.f, 0.f);
		form->box.size = math::Vec(formW, formH);

		float innerW = DIALOG_WIDTH - 2 * DIALOG_PAD;
		errorLabel->fit(innerW);
		float errorH = errorLabel->text.empty() ? 0.f : errorLabel->box.size.y + 8.f;
		float footerH = 10.f + errorH + BND_WIDGET_HEIGHT + DIALOG_PAD;

		float maxScrollH = std::max(available.y - 40.f - TITLE_HEIGHT - footerH, 120.f);
		float scrollH = std::min(formH + 4.f, maxScrollH);

		box.size = math::Vec(DIALOG_WIDTH, TITLE_HEIGHT + scrollH + footerH);
		title->box.pos = math::Vec(DIALOG_PAD, 0.f);
		title->box.size = math::Vec(innerW, TITLE_HEIGHT);
		scroll->box.pos = math::Vec(DIALOG_PAD, TITLE_HEIGHT);
		scroll->box.size = math::Vec(innerW, scrollH);

		float y = TITLE_HEIGHT + scrollH + 10.f;
		errorLabel->box.pos = math::Vec(DIALOG_PAD, y);
		y += errorH;
		saveButton->box.pos = math::Vec(DIALOG_WIDTH - DIALOG_PAD - saveButton->box.size.x, y);
		cancelButton->box.pos = math::Vec(saveButton->box.pos.x - 8.f - cancelButton->box.size.x, y);
	}

	/** True if `w` is a widget of the dialog that consumes all key presses */
	bool isKeyTarget(widget::Widget* w) {
		if (w == this)
			return true;
		ui::TextField* fields[] = {baseUrl, model, maxTokens, temperature, apiKey, headers, maxRounds, timeout, caBundle};
		for (ui::TextField* f : fields) {
			if (w == f)
				return true;
		}
		return false;
	}

	/** Keys must not reach the Scene while the dialog is open (e.g. Backspace would delete selected modules). When no field has focus, the dialog itself is the selected widget and swallows them. */
	void onSelectKey(const SelectKeyEvent& e) override {
		if (e.action == GLFW_PRESS || e.action == GLFW_REPEAT) {
			if (e.isKeyCommand(GLFW_KEY_ESCAPE)) {
				// A menu above the dialog gets the key
				if (e.action == GLFW_PRESS && !escape())
					return;
			}
			else if (e.isKeyCommand(GLFW_KEY_TAB)) {
				APP->event->setSelectedWidget(baseUrl);
			}
			else if (e.isKeyCommand(GLFW_KEY_TAB, GLFW_MOD_SHIFT)) {
				APP->event->setSelectedWidget(caBundle);
			}
		}
		e.consume(this);
	}

	void onSelectText(const SelectTextEvent& e) override {
		e.consume(this);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		bndMenuBackground(vg, 0.0, 0.0, box.size.x, box.size.y, BND_CORNER_ALL);
		bndBevel(vg, 0.0, 0.0, box.size.x, box.size.y);
		Widget::draw(args);
	}
};


struct SettingsOverlay : ui::MenuOverlay {
	Dialog* dialog;

	SettingsOverlay(Controller* controller) {
		bgColor = nvgRGBAf(0.f, 0.f, 0.f, 0.4f);
		dialog = new Dialog(controller);
		dialog->onClose = [this]() {
			requestDelete();
		};
		addChild(dialog);
	}

	~SettingsOverlay() {
		if (gOverlay == this)
			gOverlay = NULL;
	}

	void step() override {
		MenuOverlay::step();
		// Keep keyboard focus on a widget that swallows keys: a text field of the dialog or the dialog itself.
		// Clicking a button, a scroll bar or the dark backdrop selects that widget, which would let keys reach the Scene.
		widget::Widget* sel = APP->event->selectedWidget;
		if (!sel || (!dialog->isKeyTarget(sel) && dialog->isTopmost()))
			APP->event->setSelectedWidget(dialog);
		dialog->layoutDialog(box.size);
		dialog->box.pos = box.size.minus(dialog->box.size).div(2.f).round();
	}

	/** Clicking outside the dialog does nothing, so a misclick cannot discard the edits (e.g. a pasted API key). Close with Save, Cancel or Esc. */
	void onButton(const ButtonEvent& e) override {
		widget::OpaqueWidget::onButton(e);
		// Keep the target of a child that consumed the click (a field must become the selected widget)
		if (e.isConsumed() && e.getTarget() != this)
			return;
		e.consume(this);
	}

	/** Esc when no field has focus */
	void onAction(const ActionEvent& e) override {
		requestDelete();
	}
};


} // namespace


void showSettingsDialog(Controller* controller) {
	if (!controller || gOverlay)
		return;
	SettingsOverlay* overlay = new SettingsOverlay(controller);
	gOverlay = overlay;
	APP->scene->addChild(overlay);
	APP->event->setSelectedWidget(overlay->dialog->baseUrl);
}


} // namespace assistant
} // namespace rack
