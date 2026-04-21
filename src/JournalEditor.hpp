#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// JournalEditor — the widget side of the new text system.
//
// Owns a journal::Doc internally. The host module binds to it via
// setMarkdown/getMarkdown so persistence stays simple (markdown in JSON).
// Every mutation goes through the journal:: transactions in JournalDoc —
// the widget never touches Block::text / Block::marks directly.
// ─────────────────────────────────────────────────────────────────────────────

#include <functional>
#include <rack.hpp>
#include <string>
#include <vector>

#include "JournalDoc.hpp"

using namespace rack;

struct NotesModule;

struct JournalEditor : widget::OpaqueWidget {
    // ── Model ──
    journal::Doc       doc;
    journal::Selection sel;
    uint8_t            pendingMarks    = 0;       // staged marks for next-typed-char
    bool               hasPendingMarks = false;   // if true, pendingMarks overrides context

    // ── Display config ──
    float       fontSize    = 13.f;
    float       lineSpacing = 1.35f;
    math::Vec   textOffset  = math::Vec(6.f, 6.f);
    std::string placeholder;

    // ── View state ──
    float  scroll       = 0.f;
    float  preferredX   = -1.f;    // for visual up/down nav
    double lastCursorT  = 0.0;     // for caret blink

    // ── Host ──
    NotesModule* nm = nullptr;
    std::function<void()> onChange;

    // ── Row layout cache ──
    struct Row {
        int   blockIdx;
        int   byteStart;    // inclusive, offset into block.text
        int   byteEnd;      // exclusive
        float y;
        float height;
        float leftPad;      // x indent for hanging bullets etc.
        float rowFontSize;
        bool  lastOfBlock;  // true if this row contains block.end
    };
    std::vector<Row> rows;
    bool             rowsDirty = true;

    // ── Ctor / overrides ──
    JournalEditor();

    void draw(const DrawArgs& args) override;
    void step() override;
    void onButton(const event::Button& e) override;
    void onDragHover(const event::DragHover& e) override;
    void onSelectText(const event::SelectText& e) override;
    void onSelectKey(const event::SelectKey& e) override;
    void onHoverScroll(const event::HoverScroll& e) override;

    // ── Undo history ──
    struct Snapshot {
        journal::Doc       doc;
        journal::Selection sel;
    };
    std::vector<Snapshot> undoStack;
    std::vector<Snapshot> redoStack;
    enum class EditKind : uint8_t { OTHER, TYPING, DELETING };
    EditKind lastEditKind = EditKind::OTHER;
    double   lastEditT    = 0.0;

    // Per-instance width cache: row layout depends on box width, so we
    // invalidate when it changes. Must be per-widget — two open journals can
    // have different widths and mustn't share this flag.
    float lastBoxW = -1.f;

    // Multi-click tracking for word / paragraph select.
    double lastClickT   = -1.0;
    int    clickStreak  = 0;
    math::Vec lastClickPos;

    // ── Public API ──
    void setMarkdown(const std::string& md);
    std::string getMarkdown() const { return journal::toMarkdown(doc); }
    std::string getPlainText() const { return journal::toPlainText(doc); }

    // History
    void pushUndo(EditKind kind);  // coalesces consecutive typing/deleting
    void undo();
    void redo();

    // Commands — all go through these so history stays consistent.
    void cmdInsertText(const std::string& s);
    void cmdBackspace();
    void cmdDelete();
    void cmdSplitBlock();
    void cmdToggleInlineMark(uint8_t mark);
    void cmdChangeHeading(int direction);   // +1 larger, -1 smaller
    void cmdIndentList(int direction);
    void cmdInsertHR();
    void cmdSelectAll();
    void cmdCopy();
    void cmdCut();
    void cmdPaste();
    void cmdDeleteWordBack();

    // After insertText, check for auto-conversion triggers ("- ", "# ", "---").
    bool maybeApplyTriggers(char lastChar);

    // ── Helpers ──
    void invalidateRows() { rowsDirty = true; }
    void rebuildRows(NVGcontext* vg);
    void ensureCaretVisible();
    void markChanged();

    // Maps a local-coordinate point to the closest doc position.
    journal::Pos posFromLocal(NVGcontext* vg, math::Vec localPos);
    // X coordinate of the given position within the row containing it.
    float xOfPos(NVGcontext* vg, const journal::Pos& p);
    int rowIndexOf(const journal::Pos& p);

    // Chooses font + size for a given marks byte on top of the given row size.
    void configureFont(NVGcontext* vg, uint8_t marks, float size);

    // Walks a byte range of a block into runs of uniform marks (for drawing).
    struct Run {
        int  start;
        int  end;
        uint8_t marks;
    };
    std::vector<Run> runsForRange(const journal::Block& b, int from, int to);
};
