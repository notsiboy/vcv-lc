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

    // Per-slot CV conditioning. Applied as `v = v * atten + offset` before
    // the polarity-aware normalise step, so users can trim a too-hot CV or
    // bias a bipolar signal without rewiring.
    float attenuator[NUM_SLOTS];   // default 1.0, range -2..+2
    float offset[NUM_SLOTS];       // default 0.0V, range -10..+10 V

    // Last 0..1 normalised input for each slot — written by process(), read
    // by the UI to drive the per-slot arm-LED brightness. Non-atomic: one
    // writer, many readers, UI only needs a visually-plausible value.
    float modLevel[NUM_SLOTS] = {};

    // When a qmap is flanked by qmods on BOTH sides, which one feeds the aux
    // inputs: 0 = left (default), 1 = right. Ignored when only one side has
    // a qmod; the available one is used automatically.
    int qmodFavour = 0;

    // Array membership. When false, this module is a singleton — won't
    // join the contiguous Q-array containing it, and neighbouring Q
    // modules skip it when walking.
    bool inArray = true;

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
