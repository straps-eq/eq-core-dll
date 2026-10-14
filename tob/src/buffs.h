#pragma once
// BuffTooltips: caster, remaining time, effect summary and cure counters on buff/song/target buff tooltips.
// Tables and prepare check for patches.cpp.
#include "patches.h"

extern const HookSite kBuffTooltipsHooks[2];
extern const RefSite kBuffTooltipsRefs[20];
const char* BuffTooltipsPrepare();
