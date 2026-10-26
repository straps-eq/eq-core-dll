#pragma once
// ItemDisplayExtras: spell descriptions, ids, value, tribute, ratio, lore, levels, container, clicky
// cooldown and per-effect spell details added to the item window text. Tables and prepare check for patches.cpp.
#include <cstdint>
#include "patches.h"

extern const HookSite kItemDisplayExtrasHooks[1];
extern const RefSite kItemDisplayExtrasRefs[23];
const char* ItemDisplayExtrasPrepare();
// ItemHiddenStats: stats the ToB item format lacks, from the server (S2C/C2S 0x7e51), on the same finalise hook plus
// the shared packet dispatcher; uses kItemDisplayExtrasRefs.
extern const HookSite kItemHiddenStatsHooks[2];
const char* ItemHiddenStatsPrepare();
void ItemStatsOnPacket(uint32_t op, const uint8_t* data, uint32_t size);   // packet dispatcher, before the client
