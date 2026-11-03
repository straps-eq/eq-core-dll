// HeroicModsFromServer: the Stats tab's Heroic Mods rows with the server's numbers.
//
// ToB items have no fields for these mods, so the client derives them from heroic stats: the rows are labels with
// EQTypes 215-224 (value) and 277-286 (cap), filled by the label text function FUN_1404ffba0(type, CXStr* out,
// bool* colourIt, u32* colour). Value: a PcClient member (pLocalPC +0x27f8..+0x281c, one per row). Cap: FUN_1400fb690
// (pc+0x27b8, type - 0x111, 1), in grey, or the higher cap (same call with 0) in gold once the value reaches it. Text
// goes through the CXStr printf FUN_140587480(out, "%d", n); value colour as FUN_14050c0b0 does it: 0 grey, above 0
// green, below 0 red.
// The detour runs the original, then for a row the server has sent writes the server's number into the same CXStr
// and sets the colour the same way (cap: gold when value >= cap). Rows the server never sent keep the client's text.
// The rows' tooltips (FUN_140509d20) still describe the client's own numbers.
//
// Wire, S2C kOpClientExtras 0x7e52 (no client handler or sender uses it), kind 1:
//   {u8 kind=1; u8 count; count x {u8 row; i32 value; i32 cap}}
//   row 0 Accuracy, 1 Avoidance, 2 Combat Effects, 3 Damage Shielding, 4 DS Mitigation, 5 DoT Shielding,
//   6 Melee Shielding, 7 Spell Shielding, 8 Strike Through, 9 Stun Resist. Value is already capped; both are shown
//   as sent. A packet sets the rows it carries.
#include "heromods.h"
#include "itemdisplay.h"
#include "nameplates.h"
#include <cstdint>
#include <cstring>

namespace {

const int kRows = 10;
const int kValueTypeFirst = 215, kCapTypeFirst = 277, kTypeCount = 10;
// Label EQType - 215 -> server row (EQUI_Inventory.xml IWS_Current*: SpellShield, CombatEffects, Shielding,
// DamageShielding, DoTShielding, DamageShieldMitigation, ItemAvoidance, ItemAccuracy, StunResist, StrikeThrough).
const int kTypeRow[kTypeCount] = { 7, 2, 6, 3, 5, 4, 1, 0, 9, 8 };

const uint32_t kColourGrey = 0xFFC0C0C0, kColourGreen = 0xFF00FF00, kColourRed = 0xFFFF0000, kColourGold = 0xFFD6B228;

struct Row {
	bool have;
	int32_t value;
	int32_t cap;
};
Row g_rows[kRows] = {};
bool g_on = false;

using LabelFn = uint8_t (*)(int type, void** out, uint8_t* colourIt, uint32_t* colour);
using CxPrintfFn = void (*)(void** out, const char* fmt, ...);
LabelFn g_origLabel = nullptr;   // FUN_1404ffba0
CxPrintfFn g_cxPrintf = nullptr; // FUN_140587480

uint8_t LabelDetour(int type, void** out, uint8_t* colourIt, uint32_t* colour) {
	const uint8_t result = g_origLabel(type, out, colourIt, colour);
	if (!g_on || !out) return result;
	const bool isValue = type >= kValueTypeFirst && type < kValueTypeFirst + kTypeCount;
	const bool isCap = type >= kCapTypeFirst && type < kCapTypeFirst + kTypeCount;
	if (!isValue && !isCap) return result;
	const Row& r = g_rows[kTypeRow[type - (isValue ? kValueTypeFirst : kCapTypeFirst)]];
	if (!r.have) return result;
	if (isValue) {
		g_cxPrintf(out, "%d", r.value);
		if (colour && colourIt && *colourIt) *colour = r.value == 0 ? kColourGrey : r.value > 0 ? kColourGreen : kColourRed;
	}
	else {
		g_cxPrintf(out, "%d", r.cap);
		if (colour) *colour = r.value >= r.cap ? kColourGold : kColourGrey;
	}
	return result;
}

// kind 1 {u8 kind; u8 count; count x {u8 row; i32 value; i32 cap}}
void OnHeroicMods(const uint8_t* data, uint32_t size) {
	if (size < 2) return;
	const uint8_t count = data[1];
	uint32_t at = 2;
	for (uint8_t i = 0; i < count && at + 9 <= size; ++i, at += 9) {
		const uint8_t row = data[at];
		if (row >= kRows) continue;
		memcpy(&g_rows[row].value, data + at + 1, 4);
		memcpy(&g_rows[row].cap, data + at + 5, 4);
		g_rows[row].have = true;
	}
}

} // namespace

const HookSite kHeroicModsHooks[2] = {
	// Label text FUN_1404ffba0(type, CXStr*, bool*, u32* colour). Stolen (5): mov [rsp+20],r9.
	{ "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 89 4C 24 08 48 81 EC E8 07 00 00 C7 84 24 B8 07 00 00 00 00 00 00 "
	  "C6 44 24 40 00",
	  5, reinterpret_cast<void*>(&LabelDetour), reinterpret_cast<void**>(&g_origLabel) },
	// Shared with Nameplates / EquipmentKeyRing / ItemHiddenStats: the packet dispatcher FUN_1401e4010.
	{ kDispatchSig, 5, reinterpret_cast<void*>(g_dispatchDetour), reinterpret_cast<void**>(g_origDispatchSlot) },
};

const RefSite kHeroicModsRefs[1] = {
	// CXStr printf FUN_140587480(CXStr*, fmt, ...): spills rdx/r8/r9, va_list, calls the worker FUN_1405874b0
	{ "48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 48 83 EC 38 48 8D 44 24 50 4C 8D 44 24 20 48 89 44 24 20 E8",
	  0, 0, reinterpret_cast<void**>(&g_cxPrintf) },
};

const char* HeroicModsPrepare() {
	g_on = true;
	return nullptr;
}

void ClientExtrasOnPacket(uint32_t op, const uint8_t* data, uint32_t size) {
	if (op != kOpClientExtras || !data || size < 1) return;
	if (data[0] == 1 && g_on) OnHeroicMods(data, size);
	else if (data[0] == 2) ItemUpgradeOnPacket(data, size);
}
