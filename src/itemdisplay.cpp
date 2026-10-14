// ItemDisplayExtras: appends a few lines to the item window's description text (the "ItemDescription"
// STML): the item's coin value, how long a clicky still has to cool down, and for each click / proc /
// worn / focus / scroll / blessing effect the spell's name and a compact summary of its effect slots.
//
// The window (CItemDisplayWnd) builds its text in SetItem (0x1404314e0) -> UpdateStrings
// (FUN_14041c1e0) -> FUN_1404340d0, the finalise step that pushes the assembled CXStr into the STML
// with CStmlWnd::SetSTMLText (FUN_1405ca040, append = 0). The finalise step also runs on its own when
// server text (lore, item information) arrives or the window is refreshed, so we hook it rather than
// SetItem: every run of the original replaces the whole text, and we append ours once afterwards with
// the same client call in append mode (FUN_1405ca040(stml, &cxstr, 1, 0)). The CXStr is built with the
// client's own constructor (FUN_1405853f0) and consumed (released) by SetSTMLText, so nothing leaks.
//
// Data read: ItemClient (window+0x3E8) -> ItemDefinition via vtbl+0x28; effects at def+0x220, 7 entries of
// 0x64 bytes in the server's serialisation order (click, proc, worn, focus, scroll, focus2, blessing);
// value via ItemBase::GetCost (FUN_14064f280, def+0xC0 in copper); recast ready times at ItemClient+0x14
// and PcProfile+0x4CC4+type*4 (both unix seconds, written by the OP_ItemRecastDelay handler
// FUN_1402ed830 / FUN_14066ece0); spells from the spell manager (DAT_140ee6480, vtbl+0x40 GetSpellByID,
// vtbl+0x48 effect slot by index, slot count/base at spell+0x64/+0x60, SPA at slot+0x24, base at slot+8).
#include "itemdisplay.h"
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace {

// CItemDisplayWnd fields (ctor FUN_1404119e0, SetItem, finalise).
const size_t kWndDescription = 0x2c8;   // CStmlWnd* "ItemDescription"
const size_t kWndItem = 0x3e8;          // ItemPtr: ItemClient* at +0
const size_t kWndHasItem = 0x3f8;       // u8
const size_t kGameState = 0x5e4;        // int at *DAT_140ee7010
const int kGameStateInZone = 5;

// ItemClient / ItemDefinition.
const size_t kItemReadyTime = 0x14;     // int unix seconds (OP_ItemRecastDelay)
const size_t kItemVtblGetDef = 0x28;    // ItemDefinition* GetItemDefinition()
const size_t kDefEffects = 0x220;       // 7 x Effect
const size_t kEffectSize = 0x64;
const int kEffectCount = 7;
const size_t kEffectSpellId = 0x00;     // i32
const size_t kEffectRecastDelay = 0x14; // i32 seconds
const size_t kEffectRecastType = 0x18;  // i32, -1 none
const size_t kEffectProcRate = 0x1c;    // i32 proc rate modifier (server sends 0 today)
const int kEffectClick = 0;
const int kEffectProc = 1;
const char* const kEffectLabels[kEffectCount] = { "Click", "Proc", "Worn", "Focus", "Scroll", "Focus", "Blessing" };

// PcProfile per-recast-type ready times: prof + 0x4CC4 + type*4 (types 1..99), see FUN_14066ece0.
const size_t kProfileRecastTimers = 0x4cc4;
const int kRecastTypes = 100;

// Spell manager and SPELL (verified in FUN_1400d3a00 / FUN_140428be0).
const size_t kMgrMaxSpellId = 0x64;
const size_t kMgrVtblGetSpell = 0x40;   // SPELL* (mgr, int64 id); unknown ids return the manager's empty spell
const size_t kMgrVtblGetSlot = 0x48;    // EffectSlot* (mgr, int index)
const size_t kSpellSlotBase = 0x60;     // int: index of the spell's first effect slot
const size_t kSpellSlotCount = 0x64;    // int
const size_t kSpellId = 0x8c;           // int
const size_t kSpellName = 0x18f;        // char[]
const size_t kSlotBase1 = 0x08;         // i64
const size_t kSlotSpa = 0x24;           // int
const int kMaxSlots = 24;

using FinaliseFn = void (*)(uint8_t* wnd);
using GetCostFn = int (*)(void* item, int includeAugments);
using CXStrFromCStrFn = void* (*)(void** out, const char* text);
using SetStmlTextFn = void (*)(void* stml, void** text, char append, void* fmt);
using GetPcProfileFn = uint8_t* (*)(uint8_t* key);

FinaliseFn g_origFinalise = nullptr;       // trampoline into FUN_1404340d0
GetCostFn g_getCost = nullptr;             // FUN_14064f280
CXStrFromCStrFn g_cxstrFromCStr = nullptr; // FUN_1405853f0
SetStmlTextFn g_setStmlText = nullptr;     // FUN_1405ca040
GetPcProfileFn g_getPcProfile = nullptr;   // FUN_14067c550 (call target inside FUN_14066ece0)
void** g_pLocalPC = nullptr;               // &DAT_140e65d90
uint8_t** g_gameStateOwner = nullptr;      // &DAT_140ee7010; +0x5e4 = game state (5 in a zone, 1 character select)
uint8_t** g_spellManager = nullptr;        // &DAT_140ee6480 (GetSpellByID site)
uint8_t** g_spellManager2 = nullptr;       // &DAT_140ee6480 (effect-slot site), cross-check

template <typename R, typename... A>
R VCall(void* obj, size_t slot, A... args) {
	auto fn = *reinterpret_cast<R (**)(void*, A...)>(*reinterpret_cast<uint8_t**>(obj) + slot);
	return fn(obj, args...);
}

int32_t I32(const uint8_t* p, size_t off) { return *reinterpret_cast<const int32_t*>(p + off); }
int64_t I64(const uint8_t* p, size_t off) { return *reinterpret_cast<const int64_t*>(p + off); }

// ---- text buffer (STML: <BR> separates lines) ----

struct Text {
	char buf[6144];
	size_t len = 0;
	bool full = false;
	void Add(const char* fmt, ...) {
		if (full) return;
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
		va_end(ap);
		if (n < 0 || static_cast<size_t>(n) >= sizeof(buf) - len) {
			buf[len] = '\0';
			full = true;
			return;
		}
		len += static_cast<size_t>(n);
	}
};

void AddDuration(Text& t, int seconds) {
	int h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
	if (h) t.Add("%dh ", h);
	if (h || m) t.Add("%dm ", m);
	t.Add("%ds", s);
}

// ---- value ----

void AddValue(Text& t, void* item) {
	int copper = g_getCost(item, 0);
	if (copper <= 0) return;
	int pp = copper / 1000, gp = copper / 100 % 10, sp = copper / 10 % 10, cp = copper % 10;
	t.Add("<BR>Value:");
	if (pp) t.Add(" %dpp", pp);
	if (gp) t.Add(" %dgp", gp);
	if (sp) t.Add(" %dsp", sp);
	if (cp) t.Add(" %dcp", cp);
}

// ---- clicky cooldown ----

// Ready time of the item's click effect: the later of the item's own timer and the shared timer of its
// recast type (the client checks both the same way). 0 when nothing is recorded.
int ClickReadyTime(const uint8_t* item, const uint8_t* def) {
	const uint8_t* click = def + kDefEffects + kEffectClick * kEffectSize;
	if (I32(click, kEffectSpellId) <= 0) return 0;
	int ready = I32(item, kItemReadyTime);
	int type = I32(click, kEffectRecastType);
	uint8_t* pc = static_cast<uint8_t*>(*g_pLocalPC);
	if (type >= 1 && type < kRecastTypes && pc) {
		uint8_t* sub = *reinterpret_cast<uint8_t**>(pc + 8);
		if (sub) {
			uint8_t* prof = g_getPcProfile(pc + 0x10 + I32(sub, 4));
			if (prof) {
				int shared = I32(prof, kProfileRecastTimers + static_cast<size_t>(type) * 4);
				if (shared > ready) ready = shared;
			}
		}
	}
	return ready;
}

void AddCooldown(Text& t, const uint8_t* item, const uint8_t* def) {
	int ready = ClickReadyTime(item, def);
	if (ready <= 0) return;
	int64_t now = static_cast<int64_t>(_time64(nullptr));
	int64_t remaining = static_cast<int64_t>(ready) - now;
	if (remaining <= 0 || remaining > 7 * 24 * 3600) return;
	t.Add("<BR>Ready in: ");
	AddDuration(t, static_cast<int>(remaining));
}

// ---- spell summary ----

struct SpaEntry {
	int spa;
	const char* name;
};
// The most common SPAs; base1 is shown as sent in the spell file (no level scaling applied).
const SpaEntry kSpaNames[] = {
	{ 0, "HP" }, { 1, "AC" }, { 2, "ATK" }, { 3, "Movement %" }, { 4, "STR" }, { 5, "DEX" }, { 6, "AGI" },
	{ 7, "STA" }, { 8, "INT" }, { 9, "WIS" }, { 10, "CHA" }, { 15, "Mana" }, { 24, "Endurance" },
	{ 46, "FR" }, { 47, "CR" }, { 48, "PR" }, { 49, "DR" }, { 50, "MR" }, { 55, "Rune" },
	{ 59, "Damage shield" }, { 69, "Max HP" }, { 79, "HP" }, { 97, "Max mana" }, { 111, "All resists" },
	{ 115, "Food/drink" }, { 116, "Curse counters" }, { 35, "Disease counters" }, { 36, "Poison counters" },
	{ 100, "HP regen" }, { 125, "Spell damage %" }, { 127, "Cast time %" }, { 132, "Mana cost %" },
	{ 169, "Crit chance %" }, { 170, "Crit damage %" },
};

const char* SpaName(int spa) {
	for (const SpaEntry& s : kSpaNames)
		if (s.spa == spa) return s.name;
	return nullptr;
}

void AddSpellSlots(Text& t, uint8_t* mgr, const uint8_t* spell) {
	int count = I32(spell, kSpellSlotCount);
	int base = I32(spell, kSpellSlotBase);
	if (count <= 0) return;
	if (count > kMaxSlots) count = kMaxSlots;
	bool any = false;
	for (int i = 0; i < count; ++i) {
		uint8_t* slot = VCall<uint8_t*>(mgr, kMgrVtblGetSlot, base + i);
		if (!slot) continue;
		int spa = I32(slot, kSlotSpa);
		if (spa < 0 || spa == 254) continue;    // 254 = empty slot
		int64_t base1 = I64(slot, kSlotBase1);
		t.Add(any ? ", " : "<BR>Effects: ");
		any = true;
		const char* name = SpaName(spa);
		if (spa == 11 || spa == 98) t.Add("Haste %+lld%%", static_cast<long long>(base1 - 100));
		else if (name) t.Add("%s %+lld", name, static_cast<long long>(base1));
		else t.Add("SPA %d: %lld", spa, static_cast<long long>(base1));
	}
}

void AddEffects(Text& t, const uint8_t* def) {
	uint8_t* mgr = *g_spellManager;
	if (!mgr) return;
	int maxId = I32(mgr, kMgrMaxSpellId);
	for (int idx = 0; idx < kEffectCount; ++idx) {
		const uint8_t* e = def + kDefEffects + static_cast<size_t>(idx) * kEffectSize;
		int id = I32(e, kEffectSpellId);
		if (id <= 0 || id > maxId) continue;
		uint8_t* spell = VCall<uint8_t*>(mgr, kMgrVtblGetSpell, static_cast<int64_t>(id));
		if (!spell || I32(spell, kSpellId) != id) continue;   // unknown id: the manager hands back its empty spell
		t.Add("<BR>%s effect: %.63s (spell %d)", kEffectLabels[idx], reinterpret_cast<const char*>(spell + kSpellName), id);
		if (idx == kEffectProc && I32(e, kEffectProcRate) != 0) t.Add(", proc rate %+d%%", I32(e, kEffectProcRate));
		AddSpellSlots(t, mgr, spell);
	}
}

// ---- hook ----

bool InZone() {
	uint8_t* owner = *g_gameStateOwner;
	return owner && *reinterpret_cast<int*>(owner + kGameState) == kGameStateInZone && *g_spellManager && *g_pLocalPC;
}

void AppendExtras(uint8_t* wnd) {
	void* stml = *reinterpret_cast<void**>(wnd + kWndDescription);
	if (!stml || !wnd[kWndHasItem]) return;
	uint8_t* item = *reinterpret_cast<uint8_t**>(wnd + kWndItem);
	if (!item) return;
	uint8_t* def = VCall<uint8_t*>(item, kItemVtblGetDef);
	if (!def) return;

	Text t;
	t.buf[0] = '\0';
	AddValue(t, item);
	AddCooldown(t, item, def);
	AddEffects(t, def);
	if (!t.len) return;

	void* cxstr = nullptr;
	g_cxstrFromCStr(&cxstr, t.buf);
	if (!cxstr) return;
	g_setStmlText(stml, &cxstr, 1, nullptr);   // append; releases cxstr
}

void FinaliseDetour(uint8_t* wnd) {
	static bool inside = false;
	g_origFinalise(wnd);
	if (inside || !InZone()) return;   // the window also renders at character select, with no spell data loaded
	inside = true;
	AppendExtras(wnd);
	inside = false;
}

#define REF(var) reinterpret_cast<void**>(&var)

// CItemDisplayWnd finalise FUN_1404340d0. Stolen (5 bytes, position independent): mov [rsp+10],rbx.
// The stack cookie load at +27 is RIP-relative and stays untouched.
const char* kFinaliseSig =
	"48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 B9 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? "
	"48 33 C4 48 89 45 37 4C 8B F1";

// SetNameSpriteTint FUN_1403014a0 prologue and GM-mode check; the second global is the game-state owner.
const char* kTintSig =
	"40 55 57 48 8B EC 48 83 EC 38 48 8B F9 48 8B 89 70 11 00 00 48 85 C9 0F 84 ?? ?? ?? ?? "
	"48 83 3D ?? ?? ?? ?? 00 74 30 48 8B 05 ?? ?? ?? ?? 83 B8 E4 05 00 00 01";

// CTargetWnd per-frame FUN_14050e150: mov [rax+13bc],-1; mov rdx,[pTarget]; mov rcx,[pLocalPC]; call GetConLevel.
const char* kTargetWndConSig =
	"C7 80 BC 13 00 00 FF FF FF FF 48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";

// Buff-slot scan FUN_1400d3a00: mov rcx,[spellManager]; cmp eax,[rcx+64] (max spell id); jg; mov rax,[rcx];
// call [rax+40] (GetSpellByID); mov [rsp+60],rax.
const char* kSpellMgrGetSpellSig =
	"48 8B 0D ?? ?? ?? ?? 3B 41 64 0F 8F ?? ?? ?? ?? 48 8B 01 FF 50 40 48 89 44 24 60";

// Same function, slot loop: mov rcx,[spellManager]; add edx,[rsi+60] (slot base); call [r8+48] (slot by index);
// ... cmp [rsi+8c],0 (spell id); ... call [rax+50] (empty slot); mov edx,[rax+24] (SPA).
const char* kSpellMgrSlotSig =
	"48 8B 0D ?? ?? ?? ?? 03 56 60 4C 8B 01 41 FF 50 48 EB ?? 85 D2 75 ?? 83 BE 8C 00 00 00 00 75 ?? "
	"B2 01 EB ?? 33 D2 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 50 8B 50 24";

// FUN_14066ece0(pLocalPC, type, readyTime): the per-type recast timer setter, whole body. The call at +31
// targets FUN_14067c550 (profile lookup keyed by pLocalPC+0x10+vbadj); the store is [rax+rbx*4+4cc4].
const char* kRecastTypeSetterSig =
	"48 89 5C 24 08 57 48 83 EC 20 48 8B 41 08 41 8B F8 48 83 C1 10 8B DA 4C 63 48 04 49 03 C9 E8 ?? ?? ?? ?? "
	"83 FB 64 72 01 CC 89 BC 98 C4 4C 00 00";

} // namespace

const HookSite kItemDisplayExtrasHooks[1] = {
	{ kFinaliseSig, 5, reinterpret_cast<void*>(&FinaliseDetour), REF(g_origFinalise) },
};

const RefSite kItemDisplayExtrasRefs[8] = {
	// ItemBase::GetCost FUN_14064f280(item, includeAugments): def = vtbl+0x28; mov r15d,[rax+c0]
	{ "40 53 41 56 41 57 48 83 EC 50 48 8B 01 0F B6 DA 4C 8B F1 FF 50 28 4D 8B 06 49 8B CE 44 8B B8 C0 00 00 00",
	  0, 0, REF(g_getCost) },
	// CXStr::CXStr(const char*) FUN_1405853f0: null text -> empty, else strlen, allocate, copy to rep+0x18
	{ "48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 74 ?? 48 89 5C 24 30", 0, 0, REF(g_cxstrFromCStr) },
	// CStmlWnd::SetSTMLText FUN_1405ca040(stml, CXStr*, append, fmt)
	{ "48 89 5C 24 18 48 89 6C 24 20 48 89 54 24 10 56 57 41 56", 0, 0, REF(g_setStmlText) },
	{ kTargetWndConSig, 20, 24, REF(g_pLocalPC) },
	{ kSpellMgrGetSpellSig, 3, 7, REF(g_spellManager) },
	{ kSpellMgrSlotSig, 3, 7, REF(g_spellManager2) },
	{ kRecastTypeSetterSig, 31, 35, REF(g_getPcProfile) },
	{ kTintSig, 42, 46, REF(g_gameStateOwner) },
};

const char* ItemDisplayExtrasPrepare() {
	if (g_spellManager != g_spellManager2)
		return "the two spell manager references disagree";
	return nullptr;
}
