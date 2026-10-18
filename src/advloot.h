#pragma once
// AdvLootActions: Sell / Store in the Advanced Loot window, per-item auto actions and sale/loot text.
// See advloot.cpp.
#include <cstdint>
#include "patches.h"

extern const HookSite kAdvLootHooks[6];
extern const RefSite kAdvLootRefs[7];
const char* AdvLootPrepare();
// Called once per frame from the shared DrawWindows hook (button visibility, auto filters).
void AdvLootFrame(bool inGame);
