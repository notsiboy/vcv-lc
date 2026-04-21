#include "Theme.hpp"
#include <cstdio>
#include <jansson.h>

using namespace rack;

namespace lc {

Theme theme;

static std::string themePath() {
    std::string dir = rack::asset::user("LuxCache");
    rack::system::createDirectories(dir);
    return dir + "/theme.json";
}

void loadTheme() {
    std::string path = themePath();
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    json_error_t err;
    json_t* root = json_loadf(f, 0, &err);
    std::fclose(f);
    if (!root) return;
    if (json_t* j = json_object_get(root, "dark"))
        theme.dark = json_boolean_value(j);
    if (json_t* j = json_object_get(root, "grey"))
        theme.grey = json_boolean_value(j);
    // Grey implies dark — coerce if loaded file is inconsistent.
    if (theme.grey) theme.dark = true;
    json_decref(root);
}

void saveTheme() {
    std::string path = themePath();
    json_t* root = json_object();
    json_object_set_new(root, "dark", json_boolean(theme.dark));
    json_object_set_new(root, "grey", json_boolean(theme.grey));
    FILE* f = std::fopen(path.c_str(), "w");
    if (f) {
        json_dumpf(root, f, JSON_INDENT(2));
        std::fclose(f);
    }
    json_decref(root);
}

NVGcolor panelBg() {
    if (theme.grey) return nvgRGB(53, 53, 53);   // #353535
    if (theme.dark) return nvgRGB(0, 0, 0);
    return nvgRGB(255, 255, 255);
}

std::string logoAsset(const std::string& lightPath,
                      const std::string& darkPath,
                      const std::string& greyPath) {
    if (theme.grey && !greyPath.empty()) return greyPath;
    if (theme.dark && !darkPath.empty()) return darkPath;
    return lightPath;
}

void appendThemeMenu(rack::ui::Menu* menu) {
    struct ThemeMenu : rack::ui::MenuItem {
        rack::ui::Menu* createChildMenu() override {
            rack::ui::Menu* m = new rack::ui::Menu;
            auto add = [&](const std::string& name, bool wantDark, bool wantGrey) {
                bool active = (theme.dark == wantDark && theme.grey == wantGrey);
                m->addChild(rack::createMenuItem(name, CHECKMARK(active),
                    [wantDark, wantGrey]() {
                        theme.dark = wantDark;
                        theme.grey = wantGrey;
                        saveTheme();
                    }));
            };
            add("Light", false, false);
            add("Dark",  true,  false);
            add("Grey",  true,  true);
            return m;
        }
    };
    ThemeMenu* tm = rack::createMenuItem<ThemeMenu>("Theme", RIGHT_ARROW);
    menu->addChild(tm);
}

} // namespace lc
