#include "plugin.hpp"
#include "Theme.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
    pluginInstance = p;
    lc::loadTheme();
    p->addModel(modelNotes);
    p->addModel(modelTidy);
    p->addModel(modelGrab);
    p->addModel(modelTake);
    p->addModel(modelCapture);
    p->addModel(modelGrab2);
    p->addModel(modelQMap);
}
