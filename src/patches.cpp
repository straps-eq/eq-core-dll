#include "patches.h"
#include "nametags.h"
#include "map.h"
#include "buffs.h"
#include "hud.h"
#include "itemdisplay.h"
#include "spellwindow.h"
#include "fct.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

// Solo LDoN: skip the "must be in a group of 2+" check in the adventure request window
// (FUN_1400be1b0, eqstr 0x1405). JMP lands on the group-leader check, same as the RoF2 patch,
// so a member of someone else's group still gets eqstr 0x1407.
static const PatchSite kSoloLDoN[] = {
	{ "48 8B 88 30 2E 00 00 48 85 C9 74 0C B2 01 E8 ?? ?? ?? ?? 83 F8 02 73 07 BA 05 14 00 00 EB 3E",
	  1, 10, "74 0C", "EB 13" },
};

// Damage applied twice: the server sends the reduced HP first, then OP_Damage, and the client
// subtracts the hit again (OP_Damage handler FUN_140216190). The client then runs one hit behind
// the server: a bouncing HP bar, and players knocked out locally at about half health while the
// server has them alive. Skip the client-side subtraction; OP_HPUpdate stays authoritative.
// (ToB twin of the old RoF2 isCombatDamageDoubleAppliedFixEnabled.)
static const PatchSite kNoDoubleDamage[] = {
	{ "48 F7 DA 48 81 C1 B8 27 00 00 C7 44 24 28 00 00 00 00 C7 44 24 20 00 00 00 00 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 44 0F B6 C6",
	  1, 26, "E8 F6 54 EE FF", "0F 1F 44 00 00" },
};

// Max HP above 10,000,000: remove the three client-side clamps.
static const PatchSite kMaxHP[] = {
	// Max HP calc FUN_1400fb4c0: CMOVG R8,RAX (RAX = 10,000,000) -> NOP
	{ "B8 80 96 98 00 48 8B 6C 24 68 4C 3B C0 48 8B 74 24 70 0F 28 74 24 30 4C 0F 4F C0 49 8B C0",
	  1, 23, "4C 0F 4F C0", "0F 1F 40 00" },
	// Current HP FUN_1400eae00: CMOVG RBX,RAX -> NOP
	{ "B8 80 96 98 00 48 3B D8 48 0F 4F D8 48 8B C3 48 8B 5C 24 68",
	  1, 8, "48 0F 4F D8", "0F 1F 40 00" },
	// HP delta applier FUN_1400fb7a0: MOV EBX,10M; CMP RAX,RBX; CMOVL RBX,RAX -> MOV RBX,RAX + NOPs
	{ "48 03 C3 BB 80 96 98 00 48 3B C3 48 0F 4C D8 48 C7 C0 80 69 67 FF 48 3B D8 48 0F 4C D8",
	  1, 3, "BB 80 96 98 00 48 3B C3 48 0F 4C D8", "48 8B D8 90 90 90 90 90 90 90 90 90" },
};

// AA below level 51: AA window (FUN_140356760) level gates 50/51 -> 0/1.
static const PatchSite kAAPre51[] = {
	{ "45 85 FF 75 ?? 41 80 FC 32 77 ?? 40 84 F6 74", 1, 8, "32", "00" },
	{ "45 85 FF 75 ?? 85 FF 74 ?? 41 80 FC 33 72",    1, 12, "33", "01" },
	{ "45 85 FF 75 ?? 83 FF 64 73 ?? 41 80 FC 33 72", 1, 13, "33", "01" },
};

// Optional, cosmetic: show the AA exp controls on the inventory window below 51 (FUN_1403fe480).
// One shared signature, exactly 10 sites. (The old RoF2 version crashed because it patched the
// ModRM byte instead of the immediate.)
static const PatchSite kAAPre51Inventory[] = {
	{ "FF 90 E8 00 00 00 83 F8 33 7C 04 B2 01 EB 02 33 D2", 10, 8, "33", "01" },
};

// Optional: allow augment combinations the character can't use (FUN_14042dcc0, FUN_1404323a0).
// Medium confidence: the usability check FUN_1400e9a80 was not fully decompiled.
static const PatchSite kIllegalAugments[] = {
	{ "E8 ?? ?? ?? ?? 84 C0 75 05 40 B6 01 EB 03 40 32 F6 F6 44 24 70 01",
	  1, 7, "75 05", "EB 05" },
	{ "E8 ?? ?? ?? ?? 84 C0 0F 84 8C 00 00 00 C6 44 24 20 01 45 33 C9 45 8B C5",
	  1, 7, "0F 84 8C 00 00 00", "66 0F 1F 44 00 00" },
};

// Optional: skip client hunger/thirst processing and messages (FUN_1400fef10). Like RoF2, this
// disables the whole client-side tick, not only the text.
static const PatchSite kFoodDrinkSpam[] = {
	{ "48 8B 41 10 45 33 F6 8B EA 48 8B F1 48 85 C0 0F 84 DA 03 00 00 44 38 B0 35 01 00 00",
	  1, 15, "0F 84 DA 03 00 00", "90 E9 DA 03 00 00" },
};

// Optional: start eqgame.exe without the "patchme" argument (WinMain FUN_140346ca0).
// Note: patches are applied while eqgame.exe loads its imports, before WinMain runs.
static const PatchSite kPatchme[] = {
	{ "44 38 7C 24 60 0F 84 E1 04 00 00 48 8D 55 E0",
	  1, 5, "0F 84 E1 04 00 00", "66 0F 1F 44 00 00" },
};

// Con-coloured NPC name tags: hook on EQPlayer::SetNameSpriteTint (FUN_1403014a0), see nametags.cpp.
static const Feature kFeatures[] = {
	{ "SoloLDoN",         true, "Solo LDoN adventures (no group needed)",          kSoloLDoN,         COUNT(kSoloLDoN) },
	{ "NoDoubleDamage",   true, "Don't apply melee damage to own HP twice",        kNoDoubleDamage,   COUNT(kNoDoubleDamage) },
	{ "MaxHPAbove10M",    true, "Remove the 10,000,000 HP caps",                   kMaxHP,            COUNT(kMaxHP) },
	{ "AAPre51",          true, "AA window usable below level 51",                 kAAPre51,          COUNT(kAAPre51) },
	{ "AAPre51Inventory", true, "Show AA exp on the inventory window below 51",    kAAPre51Inventory, COUNT(kAAPre51Inventory) },
	{ "IllegalAugments",  false, "Allow augment combinations the player can't use", kIllegalAugments,  COUNT(kIllegalAugments) },
	{ "NoFoodDrinkSpam",  false, "No hunger/thirst client processing or messages",  kFoodDrinkSpam,    COUNT(kFoodDrinkSpam) },
	{ "NoPatchme",        false, "Start without the patchme argument",              kPatchme,          COUNT(kPatchme) },
	{ "ConNameTags",      true, "Colour NPC name tags by con colour",              nullptr,           0,
	  kConNameTagsHooks, COUNT(kConNameTagsHooks), kConNameTagsRefs, COUNT(kConNameTagsRefs), ConNameTagsPrepare },
	// Spawn dots/labels and target line on the map: hook on MapViewMap::PostDraw (FUN_140455a60), see map.cpp.
	{ "MapSpawns",        true, "Spawns and target line on the map window",        nullptr,           0,
	  kMapSpawnsHooks, COUNT(kMapSpawnsHooks), kMapSpawnsRefs, COUNT(kMapSpawnsRefs), MapSpawnsPrepare },
	// Caster, effects and cure counters on buff tooltips: hooks on the buff/song (FUN_140390640) and
	// target (FUN_14050ec00) tooltip refresh, see buffs.cpp.
	{ "BuffTooltips",     true, "Caster, effects and cures on buff tooltips",      nullptr,           0,
	  kBuffTooltipsHooks, COUNT(kBuffTooltipsHooks), kBuffTooltipsRefs, COUNT(kBuffTooltipsRefs), BuffTooltipsPrepare },
	// Level/race/class and distance line in the Target window: hook on CTargetWnd::Draw (FUN_14050db60);
	// on-screen text: hook on CXWndManager::DrawWindows (FUN_1405cc3a0). Both in hud.cpp.
	{ "TargetDistance",   false, "Distance to the target in the Target window",     nullptr,           0,
	  kTargetDistanceHooks, COUNT(kTargetDistanceHooks), kHudRefs, COUNT(kHudRefs), TargetDistancePrepare },
	{ "Hud",              false, "On-screen loc, zone, target, FPS and clock",      nullptr,           0,
	  kHudHooks, COUNT(kHudHooks), kHudRefs, COUNT(kHudRefs), HudPrepare },
	// Damage numbers over spawns: hook on the OP_Damage handler (FUN_140216190) for the events and the Hud's
	// DrawWindows hook for drawing; reuses the Hud's text refs. See fct.cpp.
	{ "FloatingCombatText", true, "Damage numbers rising from spawns in the 3D world", nullptr, 0,
	  kFctHooks, COUNT(kFctHooks), kHudRefs, COUNT(kHudRefs), FctPrepare, kFctRefs, COUNT(kFctRefs) },
	// Legends' "Combat Text" tab in Options: hooks on COptionsWnd::RefreshDisplayPage (FUN_14049db70, UI load)
	// and COptionsWnd::WndNotification (FUN_14049f110). Needs the patched EQUI_OptionsWindow.xml. See fct.cpp.
	{ "FCTOptionsPage",   true, "Combat Text options page (Legends layout)",       nullptr,           0,
	  kFctPageHooks, COUNT(kFctPageHooks), kHudRefs, COUNT(kHudRefs), FctPagePrepare,
	  kFctPageRefs, COUNT(kFctPageRefs) },
	// Lore, value, tribute, levels, clicky cooldown and spell details in the item window: hook on the
	// CItemDisplayWnd text finalise step (FUN_1404340d0), see itemdisplay.cpp.
	{ "ItemDisplayExtras", false, "Lore, value, levels and spell details in the item window", nullptr, 0,
	  kItemDisplayExtrasHooks, COUNT(kItemDisplayExtrasHooks), kItemDisplayExtrasRefs, COUNT(kItemDisplayExtrasRefs), ItemDisplayExtrasPrepare },
	// Effect slots, timers, classes and category in the spell window: hook on the CSpellDisplayWnd text
	// finalise step (FUN_1404fcdc0), see spellwindow.cpp.
	{ "SpellWindowExtras", true, "Effects, timers, classes and category in the spell window", nullptr, 0,
	  kSpellWindowExtrasHooks, COUNT(kSpellWindowExtrasHooks), kSpellWindowExtrasRefs, COUNT(kSpellWindowExtrasRefs), SpellWindowExtrasPrepare },
};

const Feature* GetFeatures(size_t* count) {
	*count = COUNT(kFeatures);
	return kFeatures;
}
