#include "Theme.hpp"
#include <cstdio>
#include <jansson.h>

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
    json_decref(root);
}

void saveTheme() {
    std::string path = themePath();
    json_t* root = json_object();
    json_object_set_new(root, "dark", json_boolean(theme.dark));
    FILE* f = std::fopen(path.c_str(), "w");
    if (f) {
        json_dumpf(root, f, JSON_INDENT(2));
        std::fclose(f);
    }
    json_decref(root);
}

} // namespace lc
