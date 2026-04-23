#pragma once
#include <rack.hpp>
#include <componentlibrary.hpp>

namespace lc {

// PJ301M input port with the light-grey annulus around the jack hole
// repainted pure white. Lets every Lux Cache INPUT read as a distinct class
// at a glance, without needing a custom SVG.
struct WhiteRingPJ301MPort : rack::ThemedPJ301MPort {
    void draw(const DrawArgs& args) override {
        rack::ThemedPJ301MPort::draw(args);
        // Radii taken from Rack's PJ301M.svg (viewBox 23.7 × 23.7). The
        // annulus we recolour sits between the inner gradient ring
        // (r = 5.514) and the outer E0E0E0 disc (r = 6.800).
        float cx = box.size.x / 2.f;
        float cy = box.size.y / 2.f;
        float scale = box.size.x / 23.7f;
        float rOuter = 6.79996f * scale;
        float rInner = 5.51413f * scale;
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, cx, cy, rOuter);
        nvgCircle(args.vg, cx, cy, rInner);
        nvgPathWinding(args.vg, NVG_HOLE);
        nvgFillColor(args.vg, nvgRGB(255, 255, 255));
        nvgFill(args.vg);
    }
};

} // namespace lc
