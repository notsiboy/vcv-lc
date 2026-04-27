#include "Capture.hpp"
#include "Theme.hpp"
#include "plugin.hpp"
#include <patch.hpp>

#include <GL/glew.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <map>
#include <osdialog.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

// Number of frames we wait between changing the view (fit-all zoom) and
// reading pixels. One frame is enough for widgets that redraw immediately,
// but plenty of third-party modules cache panels in framebuffer widgets that
// only re-render after a few step/draw ticks. At 60 fps, 18 frames ≈ 300 ms.
static constexpr int SETTLE_FRAMES = 18;

// ─── Module ─────────────────────────────────────────────────────────────────

CaptureModule::CaptureModule() {
    config(0, 0, 0, 0);
}

json_t* CaptureModule::dataToJson() {
    json_t* r = json_object();
    json_object_set_new(r, "prefix",        json_string(prefix.c_str()));
    json_object_set_new(r, "scanPrefix",    json_string(scanPrefix.c_str()));
    json_object_set_new(r, "outputDir",     json_string(outputDir.c_str()));
    json_object_set_new(r, "hideSelf",      json_boolean(hideSelf));
    json_object_set_new(r, "viewportOnly",  json_boolean(viewportOnly));
    json_object_set_new(r, "fitAll",        json_boolean(fitAll));
    json_object_set_new(r, "copyClipboard", json_boolean(copyClipboard));
    return r;
}

void CaptureModule::dataFromJson(json_t* root) {
    if (json_t* j = json_object_get(root, "prefix"))        prefix        = json_string_value(j);
    if (json_t* j = json_object_get(root, "scanPrefix"))    scanPrefix    = json_string_value(j);
    if (json_t* j = json_object_get(root, "outputDir"))     outputDir     = json_string_value(j);
    if (json_t* j = json_object_get(root, "hideSelf"))      hideSelf      = json_boolean_value(j);
    if (json_t* j = json_object_get(root, "viewportOnly"))  viewportOnly  = json_boolean_value(j);
    if (json_t* j = json_object_get(root, "fitAll"))        fitAll        = json_boolean_value(j);
    if (json_t* j = json_object_get(root, "copyClipboard")) copyClipboard = json_boolean_value(j);
}

std::string CaptureModule::resolveOutputDir() const {
    if (outputDir.empty()) return asset::plugin(pluginInstance, "test");
    if (outputDir[0] == '/' || (outputDir.size() > 1 && outputDir[1] == ':'))
        return outputDir;
    return asset::plugin(pluginInstance, outputDir);
}

// Lowercase the extension for a case-insensitive compare against on-disk
// filenames. Accepts ".png", ".md", etc.
int CaptureModule::nextIndex(const std::string& dir,
                             const std::string& pfx,
                             const std::string& ext) const {
    int maxN = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 1;
    while (dirent* ent = readdir(d)) {
        std::string name = ent->d_name;
        if (name.size() < pfx.size() + ext.size()) continue;
        if (name.compare(0, pfx.size(), pfx) != 0) continue;
        size_t dot = name.rfind('.');
        if (dot == std::string::npos) continue;
        std::string e = name.substr(dot);
        for (char& c : e) c = (char)std::tolower((unsigned char)c);
        if (e != ext) continue;
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

// ─── Writer thread ──────────────────────────────────────────────────────────

namespace {

struct CaptureJob {
    std::vector<uint8_t> pixels;   // RGBA, top-down after vertical flip
    int w = 0;
    int h = 0;
    std::string path;
};

void writePng(CaptureJob* job) {
    if (!job->pixels.empty() && job->w > 0 && job->h > 0) {
        stbi_write_png(job->path.c_str(), job->w, job->h, 4,
                       job->pixels.data(), job->w * 4);
    }
    delete job;
}

// ─── Scan helpers ───────────────────────────────────────────────────────────

struct PluginBucket {
    const rack::plugin::Plugin* plugin = nullptr;
    std::map<std::string, int>  moduleCounts;   // slug → count
};

std::string nowStamp() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::tm* lt = std::localtime(&t);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", lt);
    return buf;
}

} // namespace

// Walks every module in the rack, groups by plugin, writes a markdown report
// with plugin names + versions + per-module counts. Optionally copies the
// report to the clipboard. Synchronous — the walk is fast.
std::string CaptureModule::buildScanMarkdown() const {
    if (!APP || !APP->scene || !APP->scene->rack) return std::string();

    std::map<std::string, PluginBucket> byPlugin;
    int totalModules  = 0;
    int missingPlugin = 0;

    for (ModuleWidget* mw : APP->scene->rack->getModules()) {
        if (!mw || !mw->module || !mw->model) continue;
        totalModules++;
        const rack::plugin::Plugin* p = mw->model->plugin;
        if (!p) { missingPlugin++; continue; }
        auto& b = byPlugin[p->slug];
        b.plugin = p;
        b.moduleCounts[mw->model->slug]++;
    }

    std::string md;
    md += "# scan · " + nowStamp() + "\n\n";
    md += std::to_string(totalModules) + " modules · "
       +  std::to_string((int)byPlugin.size()) + " plugins\n\n";

    std::vector<const PluginBucket*> ordered;
    ordered.reserve(byPlugin.size());
    for (auto& kv : byPlugin) ordered.push_back(&kv.second);
    std::sort(ordered.begin(), ordered.end(),
              [](const PluginBucket* a, const PluginBucket* b) {
                  auto na = a->plugin ? a->plugin->name : std::string();
                  auto nb = b->plugin ? b->plugin->name : std::string();
                  return na < nb;
              });

    md += "## plugins\n\n";
    for (const PluginBucket* b : ordered) {
        if (!b->plugin) continue;
        const auto& p = *b->plugin;
        int subtotal = 0;
        for (auto& kv : b->moduleCounts) subtotal += kv.second;
        md += "- **" + p.name + "** (`" + p.slug + "`)"
            + (p.version.empty() ? "" : " v" + p.version)
            + " — " + std::to_string(subtotal) + " module"
            + (subtotal == 1 ? "" : "s") + "\n";

        std::vector<std::pair<std::string, int>> rows(
            b->moduleCounts.begin(), b->moduleCounts.end());
        std::sort(rows.begin(), rows.end(),
                  [](const std::pair<std::string, int>& a,
                     const std::pair<std::string, int>& c) {
                      if (a.second != c.second) return a.second > c.second;
                      return a.first < c.first;
                  });
        for (auto& r : rows) {
            md += "  - " + r.first;
            if (r.second > 1) md += " × " + std::to_string(r.second);
            md += "\n";
        }
    }

    // Install list — slugs match https://library.vcvrack.com/<slug>. No bulk
    // subscribe URL exists, so the count prefix sets expectations.
    int nPlugins = 0;
    for (const PluginBucket* b : ordered) if (b->plugin) nPlugins++;
    md += "\n## install list (" + std::to_string(nPlugins) + " plugin"
        + (nPlugins == 1 ? "" : "s") + " to subscribe to)\n\n";
    for (const PluginBucket* b : ordered) {
        if (!b->plugin) continue;
        const auto& p = *b->plugin;
        md += "- [" + p.slug + "](https://library.vcvrack.com/" + p.slug + ")";
        if (!p.version.empty()) md += " _v" + p.version + "_";
        md += "\n";
    }

    if (missingPlugin > 0) {
        md += "\n_note: " + std::to_string(missingPlugin)
            + " module"  + (missingPlugin == 1 ? "" : "s")
            + " had no plugin pointer and were skipped._\n";
    }

    return md;
}

void CaptureModule::runScan() {
    std::string md = buildScanMarkdown();
    if (md.empty()) return;

    std::string dir = resolveOutputDir();
    rack::system::createDirectories(dir);
    std::string pfx = scanPrefix.empty() ? std::string("scan_") : scanPrefix;
    int idx = nextIndex(dir, pfx, ".md");
    char name[64];
    std::snprintf(name, sizeof(name), "%s%02d.md", pfx.c_str(), idx);
    std::string path = dir + "/" + name;
    {
        std::ofstream out(path);
        if (out) out << md;
    }

    if (copyClipboard && APP->window && APP->window->win) {
        glfwSetClipboardString(APP->window->win, md.c_str());
    }

    scanFlashT.store(rack::system::getTime(), std::memory_order_relaxed);
}

// ─── Widget ─────────────────────────────────────────────────────────────────

namespace {

struct CaptureBackground : widget::Widget {
    void draw(const DrawArgs& args) override {
                nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, lc::panelBg());
        nvgFill(args.vg);
    }
};

struct CaptureLogo : widget::Widget {
    std::string path, darkPath, greyPath;
    void draw(const DrawArgs& args) override {
                std::string use = lc::logoAsset(path, darkPath, greyPath);
        auto img = APP->window->loadImage(use);
        if (!img || img->handle < 0) return;
        NVGpaint p = nvgImagePattern(args.vg, 0, 0, box.size.x, box.size.y, 0, img->handle, 1.f);
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillPaint(args.vg, p);
        nvgFill(args.vg);
    }
};

struct CaptureLabel : widget::Widget {
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

// Matches the Take module's SaveButton look — bezel, inner face, grey core
// dot that flashes amber on a successful capture.
struct ShutterButton : widget::OpaqueWidget {
    CaptureModule* cm = nullptr;

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
        double t0  = cm ? cm->flashT.load(std::memory_order_relaxed) : -1.0;
        float flashA = 0.f;
        if (t0 > 0 && now - t0 < 0.6) flashA = 1.f - (float)((now - t0) / 0.6);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rOuter);
        nvgFillColor(args.vg, rim); nvgFill(args.vg);
        nvgStrokeColor(args.vg, rimEdge); nvgStrokeWidth(args.vg, 0.6f); nvgStroke(args.vg);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rInner);
        nvgFillColor(args.vg, face); nvgFill(args.vg);
        nvgStrokeColor(args.vg, faceEdge); nvgStrokeWidth(args.vg, 0.4f); nvgStroke(args.vg);

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
        if (layer == 1 && cm) {
            double now = rack::system::getTime();
            double t0  = cm->flashT.load(std::memory_order_relaxed);
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
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && cm) {
            if (cm->stage.load() == CaptureModule::IDLE) {
                // Fit-all: reframe now; leave the view like this after the
                // shot (no restore — user asked to stay zoomed out).
                if (cm->fitAll && APP && APP->scene && APP->scene->rackScroll)
                    APP->scene->rackScroll->zoomToModules();

                // Enter SETTLE — give the rack a window of frames to let
                // cached module panels (framebuffer widgets) redraw at the
                // new zoom before we take the shot.
                cm->settleCountdown.store(SETTLE_FRAMES);
                cm->stage.store(CaptureModule::SETTLE);
            }
            e.consume(this);
            return;
        }
        OpaqueWidget::onButton(e);
    }
};

// Scan button — same visual vocabulary as the shutter, bound to runScan and
// its own flash atomic so the two actions animate independently.
struct ScanRunButton : widget::OpaqueWidget {
    CaptureModule* cm = nullptr;

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
        double t0  = cm ? cm->scanFlashT.load(std::memory_order_relaxed) : -1.0;
        float flashA = 0.f;
        if (t0 > 0 && now - t0 < 0.6) flashA = 1.f - (float)((now - t0) / 0.6);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rOuter);
        nvgFillColor(args.vg, rim); nvgFill(args.vg);
        nvgStrokeColor(args.vg, rimEdge); nvgStrokeWidth(args.vg, 0.6f); nvgStroke(args.vg);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rInner);
        nvgFillColor(args.vg, face); nvgFill(args.vg);
        nvgStrokeColor(args.vg, faceEdge); nvgStrokeWidth(args.vg, 0.4f); nvgStroke(args.vg);

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
        if (layer == 1 && cm) {
            double now = rack::system::getTime();
            double t0  = cm->scanFlashT.load(std::memory_order_relaxed);
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
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && cm) {
            cm->runScan();
            e.consume(this);
            return;
        }
        OpaqueWidget::onButton(e);
    }
};

// Short-lived "copied" confirmation — fades in + out under the scan button
// so the user knows the markdown hit the clipboard (button flash alone
// doesn't distinguish file-write from clipboard-copy).
struct CopiedReadout : widget::Widget {
    CaptureModule* cm = nullptr;
    static constexpr double VISIBLE_S = 1.6;
    void draw(const DrawArgs& args) override {
        if (!cm || !cm->copyClipboard) return;
        double t0 = cm->scanFlashT.load(std::memory_order_relaxed);
        if (t0 <= 0) return;
        double dt = rack::system::getTime() - t0;
        if (dt > VISIBLE_S) return;

        float a = 1.f;
        if (dt > VISIBLE_S - 0.4) a = (float)((VISIBLE_S - dt) / 0.4);
        a = std::max(0.f, std::min(1.f, a));

        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, 6.f);
        nvgTextLetterSpacing(args.vg, 0.3f);
        NVGcolor c = lc::theme.dark ? nvgRGB(230, 230, 230) : nvgRGB(26, 26, 26);
        nvgFillColor(args.vg, nvgRGBAf(c.r, c.g, c.b, a));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(args.vg, box.size.x / 2.f, 0.f, "copied", NULL);
    }
};

} // namespace

CaptureWidget::CaptureWidget(CaptureModule* module) {
    setModule(module);
    cm = module;
    box.size = math::Vec(RACK_GRID_WIDTH * 3, RACK_GRID_HEIGHT);

    CaptureBackground* bg = new CaptureBackground;
    bg->box.size = box.size;
    addChild(bg);

    app::PanelBorder* border = new app::PanelBorder;
    border->box.size = box.size;
    addChild(border);

    // Y-positions mirror tidy's two-toggle layout so the qol family reads as
    // one visual system: top button at logoTop - 21mm, bottom at logoTop - 10mm,
    // labels sit 3mm above each button.
    const float logoTopMM = 128.5f - 8.f - 9.f + 2.f;
    const float captureAnchorMM = logoTopMM - 21.f;
    const float scanAnchorMM    = logoTopMM - 10.f;

    auto placeLabel = [&](const std::string& text, float anchorMM) {
        CaptureLabel* l = new CaptureLabel;
        l->text = text;
        l->box.size = math::Vec(box.size.x, mm2px(3.f));
        l->box.pos  = math::Vec(0, mm2px(anchorMM - 3.f));
        addChild(l);
    };

    placeLabel("capture", captureAnchorMM);
    {
        ShutterButton* b = new ShutterButton;
        b->cm = module;
        b->box.size = mm2px(math::Vec(5.f, 5.f));
        b->box.pos  = math::Vec((box.size.x - b->box.size.x) / 2.f,
                                mm2px(captureAnchorMM));
        addChild(b);
    }

    placeLabel("scan", scanAnchorMM);
    {
        ScanRunButton* b = new ScanRunButton;
        b->cm = module;
        b->box.size = mm2px(math::Vec(5.f, 5.f));
        b->box.pos  = math::Vec((box.size.x - b->box.size.x) / 2.f,
                                mm2px(scanAnchorMM));
        addChild(b);
    }

    // "copied" readout, tucked right under the scan button in the narrow
    // gap before the logo.
    {
        CopiedReadout* r = new CopiedReadout;
        r->cm = module;
        r->box.size = math::Vec(box.size.x, mm2px(3.f));
        r->box.pos  = math::Vec(0, mm2px(scanAnchorMM + 5.2f));
        addChild(r);
    }

    // Logo at bottom
    {
        CaptureLogo* lg = new CaptureLogo;
        lg->path     = asset::plugin(pluginInstance, "res/lc-icon-new.png");
        lg->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");

        lg->greyPath = asset::plugin(pluginInstance, "res/lc-icon-grey.png");
        lg->box.size = mm2px(Vec(9.f, 9.f));
        lg->box.pos  = math::Vec((box.size.x - lg->box.size.x) / 2.f,
                                 mm2px(logoTopMM));
        addChild(lg);
    }
}

// Per-frame state machine:
//   click  → SETTLE (count down N frames while the new view paints)
//   SETTLE → ARMED  (once countdown hits 0)
//   draw of ARMED → hide self, promote to SHOOT for next frame
//   step sees SHOOT → read front buffer, IDLE, flash
//
// step() runs before draw() every frame, so the ARMED → SHOOT transition
// lives inside draw() — if we moved it into step, draw would never see
// ARMED and the hide wouldn't take effect.
void CaptureWidget::step() {
    ModuleWidget::step();
    if (!cm) return;
    int s = cm->stage.load(std::memory_order_relaxed);
    if (s == CaptureModule::SETTLE) {
        int c = cm->settleCountdown.load(std::memory_order_relaxed);
        if (c <= 0) cm->stage.store(CaptureModule::ARMED);
        else        cm->settleCountdown.store(c - 1);
    } else if (s == CaptureModule::SHOOT) {
        performCapture();
        cm->stage.store(CaptureModule::IDLE);
        cm->flashT.store(rack::system::getTime(), std::memory_order_relaxed);

        // Bundle finalisation runs right after the shot: scan.md dropped
        // beside the PNG and a copy of the current patch.vcv if one exists.
        // Reveal the folder in Finder / file manager so the user can grab it.
        if (!cm->bundleDir.empty()) {
            const std::string dir = cm->bundleDir;
            {
                std::string md = cm->buildScanMarkdown();
                std::ofstream out(dir + "/scan.md");
                if (out) out << md;
            }
            if (APP && APP->patch) {
                std::string patchPath = APP->patch->path;
                if (!patchPath.empty()) {
                    std::ifstream in(patchPath, std::ios::binary);
                    std::ofstream out(dir + "/patch.vcv", std::ios::binary);
                    if (in && out) out << in.rdbuf();
                }
            }
            rack::system::openDirectory(dir);
            cm->bundleDir.clear();
        }
    }
}

void CaptureWidget::draw(const DrawArgs& args) {
    if (cm) {
        int s = cm->stage.load(std::memory_order_relaxed);
        if (s == CaptureModule::ARMED) {
            cm->stage.store(CaptureModule::SHOOT);
            if (cm->hideSelf) return;
        }
    }
    ModuleWidget::draw(args);
}

// ─── The actual screen grab ─────────────────────────────────────────────────

void CaptureWidget::performCapture() {
    if (!cm || !APP || !APP->window || !APP->window->win) return;

    GLFWwindow* win = APP->window->win;
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(win, &fbW, &fbH);
    if (fbW <= 0 || fbH <= 0) return;

    int winW = 0, winH = 0;
    glfwGetWindowSize(win, &winW, &winH);
    float rx = (winW > 0) ? (float)fbW / (float)winW : 1.f;
    float ry = (winH > 0) ? (float)fbH / (float)winH : 1.f;

    // Capture region in framebuffer pixels.
    int px = 0, py = 0, pw = fbW, ph = fbH;
    if (cm->viewportOnly && APP->scene && APP->scene->rackScroll) {
        math::Rect r = APP->scene->rackScroll->box;
        px = (int)std::floor(r.pos.x  * rx);
        py = (int)std::floor(r.pos.y  * ry);
        pw = (int)std::ceil (r.size.x * rx);
        ph = (int)std::ceil (r.size.y * ry);
    }
    // Clamp to framebuffer bounds.
    if (px < 0) { pw += px; px = 0; }
    if (py < 0) { ph += py; py = 0; }
    if (px + pw > fbW) pw = fbW - px;
    if (py + ph > fbH) ph = fbH - py;
    if (pw <= 0 || ph <= 0) return;

    // glReadPixels uses y=0 at the bottom of the framebuffer; Rack widget
    // coords have y=0 at the top. Convert.
    int glY = fbH - (py + ph);

    // Read. GL_FRONT gives us the currently-displayed frame, which is
    // exactly what the user's looking at.
    std::vector<uint8_t> raw(pw * ph * 4);
    GLint prevBuffer = GL_BACK;
    glGetIntegerv(GL_READ_BUFFER, &prevBuffer);
    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(px, glY, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    glReadBuffer(prevBuffer);

    // Flip rows so the PNG is top-down.
    CaptureJob* job = new CaptureJob;
    job->w = pw;
    job->h = ph;
    job->pixels.resize(pw * ph * 4);
    for (int y = 0; y < ph; y++) {
        std::memcpy(job->pixels.data() + y * pw * 4,
                    raw.data() + (ph - 1 - y) * pw * 4,
                    pw * 4);
    }

    // Build output path. Bundle mode routes the PNG to <bundleDir>/capture.png
    // with a fixed name (no indexing) so the bundle is a clean drop-in folder.
    if (!cm->bundleDir.empty()) {
        rack::system::createDirectories(cm->bundleDir);
        job->path = cm->bundleDir + "/capture.png";
    } else {
        std::string dir = cm->resolveOutputDir();
        rack::system::createDirectories(dir);
        std::string pfx = cm->prefix.empty() ? std::string("capture_") : cm->prefix;
        int idx = cm->nextIndex(dir, pfx, ".png");
        char name[64];
        std::snprintf(name, sizeof(name), "%s%02d.png", pfx.c_str(), idx);
        job->path = dir + "/" + name;
    }

    std::thread(&writePng, job).detach();
}

// ─── Context menu ───────────────────────────────────────────────────────────

namespace {

struct PrefixField : ui::TextField {
    CaptureModule* cm = nullptr;
    PrefixField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (cm) cm->prefix = text;
    }
};

struct ScanPrefixField : ui::TextField {
    CaptureModule* cm = nullptr;
    ScanPrefixField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (cm) cm->scanPrefix = text;
    }
};

struct DirField : ui::TextField {
    CaptureModule* cm = nullptr;
    DirField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (cm) cm->outputDir = text;
    }
};

} // namespace

void CaptureWidget::appendContextMenu(Menu* menu) {
    CaptureModule* m = dynamic_cast<CaptureModule*>(module);
    if (!m) return;

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Capture"));
    menu->addChild(createBoolPtrMenuItem("Fit whole rack (zoom out before shot)", "", &m->fitAll));
    menu->addChild(createBoolPtrMenuItem("Hide this module during capture", "", &m->hideSelf));
    menu->addChild(createBoolPtrMenuItem("Rack viewport only (off = whole window)", "", &m->viewportOnly));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Scan"));
    menu->addChild(createMenuItem("Run scan now", "", [m]() { m->runScan(); }));
    menu->addChild(createBoolPtrMenuItem("Copy report to clipboard", "", &m->copyClipboard));

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Bundle"));
    // Bundle patch — pops Finder/file dialog for a destination folder,
    // spins up a dated project subfolder inside it, then runs the normal
    // capture flow but routes PNG + scan.md + patch.vcv into that folder.
    // Finder is opened at the new folder once everything lands.
    menu->addChild(createMenuItem("Bundle patch…", "", [m]() {
        char* p = osdialog_file(OSDIALOG_OPEN_DIR, nullptr, nullptr, nullptr);
        if (!p) return;
        std::string destParent = p;
        std::free(p);

        std::string patchName = "untitled";
        if (APP && APP->patch) {
            std::string path = APP->patch->path;
            if (!path.empty()) {
                size_t slash = path.find_last_of("/\\");
                std::string name = (slash == std::string::npos)
                    ? path : path.substr(slash + 1);
                size_t dot = name.rfind('.');
                if (dot != std::string::npos) name = name.substr(0, dot);
                if (!name.empty()) patchName = name;
            }
        }
        std::time_t now = std::time(nullptr);
        std::tm* lt = std::localtime(&now);
        char datebuf[16];
        std::strftime(datebuf, sizeof(datebuf), "%d_%m_%Y", lt);
        std::string bundle = destParent + "/" + patchName + "_bundle_" + datebuf;
        rack::system::createDirectories(bundle);

        // Kick off the normal capture flow, same as the shutter button —
        // fit-all zoom, settle window, then SHOOT. step() will notice the
        // bundleDir is set and drop scan.md + patch.vcv once the PNG lands.
        m->bundleDir = bundle;
        if (m->fitAll && APP && APP->scene && APP->scene->rackScroll)
            APP->scene->rackScroll->zoomToModules();
        m->settleCountdown.store(SETTLE_FRAMES);
        m->stage.store(CaptureModule::SETTLE);
    }));

    menu->addChild(new MenuSeparator);

    menu->addChild(createMenuLabel("Capture filename prefix"));
    PrefixField* pf = new PrefixField; pf->cm = m; pf->text = m->prefix;
    menu->addChild(pf);

    menu->addChild(createMenuLabel("Scan filename prefix"));
    ScanPrefixField* sf = new ScanPrefixField; sf->cm = m; sf->text = m->scanPrefix;
    menu->addChild(sf);

    menu->addChild(createMenuLabel("Output directory"));
    DirField* df = new DirField; df->cm = m; df->text = m->outputDir;
    menu->addChild(df);

    menu->addChild(createMenuItem("Choose folder…", "", [m]() {
        char* p = osdialog_file(OSDIALOG_OPEN_DIR, nullptr, nullptr, nullptr);
        if (p) { m->outputDir = p; std::free(p); }
    }));
    menu->addChild(createMenuItem("Reveal output folder", "", [m]() {
        std::string d = m->resolveOutputDir();
        rack::system::createDirectories(d);
        rack::system::openDirectory(d);
    }));

    menu->addChild(new MenuSeparator);
    lc::appendThemeMenu(menu);
}

Model* modelCapture = createModel<CaptureModule, CaptureWidget>("capture");
