#include "Grab.hpp"
#include "Theme.hpp"
#include <osdialog.h>

namespace {

// ─── Slider quantities ──────────────────────────────────────────────────────

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
    // Force fixed-point formatting; Quantity's default uses %g which flips
    // to scientific notation at larger magnitudes (showing e.g. "-4e+01").
    std::string getDisplayValueString() override {
        return string::f("%.*f", precision, getDisplayValue());
    }
};

struct RangedSlider : ui::Slider {
    RangedSlider(Quantity* q) { quantity = q; box.size.x = 200.f; }
    ~RangedSlider() override { delete quantity; }
};

// ─── Rebuild-on-change slider (for params that resize the buffers) ──────────

struct RebuildQuantity : RangedQuantity {
    GrabModule* gm;
    RebuildQuantity(GrabModule* g, float* p, std::string l, std::string u,
                    float mn, float mx, float dv, int pr = 1)
        : RangedQuantity(p, std::move(l), std::move(u), mn, mx, dv, pr), gm(g) {}
    void setValue(float v) override {
        float clamped = math::clamp(v, minV, maxV);
        if (clamped != *ptr) {
            *ptr = clamped;
            if (gm) gm->rebuildBuffers();
        }
    }
};

// ─── Text fields (prefix / output dir) ──────────────────────────────────────

struct PrefixField : ui::TextField {
    GrabModule* gm = nullptr;
    PrefixField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (gm) gm->prefix = text;
    }
};

struct DirField : ui::TextField {
    GrabModule* gm = nullptr;
    DirField() { box.size.x = 200.f; }
    void onChange(const event::Change& e) override {
        ui::TextField::onChange(e);
        if (gm) gm->outputDir = text;
    }
};

// ─── Bit-depth submenu ──────────────────────────────────────────────────────

struct BitDepthMenu : MenuItem {
    GrabModule* gm;
    Menu* createChildMenu() override {
        Menu* m = new Menu;
        auto add = [&](const std::string& name, int v) {
            m->addChild(createMenuItem(name, CHECKMARK(gm->bitDepth == v), [this, v]() {
                gm->bitDepth = v;
            }));
        };
        add("16-bit PCM", 16);
        add("24-bit PCM", 24);
        add("32-bit float", 0);
        return m;
    }
};

} // namespace

void GrabWidget::appendContextMenu(Menu* menu) {
    GrabModule* gm = dynamic_cast<GrabModule*>(module);
    if (!gm) return;

    menu->addChild(new MenuSeparator);

    menu->addChild(new RangedSlider(new RangedQuantity(
        &gm->thresholdDb, "Threshold", " dB", -80.f, 0.f, -65.f, 1)));
    menu->addChild(new RangedSlider(new RangedQuantity(
        &gm->hangoverMs, "Hangover", " ms", 0.f, 5000.f, 250.f, 0)));
    menu->addChild(new RangedSlider(new RebuildQuantity(
        gm, &gm->preRollMs, "Pre-roll", " ms", 0.f, 500.f, 100.f, 0)));
    menu->addChild(new RangedSlider(new RangedQuantity(
        &gm->fadeInMs, "Fade in", " ms", 0.f, 50.f, 0.f, 1)));
    menu->addChild(new RangedSlider(new RangedQuantity(
        &gm->fadeOutMs, "Fade out", " ms", 0.f, 50.f, 0.f, 1)));
    menu->addChild(new RangedSlider(new RebuildQuantity(
        gm, &gm->maxTakeSec, "Max take", " s", 1.f, 300.f, 60.f, 0)));

    menu->addChild(new MenuSeparator);

    menu->addChild(createBoolPtrMenuItem("Normalize to 0 dB", "", &gm->normalize));
    BitDepthMenu* bd = createMenuItem<BitDepthMenu>("Bit depth",
        std::string(gm->bitDepth == 0 ? "float32" :
                    gm->bitDepth == 16 ? "16-bit" : "24-bit") + "  " + RIGHT_ARROW);
    bd->gm = gm;
    menu->addChild(bd);

    menu->addChild(new MenuSeparator);

    menu->addChild(createMenuLabel("Filename prefix"));
    PrefixField* pf = new PrefixField;
    pf->gm = gm;
    pf->text = gm->prefix;
    menu->addChild(pf);

    menu->addChild(createMenuLabel("Output directory"));
    DirField* df = new DirField;
    df->gm = gm;
    df->text = gm->outputDir;
    menu->addChild(df);

    menu->addChild(createMenuItem("Choose folder…", "", [gm]() {
        char* picked = osdialog_file(OSDIALOG_OPEN_DIR, nullptr, nullptr, nullptr);
        if (picked) {
            gm->outputDir = picked;
            std::free(picked);
        }
    }));
    menu->addChild(createMenuItem("Reveal output folder", "", [gm]() {
        std::string dir = gm->resolveOutputDir();
        rack::system::createDirectories(dir);
        rack::system::openDirectory(dir);
    }));

    menu->addChild(new MenuSeparator);

    menu->addChild(createMenuItem("Dark mode (shared)",
        CHECKMARK(lc::theme.dark), []() {
            lc::theme.dark = !lc::theme.dark;
            lc::saveTheme();
        }));
}
