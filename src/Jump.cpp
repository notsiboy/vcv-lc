#include "Jump.hpp"
#include "Theme.hpp"
#include "plugin.hpp"

#include <algorithm>
#include <cmath>

// ─── JumpModule ─────────────────────────────────────────────────────────────

JumpModule::JumpModule() { config(0, 0, 0, 0); }

json_t* JumpModule::dataToJson() {
    json_t* root = json_object();
    json_t* slotsJ = json_array();
    for (const jump::Slot& s : slots) {
        json_t* sj = json_object();
        json_object_set_new(sj, "occupied", json_boolean(s.occupied));
        json_object_set_new(sj, "rx", json_real(s.rackRect.pos.x));
        json_object_set_new(sj, "ry", json_real(s.rackRect.pos.y));
        json_object_set_new(sj, "rw", json_real(s.rackRect.size.x));
        json_object_set_new(sj, "rh", json_real(s.rackRect.size.y));
        json_object_set_new(sj, "name", json_string(s.name.c_str()));
        json_array_append_new(slotsJ, sj);
    }
    json_object_set_new(root, "slots", slotsJ);
    return root;
}

void JumpModule::dataFromJson(json_t* root) {
    if (json_t* slotsJ = json_object_get(root, "slots")) {
        size_t n = std::min((size_t)N_SLOTS, json_array_size(slotsJ));
        for (size_t i = 0; i < n; i++) {
            json_t* sj = json_array_get(slotsJ, i);
            jump::Slot& s = slots[i];
            if (json_t* x = json_object_get(sj, "occupied")) s.occupied       = json_boolean_value(x);
            if (json_t* x = json_object_get(sj, "rx"))       s.rackRect.pos.x  = json_real_value(x);
            if (json_t* x = json_object_get(sj, "ry"))       s.rackRect.pos.y  = json_real_value(x);
            if (json_t* x = json_object_get(sj, "rw"))       s.rackRect.size.x = json_real_value(x);
            if (json_t* x = json_object_get(sj, "rh"))       s.rackRect.size.y = json_real_value(x);
            if (json_t* x = json_object_get(sj, "name"))     s.name           = json_string_value(x);
        }
    }
}

// ─── Overlay ────────────────────────────────────────────────────────────────
//
// Scene-level widget that polls global keyboard state every frame (onHoverKey
// doesn't work reliably because modules cover the rack area and eat events)
// and draws the pulse / arm-mode feedback.

struct JumpOverlay : widget::Widget {
    JumpModule* jm      = nullptr;
    bool        armed   = false;

    // ── Nav history ────────────────────────────────────────────────────────
    std::vector<jump::View> backStack;
    std::vector<jump::View> fwdStack;

    // ── Pulse ──────────────────────────────────────────────────────────────
    double      pulseStartT = -1.0;
    std::string pulseLabel;
    math::Rect  pulseRackRect;   // optional: outline this module rect

    // ── Key polling state ──────────────────────────────────────────────────
    struct KeyMap {
        bool cmd = false, shift = false;
        bool escape = false;
        bool leftBracket = false, rightBracket = false;
        bool digit[10] = {};
    };
    KeyMap prev;

    // ── Lifecycle ──────────────────────────────────────────────────────────
    void step() override {
        Widget::step();
        if (APP && APP->scene && APP->scene->rack)
            box.size = APP->scene->rack->box.size;
        pollGlobalKeys();
    }

    // ── View capture / restore ─────────────────────────────────────────────
    //
    // Save the raw ScrollWidget::offset (in zoomed container pixels) plus
    // the zoom. On restore: setZoom first — this re-pivots around the
    // viewport centre so offset is in the right unit system for the new
    // zoom — then overwrite offset with the saved value. ScrollWidget's
    // step() clamps it to containerBox bounds automatically.
    //
    // Using zoomToBound adds padding around the rect, so the restored zoom
    // drifts (1.400 saved → 1.309 restored, visibly off). Direct write is
    // pixel-exact.
    // Capture: compute the rack-space rectangle currently visible, by
    // converting scrollOffset (zoomed pixels) → rack-space at 1x zoom.
    static jump::View currentView() {
        jump::View v;
        if (!APP || !APP->scene || !APP->scene->rackScroll) return v;
        auto rs = APP->scene->rackScroll;
        float zoom = std::max(0.01f, rs->getZoom());
        math::Vec off  = rs->getScrollOffset();
        math::Vec vpPx = rs->box.size;
        v.rackRect.pos  = off.div(zoom);
        v.rackRect.size = vpPx.div(zoom);
        return v;
    }

    void applyView(const jump::View& v) {
        if (!APP || !APP->scene || !APP->scene->rackScroll) return;
        if (v.rackRect.size.x <= 0 || v.rackRect.size.y <= 0) return;
        auto rs = APP->scene->rackScroll;
        rs->zoomToBound(v.rackRect);
        // Report how closely we landed: desired zoom = vpW/rect.w.
        float desired = rs->box.size.x / v.rackRect.size.x;
        DEBUG("jump: apply target=(%.1f,%.1f %.1fx%.1f) desired zoom=%.3f → landed zoom=%.3f (Δ=%.3f)",
              v.rackRect.pos.x, v.rackRect.pos.y,
              v.rackRect.size.x, v.rackRect.size.y,
              desired, rs->getZoom(), rs->getZoom() - desired);
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

    // ── Slot ops ───────────────────────────────────────────────────────────
    void saveSlot(int idx) {
        if (!jm || idx < 0 || idx >= JumpModule::N_SLOTS) return;
        jump::View v = currentView();
        jm->slots[idx].occupied = true;
        jm->slots[idx].rackRect = v.rackRect;
        startPulse("saved → " + std::to_string(idx + 1));
        DEBUG("jump: save slot %d rect=(%.1f,%.1f %.1fx%.1f)",
              idx + 1, v.rackRect.pos.x, v.rackRect.pos.y,
              v.rackRect.size.x, v.rackRect.size.y);
    }
    void jumpSlot(int idx) {
        if (!jm || idx < 0 || idx >= JumpModule::N_SLOTS) return;
        const jump::Slot& s = jm->slots[idx];
        if (!s.occupied) { startPulse("slot " + std::to_string(idx + 1) + " empty"); return; }
        pushHistory();
        jump::View v; v.rackRect = s.rackRect;
        applyView(v);
        std::string label = s.name.empty()
            ? ("→ slot " + std::to_string(idx + 1))
            : ("→ " + s.name);
        startPulse(label);
        DEBUG("jump: jump slot %d", idx + 1);
    }

    // ── Arm toggle ─────────────────────────────────────────────────────────
    void toggleArm() {
        armed = !armed;
        startPulse(armed ? "armed · cmd+1..9 to save" : "cancelled");
        DEBUG("jump: armed=%d", armed);
    }

    // ── Pulse ──────────────────────────────────────────────────────────────
    void startPulse(const std::string& label) {
        pulseStartT   = rack::system::getTime();
        pulseLabel    = label;
        pulseRackRect = math::Rect();
    }

    void draw(const DrawArgs& args) override {
        drawPulse(args.vg);
    }
    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) drawPulse(args.vg);
        Widget::drawLayer(args, layer);
    }

    void drawPulse(NVGcontext* vg) {
        if (pulseStartT < 0) return;
        double t = rack::system::getTime() - pulseStartT;
        if (t > 1.2) { pulseStartT = -1; return; }
        float alpha = std::max(0.f, 1.f - (float)(t / 1.2));

        if (pulseRackRect.size.x > 0.f) {
            nvgBeginPath(vg);
            nvgRect(vg, pulseRackRect.pos.x - 2.f, pulseRackRect.pos.y - 2.f,
                    pulseRackRect.size.x + 4.f, pulseRackRect.size.y + 4.f);
            nvgStrokeColor(vg, nvgRGBAf(0.95f, 0.6f, 0.1f, alpha));
            nvgStrokeWidth(vg, 3.f);
            nvgStroke(vg);
        }

        if (pulseLabel.empty()) return;
        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        nvgFontFaceId(vg, font->handle);
        nvgFontSize(vg, 14.f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);

        float cx = box.size.x / 2.f;
        if (APP && APP->scene && APP->scene->rackScroll)
            cx = APP->scene->rackScroll->box.pos.x + APP->scene->rackScroll->box.size.x / 2.f;
        float y = 56.f;
        float b[4]; nvgTextBounds(vg, 0, 0, pulseLabel.c_str(), NULL, b);
        float tw = b[2] - b[0];
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - tw / 2.f - 10.f, y - 6.f, tw + 20.f, 26.f, 6.f);
        nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.75f * alpha));
        nvgFill(vg);
        nvgFillColor(vg, nvgRGBAf(1.f, 1.f, 1.f, alpha));
        nvgText(vg, cx, y, pulseLabel.c_str(), NULL);
    }

    // ── Key polling ────────────────────────────────────────────────────────
    static bool kdown(GLFWwindow* w, int k) { return glfwGetKey(w, k) == GLFW_PRESS; }
    static bool pressed(bool p, bool c) { return c && !p; }

    void pollGlobalKeys() {
        if (!jm || !APP || !APP->window || !APP->window->win) return;
        GLFWwindow* w = APP->window->win;

        KeyMap cur;
        cur.cmd          = kdown(w, GLFW_KEY_LEFT_SUPER)   || kdown(w, GLFW_KEY_RIGHT_SUPER)
                         || kdown(w, GLFW_KEY_LEFT_CONTROL) || kdown(w, GLFW_KEY_RIGHT_CONTROL);
        cur.shift        = kdown(w, GLFW_KEY_LEFT_SHIFT)   || kdown(w, GLFW_KEY_RIGHT_SHIFT);
        cur.escape       = kdown(w, GLFW_KEY_ESCAPE);
        cur.leftBracket  = kdown(w, GLFW_KEY_LEFT_BRACKET);
        cur.rightBracket = kdown(w, GLFW_KEY_RIGHT_BRACKET);
        for (int d = 1; d <= 9; d++) cur.digit[d] = kdown(w, GLFW_KEY_0 + d);

        // Skip global shortcuts while a text field is being typed into.
        widget::Widget* sel = APP->event->getSelectedWidget();
        bool typing = (dynamic_cast<ui::TextField*>(sel) != nullptr);

        if (!typing) {
            // Escape cancels arm mode at any time.
            if (armed && pressed(prev.escape, cur.escape)) {
                armed = false;
                startPulse("cancelled");
            }

            // Nav history — always available (unmodified Cmd+[ / Cmd+]).
            if (cur.cmd && !cur.shift) {
                if (pressed(prev.leftBracket,  cur.leftBracket))  goBack();
                if (pressed(prev.rightBracket, cur.rightBracket)) goForward();
            }

            // Cmd+1..9 — save (if armed) or jump (if not).
            if (cur.cmd && !cur.shift) {
                for (int d = 1; d <= 9; d++) {
                    if (pressed(prev.digit[d], cur.digit[d])) {
                        if (armed) { saveSlot(d - 1); armed = false; }
                        else         jumpSlot(d - 1);
                        break;
                    }
                }
            }
        }

        prev = cur;
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

// Single clickable dot. Click toggles arm mode. Pulses amber while armed.
struct ArmDot : widget::OpaqueWidget {
    JumpOverlay* overlay = nullptr;

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        NVGcolor idle   = dark ? nvgRGB(80, 80, 80) : nvgRGB(180, 180, 180);
        NVGcolor accent = nvgRGB(240, 150, 40);

        float cx = box.size.x / 2.f;
        float cy = box.size.y / 2.f;
        float r  = std::min(cx, cy) - 1.f;

        bool isArmed = overlay && overlay->armed;
        double t = rack::system::getTime();
        float pulse = 0.55f + 0.45f * (float)std::sin(t * 6.0);

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, r);
        if (isArmed) {
            NVGcolor c = accent; c.a = pulse;
            nvgFillColor(args.vg, c);
            nvgFill(args.vg);
            // Halo
            nvgBeginPath(args.vg);
            nvgCircle(args.vg, cx, cy, r * 1.8f);
            nvgStrokeColor(args.vg, nvgRGBAf(accent.r, accent.g, accent.b, 0.25f * pulse));
            nvgStrokeWidth(args.vg, 1.2f);
            nvgStroke(args.vg);
        } else {
            nvgFillColor(args.vg, idle);
            nvgFill(args.vg);
        }
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && overlay && overlay->armed) {
            float cx = box.size.x / 2.f;
            float cy = box.size.y / 2.f;
            float r  = std::min(cx, cy);
            NVGcolor accent = nvgRGB(240, 150, 40);
            double t = rack::system::getTime();
            float pulse = 0.55f + 0.45f * (float)std::sin(t * 6.0);
            NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, r * 0.8f, r * 3.f,
                nvgRGBAf(accent.r, accent.g, accent.b, 0.5f * pulse),
                nvgRGBAf(accent.r, accent.g, accent.b, 0.f));
            nvgBeginPath(args.vg);
            nvgRect(args.vg, -r * 2.5f, -r * 2.5f, box.size.x + r * 5.f, box.size.y + r * 5.f);
            nvgFillPaint(args.vg, glow);
            nvgFill(args.vg);
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && overlay) {
            overlay->toggleArm();
            e.consume(this);
            return;
        }
        OpaqueWidget::onButton(e);
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

    // "jump" label
    {
        JumpLabel* l = new JumpLabel;
        l->text = "jump";
        l->box.size = math::Vec(box.size.x, mm2px(3.f));
        l->box.pos  = math::Vec(0, mm2px(9.f));
        addChild(l);
    }

    // The single arm dot (matches Tidy's toggle sizing — 5mm)
    ArmDot* arm = new ArmDot;
    arm->box.size = mm2px(math::Vec(5.f, 5.f));
    arm->box.pos  = math::Vec((box.size.x - arm->box.size.x) / 2.f, mm2px(14.f));
    addChild(arm);

    // Logo at bottom
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
        arm->overlay = overlay;
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

        if (s.occupied) {
            m->addChild(createMenuItem("Jump here", "cmd+" + std::to_string(idx + 1),
                [this]() { if (widget && widget->overlay) widget->overlay->jumpSlot(idx); }));
            m->addChild(createMenuItem("Save current view", "arm → cmd+" + std::to_string(idx + 1),
                [this]() { if (widget && widget->overlay) widget->overlay->saveSlot(idx); }));
            m->addChild(new MenuSeparator);
            m->addChild(createMenuLabel("Name"));
            SlotRenameField* tf = new SlotRenameField;
            tf->jm = jm; tf->idx = idx; tf->text = s.name;
            m->addChild(tf);
            m->addChild(new MenuSeparator);
            m->addChild(createMenuItem("Clear", "", [this]() { jm->slots[idx] = jump::Slot{}; }));
        } else {
            m->addChild(createMenuItem("(empty)", "", []() {}, true));
            m->addChild(createMenuItem("Save current view", "arm → cmd+" + std::to_string(idx + 1),
                [this]() { if (widget && widget->overlay) widget->overlay->saveSlot(idx); }));
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

    menu->addChild(createMenuItem(overlay && overlay->armed ? "Cancel arm" : "Arm (click dot)", "",
        [this]() { if (overlay) overlay->toggleArm(); }));
    menu->addChild(createMenuItem("Back",    "cmd+[", [this]() { if (overlay) overlay->goBack();    }));
    menu->addChild(createMenuItem("Forward", "cmd+]", [this]() { if (overlay) overlay->goForward(); }));

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
    menu->addChild(createMenuItem("Dark mode (shared)",
        CHECKMARK(lc::theme.dark), []() {
            lc::theme.dark = !lc::theme.dark;
            lc::saveTheme();
        }));
}

Model* modelJump = createModel<JumpModule, JumpWidget>("jump");
