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
