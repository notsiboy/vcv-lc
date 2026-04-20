#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// jump — saved-view bookmarks + fuzzy module finder for the rack.
//
// • 9 named slots, each a captured (gridOffset, zoom) pair.
// • Chord or overlay hotkey mode — toggle in right-click menu.
// • Back/forward navigation stack (Cmd+[ / Cmd+]).
// • Pulse-on-arrival highlight.
// • Fuzzy-search over every module in the rack.
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <rack.hpp>
#include <string>
#include <vector>

using namespace rack;

namespace jump {

struct Slot {
    bool        occupied = false;
    math::Vec   gridOffset;     // top-left of the scroll viewport, in grid units
    float       zoom     = 1.f;
    std::string name;           // short label, optional
};

struct View {
    math::Vec gridOffset;
    float     zoom = 1.f;
};

} // namespace jump

struct JumpOverlay;

struct JumpModule : Module {
    static constexpr int N_SLOTS = 9;
    std::array<jump::Slot, N_SLOTS> slots;

    enum InputMode : int {
        MODE_CHORD   = 0,   // Cmd+J  1..9 directly / Cmd+J /  for find
        MODE_OVERLAY = 1,   // Cmd+J opens a Raycast-style modal
    };
    int inputMode = MODE_CHORD;

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
