#pragma once
#include "plugin.hpp"

struct FlowModule : Module {
    static const int NUM_EFFECTS = 4;
    static const int NUM_PRESETS = 8;

    enum InputIds {
        IN_CHAIN,
        ENUMS(IN_RETURN, NUM_EFFECTS),
        IN_CV_ORDER,
        IN_BYPASS_GATE,
        NUM_INPUTS
    };

    enum OutputIds {
        OUT_CHAIN,
        ENUMS(OUT_SEND, NUM_EFFECTS),
        NUM_OUTPUTS
    };

    // Currently active permutation index (0..NUM_PRESETS-1). Updated by the
    // master button, right-click menu, or the CV order input.
    int preset = 0;

    // Transition style when the preset changes:
    //   FADE  — classic quick 20 ms output duck, just enough to hide the
    //           sample-level jump. Clicky material switches cleanly.
    //   MORPH — audible 400 ms crossfade; sounds like a smooth transition
    //           between two chains and the A/B/C/D display cross-dissolves
    //           to match.
    enum FadeMode {
        FADE_FAST = 0,
        FADE_MORPH,
        NUM_FADE_MODES
    };
    int fadeMode = FADE_FAST;

    // Click suppression on preset change. When `preset` moves, we arm a
    // short output fade (0 → 1) and apply it to every send + the chain
    // output so effects and downstream modules see a smooth transition
    // instead of a sample-level discontinuity in their input.
    int lastPreset = 0;
    int prevPresetForXfade = 0;   // what we were on before the current fade
    int xfadeSamples = 0;
    int xfadeTotal = 0;

    // Bypass state. When active, chain OUT passes chain IN through untouched
    // (sends still carry the input so effects stay primed for a clean
    // re-entry). The panel button toggles this; the gate input OR-s in — as
    // long as the gate is high, bypass is forced on. Tracked transitions
    // arm the same fade machinery used for preset swaps so the handoff is
    // click-free.
    bool bypassed = false;
    bool lastBypassActive = false;

    // Fixed factory permutation table. Each row lists indices into
    // [A=0, B=1, C=2, D=3] in chain-processing order (index 0 = first effect
    // the signal hits). Ordering is curated to cover straight, reverse,
    // pair-swap, inner/outer-swap and rotation feels.
    static const int FACTORY[NUM_PRESETS][NUM_EFFECTS];

    FlowModule();
    void process(const ProcessArgs& args) override;
    json_t* dataToJson() override;
    void dataFromJson(json_t* rootJ) override;

    void setPreset(int p);
    void cyclePreset();
};

struct FlowWidget : ModuleWidget {
    FlowWidget(FlowModule* module);
    void appendContextMenu(Menu* menu) override;
};
