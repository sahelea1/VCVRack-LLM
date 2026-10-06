#include <assistant/Panel.hpp>
#include <assistant/Controller.hpp>
#include <assistant/SettingsDialog.hpp>
#include "UiCommon.hpp"

#include <memory>
#include <set>
#include <vector>
#include <functional>
#include <algorithm>

#include <widget/Widget.hpp>
#include <widget/OpaqueWidget.hpp>
#include <ui/ScrollWidget.hpp>
#include <ui/Button.hpp>
#include <ui/TextField.hpp>
#include <app/Scene.hpp>
#include <app/RackWidget.hpp>
#include <window/Window.hpp>
#include <context.hpp>
#include <helpers.hpp>
#include <system.hpp>
#include <color.hpp>
#include <string.hpp>


namespace rack {
namespace assistant {


namespace {


using namespace uic;


// Layout constants
const float PANEL_MIN_WIDTH = 280.f;
const float PANEL_MAX_WIDTH = 1200.f;
const float HEADER_HEIGHT = 32.f;
const float GAP = 6.f;
const float MSG_MARGIN_LEFT = 8.f;
const float MSG_MARGIN_RIGHT = 4.f;
const float MSG_SPACING = 6.f;
/** Indentation of user (left) and assistant (right) bubbles, for a chat look */
const float BUBBLE_INDENT = 18.f;
const float BUBBLE_PAD_X = 8.f;
const float BUBBLE_PAD_Y = 6.f;
const float STATUS_HEIGHT = 18.f;
const float HANDLE_WIDTH = 5.f;

const float FONT_ACTIONS = 11.5f;
const float LINE_ACTIONS = 1.3f;


////////////////////
// Helpers
////////////////////


std::string joinActions(const std::vector<ChatEntry::Action>& actions) {
	std::string s;
	for (size_t i = 0; i < actions.size(); i++) {
		if (i > 0)
			s += " \xc2\xb7 ";
		s += actions[i].text;
	}
	return s;
}


////////////////////
// Message widget
////////////////////


struct MessageWidget : widget::Widget {
	ChatEntry entry;
	bool reasoningOpen = false;
	std::function<void()> onToggleReasoning;
	std::function<void(uint64_t, bool)> onConfirm;

	ui::Button* allowButton = NULL;
	ui::Button* denyButton = NULL;

	// Geometry computed by layout()
	float bx = 0.f;
	float bw = 0.f;
	float textY = 0.f;
	float textW = 0.f;
	float reasonY = 0.f;
	float reasonTextY = 0.f;
	float buttonY = 0.f;

	// ACTIONS rich text
	struct Segment {
		int start;
		int end;
		NVGcolor color;
	};
	std::string joined;
	std::vector<Segment> segments;
	std::vector<RowInfo> rows;

	struct ActionButton : ui::Button {
		std::function<void()> callback;
		void onAction(const ActionEvent& e) override {
			if (callback)
				callback();
		}
	};

	MessageWidget(const ChatEntry& entry) : entry(entry) {
		if (entry.kind == ChatEntry::CONFIRM) {
			ActionButton* a = new ActionButton;
			a->text = "Allow";
			a->box.size = math::Vec(66, BND_WIDGET_HEIGHT);
			a->callback = [this]() {
				if (onConfirm)
					onConfirm(this->entry.id, true);
			};
			addChild(a);
			allowButton = a;
			ActionButton* d = new ActionButton;
			d->text = "Deny";
			d->box.size = math::Vec(66, BND_WIDGET_HEIGHT);
			d->callback = [this]() {
				if (onConfirm)
					onConfirm(this->entry.id, false);
			};
			addChild(d);
			denyButton = d;
		}
	}

	std::string getCopyText() {
		if (entry.kind == ChatEntry::ACTIONS)
			return joinActions(entry.actions);
		return entry.text;
	}

	bool hasReasoning() {
		return entry.kind == ChatEntry::ASSISTANT && !entry.reasoning.empty();
	}

	std::string reasoningLabel() {
		return std::string(reasoningOpen ? "\xe2\x96\xbe" : "\xe2\x96\xb8") + " Reasoning";
	}

	/** Computes geometry and box.size for the given available width. */
	void layout(float W) {
		float h = 0.f;
		switch (entry.kind) {
			case ChatEntry::USER: {
				bx = BUBBLE_INDENT;
				bw = W - BUBBLE_INDENT;
				textW = bw - 2 * BUBBLE_PAD_X;
				textY = BUBBLE_PAD_Y;
				h = textY + measureText(entry.text, FONT_BODY, LINE_BODY, textW) + BUBBLE_PAD_Y;
			} break;
			case ChatEntry::ASSISTANT: {
				bx = 0.f;
				bw = W - BUBBLE_INDENT;
				textW = bw - 2 * BUBBLE_PAD_X;
				float y = BUBBLE_PAD_Y;
				if (hasReasoning()) {
					reasonY = y;
					y += 16.f;
					if (reasoningOpen) {
						y += 2.f;
						reasonTextY = y;
						y += measureText(entry.reasoning, FONT_SMALL, LINE_BODY, textW) + 4.f;
					}
					if (!entry.text.empty())
						y += 4.f;
				}
				textY = y;
				if (!entry.text.empty())
					y += measureText(entry.text, FONT_BODY, LINE_BODY, textW);
				h = y + BUBBLE_PAD_Y;
			} break;
			case ChatEntry::ACTIONS: {
				bx = 0.f;
				bw = W;
				textW = bw - 8.f;
				textY = 2.f;
				buildActionRuns();
				NVGcontext* vg = APP->window->vg;
				nvgSave(vg);
				setFont(vg, FONT_ACTIONS, LINE_ACTIONS);
				breakRows(vg, joined, textW, rows);
				nvgRestore(vg);
				h = textY + std::max<size_t>(rows.size(), 1) * actionsLineStep() + 2.f;
			} break;
			case ChatEntry::ERROR: {
				bx = 0.f;
				bw = W;
				textW = bw - 2 * BUBBLE_PAD_X;
				textY = BUBBLE_PAD_Y;
				h = textY + measureText(entry.text, FONT_BODY, LINE_BODY, textW) + BUBBLE_PAD_Y;
			} break;
			case ChatEntry::INFO: {
				bx = 0.f;
				bw = W;
				textW = bw - 8.f;
				textY = 2.f;
				h = textY + measureText(entry.text, FONT_SMALL, LINE_BODY, textW) + 2.f;
			} break;
			case ChatEntry::CONFIRM: {
				bx = 0.f;
				bw = W;
				textW = bw - 2 * BUBBLE_PAD_X;
				textY = BUBBLE_PAD_Y;
				float y = textY + measureText(entry.text, FONT_BODY, LINE_BODY, textW) + 6.f;
				buttonY = y;
				y += BND_WIDGET_HEIGHT;
				h = y + BUBBLE_PAD_Y;
				bool pending = entry.confirmState == ChatEntry::PENDING;
				allowButton->setVisible(pending);
				denyButton->setVisible(pending);
				allowButton->box.pos = math::Vec(bx + BUBBLE_PAD_X, buttonY);
				denyButton->box.pos = math::Vec(bx + BUBBLE_PAD_X + 66 + 6, buttonY);
			} break;
		}
		box.size = math::Vec(W, h);
	}

	float actionsLineStep() {
		NVGcontext* vg = APP->window->vg;
		nvgSave(vg);
		setFont(vg, FONT_ACTIONS, LINE_ACTIONS);
		float asc, desc, lineh;
		nvgTextMetrics(vg, &asc, &desc, &lineh);
		nvgRestore(vg);
		return lineh * LINE_ACTIONS;
	}

	void buildActionRuns() {
		// Colors are resolved at draw time; only kinds are stored here (encoded in the alpha channel)
		joined.clear();
		segments.clear();
		for (size_t i = 0; i < entry.actions.size(); i++) {
			const ChatEntry::Action& a = entry.actions[i];
			if (i > 0) {
				Segment sep;
				sep.start = joined.size();
				joined += " \xc2\xb7 ";
				sep.end = joined.size();
				sep.color = nvgRGBAf(0, 0, 0, 0);
				segments.push_back(sep);
			}
			Segment s;
			s.start = joined.size();
			joined += a.text;
			s.end = joined.size();
			// kind: r = failed, g = read-only, b = ok
			s.color = a.ok ? (a.readOnly ? nvgRGBAf(0, 1, 0, 1) : nvgRGBAf(0, 0, 1, 1)) : nvgRGBAf(1, 0, 0, 1);
			segments.push_back(s);
		}
	}

	void drawBox(NVGcontext* vg, NVGcolor fill, NVGcolor border) {
		nvgBeginPath(vg);
		nvgRoundedRect(vg, bx + 0.5f, 0.5f, bw - 1.f, box.size.y - 1.f, 4.f);
		nvgFillColor(vg, fill);
		nvgFill(vg);
		if (border.a > 0.f) {
			nvgStrokeWidth(vg, 1.f);
			nvgStrokeColor(vg, border);
			nvgStroke(vg);
		}
	}

	void drawText(NVGcontext* vg, const std::string& s, float x, float y, float w, float size, float lineHeight, NVGcolor color) {
		if (s.empty())
			return;
		setFont(vg, size, lineHeight);
		nvgFillColor(vg, color);
		nvgTextBox(vg, x, y, w, s.c_str(), NULL);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const BNDtheme* theme = bndGetTheme();
		nvgSave(vg);
		switch (entry.kind) {
			case ChatEntry::USER: {
				drawBox(vg, theme->textFieldTheme.innerColor, color::alpha(theme->textFieldTheme.outlineColor, 0.6f));
				drawText(vg, entry.text, bx + BUBBLE_PAD_X, textY, textW, FONT_BODY, LINE_BODY, theme->textFieldTheme.textColor);
			} break;
			case ChatEntry::ASSISTANT: {
				drawBox(vg, theme->menuTheme.innerColor, color::alpha(theme->menuTheme.outlineColor, 0.6f));
				if (hasReasoning()) {
					drawText(vg, reasoningLabel(), bx + BUBBLE_PAD_X, reasonY + 1.f, textW, FONT_SMALL, 1.f, dimColor(0.6f));
					if (reasoningOpen)
						drawText(vg, entry.reasoning, bx + BUBBLE_PAD_X, reasonTextY, textW, FONT_SMALL, LINE_BODY, dimColor(0.6f));
				}
				drawText(vg, entry.text, bx + BUBBLE_PAD_X, textY, textW, FONT_BODY, LINE_BODY, textColor());
			} break;
			case ChatEntry::ACTIONS: {
				drawActions(vg);
			} break;
			case ChatEntry::ERROR: {
				NVGcolor red = errorColor();
				drawBox(vg, color::alpha(red, isLightTheme() ? 0.10f : 0.14f), color::alpha(red, 0.55f));
				drawText(vg, entry.text, bx + BUBBLE_PAD_X, textY, textW, FONT_BODY, LINE_BODY, red);
			} break;
			case ChatEntry::INFO: {
				drawText(vg, entry.text, 4.f, textY, textW, FONT_SMALL, LINE_BODY, dimColor(0.6f));
			} break;
			case ChatEntry::CONFIRM: {
				bool pending = entry.confirmState == ChatEntry::PENDING;
				NVGcolor amber = isLightTheme() ? nvgRGB(0xc0, 0x80, 0x00) : nvgRGB(0xe6, 0xa0, 0x28);
				drawBox(vg, theme->menuTheme.innerColor, color::alpha(amber, pending ? 0.8f : 0.3f));
				drawText(vg, entry.text, bx + BUBBLE_PAD_X, textY, textW, FONT_BODY, LINE_BODY, textColor());
				if (!pending) {
					const char* label = "Cancelled";
					if (entry.confirmState == ChatEntry::ALLOWED)
						label = "Allowed";
					else if (entry.confirmState == ChatEntry::DENIED)
						label = "Declined";
					drawText(vg, label, bx + BUBBLE_PAD_X, buttonY + 3.f, textW, FONT_SMALL, 1.f, dimColor(0.7f));
				}
			} break;
		}
		nvgRestore(vg);

		Widget::draw(args);
	}

	void drawActions(NVGcontext* vg) {
		setFont(vg, FONT_ACTIONS, LINE_ACTIONS);
		float step = actionsLineStep();
		NVGcolor red = errorColor();
		NVGcolor okColor = dimColor(0.7f);
		NVGcolor roColor = dimColor(0.42f);
		NVGcolor sepColor = dimColor(0.35f);
		const char* str = joined.c_str();
		for (size_t i = 0; i < rows.size(); i++) {
			float x = 4.f;
			float y = textY + i * step;
			for (const Segment& seg : segments) {
				int s = std::max(seg.start, rows[i].start);
				int e = std::min(seg.end, rows[i].end);
				if (e <= s)
					continue;
				NVGcolor c = sepColor;
				if (seg.color.a > 0.f)
					c = seg.color.r > 0.5f ? red : (seg.color.g > 0.5f ? roColor : okColor);
				nvgFillColor(vg, c);
				x = nvgText(vg, x, y, str + s, str + e);
			}
		}
	}

	void onButton(const ButtonEvent& e) override {
		Widget::onButton(e);
		if (e.isConsumed())
			return;
		if (e.action != GLFW_PRESS)
			return;

		if (e.button == GLFW_MOUSE_BUTTON_LEFT && hasReasoning()) {
			float lw = measureWidth(reasoningLabel(), FONT_SMALL);
			math::Rect r(bx + BUBBLE_PAD_X - 2.f, reasonY - 1.f, lw + 8.f, 18.f);
			if (r.isContaining(e.pos)) {
				reasoningOpen = !reasoningOpen;
				if (onToggleReasoning)
					onToggleReasoning();
				e.consume(this);
				return;
			}
		}

		if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			std::string copy = getCopyText();
			ui::Menu* menu = createMenu();
			menu->addChild(createMenuItem("Copy text", "", [=]() {
				glfwSetClipboardString(APP->window->win, copy.c_str());
			}));
			e.consume(this);
		}
	}
};


////////////////////
// Input
////////////////////


struct ChatInput : uic::MultilineField {
	std::function<void()> onSend;

	ChatInput() {
		placeholder = "Ask the assistant\xe2\x80\xa6 (Enter to send, Shift+Enter for newline)";
	}

	void onSelectKey(const SelectKeyEvent& e) override {
		if (e.action == GLFW_PRESS || e.action == GLFW_REPEAT) {
			bool enter = e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER;
			int mods = e.mods & RACK_MOD_MASK;
			if (enter) {
				if (mods == 0) {
					if (onSend)
						onSend();
				}
				else {
					// Shift+Enter (and Ctrl/Alt+Enter) inserts a newline
					insertText("\n");
				}
				e.consume(this);
				return;
			}
			if (e.isKeyCommand(GLFW_KEY_ESCAPE)) {
				APP->event->setSelectedWidget(NULL);
				e.consume(this);
				return;
			}
			// Undo/redo shortcuts must not reach the patch while typing.
			// With an empty input they fall through to the Scene, so Ctrl+Z right after a run undoes it.
			if (e.isKeyCommand(GLFW_KEY_Z, RACK_MOD_CTRL) || e.isKeyCommand(GLFW_KEY_Z, RACK_MOD_CTRL | GLFW_MOD_SHIFT)
				|| e.isKeyCommand(GLFW_KEY_Y, RACK_MOD_CTRL)) {
				if (!getText().empty())
					e.consume(this);
				return;
			}
		}
		TextField::onSelectKey(e);
	}
};


////////////////////
// Small widgets
////////////////////


struct CallbackButton : ui::Button {
	std::function<void()> callback;
	/** Draws as a checkbox with this state instead of a push button */
	bool checkbox = false;
	bool checked = false;

	void draw(const DrawArgs& args) override {
		BNDwidgetState state = BND_DEFAULT;
		if (APP->event->getHoveredWidget() == this)
			state = BND_HOVER;
		if (APP->event->getDraggedWidget() == this)
			state = BND_ACTIVE;
		if (checkbox) {
			bndOptionButton(args.vg, 0.0, 0.0, box.size.x, box.size.y, checked ? BND_ACTIVE : state, text.c_str());
			return;
		}
		bndToolButton(args.vg, 0.0, 0.0, box.size.x, box.size.y, BND_CORNER_NONE, state, -1, text.c_str());
	}

	void onAction(const ActionEvent& e) override {
		if (callback)
			callback();
	}
};


struct ResizeEdge : widget::OpaqueWidget {
	/** Called with the total drag distance (positive = wider) since the drag started */
	std::function<void(float)> onResize;
	std::function<void()> onResizeStart;
	std::function<void()> onResizeEnd;
	float dragTotal = 0.f;

	void draw(const DrawArgs& args) override {
		bool hot = APP->event->getHoveredWidget() == this || APP->event->getDraggedWidget() == this;
		if (!hot)
			return;
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0.f, 0.f, 2.f, box.size.y);
		nvgFillColor(args.vg, color::alpha(bndGetTheme()->regularTheme.itemColor, 0.8f));
		nvgFill(args.vg);
	}

	void onDragStart(const DragStartEvent& e) override {
		if (e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		dragTotal = 0.f;
		if (onResizeStart)
			onResizeStart();
		OpaqueWidget::onDragStart(e);
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		// Dragging left makes the panel wider
		dragTotal += -e.mouseDelta.x / getAbsoluteZoom();
		if (onResize)
			onResize(dragTotal);
	}

	void onDragEnd(const DragEndEvent& e) override {
		if (e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		if (onResizeEnd)
			onResizeEnd();
	}
};


////////////////////
// Panel
////////////////////


struct Panel;
Panel* gPanel = NULL;


struct Panel : widget::OpaqueWidget {
	std::unique_ptr<Controller> controller;

	float width = 400.f;
	float areaWidth = 0.f;

	ui::ScrollWidget* scroll;
	widget::Widget* list;
	ChatInput* input;
	CallbackButton* newButton;
	CallbackButton* settingsButton;
	CallbackButton* closeButton;
	CallbackButton* selectionButton;
	CallbackButton* sendButton;
	/** Reverts the last run's changes. Shown in the status row while idle and while the run's changes are still the newest undo step. */
	CallbackButton* undoButton;
	ResizeEdge* resizeEdge;

	// Layout cache
	uint64_t builtRevision = 0;
	bool built = false;
	float builtWidth = -1.f;
	bool dirty = true;
	/** The message list follows new content */
	bool pinned = true;
	bool forcePin = false;
	/** Width when the current resize drag started, and whether one is active */
	float resizeStartWidth = 0.f;
	bool resizing = false;
	double lastRebuildTime = 0.0;
	float contentHeight = 0.f;
	float statusY = 0.f;
	float statusHeight = 0.f;
	std::string statusText;
	/** Entry ids whose reasoning is expanded; survives rebuilds */
	std::set<uint64_t> openReasoning;

	Panel() {
		gPanel = this;
		visible = false;
		controller.reset(new Controller);
		width = math::clamp(controller->getConfig().panelWidth, PANEL_MIN_WIDTH, PANEL_MAX_WIDTH);

		scroll = new ui::ScrollWidget;
		addChild(scroll);
		list = new widget::Widget;
		scroll->container->addChild(list);

		input = new ChatInput;
		input->onSend = [this]() {
			doSend();
		};
		addChild(input);

		newButton = addButton("New", [this]() {
			controller->newChat();
			openReasoning.clear();
			focusInput();
		});
		settingsButton = addButton("Settings", [this]() {
			showSettingsDialog(controller.get());
		});
		closeButton = addButton("\xc3\x97", [this]() {
			setPanelVisible(false);
		});

		selectionButton = addButton("Selection: 0", [this]() {
			Config c = controller->getConfig();
			c.attachSelection = !c.attachSelection;
			controller->setConfig(c);
			focusInput();
		});
		sendButton = addButton("Send", [this]() {
			if (controller->isBusy())
				controller->cancel();
			else
				doSend();
			focusInput();
		});
		sendButton->box.size.x = 80.f;

		undoButton = addButton("Undo changes", [this]() {
			controller->undoLastRun();
			focusInput();
		});
		undoButton->hide();

		resizeEdge = new ResizeEdge;
		resizeEdge->box.size.x = HANDLE_WIDTH;
		resizeEdge->onResizeStart = [this]() {
			resizeStartWidth = width;
			resizing = true;
		};
		resizeEdge->onResize = [this](float total) {
			// Relative to the start, so the edge keeps following the mouse after the width was clamped
			setWidth(resizeStartWidth + total);
		};
		resizeEdge->onResizeEnd = [this]() {
			resizing = false;
			Config c = controller->getConfig();
			if (c.panelWidth != width) {
				c.panelWidth = width;
				controller->setConfig(c);
			}
		};
		addChild(resizeEdge);
	}

	~Panel() {
		if (gPanel == this)
			gPanel = NULL;
	}

	CallbackButton* addButton(const std::string& text, std::function<void()> callback) {
		CallbackButton* b = new CallbackButton;
		b->text = text;
		b->callback = callback;
		addChild(b);
		return b;
	}

	float maxWidth() {
		float hi = PANEL_MAX_WIDTH;
		if (areaWidth > 0.f)
			hi = std::min(hi, areaWidth * 0.7f);
		return std::max(hi, PANEL_MIN_WIDTH);
	}

	void setWidth(float w) {
		width = math::clamp(w, PANEL_MIN_WIDTH, maxWidth());
	}

	void doSend() {
		if (controller->isBusy())
			return;
		std::string text = string::trim(input->text);
		if (text.empty())
			return;
		if (controller->send(text, controller->getConfig().attachSelection)) {
			input->setText("");
			input->scrollY = 0.f;
			forcePin = true;
		}
	}

	float layoutPanel(math::Rect area) {
		if (!visible)
			return 0.f;
		areaWidth = area.size.x;
		setWidth(width);
		box.pos = math::Vec(area.pos.x + area.size.x - width, area.pos.y);
		box.size = math::Vec(width, area.size.y);
		return width;
	}

	void focusInput() {
		APP->event->setSelectedWidget(input);
	}

	void rebuild(float msgWidth) {
		const std::vector<ChatEntry>& entries = controller->getEntries();
		list->clearChildren();
		float y = MSG_SPACING;
		for (const ChatEntry& entry : entries) {
			MessageWidget* m = new MessageWidget(entry);
			m->reasoningOpen = openReasoning.count(entry.id) > 0;
			m->onToggleReasoning = [this, m]() {
				if (m->reasoningOpen)
					openReasoning.insert(m->entry.id);
				else
					openReasoning.erase(m->entry.id);
				dirty = true;
			};
			m->onConfirm = [this](uint64_t id, bool allow) {
				controller->confirm(id, allow);
			};
			m->layout(msgWidth);
			m->box.pos = math::Vec(MSG_MARGIN_LEFT, y);
			y += m->box.size.y + MSG_SPACING;
			list->addChild(m);
		}
		contentHeight = y;
		list->box.pos = math::Vec(0.f, 0.f);
		list->box.size = math::Vec(scroll->box.size.x, contentHeight);
	}

	bool hasPendingConfirm() {
		const std::vector<ChatEntry>& es = controller->getEntries();
		for (size_t i = es.size(); i > 0; i--) {
			if (es[i - 1].kind == ChatEntry::CONFIRM)
				return es[i - 1].confirmState == ChatEntry::PENDING;
		}
		return false;
	}

	void step() override {
		// The controller must advance even while the panel is hidden
		controller->step();

		// A run that waits for an answer must not stall behind a hidden panel: show it again, without taking keyboard focus
		if (!visible && controller->getState() == Controller::WAITING_CONFIRM && hasPendingConfirm()) {
			show();
			forcePin = true;
			// Laid out by the Scene on the next frame
			return;
		}

		if (!visible)
			return;

		const float W = box.size.x;
		const float H = box.size.y;
		const Config& config = controller->getConfig();

		// Was the list scrolled to the bottom before anything changes?
		math::Rect bound = scroll->getContainerOffsetBound();
		bool atBottom = bound.size.y <= 0.f || scroll->offset.y >= bound.pos.y + bound.size.y - 2.f;
		pinned = atBottom || forcePin;
		forcePin = false;

		// Header. The window does not exist yet while the Scene is constructed, so button widths are computed here.
		newButton->box.size.x = std::ceil(measureWidth(newButton->text, FONT_BODY)) + 18.f;
		settingsButton->box.size.x = std::ceil(measureWidth(settingsButton->text, FONT_BODY)) + 18.f;
		closeButton->box.size.x = BND_WIDGET_HEIGHT;
		float bx = W - GAP;
		CallbackButton* headerButtons[3] = {closeButton, settingsButton, newButton};
		for (CallbackButton* b : headerButtons) {
			bx -= b->box.size.x;
			b->box.pos = math::Vec(bx, (HEADER_HEIGHT - BND_WIDGET_HEIGHT) / 2.f);
			bx -= 4.f;
		}

		// Bottom up
		float y = H - GAP;
		y -= BND_WIDGET_HEIGHT;
		float bottomRowY = y;
		selectionButton->box.pos = math::Vec(GAP, bottomRowY);
		size_t selected = APP->scene->rack->getSelected().size();
		selectionButton->text = string::f("Selection: %d", (int) selected);
		selectionButton->checkbox = true;
		selectionButton->box.size.x = std::ceil(measureWidth(selectionButton->text, FONT_BODY)) + 30.f;
		selectionButton->checked = config.attachSelection;
		sendButton->box.pos = math::Vec(W - GAP - sendButton->box.size.x, bottomRowY);
		sendButton->text = controller->isBusy() ? "Cancel" : "Send";

		y -= GAP;
		input->box.size.x = W - 2 * GAP;
		input->box.size.y = input->desiredHeight(input->box.size.x);
		y -= input->box.size.y;
		input->box.pos = math::Vec(GAP, y);
		input->updateScroll();

		statusText = controller->getStatusText();
		bool showUndo = !controller->isBusy() && controller->canUndoLastRun();
		undoButton->setVisible(showUndo);
		statusHeight = (statusText.empty() && !controller->isBusy() && !showUndo) ? 0.f : STATUS_HEIGHT;
		if (showUndo)
			statusHeight = BND_WIDGET_HEIGHT;
		y -= statusHeight;
		statusY = y;
		undoButton->box.size.x = std::ceil(measureWidth(undoButton->text, FONT_BODY)) + 18.f;
		undoButton->box.pos = math::Vec(W - GAP - undoButton->box.size.x, statusY);
		if (statusHeight > 0.f)
			y -= 2.f;
		else
			y -= GAP - 2.f;

		scroll->box.pos = math::Vec(0.f, HEADER_HEIGHT);
		scroll->box.size = math::Vec(W, std::max(y - HEADER_HEIGHT, 20.f));

		resizeEdge->box.pos = math::Vec(0.f, 0.f);
		resizeEdge->box.size = math::Vec(HANDLE_WIDTH, H);

		// Rebuild the messages on revision or width change only
		float msgWidth = std::max(W - MSG_MARGIN_LEFT - MSG_MARGIN_RIGHT - BND_SCROLLBAR_WIDTH, 100.f);
		uint64_t revision = controller->getRevision();
		bool widthChanged = msgWidth != builtWidth;
		// While dragging the edge, re-measuring the whole transcript every frame stutters: rebuild at most every 80 ms (and once more when the drag ends)
		bool throttled = resizing && widthChanged && !dirty && revision == builtRevision && built && (system::getTime() - lastRebuildTime) < 0.08;
		if (!throttled && (dirty || !built || revision != builtRevision || widthChanged)) {
			rebuild(msgWidth);
			built = true;
			dirty = false;
			builtRevision = revision;
			builtWidth = msgWidth;
			lastRebuildTime = system::getTime();
		}
		list->box.size.x = scroll->box.size.x;

		if (pinned)
			scroll->offset.y = std::max(0.f, contentHeight - scroll->box.size.y);

		OpaqueWidget::step();
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const BNDtheme* theme = bndGetTheme();

		nvgSave(vg);
		// Let the shadow fall to the left only
		nvgScissor(vg, -14.f, 0.f, box.size.x + 14.f, box.size.y);
		bndMenuBackground(vg, 0.0, 0.0, box.size.x, box.size.y, BND_CORNER_NONE);
		nvgRestore(vg);

		// Left border
		nvgBeginPath(vg);
		nvgRect(vg, 0.f, 0.f, 1.f, box.size.y);
		nvgFillColor(vg, theme->menuTheme.outlineColor);
		nvgFill(vg);

		// Header: title and model name
		nvgSave(vg);
		setFont(vg, 14.f, 1.f);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, textColor());
		float titleW = nvgText(vg, 12.f, HEADER_HEIGHT / 2.f, "Assistant", NULL) - 12.f;
		std::string model = controller->getConfig().model;
		if (controller->getConfig().mock)
			model = "mock";
		setFont(vg, FONT_SMALL, 1.f);
		nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		float modelX = 12.f + titleW + 8.f;
		float modelMax = newButton->box.pos.x - 6.f - modelX;
		if (modelMax > 20.f) {
			nvgFillColor(vg, dimColor(0.5f));
			nvgText(vg, modelX, HEADER_HEIGHT / 2.f + 0.5f, fitText(vg, model, modelMax).c_str(), NULL);
		}
		nvgRestore(vg);

		// Status line
		if (statusHeight > 0.f) {
			nvgSave(vg);
			float cy = statusY + statusHeight / 2.f;
			float tx = GAP + 2.f;
			if (controller->isBusy()) {
				// Three pulsing dots
				double t = system::getTime();
				for (int i = 0; i < 3; i++) {
					float phase = std::fmod((float) (t * 1.6) - i * 0.18f, 1.f);
					if (phase < 0.f)
						phase += 1.f;
					float a = 0.25f + 0.65f * (0.5f + 0.5f * std::sin(phase * 2.f * M_PI));
					nvgBeginPath(vg);
					nvgCircle(vg, GAP + 5.f + i * 8.f, cy, 2.2f);
					nvgFillColor(vg, color::alpha(textColor(), a));
					nvgFill(vg);
				}
				tx = GAP + 5.f + 3 * 8.f + 2.f;
			}
			setFont(vg, FONT_SMALL, 1.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, dimColor(0.65f));
			nvgText(vg, tx, cy, fitText(vg, statusText, box.size.x - tx - GAP).c_str(), NULL);
			nvgRestore(vg);
		}

		Widget::draw(args);
	}
};


Panel* livePanel() {
	return gPanel;
}


} // namespace


widget::Widget* createPanel() {
	return new Panel;
}

Controller* getController() {
	return gPanel ? gPanel->controller.get() : NULL;
}

bool isPanelVisible() {
	return gPanel && gPanel->isVisible();
}

void setPanelVisible(bool visible) {
	Panel* p = livePanel();
	if (!p)
		return;
	if (visible) {
		p->show();
		p->forcePin = true;
		p->focusInput();
	}
	else {
		// Drop keyboard focus from the hidden panel
		if (APP->event->selectedWidget && APP->event->selectedWidget->isDescendantOf(p))
			APP->event->setSelectedWidget(NULL);
		p->hide();
	}
}

void togglePanel() {
	setPanelVisible(!isPanelVisible());
}

void newChat() {
	Panel* p = livePanel();
	if (!p)
		return;
	p->controller->newChat();
	p->openReasoning.clear();
}

void openSettings() {
	Panel* p = livePanel();
	if (!p)
		return;
	showSettingsDialog(p->controller.get());
}

float layoutPanel(math::Rect area) {
	Panel* p = livePanel();
	if (!p)
		return 0.f;
	return p->layoutPanel(area);
}


} // namespace assistant
} // namespace rack
