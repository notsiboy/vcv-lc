#include "JournalDoc.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace journal {

// ─── Doc helpers ────────────────────────────────────────────────────────────

Pos Doc::clampPos(const Pos& p) const {
    if (blocks.empty()) return {0, 0};
    Pos r = p;
    if (r.block < 0) r.block = 0;
    if (r.block >= (int)blocks.size()) r.block = (int)blocks.size() - 1;

    // Never let the caret land on an HR. Prefer the next content block
    // (caret at its start); if nothing ahead, fall back to the previous
    // content block (caret at its end). If the doc is all HRs (degenerate),
    // leave r.block as-is — the offset clamp below still produces a valid
    // Pos.
    if (blocks[r.block].type == BLOCK_HR) {
        int n = (int)blocks.size();
        int found = -1;
        for (int i = r.block + 1; i < n; i++) {
            if (blocks[i].type != BLOCK_HR) { found = i; r.offset = 0; break; }
        }
        if (found < 0) {
            for (int i = r.block - 1; i >= 0; i--) {
                if (blocks[i].type != BLOCK_HR) {
                    found = i;
                    r.offset = blocks[i].length();
                    break;
                }
            }
        }
        if (found >= 0) r.block = found;
    }

    int len = blocks[r.block].length();
    if (r.offset < 0)   r.offset = 0;
    if (r.offset > len) r.offset = len;
    return r;
}

// ─── Internal helpers ───────────────────────────────────────────────────────

namespace {

// Clamp offset onto a UTF-8 codepoint boundary (never between lead and
// continuation bytes). Move LEFT to the nearest lead byte.
int snapLeft(const std::string& s, int off) {
    if (off <= 0) return 0;
    if (off > (int)s.size()) off = (int)s.size();
    while (off > 0 && utf8IsCont((unsigned char)s[off])) off--;
    return off;
}

// Delete a range within a single block, maintaining text/marks parity.
void eraseInBlock(Block& b, int from, int to) {
    if (from > to) std::swap(from, to);
    from = std::max(0, from);
    to   = std::min((int)b.text.size(), to);
    if (from >= to) return;
    b.text .erase(b.text .begin() + from, b.text .begin() + to);
    b.marks.erase(b.marks.begin() + from, b.marks.begin() + to);
}

// Insert raw UTF-8 bytes at `off` in block `b`, tagging each inserted byte
// with `marks`. Newlines in `s` are NOT allowed here — splitBlock is how you
// do that. Callers that need newlines must split `s` themselves.
void insertInBlock(Block& b, int off, const std::string& s, uint8_t marks) {
    off = std::max(0, std::min(off, (int)b.text.size()));
    b.text .insert(b.text .begin() + off, s.begin(), s.end());
    b.marks.insert(b.marks.begin() + off, s.size(), marks);
}

// Drop everything inside [from, to]. Collapses across blocks. Sel ends
// collapsed at `from`.
void deleteRange(Doc& doc, Selection& sel, Pos from, Pos to) {
    from = doc.clampPos(from);
    to   = doc.clampPos(to);
    if (to < from) std::swap(from, to);
    if (from == to) return;

    if (from.block == to.block) {
        eraseInBlock(doc.at(from.block), from.offset, to.offset);
        sel = Selection::caret(from);
        return;
    }

    // Multi-block: keep `from`'s head + `to`'s tail, drop everything between,
    // merge into one block whose type is `from`'s.
    Block& fromB = doc.at(from.block);
    Block& toB   = doc.at(to.block);

    // Tail of `to`.
    std::string tailText(toB.text.begin() + to.offset, toB.text.end());
    std::vector<uint8_t> tailMarks(toB.marks.begin() + to.offset, toB.marks.end());

    // Trim `from` at its cut point.
    fromB.text .erase(fromB.text .begin() + from.offset, fromB.text .end());
    fromB.marks.erase(fromB.marks.begin() + from.offset, fromB.marks.end());

    // Append tail.
    fromB.text .insert(fromB.text .end(), tailText.begin(),  tailText.end());
    fromB.marks.insert(fromB.marks.end(), tailMarks.begin(), tailMarks.end());

    // Remove intermediate blocks + the `to` block.
    doc.blocks.erase(doc.blocks.begin() + from.block + 1,
                     doc.blocks.begin() + to.block + 1);

    sel = Selection::caret(from);
}

// Split block `b` at byte `off` into two blocks. The second block keeps the
// text from `off` onwards; its type is PARAGRAPH by default. Returns a
// reference to the newly inserted block (at index `blockIdx + 1`).
Block& splitBlockAt(Doc& doc, int blockIdx, int off) {
    Block& src = doc.at(blockIdx);
    Block next;
    next.type = BLOCK_PARAGRAPH;  // default continuation
    next.meta = 0;
    next.text .assign(src.text .begin() + off, src.text .end());
    next.marks.assign(src.marks.begin() + off, src.marks.end());
    src.text .erase(src.text .begin() + off, src.text .end());
    src.marks.erase(src.marks.begin() + off, src.marks.end());
    doc.blocks.insert(doc.blocks.begin() + blockIdx + 1, std::move(next));
    return doc.at(blockIdx + 1);
}

} // namespace

// ─── insertText ─────────────────────────────────────────────────────────────

void insertText(Doc& doc, Selection& sel, const std::string& s, uint8_t marks) {
    if (!sel.isCollapsed()) {
        Pos f = sel.from(), t = sel.to();
        deleteRange(doc, sel, f, t);
    }
    Pos p = sel.head;

    // If inserting into an HR block, redirect to a fresh paragraph after it.
    if (doc.at(p.block).type == BLOCK_HR) {
        Block para;
        doc.blocks.insert(doc.blocks.begin() + p.block + 1, para);
        p.block++;
        p.offset = 0;
    }

    // Walk through `s`, splitting on '\n' into separate block-insertions.
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        size_t end = (nl == std::string::npos) ? s.size() : nl;
        std::string piece = s.substr(start, end - start);

        if (!piece.empty()) {
            insertInBlock(doc.at(p.block), p.offset, piece, marks);
            p.offset += (int)piece.size();
        }

        if (nl == std::string::npos) break;

        // Newline: split the current block, caret lands at start of new.
        splitBlockAt(doc, p.block, p.offset);
        p.block++;
        p.offset = 0;
        start = nl + 1;
    }

    sel = Selection::caret(p);
}

// ─── delete (backspace) ─────────────────────────────────────────────────────

void deleteBackward(Doc& doc, Selection& sel) {
    if (!sel.isCollapsed()) {
        deleteRange(doc, sel, sel.from(), sel.to());
        return;
    }
    Pos p = sel.head;

    if (p.offset > 0) {
        // Drop previous codepoint in this block.
        Block& b = doc.at(p.block);
        int prev = utf8Prev(b.text, p.offset);
        eraseInBlock(b, prev, p.offset);
        sel = Selection::caret({p.block, prev});
        return;
    }

    // At block start. If there's a block above, merge.
    if (p.block == 0) return;  // nothing to do

    int prevIdx = p.block - 1;
    Block& prev = doc.at(prevIdx);
    if (prev.type == BLOCK_HR) {
        // Remove the HR entirely, caret stays at start of current block.
        doc.blocks.erase(doc.blocks.begin() + prevIdx);
        sel = Selection::caret({prevIdx, 0});
        return;
    }

    // Merge current into previous — inherits previous block's type.
    int mergeOffset = prev.length();
    Block& cur = doc.at(p.block);
    prev.text .insert(prev.text .end(), cur.text .begin(), cur.text .end());
    prev.marks.insert(prev.marks.end(), cur.marks.begin(), cur.marks.end());
    doc.blocks.erase(doc.blocks.begin() + p.block);
    sel = Selection::caret({prevIdx, mergeOffset});
}

// ─── delete (forward) ───────────────────────────────────────────────────────

void deleteForward(Doc& doc, Selection& sel) {
    if (!sel.isCollapsed()) {
        deleteRange(doc, sel, sel.from(), sel.to());
        return;
    }
    Pos p = sel.head;
    Block& b = doc.at(p.block);

    if (p.offset < b.length()) {
        int nxt = utf8Next(b.text, p.offset);
        eraseInBlock(b, p.offset, nxt);
        return;
    }

    // At end of block. Merge next block into this one (or eat next HR).
    if (p.block + 1 >= doc.nBlocks()) return;
    Block& nxt = doc.at(p.block + 1);
    if (nxt.type == BLOCK_HR) {
        doc.blocks.erase(doc.blocks.begin() + p.block + 1);
        return;
    }
    b.text .insert(b.text .end(), nxt.text .begin(), nxt.text .end());
    b.marks.insert(b.marks.end(), nxt.marks.begin(), nxt.marks.end());
    doc.blocks.erase(doc.blocks.begin() + p.block + 1);
}

// ─── splitBlock (Enter) ─────────────────────────────────────────────────────
//
// ProseMirror model: lists are a block type + depth (meta). The marker
// ("- ", "1. ", "•") is rendered from structure, never stored as text. So
// Enter semantics collapse to:
//
//   empty list item      → outdent one level; at level 0 → paragraph
//   otherwise            → split at caret; new block inherits type + depth
//   heading              → new block is always paragraph

void splitBlock(Doc& doc, Selection& sel) {
    if (!sel.isCollapsed()) {
        Pos f = sel.from(), t = sel.to();
        deleteRange(doc, sel, f, t);
    }
    Pos p = sel.head;
    Block& src = doc.at(p.block);
    BlockType srcType = src.type;
    uint8_t   srcMeta = src.meta;

    if ((srcType == BLOCK_BULLET || srcType == BLOCK_ORDERED) && src.length() == 0) {
        if (srcMeta > 0) {
            src.meta = srcMeta - 1;
        } else {
            src.type = BLOCK_PARAGRAPH;
            src.meta = 0;
        }
        sel = Selection::caret({p.block, 0});
        return;
    }

    Block& next = splitBlockAt(doc, p.block, p.offset);
    if (srcType == BLOCK_HEADING) {
        next.type = BLOCK_PARAGRAPH;
        next.meta = 0;
    } else {
        next.type = srcType;
        next.meta = srcMeta;
    }
    sel = Selection::caret({p.block + 1, 0});
}

// ─── Marks ──────────────────────────────────────────────────────────────────

void setMarks(Doc& doc, const Selection& sel, uint8_t mask, bool on) {
    if (sel.isCollapsed()) return;
    Pos f = sel.from(), t = sel.to();
    for (int bi = f.block; bi <= t.block && bi < doc.nBlocks(); bi++) {
        Block& b = doc.at(bi);
        int start = (bi == f.block) ? f.offset : 0;
        int end   = (bi == t.block) ? t.offset : b.length();
        if (start > end) std::swap(start, end);
        if (end > (int)b.marks.size()) end = (int)b.marks.size();
        for (int i = start; i < end; i++) {
            if (on) b.marks[i] |= mask;
            else    b.marks[i] &= ~mask;
        }
    }
}

uint8_t marksAtSelection(const Doc& doc, const Selection& sel) {
    if (sel.isCollapsed()) {
        Pos p = sel.head;
        const Block& b = doc.at(p.block);
        // Mark to the left of caret is the "current typing style".
        int left = snapLeft(b.text, p.offset - 1);
        if (left >= 0 && left < b.length()) return b.markAt(left);
        return MARK_NONE;
    }
    uint8_t out = 0;
    Pos f = sel.from(), t = sel.to();
    for (int bi = f.block; bi <= t.block && bi < doc.nBlocks(); bi++) {
        const Block& b = doc.at(bi);
        int start = (bi == f.block) ? f.offset : 0;
        int end   = (bi == t.block) ? t.offset : b.length();
        for (int i = start; i < end; i++) out |= b.markAt(i);
    }
    return out;
}

std::string textOfSelection(const Doc& doc, const Selection& sel) {
    if (sel.isCollapsed()) return "";
    Pos f = sel.from(), t = sel.to();
    if (f.block == t.block) return doc.at(f.block).text.substr(f.offset, t.offset - f.offset);
    std::string out;
    for (int bi = f.block; bi <= t.block; bi++) {
        const Block& b = doc.at(bi);
        int s = (bi == f.block) ? f.offset : 0;
        int e = (bi == t.block) ? t.offset : b.length();
        out.append(b.text, s, e - s);
        if (bi != t.block) out.push_back('\n');
    }
    return out;
}

// ─── Block type / indent ────────────────────────────────────────────────────

void setBlockType(Doc& doc, const Selection& sel, BlockType t, uint8_t meta) {
    int from = std::min(sel.anchor.block, sel.head.block);
    int to   = std::max(sel.anchor.block, sel.head.block);
    for (int bi = from; bi <= to && bi < doc.nBlocks(); bi++) {
        Block& b = doc.at(bi);
        if (b.type == BLOCK_HR) continue;
        b.type = t;
        b.meta = meta;
    }
}

void indentList(Doc& doc, const Selection& sel, int delta) {
    int from = std::min(sel.anchor.block, sel.head.block);
    int to   = std::max(sel.anchor.block, sel.head.block);
    for (int bi = from; bi <= to && bi < doc.nBlocks(); bi++) {
        Block& b = doc.at(bi);
        if (b.type != BLOCK_BULLET && b.type != BLOCK_ORDERED) continue;
        int depth = blockDepth(b.meta) + delta;
        if (depth < 0) {
            // Shift+Tab past level 0 exits the list (→ paragraph).
            b.type = BLOCK_PARAGRAPH;
            b.meta = 0;
        } else {
            if (depth > 8) depth = 8;
            // Preserve the marker-kind bits (only meaningful for bullets).
            b.meta = (uint8_t)((b.meta & 0xF0) | (depth & 0x0F));
        }
    }
}

// ─── HR ─────────────────────────────────────────────────────────────────────

void insertHR(Doc& doc, Selection& sel) {
    if (!sel.isCollapsed()) {
        Pos f = sel.from(), t = sel.to();
        deleteRange(doc, sel, f, t);
    }
    Pos p = sel.head;

    // If the current block is empty, convert IT to HR (matches "type --- on
    // an empty line" UX). Otherwise split, then insert.
    Block& b = doc.at(p.block);
    if (b.length() == 0 && b.type != BLOCK_HR) {
        b.type = BLOCK_HR;
        b.meta = 0;
        // Follow with an empty paragraph so the caret has somewhere to land.
        Block para;
        doc.blocks.insert(doc.blocks.begin() + p.block + 1, para);
        sel = Selection::caret({p.block + 1, 0});
        return;
    }

    // Mid-block: split so content after caret survives, then add HR between.
    if (p.offset > 0 && p.offset < b.length()) {
        splitBlockAt(doc, p.block, p.offset);
    }

    Block hr; hr.type = BLOCK_HR;
    Block after;  // fresh paragraph below
    int insertAt = (p.offset == 0) ? p.block : p.block + 1;
    doc.blocks.insert(doc.blocks.begin() + insertAt, after);
    doc.blocks.insert(doc.blocks.begin() + insertAt, hr);
    sel = Selection::caret({insertAt + 1, 0});
}

// ─── toMarkdown ─────────────────────────────────────────────────────────────
//
// Emits a minimal, round-trippable subset. Inline marks become **bold**,
// _italic_, `code`; nested lists use two-space indents; HR is "---".

namespace {

void emitInline(const Block& b, std::string& out) {
    // Walk runs of equal mark bytes. For each run, wrap in markers in the
    // order bold → italic → code (outside → inside). Adjacent runs of the
    // same mark coalesce implicitly because we only emit markers at run
    // boundaries.
    int n = (int)b.text.size();
    int i = 0;
    while (i < n) {
        uint8_t m = b.markAt(i);
        int j = i + 1;
        while (j < n && b.markAt(j) == m) j++;

        std::string chunk(b.text.begin() + i, b.text.begin() + j);

        std::string prefix, suffix;
        if (m & MARK_BOLD)   { prefix += "**";  suffix = "**"   + suffix; }
        if (m & MARK_ITALIC) { prefix += "_";   suffix = "_"    + suffix; }
        if (m & MARK_CODE)   { prefix += "`";   suffix = "`"    + suffix; }

        out += prefix;
        out += chunk;
        out += suffix;
        i = j;
    }
}

} // namespace

std::string toMarkdown(const Doc& doc) {
    std::string out;
    // Ordered-list counter per nesting depth, reset whenever the contiguous
    // run of ordered items breaks (any non-ORDERED block, or the depth shifts
    // in a way that resets a deeper counter).
    int orderedCounter[9] = {0};

    for (int i = 0; i < doc.nBlocks(); i++) {
        const Block& b = doc.at(i);

        if (b.type != BLOCK_ORDERED) {
            for (int& c : orderedCounter) c = 0;
        }

        switch (b.type) {
            case BLOCK_HEADING: {
                int lvl = b.meta < 1 ? 1 : (b.meta > 6 ? 6 : b.meta);
                out.append(lvl, '#');
                out.push_back(' ');
                emitInline(b, out);
                break;
            }
            case BLOCK_BULLET: {
                int d    = blockDepth(b.meta);
                int kind = bulletKind(b.meta);
                if (kind < 0 || kind >= BULLET_MARKER_COUNT) kind = 0;
                out.append(d * 2, ' ');
                out.append(BULLET_MARKERS[kind]);
                out.push_back(' ');
                emitInline(b, out);
                break;
            }
            case BLOCK_ORDERED: {
                int d = blockDepth(b.meta);
                if (d > 8) d = 8;
                for (int j = d + 1; j < 9; j++) orderedCounter[j] = 0;
                orderedCounter[d]++;
                out.append(d * 2, ' ');
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d. ", orderedCounter[d]);
                out.append(buf);
                emitInline(b, out);
                break;
            }
            case BLOCK_HR:
                out.append("---");
                break;
            case BLOCK_PARAGRAPH:
            default:
                emitInline(b, out);
                break;
        }
        if (i + 1 < doc.nBlocks()) out.push_back('\n');
    }
    return out;
}

std::string toPlainText(const Doc& doc) {
    std::string out;
    for (int i = 0; i < doc.nBlocks(); i++) {
        const Block& b = doc.at(i);
        if (b.type == BLOCK_HR) out += "---";
        else out += b.text;
        if (i + 1 < doc.nBlocks()) out.push_back('\n');
    }
    return out;
}

// ─── fromMarkdown ───────────────────────────────────────────────────────────
//
// Intentionally forgiving. It's used for paste-in and for fresh loads, not
// for parsing every corner of CommonMark. Anything we don't recognise becomes
// plain paragraph text.

namespace {

// Parse inline markers in `line`, pushing text+marks into the given block.
void parseInlineInto(const std::string& line, Block& b) {
    auto append = [&](const std::string& s, uint8_t m) {
        for (char c : s) {
            b.text.push_back(c);
            b.marks.push_back(m);
        }
    };

    int n = (int)line.size();
    int i = 0;
    uint8_t active = 0;  // marks active from earlier openings in the line

    while (i < n) {
        char c = line[i];

        // Inline code: `…`
        if (c == '`' && !(active & MARK_CODE)) {
            int j = i + 1;
            while (j < n && line[j] != '`') j++;
            if (j < n) {
                append(line.substr(i + 1, j - i - 1), (uint8_t)(active | MARK_CODE));
                i = j + 1;
                continue;
            }
        }

        // Bold: **…**
        if (c == '*' && i + 1 < n && line[i + 1] == '*') {
            size_t end = line.find("**", i + 2);
            if (end != std::string::npos) {
                // Recursively process inner text so nested italics / code
                // still apply. Simpler: flip the bold bit and walk the slice.
                Block inner;
                parseInlineInto(line.substr(i + 2, end - (i + 2)), inner);
                for (size_t k = 0; k < inner.text.size(); k++) {
                    b.text.push_back(inner.text[k]);
                    b.marks.push_back((uint8_t)(inner.markAt((int)k) | MARK_BOLD));
                }
                i = (int)end + 2;
                continue;
            }
        }

        // Italic: _…_ or *…* (single char, word-boundary heuristic)
        if ((c == '_' || c == '*')
            && (i == 0 || !std::isalnum((unsigned char)line[i - 1]))) {
            char marker = c;
            int j = i + 1;
            while (j < n && line[j] != marker) j++;
            if (j < n && j > i + 1
                && (j + 1 >= n || !std::isalnum((unsigned char)line[j + 1]))) {
                Block inner;
                parseInlineInto(line.substr(i + 1, j - i - 1), inner);
                for (size_t k = 0; k < inner.text.size(); k++) {
                    b.text.push_back(inner.text[k]);
                    b.marks.push_back((uint8_t)(inner.markAt((int)k) | MARK_ITALIC));
                }
                i = j + 1;
                continue;
            }
        }

        b.text.push_back(c);
        b.marks.push_back(active);
        i++;
    }
}

} // namespace

Doc fromMarkdown(const std::string& md) {
    Doc doc;
    doc.blocks.clear();

    // Split on '\n'. Each non-empty logical line becomes one block.
    size_t start = 0;
    while (start <= md.size()) {
        size_t nl = md.find('\n', start);
        std::string line = md.substr(start, (nl == std::string::npos ? md.size() : nl) - start);

        Block b;

        // Leading whitespace → indent count (2 spaces per level for lists).
        int indentSpaces = 0;
        size_t p = 0;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) {
            if (line[p] == '\t') indentSpaces += 4;
            else                 indentSpaces += 1;
            p++;
        }
        std::string body = line.substr(p);

        // Horizontal rule
        auto allOf = [&](char c) {
            return body.size() >= 3 && body.find_first_not_of(c) == std::string::npos;
        };
        if (allOf('-') || allOf('*') || allOf('_')) {
            b.type = BLOCK_HR;
            doc.blocks.push_back(b);
            if (nl == std::string::npos) break;
            start = nl + 1;
            continue;
        }

        // Heading
        if (!body.empty() && body[0] == '#') {
            int h = 0;
            while (h < 6 && h < (int)body.size() && body[h] == '#') h++;
            if (h > 0 && h < (int)body.size() && body[h] == ' ') {
                b.type = BLOCK_HEADING;
                b.meta = (uint8_t)h;
                parseInlineInto(body.substr(h + 1), b);
                doc.blocks.push_back(std::move(b));
                if (nl == std::string::npos) break;
                start = nl + 1;
                continue;
            }
        }

        // Bullet list: -, *, +, →, —, – — marker kind preserved from text.
        {
            int matchedKind = -1;
            size_t matchedLen = 0;
            for (int k = 0; k < BULLET_MARKER_COUNT; k++) {
                const char* m = BULLET_MARKERS[k];
                size_t mLen = std::strlen(m);
                if (body.size() >= mLen + 1 &&
                    body.compare(0, mLen, m) == 0 &&
                    body[mLen] == ' ') {
                    matchedKind = k;
                    matchedLen  = mLen;
                    break;
                }
            }
            if (matchedKind >= 0) {
                b.type = BLOCK_BULLET;
                b.meta = makeBulletMeta(std::min(8, indentSpaces / 2), matchedKind);
                parseInlineInto(body.substr(matchedLen + 1), b);
                doc.blocks.push_back(std::move(b));
                if (nl == std::string::npos) break;
                start = nl + 1;
                continue;
            }
        }

        // Ordered list: N.  — marker stripped; display number is derived.
        {
            size_t k = 0;
            while (k < body.size() && std::isdigit((unsigned char)body[k])) k++;
            if (k > 0 && k + 1 < body.size() && body[k] == '.' && body[k + 1] == ' ') {
                b.type = BLOCK_ORDERED;
                b.meta = (uint8_t)std::min(8, indentSpaces / 2);
                parseInlineInto(body.substr(k + 2), b);
                doc.blocks.push_back(std::move(b));
                if (nl == std::string::npos) break;
                start = nl + 1;
                continue;
            }
        }

        // Fallback: paragraph (the preceding whitespace goes into the text
        // verbatim so indentation is preserved for anyone writing poetry).
        b.type = BLOCK_PARAGRAPH;
        parseInlineInto(line, b);
        doc.blocks.push_back(std::move(b));

        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    if (doc.blocks.empty()) doc.blocks.emplace_back();
    return doc;
}

} // namespace journal
