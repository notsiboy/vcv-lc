#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// scan — dependency inventory for the current patch.
//
// Walks every module in the rack, groups by plugin, writes a markdown file
// with the plugin list + versions + per-module counts. Lets anyone you send
// the .vcv file to know exactly which plugins to install. Rack buries this
// information in the patch JSON as slugs only — scan surfaces it in a
// readable form plus optionally copies it to the clipboard.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <rack.hpp>
#include <string>

using namespace rack;

struct ScanModule : Module {
    std::string prefix     = "scan_";
    std::string outputDir  = "";
    bool        copyClipboard = true;   // also drop the report on the clipboard

    std::atomic<double> flashT{-1.0};
    std::string         lastSummary;    // e.g. "42 modules · 8 plugins"

    ScanModule();
    json_t* dataToJson() override;
    void    dataFromJson(json_t* root) override;

    std::string resolveOutputDir() const;
    int         scanNextIndex(const std::string& dir) const;

    // Called on button click. Synchronous — the walk is fast, writing the
    // markdown is tiny. No threading needed.
    void runScan();
};

struct ScanWidget : ModuleWidget {
    ScanWidget(ScanModule* module);
    void appendContextMenu(Menu* menu) override;
};
