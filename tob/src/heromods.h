#pragma once
// HeroicModsFromServer: the Stats tab's Heroic Mods rows (Spell Shield ... Strike Through) show the value and cap the
// server sends, instead of the numbers ToB works out from heroic stats. See heromods.cpp.
#include <cstdint>
#include "patches.h"

// S2C 0x7e52 "client extras": the first byte is the kind (1 heroic mods, here; 2 Power Source progress, itemdisplay.cpp).
const uint32_t kOpClientExtras = 0x7e52;

extern const HookSite kHeroicModsHooks[2];
extern const RefSite kHeroicModsRefs[1];
const char* HeroicModsPrepare();

// From the shared packet dispatcher, before the client: routes both kinds of kOpClientExtras.
void ClientExtrasOnPacket(uint32_t op, const uint8_t* data, uint32_t size);
