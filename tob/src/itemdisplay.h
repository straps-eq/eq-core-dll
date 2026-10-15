#pragma once
// ItemDisplayExtras: spell descriptions, ids, value, tribute, ratio, lore, levels, container, clicky
// cooldown and per-effect spell details added to the item window text. Tables and prepare check for patches.cpp.
#include "patches.h"

extern const HookSite kItemDisplayExtrasHooks[1];
extern const RefSite kItemDisplayExtrasRefs[21];
const char* ItemDisplayExtrasPrepare();
