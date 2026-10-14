// BuffTooltips: richer tooltips on the buff, song and target buff windows.
//
// Natively the client sets every buff button's tooltip to "<spell> (<1h 2m 3s>)" (or "<spell>:Permanent")
// every 50 ms from CBuffWindow's tooltip refresh (FUN_140390640, called by the per-frame FUN_14038f880)
// and CTargetWnd's twin (FUN_14050ec00): spell lookup through the spell manager (DAT_140ee6480,
// vtbl+0x40), the duration formatted by FUN_1405d2880, a CXStr built with FUN_1405853f0, pushed with
// button->vtbl[+0x288] (SetTooltip) and released with FUN_140587670. The caster name the server sends
// sits unused in the slot. We hook both refresh functions and replace them: for every slot with a button
// we set the same first line, then the caster (when known), a compact effect summary for the common SPAs
// and the cure counters the spell carries. Because the original would overwrite our text every 50 ms,
// it is not called (except as a fallback while the spell manager does not exist yet). The text is set
// only when something it depends on changed (button, spell, caster, remaining seconds), so SetTooltip
// runs about once a second per buff instead of 20 times, and the effect summary is rebuilt only when
// the slot's spell changes. Nothing is allocated at runtime apart from the CXStr the client allocates
// for each SetTooltip, which is released the way the client does it.
//
// Slot layout (buff/song: this+0x2f0, 0x90 bytes, count this+0x330; target: this+0x2f8, 0x78 bytes,
// count this+0x300): +0 CButton*, +0x10 spell id, +0x14 remaining ms (<0 permanent, 0 none),
// +0x18 caster CXStr-like (+0x20 char*, +0x28 length). Spell effect slots: count spell+0x64, first index
// spell+0x60, slot = spellmgr->vtbl[+0x48](mgr, index): +8 base1, +0x10 base2, +0x18 max, +0x20 formula,
// +0x24 SPA (the spell file order slot|spa|base|base2|formula|max).
#include "buffs.h"
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// CBuffWindow (long buffs DAT_140db5358, songs DAT_140db5360)
const size_t kBuffSlots = 0x2f0;
const size_t kBuffSlotCapacity = 0x2f8;   // array length (0x3e)
const size_t kBuffSlotCount = 0x330;      // slots in use (62 buffs / 30 songs), the per-frame loop bound
const size_t kBuffSlotSize = 0x90;
// CTargetWnd
const size_t kTargetSlots = 0x2f8;
const size_t kTargetSlotCount = 0x300;
const size_t kTargetSlotSize = 0x78;
// slot fields shared by both layouts
const size_t kSlotButton = 0x00;
const size_t kSlotSpellId = 0x10;
const size_t kSlotRemainingMs = 0x14;
const size_t kSlotCasterText = 0x20;
const size_t kSlotCasterLen = 0x28;
const size_t kButtonVtblSetTooltip = 0x288;  // (CButton*, CXStr*)
// SPELL
const size_t kSpellEffectFirst = 0x60;
const size_t kSpellEffectCount = 0x64;
const size_t kSpellName = 0x18f;             // char[64]
// effect slot
const size_t kEffectBase1 = 0x08;            // int64
const size_t kEffectMax = 0x18;              // int64
const size_t kEffectSpa = 0x24;              // int
// spell manager
const size_t kSpellMgrVtblGetSpell = 0x40;   // (mgr, int id) -> SPELL*
const size_t kSpellMgrVtblGetEffect = 0x48;  // (mgr, int index) -> effect slot*

const int kSpellNameMax = 63;
const int kDurationCap = 0x19;               // what the client passes to FUN_1405d2880
const int kMaxEffectSlots = 64;              // more than any stock spell

using RefreshFn = void (*)(uint8_t* wnd);
using FormatDurationFn = void (*)(char* out, int cap, int* seconds);
using CXStrAssignFn = void* (*)(void** str, const char* text);   // *str = new rep (nullptr for "")
using CXStrReleaseFn = void (*)(void** str, void* rep);          // rep = *str; caller then zeroes *str

RefreshFn g_origBuffRefresh = nullptr;       // trampoline into FUN_140390640
RefreshFn g_origTargetRefresh = nullptr;     // trampoline into FUN_14050ec00
FormatDurationFn g_formatDuration = nullptr; // FUN_1405d2880
CXStrAssignFn g_cxstrAssign = nullptr;       // FUN_1405853f0
CXStrReleaseFn g_cxstrRelease = nullptr;     // FUN_140587670
void** g_spellManager = nullptr;             // &DAT_140ee6480 (from its write)
void** g_spellManagerRead = nullptr;         // &DAT_140ee6480 (from an unrelated read, cross-check)
uint8_t* g_buffRefreshFn = nullptr;          // FUN_140390640, to cross-check its callees
uint8_t* g_targetRefreshFn = nullptr;        // FUN_14050ec00

template <typename R, typename... A>
R VCall(void* obj, size_t slot, A... args) {
	auto fn = *reinterpret_cast<R (**)(void*, A...)>(*reinterpret_cast<uint8_t**>(obj) + slot);
	return fn(obj, args...);
}

// ---- configuration ([Buffs] in dinput8.ini) ----

struct Config {
	bool multiLine = true;      // one line per part ("\n"), else " | "
	bool showCaster = true;
	bool showEffects = true;
	bool showCures = true;
	int maxEffects = 8;         // effects listed per spell
};
Config g_cfg;

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void LoadConfig() {
	char ini[MAX_PATH];
	GetModuleFileNameA(nullptr, ini, MAX_PATH);
	char* slash = strrchr(ini, '\\');
	if (slash) *slash = '\0';
	strcat_s(ini, "\\dinput8.ini");
	auto Int = [&](const char* key, int def) { return static_cast<int>(GetPrivateProfileIntA("Buffs", key, def, ini)); };
	Config c;
	c.multiLine = Int("MultiLine", c.multiLine) != 0;
	c.showCaster = Int("ShowCaster", c.showCaster) != 0;
	c.showEffects = Int("ShowEffects", c.showEffects) != 0;
	c.showCures = Int("ShowCures", c.showCures) != 0;
	c.maxEffects = Clamp(Int("MaxEffects", c.maxEffects), 1, 20);
	g_cfg = c;
}

// ---- per-slot cache ----

const int kMaxWindows = 4;    // buff, song, target (+1 spare)
const int kMaxSlots = 64;     // >= 0x3e, the largest slot array
const int kCasterMax = 64;
const int kSummaryMax = 256;
const int kCuresMax = 96;
const int kTextMax = 512;

struct SlotCache {
	void* button;             // CButton the text was set on (a UI reload makes new buttons)
	int spellId;              // 0 = empty tooltip was set
	int seconds;              // remaining seconds the text shows (-1 permanent)
	bool valid;               // something has been set for this slot
	char caster[kCasterMax];
	char summary[kSummaryMax];
	char cures[kCuresMax];
};
struct WindowCache {
	uint8_t* wnd;
	SlotCache slots[kMaxSlots];
};
WindowCache g_windows[kMaxWindows];
int g_nextWindow = 0;

WindowCache* CacheFor(uint8_t* wnd) {
	for (WindowCache& w : g_windows)
		if (w.wnd == wnd) return &w;
	WindowCache& w = g_windows[g_nextWindow];
	g_nextWindow = (g_nextWindow + 1) % kMaxWindows;
	memset(&w, 0, sizeof(w));
	w.wnd = wnd;
	return &w;
}

// ---- text ----

void Append(char* out, size_t cap, size_t* len, const char* fmt, ...) {
	if (*len >= cap) return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out + *len, cap - *len, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	*len += static_cast<size_t>(n);
	if (*len >= cap) *len = cap - 1;
}

// "+500" or "+10 to 40" when the slot has a higher cap than its base value.
void Amount(char* out, size_t cap, long long base, long long max) {
	if (max != 0 && (max > 0 ? max : -max) > (base > 0 ? base : -base) && (max > 0) == (base > 0))
		snprintf(out, cap, "%+lld to %lld", base, max);
	else
		snprintf(out, cap, "%+lld", base);
}

// Appends ", <effect>" for the SPAs we summarise; returns false for anything else (left out).
bool DescribeEffect(int spa, long long base, long long max, char* out, size_t cap, size_t* len) {
	char amt[48];
	Amount(amt, sizeof(amt), base, max);
	const char* sep = *len ? ", " : "";
	switch (spa) {
	case 0: case 100: Append(out, cap, len, "%s%s HP/tick", sep, amt); return true;   // HoT / DoT
	case 1:   Append(out, cap, len, "%s%s AC", sep, amt); return true;
	case 2:   Append(out, cap, len, "%s%s ATK", sep, amt); return true;
	case 3:   Append(out, cap, len, "%s%s%% move", sep, amt); return true;
	case 4:   Append(out, cap, len, "%s%s STR", sep, amt); return true;
	case 5:   Append(out, cap, len, "%s%s DEX", sep, amt); return true;
	case 6:   Append(out, cap, len, "%s%s AGI", sep, amt); return true;
	case 7:   Append(out, cap, len, "%s%s STA", sep, amt); return true;
	case 8:   Append(out, cap, len, "%s%s INT", sep, amt); return true;
	case 9:   Append(out, cap, len, "%s%s WIS", sep, amt); return true;
	case 10:
		if (base == 0) return false;   // the spell file's blank placeholder
		Append(out, cap, len, "%s%s CHA", sep, amt);
		return true;
	case 11:   // attack speed: 140 = +40% haste, 60 = 40% slow
		if (base > 100) Append(out, cap, len, "%s+%lld%% haste", sep, base - 100);
		else if (base > 0 && base < 100) Append(out, cap, len, "%s%lld%% slow", sep, 100 - base);
		else return false;
		return true;
	case 15:  Append(out, cap, len, "%s%s mana/tick", sep, amt); return true;
	case 46:  Append(out, cap, len, "%s%s FR", sep, amt); return true;
	case 47:  Append(out, cap, len, "%s%s CR", sep, amt); return true;
	case 48:  Append(out, cap, len, "%s%s PR", sep, amt); return true;
	case 49:  Append(out, cap, len, "%s%s DR", sep, amt); return true;
	case 50:  Append(out, cap, len, "%s%s MR", sep, amt); return true;
	case 55:  Append(out, cap, len, "%srune %lld", sep, base); return true;
	case 59:  Append(out, cap, len, "%sDS %lld", sep, base); return true;
	case 69:  Append(out, cap, len, "%s%s max HP", sep, amt); return true;
	case 97:  Append(out, cap, len, "%s%s max mana", sep, amt); return true;
	case 111: Append(out, cap, len, "%s%s all resists", sep, amt); return true;
	default:  return false;
	}
}

const char* CounterName(int spa) {
	switch (spa) {
	case 35: return "disease";
	case 36: return "poison";
	case 116: return "curse";
	case 369: return "corruption";
	default: return nullptr;
	}
}

// Fills the per-spell parts of the tooltip from the spell's effect slots.
void BuildSummary(void* mgr, uint8_t* spell, char* summary, char* cures) {
	summary[0] = '\0';
	cures[0] = '\0';
	size_t sumLen = 0, cureLen = 0;
	int shown = 0;
	int first = *reinterpret_cast<int*>(spell + kSpellEffectFirst);
	int count = *reinterpret_cast<int*>(spell + kSpellEffectCount);
	if (count > kMaxEffectSlots) count = kMaxEffectSlots;
	for (int i = 0; i < count; ++i) {
		uint8_t* effect = VCall<uint8_t*>(mgr, kSpellMgrVtblGetEffect, first + i);
		if (!effect) continue;
		int spa = *reinterpret_cast<int*>(effect + kEffectSpa);
		long long base = *reinterpret_cast<long long*>(effect + kEffectBase1);
		long long max = *reinterpret_cast<long long*>(effect + kEffectMax);
		if (const char* counter = CounterName(spa)) {
			if (g_cfg.showCures && base > 0)
				Append(cures, kCuresMax, &cureLen, "%s%lld %s", cureLen ? ", " : "needs cure: ", base, counter);
			continue;
		}
		if (!g_cfg.showEffects || shown >= g_cfg.maxEffects) continue;
		if (DescribeEffect(spa, base, max, summary, kSummaryMax, &sumLen)) ++shown;
	}
}

void SetTooltip(void* button, const char* text) {
	void* str = nullptr;
	g_cxstrAssign(&str, text);
	VCall<void>(button, kButtonVtblSetTooltip, &str);
	if (str) g_cxstrRelease(&str, str);
}

// Replacement for both native refresh loops: slots[0..count) of `size` bytes each.
void Refresh(uint8_t* wnd, uint8_t* slots, int count, size_t size) {
	void* mgr = *g_spellManager;
	WindowCache* cache = CacheFor(wnd);
	if (count > kMaxSlots) count = kMaxSlots;
	for (int i = 0; i < count; ++i) {
		uint8_t* slot = slots + static_cast<size_t>(i) * size;
		void* button = *reinterpret_cast<void**>(slot + kSlotButton);
		if (!button) continue;
		SlotCache& c = cache->slots[i];
		int spellId = *reinterpret_cast<int*>(slot + kSlotSpellId);
		int ms = *reinterpret_cast<int*>(slot + kSlotRemainingMs);
		uint8_t* spell = (spellId > 0 && mgr) ? VCall<uint8_t*>(mgr, kSpellMgrVtblGetSpell, spellId) : nullptr;
		if (!spell || ms == 0) {
			// As the client: no tooltip without a spell or with nothing remaining.
			if (c.valid && c.button == button && c.spellId == 0) continue;
			SetTooltip(button, "");
			memset(&c, 0, sizeof(c));
			c.button = button;
			c.valid = true;
			continue;
		}
		int seconds = ms < 0 ? -1 : ms / 1000;
		const char* caster = *reinterpret_cast<const char**>(slot + kSlotCasterText);
		if (!caster || *reinterpret_cast<int*>(slot + kSlotCasterLen) <= 0) caster = "";
		bool spellChanged = !c.valid || c.button != button || c.spellId != spellId;
		if (!spellChanged && c.seconds == seconds && strncmp(c.caster, caster, kCasterMax - 1) == 0) continue;
		if (spellChanged) BuildSummary(mgr, spell, c.summary, c.cures);
		c.button = button;
		c.spellId = spellId;
		c.seconds = seconds;
		c.valid = true;
		snprintf(c.caster, kCasterMax, "%s", caster);

		char text[kTextMax];
		size_t len = 0;
		const char* name = reinterpret_cast<const char*>(spell + kSpellName);
		if (ms < 0) {
			Append(text, kTextMax, &len, "%.*s:Permanent", kSpellNameMax, name);
		}
		else {
			char duration[40] = {};
			g_formatDuration(duration, kDurationCap, &seconds);
			Append(text, kTextMax, &len, "%.*s (%s)", kSpellNameMax, name, duration);
		}
		const char* sep = g_cfg.multiLine ? "\n" : " | ";
		if (g_cfg.showCaster && c.caster[0]) Append(text, kTextMax, &len, "%sCaster: %s", sep, c.caster);
		if (c.summary[0]) Append(text, kTextMax, &len, "%s%s", sep, c.summary);
		if (c.cures[0]) Append(text, kTextMax, &len, "%s%s", sep, c.cures);
		SetTooltip(button, text);
	}
}

void BuffRefreshDetour(uint8_t* wnd) {
	if (!*g_spellManager) return g_origBuffRefresh(wnd);
	int count = *reinterpret_cast<int*>(wnd + kBuffSlotCount);
	int capacity = *reinterpret_cast<int*>(wnd + kBuffSlotCapacity);
	if (count > capacity) count = capacity;
	Refresh(wnd, *reinterpret_cast<uint8_t**>(wnd + kBuffSlots), count, kBuffSlotSize);
}

void TargetRefreshDetour(uint8_t* wnd) {
	if (!*g_spellManager) return g_origTargetRefresh(wnd);
	int count = *reinterpret_cast<int*>(wnd + kTargetSlotCount);
	Refresh(wnd, *reinterpret_cast<uint8_t**>(wnd + kTargetSlots), count, kTargetSlotSize);
}

#define REF(var) reinterpret_cast<void**>(&var)

// The two refresh functions share their 49-byte prologue up to the slot-array load (+0x308 buff map /
// +0x310 target map). Stolen (5 bytes, position independent): mov [rsp+10],rbx. The stack cookie load
// at +21 is RIP-relative, so nothing more can be stolen.
const char* kBuffRefreshSig =
	"48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 81 EC 70 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
	"48 89 84 24 60 01 00 00 48 8B E9 48 8B 99 08 03 00 00";
const char* kTargetRefreshSig =
	"48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 81 EC 70 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
	"48 89 84 24 60 01 00 00 48 8B E9 48 8B 99 10 03 00 00";

const size_t kRefreshFnSize = 0x1fa;   // both refresh functions are 506 bytes

// Scans a refresh function body for a direct call to `target` (E8 rel32).
bool CallsFunction(const uint8_t* fn, const void* target) {
	for (size_t i = 5; i + 5 <= kRefreshFnSize; ++i) {
		if (fn[i] != 0xE8) continue;
		int32_t rel;
		memcpy(&rel, fn + i + 1, 4);
		if (fn + i + 5 + rel == static_cast<const uint8_t*>(target)) return true;
	}
	return false;
}

// Scans a refresh function body for mov rcx,[rip+disp32] reading `global`.
bool ReadsGlobal(const uint8_t* fn, const void* global) {
	for (size_t i = 5; i + 7 <= kRefreshFnSize; ++i) {
		if (fn[i] != 0x48 || fn[i + 1] != 0x8B || fn[i + 2] != 0x0D) continue;
		int32_t rel;
		memcpy(&rel, fn + i + 3, 4);
		if (fn + i + 7 + rel == static_cast<const uint8_t*>(global)) return true;
	}
	return false;
}

} // namespace

const HookSite kBuffTooltipsHooks[2] = {
	{ kBuffRefreshSig, 5, reinterpret_cast<void*>(&BuffRefreshDetour), REF(g_origBuffRefresh) },
	{ kTargetRefreshSig, 5, reinterpret_cast<void*>(&TargetRefreshDetour), REF(g_origTargetRefresh) },
};

const RefSite kBuffTooltipsRefs[7] = {
	// FUN_1405d2880(char* out, int cap, int* seconds): "%dh %dm %ds" / "%dm %ds" / "%ds"
	{ "48 89 5C 24 10 57 48 83 EC 40 49 8B C0", 0, 0, REF(g_formatDuration) },
	// FUN_1405853f0(CXStr*, const char*): strlen, *str = 0, allocate rep (len+1), copy at rep+0x18
	{ "48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 74 ?? 48 89 5C 24 30", 0, 0, REF(g_cxstrAssign) },
	// FUN_140587670(CXStr*, rep): lock-guarded release, drops the rep's refcount at rep+0
	{ "48 89 5C 24 08 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B DA 48 8B F9 74 ?? 8B 05 ?? ?? ?? ??", 0, 0, REF(g_cxstrRelease) },
	// Spell manager creation in FUN_14026e320: mov [spellManager],rcx; mov rax,[rcx]
	{ "48 89 0D ?? ?? ?? ?? 48 8B 01", 3, 7, REF(g_spellManager) },
	// FUN_1400d3a00: mov rcx,[spellManager]; cmp eax,[rcx+64]; jg; mov rax,[rcx]; call [rax+40] (GetSpell)
	{ "48 8B 0D ?? ?? ?? ?? 3B 41 64 0F 8F ?? ?? ?? ?? 48 8B 01 FF 50 40 48 89 44 24 60", 3, 7, REF(g_spellManagerRead) },
	{ kBuffRefreshSig, 0, 0, REF(g_buffRefreshFn) },
	{ kTargetRefreshSig, 0, 0, REF(g_targetRefreshFn) },
};

const char* BuffTooltipsPrepare() {
	if (g_spellManager != g_spellManagerRead)
		return "the two spell manager references disagree";
	const uint8_t* fns[] = { g_buffRefreshFn, g_targetRefreshFn };
	for (const uint8_t* fn : fns) {
		if (!ReadsGlobal(fn, g_spellManager)) return "a refresh function does not read the spell manager";
		if (!CallsFunction(fn, g_formatDuration)) return "a refresh function does not call the duration formatter";
		if (!CallsFunction(fn, g_cxstrAssign)) return "a refresh function does not call the CXStr constructor";
		if (!CallsFunction(fn, g_cxstrRelease)) return "a refresh function does not call the CXStr release";
	}
	LoadConfig();
	memset(g_windows, 0, sizeof(g_windows));
	return nullptr;
}
