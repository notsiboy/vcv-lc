#pragma once
#include "plugin.hpp"
#include <dsp/digital.hpp>

// qmod+ — 6 HP variant of qmod with:
//   • a separate global mode cycle button per column (left/right)
//   • the trigger input moved to a new 2 HP strip on the right
//   • a per-row rate knob in that strip (log-scaled ±1 decade around 1×)
//
// Per-slot overrides, LFO shape, input modes, range, atten/offset, fades and
// slew behaviour all inherit qmod's semantics unchanged.

struct QModPlusModule : Module {
    static const int NUM_SLOTS = 14;
    static const int NUM_ROWS  = 7;    // 2 slots per row (left/right column)

    enum InputIds {
        IN_TRIG,
        NUM_INPUTS
    };

    enum OutputIds {
        ENUMS(MOD_OUTPUT, NUM_SLOTS),
        NUM_OUTPUTS
    };

    enum ParamIds {
        ENUMS(RATE_KNOB, NUM_ROWS),
        NUM_PARAMS
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

    enum InputMode {
        INPUT_TRIGGER = 0,
        INPUT_GATE,
        INPUT_CV_RATE,
        INPUT_CV_SMOOTHNESS,
        INPUT_CV_MODE,
        NUM_INPUT_MODES
    };

    // Split global mode — one per column. Each cycle button broadcasts into
    // only its half of slotMode[]. Per-slot arm clicks still override.
    int modeL = MODE_SMOOTH;
    int modeR = MODE_SMOOTH;

    int inputMode = INPUT_TRIGGER;
    int lfoShape = LFO_SINE;
    float spreadRatio = 0.1f;

    int slotMode[NUM_SLOTS];
    int range[NUM_SLOTS];
    float attenuator[NUM_SLOTS];
    float offset[NUM_SLOTS];
    // Per-slot asymmetric output slew — see QMod.hpp for the convention.
    float slew[NUM_SLOTS];
    float slewShape[NUM_SLOTS];

    // qmod+ has no persistent "stagger" state. The row knobs are the single
    // source of truth for per-row rate; `spreadRatio` just parameterises the
    // one-shot "apply stagger" menu action that writes the log-spread
    // multipliers into those knobs so the stagger literally shows in the
    // knob positions.
    float globalRate = 4.5f;
    // Default is 0 — smooth random always has its own full smootherstep
    // regardless of this value; smoothness only affects the post-process
    // slew on LFO / S+H / triggered S+H.
    float smoothness = 0.f;

    // Array membership. When false, this module is a singleton.
    bool inArray = true;

    float rateOverride = -1.f;
    float smoothnessOverride = -1.f;

    float phase[NUM_SLOTS];
    float shValue[NUM_SLOTS];
    float shCounter[NUM_SLOTS];
    float smoothFrom[NUM_SLOTS];
    float smoothTo[NUM_SLOTS];
    float smoothCounter[NUM_SLOTS];
    float smoothPeriod[NUM_SLOTS];
    float trigCounter[NUM_SLOTS];
    rack::dsp::PulseGenerator trigPulse[NUM_SLOTS];
    bool gateState[NUM_SLOTS];
    float slewState[NUM_SLOTS];
    float modLevel[NUM_SLOTS];

    rack::dsp::SchmittTrigger trigInEdge;

    QModPlusModule();
    void process(const ProcessArgs& args) override;
    json_t* dataToJson() override;
    void dataFromJson(json_t* rootJ) override;

    // Per-column cycles and broadcasts. col: 0 = left (even slots), 1 = right.
    void cycleColumnMode(int col);
    void setColumnMode(int col, int newMode);
    // Writes row-knob positions so the 7 row multipliers land on a log spread
    // from 1× (row 0) → spreadRatio× (row 6). Call from the menu; knobs
    // animate into their new positions and the user can hand-tweak from there.
    void applyStaggerToKnobs();
    void resetRowKnobs();
    void cycleSlotMode(int slot);
    void resyncAll();
    float frequencyHz(int slot);             // non-const because it reads a Param
    float mapToVoltage(int slot, float v01) const;
};

struct QModPlusWidget : ModuleWidget {
    QModPlusWidget(QModPlusModule* module);
    void appendContextMenu(Menu* menu) override;
};
