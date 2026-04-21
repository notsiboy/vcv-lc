#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// capture — one-click PNG of the visible rack, plus a scan button that writes
// a markdown dependency inventory of the current patch (plugins + versions,
// optionally copied to the clipboard).
//
// PNG uses glReadPixels off the GL front buffer, so the output is the exact
// pixels that were on screen when you clicked. High-DPI native.
//
// Files: <prefix><NN>.(png|md), same auto-indexing / output-dir conventions
// as grab and take.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <rack.hpp>
#include <string>

using namespace rack;

struct CaptureModule : Module {
    // Menu-configurable, persisted.
    std::string prefix         = "capture_";   // .png filenames
    std::string scanPrefix     = "scan_";      // .md filenames
    std::string outputDir      = "";
    bool        hideSelf       = true;    // hide our own panel during the shot
    bool        viewportOnly   = true;    // capture rack viewport vs whole window
    bool        fitAll         = true;    // zoom out to fit the whole rack before shooting
    bool        copyClipboard  = true;    // also drop the scan report on the clipboard

    // Runtime (UI thread only).
    enum Stage : int { IDLE = 0, SETTLE = 1, ARMED = 2, SHOOT = 3 };
    std::atomic<int>    stage{IDLE};
    std::atomic<int>    settleCountdown{0};
    std::atomic<double> flashT{-1.0};     // capture button flash
    std::atomic<double> scanFlashT{-1.0}; // scan button flash

    CaptureModule();
    json_t* dataToJson() override;
    void    dataFromJson(json_t* root) override;

    std::string resolveOutputDir() const;
    int         nextIndex(const std::string& dir,
                          const std::string& pfx,
                          const std::string& ext) const;
    void        runScan();
};

struct CaptureWidget : ModuleWidget {
    CaptureModule* cm = nullptr;

    CaptureWidget(CaptureModule* module);
    void step() override;
    void draw(const DrawArgs& args) override;
    void appendContextMenu(Menu* menu) override;

    // The actual screen grab. Runs on the UI thread (GL context must be
    // current). Spawns a detached thread to do the PNG encoding + disk
    // write so we don't stall the frame loop.
    void performCapture();
};
