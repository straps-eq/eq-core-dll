// ItemDisplayExtras: appends lines to the item window's description text (the "ItemDescription"
// STML): the item's lore text, its coin value, tribute values, required/recommended level, container
// details, stack size, how long a clicky still has to cool down, and for each click / proc / worn /
// focus / scroll / blessing effect a header with the spell's name and the effect's level, charges, cast
// and recast, followed by the spell's effects, duration and range in words (spellinfo.cpp; the full
// details are in the spell window, which the client opens from the spell name link).
//
// The window (CItemDisplayWnd) builds its text in SetItem (0x1404314e0) -> UpdateStrings
// (FUN_14041c1e0) -> FUN_1404340d0, the finalise step that pushes the assembled CXStr into the STML
// with CStmlWnd::SetSTMLText (FUN_1405ca040, append = 0). The finalise step also runs on its own when
// server text (lore, item information) arrives or the window is refreshed, so we hook it rather than
// SetItem: every run of the original replaces the whole text, and we append ours once afterwards with
// the same client call in append mode (FUN_1405ca040(stml, &cxstr, 1, 0)). The CXStr is built with the
// client's own constructor (FUN_1405853f0) and consumed (released) by SetSTMLText, so nothing leaks.
//
// Data read: ItemClient (window+0x3E8) -> ItemDefinition via vtbl+0x28, fields as the item
// deserialiser FUN_1406998d0 stores them; effects at def+0x220, 7 entries of 0x64 bytes in the server's
// serialisation order (click, proc, worn, focus, scroll, focus2, blessing); value via ItemBase::GetCost
// (FUN_14064f280, def+0xC0 in copper); recast ready times at ItemClient+0x14 and PcProfile+0x4CC4+type*4
// (both unix seconds, written by the OP_ItemRecastDelay handler FUN_1402ed830 / FUN_14066ece0).
#include "itemdisplay.h"
#include "spellinfo.h"
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
const size_t kDefItemClass = 0xe9;      // u8: 0 common, 1 container, 2 book
const size_t kDefReqLevel = 0x110;      // i32
const size_t kDefRecLevel = 0x114;      // i32
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
const size_t kEffectProcRate = 0x60;    // i32 proc rate modifier (server sends 0 today)
const int kEffectClick = 0;
const int kEffectProc = 1;
const char* const kEffectLabels[kEffectCount] = { "Click", "Proc", "Worn", "Focus", "Scroll", "Focus", "Blessing" };
const char* const kSizeNames[] = { "tiny", "small", "medium", "large", "giant" };

// PcProfile per-recast-type ready times: prof + 0x4CC4 + type*4 (types 1..99), see FUN_14066ece0.
const size_t kProfileRecastTimers = 0x4cc4;
const int kRecastTypes = 100;

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
uint8_t** g_pLocalPC = nullptr;            // &DAT_140e65d90

template <typename R, typename... A>
R VCall(void* obj, size_t slot, A... args) {
	auto fn = *reinterpret_cast<R (**)(void*, A...)>(*reinterpret_cast<uint8_t**>(obj) + slot);
	return fn(obj, args...);
}

int32_t I32(const uint8_t* p, size_t off) { return *reinterpret_cast<const int32_t*>(p + off); }
uint32_t U32(const uint8_t* p, size_t off) { return *reinterpret_cast<const uint32_t*>(p + off); }

// ---- configuration ([ItemDisplay] in dinput8.ini) ----

struct Config {
	bool lore = true;
	bool value = true;
	bool noValue = false;       // print "Value: none" for items without a price
	bool tribute = true;
	bool levels = true;
	bool container = true;
	bool stackSize = true;
	bool cooldown = true;
	bool effects = true;
	bool effectDetails = true;  // effect lines, duration and range under each effect header
	int maxEffects = 8;
	uint32_t colour = 0x8aa3ff; // label colour (0 = plain)
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
	Config c;
	c.lore = Int("ShowLore", c.lore) != 0;
	c.value = Int("ShowValue", c.value) != 0;
	c.noValue = Int("ShowNoValue", c.noValue) != 0;
	c.tribute = Int("ShowTribute", c.tribute) != 0;
	c.levels = Int("ShowLevels", c.levels) != 0;
	c.container = Int("ShowContainer", c.container) != 0;
	c.stackSize = Int("ShowStackSize", c.stackSize) != 0;
	c.cooldown = Int("ShowCooldown", c.cooldown) != 0;
	c.effects = Int("ShowEffects", c.effects) != 0;
	c.effectDetails = Int("ShowEffectDetails", c.effectDetails) != 0;
	c.maxEffects = Clamp(Int("MaxEffects", c.maxEffects), 1, 40);
	char buf[32] = {};
	GetPrivateProfileStringA("ItemDisplay", "Colour", "", buf, sizeof(buf), ini);
	if (buf[0]) c.colour = static_cast<uint32_t>(strtoul(buf, nullptr, 16)) & 0xffffff;
	g_cfg = c;
}

// ---- text ----

// "<BR><C #rrggbb>Label:</C> " (plain when no colour is set).
void Label(TextBuf& t, const char* label) {
	t.Add("<BR>");
	if (g_cfg.colour) t.Add("<C #%06x>%s</C> ", g_cfg.colour, label);
	else t.Add("%s ", label);
}

void AddLore(TextBuf& t, const uint8_t* def) {
	const char* lore = reinterpret_cast<const char*>(def + kDefLore);
	const char* name = reinterpret_cast<const char*>(def + kDefName);
	if (!lore[0] || strncmp(lore, name, 0x40) == 0) return;
	if (lore[0] == '*') ++lore;   // the lore-item marker the server prefixes
	if (!lore[0] || strncmp(lore, name, 0x40) == 0) return;
	Label(t, "Lore:");
	t.Add("%.79s", lore);
}

void AddValue(TextBuf& t, void* item) {
	int copper = g_getCost(item, 0);
	if (copper <= 0) {
		if (g_cfg.noValue) {
			Label(t, "Value:");
			t.Add("none");
		}
		return;
	}
	int pp = copper / 1000, gp = copper / 100 % 10, sp = copper / 10 % 10, cp = copper % 10;
	Label(t, "Value:");
	bool any = false;
	if (pp) { t.Add("%dpp", pp); any = true; }
	if (gp) { t.Add("%s%dgp", any ? " " : "", gp); any = true; }
	if (sp) { t.Add("%s%dsp", any ? " " : "", sp); any = true; }
	if (cp) t.Add("%s%dcp", any ? " " : "", cp);
}

void AddTribute(TextBuf& t, const uint8_t* def) {
	uint32_t favor = U32(def, kDefFavor), guild = U32(def, kDefGuildFavor);
	if (!favor && !guild) return;
	Label(t, "Tribute:");
	t.Add("%u", favor);
	if (guild) t.Add(", guild tribute %u", guild);
}

void AddLevels(TextBuf& t, const uint8_t* def) {
	int req = I32(def, kDefReqLevel), rec = I32(def, kDefRecLevel);
	if (req <= 0 && rec <= 0) return;
	Label(t, "Level:");
	if (req > 0) t.Add("required %d", req);
	if (req > 0 && rec > 0) t.Add(", ");
	if (rec > 0) t.Add("recommended %d", rec);
}

void AddContainer(TextBuf& t, const uint8_t* def) {
	if (def[kDefItemClass] != 1) return;
	int slots = def[kDefBagSlots], size = def[kDefBagSize], wr = def[kDefBagWR];
	Label(t, "Container:");
	t.Add("%d slot%s", slots, slots == 1 ? "" : "s");
	if (size >= 0 && size < static_cast<int>(sizeof(kSizeNames) / sizeof(kSizeNames[0]))) t.Add(", holds %s items", kSizeNames[size]);
	if (wr > 0) t.Add(", %d%% weight reduction", wr);
}

void AddStackSize(TextBuf& t, const uint8_t* def) {
	uint32_t stack = U32(def, kDefStackSize);
	if (stack <= 1 || stack >= 0x7fffffff) return;
	Label(t, "Stack size:");
	t.Add("%u", stack);
}

// ---- clicky cooldown ----

// Ready time of the item's click effect: the later of the item's own timer and the shared timer of its
// recast type (the client checks both the same way). 0 when nothing is recorded.
int ClickReadyTime(const uint8_t* item, const uint8_t* def) {
	const uint8_t* click = def + kDefEffects + kEffectClick * kEffectSize;
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
	Label(t, "Ready in:");
	SpellInfoSeconds(t, static_cast<int>(remaining));
}

// ---- effects ----

void AddEffects(TextBuf& t, const uint8_t* def) {
	for (int idx = 0; idx < kEffectCount; ++idx) {
		const uint8_t* e = def + kDefEffects + static_cast<size_t>(idx) * kEffectSize;
		int id = I32(e, kEffectSpellId);
		const uint8_t* spell = id > 0 ? SpellInfoGetSpell(id) : nullptr;
		if (!spell) continue;
		char header[96];
		snprintf(header, sizeof(header), "%s: %.63s", kEffectLabels[idx], SpellInfoName(spell));
		Label(t, header);
		int castLevel = I32(e, kEffectCastLevel), reqLevel = e[kEffectReqLevel], charges = I32(e, kEffectMaxCharges);
		int castMs = I32(e, kEffectCastTime), recast = I32(e, kEffectRecastDelay);
		bool any = false;
		auto Part = [&](const char* fmt, auto... args) {
			t.Add(any ? ", " : "(");
			t.Add(fmt, args...);
			any = true;
		};
		if (castLevel > 0) Part("cast at level %d", castLevel);
		if (reqLevel > 0) Part("usable at level %d", reqLevel);
		if (charges > 0) Part("%d charge%s", charges, charges == 1 ? "" : "s");
		if (idx == kEffectClick && castMs > 0) Part("%.1f sec cast", castMs / 1000.0);
		if (recast > 0) Part("recast %d sec", recast);
		if (idx == kEffectProc && I32(e, kEffectProcRate) != 0) Part("proc rate %+d%%", I32(e, kEffectProcRate));
		if (any) t.Add(")");
		if (!g_cfg.effectDetails) continue;
		SpellTextStyle style;
		style.lineBreak = "<BR>";
		style.indent = "- ";
		style.labelColour = 0;
		style.maxEffects = g_cfg.maxEffects;
		SpellInfoWrite(t, spell, castLevel, kSpellEffects | kSpellDuration | kSpellRange, style);
	}
}

// ---- hook ----

const size_t kTextMax = 8192;

void AppendExtras(uint8_t* wnd) {
	void* stml = *reinterpret_cast<void**>(wnd + kWndDescription);
	if (!stml || !wnd[kWndHasItem]) return;
	uint8_t* item = *reinterpret_cast<uint8_t**>(wnd + kWndItem);
	if (!item) return;
	uint8_t* def = VCall<uint8_t*>(item, kItemVtblGetDef);
	if (!def) return;

	static char text[kTextMax];
	TextBuf t(text, sizeof(text));
	if (g_cfg.lore) AddLore(t, def);
	if (g_cfg.value) AddValue(t, item);
	if (g_cfg.tribute) AddTribute(t, def);
	if (g_cfg.levels) AddLevels(t, def);
	if (g_cfg.container) AddContainer(t, def);
	if (g_cfg.stackSize) AddStackSize(t, def);
	if (g_cfg.cooldown) AddCooldown(t, item, def);
	if (g_cfg.effects) AddEffects(t, def);
	if (!t.len) return;

	void* cxstr = nullptr;
	g_cxstrFromCStr(&cxstr, text);
	if (!cxstr) return;
	g_setStmlText(stml, &cxstr, 1, nullptr);   // append; releases cxstr
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

// FUN_14066ece0(pLocalPC, type, readyTime): the per-type recast timer setter, whole body. The call at +31
// targets FUN_14067c550 (profile lookup keyed by pLocalPC+0x10+vbadj); the store is [rax+rbx*4+4cc4].
const char* kRecastTypeSetterSig =
	"48 89 5C 24 08 57 48 83 EC 20 48 8B 41 08 41 8B F8 48 83 C1 10 8B DA 4C 63 48 04 49 03 C9 E8 ?? ?? ?? ?? "
	"83 FB 64 72 01 CC 89 BC 98 C4 4C 00 00";

} // namespace

const HookSite kItemDisplayExtrasHooks[1] = {
	{ kFinaliseSig, 5, reinterpret_cast<void*>(&FinaliseDetour), REF(g_origFinalise) },
};

const RefSite kItemDisplayExtrasRefs[19] = {
	// ItemBase::GetCost FUN_14064f280(item, includeAugments): def = vtbl+0x28; mov r15d,[rax+c0]
	{ "40 53 41 56 41 57 48 83 EC 50 48 8B 01 0F B6 DA 4C 8B F1 FF 50 28 4D 8B 06 49 8B CE 44 8B B8 C0 00 00 00",
	  0, 0, REF(g_getCost) },
	// CXStr::CXStr(const char*) FUN_1405853f0: null text -> empty, else strlen, allocate, copy to rep+0x18
	{ "48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 74 ?? 48 89 5C 24 30", 0, 0, REF(g_cxstrFromCStr) },
	// CStmlWnd::SetSTMLText FUN_1405ca040(stml, CXStr*, append, fmt)
	{ "48 89 5C 24 18 48 89 6C 24 20 48 89 54 24 10 56 57 41 56", 0, 0, REF(g_setStmlText) },
	{ kTargetWndConSig, 20, 24, REF(g_pLocalPC) },
	{ kRecastTypeSetterSig, 31, 35, REF(g_getPcProfile) },
	SPELLINFO_REF_SITES,
};

const char* ItemDisplayExtrasPrepare() {
	if (const char* e = SpellInfoPrepare()) return e;
	LoadConfig();
	return nullptr;
}
