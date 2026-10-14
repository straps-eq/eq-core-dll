#pragma once
// Shared spell text formatter for the spell window, item window and buff tooltips: effect slots in
// words (values from the client's own calculator), duration (client's duration function), costs,
// times, resist, target, skill, classes, category and the cast messages. Features that use it list
// its client references through SPELLINFO_REF_SITES and call SpellInfoPrepare() from their prepare.
#include "patches.h"
#include <cstdint>
#include <cstddef>

// Fixed-capacity text sink. Once a write would not fit, it and everything after it are dropped.
struct TextBuf {
	char* buf;
	size_t cap;
	size_t len = 0;
	bool full = false;
	TextBuf(char* b, size_t c) : buf(b), cap(c) { if (cap) buf[0] = '\0'; }
	void Add(const char* fmt, ...);
};

// Which lines SpellInfoWrite produces (bit set).
enum SpellParts : unsigned {
	kSpellEffects  = 1u << 0,   // one line per effect slot
	kSpellDuration = 1u << 1,   // "Duration: ..." (nothing for instant spells)
	kSpellTimes    = 1u << 2,   // cast / recast / recovery
	kSpellCosts    = 1u << 3,   // mana / endurance
	kSpellResist   = 1u << 4,   // detrimental spells: resist type and adjust
	kSpellTarget   = 1u << 5,
	kSpellRange    = 1u << 6,   // range, AE range
	kSpellSkill    = 1u << 7,
	kSpellClasses  = 1u << 8,   // "Classes: Cleric (1), Paladin (9)"
	kSpellCategory = 1u << 9,   // dbstr category / subcategory
	kSpellMessages = 1u << 10,  // cast on you / cast on other / wears off
	kSpellId       = 1u << 11,
};

struct SpellTextStyle {
	const char* lineBreak = "<BR>";  // every line starts with this ("\n" for tooltips)
	const char* indent = "";         // then this
	uint32_t labelColour = 0;        // 0x00RRGGBB for STML <C> tags round labels; 0 = none
	int maxEffects = 20;             // effect lines per spell
	bool skipCounters = false;       // leave disease/poison/curse/corruption counters to the caller
};

// The spell for an id, or nullptr when the spell manager does not know it.
uint8_t* SpellInfoGetSpell(int id);
const char* SpellInfoName(const uint8_t* spell);   // char[64] inside the SPELL
// Appends the chosen parts. castLevel > 0 computes values and duration for that level; 0 shows the
// spell's own level range ("by 10 (L1) to 60 (L50)").
void SpellInfoWrite(TextBuf& out, const uint8_t* spell, int castLevel, unsigned parts, const SpellTextStyle& style);
// "needs cure: 3 poison, 2 disease" for a spell carrying counters; false when it carries none.
bool SpellInfoCures(TextBuf& out, const uint8_t* spell);
// Seconds -> "1h 2m 3s" / "2m 3s" / "3s".
void SpellInfoSeconds(TextBuf& out, int seconds);
bool SpellInfoInGame();   // game state 5 and the spell manager / local PC exist
// Cross-checks on the resolved references; an error string skips the calling feature.
const char* SpellInfoPrepare();

// Client references the formatter needs. Each feature using it splices SPELLINFO_REF_SITES (14
// entries) into its own RefSite table; resolving them more than once stores the same addresses.
namespace spellinfo {
using CalcValueFn = int64_t (*)(void* affects, const uint8_t* spell, uint8_t level, uint8_t slot, void* affect, int64_t base, uint8_t applyMax);
using DurationFn = unsigned (*)(void* affects, const uint8_t* spell, uint8_t level, uint8_t strictUnknown);
using SpellStringFn = const char* (*)(void* table, unsigned spellId, int which);
using DbStrFn = const char* (*)(void* table, unsigned id, unsigned type, bool* found);
using SkillNameIdFn = int (*)(void* table, unsigned skill);
using EqStrFn = const char* (*)(void* table, int id, int flags);
extern CalcValueFn g_calcValue;         // FUN_1400d48e0
extern DurationFn g_duration;           // FUN_1400e27a0
extern SpellStringFn g_spellString;     // FUN_1400c2410
extern DbStrFn g_dbstr;                 // FUN_14018b660
extern SkillNameIdFn g_skillNameId;     // FUN_14032e520
extern EqStrFn g_eqstr;                 // FUN_140673e50
extern void** g_spellStringTable;       // &DAT_140d99970
extern void** g_dbstrTable;             // &DAT_140db4de0
extern void** g_skillTable;             // &DAT_140ee6418
extern void** g_eqstrTable;             // &DAT_140e65d98
extern uint8_t** g_spellManager;        // &DAT_140ee6480 (from its write)
extern uint8_t** g_spellManagerRead;    // &DAT_140ee6480 (from a read, cross-check)
extern uint8_t** g_pLocalPC;            // &DAT_140e65d90
extern uint8_t** g_everQuest;           // &DAT_140ee7010; +0x5e4 game state
}
const size_t kSpellInfoRefCount = 14;
#define SPELLINFO_REF(var) reinterpret_cast<void**>(&spellinfo::var)
#define SPELLINFO_REF_SITES \
	/* Spell manager creation in FUN_14026e320: mov [spellManager],rcx; mov rax,[rcx] */ \
	{ "48 89 0D ?? ?? ?? ?? 48 8B 01", 3, 7, SPELLINFO_REF(g_spellManager) }, \
	/* Buff-slot scan FUN_1400d3a00: mov rcx,[spellManager]; cmp eax,[rcx+64]; jg; mov rax,[rcx]; call [rax+40] */ \
	{ "48 8B 0D ?? ?? ?? ?? 3B 41 64 0F 8F ?? ?? ?? ?? 48 8B 01 FF 50 40 48 89 44 24 60", 3, 7, SPELLINFO_REF(g_spellManagerRead) }, \
	/* CTargetWnd per-frame FUN_14050e150: mov [rax+13bc],-1; mov rdx,[pTarget]; mov rcx,[pLocalPC]; call GetConLevel */ \
	{ "C7 80 BC 13 00 00 FF FF FF FF 48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??", 20, 24, SPELLINFO_REF(g_pLocalPC) }, \
	/* mov rcx,[pEverQuest]; cmp dword [rcx+5e4],5 (the client's in-game test) */ \
	{ "48 8B 0D ?? ?? ?? ?? 83 B9 E4 05 00 00 05", 3, 7, SPELLINFO_REF(g_everQuest) }, \
	/* FUN_1400d48e0(affects, spell, u8 level, u8 slot, affect, i64 base, bool applyMax): effect value calculator */ \
	{ "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8B 44 24 78", 0, 0, SPELLINFO_REF(g_calcValue) }, \
	/* FUN_1400e27a0(affects, spell, u8 level, bool strict): duration in ticks (0xffffffff permanent) */ \
	{ "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 33 FF", 0, 0, SPELLINFO_REF(g_duration) }, \
	/* FUN_1400c2410(table, spellId, which): spell string (2 on you, 3 on other, 4 wears off); "" when missing */ \
	{ "44 8B D2 4C 8D 0D ?? ?? ?? ??", 0, 0, SPELLINFO_REF(g_spellString) }, \
	/* FUN_1400e1a70: mov rcx,[spellStringTable]; test rcx,rcx; jz; test r15,r15 */ \
	{ "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 4D 85 FF", 3, 7, SPELLINFO_REF(g_spellStringTable) }, \
	/* FUN_14018b660(table, id, type, bool* found): dbstr lookup */ \
	{ "48 89 5C 24 08 57 48 83 EC 20 44 8B DA", 0, 0, SPELLINFO_REF(g_dbstr) }, \
	/* dbstr table creation in FUN_14027cea0: mov [dbstrTable],rdi; mov ecx,20 */ \
	{ "48 89 3D ?? ?? ?? ?? B9 20 00 00 00", 3, 7, SPELLINFO_REF(g_dbstrTable) }, \
	/* FUN_14032e520(table, skill): skill < 100 -> eqstr id at entry+8, else 0 */ \
	{ "83 FA 63 77 ?? 8B C2 48 8B 0C C1 48 85 C9 74 ?? 8B 41 08", 0, 0, SPELLINFO_REF(g_skillNameId) }, \
	/* skill table creation in FUN_140261bd0: mov [skillTable],rax; mov edi,r12d */ \
	{ "48 89 05 ?? ?? ?? ?? 41 8B FC", 3, 7, SPELLINFO_REF(g_skillTable) }, \
	/* FUN_140673e50(table, id, flags): eqstr text */ \
	{ "48 89 5C 24 08 57 48 83 EC 20 44 8B 59 0C", 0, 0, SPELLINFO_REF(g_eqstr) }, \
	/* eqstr table creation in FUN_140261bd0: mov [eqstrTable],r12; mov rcx,rbx */ \
	{ "4C 89 25 ?? ?? ?? ?? 48 8B CB", 3, 7, SPELLINFO_REF(g_eqstrTable) }
