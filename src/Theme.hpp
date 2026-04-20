#pragma once
#include <rack.hpp>

// Shared across all Lux Cache modules. Persisted to
// <Rack user dir>/LuxCache/theme.json so the preference outlives patches
// and restarts. Toggling from any module's right-click menu updates the
// singleton; every other LC module picks up the change on the next frame.

namespace lc {

struct Theme {
    bool dark = false;
};

extern Theme theme;

void loadTheme();
void saveTheme();

} // namespace lc
