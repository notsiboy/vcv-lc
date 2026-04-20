#include "Grab.hpp"
#include "Theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <thread>

// ─── Persistent config ──────────────────────────────────────────────────────

GrabModule::GrabModule() {
    config(0, NUM_INPUTS, 0, 0);
    configInput(IN_L, "Left");
    configInput(IN_R, "Right");
    sampleRate = APP->engine->getSampleRate();
    rebuildBuffers();
}

void GrabModule::onSampleRateChange(const SampleRateChangeEvent& e) {
    sampleRate = e.sampleRate;
    rebuildBuffers();
}

void GrabModule::rebuildBuffers() {
    // Pre-roll: stereo interleaved, sized for preRollMs
    size_t preFrames = (size_t)std::ceil(preRollMs * 0.001f * sampleRate);
    preRollCap = std::max<size_t>(preFrames, 1);
    preRoll.assign(preRollCap * 2, 0.f);
    preRollWrite = 0;
    preRollFilled = 0;

    // Take buffer: worst-case stereo interleaved
    capSamples = (size_t)std::ceil(maxTakeSec * sampleRate) * 2;
    takeBuf.clear();
    spareBuf.clear();
    takeBuf.reserve(capSamples);
    spareBuf.reserve(capSamples);
    spareNeedsRealloc.store(false);

    hangoverSamples = 0.f;
    recording.store(false);
    takeStereo = false;
}

json_t* GrabModule::dataToJson() {
    json_t* root = json_object();
    json_object_set_new(root, "thresholdDb", json_real(thresholdDb));
    json_object_set_new(root, "hangoverMs",  json_real(hangoverMs));
    json_object_set_new(root, "preRollMs",   json_real(preRollMs));
    json_object_set_new(root, "fadeInMs",    json_real(fadeInMs));
    json_object_set_new(root, "fadeOutMs",   json_real(fadeOutMs));
    json_object_set_new(root, "maxTakeSec",  json_real(maxTakeSec));
    json_object_set_new(root, "normalize",   json_boolean(normalize));
    json_object_set_new(root, "bitDepth",    json_integer(bitDepth));
    json_object_set_new(root, "prefix",      json_string(prefix.c_str()));
    json_object_set_new(root, "outputDir",   json_string(outputDir.c_str()));
    json_object_set_new(root, "armed",       json_boolean(armed.load()));
    return root;
}

void GrabModule::dataFromJson(json_t* root) {
    if (json_t* j = json_object_get(root, "thresholdDb")) thresholdDb = json_real_value(j);
    if (json_t* j = json_object_get(root, "hangoverMs"))  hangoverMs  = json_real_value(j);
    if (json_t* j = json_object_get(root, "preRollMs"))   preRollMs   = json_real_value(j);
    if (json_t* j = json_object_get(root, "fadeInMs"))    fadeInMs    = json_real_value(j);
    if (json_t* j = json_object_get(root, "fadeOutMs"))   fadeOutMs   = json_real_value(j);
    if (json_t* j = json_object_get(root, "maxTakeSec"))  maxTakeSec  = json_real_value(j);
    if (json_t* j = json_object_get(root, "normalize"))   normalize   = json_boolean_value(j);
    if (json_t* j = json_object_get(root, "bitDepth"))    bitDepth    = (int)json_integer_value(j);
    if (json_t* j = json_object_get(root, "prefix"))      prefix      = json_string_value(j);
    if (json_t* j = json_object_get(root, "outputDir"))   outputDir   = json_string_value(j);
    if (json_t* j = json_object_get(root, "armed"))       armed.store(json_boolean_value(j));
    rebuildBuffers();
}

// ─── Output-path resolution ─────────────────────────────────────────────────

std::string GrabModule::resolveOutputDir() const {
    if (outputDir.empty())
        return asset::plugin(pluginInstance, "test");
    // Absolute path? use as-is. Otherwise resolve relative to plugin dir.
    if (!outputDir.empty() && (outputDir[0] == '/' ||
        (outputDir.size() > 1 && outputDir[1] == ':')))
        return outputDir;
    return asset::plugin(pluginInstance, outputDir);
}

int GrabModule::scanNextIndex(const std::string& dir) const {
    std::string pfx = prefix.empty() ? std::string("grab_") : prefix;
    int maxN = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 1;
    while (dirent* ent = readdir(d)) {
        std::string name = ent->d_name;
        if (name.size() < pfx.size() + 4) continue;                 // prefix + NN + .wav
        if (name.compare(0, pfx.size(), pfx) != 0) continue;
        size_t dot = name.rfind('.');
        if (dot == std::string::npos) continue;
        std::string ext = name.substr(dot);
        if (ext != ".wav" && ext != ".WAV") continue;
        std::string numStr = name.substr(pfx.size(), dot - pfx.size());
        if (numStr.empty()) continue;
        bool allDigits = true;
        for (char c : numStr) if (!std::isdigit((unsigned char)c)) { allDigits = false; break; }
        if (!allDigits) continue;
        int n = std::atoi(numStr.c_str());
        if (n > maxN) maxN = n;
    }
    closedir(d);
    return maxN + 1;
}

// ─── WAV writer (minimal, PCM16 / PCM24 / float32) ──────────────────────────

namespace {

inline void w32(std::FILE* f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    std::fwrite(b, 1, 4, f);
}
inline void w16(std::FILE* f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    std::fwrite(b, 1, 2, f);
}

bool writeWavFile(const std::string& path, const float* data, size_t frames, int channels,
                  float sampleRate, int bitDepth) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    uint16_t format   = (bitDepth == 0) ? 3 : 1; // 3 = IEEE float
    uint16_t bits     = (bitDepth == 0) ? 32 : (uint16_t)bitDepth;
    uint16_t blockAlg = (uint16_t)(channels * bits / 8);
    uint32_t byteRate = (uint32_t)(sampleRate * blockAlg);
    uint32_t dataSz   = (uint32_t)(frames * blockAlg);
    uint32_t riffSz   = 4 + (8 + 16) + (8 + dataSz);

    std::fwrite("RIFF", 1, 4, f); w32(f, riffSz); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(f, 16);
    w16(f, format); w16(f, (uint16_t)channels);
    w32(f, (uint32_t)sampleRate); w32(f, byteRate);
    w16(f, blockAlg); w16(f, bits);
    std::fwrite("data", 1, 4, f); w32(f, dataSz);

    const size_t n = frames * channels;
    if (bitDepth == 0) {
        std::fwrite(data, sizeof(float), n, f);
    } else if (bitDepth == 16) {
        std::vector<int16_t> buf(n);
        for (size_t i = 0; i < n; i++) {
            float s = std::max(-1.f, std::min(1.f, data[i]));
            buf[i] = (int16_t)std::lrint(s * 32767.f);
        }
        std::fwrite(buf.data(), sizeof(int16_t), n, f);
    } else { // 24-bit PCM
        std::vector<uint8_t> buf(n * 3);
        for (size_t i = 0; i < n; i++) {
            float s = std::max(-1.f, std::min(1.f, data[i]));
            int32_t v = (int32_t)std::lrint(s * 8388607.f);
            buf[i*3+0] = (uint8_t)(v & 0xFF);
            buf[i*3+1] = (uint8_t)((v >> 8) & 0xFF);
            buf[i*3+2] = (uint8_t)((v >> 16) & 0xFF);
        }
        std::fwrite(buf.data(), 1, buf.size(), f);
    }
    std::fclose(f);
    return true;
}

struct TakeJob {
    std::vector<float> buf;
    int channels;
    float sampleRate;
    int bitDepth;
    float fadeInMs;
    float fadeOutMs;
    bool normalize;
    std::string path;
};

void processAndWrite(TakeJob* job) {
    const size_t frames = job->buf.size() / job->channels;
    if (frames == 0) { delete job; return; }

    // Normalize to 0 dBFS
    if (job->normalize) {
        float peak = 0.f;
        for (float s : job->buf) peak = std::max(peak, std::fabs(s));
        if (peak > 1e-9f) {
            float g = 1.f / peak;
            for (float& s : job->buf) s *= g;
        }
    }

    // Fades
    size_t fadeInF  = (size_t)std::min<double>(job->fadeInMs  * 0.001 * job->sampleRate, (double)frames);
    size_t fadeOutF = (size_t)std::min<double>(job->fadeOutMs * 0.001 * job->sampleRate, (double)frames);
    int ch = job->channels;
    for (size_t i = 0; i < fadeInF; i++) {
        float g = (float)(i + 1) / (float)fadeInF;
        for (int c = 0; c < ch; c++) job->buf[i * ch + c] *= g;
    }
    for (size_t i = 0; i < fadeOutF; i++) {
        float g = (float)(fadeOutF - i) / (float)fadeOutF;
        size_t idx = frames - 1 - i;
        for (int c = 0; c < ch; c++) job->buf[idx * ch + c] *= g;
    }

    writeWavFile(job->path, job->buf.data(), frames, job->channels, job->sampleRate, job->bitDepth);
    delete job;
}

} // namespace

// ─── DSP ─────────────────────────────────────────────────────────────────────

void GrabModule::process(const ProcessArgs& args) {
    const bool lConnected = inputs[IN_L].isConnected();
    const bool rConnected = inputs[IN_R].isConnected();
    const bool stereo = lConnected && rConnected;

    // Read — normalize 10 V range to ±1 for file; many sources exceed this but clip at write.
    float l = lConnected ? inputs[IN_L].getVoltage() * 0.1f : 0.f;
    float r = rConnected ? inputs[IN_R].getVoltage() * 0.1f : 0.f;
    if (!stereo && !lConnected && rConnected) { l = r; }

    float mag = std::max(std::fabs(l), std::fabs(r));

    // Peak meter (atomic decay for UI).
    float pL = peakL.load(std::memory_order_relaxed);
    float pR = peakR.load(std::memory_order_relaxed);
    float decay = std::exp(-1.f / (args.sampleRate * 0.25f));
    pL = std::max(std::fabs(l), pL * decay);
    pR = std::max(std::fabs(r), pR * decay);
    peakL.store(pL, std::memory_order_relaxed);
    peakR.store(pR, std::memory_order_relaxed);

    // Pre-roll circular write (always active).
    const int preCh = 2; // always keep stereo pre-roll, downmix on commit
    size_t pi = preRollWrite * preCh;
    preRoll[pi + 0] = l;
    preRoll[pi + 1] = r;
    preRollWrite = (preRollWrite + 1) % preRollCap;
    if (preRollFilled < preRollCap) preRollFilled++;

    const float threshLin = std::pow(10.f, thresholdDb / 20.f);
    const bool signalAbove = mag > threshLin;

    const bool isArmed = armed.load(std::memory_order_relaxed);

    if (!recording.load(std::memory_order_relaxed)) {
        if (!isArmed)                    { armCounter = 0; return; }
        if (!(lConnected || rConnected)) { armCounter = 0; return; }
        // Require N contiguous samples above threshold to reject single-sample clicks.
        if (signalAbove) armCounter++;
        else             armCounter = 0;
        int armNeeded = (int)std::ceil(ARM_GUARD_MS * 0.001f * args.sampleRate);
        if (armCounter < armNeeded) return;
        // Start a new take.
        takeStereo = stereo;
        const int tch = takeStereo ? 2 : 1;
        takeBuf.clear();
        // Dump pre-roll (ordered oldest→newest).
        size_t start = (preRollWrite + preRollCap - preRollFilled) % preRollCap;
        for (size_t n = 0; n < preRollFilled; n++) {
            size_t idx = (start + n) % preRollCap;
            float pl = preRoll[idx * preCh + 0];
            float pr = preRoll[idx * preCh + 1];
            if (takeStereo) {
                if (takeBuf.size() + 2 <= capSamples) { takeBuf.push_back(pl); takeBuf.push_back(pr); }
            } else {
                float mono = lConnected ? pl : pr;
                if (takeBuf.size() + 1 <= capSamples) takeBuf.push_back(mono);
            }
            (void)tch;
        }
        hangoverSamples = hangoverMs * 0.001f * args.sampleRate;
        recording.store(true, std::memory_order_relaxed);
    }

    // Recording — append current frame.
    {
        const int tch = takeStereo ? 2 : 1;
        if (takeBuf.size() + tch <= capSamples) {
            if (takeStereo) { takeBuf.push_back(l); takeBuf.push_back(r); }
            else            { takeBuf.push_back(lConnected ? l : r); }
        }

        if (!isArmed)         hangoverSamples = -1.f; // disarm → force stop
        else if (signalAbove) hangoverSamples = hangoverMs * 0.001f * args.sampleRate;
        else                  hangoverSamples -= 1.f;

        const bool atCap   = (takeBuf.size() + tch > capSamples);
        const bool timeout = (hangoverSamples <= 0.f);

        if (atCap || timeout) {
            // Stop recording. Drop takes shorter than MIN_TAKE_MS (probably a click).
            const size_t frames = takeBuf.size() / (size_t)tch;
            const size_t minFrames = (size_t)std::ceil(MIN_TAKE_MS * 0.001f * args.sampleRate);

            if (frames >= minFrames) {
                std::string dir = resolveOutputDir();
                rack::system::createDirectories(dir);
                int idx = scanNextIndex(dir);
                char name[64];
                std::snprintf(name, sizeof(name), "%s%02d.wav",
                              prefix.empty() ? "grab_" : prefix.c_str(), idx);
                std::string path = dir + "/" + name;

                TakeJob* job = new TakeJob();
                job->buf = std::move(takeBuf);         // steals storage
                job->channels   = tch;
                job->sampleRate = args.sampleRate;
                job->bitDepth   = bitDepth;
                job->fadeInMs   = fadeInMs;
                job->fadeOutMs  = fadeOutMs;
                job->normalize  = normalize;
                job->path       = std::move(path);

                std::thread(&processAndWrite, job).detach();

                // Swap in the pre-reserved spare so the next take has capacity.
                std::swap(takeBuf, spareBuf);
                spareNeedsRealloc.store(true, std::memory_order_relaxed);
            } else {
                // Discard in place — keep reserved capacity.
                takeBuf.clear();
            }

            recording.store(false, std::memory_order_relaxed);
            hangoverSamples = 0.f;
            armCounter = 0;
        }
    }
}

// ─── Widget ──────────────────────────────────────────────────────────────────

namespace {

struct GrabBackground : widget::Widget {
    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, dark ? nvgRGB(0, 0, 0) : nvgRGB(255, 255, 255));
        nvgFill(args.vg);
    }
};

struct ArmToggle : widget::OpaqueWidget {
    GrabModule* gm = nullptr;

    bool isOn() const { return gm && gm->armed.load(std::memory_order_relaxed); }

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        float cx = box.size.x / 2.f;
        float cy = box.size.y / 2.f;
        float rOuter = std::min(cx, cy) - 0.5f;
        float rInner = rOuter * 0.78f;
        float rLed   = rOuter * 0.50f;

        NVGcolor rim      = dark ? nvgRGB(40, 40, 40)  : nvgRGB(255, 255, 255);
        NVGcolor rimEdge  = dark ? nvgRGB(90, 90, 90)  : nvgRGB(180, 180, 180);
        NVGcolor face     = dark ? nvgRGB(55, 55, 55)  : nvgRGB(240, 240, 240);
        NVGcolor faceEdge = dark ? nvgRGB(75, 75, 75)  : nvgRGB(210, 210, 210);
        NVGcolor green    = nvgRGB(60, 210, 90);
        NVGcolor red      = nvgRGB(230, 60, 60);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rOuter);
        nvgFillColor(args.vg, rim); nvgFill(args.vg);
        nvgStrokeColor(args.vg, rimEdge); nvgStrokeWidth(args.vg, 0.6f); nvgStroke(args.vg);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rInner);
        nvgFillColor(args.vg, face); nvgFill(args.vg);
        nvgStrokeColor(args.vg, faceEdge); nvgStrokeWidth(args.vg, 0.4f); nvgStroke(args.vg);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rLed);
        nvgFillColor(args.vg, isOn() ? green : red);
        nvgFill(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            float cx = box.size.x / 2.f;
            float cy = box.size.y / 2.f;
            float rOuter = std::min(cx, cy) - 0.5f;
            float rLed   = rOuter * 0.50f;
            NVGcolor c = isOn() ? nvgRGB(60, 210, 90) : nvgRGB(230, 60, 60);
            NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, rLed * 0.6f, rLed * 3.f,
                nvgRGBA((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 140),
                nvgRGBA((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 0));
            nvgBeginPath(args.vg);
            nvgRect(args.vg, cx - rLed * 4, cy - rLed * 4, rLed * 8, rLed * 8);
            nvgFillPaint(args.vg, glow);
            nvgFill(args.vg);
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && gm) {
            gm->armed.store(!gm->armed.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
            e.consume(this);
        }
    }
};

struct PngLogo : widget::Widget {
    std::string path;
    std::string darkPath;
    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        std::string use = (dark && !darkPath.empty()) ? darkPath : path;
        std::shared_ptr<window::Image> img = APP->window->loadImage(use);
        if (!img || img->handle < 0) return;
        NVGpaint paint = nvgImagePattern(args.vg, 0, 0, box.size.x, box.size.y, 0, img->handle, 1.f);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillPaint(args.vg, paint);
        nvgFill(args.vg);
    }
};

struct RecLed : widget::Widget {
    GrabModule* gm = nullptr;
    NVGcolor onColor = nvgRGB(230, 60, 60);
    bool isOn() { return gm && gm->recording.load(std::memory_order_relaxed); }
    void draw(const DrawArgs& args) override {
        float cx = box.size.x / 2.f;
        float cy = box.size.y / 2.f;
        float r  = std::min(cx, cy) - 0.5f;
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, r);
        if (isOn()) nvgFillColor(args.vg, onColor);
        else        nvgFillColor(args.vg, nvgRGB(200, 200, 200));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGB(150, 150, 150));
        nvgStrokeWidth(args.vg, 0.3f);
        nvgStroke(args.vg);
    }
    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && isOn()) {
            float cx = box.size.x / 2.f;
            float cy = box.size.y / 2.f;
            float r  = std::min(cx, cy);
            NVGcolor c = onColor;
            NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, r * 0.8f, r * 2.5f,
                nvgRGBA((int)(c.r*255), (int)(c.g*255), (int)(c.b*255), 180),
                nvgRGBA((int)(c.r*255), (int)(c.g*255), (int)(c.b*255), 0));
            nvgBeginPath(args.vg);
            nvgRect(args.vg, -r * 2, -r * 2, box.size.x + r * 4, box.size.y + r * 4);
            nvgFillPaint(args.vg, glow);
            nvgFill(args.vg);
        }
        Widget::drawLayer(args, layer);
    }
};

struct PeakMeter : widget::Widget {
    GrabModule* gm = nullptr;

    // Thresholds in dB, bottom → top. Top LED = 0 dBFS.
    static constexpr int N_SEG = 8;
    static constexpr float FLOOR_DB = -60.f;
    static constexpr float SEG_DB[N_SEG] = {
        -48.f, -36.f, -24.f, -18.f, -12.f, -6.f, -3.f, 0.f
    };
    // Color zones: 0-4 green, 5-6 yellow, 7 red.
    static NVGcolor segColor(int i) {
        if (i >= 7) return nvgRGB(230, 60, 60);   // red   — 0 dB
        if (i >= 5) return nvgRGB(230, 200, 60);  // yellow — -6, -3
        return nvgRGB(80, 210, 100);              // green — everything below
    }

    static float linToDb(float m) {
        if (m <= 1e-6f) return -1e6f;
        return 20.f * std::log10(m);
    }
    static NVGcolor lerpColor(NVGcolor a, NVGcolor b, float t) {
        return nvgRGBAf(
            a.r + (b.r - a.r) * t,
            a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t);
    }

    // Brightness (0..1) for LED i at the given dB level. Continuous ramp
    // between the previous segment's threshold and this one's, so the highest
    // lit LED smoothly tracks amplitude between steps.
    static float brightness(int i, float db) {
        float T = SEG_DB[i];
        float P = (i == 0) ? FLOOR_DB : SEG_DB[i - 1];
        if (db >= T) return 1.f;
        if (db <= P) return 0.f;
        return (db - P) / (T - P);
    }

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        float w = box.size.x, h = box.size.y;

        float peakLv = gm ? gm->peakL.load(std::memory_order_relaxed) : 0.f;
        float peakRv = gm ? gm->peakR.load(std::memory_order_relaxed) : 0.f;
        float dbL = linToDb(peakLv);
        float dbR = linToDb(peakRv);

        NVGcolor off = dark ? nvgRGB(32, 32, 32) : nvgRGB(225, 225, 225);

        float colW = w * 0.32f;
        float xL   = 0.f;
        float xR   = w - colW;
        float cellH = h / (float)N_SEG;
        float ledD  = std::min(colW, cellH) * 0.70f;

        for (int i = 0; i < N_SEG; i++) {
            int visualRow = N_SEG - 1 - i;
            float cy = visualRow * cellH + cellH * 0.5f;
            NVGcolor on = segColor(i);
            float bL = brightness(i, dbL);
            float bR = brightness(i, dbR);

            auto dot = [&](float x, float b) {
                float cx = x + colW * 0.5f;
                nvgBeginPath(args.vg);
                nvgCircle(args.vg, cx, cy, ledD * 0.5f);
                nvgFillColor(args.vg, lerpColor(off, on, b));
                nvgFill(args.vg);
            };
            dot(xL, bL);
            dot(xR, bR);
        }
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1 || !gm) { Widget::drawLayer(args, layer); return; }

        float w = box.size.x, h = box.size.y;
        float dbL = linToDb(gm->peakL.load(std::memory_order_relaxed));
        float dbR = linToDb(gm->peakR.load(std::memory_order_relaxed));

        float colW = w * 0.32f;
        float xL   = 0.f;
        float xR   = w - colW;
        float cellH = h / (float)N_SEG;
        float ledD  = std::min(colW, cellH) * 0.70f;

        // Glow only the red (top) LED, scaled by its brightness.
        int i = N_SEG - 1;
        int visualRow = 0;
        float cy = visualRow * cellH + cellH * 0.5f;
        NVGcolor red = segColor(i);
        float bL = brightness(i, dbL);
        float bR = brightness(i, dbR);

        auto glow = [&](float x, float b) {
            if (b <= 0.f) return;
            float cx = x + colW * 0.5f;
            float r = ledD * 0.5f;
            int a = (int)(160.f * b);
            NVGpaint g = nvgRadialGradient(args.vg, cx, cy, r * 0.6f, r * 2.4f,
                nvgRGBA((int)(red.r * 255), (int)(red.g * 255), (int)(red.b * 255), a),
                nvgRGBA((int)(red.r * 255), (int)(red.g * 255), (int)(red.b * 255), 0));
            nvgBeginPath(args.vg);
            nvgRect(args.vg, cx - r * 3, cy - r * 3, r * 6, r * 6);
            nvgFillPaint(args.vg, g);
            nvgFill(args.vg);
        };
        glow(xL, bL);
        glow(xR, bR);

        Widget::drawLayer(args, layer);
    }
};

constexpr float PeakMeter::SEG_DB[PeakMeter::N_SEG];

struct JackLabel : widget::Widget {
    std::string text;
    float fontSize = 11.f;
    void draw(const DrawArgs& args) override {
        std::shared_ptr<window::Font> font = APP->window->loadFont(
            asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        bool dark = lc::theme.dark;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, fontSize);
        nvgTextLetterSpacing(args.vg, 0.3f);
        nvgFillColor(args.vg, dark ? nvgRGB(230, 230, 230) : nvgRGB(26, 26, 26));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(args.vg, box.size.x / 2.f, 0.f, text.c_str(), NULL);
    }
};

} // namespace

GrabWidget::GrabWidget(GrabModule* module) {
    setModule(module);
    box.size = Vec(RACK_GRID_WIDTH * 3, RACK_GRID_HEIGHT);

    GrabBackground* bg = new GrabBackground;
    bg->box.size = box.size;
    addChild(bg);

    float screwX = (box.size.x - RACK_GRID_WIDTH) / 2.f;
    addChild(createWidget<ScrewBlack>(Vec(screwX, 0)));
    addChild(createWidget<ScrewBlack>(Vec(screwX, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    // Thin grey outline — matches Rack's default module border.
    app::PanelBorder* border = new app::PanelBorder;
    border->box.size = box.size;
    addChild(border);

    // Layout (top → bottom): "grab" label, arm toggle, peak meter, rec LED, L, R, logo.
    // "grab" label (size matches Tidy's button labels)
    {
        JackLabel* lab = new JackLabel;
        lab->text = "grab";
        lab->fontSize = 7.f;
        lab->box.size = Vec(box.size.x, mm2px(3.f));
        lab->box.pos = Vec(0, mm2px(9.f));
        addChild(lab);
    }

    // Arm toggle — round button with centered red/green LED
    {
        ArmToggle* t = new ArmToggle;
        t->gm = module;
        t->box.size = mm2px(Vec(5.f, 5.f));
        t->box.pos = Vec((box.size.x - t->box.size.x) / 2.f, mm2px(14.f));
        addChild(t);
    }

    // Peak meter (thin vertical LED ladder, tighter than before)
    {
        PeakMeter* m = new PeakMeter;
        m->gm = module;
        m->box.size = mm2px(Vec(4.f, 44.f));
        m->box.pos = Vec((box.size.x - m->box.size.x) / 2.f, mm2px(24.f));
        addChild(m);
    }

    // Red recording LED — under meter, above L label
    {
        RecLed* led = new RecLed;
        led->gm = module;
        led->box.size = mm2px(Vec(2.5f, 2.5f));
        led->box.pos = Vec((box.size.x - led->box.size.x) / 2.f, mm2px(75.5f));
        addChild(led);
    }

    // L label + jack
    {
        JackLabel* lab = new JackLabel;
        lab->text = "L";
        lab->box.size = Vec(box.size.x, mm2px(4.f));
        lab->box.pos = Vec(0, mm2px(84.f));
        addChild(lab);

        addInput(createInputCentered<ThemedPJ301MPort>(
            mm2px(Vec(15.24f / 2.f, 92.f)), module, GrabModule::IN_L));
    }

    // R label + jack
    {
        JackLabel* lab = new JackLabel;
        lab->text = "R";
        lab->box.size = Vec(box.size.x, mm2px(4.f));
        lab->box.pos = Vec(0, mm2px(97.f));
        addChild(lab);

        addInput(createInputCentered<ThemedPJ301MPort>(
            mm2px(Vec(15.24f / 2.f, 105.f)), module, GrabModule::IN_R));
    }

    // Logo (9mm, bottom)
    {
        PngLogo* logo = new PngLogo;
        logo->path     = asset::plugin(pluginInstance, "res/lc-icon-new.png");
        logo->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");
        logo->box.size = mm2px(Vec(9.f, 9.f));
        logo->box.pos  = Vec((box.size.x - logo->box.size.x) / 2.f, mm2px(128.5f - 8.f - 9.f));
        addChild(logo);
    }
}

void GrabWidget::step() {
    ModuleWidget::step();
    if (!module) return;
    GrabModule* gm = dynamic_cast<GrabModule*>(module);
    if (!gm) return;
    // Re-reserve the spare buffer on the UI thread after it was consumed.
    if (gm->spareNeedsRealloc.exchange(false, std::memory_order_relaxed)) {
        gm->spareBuf = std::vector<float>();
        gm->spareBuf.reserve(gm->capSamples);
    }
}

Model* modelGrab = createModel<GrabModule, GrabWidget>("grab");
