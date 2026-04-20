#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// jump — 9 saved-view bookmarks driven by one clickable dot.
//
//   Not armed: Cmd+1..9 jumps to slot.
//   Armed   : Cmd+1..9 saves current view into slot.  (arm by clicking the dot)
//   Cmd+[ / Cmd+]: navigation history (back / forward).
//
// Saved view = the rack-space rectangle that was visible in the viewport,
// restored via rackScroll->zoomToBound so zoom + offset are set atomically.
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <rack.hpp>
#include <string>

using namespace rack;

namespace jump {

// Saved-view format: the viewport rectangle in module (rack-space) coords.
// Restored via rackScroll->zoomToBound with a padding-compensated rect —
// zoomToBound reliably survives Rack's step() logic (direct offset writes
// don't), and the padding is a fixed ratio we can pre-compensate for.
struct Slot {
    bool        occupied = false;
    math::Rect  rackRect;     // viewport bounds in rack-space (at 1x zoom)
    std::string name;
};

struct View {
    math::Rect rackRect;
};

} // namespace jump

struct JumpOverlay;

struct JumpModule : Module {
    static constexpr int N_SLOTS = 9;
    std::array<jump::Slot, N_SLOTS> slots;

    JumpModule();
    json_t* dataToJson() override;
    void    dataFromJson(json_t* root) override;
};

struct JumpWidget : ModuleWidget {
    JumpOverlay* overlay = nullptr;

    JumpWidget(JumpModule* module);
    ~JumpWidget() override;

    void appendContextMenu(Menu* menu) override;
};
