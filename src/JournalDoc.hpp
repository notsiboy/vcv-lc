#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// journal — document model for the "journal" module's rich text.
//
// Architecture follows ProseMirror's bones:
//   • Doc       = ordered list of Blocks.
//   • Block     = typed container (paragraph, heading, bullet-item,
//                 ordered-item, horizontal-rule) holding a UTF-8 byte string
//                 and a parallel per-byte marks array.
//   • Pos       = (block index, byte offset within the block's text).
//   • Selection = { anchor, head }.
//   • Transactions are free functions that mutate Doc + Selection in place.
//
// Formatting is METADATA on bytes, not characters in the buffer. There are no
// '**' or '`' markers stored anywhere. Markdown only appears at export time
// via toMarkdown().
//
// Byte-level marks (one byte per text byte) keep editing primitives trivial:
// insertText, delete, splitBlock all manipulate text and marks with the same
// offsets. Multi-byte UTF-8 codepoints are handled as a unit by navigation
// and mark toggling — all the bytes of a codepoint carry the same mark.
//
// Scope: paragraph / heading / bullet / ordered / HR. Code blocks deliberately
// deferred. No raw-mode — since nothing markdowny is stored, there's nothing
// to "reveal".
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <vector>

namespace journal {

// Inline formatting flags. Bits combine via OR. Bold + italic is legal.
enum Mark : uint8_t {
    MARK_NONE   = 0,
    MARK_BOLD   = 1 << 0,
    MARK_ITALIC = 1 << 1,
    MARK_CODE   = 1 << 2,
    MARK_ALL    = MARK_BOLD | MARK_ITALIC | MARK_CODE,
};

enum BlockType : uint8_t {
    BLOCK_PARAGRAPH = 0,
    BLOCK_HEADING   = 1,  // meta = 1..6
    BLOCK_BULLET    = 2,  // meta = indent level (0..N)
    BLOCK_ORDERED   = 3,  // meta = indent level
    BLOCK_HR        = 4,  // empty; the caret cannot land inside
};

struct Block {
    BlockType            type = BLOCK_PARAGRAPH;
    uint8_t              meta = 0;     // heading level or list indent
    std::string          text;         // raw UTF-8 bytes, no markers
    std::vector<uint8_t> marks;        // marks.size() == text.size()

    int length() const { return (int)text.size(); }

    // Keep marks array the same size as text. Every mutator in JournalDoc.cpp
    // upholds this invariant; this helper is a fallback if someone edits
    // .text directly.
    void syncMarks(uint8_t fill = MARK_NONE) {
        if (marks.size() < text.size()) marks.resize(text.size(), fill);
        else if (marks.size() > text.size()) marks.resize(text.size());
    }

    uint8_t markAt(int i) const {
        return (i >= 0 && i < (int)marks.size()) ? marks[i] : (uint8_t)MARK_NONE;
    }
};

struct Pos {
    int block;
    int offset;  // byte offset into blocks[block].text

    Pos() : block(0), offset(0) {}
    Pos(int b, int o) : block(b), offset(o) {}

    bool operator==(const Pos& o) const { return block == o.block && offset == o.offset; }
    bool operator!=(const Pos& o) const { return !(*this == o); }
    bool operator<(const Pos& o)  const { return block < o.block || (block == o.block && offset < o.offset); }
    bool operator<=(const Pos& o) const { return *this < o || *this == o; }
};

struct Selection {
    Pos anchor;
    Pos head;

    bool isCollapsed() const { return anchor == head; }
    Pos  from()        const { return (anchor < head) ? anchor : head; }
    Pos  to()          const { return (anchor < head) ? head   : anchor; }

    static Selection caret(Pos p) { return {p, p}; }
};

struct Doc {
    std::vector<Block> blocks;

    Doc() { blocks.emplace_back(); }  // always at least one block

    int nBlocks() const { return (int)blocks.size(); }

    Block&       at(int i)       { return blocks[i]; }
    const Block& at(int i) const { return blocks[i]; }

    Pos startPos() const { return {0, 0}; }
    Pos endPos()   const {
        int b = std::max(0, (int)blocks.size() - 1);
        return {b, blocks[b].length()};
    }

    Pos clampPos(const Pos& p) const;
};

// ─── UTF-8 helpers ──────────────────────────────────────────────────────────
// Continuation bytes start 10xxxxxx. A "character" for cursor movement is one
// lead byte plus its trailing continuation bytes.

inline bool utf8IsCont(unsigned char c) { return (c & 0xC0) == 0x80; }

// Move forward one codepoint from byte `i` in `s`; returns the new index.
inline int utf8Next(const std::string& s, int i) {
    int n = (int)s.size();
    if (i < 0) i = 0;
    if (i >= n) return n;
    i++;
    while (i < n && utf8IsCont((unsigned char)s[i])) i++;
    return i;
}
inline int utf8Prev(const std::string& s, int i) {
    if (i <= 0) return 0;
    if (i > (int)s.size()) i = (int)s.size();
    i--;
    while (i > 0 && utf8IsCont((unsigned char)s[i])) i--;
    return i;
}

// ─── Transactions ───────────────────────────────────────────────────────────
// Every function that changes the document is here. The editor widget never
// edits Block::text or Block::marks directly — it always goes through one of
// these, so marks stay in sync and selections stay valid.
//
// Ranged selections always get collapsed via the edit. On return, `sel` is
// collapsed at the natural end-of-edit position.

// Replace the current selection with the given UTF-8 string. Newlines inside
// `s` split blocks. `marks` is OR'd onto every inserted byte.
void insertText(Doc& doc, Selection& sel, const std::string& s, uint8_t marks = MARK_NONE);

// Backspace. Collapses a ranged selection first. At block start, merges with
// the previous block (or, if that's an HR, removes the HR).
void deleteBackward(Doc& doc, Selection& sel);

// Forward-delete. Symmetric to deleteBackward.
void deleteForward(Doc& doc, Selection& sel);

// Split the current block at the selection head. Ranged selections are
// deleted first. Caret ends at offset 0 of the new block.
void splitBlock(Doc& doc, Selection& sel);

// Apply or clear marks across the selection. Returns the mark state of the
// affected range after the call (useful for UI state in menus).
//   on == true  → set the bits in `mask` on every byte of the selection
//   on == false → clear the bits in `mask`
// For collapsed selections this is a no-op; the editor tracks "pending marks"
// for next-typed-char itself.
void setMarks(Doc& doc, const Selection& sel, uint8_t mask, bool on);

// Change the type of every block the selection touches.
void setBlockType(Doc& doc, const Selection& sel, BlockType t, uint8_t meta = 0);

// For every list block the selection touches, clamp indent to [0,8] and add
// `delta`. Other block types are ignored.
void indentList(Doc& doc, const Selection& sel, int delta);

// Insert a horizontal-rule block. If the caret is mid-block the block is
// split first; the HR is inserted between the two halves. Caret lands in a
// fresh paragraph after the HR.
void insertHR(Doc& doc, Selection& sel);

// ─── Convenience queries ────────────────────────────────────────────────────

// OR together the marks present at any byte of the selection range. For a
// collapsed selection, returns the mark of the byte to the left of the caret.
uint8_t marksAtSelection(const Doc& doc, const Selection& sel);

// Plain text content of the selection range.
std::string textOfSelection(const Doc& doc, const Selection& sel);

// ─── Serialization ──────────────────────────────────────────────────────────

// Canonical markdown export. Round-trips through fromMarkdown for the subset
// we understand (paragraphs, #-headings, - / 1. lists with indent, ---).
std::string toMarkdown(const Doc& doc);

// Parses a liberal subset of markdown for import. Used for .txt / .md paste
// and to reload legacy body text from old patches (loose — unknown syntax
// becomes plain paragraphs).
Doc fromMarkdown(const std::string& md);

std::string toPlainText(const Doc& doc);

} // namespace journal
