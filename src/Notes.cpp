#include "plugin.hpp"
#include "Theme.hpp"
#include <ui/TextField.hpp>
#include <osdialog.h>
#include <cctype>
#include <algorithm>
#include <chrono>
#include <fstream>

// ============================================================================
// NotesModule
// ============================================================================

struct NotesModule : Module {
    std::string text;
    std::string title = "notes";
    int widthHP = 6;
    bool hideLogo = false;
    bool rawMode = false;

    NotesModule() {
        config(0, 0, 0, 0);
    }

    void fromJson(json_t* rootJ) override {
        Module::fromJson(rootJ);
        if (json_t* j = json_object_get(rootJ, "width"))
            widthHP = std::round(json_number_value(j) / RACK_GRID_WIDTH);
    }

    json_t* dataToJson() override {
        json_t* root = json_object();
        json_object_set_new(root, "text", json_string(text.c_str()));
        json_object_set_new(root, "title", json_string(title.c_str()));
        json_object_set_new(root, "width", json_integer(widthHP));
        json_object_set_new(root, "hideLogo", json_boolean(hideLogo));
        json_object_set_new(root, "rawMode", json_boolean(rawMode));
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (json_t* j = json_object_get(root, "text"))
            text = json_string_value(j);
        if (json_t* j = json_object_get(root, "title"))
            title = json_string_value(j);
        if (json_t* j = json_object_get(root, "width"))
            widthHP = json_integer_value(j);
        if (json_t* j = json_object_get(root, "hideLogo"))
            hideLogo = json_boolean_value(j);
        if (json_t* j = json_object_get(root, "rawMode"))
            rawMode = json_boolean_value(j);
        // Back-compat: inherit old per-patch darkMode into the shared setting.
        if (json_t* j = json_object_get(root, "darkMode"))
            lc::theme.dark = json_boolean_value(j);
    }
};

// ============================================================================
// Text helpers
// ============================================================================

static bool isWordChar(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c < 0;  // treat UTF-8 bytes as word
}

static int prevCharIndex(const std::string& s, int pos) {
    if (pos <= 0) return 0;
    int i = pos - 1;
    // UTF-8: step back to start of code point
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int nextCharIndex(const std::string& s, int pos) {
    int n = (int)s.size();
    if (pos >= n) return n;
    int i = pos + 1;
    while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static int prevWordIndex(const std::string& s, int pos) {
    int i = pos;
    // Skip whitespace backwards
    while (i > 0 && std::isspace((unsigned char)s[i - 1])) i--;
    // Skip word chars backwards
    while (i > 0 && isWordChar(s[i - 1])) i--;
    // If we didn't move, step one char (punctuation)
    if (i == pos && i > 0) i = prevCharIndex(s, pos);
    return i;
}

static int nextWordIndex(const std::string& s, int pos) {
    int n = (int)s.size();
    int i = pos;
    while (i < n && isWordChar(s[i])) i++;
    while (i < n && std::isspace((unsigned char)s[i]) && s[i] != '\n') i++;
    if (i == pos && i < n) i = nextCharIndex(s, pos);
    return i;
}

static int lineStartIndex(const std::string& s, int pos) {
    int i = std::min((int)s.size(), std::max(0, pos));
    while (i > 0 && s[i - 1] != '\n') i--;
    return i;
}

static int lineEndIndex(const std::string& s, int pos) {
    int i = std::min((int)s.size(), std::max(0, pos));
    while (i < (int)s.size() && s[i] != '\n') i++;
    return i;
}

// Returns the byte length of a bullet marker at pos i (marker + trailing space),
// or 0 if none.
static int bulletMarkerLen(const std::string& s, int i) {
    int n = (int)s.size();
    if (i >= n) return 0;
    // ASCII single-byte bullets: - * +
    if (i + 1 < n && s[i + 1] == ' ' && (s[i] == '-' || s[i] == '*' || s[i] == '+'))
        return 2;
    // UTF-8 multibyte: • (E2 80 A2), → (E2 86 92), — (E2 80 94), · (C2 B7), ▪ (E2 96 AA)
    auto match3 = [&](unsigned char a, unsigned char b, unsigned char c) -> bool {
        return i + 3 < n
            && (unsigned char)s[i] == a
            && (unsigned char)s[i + 1] == b
            && (unsigned char)s[i + 2] == c
            && s[i + 3] == ' ';
    };
    if (match3(0xE2, 0x80, 0xA2)) return 4;  // •
    if (match3(0xE2, 0x86, 0x92)) return 4;  // →
    if (match3(0xE2, 0x80, 0x94)) return 4;  // —
    if (match3(0xE2, 0x96, 0xAA)) return 4;  // ▪
    // 2-byte: · (C2 B7)
    if (i + 2 < n && (unsigned char)s[i] == 0xC2 && (unsigned char)s[i + 1] == 0xB7
        && s[i + 2] == ' ')
        return 3;
    return 0;
}

static std::string continuationPrefix(const std::string& line, bool& emptyBullet) {
    emptyBullet = false;
    size_t i = 0;
    std::string indent;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        indent += line[i];
        i++;
    }
    if (i >= line.size()) return "";

    int bl = bulletMarkerLen(line, (int)i);
    if (bl > 0) {
        std::string body = line.substr(i + bl);
        if (body.find_first_not_of(" \t") == std::string::npos) emptyBullet = true;
        return indent + line.substr(i, bl);
    }

    size_t j = i;
    while (j < line.size() && std::isdigit((unsigned char)line[j])) j++;
    if (j > i && j + 1 < line.size() && line[j] == '.' && line[j + 1] == ' ') {
        int n = std::atoi(line.substr(i, j - i).c_str());
        std::string body = line.substr(j + 2);
        if (body.find_first_not_of(" \t") == std::string::npos) emptyBullet = true;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d. ", n + 1);
        return indent + std::string(buf);
    }

    return "";
}

// ============================================================================
// Text field
// ============================================================================

// Inline style bits
static const uint8_t ST_BOLD   = 1 << 0;
static const uint8_t ST_ITALIC = 1 << 1;
static const uint8_t ST_CODE   = 1 << 2;
static const uint8_t ST_MARKER = 1 << 3;  // marker chars: **, _, `, #, - (bullet)
static const uint8_t ST_BULLET = 1 << 4;  // bullet list marker
static const uint8_t ST_HR     = 1 << 5;  // horizontal rule (--- line)

struct NotesTextField : ui::TextField {
    NotesModule* nm = nullptr;
    std::string* modelBinding = nullptr;  // if null, bind to nm->text

    // Behavior
    bool singleLine = false;
    bool centered = false;
    bool baseBold = false;
    bool showScrollbar = true;

    // Rendering
    std::string fontPath;
    math::Vec textOffset = math::Vec(8.f, 8.f);
    float fontSize = 12.5f;
    float lineSpacing = 1.45f;

    // Visual row cache (rebuilt when text/width changes)
    struct Row { int start, end; float y; bool endsWithNewline; uint8_t heading; bool isHR; float leftPad; };
    std::vector<Row> rows;
    std::vector<uint8_t> styleMap;   // per-char style
    std::string cachedText;
    float cachedWidth = -1.f;
    float contentHeight = 0.f;

    struct StyledRun { int start, end; uint8_t style; float x; float width; };

    // Preferred x for vertical arrow navigation
    float preferredX = -1.f;

    // Scrolling
    float scroll = 0.f;

    // Caret blink
    double lastInteractionTime = 0.0;

    // Undo/redo
    struct Snap { std::string text; int cursor; int selection; };
    std::vector<Snap> undoStack;
    std::vector<Snap> redoStack;
    double lastEditTime = -1e9;
    static constexpr double EDIT_COALESCE_S = 0.7;
    bool pendingUndoPush = true;

    // Click tracking for double/triple click
    double lastClickTime = -1e9;
    int clickCount = 0;
    int lastClickPos = -1;

    NotesTextField() {
        multiline = true;
        fontPath = asset::plugin(pluginInstance, "res/_suisseSans_e8ec54.ttf");
    }

    std::string& modelText() {
        static std::string dummy;
        if (modelBinding) return *modelBinding;
        if (nm) return nm->text;
        return dummy;
    }

    // ---- Font ---------------------------------------------------------------

    std::shared_ptr<window::Font> getFont() {
        std::shared_ptr<window::Font> f = APP->window->loadFont(fontPath);
        if (!f || !f->handle)
            f = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
        return f;
    }

    std::shared_ptr<window::Font> getMonoFont() {
        return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
    }

    NVGcolor fgColor()  { return (lc::theme.dark) ? nvgRGB(235, 235, 235) : nvgRGB(20, 20, 20); }
    NVGcolor selColor() { return (lc::theme.dark) ? nvgRGBA(255,255,255,60) : nvgRGBA(0,0,0,50); }
    NVGcolor markerColor() {
        NVGcolor c = fgColor();
        return nvgRGBA(c.r * 255, c.g * 255, c.b * 255, 95);
    }
    NVGcolor codeBg() {
        return (lc::theme.dark) ? nvgRGBA(255,255,255,25) : nvgRGBA(0,0,0,20);
    }

    uint8_t styleAt(int i) const {
        return (i >= 0 && i < (int)styleMap.size()) ? styleMap[i] : 0;
    }

    static float headingScale(uint8_t h) {
        switch (h) {
            case 1: return 1.75f;
            case 2: return 1.45f;
            case 3: return 1.22f;
            case 4: return 1.12f;
            case 5: return 1.05f;
            case 6: return 1.00f;
            default: return 1.0f;
        }
    }

    void touchInteraction() { lastInteractionTime = nowSec(); }

    // ---- Parser -------------------------------------------------------------

    void parseStyles() {
        int n = (int)text.size();
        styleMap.assign(n, 0);
        int i = 0;
        while (i < n) {
            if (text[i] == '\n') { i++; continue; }

            bool atLineStart = (i == 0) || (text[i - 1] == '\n');

            // Horizontal rule (--- or *** or ___ alone on line)
            if (!singleLine && atLineStart) {
                int lineEnd = i;
                while (lineEnd < n && text[lineEnd] != '\n') lineEnd++;
                std::string lineStr = text.substr(i, lineEnd - i);
                // trim spaces
                std::string trimmed = lineStr;
                while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(trimmed.begin());
                while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) trimmed.pop_back();
                if (trimmed.size() >= 3) {
                    char c = trimmed[0];
                    if ((c == '-' || c == '*' || c == '_') &&
                        trimmed.find_first_not_of(c) == std::string::npos) {
                        for (int k = i; k < lineEnd; k++) styleMap[k] |= ST_MARKER | ST_HR;
                        i = lineEnd;
                        continue;
                    }
                }
            }

            // Heading at line start (skip in single-line mode)
            if (!singleLine && atLineStart && text[i] == '#') {
                int h = 0;
                while (i + h < n && text[i + h] == '#' && h < 6) h++;
                if (h > 0 && i + h < n && text[i + h] == ' ') {
                    for (int k = 0; k < h; k++) styleMap[i + k] |= ST_MARKER;
                    styleMap[i + h] |= ST_MARKER;
                    i += h + 1;
                    continue;
                }
            }

            // Bullet list marker at line start (after optional indent)
            if (!singleLine && atLineStart) {
                int k = i;
                while (k < n && (text[k] == ' ' || text[k] == '\t')) k++;
                int bl = bulletMarkerLen(text, k);
                if (bl > 0) {
                    // Bullet chars stay visible — ST_BULLET is metadata only.
                    for (int b = 0; b < bl; b++) styleMap[k + b] |= ST_BULLET;
                    i = k + bl;
                    continue;
                }
            }

            // Code
            if (text[i] == '`') {
                int j = i + 1;
                while (j < n && text[j] != '`' && text[j] != '\n') j++;
                if (j < n && text[j] == '`') {
                    styleMap[i] |= ST_MARKER | ST_CODE;
                    for (int k = i + 1; k < j; k++) styleMap[k] |= ST_CODE;
                    styleMap[j] |= ST_MARKER | ST_CODE;
                    i = j + 1;
                    continue;
                }
            }

            // Bold ** or __  (allow empty; stop at \n)
            auto tryBold = [&](char ch) -> bool {
                if (!(i + 1 < n && text[i] == ch && text[i + 1] == ch)) return false;
                int j = i + 2;
                while (j + 1 < n && !(text[j] == ch && text[j + 1] == ch) && text[j] != '\n') j++;
                if (j + 1 < n && text[j] == ch && text[j + 1] == ch) {
                    styleMap[i] |= ST_MARKER | ST_BOLD;
                    styleMap[i + 1] |= ST_MARKER | ST_BOLD;
                    for (int k = i + 2; k < j; k++) styleMap[k] |= ST_BOLD;
                    styleMap[j] |= ST_MARKER | ST_BOLD;
                    styleMap[j + 1] |= ST_MARKER | ST_BOLD;
                    i = j + 2;
                    return true;
                }
                return false;
            };
            if (tryBold('*')) continue;
            if (tryBold('_')) continue;

            // Italic * or _  (word-boundary, non-empty)
            auto tryItalic = [&](char ch) -> bool {
                if (text[i] != ch) return false;
                if (i + 1 < n && text[i + 1] == ch) return false;
                bool atStart = (i == 0) || !isalnum((unsigned char)text[i - 1]);
                if (!atStart) return false;
                int j = i + 1;
                while (j < n && text[j] != ch && text[j] != '\n') j++;
                if (j >= n || text[j] != ch) return false;
                bool atEnd = (j + 1 >= n) || !isalnum((unsigned char)text[j + 1]);
                if (!atEnd) return false;
                if (j <= i + 1) return false;
                styleMap[i] |= ST_MARKER | ST_ITALIC;
                for (int k = i + 1; k < j; k++) styleMap[k] |= ST_ITALIC;
                styleMap[j] |= ST_MARKER | ST_ITALIC;
                i = j + 1;
                return true;
            };
            if (tryItalic('*')) continue;
            if (tryItalic('_')) continue;

            i++;
        }
    }

    // ---- Run construction ---------------------------------------------------

    void configureFontForStyle(NVGcontext* vg, uint8_t style, float baseSize) {
        std::shared_ptr<window::Font> f =
            (style & ST_CODE) ? getMonoFont() : getFont();
        if (f && f->handle) nvgFontFaceId(vg, f->handle);
        float size = (style & ST_CODE) ? (baseSize * 0.94f) : baseSize;
        nvgFontSize(vg, size);
    }

    float rowBaseFontSize(int rowIdx) {
        if (rowIdx < 0 || rowIdx >= (int)rows.size()) return fontSize;
        return fontSize * headingScale(rows[rowIdx].heading);
    }

    std::vector<StyledRun> buildRowRuns(NVGcontext* vg, int rowIdx) {
        std::vector<StyledRun> out;
        if (rowIdx < 0 || rowIdx >= (int)rows.size()) return out;
        const Row& r = rows[rowIdx];
        if (r.end <= r.start) return out;

        float rowFS = rowBaseFontSize(rowIdx);
        float x = textOffset.x + r.leftPad;
        int i = r.start;
        while (i < r.end) {
            uint8_t s = styleAt(i);
            int j = i + 1;
            while (j < r.end && styleAt(j) == s) j++;

            float w;
            bool raw = nm && nm->rawMode;
            if ((s & ST_MARKER) && !raw) {
                w = 0.f;
            } else {
                configureFontForStyle(vg, s, rowFS);
                float b[4];
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                nvgTextBounds(vg, 0, 0, text.c_str() + i, text.c_str() + j, b);
                w = b[2] - b[0];
            }
            out.push_back({i, j, s, x, w});
            x += w;
            i = j;
        }

        if (centered && !out.empty()) {
            float totalW = (out.back().x + out.back().width) - textOffset.x;
            float shift = (box.size.x - totalW) / 2.f - textOffset.x;
            for (auto& run : out) run.x += shift;
        }
        return out;
    }

    // ---- Row model ----------------------------------------------------------

    float wrapWidth() {
        float w = box.size.x - textOffset.x * 2.f;
        return w < 20.f ? 20.f : w;
    }

    void rebuildRowsIfNeeded(NVGcontext* vg) {
        float w = wrapWidth();
        if (text == cachedText && std::abs(w - cachedWidth) < 0.5f && !rows.empty())
            return;
        parseStyles();
        rebuildRows(vg, w);
        cachedText = text;
        cachedWidth = w;
    }

    void rebuildRows(NVGcontext* vg, float w) {
        rows.clear();
        std::shared_ptr<window::Font> font = getFont();
        if (!font || !font->handle) return;

        nvgFontFaceId(vg, font->handle);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgTextLineHeight(vg, lineSpacing);

        int n = (int)text.size();

        if (singleLine) {
            rows.push_back({0, n, textOffset.y, false, 0, false, 0.f});
            contentHeight = fontSize * lineSpacing + textOffset.y * 2.f;
            return;
        }

        float y = textOffset.y;
        int lineStart = 0;

        while (lineStart <= n) {
            int logEnd = lineStart;
            while (logEnd < n && text[logEnd] != '\n') logEnd++;

            // Heading level for this logical line
            uint8_t hLevel = 0;
            {
                int h = 0;
                while (lineStart + h < n && text[lineStart + h] == '#' && h < 6) h++;
                if (h > 0 && lineStart + h < n && text[lineStart + h] == ' ') hLevel = h;
            }
            float rowFS = fontSize * headingScale(hLevel);
            float lh = rowFS * lineSpacing;
            if (hLevel > 0) lh *= 1.1f;  // a bit more breathing room

            nvgFontSize(vg, rowFS);

            // Detect HR line: all chars ST_HR
            bool isHR = false;
            if (logEnd > lineStart) {
                isHR = true;
                for (int k = lineStart; k < logEnd; k++) {
                    if (!(styleAt(k) & ST_HR)) { isHR = false; break; }
                }
            }

            // Detect bullet line (any char flagged ST_BULLET in this logical line)
            bool isBulletLine = false;
            for (int k = lineStart; k < logEnd; k++) {
                if (styleAt(k) & ST_BULLET) { isBulletLine = true; break; }
            }
            float leftPad = isBulletLine ? 14.f : 0.f;

            if (lineStart == logEnd) {
                rows.push_back({lineStart, lineStart, y, logEnd < n, hLevel, false, leftPad});
                y += lh;
            } else {
                const char* start = text.c_str() + lineStart;
                const char* end = text.c_str() + logEnd;
                NVGtextRow wrapRows[128];
                int nrows = nvgTextBreakLines(vg, start, end, w - leftPad, wrapRows, 128);
                if (nrows == 0) {
                    rows.push_back({lineStart, logEnd, y, logEnd < n, hLevel, isHR, leftPad});
                    y += lh;
                } else {
                    for (int r = 0; r < nrows; r++) {
                        int rs = lineStart + (int)(wrapRows[r].start - start);
                        int re = lineStart + (int)(wrapRows[r].end - start);
                        bool last = (r == nrows - 1);
                        rows.push_back({rs, re, y, last && logEnd < n, hLevel, isHR && (r == 0), leftPad});
                        y += lh;
                    }
                }
            }

            if (logEnd == n) break;
            lineStart = logEnd + 1;
        }
        if (rows.empty())
            rows.push_back({0, 0, textOffset.y, false, 0, false, 0.f});

        // Compute total content height
        if (!rows.empty()) {
            const Row& last = rows.back();
            float lastFS = fontSize * headingScale(last.heading);
            float lastLH = lastFS * lineSpacing;
            if (last.heading > 0) lastLH *= 1.1f;
            contentHeight = (last.y - textOffset.y) + lastLH + textOffset.y;
        } else contentHeight = 0.f;
    }

    int rowForCursor(int pos) {
        // Find the row that "owns" cursor position `pos`.
        // A row owns pos if row.start <= pos <= row.end.
        // When pos lies on the boundary between two rows (wrap), prefer the one
        // where pos == row.start (the later row) unless that's the next logical line.
        if (rows.empty()) return 0;
        int best = 0;
        for (int i = 0; i < (int)rows.size(); i++) {
            const Row& r = rows[i];
            if (pos >= r.start && pos <= r.end) best = i;
            if (pos > r.end) continue;
            if (pos < r.start) break;
        }
        // Prefer next row if pos == row.end and this row is a wrap (no newline)
        // and there's a next row starting at pos.
        if (best < (int)rows.size() - 1) {
            const Row& cur = rows[best];
            const Row& nxt = rows[best + 1];
            if (pos == cur.end && !cur.endsWithNewline && nxt.start == pos)
                best = best + 1;
        }
        return best;
    }

    // Width from row's textOffset.x to the given absolute text index `pos`,
    // respecting styled runs. `pos` must be within [row.start, row.end].
    float rowWidthTo(NVGcontext* vg, int rowIdx, int pos) {
        if (rowIdx < 0 || rowIdx >= (int)rows.size()) return 0.f;
        const Row& r = rows[rowIdx];
        pos = math::clamp(pos, r.start, r.end);
        std::vector<StyledRun> runs = buildRowRuns(vg, rowIdx);
        float rowFS = rowBaseFontSize(rowIdx);
        float x = 0.f;
        for (auto& run : runs) {
            if (pos >= run.end) { x = run.x + run.width - textOffset.x; continue; }
            if (pos <= run.start) break;
            if (run.style & ST_MARKER) { x = run.x - textOffset.x; return x; }
            configureFontForStyle(vg, run.style, rowFS);
            float b[4];
            nvgTextBounds(vg, 0, 0, text.c_str() + run.start, text.c_str() + pos, b);
            x = (run.x - textOffset.x) + (b[2] - b[0]);
            return x;
        }
        return x;
    }

    int getTextPosition(math::Vec mousePos) override {
        NVGcontext* vg = APP->window->vg;
        if (!vg) return 0;
        std::shared_ptr<window::Font> font = getFont();
        if (!font || !font->handle) return 0;

        rebuildRowsIfNeeded(vg);
        if (rows.empty()) return 0;

        nvgFontFaceId(vg, font->handle);
        nvgFontSize(vg, fontSize);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

        float yInDoc = mousePos.y + scroll;
        int rowIdx = (int)rows.size() - 1;
        for (int i = 0; i < (int)rows.size(); i++) {
            float rowFS = rowBaseFontSize(i);
            float lh = rowFS * lineSpacing;
            if (rows[i].heading > 0) lh *= 1.1f;
            if (yInDoc < rows[i].y + lh) { rowIdx = i; break; }
        }
        return cursorAtRowX(vg, rowIdx, mousePos.x);
    }

    int cursorAtRowX(NVGcontext* vg, int rowIdx, float x) {
        if (rowIdx < 0 || rowIdx >= (int)rows.size()) return 0;
        const Row& r = rows[rowIdx];
        if (r.end == r.start) return r.start;

        std::vector<StyledRun> runs = buildRowRuns(vg, rowIdx);
        if (runs.empty()) return r.start;

        float rowFS = rowBaseFontSize(rowIdx);
        int best = r.start;
        float bestDist = std::abs(x - textOffset.x);

        for (auto& run : runs) {
            if (run.style & ST_MARKER) continue;
            // Before the run
            if (x < run.x) {
                float d = std::abs(x - run.x);
                if (d < bestDist) { bestDist = d; best = run.start; }
                return best;
            }
            // Inside or past the run
            configureFontForStyle(vg, run.style, rowFS);
            NVGglyphPosition gp[1024];
            int ng = nvgTextGlyphPositions(vg, run.x, r.y,
                text.c_str() + run.start, text.c_str() + run.end, gp, 1024);
            for (int g = 0; g < ng; g++) {
                float d = std::abs(x - gp[g].x);
                if (d < bestDist) { bestDist = d; best = run.start + g; }
            }
            float xEnd = (ng > 0) ? gp[ng - 1].maxx : (run.x + run.width);
            float d = std::abs(x - xEnd);
            if (d < bestDist) { bestDist = d; best = run.end; }
        }
        // Snap out of any marker range
        int n = (int)text.size();
        while (best > 0 && best < n && (styleAt(best) & ST_MARKER)) best++;
        return best;
    }

    // ---- Drawing ------------------------------------------------------------

    void drawStyledRun(NVGcontext* vg, float x, float y, const StyledRun& run, float rowFS, bool forceBold) {
        bool raw = nm && nm->rawMode;
        if ((run.style & ST_MARKER) && !raw) return;  // hidden

        configureFontForStyle(vg, run.style, rowFS);

        if (run.style & ST_CODE) {
            nvgBeginPath(vg);
            nvgRect(vg, x - 1.f, y - 1.f, run.width + 2.f, rowFS + 3.f);
            nvgFillColor(vg, codeBg());
            nvgFill(vg);
        }

        if ((run.style & ST_MARKER) && raw)
            nvgFillColor(vg, markerColor());
        else
            nvgFillColor(vg, fgColor());

        const char* s = text.c_str() + run.start;
        const char* e = text.c_str() + run.end;

        auto drawMaybeItalic = [&](float dx) {
            if (run.style & ST_ITALIC) {
                nvgSave(vg);
                float skew = 0.2f;
                float yBaseline = y + rowFS;
                nvgTransform(vg, 1, 0, -skew, 1, skew * yBaseline, 0);
                nvgText(vg, x + dx, y, s, e);
                nvgRestore(vg);
            } else {
                nvgText(vg, x + dx, y, s, e);
            }
        };
        drawMaybeItalic(0.f);
        if ((run.style & ST_BOLD) || forceBold) drawMaybeItalic(0.5f);
    }

    void draw(const DrawArgs& args) override {
        rebuildRowsIfNeeded(args.vg);
        clampScroll();

        std::shared_ptr<window::Font> font = getFont();
        if (!font || !font->handle) return;

        nvgScissor(args.vg, 0, 0, box.size.x, box.size.y);

        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, fontSize);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgTextLineHeight(args.vg, lineSpacing);

        bool focused = APP->event->getSelectedWidget() == this;
        bool isPlaceholder = text.empty() && !placeholder.empty() && !focused;

        if (isPlaceholder) {
            NVGcolor c = fgColor();
            nvgFillColor(args.vg, nvgRGBA(c.r * 255, c.g * 255, c.b * 255, 95));
            float px = textOffset.x;
            if (centered) {
                float b[4];
                nvgTextBounds(args.vg, 0, 0, placeholder.c_str(), NULL, b);
                float tw = b[2] - b[0];
                px = (box.size.x - tw) / 2.f;
            }
            nvgText(args.vg, px, textOffset.y, placeholder.c_str(), NULL);
            nvgResetScissor(args.vg);
            return;
        }

        int selStart = std::min(cursor, selection);
        int selEnd = std::max(cursor, selection);
        int caretRow = rowForCursor(cursor);

        for (int i = 0; i < (int)rows.size(); i++) {
            const Row& r = rows[i];
            float y = r.y - scroll;
            float rowFS = rowBaseFontSize(i);
            // Cull offscreen
            if (y + rowFS * lineSpacing < 0.f) continue;
            if (y > box.size.y) break;

            std::vector<StyledRun> runs = buildRowRuns(args.vg, i);

            if (selEnd > selStart) {
                int s = std::max(selStart, r.start);
                int e = std::min(selEnd, r.end);
                if (e > s) {
                    float xS = textOffset.x + rowWidthTo(args.vg, i, s);
                    float xE = textOffset.x + rowWidthTo(args.vg, i, e);
                    nvgBeginPath(args.vg);
                    nvgRect(args.vg, xS, y - 1.f, xE - xS, rowFS + 3.f);
                    nvgFillColor(args.vg, selColor());
                    nvgFill(args.vg);
                }
                if (selStart <= r.end && selEnd > r.end && r.endsWithNewline) {
                    float xR = textOffset.x + rowWidthTo(args.vg, i, r.end);
                    nvgBeginPath(args.vg);
                    nvgRect(args.vg, xR, y - 1.f, 5.f, rowFS + 3.f);
                    nvgFillColor(args.vg, selColor());
                    nvgFill(args.vg);
                }
            }

            bool forceBold = (r.heading > 0) || baseBold;
            bool raw = nm && nm->rawMode;
            if (r.isHR && !raw) {
                // Full-width rule across the field
                NVGcolor fc = fgColor();
                float lineY = y + rowFS * 0.55f;
                float pad = 4.f;
                nvgBeginPath(args.vg);
                nvgMoveTo(args.vg, pad, std::round(lineY) + 0.5f);
                nvgLineTo(args.vg, box.size.x - pad, std::round(lineY) + 0.5f);
                nvgStrokeColor(args.vg, nvgRGBA(fc.r * 255, fc.g * 255, fc.b * 255, 110));
                nvgStrokeWidth(args.vg, 1.f);
                nvgStroke(args.vg);
            } else {
                for (auto& run : runs) {
                    drawStyledRun(args.vg, run.x, y, run, rowFS, forceBold);
                }
            }

            // Caret with blink
            if (focused && cursor == selection && i == caretRow) {
                double since = nowSec() - lastInteractionTime;
                bool visible = (since < 0.5) || (std::fmod(since - 0.5, 1.06) < 0.53);
                if (visible) {
                    float xC = textOffset.x + rowWidthTo(args.vg, i, cursor);
                    nvgBeginPath(args.vg);
                    nvgMoveTo(args.vg, xC + 0.5f, y);
                    nvgLineTo(args.vg, xC + 0.5f, y + rowFS + 1.f);
                    nvgStrokeColor(args.vg, fgColor());
                    nvgStrokeWidth(args.vg, 1.f);
                    nvgStroke(args.vg);
                }
            }
        }

        // Minimal scrollbar on right when content overflows
        if (showScrollbar && !singleLine && contentHeight > box.size.y + 0.5f) {
            float trackTop = 4.f;
            float trackH = box.size.y - 8.f;
            float ratio = box.size.y / contentHeight;
            float thumbH = std::max(24.f, trackH * ratio);
            float maxScroll = contentHeight - box.size.y;
            float thumbY = trackTop + (maxScroll > 0.f ? (scroll / maxScroll) : 0.f) * (trackH - thumbH);
            float thumbW = 3.f;
            float thumbX = box.size.x - thumbW - 3.f;
            NVGcolor fc = fgColor();
            nvgBeginPath(args.vg);
            nvgRoundedRect(args.vg, thumbX, thumbY, thumbW, thumbH, thumbW * 0.5f);
            nvgFillColor(args.vg, nvgRGBA(fc.r * 255, fc.g * 255, fc.b * 255, 75));
            nvgFill(args.vg);
        }

        nvgResetScissor(args.vg);
    }

    // ---- Scrolling ----------------------------------------------------------

    void clampScroll() {
        float maxScroll = std::max(0.f, contentHeight - box.size.y);
        scroll = math::clamp(scroll, 0.f, maxScroll);
    }

    void ensureCaretVisible() {
        NVGcontext* vg = APP->window->vg;
        if (vg) rebuildRowsIfNeeded(vg);
        if (rows.empty()) return;
        int rIdx = rowForCursor(cursor);
        const Row& r = rows[rIdx];
        float rowFS = rowBaseFontSize(rIdx);
        float lh = rowFS * lineSpacing;
        if (r.y < scroll) scroll = r.y - textOffset.y;
        if (r.y + lh > scroll + box.size.y) scroll = r.y + lh - box.size.y + textOffset.y;
        clampScroll();
    }

    void onHoverScroll(const event::HoverScroll& e) override {
        if (singleLine) return;
        float maxScroll = std::max(0.f, contentHeight - box.size.y);
        if (maxScroll <= 0.f) return;
        scroll -= e.scrollDelta.y;
        clampScroll();
        e.consume(this);
    }

    // ---- State sync ---------------------------------------------------------

    void step() override {
        TextField::step();
        if (!nm && !modelBinding) return;
        std::string& mt = modelText();
        bool focused = APP->event->getSelectedWidget() == this;
        if (focused) {
            if (mt != text) mt = text;
        } else {
            if (mt != text) {
                text = mt;
                cursor = std::min(cursor, (int)text.size());
                selection = std::min(selection, (int)text.size());
                invalidateRows();
            }
        }
    }

    void invalidateRows() { cachedText.clear(); cachedWidth = -1.f; }
    void writeBack() { modelText() = text; }

    // ---- Undo/redo ----------------------------------------------------------

    double nowSec() {
        using clock = std::chrono::steady_clock;
        return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    void pushUndo(bool force = false) {
        Snap s{text, cursor, selection};
        double now = nowSec();
        bool recent = (now - lastEditTime) < EDIT_COALESCE_S;
        if (!force && recent && !undoStack.empty() && !pendingUndoPush) return;

        undoStack.push_back(s);
        if (undoStack.size() > 500) undoStack.erase(undoStack.begin());
        redoStack.clear();
        pendingUndoPush = false;
    }

    void markEdit() { lastEditTime = nowSec(); }

    void undo() {
        if (undoStack.empty()) return;
        Snap cur{text, cursor, selection};
        Snap s = undoStack.back(); undoStack.pop_back();
        redoStack.push_back(cur);
        text = s.text; cursor = s.cursor; selection = s.selection;
        invalidateRows();
        writeBack();
        pendingUndoPush = true;
    }

    void redo() {
        if (redoStack.empty()) return;
        Snap cur{text, cursor, selection};
        Snap s = redoStack.back(); redoStack.pop_back();
        undoStack.push_back(cur);
        text = s.text; cursor = s.cursor; selection = s.selection;
        invalidateRows();
        writeBack();
        pendingUndoPush = true;
    }

    // ---- Editing primitives -------------------------------------------------

    void replaceSelection(const std::string& s) {
        int a = std::min(cursor, selection);
        int b = std::max(cursor, selection);
        text = text.substr(0, a) + s + text.substr(b);
        cursor = a + (int)s.size();
        selection = cursor;
        invalidateRows();
        writeBack();
        markEdit();
        touchInteraction();
        ensureCaretVisible();
    }

    void deleteRange(int a, int b) {
        if (a > b) std::swap(a, b);
        a = std::max(0, a);
        b = std::min((int)text.size(), b);
        if (a >= b) return;
        text.erase(a, b - a);
        if (cursor > b) cursor -= (b - a); else if (cursor > a) cursor = a;
        if (selection > b) selection -= (b - a); else if (selection > a) selection = a;
        invalidateRows();
        writeBack();
        markEdit();
        touchInteraction();
        ensureCaretVisible();
    }

    void wrapSelection(const std::string& left, const std::string& right) {
        pushUndo(true);
        int a = std::min(cursor, selection);
        int b = std::max(cursor, selection);
        std::string sel = text.substr(a, b - a);
        text = text.substr(0, a) + left + sel + right + text.substr(b);
        cursor = a + (int)left.size() + (int)sel.size();
        selection = cursor;
        invalidateRows();
        writeBack();
        markEdit();
        touchInteraction();
        pendingUndoPush = true;
    }

    // Change heading level of the line containing cursor.
    // direction > 0: bigger text; direction < 0: smaller text.
    void changeHeading(int direction) {
        pushUndo(true);
        int ls = lineStartIndex(text, std::min(cursor, selection));
        int n = (int)text.size();
        int h = 0;
        while (ls + h < n && text[ls + h] == '#' && h < 6) h++;
        bool hasSpace = (h > 0 && ls + h < n && text[ls + h] == ' ');
        int oldLen = hasSpace ? (h + 1) : 0;

        int newH;
        if (direction > 0) {
            if (h == 0) newH = 3;         // body -> H3
            else if (h <= 1) newH = 1;    // already max
            else newH = h - 1;
        } else {
            if (h == 0) return;           // already body
            else if (h >= 6) newH = 0;    // H6 -> body
            else newH = h + 1;
        }

        std::string ins = (newH > 0) ? (std::string(newH, '#') + " ") : "";
        text.erase(ls, oldLen);
        text.insert(ls, ins);
        int delta = (int)ins.size() - oldLen;
        if (cursor >= ls) cursor += delta;
        if (selection >= ls) selection += delta;

        invalidateRows();
        writeBack();
        markEdit();
        touchInteraction();
        pendingUndoPush = true;
    }

    // ---- Smart Enter/Tab ----------------------------------------------------

    void smartEnter() {
        if (singleLine) return;
        pushUndo(true);
        if (cursor != selection) {
            replaceSelection("\n");
            pendingUndoPush = true;
            return;
        }
        int ls = lineStartIndex(text, cursor);
        std::string line = text.substr(ls, cursor - ls);
        bool emptyBullet = false;
        std::string prefix = continuationPrefix(line, emptyBullet);

        if (emptyBullet) {
            text = text.substr(0, ls) + "\n" + text.substr(cursor);
            cursor = ls + 1;
            selection = cursor;
            invalidateRows(); writeBack(); markEdit(); touchInteraction();
            pendingUndoPush = true;
            return;
        }

        // Detect inline styles currently surrounding the cursor
        uint8_t leftS = (cursor > 0) ? styleAt(cursor - 1) : 0;
        uint8_t rightS = (cursor < (int)text.size()) ? styleAt(cursor) : 0;
        uint8_t active = 0;
        for (uint8_t bit : {(uint8_t)ST_BOLD, (uint8_t)ST_ITALIC, (uint8_t)ST_CODE}) {
            if ((leftS & bit) && (rightS & bit)) active |= bit;
        }

        std::string marker;
        if (active & ST_CODE)        marker = "`";
        else if (active & ST_BOLD)   marker = "**";
        else if (active & ST_ITALIC) marker = "_";

        std::string insert;
        int cursorShift;
        if (marker.empty()) {
            insert = "\n" + prefix;
            cursorShift = (int)insert.size();
            text.insert(cursor, insert);
        } else {
            // Close current span, newline, reopen on next line with a placeholder close.
            // Cursor lands between the new opening markers and placeholder close.
            std::string pre  = marker + "\n" + prefix + marker;
            std::string post = marker;
            text.insert(cursor, pre + post);
            cursorShift = (int)pre.size();
        }
        cursor += cursorShift;
        selection = cursor;
        invalidateRows();
        writeBack();
        markEdit();
        touchInteraction();
        pendingUndoPush = true;
    }

    void smartTab(bool shift) {
        pushUndo(true);
        const int INDENT = 4;
        const std::string INDENT_STR(INDENT, ' ');
        // If selection spans multiple lines, indent/outdent each
        int a = std::min(cursor, selection);
        int b = std::max(cursor, selection);
        int lsA = lineStartIndex(text, a);
        int lsB = lineStartIndex(text, b);
        if (a != b && (lsA != lsB || text.find('\n', lsA) < (size_t)b)) {
            int cur = lsA;
            while (cur <= lsB) {
                if (shift) {
                    int r = 0;
                    if (cur < (int)text.size() && text[cur] == '\t') r = 1;
                    else {
                        while (r < INDENT && cur + r < (int)text.size() && text[cur + r] == ' ') r++;
                    }
                    if (r > 0) {
                        text.erase(cur, r);
                        if (cursor > cur) cursor = std::max(cur, cursor - r);
                        if (selection > cur) selection = std::max(cur, selection - r);
                        lsB -= r;
                        b -= r;
                    }
                } else {
                    text.insert(cur, INDENT_STR);
                    if (cursor >= cur) cursor += INDENT;
                    if (selection >= cur) selection += INDENT;
                    lsB += INDENT;
                    b += INDENT;
                }
                int nl = text.find('\n', cur);
                if (nl == (int)std::string::npos || nl >= lsB) break;
                cur = nl + 1;
            }
        } else {
            int ls = lineStartIndex(text, cursor);
            if (shift) {
                int r = 0;
                if (ls < (int)text.size() && text[ls] == '\t') r = 1;
                else { while (r < INDENT && ls + r < (int)text.size() && text[ls + r] == ' ') r++; }
                if (r > 0) {
                    text.erase(ls, r);
                    if (cursor > ls) cursor = std::max(ls, cursor - r);
                    if (selection > ls) selection = std::max(ls, selection - r);
                }
            } else {
                text.insert(ls, INDENT_STR);
                if (cursor >= ls) cursor += INDENT;
                if (selection >= ls) selection += INDENT;
            }
        }
        invalidateRows();
        writeBack();
        markEdit();
        pendingUndoPush = true;
    }

    // ---- Cursor navigation --------------------------------------------------

    void skipMarkers(int dir) {
        int n = (int)text.size();
        while (cursor > 0 && cursor < n && (styleAt(cursor) & ST_MARKER)) {
            int next = (dir < 0) ? prevCharIndex(text, cursor) : nextCharIndex(text, cursor);
            if (next == cursor) break;
            cursor = next;
        }
    }

    void moveHoriz(int dir, bool select, bool word) {
        if (!select && cursor != selection) {
            cursor = (dir < 0) ? std::min(cursor, selection) : std::max(cursor, selection);
            selection = cursor;
            preferredX = -1.f;
            touchInteraction();
            return;
        }
        int n = (int)text.size();
        if (word) cursor = (dir < 0) ? prevWordIndex(text, cursor) : nextWordIndex(text, cursor);
        else       cursor = (dir < 0) ? prevCharIndex(text, cursor) : nextCharIndex(text, cursor);
        cursor = math::clamp(cursor, 0, n);
        skipMarkers(dir);
        if (!select) selection = cursor;
        preferredX = -1.f;
        touchInteraction();
        ensureCaretVisible();
    }

    void moveVert(int dir, bool select) {
        if (singleLine) return;
        NVGcontext* vg = APP->window->vg;
        if (!vg) return;
        rebuildRowsIfNeeded(vg);
        if (rows.empty()) return;
        int rIdx = rowForCursor(cursor);
        float x = preferredX;
        if (x < 0.f) {
            x = textOffset.x + rowWidthTo(vg, rIdx, cursor);
            preferredX = x;
        }
        int tgt = rIdx + dir;
        if (tgt < 0) { cursor = 0; if (!select) selection = cursor; touchInteraction(); ensureCaretVisible(); return; }
        if (tgt >= (int)rows.size()) { cursor = (int)text.size(); if (!select) selection = cursor; touchInteraction(); ensureCaretVisible(); return; }
        cursor = cursorAtRowX(vg, tgt, x);
        if (!select) selection = cursor;
        touchInteraction();
        ensureCaretVisible();
    }

    void moveToVisualLineStart(bool select) {
        NVGcontext* vg = APP->window->vg;
        if (vg) rebuildRowsIfNeeded(vg);
        if (rows.empty()) return;
        int rIdx = rowForCursor(cursor);
        cursor = rows[rIdx].start;
        if (!select) selection = cursor;
        preferredX = -1.f;
    }

    void moveToVisualLineEnd(bool select) {
        NVGcontext* vg = APP->window->vg;
        if (vg) rebuildRowsIfNeeded(vg);
        if (rows.empty()) return;
        int rIdx = rowForCursor(cursor);
        cursor = rows[rIdx].end;
        if (!select) selection = cursor;
        preferredX = -1.f;
    }

    void moveDocStart(bool select) { cursor = 0; if (!select) selection = 0; preferredX = -1.f; }
    void moveDocEnd(bool select)   { cursor = (int)text.size(); if (!select) selection = cursor; preferredX = -1.f; }

    // ---- Mouse --------------------------------------------------------------

    void selectWordAt(int pos) {
        int n = (int)text.size();
        if (n == 0) return;
        pos = math::clamp(pos, 0, n);
        // Expand outward while adjacent to word chars.
        int a = pos, b = pos;
        while (a > 0 && isWordChar(text[a - 1])) a--;
        while (b < n && isWordChar(text[b])) b++;
        if (a == b) {
            // Cursor on a non-word char. Try selecting the char itself.
            if (pos < n) { a = pos; b = pos + 1; }
            else if (pos > 0) { a = pos - 1; b = pos; }
            else return;
        }
        selection = a;
        cursor = b;
        preferredX = -1.f;
    }

    void selectParagraphAt(int pos) {
        selection = lineStartIndex(text, pos);
        cursor = lineEndIndex(text, pos);
        preferredX = -1.f;
    }

    Vec lastClickPx = Vec(-1e6, -1e6);

    void onButton(const event::Button& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
            APP->event->setSelectedWidget(this);
            openContextMenu();
            e.consume(this);
            return;
        }
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            int pos = getTextPosition(e.pos);
            double now = nowSec();
            float dPx = std::hypot(e.pos.x - lastClickPx.x, e.pos.y - lastClickPx.y);
            bool near = (dPx < 10.f);
            if (now - lastClickTime < 0.55 && near) {
                clickCount++;
            } else {
                clickCount = 1;
            }
            lastClickTime = now;
            lastClickPos = pos;
            lastClickPx = e.pos;

            if (clickCount >= 3) {
                APP->event->setSelectedWidget(this);
                selectParagraphAt(pos);
                touchInteraction();
                e.consume(this);
                return;
            }
            if (clickCount == 2) {
                APP->event->setSelectedWidget(this);
                selectWordAt(pos);
                touchInteraction();
                e.consume(this);
                return;
            }
        }
        TextField::onButton(e);
    }

    void onDragHover(const event::DragHover& e) override {
        if (e.origin == this) {
            // Don't let drag collapse a multi-click selection on tiny wiggles.
            if (clickCount >= 2) {
                e.consume(this);
                return;
            }
            int pos = getTextPosition(e.pos);
            cursor = pos;
            preferredX = -1.f;
            e.consume(this);
            return;
        }
        TextField::onDragHover(e);
    }

    // ---- Key handling -------------------------------------------------------

    void onSelectKey(const event::SelectKey& e) override {
        if (e.action == GLFW_PRESS || e.action == GLFW_REPEAT) {
            int mods = e.mods & RACK_MOD_MASK;
            bool cmd   = (mods & RACK_MOD_CTRL) != 0;
            bool alt   = (mods & GLFW_MOD_ALT) != 0;
            bool shift = (e.mods & GLFW_MOD_SHIFT) != 0;

            // Undo / redo
            if (cmd && !alt && e.key == GLFW_KEY_Z) {
                if (shift) redo(); else undo();
                e.consume(this); return;
            }
            if (cmd && !alt && e.key == GLFW_KEY_Y) { redo(); e.consume(this); return; }

            // Formatting
            if (cmd && !alt && !shift && e.key == GLFW_KEY_B) { wrapSelection("**", "**"); e.consume(this); return; }
            if (cmd && !alt && !shift && e.key == GLFW_KEY_I) { wrapSelection("_", "_"); e.consume(this); return; }
            if (cmd && !alt && !shift && e.key == GLFW_KEY_E) { wrapSelection("`", "`"); e.consume(this); return; }

            // Heading size: Cmd+Shift+] = bigger, Cmd+Shift+[ = smaller
            if (cmd && shift && e.key == GLFW_KEY_RIGHT_BRACKET) { changeHeading(+1); e.consume(this); return; }
            if (cmd && shift && e.key == GLFW_KEY_LEFT_BRACKET)  { changeHeading(-1); e.consume(this); return; }

            // Clipboard / select all (let base handle, but push undo first for paste/cut)
            if (cmd && !alt && e.key == GLFW_KEY_A) {
                selectAll(); e.consume(this); return;
            }
            if (cmd && !alt && e.key == GLFW_KEY_C) {
                copyClipboard(); e.consume(this); return;
            }
            if (cmd && !alt && e.key == GLFW_KEY_X) {
                pushUndo(true); cutClipboard(); writeBack(); invalidateRows();
                markEdit(); pendingUndoPush = true;
                e.consume(this); return;
            }
            if (cmd && !alt && e.key == GLFW_KEY_V) {
                pushUndo(true); pasteClipboard(); writeBack(); invalidateRows();
                markEdit(); pendingUndoPush = true;
                e.consume(this); return;
            }

            // Enter
            if (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER) {
                smartEnter();
                e.consume(this); return;
            }

            // Tab
            if (e.key == GLFW_KEY_TAB) {
                smartTab(shift);
                e.consume(this); return;
            }

            // Horizontal nav
            if (e.key == GLFW_KEY_LEFT) {
                if (cmd) moveToVisualLineStart(shift);
                else     moveHoriz(-1, shift, alt);
                e.consume(this); return;
            }
            if (e.key == GLFW_KEY_RIGHT) {
                if (cmd) moveToVisualLineEnd(shift);
                else     moveHoriz(+1, shift, alt);
                e.consume(this); return;
            }

            // Vertical nav
            if (e.key == GLFW_KEY_UP) {
                if (cmd) moveDocStart(shift);
                else     moveVert(-1, shift);
                e.consume(this); return;
            }
            if (e.key == GLFW_KEY_DOWN) {
                if (cmd) moveDocEnd(shift);
                else     moveVert(+1, shift);
                e.consume(this); return;
            }

            // Home / End
            if (e.key == GLFW_KEY_HOME) {
                if (cmd) moveDocStart(shift); else moveToVisualLineStart(shift);
                e.consume(this); return;
            }
            if (e.key == GLFW_KEY_END) {
                if (cmd) moveDocEnd(shift); else moveToVisualLineEnd(shift);
                e.consume(this); return;
            }

            // Page up/down (approximate)
            if (e.key == GLFW_KEY_PAGE_UP)   { for (int i=0;i<12;i++) moveVert(-1, shift); e.consume(this); return; }
            if (e.key == GLFW_KEY_PAGE_DOWN) { for (int i=0;i<12;i++) moveVert(+1, shift); e.consume(this); return; }

            // Backspace
            if (e.key == GLFW_KEY_BACKSPACE) {
                pushUndo(true);
                if (cursor != selection) {
                    replaceSelection("");
                } else if (cmd) {
                    int ls = lineStartIndex(text, cursor);
                    deleteRange(ls, cursor);
                } else if (alt) {
                    int pw = prevWordIndex(text, cursor);
                    deleteRange(pw, cursor);
                } else {
                    int pc = prevCharIndex(text, cursor);
                    deleteRange(pc, cursor);
                }
                pendingUndoPush = true;
                preferredX = -1.f;
                e.consume(this); return;
            }

            // Delete
            if (e.key == GLFW_KEY_DELETE) {
                pushUndo(true);
                if (cursor != selection) {
                    replaceSelection("");
                } else if (cmd) {
                    int le = lineEndIndex(text, cursor);
                    deleteRange(cursor, le);
                } else if (alt) {
                    int nw = nextWordIndex(text, cursor);
                    deleteRange(cursor, nw);
                } else {
                    int nc = nextCharIndex(text, cursor);
                    deleteRange(cursor, nc);
                }
                pendingUndoPush = true;
                preferredX = -1.f;
                e.consume(this); return;
            }

            // Escape: deselect
            if (e.key == GLFW_KEY_ESCAPE) {
                APP->event->setSelectedWidget(NULL);
                e.consume(this); return;
            }
        }
        // Let anything unhandled fall through
    }

    void onSelectText(const event::SelectText& e) override {
        if (e.codepoint < 32 || e.codepoint == 127) return;
        // Coalesce typing into a single undo step
        pushUndo(false);
        // Encode codepoint as UTF-8
        char buf[8]; int n = 0;
        uint32_t c = e.codepoint;
        if (c < 0x80) { buf[n++] = c; }
        else if (c < 0x800) { buf[n++] = 0xC0 | (c >> 6); buf[n++] = 0x80 | (c & 0x3F); }
        else if (c < 0x10000) { buf[n++] = 0xE0 | (c >> 12); buf[n++] = 0x80 | ((c >> 6) & 0x3F); buf[n++] = 0x80 | (c & 0x3F); }
        else { buf[n++] = 0xF0 | (c >> 18); buf[n++] = 0x80 | ((c >> 12) & 0x3F); buf[n++] = 0x80 | ((c >> 6) & 0x3F); buf[n++] = 0x80 | (c & 0x3F); }
        replaceSelection(std::string(buf, n));
        markEdit();
        preferredX = -1.f;
        e.consume(this);
    }

    // ---- Context menu -------------------------------------------------------

    void openContextMenu() {
        ui::Menu* menu = createMenu();
        menu->addChild(createMenuLabel("Notes"));

        menu->addChild(createMenuItem("Undo", RACK_MOD_CTRL_NAME "+Z", [this]() {
            APP->event->setSelectedWidget(this); undo();
        }));
        menu->addChild(createMenuItem("Redo", RACK_MOD_CTRL_NAME "+" "Shift+Z", [this]() {
            APP->event->setSelectedWidget(this); redo();
        }));
        menu->addChild(new ui::MenuSeparator);

        menu->addChild(createMenuItem("Bold", RACK_MOD_CTRL_NAME "+B", [this]() {
            APP->event->setSelectedWidget(this); wrapSelection("**", "**");
        }));
        menu->addChild(createMenuItem("Italic", RACK_MOD_CTRL_NAME "+I", [this]() {
            APP->event->setSelectedWidget(this); wrapSelection("_", "_");
        }));
        menu->addChild(createMenuItem("Inline code", RACK_MOD_CTRL_NAME "+E", [this]() {
            APP->event->setSelectedWidget(this); wrapSelection("`", "`");
        }));
        menu->addChild(new ui::MenuSeparator);

        menu->addChild(createMenuItem("Heading", "", [this]() {
            APP->event->setSelectedWidget(this);
            pushUndo(true);
            int ls = lineStartIndex(text, cursor);
            text.insert(ls, "# ");
            if (cursor >= ls) cursor += 2;
            if (selection >= ls) selection += 2;
            invalidateRows(); writeBack(); markEdit(); pendingUndoPush = true;
        }));
        menu->addChild(createMenuItem("Bullet list", "", [this]() {
            APP->event->setSelectedWidget(this);
            pushUndo(true);
            int ls = lineStartIndex(text, cursor);
            text.insert(ls, "- ");
            if (cursor >= ls) cursor += 2;
            if (selection >= ls) selection += 2;
            invalidateRows(); writeBack(); markEdit(); pendingUndoPush = true;
        }));
        menu->addChild(createMenuItem("Numbered list", "", [this]() {
            APP->event->setSelectedWidget(this);
            pushUndo(true);
            int ls = lineStartIndex(text, cursor);
            text.insert(ls, "1. ");
            if (cursor >= ls) cursor += 3;
            if (selection >= ls) selection += 3;
            invalidateRows(); writeBack(); markEdit(); pendingUndoPush = true;
        }));
        menu->addChild(createMenuItem("Insert horizontal line", "", [this]() {
            APP->event->setSelectedWidget(this);
            pushUndo(true);
            int le = lineEndIndex(text, cursor);
            text.insert(le, "\n---");
            invalidateRows(); writeBack(); markEdit(); pendingUndoPush = true;
        }));
        menu->addChild(new ui::MenuSeparator);

        menu->addChild(createMenuItem("Cut", RACK_MOD_CTRL_NAME "+X", [this]() {
            APP->event->setSelectedWidget(this);
            pushUndo(true); cutClipboard(); writeBack(); invalidateRows();
            markEdit(); pendingUndoPush = true;
        }));
        menu->addChild(createMenuItem("Copy", RACK_MOD_CTRL_NAME "+C", [this]() {
            APP->event->setSelectedWidget(this); copyClipboard();
        }));
        menu->addChild(createMenuItem("Paste", RACK_MOD_CTRL_NAME "+V", [this]() {
            APP->event->setSelectedWidget(this);
            pushUndo(true); pasteClipboard(); writeBack(); invalidateRows();
            markEdit(); pendingUndoPush = true;
        }));
        menu->addChild(createMenuItem("Select all", RACK_MOD_CTRL_NAME "+A", [this]() {
            APP->event->setSelectedWidget(this); selectAll();
        }));
        menu->addChild(new ui::MenuSeparator);

        menu->addChild(createMenuItem("Clear all", "", [this]() {
            pushUndo(true);
            text.clear(); cursor = selection = 0;
            invalidateRows(); writeBack(); markEdit(); pendingUndoPush = true;
        }));

        // Module-wide toggles (always accessible from the field right-click)
        if (nm) {
            menu->addChild(new ui::MenuSeparator);
            menu->addChild(createBoolPtrMenuItem("View mode \u2192 raw (show markers)", "", &nm->rawMode));
            menu->addChild(createMenuItem("Export as Markdown (.md)", "", [this]() {
                if (!nm) return;
                std::string def = (!nm->title.empty() ? nm->title : std::string("notes")) + ".md";
                char* path = osdialog_file(OSDIALOG_SAVE, NULL, def.c_str(), NULL);
                if (!path) return;
                std::ofstream out(path);
                if (out) {
                    if (!nm->title.empty()) out << "# " << nm->title << "\n\n";
                    out << nm->text;
                }
                std::free(path);
            }));
            menu->addChild(createMenuItem("Export as Text (.txt)", "", [this]() {
                if (!nm) return;
                std::string def = (!nm->title.empty() ? nm->title : std::string("notes")) + ".txt";
                char* path = osdialog_file(OSDIALOG_SAVE, NULL, def.c_str(), NULL);
                if (!path) return;
                std::ofstream out(path);
                if (out) {
                    std::string src = nm->title.empty() ? nm->text : (nm->title + "\n\n" + nm->text);
                    std::string stripped; stripped.reserve(src.size());
                    bool atLS = true;
                    for (size_t k = 0; k < src.size(); k++) {
                        char c = src[k];
                        if (atLS) {
                            size_t j = k; int h = 0;
                            while (j < src.size() && src[j] == '#' && h < 6) { j++; h++; }
                            if (h > 0 && j < src.size() && src[j] == ' ') { k = j; continue; }
                        }
                        if (c == '*' || c == '_' || c == '`') continue;
                        stripped += c;
                        atLS = (c == '\n');
                    }
                    out << stripped;
                }
                std::free(path);
            }));
            menu->addChild(createBoolPtrMenuItem("Hide logo", "", &nm->hideLogo));
            menu->addChild(new ui::MenuSeparator);
            menu->addChild(createMenuItem("Dark mode (shared)",
                CHECKMARK(lc::theme.dark), []() {
                    lc::theme.dark = !lc::theme.dark;
                    lc::saveTheme();
                }));
        }
    }
};

// ============================================================================
// Background / logo / resize handle
// ============================================================================

struct NotesBackground : widget::Widget {
    NotesModule* nm = nullptr;
    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, dark ? nvgRGB(0, 0, 0) : nvgRGB(255, 255, 255));
        nvgFill(args.vg);
    }
};

struct NotesLogo : widget::Widget {
    NotesModule* nm = nullptr;
    void draw(const DrawArgs& args) override {
        if (nm && nm->hideLogo) return;
        bool dark = lc::theme.dark;
        std::string path = asset::plugin(pluginInstance,
            dark ? "res/lc-icon-white.png" : "res/lc-icon-new.png");
        std::shared_ptr<window::Image> img = APP->window->loadImage(path);
        if (!img || img->handle < 0) return;
        NVGpaint paint = nvgImagePattern(args.vg, 0, 0, box.size.x, box.size.y, 0, img->handle, 1.f);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillPaint(args.vg, paint);
        nvgFill(args.vg);
    }
};

struct NotesResizeHandle : widget::OpaqueWidget {
    NotesModule* nm = nullptr;
    Vec dragPos;
    Rect originalBox;
    static const int MIN_HP = 3;
    static const int MAX_HP = 128;

    NotesResizeHandle() { box.size = Vec(14.f, 14.f); }

    void onDragStart(const event::DragStart& e) override {
        if (e.button != GLFW_MOUSE_BUTTON_LEFT) return;
        dragPos = APP->scene->rack->getMousePos();
        ModuleWidget* mw = getAncestorOfType<ModuleWidget>();
        if (mw) originalBox = mw->box;
    }

    void onDragMove(const event::DragMove& e) override {
        ModuleWidget* mw = getAncestorOfType<ModuleWidget>();
        if (!mw || !nm) return;
        Vec newDragPos = APP->scene->rack->getMousePos();
        float dx = newDragPos.x - dragPos.x;

        Rect newBox = originalBox;
        Rect oldBox = mw->box;
        const float minW = MIN_HP * RACK_GRID_WIDTH;
        const float maxW = MAX_HP * RACK_GRID_WIDTH;
        newBox.size.x += dx;
        newBox.size.x = math::clamp(newBox.size.x, minW, maxW);
        newBox.size.x = std::round(newBox.size.x / RACK_GRID_WIDTH) * RACK_GRID_WIDTH;

        mw->box = newBox;
        if (!APP->scene->rack->requestModulePos(mw, newBox.pos)) mw->box = oldBox;
        nm->widthHP = (int)std::round(mw->box.size.x / RACK_GRID_WIDTH);
    }

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        NVGcolor c = dark ? nvgRGBA(255,255,255,110) : nvgRGBA(0,0,0,110);
        float pad = 3.f;
        float w = box.size.x - pad;
        float h = box.size.y - pad;
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, w, pad + 2.f);
        nvgLineTo(args.vg, w, h);
        nvgLineTo(args.vg, pad + 2.f, h);
        nvgClosePath(args.vg);
        nvgFillColor(args.vg, c);
        nvgFill(args.vg);
    }
};

// ============================================================================
// Widget
// ============================================================================

struct NotesWidget : ModuleWidget {
    NotesBackground* bg = nullptr;
    NotesTextField* field = nullptr;
    NotesTextField* titleField = nullptr;
    NotesResizeHandle* handle = nullptr;
    NotesLogo* logo = nullptr;
    Widget* topLeftScrew = nullptr;
    Widget* topRightScrew = nullptr;
    Widget* bottomLeftScrew = nullptr;
    Widget* bottomRightScrew = nullptr;

    static constexpr float LOGO_MM = 9.f;
    static constexpr float HANDLE_PX = 14.f;

    NotesWidget(NotesModule* module) {
        setModule(module);
        box.size = Vec(RACK_GRID_WIDTH * 6, RACK_GRID_HEIGHT);
        if (module) box.size.x = module->widthHP * RACK_GRID_WIDTH;

        bg = new NotesBackground;
        bg->nm = module;
        bg->box.size = box.size;
        addChild(bg);

        // Screws: 1 HP inset from corners
        topLeftScrew     = createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0));
        topRightScrew    = createWidget<ScrewBlack>(Vec(box.size.x - RACK_GRID_WIDTH * 2, 0));
        bottomLeftScrew  = createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH));
        bottomRightScrew = createWidget<ScrewBlack>(Vec(box.size.x - RACK_GRID_WIDTH * 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH));
        addChild(topLeftScrew);
        addChild(topRightScrew);
        addChild(bottomLeftScrew);
        addChild(bottomRightScrew);

        titleField = new NotesTextField;
        titleField->nm = module;
        titleField->modelBinding = module ? &module->title : nullptr;
        titleField->singleLine = true;
        titleField->centered = true;
        titleField->baseBold = true;
        titleField->showScrollbar = false;
        titleField->fontSize = 10.5f;
        titleField->textOffset = Vec(4.f, 3.f);
        titleField->placeholder = "notes";
        if (module) titleField->text = module->title;
        addChild(titleField);

        field = new NotesTextField;
        field->nm = module;
        field->placeholder = "type...";
        if (module) field->text = module->text;
        addChild(field);

        logo = new NotesLogo;
        logo->nm = module;
        logo->box.size = mm2px(Vec(LOGO_MM, LOGO_MM));
        addChild(logo);

        handle = new NotesResizeHandle;
        handle->nm = module;
        addChild(handle);

        layout();
    }

    void layout() {
        if (titleField) {
            // Centered between the two top screws, padded so text doesn't clip.
            float x0 = RACK_GRID_WIDTH * 2.f;
            float x1 = box.size.x - RACK_GRID_WIDTH * 2.f;
            float titleY = 2.f;
            float titleH = RACK_GRID_WIDTH + 4.f;  // ~19px, plenty for 10.5pt font
            titleField->box.pos = Vec(x0, titleY);
            titleField->box.size = Vec(std::max(0.f, x1 - x0), titleH);
        }
        // Logo matches Tidy's y position: logoTopMM = 128.5 - 8 - 9 = 111.5mm
        float logoYPx = mm2px(128.5f - 8.f - LOGO_MM);
        if (field) {
            float top = RACK_GRID_WIDTH + 8.f;
            float bottom = logoYPx - 4.f;
            field->box.pos = Vec(6, top);
            field->box.size = Vec(box.size.x - 12, bottom - top);
            field->invalidateRows();
        }
        if (logo) {
            logo->box.pos = Vec(
                box.size.x - logo->box.size.x - 6.f,
                logoYPx);
        }
        if (handle) {
            handle->box.pos = Vec(
                box.size.x - HANDLE_PX - 2.f,
                box.size.y - HANDLE_PX - 2.f);
        }
        if (topRightScrew)
            topRightScrew->box.pos.x = box.size.x - RACK_GRID_WIDTH * 2;
        if (bottomRightScrew)
            bottomRightScrew->box.pos.x = box.size.x - RACK_GRID_WIDTH * 2;
    }

    void step() override {
        NotesModule* m = dynamic_cast<NotesModule*>(this->module);
        if (m) box.size.x = m->widthHP * RACK_GRID_WIDTH;
        if (bg) bg->box.size = box.size;
        layout();
        ModuleWidget::step();
    }

    void appendContextMenu(ui::Menu* menu) override {
        NotesModule* m = dynamic_cast<NotesModule*>(module);
        if (!m) return;
        menu->addChild(new ui::MenuSeparator);
        menu->addChild(createMenuLabel("Lux Cache Notes"));

        menu->addChild(createBoolPtrMenuItem("Raw mode (show markers)", "", &m->rawMode));

        menu->addChild(createMenuItem("Export as Markdown (.md)", "", [m]() {
            std::string def = (!m->title.empty() ? m->title : std::string("notes")) + ".md";
            char* path = osdialog_file(OSDIALOG_SAVE, NULL, def.c_str(), NULL);
            if (!path) return;
            std::ofstream out(path);
            if (out) {
                if (!m->title.empty()) out << "# " << m->title << "\n\n";
                out << m->text;
            }
            std::free(path);
        }));

        menu->addChild(createMenuItem("Export as Text (.txt)", "", [m]() {
            std::string def = (!m->title.empty() ? m->title : std::string("notes")) + ".txt";
            char* path = osdialog_file(OSDIALOG_SAVE, NULL, def.c_str(), NULL);
            if (!path) return;
            std::ofstream out(path);
            if (out) {
                // Strip marker chars for plain-text output
                std::string src;
                if (!m->title.empty()) src = m->title + "\n\n" + m->text;
                else src = m->text;
                // Quick strip: remove `*`, `_`, backticks, leading `#`s, `---` lines
                std::string stripped;
                stripped.reserve(src.size());
                bool atLineStart = true;
                for (size_t i = 0; i < src.size(); i++) {
                    char c = src[i];
                    if (atLineStart) {
                        // strip leading #'s + space
                        size_t j = i;
                        int hashes = 0;
                        while (j < src.size() && src[j] == '#' && hashes < 6) { j++; hashes++; }
                        if (hashes > 0 && j < src.size() && src[j] == ' ') { i = j; continue; }
                    }
                    if (c == '*' || c == '_' || c == '`') continue;
                    stripped += c;
                    atLineStart = (c == '\n');
                }
                out << stripped;
            }
            std::free(path);
        }));

        menu->addChild(new ui::MenuSeparator);

        menu->addChild(createMenuItem("Clear text", "", [m, this]() {
            if (field) { field->pushUndo(true); field->text.clear();
                         field->cursor = field->selection = 0;
                         field->invalidateRows(); field->writeBack(); }
            m->text = "";
        }));
        menu->addChild(createBoolPtrMenuItem("Hide logo", "", &m->hideLogo));
        menu->addChild(new ui::MenuSeparator);
        menu->addChild(createMenuItem("Dark mode (shared)",
            CHECKMARK(lc::theme.dark), []() {
                lc::theme.dark = !lc::theme.dark;
                lc::saveTheme();
            }));
    }
};

Model* modelNotes = createModel<NotesModule, NotesWidget>("Notes");
