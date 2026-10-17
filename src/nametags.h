#pragma once
// ConNameTags: colour NPC name tags by con colour. Tables and prepare check for patches.cpp.
#include "patches.h"

extern const HookSite kConNameTagsHooks[1];
extern const RefSite kConNameTagsRefs[10];
const char* ConNameTagsPrepare();
