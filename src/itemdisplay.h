#pragma once
// ItemDisplayExtras: value, clicky cooldown and spell details appended to the item window text.
// Tables and prepare check for patches.cpp.
#include "patches.h"

extern const HookSite kItemDisplayExtrasHooks[1];
extern const RefSite kItemDisplayExtrasRefs[8];
const char* ItemDisplayExtrasPrepare();
