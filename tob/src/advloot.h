#pragma once
// AdvLootActions: Sell / Store in the Advanced Loot window, per-item auto actions and sale/loot text.
// See advloot.cpp.
#include <cstdint>
#include "patches.h"

extern const HookSite kAdvLootHooks[6];
extern const RefSite kAdvLootRefs[7];
const char* AdvLootPrepare();
// AS / ST columns in the Edit Loot Filters window: a second table under the same ini key, so a changed window
// only switches the columns off while the loot window keeps its buttons and menu.
extern const HookSite kAdvLootFilterHooks[5];
extern const RefSite kAdvLootFilterRefs[17];
const char* AdvLootFiltersPrepare();
// Called once per frame from the shared DrawWindows hook (button visibility, auto filters).
void AdvLootFrame(bool inGame);
