#pragma once
// Private UI helpers shared by Panel.cpp and SettingsDialog.cpp. Not part of the public API.
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

#include <ui/TextField.hpp>
#include <window/Window.hpp>
#include <context.hpp>
#include <color.hpp>
#include <string.hpp>


namespace rack {
namespace assistant {
namespace uic {


const float FONT_BODY = 13.f;
const float LINE_BODY = 1.25f;
const float FONT_SMALL = 12.f;


inline bool isLightTheme() {
	return bndGetTheme()->backgroundColor.r > 0.5f;
}

inline NVGcolor textColor() {
	return bndGetTheme()->regularTheme.textColor;
}

inline NVGcolor dimColor(float a = 0.55f) {
	return color::alpha(textColor(), a);
}

/** Red tone readable on the current theme background */
inline NVGcolor errorColor() {
	return isLightTheme() ? nvgRGB(0xb0, 0x1c, 0x1c) : nvgRGB(0xff, 0x80, 0x80);
}

inline void setFont(NVGcontext* vg, float size, float lineHeight) {
	if (APP->window->uiFont && APP->window->uiFont->handle >= 0)
		nvgFontFaceId(vg, APP->window->uiFont->handle);
	nvgFontSize(vg, size);
	nvgTextLineHeight(vg, lineHeight);
	nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
}

/** Height of the word-wrapped text, at least one line. */
inline float measureText(const std::string& s, float size, float lineHeight, float width) {
	NVGcontext* vg = APP->window->vg;
	nvgSave(vg);
	setFont(vg, size, lineHeight);
	float asc, desc, lineh;
	nvgTextMetrics(vg, &asc, &desc, &lineh);
	float h = lineh * lineHeight;
	if (!s.empty()) {
		float b[4];
		nvgTextBoxBounds(vg, 0, 0, std::max(width, 10.f), s.c_str(), NULL, b);
		h = std::max(h, b[3] - b[1]);
	}
	nvgRestore(vg);
	return std::ceil(h);
}

inline float measureWidth(const std::string& s, float size) {
	NVGcontext* vg = APP->window->vg;
	nvgSave(vg);
	setFont(vg, size, 1.f);
	float w = nvgTextBounds(vg, 0, 0, s.c_str(), NULL, NULL);
	nvgRestore(vg);
	return w;
}

/** Shortens `s` with an ellipsis so that it fits into `maxW`. The font must be set. */
inline std::string fitText(NVGcontext* vg, const std::string& s, float maxW) {
	if (nvgTextBounds(vg, 0, 0, s.c_str(), NULL, NULL) <= maxW)
		return s;
	std::string t = s;
	while (!t.empty()) {
		size_t pos = string::UTF8PrevCodepoint(t, t.size());
		t.resize(pos);
		std::string e = t + "\xe2\x80\xa6";
		if (nvgTextBounds(vg, 0, 0, e.c_str(), NULL, NULL) <= maxW)
			return e;
	}
	return "";
}

struct RowInfo {
	int start;
	int end;
	int next;
};

/** Word-wraps `s` like nvgTextBox. The font must be set. */
inline void breakRows(NVGcontext* vg, const std::string& s, float width, std::vector<RowInfo>& rows) {
	rows.clear();
	const char* base = s.c_str();
	const char* p = base;
	const char* endp = base + s.size();
	NVGtextRow tmp[64];
	while (p < endp && rows.size() < 4000) {
		int n = nvgTextBreakLines(vg, p, NULL, width, tmp, 64);
		if (n <= 0)
			break;
		for (int i = 0; i < n; i++) {
			RowInfo r;
			r.start = tmp[i].start - base;
			r.end = tmp[i].end - base;
			r.next = tmp[i].next - base;
			rows.push_back(r);
		}
		const char* next = tmp[n - 1].next;
		if (next <= p)
			break;
		p = next;
	}
	if (!s.empty() && s.back() == '\n') {
		RowInfo r;
		r.start = r.end = r.next = s.size();
		rows.push_back(r);
	}
}


/** Multiline text field whose height follows its text between `minLines` and `maxLines`, clipped to its box, scrolling to keep the caret visible. */
struct MultilineField : ui::TextField {
	float scrollY = 0.f;
	int minLines = 2;
	int maxLines = 8;

	MultilineField() {
		multiline = true;
	}

	float textWidth(float w) {
		return w - 2 * BND_TEXT_RADIUS;
	}

	float lineHeight() {
		NVGcontext* vg = APP->window->vg;
		nvgSave(vg);
		setFont(vg, FONT_BODY, 1.f);
		float asc, desc, lineh;
		nvgTextMetrics(vg, &asc, &desc, &lineh);
		nvgRestore(vg);
		return lineh;
	}

	/** Field height for the current text, between minLines and maxLines lines. */
	float desiredHeight(float w) {
		NVGcontext* vg = APP->window->vg;
		float lh = lineHeight();
		nvgSave(vg);
		setFont(vg, FONT_BODY, 1.f);
		float b[4];
		std::string s = text + "i";
		nvgTextBoxBounds(vg, 0, 0, textWidth(w), s.c_str(), NULL, b);
		nvgRestore(vg);
		float h = (b[3] - b[1]) + 8.f;
		float minH = std::ceil(lh * minLines + 8.f);
		float maxH = std::ceil(lh * maxLines + 8.f);
		return std::ceil(math::clamp(h, minH, maxH));
	}

	/** Keeps the caret visible when the text is taller than the field. */
	void updateScroll() {
		NVGcontext* vg = APP->window->vg;
		float lh = lineHeight();
		nvgSave(vg);
		setFont(vg, FONT_BODY, 1.f);
		std::vector<RowInfo> rows;
		breakRows(vg, text, textWidth(box.size.x), rows);
		nvgRestore(vg);
		float contentH = rows.size() * lh + 8.f;
		if (contentH <= box.size.y) {
			scrollY = 0.f;
			return;
		}
		int cr = (int) rows.size() - 1;
		for (size_t i = 0; i < rows.size(); i++) {
			if (cursor < rows[i].next) {
				cr = i;
				break;
			}
		}
		float top = cr * lh;
		float bottom = top + lh + 4.f;
		if (top < scrollY)
			scrollY = top;
		if (bottom > scrollY + box.size.y - 2.f)
			scrollY = bottom - (box.size.y - 2.f);
		scrollY = math::clamp(scrollY, 0.f, contentH - box.size.y);
	}

	int getTextPosition(math::Vec mousePos) override {
		return TextField::getTextPosition(mousePos.plus(math::Vec(0.f, scrollY)));
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		const BNDtheme* theme = bndGetTheme();
		BNDwidgetState state = BND_DEFAULT;
		if (this == APP->event->selectedWidget)
			state = BND_ACTIVE;
		else if (this == APP->event->hoveredWidget)
			state = BND_HOVER;

		// Background and outline
		bndTextField(vg, 0.0, 0.0, box.size.x, box.size.y, BND_CORNER_NONE, state, -1, "", 0, -1);

		nvgSave(vg);
		nvgIntersectScissor(vg, 0.f, 1.f, box.size.x, box.size.y - 2.f);
		nvgTextLineHeight(vg, 1.f);
		if (text.empty()) {
			bndIconLabelCaret(vg, 0.0, 0.0, box.size.x, box.size.y, -1, theme->textFieldTheme.itemColor, FONT_BODY, placeholder.c_str(), theme->textFieldTheme.itemColor, 0, -1);
			if (state == BND_ACTIVE) {
				// Caret on an empty field
				bndIconLabelCaret(vg, 0.0, 0.0, box.size.x, box.size.y, -1, theme->textFieldTheme.textSelectedColor, FONT_BODY, "", theme->textFieldTheme.itemColor, 0, 0);
			}
		}
		else {
			nvgTranslate(vg, 0.f, -scrollY);
			int begin = std::min(cursor, selection);
			int end = std::max(cursor, selection);
			NVGcolor c = state == BND_ACTIVE ? theme->textFieldTheme.textSelectedColor : theme->textFieldTheme.textColor;
			bndIconLabelCaret(vg, 0.0, 0.0, box.size.x, box.size.y, -1, c, FONT_BODY, text.c_str(), theme->textFieldTheme.itemColor, begin, state == BND_ACTIVE ? end : -1);
		}
		nvgRestore(vg);
	}
};


} // namespace uic
} // namespace assistant
} // namespace rack
