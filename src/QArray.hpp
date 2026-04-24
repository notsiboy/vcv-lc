#pragma once
#include "plugin.hpp"
#include <vector>

// Shared helpers for the Q-family array feature. A Q-array is a contiguous
// horizontal run of qmap / qmod / qmod+ modules placed edge-to-edge; each
// member exposes a bool `inArray` so the user can opt a module out and
// break the chain at that point.

namespace lc {

// Returns true if `m` is one of the LC Q-family modules that participate in
// arrays.
bool isQDevice(rack::engine::Module* m);

// Returns `m`'s in-array opt-in flag. For non-Q modules, returns false — so
// walking stops at the first non-Q neighbour. For a Q module with `inArray
// == false`, walking also stops at the module itself (the opted-out module
// becomes its own singleton).
bool getInArray(rack::engine::Module* m);

// Returns the contiguous Q-array that contains `start`, ordered left-to-
// right (leftmost first). Empty vector if `start` is not a Q device. If
// `start` is Q but has `inArray == false`, the returned vector contains
// only `start`.
std::vector<rack::engine::Module*> walkArray(rack::engine::Module* start);

// Returns `m`'s 0-based index within its Q-array, or -1 if not found.
int indexInArray(rack::engine::Module* m);

// For slot numbering across an array, every Q device has 14 slots. This
// returns `indexInArray(m) * 14` as the base to add to a per-module slot
// index to get its global number. Returns 0 if `m` isn't in an array.
int arraySlotBase(rack::engine::Module* m);

} // namespace lc
