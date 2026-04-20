#include "Tidy.hpp"
#include "Theme.hpp"

static json_t* serializePresetBody(const Preset& p);
static Preset deserializePresetBody(json_t* j);

TidyModule::TidyModule() {
    config(0, 0, 0, 0);
    colorOpacity.assign(settings::cableColors.size(), 1.f);
}

json_t* TidyModule::dataToJson() {
    json_t* root = json_object();
    json_object_set_new(root, "masterEnable", json_boolean(masterEnable));
    json_object_set_new(root, "hidePlugHeads", json_boolean(hidePlugHeads));
    json_object_set_new(root, "darkModuleStrength", json_real(darkModuleStrength));

    json_t* colorsJ = json_array();
    for (float v : colorOpacity)
        json_array_append_new(colorsJ, json_real(v));
    json_object_set_new(root, "colorOpacity", colorsJ);

    json_t* modulesJ = json_array();
    for (auto& kv : moduleRules) {
        const ModuleRule& r = kv.second;
        if (!r.moduleHidden && !r.moduleDark && r.cableOpacity == 1.f && !r.hideConnectedCables
            && r.brightness == 1.f)
            continue;
        json_t* mJ = json_object();
        json_object_set_new(mJ, "id", json_integer(kv.first));
        json_object_set_new(mJ, "moduleHidden", json_boolean(r.moduleHidden));
        json_object_set_new(mJ, "moduleDark", json_boolean(r.moduleDark));
        json_object_set_new(mJ, "brightness", json_real(r.brightness));
        json_object_set_new(mJ, "cableOpacity", json_real(r.cableOpacity));
        json_object_set_new(mJ, "hideConnectedCables", json_boolean(r.hideConnectedCables));
        json_array_append_new(modulesJ, mJ);
    }
    json_object_set_new(root, "modules", modulesJ);

    json_t* presetsJ = json_array();
    for (const Preset& p : presets)
        json_array_append_new(presetsJ, serializePresetBody(p));
    json_object_set_new(root, "presets", presetsJ);

    return root;
}

static json_t* serializePresetBody(const Preset& p) {
    json_t* j = json_object();
    json_object_set_new(j, "name", json_string(p.name.c_str()));
    json_object_set_new(j, "hidePlugHeads", json_boolean(p.hidePlugHeads));
    json_t* colorsJ = json_array();
    for (float v : p.colorOpacity) json_array_append_new(colorsJ, json_real(v));
    json_object_set_new(j, "colorOpacity", colorsJ);
    json_t* modulesJ = json_array();
    for (auto& kv : p.moduleRules) {
        const ModuleRule& r = kv.second;
        if (!r.moduleHidden && !r.moduleDark && r.cableOpacity == 1.f && !r.hideConnectedCables
            && r.brightness == 1.f) continue;
        json_t* mJ = json_object();
        json_object_set_new(mJ, "id", json_integer(kv.first));
        json_object_set_new(mJ, "moduleHidden", json_boolean(r.moduleHidden));
        json_object_set_new(mJ, "moduleDark", json_boolean(r.moduleDark));
        json_object_set_new(mJ, "brightness", json_real(r.brightness));
        json_object_set_new(mJ, "cableOpacity", json_real(r.cableOpacity));
        json_object_set_new(mJ, "hideConnectedCables", json_boolean(r.hideConnectedCables));
        json_array_append_new(modulesJ, mJ);
    }
    json_object_set_new(j, "modules", modulesJ);
    return j;
}

static Preset deserializePresetBody(json_t* j) {
    Preset p;
    if (json_t* n = json_object_get(j, "name")) p.name = json_string_value(n);
    if (json_t* b = json_object_get(j, "hidePlugHeads")) p.hidePlugHeads = json_boolean_value(b);
    if (json_t* colorsJ = json_object_get(j, "colorOpacity")) {
        size_t n = json_array_size(colorsJ);
        p.colorOpacity.resize(n);
        for (size_t i = 0; i < n; i++) p.colorOpacity[i] = json_real_value(json_array_get(colorsJ, i));
    }
    if (json_t* modulesJ = json_object_get(j, "modules")) {
        size_t n = json_array_size(modulesJ);
        for (size_t i = 0; i < n; i++) {
            json_t* mJ = json_array_get(modulesJ, i);
            int64_t id = json_integer_value(json_object_get(mJ, "id"));
            ModuleRule r;
            if (json_t* x = json_object_get(mJ, "moduleHidden")) r.moduleHidden = json_boolean_value(x);
            if (json_t* x = json_object_get(mJ, "moduleDark")) r.moduleDark = json_boolean_value(x);
            if (json_t* x = json_object_get(mJ, "brightness")) r.brightness = json_real_value(x);
            if (json_t* x = json_object_get(mJ, "cableOpacity")) r.cableOpacity = json_real_value(x);
            if (json_t* x = json_object_get(mJ, "hideConnectedCables")) r.hideConnectedCables = json_boolean_value(x);
            p.moduleRules[id] = r;
        }
    }
    return p;
}

void TidyModule::savePreset(const std::string& name) {
    Preset p;
    p.name = name;
    p.hidePlugHeads = hidePlugHeads;
    p.colorOpacity = colorOpacity;
    p.moduleRules = moduleRules;
    presets.push_back(p);
}

void TidyModule::loadPreset(size_t index) {
    if (index >= presets.size()) return;
    const Preset& p = presets[index];
    hidePlugHeads = p.hidePlugHeads;
    colorOpacity = p.colorOpacity;
    colorOpacity.resize(settings::cableColors.size(), 1.f);
    moduleRules = p.moduleRules;
}

void TidyModule::overwritePreset(size_t index) {
    if (index >= presets.size()) return;
    std::string name = presets[index].name;
    presets[index] = Preset();
    presets[index].name = name;
    presets[index].hidePlugHeads = hidePlugHeads;
    presets[index].colorOpacity = colorOpacity;
    presets[index].moduleRules = moduleRules;
}

void TidyModule::deletePreset(size_t index) {
    if (index >= presets.size()) return;
    presets.erase(presets.begin() + index);
}

void TidyModule::dataFromJson(json_t* root) {
    if (json_t* j = json_object_get(root, "masterEnable")) masterEnable = json_boolean_value(j);
    if (json_t* j = json_object_get(root, "hidePlugHeads")) hidePlugHeads = json_boolean_value(j);
    if (json_t* j = json_object_get(root, "darkModuleStrength")) darkModuleStrength = json_real_value(j);
    // Back-compat: migrate old per-patch darkMode into the shared setting.
    if (json_t* j = json_object_get(root, "darkMode")) lc::theme.dark = json_boolean_value(j);

    colorOpacity.assign(settings::cableColors.size(), 1.f);
    if (json_t* colorsJ = json_object_get(root, "colorOpacity")) {
        size_t n = std::min((size_t)json_array_size(colorsJ), colorOpacity.size());
        for (size_t i = 0; i < n; i++)
            colorOpacity[i] = json_real_value(json_array_get(colorsJ, i));
    }

    moduleRules.clear();
    if (json_t* modulesJ = json_object_get(root, "modules")) {
        size_t n = json_array_size(modulesJ);
        for (size_t i = 0; i < n; i++) {
            json_t* mJ = json_array_get(modulesJ, i);
            int64_t id = json_integer_value(json_object_get(mJ, "id"));
            ModuleRule r;
            if (json_t* j = json_object_get(mJ, "moduleHidden")) r.moduleHidden = json_boolean_value(j);
            if (json_t* j = json_object_get(mJ, "moduleDark")) r.moduleDark = json_boolean_value(j);
            if (json_t* j = json_object_get(mJ, "brightness")) r.brightness = json_real_value(j);
            if (json_t* j = json_object_get(mJ, "cableOpacity")) r.cableOpacity = json_real_value(j);
            if (json_t* j = json_object_get(mJ, "hideConnectedCables")) r.hideConnectedCables = json_boolean_value(j);
            moduleRules[id] = r;
        }
    }

    justLoaded = true;

    presets.clear();
    if (json_t* presetsJ = json_object_get(root, "presets")) {
        size_t n = json_array_size(presetsJ);
        for (size_t i = 0; i < n; i++)
            presets.push_back(deserializePresetBody(json_array_get(presetsJ, i)));
    }
}

struct PngImageWidget : widget::Widget {
    std::string path;
    std::string darkPath;
    TidyModule* tm = nullptr;
    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        std::string use = (dark && !darkPath.empty()) ? darkPath : path;
        std::shared_ptr<window::Image> img = APP->window->loadImage(use);
        if (!img || img->handle < 0) return;
        NVGpaint paint = nvgImagePattern(args.vg, 0, 0, box.size.x, box.size.y, 0, img->handle, 1.f);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillPaint(args.vg, paint);
        nvgFill(args.vg);
    }
};

struct BackgroundWidget : widget::Widget {
    TidyModule* tm = nullptr;
    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, dark ? nvgRGB(0, 0, 0) : nvgRGB(255, 255, 255));
        nvgFill(args.vg);
    }
};

static void drawToggleButton(NVGcontext* vg, float w, float h, bool dark) {
    float cx = w / 2.f;
    float cy = h / 2.f;
    float rOuter = std::min(cx, cy) - 0.5f;
    float rInner = rOuter * 0.78f;

    NVGcolor rim     = dark ? nvgRGB(40, 40, 40)   : nvgRGB(255, 255, 255);
    NVGcolor rimEdge = dark ? nvgRGB(90, 90, 90)   : nvgRGB(180, 180, 180);
    NVGcolor face    = dark ? nvgRGB(55, 55, 55)   : nvgRGB(240, 240, 240);
    NVGcolor faceEdge= dark ? nvgRGB(75, 75, 75)   : nvgRGB(210, 210, 210);

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

struct EnableToggleWidget : widget::OpaqueWidget {
    TidyModule* tm = nullptr;

    void draw(const DrawArgs& args) override {
        drawToggleButton(args.vg, box.size.x, box.size.y, lc::theme.dark);
    }

    void onButton(const event::Button& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && tm) {
            tm->masterEnable = !tm->masterEnable;
            e.consume(this);
        }
    }
};

struct StateLedWidget : widget::Widget {
    bool* state = nullptr;
    NVGcolor onColor = nvgRGB(60, 210, 90);

    bool isOn() { return state && *state; }

    void draw(const DrawArgs& args) override {
        float cx = box.size.x / 2.f;
        float cy = box.size.y / 2.f;
        float r = std::min(cx, cy) - 0.5f;

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, r);
        if (isOn()) nvgFillColor(args.vg, onColor);
        else nvgFillColor(args.vg, nvgRGB(200, 200, 200));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGB(150, 150, 150));
        nvgStrokeWidth(args.vg, 0.3f);
        nvgStroke(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && isOn()) {
            float cx = box.size.x / 2.f;
            float cy = box.size.y / 2.f;
            float r = std::min(cx, cy);
            NVGcolor c = onColor;
            NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, r * 0.8f, r * 2.5f,
                nvgRGBA(c.r * 255, c.g * 255, c.b * 255, 180),
                nvgRGBA(c.r * 255, c.g * 255, c.b * 255, 0));
            nvgBeginPath(args.vg);
            nvgRect(args.vg, -r * 2, -r * 2, box.size.x + r * 4, box.size.y + r * 4);
            nvgFillPaint(args.vg, glow);
            nvgFill(args.vg);
        }
        Widget::drawLayer(args, layer);
    }
};

struct PickerToggleWidget : widget::OpaqueWidget {
    TidyModule* tm = nullptr;

    void draw(const DrawArgs& args) override {
        drawToggleButton(args.vg, box.size.x, box.size.y, lc::theme.dark);
    }

    void onButton(const event::Button& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && tm) {
            tm->pickerMode = (tm->pickerMode + 1) % 3; // off → hide → dark → off
            e.consume(this);
        }
    }
};

struct PickerLedWidget : widget::Widget {
    TidyModule* tm = nullptr;

    void draw(const DrawArgs& args) override {
        float cx = box.size.x / 2.f;
        float cy = box.size.y / 2.f;
        float r = std::min(cx, cy) - 0.5f;

        NVGcolor fill;
        if (!tm || tm->pickerMode == 0) fill = nvgRGB(200, 200, 200);
        else if (tm->pickerMode == 1)   fill = nvgRGB(70, 140, 240);  // blue
        else                            fill = nvgRGB(180, 80, 220);  // purple

        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, r);
        nvgFillColor(args.vg, fill);
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGB(150, 150, 150));
        nvgStrokeWidth(args.vg, 0.3f);
        nvgStroke(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && tm && tm->pickerMode != 0) {
            float cx = box.size.x / 2.f;
            float cy = box.size.y / 2.f;
            float r = std::min(cx, cy);
            NVGcolor c = (tm->pickerMode == 1) ? nvgRGB(70, 140, 240) : nvgRGB(180, 80, 220);
            NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, r * 0.8f, r * 2.5f,
                nvgRGBA(c.r * 255, c.g * 255, c.b * 255, 180),
                nvgRGBA(c.r * 255, c.g * 255, c.b * 255, 0));
            nvgBeginPath(args.vg);
            nvgRect(args.vg, -r * 2, -r * 2, box.size.x + r * 4, box.size.y + r * 4);
            nvgFillPaint(args.vg, glow);
            nvgFill(args.vg);
        }
        Widget::drawLayer(args, layer);
    }
};

struct PickerOverlay : widget::Widget {
    TidyModule* tm = nullptr;
    int64_t hoveredModuleId = -1;

    ModuleWidget* findModuleAt(Vec rackPos) {
        if (!APP || !APP->scene || !APP->scene->rack) return nullptr;
        for (ModuleWidget* mw : APP->scene->rack->getModules()) {
            if (mw && mw->box.contains(rackPos)) return mw;
        }
        return nullptr;
    }

    void step() override {
        Widget::step();
        if (APP && APP->scene && APP->scene->rack)
            box.size = APP->scene->rack->box.size;
    }

    bool isOwnTidy(ModuleWidget* mw) {
        return mw && mw->module && tm && mw->module->id == tm->id;
    }

    void onHover(const event::Hover& e) override {
        if (!tm || tm->pickerMode == 0) {
            hoveredModuleId = -1;
            return;
        }
        ModuleWidget* mw = findModuleAt(e.pos);
        if (isOwnTidy(mw)) {
            hoveredModuleId = -1;
            return; // pass through so Tidy's panel widgets get hover
        }
        hoveredModuleId = (mw && mw->module) ? mw->module->id : -1;
        e.stopPropagating();
        if (!e.isConsumed()) e.consume(this);
    }

    void onButton(const event::Button& e) override {
        if (!tm || tm->pickerMode == 0) return;
        ModuleWidget* mw = findModuleAt(e.pos);
        if (isOwnTidy(mw)) return; // pass through to Tidy's own panel UI
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            if (mw && mw->module) {
                ModuleRule& r = tm->moduleRules[mw->module->id];
                if (tm->pickerMode == 1) r.moduleHidden = !r.moduleHidden;
                else if (tm->pickerMode == 2) r.moduleDark = !r.moduleDark;
            }
            e.consume(this);
        } else if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
            tm->pickerMode = 0;
            e.consume(this);
        }
        e.stopPropagating();
    }

    void onHoverKey(const event::HoverKey& e) override {
        if (tm && tm->pickerMode != 0 && e.action == GLFW_PRESS && e.key == GLFW_KEY_ESCAPE) {
            tm->pickerMode = 0;
            e.consume(this);
        }
    }

    void draw(const DrawArgs& args) override {
        if (!tm || !APP || !APP->scene || !APP->scene->rack) return;

        // Dark-mode overlay: draw an inverting white rect over each targeted
        // module, regardless of picker state. Skip when master is disabled so
        // the rule obeys the global toggle.
        if (tm->masterEnable) {
            // Invert (dark mode): inverts pixels beneath via blend.
            if (tm->darkModuleStrength > 0.f) {
                bool anyDark = false;
                for (auto& kv : tm->moduleRules) if (kv.second.moduleDark) { anyDark = true; break; }
                if (anyDark) {
                    float a = math::clamp(tm->darkModuleStrength, 0.f, 1.f);
                    nvgGlobalCompositeBlendFunc(args.vg, NVG_ONE_MINUS_DST_COLOR, NVG_ZERO);
                    for (ModuleWidget* mw : APP->scene->rack->getModules()) {
                        if (!mw || !mw->module || !mw->isVisible()) continue;
                        auto it = tm->moduleRules.find(mw->module->id);
                        if (it == tm->moduleRules.end() || !it->second.moduleDark) continue;
                        nvgBeginPath(args.vg);
                        nvgRect(args.vg, mw->box.pos.x, mw->box.pos.y,
                                mw->box.size.x, mw->box.size.y);
                        nvgFillColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, a));
                        nvgFill(args.vg);
                    }
                    nvgGlobalCompositeOperation(args.vg, NVG_SOURCE_OVER);
                }
            }

            // Brightness: overlay black to darken.
            for (ModuleWidget* mw : APP->scene->rack->getModules()) {
                if (!mw || !mw->module || !mw->isVisible()) continue;
                auto it = tm->moduleRules.find(mw->module->id);
                if (it == tm->moduleRules.end()) continue;
                const ModuleRule& r = it->second;
                if (r.brightness >= 1.f) continue;
                float alpha = 1.f - math::clamp(r.brightness, 0.f, 1.f);
                nvgBeginPath(args.vg);
                nvgRect(args.vg, mw->box.pos.x, mw->box.pos.y,
                        mw->box.size.x, mw->box.size.y);
                nvgFillColor(args.vg, nvgRGBAf(0.f, 0.f, 0.f, alpha));
                nvgFill(args.vg);
            }
        }

        // Picker-mode outlines. Every targeted module gets a persistent
        // outline in its own rule-colour (blue for hide, purple for invert),
        // so the user can see both kinds of state regardless of which
        // sub-mode the picker is currently in. Hovered module gets white.
        if (tm->pickerMode == 0) return;

        for (ModuleWidget* mw : APP->scene->rack->getModules()) {
            if (!mw || !mw->module) continue;
            int64_t id = mw->module->id;
            bool hovered = (id == hoveredModuleId);
            auto it = tm->moduleRules.find(id);
            bool hidden = (it != tm->moduleRules.end() && it->second.moduleHidden);
            bool inverted = (it != tm->moduleRules.end() && it->second.moduleDark);

            if (!hovered && !hidden && !inverted) continue;

            float x = mw->box.pos.x, y = mw->box.pos.y;
            float w = mw->box.size.x, h = mw->box.size.y;

            if (hidden) {
                nvgBeginPath(args.vg);
                nvgRect(args.vg, x - 1, y - 1, w + 2, h + 2);
                nvgStrokeColor(args.vg, nvgRGBA(70, 140, 240, 180));  // blue
                nvgStrokeWidth(args.vg, 1.5f);
                nvgStroke(args.vg);
            }
            if (inverted) {
                float pad = hidden ? 3.f : 1.f;
                nvgBeginPath(args.vg);
                nvgRect(args.vg, x - pad, y - pad, w + 2 * pad, h + 2 * pad);
                nvgStrokeColor(args.vg, nvgRGBA(180, 80, 220, 180));  // purple
                nvgStrokeWidth(args.vg, 1.5f);
                nvgStroke(args.vg);
            }
            if (hovered) {
                nvgBeginPath(args.vg);
                nvgRect(args.vg, x - 0.5f, y - 0.5f, w + 1.f, h + 1.f);
                nvgStrokeColor(args.vg, nvgRGB(255, 255, 255));
                nvgStrokeWidth(args.vg, 2.f);
                nvgStroke(args.vg);
            }
        }
    }
};

struct StateLabelWidget : widget::Widget {
    std::string text;
    TidyModule* tm = nullptr;
    void draw(const DrawArgs& args) override {
        std::shared_ptr<window::Font> font = APP->window->loadFont(
            asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        bool dark = lc::theme.dark;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, 7.f);
        nvgTextLetterSpacing(args.vg, 0.2f);
        nvgFillColor(args.vg, dark ? nvgRGB(230, 230, 230) : nvgRGB(26, 26, 26));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(args.vg, box.size.x / 2.f, 0.f, text.c_str(), NULL);
    }
};

TidyWidget::TidyWidget(TidyModule* module) {
    setModule(module);
    setPanel(createPanel(asset::plugin(pluginInstance, "res/Tidy.svg")));

    BackgroundWidget* bg = new BackgroundWidget;
    bg->tm = module;
    bg->box.size = box.size;
    bg->box.pos = Vec(0, 0);
    addChildBottom(bg);

    float screwX = (box.size.x - RACK_GRID_WIDTH) / 2.f;
    addChild(createWidget<ScrewBlack>(Vec(screwX, 0)));
    addChild(createWidget<ScrewBlack>(Vec(screwX, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    float logoTopMM = 128.5f - 8.f - 9.f;

    PngImageWidget* logo = new PngImageWidget;
    logo->tm = module;
    logo->path = asset::plugin(pluginInstance, "res/lc-icon-new.png");
    logo->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");
    logo->box.size = mm2px(Vec(9.f, 9.f));
    logo->box.pos = Vec((box.size.x - logo->box.size.x) / 2.f, mm2px(logoTopMM));
    addChild(logo);

    auto placeToggle = [&](float anchorMM, widget::Widget* toggle) {
        toggle->box.size = mm2px(Vec(5.f, 5.f));
        toggle->box.pos = Vec((box.size.x - toggle->box.size.x) / 2.f, mm2px(anchorMM));
        addChild(toggle);
    };
    auto placeLed = [&](float anchorMM, widget::Widget* led) {
        led->box.size = mm2px(Vec(1.6f, 1.6f));
        led->box.pos = Vec((box.size.x - led->box.size.x) / 2.f, mm2px(anchorMM - 3.f));
        addChild(led);
    };
    auto placeLabel = [&](float anchorMM, const std::string& text) {
        StateLabelWidget* label = new StateLabelWidget;
        label->text = text;
        label->tm = module;
        label->box.size = Vec(box.size.x, mm2px(3.f));
        label->box.pos = Vec(0, mm2px(anchorMM + 6.f));
        addChild(label);
    };

    EnableToggleWidget* enableToggle = new EnableToggleWidget;
    enableToggle->tm = module;
    placeToggle(logoTopMM - 10.f, enableToggle);
    StateLedWidget* enableLed = new StateLedWidget;
    enableLed->state = module ? &module->masterEnable : nullptr;
    enableLed->onColor = nvgRGB(60, 210, 90);
    placeLed(logoTopMM - 10.f, enableLed);
    placeLabel(logoTopMM - 10.f, "enable");

    PickerToggleWidget* pickerToggle = new PickerToggleWidget;
    pickerToggle->tm = module;
    placeToggle(logoTopMM - 25.f, pickerToggle);
    PickerLedWidget* pickerLed = new PickerLedWidget;
    pickerLed->tm = module;
    placeLed(logoTopMM - 25.f, pickerLed);
    placeLabel(logoTopMM - 25.f, "pick");

    if (module && APP && APP->scene && APP->scene->rack) {
        pickerOverlay = new PickerOverlay;
        pickerOverlay->tm = module;
        APP->scene->rack->addChild(pickerOverlay);
    }
}

TidyWidget::~TidyWidget() {
    restoreAll();
    if (pickerOverlay && APP && APP->scene && APP->scene->rack) {
        APP->scene->rack->removeChild(pickerOverlay);
        delete pickerOverlay;
        pickerOverlay = nullptr;
    }
}

int TidyWidget::matchPaletteIndex(NVGcolor c) {
    const auto& palette = settings::cableColors;
    const float eps = 1.f / 255.f;
    int best = -1;
    float bestDist = 1e9f;
    for (size_t i = 0; i < palette.size(); i++) {
        const NVGcolor& p = palette[i];
        float dr = p.r - c.r, dg = p.g - c.g, db = p.b - c.b;
        float dist = dr * dr + dg * dg + db * db;
        if (dist < bestDist) {
            bestDist = dist;
            best = (int)i;
        }
    }
    if (bestDist <= 3 * eps * eps) return best;
    return -1;
}

void TidyWidget::restoreAll() {
    if (!APP || !APP->scene || !APP->scene->rack) return;
    RackWidget* rack = APP->scene->rack;

    for (auto& kv : originalCableColors) {
        if (CableWidget* cw = rack->getCable(kv.first))
            cw->color = kv.second;
    }
    originalCableColors.clear();

    for (int64_t id : hiddenCables) {
        if (CableWidget* cw = rack->getCable(id)) {
            cw->setVisible(true);
            if (cw->inputPlug) cw->inputPlug->setVisible(true);
            if (cw->outputPlug) cw->outputPlug->setVisible(true);
        }
    }
    hiddenCables.clear();

    for (int64_t id : hiddenPlugCables) {
        if (CableWidget* cw = rack->getCable(id)) {
            if (cw->inputPlug) cw->inputPlug->setVisible(true);
            if (cw->outputPlug) cw->outputPlug->setVisible(true);
        }
    }
    hiddenPlugCables.clear();

    for (int64_t id : hiddenModules) {
        if (ModuleWidget* mw = rack->getModule(id))
            mw->setVisible(true);
    }
    hiddenModules.clear();
}

void TidyWidget::applyRules() {
    TidyModule* tm = dynamic_cast<TidyModule*>(module);
    if (!tm) return;
    if (!APP || !APP->scene || !APP->scene->rack) return;
    RackWidget* rack = APP->scene->rack;

    if (!tm->masterEnable) {
        restoreAll();
        return;
    }

    std::set<int64_t> nextHiddenCables;
    std::set<int64_t> nextHiddenPlugs;
    std::set<int64_t> nextHiddenModules;
    std::map<int64_t, NVGcolor> nextOriginals;

    for (ModuleWidget* mw : rack->getModules()) {
        if (!mw || !mw->module) continue;
        int64_t id = mw->module->id;
        auto it = tm->moduleRules.find(id);
        if (it == tm->moduleRules.end()) continue;
        const ModuleRule& r = it->second;
        if (r.moduleHidden) {
            mw->setVisible(false);
            nextHiddenModules.insert(id);
        }
    }
    for (int64_t id : hiddenModules) {
        if (!nextHiddenModules.count(id)) {
            if (ModuleWidget* mw = rack->getModule(id))
                mw->setVisible(true);
        }
    }
    hiddenModules = nextHiddenModules;

    for (CableWidget* cw : rack->getCompleteCables()) {
        if (!cw || !cw->cable) continue;
        int64_t cid = cw->cable->id;

        float effective = 1.f;

        int paletteIdx = -1;
        auto origIt = originalCableColors.find(cid);
        NVGcolor origColor = (origIt != originalCableColors.end()) ? origIt->second : cw->color;
        paletteIdx = matchPaletteIndex(origColor);
        if (paletteIdx >= 0 && paletteIdx < (int)tm->colorOpacity.size())
            effective = std::min(effective, tm->colorOpacity[paletteIdx]);

        int64_t inId = cw->cable->inputModule ? cw->cable->inputModule->id : -1;
        int64_t outId = cw->cable->outputModule ? cw->cable->outputModule->id : -1;
        bool hideConnected = false;
        auto inRule = tm->moduleRules.find(inId);
        if (inRule != tm->moduleRules.end()) {
            effective = std::min(effective, inRule->second.cableOpacity);
            hideConnected = hideConnected || inRule->second.hideConnectedCables;
        }
        auto outRule = tm->moduleRules.find(outId);
        if (outRule != tm->moduleRules.end()) {
            effective = std::min(effective, outRule->second.cableOpacity);
            hideConnected = hideConnected || outRule->second.hideConnectedCables;
        }
        if (hideConnected) effective = 0.f;

        if (effective < 1.f && !originalCableColors.count(cid))
            nextOriginals[cid] = cw->color;
        else if (originalCableColors.count(cid))
            nextOriginals[cid] = originalCableColors[cid];

        if (effective <= 0.f) {
            cw->setVisible(false);
            nextHiddenCables.insert(cid);
        } else {
            NVGcolor c = origColor;
            c.a = effective;
            cw->color = c;
            if (!cw->isVisible()) cw->setVisible(true);
        }

        if (tm->hidePlugHeads && effective <= 0.f) {
            if (cw->inputPlug) cw->inputPlug->setVisible(false);
            if (cw->outputPlug) cw->outputPlug->setVisible(false);
            nextHiddenPlugs.insert(cid);
        }
    }

    for (int64_t id : hiddenCables) {
        if (!nextHiddenCables.count(id)) {
            if (CableWidget* cw = rack->getCable(id)) {
                cw->setVisible(true);
                auto it = originalCableColors.find(id);
                if (it != originalCableColors.end()) cw->color = it->second;
            }
        }
    }
    for (auto& kv : originalCableColors) {
        if (!nextOriginals.count(kv.first)) {
            if (CableWidget* cw = rack->getCable(kv.first))
                cw->color = kv.second;
        }
    }
    for (int64_t id : hiddenPlugCables) {
        if (!nextHiddenPlugs.count(id)) {
            if (CableWidget* cw = rack->getCable(id)) {
                if (cw->inputPlug) cw->inputPlug->setVisible(true);
                if (cw->outputPlug) cw->outputPlug->setVisible(true);
            }
        }
    }

    hiddenCables = nextHiddenCables;
    hiddenPlugCables = nextHiddenPlugs;
    originalCableColors = nextOriginals;
}

void TidyWidget::step() {
    ModuleWidget::step();
    if (!module) return;

    TidyModule* tm = dynamic_cast<TidyModule*>(module);
    if (tm && tm->masterEnable != lastMasterEnable) {
        lastMasterEnable = tm->masterEnable;
        scanAccumulator = scanInterval;
    }
    if (tm && tm->justLoaded) {
        tm->justLoaded = false;
        // VCV may still be populating the rack when dataFromJson fires.
        // Re-apply every frame for ~1s so late-arriving target modules get
        // caught by the rule sweep even if they weren't in rack yet on load.
        forceScanFrames = 60;
    }

    if (forceScanFrames > 0) {
        forceScanFrames--;
        applyRules();
        scanAccumulator = 0.f;
    } else {
        scanAccumulator += APP->window->getLastFrameDuration();
        if (scanAccumulator >= scanInterval) {
            scanAccumulator = 0.f;
            applyRules();
        }
    }
}

Model* modelTidy = createModel<TidyModule, TidyWidget>("Tidy");
