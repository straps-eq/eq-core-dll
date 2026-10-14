#pragma once
// SpellWindowExtras: effect slots, timers, classes and category appended to the spell window text.
// Tables and prepare check for patches.cpp.
#include "patches.h"

extern const HookSite kSpellWindowExtrasHooks[1];
extern const RefSite kSpellWindowExtrasRefs[16];
const char* SpellWindowExtrasPrepare();
