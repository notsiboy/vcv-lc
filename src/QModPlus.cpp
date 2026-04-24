#include "QModPlus.hpp"
#include "QArray.hpp"
#include "Theme.hpp"
#include "LcPorts.hpp"

#include <componentlibrary.hpp>
#include <history.hpp>
#include <random.hpp>
#include <ui/Slider.hpp>

// Module-scope clipboard for qmod. Copy captures dataToJson; paste applies
// it via dataFromJson on the target qmod.
static json_t* g_qmodPlusClipboard = nullptr;

namespace {
struct QModPlusJsonAction : rack::history::ModuleAction {
    json_t* oldJ = nullptr;
    json_t* newJ = nullptr;
    ~QModPlusJsonAction() {
        if (oldJ) json_decref(oldJ);
        if (newJ) json_decref(newJ);
    }
    void apply(json_t* j) {
        if (!j) return;
        rack::engine::Module* m = APP->engine->getModule(moduleId);
        QModPlusModule* qm = dynamic_cast<QModPlusModule*>(m);
        if (qm) qm->dataFromJson(j);
    }
    void undo() override { apply(oldJ); }
    void redo() override { apply(newJ); }
};
}

static void pushQModPlusJsonAction(QModPlusModule* qm, json_t* before, const char* name) {
    if (!qm || !before) { if (before) json_decref(before); return; }
    QModPlusJsonAction* a = new QModPlusJsonAction;
    a->moduleId = qm->id;
    a->name = name ? name : "qmod change";
    a->oldJ = before;
    a->newJ = qm->dataToJson();
    APP->history->push(a);
}

// ─── Module ─────────────────────────────────────────────────────────────────

QModPlusModule::QModPlusModule() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, 0);
    configInput(IN_TRIG, "Trigger / resync");
    // Per-row rate multiplier knob. Log-scaled via displayBase so the knob's
    // centre position reads as 1× (no change), left as 0.1× and right as 10×.
    for (int r = 0; r < NUM_ROWS; r++) {
        configParam(RATE_KNOB + r, -1.f, 1.f, 0.f,
                    string::f("Row %d rate", r + 1), "×", 10.f, 1.f);
    }
    for (int i = 0; i < NUM_SLOTS; i++) {
        // Even slots = left column, odd = right column.
        slotMode[i] = (i % 2 == 0) ? modeL : modeR;
        range[i] = RANGE_BI_5;
        attenuator[i] = 1.f;
        offset[i] = 0.f;
        phase[i] = random::uniform();
        shValue[i] = 0.f;
        shCounter[i] = 0.f;
        smoothFrom[i] = 0.5f;
        smoothTo[i] = random::uniform();
        smoothCounter[i] = 0.f;
        smoothPeriod[i] = 1.f;
        trigCounter[i] = 0.f;
        slewState[i] = 0.5f;
        modLevel[i] = 0.f;
        configOutput(MOD_OUTPUT + i, string::f("Mod %d", i + 1));
    }
}

void QModPlusModule::cycleColumnMode(int col) {
    int& m = (col == 0) ? modeL : modeR;
    m = (m + 1) % NUM_MODES;
    // Broadcast to this column only (even slots = left, odd = right).
    for (int i = 0; i < NUM_SLOTS; i++) {
        if ((i % 2) == col) slotMode[i] = m;
    }
}

void QModPlusModule::setColumnMode(int col, int newMode) {
    int& m = (col == 0) ? modeL : modeR;
    m = math::clamp(newMode, 0, (int)NUM_MODES - 1);
    for (int i = 0; i < NUM_SLOTS; i++) {
        if ((i % 2) == col) slotMode[i] = m;
    }
}

void QModPlusModule::cycleSlotMode(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS) return;
    slotMode[slot] = (slotMode[slot] + 1) % NUM_MODES;
}

void QModPlusModule::resyncAll() {
    // Trigger input rising edge — behaviour depends on each slot's mode.
    for (int i = 0; i < NUM_SLOTS; i++) {
        switch (slotMode[i]) {
            case MODE_LFO:
                phase[i] = 0.f;
                break;
            case MODE_SH:
                shCounter[i] = 0.f;      // resample on next process step
                break;
            case MODE_SMOOTH:
                smoothCounter[i] = 0.f;  // pick new target on next step
                break;
            case MODE_RAND_TRIG:
                trigCounter[i] = 0.f;    // fire next tick
                break;
            case MODE_TRIG_SH:
                // Triggered S+H has no internal clock — the trigger input IS
                // the clock. Sample a fresh value right now.
                shValue[i] = random::uniform();
                break;
        }
    }
}

float QModPlusModule::frequencyHz(int slot) {
    float base = (rateOverride >= 0.f) ? rateOverride : globalRate;
    // Per-row knob — but when this qmod+ shares an array with another,
    // leftmost wins. So we look up row knobs on whichever qmod+ is first
    // in the array; that could be us or a neighbour.
    auto arr = lc::walkArray(this);
    QModPlusModule* src = this;
    for (auto* m : arr) {
        if (m && m->model == modelQModPlus) {
            src = dynamic_cast<QModPlusModule*>(m);
            break;
        }
    }
    if (!src) src = this;
    int row = slot / 2;
    float knobV = src->params[RATE_KNOB + row].getValue();
    return base * std::pow(10.f, knobV);
}

void QModPlusModule::applyStaggerToKnobs() {
    // Write log-spread multipliers into the row knobs. Row 0 lands on 1×
    // (knob value 0), row NUM_ROWS-1 lands on spreadRatio× (knob value
    // log10(spreadRatio)). Intermediate rows interpolate log-linearly.
    float r = math::clamp(spreadRatio, 1e-4f, 1.f);
    float lo = std::log10(r);   // e.g. -1 for spreadRatio = 0.1
    for (int row = 0; row < NUM_ROWS; row++) {
        float t = (NUM_ROWS > 1) ? (float)row / (float)(NUM_ROWS - 1) : 0.f;
        float knobV = math::clamp(lo * t, -1.f, 1.f);
        params[RATE_KNOB + row].setValue(knobV);
    }
}

void QModPlusModule::resetRowKnobs() {
    for (int row = 0; row < NUM_ROWS; row++)
        params[RATE_KNOB + row].setValue(0.f);
}

float QModPlusModule::mapToVoltage(int slot, float v01) const {
    switch (range[slot]) {
        case RANGE_UNI_10: return v01 * 10.f;
        case RANGE_UNI_5:  return v01 * 5.f;
        case RANGE_UNI_1:  return v01 * 1.f;
        case RANGE_BI_10:  return (v01 - 0.5f) * 20.f;
        case RANGE_BI_5:   return (v01 - 0.5f) * 10.f;
        case RANGE_BI_1:   return (v01 - 0.5f) * 2.f;
    }
    return 0.f;
}

void QModPlusModule::process(const ProcessArgs& args) {
    bool gateFrozen = false;

    // Default both CV overrides off each sample; the switch below turns them
    // on if the CV is live. This keeps the menu-set values authoritative when
    // the cable's removed or the input mode is changed.
    rateOverride = -1.f;
    smoothnessOverride = -1.f;

    if (inputs[IN_TRIG].isConnected()) {
        float iv = inputs[IN_TRIG].getVoltage();
        // Accept both unipolar (0..10V) and bipolar (-5..+5V) sources by
        // folding the signal into a 0..1 window centred on 0V.
        float t = math::clamp((iv + 5.f) / 10.f, 0.f, 1.f);

        switch (inputMode) {
            case INPUT_TRIGGER: {
                if (trigInEdge.process(iv, 0.1f, 1.f)) resyncAll();
                break;
            }
            case INPUT_GATE: {
                // Schmitt semantics on the gate so noisy edges don't flicker
                // the freeze state. 0.1V low, 1V high.
                if (iv >= 1.f)      gateFrozen = false;
                else if (iv < 0.1f) gateFrozen = true;
                else                gateFrozen = (iv < 0.5f);
                break;
            }
            case INPUT_CV_RATE: {
                // Full window maps to the slider's full range (0.01..10 Hz).
                // Bipolar -5V → slowest, +5V → fastest; unipolar 0V → slowest,
                // 10V → fastest. Either works without freezing half the cycle.
                rateOverride = 0.01f + t * (10.f - 0.01f);
                break;
            }
            case INPUT_CV_SMOOTHNESS: {
                smoothnessOverride = t;
                break;
            }
            case INPUT_CV_MODE: {
                // CV → mode drives BOTH columns so a single CV can still
                // sweep the whole bank through its modes. Menu and per-slot
                // clicks can still diverge from this afterwards.
                int newMode = (int)std::min((float)(NUM_MODES - 1),
                                            t * (float)NUM_MODES);
                if (newMode != modeL) setColumnMode(0, newMode);
                if (newMode != modeR) setColumnMode(1, newMode);
                break;
            }
        }
    }

    // Live value to use for smooth-random slew shaping. Same override rule.
    float activeSmoothness = (smoothnessOverride >= 0.f)
        ? smoothnessOverride : smoothness;

    if (gateFrozen) {
        // Gate low — freeze phases/counters and keep the current output
        // voltages on the jacks. modLevel is left alone so the LEDs hold
        // their brightness too.
        return;
    }

    const float sr = args.sampleRate;

    for (int i = 0; i < NUM_SLOTS; i++) {
        float f = std::max(1e-4f, frequencyHz(i));
        float outV = 0.f;
        int m = slotMode[i];

        // Raw 0..1 signal produced by each mode. The slew limiter (below)
        // turns `smoothness` into a glide for modes with step-shaped output.
        float v01 = 0.f;
        bool slewable = false;

        if (m == MODE_LFO) {
            phase[i] += f / sr;
            if (phase[i] >= 1.f) phase[i] -= 1.f;
            float p = phase[i];
            switch (lfoShape) {
                case LFO_SINE:
                    v01 = 0.5f + 0.5f * std::sin(p * 2.f * (float)M_PI);
                    break;
                case LFO_TRIANGLE:
                    v01 = (p < 0.5f) ? (p * 2.f) : (2.f - p * 2.f);
                    break;
                case LFO_SQUARE:
                    v01 = (p < 0.5f) ? 1.f : 0.f;
                    break;
                case LFO_SAW:
                    v01 = p;
                    break;
                default:
                    v01 = 0.5f + 0.5f * std::sin(p * 2.f * (float)M_PI);
                    break;
            }
            slewable = true;
        } else if (m == MODE_SH) {
            if (shCounter[i] <= 0.f) {
                shValue[i] = random::uniform();
                shCounter[i] = sr / f;
            }
            shCounter[i] -= 1.f;
            v01 = shValue[i];
            slewable = true;
        } else if (m == MODE_TRIG_SH) {
            // No internal clock — value updates only when the trigger input
            // fires (resyncAll pokes shValue).
            v01 = shValue[i];
            slewable = true;
        } else if (m == MODE_SMOOTH) {
            if (smoothCounter[i] <= 0.f) {
                smoothFrom[i] = smoothTo[i];
                smoothTo[i] = random::uniform();
                smoothPeriod[i] = sr / f;
                smoothCounter[i] = smoothPeriod[i];
            }
            float progress = smoothPeriod[i] > 0.f
                ? 1.f - (smoothCounter[i] / smoothPeriod[i])
                : 1.f;
            progress = math::clamp(progress, 0.f, 1.f);
            // Smooth random already produces a continuous slew between
            // random targets, so the output LPF would just smear it further.
            // Keep smootherstep shaping here as before.
            float curved = progress * progress * (3.f - 2.f * progress);
            float shape = progress + (curved - progress) * activeSmoothness;
            v01 = smoothFrom[i] + (smoothTo[i] - smoothFrom[i]) * shape;
            smoothCounter[i] -= 1.f;
        } else { // MODE_RAND_TRIG
            if (trigCounter[i] <= 0.f) {
                trigPulse[i].trigger(1e-3f);    // 1 ms pulse
                // Randomise the next interval ±50% so triggers feel organic
                // rather than metronomic; mean interval = sr / f.
                float mean = sr / f;
                float jitter = mean * (0.5f + random::uniform());
                trigCounter[i] = jitter;
            }
            trigCounter[i] -= 1.f;
            bool hi = trigPulse[i].process(args.sampleTime);
            // Triggers live in the positive half of the chosen range so
            // "Bipolar -5..5V" still produces a +5V pulse.
            outV = hi ? mapToVoltage(i, 1.f) : mapToVoltage(i, 0.5f);
            if (hi) outV = std::max(outV, 5.f);
            else outV = 0.f;
            // Flash on trigger, then decay so the LED stays readable at the
            // UI's 60 fps even for sub-millisecond pulses.
            if (hi) modLevel[i] = 1.f;
            else    modLevel[i] = std::max(0.f, modLevel[i] - args.sampleTime / 0.12f);
            // Random triggers skip the slew (pulses must stay crisp) and the
            // common voltage mapping below — but still honour atten/offset so
            // a slot can be biased or inverted without rewiring.
            outV = outV * attenuator[i] + offset[i];
            outputs[MOD_OUTPUT + i].setVoltage(outV);
            continue;
        }

        // Output slew for modes that would otherwise emit step changes
        // (S+H, triggered S+H) or benefit from soft filtering (LFO). Time
        // constant is quadratic so the slider's lower half stays subtle.
        if (slewable && activeSmoothness > 0.f) {
            const float maxTau = 0.3f;  // seconds at smoothness = 1
            float tau = activeSmoothness * activeSmoothness * maxTau;
            float alpha = 1.f - std::exp(-args.sampleTime / (tau + 1e-6f));
            slewState[i] += alpha * (v01 - slewState[i]);
            v01 = slewState[i];
        } else {
            // Keep the slew state locked to the raw signal while smoothing
            // is disabled so turning the knob up doesn't cause a jump.
            slewState[i] = v01;
        }

        modLevel[i] = v01;
        outV = mapToVoltage(i, v01);
        outV = outV * attenuator[i] + offset[i];
        outputs[MOD_OUTPUT + i].setVoltage(outV);
    }
}

json_t* QModPlusModule::dataToJson() {
    json_t* root = json_object();
    json_object_set_new(root, "modeL", json_integer(modeL));
    json_object_set_new(root, "modeR", json_integer(modeR));
    json_object_set_new(root, "inputMode", json_integer(inputMode));
    json_object_set_new(root, "lfoShape", json_integer(lfoShape));
    json_object_set_new(root, "spreadRatio", json_real(spreadRatio));
    json_object_set_new(root, "globalRate", json_real(globalRate));
    json_object_set_new(root, "smoothness", json_real(smoothness));
    json_object_set_new(root, "inArray", json_boolean(inArray));
    json_t* attensJ = json_array();
    json_t* offsetsJ = json_array();
    for (int i = 0; i < NUM_SLOTS; i++) {
        json_array_append_new(attensJ,  json_real(attenuator[i]));
        json_array_append_new(offsetsJ, json_real(offset[i]));
    }
    json_object_set_new(root, "attens", attensJ);
    json_object_set_new(root, "offsets", offsetsJ);
    json_t* rangesJ = json_array();
    for (int i = 0; i < NUM_SLOTS; i++)
        json_array_append_new(rangesJ, json_integer(range[i]));
    json_object_set_new(root, "ranges", rangesJ);
    json_t* modesJ = json_array();
    for (int i = 0; i < NUM_SLOTS; i++)
        json_array_append_new(modesJ, json_integer(slotMode[i]));
    json_object_set_new(root, "slotModes", modesJ);
    return root;
}

void QModPlusModule::dataFromJson(json_t* root) {
    if (json_t* j = json_object_get(root, "modeL"))
        modeL = (int)json_integer_value(j);
    if (json_t* j = json_object_get(root, "modeR"))
        modeR = (int)json_integer_value(j);
    if (json_t* j = json_object_get(root, "inputMode"))
        inputMode = (int)json_integer_value(j);
    if (json_t* j = json_object_get(root, "lfoShape"))
        lfoShape = (int)json_integer_value(j);
    if (json_t* j = json_object_get(root, "spreadRatio"))
        spreadRatio = json_real_value(j);
    if (json_t* j = json_object_get(root, "globalRate"))
        globalRate = json_real_value(j);
    if (json_t* j = json_object_get(root, "smoothness"))
        smoothness = json_real_value(j);
    if (json_t* j = json_object_get(root, "inArray"))
        inArray = json_boolean_value(j);
    for (int i = 0; i < NUM_SLOTS; i++) { attenuator[i] = 1.f; offset[i] = 0.f; }
    if (json_t* a = json_object_get(root, "attens")) {
        size_t n = std::min((size_t)NUM_SLOTS, json_array_size(a));
        for (size_t i = 0; i < n; i++) attenuator[i] = json_real_value(json_array_get(a, i));
    }
    if (json_t* a = json_object_get(root, "offsets")) {
        size_t n = std::min((size_t)NUM_SLOTS, json_array_size(a));
        for (size_t i = 0; i < n; i++) offset[i] = json_real_value(json_array_get(a, i));
    }
    if (json_t* rangesJ = json_object_get(root, "ranges")) {
        size_t n = std::min((size_t)NUM_SLOTS, json_array_size(rangesJ));
        for (size_t i = 0; i < n; i++)
            range[i] = (int)json_integer_value(json_array_get(rangesJ, i));
    }
    // Default every slot to its column's mode if the slotModes array is
    // missing (e.g. reading from an older save or a hand-edited JSON).
    for (int i = 0; i < NUM_SLOTS; i++)
        slotMode[i] = (i % 2 == 0) ? modeL : modeR;
    if (json_t* modesJ = json_object_get(root, "slotModes")) {
        size_t n = std::min((size_t)NUM_SLOTS, json_array_size(modesJ));
        for (size_t i = 0; i < n; i++)
            slotMode[i] = (int)json_integer_value(json_array_get(modesJ, i));
    }
}

// ─── Widget helpers ─────────────────────────────────────────────────────────

namespace {

struct QModPlusBackground : widget::Widget {
    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, lc::panelBg());
        nvgFill(args.vg);
    }
};

struct QModPlusLabel : widget::Widget {
    std::string text;
    float fontSize = 7.f;
    void draw(const DrawArgs& args) override {
        std::shared_ptr<window::Font> font = APP->window->loadFont(
            asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        bool dark = lc::theme.dark;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, fontSize);
        nvgTextLetterSpacing(args.vg, 0.2f);
        nvgFillColor(args.vg, dark ? nvgRGB(230, 230, 230) : nvgRGB(26, 26, 26));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(args.vg, box.size.x / 2.f, 0.f, text.c_str(), NULL);
    }
};

// Dynamic slot-number label for qmod+. Reads lc::arraySlotBase each frame.
struct SlotNumberLabel : widget::Widget {
    engine::Module* module = nullptr;
    int slot = 0;
    void draw(const DrawArgs& args) override {
        if (!module) return;
        int base = lc::arraySlotBase(module);
        int n = base + slot + 1;
        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, 6.5f);
        nvgFillColor(args.vg, lc::theme.dark ? nvgRGB(190, 190, 190) : nvgRGB(70, 70, 70));
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, box.size.x, box.size.y / 2.f, string::f("%d", n).c_str(), nullptr);
    }
};

struct QModPlusLogo : widget::Widget {
    std::string path, darkPath, greyPath;
    void draw(const DrawArgs& args) override {
        std::string use = lc::logoAsset(path, darkPath, greyPath);
        std::shared_ptr<window::Image> img = APP->window->loadImage(use);
        if (!img || img->handle < 0) return;
        NVGpaint paint = nvgImagePattern(args.vg, 0, 0, box.size.x, box.size.y, 0, img->handle, 1.f);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillPaint(args.vg, paint);
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
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, rOuter);
    nvgFillColor(vg, rim);
    nvgFill(vg);
    nvgStrokeColor(vg, rimEdge);
    nvgStrokeWidth(vg, 0.6f);
    nvgStroke(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, rInner);
    nvgFillColor(vg, face);
    nvgFill(vg);
    nvgStrokeColor(vg, faceEdge);
    nvgStrokeWidth(vg, 0.4f);
    nvgStroke(vg);
}

static void drawCenterDot(NVGcontext* vg, float w, float h, NVGcolor color, bool on, bool dark,
                          float scale) {
    float cx = w / 2.f, cy = h / 2.f;
    float rOuter = std::min(cx, cy) - 0.5f;
    float rLed = rOuter * scale;
    NVGcolor off = dark ? nvgRGB(80, 80, 80) : nvgRGB(180, 180, 180);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, rLed);
    nvgFillColor(vg, on ? color : off);
    nvgFill(vg);
}

static void drawCenterDotGlow(NVGcontext* vg, float w, float h, NVGcolor color, float alpha,
                              float scale) {
    float cx = w / 2.f, cy = h / 2.f;
    float rOuter = std::min(cx, cy) - 0.5f;
    float rLed = rOuter * scale;
    int a = (int)math::clamp(alpha * 180.f, 0.f, 255.f);
    NVGpaint glow = nvgRadialGradient(vg, cx, cy, rLed * 0.6f, rLed * 3.f,
        nvgRGBA((int)(color.r * 255), (int)(color.g * 255), (int)(color.b * 255), a),
        nvgRGBA((int)(color.r * 255), (int)(color.g * 255), (int)(color.b * 255), 0));
    nvgBeginPath(vg);
    nvgRect(vg, cx - rLed * 4, cy - rLed * 4, rLed * 8, rLed * 8);
    nvgFillPaint(vg, glow);
    nvgFill(vg);
}

static NVGcolor modeColor(int m) {
    switch (m) {
        case QModPlusModule::MODE_RAND_TRIG: return nvgRGB(220, 70, 70);   // red
        case QModPlusModule::MODE_TRIG_SH:   return nvgRGB(255, 140, 40);  // orange
        case QModPlusModule::MODE_SMOOTH:    return nvgRGB(70, 200, 210);  // cyan
        case QModPlusModule::MODE_SH:        return nvgRGB(180, 80, 220);  // purple
        case QModPlusModule::MODE_LFO:       return nvgRGB(60, 210, 90);   // green
    }
    return nvgRGB(180, 180, 180);
}

// Per-slot mode LED. Displays the slot's current mode colour (set by the
// master button's broadcast, or by clicking this button to diverge from the
// global mode). Left-click cycles just this slot.
struct SlotModeButton : widget::OpaqueWidget {
    QModPlusModule* qm = nullptr;
    int slot = -1;
    float dotScale = 0.55f;

    int currentMode() const { return qm ? qm->slotMode[slot] : 0; }
    float level() const {
        return qm ? math::clamp(qm->modLevel[slot], 0.f, 1.f) : 0.f;
    }

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        drawRoundButton(args.vg, box.size.x, box.size.y, dark);
        if (!qm) return;
        // Base dot: always a dim mode-coloured disc so the mode assignment
        // reads even when the CV is at its minimum.
        NVGcolor c = modeColor(currentMode());
        float b = 0.25f + 0.55f * level();
        NVGcolor dim = nvgRGBf(c.r * b, c.g * b, c.b * b);
        drawCenterDot(args.vg, box.size.x, box.size.y, dim, true, dark, dotScale);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && qm) {
            // Glow alpha tracks the CV — brighter when the output is near
            // its positive peak, dim at the negative peak (or between
            // triggers in random-trig mode).
            float alpha = level();
            if (alpha > 0.f) {
                drawCenterDotGlow(args.vg, box.size.x, box.size.y,
                                  modeColor(currentMode()), alpha, dotScale);
            }
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && qm) {
            json_t* before = qm->dataToJson();
            qm->cycleSlotMode(slot);
            pushQModPlusJsonAction(qm, before, "qmod slot mode");
            e.consume(this);
        }
    }
};

// Column mode-cycle button. Left-click steps through the modes for just
// this column (left=0 or right=1). Each column has its own button and LED
// colour tracking its own mode.
struct ModeCycleButton : widget::OpaqueWidget {
    QModPlusModule* qm = nullptr;
    int col = 0;
    float dotScale = 0.50f;

    int columnMode() const {
        if (!qm) return 0;
        return (col == 0) ? qm->modeL : qm->modeR;
    }

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        drawRoundButton(args.vg, box.size.x, box.size.y, dark);
        if (qm) {
            NVGcolor c = modeColor(columnMode());
            drawCenterDot(args.vg, box.size.x, box.size.y, c, true, dark, dotScale);
        } else {
            drawCenterDot(args.vg, box.size.x, box.size.y, nvgRGB(180,180,180), false, dark, dotScale);
        }
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && qm) {
            drawCenterDotGlow(args.vg, box.size.x, box.size.y, modeColor(columnMode()), 0.7f, dotScale);
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && qm) {
            json_t* before = qm->dataToJson();
            qm->cycleColumnMode(col);
            pushQModPlusJsonAction(qm, before, col == 0 ? "qmod+ left mode" : "qmod+ right mode");
            e.consume(this);
        }
    }
};

// Simple Quantity wrapper around a float* so ui::Slider can edit module state.
struct FloatQuantity : Quantity {
    float* ptr;
    std::string label_, unit_;
    float minV, maxV, defV;
    int precision;
    FloatQuantity(float* p, std::string l, std::string u, float mn, float mx, float dv, int pr = 2)
        : ptr(p), label_(std::move(l)), unit_(std::move(u)), minV(mn), maxV(mx), defV(dv), precision(pr) {}
    void setValue(float v) override { *ptr = math::clamp(v, minV, maxV); }
    float getValue() override { return *ptr; }
    float getMinValue() override { return minV; }
    float getMaxValue() override { return maxV; }
    float getDefaultValue() override { return defV; }
    std::string getLabel() override { return label_; }
    std::string getUnit() override { return unit_; }
    int getDisplayPrecision() override { return precision; }
};

struct MenuSlider : ui::Slider {
    MenuSlider(Quantity* q) {
        quantity = q;
        box.size.x = 220.f;
    }
    ~MenuSlider() { delete quantity; }
};

// Trigger-input jack with an inline Input-jack submenu so the mode picker
// is reachable with a right-click directly on the port.
struct TrigInputPort : lc::WhiteRingPJ301MPort {
    QModPlusModule* qm = nullptr;
    void appendContextMenu(ui::Menu* menu) override {
        if (!qm) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Input jack mode"));
        auto addInput = [&](const char* label, int v) {
            menu->addChild(createCheckMenuItem(label, "",
                [=]() { return qm->inputMode == v; },
                [=]() { qm->inputMode = v; }));
        };
        addInput("Trigger / resync",   QModPlusModule::INPUT_TRIGGER);
        addInput("Gate (run/freeze)",  QModPlusModule::INPUT_GATE);
        addInput("CV → rate",          QModPlusModule::INPUT_CV_RATE);
        addInput("CV → smoothness",    QModPlusModule::INPUT_CV_SMOOTHNESS);
        addInput("CV → mode",          QModPlusModule::INPUT_CV_MODE);
    }
};

struct QModPlusOutputPort : ThemedPJ301MPort {
    QModPlusModule* qm = nullptr;
    int slot = -1;

    void draw(const DrawArgs& args) override {
        ThemedPJ301MPort::draw(args);
        if (!module || slot < 0) return;
        int modBase = lc::arraySlotBase(module);
        int globalIdx = modBase + slot;
        bool paired = false;
        auto arr = lc::walkArray(module);
        for (auto* m : arr) {
            if (!m || m == module || m->model != modelQMap) continue;
            int qmapBase = lc::arraySlotBase(m);
            if (globalIdx >= qmapBase && globalIdx < qmapBase + QModPlusModule::NUM_SLOTS) {
                paired = true;
                break;
            }
        }
        if (!paired) return;
        float cx = box.size.x / 2.f, cy = box.size.y / 2.f;
        float r = box.size.x * 0.08f;
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, r);
        nvgFillColor(args.vg, nvgRGB(255, 255, 255));
        nvgFill(args.vg);
    }

    void appendContextMenu(ui::Menu* menu) override {
        if (!qm) return;
        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel(string::f("Mod %d range", slot + 1)));
        auto addRange = [&](const char* label, int r) {
            menu->addChild(createCheckMenuItem(label, "",
                [=]() { return qm && qm->range[slot] == r; },
                [=]() { if (qm) qm->range[slot] = r; }));
        };
        addRange("Unipolar 0..10V", QModPlusModule::RANGE_UNI_10);
        addRange("Unipolar 0..5V",  QModPlusModule::RANGE_UNI_5);
        addRange("Unipolar 0..1V",  QModPlusModule::RANGE_UNI_1);
        addRange("Bipolar -10..10V", QModPlusModule::RANGE_BI_10);
        addRange("Bipolar -5..5V",   QModPlusModule::RANGE_BI_5);
        addRange("Bipolar -1..1V",   QModPlusModule::RANGE_BI_1);

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Attenuator"));
        menu->addChild(new MenuSlider(new FloatQuantity(
            &qm->attenuator[slot], "Attenuator", "×", -2.f, 2.f, 1.f, 2)));
        menu->addChild(createMenuLabel("Offset"));
        menu->addChild(new MenuSlider(new FloatQuantity(
            &qm->offset[slot], "Offset", " V", -10.f, 10.f, 0.f, 2)));
    }
};

} // namespace

// ─── Widget ─────────────────────────────────────────────────────────────────

QModPlusWidget::QModPlusWidget(QModPlusModule* module) {
    setModule(module);
    // 6 HP — 4 HP of qmod's existing layout on the left, 2 HP of rate-knob
    // strip on the right.
    box.size = Vec(RACK_GRID_WIDTH * 6, RACK_GRID_HEIGHT);

    QModPlusBackground* bg = new QModPlusBackground;
    bg->box.size = box.size;
    addChild(bg);

    float screwX = (box.size.x - RACK_GRID_WIDTH) / 2.f;
    addChild(createWidget<ScrewBlack>(Vec(screwX, 0)));
    addChild(createWidget<ScrewBlack>(Vec(screwX, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    app::PanelBorder* border = new app::PanelBorder;
    border->box.size = box.size;
    addChild(border);

    // Left 4 HP mirrors qmod exactly — same column centres, same row pitch
    // — so a qmod+ lines up with neighbouring qmap/qmod modules. The new
    // 2 HP strip sits at x > 20.32 mm.
    const float qmodPanelW_mm = 4.f * (float)RACK_GRID_WIDTH / mm2px(1.f);
    const float colL_mm = qmodPanelW_mm * 0.25f;
    const float colR_mm = qmodPanelW_mm * 0.75f;
    // Right strip: centred in the new 2 HP area (x = 20.32..30.48 mm).
    const float rightStripX_mm = qmodPanelW_mm + (2.f * (float)RACK_GRID_WIDTH / mm2px(1.f)) * 0.5f;

    // Title — same y as qmod. Centred across the full 6 HP panel so it's
    // clearly visible.
    {
        QModPlusLabel* lab = new QModPlusLabel;
        lab->text = "qmod";
        lab->fontSize = 7.f;
        lab->box.size = Vec(box.size.x, mm2px(3.f));
        lab->box.pos = Vec(0, mm2px(7.f));
        addChild(lab);
    }

    // Header row: two column mode-cycle buttons side by side over the
    // output columns, trigger input moved to the new right strip.
    const float headerY = 14.5f;
    {
        ModeCycleButton* b = new ModeCycleButton;
        b->qm = module;
        b->col = 0;
        b->box.size = mm2px(Vec(5.f, 5.f));
        b->box.pos = Vec(mm2px(colL_mm) - b->box.size.x / 2.f,
                         mm2px(headerY) - b->box.size.y / 2.f);
        addChild(b);
    }
    {
        ModeCycleButton* b = new ModeCycleButton;
        b->qm = module;
        b->col = 1;
        b->box.size = mm2px(Vec(5.f, 5.f));
        b->box.pos = Vec(mm2px(colR_mm) - b->box.size.x / 2.f,
                         mm2px(headerY) - b->box.size.y / 2.f);
        addChild(b);
    }
    {
        auto* trig = createInputCentered<TrigInputPort>(
            mm2px(Vec(rightStripX_mm, headerY)), module, QModPlusModule::IN_TRIG);
        trig->qm = module;
        addInput(trig);
    }

    // 7 rows × 2 columns — same geometry as qmod.
    const float bottomJackY = 106.f;
    const int numRows = QModPlusModule::NUM_ROWS;
    const float rowStep = 13.f;
    const float btnAboveJack = 6.5f;
    const float btnSize = 2.6f;
    for (int i = 0; i < QModPlusModule::NUM_SLOTS; i++) {
        int row = i / 2;
        int col = i % 2;
        float jackY = bottomJackY - (numRows - 1 - row) * rowStep;
        float cx = (col == 0) ? colL_mm : colR_mm;

        const float btnShift = 1.5f;
        SlotModeButton* btn = new SlotModeButton;
        btn->qm = module;
        btn->slot = i;
        btn->box.size = mm2px(Vec(btnSize, btnSize));
        btn->box.pos = Vec(mm2px(cx + btnShift) - btn->box.size.x / 2.f,
                           mm2px(jackY - btnAboveJack) - btn->box.size.y / 2.f);
        addChild(btn);

        SlotNumberLabel* lbl = new SlotNumberLabel;
        lbl->module = module;
        lbl->slot = i;
        lbl->box.size = mm2px(Vec(3.f, 3.f));
        lbl->box.pos = Vec(mm2px(cx - btnShift - 0.2f) - lbl->box.size.x,
                           mm2px(jackY - btnAboveJack) - lbl->box.size.y / 2.f);
        addChild(lbl);

        QModPlusOutputPort* port = createOutputCentered<QModPlusOutputPort>(
            mm2px(Vec(cx, jackY)), module, QModPlusModule::MOD_OUTPUT + i);
        port->qm = module;
        port->slot = i;
        addOutput(port);
    }

    // Per-row rate knobs — one Trimpot in the right strip at each row's jack
    // y. Centre-click resets to 0 (= 1× multiplier).
    for (int r = 0; r < QModPlusModule::NUM_ROWS; r++) {
        float jackY = bottomJackY - (numRows - 1 - r) * rowStep;
        addParam(createParamCentered<Trimpot>(
            mm2px(Vec(rightStripX_mm, jackY)), module,
            QModPlusModule::RATE_KNOB + r));
    }

    // Logo.
    {
        QModPlusLogo* logo = new QModPlusLogo;
        logo->path     = asset::plugin(pluginInstance, "res/lc-icon-new.png");
        logo->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");
        logo->greyPath = asset::plugin(pluginInstance, "res/lc-icon-grey.png");
        logo->box.size = mm2px(Vec(9.f, 9.f));
        logo->box.pos  = Vec((box.size.x - logo->box.size.x) / 2.f, mm2px(128.5f - 8.f - 9.f + 2.f));
        addChild(logo);
    }
}

void QModPlusWidget::appendContextMenu(Menu* menu) {
    QModPlusModule* qm = dynamic_cast<QModPlusModule*>(module);
    if (!qm) return;

    menu->addChild(new MenuSeparator);
    auto modeLabel = [](int m) -> std::string {
        switch (m) {
            case QModPlusModule::MODE_RAND_TRIG: return "Random triggers";
            case QModPlusModule::MODE_TRIG_SH:   return "Triggered S+H (ext. clock)";
            case QModPlusModule::MODE_SMOOTH:    return "Smooth random";
            case QModPlusModule::MODE_SH:        return "Sample & hold";
            case QModPlusModule::MODE_LFO:       return "LFO";
        }
        return "";
    };
    auto modePicker = [=](int col) {
        return [=](ui::Menu* sub) {
            auto addMode = [&](int m) {
                sub->addChild(createCheckMenuItem(modeLabel(m), "",
                    [=]() {
                        int cur = (col == 0) ? qm->modeL : qm->modeR;
                        return cur == m;
                    },
                    [=]() { qm->setColumnMode(col, m); }));
            };
            addMode(QModPlusModule::MODE_RAND_TRIG);
            addMode(QModPlusModule::MODE_TRIG_SH);
            addMode(QModPlusModule::MODE_SMOOTH);
            addMode(QModPlusModule::MODE_SH);
            addMode(QModPlusModule::MODE_LFO);
        };
    };
    menu->addChild(createSubmenuItem("Left column mode", modeLabel(qm->modeL), modePicker(0)));
    menu->addChild(createSubmenuItem("Right column mode", modeLabel(qm->modeR), modePicker(1)));
    menu->addChild(createMenuItem("Set both columns to left's mode", "", [=]() {
        qm->setColumnMode(1, qm->modeL);
    }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createSubmenuItem("LFO waveshape", [=]() {
        switch (qm->lfoShape) {
            case QModPlusModule::LFO_SINE:     return "Sine";
            case QModPlusModule::LFO_TRIANGLE: return "Triangle";
            case QModPlusModule::LFO_SQUARE:   return "Square";
            case QModPlusModule::LFO_SAW:      return "Saw";
        }
        return "";
    }(), [=](ui::Menu* sub) {
        auto addShape = [&](const char* label, int s) {
            sub->addChild(createCheckMenuItem(label, "",
                [=]() { return qm->lfoShape == s; },
                [=]() { qm->lfoShape = s; }));
        };
        addShape("Sine",     QModPlusModule::LFO_SINE);
        addShape("Triangle", QModPlusModule::LFO_TRIANGLE);
        addShape("Square",   QModPlusModule::LFO_SQUARE);
        addShape("Saw",      QModPlusModule::LFO_SAW);
    }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Global rate"));
    menu->addChild(new MenuSlider(new FloatQuantity(
        &qm->globalRate, "Rate", " Hz", 0.01f, 10.f, 4.5f, 3)));

    menu->addChild(createMenuLabel("Smoothness (slew / shape)"));
    menu->addChild(new MenuSlider(new FloatQuantity(
        &qm->smoothness, "Smoothness", "", 0.f, 1.f, 0.4f, 2)));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Stagger spread (applied to row knobs)"));
    menu->addChild(new MenuSlider(new FloatQuantity(
        &qm->spreadRatio, "Spread", "×", 0.005f, 1.f, 0.1f, 3)));
    menu->addChild(createMenuItem("Apply stagger to row knobs", "",
        [=]() {
            json_t* before = qm->dataToJson();
            qm->applyStaggerToKnobs();
            pushQModPlusJsonAction(qm, before, "qmod+ apply stagger");
        }));
    menu->addChild(createMenuItem("Reset row knobs to 1×", "",
        [=]() {
            json_t* before = qm->dataToJson();
            qm->resetRowKnobs();
            pushQModPlusJsonAction(qm, before, "qmod+ reset row knobs");
        }));

    auto rangeChecked = [=](int r) {
        int first = qm->range[0];
        for (int i = 1; i < QModPlusModule::NUM_SLOTS; i++)
            if (qm->range[i] != first) return false;
        return first == r;
    };
    auto setAllRange = [=](int r) {
        for (int i = 0; i < QModPlusModule::NUM_SLOTS; i++) qm->range[i] = r;
    };
    auto rangeLabelFor = [=]() -> std::string {
        int first = qm->range[0];
        for (int i = 1; i < QModPlusModule::NUM_SLOTS; i++)
            if (qm->range[i] != first) return "Mixed";
        switch (first) {
            case QModPlusModule::RANGE_UNI_10: return "0..10 V";
            case QModPlusModule::RANGE_UNI_5:  return "0..5 V";
            case QModPlusModule::RANGE_UNI_1:  return "0..1 V";
            case QModPlusModule::RANGE_BI_10:  return "-10..10 V";
            case QModPlusModule::RANGE_BI_5:   return "-5..5 V";
            case QModPlusModule::RANGE_BI_1:   return "-1..1 V";
        }
        return "";
    };
    menu->addChild(createSubmenuItem("Range (all slots)", rangeLabelFor(),
        [=](ui::Menu* rs) {
            auto addRange = [&](const char* label, int r) {
                rs->addChild(createCheckMenuItem(label, "",
                    [=]() { return rangeChecked(r); },
                    [=]() { setAllRange(r); }));
            };
            rs->addChild(createMenuLabel("Unipolar"));
            addRange("0..10 V", QModPlusModule::RANGE_UNI_10);
            addRange("0..5 V",  QModPlusModule::RANGE_UNI_5);
            addRange("0..1 V",  QModPlusModule::RANGE_UNI_1);
            rs->addChild(new MenuSeparator);
            rs->addChild(createMenuLabel("Bipolar"));
            addRange("-10..10 V", QModPlusModule::RANGE_BI_10);
            addRange("-5..5 V",   QModPlusModule::RANGE_BI_5);
            addRange("-1..1 V",   QModPlusModule::RANGE_BI_1);
        }));

    // How the trigger input jack is read.
    menu->addChild(new MenuSeparator);
    auto inputModeLabel = [=]() -> std::string {
        switch (qm->inputMode) {
            case QModPlusModule::INPUT_TRIGGER:       return "Trigger / resync";
            case QModPlusModule::INPUT_GATE:          return "Gate (run/freeze)";
            case QModPlusModule::INPUT_CV_RATE:       return "CV → rate";
            case QModPlusModule::INPUT_CV_SMOOTHNESS: return "CV → smoothness";
            case QModPlusModule::INPUT_CV_MODE:       return "CV → mode";
        }
        return "";
    };
    menu->addChild(createSubmenuItem("Input jack", inputModeLabel(),
        [=](ui::Menu* sub) {
            auto addInput = [&](const char* label, int v) {
                sub->addChild(createCheckMenuItem(label, "",
                    [=]() { return qm->inputMode == v; },
                    [=]() { qm->inputMode = v; }));
            };
            addInput("Trigger / resync",   QModPlusModule::INPUT_TRIGGER);
            addInput("Gate (run/freeze)",  QModPlusModule::INPUT_GATE);
            addInput("CV → rate",          QModPlusModule::INPUT_CV_RATE);
            addInput("CV → smoothness",    QModPlusModule::INPUT_CV_SMOOTHNESS);
            addInput("CV → mode",          QModPlusModule::INPUT_CV_MODE);
        }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createBoolPtrMenuItem("Join array with neighbouring LC Q modules", "", &qm->inArray));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Copy settings", "", [=]() {
        if (g_qmodPlusClipboard) json_decref(g_qmodPlusClipboard);
        g_qmodPlusClipboard = qm->dataToJson();
    }));
    menu->addChild(createMenuItem("Paste settings", "",
        [=]() {
            if (!g_qmodPlusClipboard) return;
            json_t* before = qm->dataToJson();
            qm->dataFromJson(g_qmodPlusClipboard);
            pushQModPlusJsonAction(qm, before, "paste qmod settings");
        },
        /*disabled*/ g_qmodPlusClipboard == nullptr));

    // Adjacency status — shows whether a qmap is currently detected on
    // either side, with a hint when it isn't. Checked at menu-open time.
    menu->addChild(new MenuSeparator);
    bool linkedLeft = qm->leftExpander.module
                      && qm->leftExpander.module->model == modelQMap;
    bool linkedRight = qm->rightExpander.module
                       && qm->rightExpander.module->model == modelQMap;
    if (linkedLeft || linkedRight) {
        std::string side = linkedLeft ? "left" : "right";
        menu->addChild(createMenuLabel(
            string::f("qmap linked on the %s — outputs auto-feed its inputs",
                      side.c_str())));
    } else {
        menu->addChild(createMenuLabel(
            "No qmap adjacent — place one next to this module to auto-feed its aux inputs"));
    }

    menu->addChild(new MenuSeparator);
    lc::appendThemeMenu(menu);
}

// Slug "qmod" — this is the user-facing "qmod" module (6 HP, with row knobs).
// The C++ class names stay QModPlusModule / QModPlusWidget for historical
// reasons; the slug is what users see and what patches reference.
Model* modelQModPlus = createModel<QModPlusModule, QModPlusWidget>("qmod");
