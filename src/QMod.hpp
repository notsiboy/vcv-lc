#pragma once
#include "plugin.hpp"
#include <dsp/digital.hpp>

struct QModModule : Module {
    static const int NUM_SLOTS = 14;

    enum InputIds {
        IN_TRIG,
        NUM_INPUTS
    };

    enum OutputIds {
        ENUMS(MOD_OUTPUT, NUM_SLOTS),
        NUM_OUTPUTS
    };

    // Cycle order on the master button. Triggered S+H sits right after
    // random triggers since both modes lean on the external trigger input.
    enum ModMode {
        MODE_RAND_TRIG = 0,
        MODE_TRIG_SH,
        MODE_SMOOTH,
        MODE_SH,
        MODE_LFO,
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

    // How the trigger input jack is interpreted.
    enum InputMode {
        INPUT_TRIGGER = 0,        // rising edge → resyncAll()
        INPUT_GATE,               // run while high, freeze while low
        INPUT_CV_RATE,            // 0..10V → globalRate
        INPUT_CV_SMOOTHNESS,      // 0..10V → smoothness
        INPUT_CV_MODE,            // 0..10V → broadcast mode
        NUM_INPUT_MODES
    };

    int mode = MODE_SMOOTH;
    int inputMode = INPUT_TRIGGER;
    int lfoShape = LFO_SINE;
    // Spread ratio: slowest slot = globalRate * spreadRatio when stagger is
    // on. 0.1 => slot 13 runs at 1/10 of slot 0; smaller values dig deeper
    // into the slow end.
    float spreadRatio = 0.1f;
    // Per-slot mode override. Cycling the master button broadcasts `mode`
    // into every entry here; clicking a per-slot arm button cycles just that
    // slot, so it can diverge from the global value.
    int slotMode[NUM_SLOTS];
    int range[NUM_SLOTS];
    // Per-slot output conditioning, applied after voltage mapping.
    float attenuator[NUM_SLOTS];   // default 1.0, range -2..+2
    float offset[NUM_SLOTS];       // default 0.0V, range -10..+10 V

    // When true, per-slot rates spread log-wise fast→slow (slot 0 fastest).
    // When false, every slot runs at `globalRate`.
    bool rateSpread = true;
    // Fastest rate (Hz). When rateSpread is on, slot 0 runs at globalRate and
    // slot N-1 runs at globalRate * spreadRatio (default 0.008 ≈ 125× slower).
    float globalRate = 4.5f;
    // 0 = hard step between random targets, 1 = full smootherstep slew.
    // Only applies to MODE_SMOOTH.
    float smoothness = 0.4f;

    // CV overrides. When the trigger jack is in INPUT_CV_RATE or
    // INPUT_CV_SMOOTHNESS mode and a cable is connected, these hold the
    // live CV-derived value. <0 means "inactive, use the menu setting".
    // Resets to inactive the moment the cable or input mode goes away.
    float rateOverride = -1.f;
    float smoothnessOverride = -1.f;

    // Per-slot state for all generators. All live simultaneously so mode
    // switches are glitch-free.
    float phase[NUM_SLOTS];
    float shValue[NUM_SLOTS];
    float shCounter[NUM_SLOTS];
    float smoothFrom[NUM_SLOTS];
    float smoothTo[NUM_SLOTS];
    float smoothCounter[NUM_SLOTS];
    float smoothPeriod[NUM_SLOTS];
    float trigCounter[NUM_SLOTS];     // samples until next random trigger
    rack::dsp::PulseGenerator trigPulse[NUM_SLOTS];
    // One-pole LPF state per slot. Active for LFO / S+H / triggered S+H so
    // the `smoothness` slider doubles as a slew limiter for step-shaped
    // modes. Random triggers deliberately bypass it so pulses stay sharp.
    float slewState[NUM_SLOTS];
    // Latest 0..1 output value per slot — written by process(), read by the
    // UI thread to drive the per-slot LED glow brightness. Non-atomic: one
    // writer, many readers, and the UI only needs a visually-plausible value.
    float modLevel[NUM_SLOTS];

    rack::dsp::SchmittTrigger trigInEdge;

    QModModule();
    void process(const ProcessArgs& args) override;
    json_t* dataToJson() override;
    void dataFromJson(json_t* rootJ) override;

    // Global cycle — advances `mode` and broadcasts it to every slot,
    // erasing any per-slot overrides.
    void cycleMode();
    // Advances the override for a single slot.
    void cycleSlotMode(int slot);
    // Set the global mode and broadcast (used by the right-click menu).
    void setGlobalMode(int newMode);
    void resyncAll();
    float frequencyHz(int slot) const;
    float mapToVoltage(int slot, float v01) const;
};

struct QModWidget : ModuleWidget {
    QModWidget(QModModule* module);
    void appendContextMenu(Menu* menu) override;
};
