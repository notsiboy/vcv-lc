#include "QMod.hpp"
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
static json_t* g_qmodClipboard = nullptr;

namespace {
struct QModJsonAction : rack::history::ModuleAction {
    json_t* oldJ = nullptr;
    json_t* newJ = nullptr;
    ~QModJsonAction() {
        if (oldJ) json_decref(oldJ);
        if (newJ) json_decref(newJ);
    }
    void apply(json_t* j) {
        if (!j) return;
        rack::engine::Module* m = APP->engine->getModule(moduleId);
        QModModule* qm = dynamic_cast<QModModule*>(m);
        if (qm) qm->dataFromJson(j);
    }
    void undo() override { apply(oldJ); }
    void redo() override { apply(newJ); }
};
}

static void pushQModJsonAction(QModModule* qm, json_t* before, const char* name) {
    if (!qm || !before) { if (before) json_decref(before); return; }
    QModJsonAction* a = new QModJsonAction;
    a->moduleId = qm->id;
    a->name = name ? name : "qmod change";
    a->oldJ = before;
    a->newJ = qm->dataToJson();
    APP->history->push(a);
}

// ─── Module ─────────────────────────────────────────────────────────────────

QModModule::QModModule() {
    config(0, NUM_INPUTS, NUM_OUTPUTS, 0);
    for (int i = 0; i < NUM_SLOTS; i++) {
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

void QModModule::cycleColumnMode(int col) {
    int& m = (col == 0) ? modeL : modeR;
    m = (m + 1) % NUM_MODES;
    for (int i = 0; i < NUM_SLOTS; i++) {
        if ((i % 2) == col) slotMode[i] = m;
    }
}

void QModModule::setColumnMode(int col, int newMode) {
    int& m = (col == 0) ? modeL : modeR;
    m = math::clamp(newMode, 0, (int)NUM_MODES - 1);
    for (int i = 0; i < NUM_SLOTS; i++) {
        if ((i % 2) == col) slotMode[i] = m;
    }
}

void QModModule::cycleSlotMode(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS) return;
    slotMode[slot] = (slotMode[slot] + 1) % NUM_MODES;
}

float QModModule::frequencyHz(int slot) {
    float base = globalRate;
    float freq = base;
    if (rateSpread && NUM_SLOTS > 1) {
        float r = math::clamp(spreadRatio, 1e-4f, 1.f);
        float t = (float)slot / (float)(NUM_SLOTS - 1);
        freq = base * std::pow(r, t);
    }
    // If the same Q-array contains a qmod+ to the left of us (leftmost
    // wins), pick up this row's knob value from it — that's how row knobs
    // propagate across multiple qmods in an array.
    auto arr = lc::walkArray(this);
    for (auto* m : arr) {
        if (m && m->model == modelQModPlus) {
            auto* qmp = dynamic_cast<QModPlusModule*>(m);
            if (!qmp) break;
            int row = slot / 2;
            if (row >= 0 && row < QModPlusModule::NUM_ROWS) {
                float knobV = qmp->params[QModPlusModule::RATE_KNOB + row].getValue();
                freq *= std::pow(10.f, knobV);
            }
            break;
        }
    }
    return freq;
}

float QModModule::mapToVoltage(int slot, float v01) const {
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

void QModModule::process(const ProcessArgs& args) {
    // No trigger/CV input any more — all behaviour comes from the two column
    // mode buttons and the right-click menu.
    float activeSmoothness = smoothness;
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

json_t* QModModule::dataToJson() {
    json_t* root = json_object();
    json_object_set_new(root, "modeL", json_integer(modeL));
    json_object_set_new(root, "modeR", json_integer(modeR));
    json_object_set_new(root, "lfoShape", json_integer(lfoShape));
    json_object_set_new(root, "inArray", json_boolean(inArray));
    json_object_set_new(root, "rateSpread", json_boolean(rateSpread));
    json_object_set_new(root, "spreadRatio", json_real(spreadRatio));
    json_object_set_new(root, "globalRate", json_real(globalRate));
    json_object_set_new(root, "smoothness", json_real(smoothness));
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

void QModModule::dataFromJson(json_t* root) {
    if (json_t* j = json_object_get(root, "modeL"))
        modeL = (int)json_integer_value(j);
    if (json_t* j = json_object_get(root, "modeR"))
        modeR = (int)json_integer_value(j);
    if (json_t* j = json_object_get(root, "lfoShape"))
        lfoShape = (int)json_integer_value(j);
    if (json_t* j = json_object_get(root, "inArray"))
        inArray = json_boolean_value(j);
    if (json_t* j = json_object_get(root, "rateSpread"))
        rateSpread = json_boolean_value(j);
    if (json_t* j = json_object_get(root, "spreadRatio"))
        spreadRatio = json_real_value(j);
    if (json_t* j = json_object_get(root, "globalRate"))
        globalRate = json_real_value(j);
    if (json_t* j = json_object_get(root, "smoothness"))
        smoothness = json_real_value(j);
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
    // Default every slot to its column's mode first, so legacy saves with
    // no slotModes array still produce sensible state.
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

struct QModBackground : widget::Widget {
    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, lc::panelBg());
        nvgFill(args.vg);
    }
};

struct QModLabel : widget::Widget {
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

// Dynamic slot-number label for qmod. Reads lc::arraySlotBase each frame.
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

struct QModLogo : widget::Widget {
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
        case QModModule::MODE_RAND_TRIG: return nvgRGB(220, 70, 70);   // red
        case QModModule::MODE_TRIG_SH:   return nvgRGB(255, 140, 40);  // orange
        case QModModule::MODE_SMOOTH:    return nvgRGB(70, 200, 210);  // cyan
        case QModModule::MODE_SH:        return nvgRGB(180, 80, 220);  // purple
        case QModModule::MODE_LFO:       return nvgRGB(60, 210, 90);   // green
    }
    return nvgRGB(180, 180, 180);
}

// Per-slot mode LED. Displays the slot's current mode colour (set by the
// master button's broadcast, or by clicking this button to diverge from the
// global mode). Left-click cycles just this slot.
struct SlotModeButton : widget::OpaqueWidget {
    QModModule* qm = nullptr;
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
            pushQModJsonAction(qm, before, "qmod slot mode");
            e.consume(this);
        }
    }
};

// Column mode-cycle button. Left-click cycles the modes for just this
// column (0 = left / even slots, 1 = right / odd slots).
struct ModeCycleButton : widget::OpaqueWidget {
    QModModule* qm = nullptr;
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
            pushQModJsonAction(qm, before, col == 0 ? "qmod left mode" : "qmod right mode");
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

struct QModOutputPort : ThemedPJ301MPort {
    QModModule* qm = nullptr;
    int slot = -1;

    void draw(const DrawArgs& args) override {
        ThemedPJ301MPort::draw(args);
        if (!module) return;
        // Dot shows when this qmod shares a Q-array with any qmap.
        bool linked = false;
        auto arr = lc::walkArray(module);
        for (auto* m : arr) {
            if (m && m != module && m->model == modelQMap) {
                linked = true;
                break;
            }
        }
        if (!linked) return;
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
        addRange("Unipolar 0..10V", QModModule::RANGE_UNI_10);
        addRange("Unipolar 0..5V",  QModModule::RANGE_UNI_5);
        addRange("Unipolar 0..1V",  QModModule::RANGE_UNI_1);
        addRange("Bipolar -10..10V", QModModule::RANGE_BI_10);
        addRange("Bipolar -5..5V",   QModModule::RANGE_BI_5);
        addRange("Bipolar -1..1V",   QModModule::RANGE_BI_1);

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

QModWidget::QModWidget(QModModule* module) {
    setModule(module);
    box.size = Vec(RACK_GRID_WIDTH * 4, RACK_GRID_HEIGHT);

    QModBackground* bg = new QModBackground;
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

    // Title — pulled up slightly so the header row (button + trigger input)
    // has space to sit at the full PJ301M height without colliding.
    {
        QModLabel* lab = new QModLabel;
        lab->text = "qmod+";
        lab->fontSize = 7.f;
        lab->box.size = Vec(box.size.x, mm2px(3.f));
        lab->box.pos = Vec(0, mm2px(7.f));
        addChild(lab);
    }

    // Header row: two per-column mode-cycle buttons — one above each output
    // column — centred on y = 14.5 mm. The old trigger/CV jack is gone.
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

    // 7 rows × 2 columns — identical geometry to qmap so both modules line up
    // row-for-row when placed side by side.
    const float bottomJackY = 106.f;
    const int numRows = QModModule::NUM_SLOTS / 2;
    const float rowStep = 13.f;
    const float btnAboveJack = 6.5f;
    const float btnSize = 2.6f;
    for (int i = 0; i < QModModule::NUM_SLOTS; i++) {
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

        QModOutputPort* port = createOutputCentered<QModOutputPort>(
            mm2px(Vec(cx, jackY)), module, QModModule::MOD_OUTPUT + i);
        port->qm = module;
        port->slot = i;
        addOutput(port);
    }

    // Logo.
    {
        QModLogo* logo = new QModLogo;
        logo->path     = asset::plugin(pluginInstance, "res/lc-icon-new.png");
        logo->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");
        logo->greyPath = asset::plugin(pluginInstance, "res/lc-icon-grey.png");
        logo->box.size = mm2px(Vec(9.f, 9.f));
        logo->box.pos  = Vec((box.size.x - logo->box.size.x) / 2.f, mm2px(128.5f - 8.f - 9.f + 2.f));
        addChild(logo);
    }
}

void QModWidget::appendContextMenu(Menu* menu) {
    QModModule* qm = dynamic_cast<QModModule*>(module);
    if (!qm) return;

    menu->addChild(new MenuSeparator);
    auto modeLabel = [](int m) -> std::string {
        switch (m) {
            case QModModule::MODE_RAND_TRIG: return "Random triggers";
            case QModModule::MODE_TRIG_SH:   return "Triggered S+H";
            case QModModule::MODE_SMOOTH:    return "Smooth random";
            case QModModule::MODE_SH:        return "Sample & hold";
            case QModModule::MODE_LFO:       return "LFO";
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
            addMode(QModModule::MODE_RAND_TRIG);
            addMode(QModModule::MODE_TRIG_SH);
            addMode(QModModule::MODE_SMOOTH);
            addMode(QModModule::MODE_SH);
            addMode(QModModule::MODE_LFO);
        };
    };
    menu->addChild(createSubmenuItem("Left column mode",  modeLabel(qm->modeL), modePicker(0)));
    menu->addChild(createSubmenuItem("Right column mode", modeLabel(qm->modeR), modePicker(1)));
    menu->addChild(createMenuItem("Set both columns to left's mode", "", [=]() {
        qm->setColumnMode(1, qm->modeL);
    }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createSubmenuItem("LFO waveshape", [=]() {
        switch (qm->lfoShape) {
            case QModModule::LFO_SINE:     return "Sine";
            case QModModule::LFO_TRIANGLE: return "Triangle";
            case QModModule::LFO_SQUARE:   return "Square";
            case QModModule::LFO_SAW:      return "Saw";
        }
        return "";
    }(), [=](ui::Menu* sub) {
        auto addShape = [&](const char* label, int s) {
            sub->addChild(createCheckMenuItem(label, "",
                [=]() { return qm->lfoShape == s; },
                [=]() { qm->lfoShape = s; }));
        };
        addShape("Sine",     QModModule::LFO_SINE);
        addShape("Triangle", QModModule::LFO_TRIANGLE);
        addShape("Square",   QModModule::LFO_SQUARE);
        addShape("Saw",      QModModule::LFO_SAW);
    }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createBoolPtrMenuItem("Stagger rates (fast → slow)", "", &qm->rateSpread));

    menu->addChild(createMenuLabel("Global rate"));
    menu->addChild(new MenuSlider(new FloatQuantity(
        &qm->globalRate, "Rate", " Hz", 0.01f, 10.f, 4.5f, 3)));

    menu->addChild(createMenuLabel("Smoothness (slew / shape)"));
    menu->addChild(new MenuSlider(new FloatQuantity(
        &qm->smoothness, "Smoothness", "", 0.f, 1.f, 0.4f, 2)));

    menu->addChild(createMenuLabel("Stagger spread (slow-end multiplier)"));
    menu->addChild(new MenuSlider(new FloatQuantity(
        &qm->spreadRatio, "Spread", "×", 0.005f, 1.f, 0.1f, 3)));

    auto rangeChecked = [=](int r) {
        int first = qm->range[0];
        for (int i = 1; i < QModModule::NUM_SLOTS; i++)
            if (qm->range[i] != first) return false;
        return first == r;
    };
    auto setAllRange = [=](int r) {
        for (int i = 0; i < QModModule::NUM_SLOTS; i++) qm->range[i] = r;
    };
    auto rangeLabelFor = [=]() -> std::string {
        int first = qm->range[0];
        for (int i = 1; i < QModModule::NUM_SLOTS; i++)
            if (qm->range[i] != first) return "Mixed";
        switch (first) {
            case QModModule::RANGE_UNI_10: return "0..10 V";
            case QModModule::RANGE_UNI_5:  return "0..5 V";
            case QModModule::RANGE_UNI_1:  return "0..1 V";
            case QModModule::RANGE_BI_10:  return "-10..10 V";
            case QModModule::RANGE_BI_5:   return "-5..5 V";
            case QModModule::RANGE_BI_1:   return "-1..1 V";
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
            addRange("0..10 V", QModModule::RANGE_UNI_10);
            addRange("0..5 V",  QModModule::RANGE_UNI_5);
            addRange("0..1 V",  QModModule::RANGE_UNI_1);
            rs->addChild(new MenuSeparator);
            rs->addChild(createMenuLabel("Bipolar"));
            addRange("-10..10 V", QModModule::RANGE_BI_10);
            addRange("-5..5 V",   QModModule::RANGE_BI_5);
            addRange("-1..1 V",   QModModule::RANGE_BI_1);
        }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createBoolPtrMenuItem("Join array with neighbouring LC Q modules", "", &qm->inArray));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Copy settings", "", [=]() {
        if (g_qmodClipboard) json_decref(g_qmodClipboard);
        g_qmodClipboard = qm->dataToJson();
    }));
    menu->addChild(createMenuItem("Paste settings", "",
        [=]() {
            if (!g_qmodClipboard) return;
            json_t* before = qm->dataToJson();
            qm->dataFromJson(g_qmodClipboard);
            pushQModJsonAction(qm, before, "paste qmod settings");
        },
        /*disabled*/ g_qmodClipboard == nullptr));

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

// Slug "qmodplus" — this is the user-facing "qmod+" module (4 HP, no row knobs).
// The C++ class names stay QModModule / QModWidget for historical reasons;
// the slug is what users see and what patches reference.
Model* modelQMod = createModel<QModModule, QModWidget>("qmodplus");
