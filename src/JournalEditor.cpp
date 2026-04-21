#include "JournalEditor.hpp"
#include "Theme.hpp"
#include "plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

// ─── Small colour helpers — match the old Notes palette ────────────────────

static NVGcolor fgColor()  { return lc::theme.dark ? nvgRGB(235, 235, 235) : nvgRGB(20, 20, 20); }
static NVGcolor selColor() { return lc::theme.dark ? nvgRGBA(255, 255, 255, 60) : nvgRGBA(0, 0, 0, 50); }
static NVGcolor codeBg()   { return lc::theme.dark ? nvgRGBA(255, 255, 255, 25) : nvgRGBA(0, 0, 0, 20); }

// ─── Fonts ──────────────────────────────────────────────────────────────────

static std::shared_ptr<window::Font> getTextFont() {
    // Custom font shipped in res/
    std::string path = asset::plugin(pluginInstance, "res/_suisseSans_e8ec54.ttf");
    return APP->window->loadFont(path);
}
static std::shared_ptr<window::Font> getMonoFont() {
    return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
}

static float headingScale(int lvl) {
    switch (lvl) {
        case 1: return 1.75f;
        case 2: return 1.45f;
        case 3: return 1.22f;
        case 4: return 1.12f;
        case 5: return 1.04f;
        case 6: return 1.00f;
        default: return 1.f;
    }
}

// ─── Word-boundary helpers (file scope so onButton + onSelectKey share) ─────

static bool isWordChar(const std::string& s, int i) {
    if (i < 0 || i >= (int)s.size()) return false;
    unsigned char c = (unsigned char)s[i];
    if (c >= 0x80) return true;   // treat non-ASCII bytes as word
    return std::isalnum(c) || c == '_' || c == '\'';
}

static int wordBoundaryLeft(const std::string& s, int i) {
    while (i > 0 && !isWordChar(s, i - 1)) i = journal::utf8Prev(s, i);
    while (i > 0 &&  isWordChar(s, i - 1)) i = journal::utf8Prev(s, i);
    return i;
}
static int wordBoundaryRight(const std::string& s, int i) {
    int n = (int)s.size();
    while (i < n &&  isWordChar(s, i)) i = journal::utf8Next(s, i);
    while (i < n && !isWordChar(s, i)) i = journal::utf8Next(s, i);
    return i;
}

// Right edge of the word containing `i` — unlike wordBoundaryRight this
// trims off the trailing non-word run so a word select doesn't include the
// space after it.
static int wordEndAt(const std::string& s, int i) {
    int r = wordBoundaryRight(s, i);
    while (r > i && r > 0 && !isWordChar(s, r - 1))
        r = journal::utf8Prev(s, r);
    return r;
}

// ─── JournalEditor ──────────────────────────────────────────────────────────

JournalEditor::JournalEditor() {
    box.size = math::Vec(80.f, 60.f);
}

void JournalEditor::setMarkdown(const std::string& md) {
    doc = journal::fromMarkdown(md);
    sel = journal::Selection::caret(doc.startPos());
    scroll = 0.f;
    invalidateRows();
}

void JournalEditor::configureFont(NVGcontext* vg, uint8_t marks, float size) {
    auto f = (marks & journal::MARK_CODE) ? getMonoFont() : getTextFont();
    if (f && f->handle) nvgFontFaceId(vg, f->handle);
    float fs = (marks & journal::MARK_CODE) ? size * 0.94f : size;
    nvgFontSize(vg, fs);
}

std::vector<JournalEditor::Run>
JournalEditor::runsForRange(const journal::Block& b, int from, int to) {
    std::vector<Run> out;
    from = std::max(0, from);
    to   = std::min((int)b.text.size(), to);
    int i = from;
    while (i < to) {
        uint8_t m = b.markAt(i);
        int j = i + 1;
        while (j < to && b.markAt(j) == m) j++;
        out.push_back({i, j, m});
        i = j;
    }
    return out;
}

// ─── Layout ─────────────────────────────────────────────────────────────────

void JournalEditor::rebuildRows(NVGcontext* vg) {
    rows.clear();
    float y = textOffset.y;
    float innerW = std::max(8.f, box.size.x - textOffset.x * 2.f);

    // Ordered-list numbering by depth, reset when the sequence breaks.
    int orderedCounter[9] = {0};

    for (int bi = 0; bi < doc.nBlocks(); bi++) {
        const journal::Block& b = doc.at(bi);

        if (b.type != journal::BLOCK_ORDERED)
            for (int& c : orderedCounter) c = 0;

        float baseFS = fontSize;
        float depthPad = 0.f;
        std::string markerText;
        float markerW = 0.f;
        const float INDENT_PX = 14.f;

        if (b.type == journal::BLOCK_HEADING) {
            baseFS = fontSize * headingScale(b.meta);
        } else if (b.type == journal::BLOCK_BULLET) {
            depthPad = (float)b.meta * INDENT_PX;
            markerText = "•  ";
        } else if (b.type == journal::BLOCK_ORDERED) {
            int d = b.meta > 8 ? 8 : b.meta;
            for (int j = d + 1; j < 9; j++) orderedCounter[j] = 0;
            orderedCounter[d]++;
            depthPad = (float)d * INDENT_PX;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d.  ", orderedCounter[d]);
            markerText = buf;
        } else if (b.type == journal::BLOCK_HR) {
            Row r; r.blockIdx = bi; r.byteStart = 0; r.byteEnd = 0;
            r.y = y; r.height = baseFS * lineSpacing;
            r.leftPad = 0.f; r.rowFontSize = baseFS; r.lastOfBlock = true;
            rows.push_back(r);
            y += r.height;
            continue;
        }

        if (!markerText.empty()) {
            configureFont(vg, 0, baseFS);
            float bb[4];
            nvgTextBounds(vg, 0, 0, markerText.c_str(), nullptr, bb);
            markerW = bb[2] - bb[0];
        }
        const float leftPad = depthPad + markerW;

        float rowWidth = std::max(8.f, innerW - leftPad);

        configureFont(vg, 0, baseFS);
        const char* start = b.text.c_str();
        const char* end   = b.text.c_str() + b.text.size();
        NVGtextRow wrapRows[256];
        int nrows = 0;
        if (end > start)
            nrows = nvgTextBreakLines(vg, start, end, rowWidth, wrapRows, 256);

        auto pushRow = [&](int byteStart, int byteEnd, bool last, bool first) {
            Row r;
            r.blockIdx    = bi;
            r.byteStart   = byteStart;
            r.byteEnd     = byteEnd;
            r.y           = y;
            r.height      = baseFS * lineSpacing;
            r.leftPad     = leftPad;
            r.rowFontSize = baseFS;
            r.lastOfBlock = last;
            if (first && !markerText.empty()) {
                r.markerText = markerText;
                r.markerX    = depthPad;
            }
            rows.push_back(r);
            y += r.height;
        };

        if (nrows == 0) {
            pushRow(0, 0, true, true);
            continue;
        }
        for (int ri = 0; ri < nrows; ri++) {
            pushRow((int)(wrapRows[ri].start - start),
                    (int)(wrapRows[ri].end   - start),
                    ri == nrows - 1,
                    ri == 0);
        }
    }

    rowsDirty = false;
}

// ─── Scrolling helpers ──────────────────────────────────────────────────────

void JournalEditor::ensureCaretVisible() {
    if (rows.empty()) return;
    int ri = rowIndexOf(sel.head);
    if (ri < 0) return;
    const Row& r = rows[ri];
    float top = r.y;
    float bot = r.y + r.height;
    float margin = 4.f;
    if (top - scroll < margin)
        scroll = top - margin;
    else if (bot - scroll > box.size.y - margin)
        scroll = bot - box.size.y + margin;
    scroll = std::max(0.f, scroll);
}

// ─── Row lookup ─────────────────────────────────────────────────────────────

int JournalEditor::rowIndexOf(const journal::Pos& p) {
    int best = -1;
    for (int i = 0; i < (int)rows.size(); i++) {
        const Row& r = rows[i];
        if (r.blockIdx != p.block) continue;
        best = i;
        if (p.offset >= r.byteStart && p.offset <= r.byteEnd) {
            // If on a row boundary, prefer the row that CONTAINS offset
            // (end-of-row vs start-of-next-row) using "last of block" rule.
            if (p.offset == r.byteEnd && !r.lastOfBlock
                && i + 1 < (int)rows.size() && rows[i + 1].blockIdx == p.block)
                continue;
            return i;
        }
    }
    return best;
}

// ─── X coordinate of a position within its row ─────────────────────────────

float JournalEditor::xOfPos(NVGcontext* vg, const journal::Pos& p) {
    int ri = rowIndexOf(p);
    if (ri < 0) return textOffset.x;
    const Row& r = rows[ri];
    const journal::Block& b = doc.at(r.blockIdx);
    int measureTo = std::min((int)p.offset, r.byteEnd);
    measureTo = std::max(measureTo, r.byteStart);

    float x = textOffset.x + r.leftPad;
    auto runs = runsForRange(b, r.byteStart, measureTo);
    for (const auto& run : runs) {
        configureFont(vg, run.marks, r.rowFontSize);
        float b4[4];
        nvgTextBounds(vg, 0, 0,
                      b.text.c_str() + run.start,
                      b.text.c_str() + run.end, b4);
        x += (b4[2] - b4[0]);
    }
    return x;
}

// ─── Mouse hit-test ─────────────────────────────────────────────────────────

journal::Pos JournalEditor::posFromLocal(NVGcontext* vg, math::Vec localPos) {
    if (rows.empty()) return doc.startPos();
    float y = localPos.y + scroll;

    // Pick the row by y.
    int ri = -1;
    for (int i = 0; i < (int)rows.size(); i++) {
        const Row& r = rows[i];
        if (y < r.y + r.height) { ri = i; break; }
    }
    if (ri < 0) ri = (int)rows.size() - 1;

    const Row& r = rows[ri];
    const journal::Block& b = doc.at(r.blockIdx);

    if (b.type == journal::BLOCK_HR) return {r.blockIdx, 0};

    // Walk runs until we cross the target x.
    float x = textOffset.x + r.leftPad;
    float tx = localPos.x;
    if (tx < x) return {r.blockIdx, r.byteStart};

    auto runs = runsForRange(b, r.byteStart, r.byteEnd);
    for (const auto& run : runs) {
        configureFont(vg, run.marks, r.rowFontSize);
        int i = run.start;
        while (i < run.end) {
            int next = journal::utf8Next(b.text, i);
            float glyph[4];
            nvgTextBounds(vg, 0, 0, b.text.c_str() + i, b.text.c_str() + next, glyph);
            float w = glyph[2] - glyph[0];
            if (tx < x + w * 0.5f) return {r.blockIdx, i};
            x += w;
            i = next;
        }
    }
    return {r.blockIdx, r.byteEnd};
}

// ─── Drawing ────────────────────────────────────────────────────────────────

void JournalEditor::draw(const DrawArgs& args) {
    if (rowsDirty) rebuildRows(args.vg);

    nvgScissor(args.vg, 0, 0, box.size.x, box.size.y);
    nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

    // Placeholder
    bool focused = APP->event->getSelectedWidget() == this;
    bool docEmpty = (doc.nBlocks() == 1 && doc.at(0).length() == 0 && doc.at(0).type == journal::BLOCK_PARAGRAPH);
    if (docEmpty && !placeholder.empty() && !focused) {
        configureFont(args.vg, 0, fontSize);
        NVGcolor c = fgColor();
        nvgFillColor(args.vg, nvgRGBA((int)(c.r*255), (int)(c.g*255), (int)(c.b*255), 95));
        nvgText(args.vg, textOffset.x, textOffset.y, placeholder.c_str(), NULL);
        nvgResetScissor(args.vg);
        return;
    }

    journal::Pos from = sel.from();
    journal::Pos to   = sel.to();

    for (int i = 0; i < (int)rows.size(); i++) {
        const Row& r = rows[i];
        float y = r.y - scroll;
        if (y + r.height < 0.f) continue;
        if (y > box.size.y) break;

        const journal::Block& b = doc.at(r.blockIdx);

        // HR row
        if (b.type == journal::BLOCK_HR) {
            NVGcolor c = fgColor();
            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, textOffset.x, y + r.height * 0.5f);
            nvgLineTo(args.vg, box.size.x - textOffset.x, y + r.height * 0.5f);
            nvgStrokeColor(args.vg, nvgRGBA((int)(c.r*255), (int)(c.g*255), (int)(c.b*255), 150));
            nvgStrokeWidth(args.vg, 1.f);
            nvgStroke(args.vg);
            continue;
        }

        // Selection highlight for this row (covers the range intersection).
        bool rowInSel = !sel.isCollapsed() && from.block <= r.blockIdx && r.blockIdx <= to.block;
        if (rowInSel) {
            int s = (r.blockIdx == from.block) ? std::max(from.offset, r.byteStart) : r.byteStart;
            int e = (r.blockIdx == to.block)   ? std::min(to.offset,   r.byteEnd)   : r.byteEnd;
            if (e > s) {
                journal::Pos ps{r.blockIdx, s};
                journal::Pos pe{r.blockIdx, e};
                float xS = xOfPos(args.vg, ps);
                float xE = xOfPos(args.vg, pe);
                nvgBeginPath(args.vg);
                nvgRect(args.vg, xS, y, std::max(2.f, xE - xS), r.height);
                nvgFillColor(args.vg, selColor());
                nvgFill(args.vg);
            }
        }

        // List marker — first row of a bullet / ordered block.
        if (!r.markerText.empty()) {
            configureFont(args.vg, 0, r.rowFontSize);
            nvgFillColor(args.vg, fgColor());
            nvgText(args.vg, textOffset.x + r.markerX, y,
                    r.markerText.c_str(), nullptr);
        }

        float x = textOffset.x + r.leftPad;

        // Draw runs.
        auto runs = runsForRange(b, r.byteStart, r.byteEnd);
        for (const auto& run : runs) {
            configureFont(args.vg, run.marks, r.rowFontSize);
            float bb[4];
            nvgTextBounds(args.vg, 0, 0,
                          b.text.c_str() + run.start,
                          b.text.c_str() + run.end, bb);
            float w = bb[2] - bb[0];

            if (run.marks & journal::MARK_CODE) {
                nvgBeginPath(args.vg);
                nvgRect(args.vg, x - 1.f, y - 1.f, w + 2.f, r.rowFontSize + 3.f);
                nvgFillColor(args.vg, codeBg());
                nvgFill(args.vg);
            }

            nvgFillColor(args.vg, fgColor());

            auto drawGlyph = [&](float dx) {
                if (run.marks & journal::MARK_ITALIC) {
                    nvgSave(args.vg);
                    float skew = 0.2f;
                    float yBaseline = y + r.rowFontSize;
                    nvgTransform(args.vg, 1, 0, -skew, 1, skew * yBaseline, 0);
                    nvgText(args.vg, x + dx, y, b.text.c_str() + run.start, b.text.c_str() + run.end);
                    nvgRestore(args.vg);
                } else {
                    nvgText(args.vg, x + dx, y, b.text.c_str() + run.start, b.text.c_str() + run.end);
                }
            };
            // Headings render at normal weight by default; size alone carries
            // the hierarchy. Users can still cmd+B any span (heading or body).
            drawGlyph(0.f);
            if (run.marks & journal::MARK_BOLD) drawGlyph(0.5f);

            x += w;
        }
    }

    // Caret
    if (focused && sel.isCollapsed()) {
        // macOS-like blink: 530ms on, 530ms off.
        double t = rack::system::getTime();
        double phase = std::fmod(t - lastCursorT, 1.06);
        bool on = phase < 0.53;
        if (on) {
            int ri = rowIndexOf(sel.head);
            if (ri >= 0) {
                const Row& r = rows[ri];
                float y = r.y - scroll;
                float x = xOfPos(args.vg, sel.head);
                nvgBeginPath(args.vg);
                nvgRect(args.vg, x, y, 1.f, r.rowFontSize + 2.f);
                nvgFillColor(args.vg, fgColor());
                nvgFill(args.vg);
            }
        }
    }

    nvgResetScissor(args.vg);
}

// ─── Lifecycle ──────────────────────────────────────────────────────────────

void JournalEditor::step() {
    OpaqueWidget::step();
    // Invalidate row cache if the box width changed since last step.
    if (box.size.x != lastBoxW) { invalidateRows(); lastBoxW = box.size.x; }
}

// ─── Input ──────────────────────────────────────────────────────────────────

static bool hasCmd(int mods)   { return mods & RACK_MOD_CTRL; }
static bool hasShift(int mods) { return mods & GLFW_MOD_SHIFT; }
static bool hasAlt(int mods)   { return mods & GLFW_MOD_ALT; }

void JournalEditor::onButton(const event::Button& e) {
    OpaqueWidget::onButton(e);
    if (e.isConsumed()) return;
    if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
        APP->event->setSelectedWidget(this);
        NVGcontext* vg = APP->window->vg;
        if (rowsDirty) rebuildRows(vg);
        journal::Pos p = posFromLocal(vg, e.pos);
        p = doc.clampPos(p);

        // Multi-click streak tracking for word/line select.
        double now = rack::system::getTime();
        bool near = (e.pos.minus(lastClickPos).norm() < 4.f);
        if (near && (now - lastClickT) < 0.4) clickStreak++;
        else clickStreak = 1;
        lastClickT = now;
        lastClickPos = e.pos;

        if (hasShift(e.mods)) {
            // Shift-click extends the existing selection; keep the current
            // dragMode so shift-drag behaves consistently.
            sel.head = p;
        } else if (clickStreak >= 3) {
            // Triple-click: whole block. Pivot is the block bounds — drag
            // snaps selection to whole blocks.
            pivotFrom = {p.block, 0};
            pivotTo   = {p.block, doc.at(p.block).length()};
            sel.anchor = pivotFrom;
            sel.head   = pivotTo;
            dragMode = DragMode::BLOCK;
        } else if (clickStreak == 2) {
            // Double-click: select word at click. Pivot is that word — drag
            // snaps selection to word boundaries.
            const journal::Block& b = doc.at(p.block);
            int wStart = wordBoundaryLeft(b.text, p.offset);
            int wEnd   = wordEndAt(b.text, p.offset);
            pivotFrom = {p.block, wStart};
            pivotTo   = {p.block, wEnd};
            sel.anchor = pivotFrom;
            sel.head   = pivotTo;
            dragMode = DragMode::WORD;
        } else {
            sel = journal::Selection::caret(p);
            pivotFrom = pivotTo = p;
            dragMode = DragMode::CARET;
        }
        hasPendingMarks = false;
        preferredX = -1.f;
        lastCursorT = rack::system::getTime();
        e.consume(this);
    }
}

// Order two Pos values — returns true if a comes strictly before b.
static bool posLess(const journal::Pos& a, const journal::Pos& b) {
    if (a.block != b.block) return a.block < b.block;
    return a.offset < b.offset;
}

void JournalEditor::onDragHover(const event::DragHover& e) {
    OpaqueWidget::onDragHover(e);
    // Only extend selection when the drag started on us (left button).
    if (e.origin != this) return;
    NVGcontext* vg = APP->window->vg;
    if (rowsDirty) rebuildRows(vg);
    journal::Pos p = posFromLocal(vg, e.pos);
    p = doc.clampPos(p);

    // Moving far from the original press invalidates the click streak so
    // a click after this drag isn't misread as a double / triple.
    if (e.pos.minus(lastClickPos).norm() > 8.f) lastClickT = -1.0;

    switch (dragMode) {
        case DragMode::CARET:
            sel.head = p;
            break;

        case DragMode::WORD: {
            const journal::Block& b = doc.at(p.block);
            int wStart = wordBoundaryLeft(b.text, p.offset);
            int wEnd   = wordEndAt(b.text, p.offset);
            journal::Pos pw_start{p.block, wStart};
            journal::Pos pw_end  {p.block, wEnd};
            if (posLess(p, pivotFrom)) {
                // Pointer is left of pivot → anchor = pivot end, head = word start.
                sel.anchor = pivotTo;
                sel.head   = pw_start;
            } else if (posLess(pivotTo, p)) {
                sel.anchor = pivotFrom;
                sel.head   = pw_end;
            } else {
                sel.anchor = pivotFrom;
                sel.head   = pivotTo;
            }
            break;
        }

        case DragMode::BLOCK: {
            int b = p.block;
            if (b < pivotFrom.block) {
                sel.anchor = pivotTo;
                sel.head   = {b, 0};
            } else if (b > pivotTo.block) {
                sel.anchor = pivotFrom;
                sel.head   = {b, doc.at(b).length()};
            } else {
                sel.anchor = pivotFrom;
                sel.head   = pivotTo;
            }
            break;
        }
    }

    hasPendingMarks = false;
    preferredX = -1.f;
    lastCursorT = rack::system::getTime();
}

void JournalEditor::onSelectText(const event::SelectText& e) {
    if (e.codepoint < 32 || e.codepoint == 127) return;

    char buf[8]; int n = 0;
    uint32_t c = e.codepoint;
    if (c < 0x80)          { buf[n++] = c; }
    else if (c < 0x800)    { buf[n++] = 0xC0 | (c >> 6);  buf[n++] = 0x80 | (c & 0x3F); }
    else if (c < 0x10000)  { buf[n++] = 0xE0 | (c >> 12); buf[n++] = 0x80 | ((c >> 6) & 0x3F); buf[n++] = 0x80 | (c & 0x3F); }
    else                   { buf[n++] = 0xF0 | (c >> 18); buf[n++] = 0x80 | ((c >> 12) & 0x3F); buf[n++] = 0x80 | ((c >> 6) & 0x3F); buf[n++] = 0x80 | (c & 0x3F); }

    cmdInsertText(std::string(buf, n));
    e.consume(this);
}

void JournalEditor::onSelectKey(const event::SelectKey& e) {
    if (e.action != GLFW_PRESS && e.action != GLFW_REPEAT) return;

    bool shift = hasShift(e.mods);
    bool cmd   = hasCmd(e.mods);
    bool alt   = hasAlt(e.mods);

    auto setCaret = [&](journal::Pos p) {
        p = doc.clampPos(p);
        if (shift) sel.head = p;
        else       sel = journal::Selection::caret(p);
        preferredX = -1.f;
        hasPendingMarks = false;
        lastCursorT = rack::system::getTime();
    };

    NVGcontext* vg = APP->window->vg;
    if (rowsDirty) rebuildRows(vg);

    auto visualMove = [&](int dir /* -1 up, +1 down */) {
        if (rows.empty()) return;
        int ri = rowIndexOf(sel.head);
        if (ri < 0) return;
        if (preferredX < 0.f) preferredX = xOfPos(vg, sel.head);
        int nri = ri + dir;
        if (nri < 0 || nri >= (int)rows.size()) {
            setCaret(dir < 0 ? doc.startPos() : doc.endPos());
            return;
        }
        const Row& r = rows[nri]; (void)r;
        const journal::Block& b = doc.at(rows[nri].blockIdx);
        if (b.type == journal::BLOCK_HR) {
            nri += dir;
            if (nri < 0 || nri >= (int)rows.size()) {
                setCaret(dir < 0 ? doc.startPos() : doc.endPos());
                return;
            }
        }
        const Row& tgt = rows[nri];
        math::Vec hit(preferredX, tgt.y - scroll + tgt.height * 0.5f);
        journal::Pos p = posFromLocal(vg, hit);
        if (shift) sel.head = doc.clampPos(p);
        else       sel      = journal::Selection::caret(doc.clampPos(p));
        hasPendingMarks = false;
        lastCursorT = rack::system::getTime();
    };

    // ── Cmd + key ────────────────────────────────────────────────────────
    if (cmd && !alt) {
        switch (e.key) {
            case GLFW_KEY_A: cmdSelectAll(); e.consume(this); return;
            case GLFW_KEY_C: cmdCopy();      e.consume(this); return;
            case GLFW_KEY_X: cmdCut();       e.consume(this); return;
            case GLFW_KEY_V: cmdPaste();     e.consume(this); return;
            case GLFW_KEY_Z:
                if (shift) redo(); else undo();
                e.consume(this); return;
            case GLFW_KEY_B: cmdToggleInlineMark(journal::MARK_BOLD);   e.consume(this); return;
            case GLFW_KEY_I: cmdToggleInlineMark(journal::MARK_ITALIC); e.consume(this); return;
            case GLFW_KEY_E: cmdToggleInlineMark(journal::MARK_CODE);   e.consume(this); return;

            case GLFW_KEY_RIGHT_BRACKET:
                if (shift) { cmdChangeHeading(+1); e.consume(this); return; }
                break;
            case GLFW_KEY_LEFT_BRACKET:
                if (shift) { cmdChangeHeading(-1); e.consume(this); return; }
                break;

            case GLFW_KEY_LEFT: {
                // Cmd+Left = line start (row start).
                int ri = rowIndexOf(sel.head);
                if (ri >= 0) setCaret({rows[ri].blockIdx, rows[ri].byteStart});
                e.consume(this); return;
            }
            case GLFW_KEY_RIGHT: {
                int ri = rowIndexOf(sel.head);
                if (ri >= 0) setCaret({rows[ri].blockIdx, rows[ri].byteEnd});
                e.consume(this); return;
            }
            case GLFW_KEY_UP:    setCaret(doc.startPos()); e.consume(this); return;
            case GLFW_KEY_DOWN:  setCaret(doc.endPos());   e.consume(this); return;
        }
    }

    // ── Alt + key ────────────────────────────────────────────────────────
    if (alt && !cmd) {
        switch (e.key) {
            case GLFW_KEY_LEFT: {
                journal::Pos p = sel.head;
                if (p.offset > 0) p.offset = wordBoundaryLeft(doc.at(p.block).text, p.offset);
                else if (p.block > 0) { p.block--; p.offset = doc.at(p.block).length(); }
                setCaret(p);
                e.consume(this); return;
            }
            case GLFW_KEY_RIGHT: {
                journal::Pos p = sel.head;
                int len = doc.at(p.block).length();
                if (p.offset < len) p.offset = wordBoundaryRight(doc.at(p.block).text, p.offset);
                else if (p.block + 1 < doc.nBlocks()) { p.block++; p.offset = 0; }
                setCaret(p);
                e.consume(this); return;
            }
            case GLFW_KEY_BACKSPACE:
                cmdDeleteWordBack();
                e.consume(this); return;
        }
    }

    // ── Plain / Shift + key ──────────────────────────────────────────────
    switch (e.key) {
        case GLFW_KEY_LEFT: {
            if (!shift && !sel.isCollapsed()) { setCaret(sel.from()); break; }
            journal::Pos p = sel.head;
            if (p.offset > 0) p.offset = journal::utf8Prev(doc.at(p.block).text, p.offset);
            else if (p.block > 0) { p.block--; p.offset = doc.at(p.block).length(); }
            setCaret(p);
            break;
        }
        case GLFW_KEY_RIGHT: {
            if (!shift && !sel.isCollapsed()) { setCaret(sel.to()); break; }
            journal::Pos p = sel.head;
            int len = doc.at(p.block).length();
            if (p.offset < len) p.offset = journal::utf8Next(doc.at(p.block).text, p.offset);
            else if (p.block + 1 < doc.nBlocks()) { p.block++; p.offset = 0; }
            setCaret(p);
            break;
        }
        case GLFW_KEY_UP:    visualMove(-1); break;
        case GLFW_KEY_DOWN:  visualMove(+1); break;

        case GLFW_KEY_HOME: {
            int ri = rowIndexOf(sel.head);
            if (ri >= 0) setCaret({rows[ri].blockIdx, rows[ri].byteStart});
            break;
        }
        case GLFW_KEY_END: {
            int ri = rowIndexOf(sel.head);
            if (ri >= 0) setCaret({rows[ri].blockIdx, rows[ri].byteEnd});
            break;
        }

        case GLFW_KEY_BACKSPACE: cmdBackspace();   break;
        case GLFW_KEY_DELETE:    cmdDelete();      break;

        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER:
            cmdSplitBlock();
            break;

        case GLFW_KEY_TAB: {
            const journal::Block& cur = doc.at(sel.head.block);
            bool inList = (cur.type == journal::BLOCK_BULLET
                        || cur.type == journal::BLOCK_ORDERED);
            if (inList) {
                cmdIndentList(shift ? -1 : +1);
                break;
            }
            if (shift) return;          // let Rack's focus-prev handle it
            cmdInsertText("\t");        // plain Tab in non-list → literal tab
            break;
        }

        default:
            return;
    }
    e.consume(this);
}

void JournalEditor::onHoverScroll(const event::HoverScroll& e) {
    if (rowsDirty) rebuildRows(APP->window->vg);
    float total = rows.empty() ? 0.f : rows.back().y + rows.back().height;
    float maxScroll = std::max(0.f, total - box.size.y + textOffset.y);
    scroll = math::clamp(scroll - e.scrollDelta.y, 0.f, maxScroll);
    e.consume(this);
}

// ─── Persistence ────────────────────────────────────────────────────────────

void JournalEditor::markChanged() {
    if (onChange) onChange();
}

// ─── Undo / redo ────────────────────────────────────────────────────────────

void JournalEditor::pushUndo(EditKind kind) {
    double now = rack::system::getTime();
    // Any new edit invalidates the redo chain.
    redoStack.clear();
    // Coalesce consecutive typing / deleting within 500ms — one undo step
    // per run of characters feels right for writing.
    if (!undoStack.empty()
        && kind != EditKind::OTHER
        && kind == lastEditKind
        && (now - lastEditT) < 0.5) {
        lastEditT = now;
        return;
    }
    Snapshot s{doc, sel};
    undoStack.push_back(std::move(s));
    if (undoStack.size() > 200) undoStack.erase(undoStack.begin());
    lastEditKind = kind;
    lastEditT = now;
}

void JournalEditor::undo() {
    if (undoStack.empty()) return;
    Snapshot cur{doc, sel};
    redoStack.push_back(std::move(cur));
    Snapshot prev = undoStack.back();
    undoStack.pop_back();
    doc = std::move(prev.doc);
    sel = prev.sel;
    hasPendingMarks = false;
    pendingMarks = 0;
    lastEditKind = EditKind::OTHER;
    invalidateRows();
    ensureCaretVisible();
    markChanged();
}

void JournalEditor::redo() {
    if (redoStack.empty()) return;
    Snapshot cur{doc, sel};
    undoStack.push_back(std::move(cur));
    Snapshot nxt = redoStack.back();
    redoStack.pop_back();
    doc = std::move(nxt.doc);
    sel = nxt.sel;
    hasPendingMarks = false;
    pendingMarks = 0;
    lastEditKind = EditKind::OTHER;
    invalidateRows();
    ensureCaretVisible();
    markChanged();
}

// ─── Commands ───────────────────────────────────────────────────────────────

void JournalEditor::cmdInsertText(const std::string& s) {
    if (s.empty()) return;
    pushUndo(EditKind::TYPING);

    // Pick the marks to apply to the inserted bytes.
    uint8_t marks;
    if (hasPendingMarks)        marks = pendingMarks;
    else                        marks = journal::marksAtSelection(doc, sel);

    journal::insertText(doc, sel, s, marks);
    hasPendingMarks = false;
    pendingMarks = 0;

    // Apply Markdown triggers (e.g. "- " at line start → bullet). Only runs
    // when we just typed a single byte that could be a trigger tail.
    if (s.size() == 1) maybeApplyTriggers(s[0]);

    preferredX = -1.f;
    lastCursorT = rack::system::getTime();
    invalidateRows();
    ensureCaretVisible();
    markChanged();
}

void JournalEditor::cmdBackspace() {
    pushUndo(EditKind::DELETING);
    journal::deleteBackward(doc, sel);
    hasPendingMarks = false;
    preferredX = -1.f;
    invalidateRows();
    ensureCaretVisible();
    markChanged();
}

void JournalEditor::cmdDelete() {
    pushUndo(EditKind::DELETING);
    journal::deleteForward(doc, sel);
    hasPendingMarks = false;
    preferredX = -1.f;
    invalidateRows();
    ensureCaretVisible();
    markChanged();
}

void JournalEditor::cmdSplitBlock() {
    pushUndo(EditKind::OTHER);
    journal::splitBlock(doc, sel);
    hasPendingMarks = false;
    preferredX = -1.f;
    invalidateRows();
    ensureCaretVisible();
    markChanged();
}

void JournalEditor::cmdToggleInlineMark(uint8_t mark) {
    if (!sel.isCollapsed()) {
        pushUndo(EditKind::OTHER);
        uint8_t present = journal::marksAtSelection(doc, sel);
        bool allOn = (present & mark) == mark;
        journal::setMarks(doc, sel, mark, !allOn);
        invalidateRows();
        markChanged();
        return;
    }
    // Collapsed: stage for next typed char.
    uint8_t base = hasPendingMarks ? pendingMarks : journal::marksAtSelection(doc, sel);
    base ^= mark;
    pendingMarks = base;
    hasPendingMarks = true;
}

void JournalEditor::cmdChangeHeading(int direction) {
    pushUndo(EditKind::OTHER);
    journal::Pos anchor = sel.anchor, head = sel.head;
    journal::Block& b = doc.at(head.block);
    int curLvl = (b.type == journal::BLOCK_HEADING) ? (int)b.meta : 0;
    int newLvl;
    if (direction > 0) {
        if (curLvl == 0) newLvl = 3;
        else if (curLvl <= 1) newLvl = 1;
        else newLvl = curLvl - 1;
    } else {
        if (curLvl == 0) { return; }
        else if (curLvl >= 6) newLvl = 0;
        else newLvl = curLvl + 1;
    }
    if (newLvl == 0) journal::setBlockType(doc, sel, journal::BLOCK_PARAGRAPH, 0);
    else             journal::setBlockType(doc, sel, journal::BLOCK_HEADING, (uint8_t)newLvl);
    sel.anchor = anchor; sel.head = head;
    invalidateRows();
    markChanged();
}

void JournalEditor::cmdIndentList(int direction) {
    pushUndo(EditKind::OTHER);
    journal::indentList(doc, sel, direction);
    invalidateRows();
    markChanged();
}

void JournalEditor::cmdInsertHR() {
    pushUndo(EditKind::OTHER);
    journal::insertHR(doc, sel);
    invalidateRows();
    ensureCaretVisible();
    markChanged();
}

void JournalEditor::cmdSelectAll() {
    sel.anchor = doc.startPos();
    sel.head   = doc.endPos();
    preferredX = -1.f;
}

void JournalEditor::cmdCopy() {
    std::string s = journal::textOfSelection(doc, sel);
    if (!s.empty()) glfwSetClipboardString(APP->window->win, s.c_str());
}

void JournalEditor::cmdCut() {
    if (sel.isCollapsed()) return;
    cmdCopy();
    pushUndo(EditKind::OTHER);
    journal::deleteBackward(doc, sel);  // ranged → collapse-delete
    invalidateRows();
    ensureCaretVisible();
    markChanged();
}

void JournalEditor::cmdPaste() {
    const char* cs = glfwGetClipboardString(APP->window->win);
    if (!cs || !*cs) return;
    cmdInsertText(cs);
}

void JournalEditor::cmdDeleteWordBack() {
    if (!sel.isCollapsed()) { cmdBackspace(); return; }
    journal::Pos p = sel.head;
    if (p.offset == 0) { cmdBackspace(); return; }
    int newOff = wordBoundaryLeft(doc.at(p.block).text, p.offset);
    sel.anchor = {p.block, newOff};
    sel.head   = p;
    cmdBackspace();
}

// ─── Triggers ───────────────────────────────────────────────────────────────
// Called from cmdInsertText after a single-char insert. Returns true if a
// trigger fired (caller doesn't need to know but the bool is handy in tests).

bool JournalEditor::maybeApplyTriggers(char lastChar) {
    if (!sel.isCollapsed()) return false;
    journal::Pos p = sel.head;
    journal::Block& b = doc.at(p.block);

    // "---" on its own line → HR + paragraph below.
    if (lastChar == '-'
        && b.type == journal::BLOCK_PARAGRAPH
        && b.text == "---") {
        pushUndo(EditKind::OTHER);
        b.text.clear();
        b.marks.clear();
        b.type = journal::BLOCK_HR;
        journal::Block para;
        doc.blocks.insert(doc.blocks.begin() + p.block + 1, para);
        sel = journal::Selection::caret({p.block + 1, 0});
        invalidateRows();
        markChanged();
        return true;
    }

    // Space-triggered: "# " / "## " … / "- " / "1. " / "42. " at line start.
    if (lastChar == ' '
        && b.type == journal::BLOCK_PARAGRAPH
        && p.offset >= 2) {
        // Text up to and including the space, but we care about what's
        // before the space.
        std::string beforeSpace = b.text.substr(0, p.offset - 1);

        // Heading: N hashes (1-6)
        int hashes = 0;
        while (hashes < 6 && hashes < (int)beforeSpace.size() && beforeSpace[hashes] == '#') hashes++;
        if (hashes > 0 && hashes == (int)beforeSpace.size()) {
            pushUndo(EditKind::OTHER);
            b.type = journal::BLOCK_HEADING;
            b.meta = (uint8_t)hashes;
            int eraseLen = hashes + 1;  // N hashes + space
            b.text.erase(0, eraseLen);
            b.marks.erase(b.marks.begin(), b.marks.begin() + eraseLen);
            sel = journal::Selection::caret({p.block, 0});
            invalidateRows();
            markChanged();
            return true;
        }

        // Bullet: "-", "*", "+"  — strip the typed marker. It's rendered
        // from structure (block type + depth), not stored in text.
        if (beforeSpace.size() == 1
            && (beforeSpace[0] == '-' || beforeSpace[0] == '*' || beforeSpace[0] == '+')) {
            pushUndo(EditKind::OTHER);
            b.type = journal::BLOCK_BULLET;
            b.meta = 0;
            int stripLen = p.offset;               // "- " (or "* "/"+ ")
            b.text.erase(0, stripLen);
            b.marks.erase(b.marks.begin(), b.marks.begin() + stripLen);
            sel = journal::Selection::caret({p.block, 0});
            invalidateRows();
            markChanged();
            return true;
        }

        // Ordered: "1. ", "42. ", etc. Same strip-the-marker approach; the
        // display number is auto-derived at render time.
        if (beforeSpace.size() >= 2 && beforeSpace.back() == '.') {
            bool allDigits = true;
            for (size_t i = 0; i + 1 < beforeSpace.size(); i++) {
                if (!std::isdigit((unsigned char)beforeSpace[i])) { allDigits = false; break; }
            }
            if (allDigits) {
                pushUndo(EditKind::OTHER);
                b.type = journal::BLOCK_ORDERED;
                b.meta = 0;
                int stripLen = p.offset;           // digits + "." + " "
                b.text.erase(0, stripLen);
                b.marks.erase(b.marks.begin(), b.marks.begin() + stripLen);
                sel = journal::Selection::caret({p.block, 0});
                invalidateRows();
                markChanged();
                return true;
            }
        }
    }

    return false;
}
