#include "Jump.hpp"
#include "Theme.hpp"
#include "plugin.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

// ─── JumpModule ─────────────────────────────────────────────────────────────

JumpModule::JumpModule() {
    config(0, 0, 0, 0);
}

json_t* JumpModule::dataToJson() {
    json_t* root = json_object();
    json_object_set_new(root, "inputMode", json_integer(inputMode));
    json_t* slotsJ = json_array();
    for (const jump::Slot& s : slots) {
        json_t* sj = json_object();
        json_object_set_new(sj, "occupied", json_boolean(s.occupied));
        json_object_set_new(sj, "gx",  json_real(s.gridOffset.x));
        json_object_set_new(sj, "gy",  json_real(s.gridOffset.y));
        json_object_set_new(sj, "zoom", json_real(s.zoom));
        json_object_set_new(sj, "name", json_string(s.name.c_str()));
        json_array_append_new(slotsJ, sj);
    }
    json_object_set_new(root, "slots", slotsJ);
    return root;
}

void JumpModule::dataFromJson(json_t* root) {
    if (json_t* j = json_object_get(root, "inputMode")) inputMode = (int)json_integer_value(j);
    if (json_t* slotsJ = json_object_get(root, "slots")) {
        size_t n = std::min((size_t)N_SLOTS, json_array_size(slotsJ));
        for (size_t i = 0; i < n; i++) {
            json_t* sj = json_array_get(slotsJ, i);
            jump::Slot& s = slots[i];
            if (json_t* x = json_object_get(sj, "occupied")) s.occupied   = json_boolean_value(x);
            if (json_t* x = json_object_get(sj, "gx"))       s.gridOffset.x = json_real_value(x);
            if (json_t* x = json_object_get(sj, "gy"))       s.gridOffset.y = json_real_value(x);
            if (json_t* x = json_object_get(sj, "zoom"))     s.zoom       = json_real_value(x);
            if (json_t* x = json_object_get(sj, "name"))     s.name       = json_string_value(x);
        }
    }
}

// ─── JumpOverlay ────────────────────────────────────────────────────────────
//
// A scene-level widget that:
//   • captures hotkeys
//   • draws a modal when in find / overlay mode
//   • draws a transient pulse on the destination after a jump
//
// Hoisted in the header via forward declaration. Lives as a child of the rack
// scene (like Tidy's PickerOverlay) so it receives hover-key events regardless
// of where the user's mouse is.

struct JumpOverlay : widget::Widget {
    JumpModule* jm = nullptr;

    // ── Chord state ────────────────────────────────────────────────────────
    bool   waitingForChord = false;
    bool   savePrefix      = false;   // Cmd+Shift+J started the chord
    double chordStartT     = 0.0;

    // ── Modal state ────────────────────────────────────────────────────────
    bool        modalOpen     = false;
    std::string query;
    int         modalSelected = 0;

    // ── Back / forward history ─────────────────────────────────────────────
    std::vector<jump::View> backStack;
    std::vector<jump::View> fwdStack;

    // ── Pulse ──────────────────────────────────────────────────────────────
    double      pulseStartT   = -1.0;
    math::Rect  pulseRackRect;            // in rack coordinates (grid units)
    std::string pulseLabel;

    // ── Lifecycle ──────────────────────────────────────────────────────────
    void step() override {
        Widget::step();
        if (APP && APP->scene && APP->scene->rack)
            box.size = APP->scene->rack->box.size;

        // Chord timeout — 1.2s and it clears.
        if (waitingForChord && (rack::system::getTime() - chordStartT) > 1.2) {
            DEBUG("jump: chord timed out");
            waitingForChord = false;
            savePrefix      = false;
        }

        pollGlobalKeys();
    }

    // ── Global keyboard polling ────────────────────────────────────────────
    // We can't reliably catch keys through onHoverKey because modules cover
    // the rack area and eat the event first. So we poll GLFW directly every
    // frame and detect press transitions ourselves.
    struct KeyMap {
        bool cmd = false, shift = false;
        bool j = false, escape = false, enter = false, slash = false;
        bool f = false, backspace = false;
        bool up = false, down = false;
        bool leftBracket = false, rightBracket = false;
        bool digit[10] = {};  // 0..9 (we only use 1..9 semantically)
    };
    KeyMap prev;

    static bool kdown(GLFWwindow* w, int k) { return glfwGetKey(w, k) == GLFW_PRESS; }

    void pollGlobalKeys() {
        if (!jm || !APP || !APP->window || !APP->window->win) return;
        GLFWwindow* w = APP->window->win;

        KeyMap cur;
        cur.cmd          = kdown(w, GLFW_KEY_LEFT_SUPER)   || kdown(w, GLFW_KEY_RIGHT_SUPER)
                         || kdown(w, GLFW_KEY_LEFT_CONTROL) || kdown(w, GLFW_KEY_RIGHT_CONTROL);
        cur.shift        = kdown(w, GLFW_KEY_LEFT_SHIFT)   || kdown(w, GLFW_KEY_RIGHT_SHIFT);
        cur.j            = kdown(w, GLFW_KEY_J);
        cur.escape       = kdown(w, GLFW_KEY_ESCAPE);
        cur.enter        = kdown(w, GLFW_KEY_ENTER) || kdown(w, GLFW_KEY_KP_ENTER);
        cur.slash        = kdown(w, GLFW_KEY_SLASH);
        cur.f            = kdown(w, GLFW_KEY_F);
        cur.backspace    = kdown(w, GLFW_KEY_BACKSPACE);
        cur.up           = kdown(w, GLFW_KEY_UP);
        cur.down         = kdown(w, GLFW_KEY_DOWN);
        cur.leftBracket  = kdown(w, GLFW_KEY_LEFT_BRACKET);
        cur.rightBracket = kdown(w, GLFW_KEY_RIGHT_BRACKET);
        for (int d = 1; d <= 9; d++)
            cur.digit[d] = kdown(w, GLFW_KEY_0 + d);

        auto pressed = [](bool p, bool c) { return c && !p; };

        // Back / forward — always available
        if (cur.cmd && !cur.shift && pressed(prev.leftBracket,  cur.leftBracket))  goBack();
        if (cur.cmd && !cur.shift && pressed(prev.rightBracket, cur.rightBracket)) goForward();

        // Modal open — only Escape / arrows / Enter / digits here, text goes
        // through onSelectText while the modal has us as the selected widget.
        if (modalOpen) {
            if (pressed(prev.escape, cur.escape)) { DEBUG("jump: modal close (esc)"); closeModal(); }
            else if (pressed(prev.enter, cur.enter)) { DEBUG("jump: modal commit"); commitMatch(); }
            else if (pressed(prev.up, cur.up))   modalSelected = std::max(0, modalSelected - 1);
            else if (pressed(prev.down, cur.down)) modalSelected++;
            else if (cur.shift) {
                for (int d = 1; d <= 9; d++) {
                    if (pressed(prev.digit[d], cur.digit[d])) { saveSlot(d - 1); closeModal(); break; }
                }
            }
            else if (query.empty()) {
                for (int d = 1; d <= 9; d++) {
                    if (pressed(prev.digit[d], cur.digit[d])) { jumpSlot(d - 1); closeModal(); break; }
                }
            }
        }
        // Entry point — Cmd+J or Cmd+Shift+J
        else if (cur.cmd && pressed(prev.j, cur.j)) {
            DEBUG("jump: Cmd%s+J detected (mode=%d)", cur.shift ? "+Shift" : "",
                  jm->inputMode);
            if (jm->inputMode == JumpModule::MODE_OVERLAY) {
                if (cur.shift) {
                    int idx = 0;
                    while (idx < JumpModule::N_SLOTS && jm->slots[idx].occupied) idx++;
                    if (idx < JumpModule::N_SLOTS) saveSlot(idx);
                    else                           startPulse("no free slots");
                } else {
                    openModal();
                }
            } else {
                // chord
                waitingForChord = true;
                savePrefix      = cur.shift;
                chordStartT     = rack::system::getTime();
            }
        }
        // Chord continuation
        else if (waitingForChord) {
            for (int d = 1; d <= 9; d++) {
                if (pressed(prev.digit[d], cur.digit[d])) {
                    DEBUG("jump: chord %s slot %d", savePrefix ? "save" : "jump", d);
                    if (savePrefix) saveSlot(d - 1);
                    else            jumpSlot(d - 1);
                    waitingForChord = false;
                    savePrefix      = false;
                    break;
                }
            }
            if (waitingForChord
                && (pressed(prev.slash, cur.slash) || pressed(prev.f, cur.f))) {
                DEBUG("jump: chord → find");
                openModal();
                waitingForChord = false;
                savePrefix      = false;
            }
        }

        prev = cur;
    }

    // ── View capture / restore ─────────────────────────────────────────────
    static jump::View currentView() {
        jump::View v;
        if (APP && APP->scene && APP->scene->rackScroll) {
            v.gridOffset = APP->scene->rackScroll->getGridOffset();
            v.zoom       = APP->scene->rackScroll->getZoom();
        }
        return v;
    }

    void applyView(const jump::View& v) {
        if (!APP || !APP->scene || !APP->scene->rackScroll) return;
        APP->scene->rackScroll->setZoom(v.zoom);
        APP->scene->rackScroll->setGridOffset(v.gridOffset);
    }

    void pushHistory() {
        backStack.push_back(currentView());
        if (backStack.size() > 64) backStack.erase(backStack.begin());
        fwdStack.clear();
    }

    void goBack() {
        if (backStack.empty()) return;
        fwdStack.push_back(currentView());
        applyView(backStack.back());
        backStack.pop_back();
        startPulse("back");
    }
    void goForward() {
        if (fwdStack.empty()) return;
        backStack.push_back(currentView());
        applyView(fwdStack.back());
        fwdStack.pop_back();
        startPulse("forward");
    }

    // ── Slot operations ────────────────────────────────────────────────────
    void saveSlot(int idx) {
        if (!jm || idx < 0 || idx >= JumpModule::N_SLOTS) return;
        jump::View v = currentView();
        jm->slots[idx].occupied   = true;
        jm->slots[idx].gridOffset = v.gridOffset;
        jm->slots[idx].zoom       = v.zoom;
        startPulse("saved " + std::to_string(idx + 1));
    }
    void jumpSlot(int idx) {
        if (!jm || idx < 0 || idx >= JumpModule::N_SLOTS) return;
        const jump::Slot& s = jm->slots[idx];
        if (!s.occupied) { startPulse("slot " + std::to_string(idx + 1) + " empty"); return; }
        pushHistory();
        jump::View v; v.gridOffset = s.gridOffset; v.zoom = s.zoom;
        applyView(v);
        std::string label = s.name.empty()
            ? ("→ slot " + std::to_string(idx + 1))
            : ("→ " + s.name);
        startPulse(label);
    }

    // ── Jump to a specific module ──────────────────────────────────────────
    void jumpToModule(int64_t moduleId) {
        if (!APP || !APP->scene || !APP->scene->rack) return;
        ModuleWidget* mw = APP->scene->rack->getModule(moduleId);
        if (!mw) return;
        pushHistory();
        APP->scene->rackScroll->zoomToBound(mw->box);
        pulseStartT = rack::system::getTime();
        pulseLabel  = "→ " + (mw->model ? mw->model->name : std::string("module"));
        pulseRackRect = mw->box;
    }

    // ── Pulse ──────────────────────────────────────────────────────────────
    void startPulse(const std::string& label) {
        pulseStartT   = rack::system::getTime();
        pulseLabel    = label;
        pulseRackRect = math::Rect();   // module-less pulse just shows the label
    }

    // ── Drawing ────────────────────────────────────────────────────────────
    void draw(const DrawArgs& args) override {
        drawPulse(args.vg);
        if (modalOpen) drawModal(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            drawPulse(args.vg);
            if (modalOpen) drawModal(args.vg);
        }
        Widget::drawLayer(args, layer);
    }

    void drawPulse(NVGcontext* vg) {
        if (pulseStartT < 0) return;
        double t = rack::system::getTime() - pulseStartT;
        if (t > 1.2) { pulseStartT = -1; return; }
        float alpha = 1.f - (float)(t / 1.2);
        alpha = std::max(0.f, alpha);

        // If we're pulsing a specific module, outline it.
        if (pulseRackRect.size.x > 0.f) {
            nvgBeginPath(vg);
            nvgRect(vg, pulseRackRect.pos.x - 2.f, pulseRackRect.pos.y - 2.f,
                    pulseRackRect.size.x + 4.f, pulseRackRect.size.y + 4.f);
            nvgStrokeColor(vg, nvgRGBAf(0.95f, 0.6f, 0.1f, alpha));
            nvgStrokeWidth(vg, 3.f);
            nvgStroke(vg);
        }

        // Floating label, top-centre of the scroll viewport.
        if (!pulseLabel.empty()) {
            auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
            if (!font || !font->handle) return;
            nvgFontFaceId(vg, font->handle);
            nvgFontSize(vg, 14.f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            // Draw near the top of the scene viewport.
            float cx = box.size.x / 2.f;
            if (APP && APP->scene && APP->scene->rackScroll) {
                cx = APP->scene->rackScroll->box.pos.x + APP->scene->rackScroll->box.size.x / 2.f;
            }
            float y = 56.f;
            // Tray background
            float b[4];
            nvgTextBounds(vg, 0, 0, pulseLabel.c_str(), NULL, b);
            float tw = b[2] - b[0];
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - tw / 2.f - 10.f, y - 6.f, tw + 20.f, 26.f, 6.f);
            nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.75f * alpha));
            nvgFill(vg);
            nvgFillColor(vg, nvgRGBAf(1.f, 1.f, 1.f, alpha));
            nvgText(vg, cx, y, pulseLabel.c_str(), NULL);
        }
    }

    // ── Modal (Raycast-style palette) ──────────────────────────────────────
    struct MatchRow {
        std::string title;
        std::string subtitle;
        enum Kind { SLOT, MODULE } kind;
        int         slotIdx = -1;
        int64_t     moduleId = 0;
    };

    std::vector<MatchRow> buildMatches() {
        std::vector<MatchRow> out;
        if (!jm) return out;

        // Slots first.
        for (int i = 0; i < JumpModule::N_SLOTS; i++) {
            const jump::Slot& s = jm->slots[i];
            if (!s.occupied && query.empty()) continue;
            if (!s.occupied) continue;
            std::string title = std::to_string(i + 1) + "  "
                              + (s.name.empty() ? std::string("saved view") : s.name);
            if (!matchesQuery(title)) continue;
            MatchRow r; r.title = title; r.subtitle = "slot"; r.kind = MatchRow::SLOT; r.slotIdx = i;
            out.push_back(r);
        }

        // Modules.
        if (APP && APP->scene && APP->scene->rack) {
            for (ModuleWidget* mw : APP->scene->rack->getModules()) {
                if (!mw || !mw->module || !mw->model) continue;
                std::string title = mw->model->name;
                std::string subtitle = mw->model->plugin ? mw->model->plugin->brand : "";
                if (!matchesQuery(title) && !matchesQuery(subtitle)) continue;
                MatchRow r;
                r.title = title; r.subtitle = subtitle;
                r.kind = MatchRow::MODULE; r.moduleId = mw->module->id;
                out.push_back(r);
            }
        }
        return out;
    }

    // Case-insensitive "contains" — simple for v1. Fuzzy matching later.
    static bool matchesQuery_(const std::string& hay, const std::string& needle) {
        if (needle.empty()) return true;
        auto lower = [](char c){ return (char)std::tolower((unsigned char)c); };
        std::string h(hay.size(), 0);  std::transform(hay.begin(),    hay.end(),    h.begin(), lower);
        std::string n(needle.size(), 0); std::transform(needle.begin(), needle.end(), n.begin(), lower);
        return h.find(n) != std::string::npos;
    }
    bool matchesQuery(const std::string& s) { return matchesQuery_(s, query); }

    void drawModal(NVGcontext* vg) {
        // Dim the background.
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.35f));
        nvgFill(vg);

        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;

        float panelW = 480.f;
        float panelX = (box.size.x - panelW) / 2.f;
        float panelY = 140.f;
        float rowH   = 26.f;
        std::vector<MatchRow> matches = buildMatches();
        int nRows = (int)std::min((size_t)10, matches.size());
        float panelH = 46.f + nRows * rowH;

        // Panel
        nvgBeginPath(vg);
        nvgRoundedRect(vg, panelX, panelY, panelW, panelH, 8.f);
        nvgFillColor(vg, nvgRGBA(22, 22, 22, 250));
        nvgFill(vg);
        nvgStrokeColor(vg, nvgRGBA(80, 80, 80, 200));
        nvgStrokeWidth(vg, 1.f);
        nvgStroke(vg);

        // Input
        nvgFontFaceId(vg, font->handle);
        nvgFontSize(vg, 15.f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, nvgRGBA(235, 235, 235, 255));
        nvgText(vg, panelX + 16.f, panelY + 22.f, "›", NULL);
        std::string input = query.empty() ? std::string("type to search…") : query;
        nvgFillColor(vg, query.empty() ? nvgRGBA(140, 140, 140, 255) : nvgRGBA(235, 235, 235, 255));
        nvgText(vg, panelX + 32.f, panelY + 22.f, input.c_str(), NULL);

        // Divider
        nvgBeginPath(vg);
        nvgMoveTo(vg, panelX + 10.f, panelY + 42.f);
        nvgLineTo(vg, panelX + panelW - 10.f, panelY + 42.f);
        nvgStrokeColor(vg, nvgRGBA(60, 60, 60, 255));
        nvgStrokeWidth(vg, 0.8f);
        nvgStroke(vg);

        // Rows
        if (modalSelected >= (int)matches.size()) modalSelected = std::max(0, (int)matches.size() - 1);
        for (int i = 0; i < nRows; i++) {
            float rowY = panelY + 46.f + i * rowH;
            if (i == modalSelected) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, panelX + 6.f, rowY, panelW - 12.f, rowH - 2.f, 4.f);
                nvgFillColor(vg, nvgRGBA(55, 55, 55, 255));
                nvgFill(vg);
            }
            nvgFontSize(vg, 14.f);
            nvgFillColor(vg, nvgRGBA(235, 235, 235, 255));
            nvgText(vg, panelX + 16.f, rowY + rowH / 2.f - 1.f, matches[i].title.c_str(), NULL);
            if (!matches[i].subtitle.empty()) {
                nvgFontSize(vg, 11.f);
                nvgFillColor(vg, nvgRGBA(140, 140, 140, 255));
                nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
                nvgText(vg, panelX + panelW - 16.f, rowY + rowH / 2.f - 1.f,
                        matches[i].subtitle.c_str(), NULL);
                nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            }
        }
    }

    // ── Input handling ─────────────────────────────────────────────────────
    void openModal() {
        DEBUG("jump: opening modal");
        modalOpen = true;
        query.clear();
        modalSelected = 0;
        // Claim keyboard focus so onSelectText fires for typing.
        APP->event->setSelectedWidget(this);
    }
    void closeModal() {
        modalOpen = false;
        query.clear();
        APP->event->setSelectedWidget(NULL);
    }

    void commitMatch() {
        std::vector<MatchRow> m = buildMatches();
        if (m.empty()) return;
        int idx = math::clamp(modalSelected, 0, (int)m.size() - 1);
        const MatchRow& r = m[idx];
        closeModal();
        if      (r.kind == MatchRow::SLOT)   jumpSlot(r.slotIdx);
        else if (r.kind == MatchRow::MODULE) jumpToModule(r.moduleId);
    }

    // Once the modal is open, onSelectText fires on us for typed text.
    void onSelectText(const event::SelectText& e) override {
        if (!modalOpen) return;
        if (e.codepoint < 32 || e.codepoint == 127) return;
        if (e.codepoint < 128) {
            query.push_back((char)e.codepoint);
            modalSelected = 0;
            e.consume(this);
        }
    }

    // Backspace inside the modal — polling handles it too, but the SelectKey
    // path is more responsive for repeat.
    void onSelectKey(const event::SelectKey& e) override {
        if (!modalOpen) return;
        if (e.action != GLFW_PRESS && e.action != GLFW_REPEAT) return;
        if (e.key == GLFW_KEY_BACKSPACE) {
            if (!query.empty()) query.pop_back();
            modalSelected = 0;
            e.consume(this);
        }
    }
};

// ─── Panel ──────────────────────────────────────────────────────────────────

namespace {

struct JumpBackground : widget::Widget {
    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, dark ? nvgRGB(0, 0, 0) : nvgRGB(255, 255, 255));
        nvgFill(args.vg);
    }
};

struct JumpLogo : widget::Widget {
    std::string path, darkPath;
    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        std::string use = (dark && !darkPath.empty()) ? darkPath : path;
        auto img = APP->window->loadImage(use);
        if (!img || img->handle < 0) return;
        NVGpaint p = nvgImagePattern(args.vg, 0, 0, box.size.x, box.size.y, 0, img->handle, 1.f);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillPaint(args.vg, p);
        nvgFill(args.vg);
    }
};

struct JumpLabel : widget::Widget {
    std::string text;
    float size = 7.f;
    void draw(const DrawArgs& args) override {
        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, size);
        nvgTextLetterSpacing(args.vg, 0.2f);
        nvgFillColor(args.vg, lc::theme.dark ? nvgRGB(230, 230, 230) : nvgRGB(26, 26, 26));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(args.vg, box.size.x / 2.f, 0.f, text.c_str(), NULL);
    }
};

// Dot that shows slot occupancy — lights up if that slot has a saved view.
struct SlotDots : widget::Widget {
    JumpModule* jm = nullptr;
    void draw(const DrawArgs& args) override {
        if (!jm) return;
        bool dark = lc::theme.dark;
        NVGcolor on  = dark ? nvgRGB(220, 220, 220) : nvgRGB(30, 30, 30);
        NVGcolor off = dark ? nvgRGB(45, 45, 45)    : nvgRGB(220, 220, 220);
        float w = box.size.x, h = box.size.y;
        float r = std::min(w, h / 9.f) * 0.28f;
        float cx = w / 2.f;
        for (int i = 0; i < JumpModule::N_SLOTS; i++) {
            float cy = (h / (JumpModule::N_SLOTS + 1)) * (i + 1);
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, cx, cy, r);
            nvgFillColor(args.vg, jm->slots[i].occupied ? on : off);
            nvgFill(args.vg);
        }
    }
};

} // namespace

JumpWidget::JumpWidget(JumpModule* module) {
    setModule(module);
    box.size = math::Vec(RACK_GRID_WIDTH * 3, RACK_GRID_HEIGHT);

    JumpBackground* bg = new JumpBackground;
    bg->box.size = box.size;
    addChild(bg);

    float screwX = (box.size.x - RACK_GRID_WIDTH) / 2.f;
    addChild(createWidget<ScrewBlack>(math::Vec(screwX, 0)));
    addChild(createWidget<ScrewBlack>(math::Vec(screwX, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    app::PanelBorder* border = new app::PanelBorder;
    border->box.size = box.size;
    addChild(border);

    // "jump" label up top (matches grab's "grab" label style)
    {
        JumpLabel* l = new JumpLabel;
        l->text = "jump";
        l->size = 7.f;
        l->box.size = math::Vec(box.size.x, mm2px(3.f));
        l->box.pos  = math::Vec(0, mm2px(9.f));
        addChild(l);
    }

    // Slot indicator dots — column of 9 small dots showing which slots are saved.
    {
        SlotDots* d = new SlotDots;
        d->jm = module;
        d->box.size = mm2px(math::Vec(3.f, 70.f));
        d->box.pos  = math::Vec((box.size.x - d->box.size.x) / 2.f, mm2px(20.f));
        addChild(d);
    }

    // Logo at the bottom
    {
        JumpLogo* lg = new JumpLogo;
        lg->path     = asset::plugin(pluginInstance, "res/lc-icon-new.png");
        lg->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");
        lg->box.size = mm2px(math::Vec(9.f, 9.f));
        lg->box.pos  = math::Vec((box.size.x - lg->box.size.x) / 2.f,
                                 mm2px(128.5f - 8.f - 9.f));
        addChild(lg);
    }

    if (module && APP && APP->scene && APP->scene->rack) {
        overlay = new JumpOverlay;
        overlay->jm = module;
        APP->scene->rack->addChild(overlay);
    }
}

JumpWidget::~JumpWidget() {
    if (overlay && APP && APP->scene && APP->scene->rack) {
        APP->scene->rack->removeChild(overlay);
        delete overlay;
        overlay = nullptr;
    }
}

// ─── Context menu ───────────────────────────────────────────────────────────

namespace {

struct SlotRenameField : ui::TextField {
    JumpModule* jm = nullptr;
    int idx = 0;
    SlotRenameField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (jm && idx >= 0 && idx < JumpModule::N_SLOTS) jm->slots[idx].name = text;
    }
};

struct SingleSlotMenu : MenuItem {
    JumpWidget* widget = nullptr;
    JumpModule* jm = nullptr;
    int idx = 0;
    Menu* createChildMenu() override {
        Menu* m = new Menu;
        jump::Slot& s = jm->slots[idx];

        m->addChild(createMenuItem(s.occupied ? "Jump here" : "(empty)",
            s.occupied ? "cmd+j  " + std::to_string(idx + 1) : "",
            [this]() { if (widget && widget->overlay) widget->overlay->jumpSlot(idx); }));

        m->addChild(createMenuItem("Save current view here",
            "cmd+shift+j  " + std::to_string(idx + 1),
            [this]() { if (widget && widget->overlay) widget->overlay->saveSlot(idx); }));

        if (s.occupied) {
            m->addChild(new MenuSeparator);
            m->addChild(createMenuLabel("Name"));
            SlotRenameField* tf = new SlotRenameField;
            tf->jm = jm; tf->idx = idx; tf->text = s.name;
            m->addChild(tf);

            m->addChild(new MenuSeparator);
            m->addChild(createMenuItem("Clear", "", [this]() {
                jm->slots[idx] = jump::Slot{};
            }));
        }
        return m;
    }
};

} // namespace

void JumpWidget::appendContextMenu(Menu* menu) {
    JumpModule* m = dynamic_cast<JumpModule*>(module);
    if (!m) return;

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Jump"));

    menu->addChild(createMenuItem("Find…", "cmd+j  /", [this]() {
        if (overlay) overlay->openModal();
    }));

    menu->addChild(createMenuItem("Back",    "cmd+[",
        [this]() { if (overlay) overlay->goBack();    }));
    menu->addChild(createMenuItem("Forward", "cmd+]",
        [this]() { if (overlay) overlay->goForward(); }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Slots"));
    for (int i = 0; i < JumpModule::N_SLOTS; i++) {
        const jump::Slot& s = m->slots[i];
        std::string row = std::to_string(i + 1) + "   "
                       + (s.occupied ? (s.name.empty() ? std::string("(unnamed)") : s.name)
                                     : std::string("—"));
        SingleSlotMenu* item = createMenuItem<SingleSlotMenu>(row, RIGHT_ARROW);
        item->widget = this;
        item->jm = m;
        item->idx = i;
        menu->addChild(item);
    }

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Hotkey mode"));
    menu->addChild(createMenuItem("Chord   (cmd+j then digit)",
        CHECKMARK(m->inputMode == JumpModule::MODE_CHORD),
        [m]() { m->inputMode = JumpModule::MODE_CHORD; }));
    menu->addChild(createMenuItem("Overlay (cmd+j opens palette)",
        CHECKMARK(m->inputMode == JumpModule::MODE_OVERLAY),
        [m]() { m->inputMode = JumpModule::MODE_OVERLAY; }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Dark mode (shared)",
        CHECKMARK(lc::theme.dark), []() {
            lc::theme.dark = !lc::theme.dark;
            lc::saveTheme();
        }));
}

Model* modelJump = createModel<JumpModule, JumpWidget>("jump");
