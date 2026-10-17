// Nameplates: EverQuest Legends-style bars under spawn names (analysis\legends\nameplates-parity\dll-spec.md,
// behaviour-spec.md): an HP row, then a cast-or-mana row 1 px below, a 500 ms red flash in the cast row when a
// cast is interrupted, and a pulsing red frame around the bars of NPCs that have aggro on me (or my group).
//
// Drawing: from the CXWndManager::DrawWindows hook shared with the Hud and FCT, after the client's windows, with
// the renderer's DrawColoredQuad (vtbl +0x140) and DrawLine2D (+0x110), checked once against the EQGraphics.dll
// this was mapped on (renderer vtable +0x2c0830, slot start bytes). Bars hang under ToB's own engine name: the
// name sprite (actor +0x2f0, actor = spawn +0x1170) keeps its last screen rect at +0x1c..+0x28 (font +0x18 = 10).
// The rect is used when it is plausible and agrees with our own projection of the spawn's head; otherwise a
// name box is made around the projected head point (camera WorldToScreen, shared with FCT).
// Layout and colours as Legends: HP fill 0xFFFF1919, mana 0xFF1919FF, cast 0xFF820082 (the bar empties:
// remaining / total), backing 0xFF191919 shifted 1 px right and 1 px out; bar height = name height / 9 (min 2,
// at most 6 x Size), width = 4.5 name heights (24 px .. a quarter of the viewport); Size scales both (ToB's name
// itself cannot be scaled). Visibility (Legends "TargetHealth"): every spawn with 0 < HP < 100 % plus the target;
// never corpses or HP < 2; spawns whose name the client hides (MayShowName FUN_1402fa230, invisible to me
// FUN_140315770, untargetable +0x18e) get nothing; XY distance from the camera above DrawDistance hides all but
// the target. Self bars follow the Self options (HP only when hurt or self-targeted).
//
// Data: HP cur/max at spawn+0x5e8/+0x5f8 (percent / 100 for others), mana +0x248/+0x1d4 (unknown = 100 %). Casts:
// detour on StartCasting FUN_140293100 (OP_BeginCast: spell int @0, caster u16 @4, cast time u32 ms @6) keeps
// {caster, spell, start, end}. The packet dispatcher FUN_1401e4010 (detoured as a pure passthrough) gives
// OP_InterruptCast 0x5313 (u32 spawn id: 500 ms flash, cast dropped), OP_TargetHoTT 0x117a (u32 my target's
// target) and OP_Death 0x1e90 (u32 spawn id: state dropped). Threat (ThreatDisplay 1 = the NPC targets me, 2 = it
// targets me, my group or my pet): exact for my target from OP_TargetHoTT, otherwise "the NPC swung at me (or my
// group / pet) within ThreatHoldMs", from the OP_Damage detour shared with FCT. Drawn only while FontShadow or
// FontOutline is on, as Legends shows the indicator through those layers. Group members: pLocalPC +0x2e30 (as map.cpp).
//
// Occlusion (Occlude=1, analysis\legends\nameplates-occlusion\dll-spec.md): ToB has no line-of-sight test for
// names; the engine queues a name only when its actor rendered, and the occlusion culling skips actors behind
// geometry. So a bar is drawn only when the actor was drawn last frame (EQGraphics actor vtbl +0x408
// WasVisibleThisFrame, the call eqgame makes in RealRender_World; its target and bytes are checked once per actor
// vtable), its name sprite has text (+0x10) and a bone (+0xb8), and it is within the sprite's own XY cutoff
// (+0xa8 = max distance squared, scaled by +0x90). This applies to the target too. If the check of the engine
// function fails, the gate (and with it every bar) is turned off and dinput8.log says why.
//
// NameplateOptionsPage: Legends' "Nameplates" tab (OptionsNamePlatesPage in the patched EQUI_OptionsWindow.xml),
// on the two COptionsWnd detours the Combat Text tab uses (fct.cpp). Settings in dinput8.ini [NameplateOptions]
// with Legends' key names; ConColor switches the ConNameTags feature live ([Features] ConNameTags); Name Filters
// sets the client's own /shownames level (DAT_140e6e3a8) and rebuilds the names (CDisplay::SetNPCNames
// FUN_1401a3bf0). SelfName is stored only (phase 2).
#include "nameplates.h"
#include "fct.h"
#include "hud.h"
#include "nametags.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace {

// PlayerClient (spawn) fields.
const size_t kSpawnY = 0x74;
const size_t kSpawnX = 0x78;
const size_t kSpawnZ = 0x7c;
const size_t kSpawnNext = 0x10;          // next spawn in the manager's list
const size_t kSpawnType = 0x135;         // u8: 0 PC, 1 NPC, 2+ corpse
const size_t kSpawnSize = 0x154;         // float model height
const size_t kSpawnId = 0x168;           // u32
const size_t kSpawnUntargetable = 0x18e; // u8
const size_t kSpawnManaCur = 0x248;      // int (percent for others)
const size_t kSpawnManaMax = 0x1d4;      // int
const size_t kSpawnMasterId = 0x2d8;     // u32 pet owner
const size_t kSpawnHpCur = 0x5e8;        // int64 (percent for others)
const size_t kSpawnHpMax = 0x5f8;        // int64 (100 for others)
const size_t kSpawnActor = 0x1170;       // CActorInterface*
const size_t kActorNameSprite = 0x2f0;   // EQGraphics string sprite, may be null
const size_t kSpriteText = 0x10;         // text pointer, null when the name is empty
const size_t kSpriteFont = 0x18;         // int, 10 when text is set
const size_t kSpriteScale = 0x90;        // float, actor scale written on each draw
const size_t kSpritePos = 0x98;          // float[3] world position (spawn memory order)
const size_t kSpriteMaxDist2 = 0xa8;     // float, 150^2 by default
const size_t kSpriteBone = 0xb8;         // bone pointer, null = never drawn
const size_t kActorVtblWasVisible = 0x408;
const size_t kWasVisibleRva = 0x534c0;
const size_t kSpriteRect = 0x1c;         // int l, t, r, b (viewport px, last draw)
const int kSpriteFontName = 10;

// pLocalPC group (map.cpp).
const size_t kPcGroup = 0x2e30;
const size_t kGroupMemberSpawn = 0x58;
const int kGroupMembers = 6;

// Packets.
const size_t kDmgTarget = 0x00, kDmgSource = 0x02, kDmgAmount = 0x08;
const int64_t kDamageInvulnerable = -5;
const size_t kCastSpell = 0x0, kCastCaster = 0x4, kCastTime = 0x6;   // OP_BeginCast
const uint32_t kOpInterruptCast = 0x5313, kOpTargetHoTT = 0x117a, kOpDeath = 0x1e90;

// Spell manager: vtbl+0x40 GetSpell(id), +0x64 highest id; SPELL +0x8c id, +0x18f name.
const size_t kSpellMgrMaxId = 0x64;
const size_t kVtblGetSpell = 0x40;
const size_t kSpellId = 0x8c;
const size_t kSpellName = 0x18f;

// EQGraphics renderer (dll-spec-w2s.md 1).
const size_t kRendererVtblRva = 0x2c0830;
const size_t kVtblDrawLine2D = 0x110;
const size_t kVtblDrawColoredQuad = 0x140;

// Legends colours.
const uint32_t kColourHp = 0xFFFF1919;
const uint32_t kColourMana = 0xFF1919FF;
const uint32_t kColourCast = 0xFF820082;
const uint32_t kColourBacking = 0xFF191919;
const uint32_t kFlashMs = 500;
const uint32_t kCastLingerMs = 500;

using StartCastingFn = uint64_t (*)(void* eq, int* pkt);
using DispatchFn = uint64_t (*)(void* self, void* a2, uint32_t op, uint8_t* data, uint32_t size);
using MayShowNameFn = char (*)(uint8_t* spawn);
using IsInvisibleToFn = char (*)(uint8_t* me, uint8_t* spawn, char lenient);
using GetSpellFn = uint8_t* (*)(void* mgr, int id);
using DrawLineFn = void (*)(void* renderer, const float* p1, const float* p2, uint32_t argb);
using DrawQuadFn = void (*)(void* renderer, const float* quad, uint32_t argb);

StartCastingFn g_origStartCasting = nullptr;   // trampoline into FUN_140293100
DispatchFn g_origDispatch = nullptr;           // trampoline into FUN_1401e4010
uint8_t** g_pTarget = nullptr;                 // &DAT_140e662d0
void** g_spellMgr = nullptr;                   // &DAT_140ee6480
MayShowNameFn g_mayShowName = nullptr;         // FUN_1402fa230
IsInvisibleToFn g_isInvisibleTo = nullptr;     // FUN_140315770
bool g_on = false;
char g_iniPath[MAX_PATH] = {};

// ---- configuration ([NameplateOptions]) ----

struct Config {
	bool targetHealth = true;
	bool cast = true;
	bool mana = true;
	bool selfHealth = false;   // Legends defaults both self bars on
	bool selfMana = false;
	bool selfCast = true;
	bool fontShadow = true;
	bool fontOutline = true;
	bool selfName = true;
	float drawDistance = 200.0f;   // 50..400
	float size = 1.0f;             // 0.2..2.0
	int threatDisplay = 0;         // 0 off, 1 aggro on me, 2 aggro on me / my group / my pet
	// extras
	int threatHoldMs = 6000;
	bool castSpellName = false;
	bool hpText = false;
	bool hpByCon = false;
	bool occlude = true;           // no bar when the client did not draw the spawn (behind walls)
	bool selfIgnoresOcclusion = false;
	float depthBias = 0.0f;        // world units (x actor scale) the depth-tested bars sit toward the camera
	uint32_t threatColour = 0;     // 0 = Legends pulse
};
Config g_cfg;
const char* const kSection = "NameplateOptions";

float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void LoadConfig() {
	GetModuleFileNameA(nullptr, g_iniPath, MAX_PATH);
	char* slash = strrchr(g_iniPath, '\\');
	if (slash) *slash = '\0';
	strcat_s(g_iniPath, "\\dinput8.ini");
	const char* ini = g_iniPath;
	auto Int = [&](const char* key, int def) { return static_cast<int>(GetPrivateProfileIntA(kSection, key, def, ini)); };
	auto Float = [&](const char* key, float def) {
		char buf[32] = {};
		GetPrivateProfileStringA(kSection, key, "", buf, sizeof(buf), ini);
		char* end = nullptr;
		float v = strtof(buf, &end);
		return (buf[0] && end != buf && std::isfinite(v)) ? v : def;
	};
	Config c;
	c.targetHealth = Int("TargetHealth", c.targetHealth) != 0;
	c.cast = Int("Cast", c.cast) != 0;
	c.mana = Int("Mana", c.mana) != 0;
	c.selfHealth = Int("SelfHealth", c.selfHealth) != 0;
	c.selfMana = Int("SelfMana", c.selfMana) != 0;
	c.selfCast = Int("SelfCast", c.selfCast) != 0;
	c.fontShadow = Int("FontShadow", c.fontShadow) != 0;
	c.fontOutline = Int("FontOutline", c.fontOutline) != 0;
	c.selfName = Int("SelfName", c.selfName) != 0;
	c.drawDistance = ClampF(Float("DrawDistance", c.drawDistance), 50.0f, 400.0f);
	c.size = ClampF(Float("Size", c.size), 0.2f, 2.0f);
	c.threatDisplay = Clamp(Int("ThreatDisplay", c.threatDisplay), 0, 2);
	c.threatHoldMs = Clamp(Int("ThreatHoldMs", c.threatHoldMs), 1000, 30000);
	c.castSpellName = Int("CastSpellName", c.castSpellName) != 0;
	c.hpText = Int("HpText", c.hpText) != 0;
	c.hpByCon = Int("HpByCon", c.hpByCon) != 0;
	c.occlude = Int("Occlude", c.occlude) != 0;
	c.selfIgnoresOcclusion = Int("SelfIgnoresOcclusion", c.selfIgnoresOcclusion) != 0;
	c.depthBias = ClampF(Float("DepthBias", c.depthBias), 0.0f, 10.0f);
	char buf[32] = {};
	GetPrivateProfileStringA(kSection, "ThreatFrameColour", "", buf, sizeof(buf), ini);
	char* end = nullptr;
	unsigned long v = strtoul(buf, &end, 16);
	c.threatColour = end != buf ? static_cast<uint32_t>(v) | 0xFF000000u : 0u;
	g_cfg = c;
}

void SaveKey(const char* key, const char* text) {
	if (g_iniPath[0]) WritePrivateProfileStringA(kSection, key, text, g_iniPath);
}

// ---- state ----

struct CastEntry {
	uint32_t caster;
	int spell;
	uint64_t start, end;
};
struct FlashEntry {
	uint32_t spawn;
	uint64_t until;
};
struct ThreatEntry {
	uint32_t npc;
	uint64_t lastHitMe, lastHitGroup;
};
const int kMaxCasts = 64, kMaxFlashes = 32, kMaxThreats = 128;
CastEntry g_casts[kMaxCasts];
FlashEntry g_flashes[kMaxFlashes];
ThreatEntry g_threats[kMaxThreats];
uint32_t g_hottTarget = 0;
bool g_hottValid = false;
uint8_t* g_lastTarget = nullptr;

uint64_t Now() { return GetTickCount64(); }

void ClearState() {
	memset(g_casts, 0, sizeof(g_casts));
	memset(g_flashes, 0, sizeof(g_flashes));
	memset(g_threats, 0, sizeof(g_threats));
	g_hottTarget = 0;
	g_hottValid = false;
	g_lastTarget = nullptr;
}

void ForgetSpawn(uint32_t id) {
	for (CastEntry& c : g_casts) if (c.caster == id) c = {};
	for (FlashEntry& f : g_flashes) if (f.spawn == id) f = {};
	for (ThreatEntry& t : g_threats) if (t.npc == id) t = {};
}

CastEntry* FindCast(uint32_t id) {
	for (CastEntry& c : g_casts) if (c.caster == id) return &c;
	return nullptr;
}

// The slot for `id`, or the oldest free/expired one.
CastEntry& CastSlot(uint32_t id) {
	if (CastEntry* c = FindCast(id)) return *c;
	CastEntry* best = &g_casts[0];
	for (CastEntry& c : g_casts) {
		if (!c.caster) return c;
		if (c.end < best->end) best = &c;
	}
	return *best;
}

ThreatEntry& ThreatSlot(uint32_t id) {
	ThreatEntry* best = &g_threats[0];
	for (ThreatEntry& t : g_threats) {
		if (t.npc == id) return t;
	}
	for (ThreatEntry& t : g_threats) {
		if (!t.npc) return t;
		uint64_t last = t.lastHitMe > t.lastHitGroup ? t.lastHitMe : t.lastHitGroup;
		uint64_t bestLast = best->lastHitMe > best->lastHitGroup ? best->lastHitMe : best->lastHitGroup;
		if (last < bestLast) best = &t;
	}
	*best = {};
	return *best;
}

const ThreatEntry* FindThreat(uint32_t id) {
	for (const ThreatEntry& t : g_threats) if (t.npc == id) return &t;
	return nullptr;
}

bool Flashing(uint32_t id, uint64_t now) {
	for (FlashEntry& f : g_flashes) {
		if (!f.spawn) continue;
		if (now >= f.until) {
			f = {};
			continue;
		}
		if (f.spawn == id) return true;
	}
	return false;
}

uint32_t MyId() {
	uint8_t* me = FctLocalPlayer();
	return me ? *reinterpret_cast<uint32_t*>(me + kSpawnId) : 0;
}

bool IsGroupMember(const uint8_t* spawn) {
	uint8_t* pc = static_cast<uint8_t*>(HudLocalPC());
	if (!pc || !spawn) return false;
	uint8_t* group = *reinterpret_cast<uint8_t**>(pc + kPcGroup);
	if (!group) return false;
	for (int i = 0; i < kGroupMembers; ++i) {
		uint8_t* member = *reinterpret_cast<uint8_t**>(group + 8 + i * 8);
		if (member && *reinterpret_cast<uint8_t**>(member + kGroupMemberSpawn) == spawn) return true;
	}
	return false;
}

bool IsMyPet(const uint8_t* spawn, uint32_t myId) {
	return spawn && myId && *reinterpret_cast<const uint32_t*>(spawn + kSpawnMasterId) == myId;
}

// ---- hooks ----

uint64_t StartCastingDetour(void* eq, int* pkt) {
	uint64_t result = g_origStartCasting(eq, pkt);
	if (!g_on || !pkt) return result;
	const uint8_t* p = reinterpret_cast<const uint8_t*>(pkt);
	const uint32_t caster = *reinterpret_cast<const uint16_t*>(p + kCastCaster);
	const uint32_t castTime = *reinterpret_cast<const uint32_t*>(p + kCastTime);
	if (!caster || !castTime) return result;
	CastEntry& c = CastSlot(caster);
	const uint64_t now = Now();
	c = { caster, *reinterpret_cast<const int*>(p + kCastSpell), now, now + castTime };
	for (FlashEntry& f : g_flashes) if (f.spawn == caster) f = {};
	return result;
}

uint64_t DispatchDetour(void* self, void* a2, uint32_t op, uint8_t* data, uint32_t size) {
	if (g_on && data) {
		if (op == kOpInterruptCast && size >= 8) {
			const uint32_t id = *reinterpret_cast<const uint32_t*>(data);
			if (CastEntry* c = FindCast(id)) *c = {};
			FlashEntry* slot = &g_flashes[0];
			for (FlashEntry& f : g_flashes) {
				if (f.spawn == id || !f.spawn) {
					slot = &f;
					break;
				}
				if (f.until < slot->until) slot = &f;
			}
			*slot = { id, Now() + kFlashMs };
		}
		else if (op == kOpTargetHoTT && size >= 4) {
			g_hottTarget = *reinterpret_cast<const uint32_t*>(data);
			g_hottValid = true;
		}
		else if (op == kOpDeath && size >= 8) {
			ForgetSpawn(*reinterpret_cast<const uint32_t*>(data));
		}
	}
	return g_origDispatch(self, a2, op, data, size);
}

// ---- drawing ----

void* g_renderer = nullptr;      // the renderer the slot checks passed for
bool g_rendererOk = false;
DrawLineFn g_drawLine = nullptr;
DrawQuadFn g_drawQuad = nullptr;

bool StartsWith(const void* fn, const char* pattern) {
	const uint8_t* p = static_cast<const uint8_t*>(fn);
	for (size_t i = 0; pattern[0]; ++i) {
		while (*pattern == ' ') ++pattern;
		if (!pattern[0]) break;
		if (pattern[0] != '?') {
			char hex[3] = { pattern[0], pattern[1], 0 };
			if (p[i] != static_cast<uint8_t>(strtoul(hex, nullptr, 16))) return false;
		}
		pattern += 2;
	}
	return true;
}

// The renderer's 2D calls, checked once per renderer object.
// ---- occlusion gate ----

using ActorVisibleFn = bool (*)(void* actor);
void* g_checkedActorVtbl[8] = {};       // actor vtables whose +0x408 slot passed the check
int g_checkedActorVtbls = 0;
bool g_gateBroken = false;

void LogOnce(const char* text) {
	char path[MAX_PATH];
	strcpy_s(path, g_iniPath);
	char* slash = strrchr(path, '\\');
	if (!slash) return;
	strcpy_s(slash + 1, static_cast<size_t>(path + sizeof(path) - (slash + 1)), "dinput8.log");
	FILE* f = nullptr;
	if (fopen_s(&f, path, "a") != 0 || !f) return;
	fprintf(f, "Nameplates: %s\n", text);
	fclose(f);
}

bool StartsWith(const void* fn, const char* pattern);

// The engine's WasVisibleThisFrame for this actor's class, checked once per vtable.
ActorVisibleFn ActorVisibleCall(uint8_t* actor) {
	void** vt = *reinterpret_cast<void***>(actor);
	if (!vt) return nullptr;
	void* fn = vt[kActorVtblWasVisible / sizeof(void*)];
	for (int i = 0; i < g_checkedActorVtbls; ++i)
		if (g_checkedActorVtbl[i] == vt) return reinterpret_cast<ActorVisibleFn>(fn);
	uint8_t* gfx = reinterpret_cast<uint8_t*>(GetModuleHandleA("EQGraphics.dll"));
	if (!gfx || fn != gfx + kWasVisibleRva ||
	    !StartsWith(fn, "48 8B 05 ?? ?? ?? ?? 8B 90 E0 B8 00 00 39 51 50 0F 94 C0 C3")) {
		g_gateBroken = true;
		LogOnce("bars off: the engine's actor visibility test does not match this EQGraphics.dll (Occlude=0 draws them without it)");
		return nullptr;
	}
	if (g_checkedActorVtbls < 8) g_checkedActorVtbl[g_checkedActorVtbls++] = vt;
	return reinterpret_cast<ActorVisibleFn>(fn);
}

// True when the client drew this spawn's name last frame (or may have: its actor rendered, the name has text and
// a bone, and it is inside the name's own distance cutoff).
bool NameDrawnLastFrame(uint8_t* spawn, float camX, float camY) {
	uint8_t* actor = *reinterpret_cast<uint8_t**>(spawn + kSpawnActor);
	uint8_t* sprite = actor ? *reinterpret_cast<uint8_t**>(actor + kActorNameSprite) : nullptr;
	if (!actor || !sprite) return false;
	ActorVisibleFn visible = ActorVisibleCall(actor);
	if (!visible || !visible(actor)) return false;
	if (!*reinterpret_cast<void**>(sprite + kSpriteText) || !*reinterpret_cast<void**>(sprite + kSpriteBone)) return false;
	const float scale = *reinterpret_cast<float*>(sprite + kSpriteScale);
	const float max2 = *reinterpret_cast<float*>(sprite + kSpriteMaxDist2);
	const float dx = *reinterpret_cast<float*>(sprite + kSpritePos) - camX;
	const float dy = *reinterpret_cast<float*>(sprite + kSpritePos + 4) - camY;
	return dx * dx + dy * dy <= scale * scale * max2;
}

bool RendererReady() {
	void* renderer = FctRenderer();
	if (!renderer) return false;
	if (renderer == g_renderer) return g_rendererOk;
	g_renderer = renderer;
	g_rendererOk = false;
	uint8_t* gfx = reinterpret_cast<uint8_t*>(GetModuleHandleA("EQGraphics.dll"));
	void** vt = *reinterpret_cast<void***>(renderer);
	if (!gfx || reinterpret_cast<uint8_t*>(vt) != gfx + kRendererVtblRva) return false;
	void* line = vt[kVtblDrawLine2D / sizeof(void*)];
	void* quad = vt[kVtblDrawColoredQuad / sizeof(void*)];
	if (!StartsWith(line, "40 53 48 83 EC 40 80 3D ?? ?? ?? ?? 00")) return false;
	if (!StartsWith(quad, "48 89 5C 24 08 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 41 8B D8")) return false;
	g_drawLine = reinterpret_cast<DrawLineFn>(line);
	g_drawQuad = reinterpret_cast<DrawQuadFn>(quad);
	g_rendererOk = true;
	return true;
}

void Quad(float x0, float y0, float x1, float y1, uint32_t argb) {
	const float q[12] = { x0, y0, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0 };
	g_drawQuad(g_renderer, q, argb);
}

// Legends' threat pulse: red, alpha 190, brightness 50..100 % on a 2 s triangle, fading beyond 80 units.
uint32_t ThreatColour(float distance) {
	float alpha = 190.0f * (distance > 80.0f ? 80.0f / distance : 1.0f);
	if (g_cfg.threatColour) return (static_cast<uint32_t>(alpha) << 24) | (g_cfg.threatColour & 0x00FFFFFFu);
	const uint64_t t = Now();
	const float phase = static_cast<float>(t % 1000) / 1000.0f;
	const float tri = (t % 2000 < 1000) ? phase : 1.0f - phase;
	const uint32_t red = static_cast<uint32_t>(255.0f * (1.0f - 0.5f * tri));
	return (static_cast<uint32_t>(alpha) << 24) | (red << 16);
}

// Text the DLL draws next to the bars, with the FontShadow / FontOutline layers.
void DrawLabel(uint8_t* mgr, const char* text, int x, int y, uint32_t argb, bool centre) {
	int count = HudFontCount(mgr);
	uint8_t* font = count > 0 ? HudFont(mgr, Clamp(static_cast<int>(lroundf(2.0f * g_cfg.size)), 0, count - 1)) : nullptr;
	if (!font) return;
	int screenH = 0;
	const int screenW = HudScreenSize(mgr, &screenH);
	const int clip[4] = { 0, 0, screenW, screenH };
	if (centre) x -= HudTextWidth(font, text) / 2;
	if (x < 1 || y < 1 || x >= screenW || y >= screenH) return;
	if (g_cfg.fontOutline) {
		static const int kOffsets[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
		for (const auto& o : kOffsets) HudDrawText(font, text, x + o[0], y + o[1], clip, 0xFF000000u);
	}
	else if (g_cfg.fontShadow) {
		HudDrawText(font, text, x + 1, y + 1, clip, 0xFF000000u);
	}
	HudDrawText(font, text, x, y, clip, argb);
}

// The engine name's screen box, or one made around the projected head point. False = behind the camera.
bool NameBox(uint8_t* spawn, uint8_t* mgr, float* box) {
	float size = *reinterpret_cast<float*>(spawn + kSpawnSize);
	if (!(size > 0.0f && size < 200.0f)) size = 6.0f;
	const float head[3] = {
		*reinterpret_cast<float*>(spawn + kSpawnY),
		*reinterpret_cast<float*>(spawn + kSpawnX),
		*reinterpret_cast<float*>(spawn + kSpawnZ) + size * 0.5f,
	};
	float sx = 0.0f, sy = 0.0f;
	if (!FctProject(head, &sx, &sy)) return false;
	const int32_t* vp = FctViewport();
	uint8_t* actor = *reinterpret_cast<uint8_t**>(spawn + kSpawnActor);
	uint8_t* sprite = actor ? *reinterpret_cast<uint8_t**>(actor + kActorNameSprite) : nullptr;
	if (sprite && *reinterpret_cast<int*>(sprite + kSpriteFont) == kSpriteFontName) {
		const int* r = reinterpret_cast<const int*>(sprite + kSpriteRect);
		const int w = r[2] - r[0], h = r[3] - r[1];
		if (w >= 1 && w <= vp[2] && h >= 1 && h <= vp[3] / 2 && r[0] >= -64 && r[2] <= vp[2] + 64 && r[1] >= -64 &&
		    r[3] <= vp[3] + 64) {
			const float left = static_cast<float>(r[0] + vp[0]), right = static_cast<float>(r[2] + vp[0]);
			const float top = static_cast<float>(r[1] + vp[1]), bottom = static_cast<float>(r[3] + vp[1]);
			// The rect is from the name's last draw: only trust it near where the spawn is now.
			const float slack = static_cast<float>(w > 64 ? w : 64);
			if (fabsf(sx - (left + right) * 0.5f) <= slack && sy >= top - 6.0f * h - 64.0f && sy <= bottom + 6.0f * h + 64.0f) {
				box[0] = left;
				box[1] = top;
				box[2] = right;
				box[3] = bottom;
				return true;
			}
		}
	}
	uint8_t* font = HudFont(mgr, 2);
	const float h = static_cast<float>(font ? HudLineHeight(font) : 14);
	box[0] = sx - 2.0f * h;
	box[1] = sy - h;
	box[2] = sx + 2.0f * h;
	box[3] = sy;
	return true;
}

const char* SpellName(int id) {
	uint8_t* mgr = static_cast<uint8_t*>(*g_spellMgr);
	if (!mgr || id <= 0 || id > *reinterpret_cast<int*>(mgr + kSpellMgrMaxId)) return nullptr;
	auto getSpell = reinterpret_cast<GetSpellFn>((*reinterpret_cast<void***>(mgr))[kVtblGetSpell / sizeof(void*)]);
	uint8_t* spell = getSpell(mgr, id);
	if (!spell || *reinterpret_cast<int*>(spell + kSpellId) != id) return nullptr;
	const char* name = reinterpret_cast<const char*>(spell + kSpellName);
	return name[0] ? name : nullptr;
}

// Remaining fraction of the spawn's cast (the bar empties); false when it has none.
bool CastProgress(uint32_t id, uint64_t now, float* p, int* spell) {
	CastEntry* c = FindCast(id);
	if (!c) return false;
	if (now >= c->end + kCastLingerMs) {
		*c = {};
		return false;
	}
	if (now >= c->end || c->end <= c->start) return false;
	*p = static_cast<float>(c->end - now) / static_cast<float>(c->end - c->start);
	*spell = c->spell;
	return *p > 0.0f && *p <= 1.0f;
}

bool ThreatFlag(uint8_t* spawn, uint32_t id, bool isTarget, uint64_t now) {
	if (!g_cfg.threatDisplay || !(g_cfg.fontShadow || g_cfg.fontOutline)) return false;
	const uint32_t me = MyId();
	if (isTarget && g_hottValid) {
		if (g_hottTarget == me) return true;
		if (g_cfg.threatDisplay != 2 || !g_hottTarget) return false;
		uint8_t* hott = FctSpawnById(g_hottTarget);
		return hott && (IsGroupMember(hott) || IsMyPet(hott, me));
	}
	(void)spawn;
	const ThreatEntry* t = FindThreat(id);
	if (!t) return false;
	const uint64_t hold = static_cast<uint64_t>(g_cfg.threatHoldMs);
	if (t->lastHitMe && now - t->lastHitMe <= hold) return true;
	return g_cfg.threatDisplay == 2 && t->lastHitGroup && now - t->lastHitGroup <= hold;
}

// ---- depth-tested bars (analysis\legends\nameplates-depth\dll-spec.md) ----
//
// Each bar rect is a DLL-owned CStringSprite holding one U+2588 FULL BLOCK, stretched over the rect, queued into the
// engine's name-tag text list at the name's own depth and sort key, so the "Render Name Tags" pass Z-tests it per
// pixel exactly like the name. Queued from the renderer's post-visibility callback (renderer vtbl +0x198 setter,
// called inside +0x158 after this frame's names are queued and before their flush). A spawn gets depth bars only
// when its name sprite is queued this frame (+0x70 != 0). Items still queued from a skipped flush are unlinked at
// the start of each callback. Labels (HP %, spell name) stay 2D in the DrawWindows hook. When a check fails, the
// block glyph looks wrong or no callback arrives, the 2D path (with the WasVisibleThisFrame gate) draws the bars.

const size_t kRendererSetPostVis = 0x198;
const size_t kRendererEndScene3D = 0x158;
const size_t kRendererPostVisCb = 0xb8b0;        // the callback slot the setter writes
const size_t kSetPostVisRva = 0xd9210, kPostVisCallSiteRva = 0xd7d35, kEndScene3DRva = 0xd7820;
const size_t kSpriteCtorRva = 0xfcff0, kSpriteSetTextRva = 0xfd890, kGetBatchRva = 0xf2670;
const size_t kBatchAddTextRva = 0xf1bb0, kBatchRemoveTextRva = 0xf2b20;
const size_t kSpriteVtblRva = 0x2c5498, kSpriteTextItemVtblRva = 0x2c5240;
const size_t kItemSize = 0xc0;
const size_t kItemTextW = 0x84, kItemTextH = 0x88;   // font-10 metrics after SetText
const size_t kItemArgb = 0x2c;
const size_t kItemDirty = 0x48;
const size_t kItemDepth = 0x58;
const size_t kItemKey = 0x5c;
const size_t kItemQueued = 0x70;                 // batch owner while queued
const size_t kBatchCount = 0x40;                 // int, flush gate "count > 0"
const size_t kSpawnUiOnly = 0x604;               // pLocalPlayer: non-zero = UI-only frames (no world)
const int kMaxItems = 512;

using SetPostVisFn = void* (*)(void* renderer, void (*cb)());
using SpriteCtorFn = void* (*)(void* mem);
using SpriteSetTextFn = void (*)(void* s, const char* text, const char* text2);
using GetBatchFn = void* (*)();
using BatchTextFn = void (*)(void* batch, void* item);

SpriteCtorFn g_itemCtor = nullptr;
SpriteSetTextFn g_itemSetText = nullptr;
GetBatchFn g_getBatch = nullptr;
BatchTextFn g_batchAdd = nullptr;
BatchTextFn g_batchRemove = nullptr;
void (*g_prevPostVis)() = nullptr;
void* g_cbRenderer = nullptr;                    // renderer the callback was installed on

struct alignas(16) Item {
	uint8_t mem[kItemSize];
};
Item g_items[kMaxItems];
int g_itemsBuilt = 0;
int g_itemsUsed = 0;
bool g_poolFullLogged = false;
enum DepthState { kDepthUnchecked, kDepthReady, kDepthOff };
DepthState g_depth = kDepthUnchecked;
volatile uint64_t g_lastCallback = 0;            // GetTickCount64 of the last callback
uint64_t g_depthSince = 0;                       // when the callback was installed

struct NameRef {
	float depth, key;
	uint32_t alpha;                                  // the name's fade, 0..255
};

bool GfxBytes(uint8_t* gfx, size_t rva, const char* pattern) {
	return StartsWith(gfx + rva, pattern);
}

void PostVisibleCallback();

// The renderer that draws the world: the EQGraphics global WasVisibleThisFrame compares frame numbers with
// (`mov rax,[rip+disp32]` at the start of RVA 0x534c0). eqgame's renderer global may hold another instance of the
// same class; the callback has to sit on the one whose +0x158 renders the scene.
void* EngineRenderer(uint8_t* gfx) {
	void* fallback = FctRenderer();
	if (!gfx || !StartsWith(gfx + kWasVisibleRva, "48 8B 05 ?? ?? ?? ?? 8B 90 E0 B8 00 00")) return fallback;
	int32_t disp;
	memcpy(&disp, gfx + kWasVisibleRva + 3, 4);
	void* engine = *reinterpret_cast<void**>(gfx + kWasVisibleRva + 7 + disp);
	if (!engine || *reinterpret_cast<uint8_t**>(engine) != gfx + kRendererVtblRva) return fallback;
	if (engine != fallback) LogOnce("the engine's world renderer is not eqgame's renderer global; using the engine's");
	return engine;
}

// Builds the pool and installs the callback once all checks pass (first in-game frame).
void InstallDepthBars() {
	if (g_depth != kDepthUnchecked) return;
	g_depth = kDepthOff;
	uint8_t* gfx = reinterpret_cast<uint8_t*>(GetModuleHandleA("EQGraphics.dll"));
	void* renderer = EngineRenderer(gfx);
	if (!gfx || !renderer) {
		LogOnce("2D bars: EQGraphics.dll or the renderer is missing");
		return;
	}
	void** vt = *reinterpret_cast<void***>(renderer);
	if (reinterpret_cast<uint8_t*>(vt) != gfx + kRendererVtblRva ||
	    reinterpret_cast<uint8_t*>(vt[kRendererSetPostVis / sizeof(void*)]) != gfx + kSetPostVisRva ||
	    reinterpret_cast<uint8_t*>(vt[kRendererEndScene3D / sizeof(void*)]) != gfx + kEndScene3DRva ||
	    !GfxBytes(gfx, kSetPostVisRva, "48 8B 81 B0 B8 00 00 48 89 91 B0 B8 00 00 C3") ||
	    !GfxBytes(gfx, kPostVisCallSiteRva, "48 8B 87 B0 B8 00 00 48 85 C0 74 02 FF D0") ||
	    !GfxBytes(gfx, kEndScene3DRva, "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55") ||
	    !GfxBytes(gfx, kSpriteCtorRva, "40 53 48 83 EC 20 48 8B D9 E8") ||
	    !GfxBytes(gfx, kSpriteSetTextRva, "40 53 48 83 EC 20 48 8B D9 48 85 D2 74") ||
	    !GfxBytes(gfx, kGetBatchRva, "40 53 48 83 EC 30 8B 0D") ||
	    !GfxBytes(gfx, kBatchAddTextRva, "4C 8B D2 4C 8B C9 48 85 D2") ||
	    !GfxBytes(gfx, kBatchRemoveTextRva, "48 85 D2 74 57 48 83 7A 68 00 74 50 48 83 C2 58")) {
		LogOnce("2D bars: the engine's name pass does not match this EQGraphics.dll");
		return;
	}
	g_itemCtor = reinterpret_cast<SpriteCtorFn>(gfx + kSpriteCtorRva);
	g_itemSetText = reinterpret_cast<SpriteSetTextFn>(gfx + kSpriteSetTextRva);
	g_getBatch = reinterpret_cast<GetBatchFn>(gfx + kGetBatchRva);
	g_batchAdd = reinterpret_cast<BatchTextFn>(gfx + kBatchAddTextRva);
	g_batchRemove = reinterpret_cast<BatchTextFn>(gfx + kBatchRemoveTextRva);
	static const char kBlock[] = "\xE2\x96\x88";   // U+2588 FULL BLOCK
	for (int i = 0; i < kMaxItems; ++i) {
		uint8_t* s = g_items[i].mem;
		g_itemCtor(s);
		g_itemsBuilt = i + 1;
		if (i == 0 && (*reinterpret_cast<uint8_t**>(s) != gfx + kSpriteVtblRva ||
		               *reinterpret_cast<uint8_t**>(s + 8) != gfx + kSpriteTextItemVtblRva)) {
			LogOnce("2D bars: the string sprite is not the expected class");
			return;
		}
		g_itemSetText(s, kBlock, kBlock);
		if (i == 0) {
			const int w = *reinterpret_cast<int*>(s + kItemTextW), h = *reinterpret_cast<int*>(s + kItemTextH);
			if (w < 10 || w > 60 || h < 30 || h > 60) {
				LogOnce("2D bars: the name font has no usable block glyph");
				return;
			}
		}
	}
	g_cbRenderer = renderer;
	g_prevPostVis = reinterpret_cast<void (*)()>(
		reinterpret_cast<SetPostVisFn>(vt[kRendererSetPostVis / sizeof(void*)])(renderer, &PostVisibleCallback));
	if (g_prevPostVis) LogOnce("depth bars on (chained to an existing post-visibility callback)");
	else LogOnce("depth bars on");
	g_depth = kDepthReady;
	g_depthSince = Now();
}

// Puts the previous callback back if ours is still installed; the items stay (they are reused if re-enabled).
void UninstallDepthBars(const char* why) {
	if (g_depth != kDepthReady) return;
	g_depth = kDepthOff;
	void* renderer = g_cbRenderer;
	const bool ours = renderer && *reinterpret_cast<void**>(static_cast<uint8_t*>(renderer) + kRendererPostVisCb) ==
	                                  reinterpret_cast<void*>(&PostVisibleCallback);
	if (ours)
		reinterpret_cast<SetPostVisFn>((*reinterpret_cast<void***>(renderer))[kRendererSetPostVis / sizeof(void*)])(renderer, g_prevPostVis);
	char text[256];
	snprintf(text, sizeof(text), "2D bars: %s (renderer %p, eqgame renderer %p, callback slot %s, frame %u)", why, renderer,
	         FctRenderer(), ours ? "still ours" : "replaced", renderer ? *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(renderer) + 0xb8e0) : 0u);
	LogOnce(text);
}

// Depth bars are drawing: installed and called back within the last half second.
bool DepthActive() {
	return g_depth == kDepthReady && g_lastCallback && Now() - g_lastCallback < 500;
}

// The camera's view-space depth inverse (dll-spec 4.3): moves a vertex depth `bias` world units toward the camera.
float BiasDepth(float z, float bias) {
	uint8_t* cam = FctCamera();
	if (!cam) return z;
	const float c10 = *reinterpret_cast<float*>(cam + 0x10), c14 = *reinterpret_cast<float*>(cam + 0x14);
	if (c14 == z) return z;
	const float zc = c14 * c10 / (c14 - z);
	const float zc2 = zc - bias;
	return zc2 > 0.01f ? c14 - c14 * c10 / zc2 : z;
}

void QueueRect(const NameRef& n, int l, int t, int r, int b, uint32_t argb) {
	if (r - l < 1 || b <= t) return;
	if (g_itemsUsed >= g_itemsBuilt) {
		if (!g_poolFullLogged) LogOnce("more bars than the item pool holds this frame; the rest are skipped");
		g_poolFullLogged = true;
		return;
	}
	uint8_t* s = g_items[g_itemsUsed++].mem;
	int* rc = reinterpret_cast<int*>(s + kSpriteRect);
	rc[0] = l;
	rc[1] = t;
	rc[2] = r;
	rc[3] = b;
	*reinterpret_cast<int*>(s + kSpriteFont) = kSpriteFontName;
	const uint32_t a = ((argb >> 24) * n.alpha + 127) / 255;   // fade with the name
	*reinterpret_cast<uint32_t*>(s + kItemArgb) = (a << 24) | (argb & 0x00FFFFFFu);
	*reinterpret_cast<float*>(s + kItemDepth) = n.depth;
	*reinterpret_cast<float*>(s + kItemKey) = n.key;           // equal keys draw in queue order
	s[kItemDirty] = 1;
	g_batchAdd(g_getBatch(), s + 8);
}

// ---- one spawn's bars, shared by both paths ----

struct Plan {
	uint8_t* spawn;
	uint8_t type;
	bool isMe, isTarget;
	float dist;                 // XY from the camera
	float hp, mana, castLeft;
	int spell;
	bool showHp, casting, flashing, showMana, threat;
};

// Everything but the occlusion gate and the name box. False = no bars for this spawn.
bool PlanSpawn(uint8_t* spawn, uint8_t* me, uint8_t* target, float camX, float camY, uint64_t now, Plan* p) {
	const uint8_t type = spawn[kSpawnType];
	if (type >= 2) return false;   // corpse
	const bool isMe = spawn == me, isTarget = spawn == target;
	if (!isMe && (!g_mayShowName(spawn) || spawn[kSpawnUntargetable] || g_isInvisibleTo(me, spawn, 0))) return false;
	const float dx = *reinterpret_cast<float*>(spawn + kSpawnY) - camX;
	const float dy = *reinterpret_cast<float*>(spawn + kSpawnX) - camY;
	if (!isTarget && dx * dx + dy * dy > g_cfg.drawDistance * g_cfg.drawDistance) return false;
	const int64_t hpCur = *reinterpret_cast<int64_t*>(spawn + kSpawnHpCur);
	const int64_t hpMax = *reinterpret_cast<int64_t*>(spawn + kSpawnHpMax);
	if (hpCur < 2 || hpMax <= 0) return false;
	Plan q = {};
	q.spawn = spawn;
	q.type = type;
	q.isMe = isMe;
	q.isTarget = isTarget;
	q.dist = sqrtf(dx * dx + dy * dy);
	q.hp = static_cast<float>(hpCur) / static_cast<float>(hpMax);
	const uint32_t id = *reinterpret_cast<uint32_t*>(spawn + kSpawnId);
	q.showHp = isMe ? g_cfg.selfHealth && (q.hp < 1.0f || isTarget) : g_cfg.targetHealth && (q.hp < 1.0f || isTarget);
	const bool castOn = isMe ? g_cfg.selfCast : g_cfg.cast;
	const bool manaOn = isMe ? g_cfg.selfMana : g_cfg.mana;
	q.casting = castOn && CastProgress(id, now, &q.castLeft, &q.spell);
	q.flashing = castOn && Flashing(id, now);
	const int32_t manaCur = *reinterpret_cast<int32_t*>(spawn + kSpawnManaCur);
	const int32_t manaMax = *reinterpret_cast<int32_t*>(spawn + kSpawnManaMax);
	q.mana = manaMax > 0 ? static_cast<float>(manaCur) / static_cast<float>(manaMax) : 1.0f;
	q.showMana = manaOn && !q.casting && manaCur >= 0 && (q.mana < 1.0f || isTarget) && (manaMax > 0 || isTarget) && q.mana > 0.0f;
	q.threat = !isMe && type == 1 && ThreatFlag(spawn, id, isTarget, now);
	if (!q.showHp && !q.casting && !q.flashing && !q.showMana && !q.threat) return false;
	*p = q;
	return true;
}

// Where rects go: 2D quads over everything, or name-pass items at the name's depth.
struct Sink {
	bool depth;
	NameRef name;
};

void PutRect(const Sink& s, float l, float t, float r, float b, uint32_t argb) {
	if (s.depth) QueueRect(s.name, static_cast<int>(l), static_cast<int>(t), static_cast<int>(r), static_cast<int>(b), argb);
	else Quad(l, t, r, b, argb);
}

void PutFrame(const Sink& s, float x0, float y0, float x1, float y1, uint32_t argb) {
	PutRect(s, x0, y0, x1, y0 + 1, argb);
	PutRect(s, x0, y1 - 1, x1, y1, argb);
	PutRect(s, x0, y0, x0 + 1, y1, argb);
	PutRect(s, x1 - 1, y0, x1, y1, argb);
}

void PutBar(const Sink& s, float cx, float y, float w, float h, float pct, uint32_t fill, bool threat, uint32_t threatColour) {
	const float x0 = floorf(cx - w * 0.5f);
	w = floorf(w);
	h = floorf(h);
	y = floorf(y);
	PutRect(s, x0 + 1, y - 1, x0 + w + 1, y + h + 1, kColourBacking);
	if (pct > 0.0f) PutRect(s, x0, y, x0 + roundf(w * ClampF(pct, 0.0f, 1.0f)), y + h, fill);
	if (threat) PutFrame(s, x0 - 1, y - 2, x0 + w + 2, y + h + 2, threatColour);
}

// Lays out a spawn's bars under the name box; `mgr` non-null draws the 2D labels too, `bars` false skips the rects.
void Layout(const Sink& s, const Plan& p, const float* box, uint8_t* mgr, bool bars) {
	const float nameH = box[3] - box[1];
	const float viewportW = static_cast<float>(FctViewport()[2]);
	const float h = ClampF(roundf(nameH / 9.0f * g_cfg.size), 2.0f, (6.0f * g_cfg.size > 2.0f ? 6.0f * g_cfg.size : 2.0f));
	const float w = ClampF(roundf(nameH * 4.5f * g_cfg.size), 24.0f, viewportW / 4.0f > 24.0f ? viewportW / 4.0f : 24.0f);
	const float cx = (box[0] + box[2]) * 0.5f;
	float y = box[3];
	const uint32_t threatColour = p.threat ? ThreatColour(p.dist) : 0;
	if (p.showHp) {
		const uint32_t fill = g_cfg.hpByCon && p.type == 1 ? HudConColour(p.spawn) : kColourHp;
		if (bars) PutBar(s, cx, y, w, h, p.hp, fill, p.threat, threatColour);
		if (mgr && g_cfg.hpText) {
			char text[16];
			snprintf(text, sizeof(text), "%d%%", static_cast<int>(p.hp * 100.0f + 0.5f));
			DrawLabel(mgr, text, static_cast<int>(cx + w * 0.5f + 4.0f), static_cast<int>(y + h * 0.5f) - 6, 0xFFFFFFFFu, false);
		}
		y += h + 1.0f;
	}
	if (p.flashing) {
		if (bars) PutBar(s, cx, y, w, h, 1.0f, kColourHp, p.threat, threatColour);
		y += h + 1.0f;
	}
	else if (p.casting) {
		if (bars) PutBar(s, cx, y, w, h, p.castLeft, kColourCast, p.threat, threatColour);
		y += h + 1.0f;
		if (mgr && g_cfg.castSpellName) {
			if (const char* name = SpellName(p.spell))
				DrawLabel(mgr, name, static_cast<int>(cx), static_cast<int>(y), 0xFFFFFFFFu, true);
		}
	}
	if (p.showMana && bars) PutBar(s, cx, y, w, h, p.mana, kColourMana, p.threat, threatColour);
	if (p.threat && bars && !p.showHp && !p.casting && !p.flashing && !p.showMana)
		PutFrame(s, box[0] - 1, box[1] - 1, box[2] + 1, box[3] + 1, threatColour);
}

void TrackTarget() {
	uint8_t* target = *g_pTarget;
	if (target != g_lastTarget) {
		g_lastTarget = target;
		g_hottTarget = 0;
		g_hottValid = false;
	}
}

// Renderer post-visibility callback: this frame's names are queued, the flush has not run yet.
void PostVisibleCallback() {
	if (g_prevPostVis) g_prevPostVis();
	g_lastCallback = Now();
	void* batch = g_getBatch();
	for (int i = 0; i < g_itemsUsed; ++i) {
		uint8_t* s = g_items[i].mem;
		if (*reinterpret_cast<void**>(s + kItemQueued)) g_batchRemove(batch, s + 8);   // the flush was skipped
	}
	g_itemsUsed = 0;
	if (batch) {
		int* count = reinterpret_cast<int*>(static_cast<uint8_t*>(batch) + kBatchCount);
		if (*count > 0x40000000) *count = 0x100000;   // keep the flush gate positive, never let it wrap
	}
	if (!g_on || !g_cfg.occlude || !FctInGame()) return;
	uint8_t* cam = FctCamera();
	uint8_t* me = FctLocalPlayer();
	if (!cam || !me) return;
	TrackTarget();
	uint8_t* target = *g_pTarget;
	const uint64_t now = Now();
	const float camX = *reinterpret_cast<float*>(cam + 0xc0), camY = *reinterpret_cast<float*>(cam + 0xc4);
	int guard = 0;
	for (uint8_t* spawn = FctFirstSpawn(); spawn && guard < 4096;
	     spawn = *reinterpret_cast<uint8_t**>(spawn + kSpawnNext), ++guard) {
		Plan p;
		if (!PlanSpawn(spawn, me, target, camX, camY, now, &p)) continue;
		if (p.isMe && g_cfg.selfIgnoresOcclusion) continue;   // 2D path
		uint8_t* actor = *reinterpret_cast<uint8_t**>(spawn + kSpawnActor);
		uint8_t* name = actor ? *reinterpret_cast<uint8_t**>(actor + kActorNameSprite) : nullptr;
		if (!name || !*reinterpret_cast<void**>(name + kItemQueued)) continue;   // the name is not drawn this frame
		if (*reinterpret_cast<int*>(name + kSpriteFont) != kSpriteFontName) continue;
		const int* r = reinterpret_cast<const int*>(name + kSpriteRect);
		if (r[2] <= r[0] || r[3] <= r[1]) continue;
		Sink s = { true, {} };
		s.name.depth = *reinterpret_cast<float*>(name + kItemDepth);
		s.name.key = *reinterpret_cast<float*>(name + kItemKey);
		s.name.alpha = *reinterpret_cast<uint32_t*>(name + kItemArgb) >> 24;
		if (g_cfg.depthBias > 0.0f) s.name.depth = BiasDepth(s.name.depth, g_cfg.depthBias * *reinterpret_cast<float*>(name + kSpriteScale));
		const float box[4] = { static_cast<float>(r[0]), static_cast<float>(r[1]), static_cast<float>(r[2]), static_cast<float>(r[3]) };
		Layout(s, p, box, nullptr, true);   // the name's rect is already in vertex pixel space
	}
}

void DrawNameplates(uint8_t* mgr) {
	uint8_t* cam = FctCamera();
	if (!cam || !RendererReady()) return;
	uint8_t* me = FctLocalPlayer();
	if (!me) return;
	if (g_cfg.occlude) InstallDepthBars();
	// Callbacks stopped while world frames run: back to 2D.
	if (g_depth == kDepthReady && !me[kSpawnUiOnly]) {
		const uint64_t last = g_lastCallback ? g_lastCallback : g_depthSince;
		if (Now() - last > 2000) UninstallDepthBars("no name-pass callback for two seconds");
	}
	TrackTarget();
	uint8_t* target = *g_pTarget;
	const bool depth = g_cfg.occlude && DepthActive();
	const uint64_t now = Now();
	const float camX = *reinterpret_cast<float*>(cam + 0xc0), camY = *reinterpret_cast<float*>(cam + 0xc4);
	const Sink flat = { false, {} };
	int guard = 0;
	for (uint8_t* spawn = FctFirstSpawn(); spawn && guard < 4096;
	     spawn = *reinterpret_cast<uint8_t**>(spawn + kSpawnNext), ++guard) {
		Plan p;
		if (!PlanSpawn(spawn, me, target, camX, camY, now, &p)) continue;
		const bool gated = g_cfg.occlude && !(p.isMe && g_cfg.selfIgnoresOcclusion);
		const bool bars = !(depth && gated);   // the callback draws these bars in the name pass
		if (!bars && !(g_cfg.hpText || g_cfg.castSpellName)) continue;
		if (gated && (g_gateBroken || !NameDrawnLastFrame(spawn, camX, camY))) continue;   // behind a wall, culled or no name
		float box[4];
		if (!NameBox(spawn, mgr, box)) continue;
		Layout(flat, p, box, mgr, bars);
	}
}

#define REF(var) reinterpret_cast<void**>(&var)

} // namespace

void NameplatesFrame(uint8_t* mgr, bool inGame) {
	if (!g_on) return;
	if (!inGame || !FctInGame()) {
		ClearState();
		return;
	}
	DrawNameplates(mgr);
}

void NameplatesOnDamage(const uint8_t* pkt) {
	if (!g_on || !g_cfg.threatDisplay || !FctInGame()) return;
	const int64_t damage = *reinterpret_cast<const int64_t*>(pkt + kDmgAmount);
	if (damage == kDamageInvulnerable) return;
	const uint32_t targetId = *reinterpret_cast<const uint16_t*>(pkt + kDmgTarget);
	const uint32_t sourceId = *reinterpret_cast<const uint16_t*>(pkt + kDmgSource);
	const uint32_t me = MyId();
	uint8_t* source = sourceId ? FctSpawnById(sourceId) : nullptr;
	if (!source || source[kSpawnType] != 1 || IsMyPet(source, me)) return;
	if (targetId == me) {
		ThreatEntry& t = ThreatSlot(sourceId);
		t.npc = sourceId;
		t.lastHitMe = Now();
		return;
	}
	uint8_t* target = FctSpawnById(targetId);
	if (target && (IsGroupMember(target) || IsMyPet(target, me))) {
		ThreatEntry& t = ThreatSlot(sourceId);
		t.npc = sourceId;
		t.lastHitGroup = Now();
	}
}

const HookSite kNameplatesHooks[4] = {
	// Shared with the Hud and FCT (same signature, detour and trampoline).
	{ kDrawWindowsSig, 5, reinterpret_cast<void*>(&DrawWindowsDetour), reinterpret_cast<void**>(&g_origDrawWindows) },
	// Shared with FCT: the OP_Damage handler FUN_140216190.
	{ kFctDamageSig, 5, reinterpret_cast<void*>(g_fctDamageDetour), reinterpret_cast<void**>(g_fctOrigDamageSlot) },
	// StartCasting FUN_140293100(CEverQuest*, int* pkt) (OP_BeginCast). Stolen (7 bytes, position independent):
	// mov rax,rsp; mov [rax+8],rbx.
	{ "48 8B C4 48 89 58 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 B0",
	  7, reinterpret_cast<void*>(&StartCastingDetour), REF(g_origStartCasting) },
	// Packet dispatcher FUN_1401e4010(this, rdx, opcode, data, size). Stolen (5 bytes, position independent):
	// mov [rsp+20],r9.
	{ "4C 89 4C 24 20 44 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 56",
	  5, reinterpret_cast<void*>(&DispatchDetour), REF(g_origDispatch) },
};

const RefSite kNameplatesRefs[4] = {
	// mov rcx,[pTarget]; mov byte [rsp+rbx+30],0; test rcx,rcx; je
	{ "48 8B 0D ?? ?? ?? ?? C6 44 1C 30 00 48 85 C9 0F 84 ?? ?? ?? ??", 3, 7, REF(g_pTarget) },
	// mov rcx,[spellMgr]; cmp eax,[rcx+64]; jg; mov rax,[rcx]; call [rax+40] (GetSpell)
	{ "48 8B 0D ?? ?? ?? ?? 3B 41 64 0F 8F ?? ?? ?? ?? 48 8B 01 FF 50 40 48 89 44 24 60", 3, 7, REF(g_spellMgr) },
	// MayShowName FUN_1402fa230(spawn): cmp byte [rcx+135],1; jne; mov rax,[pDisplay]
	{ "80 B9 35 01 00 00 01 75 ?? 48 8B 05 ?? ?? ?? ??", 0, 0, REF(g_mayShowName) },
	// IsInvisibleToMe FUN_140315770(me, spawn, lenient)
	{ "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20", 0, 0, REF(g_isInvisibleTo) },
};

const char* NameplatesPrepare() {
	if (const char* e = FctSharedPrepare()) return e;
	LoadConfig();
	g_on = true;
	return nullptr;
}

// ---- NameplateOptionsPage ----

namespace {

using SetNpcNamesFn = void (*)(uint8_t* display, uint8_t on);
int32_t* g_showNamesLevel = nullptr;     // &DAT_140e6e3a8, /shownames
SetNpcNamesFn g_setNpcNames = nullptr;   // CDisplay::SetNPCNames FUN_1401a3bf0
uint8_t** g_pDisplayPage = nullptr;      // &DAT_140e65d60
const size_t kDisplayNpcNames = 0x5512;  // u8, current NPCNames setting
bool g_pageOn = false;

struct BoxInfo {
	const char* id;
	const char* key;
	bool Config::*field;
};
const BoxInfo kBoxes[] = {
	{ "ODP_NameplateShowHealthCheckbox", "TargetHealth", &Config::targetHealth },
	{ "ODP_NameplateShowCastCheckbox", "Cast", &Config::cast },
	{ "ODP_NameplateShowManaCheckbox", "Mana", &Config::mana },
	{ "ODP_NameplateShowSelfHealthCheckbox", "SelfHealth", &Config::selfHealth },
	{ "ODP_NameplateShowSelfManaCheckbox", "SelfMana", &Config::selfMana },
	{ "ODP_NameplateShowSelfCastCheckbox", "SelfCast", &Config::selfCast },
	{ "ODP_NameplateShadowCheckbox", "FontShadow", &Config::fontShadow },
	{ "ODP_NameplateOutlineCheckbox", "FontOutline", &Config::fontOutline },
	{ "ODP_NameplateSelfNameCheckbox", "SelfName", &Config::selfName },
	{ "ODP_NameplateCastSpellNameCheckbox", "CastSpellName", &Config::castSpellName },
	{ "ODP_NameplateHpTextCheckbox", "HpText", &Config::hpText },
	{ "ODP_NameplateOccludeCheckbox", "Occlude", &Config::occlude },
	{ "ODP_NameplateHpByConCheckbox", "HpByCon", &Config::hpByCon },
};
const int kBoxCount = static_cast<int>(sizeof(kBoxes) / sizeof(kBoxes[0]));
const int kHpByConBox = kBoxCount - 1;

enum Slider { kSDrawDistance, kSSize, kSThreatHold, kSliderCount };
const char* const kSliderIds[kSliderCount] = { "ODP_NameplateDrawDistanceSlider", "ODP_NameplateSizeSlider", "ODP_NameplateThreatHoldSlider" };
const char* const kValueIds[kSliderCount] = { "ODP_NameplateDrawDistanceValueLabel", "ODP_NameplateSizeValueLabel", "ODP_NameplateThreatHoldValueLabel" };
const int kSliderTicks[kSliderCount] = { 100, 200, 59 };

// Threat frame colour rows: Legends pulse (empty), presets, Custom (ini).
const uint32_t kThreatColours[] = { 0, 0xFF0000, 0xFF8000, 0xFFFF00, 0xFFFFFF, 0xC000FF };
const int kThreatColourCount = static_cast<int>(sizeof(kThreatColours) / sizeof(kThreatColours[0]));

struct Page {
	uint8_t* owner;
	uint8_t* page;
	uint8_t* box[kBoxCount];
	uint8_t* conColour;
	uint8_t* slider[kSliderCount];
	uint8_t* value[kSliderCount];
	uint8_t* nameFilters;
	uint8_t* threat;
	uint8_t* threatColour;
};
Page g_page = {};

// Slider position <-> value (dll-spec 6, addendum 2).
float SliderValue(int s, int pos) {
	pos = Clamp(pos, 0, kSliderTicks[s] - 1);
	switch (s) {
	case kSDrawDistance: return 50.0f + (pos + 1) / 100.0f * 350.0f;
	case kSSize: return (pos + 1) / 200.0f * 2.0f;
	default: return 1000.0f + pos * 500.0f;
	}
}

int SliderPosition(int s) {
	switch (s) {
	case kSDrawDistance: return Clamp(static_cast<int>(lroundf((g_cfg.drawDistance - 50.0f) / 350.0f * 100.0f)) - 1, 0, 99);
	case kSSize: return Clamp(static_cast<int>(lroundf(g_cfg.size / 2.0f * 200.0f)) - 1, 0, 199);
	default: return Clamp((g_cfg.threatHoldMs - 1000) / 500, 0, 58);
	}
}

void SliderLabel(int s, char* text, size_t size) {
	switch (s) {
	case kSDrawDistance: snprintf(text, size, "%.0f", g_cfg.drawDistance); break;
	case kSSize: snprintf(text, size, "%.2f", g_cfg.size); break;
	default: snprintf(text, size, "%.1f", g_cfg.threatHoldMs / 1000.0f); break;
	}
}

int ThreatColourRow() {
	if (!g_cfg.threatColour) return 0;
	for (int i = 1; i < kThreatColourCount; ++i)
		if (kThreatColours[i] == (g_cfg.threatColour & 0x00FFFFFFu)) return i;
	return kThreatColourCount;   // Custom (ini)
}

void Fill() {
	for (int i = 0; i < kBoxCount; ++i) {
		bool on = g_cfg.*kBoxes[i].field;
		if (i == kHpByConBox && !ConNameTagsApplied()) on = false;
		OptSetBox(g_page.box[i], on);
	}
	OptSetBox(g_page.conColour, ConNameTagsEnabled());
	for (int s = 0; s < kSliderCount; ++s) {
		OptSliderSetValue(g_page.slider[s], SliderPosition(s));
		char text[32];
		SliderLabel(s, text, sizeof(text));
		OptSetText(g_page.value[s], text);
	}
	OptComboSetChoice(g_page.nameFilters, Clamp(*g_showNamesLevel - 1, 0, 6));
	OptComboSetChoice(g_page.threat, g_cfg.threatDisplay);
	OptComboSetChoice(g_page.threatColour, ThreatColourRow());
}

bool Resolve() {
	uint8_t* opt = OptOptionsWnd();
	if (!opt) {
		g_page = {};
		return false;
	}
	if (g_page.owner == opt) return g_page.page != nullptr;
	g_page = {};
	g_page.owner = opt;
	uint8_t* page = OptChild(opt, "OptionsNamePlatesPage");
	if (!page) return false;
	Page p = {};
	p.owner = opt;
	p.page = page;
	for (int i = 0; i < kBoxCount; ++i) {
		p.box[i] = OptChild(page, kBoxes[i].id);
		if (p.box[i] && !OptLooksLikeCheckbox(opt, p.box[i])) return false;
	}
	p.conColour = OptChild(page, "ODP_NameplateConColor");
	if (p.conColour && !OptLooksLikeCheckbox(opt, p.conColour)) return false;
	for (int s = 0; s < kSliderCount; ++s) {
		p.slider[s] = OptChild(page, kSliderIds[s]);
		p.value[s] = OptChild(page, kValueIds[s]);
		OptSliderSetTicks(p.slider[s], kSliderTicks[s]);
	}
	p.nameFilters = OptChild(page, "ODP_NameplateNameFilters");
	p.threat = OptChild(page, "ODP_NameplateThreatOptions");
	p.threatColour = OptChild(page, "ODP_NameplateThreatColourCombobox");
	g_page = p;
	Fill();
	return true;
}

} // namespace

void NameplatePageRefresh(uint8_t* opt) {
	(void)opt;
	if (g_pageOn) Resolve();
}

bool NameplatePageNotify(uint8_t* opt, uint8_t* sender, int msg, void* data) {
	if (!g_pageOn || opt != OptOptionsWnd() || !Resolve()) return false;
	if (OptIsTabSwitchTo(opt, sender, msg, data, g_page.page)) {
		Fill();          // the client never refreshes an unknown page
		return false;    // the tab box still needs the client's handling
	}
	const int arg = static_cast<int>(reinterpret_cast<intptr_t>(data));
	if (msg == 1) {
		for (int i = 0; i < kBoxCount; ++i) {
			if (sender != g_page.box[i]) continue;
			bool on = OptIsChecked(sender);
			if (i == kHpByConBox && !ConNameTagsApplied()) {
				OptSetBox(sender, false);   // needs the con colour pipeline
				return true;
			}
			g_cfg.*kBoxes[i].field = on;
			SaveKey(kBoxes[i].key, on ? "1" : "0");
			return true;
		}
		if (sender == g_page.conColour) {
			if (!ConNameTagsApplied()) {
				OptSetBox(sender, false);
				return true;
			}
			const bool on = OptIsChecked(sender);
			ConNameTagsSetEnabled(on);
			if (g_iniPath[0]) WritePrivateProfileStringA("Features", "ConNameTags", on ? "1" : "0", g_iniPath);
			return true;
		}
		if (sender == g_page.nameFilters) {
			*g_showNamesLevel = Clamp(arg, 0, 6) + 1;
			uint8_t* display = *g_pDisplayPage;
			if (display) g_setNpcNames(display, display[kDisplayNpcNames]);
			return true;
		}
		if (sender == g_page.threat) {
			g_cfg.threatDisplay = Clamp(arg, 0, 2);
			char text[8];
			snprintf(text, sizeof(text), "%d", g_cfg.threatDisplay);
			SaveKey("ThreatDisplay", text);
			return true;
		}
		if (sender == g_page.threatColour) {
			if (arg < 0 || arg >= kThreatColourCount) {
				OptComboSetChoice(sender, ThreatColourRow());   // Custom (ini): keep the ini colour
				return true;
			}
			const uint32_t rgb = kThreatColours[arg];
			g_cfg.threatColour = rgb ? (rgb | 0xFF000000u) : 0;
			char text[16] = "";
			if (rgb) snprintf(text, sizeof(text), "%06X", rgb);
			SaveKey("ThreatFrameColour", text);
			return true;
		}
	}
	if (msg == 0xe) {
		for (int s = 0; s < kSliderCount; ++s) {
			if (sender != g_page.slider[s]) continue;
			const float v = SliderValue(s, arg);
			char text[32];
			if (s == kSDrawDistance) {
				g_cfg.drawDistance = v;
				snprintf(text, sizeof(text), "%.0f", v);
				SaveKey("DrawDistance", text);
			}
			else if (s == kSSize) {
				g_cfg.size = ClampF(v, 0.2f, 2.0f);
				snprintf(text, sizeof(text), "%.2f", g_cfg.size);
				SaveKey("Size", text);
			}
			else {
				g_cfg.threatHoldMs = static_cast<int>(v);
				snprintf(text, sizeof(text), "%d", g_cfg.threatHoldMs);
				SaveKey("ThreatHoldMs", text);
			}
			SliderLabel(s, text, sizeof(text));
			OptSetText(g_page.value[s], text);
			return true;
		}
	}
	return false;
}

const HookSite kNameplatePageHooks[2] = {
	// Shared with FCTOptionsPage (same signatures, detours and trampolines).
	{ kFctRefreshSig, 7, reinterpret_cast<void*>(g_fctRefreshDetour), reinterpret_cast<void**>(g_fctOrigRefreshSlot) },
	{ kFctNotifySig, 5, reinterpret_cast<void*>(g_fctNotifyDetour), reinterpret_cast<void**>(g_fctOrigNotifySlot) },
};

const RefSite kNameplatePageRefs[3] = {
	// cmp dword [ShowNamesLevel],0; jg; cmp byte [rdi+135],1 (SetNameSpriteState)
	{ "83 3D ?? ?? ?? ?? 00 7F ?? 80 BF 35 01 00 00 01", 2, 7, REF(g_showNamesLevel) },
	// CDisplay::SetNPCNames FUN_1401a3bf0(pDisplay, on): ... mov [rdi+5512],sil
	{ "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 0F B6 F2 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 08 40 88 B7 12 55 00 00",
	  0, 0, REF(g_setNpcNames) },
	// Game time read: mov rax,[pDisplay]; mov ecx,[rax+16c]; mov [rbx+45c],ecx
	{ "48 8B 05 ?? ?? ?? ?? 8B 88 6C 01 00 00 89 8B 5C 04 00 00", 3, 7, REF(g_pDisplayPage) },
};

const char* NameplatePagePrepare() {
	if (const char* e = FctPageSharedPrepare()) return e;
	if (!g_on) LoadConfig();   // the page edits the settings even when the bars are off
	g_pageOn = true;
	return nullptr;
}
