#include "Tidy.hpp"
#include "Theme.hpp"

namespace {

// ─── Opacity slider (used in color + module submenus) ────────────────────────

struct OpacityQuantity : Quantity {
    float* ptr;
    std::string label;
    OpacityQuantity(float* ptr, std::string label) : ptr(ptr), label(label) {}
    void setValue(float v) override { *ptr = math::clamp(v, 0.f, 1.f); }
    float getValue() override { return *ptr; }
    float getMinValue() override { return 0.f; }
    float getMaxValue() override { return 1.f; }
    float getDefaultValue() override { return 1.f; }
    std::string getLabel() override { return label; }
    std::string getUnit() override { return ""; }
    int getDisplayPrecision() override { return 2; }
};

struct OpacitySlider : ui::Slider {
    OpacitySlider(float* ptr, std::string label) {
        quantity = new OpacityQuantity(ptr, label);
        box.size.x = 200.f;
    }
    ~OpacitySlider() override { delete quantity; }
};

// ─── Color palette submenu ───────────────────────────────────────────────────

struct ColorOpacityMenu : MenuItem {
    TidyModule* tm;
    Menu* createChildMenu() override {
        Menu* m = new Menu;
        size_t n = std::min(tm->colorOpacity.size(), settings::cableColors.size());
        for (size_t i = 0; i < n; i++) {
            std::string label = "Color " + std::to_string(i + 1);
            m->addChild(new OpacitySlider(&tm->colorOpacity[i], label));
        }
        m->addChild(new MenuSeparator);
        m->addChild(createMenuItem("Reset all", "", [this]() {
            std::fill(tm->colorOpacity.begin(), tm->colorOpacity.end(), 1.f);
        }));
        return m;
    }
};

// ─── Per-module submenu (cables / module opacity / hide-connected) ──────────

struct SingleModuleMenu : MenuItem {
    TidyModule* tm;
    int64_t moduleId;
    Menu* createChildMenu() override {
        Menu* m = new Menu;
        ModuleRule& r = tm->moduleRules[moduleId];
        m->addChild(new OpacitySlider(&r.cableOpacity, "Cable opacity"));
        m->addChild(createBoolPtrMenuItem("Hide module", "", &r.moduleHidden));
        m->addChild(createBoolPtrMenuItem("Force dark mode", "invert panel", &r.moduleDark));
        m->addChild(new OpacitySlider(&r.brightness, "Brightness"));
        m->addChild(createBoolPtrMenuItem("Hide connected cables", "", &r.hideConnectedCables));
        m->addChild(new MenuSeparator);
        m->addChild(createMenuItem("Reset this module", "", [this]() {
            tm->moduleRules.erase(moduleId);
        }));
        return m;
    }
};

struct ModulesMenu : MenuItem {
    TidyModule* tm;
    TidyWidget* self;
    Menu* createChildMenu() override {
        Menu* m = new Menu;
        if (!APP || !APP->scene || !APP->scene->rack) return m;

        std::vector<ModuleWidget*> mods = APP->scene->rack->getModules();
        std::sort(mods.begin(), mods.end(), [](ModuleWidget* a, ModuleWidget* b) {
            return a->box.pos.x < b->box.pos.x;
        });

        for (ModuleWidget* mw : mods) {
            if (!mw || !mw->module || !mw->model) continue;
            int64_t id = mw->module->id;
            bool isSelf = (self->module && id == self->module->id);
            auto it = tm->moduleRules.find(id);
            bool hasRule = it != tm->moduleRules.end();

            std::string label = isSelf ? "This Tidy" : mw->model->name;
            std::string status;
            if (hasRule) {
                const ModuleRule& r = it->second;
                if (r.moduleHidden) status = "hidden";
                else if (r.cableOpacity < 1.f || r.hideConnectedCables) status = "cables";
                else status = "•";
            }

            SingleModuleMenu* item = createMenuItem<SingleModuleMenu>(label, status + (status.empty() ? "" : "  ") + RIGHT_ARROW);
            item->tm = tm;
            item->moduleId = id;
            m->addChild(item);
        }
        return m;
    }
};

// ─── Dark modules submenu ──────────────────────────────────────────────────

struct DarkStrengthQuantity : Quantity {
    float* ptr;
    DarkStrengthQuantity(float* ptr) : ptr(ptr) {}
    void setValue(float v) override { *ptr = math::clamp(v, 0.f, 1.f); }
    float getValue() override { return *ptr; }
    float getMinValue() override { return 0.f; }
    float getMaxValue() override { return 1.f; }
    float getDefaultValue() override { return 1.f; }
    std::string getLabel() override { return "Invert strength"; }
    std::string getUnit() override { return ""; }
    int getDisplayPrecision() override { return 2; }
};

struct DarkStrengthSlider : ui::Slider {
    DarkStrengthSlider(float* ptr) {
        quantity = new DarkStrengthQuantity(ptr);
        box.size.x = 200.f;
    }
    ~DarkStrengthSlider() override { delete quantity; }
};

struct DarkModulesMenu : MenuItem {
    TidyModule* tm;
    Menu* createChildMenu() override {
        Menu* m = new Menu;
        if (!APP || !APP->scene || !APP->scene->rack) return m;

        m->addChild(new DarkStrengthSlider(&tm->darkModuleStrength));
        m->addChild(new MenuSeparator);

        std::vector<std::pair<int64_t, std::string>> dark;
        for (auto& kv : tm->moduleRules) {
            if (!kv.second.moduleDark) continue;
            std::string name;
            if (ModuleWidget* mw = APP->scene->rack->getModule(kv.first)) {
                if (mw->model) name = mw->model->name;
            }
            if (name.empty()) name = "(unknown)";
            dark.push_back({kv.first, name});
        }

        if (dark.empty()) {
            m->addChild(createMenuLabel("(none)"));
            return m;
        }

        for (auto& kv : dark) {
            int64_t id = kv.first;
            std::string name = kv.second;
            m->addChild(createMenuItem(name, "clear", [this, id]() {
                auto it = tm->moduleRules.find(id);
                if (it != tm->moduleRules.end()) it->second.moduleDark = false;
            }));
        }

        m->addChild(new MenuSeparator);
        m->addChild(createMenuItem("Clear all", "", [this]() {
            for (auto& kv : tm->moduleRules) kv.second.moduleDark = false;
        }));
        return m;
    }
};

// ─── Hidden modules submenu ─────────────────────────────────────────────────

struct HiddenModulesMenu : MenuItem {
    TidyModule* tm;
    Menu* createChildMenu() override {
        Menu* m = new Menu;
        if (!APP || !APP->scene || !APP->scene->rack) return m;

        std::vector<std::pair<int64_t, std::string>> hidden;
        for (auto& kv : tm->moduleRules) {
            if (!kv.second.moduleHidden) continue;
            std::string name;
            if (ModuleWidget* mw = APP->scene->rack->getModule(kv.first)) {
                if (mw->model) name = mw->model->name;
            }
            if (name.empty()) name = "(unknown)";
            hidden.push_back({kv.first, name});
        }

        if (hidden.empty()) {
            m->addChild(createMenuLabel("(none)"));
            return m;
        }

        for (auto& kv : hidden) {
            int64_t id = kv.first;
            std::string name = kv.second;
            m->addChild(createMenuItem(name, "unhide", [this, id]() {
                auto it = tm->moduleRules.find(id);
                if (it != tm->moduleRules.end()) it->second.moduleHidden = false;
            }));
        }

        m->addChild(new MenuSeparator);
        m->addChild(createMenuItem("Unhide all", "", [this]() {
            for (auto& kv : tm->moduleRules) kv.second.moduleHidden = false;
        }));
        return m;
    }
};

// ─── Presets submenu ────────────────────────────────────────────────────────

struct SingleStateMenu : MenuItem {
    TidyModule* tm;
    size_t index;
    Menu* createChildMenu() override {
        Menu* m = new Menu;
        m->addChild(createMenuItem("Load", "", [this]() { tm->loadPreset(index); }));
        m->addChild(createMenuItem("Save over this", "", [this]() { tm->overwritePreset(index); }));
        m->addChild(new MenuSeparator);
        m->addChild(createMenuItem("Delete", "", [this]() { tm->deletePreset(index); }));
        return m;
    }
};

struct StatesMenu : MenuItem {
    TidyModule* tm;
    Menu* createChildMenu() override {
        Menu* m = new Menu;
        m->addChild(createMenuItem("Save current as new state", "", [this]() {
            std::string name = "State " + std::to_string(tm->presets.size() + 1);
            tm->savePreset(name);
        }));
        if (!tm->presets.empty()) {
            m->addChild(new MenuSeparator);
            for (size_t i = 0; i < tm->presets.size(); i++) {
                SingleStateMenu* item = createMenuItem<SingleStateMenu>(tm->presets[i].name, RIGHT_ARROW);
                item->tm = tm;
                item->index = i;
                m->addChild(item);
            }
        }
        return m;
    }
};

} // namespace

// ─── Top-level context menu ─────────────────────────────────────────────────

void TidyWidget::appendContextMenu(Menu* menu) {
    TidyModule* tm = dynamic_cast<TidyModule*>(module);
    if (!tm) return;

    menu->addChild(new MenuSeparator);

    menu->addChild(createBoolPtrMenuItem("Master enable", "", &tm->masterEnable));

    const char* pickerLabel = "Picker: off";
    if (tm->pickerMode == 1) pickerLabel = "Picker: hide";
    else if (tm->pickerMode == 2) pickerLabel = "Picker: invert";
    menu->addChild(createMenuItem("Picker mode", pickerLabel, [tm]() {
        tm->pickerMode = (tm->pickerMode + 1) % 3;
    }));

    menu->addChild(new MenuSeparator);

    ColorOpacityMenu* colorItem = createMenuItem<ColorOpacityMenu>("Cable opacity by color", RIGHT_ARROW);
    colorItem->tm = tm;
    menu->addChild(colorItem);

    ModulesMenu* modItem = createMenuItem<ModulesMenu>("Modules in rack", RIGHT_ARROW);
    modItem->tm = tm;
    modItem->self = this;
    menu->addChild(modItem);

    HiddenModulesMenu* hiddenItem = createMenuItem<HiddenModulesMenu>("Hidden modules", RIGHT_ARROW);
    hiddenItem->tm = tm;
    menu->addChild(hiddenItem);

    DarkModulesMenu* darkItem = createMenuItem<DarkModulesMenu>("Dark modules", RIGHT_ARROW);
    darkItem->tm = tm;
    menu->addChild(darkItem);

    StatesMenu* stateItem = createMenuItem<StatesMenu>("States", RIGHT_ARROW);
    stateItem->tm = tm;
    menu->addChild(stateItem);

    menu->addChild(new MenuSeparator);

    menu->addChild(createMenuItem("Dark mode (shared)",
        CHECKMARK(lc::theme.dark), []() {
            lc::theme.dark = !lc::theme.dark;
            lc::saveTheme();
        }));
    menu->addChild(createBoolPtrMenuItem("Also hide plug heads", "", &tm->hidePlugHeads));

    menu->addChild(createMenuItem("Restore everything now", "", [this]() {
        restoreAll();
    }));
}
