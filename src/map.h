#pragma once
// MapSpawns: spawn dots and labels on the native map window. Tables and prepare check for patches.cpp.
#include "patches.h"

extern const HookSite kMapSpawnsHooks[1];
extern const RefSite kMapSpawnsRefs[17];
const char* MapSpawnsPrepare();
