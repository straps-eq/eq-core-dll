#pragma once
// FloatingCombatText: damage numbers rising from spawns in the 3D world, fed by the OP_Damage handler and
// drawn from the DrawWindows hook the Hud also uses (see fct.cpp).
#include <cstdint>
#include "patches.h"

extern const HookSite kFctHooks[2];
extern const RefSite kFctRefs[9];
const char* FctPrepare();
// Called once per frame from the shared DrawWindows hook, after the client's windows are drawn.
void FctFrame(uint8_t* mgr, bool inGame);

// FCTOptionsPage: Legends' "Combat Text" tab in the Options window (patched EQUI_OptionsWindow.xml), bound to
// the [FloatingCombat] settings. Needs FloatingCombatText applied first.
extern const HookSite kFctPageHooks[2];
extern const RefSite kFctPageRefs[6];
const char* FctPagePrepare();
