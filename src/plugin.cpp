#include "plugin.hpp"
#include "Theme.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
    pluginInstance = p;
    lc::loadTheme();
    p->addModel(modelNotes);
    p->addModel(modelTidy);
}
