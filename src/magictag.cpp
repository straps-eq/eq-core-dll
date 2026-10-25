// ItemMagicTag: "Magic" back at the front of the item display's flags line ("Magic, Lore, No Trade, ...").
//
// ToB builds that line in FUN_14041b100(CXStr* out, ItemPtr* item) (item window FUN_14041c1e0 at 0x14041c3f2 and the
// item tooltip): eqstr ids joined with ", " starting at 0xc17 Lore, then the trailing ", " trimmed (FUN_140588220).
// The Magic check (eqstr 3094 = 0xc16) is gone from the client and from ToB's eqstr_us.txt, while the server still
// sends the flag: ItemDefinition +0x150 (u8, the deserialiser's magic byte). The detour runs the original and, for
// a magic item, replaces the result with "Magic, " + it ("Magic" alone when it is empty). The text is hard-coded
// because the eqstr is gone. The compare window's variant FUN_14041ac00 works on a precomputed flag array and is left
// alone.
#include "magictag.h"
#include "hud.h"
#include <cstdint>
#include <cstdio>

namespace {

const size_t kItemVtblGetDef = 0x28;     // ItemDefinition* GetItemDefinition()
const size_t kDefMagic = 0x150;          // u8

struct ItemPtr {
	uint8_t* item;
	void* control;
};

using FlagsFn = void** (*)(void** out, ItemPtr* item);
using DefFn = const uint8_t* (*)(uint8_t* item);
using CStrFn = const char* (*)(void** str);

FlagsFn g_origFlags = nullptr;   // FUN_14041b100
CStrFn g_cxstrCStr = nullptr;    // FUN_1405859f0
bool g_on = false;

void** FlagsDetour(void** out, ItemPtr* item) {
	void** result = g_origFlags(out, item);
	if (!g_on || !out || !item || !item->item) return result;
	uint8_t* obj = item->item;
	const uint8_t* def = reinterpret_cast<DefFn>((*reinterpret_cast<void***>(obj))[kItemVtblGetDef / sizeof(void*)])(obj);
	if (!def || !def[kDefMagic]) return result;
	const char* old = *out ? g_cxstrCStr(out) : nullptr;
	char text[512];
	if (old && old[0]) snprintf(text, sizeof(text), "Magic, %s", old);
	else snprintf(text, sizeof(text), "Magic");
	void* fresh = nullptr;
	HudCXStr(&fresh, text);
	if (!fresh) return result;
	HudCXStrRelease(out);
	*out = fresh;
	return result;
}

#define REF(var) reinterpret_cast<void**>(&var)

} // namespace

const HookSite kItemMagicTagHooks[1] = {
	// FUN_14041b100(CXStr* out, ItemPtr* item). Stolen (5, position independent): mov [rsp+8],rbx.
	{ "48 89 5C 24 08 48 89 74 24 18 55 57 41 56 48 8B EC", 5, reinterpret_cast<void*>(&FlagsDetour), REF(g_origFlags) },
};

const RefSite kItemMagicTagRefs[1] = {
	// CXStr -> const char* FUN_1405859f0
	{ "40 53 48 83 EC 20 48 8B 01 48 8B D9 48 85 C0 74 ?? 83 78 0C 00", 0, 0, REF(g_cxstrCStr) },
};

const char* ItemMagicTagPrepare() {
	if (const char* e = HudTextPrepare()) return e;
	g_on = true;
	return nullptr;
}
