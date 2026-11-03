#pragma once
// ItemDisplayExtras: spell descriptions, ids, value, tribute, ratio, lore, levels, container, clicky
// cooldown and per-effect spell details added to the item window text. Tables and prepare check for patches.cpp.
#include <cstdint>
#include "patches.h"

extern const HookSite kItemDisplayExtrasHooks[1];
extern const RefSite kItemDisplayExtrasRefs[26];
const char* ItemDisplayExtrasPrepare();
// ItemHiddenStats: stats the ToB item format lacks, from the server (S2C/C2S 0x7e51), on the same finalise hook plus
// the shared packet dispatcher; uses kItemDisplayExtrasRefs.
extern const HookSite kItemHiddenStatsHooks[5];
const char* ItemHiddenStatsPrepare();
void ItemStatsOnPacket(uint32_t op, const uint8_t* data, uint32_t size);   // packet dispatcher, before the client
void ItemDisplayFrame(bool inGame);   // shared DrawWindows hook: rebuilds a window whose stats just arrived
// PowerSourceProgress: "Upgrade: 42.0%" in the item window for items the server lists by serial (S2C 0x7e52 kind 2,
// routed by heromods.cpp); the finalise, UpdateStrings, DrawWindows and dispatcher hooks; uses kItemDisplayExtrasRefs.
extern const HookSite kPowerSourceHooks[4];
const char* PowerSourceProgressPrepare();
void ItemUpgradeOnPacket(const uint8_t* data, uint32_t size);
