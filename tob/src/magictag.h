#pragma once
// ItemMagicTag: "Magic" at the front of the item display's flags line. See magictag.cpp.
#include "patches.h"

extern const HookSite kItemMagicTagHooks[1];
extern const RefSite kItemMagicTagRefs[1];
const char* ItemMagicTagPrepare();
