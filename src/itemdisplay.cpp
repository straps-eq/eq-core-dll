// ItemDisplayExtras: adds lines below the item window's description text (the "ItemDescription"
// STML) in the layout players know from other tools: for each spell on the item its name and
// "Spell Effect: <description>" (only when the window did not print that description itself, which it
// does after its "Effect:" lines); a block with item id, icon id, stack size, value, tribute, weapon
// ratio and efficiency, lore text, proc / click levels, required and recommended level, container
// details and how long a clicky still has to cool down; then for each effect a "Spell Info for <kind>
// effect: <name>" block with the spell's id and duration, recovery and recast, range, resist and one
// "Slot N:" line per effect in words (spellinfo.cpp).
//
// The window (CItemDisplayWnd) builds its text in SetItem (0x1404314e0) -> UpdateStrings
// (FUN_14041c1e0) -> FUN_1404340d0, the finalise step that pushes the assembled CXStr into the STML
// with CStmlWnd::SetSTMLText (FUN_1405ca040). The stat grid, class/race lines and augment slots live
// in other child windows; the description STML holds the client's "Effect: <spell> (Combat)" lines,
// the lore text and the server's "Item information" block. The finalise step also runs on its own
// when server text arrives or the window is refreshed, so we hook it rather than SetItem: after every
// run of the original we read the window's text back, add ours after it and set the whole text again
// (stml.h explains why SetSTMLText's "append" flag cannot be used).
//
// Data read: ItemClient (window+0x3E8) -> ItemDefinition via vtbl+0x28, fields as the item
// deserialiser FUN_1406998d0 stores them; effects at def+0x220, 7 entries of 0x64 bytes in the server's
// serialisation order (click, proc, worn, focus, scroll, focus2, blessing); value via ItemBase::GetCost
// (FUN_14064f280, def+0xC0 in copper); recast ready times at ItemClient+0x14 and PcProfile+0x4CC4+type*4
// (both unix seconds, written by the OP_ItemRecastDelay handler FUN_1402ed830 / FUN_14066ece0); the
// player's level from pLocalPlayer+0x494 for the "too high" colouring.
#include "itemdisplay.h"
#include "spellinfo.h"
#include "stml.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

// CItemDisplayWnd fields (ctor FUN_1404119e0, SetItem, finalise).
const size_t kWndDescription = 0x2c8;   // CStmlWnd* "ItemDescription"
const size_t kWndItem = 0x3e8;          // ItemPtr: ItemClient* at +0
const size_t kWndHasItem = 0x3f8;       // u8

// ItemClient / ItemDefinition (deserialiser FUN_1406998d0).
const size_t kItemReadyTime = 0x14;     // int unix seconds (OP_ItemRecastDelay)
const size_t kItemVtblGetDef = 0x28;    // ItemDefinition* GetItemDefinition()
const size_t kDefName = 0x00;           // char[0x40]
const size_t kDefLore = 0x40;           // char[0x50]
const size_t kDefId = 0xb8;             // u32
const size_t kDefIcon = 0xc4;           // u32
const size_t kDefItemClass = 0xe9;      // u8: 0 common, 1 container, 2 book
const size_t kDefReqLevel = 0x110;      // i32
const size_t kDefRecLevel = 0x114;      // i32
const size_t kDefDelay = 0x152;         // u8
const size_t kDefDamage = 0x158;        // u32
const size_t kDefEffects = 0x220;       // 7 x Effect
const size_t kDefBagSlots = 0x53d;      // u8
const size_t kDefBagSize = 0x53e;       // u8: largest item size held
const size_t kDefBagWR = 0x53f;         // u8: weight reduction %
const size_t kDefFavor = 0x560;         // u32 tribute
const size_t kDefGuildFavor = 0x564;    // u32
const size_t kDefStackSize = 0x590;     // u32 (0 = not stackable)
const size_t kEffectSize = 0x64;
const int kEffectCount = 7;
const size_t kEffectSpellId = 0x00;     // i32, -1 none
const size_t kEffectReqLevel = 0x04;    // u8 level needed to use the effect
const size_t kEffectCastLevel = 0x08;   // i32 level the effect is cast at
const size_t kEffectMaxCharges = 0x0c;  // i32 (-1 unlimited)
const size_t kEffectCastTime = 0x10;    // i32 ms
const size_t kEffectRecastDelay = 0x14; // i32 seconds
const size_t kEffectRecastType = 0x18;  // i32, -1 none
const int kEffectClick = 0;
const int kEffectProc = 1;
const char* const kEffectLabels[kEffectCount] = { "Click", "Proc", "Worn", "Focus", "Scroll", "Focus", "Blessing" };
const char* const kSizeNames[] = { "tiny", "small", "medium", "large", "giant" };

// PcProfile per-recast-type ready times: prof + 0x4CC4 + type*4 (types 1..99), see FUN_14066ece0.
const size_t kProfileRecastTimers = 0x4cc4;
const int kRecastTypes = 100;
// PlayerClient
const size_t kSpawnLevel = 0x494;       // u8

using FinaliseFn = void (*)(uint8_t* wnd);
using GetCostFn = int (*)(void* item, int includeAugments);
using GetPcProfileFn = uint8_t* (*)(uint8_t* key);

FinaliseFn g_origFinalise = nullptr;         // trampoline into FUN_1404340d0
GetCostFn g_getCost = nullptr;               // FUN_14064f280
StmlCXStrCtorFn g_cxstrFromCStr = nullptr;   // FUN_1405853f0
StmlSetTextFn g_setStmlText = nullptr;       // FUN_1405ca040
GetPcProfileFn g_getPcProfile = nullptr;     // FUN_14067c550 (call target inside FUN_14066ece0)
uint8_t** g_pLocalPC = nullptr;              // &DAT_140e65d90
uint8_t** g_pLocalPlayer = nullptr;          // &DAT_140e66228

template <typename R, typename... A>
R VCall(void* obj, size_t slot, A... args) {
	auto fn = *reinterpret_cast<R (**)(void*, A...)>(*reinterpret_cast<uint8_t**>(obj) + slot);
	return fn(obj, args...);
}

int32_t I32(const uint8_t* p, size_t off) { return *reinterpret_cast<const int32_t*>(p + off); }
uint32_t U32(const uint8_t* p, size_t off) { return *reinterpret_cast<const uint32_t*>(p + off); }

// ---- configuration ([ItemDisplay] in dinput8.ini) ----

struct Config {
	bool ids = true;            // item id, icon id
	bool stackSize = true;      // "Stackable Count"
	bool value = true;
	bool noValue = false;       // print "Value: none" for items without a price
	bool tribute = true;
	bool ratio = true;          // weapon ratio and efficiency
	bool lore = true;
	bool effectLevels = true;   // "Procs at level N" / "Clickable at level N"
	bool levels = true;         // required / recommended level
	bool container = true;
	bool cooldown = true;
	bool effects = true;        // spell name + description above, "Spell Info" block per effect below
	int maxEffects = 12;
	uint32_t colour = 0x00ffff;          // the item block
	uint32_t spellNameColour = 0x8aa3ff; // spell names above the item block
	uint32_t spellInfoColour = 0xff66ff; // "Spell Info for ..." blocks
	uint32_t tooHighColour = 0xff4040;   // proc / click level above the player's level
};
Config g_cfg;

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void LoadConfig() {
	char ini[MAX_PATH];
	GetModuleFileNameA(nullptr, ini, MAX_PATH);
	char* slash = strrchr(ini, '\\');
	if (slash) *slash = '\0';
	strcat_s(ini, "\\dinput8.ini");
	auto Int = [&](const char* key, int def) { return static_cast<int>(GetPrivateProfileIntA("ItemDisplay", key, def, ini)); };
	auto Colour = [&](const char* key, uint32_t def) {
		char buf[32] = {};
		GetPrivateProfileStringA("ItemDisplay", key, "", buf, sizeof(buf), ini);
		if (!buf[0]) return def;
		return static_cast<uint32_t>(strtoul(buf, nullptr, 16)) & 0xffffffu;
	};
	Config c;
	c.ids = Int("ShowIds", c.ids) != 0;
	c.stackSize = Int("ShowStackSize", c.stackSize) != 0;
	c.value = Int("ShowValue", c.value) != 0;
	c.noValue = Int("ShowNoValue", c.noValue) != 0;
	c.tribute = Int("ShowTribute", c.tribute) != 0;
	c.ratio = Int("ShowRatio", c.ratio) != 0;
	c.lore = Int("ShowLore", c.lore) != 0;
	c.effectLevels = Int("ShowEffectLevels", c.effectLevels) != 0;
	c.levels = Int("ShowLevels", c.levels) != 0;
	c.container = Int("ShowContainer", c.container) != 0;
	c.cooldown = Int("ShowCooldown", c.cooldown) != 0;
	c.effects = Int("ShowEffects", c.effects) != 0;
	c.maxEffects = Clamp(Int("MaxEffects", c.maxEffects), 1, 40);
	c.colour = Colour("Colour", c.colour);
	c.spellNameColour = Colour("SpellNameColour", c.spellNameColour);
	c.spellInfoColour = Colour("SpellInfoColour", c.spellInfoColour);
	c.tooHighColour = Colour("TooHighColour", c.tooHighColour);
	g_cfg = c;
}

// ---- text ----

// The client's own colour syntax: FUN_1405c3030 formats a colour as "#RRGGBB" in quotes for <C ...>.
void ColourOpen(TextBuf& t, uint32_t colour) { if (colour) t.Add("<C \"#%06X\">", colour & 0xffffff); }
void ColourClose(TextBuf& t, uint32_t colour) { if (colour) t.Add("</C>"); }

int PlayerLevel() {
	uint8_t* me = *g_pLocalPlayer;
	return me ? me[kSpawnLevel] : 0;
}

const uint8_t* Effect(const uint8_t* def, int idx) { return def + kDefEffects + static_cast<size_t>(idx) * kEffectSize; }

const uint8_t* EffectSpell(const uint8_t* def, int idx) {
	int id = I32(Effect(def, idx), kEffectSpellId);
	return id > 0 ? SpellInfoGetSpell(id) : nullptr;
}

// Spell name and description, one pair per effect, before the item block. The window itself prints each
// effect's name and description after its "Effect:" lines for most items; the pair is added only for
// effects whose description is missing from the client's text.
void AddSpellHeaders(TextBuf& t, const uint8_t* def, const char* native) {
	static char description[2048];
	for (int idx = 0; idx < kEffectCount; ++idx) {
		const uint8_t* spell = EffectSpell(def, idx);
		if (!spell) continue;
		TextBuf d(description, sizeof(description));
		if (!SpellInfoDescription(d, spell, I32(Effect(def, idx), kEffectCastLevel))) continue;   // the "Effect:" line already names it
		if (StmlHasText(native, description)) continue;
		t.Add("<BR><BR>");
		ColourOpen(t, g_cfg.spellNameColour);
		t.Add("%.63s", SpellInfoName(spell));
		ColourClose(t, g_cfg.spellNameColour);
		t.Add("<BR>Spell Effect: %s", description);
	}
}

void AddIds(TextBuf& t, const uint8_t* def) {
	t.Add("<BR>Item ID: %u", U32(def, kDefId));
	t.Add("<BR>Icon ID: %u", U32(def, kDefIcon));
}

void AddStackSize(TextBuf& t, const uint8_t* def) {
	uint32_t stack = U32(def, kDefStackSize);
	if (stack <= 1 || stack >= 0x7fffffff) return;
	t.Add("<BR>Stackable Count: %u", stack);
}

void AddValue(TextBuf& t, void* item) {
	int copper = g_getCost(item, 0);
	if (copper <= 0) {
		if (g_cfg.noValue) t.Add("<BR>Value: none");
		return;
	}
	int pp = copper / 1000, gp = copper / 100 % 10, sp = copper / 10 % 10, cp = copper % 10;
	t.Add("<BR>Value:");
	if (pp) t.Add(" %dpp", pp);
	if (gp) t.Add(" %dgp", gp);
	if (sp) t.Add(" %dsp", sp);
	if (cp) t.Add(" %dcp", cp);
}

void AddTribute(TextBuf& t, const uint8_t* def) {
	uint32_t favor = U32(def, kDefFavor), guild = U32(def, kDefGuildFavor);
	if (favor) t.Add("<BR>Tribute Value: %u", favor);
	if (guild) t.Add("<BR>Guild Tribute Value: %u", guild);
}

// Weapons: ratio = delay / damage, efficiency = damage * 100 / delay.
void AddRatio(TextBuf& t, const uint8_t* def) {
	int delay = def[kDefDelay];
	uint32_t damage = U32(def, kDefDamage);
	if (delay <= 0 || damage == 0 || damage > 100000) return;
	t.Add("<BR>Ratio: %.3f", static_cast<double>(delay) / static_cast<double>(damage));
	t.Add("<BR>Efficiency: %.0f", static_cast<double>(damage) * 100.0 / static_cast<double>(delay));
}

void AddLore(TextBuf& t, const uint8_t* def) {
	const char* lore = reinterpret_cast<const char*>(def + kDefLore);
	const char* name = reinterpret_cast<const char*>(def + kDefName);
	if (lore[0] == '*') ++lore;   // the lore-item marker the server prefixes
	if (!lore[0] || strncmp(lore, name, 0x40) == 0) return;
	t.Add("<BR>Item Lore: %.79s", lore);
}

void AddLevelLine(TextBuf& t, const char* text, int level) {
	bool tooHigh = level > PlayerLevel() && PlayerLevel() > 0;
	t.Add("<BR>");
	if (tooHigh) ColourOpen(t, g_cfg.tooHighColour);
	t.Add("%s %d", text, level);
	if (tooHigh) ColourClose(t, g_cfg.tooHighColour);
}

void AddEffectLevels(TextBuf& t, const uint8_t* def) {
	const uint8_t* proc = Effect(def, kEffectProc);
	if (I32(proc, kEffectSpellId) > 0 && I32(proc, kEffectCastLevel) > 0)
		AddLevelLine(t, "Procs at level", I32(proc, kEffectCastLevel));
	const uint8_t* click = Effect(def, kEffectClick);
	if (I32(click, kEffectSpellId) > 0 && click[kEffectReqLevel] > 0)
		AddLevelLine(t, "Clickable at level", click[kEffectReqLevel]);
}

void AddLevels(TextBuf& t, const uint8_t* def) {
	int req = I32(def, kDefReqLevel), rec = I32(def, kDefRecLevel);
	if (req > 0) t.Add("<BR>Required Level: %d", req);
	if (rec > 0) t.Add("<BR>Recommended Level: %d", rec);
}

void AddContainer(TextBuf& t, const uint8_t* def) {
	if (def[kDefItemClass] != 1) return;
	int slots = def[kDefBagSlots], size = def[kDefBagSize], wr = def[kDefBagWR];
	t.Add("<BR>Container: %d slot%s", slots, slots == 1 ? "" : "s");
	if (size >= 0 && size < static_cast<int>(sizeof(kSizeNames) / sizeof(kSizeNames[0]))) t.Add(", holds %s items", kSizeNames[size]);
	if (wr > 0) t.Add(", %d%% weight reduction", wr);
}

// ---- clicky cooldown ----

// Ready time of the item's click effect: the later of the item's own timer and the shared timer of its
// recast type (the client checks both the same way). 0 when nothing is recorded.
int ClickReadyTime(const uint8_t* item, const uint8_t* def) {
	const uint8_t* click = Effect(def, kEffectClick);
	if (I32(click, kEffectSpellId) <= 0) return 0;
	int ready = I32(item, kItemReadyTime);
	int type = I32(click, kEffectRecastType);
	uint8_t* pc = *g_pLocalPC;
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

void AddCooldown(TextBuf& t, const uint8_t* item, const uint8_t* def) {
	int ready = ClickReadyTime(item, def);
	if (ready <= 0) return;
	int64_t now = static_cast<int64_t>(_time64(nullptr));
	int64_t remaining = static_cast<int64_t>(ready) - now;
	if (remaining <= 0 || remaining > 7 * 24 * 3600) return;
	t.Add("<BR>Ready in: ");
	SpellInfoSeconds(t, static_cast<int>(remaining));
}

// ---- per-effect spell blocks ----

void AddSpellInfo(TextBuf& t, const uint8_t* def) {
	for (int idx = 0; idx < kEffectCount; ++idx) {
		const uint8_t* spell = EffectSpell(def, idx);
		if (!spell) continue;
		const uint8_t* e = Effect(def, idx);
		t.Add("<BR>");
		ColourOpen(t, g_cfg.spellInfoColour);
		t.Add("<BR>Spell Info for %s effect: %.63s", kEffectLabels[idx], SpellInfoName(spell));
		int charges = I32(e, kEffectMaxCharges), castMs = I32(e, kEffectCastTime), recast = I32(e, kEffectRecastDelay);
		if (charges > 0) t.Add("<BR>Charges: %d", charges);
		if (idx == kEffectClick && castMs > 0) t.Add("<BR>Cast Time: %.1f sec", castMs / 1000.0);
		if (recast > 0) t.Add("<BR>Recast Delay: %d sec", recast);
		SpellTextStyle style;
		style.lineBreak = "<BR>";
		style.indent = "";
		style.labelColour = 0;
		style.maxEffects = g_cfg.maxEffects;
		style.gap = kStmlGap;
		SpellInfoDetails(t, spell, I32(e, kEffectCastLevel), style);
		ColourClose(t, g_cfg.spellInfoColour);
	}
}

// ---- hook ----

const size_t kTextMax = 65536;   // the client's text (lore, item information) plus ours

void AppendExtras(uint8_t* wnd) {
	void* stml = *reinterpret_cast<void**>(wnd + kWndDescription);
	if (!stml || !wnd[kWndHasItem]) return;
	uint8_t* item = *reinterpret_cast<uint8_t**>(wnd + kWndItem);
	if (!item) return;
	uint8_t* def = VCall<uint8_t*>(item, kItemVtblGetDef);
	if (!def) return;

	// The client's text ends in line breaks; after trimming them, every section below starts with one "<BR>"
	// (a blank line) and every line with another, so sections are separated by exactly one blank line.
	static char text[kTextMax];
	size_t native = StmlTrimBreaks(text, StmlCurrentText(stml, text, sizeof(text)));
	TextBuf t(text + native, sizeof(text) - native);
	if (g_cfg.effects) AddSpellHeaders(t, def, text);
	size_t blockStart = t.len;
	t.Add("<BR>");
	ColourOpen(t, g_cfg.colour);
	size_t mark = t.len;
	if (g_cfg.ids) AddIds(t, def);
	if (g_cfg.stackSize) AddStackSize(t, def);
	if (g_cfg.value) AddValue(t, item);
	if (g_cfg.tribute) AddTribute(t, def);
	if (g_cfg.ratio) AddRatio(t, def);
	if (g_cfg.lore) AddLore(t, def);
	if (g_cfg.effectLevels) AddEffectLevels(t, def);
	if (g_cfg.levels) AddLevels(t, def);
	if (g_cfg.container) AddContainer(t, def);
	if (g_cfg.cooldown) AddCooldown(t, item, def);
	if (t.len == mark && !t.full) {
		t.len = blockStart;   // empty block: drop its colour tag
		t.buf[t.len] = '\0';
	}
	else {
		ColourClose(t, g_cfg.colour);
	}
	if (g_cfg.effects) AddSpellInfo(t, def);
	if (!t.len) return;
	StmlSetText(stml, text, g_cxstrFromCStr, g_setStmlText);
}

void FinaliseDetour(uint8_t* wnd) {
	static bool inside = false;
	g_origFinalise(wnd);
	if (inside || !SpellInfoInGame()) return;   // the window also renders at character select, with no spell data loaded
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

// CTargetWnd per-frame FUN_14050e150: mov [rax+13bc],-1; mov rdx,[pTarget]; mov rcx,[pLocalPC]; call GetConLevel.
const char* kTargetWndConSig =
	"C7 80 BC 13 00 00 FF FF FF FF 48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";

// Pet-owner lookup inside SetNameSpriteTint FUN_1403014a0: ... mov rcx,[pLocalPlayer]; cmp [rax+168],[rcx+168].
const char* kSpawnMgrSig =
	"48 8B 0D ?? ?? ?? ?? 48 85 C9 74 6E 48 8B 01 48 8B 97 B8 01 00 00 FF 50 18 48 8B D8 48 85 C0 74 59 "
	"48 8B 0D ?? ?? ?? ?? 8B 91 68 01 00 00 39 90 68 01 00 00";

// FUN_14066ece0(pLocalPC, type, readyTime): the per-type recast timer setter, whole body. The call at +31
// targets FUN_14067c550 (profile lookup keyed by pLocalPC+0x10+vbadj); the store is [rax+rbx*4+4cc4].
const char* kRecastTypeSetterSig =
	"48 89 5C 24 08 57 48 83 EC 20 48 8B 41 08 41 8B F8 48 83 C1 10 8B DA 4C 63 48 04 49 03 C9 E8 ?? ?? ?? ?? "
	"83 FB 64 72 01 CC 89 BC 98 C4 4C 00 00";

} // namespace

const HookSite kItemDisplayExtrasHooks[1] = {
	{ kFinaliseSig, 5, reinterpret_cast<void*>(&FinaliseDetour), REF(g_origFinalise) },
};

const RefSite kItemDisplayExtrasRefs[21] = {
	// ItemBase::GetCost FUN_14064f280(item, includeAugments): def = vtbl+0x28; mov r15d,[rax+c0]
	{ "40 53 41 56 41 57 48 83 EC 50 48 8B 01 0F B6 DA 4C 8B F1 FF 50 28 4D 8B 06 49 8B CE 44 8B B8 C0 00 00 00",
	  0, 0, REF(g_getCost) },
	// CXStr::CXStr(const char*) FUN_1405853f0: null text -> empty, else strlen, allocate, copy to rep+0x18
	{ "48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 74 ?? 48 89 5C 24 30", 0, 0, REF(g_cxstrFromCStr) },
	// CStmlWnd::SetSTMLText FUN_1405ca040(stml, CXStr*, flag, fmt)
	{ "48 89 5C 24 18 48 89 6C 24 20 48 89 54 24 10 56 57 41 56", 0, 0, REF(g_setStmlText) },
	{ kTargetWndConSig, 20, 24, REF(g_pLocalPC) },
	{ kSpawnMgrSig, 36, 40, REF(g_pLocalPlayer) },
	{ kRecastTypeSetterSig, 31, 35, REF(g_getPcProfile) },
	SPELLINFO_REF_SITES,
};

const char* ItemDisplayExtrasPrepare() {
	if (const char* e = SpellInfoPrepare()) return e;
	LoadConfig();
	return nullptr;
}
