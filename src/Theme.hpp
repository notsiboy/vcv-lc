#pragma once
#include <rack.hpp>
#include <string>

// Shared across all Lux Cache modules. Persisted to
// <Rack user dir>/LuxCache/theme.json so the preference outlives patches
// and restarts. Selecting a theme from any module's right-click menu updates
// the singleton; every other LC module picks up the change on the next frame.
//
// Three themes:
//   Light   — dark=false, grey=false   white panel
//   Dark    — dark=true,  grey=false   black panel
//   Grey    — dark=true,  grey=true    #353535 panel (borrows all dark-mode
//                                      colours everywhere except background
//                                      and logo asset)
//
// `grey` implies `dark`. Existing code that only checks `theme.dark` keeps
// working in grey mode (reads as "dark"). Grey-specific drawing opts in by
// checking `theme.grey` as well.

namespace lc {

struct Theme {
    bool dark = false;
    bool grey = false;
};

extern Theme theme;

void loadTheme();
void saveTheme();

// Convenience panel-bg colour for the current theme.
NVGcolor panelBg();

// Returns lightPath / darkPath / greyPath depending on theme. Empty fallbacks
// degrade gracefully (grey falls back to dark, dark to light).
std::string logoAsset(const std::string& lightPath,
                      const std::string& darkPath,
                      const std::string& greyPath);

// Appends a "Theme ▸" submenu with Light / Dark / Grey radio items. Wire
// this into every module's appendContextMenu instead of the old "Dark mode
// (shared)" item.
void appendThemeMenu(rack::ui::Menu* menu);

} // namespace lc
