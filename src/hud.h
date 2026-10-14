#pragma once
// Hud / TargetDistance: on-screen text drawn from CXWndManager::DrawWindows. Both features share the
// same hook and ref tables (the hook is installed once); each has its own prepare that turns it on.
#include "patches.h"

extern const HookSite kHudHooks[1];
extern const RefSite kHudRefs[15];
const char* HudPrepare();
const char* TargetDistancePrepare();
