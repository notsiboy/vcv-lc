#pragma once
#include "plugin.hpp"
#include <dsp/digital.hpp>

struct QModModule : Module {
    static const int NUM_SLOTS = 14;
    static const int NUM_ROWS  = 7;

    // No inputs — the trigger/CV jack that used to live in qmod was pulled
    // out so the header row can host two per-column mode buttons instead.
    // Trigger-driven behaviour still lives on qmod+ (which keeps its own
    // trigger input).
    enum InputIds {
        NUM_INPUTS
    };

    enum OutputIds {
        ENUMS(MOD_OUTPUT, NUM_SLOTS),
        NUM_OUTPUTS
    };

    enum ModMode {
        MODE_RAND_TRIG = 0,
        MODE_TRIG_SH,
        MODE_SMOOTH,
        MODE_SH,
        MODE_LFO,
        MODE_RAND_GATE,
        NUM_MODES
    };

    enum LfoShape {
        LFO_SINE = 0,
        LFO_TRIANGLE,
        LFO_SQUARE,
        LFO_SAW,
        NUM_LFO_SHAPES
    };

    enum Range {
        RANGE_UNI_10 = 0,
        RANGE_UNI_5,
        RANGE_UNI_1,
        RANGE_BI_10,
        RANGE_BI_5,
        RANGE_BI_1,
        NUM_RANGES
    };

    // Split global mode — one per column. Each cycle button broadcasts into
    // only its half of slotMode[]. Per-slot arm clicks still override.
    int modeL = MODE_SMOOTH;
    int modeR = MODE_SMOOTH;

    int lfoShape = LFO_SINE;
    float spreadRatio = 0.1f;

    int slotMode[NUM_SLOTS];
    int range[NUM_SLOTS];
    float attenuator[NUM_SLOTS];
    float offset[NUM_SLOTS];

    bool rateSpread = true;
    float globalRate = 4.5f;
    // Default is 0 — no extra slew on step-shaped modes; smooth random
    // always has its own baked-in smootherstep regardless of this value.
    float smoothness = 0.f;

    // Array membership. When false, this module is a singleton — it will
    // not join into a contiguous run of LC Q-devices, and neighbouring Q
    // modules skip it when walking the array.
    bool inArray = true;

    // Per-slot state for all generators. All live simultaneously so mode
    // switches are glitch-free.
    float phase[NUM_SLOTS];
    float shValue[NUM_SLOTS];
    float shCounter[NUM_SLOTS];
    float smoothFrom[NUM_SLOTS];
    float smoothTo[NUM_SLOTS];
    float smoothCounter[NUM_SLOTS];
    float smoothPeriod[NUM_SLOTS];
    float trigCounter[NUM_SLOTS];
    rack::dsp::PulseGenerator trigPulse[NUM_SLOTS];
    // Random-gate state: on/off per slot, flipped when its counter runs out.
    bool gateState[NUM_SLOTS];
    float slewState[NUM_SLOTS];
    float modLevel[NUM_SLOTS];

    QModModule();
    void process(const ProcessArgs& args) override;
    json_t* dataToJson() override;
    void dataFromJson(json_t* rootJ) override;

    // Per-column cycles and broadcasts. col: 0 = left (even slots), 1 = right.
    void cycleColumnMode(int col);
    void setColumnMode(int col, int newMode);
    void cycleSlotMode(int slot);
    // Invoked from a neighbouring qmod+'s trigger input when their arrays
    // are joined — resets phases / samples / gate states as appropriate
    // per slot's mode.
    void resyncAll();
    float frequencyHz(int slot);
    float mapToVoltage(int slot, float v01) const;
};

struct QModWidget : ModuleWidget {
    QModWidget(QModModule* module);
    void appendContextMenu(Menu* menu) override;
};
