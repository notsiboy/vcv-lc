#pragma once
#include "plugin.hpp"
#include <atomic>
#include <string>
#include <vector>

struct GrabModule : Module {
    enum InputId { IN_L, IN_R, NUM_INPUTS };

    // Menu-configurable, persisted
    float thresholdDb = -65.f;    // open / close threshold
    float hangoverMs  = 250.f;    // silence duration before stop
    float preRollMs   = 100.f;    // pre-trigger buffer
    float fadeInMs    = 0.f;
    float fadeOutMs   = 0.f;
    float maxTakeSec  = 60.f;     // 1 minute default; range up to 300
    bool  normalize   = false;
    int   bitDepth    = 24;       // 16, 24, or 0 (float32)
    std::string prefix    = "grab_";
    std::string outputDir = "";   // empty = default (plugin_dir/test)

    // Live read from UI thread (peak meter / rec LED)
    std::atomic<float> peakL{0.f};
    std::atomic<float> peakR{0.f};
    std::atomic<bool>  recording{false};
    std::atomic<bool>  armed{false};
    std::atomic<bool>  spareNeedsRealloc{false};

    // Internal DSP state
    float sampleRate = 44100.f;
    std::vector<float> preRoll;       // circular, interleaved stereo
    size_t preRollCap    = 0;         // frames
    size_t preRollWrite  = 0;
    size_t preRollFilled = 0;

    std::vector<float> takeBuf;       // active record buffer (interleaved)
    std::vector<float> spareBuf;      // swap-in replacement
    size_t capSamples = 0;            // pre-reserved sample count (interleaved)

    float hangoverSamples = 0.f;      // countdown
    int   armCounter = 0;             // contiguous samples above threshold
    bool  takeStereo = false;

    static constexpr float ARM_GUARD_MS = 3.f;
    static constexpr float MIN_TAKE_MS = 50.f;

    GrabModule();
    void process(const ProcessArgs& args) override;
    void onSampleRateChange(const SampleRateChangeEvent& e) override;

    json_t* dataToJson() override;
    void dataFromJson(json_t* root) override;

    void rebuildBuffers();
    std::string resolveOutputDir() const;
    int scanNextIndex(const std::string& dir) const;
};

struct GrabWidget : ModuleWidget {
    GrabWidget(GrabModule* module);
    void step() override;
    void appendContextMenu(Menu* menu) override;
};
