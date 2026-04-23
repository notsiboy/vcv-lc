#include "QMap.hpp"
#include "Theme.hpp"

#include <componentlibrary.hpp>

// ─── Module ─────────────────────────────────────────────────────────────────

QMapModule::QMapModule() {
    config(0, NUM_INPUTS, 0, 0);
    for (int i = 0; i < NUM_SLOTS; i++) {
        bipolar[i] = false;
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
    for (int i = 0; i < NUM_SLOTS; i++) {
        engine::ParamHandle& h = paramHandles[i];
        if (!h.module) continue;
        if (!inputs[AUX_INPUT + i].isConnected()) continue;

        engine::ParamQuantity* pq = h.module->getParamQuantity(h.paramId);
        if (!pq) continue;

        float v = inputs[AUX_INPUT + i].getVoltage();
        float t = bipolar[i]
            ? math::clamp((v + 5.f) / 10.f, 0.f, 1.f)
            : math::clamp(v / 10.f, 0.f, 1.f);
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
        json_array_append_new(slotsJ, s);
    }
    json_object_set_new(root, "slots", slotsJ);
    return root;
}

void QMapModule::dataFromJson(json_t* root) {
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

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        drawRoundButton(args.vg, box.size.x, box.size.y, dark);
        if (armed()) {
            drawCenterDot(args.vg, box.size.x, box.size.y, AMBER, true, dark, 0.55f);
        } else if (bound()) {
            drawCenterDot(args.vg, box.size.x, box.size.y, AMBER, true, dark, 0.30f);
        } else {
            drawCenterDot(args.vg, box.size.x, box.size.y, AMBER, false, dark, 0.30f);
        }
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && armed()) {
            drawCenterDotGlow(args.vg, box.size.x, box.size.y, AMBER, amberPulse(), 0.55f);
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
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && qm) {
            qm->advanceArm();
            e.consume(this);
        }
    }
};

// Aux input port with a per-slot unipolar/bipolar right-click menu.
struct QMapInputPort : ThemedPJ301MPort {
    QMapModule* qm = nullptr;
    int slot = -1;

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
        lab->box.pos = Vec(0, mm2px(9.f));
        addChild(lab);
    }
    {
        MasterArmButton* b = new MasterArmButton;
        b->qm = module;
        b->box.size = mm2px(Vec(5.f, 5.f));
        b->box.pos = Vec((box.size.x - b->box.size.x) / 2.f, mm2px(14.f));
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

        SlotArmButton* btn = new SlotArmButton;
        btn->qm = module;
        btn->slot = i;
        btn->box.size = mm2px(Vec(btnSize, btnSize));
        btn->box.pos = Vec(mm2px(cx) - btn->box.size.x / 2.f,
                           mm2px(jackY - btnAboveJack) - btn->box.size.y / 2.f);
        addChild(btn);

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
        logo->box.pos  = Vec((box.size.x - logo->box.size.x) / 2.f, mm2px(128.5f - 8.f - 9.f));
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
    APP->scene->rack->setTouchedParam(NULL);
    APP->engine->updateParamHandle(&qm->paramHandles[slot], pw->module->id, pw->paramId, true);

    if (qm->sequentialArm) {
        qm->advanceArm();
    } else {
        qm->armedSlot = -1;
    }
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
        for (int i = 0; i < QMapModule::NUM_SLOTS; i++) qm->clearSlot(i);
        qm->armedSlot = -1;
        qm->sequentialArm = false;
    }));

    menu->addChild(new MenuSeparator);
    lc::appendThemeMenu(menu);
}

Model* modelQMap = createModel<QMapModule, QMapWidget>("qmap");
