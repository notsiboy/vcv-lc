#include "Take.hpp"
#include "Theme.hpp"
#include "plugin.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <osdialog.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

// ─── Minimal WAV writer (shared shape with Grab; pulled in per-module for
// now to keep scopes isolated — factor into a helper header later if this
// pattern repeats a third time). ────────────────────────────────────────────

namespace {

inline void w32(std::FILE* f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    std::fwrite(b, 1, 4, f);
}
inline void w16(std::FILE* f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    std::fwrite(b, 1, 2, f);
}

bool writeWavFile(const std::string& path, const float* data, size_t frames,
                  int channels, float sampleRate, int bitDepth) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    uint16_t format   = (bitDepth == 0) ? 3 : 1;
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
    } else {
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

    if (job->normalize) {
        float peak = 0.f;
        for (float s : job->buf) peak = std::max(peak, std::fabs(s));
        if (peak > 1e-9f) { float g = 1.f / peak; for (float& s : job->buf) s *= g; }
    }

    size_t fIn  = (size_t)std::min<double>(job->fadeInMs  * 0.001 * job->sampleRate, (double)frames);
    size_t fOut = (size_t)std::min<double>(job->fadeOutMs * 0.001 * job->sampleRate, (double)frames);
    int ch = job->channels;
    for (size_t i = 0; i < fIn; i++) {
        float g = (float)(i + 1) / (float)fIn;
        for (int c = 0; c < ch; c++) job->buf[i * ch + c] *= g;
    }
    for (size_t i = 0; i < fOut; i++) {
        float g = (float)(fOut - i) / (float)fOut;
        size_t idx = frames - 1 - i;
        for (int c = 0; c < ch; c++) job->buf[idx * ch + c] *= g;
    }

    writeWavFile(job->path, job->buf.data(), frames, job->channels,
                 job->sampleRate, job->bitDepth);
    delete job;
}

} // namespace

// ─── TakeModule ─────────────────────────────────────────────────────────────

TakeModule::TakeModule() {
    config(0, NUM_INPUTS, 0, 0);
    configInput(IN_L, "Left");
    configInput(IN_R, "Right");
    sampleRate = APP->engine->getSampleRate();
    rebuildBuffer();
}

void TakeModule::onSampleRateChange(const SampleRateChangeEvent& e) {
    sampleRate = e.sampleRate;
    rebuildBuffer();
}

void TakeModule::rebuildBuffer() {
    capFrames = std::max<size_t>(1, (size_t)std::ceil(bufferSec * sampleRate));
    buffer.assign(capFrames * 2, 0.f);
    writePos = 0;
    filled   = false;

    // Bins cover the whole buffer duration. 240 bins across ~60s = 250ms per
    // bin. Across 300s = 1.25s per bin — still smooth enough visually.
    samplesPerBin = std::max<size_t>(1, capFrames / N_BINS);
    for (auto& b : bins) { b.l = 0.f; b.r = 0.f; }
    binWriteIdx  = 0;
    samplesInBin = 0;
    curBinPeakL  = 0.f;
    curBinPeakR  = 0.f;
}

json_t* TakeModule::dataToJson() {
    json_t* r = json_object();
    json_object_set_new(r, "bufferSec", json_real(bufferSec));
    json_object_set_new(r, "fadeInMs",  json_real(fadeInMs));
    json_object_set_new(r, "fadeOutMs", json_real(fadeOutMs));
    json_object_set_new(r, "normalize", json_boolean(normalize));
    json_object_set_new(r, "bitDepth",  json_integer(bitDepth));
    json_object_set_new(r, "prefix",    json_string(prefix.c_str()));
    json_object_set_new(r, "outputDir", json_string(outputDir.c_str()));
    return r;
}

void TakeModule::dataFromJson(json_t* root) {
    if (json_t* j = json_object_get(root, "bufferSec")) bufferSec = json_real_value(j);
    if (json_t* j = json_object_get(root, "fadeInMs"))  fadeInMs  = json_real_value(j);
    if (json_t* j = json_object_get(root, "fadeOutMs")) fadeOutMs = json_real_value(j);
    if (json_t* j = json_object_get(root, "normalize")) normalize = json_boolean_value(j);
    if (json_t* j = json_object_get(root, "bitDepth"))  bitDepth  = (int)json_integer_value(j);
    if (json_t* j = json_object_get(root, "prefix"))    prefix    = json_string_value(j);
    if (json_t* j = json_object_get(root, "outputDir")) outputDir = json_string_value(j);
    rebuildBuffer();
}

std::string TakeModule::resolveOutputDir() const {
    if (outputDir.empty()) return asset::plugin(pluginInstance, "test");
    if (outputDir[0] == '/' || (outputDir.size() > 1 && outputDir[1] == ':'))
        return outputDir;
    return asset::plugin(pluginInstance, outputDir);
}

int TakeModule::scanNextIndex(const std::string& dir) const {
    std::string pfx = prefix.empty() ? std::string("take_") : prefix;
    int maxN = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 1;
    while (dirent* ent = readdir(d)) {
        std::string name = ent->d_name;
        if (name.size() < pfx.size() + 4) continue;
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

// ─── DSP ─────────────────────────────────────────────────────────────────────
//
// Every sample: read L/R, write into the ring, update the current bin's peak,
// advance to the next bin when enough samples have accumulated.

void TakeModule::process(const ProcessArgs& args) {
    const bool lCon = inputs[IN_L].isConnected();
    const bool rCon = inputs[IN_R].isConnected();
    float l = lCon ? inputs[IN_L].getVoltage() * 0.1f : 0.f;
    float r = rCon ? inputs[IN_R].getVoltage() * 0.1f : 0.f;
    if (lCon && !rCon) r = l;       // duplicate mono-left into right
    else if (!lCon && rCon) l = r;  // mono-right into left

    // Ring buffer write.
    buffer[writePos * 2 + 0] = l;
    buffer[writePos * 2 + 1] = r;
    writePos++;
    if (writePos >= capFrames) { writePos = 0; filled = true; }

    // Bin accumulation — peak-hold per bin.
    curBinPeakL = std::max(curBinPeakL, std::fabs(l));
    curBinPeakR = std::max(curBinPeakR, std::fabs(r));
    samplesInBin++;
    if (samplesInBin >= samplesPerBin) {
        bins[binWriteIdx].l = curBinPeakL;
        bins[binWriteIdx].r = curBinPeakR;
        binWriteIdx = (binWriteIdx + 1) % N_BINS;
        samplesInBin = 0;
        curBinPeakL = 0.f;
        curBinPeakR = 0.f;
    }
}

// ─── Save ────────────────────────────────────────────────────────────────────

void TakeModule::saveTake() {
    if (capFrames == 0) return;

    // Compose output path.
    std::string dir = resolveOutputDir();
    rack::system::createDirectories(dir);
    int idx = scanNextIndex(dir);
    char name[64];
    std::snprintf(name, sizeof(name), "%s%02d.wav",
                  prefix.empty() ? "take_" : prefix.c_str(), idx);
    std::string path = dir + "/" + name;

    // Copy the ring in chronological order.
    TakeJob* job = new TakeJob();
    job->channels   = 2;
    job->sampleRate = sampleRate;
    job->bitDepth   = bitDepth;
    job->fadeInMs   = fadeInMs;
    job->fadeOutMs  = fadeOutMs;
    job->normalize  = normalize;
    job->path       = path;

    if (!filled) {
        // Only writePos frames have meaningful data; they start at 0.
        job->buf.assign(buffer.begin(), buffer.begin() + writePos * 2);
    } else {
        // Full buffer. Oldest frame is at writePos; walk around.
        job->buf.reserve(capFrames * 2);
        size_t tail = capFrames - writePos;
        // from writePos .. end
        job->buf.insert(job->buf.end(),
                        buffer.begin() + writePos * 2,
                        buffer.begin() + writePos * 2 + tail * 2);
        // from 0 .. writePos
        job->buf.insert(job->buf.end(),
                        buffer.begin(),
                        buffer.begin() + writePos * 2);
    }

    std::thread(&processAndWrite, job).detach();
    saveFlashT.store(rack::system::getTime(), std::memory_order_relaxed);
}

// ─── Widget ──────────────────────────────────────────────────────────────────

namespace {

struct TakeBackground : widget::Widget {
    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, dark ? nvgRGB(0, 0, 0) : nvgRGB(255, 255, 255));
        nvgFill(args.vg);
    }
};

struct TakeLogo : widget::Widget {
    std::string path, darkPath;
    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        std::string use = (dark && !darkPath.empty()) ? darkPath : path;
        auto img = APP->window->loadImage(use);
        if (!img || img->handle < 0) return;
        NVGpaint p = nvgImagePattern(args.vg, 0, 0, box.size.x, box.size.y, 0, img->handle, 1.f);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillPaint(args.vg, p);
        nvgFill(args.vg);
    }
};

struct TakeLabel : widget::Widget {
    std::string text;
    float size = 7.f;
    void draw(const DrawArgs& args) override {
        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, size);
        nvgTextLetterSpacing(args.vg, 0.2f);
        nvgFillColor(args.vg, lc::theme.dark ? nvgRGB(230, 230, 230) : nvgRGB(26, 26, 26));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(args.vg, box.size.x / 2.f, 0.f, text.c_str(), NULL);
    }
};

// Big circular save button. Flashes amber briefly after a successful save.
struct SaveButton : widget::OpaqueWidget {
    TakeModule* tm = nullptr;

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        float cx = box.size.x / 2.f;
        float cy = box.size.y / 2.f;
        float rOuter = std::min(cx, cy) - 0.5f;
        float rInner = rOuter * 0.78f;
        float rCore  = rOuter * 0.50f;

        NVGcolor rim      = dark ? nvgRGB(40, 40, 40)  : nvgRGB(255, 255, 255);
        NVGcolor rimEdge  = dark ? nvgRGB(90, 90, 90)  : nvgRGB(180, 180, 180);
        NVGcolor face     = dark ? nvgRGB(55, 55, 55)  : nvgRGB(240, 240, 240);
        NVGcolor faceEdge = dark ? nvgRGB(75, 75, 75)  : nvgRGB(210, 210, 210);
        NVGcolor idle     = dark ? nvgRGB(95, 95, 95)  : nvgRGB(170, 170, 170);
        NVGcolor flash    = nvgRGB(240, 150, 40);

        double now = rack::system::getTime();
        double t0  = tm ? tm->saveFlashT.load(std::memory_order_relaxed) : -1.0;
        float flashA = 0.f;
        if (t0 > 0 && now - t0 < 0.6) flashA = 1.f - (float)((now - t0) / 0.6);

        // Bezel
        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rOuter);
        nvgFillColor(args.vg, rim); nvgFill(args.vg);
        nvgStrokeColor(args.vg, rimEdge); nvgStrokeWidth(args.vg, 0.6f); nvgStroke(args.vg);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rInner);
        nvgFillColor(args.vg, face); nvgFill(args.vg);
        nvgStrokeColor(args.vg, faceEdge); nvgStrokeWidth(args.vg, 0.4f); nvgStroke(args.vg);

        // Core dot — idle grey, amber-flashed after save.
        NVGcolor core;
        if (flashA > 0.f) {
            core.r = idle.r + (flash.r - idle.r) * flashA;
            core.g = idle.g + (flash.g - idle.g) * flashA;
            core.b = idle.b + (flash.b - idle.b) * flashA;
            core.a = 1.f;
        } else core = idle;
        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rCore);
        nvgFillColor(args.vg, core); nvgFill(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && tm) {
            double now = rack::system::getTime();
            double t0  = tm->saveFlashT.load(std::memory_order_relaxed);
            if (t0 > 0 && now - t0 < 0.6) {
                float a = 1.f - (float)((now - t0) / 0.6);
                float cx = box.size.x / 2.f;
                float cy = box.size.y / 2.f;
                float r  = std::min(cx, cy);
                NVGcolor c = nvgRGB(240, 150, 40);
                NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, r * 0.6f, r * 2.8f,
                    nvgRGBAf(c.r, c.g, c.b, 0.45f * a),
                    nvgRGBAf(c.r, c.g, c.b, 0.f));
                nvgBeginPath(args.vg);
                nvgRect(args.vg, -r * 2.5f, -r * 2.5f, box.size.x + r * 5.f, box.size.y + r * 5.f);
                nvgFillPaint(args.vg, glow);
                nvgFill(args.vg);
            }
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && tm) {
            tm->saveTake();
            e.consume(this);
            return;
        }
        OpaqueWidget::onButton(e);
    }
};

// Vertical waveform. Newest bin at the top, oldest at the bottom. Each bin
// renders as a short horizontal line whose length scales with the stereo
// peak; we extend symmetrically out from the centre so L and R contribute to
// a single centred silhouette (classic voice-note look).
struct WaveformStrip : widget::Widget {
    TakeModule* tm = nullptr;

    void draw(const DrawArgs& args) override {
        if (!tm) return;
        bool dark = lc::theme.dark;

        float w = box.size.x;
        float h = box.size.y;
        float cx = w / 2.f;

        NVGcolor gutter = dark ? nvgRGB(18, 18, 18) : nvgRGB(238, 238, 238);
        NVGcolor rule   = dark ? nvgRGB(40, 40, 40) : nvgRGB(210, 210, 210);
        NVGcolor stroke = dark ? nvgRGB(220, 220, 220) : nvgRGB(40, 40, 40);

        // Scissor to our bounds — cheap safety against any rendering overflow
        // from fractional bin heights.
        nvgSave(args.vg);
        nvgScissor(args.vg, 0, 0, w, h);

        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, w, h);
        nvgFillColor(args.vg, gutter);
        nvgFill(args.vg);

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, cx, 0);
        nvgLineTo(args.vg, cx, h);
        nvgStrokeColor(args.vg, rule);
        nvgStrokeWidth(args.vg, 0.5f);
        nvgStroke(args.vg);

        // Cap visible bins to what fits at ≥1px each — otherwise bins overflow
        // the container when N_BINS × 1px exceeds h.
        int maxBins = (int)std::floor(h);
        int n = std::min((int)TakeModule::N_BINS, maxBins);
        if (n < 1) { nvgResetScissor(args.vg); nvgRestore(args.vg); return; }
        float binPx = h / (float)n;

        int writeIdx = (int)tm->binWriteIdx;
        int total = (int)TakeModule::N_BINS;
        for (int i = 0; i < n; i++) {
            int b = (writeIdx - 1 - i + total * 2) % total;
            float peakL = tm->bins[b].l;
            float peakR = tm->bins[b].r;
            float peak = std::max(peakL, peakR);
            if (peak < 0.001f) continue;

            float y = (float)i * binPx + binPx * 0.5f;
            float len = std::min(peak, 1.f) * (w * 0.45f);

            nvgBeginPath(args.vg);
            nvgMoveTo(args.vg, cx - len, y);
            nvgLineTo(args.vg, cx + len, y);
            nvgStrokeColor(args.vg, stroke);
            nvgStrokeWidth(args.vg, std::max(0.8f, binPx));
            nvgStroke(args.vg);
        }

        nvgResetScissor(args.vg);
        nvgRestore(args.vg);
    }
};

} // namespace

TakeWidget::TakeWidget(TakeModule* module) {
    setModule(module);
    box.size = math::Vec(RACK_GRID_WIDTH * 4, RACK_GRID_HEIGHT);

    TakeBackground* bg = new TakeBackground;
    bg->box.size = box.size;
    addChild(bg);

    float screwX = (box.size.x - RACK_GRID_WIDTH) / 2.f;
    addChild(createWidget<ScrewBlack>(math::Vec(screwX, 0)));
    addChild(createWidget<ScrewBlack>(math::Vec(screwX, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    app::PanelBorder* border = new app::PanelBorder;
    border->box.size = box.size;
    addChild(border);

    // "take" label
    {
        TakeLabel* l = new TakeLabel;
        l->text = "take";
        l->box.size = math::Vec(box.size.x, mm2px(3.f));
        l->box.pos  = math::Vec(0, mm2px(9.f));
        addChild(l);
    }

    // Save button
    {
        SaveButton* b = new SaveButton;
        b->tm = module;
        b->box.size = mm2px(math::Vec(5.f, 5.f));
        b->box.pos  = math::Vec((box.size.x - b->box.size.x) / 2.f, mm2px(14.f));
        addChild(b);
    }

    // Vertical waveform strip. Newest at the top.
    {
        WaveformStrip* w = new WaveformStrip;
        w->tm = module;
        w->box.size = mm2px(math::Vec(12.f, 58.f));
        w->box.pos  = math::Vec((box.size.x - w->box.size.x) / 2.f, mm2px(22.f));
        addChild(w);
    }

    // L input + label
    {
        TakeLabel* l = new TakeLabel;
        l->text = "L";
        l->size = 11.f;
        l->box.size = math::Vec(box.size.x, mm2px(4.f));
        l->box.pos  = math::Vec(0, mm2px(84.f));
        addChild(l);
        addInput(createInputCentered<ThemedPJ301MPort>(
            mm2px(math::Vec(box.size.x / mm2px(1.f) / 2.f, 92.f)), module, TakeModule::IN_L));
    }
    // R input + label
    {
        TakeLabel* l = new TakeLabel;
        l->text = "R";
        l->size = 11.f;
        l->box.size = math::Vec(box.size.x, mm2px(4.f));
        l->box.pos  = math::Vec(0, mm2px(97.f));
        addChild(l);
        addInput(createInputCentered<ThemedPJ301MPort>(
            mm2px(math::Vec(box.size.x / mm2px(1.f) / 2.f, 105.f)), module, TakeModule::IN_R));
    }

    // Logo
    {
        TakeLogo* lg = new TakeLogo;
        lg->path     = asset::plugin(pluginInstance, "res/lc-icon-new.png");
        lg->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");
        lg->box.size = mm2px(math::Vec(9.f, 9.f));
        lg->box.pos  = math::Vec((box.size.x - lg->box.size.x) / 2.f,
                                 mm2px(128.5f - 8.f - 9.f));
        addChild(lg);
    }
}

// ─── Menu ───────────────────────────────────────────────────────────────────

namespace {

struct RangedQuantity : Quantity {
    float* ptr;
    std::string label, unit;
    float minV, maxV, defV;
    int precision;
    RangedQuantity(float* p, std::string l, std::string u, float mn, float mx, float dv, int pr = 1)
        : ptr(p), label(std::move(l)), unit(std::move(u)), minV(mn), maxV(mx), defV(dv), precision(pr) {}
    void setValue(float v) override { *ptr = math::clamp(v, minV, maxV); }
    float getValue() override { return *ptr; }
    float getMinValue() override { return minV; }
    float getMaxValue() override { return maxV; }
    float getDefaultValue() override { return defV; }
    std::string getLabel() override { return label; }
    std::string getUnit() override { return unit; }
    int getDisplayPrecision() override { return precision; }
    std::string getDisplayValueString() override {
        return string::f("%.*f", precision, getDisplayValue());
    }
};

struct RangedSlider : ui::Slider {
    RangedSlider(Quantity* q) { quantity = q; box.size.x = 200.f; }
    ~RangedSlider() override { delete quantity; }
};

struct RebuildQuantity : RangedQuantity {
    TakeModule* tm;
    RebuildQuantity(TakeModule* t, float* p, std::string l, std::string u,
                    float mn, float mx, float dv, int pr = 0)
        : RangedQuantity(p, std::move(l), std::move(u), mn, mx, dv, pr), tm(t) {}
    void setValue(float v) override {
        float c = math::clamp(v, minV, maxV);
        if (c != *ptr) { *ptr = c; if (tm) tm->rebuildBuffer(); }
    }
};

struct PrefixField : ui::TextField {
    TakeModule* tm = nullptr;
    PrefixField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (tm) tm->prefix = text;
    }
};

struct DirField : ui::TextField {
    TakeModule* tm = nullptr;
    DirField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (tm) tm->outputDir = text;
    }
};

struct BitDepthMenu : MenuItem {
    TakeModule* tm;
    Menu* createChildMenu() override {
        Menu* m = new Menu;
        auto add = [&](const std::string& name, int v) {
            m->addChild(createMenuItem(name, CHECKMARK(tm->bitDepth == v),
                                       [this, v]() { tm->bitDepth = v; }));
        };
        add("16-bit PCM", 16);
        add("24-bit PCM", 24);
        add("32-bit float", 0);
        return m;
    }
};

} // namespace

void TakeWidget::appendContextMenu(Menu* menu) {
    TakeModule* tm = dynamic_cast<TakeModule*>(module);
    if (!tm) return;

    menu->addChild(new MenuSeparator);

    menu->addChild(new RangedSlider(new RebuildQuantity(
        tm, &tm->bufferSec, "Buffer length", " s", 10.f, 300.f, 60.f, 0)));
    menu->addChild(new RangedSlider(new RangedQuantity(
        &tm->fadeInMs,  "Fade in",  " ms", 0.f, 50.f, 0.f, 1)));
    menu->addChild(new RangedSlider(new RangedQuantity(
        &tm->fadeOutMs, "Fade out", " ms", 0.f, 50.f, 0.f, 1)));

    menu->addChild(new MenuSeparator);

    menu->addChild(createBoolPtrMenuItem("Normalize to 0 dB", "", &tm->normalize));
    BitDepthMenu* bd = createMenuItem<BitDepthMenu>("Bit depth",
        std::string(tm->bitDepth == 0 ? "float32" :
                    tm->bitDepth == 16 ? "16-bit" : "24-bit") + "  " + RIGHT_ARROW);
    bd->tm = tm;
    menu->addChild(bd);

    menu->addChild(new MenuSeparator);

    menu->addChild(createMenuLabel("Filename prefix"));
    PrefixField* pf = new PrefixField; pf->tm = tm; pf->text = tm->prefix;
    menu->addChild(pf);

    menu->addChild(createMenuLabel("Output directory"));
    DirField* df = new DirField; df->tm = tm; df->text = tm->outputDir;
    menu->addChild(df);

    menu->addChild(createMenuItem("Choose folder…", "", [tm]() {
        char* p = osdialog_file(OSDIALOG_OPEN_DIR, nullptr, nullptr, nullptr);
        if (p) { tm->outputDir = p; std::free(p); }
    }));
    menu->addChild(createMenuItem("Reveal output folder", "", [tm]() {
        std::string d = tm->resolveOutputDir();
        rack::system::createDirectories(d);
        rack::system::openDirectory(d);
    }));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Dark mode (shared)",
        CHECKMARK(lc::theme.dark), []() {
            lc::theme.dark = !lc::theme.dark;
            lc::saveTheme();
        }));
}

Model* modelTake = createModel<TakeModule, TakeWidget>("take");
