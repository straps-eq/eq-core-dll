// Shared spell text: effect slots in words, duration, costs, times, resist, target, skill, classes,
// category and the cast messages, for the spell window (spellwindow.cpp), the item window
// (itemdisplay.cpp) and the buff tooltips (buffs.cpp).
//
// Nothing about a spell is recomputed here that the client already computes: effect values come from
// the client's own calculator (FUN_1400d48e0: calc code, level, base, max, the attack-speed special
// case and the max clamp, exactly as the client applies them), the duration from its duration function
// (FUN_1400e27a0, ticks; 0xffffffff permanent), cast messages from the spell string table
// (FUN_1400c2410 on DAT_140d99970), category names from the dbstr table (FUN_14018b660 on
// DAT_140db4de0, type 5), and the skill name from the skill table (FUN_14032e520 on DAT_140ee6418 ->
// eqstr id, FUN_140673e50 on DAT_140e65d98 -> text). Both calculators take the local PC's affect block
// (pLocalPC + 0x27b8) as their first argument, as every caller in the client does; with no running
// buff (affect = null) the time-dependent calc codes (1000s/4000s) fall back to the base value, which is
// what the client does too.
//
// Only the wording is ours: a table of the common effect ids (the "SPA" column of the spell file)
// with how each is phrased; anything not in the table prints as "Effect n: value".
//
// SPELL fields (parser FUN_1401d8aa0): +0x00 range, +0x04 AE range, +0x10 cast ms, +0x14 recovery ms,
// +0x18 recast ms, +0x1c duration formula, +0x28 mana, +0x2c mana % of max, +0x60 first slot index,
// +0x64 slot count, +0x6c resist adjust, +0x8c id, +0x94 category, +0x98 subcategory, +0xac endurance,
// +0xb0 endurance %, +0xe4 PC/merc-only flag, +0x15c+class level per class (255 = cannot), +0x181
// beneficial, +0x182 resist type, +0x183 target type, +0x185 skill, +0x18f name, +0x1cf extra string.
// Effect slot (spell manager vtbl+0x48, index +0x60 + i): +0x08 base1, +0x10 base2, +0x18 max, +0x20 calc,
// +0x24 SPA (254 = empty).
#include "spellinfo.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

void TextBuf::Add(const char* fmt, ...) {
	if (full || len >= cap) return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + len, cap - len, fmt, ap);
	va_end(ap);
	if (n < 0 || static_cast<size_t>(n) >= cap - len) {
		buf[len] = '\0';
		full = true;
		return;
	}
	len += static_cast<size_t>(n);
}

namespace {

// SPELL
const size_t kOffRange = 0x00;
const size_t kOffAERange = 0x04;
const size_t kOffCastMs = 0x10;
const size_t kOffRecoveryMs = 0x14;
const size_t kOffRecastMs = 0x18;
const size_t kOffDurationFormula = 0x1c;
const size_t kOffMana = 0x28;
const size_t kOffManaPct = 0x2c;
const size_t kOffSlotBase = 0x60;
const size_t kOffSlotCount = 0x64;
const size_t kOffResistAdjust = 0x6c;
const size_t kOffId = 0x8c;
const size_t kOffCategory = 0x94;
const size_t kOffSubcategory = 0x98;
const size_t kOffEndurance = 0xac;
const size_t kOffEndurancePct = 0xb0;
const size_t kOffPcOnly = 0xe4;
const size_t kOffClassLevels = 0x15c;    // + class (1..16)
const size_t kOffBeneficial = 0x181;
const size_t kOffResistType = 0x182;
const size_t kOffTargetType = 0x183;
const size_t kOffSkill = 0x185;
const size_t kOffName = 0x18f;
const size_t kOffExtra = 0x1cf;          // char[0x20]
// effect slot
const size_t kSlotBase1 = 0x08;
const size_t kSlotBase2 = 0x10;
const size_t kSlotMax = 0x18;
const size_t kSlotCalc = 0x20;
const size_t kSlotSpa = 0x24;
const int kSpaEmpty = 254;
// spell manager
const size_t kMgrMaxSpellId = 0x64;
const size_t kMgrVtblGetSpell = 0x40;      // SPELL* (mgr, int64 id); unknown ids give the manager's empty spell
const size_t kMgrVtblGetSlot = 0x48;       // EffectSlot* (mgr, int index)
// local PC / game state
const size_t kPcAffects = 0x27b8;          // PcZoneClient affect block, first argument of both calculators
const size_t kGameState = 0x5e4;
const int kGameStateInGame = 5;

const int kClassCount = 16;
const int kMaxLevel = 125;
const int kMaxSlots = 64;
const int kDbstrSpellCategory = 5;
const unsigned kDurationPermanent = 0xffffffffu;
const int kTickSeconds = 6;

} // namespace

namespace spellinfo {
CalcValueFn g_calcValue = nullptr;
DurationFn g_duration = nullptr;
SpellStringFn g_spellString = nullptr;
DbStrFn g_dbstr = nullptr;
SkillNameIdFn g_skillNameId = nullptr;
EqStrFn g_eqstr = nullptr;
void** g_spellStringTable = nullptr;
void** g_dbstrTable = nullptr;
void** g_skillTable = nullptr;
void** g_eqstrTable = nullptr;
uint8_t** g_spellManager = nullptr;
uint8_t** g_spellManagerRead = nullptr;
uint8_t** g_pLocalPC = nullptr;
uint8_t** g_everQuest = nullptr;
}
using namespace spellinfo;

namespace {

template <typename R, typename... A>
R VCall(void* obj, size_t slot, A... args) {
	auto fn = *reinterpret_cast<R (**)(void*, A...)>(*reinterpret_cast<uint8_t**>(obj) + slot);
	return fn(obj, args...);
}

int32_t I32(const uint8_t* p, size_t off) { return *reinterpret_cast<const int32_t*>(p + off); }
int64_t I64(const uint8_t* p, size_t off) { return *reinterpret_cast<const int64_t*>(p + off); }
float F32(const uint8_t* p, size_t off) { return *reinterpret_cast<const float*>(p + off); }
long long Abs(long long v) { return v < 0 ? -v : v; }

void* Affects() {
	uint8_t* pc = *g_pLocalPC;
	return pc ? pc + kPcAffects : nullptr;
}

// ---- wording tables ----

const char* const kClassNames[kClassCount + 1] = {
	"", "Warrior", "Cleric", "Paladin", "Ranger", "Shadow Knight", "Druid", "Monk", "Bard", "Rogue", "Shaman",
	"Necromancer", "Wizard", "Magician", "Enchanter", "Beastlord", "Berserker",
};

const char* const kResistNames[] = {
	"Unresistable", "Magic", "Fire", "Cold", "Poison", "Disease", "Chromatic", "Prismatic", "Physical", "Corruption",
};

struct NamedId {
	int id;
	const char* name;
};
const NamedId kTargetTypes[] = {
	{ 1, "Line of Sight" }, { 2, "Area of Effect (PCs)" }, { 3, "Group v1" }, { 4, "Point Blank AE" }, { 5, "Single" },
	{ 6, "Self" }, { 8, "Targeted AE" }, { 9, "Animal" }, { 10, "Undead" }, { 11, "Summoned" }, { 13, "Lifetap" },
	{ 14, "Pet" }, { 15, "Corpse" }, { 16, "Plant" }, { 17, "Giant" }, { 18, "Dragon" }, { 20, "Targeted AE Lifetap" },
	{ 24, "AE Undead" }, { 25, "AE Summoned" }, { 32, "AE Caster" }, { 33, "NPC Hate List" }, { 34, "Dungeon Object" },
	{ 35, "Muramite" }, { 36, "Area (PCs only)" }, { 37, "Area (NPCs only)" }, { 38, "Summoned Pet" }, { 39, "Group (no pets)" },
	{ 40, "AE PC v2" }, { 41, "Group v2" }, { 42, "Directional AE" }, { 43, "Group (with pets)" }, { 44, "Beam" },
	{ 45, "Free Target" }, { 46, "Target's Target" }, { 47, "Pet Owner" }, { 50, "Targeted AE (no player pets)" },
};

// How an effect id is phrased. value = what the client's calculator returns for the slot.
enum Kind : uint8_t {
	kStat,      // "Increase <name> by V<unit>" / "Decrease ..." by the sign of V
	kFlag,      // "<name>"
	kHaste,     // V is 100-based: "Increase Attack Speed by V-100%"
	kMillis,    // "<name> for V/1000 sec"
	kLevelCap,  // "<name> (up to level <max>)"
	kOffRef,  // base1 is a spell id: "<name>: <spell>"
	kChanceRef, // base1 chance %, base2 spell id: "<name> (V%): <spell>"
	kExtra,     // "<name>: <spell's extra string>"
	kShield,    // damage shield: stored negative when it hurts the attacker
	kRaw,       // "<name>: V"
	kAbsorb,    // "<name>: absorb V damage"
	kMitigate,  // "<name>: V% (up to <max> total)"
	kPctHeal,   // "Increase Hitpoints by V% (up to <max>)"
	kLimitSpellType, kLimitResist, kLimitSpell, kLimitEffect, kLimitTarget,
	kLimitRaw,  // "Limit: <name> V<unit>"
};

struct SpaEntry {
	int spa;
	Kind kind;
	const char* name;
	const char* unit;   // "", "%", " sec", " ticks"
};

// The effect ids players meet most, in the spell file's numbering. Anything missing prints generically.
const SpaEntry kSpaTable[] = {
	{ 0, kStat, "Hitpoints", "" }, { 1, kStat, "AC", "" }, { 2, kStat, "Attack", "" }, { 3, kStat, "Movement Speed", "%" },
	{ 4, kStat, "STR", "" }, { 5, kStat, "DEX", "" }, { 6, kStat, "AGI", "" }, { 7, kStat, "STA", "" }, { 8, kStat, "INT", "" },
	{ 9, kStat, "WIS", "" }, { 10, kStat, "CHA", "" }, { 11, kHaste, "Attack Speed", "%" }, { 12, kFlag, "Invisibility", "" },
	{ 13, kFlag, "See Invisible", "" }, { 14, kFlag, "Water Breathing", "" }, { 15, kStat, "Mana", "" },
	{ 18, kFlag, "Pacify", "" }, { 19, kStat, "Faction", "" }, { 20, kFlag, "Blindness", "" }, { 21, kMillis, "Stun", "" },
	{ 22, kLevelCap, "Charm", "" }, { 23, kLevelCap, "Fear", "" }, { 24, kStat, "Endurance", "" }, { 25, kFlag, "Bind Affinity", "" },
	{ 26, kFlag, "Gate", "" }, { 27, kRaw, "Dispel Magic", "" }, { 28, kFlag, "Invisibility to Undead", "" },
	{ 29, kFlag, "Invisibility to Animals", "" }, { 30, kStat, "Aggro Radius", "" }, { 31, kLevelCap, "Mesmerize", "" },
	{ 32, kRaw, "Summon Item (id)", "" }, { 33, kExtra, "Summon Pet", "" }, { 35, kStat, "Disease Counter", "" },
	{ 36, kStat, "Poison Counter", "" }, { 40, kFlag, "Invulnerability", "" }, { 41, kFlag, "Shadowstep", "" },
	{ 46, kStat, "Fire Resist", "" }, { 47, kStat, "Cold Resist", "" }, { 48, kStat, "Poison Resist", "" },
	{ 49, kStat, "Disease Resist", "" }, { 50, kStat, "Magic Resist", "" }, { 52, kFlag, "Sense Undead", "" },
	{ 53, kFlag, "Sense Summoned", "" }, { 54, kFlag, "Sense Animals", "" }, { 55, kAbsorb, "Rune", "" },
	{ 56, kFlag, "True North", "" }, { 57, kFlag, "Levitate", "" }, { 58, kRaw, "Illusion (race)", "" },
	{ 59, kShield, "Damage Shield", "" }, { 61, kFlag, "Identify", "" }, { 63, kStat, "Memory Blur Chance", "%" },
	{ 64, kMillis, "Spin Stun", "" }, { 65, kFlag, "Infravision", "" }, { 66, kFlag, "Ultravision", "" },
	{ 67, kFlag, "Eye of Zomm", "" }, { 68, kFlag, "Reclaim Energy", "" }, { 69, kStat, "Max Hitpoints", "" },
	{ 71, kExtra, "Summon Undead Pet", "" }, { 73, kFlag, "Bind Sight", "" }, { 74, kFlag, "Feign Death", "" },
	{ 75, kFlag, "Voice Graft", "" }, { 76, kFlag, "Sentinel", "" }, { 77, kFlag, "Locate Corpse", "" },
	{ 78, kAbsorb, "Spell Rune", "" }, { 79, kStat, "Hitpoints", "" }, { 81, kStat, "Resurrect (experience)", "%" },
	{ 82, kFlag, "Summon Player", "" }, { 83, kExtra, "Teleport", "" }, { 84, kFlag, "Toss Up", "" },
	{ 85, kOffRef, "Add Proc", "" }, { 86, kStat, "Assist Radius", "" }, { 87, kStat, "Magnification", "%" },
	{ 88, kExtra, "Evacuate", "" }, { 89, kStat, "Size", "%" }, { 91, kLevelCap, "Summon Corpse", "" },
	{ 92, kStat, "Hate", "" }, { 93, kFlag, "Stop Rain", "" }, { 95, kFlag, "Sacrifice", "" }, { 96, kFlag, "Silence", "" },
	{ 97, kStat, "Max Mana", "" }, { 98, kHaste, "Attack Speed", "%" }, { 99, kFlag, "Root", "" }, { 100, kStat, "Hitpoints", "" },
	{ 101, kFlag, "Complete Heal", "" }, { 102, kFlag, "Fearless", "" }, { 103, kFlag, "Call Pet", "" },
	{ 104, kExtra, "Translocate", "" }, { 105, kFlag, "Prevent Gate", "" }, { 106, kExtra, "Summon Warder", "" },
	{ 108, kExtra, "Summon Familiar", "" }, { 109, kRaw, "Summon Item into Bag (id)", "" }, { 111, kStat, "All Resists", "" },
	{ 112, kStat, "Effective Casting Level", "" }, { 113, kExtra, "Summon Mount", "" }, { 114, kStat, "Hate Generated", "%" },
	{ 115, kFlag, "Remove Hunger and Thirst", "" }, { 116, kStat, "Curse Counter", "" }, { 117, kFlag, "Make Weapons Magical", "" },
	{ 118, kStat, "Singing Skill", "" }, { 119, kStat, "Attack Speed (overhaste)", "%" }, { 120, kStat, "Healing Taken", "%" },
	{ 121, kStat, "Reverse Damage Shield", "" }, { 123, kFlag, "Screech", "" }, { 124, kStat, "Spell Damage", "%" },
	{ 125, kStat, "Healing", "%" }, { 126, kStat, "Spell Resist Rate", "%" }, { 127, kStat, "Spell Cast Time", "%" },
	{ 128, kStat, "Spell Duration", "%" }, { 129, kStat, "Spell Range", "%" }, { 130, kStat, "Spell and Bash Hate", "%" },
	{ 131, kStat, "Chance to Not Consume Reagent", "%" }, { 132, kStat, "Spell Mana Cost", "%" },
	{ 134, kLimitRaw, "Max Level", "" }, { 135, kLimitResist, "Resist", "" }, { 136, kLimitTarget, "Target", "" },
	{ 137, kLimitEffect, "Effect", "" }, { 138, kLimitSpellType, "Spell Type", "" }, { 139, kLimitSpell, "Spell", "" },
	{ 140, kLimitRaw, "Min Duration", " ticks" }, { 141, kFlag, "Limit: Instant spells only", "" }, { 142, kLimitRaw, "Min Level", "" },
	{ 143, kLimitRaw, "Min Cast Time", " ms" }, { 144, kLimitRaw, "Max Cast Time", " ms" }, { 147, kPctHeal, "Hitpoints", "%" },
	{ 148, kLimitRaw, "Stacking: Block (effect)", "" }, { 149, kLimitRaw, "Stacking: Overwrite (effect)", "" },
	{ 150, kStat, "Death Save Chance", "%" }, { 151, kFlag, "Suspend Pet", "" }, { 152, kExtra, "Summon Swarm Pets", "" },
	{ 153, kStat, "Balance Group Health", "%" }, { 154, kStat, "Remove Detrimental", "" }, { 157, kStat, "Spell Damage Shield", "" },
	{ 158, kStat, "Reflect Spell Chance", "%" }, { 161, kMitigate, "Mitigate Spell Damage", "" }, { 162, kMitigate, "Mitigate Melee Damage", "" },
	{ 163, kRaw, "Absorb Hits (count)", "" }, { 167, kStat, "Pet Power", "" }, { 168, kStat, "Melee Damage Taken", "%" },
	{ 169, kStat, "Chance to Critical Hit", "%" }, { 170, kStat, "Chance to Critical Nuke", "%" },
	{ 171, kStat, "Chance to Crippling Blow", "%" }, { 172, kStat, "Chance to Avoid Melee", "%" }, { 173, kStat, "Chance to Riposte", "%" },
	{ 174, kStat, "Chance to Dodge", "%" }, { 175, kStat, "Chance to Parry", "%" }, { 176, kStat, "Chance to Dual Wield", "%" },
	{ 177, kStat, "Chance to Double Attack", "%" }, { 178, kStat, "Melee Lifetap", "%" }, { 179, kStat, "All Instrument Skills", "" },
	{ 180, kStat, "Chance to Resist Spells", "%" }, { 181, kStat, "Chance to Resist Fear", "%" }, { 182, kStat, "Weapon Delay", "%" },
	{ 184, kStat, "Chance to Hit", "%" }, { 185, kStat, "Skill Damage", "%" }, { 186, kStat, "Minimum Damage", "%" },
	{ 187, kStat, "Balance Group Mana", "%" }, { 188, kStat, "Chance to Block", "%" }, { 189, kStat, "Endurance", "" },
	{ 190, kStat, "Max Endurance", "" }, { 191, kFlag, "Amnesia", "" }, { 192, kStat, "Hate", "" }, { 193, kRaw, "Skill Attack (base damage)", "" },
	{ 194, kFlag, "Fade", "" }, { 195, kStat, "Stun Resist", "%" }, { 196, kStat, "Strikethrough", "%" },
	{ 197, kStat, "Skill Damage Taken", "%" }, { 198, kStat, "Endurance", "" }, { 199, kFlag, "Taunt", "" },
	{ 200, kStat, "Proc Chance", "%" }, { 201, kOffRef, "Ranged Proc", "" }, { 202, kFlag, "Illusion Other", "" },
	{ 203, kFlag, "Mass Group Buff", "" }, { 204, kFlag, "Group Fear Immunity", "" }, { 205, kFlag, "Rampage", "" },
	{ 206, kFlag, "AE Taunt", "" }, { 209, kStat, "Dispel Beneficial", "" }, { 214, kStat, "Max Hitpoints", "%" },
	{ 216, kStat, "Accuracy", "" }, { 220, kStat, "Skill Damage Bonus", "" }, { 227, kStat, "Skill Reuse Time", " sec" },
	{ 254, kFlag, "", "" }, { 258, kStat, "Chance to Triple Backstab", "%" }, { 260, kStat, "Instrument Modifier", "" },
	{ 262, kStat, "Stat Cap", "" }, { 273, kStat, "Chance to Critical DoT", "%" }, { 274, kStat, "Chance to Critical Heal", "%" },
	{ 279, kStat, "Chance to Flurry", "%" }, { 286, kStat, "Spell Damage", "" }, { 287, kStat, "Spell Duration", " ticks" },
	{ 289, kOffRef, "Cast on Fade", "" }, { 291, kStat, "Purify (detrimental effects)", "" }, { 294, kStat, "Chance to Critical Nuke", "%" },
	{ 296, kStat, "Spell Damage Taken", "%" }, { 297, kStat, "Spell Damage Taken", "" }, { 301, kStat, "Archery Damage", "%" },
	{ 302, kStat, "Critical Spell Damage", "%" }, { 310, kStat, "Ability Reuse Time", " sec" }, { 314, kFlag, "Improved Invisibility", "" },
	{ 315, kFlag, "Improved Invisibility to Undead", "" }, { 316, kFlag, "Improved Invisibility to Animals", "" },
	{ 319, kStat, "Chance to Critical HoT", "%" }, { 320, kStat, "Shield Block Chance", "%" }, { 321, kStat, "Target's Hate", "" },
	{ 322, kFlag, "Gate to Home City", "" }, { 323, kOffRef, "Defensive Proc", "" }, { 326, kStat, "Spell Memorization Slots", "" },
	{ 328, kStat, "Max Negative Hitpoints", "" }, { 329, kStat, "Absorb Damage with Mana", "%" }, { 330, kStat, "Critical Hit Damage", "%" },
	{ 332, kFlag, "Summon to Corpse", "" }, { 333, kOffRef, "Cast on Fade", "" }, { 335, kFlag, "Block Next Matching Spell", "" },
	{ 339, kChanceRef, "Trigger on Spell Cast", "" }, { 340, kChanceRef, "Chance to Cast", "" }, { 343, kStat, "Chance to Interrupt Casting", "%" },
	{ 348, kLimitRaw, "Min Mana", "" }, { 350, kStat, "Mana Burn", "" }, { 358, kStat, "Mana", "" }, { 360, kChanceRef, "Proc on Kill Shot", "" },
	{ 361, kChanceRef, "Proc on Death", "" }, { 364, kStat, "Chance to Triple Attack", "%" }, { 365, kChanceRef, "Cast on Kill Shot", "" },
	{ 369, kStat, "Corruption Counter", "" }, { 370, kStat, "Corruption Resist", "" }, { 371, kStat, "Melee Speed (inhibit)", "%" },
	{ 373, kOffRef, "Cast on Fade", "" }, { 374, kChanceRef, "Trigger Spell", "" }, { 377, kOffRef, "Cast on Fade (doom)", "" },
	{ 379, kFlag, "Directional Shadowstep", "" }, { 380, kFlag, "Knockback", "" }, { 381, kFlag, "Pull Target to Caster", "" },
	{ 382, kLimitRaw, "Negate Effect", "" }, { 383, kChanceRef, "Trigger on Spell Cast", "" }, { 384, kFlag, "Leap to Target", "" },
	{ 385, kLimitRaw, "Spell Group", "" }, { 386, kOffRef, "Cast on Curer", "" }, { 387, kOffRef, "Cast on Cure", "" },
	{ 389, kFlag, "Reset Spell Gem Timers", "" }, { 392, kStat, "Healing", "" }, { 393, kStat, "Healing", "%" }, { 394, kStat, "Healing", "" },
	{ 396, kStat, "Healing", "" }, { 397, kStat, "Pet AC", "" }, { 398, kStat, "Pet Duration", " sec" }, { 399, kStat, "Chance to Twincast", "%" },
	{ 400, kStat, "Healburn", "" }, { 406, kOffRef, "Cast on Max Hits", "" }, { 407, kOffRef, "Cast on Focus Effect", "" },
	{ 408, kLimitRaw, "Cap Hitpoints at", "%" }, { 411, kLimitRaw, "Class (bitmask)", "" }, { 412, kLimitRaw, "Race", "" },
	{ 413, kStat, "Spell Effectiveness", "%" }, { 414, kLimitRaw, "Casting Skill", "" }, { 416, kStat, "AC", "" }, { 417, kStat, "Mana Regen", "" },
	{ 418, kStat, "Skill Damage Bonus", "" }, { 419, kOffRef, "Add Proc", "" }, { 424, kFlag, "Gravitate", "" }, { 425, kFlag, "Fly", "" },
	{ 427, kOffRef, "Skill Proc", "" }, { 428, kLimitRaw, "Skill (proc)", "" }, { 429, kOffRef, "Skill Proc on Success", "" },
	{ 437, kFlag, "Teleport to Anchor", "" }, { 438, kFlag, "Translocate to Anchor", "" }, { 441, kStat, "Remove when Moved", " units" },
	{ 450, kMitigate, "Mitigate DoT Damage", "" }, { 451, kMitigate, "Mitigate Melee Damage over Threshold", "" },
	{ 452, kMitigate, "Mitigate Spell Damage over Threshold", "" }, { 453, kOffRef, "Cast on Melee Damage over Threshold", "" },
	{ 454, kOffRef, "Cast on Spell Damage over Threshold", "" }, { 455, kStat, "Hate", "%" }, { 456, kStat, "Hate Over Time", "%" },
	{ 457, kStat, "Resource Tap", "%" }, { 458, kStat, "Faction Hit", "%" }, { 459, kStat, "Skill Damage", "%" }, { 461, kStat, "Spell Damage", "%" },
	{ 462, kStat, "Spell Damage", "" }, { 463, kFlag, "Shield Target", "" }, { 467, kStat, "Damage Shield Taken", "" },
	{ 468, kStat, "Damage Shield Taken", "%" }, { 469, kChanceRef, "Chance to Cast Best in Group", "" }, { 470, kOffRef, "Cast Best in Group", "" },
	{ 471, kStat, "Chance to Repeat Melee Round", "%" }, { 473, kStat, "Chance to Backstab from Front", "%" }, { 474, kStat, "Pet Critical Damage", "%" },
	{ 475, kChanceRef, "Trigger Spell (non-item)", "" }, { 476, kFlag, "Weapon Stance", "" }, { 477, kStat, "Hate Over Time", "" },
	{ 479, kLimitRaw, "Max Value", "" }, { 480, kLimitRaw, "Min Value", "" }, { 481, kChanceRef, "Cast on Target", "" },
	{ 482, kStat, "Base Hit Damage", "%" }, { 483, kStat, "Spell Damage Taken", "%" }, { 484, kStat, "Hate Over Time", "" },
	{ 485, kLimitRaw, "Caster Class (bitmask)", "" }, { 486, kFlag, "Limit: Same caster only", "" }, { 488, kStat, "Push Taken", "%" },
	{ 490, kLimitRaw, "Min Reuse Time", " ms" }, { 491, kLimitRaw, "Max Reuse Time", " ms" }, { 492, kLimitRaw, "Min Endurance", "" },
	{ 493, kLimitRaw, "Max Endurance", "" }, { 494, kStat, "Pet Attack", "" }, { 495, kLimitRaw, "Max Duration", " ticks" },
	{ 496, kStat, "Critical Melee Damage (max)", "%" }, { 498, kStat, "Chance of Extra Primary Attack", "%" },
	{ 499, kStat, "Chance of Extra Secondary Attack", "%" }, { 500, kStat, "Spell Cast Time", "%" }, { 501, kStat, "Spell Cast Time", " ms" },
	{ 502, kMillis, "Stun and Fear", "" }, { 503, kStat, "Rear Melee Damage", "%" }, { 504, kStat, "Frontal Melee Damage", "%" },
	{ 505, kStat, "Melee Damage (max)", "%" }, { 509, kStat, "Health Transfer", "%" }, { 510, kStat, "Incoming Resist Modifier", "" },
	{ 511, kLimitRaw, "Min Focus Timer", "" }, { 512, kStat, "Proc Timer", "%" }, { 513, kStat, "Max Mana", "%" }, { 514, kStat, "Max Endurance", "%" },
	{ 515, kStat, "AC Avoidance (max)", "%" }, { 516, kStat, "AC Mitigation (max)", "%" }, { 517, kStat, "Attack Offense (max)", "" },
	{ 518, kStat, "Luck", "" }, { 519, kStat, "Luck", "%" }, { 520, kStat, "Endurance Absorb", "%" },
};

const SpaEntry* FindSpa(int spa) {
	for (const SpaEntry& e : kSpaTable)
		if (e.spa == spa) return &e;
	return nullptr;
}

// Effect ids whose value is applied every tick while the spell lasts.
bool PerTick(int spa) { return spa == 0 || spa == 15 || spa == 24 || spa == 100 || spa == 189 || spa == 192; }

const char* TargetName(int type) {
	for (const NamedId& t : kTargetTypes)
		if (t.id == type) return t.name;
	return nullptr;
}

const char* ResistName(int type) {
	return (type >= 0 && type < static_cast<int>(sizeof(kResistNames) / sizeof(kResistNames[0]))) ? kResistNames[type] : nullptr;
}

// ---- client data access ----

uint8_t* Slot(const uint8_t* spell, int i) {
	uint8_t* mgr = *g_spellManager;
	if (!mgr) return nullptr;
	return VCall<uint8_t*>(mgr, kMgrVtblGetSlot, I32(spell, kOffSlotBase) + i);
}

// The client's calculator for slot i at `level`, with the slot's own base (what the spell window
// would show for a cast at that level; no running buff, so the ramp codes return the base).
int64_t Value(const uint8_t* spell, int slotIdx, int64_t base, int level) {
	void* affects = Affects();
	if (!affects || !g_calcValue) return base;
	return g_calcValue(affects, spell, static_cast<uint8_t>(level), static_cast<uint8_t>(slotIdx), nullptr, base, 1);
}

// Whether a calc code makes the value depend on the level.
bool Scales(int calc) {
	if (calc == 0 || calc == 100 || calc == 123) return false;
	if (calc >= 1000 && calc < 2000) return false;
	if (calc >= 3000 && calc < 5000) return false;
	return true;
}

// Lowest level any class gets the spell (1 when no class does).
int MinLevel(const uint8_t* spell) {
	int best = 0;
	for (int c = 1; c <= kClassCount; ++c) {
		int lvl = spell[kOffClassLevels + static_cast<size_t>(c)];
		if (lvl >= 1 && lvl <= kMaxLevel && (best == 0 || lvl < best)) best = lvl;
	}
	return best ? best : 1;
}

// Value of a slot at a cast level, or its range over the spell's levels.
struct Amount {
	int64_t first = 0, last = 0;
	int levelFirst = 0, levelLast = 0;
	bool range = false;
};

Amount Compute(const uint8_t* spell, int slotIdx, const uint8_t* slot, int castLevel) {
	Amount a;
	int64_t base = I64(slot, kSlotBase1);
	int calc = I32(slot, kSlotCalc);
	if (castLevel > 0 || !Scales(calc)) {
		a.first = a.last = Value(spell, slotIdx, base, castLevel > 0 ? castLevel : MinLevel(spell));
		return a;
	}
	int lo = MinLevel(spell);
	a.first = Value(spell, slotIdx, base, lo);
	a.last = Value(spell, slotIdx, base, kMaxLevel);
	a.levelFirst = lo;
	a.levelLast = kMaxLevel;
	if (a.first == a.last) return a;
	for (int l = lo + 1; l <= kMaxLevel; ++l) {
		if (Value(spell, slotIdx, base, l) == a.last) {
			a.levelLast = l;
			break;
		}
	}
	a.range = true;
	return a;
}

// "by 10 (L1) to 60 (L50)" / "by 25" with the unit after each number; `shift` is subtracted first
// (100 for the attack-speed ids) and the sign dropped.
void AddAmount(TextBuf& out, const Amount& a, int64_t shift, const char* unit) {
	long long first = Abs(static_cast<long long>(a.first - shift));
	long long last = Abs(static_cast<long long>(a.last - shift));
	if (a.range) out.Add("by %lld%s (L%d) to %lld%s (L%d)", first, unit, a.levelFirst, last, unit, a.levelLast);
	else out.Add("by %lld%s", first, unit);
}

const char* Verb(int64_t v) { return v < 0 ? "Decrease" : "Increase"; }

void AddSpellName(TextBuf& out, int64_t id) {
	uint8_t* ref = (id > 0 && id < 0x7fffffff) ? SpellInfoGetSpell(static_cast<int>(id)) : nullptr;
	if (ref) out.Add("%.63s", SpellInfoName(ref));
	else out.Add("spell %lld", static_cast<long long>(id));
}

// One effect slot in words. Returns false when the slot prints nothing (empty / filler).
bool DescribeSlot(TextBuf& out, const uint8_t* spell, int slotIdx, const uint8_t* slot, int castLevel, bool hasDuration) {
	int spa = I32(slot, kSlotSpa);
	int64_t base = I64(slot, kSlotBase1);
	int64_t base2 = I64(slot, kSlotBase2);
	int64_t max = I64(slot, kSlotMax);
	int calc = I32(slot, kSlotCalc);
	if (spa < 0 || spa == kSpaEmpty) return false;
	if (spa == 10 && base == 0) return false;   // the spell file's blank filler
	const SpaEntry* e = FindSpa(spa);
	if (!e) {
		Amount a = Compute(spell, slotIdx, slot, castLevel);
		out.Add("Effect %d: ", spa);
		if (a.range) out.Add("%lld (L%d) to %lld (L%d)", static_cast<long long>(a.first), a.levelFirst, static_cast<long long>(a.last), a.levelLast);
		else out.Add("%lld", static_cast<long long>(a.first));
		if (base2) out.Add(" (base2 %lld)", static_cast<long long>(base2));
		return true;
	}
	switch (e->kind) {
	case kStat: {
		if (calc == 123) {   // random between base and max
			out.Add("%s %s by %lld%s to %lld%s (random)", Verb(base), e->name, Abs(base), e->unit, Abs(max), e->unit);
		}
		else {
			Amount a = Compute(spell, slotIdx, slot, castLevel);
			out.Add("%s %s ", Verb(a.first), e->name);
			AddAmount(out, a, 0, e->unit);
			if (calc > 1000 && calc < 2000) out.Add(" (ramps %d per tick)", calc - 1000);
		}
		if (hasDuration && PerTick(spa)) out.Add(" per tick");
		return true;
	}
	case kFlag:
		if (!e->name[0]) return false;
		out.Add("%s", e->name);
		return true;
	case kHaste: {
		Amount a = Compute(spell, slotIdx, slot, castLevel);
		out.Add("%s %s ", Verb(a.first - 100), e->name);
		AddAmount(out, a, 100, e->unit);
		return true;
	}
	case kMillis: {
		Amount a = Compute(spell, slotIdx, slot, castLevel);
		out.Add("%s for %.1f sec", e->name, static_cast<double>(Abs(a.first)) / 1000.0);
		if (a.range) out.Add(" (L%d) to %.1f sec (L%d)", a.levelFirst, static_cast<double>(Abs(a.last)) / 1000.0, a.levelLast);
		return true;
	}
	case kLevelCap:
		if (max > 0) out.Add("%s (up to level %lld)", e->name, static_cast<long long>(max));
		else out.Add("%s", e->name);
		return true;
	case kOffRef:
		out.Add("%s: ", e->name);
		AddSpellName(out, base);
		return true;
	case kChanceRef:
		out.Add("%s (%lld%%): ", e->name, Abs(base));
		AddSpellName(out, base2);
		return true;
	case kExtra: {
		const char* extra = reinterpret_cast<const char*>(spell + kOffExtra);
		if (extra[0]) out.Add("%s: %.31s", e->name, extra);
		else out.Add("%s", e->name);
		return true;
	}
	case kShield: {
		Amount a = Compute(spell, slotIdx, slot, castLevel);
		out.Add("%s %s ", a.first <= 0 ? "Increase" : "Decrease", e->name);
		AddAmount(out, a, 0, "");
		return true;
	}
	case kRaw: {
		Amount a = Compute(spell, slotIdx, slot, castLevel);
		out.Add("%s: %lld", e->name, static_cast<long long>(a.first));
		if (a.range) out.Add(" (L%d) to %lld (L%d)", a.levelFirst, static_cast<long long>(a.last), a.levelLast);
		return true;
	}
	case kAbsorb: {
		Amount a = Compute(spell, slotIdx, slot, castLevel);
		out.Add("%s: absorb ", e->name);
		if (a.range) out.Add("%lld (L%d) to %lld (L%d)", Abs(a.first), a.levelFirst, Abs(a.last), a.levelLast);
		else out.Add("%lld", Abs(a.first));
		out.Add(" damage");
		return true;
	}
	case kMitigate:
		out.Add("%s: %lld%%", e->name, Abs(base));
		if (max > 0) out.Add(" (up to %lld total)", static_cast<long long>(max));
		return true;
	case kPctHeal:
		out.Add("%s Hitpoints by %lld%%", Verb(base), Abs(base));
		if (max > 0) out.Add(" (up to %lld)", static_cast<long long>(max));
		return true;
	case kLimitSpellType:
		out.Add("Limit: %s spells only", base ? "Beneficial" : "Detrimental");
		return true;
	case kLimitResist: {
		const char* name = ResistName(static_cast<int>(Abs(base)));
		if (name) out.Add("Limit: %s %s", base < 0 ? "exclude resist" : "resist", name);
		else out.Add("Limit: resist %lld", static_cast<long long>(base));
		return true;
	}
	case kLimitSpell:
		out.Add("Limit: %s ", base < 0 ? "exclude spell" : "spell");
		AddSpellName(out, Abs(base));
		return true;
	case kLimitEffect: {
		const SpaEntry* fx = FindSpa(static_cast<int>(Abs(base)));
		if (fx && fx->name[0]) out.Add("Limit: %s %s", base < 0 ? "exclude effect" : "effect", fx->name);
		else out.Add("Limit: %s %lld", base < 0 ? "exclude effect" : "effect", Abs(base));
		return true;
	}
	case kLimitTarget: {
		const char* name = TargetName(static_cast<int>(Abs(base)));
		if (name) out.Add("Limit: %s %s", base < 0 ? "exclude target" : "target", name);
		else out.Add("Limit: target %lld", static_cast<long long>(base));
		return true;
	}
	case kLimitRaw:
		out.Add("Limit: %s %lld%s", e->name, static_cast<long long>(base), e->unit);
		if (max) out.Add(" (%lld)", static_cast<long long>(max));
		return true;
	}
	return false;
}

// ---- lines ----

void Label(TextBuf& out, const SpellTextStyle& s, const char* label) {
	out.Add("%s%s", s.lineBreak, s.indent);
	if (s.labelColour) out.Add("<C #%06x>%s</C> ", s.labelColour & 0xffffff, label);
	else out.Add("%s ", label);
}

unsigned Ticks(const uint8_t* spell, int level) {
	void* affects = Affects();
	if (!affects || !g_duration) return 0;
	return g_duration(affects, spell, static_cast<uint8_t>(level), 0);
}

void AddTicks(TextBuf& out, unsigned ticks) {
	if (ticks == kDurationPermanent) out.Add("Permanent");
	else SpellInfoSeconds(out, static_cast<int>(ticks) * kTickSeconds);
}

void AddDuration(TextBuf& out, const uint8_t* spell, int castLevel, const SpellTextStyle& s) {
	if (I32(spell, kOffDurationFormula) == 0) return;
	int lo = castLevel > 0 ? castLevel : MinLevel(spell);
	unsigned first = Ticks(spell, lo);
	unsigned last = castLevel > 0 ? first : Ticks(spell, kMaxLevel);
	if (!first && !last) return;
	Label(out, s, "Duration:");
	AddTicks(out, first);
	if (first != last) {
		out.Add(" (L%d) to ", lo);
		AddTicks(out, last);
		out.Add(" (L%d)", kMaxLevel);
	}
}

void AddEffects(TextBuf& out, const uint8_t* spell, int castLevel, const SpellTextStyle& s) {
	int count = I32(spell, kOffSlotCount);
	if (count > kMaxSlots) count = kMaxSlots;
	bool hasDuration = I32(spell, kOffDurationFormula) != 0;
	int shown = 0;
	for (int i = 0; i < count && shown < s.maxEffects; ++i) {
		uint8_t* slot = Slot(spell, i);
		if (!slot) continue;
		int spa = I32(slot, kSlotSpa);
		if (s.skipCounters && (spa == 35 || spa == 36 || spa == 116 || spa == 369)) continue;
		size_t mark = out.len;
		out.Add("%s%s", s.lineBreak, s.indent);
		if (DescribeSlot(out, spell, i, slot, castLevel, hasDuration)) ++shown;
		else if (!out.full) { out.len = mark; out.buf[mark] = '\0'; }
	}
}

void AddTimes(TextBuf& out, const uint8_t* spell, const SpellTextStyle& s) {
	int cast = I32(spell, kOffCastMs), recast = I32(spell, kOffRecastMs), recovery = I32(spell, kOffRecoveryMs);
	Label(out, s, "Cast:");
	if (cast > 0) out.Add("%.1f sec", cast / 1000.0);
	else out.Add("Instant");
	if (recast > 0) out.Add(", recast %.1f sec", recast / 1000.0);
	if (recovery > 0) out.Add(", recovery %.1f sec", recovery / 1000.0);
}

void AddCosts(TextBuf& out, const uint8_t* spell, const SpellTextStyle& s) {
	int mana = I32(spell, kOffMana), endurance = I32(spell, kOffEndurance);
	float manaPct = F32(spell, kOffManaPct), endPct = F32(spell, kOffEndurancePct);
	if (mana > 0 || manaPct > 0) {
		Label(out, s, "Mana:");
		out.Add("%d", mana);
		if (manaPct > 0) out.Add(" + %.1f%% of max", manaPct);
	}
	if (endurance > 0 || endPct > 0) {
		Label(out, s, "Endurance:");
		out.Add("%d", endurance);
		if (endPct > 0) out.Add(" + %.1f%% of max", endPct);
	}
}

void AddResist(TextBuf& out, const uint8_t* spell, const SpellTextStyle& s) {
	if (spell[kOffBeneficial] != 0) return;
	const char* name = ResistName(spell[kOffResistType]);
	Label(out, s, "Resist:");
	if (name) out.Add("%s", name);
	else out.Add("type %d", spell[kOffResistType]);
	int adjust = I32(spell, kOffResistAdjust);
	if (adjust) out.Add(" (%+d)", adjust);
}

void AddTarget(TextBuf& out, const uint8_t* spell, const SpellTextStyle& s) {
	int type = spell[kOffTargetType];
	Label(out, s, "Target:");
	if (type == 5 && I32(spell, kOffPcOnly) == 1) out.Add("Single (PC or mercenary only)");
	else if (const char* name = TargetName(type)) out.Add("%s", name);
	else out.Add("type %d", type);
}

void AddRange(TextBuf& out, const uint8_t* spell, const SpellTextStyle& s) {
	float range = F32(spell, kOffRange), ae = F32(spell, kOffAERange);
	if (range <= 0 && ae <= 0) return;
	Label(out, s, "Range:");
	if (range > 0) out.Add("%.0f", range);
	else out.Add("self");
	if (ae > 0) out.Add(", AE range %.0f", ae);
}

void AddSkill(TextBuf& out, const uint8_t* spell, const SpellTextStyle& s) {
	int skill = spell[kOffSkill];
	int id = (*g_skillTable && g_skillNameId) ? g_skillNameId(*g_skillTable, static_cast<unsigned>(skill)) : 0;
	const char* name = (id && *g_eqstrTable && g_eqstr) ? g_eqstr(*g_eqstrTable, id, 0) : nullptr;
	Label(out, s, "Skill:");
	if (name && name[0]) out.Add("%.47s", name);
	else out.Add("%d", skill);
}

void AddClasses(TextBuf& out, const uint8_t* spell, const SpellTextStyle& s) {
	bool any = false;
	for (int c = 1; c <= kClassCount; ++c) {
		int lvl = spell[kOffClassLevels + static_cast<size_t>(c)];
		if (lvl < 1 || lvl > kMaxLevel) continue;
		if (!any) Label(out, s, "Classes:");
		out.Add("%s%s (%d)", any ? ", " : "", kClassNames[c], lvl);
		any = true;
	}
}

const char* DbStr(int id, int type) {
	if (id <= 0 || !*g_dbstrTable || !g_dbstr) return nullptr;
	bool found = false;
	const char* text = g_dbstr(*g_dbstrTable, static_cast<unsigned>(id), static_cast<unsigned>(type), &found);
	return (found && text && text[0]) ? text : nullptr;
}

void AddCategory(TextBuf& out, const uint8_t* spell, const SpellTextStyle& s) {
	const char* cat = DbStr(I32(spell, kOffCategory), kDbstrSpellCategory);
	const char* sub = DbStr(I32(spell, kOffSubcategory), kDbstrSpellCategory);
	if (!cat && !sub) return;
	Label(out, s, "Category:");
	if (cat) out.Add("%.47s", cat);
	if (cat && sub) out.Add(" / ");
	if (sub) out.Add("%.47s", sub);
}

void AddMessages(TextBuf& out, const uint8_t* spell, const SpellTextStyle& s) {
	if (!*g_spellStringTable || !g_spellString) return;
	unsigned id = static_cast<unsigned>(I32(spell, kOffId));
	static const char* const labels[3] = { "On you:", "On other:", "Wears off:" };
	for (int i = 0; i < 3; ++i) {
		const char* text = g_spellString(*g_spellStringTable, id, 2 + i);
		if (!text || strlen(text) <= 1) continue;
		Label(out, s, labels[i]);
		out.Add("%s%.95s", i == 1 ? "Soandso" : "", text);
	}
}

// Scans a function body for a RIP-relative `mov r64,[global]` (48 8B /r with mod=00 rm=101).
bool ReadsGlobal(const uint8_t* fn, size_t size, const void* global) {
	for (size_t i = 0; i + 7 <= size; ++i) {
		if (fn[i] != 0x48 || fn[i + 1] != 0x8B || (fn[i + 2] & 0xC7) != 0x05) continue;
		int32_t rel;
		memcpy(&rel, fn + i + 3, 4);
		if (fn + i + 7 + rel == static_cast<const uint8_t*>(global)) return true;
	}
	return false;
}

const size_t kCalcValueSize = 1360;   // FUN_1400d48e0
const size_t kDurationSize = 575;     // FUN_1400e27a0

} // namespace

uint8_t* SpellInfoGetSpell(int id) {
	uint8_t* mgr = *g_spellManager;
	if (!mgr || id <= 0 || id > I32(mgr, kMgrMaxSpellId)) return nullptr;
	uint8_t* spell = VCall<uint8_t*>(mgr, kMgrVtblGetSpell, static_cast<int64_t>(id));
	if (!spell || I32(spell, kOffId) != id) return nullptr;   // the manager hands back its empty spell for unknown ids
	return spell;
}

const char* SpellInfoName(const uint8_t* spell) {
	return reinterpret_cast<const char*>(spell + kOffName);
}

void SpellInfoSeconds(TextBuf& out, int seconds) {
	int h = seconds / 3600, m = (seconds / 60) % 60, sec = seconds % 60;
	if (h) out.Add("%dh ", h);
	if (h || m) out.Add("%dm ", m);
	out.Add("%ds", sec);
}

bool SpellInfoCures(TextBuf& out, const uint8_t* spell) {
	int count = I32(spell, kOffSlotCount);
	if (count > kMaxSlots) count = kMaxSlots;
	bool any = false;
	for (int i = 0; i < count; ++i) {
		uint8_t* slot = Slot(spell, i);
		if (!slot) continue;
		int spa = I32(slot, kSlotSpa);
		const char* name = spa == 35 ? "disease" : spa == 36 ? "poison" : spa == 116 ? "curse" : spa == 369 ? "corruption" : nullptr;
		int64_t base = I64(slot, kSlotBase1);
		if (!name || base <= 0) continue;
		out.Add("%s%lld %s", any ? ", " : "needs cure: ", static_cast<long long>(base), name);
		any = true;
	}
	return any;
}

void SpellInfoWrite(TextBuf& out, const uint8_t* spell, int castLevel, unsigned parts, const SpellTextStyle& style) {
	if (!spell) return;
	if (castLevel > kMaxLevel) castLevel = kMaxLevel;
	if (parts & kSpellEffects) AddEffects(out, spell, castLevel, style);
	if (parts & kSpellDuration) AddDuration(out, spell, castLevel, style);
	if (parts & kSpellTimes) AddTimes(out, spell, style);
	if (parts & kSpellCosts) AddCosts(out, spell, style);
	if (parts & kSpellResist) AddResist(out, spell, style);
	if (parts & kSpellTarget) AddTarget(out, spell, style);
	if (parts & kSpellRange) AddRange(out, spell, style);
	if (parts & kSpellSkill) AddSkill(out, spell, style);
	if (parts & kSpellClasses) AddClasses(out, spell, style);
	if (parts & kSpellCategory) AddCategory(out, spell, style);
	if (parts & kSpellMessages) AddMessages(out, spell, style);
	if (parts & kSpellId) {
		Label(out, style, "ID:");
		out.Add("%d", I32(spell, kOffId));
	}
}

bool SpellInfoInGame() {
	uint8_t* eq = *g_everQuest;
	return eq && *reinterpret_cast<int*>(eq + kGameState) == kGameStateInGame && *g_spellManager && *g_pLocalPC;
}

const char* SpellInfoPrepare() {
	if (g_spellManager != g_spellManagerRead)
		return "the two spell manager references disagree";
	if (!ReadsGlobal(reinterpret_cast<const uint8_t*>(g_calcValue), kCalcValueSize, g_spellManager))
		return "the effect value calculator does not read the spell manager";
	if (!ReadsGlobal(reinterpret_cast<const uint8_t*>(g_duration), kDurationSize, g_spellManager))
		return "the duration function does not read the spell manager";
	return nullptr;
}
