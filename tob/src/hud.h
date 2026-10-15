#pragma once
// Hud: on-screen text drawn from CXWndManager::DrawWindows. TargetDistance: a line drawn from the
// Target window's own Draw. Both share the ref table and the prepare checks; each has its own hook.
#include "patches.h"

extern const HookSite kHudHooks[1];
extern const HookSite kTargetDistanceHooks[1];
extern const RefSite kHudRefs[22];
const char* HudPrepare();
const char* TargetDistancePrepare();
