#include "plugin.hpp"
#include "Theme.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
    pluginInstance = p;
    lc::loadTheme();
    p->addModel(modelNotes);
    p->addModel(modelTidy);
    p->addModel(modelCapture);
    p->addModel(modelGrabPlus);
    p->addModel(modelQMap);
    p->addModel(modelQMod);
    p->addModel(modelQModPlus);
}
