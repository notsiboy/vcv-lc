#include "plugin.hpp"
#include "Theme.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
    pluginInstance = p;
    lc::loadTheme();
    p->addModel(modelNotes);
    p->addModel(modelTidy);
    p->addModel(modelGrab);
    p->addModel(modelJump);
    p->addModel(modelTake);
    p->addModel(modelCapture);
}
