// SpellWindowExtras: adds a details block to the spell window (CSpellDisplayWnd), the window a
// buff's right-click "Show Spell Information" opens, as well as spell links in items, chat and the
// spell book. Below the client's own text: "Spell Effect: <description>" when the client did not print
// the description itself, then one block in the layout players know from other tools: ID (and duration,
// range and resist only when the client's body lacks those lines, or always with ShowDuplicates=1),
// recovery and recast, one "Slot N:" line per effect in words with
// the values the client's own calculator gives, the classes that get the spell with their levels, the
// dbstr category and subcategory, and the spell icon number.
//
// The client builds the window's body in SetSpell (FUN_1404fb030: level, skill, mana, cast time,
// caster, stacking, resist, target, range, duration, description, cast messages) and pushes it into
// the "SDW_SpellDescription" STML (this+0x2d8) in the finalise step FUN_1404fcdc0 with
// CStmlWnd::SetSTMLText (FUN_1405ca040). The finalise step also runs on refreshes, so we hook it: the
// original runs, then the window's text is read back, ours is added after it and the whole text is set
// again (stml.h explains why SetSTMLText's "append" flag cannot be used). The window keeps the spell
// id at +0x3a0, the cast level at +0x3a4 (0 unless the window came from a link with a level) and the
// caster name at +0x328/+0x330 (length; set only by the buff menu).
#include "spellwindow.h"
#include "spellinfo.h"
#include "stml.h"
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

FinaliseFn g_origFinalise = nullptr;         // trampoline into FUN_1404fcdc0
StmlCXStrCtorFn g_cxstrFromCStr = nullptr;   // FUN_1405853f0
StmlSetTextFn g_setStmlText = nullptr;       // FUN_1405ca040

// ---- configuration ([SpellWindow] in dinput8.ini) ----

struct Config {
	bool description = true;    // "Spell Effect: ..." when the client's text lacks the description
	bool details = true;        // ID/duration, recovery/recast, range, resist, effect slots
	bool classes = true;
	bool category = true;       // category, subcategory, spell icon
	bool caster = false;        // the client prints its own "Caster:" line
	bool messages = false;      // cast on you / on other / wears off (the client prints them too)
	bool duplicates = false;    // repeat Duration / Range / Resist even when the client's text has them
	int maxEffects = 20;
	uint32_t colour = 0x00ffff; // block colour (0 = plain)
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
	c.description = Int("ShowDescription", c.description) != 0;
	c.details = Int("ShowDetails", c.details) != 0;
	c.classes = Int("ShowClasses", c.classes) != 0;
	c.category = Int("ShowCategory", c.category) != 0;
	c.caster = Int("ShowCaster", c.caster) != 0;
	c.messages = Int("ShowMessages", c.messages) != 0;
	c.duplicates = Int("ShowDuplicates", c.duplicates) != 0;
	c.maxEffects = Clamp(Int("MaxEffects", c.maxEffects), 1, 40);
	char buf[32] = {};
	GetPrivateProfileStringA("SpellWindow", "Colour", "", buf, sizeof(buf), ini);
	if (buf[0]) c.colour = static_cast<uint32_t>(strtoul(buf, nullptr, 16)) & 0xffffff;
	g_cfg = c;
}

// ---- text ----

const size_t kTextMax = 32768;      // the client's text plus ours
const size_t kDescriptionMax = 2048;

void Colour(TextBuf& t, bool open) {
	if (!g_cfg.colour) return;
	if (open) t.Add("<C \"#%06X\">", g_cfg.colour);
	else t.Add("</C>");
}

void AppendExtras(uint8_t* wnd) {
	void* stml = *reinterpret_cast<void**>(wnd + kWndDescription);
	if (!stml) return;
	const uint8_t* spell = SpellInfoGetSpell(*reinterpret_cast<int*>(wnd + kWndSpellId));
	if (!spell) return;
	int castLevel = *reinterpret_cast<int*>(wnd + kWndCastLevel);

	// The client's text ends in line breaks; after trimming them, every section below starts with one "<BR>"
	// (a blank line) and every line with another, so sections are separated by exactly one blank line.
	static char text[kTextMax];
	size_t native = StmlTrimBreaks(text, StmlCurrentText(stml, text, sizeof(text)));
	TextBuf t(text + native, sizeof(text) - native);
	SpellTextStyle style;
	style.lineBreak = "<BR>";
	style.indent = "";
	style.labelColour = 0;
	style.maxEffects = g_cfg.maxEffects;
	style.classesBare = true;
	style.gap = kStmlGap;
	// The client's body already has "Duration:", "Range:" and (detrimental spells) "Resist:" lines.
	style.duration = g_cfg.duplicates || !StmlHasText(text, "Duration:");
	style.range = g_cfg.duplicates || !StmlHasText(text, "Range:");
	style.resist = g_cfg.duplicates || !StmlHasText(text, "Resist:");

	if (g_cfg.description) {
		static char description[kDescriptionMax];
		TextBuf d(description, sizeof(description));
		if (SpellInfoDescription(d, spell, castLevel) && !StmlHasText(text, description))
			t.Add("<BR><BR>Spell Effect: %s", description);
	}
	if (g_cfg.caster && *reinterpret_cast<int*>(wnd + kWndCasterLen) > 0) {
		const char* caster = *reinterpret_cast<const char**>(wnd + kWndCasterText);
		if (caster && caster[0]) t.Add("<BR><BR>Caster: %.63s", caster);
	}
	size_t blockStart = t.len;
	Colour(t, true);
	size_t mark = t.len;
	if (g_cfg.details) {
		t.Add("<BR>");
		SpellInfoDetails(t, spell, castLevel, style);
	}
	if (g_cfg.classes) {
		t.Add("<BR>");
		SpellInfoWrite(t, spell, castLevel, kSpellClasses, style);
	}
	if (g_cfg.category) {
		const char* cat = nullptr;
		const char* sub = nullptr;
		SpellInfoCategory(spell, &cat, &sub);
		t.Add("<BR>");
		if (cat) t.Add("<BR>Category: %.47s", cat);
		if (sub) t.Add("<BR>Subcategory: %.47s", sub);
		t.Add("<BR>Spell Icon: %d", SpellInfoIcon(spell));
	}
	if (g_cfg.messages) {
		t.Add("<BR>");
		SpellInfoWrite(t, spell, castLevel, kSpellMessages, style);
	}
	if (t.len == mark && !t.full) {
		t.len = blockStart;   // nothing was added: drop the colour tag
		t.buf[t.len] = '\0';
	}
	else {
		Colour(t, false);
	}
	if (!t.len) return;
	StmlSetText(stml, text, g_cxstrFromCStr, g_setStmlText);
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

const RefSite kSpellWindowExtrasRefs[17] = {
	// CXStr::CXStr(const char*) FUN_1405853f0
	{ "48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 74 ?? 48 89 5C 24 30", 0, 0, REF(g_cxstrFromCStr) },
	// CStmlWnd::SetSTMLText FUN_1405ca040(stml, CXStr*, flag, fmt)
	{ "48 89 5C 24 18 48 89 6C 24 20 48 89 54 24 10 56 57 41 56", 0, 0, REF(g_setStmlText) },
	SPELLINFO_REF_SITES,
};

const char* SpellWindowExtrasPrepare() {
	if (const char* e = SpellInfoPrepare()) return e;
	LoadConfig();
	return nullptr;
}
