#include "Scan.hpp"
#include "Theme.hpp"
#include "plugin.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <map>
#include <osdialog.h>
#include <string>
#include <sys/stat.h>
#include <vector>

// ─── Module ─────────────────────────────────────────────────────────────────

ScanModule::ScanModule() {
    config(0, 0, 0, 0);
}

json_t* ScanModule::dataToJson() {
    json_t* r = json_object();
    json_object_set_new(r, "prefix",        json_string(prefix.c_str()));
    json_object_set_new(r, "outputDir",     json_string(outputDir.c_str()));
    json_object_set_new(r, "copyClipboard", json_boolean(copyClipboard));
    return r;
}

void ScanModule::dataFromJson(json_t* root) {
    if (json_t* j = json_object_get(root, "prefix"))        prefix        = json_string_value(j);
    if (json_t* j = json_object_get(root, "outputDir"))     outputDir     = json_string_value(j);
    if (json_t* j = json_object_get(root, "copyClipboard")) copyClipboard = json_boolean_value(j);
}

std::string ScanModule::resolveOutputDir() const {
    if (outputDir.empty()) return asset::plugin(pluginInstance, "test");
    if (outputDir[0] == '/' || (outputDir.size() > 1 && outputDir[1] == ':'))
        return outputDir;
    return asset::plugin(pluginInstance, outputDir);
}

int ScanModule::scanNextIndex(const std::string& dir) const {
    std::string pfx = prefix.empty() ? std::string("scan_") : prefix;
    int maxN = 0;
    DIR* d = opendir(dir.c_str());
    if (!d) return 1;
    while (dirent* ent = readdir(d)) {
        std::string name = ent->d_name;
        if (name.size() < pfx.size() + 3) continue;
        if (name.compare(0, pfx.size(), pfx) != 0) continue;
        size_t dot = name.rfind('.');
        if (dot == std::string::npos) continue;
        std::string ext = name.substr(dot);
        if (ext != ".md" && ext != ".MD") continue;
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

// ─── Scan walk + report ─────────────────────────────────────────────────────

namespace {

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

void ScanModule::runScan() {
    if (!APP || !APP->scene || !APP->scene->rack) return;

    std::map<std::string, PluginBucket> byPlugin;  // plugin slug → bucket
    int totalModules = 0;
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

    // Build the markdown report.
    std::string md;
    md += "# scan · " + nowStamp() + "\n\n";

    lastSummary = std::to_string(totalModules) + " modules · "
                + std::to_string((int)byPlugin.size()) + " plugins";
    md += lastSummary + "\n\n";

    // Plugins sorted alphabetically by display name for the human-readable section.
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

        // Module rows, sorted by count desc then name.
        std::vector<std::pair<std::string, int>> rows(b->moduleCounts.begin(), b->moduleCounts.end());
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

    // Install list. Each entry is a clickable link to its VCV Library page
    // so the recipient can go straight to the subscribe button. Slugs match
    // library URL pattern https://library.vcvrack.com/<plugin-slug>.
    // VCV has no bulk-subscribe URL, so the count prefix sets expectations.
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

    // Write to disk.
    std::string dir = resolveOutputDir();
    rack::system::createDirectories(dir);
    int idx = scanNextIndex(dir);
    char name[64];
    std::snprintf(name, sizeof(name), "%s%02d.md",
                  prefix.empty() ? "scan_" : prefix.c_str(), idx);
    std::string path = dir + "/" + name;
    {
        std::ofstream out(path);
        if (out) out << md;
    }

    // And to the clipboard if requested.
    if (copyClipboard && APP->window && APP->window->win) {
        glfwSetClipboardString(APP->window->win, md.c_str());
    }

    flashT.store(rack::system::getTime(), std::memory_order_relaxed);
}

// ─── Widget ─────────────────────────────────────────────────────────────────

namespace {

struct ScanBackground : widget::Widget {
    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, dark ? nvgRGB(0, 0, 0) : nvgRGB(255, 255, 255));
        nvgFill(args.vg);
    }
};

struct ScanLogo : widget::Widget {
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

struct ScanLabel : widget::Widget {
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

// Ring button. Click runs a scan. Amber pulse on success.
struct RunButton : widget::OpaqueWidget {
    ScanModule* sm = nullptr;

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
        double t0  = sm ? sm->flashT.load(std::memory_order_relaxed) : -1.0;
        float a = (t0 > 0 && now - t0 < 0.6) ? (1.f - (float)((now - t0) / 0.6)) : 0.f;

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rOuter);
        nvgFillColor(args.vg, rim); nvgFill(args.vg);
        nvgStrokeColor(args.vg, rimEdge); nvgStrokeWidth(args.vg, 0.6f); nvgStroke(args.vg);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rInner);
        nvgFillColor(args.vg, face); nvgFill(args.vg);
        nvgStrokeColor(args.vg, faceEdge); nvgStrokeWidth(args.vg, 0.4f); nvgStroke(args.vg);

        NVGcolor core = idle;
        if (a > 0) {
            core.r = idle.r + (flash.r - idle.r) * a;
            core.g = idle.g + (flash.g - idle.g) * a;
            core.b = idle.b + (flash.b - idle.b) * a;
        }
        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rCore);
        nvgFillColor(args.vg, core); nvgFill(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1 && sm) {
            double now = rack::system::getTime();
            double t0  = sm->flashT.load(std::memory_order_relaxed);
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
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && sm) {
            sm->runScan();
            e.consume(this);
            return;
        }
        OpaqueWidget::onButton(e);
    }
};

// Short-lived "copied" confirmation under the button. Fades in + out after a
// scan so the user sees feedback without a persistent stat readout competing
// for attention.
struct CopiedReadout : widget::Widget {
    ScanModule* sm = nullptr;
    static constexpr double VISIBLE_S = 1.8;
    void draw(const DrawArgs& args) override {
        if (!sm) return;
        double t0 = sm->flashT.load(std::memory_order_relaxed);
        if (t0 <= 0) return;
        double dt = rack::system::getTime() - t0;
        if (dt > VISIBLE_S) return;

        // Fade out in the last 0.4s.
        float a = 1.f;
        if (dt > VISIBLE_S - 0.4) a = (float)((VISIBLE_S - dt) / 0.4);
        a = std::max(0.f, std::min(1.f, a));

        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, 7.f);
        nvgTextLetterSpacing(args.vg, 0.3f);
        NVGcolor c = lc::theme.dark ? nvgRGB(230, 230, 230) : nvgRGB(26, 26, 26);
        nvgFillColor(args.vg, nvgRGBAf(c.r, c.g, c.b, a));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(args.vg, box.size.x / 2.f, 0.f, "copied", NULL);
    }
};

} // namespace

ScanWidget::ScanWidget(ScanModule* module) {
    setModule(module);
    box.size = math::Vec(RACK_GRID_WIDTH * 3, RACK_GRID_HEIGHT);

    ScanBackground* bg = new ScanBackground;
    bg->box.size = box.size;
    addChild(bg);

    float screwX = (box.size.x - RACK_GRID_WIDTH) / 2.f;
    addChild(createWidget<ScrewBlack>(math::Vec(screwX, 0)));
    addChild(createWidget<ScrewBlack>(math::Vec(screwX, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    app::PanelBorder* border = new app::PanelBorder;
    border->box.size = box.size;
    addChild(border);

    {
        ScanLabel* l = new ScanLabel;
        l->text = "scan";
        l->box.size = math::Vec(box.size.x, mm2px(3.f));
        l->box.pos  = math::Vec(0, mm2px(9.f));
        addChild(l);
    }

    {
        RunButton* b = new RunButton;
        b->sm = module;
        b->box.size = mm2px(math::Vec(6.f, 6.f));
        b->box.pos  = math::Vec((box.size.x - b->box.size.x) / 2.f, mm2px(15.f));
        addChild(b);
    }

    {
        CopiedReadout* r = new CopiedReadout;
        r->sm = module;
        r->box.size = math::Vec(box.size.x, mm2px(5.f));
        r->box.pos  = math::Vec(0, mm2px(28.f));
        addChild(r);
    }

    {
        ScanLogo* lg = new ScanLogo;
        lg->path     = asset::plugin(pluginInstance, "res/lc-icon-new.png");
        lg->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");
        lg->box.size = mm2px(math::Vec(9.f, 9.f));
        lg->box.pos  = math::Vec((box.size.x - lg->box.size.x) / 2.f,
                                 mm2px(128.5f - 8.f - 9.f));
        addChild(lg);
    }
}

// ─── Context menu ───────────────────────────────────────────────────────────

namespace {

struct PrefixField : ui::TextField {
    ScanModule* sm = nullptr;
    PrefixField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (sm) sm->prefix = text;
    }
};

struct DirField : ui::TextField {
    ScanModule* sm = nullptr;
    DirField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (sm) sm->outputDir = text;
    }
};

} // namespace

void ScanWidget::appendContextMenu(Menu* menu) {
    ScanModule* m = dynamic_cast<ScanModule*>(module);
    if (!m) return;

    menu->addChild(new MenuSeparator);

    menu->addChild(createMenuItem("Run scan now", "", [m]() { m->runScan(); }));
    menu->addChild(createBoolPtrMenuItem("Copy to clipboard on scan", "", &m->copyClipboard));

    menu->addChild(new MenuSeparator);

    menu->addChild(createMenuLabel("Filename prefix"));
    PrefixField* pf = new PrefixField; pf->sm = m; pf->text = m->prefix;
    menu->addChild(pf);

    menu->addChild(createMenuLabel("Output directory"));
    DirField* df = new DirField; df->sm = m; df->text = m->outputDir;
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
    menu->addChild(createMenuItem("Dark mode (shared)",
        CHECKMARK(lc::theme.dark), []() {
            lc::theme.dark = !lc::theme.dark;
            lc::saveTheme();
        }));
}

Model* modelScan = createModel<ScanModule, ScanWidget>("scan");
