#include "QMap.hpp"
#include "QMod.hpp"
#include "QModPlus.hpp"
#include "QArray.hpp"
#include "Theme.hpp"
#include "LcPorts.hpp"

#include <componentlibrary.hpp>
#include <history.hpp>
#include <ui/Slider.hpp>

// Module-scope clipboard for qmap. Raw JSON captured by dataToJson(); paste
// replays via dataFromJson(). Kept here (not in a singleton) because VCV
// plugins share a global but this pattern is well-scoped for a single click
// copy→paste between qmaps.
static json_t* g_qmapClipboard = nullptr;

// Coarse-grained undo: snapshot the full module data JSON before a change,
// snapshot again after, and restore either side by re-running dataFromJson.
// Heavyweight but simple and correct — works for any state the module
// already knows how to (de)serialise, including ParamHandle bindings.
namespace {
struct QMapJsonAction : history::ModuleAction {
    json_t* oldJ = nullptr;
    json_t* newJ = nullptr;
    ~QMapJsonAction() {
        if (oldJ) json_decref(oldJ);
        if (newJ) json_decref(newJ);
    }
    void apply(json_t* j) {
        if (!j) return;
        rack::engine::Module* m = APP->engine->getModule(moduleId);
        QMapModule* qm = dynamic_cast<QMapModule*>(m);
        if (qm) qm->dataFromJson(j);
    }
    void undo() override { apply(oldJ); }
    void redo() override { apply(newJ); }
};
}

static void pushQMapJsonAction(QMapModule* qm, json_t* before, const char* name) {
    if (!qm || !before) { if (before) json_decref(before); return; }
    QMapJsonAction* a = new QMapJsonAction;
    a->moduleId = qm->id;
    a->name = name ? name : "qmap change";
    a->oldJ = before;
    a->newJ = qm->dataToJson();
    APP->history->push(a);
}

// ─── Module ─────────────────────────────────────────────────────────────────

QMapModule::QMapModule() {
    config(0, NUM_INPUTS, 0, 0);
    for (int i = 0; i < NUM_SLOTS; i++) {
        bipolar[i] = false;
        attenuator[i] = 1.f;
        offset[i] = 0.f;
        paramHandles[i].color = nvgRGB(255, 170, 40); // amber highlight on bound params
        paramHandles[i].text = string::f("qmap aux %d", i + 1);
        configInput(AUX_INPUT + i, string::f("Aux %d", i + 1));
        // Register handles up front. Doing this in onAdd would deadlock:
        // Engine holds its exclusive lock across addModule, and
        // addParamHandle tries to re-acquire it.
        APP->engine->addParamHandle(&paramHandles[i]);
    }
}

QMapModule::~QMapModule() {
    for (int i = 0; i < NUM_SLOTS; i++)
        APP->engine->removeParamHandle(&paramHandles[i]);
}

void QMapModule::process(const ProcessArgs& args) {
    // Auto-assign feed: per-slot pairing by global array index. Each qmap
    // slot at global index (qmapBase + i) maps to the qmod / qmod+ whose
    // slot range covers that index. A real cable on an aux input always
    // wins over the array-feed.
    //
    // Example — [qmap1][qmap2][qmod]: qmap1 base = 0 (covers globals 0..13),
    // qmap2 base = 14 (covers 14..27), qmod base = 0 (covers 0..13). So
    // qmap1 slot 3 (global 3) pairs with qmod slot 3, while qmap2 slot 3
    // (global 17) has no matching mod source and stays silent.
    auto arr = lc::walkArray(this);
    int qmapBase = lc::arraySlotBase(this);
    struct Source { rack::engine::Module* m; int localSlot; };
    Source sources[NUM_SLOTS];
    for (int i = 0; i < NUM_SLOTS; i++) sources[i] = { nullptr, 0 };
    for (int i = 0; i < NUM_SLOTS; i++) {
        int globalIdx = qmapBase + i;
        for (auto* m : arr) {
            if (!m || m == this) continue;
            if (m->model != modelQMod && m->model != modelQModPlus) continue;
            int modBase = lc::arraySlotBase(m);
            if (globalIdx >= modBase && globalIdx < modBase + NUM_SLOTS) {
                sources[i] = { m, globalIdx - modBase };
                break;
            }
        }
    }

    for (int i = 0; i < NUM_SLOTS; i++) {
        float v;
        bool haveSignal = false;
        if (inputs[AUX_INPUT + i].isConnected()) {
            v = inputs[AUX_INPUT + i].getVoltage();
            haveSignal = true;
        } else if (sources[i].m) {
            // Both qmod and qmod+ place MOD_OUTPUT at index 0 with
            // NUM_SLOTS = 14, so a direct output[localSlot] read works.
            v = sources[i].m->outputs[QModModule::MOD_OUTPUT + sources[i].localSlot].getVoltage();
            haveSignal = true;
        } else {
            v = 0.f;
        }

        // Attenuate & bias the raw CV before the polarity-aware normalise.
        if (haveSignal) v = v * attenuator[i] + offset[i];

        // Always refresh the UI-facing level so the arm LEDs track the feed
        // whether or not a param is currently bound to this slot.
        float t = bipolar[i]
            ? math::clamp((v + 5.f) / 10.f, 0.f, 1.f)
            : math::clamp(v / 10.f, 0.f, 1.f);
        modLevel[i] = haveSignal ? t : 0.f;
        hasSignal[i] = haveSignal;

        engine::ParamHandle& h = paramHandles[i];
        if (!h.module || !haveSignal) continue;

        engine::ParamQuantity* pq = h.module->getParamQuantity(h.paramId);
        if (!pq) continue;

        float lo = pq->getMinValue();
        float hi = pq->getMaxValue();
        pq->setValue(lo + t * (hi - lo));
    }
}

void QMapModule::advanceArm() {
    if (armedSlot < 0) {
        armedSlot = 0;
        sequentialArm = true;
    } else {
        armedSlot += 1;
        if (armedSlot >= NUM_SLOTS) {
            armedSlot = -1;
            sequentialArm = false;
        }
    }
}

void QMapModule::clearSlot(int slot) {
    if (slot < 0 || slot >= NUM_SLOTS) return;
    APP->engine->updateParamHandle(&paramHandles[slot], -1, 0, true);
}

json_t* QMapModule::dataToJson() {
    json_t* root = json_object();
    json_t* slotsJ = json_array();
    for (int i = 0; i < NUM_SLOTS; i++) {
        json_t* s = json_object();
        json_object_set_new(s, "moduleId", json_integer(paramHandles[i].moduleId));
        json_object_set_new(s, "paramId", json_integer(paramHandles[i].paramId));
        json_object_set_new(s, "bipolar", json_boolean(bipolar[i]));
        json_object_set_new(s, "atten", json_real(attenuator[i]));
        json_object_set_new(s, "offset", json_real(offset[i]));
        json_array_append_new(slotsJ, s);
    }
    json_object_set_new(root, "slots", slotsJ);
    json_object_set_new(root, "qmodFavour", json_integer(qmodFavour));
    json_object_set_new(root, "inArray", json_boolean(inArray));
    return root;
}

void QMapModule::dataFromJson(json_t* root) {
    if (json_t* j = json_object_get(root, "qmodFavour"))
        qmodFavour = (int)json_integer_value(j);
    if (json_t* j = json_object_get(root, "inArray"))
        inArray = json_boolean_value(j);
    json_t* slotsJ = json_object_get(root, "slots");
    if (!slotsJ) return;
    size_t n = std::min((size_t)NUM_SLOTS, json_array_size(slotsJ));
    for (size_t i = 0; i < n; i++) {
        json_t* s = json_array_get(slotsJ, i);
        if (!s) continue;
        int64_t mid = -1;
        int pid = 0;
        if (json_t* j = json_object_get(s, "moduleId")) mid = json_integer_value(j);
        if (json_t* j = json_object_get(s, "paramId"))  pid = json_integer_value(j);
        if (json_t* j = json_object_get(s, "bipolar"))  bipolar[i] = json_boolean_value(j);
        attenuator[i] = 1.f;
        offset[i]     = 0.f;
        if (json_t* j = json_object_get(s, "atten"))    attenuator[i] = json_real_value(j);
        if (json_t* j = json_object_get(s, "offset"))   offset[i]     = json_real_value(j);
        APP->engine->updateParamHandle(&paramHandles[i], mid, pid, true);
    }
}

// ─── Widget helpers ─────────────────────────────────────────────────────────

namespace {

struct QMapBackground : widget::Widget {
    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, lc::panelBg());
        nvgFill(args.vg);
    }
};

struct QMapLabel : widget::Widget {
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

// Tiny dynamic label that shows this slot's global number inside the
// current Q-array. Reads lc::arraySlotBase every frame so the number
// updates live as modules are dragged into or out of the array.
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

struct PngLogo : widget::Widget {
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

static const NVGcolor AMBER = nvgRGB(255, 170, 40);

static void drawRoundButton(NVGcontext* vg, float w, float h, bool dark) {
    float cx = w / 2.f;
    float cy = h / 2.f;
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
                          float scale = 0.50f) {
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
                              float scale = 0.50f) {
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

// Flashing pulse (0..1) driven by the rack's frame clock.
static float amberPulse() {
    double t = APP->window->getFrameTime();
    float s = (float)(0.5 + 0.5 * std::sin(t * 2.0 * M_PI * 1.8));
    return 0.35f + 0.65f * s; // keep it visible even at the trough
}

// Small per-slot arm button. Arms that slot exclusively when clicked.
struct SlotArmButton : widget::OpaqueWidget {
    QMapModule* qm = nullptr;
    int slot = -1;

    bool armed() const { return qm && qm->armedSlot == slot; }
    bool bound() const { return qm && qm->paramHandles[slot].module != nullptr; }
    bool hasSignal() const { return qm && qm->hasSignal[slot]; }
    float level() const {
        return qm ? math::clamp(qm->modLevel[slot], 0.f, 1.f) : 0.f;
    }

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        drawRoundButton(args.vg, box.size.x, box.size.y, dark);
        const NVGcolor WHITE = nvgRGB(230, 230, 230);
        if (armed()) {
            drawCenterDot(args.vg, box.size.x, box.size.y, AMBER, true, dark, 0.55f);
        } else if (bound()) {
            // Bound slot: fade the amber dot with the incoming CV.
            float b = 0.2f + 0.8f * level();
            NVGcolor tinted = nvgRGBf(AMBER.r * b, AMBER.g * b, AMBER.b * b);
            drawCenterDot(args.vg, box.size.x, box.size.y, tinted, true, dark, 0.30f);
        } else if (hasSignal()) {
            // Unmapped but receiving signal (usually via the array auto-feed).
            // White tinting so it's visually distinct from amber "mapped".
            float b = 0.2f + 0.8f * level();
            NVGcolor tinted = nvgRGBf(WHITE.r * b, WHITE.g * b, WHITE.b * b);
            drawCenterDot(args.vg, box.size.x, box.size.y, tinted, true, dark, 0.30f);
        } else {
            drawCenterDot(args.vg, box.size.x, box.size.y, AMBER, false, dark, 0.30f);
        }
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1 || !qm) { Widget::drawLayer(args, layer); return; }
        const NVGcolor WHITE = nvgRGB(230, 230, 230);
        if (armed()) {
            drawCenterDotGlow(args.vg, box.size.x, box.size.y, AMBER, amberPulse(), 0.55f);
        } else if (bound()) {
            float a = level();
            if (a > 0.f)
                drawCenterDotGlow(args.vg, box.size.x, box.size.y, AMBER, a, 0.30f);
        } else if (hasSignal()) {
            float a = level();
            if (a > 0.f)
                drawCenterDotGlow(args.vg, box.size.x, box.size.y, WHITE, a, 0.30f);
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && qm) {
            if (qm->armedSlot == slot) {
                qm->armedSlot = -1;
                qm->sequentialArm = false;
            } else {
                qm->armedSlot = slot;
                qm->sequentialArm = false;
            }
            e.consume(this);
        } else if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT && qm) {
            // Right-click a slot button to clear its binding.
            qm->clearSlot(slot);
            e.consume(this);
        }
    }
};

// Top master button — bigger. Cycles through arming slots 1..8 sequentially.
struct MasterArmButton : widget::OpaqueWidget {
    QMapModule* qm = nullptr;

    bool anyArmed() const { return qm && qm->armedSlot >= 0 && qm->sequentialArm; }

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        drawRoundButton(args.vg, box.size.x, box.size.y, dark);
        drawCenterDot(args.vg, box.size.x, box.size.y, AMBER, anyArmed(), dark, 0.50f);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && anyArmed()) {
            drawCenterDotGlow(args.vg, box.size.x, box.size.y, AMBER, amberPulse(), 0.50f);
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        OpaqueWidget::onButton(e);
        if (e.action != GLFW_PRESS || !qm) return;
        if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
            // If the button is already in its flashing-amber sequential-arm
            // state, clicking again cancels the whole mapping pass rather
            // than advancing to the next slot.
            if (qm->sequentialArm && qm->armedSlot >= 0) {
                qm->armedSlot = -1;
                qm->sequentialArm = false;
            } else {
                qm->advanceArm();
            }
            e.consume(this);
        } else if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
            // Quick-access popover: the four mapping-bank actions from the
            // module menu, reachable without opening the full context menu.
            ui::Menu* menu = createMenu();
            menu->addChild(createMenuItem("Arm all (sequential)", "", [=]() {
                qm->armedSlot = 0;
                qm->sequentialArm = true;
            }));
            menu->addChild(createMenuItem("Clear all mappings", "", [=]() {
                json_t* before = qm->dataToJson();
                for (int i = 0; i < QMapModule::NUM_SLOTS; i++) qm->clearSlot(i);
                qm->armedSlot = -1;
                qm->sequentialArm = false;
                pushQMapJsonAction(qm, before, "clear qmap mappings");
            }));
            menu->addChild(new MenuSeparator);
            menu->addChild(createMenuItem("Copy mappings", "", [=]() {
                if (g_qmapClipboard) json_decref(g_qmapClipboard);
                g_qmapClipboard = qm->dataToJson();
            }));
            menu->addChild(createMenuItem("Paste mappings", "",
                [=]() {
                    if (!g_qmapClipboard) return;
                    json_t* before = qm->dataToJson();
                    qm->dataFromJson(g_qmapClipboard);
                    pushQMapJsonAction(qm, before, "paste qmap mappings");
                },
                /*disabled*/ g_qmapClipboard == nullptr));
            e.consume(this);
        }
    }
};

// Shared Quantity + Slider helpers so jack menus can expose float params as
// draggable menu sliders (like VCV's native knob menus).
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
    MenuSlider(Quantity* q) { quantity = q; box.size.x = 220.f; }
    ~MenuSlider() { delete quantity; }
};

// Aux input port with a per-slot unipolar/bipolar right-click menu. Inherits
// the shared Lux Cache white-ring visual so qmap's inputs match every other
// LC input jack. When a qmod sits adjacent (either side), we paint an extra
// white centre dot so both modules visibly flag the auto-link.
struct QMapInputPort : lc::WhiteRingPJ301MPort {
    QMapModule* qm = nullptr;
    int slot = -1;

    void draw(const DrawArgs& args) override {
        lc::WhiteRingPJ301MPort::draw(args);
        if (!module || slot < 0) return;
        // Dot shows only when this specific slot is paired with a qmod
        // slot at the same global array index. Empty array cells (e.g. the
        // 2nd qmap's slots when there's only one qmod in the array) stay
        // un-dotted to reflect that they cannot be fed.
        int qmapBase = lc::arraySlotBase(module);
        int globalIdx = qmapBase + slot;
        bool paired = false;
        auto arr = lc::walkArray(module);
        for (auto* m : arr) {
            if (!m || m == module) continue;
            if (m->model != modelQMod && m->model != modelQModPlus) continue;
            int modBase = lc::arraySlotBase(m);
            if (globalIdx >= modBase && globalIdx < modBase + QMapModule::NUM_SLOTS) {
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
        menu->addChild(createMenuLabel("Input range"));
        menu->addChild(createCheckMenuItem("Unipolar (0..10V)", "",
            [=]() { return qm && !qm->bipolar[slot]; },
            [=]() { if (qm) qm->bipolar[slot] = false; }));
        menu->addChild(createCheckMenuItem("Bipolar (-5..5V)", "",
            [=]() { return qm && qm->bipolar[slot]; },
            [=]() { if (qm) qm->bipolar[slot] = true; }));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Attenuator"));
        menu->addChild(new MenuSlider(new FloatQuantity(
            &qm->attenuator[slot], "Attenuator", "×", -2.f, 2.f, 1.f, 2)));
        menu->addChild(createMenuLabel("Offset"));
        menu->addChild(new MenuSlider(new FloatQuantity(
            &qm->offset[slot], "Offset", " V", -10.f, 10.f, 0.f, 2)));

        if (qm->paramHandles[slot].module) {
            menu->addChild(new MenuSeparator);
            menu->addChild(createMenuItem("Clear mapping", "",
                [=]() { if (qm) qm->clearSlot(slot); }));
        }
    }
};

} // namespace

// ─── Widget ─────────────────────────────────────────────────────────────────

QMapWidget::QMapWidget(QMapModule* module) {
    setModule(module);
    box.size = Vec(RACK_GRID_WIDTH * 4, RACK_GRID_HEIGHT);

    QMapBackground* bg = new QMapBackground;
    bg->box.size = box.size;
    addChild(bg);

    float screwX = (box.size.x - RACK_GRID_WIDTH) / 2.f;
    addChild(createWidget<ScrewBlack>(Vec(screwX, 0)));
    addChild(createWidget<ScrewBlack>(Vec(screwX, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    app::PanelBorder* border = new app::PanelBorder;
    border->box.size = box.size;
    addChild(border);

    // q-map label + master button (y's match take's "take" label + save button).
    {
        QMapLabel* lab = new QMapLabel;
        lab->text = "qmap";
        lab->fontSize = 7.f;
        lab->box.size = Vec(box.size.x, mm2px(3.f));
        lab->box.pos = Vec(0, mm2px(7.f));
        addChild(lab);
    }
    {
        MasterArmButton* b = new MasterArmButton;
        b->qm = module;
        b->box.size = mm2px(Vec(5.f, 5.f));
        // Centred at y = 14.5 mm to line up with qmod's header buttons.
        b->box.pos = Vec((box.size.x - b->box.size.x) / 2.f,
                         mm2px(14.5f) - b->box.size.y / 2.f);
        addChild(b);
    }

    // 7 rows × 2 columns. Bottom row anchors to grab2's L/R jack positions
    // (jack centre y = 106mm, columns at 25% / 75% of panel width); six rows
    // above follow at a tight 13mm step to fit all 14 slots between the
    // master button and the logo. Arm buttons sit 6.5mm above each jack —
    // tight but leaves a safety gap vs. the row above.
    const float panelW_mm = box.size.x / mm2px(1.f);
    const float colL_mm = panelW_mm * 0.25f;
    const float colR_mm = panelW_mm * 0.75f;
    const float bottomJackY = 106.f;
    const int numRows = QMapModule::NUM_SLOTS / 2;
    const float rowStep = 13.f;
    const float btnAboveJack = 6.5f;
    const float btnSize = 2.6f;
    for (int i = 0; i < QMapModule::NUM_SLOTS; i++) {
        int row = i / 2;
        int col = i % 2;
        float jackY = bottomJackY - (numRows - 1 - row) * rowStep;
        float cx = (col == 0) ? colL_mm : colR_mm;

        // Arm button is shifted right of the column centre so the dynamic
        // slot-number label can sit in the freed space on its left.
        const float btnShift = 1.5f;
        SlotArmButton* btn = new SlotArmButton;
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

        QMapInputPort* port = createInputCentered<QMapInputPort>(
            mm2px(Vec(cx, jackY)), module, QMapModule::AUX_INPUT + i);
        port->qm = module;
        port->slot = i;
        addInput(port);
    }

    // Logo.
    {
        PngLogo* logo = new PngLogo;
        logo->path     = asset::plugin(pluginInstance, "res/lc-icon-new.png");
        logo->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");
        logo->greyPath = asset::plugin(pluginInstance, "res/lc-icon-grey.png");
        logo->box.size = mm2px(Vec(9.f, 9.f));
        logo->box.pos  = Vec((box.size.x - logo->box.size.x) / 2.f, mm2px(128.5f - 8.f - 9.f + 2.f));
        addChild(logo);
    }
}

void QMapWidget::step() {
    ModuleWidget::step();
    QMapModule* qm = dynamic_cast<QMapModule*>(module);
    if (!qm) return;
    if (qm->armedSlot < 0) return;
    if (!APP || !APP->scene || !APP->scene->rack) return;

    app::ParamWidget* pw = APP->scene->rack->getTouchedParam();
    if (!pw || !pw->module) return;
    // Ignore our own params (we have none, but belt-and-braces).
    if (pw->module->id == qm->id) return;

    int slot = qm->armedSlot;
    json_t* before = qm->dataToJson();
    APP->scene->rack->setTouchedParam(NULL);
    APP->engine->updateParamHandle(&qm->paramHandles[slot], pw->module->id, pw->paramId, true);

    if (qm->sequentialArm) {
        qm->advanceArm();
    } else {
        qm->armedSlot = -1;
    }

    pushQMapJsonAction(qm, before, "qmap touch-assign");
}

void QMapWidget::appendContextMenu(Menu* menu) {
    QMapModule* qm = dynamic_cast<QMapModule*>(module);
    if (!qm) return;

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Mappings"));
    for (int i = 0; i < QMapModule::NUM_SLOTS; i++) {
        const engine::ParamHandle& h = qm->paramHandles[i];
        std::string name = string::f("Aux %d", i + 1);
        std::string right;
        if (h.module) {
            engine::ParamQuantity* pq = h.module->getParamQuantity(h.paramId);
            right = pq ? pq->getLabel() : string::f("param %d", h.paramId);
            if (h.module->model)
                right = h.module->model->name + " · " + right;
        } else {
            right = "—";
        }
        menu->addChild(createMenuItem(name, right, [=]() {
            if (qm->paramHandles[i].module) qm->clearSlot(i);
            else { qm->armedSlot = i; qm->sequentialArm = false; }
        }));
    }

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Arm all (sequential)", "",
        [=]() { qm->armedSlot = 0; qm->sequentialArm = true; }));
    menu->addChild(createMenuItem("Clear all mappings", "", [=]() {
        json_t* before = qm->dataToJson();
        for (int i = 0; i < QMapModule::NUM_SLOTS; i++) qm->clearSlot(i);
        qm->armedSlot = -1;
        qm->sequentialArm = false;
        pushQMapJsonAction(qm, before, "clear qmap mappings");
    }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createBoolPtrMenuItem("Join array with neighbouring LC Q modules", "", &qm->inArray));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Copy mappings", "", [=]() {
        if (g_qmapClipboard) json_decref(g_qmapClipboard);
        g_qmapClipboard = qm->dataToJson();
    }));
    menu->addChild(createMenuItem("Paste mappings", "",
        [=]() {
            if (!g_qmapClipboard) return;
            json_t* before = qm->dataToJson();
            qm->dataFromJson(g_qmapClipboard);
            pushQMapJsonAction(qm, before, "paste qmap mappings");
        },
        /*disabled*/ g_qmapClipboard == nullptr));

    // Array status — count the qmaps and mod sources participating in this
    // Q-array so the user can see at a glance whether slot pairings exist.
    menu->addChild(new MenuSeparator);
    auto arr = lc::walkArray(qm);
    int qmapCount = 0, modCount = 0;
    for (auto* m : arr) {
        if (!m) continue;
        if (m->model == modelQMap) qmapCount++;
        else if (m->model == modelQMod || m->model == modelQModPlus) modCount++;
    }
    if (modCount == 0) {
        menu->addChild(createMenuLabel(
            "No qmod in array — place a qmod / qmod+ next to any qmap here to auto-feed its inputs"));
    } else {
        int qmapBase = lc::arraySlotBase(qm);
        int paired = 0;
        for (int i = 0; i < QMapModule::NUM_SLOTS; i++) {
            int globalIdx = qmapBase + i;
            for (auto* m : arr) {
                if (!m || m == qm) continue;
                if (m->model != modelQMod && m->model != modelQModPlus) continue;
                int modBase = lc::arraySlotBase(m);
                if (globalIdx >= modBase && globalIdx < modBase + QMapModule::NUM_SLOTS) {
                    paired++;
                    break;
                }
            }
        }
        menu->addChild(createMenuLabel(
            string::f("Array: %d qmap%s + %d mod source%s — %d of this module's slots paired",
                      qmapCount, qmapCount == 1 ? "" : "s",
                      modCount,  modCount == 1 ? "" : "s",
                      paired)));
    }

    menu->addChild(new MenuSeparator);
    lc::appendThemeMenu(menu);
}

Model* modelQMap = createModel<QMapModule, QMapWidget>("qmap");
