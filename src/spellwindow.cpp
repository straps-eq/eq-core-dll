// SpellWindowExtras: appends a details block to the spell window (CSpellDisplayWnd), the window a
// buff's right-click "Show Spell Information" opens, as well as spell links in items, chat and the
// spell book: one line per effect slot in words with the values the client's own calculator gives
// (a level range when the value scales), then recast/recovery, the classes that get the spell with
// their levels, the dbstr category and optionally the spell id.
//
// The client builds the window's body in SetSpell (FUN_1404fb030: level, skill, mana, cast time,
// caster, stacking, resist, target, range, duration, description, cast messages) and pushes it into
// the "SDW_SpellDescription" STML (this+0x2d8) in the finalise step FUN_1404fcdc0 with
// CStmlWnd::SetSTMLText (FUN_1405ca040, append = 0), which replaces the text. The finalise step also
// runs on refreshes, so we hook it: the original runs, then our block is appended once with the same
// call in append mode. The window keeps the spell id at +0x3a0, the cast level at +0x3a4 (0 unless the
// window came from a link with a level) and the caster name at +0x328/+0x330 (length; set only by the
// buff menu). The client already prints "Caster:" itself when it has one, so ours is off by default.
// The CXStr is built with the client's constructor (FUN_1405853f0) and consumed by SetSTMLText.
#include "spellwindow.h"
#include "spellinfo.h"
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

// CSpellDisplayWnd (ctor FUN_1404f6b10, SetSpell FUN_1404fb030).
const size_t kWndDescription = 0x2d8;   // CStmlWnd* SDW_SpellDescription
const size_t kWndCasterText = 0x328;    // char*
const size_t kWndCasterLen = 0x330;     // int
const size_t kWndSpellId = 0x3a0;       // int
const size_t kWndCastLevel = 0x3a4;     // int

using FinaliseFn = void (*)(uint8_t* wnd);
using CXStrFromCStrFn = void* (*)(void** out, const char* text);
using SetStmlTextFn = void (*)(void* stml, void** text, char append, void* fmt);

FinaliseFn g_origFinalise = nullptr;       // trampoline into FUN_1404fcdc0
CXStrFromCStrFn g_cxstrFromCStr = nullptr; // FUN_1405853f0
SetStmlTextFn g_setStmlText = nullptr;     // FUN_1405ca040

// ---- configuration ([SpellWindow] in dinput8.ini) ----

struct Config {
	bool effects = true;
	bool recast = true;
	bool classes = true;
	bool category = true;
	bool id = false;
	bool caster = false;        // the client prints its own "Caster:" line
	bool all = false;           // also duration, costs, resist, target, range, skill and cast messages
	int maxEffects = 20;
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
	auto Int = [&](const char* key, int def) { return static_cast<int>(GetPrivateProfileIntA("SpellWindow", key, def, ini)); };
	Config c;
	c.effects = Int("ShowEffects", c.effects) != 0;
	c.recast = Int("ShowRecast", c.recast) != 0;
	c.classes = Int("ShowClasses", c.classes) != 0;
	c.category = Int("ShowCategory", c.category) != 0;
	c.id = Int("ShowId", c.id) != 0;
	c.caster = Int("ShowCaster", c.caster) != 0;
	c.all = Int("ShowAll", c.all) != 0;
	c.maxEffects = Clamp(Int("MaxEffects", c.maxEffects), 1, 40);
	char buf[32] = {};
	GetPrivateProfileStringA("SpellWindow", "Colour", "", buf, sizeof(buf), ini);
	if (buf[0]) c.colour = static_cast<uint32_t>(strtoul(buf, nullptr, 16)) & 0xffffff;
	g_cfg = c;
}

// ---- hook ----

const size_t kTextMax = 8192;

void AppendExtras(uint8_t* wnd) {
	void* stml = *reinterpret_cast<void**>(wnd + kWndDescription);
	if (!stml) return;
	const uint8_t* spell = SpellInfoGetSpell(*reinterpret_cast<int*>(wnd + kWndSpellId));
	if (!spell) return;
	int castLevel = *reinterpret_cast<int*>(wnd + kWndCastLevel);

	static char text[kTextMax];
	TextBuf t(text, sizeof(text));
	SpellTextStyle style;
	style.lineBreak = "<BR>";
	style.indent = "";
	style.labelColour = g_cfg.colour;
	style.maxEffects = g_cfg.maxEffects;

	if (g_cfg.caster && *reinterpret_cast<int*>(wnd + kWndCasterLen) > 0) {
		const char* caster = *reinterpret_cast<const char**>(wnd + kWndCasterText);
		if (caster && caster[0]) {
			t.Add("<BR>");
			if (g_cfg.colour) t.Add("<C #%06x>Caster:</C> ", g_cfg.colour);
			else t.Add("Caster: ");
			t.Add("%.63s", caster);
		}
	}
	if (g_cfg.effects) {
		t.Add("<BR>");
		if (g_cfg.colour) t.Add("<C #%06x>Effects:</C>", g_cfg.colour);
		else t.Add("Effects:");
		size_t mark = t.len;
		SpellTextStyle effects = style;
		effects.indent = "- ";
		SpellInfoWrite(t, spell, castLevel, kSpellEffects, effects);
		if (t.len == mark) t.Add("<BR>- none");
	}
	unsigned parts = 0;
	if (g_cfg.recast) parts |= kSpellTimes;
	if (g_cfg.all) parts |= kSpellDuration | kSpellCosts | kSpellResist | kSpellTarget | kSpellRange | kSpellSkill;
	if (g_cfg.classes) parts |= kSpellClasses;
	if (g_cfg.category) parts |= kSpellCategory;
	if (g_cfg.all) parts |= kSpellMessages;
	if (g_cfg.id) parts |= kSpellId;
	SpellInfoWrite(t, spell, castLevel, parts, style);
	if (!t.len) return;

	void* cxstr = nullptr;
	g_cxstrFromCStr(&cxstr, text);
	if (!cxstr) return;
	g_setStmlText(stml, &cxstr, 1, nullptr);   // append; releases cxstr
}

void FinaliseDetour(uint8_t* wnd) {
	static bool inside = false;
	g_origFinalise(wnd);
	if (inside || !SpellInfoInGame()) return;
	inside = true;
	AppendExtras(wnd);
	inside = false;
}

#define REF(var) reinterpret_cast<void**>(&var)

// CSpellDisplayWnd finalise FUN_1404fcdc0: push rbx; sub rsp,20; mov rbx,rcx; cmp [rcx+2d8],0.
// Stolen (6 bytes, position independent): push rbx; sub rsp,20.
const char* kFinaliseSig = "40 53 48 83 EC 20 48 8B D9 48 83 B9 D8 02 00 00 00";

} // namespace

const HookSite kSpellWindowExtrasHooks[1] = {
	{ kFinaliseSig, 6, reinterpret_cast<void*>(&FinaliseDetour), REF(g_origFinalise) },
};

const RefSite kSpellWindowExtrasRefs[16] = {
	// CXStr::CXStr(const char*) FUN_1405853f0
	{ "48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 74 ?? 48 89 5C 24 30", 0, 0, REF(g_cxstrFromCStr) },
	// CStmlWnd::SetSTMLText FUN_1405ca040(stml, CXStr*, append, fmt)
	{ "48 89 5C 24 18 48 89 6C 24 20 48 89 54 24 10 56 57 41 56", 0, 0, REF(g_setStmlText) },
	SPELLINFO_REF_SITES,
};

const char* SpellWindowExtrasPrepare() {
	if (const char* e = SpellInfoPrepare()) return e;
	LoadConfig();
	return nullptr;
}
