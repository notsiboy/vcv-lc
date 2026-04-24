#include "GrabPlus.hpp"
#include "Theme.hpp"
#include "plugin.hpp"
#include "LcPorts.hpp"
#include <patch.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <osdialog.h>
#include <string>

// ─── Module ─────────────────────────────────────────────────────────────────

GrabPlusModule::GrabPlusModule() {
    config(0, NUM_INPUTS, 0, 0);
    configInput(IN_L, "Left");
    configInput(IN_R, "Right");

    // Seed outputDir from the last-used folder so a fresh grab+ drops saves
    // wherever the user last worked. dataFromJson will overwrite this if the
    // patch stored a non-empty outputDir; we also defend against an explicit
    // empty-string in the JSON there.
    std::string last = loadLastOutputDir();
    if (!last.empty()) {
        grab.outputDir = last;
        take.outputDir = last;
    }
    // Lay in the initial grab/rec/take subfolder + filename prefixes for
    // the default patch name ("untitled" until the host saves).
    updateSubfolder();
}

void GrabPlusModule::onSampleRateChange(const SampleRateChangeEvent& e) {
    grab.onSampleRateChange(e);
    take.onSampleRateChange(e);
}

// Forward the outer inputs into both inner modules' input ports each sample.
// We write `channels` directly because Port::setChannels() short-circuits when
// the port is currently disconnected (channels == 0) — it's designed to bump
// polyphony on an already-connected port, not synthesise a connection from
// nothing. The inner ports never see a cable so they'd stay disconnected,
// and grab/take's `isConnected()` guards would return false, starving their
// DSP of audio.
void GrabPlusModule::process(const ProcessArgs& args) {
    int lCh = inputs[IN_L].getChannels();
    int rCh = inputs[IN_R].getChannels();

    grab.inputs[GrabModule::IN_L].channels = (uint8_t)lCh;
    grab.inputs[GrabModule::IN_R].channels = (uint8_t)rCh;
    take.inputs[TakeModule::IN_L].channels = (uint8_t)lCh;
    take.inputs[TakeModule::IN_R].channels = (uint8_t)rCh;

    if (lCh > 0) {
        float v = inputs[IN_L].getVoltage(0);
        grab.inputs[GrabModule::IN_L].setVoltage(v);
        take.inputs[TakeModule::IN_L].setVoltage(v);
    }
    if (rCh > 0) {
        float v = inputs[IN_R].getVoltage(0);
        grab.inputs[GrabModule::IN_R].setVoltage(v);
        take.inputs[TakeModule::IN_R].setVoltage(v);
    }

    // Route the mode toggle's "snip" state to take so its rolling ring can
    // freeze on silence without needing to know about GrabModule.
    take.snipActive.store(
        grab.mode.load(std::memory_order_relaxed) == GrabModule::MODE_SNIP,
        std::memory_order_relaxed);

    grab.process(args);
    take.process(args);
}

json_t* GrabPlusModule::dataToJson() {
    json_t* r = json_object();
    json_object_set_new(r, "grab", grab.dataToJson());
    json_object_set_new(r, "take", take.dataToJson());
    json_object_set_new(r, "useDatedSubfolder", json_boolean(useDatedSubfolder));
    return r;
}

void GrabPlusModule::dataFromJson(json_t* root) {
    if (json_t* g = json_object_get(root, "grab")) grab.dataFromJson(g);
    if (json_t* t = json_object_get(root, "take")) take.dataFromJson(t);
    if (json_t* j = json_object_get(root, "useDatedSubfolder"))
        useDatedSubfolder = json_boolean_value(j);
    // If the patch stored an empty outputDir (fresh-inserted grab+ then saved
    // before the user picked a folder), fall back to the last-dir file so a
    // reload still lands somewhere sensible.
    if (grab.outputDir.empty()) {
        std::string last = loadLastOutputDir();
        if (!last.empty()) {
            grab.outputDir = last;
            take.outputDir = last;
        }
    }
    updateSubfolder();
}

std::string GrabPlusModule::currentPatchName() {
    if (APP && APP->patch) {
        std::string path = APP->patch->path;
        if (!path.empty()) {
            size_t slash = path.find_last_of("/\\");
            std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
            size_t dot = name.rfind('.');
            if (dot != std::string::npos) name = name.substr(0, dot);
            if (!name.empty()) return name;
        }
    }
    return "untitled";
}

std::string GrabPlusModule::datedSubfolderName() {
    std::string patchName = currentPatchName();
    std::time_t now = std::time(nullptr);
    std::tm* lt = std::localtime(&now);
    char datebuf[16];
    std::strftime(datebuf, sizeof(datebuf), "%d_%m_%Y", lt);
    return patchName + "_" + datebuf;
}

void GrabPlusModule::updateSubfolder() {
    // Outer dated folder when toggled on; the per-type folders below it are
    // always present so grabs / recs / takes never mix.
    std::string outer = useDatedSubfolder ? datedSubfolderName() : std::string();
    auto typeSub = [&](const char* type) {
        return outer.empty() ? std::string(type) : outer + "/" + type;
    };
    grab.subfolder    = typeSub("grabs");
    grab.subfolderRec = typeSub("recs");
    take.subfolder    = typeSub("takes");

    // Filename bases — "<patchname>_<type>_".
    std::string patch = currentPatchName();
    grab.prefix    = patch + "_grab_";
    grab.prefixRec = patch + "_rec_";
    take.prefix    = patch + "_take_";

    cachedPatchPath = (APP && APP->patch) ? APP->patch->path : std::string();
}

// ─── Last-dir persistence ───────────────────────────────────────────────────
//
// Stored in a single file in Rack's user folder so every grab+ instance (and
// successive Rack sessions) share the same "last used" hint. Fire-and-forget —
// read failures just yield an empty string which the caller treats as "no
// hint available".

static std::string grabPlusLastDirPath() {
    return asset::user("LuxCache-grabplus-lastdir.txt");
}

std::string GrabPlusModule::loadLastOutputDir() {
    std::FILE* f = std::fopen(grabPlusLastDirPath().c_str(), "rb");
    if (!f) return "";
    char buf[2048];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    std::string s(buf);
    // Trim trailing whitespace / newlines.
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
    return s;
}

void GrabPlusModule::saveLastOutputDir(const std::string& dir) {
    if (dir.empty()) return;
    std::FILE* f = std::fopen(grabPlusLastDirPath().c_str(), "wb");
    if (!f) return;
    std::fwrite(dir.data(), 1, dir.size(), f);
    std::fclose(f);
}

void GrabPlusModule::setOutputDir(const std::string& dir) {
    grab.outputDir = dir;
    take.outputDir = dir;
    saveLastOutputDir(dir);
}

// ─── Widgets ────────────────────────────────────────────────────────────────
//
// First pass: widget classes are duplicated from Grab.cpp / Take.cpp because
// those sit inside anonymous namespaces and aren't exported. If we keep this
// module, lift the shared widgets to a header.

namespace {

struct GrabPlusBackground : widget::Widget {
    void draw(const DrawArgs& args) override {
                nvgBeginPath(args.vg);
        nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
        nvgFillColor(args.vg, lc::panelBg());
        nvgFill(args.vg);
    }
};

struct GrabPlusLogo : widget::Widget {
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

struct GrabPlusLabel : widget::Widget {
    std::string text;
    float fontSize = 7.f;
    void draw(const DrawArgs& args) override {
        auto font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
        if (!font || !font->handle) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgFontSize(args.vg, fontSize);
        nvgTextLetterSpacing(args.vg, 0.2f);
        nvgFillColor(args.vg, lc::theme.dark ? nvgRGB(230, 230, 230) : nvgRGB(26, 26, 26));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        nvgText(args.vg, box.size.x / 2.f, 0.f, text.c_str(), NULL);
    }
};

// ── Grab widgets (duplicated) ───────────────────────────────────────────────

// Three-way cycle button for the universal mode toggle.
//   OFF  — idle grey (no tint)
//   GRAB — yellow (auto-triggered one-shot recording)
//   SNIP — pink (rolling ring silence-gated; waveform freezes in silence)
//
// Click does NOT commit immediately — it enters a 3-second pending window so
// an accidental cycle can't fire a one-shot (or flip the buffer's snip-gate)
// on the very next audio tick. During the window the LED softly pulses
// between the committed colour and the pending colour. Further clicks cycle
// the pending target and reset the timer; the mode commits once the timer
// expires.
struct ModeCycleButton : widget::OpaqueWidget {
    GrabModule* gm = nullptr;

    int    pendingMode   = -1;   // -1 when nothing pending
    double pendingStartT = 0.0;
    double commitT       = 0.0;
    static constexpr double COMMIT_S = 2.0;

    int currentMode() const {
        return gm ? gm->mode.load(std::memory_order_relaxed) : GrabModule::MODE_OFF;
    }

    static NVGcolor colorForMode(int m, NVGcolor idle) {
        switch (m) {
            case GrabModule::MODE_GRAB: return nvgRGB(250, 235, 140);  // light yellow
            case GrabModule::MODE_SNIP: return nvgRGB(250, 180, 210);  // light pink
            default: return idle;
        }
    }

    // Hard on/off square wave while pending — a crisp flash, not a breathing
    // pulse. Toggles at ~2.5Hz (200 ms on, 200 ms off).
    float pendingPulse() const {
        if (pendingMode < 0) return 0.f;
        double t = rack::system::getTime() - pendingStartT;
        double phase = std::fmod(t * 2.5, 1.0);
        return phase < 0.5 ? 1.f : 0.f;
    }

    void step() override {
        OpaqueWidget::step();
        if (pendingMode >= 0 && rack::system::getTime() >= commitT) {
            if (gm) gm->mode.store(pendingMode, std::memory_order_relaxed);
            pendingMode = -1;
        }
    }

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
        NVGcolor idle     = dark ? nvgRGB(95, 95, 95)  : nvgRGB(170, 170, 170);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rOuter);
        nvgFillColor(args.vg, rim); nvgFill(args.vg);
        nvgStrokeColor(args.vg, rimEdge); nvgStrokeWidth(args.vg, 0.6f); nvgStroke(args.vg);

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rInner);
        nvgFillColor(args.vg, face); nvgFill(args.vg);
        nvgStrokeColor(args.vg, faceEdge); nvgStrokeWidth(args.vg, 0.4f); nvgStroke(args.vg);

        NVGcolor fromC = colorForMode(currentMode(), idle);
        NVGcolor toC   = colorForMode(pendingMode >= 0 ? pendingMode : currentMode(), idle);
        float a = pendingPulse();
        NVGcolor core;
        core.r = fromC.r + (toC.r - fromC.r) * a;
        core.g = fromC.g + (toC.g - fromC.g) * a;
        core.b = fromC.b + (toC.b - fromC.b) * a;
        core.a = 1.f;

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rLed);
        nvgFillColor(args.vg, core);
        nvgFill(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            int display = (pendingMode >= 0) ? pendingMode : currentMode();
            if (display != GrabModule::MODE_OFF) {
                float cx = box.size.x / 2.f;
                float cy = box.size.y / 2.f;
                float rOuter = std::min(cx, cy) - 0.5f;
                float rLed   = rOuter * 0.50f;
                NVGcolor c = colorForMode(display, nvgRGB(0, 0, 0));
                // Dim the glow during pending so the visual reads as "not
                // quite there yet".
                float alpha = (pendingMode >= 0) ? (0.35f + 0.5f * pendingPulse()) : 1.f;
                int a = (int)(140.f * alpha);
                NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, rLed * 0.6f, rLed * 3.f,
                    nvgRGBA((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), a),
                    nvgRGBA((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 0));
                nvgBeginPath(args.vg);
                nvgRect(args.vg, cx - rLed * 4, cy - rLed * 4, rLed * 8, rLed * 8);
                nvgFillPaint(args.vg, glow);
                nvgFill(args.vg);
            }
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && gm) {
            int base = (pendingMode >= 0) ? pendingMode : currentMode();
            int next = (base + 1) % 3;   // OFF → GRAB → SNIP → OFF
            pendingMode   = next;
            pendingStartT = rack::system::getTime();
            commitT       = pendingStartT + COMMIT_S;
            e.consume(this);
        }
    }
};

// Dual-action centre button:
//   short click  → toggle force-rec (manual record)
//   long  click  → fire take save (amber flash)
// The distinction is purely time-based; while held, the LED ramps from green
// (or idle) toward amber so the user sees when they're about to cross the
// threshold into "take" territory. Past threshold the take fires, the held
// click no longer toggles force-rec on release.
struct ForceRecButton : widget::OpaqueWidget {
    GrabModule* gm = nullptr;
    TakeModule* tm = nullptr;

    // 0.45s — long enough that an ordinary click won't trip it, short enough
    // that holding for a take doesn't feel laggy.
    static constexpr double LONG_PRESS_S = 0.45;

    double pressStartT = -1.0;
    bool   longPressFired = false;

    // LED tracks actively-recording state — it's a "tape is rolling" light.
    // One-shot armed-but-waiting doesn't count; that's the arm toggle's job.
    bool isLit() const {
        return gm && gm->recording.load(std::memory_order_relaxed);
    }

    // Fire the take once the hold crosses LONG_PRESS_S. Checked each frame so
    // the user doesn't have to release to trigger — it fires mid-hold.
    void step() override {
        OpaqueWidget::step();
        if (pressStartT > 0 && !longPressFired) {
            double now = rack::system::getTime();
            if (now - pressStartT >= LONG_PRESS_S && tm) {
                tm->saveTake();
                longPressFired = true;
            }
        }
    }

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        float cx = box.size.x / 2.f;
        float cy = box.size.y / 2.f;
        float rOuter = std::min(cx, cy) - 0.5f;
        float rInner = rOuter * 0.78f;
        // LED core stays a fixed small size even if the bezel scales up, so
        // a big chassis still reads as a little indicator dot.
        float rLed   = mm2px(1.15f);

        NVGcolor rimIdle  = dark ? nvgRGB(40, 40, 40)  : nvgRGB(255, 255, 255);
        NVGcolor rimEdge  = dark ? nvgRGB(90, 90, 90)  : nvgRGB(180, 180, 180);
        NVGcolor face     = dark ? nvgRGB(55, 55, 55)  : nvgRGB(240, 240, 240);
        NVGcolor faceEdge = dark ? nvgRGB(75, 75, 75)  : nvgRGB(210, 210, 210);
        NVGcolor idle     = dark ? nvgRGB(95, 95, 95)  : nvgRGB(170, 170, 170);
        NVGcolor recRed   = nvgRGB(230, 60, 60);
        NVGcolor amber    = nvgRGB(240, 150, 40);

        // Amber-overlay strength. Shared by rim + core so the whole button
        // reads the same way during a take flash or long-press hold.
        //   take flash       (post-fire, ~0.6s) — blend → amber fading out
        //   hold in progress (pre-fire)         — ramp → amber as hold grows
        double now = rack::system::getTime();
        double t0  = tm ? tm->saveFlashT.load(std::memory_order_relaxed) : -1.0;
        float amberA = 0.f;
        if (t0 > 0 && now - t0 < 0.6) {
            amberA = 1.f - (float)((now - t0) / 0.6);
        } else if (pressStartT > 0 && !longPressFired) {
            amberA = (float)std::min(1.0, (now - pressStartT) / LONG_PRESS_S);
        }

        auto blendToward = [&](NVGcolor base, NVGcolor target, float a) {
            NVGcolor out;
            out.r = base.r + (target.r - base.r) * a;
            out.g = base.g + (target.g - base.g) * a;
            out.b = base.b + (target.b - base.b) * a;
            out.a = 1.f;
            return out;
        };

        // Outer ring — red while recording, amber overlay during flash/hold.
        NVGcolor rimBase = isLit() ? recRed : rimIdle;
        NVGcolor rim = amberA > 0.f ? blendToward(rimBase, amber, amberA) : rimBase;

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rOuter);
        nvgFillColor(args.vg, rim); nvgFill(args.vg);
        nvgStrokeColor(args.vg, rimEdge); nvgStrokeWidth(args.vg, 0.6f); nvgStroke(args.vg);

        // Inner face stays neutral — gives contrast against the red ring.
        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rInner);
        nvgFillColor(args.vg, face); nvgFill(args.vg);
        nvgStrokeColor(args.vg, faceEdge); nvgStrokeWidth(args.vg, 0.4f); nvgStroke(args.vg);

        // Core LED — same logic as the rim.
        NVGcolor coreBase = isLit() ? recRed : idle;
        NVGcolor core = amberA > 0.f ? blendToward(coreBase, amber, amberA) : coreBase;

        nvgBeginPath(args.vg); nvgCircle(args.vg, cx, cy, rLed);
        nvgFillColor(args.vg, core); nvgFill(args.vg);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1) {
            float cx = box.size.x / 2.f;
            float cy = box.size.y / 2.f;
            float rLed = mm2px(1.15f); // matches draw() — pinned radius

            NVGcolor glowC{};
            float glowA = 0.f;

            double now = rack::system::getTime();
            double t0  = tm ? tm->saveFlashT.load(std::memory_order_relaxed) : -1.0;
            if (t0 > 0 && now - t0 < 0.6) {
                glowC = nvgRGB(240, 150, 40);
                glowA = 1.f - (float)((now - t0) / 0.6);
            } else if (isLit()) {
                glowC = nvgRGB(230, 60, 60);
                glowA = 1.f;
            }

            if (glowA > 0.f) {
                NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, rLed * 0.6f, rLed * 3.f,
                    nvgRGBA((int)(glowC.r * 255), (int)(glowC.g * 255), (int)(glowC.b * 255),
                            (int)(140 * glowA)),
                    nvgRGBA((int)(glowC.r * 255), (int)(glowC.g * 255), (int)(glowC.b * 255), 0));
                nvgBeginPath(args.vg);
                nvgRect(args.vg, cx - rLed * 4, cy - rLed * 4, rLed * 8, rLed * 8);
                nvgFillPaint(args.vg, glow);
                nvgFill(args.vg);
            }
        }
        Widget::drawLayer(args, layer);
    }

    void onButton(const event::Button& e) override {
        if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
            if (e.action == GLFW_PRESS) {
                pressStartT = rack::system::getTime();
                longPressFired = false;
                e.consume(this);
                return;
            }
            if (e.action == GLFW_RELEASE) {
                double elapsed = (pressStartT > 0)
                    ? (rack::system::getTime() - pressStartT) : 0.0;
                pressStartT = -1.0;
                if (!longPressFired && elapsed < LONG_PRESS_S && gm) {
                    gm->forceRec.store(!gm->forceRec.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
                }
                longPressFired = false;
                e.consume(this);
                return;
            }
        }
        OpaqueWidget::onButton(e);
    }

    // Drag-end is the reliable "release" — Rack routes it to the widget that
    // originally consumed the press, even if the cursor moved off-target.
    void onDragEnd(const event::DragEnd& e) override {
        OpaqueWidget::onDragEnd(e);
        if (pressStartT > 0) {
            double elapsed = rack::system::getTime() - pressStartT;
            pressStartT = -1.0;
            if (!longPressFired && elapsed < LONG_PRESS_S && gm) {
                gm->forceRec.store(!gm->forceRec.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
            }
            longPressFired = false;
        }
    }
};

struct PeakMeter : widget::Widget {
    GrabModule* gm = nullptr;
    // -1 → original dual-column (L on the left, R on the right of the box).
    //  0 → draw only the L channel, centred in the box.
    //  1 → draw only the R channel, centred in the box.
    int onlyChannel = -1;

    static constexpr int N_SEG = 10;
    static constexpr float FLOOR_DB = -60.f;
    static constexpr float SEG_DB[N_SEG] = {
        -54.f, -42.f, -30.f, -21.f, -15.f, -12.f, -9.f, -6.f, -3.f, 0.f
    };
    static NVGcolor segColor(int i) {
        if (i >= 9) return nvgRGB(230, 60, 60);    // red — 0 dB
        if (i >= 7) return nvgRGB(230, 200, 60);   // yellow — -6, -3
        return nvgRGB(80, 210, 100);               // green
    }
    static float linToDb(float m) {
        if (m <= 1e-6f) return -1e6f;
        return 20.f * std::log10(m);
    }
    static NVGcolor lerpColor(NVGcolor a, NVGcolor b, float t) {
        return nvgRGBAf(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
    }
    static float brightness(int i, float db) {
        float T = SEG_DB[i];
        float P = (i == 0) ? FLOOR_DB : SEG_DB[i - 1];
        if (db >= T) return 1.f;
        if (db <= P) return 0.f;
        return (db - P) / (T - P);
    }

    // Columns are defined in a small helper so draw/drawLayer share the geometry.
    struct Col { float x; float db; };

    std::vector<Col> columns(float w, float dbL, float dbR, float& colW) const {
        std::vector<Col> cs;
        if (onlyChannel == 0) {
            colW = w;
            cs.push_back({ 0.f, dbL });
        } else if (onlyChannel == 1) {
            colW = w;
            cs.push_back({ 0.f, dbR });
        } else {
            colW = w * 0.32f;
            cs.push_back({ 0.f,        dbL });
            cs.push_back({ w - colW,   dbR });
        }
        return cs;
    }

    void draw(const DrawArgs& args) override {
        bool dark = lc::theme.dark;
        float w = box.size.x, h = box.size.y;
        float dbL = linToDb(gm ? gm->peakL.load(std::memory_order_relaxed) : 0.f);
        float dbR = linToDb(gm ? gm->peakR.load(std::memory_order_relaxed) : 0.f);

        NVGcolor off = dark ? nvgRGB(32, 32, 32) : nvgRGB(225, 225, 225);
        float colW;
        auto cs = columns(w, dbL, dbR, colW);
        // Spread N_SEG LEDs from inset to h - inset so the stack tucks just
        // inside the waveform gutter's top/bottom edges.
        const float insetPx = mm2px(1.f);
        float stride = (h - 2.f * insetPx) / (float)(N_SEG - 1);
        float ledD   = std::min(colW, stride) * 0.70f;

        for (int i = 0; i < N_SEG; i++) {
            int visualRow = N_SEG - 1 - i;
            float cy = insetPx + visualRow * stride;
            NVGcolor on = segColor(i);
            for (const Col& c : cs) {
                float b = brightness(i, c.db);
                float cx = c.x + colW * 0.5f;
                nvgBeginPath(args.vg);
                nvgCircle(args.vg, cx, cy, ledD * 0.5f);
                nvgFillColor(args.vg, lerpColor(off, on, b));
                nvgFill(args.vg);
            }
        }
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1 || !gm) { Widget::drawLayer(args, layer); return; }
        float w = box.size.x, h = box.size.y;
        float dbL = linToDb(gm->peakL.load(std::memory_order_relaxed));
        float dbR = linToDb(gm->peakR.load(std::memory_order_relaxed));
        float colW;
        auto cs = columns(w, dbL, dbR, colW);
        const float insetPx = mm2px(1.f);
        float stride = (h - 2.f * insetPx) / (float)(N_SEG - 1);
        float ledD   = std::min(colW, stride) * 0.70f;

        int i = N_SEG - 1;
        float cy = insetPx;
        NVGcolor red = segColor(i);
        for (const Col& c : cs) {
            float b = brightness(i, c.db);
            if (b <= 0.f) continue;
            float cx = c.x + colW * 0.5f;
            float r = ledD * 0.5f;
            int a = (int)(160.f * b);
            NVGpaint g = nvgRadialGradient(args.vg, cx, cy, r * 0.6f, r * 2.4f,
                nvgRGBA((int)(red.r * 255), (int)(red.g * 255), (int)(red.b * 255), a),
                nvgRGBA((int)(red.r * 255), (int)(red.g * 255), (int)(red.b * 255), 0));
            nvgBeginPath(args.vg);
            nvgRect(args.vg, cx - r * 3, cy - r * 3, r * 6, r * 6);
            nvgFillPaint(args.vg, g); nvgFill(args.vg);
        }
        Widget::drawLayer(args, layer);
    }
};
constexpr float PeakMeter::SEG_DB[PeakMeter::N_SEG];

// ── Take widgets (duplicated) ───────────────────────────────────────────────

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

struct WaveformStrip : widget::Widget {
    TakeModule* tm = nullptr;
    void draw(const DrawArgs& args) override {
        if (!tm) return;
        bool dark = lc::theme.dark;
        float w = box.size.x, h = box.size.y;
        float cx = w / 2.f;

        NVGcolor gutter = dark ? nvgRGB(18, 18, 18) : nvgRGB(238, 238, 238);
        NVGcolor rule   = dark ? nvgRGB(40, 40, 40) : nvgRGB(210, 210, 210);
        NVGcolor stroke = dark ? nvgRGB(220, 220, 220) : nvgRGB(40, 40, 40);

        nvgSave(args.vg);
        nvgScissor(args.vg, 0, 0, w, h);
        nvgBeginPath(args.vg); nvgRect(args.vg, 0, 0, w, h);
        nvgFillColor(args.vg, gutter); nvgFill(args.vg);

        nvgBeginPath(args.vg); nvgMoveTo(args.vg, cx, 0); nvgLineTo(args.vg, cx, h);
        nvgStrokeColor(args.vg, rule); nvgStrokeWidth(args.vg, 0.5f); nvgStroke(args.vg);

        int maxBins = (int)std::floor(h);
        int n = std::min((int)TakeModule::N_BINS, maxBins);
        if (n < 1) { nvgResetScissor(args.vg); nvgRestore(args.vg); return; }
        float binPx = h / (float)n;

        int writeIdx = (int)tm->binWriteIdx;
        int total = (int)TakeModule::N_BINS;
        for (int i = 0; i < n; i++) {
            int b = (writeIdx - 1 - i + total * 2) % total;
            float peak = std::max(tm->bins[b].l, tm->bins[b].r);
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

// ─── Widget layout ──────────────────────────────────────────────────────────

GrabPlusWidget::GrabPlusWidget(GrabPlusModule* module) {
    setModule(module);
    const int HP = 4;
    box.size = math::Vec(RACK_GRID_WIDTH * HP, RACK_GRID_HEIGHT);

    // Rack instantiates widgets with module == nullptr for the browser preview.
    // Resolve the inner pointers through that gate so widget null checks fire
    // correctly — `&module->grab` on a null module is a small bogus offset,
    // not nullptr, which defeats the `if (!gm)` guards inside each widget.
    GrabModule* gmPtr = module ? &module->grab : nullptr;
    TakeModule* tmPtr = module ? &module->take : nullptr;

    GrabPlusBackground* bg = new GrabPlusBackground;
    bg->box.size = box.size;
    addChild(bg);

    // Two centered screws (matching grab / take / tidy).
    float screwX = (box.size.x - RACK_GRID_WIDTH) / 2.f;
    addChild(createWidget<ScrewBlack>(math::Vec(screwX, 0)));
    addChild(createWidget<ScrewBlack>(math::Vec(screwX, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    app::PanelBorder* border = new app::PanelBorder;
    border->box.size = box.size;
    addChild(border);

    // Title
    {
        GrabPlusLabel* l = new GrabPlusLabel;
        l->text = "grab+";
        l->box.size = math::Vec(box.size.x, mm2px(3.f));
        l->box.pos  = math::Vec(0, mm2px(7.f));
        addChild(l);
    }

    // Two columns at 25% / 75% of the panel width. Buttons side-by-side at
    // the top, vis strips below each, jacks at the bottom.
    const float panelW_mm = (float)HP * RACK_GRID_WIDTH / mm2px(1.f);
    const float colL = panelW_mm * 0.25f;
    const float colR = panelW_mm * 0.75f;
    const float btnY = 14.f;
    const float btnSize = 5.f;

    {
        ModeCycleButton* t = new ModeCycleButton;
        t->gm = gmPtr;
        t->box.size = mm2px(math::Vec(btnSize, btnSize));
        t->box.pos  = math::Vec((box.size.x - t->box.size.x) / 2.f, mm2px(btnY));
        addChild(t);
    }

    // Vis row: L peak column on the far left, waveform centred, R peak column
    // on the far right. The peak-meter widget is still a two-column meter by
    // default; here we pin each instance to one side so the L reading sits
    // over the L jack column and R over the R jack column.
    const float visY = 22.f;
    const float visH = 58.f;
    const float meterW = 2.f;
    const float waveW  = 8.f;
    const float meterInsetMM = 4.f;   // distance from panel edge to meter centre
    {
        PeakMeter* m = new PeakMeter;
        m->gm = gmPtr;
        m->onlyChannel = 0;
        m->box.size = mm2px(math::Vec(meterW, visH));
        m->box.pos  = math::Vec(mm2px(meterInsetMM) - m->box.size.x / 2.f,
                                mm2px(visY));
        addChild(m);
    }
    {
        WaveformStrip* w = new WaveformStrip;
        w->tm = tmPtr;
        w->box.size = mm2px(math::Vec(waveW, visH));
        w->box.pos  = math::Vec((box.size.x - w->box.size.x) / 2.f, mm2px(visY));
        addChild(w);
    }
    {
        PeakMeter* m = new PeakMeter;
        m->gm = gmPtr;
        m->onlyChannel = 1;
        m->box.size = mm2px(math::Vec(meterW, visH));
        m->box.pos  = math::Vec(mm2px(panelW_mm - meterInsetMM) - m->box.size.x / 2.f,
                                mm2px(visY));
        addChild(m);
    }

    // L/R jacks side-by-side — pushed down so the big rec button has room.
    const float jackY = 102.f;
    const float jackLabelTop = jackY - 4.f;

    // Centre dual-action button — oversized chassis (2× the arm toggle) with
    // a pinned small LED so it still reads as an indicator dot. Short click
    // toggles force-rec; long click fires a take save (amber flash).
    // Positioned so its centre lands halfway between the waveform's bottom
    // and the L/R labels' top.
    const float recBtnSize = 13.f;
    const float recBtnY    = ((visY + visH) + jackLabelTop - recBtnSize) / 2.f;
    {
        ForceRecButton* b = new ForceRecButton;
        b->gm = gmPtr;
        b->tm = tmPtr;
        b->box.size = mm2px(math::Vec(recBtnSize, recBtnSize));
        b->box.pos  = math::Vec((box.size.x - b->box.size.x) / 2.f,
                                mm2px(recBtnY));
        addChild(b);
    }
    auto placeLabeledJack = [&](const std::string& txt, float cx, int portId) {
        GrabPlusLabel* l = new GrabPlusLabel;
        l->text = txt;
        l->fontSize = 9.f;
        l->box.size = math::Vec(mm2px(8.f), mm2px(3.5f));
        l->box.pos  = math::Vec(mm2px(cx) - l->box.size.x / 2.f, mm2px(jackY - 4.f));
        addChild(l);
        addInput(createInputCentered<lc::WhiteRingPJ301MPort>(
            mm2px(math::Vec(cx, jackY + 4.f)), module, portId));
    };
    placeLabeledJack("L", colL, GrabPlusModule::IN_L);
    placeLabeledJack("R", colR, GrabPlusModule::IN_R);

    // Logo at bottom
    {
        GrabPlusLogo* lg = new GrabPlusLogo;
        lg->path     = asset::plugin(pluginInstance, "res/lc-icon-new.png");
        lg->darkPath = asset::plugin(pluginInstance, "res/lc-icon-white.png");

        lg->greyPath = asset::plugin(pluginInstance, "res/lc-icon-grey.png");
        lg->box.size = mm2px(Vec(9.f, 9.f));
        lg->box.pos  = math::Vec((box.size.x - lg->box.size.x) / 2.f,
                                 mm2px(128.5f - 8.f - 9.f + 2.f));
        addChild(lg);
    }
}

void GrabPlusWidget::step() {
    ModuleWidget::step();
    GrabPlusModule* m = dynamic_cast<GrabPlusModule*>(module);
    if (!m) return;
    if (m->grab.spareNeedsRealloc.exchange(false, std::memory_order_relaxed)) {
        m->grab.spareBuf = std::vector<float>();
        m->grab.spareBuf.reserve(m->grab.capSamples);
    }
    // Refresh subfolder + filename prefixes when the patch path changes
    // (i.e. first save, save-as). UI-rate check is cheap — string compare
    // on a field that's usually stable.
    std::string now = (APP && APP->patch) ? APP->patch->path : std::string();
    if (now != m->cachedPatchPath) m->updateSubfolder();
}

// ─── Context menu widgets ───────────────────────────────────────────────────
//
// These mirror the anonymous-namespace classes in GrabMenu.cpp / Take.cpp,
// duplicated here because those sit inside their own translation units and
// aren't visible to us. A future cleanup could lift the shared slider helpers
// into a header once we have a third consumer (probably this third consumer).

namespace {

struct RangedQuantity : Quantity {
    float* ptr;
    std::string label, unit;
    float minV, maxV, defV;
    int precision;
    RangedQuantity(float* p, std::string l, std::string u,
                   float mn, float mx, float dv, int pr = 1)
        : ptr(p), label(std::move(l)), unit(std::move(u)),
          minV(mn), maxV(mx), defV(dv), precision(pr) {}
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

// Rebuild quantities — grab and take resize different ring buffers on change,
// so the two variants route to their respective rebuild method.
struct GrabRebuildQuantity : RangedQuantity {
    GrabModule* gm;
    GrabRebuildQuantity(GrabModule* g, float* p, std::string l, std::string u,
                        float mn, float mx, float dv, int pr = 1)
        : RangedQuantity(p, std::move(l), std::move(u), mn, mx, dv, pr), gm(g) {}
    void setValue(float v) override {
        float c = math::clamp(v, minV, maxV);
        if (c != *ptr) { *ptr = c; if (gm) gm->rebuildBuffers(); }
    }
};

struct TakeRebuildQuantity : RangedQuantity {
    TakeModule* tm;
    TakeRebuildQuantity(TakeModule* t, float* p, std::string l, std::string u,
                        float mn, float mx, float dv, int pr = 1)
        : RangedQuantity(p, std::move(l), std::move(u), mn, mx, dv, pr), tm(t) {}
    void setValue(float v) override {
        float c = math::clamp(v, minV, maxV);
        if (c != *ptr) { *ptr = c; if (tm) tm->rebuildBuffer(); }
    }
};

struct GrabPrefixField : ui::TextField {
    GrabModule* gm = nullptr;
    GrabPrefixField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (gm) gm->prefix = text;
    }
};

struct TakePrefixField : ui::TextField {
    TakeModule* tm = nullptr;
    TakePrefixField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (tm) tm->prefix = text;
    }
};

// One folder for both — they write files with different prefixes so having
// them share a destination is the ergonomic default.
struct SharedDirField : ui::TextField {
    GrabPlusModule* m = nullptr;
    SharedDirField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (m) m->setOutputDir(text);
    }
};

struct GrabBitDepthMenu : MenuItem {
    GrabModule* gm;
    Menu* createChildMenu() override {
        Menu* menu = new Menu;
        auto add = [&](const std::string& name, int v) {
            menu->addChild(createMenuItem(name, CHECKMARK(gm->bitDepth == v),
                                          [this, v]() { gm->bitDepth = v; }));
        };
        add("16-bit PCM", 16);
        add("24-bit PCM", 24);
        add("32-bit float", 0);
        return menu;
    }
};

struct TakeBitDepthMenu : MenuItem {
    TakeModule* tm;
    Menu* createChildMenu() override {
        Menu* menu = new Menu;
        auto add = [&](const std::string& name, int v) {
            menu->addChild(createMenuItem(name, CHECKMARK(tm->bitDepth == v),
                                          [this, v]() { tm->bitDepth = v; }));
        };
        add("16-bit PCM", 16);
        add("24-bit PCM", 24);
        add("32-bit float", 0);
        return menu;
    }
};

// Submenus keep the top-level context menu short; dive in for mode-specific
// knobs.
struct GrabSettingsMenu : MenuItem {
    GrabPlusModule* m;
    Menu* createChildMenu() override {
        GrabModule* gm = &m->grab;
        Menu* menu = new Menu;

        menu->addChild(new RangedSlider(new RangedQuantity(
            &gm->thresholdDb, "Threshold", " dB", -80.f, 0.f, -65.f, 1)));
        menu->addChild(new RangedSlider(new RangedQuantity(
            &gm->hangoverMs, "Hangover", " ms", 0.f, 5000.f, 250.f, 0)));
        menu->addChild(new RangedSlider(new GrabRebuildQuantity(
            gm, &gm->preRollMs, "Pre-roll", " ms", 0.f, 500.f, 100.f, 0)));
        menu->addChild(new RangedSlider(new RangedQuantity(
            &gm->fadeInMs,  "Fade in",  " ms", 0.f, 50.f, 0.f, 1)));
        menu->addChild(new RangedSlider(new RangedQuantity(
            &gm->fadeOutMs, "Fade out", " ms", 0.f, 50.f, 0.f, 1)));
        menu->addChild(new RangedSlider(new GrabRebuildQuantity(
            gm, &gm->maxTakeSec, "Max take", " s", 1.f, 300.f, 60.f, 0)));

        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Normalize to 0 dB", "", &gm->normalize));
        GrabBitDepthMenu* bd = createMenuItem<GrabBitDepthMenu>("Bit depth",
            std::string(gm->bitDepth == 0 ? "float32" :
                        gm->bitDepth == 16 ? "16-bit" : "24-bit") + "  " + RIGHT_ARROW);
        bd->gm = gm;
        menu->addChild(bd);

        return menu;
    }
};

struct SnipSettingsMenu : MenuItem {
    GrabPlusModule* m;
    Menu* createChildMenu() override {
        TakeModule* tm = &m->take;
        Menu* menu = new Menu;
        menu->addChild(new RangedSlider(new RangedQuantity(
            &tm->snipThreshDb,   "Silence threshold", " dB", -80.f, -20.f, -60.f, 0)));
        menu->addChild(new RangedSlider(new RangedQuantity(
            &tm->snipHangoverMs, "Hangover",          " ms",   0.f, 1000.f, 100.f, 0)));
        return menu;
    }
};

struct TakeSettingsMenu : MenuItem {
    GrabPlusModule* m;
    Menu* createChildMenu() override {
        TakeModule* tm = &m->take;
        Menu* menu = new Menu;

        menu->addChild(new RangedSlider(new TakeRebuildQuantity(
            tm, &tm->bufferSec, "Buffer length", " s", 10.f, 300.f, 60.f, 0)));
        menu->addChild(new RangedSlider(new RangedQuantity(
            &tm->fadeInMs,  "Fade in",  " ms", 0.f, 50.f, 0.f, 1)));
        menu->addChild(new RangedSlider(new RangedQuantity(
            &tm->fadeOutMs, "Fade out", " ms", 0.f, 50.f, 0.f, 1)));

        menu->addChild(new MenuSeparator);
        menu->addChild(createBoolPtrMenuItem("Normalize to 0 dB", "", &tm->normalize));
        TakeBitDepthMenu* bd = createMenuItem<TakeBitDepthMenu>("Bit depth",
            std::string(tm->bitDepth == 0 ? "float32" :
                        tm->bitDepth == 16 ? "16-bit" : "24-bit") + "  " + RIGHT_ARROW);
        bd->tm = tm;
        menu->addChild(bd);

        return menu;
    }
};

} // namespace

void GrabPlusWidget::appendContextMenu(Menu* menu) {
    GrabPlusModule* m = dynamic_cast<GrabPlusModule*>(module);
    if (!m) return;

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Save take now", "",
        [m]() { m->take.saveTake(); }));

    // Mode selector — mirrors the unlabelled left-panel cycle button.
    int curMode = m->grab.mode.load(std::memory_order_relaxed);
    const char* modeLabel =
        curMode == GrabModule::MODE_GRAB ? "grab" :
        curMode == GrabModule::MODE_SNIP ? "snip" : "off";

    struct ModeItem : MenuItem {
        GrabPlusModule* m;
        int v;
        void onAction(const event::Action& e) override {
            if (m) m->grab.mode.store(v, std::memory_order_relaxed);
        }
    };

    auto modeRow = [&](const std::string& name, const std::string& desc, int v) {
        ModeItem* it = new ModeItem;
        it->m = m;
        it->v = v;
        it->text = name;
        it->rightText = desc;
        if (curMode == v) it->rightText = std::string(CHECKMARK_STRING) + "  " + desc;
        menu->addChild(it);
    };

    menu->addChild(createMenuLabel(std::string("Mode — ") + modeLabel));
    modeRow("Off",  "",                             GrabModule::MODE_OFF);
    modeRow("Grab", "auto oneshot recording",       GrabModule::MODE_GRAB);
    modeRow("Snip", "silence-gated rolling buffer", GrabModule::MODE_SNIP);

    menu->addChild(new MenuSeparator);
    GrabSettingsMenu* gs = createMenuItem<GrabSettingsMenu>(
        "Grab settings", RIGHT_ARROW);
    gs->m = m;
    menu->addChild(gs);

    TakeSettingsMenu* ts = createMenuItem<TakeSettingsMenu>(
        "Take settings", RIGHT_ARROW);
    ts->m = m;
    menu->addChild(ts);

    SnipSettingsMenu* ss = createMenuItem<SnipSettingsMenu>(
        "Snip settings", RIGHT_ARROW);
    ss->m = m;
    menu->addChild(ss);

    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuLabel("Output directory"));
    SharedDirField* df = new SharedDirField;
    df->m = m;
    // Show grab's as the canonical value — they're kept in sync on edit.
    df->text = m->grab.outputDir;
    menu->addChild(df);

    menu->addChild(createMenuItem("Choose folder…", "", [m]() {
        char* p = osdialog_file(OSDIALOG_OPEN_DIR, nullptr, nullptr, nullptr);
        if (p) {
            m->setOutputDir(p);
            std::free(p);
        }
    }));

    // Sticky toggle: when on, files land in a dated "<patch>_<dd>_<mm>_<yyyy>"
    // subfolder under outputDir. The subfolder name is refreshed each time
    // the flag is applied, so flipping it off-on later gets a fresh date.
    menu->addChild(createMenuItem(
        "Save to sub folder",
        CHECKMARK(m->useDatedSubfolder),
        [m]() {
            m->useDatedSubfolder = !m->useDatedSubfolder;
            m->updateSubfolder();
        }));
    menu->addChild(createMenuItem("Reveal output folder", "", [m]() {
        // Open the raw outputDir, not the per-type nested path — the
        // grabs / recs / takes / dated subfolders should only appear
        // after an actual recording lands.
        std::string d = m->grab.outputDir;
        if (d.empty()) d = asset::plugin(pluginInstance, "test");
        else if (d[0] != '/' && !(d.size() > 1 && d[1] == ':'))
            d = asset::plugin(pluginInstance, d);
        rack::system::createDirectories(d);
        rack::system::openDirectory(d);
    }));

    menu->addChild(new MenuSeparator);
    lc::appendThemeMenu(menu);
}

Model* modelGrabPlus = createModel<GrabPlusModule, GrabPlusWidget>("grabplus");
