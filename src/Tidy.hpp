#pragma once
#include "plugin.hpp"
#include <map>
#include <set>
#include <vector>

struct ModuleRule {
    bool moduleHidden = false;
    bool moduleDark = false;
    float brightness = 1.f;   // 1 = unchanged, 0 = black
    float cableOpacity = 1.f;
    bool hideConnectedCables = false;
};

struct Preset {
    std::string name;
    bool hidePlugHeads = false;
    std::vector<float> colorOpacity;
    std::map<int64_t, ModuleRule> moduleRules;
};

struct TidyModule : Module {
    // Picker mode values: 0 = off, 1 = hide picker, 2 = invert picker
    static const int PICKER_OFF = 0;
    static const int PICKER_HIDE = 1;
    static const int PICKER_DARK = 2;

    bool masterEnable = true;
    bool hidePlugHeads = false;
    int pickerMode = 0;
    float darkModuleStrength = 1.f;
    std::vector<float> colorOpacity;
    std::map<int64_t, ModuleRule> moduleRules;
    std::vector<Preset> presets;

    bool justLoaded = false;

    TidyModule();
    json_t* dataToJson() override;
    void dataFromJson(json_t* rootJ) override;

    void savePreset(const std::string& name);
    void loadPreset(size_t index);
    void overwritePreset(size_t index);
    void deletePreset(size_t index);
};

struct PickerOverlay;

struct TidyWidget : ModuleWidget {
    float scanAccumulator = 0.f;
    float scanInterval = 0.17f;
    bool lastMasterEnable = true;
    int forceScanFrames = 0;

    std::map<int64_t, NVGcolor> originalCableColors;
    std::set<int64_t> hiddenCables;
    std::set<int64_t> hiddenPlugCables;
    std::set<int64_t> hiddenModules;

    PickerOverlay* pickerOverlay = nullptr;

    TidyWidget(TidyModule* module);
    ~TidyWidget() override;

    void step() override;
    void appendContextMenu(Menu* menu) override;

    void applyRules();
    void restoreAll();
    int matchPaletteIndex(NVGcolor c);
};
