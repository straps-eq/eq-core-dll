#pragma once
// SpellWindowExtras: description, id/duration/times/range/resist, effect slots, classes, category and
// icon added to the spell window text.
// Tables and prepare check for patches.cpp.
#include "patches.h"

extern const HookSite kSpellWindowExtrasHooks[1];
extern const RefSite kSpellWindowExtrasRefs[17];
const char* SpellWindowExtrasPrepare();
