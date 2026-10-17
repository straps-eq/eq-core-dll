#pragma once
// ConNameTags: colour NPC name tags by con colour. Tables and prepare check for patches.cpp.
#include "patches.h"

extern const HookSite kConNameTagsHooks[1];
extern const RefSite kConNameTagsRefs[10];
const char* ConNameTagsPrepare();
// The Nameplates options page's ConColor box: whether the feature was applied, and its live switch (re-tints
// every spawn through the client's own SetNameSpriteTint).
bool ConNameTagsApplied();
bool ConNameTagsEnabled();
void ConNameTagsSetEnabled(bool on);
