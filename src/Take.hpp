#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// take — session-aware retrospective recorder.
//
// Always rolling a stereo ring buffer (default 60s, configurable up to 300s).
// Click the panel button to freeze the last N seconds to disk as a take.
//
// A vertically-scrolling waveform on the panel shows the live contents of the
// buffer — newest audio at the top, oldest at the bottom, voice-memos style.
// Peak bins are derived incrementally on the audio thread at ~240 bins per
// buffer duration so render cost is constant regardless of buffer length.
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <atomic>
#include <rack.hpp>
#include <string>
#include <vector>

using namespace rack;

struct TakeModule : Module {
    enum InputId { IN_L, IN_R, NUM_INPUTS };

    // ── Menu-configurable, persisted ────────────────────────────────────────
    float       bufferSec = 60.f;     // 10–300
    float       fadeInMs  = 0.f;
    float       fadeOutMs = 0.f;
    bool        normalize = false;
    int         bitDepth  = 24;       // 16 / 24 / 0 (= float32)
    std::string prefix    = "take_";
    std::string outputDir = "";
    std::string subfolder = "";  // appended to outputDir when non-empty

    // ── Runtime state ───────────────────────────────────────────────────────
    float  sampleRate = 44100.f;
    std::vector<float> buffer;        // stereo interleaved ring
    size_t capFrames  = 0;
    size_t writePos   = 0;
    bool   filled     = false;        // true once the ring has wrapped

    // ── Peak bins for the waveform display ──────────────────────────────────
    static constexpr size_t N_BINS = 240;
    struct PeakBin { float l = 0.f, r = 0.f; };
    std::array<PeakBin, N_BINS> bins{};
    size_t binWriteIdx    = 0;
    size_t samplesInBin   = 0;
    size_t samplesPerBin  = 1;
    float  curBinPeakL    = 0.f;
    float  curBinPeakR    = 0.f;

    // Lit briefly after a successful save, so the panel button can flash.
    std::atomic<double> saveFlashT{-1.0};

    // When true, the rolling ring freezes during silence — writePos, filled,
    // and bin accumulation all stop advancing. Used by rec+ to implement
    // "snip mode" without TakeModule needing to know about GrabModule.
    std::atomic<bool> snipActive{false};
    float snipThreshDb   = -60.f;  // below this counts as silence
    float snipHangoverMs = 100.f;  // debounce — continuous silence required before freeze

    // Snip runtime state (audio thread only).
    float snipSilenceSamples = 0.f;
    bool  snipFrozen = false;

    TakeModule();
    void process(const ProcessArgs& args) override;
    void onSampleRateChange(const SampleRateChangeEvent& e) override;

    json_t* dataToJson() override;
    void    dataFromJson(json_t* root) override;

    // ── Actions ─────────────────────────────────────────────────────────────
    void rebuildBuffer();

    // Snapshot the ring buffer and spawn a background writer thread.
    // Called from the UI thread (on button click). The audio thread keeps
    // writing while we copy; for stereo float samples that's benign (at most
    // a torn sample at the cursor, inaudible in a 60-second file).
    void saveTake();

    std::string resolveOutputDir() const;
    int         scanNextIndex(const std::string& dir) const;
};

struct TakeWidget : ModuleWidget {
    TakeWidget(TakeModule* module);
    void appendContextMenu(Menu* menu) override;
};
