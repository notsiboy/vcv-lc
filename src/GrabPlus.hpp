#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// grab+ — combined recorder. Embeds a GrabModule + TakeModule as members so
// both DSP paths run off one shared stereo input pair. Adds the universal
// mode cycle (off / grab / snip), a big dual-action rec button (short click =
// force-rec toggle, long click = save take), a "save to dated subfolder"
// convenience, and a merged right-click menu covering all three mode
// submenus.
//
// Builds on grab + take rather than duplicating their DSP — forwards inputs
// to each inner module each sample and lets them do their thing.
// ─────────────────────────────────────────────────────────────────────────────

#include "Grab.hpp"
#include "Take.hpp"
#include <rack.hpp>

using namespace rack;

struct GrabPlusModule : Module {
    enum InputId { IN_L, IN_R, NUM_INPUTS };

    GrabModule grab;
    TakeModule take;

    // When true, save files land in "<patch>_<dd>_<mm>_<yyyy>" under
    // outputDir, with the subfolder name refreshed from the current patch +
    // date whenever this flag is re-applied.
    bool useDatedSubfolder = false;

    GrabPlusModule();
    void process(const ProcessArgs& args) override;
    void onSampleRateChange(const SampleRateChangeEvent& e) override;

    json_t* dataToJson() override;
    void    dataFromJson(json_t* root) override;

    // Propagates useDatedSubfolder to grab/take's `subfolder` fields.
    void updateSubfolder();
    static std::string datedSubfolderName();
};

struct GrabPlusWidget : ModuleWidget {
    GrabPlusWidget(GrabPlusModule* module);
    void step() override;
    void appendContextMenu(Menu* menu) override;
};
