#include "QArray.hpp"
#include "QMap.hpp"
#include "QMod.hpp"
#include "QModPlus.hpp"

namespace lc {

bool isQDevice(rack::engine::Module* m) {
    if (!m || !m->model) return false;
    return m->model == modelQMap
        || m->model == modelQMod
        || m->model == modelQModPlus;
}

bool getInArray(rack::engine::Module* m) {
    if (!m) return false;
    if (m->model == modelQMap) {
        auto* q = dynamic_cast<QMapModule*>(m);
        return q && q->inArray;
    }
    if (m->model == modelQMod) {
        auto* q = dynamic_cast<QModModule*>(m);
        return q && q->inArray;
    }
    if (m->model == modelQModPlus) {
        auto* q = dynamic_cast<QModPlusModule*>(m);
        return q && q->inArray;
    }
    return false;
}

std::vector<rack::engine::Module*> walkArray(rack::engine::Module* start) {
    std::vector<rack::engine::Module*> arr;
    if (!isQDevice(start)) return arr;
    if (!getInArray(start)) {
        arr.push_back(start);
        return arr;
    }
    // Walk left to find the leftmost member. The chain only extends through
    // Q devices that have inArray == true; any break stops the walk.
    rack::engine::Module* leftmost = start;
    while (true) {
        rack::engine::Module* next = leftmost->leftExpander.module;
        if (!isQDevice(next) || !getInArray(next)) break;
        leftmost = next;
    }
    // Walk right collecting every in-array Q device.
    rack::engine::Module* cur = leftmost;
    while (cur) {
        arr.push_back(cur);
        rack::engine::Module* next = cur->rightExpander.module;
        if (!isQDevice(next) || !getInArray(next)) break;
        cur = next;
    }
    return arr;
}

int indexInArray(rack::engine::Module* m) {
    auto arr = walkArray(m);
    for (size_t i = 0; i < arr.size(); i++) {
        if (arr[i] == m) return (int)i;
    }
    return -1;
}

int arraySlotBase(rack::engine::Module* m) {
    // Counts preceding modules in the same family as `m` so qmaps are
    // numbered independently from qmods/qmod+s. This lets a qmap slot "N"
    // line up positionally with a qmod slot "N" across the array, which is
    // how assignments and array-feeds are visually paired.
    if (!m) return 0;
    auto arr = walkArray(m);
    const bool mIsQMap = (m->model == modelQMap);
    int count = 0;
    for (auto* x : arr) {
        if (x == m) break;
        if (!x) continue;
        if (mIsQMap) {
            if (x->model == modelQMap) count++;
        } else {
            // qmod family = qmod OR qmod+
            if (x->model == modelQMod || x->model == modelQModPlus) count++;
        }
    }
    return count * 14;
}

} // namespace lc
