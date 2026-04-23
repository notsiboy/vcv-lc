#pragma once
#include "plugin.hpp"
#include <array>

struct QMapModule : Module {
    static const int NUM_SLOTS = 14;

    // Engine inputs: aux CV inputs, one per slot.
    enum InputIds {
        ENUMS(AUX_INPUT, NUM_SLOTS),
        NUM_INPUTS
    };

    // Weak handles into remote params — Engine keeps these alive as modules
    // come and go. We bind/unbind via updateParamHandle().
    engine::ParamHandle paramHandles[NUM_SLOTS];

    // Per-slot input polarity: false = unipolar (0..10V), true = bipolar (-5..5V).
    bool bipolar[NUM_SLOTS];

    // -1 when no slot is armed, otherwise the slot awaiting touch-assign.
    int armedSlot = -1;
    // True when the arming sweep was kicked off by the master q-map button —
    // causes the next assign to auto-advance to the next slot.
    bool sequentialArm = false;

    QMapModule();
    ~QMapModule() override;

    void process(const ProcessArgs& args) override;

    json_t* dataToJson() override;
    void dataFromJson(json_t* rootJ) override;

    // Advance the arming cursor. Called by the master q-map button and by the
    // widget after a successful touch-assign in sequential mode.
    void advanceArm();
    void clearSlot(int slot);
};

struct QMapWidget : ModuleWidget {
    QMapWidget(QMapModule* module);
    void step() override;
    void appendContextMenu(Menu* menu) override;
};
