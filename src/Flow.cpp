#include "Flow.hpp"
#include "Theme.hpp"
#include "LcPorts.hpp"

#include <componentlibrary.hpp>
#include <history.hpp>

// ─── Factory permutations ───────────────────────────────────────────────────

const int FlowModule::FACTORY[FlowModule::NUM_PRESETS][FlowModule::NUM_EFFECTS] = {
    {0, 1, 2, 3},   // A B C D  — straight
    {3, 2, 1, 0},   // D C B A  — full reverse
    {1, 0, 3, 2},   // B A D C  — pair swap
    {0, 2, 1, 3},   // A C B D  — inner swap
    {0, 3, 2, 1},   // A D C B  — outer/inner reverse
    {2, 3, 0, 1},   // C D A B  — pair rotation
    {1, 2, 3, 0},   // B C D A  — rotate left
    {3, 0, 1, 2},   // D A B C  — rotate right
};

// ─── Module ─────────────────────────────────────────────────────────────────

FlowModule::FlowModule() {
    config(0, NUM_INPUTS, NUM_OUTPUTS, 0);
    configInput(IN_CHAIN, "Chain");
    configInput(IN_CV_ORDER, "Order CV (0..10V selects preset)");
    configInput(IN_BYPASS_GATE, "Bypass gate (high = chain IN → chain OUT)");
    configOutput(OUT_CHAIN, "Chain");
    for (int i = 0; i < NUM_EFFECTS; i++) {
        char letter = (char)('A' + i);
        configInput(IN_RETURN + i, string::f("Return %c", letter));
        configOutput(OUT_SEND + i, string::f("Send %c", letter));
    }
}

void FlowModule::setPreset(int p) {
    preset = math::clamp(p, 0, NUM_PRESETS - 1);
}

void FlowModule::cyclePreset() {
    preset = (preset + 1) % NUM_PRESETS;
}

void FlowModule::process(const ProcessArgs& args) {
    // CV-quantise preset selection with hysteresis on zone boundaries so a
    // noisy CV near an edge doesn't flicker between neighbours.
    if (inputs[IN_CV_ORDER].isConnected()) {
        float v = math::clamp(inputs[IN_CV_ORDER].getVoltage(), 0.f, 10.f - 1e-6f);
        float zoneSize = 10.f / (float)NUM_PRESETS;
        // 15% hysteresis band either side of the current preset's home zone.
        float hys = zoneSize * 0.15f;
        float bandLow  = (float)preset * zoneSize - hys;
        float bandHigh = (float)(preset + 1) * zoneSize + hys;
        if (v < bandLow || v >= bandHigh) {
            preset = math::clamp((int)(v / zoneSize), 0, NUM_PRESETS - 1);
        }
    }

    // Combine the panel toggle with the gate input (OR). Schmitt-style
    // 0.1 V / 1 V thresholds on the gate so noisy CVs don't flicker.
    bool bypassActive = bypassed;
    if (inputs[IN_BYPASS_GATE].isConnected()) {
        float gv = inputs[IN_BYPASS_GATE].getVoltage();
        if (gv >= 1.f)      bypassActive = true;
        else if (gv < 0.1f) bypassActive = bypassed; // fall back to button only
        else                bypassActive = bypassed || (gv >= 0.5f);
    }

    // Arm the fade whenever the preset actually changes or bypass flips.
    // Duration depends on fadeMode: FADE_FAST is the click-suppression duck;
    // FADE_MORPH stretches it into an audible 400 ms crossfade. Bypass flips
    // always use the short fade (a long duck into bypass feels laggy).
    if (preset != lastPreset || bypassActive != lastBypassActive) {
        float seconds = 0.020f;
        if (preset != lastPreset && fadeMode == FADE_MORPH) seconds = 0.400f;
        xfadeTotal = std::max(1, (int)(seconds * args.sampleRate));
        xfadeSamples = xfadeTotal;
        if (preset != lastPreset) prevPresetForXfade = lastPreset;
        lastPreset = preset;
        lastBypassActive = bypassActive;
    }
    float gain = 1.f;
    if (xfadeSamples > 0 && xfadeTotal > 0) {
        gain = 1.f - (float)xfadeSamples / (float)xfadeTotal;  // 0 → 1
        xfadeSamples--;
    }

    const int* perm = FACTORY[preset];

    // Polyphony: match channel count of the chain input and any return
    // cables. Stereo-through-poly "just works" when the source feeds 2
    // channels down a single cable.
    int channels = std::max(1, inputs[IN_CHAIN].getChannels());
    for (int i = 0; i < NUM_EFFECTS; i++)
        channels = std::max(channels, inputs[IN_RETURN + i].getChannels());
    outputs[OUT_CHAIN].setChannels(channels);
    for (int i = 0; i < NUM_EFFECTS; i++)
        outputs[OUT_SEND + i].setChannels(channels);

    // Chain a sample through the current permutation. Each hop reads the
    // return that the effect produced on the *previous* sample — Rack can't
    // topologically order arbitrary cable loops, so every stage adds one
    // sample of latency (≈0.02 ms per hop at 44.1 kHz). An unpatched return
    // bypasses its slot so an incomplete chain still passes audio. The
    // `gain` factor applies the fade-in to every send and the chain output
    // so routing swaps don't produce sample-level discontinuities anywhere.
    for (int c = 0; c < channels; c++) {
        float inV = inputs[IN_CHAIN].getVoltage(c);
        float x = inV;
        for (int i = 0; i < NUM_EFFECTS; i++) {
            int slot = perm[i];
            // Sends stay live even in bypass so effects keep their state
            // warm (reverb tails, delay buffers) — feels much better when
            // bypass releases than dropping them into silence.
            outputs[OUT_SEND + slot].setVoltage(x * gain, c);
            if (inputs[IN_RETURN + slot].isConnected())
                x = inputs[IN_RETURN + slot].getVoltage(c);
            // else: skip this slot — signal passes through unchanged.
        }
        float outV = bypassActive ? inV : x;
        outputs[OUT_CHAIN].setVoltage(outV * gain, c);
    }
}

json_t* FlowModule::dataToJson() {
    json_t* r = json_object();
    json_object_set_new(r, "preset", json_integer(preset));
    json_object_set_new(r, "bypassed", json_boolean(bypassed));
    json_object_set_new(r, "fadeMode", json_integer(fadeMode));
    return r;
}

void FlowModule::dataFromJson(json_t* r) {
    if (json_t* j = json_object_get(r, "preset"))
        preset = math::clamp((int)json_integer_value(j), 0, NUM_PRESETS - 1);
    if (json_t* j = json_object_get(r, "bypassed"))
        bypassed = json_boolean_value(j);
    if (json_t* j = json_object_get(r, "fadeMode"))
        fadeMode = math::clamp((int)json_integer_value(j), 0, NUM_FADE_MODES - 1);
}

// ─── Undo helper (snapshot-based, matching qmap/qmod) ──────────────────────

static json_t* g_flowClipboard = nullptr;

namespace {
struct FlowJsonAction : rack::history::ModuleAction {
    json_t* oldJ = nullptr;
    json_t* newJ = nullptr;
    ~FlowJsonAction() {
        if (oldJ) json_decref(oldJ);
        if (newJ) json_decref(newJ);
    }
    void apply(json_t* j) {
        if (!j) return;
        rack::engine::Module* m = APP->engine->getModule(moduleId);
        FlowModule* fm = dynamic_cast<FlowModule*>(m);
        if (fm) fm->dataFromJson(j);
    }
    void undo() override { apply(oldJ); }
    void redo() override { apply(newJ); }
};
}

static void pushFlowJsonAction(FlowModule* fm, json_t* before, const char* name) {
    if (!fm || !before) { if (before) json_decref(before); return; }
    FlowJsonAction* a = new FlowJsonAction;
    a->moduleId = fm->id;
    a->name = name ? name : "flow change";
    a->oldJ = before;
    a->newJ = fm->dataToJson();
    APP->history->push(a);
}

// ─── Widget helpers ─────────────────────────────────────────────────────────

namespace {

// Channel tint shades — greyscale progression so the stronger visual cue is
// the chip's position/text rather than colour. Reused on the row backgrounds
// and the order-display chips so the user can trace a chip back to its
// send/return row by shade.
static const NVGcolor CH_COLOR[FlowModule::NUM_EFFECTS] = {
    nvgRGB( 70,  70,  70),   // A  darkest
    nvgRGB(120, 120, 120),   // B
    nvgRGB(170, 170, 170),   // C
    nvgRGB(220, 220, 220),   // D  lightest
};

// Text colour on a chip — flip to dark ink on the lighter greys so the
// letters keep their contrast.
static NVGcolor chipTextColor(int slot) {
    float lum = (CH_COLOR[slot].r + CH_COLOR[slot].g + CH_COLOR[slot].b) / 3.f;
    return (lum > 0.55f) ? nvgRGB(30, 30, 30) : nvgRGB(245, 245, 245);
}

struct FlowBackground : widget::Widget {
    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, lc::panelBg());
        nvgFill(args.vg);
    }
};

// Faint tinted strip behind one effect's send/return row. Alpha tuned low so
// the colour reads as a hint, not a paint splash.
struct ChannelStrip : widget::Widget {
    int slot = 0;
    void draw(const DrawArgs& args) override {
        if (slot < 0 || slot >= FlowModule::NUM_EFFECTS) return;
        NVGcolor c = CH_COLOR[slot];
        float a = lc::theme.dark ? 0.16f : 0.10f;
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
        nvgFillColor(args.vg, nvgRGBAf(c.r, c.g, c.b, a));
        nvgFill(args.vg);
    }
};

struct FlowLabel : widget::Widget {
    std::string text;
    float fontSize = 7.f;
    NVGcolor* tint = nullptr;   // optional override
    void draw(const DrawArgs& args) override {
        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, fontSize);
        nvgTextLetterSpacing(args.vg, 0.2f);
        if (tint) nvgFillColor(args.vg, *tint);
        else nvgFillColor(args.vg, lc::theme.dark ? nvgRGB(230, 230, 230) : nvgRGB(26, 26, 26));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(args.vg, box.size.x / 2.f, 0.f, text.c_str(), NULL);
    }
};

// Tiny centred letter, used between each row's send/return jacks to mark
// which effect the pair feeds. Font-size is small because the gap between
// adjacent PJ301M jacks in 4 HP is only ~2 mm wide.
struct SlotLetter : widget::Widget {
    std::string letter;
    float fontSize = 8.f;
    void draw(const DrawArgs& args) override {
        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, fontSize);
        nvgFillColor(args.vg, lc::theme.dark ? nvgRGB(220, 220, 220) : nvgRGB(30, 30, 30));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f, letter.c_str(), nullptr);
    }
};

struct FlowLogo : widget::Widget {
    std::string path, darkPath, greyPath;
    void draw(const DrawArgs& args) override {
        std::string use = lc::logoAsset(path, darkPath, greyPath);
        auto img = APP->window->loadImage(use);
        if (!img || img->handle < 0) return;
        NVGpaint p = nvgImagePattern(args.vg, 0, 0, box.size.x, box.size.y, 0, img->handle, 1.f);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillPaint(args.vg, p);
        nvgFill(args.vg);
    }
};

static void drawRoundButton(NVGcontext* vg, float w, float h, bool dark) {
    float cx = w / 2.f, cy = h / 2.f;
    float rOuter = std::min(cx, cy) - 0.5f;
    float rInner = rOuter * 0.78f;
    NVGcolor rim      = dark ? nvgRGB(40, 40, 40)  : nvgRGB(255, 255, 255);
    NVGcolor rimEdge  = dark ? nvgRGB(90, 90, 90)  : nvgRGB(180, 180, 180);
    NVGcolor face     = dark ? nvgRGB(55, 55, 55)  : nvgRGB(240, 240, 240);
    NVGcolor faceEdge = dark ? nvgRGB(75, 75, 75)  : nvgRGB(210, 210, 210);
    nvgBeginPath(vg);   nvgCircle(vg, cx, cy, rOuter);
    nvgFillColor(vg, rim); nvgFill(vg);
    nvgStrokeColor(vg, rimEdge); nvgStrokeWidth(vg, 0.6f); nvgStroke(vg);
    nvgBeginPath(vg);   nvgCircle(vg, cx, cy, rInner);
    nvgFillColor(vg, face); nvgFill(vg);
    nvgStrokeColor(vg, faceEdge); nvgStrokeWidth(vg, 0.4f); nvgStroke(vg);
}

static void drawCenterDot(NVGcontext* vg, float w, float h, NVGcolor c, bool on, bool dark, float scale) {
    float cx = w / 2.f, cy = h / 2.f;
    float rOuter = std::min(cx, cy) - 0.5f;
    float rLed = rOuter * scale;
    NVGcolor off = dark ? nvgRGB(80, 80, 80) : nvgRGB(180, 180, 180);
    nvgBeginPath(vg);  nvgCircle(vg, cx, cy, rLed);
    nvgFillColor(vg, on ? c : off); nvgFill(vg);
}

static void drawCenterDotGlow(NVGcontext* vg, float w, float h, NVGcolor c, float alpha, float scale) {
    float cx = w / 2.f, cy = h / 2.f;
    float rOuter = std::min(cx, cy) - 0.5f;
    float rLed = rOuter * scale;
    int a = (int)math::clamp(alpha * 180.f, 0.f, 255.f);
    NVGpaint glow = nvgRadialGradient(vg, cx, cy, rLed * 0.6f, rLed * 3.f,
        nvgRGBA((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), a),
        nvgRGBA((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 0));
    nvgBeginPath(vg); nvgRect(vg, cx - rLed * 4, cy - rLed * 4, rLed * 8, rLed * 8);
    nvgFillPaint(vg, glow); nvgFill(vg);
}

static const NVGcolor AMBER = nvgRGB(255, 170, 40);

// Master cycle button — advances the preset.
struct PresetCycleButton : widget::OpaqueWidget {
    FlowModule* fm = nullptr;
    float dotScale = 0.50f;

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        drawRoundButton(args.vg, box.size.x, box.size.y, dark);
        drawCenterDot(args.vg, box.size.x, box.size.y, AMBER, true, dark, dotScale);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            drawCenterDotGlow(args.vg, box.size.x, box.size.y, AMBER, 0.55f, dotScale);
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && fm) {
            json_t* before = fm->dataToJson();
            fm->cyclePreset();
            pushFlowJsonAction(fm, before, "flow preset");
            e.consume(this);
        }
    }
};

// Sticky bypass toggle. When on, flow routes chain IN straight to chain OUT
// (effects stay primed). The gate jack next to it OR-s in.
struct BypassButton : widget::OpaqueWidget {
    FlowModule* fm = nullptr;
    float dotScale = 0.50f;

    bool isOn() const {
        if (!fm) return false;
        return fm->bypassed || fm->lastBypassActive;
    }

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        drawRoundButton(args.vg, box.size.x, box.size.y, dark);
        drawCenterDot(args.vg, box.size.x, box.size.y, AMBER, isOn(), dark, dotScale);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && isOn()) {
            drawCenterDotGlow(args.vg, box.size.x, box.size.y, AMBER, 0.55f, dotScale);
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && fm) {
            json_t* before = fm->dataToJson();
            fm->bypassed = !fm->bypassed;
            pushFlowJsonAction(fm, before, "flow bypass");
            e.consume(this);
        }
    }
};

// Horizontal strip of four colour chips showing the current permutation. The
// chip colours match the row backgrounds so the user can trace a chip back
// to its send/return pair at a glance.
struct OrderDisplay : widget::Widget {
    FlowModule* fm = nullptr;

    static void paintChip(NVGcontext* vg, float x, float chipW, float chipH,
                          int slot, float alpha,
                          std::shared_ptr<window::Font> font, bool dark) {
        if (alpha <= 0.f) return;
        NVGcolor c = CH_COLOR[slot];
        c.a = alpha;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, 0, chipW, chipH, 1.5f);
        nvgFillColor(vg, c);
        nvgFill(vg);
        NVGcolor rim = dark ? nvgRGBA(0, 0, 0, (int)(80 * alpha))
                            : nvgRGBA(0, 0, 0, (int)(30 * alpha));
        nvgStrokeColor(vg, rim);
        nvgStrokeWidth(vg, 0.4f);
        nvgStroke(vg);
        if (font && font->handle) {
            nvgFontFaceId(vg, font->handle);
            nvgFontSize(vg, 7.5f);
            NVGcolor tc = chipTextColor(slot);
            tc.a = alpha;
            nvgFillColor(vg, tc);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            char letter[2] = { (char)('A' + slot), '\0' };
            nvgText(vg, x + chipW * 0.5f, chipH * 0.5f, letter, nullptr);
        }
    }

    void draw(const DrawArgs& args) override {
        if (!fm) return;
        bool dark = lc::theme.dark;
        const int* perm     = FlowModule::FACTORY[fm->preset];
        const int* prevPerm = FlowModule::FACTORY[fm->prevPresetForXfade];
        const int n = FlowModule::NUM_EFFECTS;

        float gap = box.size.x * 0.02f;
        float chipW = (box.size.x - gap * (n - 1)) / (float)n;
        float chipH = box.size.y;

        // Crossfade progress: 0 at the moment of change, 1 when the fade
        // window has fully elapsed. When idle (xfadeSamples == 0) the new
        // chip is drawn at full opacity, old at zero.
        float progress = 1.f;
        if (fm->xfadeSamples > 0 && fm->xfadeTotal > 0) {
            progress = 1.f - (float)fm->xfadeSamples / (float)fm->xfadeTotal;
        }

        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        for (int i = 0; i < n; i++) {
            float x = i * (chipW + gap);
            int oldSlot = prevPerm[i];
            int newSlot = perm[i];
            if (oldSlot == newSlot || progress >= 1.f) {
                paintChip(args.vg, x, chipW, chipH, newSlot, 1.f, font, dark);
            } else {
                paintChip(args.vg, x, chipW, chipH, oldSlot, 1.f - progress, font, dark);
                paintChip(args.vg, x, chipW, chipH, newSlot, progress,       font, dark);
            }
        }
    }
};

} // namespace

// ─── Widget ─────────────────────────────────────────────────────────────────

FlowWidget::FlowWidget(FlowModule* module) {
    setModule(module);
    box.size = Vec(RACK_GRID_WIDTH * 4, RACK_GRID_HEIGHT);

    FlowBackground* bg = new FlowBackground;
    bg->box.size = box.size;
    addChild(bg);

    float screwX = (box.size.x - RACK_GRID_WIDTH) / 2.f;
    addChild(createWidget<ScrewBlack>(Vec(screwX, 0)));
    addChild(createWidget<ScrewBlack>(Vec(screwX, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    app::PanelBorder* border = new app::PanelBorder;
    border->box.size = box.size;
    addChild(border);

    const float panelW_mm = box.size.x / mm2px(1.f);
    const float colL_mm = panelW_mm * 0.25f;
    const float colR_mm = panelW_mm * 0.75f;

    // Title + header row (matches qmod's geometry: title y=7, header y=14.5).
    {
        FlowLabel* lab = new FlowLabel;
        lab->text = "flow";
        lab->fontSize = 7.f;
        lab->box.size = Vec(box.size.x, mm2px(3.f));
        lab->box.pos = Vec(0, mm2px(7.f));
        addChild(lab);
    }
    const float headerY = 14.5f;
    {
        PresetCycleButton* b = new PresetCycleButton;
        b->fm = module;
        b->box.size = mm2px(Vec(5.f, 5.f));
        b->box.pos = Vec(mm2px(colL_mm) - b->box.size.x / 2.f,
                         mm2px(headerY) - b->box.size.y / 2.f);
        addChild(b);
    }
    // CV order input — white-ring port since it's an input, sits where qmod's
    // trigger input sits.
    addInput(createInputCentered<lc::WhiteRingPJ301MPort>(
        mm2px(Vec(colR_mm, headerY)), module, FlowModule::IN_CV_ORDER));

    // Order display — horizontal chip strip showing the current permutation.
    {
        OrderDisplay* disp = new OrderDisplay;
        disp->fm = module;
        disp->box.size = mm2px(Vec(panelW_mm - 3.f, 4.f));
        disp->box.pos = Vec(mm2px(1.5f), mm2px(22.f));
        addChild(disp);
    }

    // Four effect rows — faint row-tint behind the jacks, send on the left,
    // return (white-ring) on the right. Send letter drawn above each pair,
    // tinted to match the row colour.
    const float effectRowTop = 32.f;
    const float effectRowStep = 13.f;
    for (int i = 0; i < FlowModule::NUM_EFFECTS; i++) {
        float y = effectRowTop + i * effectRowStep;

        ChannelStrip* strip = new ChannelStrip;
        strip->slot = i;
        strip->box.size = mm2px(Vec(panelW_mm - 3.f, 9.f));
        strip->box.pos = Vec(mm2px(1.5f), mm2px(y - 4.5f));
        addChild(strip);

        addOutput(createOutputCentered<ThemedPJ301MPort>(
            mm2px(Vec(colL_mm, y)), module, FlowModule::OUT_SEND + i));
        addInput(createInputCentered<lc::WhiteRingPJ301MPort>(
            mm2px(Vec(colR_mm, y)), module, FlowModule::IN_RETURN + i));

        // Letter label squeezed between the jacks at panel centre.
        SlotLetter* letter = new SlotLetter;
        letter->letter = std::string(1, (char)('A' + i));
        letter->fontSize = 8.f;
        float letterW = mm2px(2.2f);
        float letterH = mm2px(3.f);
        letter->box.size = Vec(letterW, letterH);
        letter->box.pos = Vec((box.size.x - letterW) / 2.f,
                              mm2px(y) - letterH / 2.f);
        addChild(letter);
    }

    // Chain IN / OUT row.
    const float ioY = 90.f;
    addInput(createInputCentered<lc::WhiteRingPJ301MPort>(
        mm2px(Vec(colL_mm, ioY)), module, FlowModule::IN_CHAIN));
    addOutput(createOutputCentered<ThemedPJ301MPort>(
        mm2px(Vec(colR_mm, ioY)), module, FlowModule::OUT_CHAIN));

    // Bypass — gate input on top, sticky toggle button below it, both
    // centred on the panel between the IN/OUT row and the logo. Gate CV
    // forces bypass on while high; the button independently latches it.
    addInput(createInputCentered<lc::WhiteRingPJ301MPort>(
        mm2px(Vec(panelW_mm * 0.5f, 99.f)), module, FlowModule::IN_BYPASS_GATE));
    {
        BypassButton* b = new BypassButton;
        b->fm = module;
        b->box.size = mm2px(Vec(5.f, 5.f));
        b->box.pos = Vec((box.size.x - b->box.size.x) / 2.f,
                         mm2px(108.f) - b->box.size.y / 2.f);
        addChild(b);
    }

    // Logo.
    {
        FlowLogo* logo = new FlowLogo;
        logo->path     = asset::plugin(pluginInstance, "res/lc-icon-new.png");
        logo->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");
        logo->greyPath = asset::plugin(pluginInstance, "res/lc-icon-grey.png");
        logo->box.size = mm2px(Vec(9.f, 9.f));
        logo->box.pos  = Vec((box.size.x - logo->box.size.x) / 2.f,
                             mm2px(128.5f - 8.f - 9.f + 2.f));
        addChild(logo);
    }
}

void FlowWidget::appendContextMenu(Menu* menu) {
    FlowModule* fm = dynamic_cast<FlowModule*>(module);
    if (!fm) return;

    auto permLabel = [](int idx) -> std::string {
        const int* p = FlowModule::FACTORY[idx];
        std::string s;
        for (int i = 0; i < FlowModule::NUM_EFFECTS; i++) {
            if (i) s += " ";
            s += (char)('A' + p[i]);
        }
        return s;
    };

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Preset order"));
    for (int i = 0; i < FlowModule::NUM_PRESETS; i++) {
        menu->addChild(createCheckMenuItem(permLabel(i), "",
            [=]() { return fm->preset == i; },
            [=]() {
                json_t* before = fm->dataToJson();
                fm->setPreset(i);
                pushFlowJsonAction(fm, before, "flow preset");
            }));
    }

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Copy preset", "", [=]() {
        if (g_flowClipboard) json_decref(g_flowClipboard);
        g_flowClipboard = fm->dataToJson();
    }));
    menu->addChild(createMenuItem("Paste preset", "",
        [=]() {
            if (!g_flowClipboard) return;
            json_t* before = fm->dataToJson();
            fm->dataFromJson(g_flowClipboard);
            pushFlowJsonAction(fm, before, "paste flow preset");
        },
        /*disabled*/ g_flowClipboard == nullptr));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Preset transition"));
    menu->addChild(createCheckMenuItem("Fade (20 ms duck)", "",
        [=]() { return fm->fadeMode == FlowModule::FADE_FAST; },
        [=]() { fm->fadeMode = FlowModule::FADE_FAST; }));
    menu->addChild(createCheckMenuItem("Morph (400 ms crossfade)", "",
        [=]() { return fm->fadeMode == FlowModule::FADE_MORPH; },
        [=]() { fm->fadeMode = FlowModule::FADE_MORPH; }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("CV order input: 0..10 V picks preset"));

    menu->addChild(new MenuSeparator);
    lc::appendThemeMenu(menu);
}

Model* modelFlow = createModel<FlowModule, FlowWidget>("flow");
