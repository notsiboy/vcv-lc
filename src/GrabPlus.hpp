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

    // When true (default), save files land in a dated patch folder
    // "<patch>_<dd>_<mm>_<yyyy>/<type>/" under outputDir. When off, files
    // still route through per-type subfolders (grabs / recs / takes) so
    // the three flavours never mix in one directory.
    bool useDatedSubfolder = true;

    // Cache of the last patch path we resolved subfolder names off. When it
    // changes between ticks, refresh the subfolder + filename strings.
    std::string cachedPatchPath;

    GrabPlusModule();
    void process(const ProcessArgs& args) override;
    void onSampleRateChange(const SampleRateChangeEvent& e) override;

    json_t* dataToJson() override;
    void    dataFromJson(json_t* root) override;

    // Recomputes grab/take subfolders + filename prefixes from the current
    // patch name, date, and useDatedSubfolder flag.
    void updateSubfolder();
    static std::string datedSubfolderName();
    static std::string currentPatchName();

    // Tiny cross-module persistence for "last chosen output directory" so a
    // freshly-inserted grab+ adopts wherever the user saved to last time.
    static std::string loadLastOutputDir();
    static void        saveLastOutputDir(const std::string& dir);

    // Apply an output-dir change: updates both inner modules and persists
    // the last-dir file.
    void setOutputDir(const std::string& dir);
};

struct GrabPlusWidget : ModuleWidget {
    GrabPlusWidget(GrabPlusModule* module);
    void step() override;
    void appendContextMenu(Menu* menu) override;
};
