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
const size_t kSpriteFont = 0x18;         // int, 10 when text is set
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
	bool selfHealth = true;
	bool selfMana = true;
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

void Frame(float x0, float y0, float x1, float y1, uint32_t argb) {
	const float a[3] = { x0, y0, 0 }, b[3] = { x1, y0, 0 }, c[3] = { x1, y1, 0 }, d[3] = { x0, y1, 0 };
	g_drawLine(g_renderer, a, b, argb);
	g_drawLine(g_renderer, b, c, argb);
	g_drawLine(g_renderer, c, d, argb);
	g_drawLine(g_renderer, d, a, argb);
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

void DrawBar(float cx, float y, float w, float h, float pct, uint32_t fill, bool threat, uint32_t threatColour) {
	const float x0 = floorf(cx - w * 0.5f);
	Quad(x0 + 1, y - 1, x0 + w + 1, y + h + 1, kColourBacking);
	if (pct > 0.0f) Quad(x0, y, x0 + w * ClampF(pct, 0.0f, 1.0f), y + h, fill);
	if (threat) Frame(x0 - 1, y - 2, x0 + w + 2, y + h + 2, threatColour);
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

void DrawNameplates(uint8_t* mgr) {
	uint8_t* cam = FctCamera();
	if (!cam || !RendererReady()) return;
	uint8_t* me = FctLocalPlayer();
	if (!me) return;
	uint8_t* target = *g_pTarget;
	if (target != g_lastTarget) {
		g_lastTarget = target;
		g_hottTarget = 0;
		g_hottValid = false;
	}
	const uint64_t now = Now();
	const float camX = *reinterpret_cast<float*>(cam + 0xc0), camY = *reinterpret_cast<float*>(cam + 0xc4);
	const float drawDist2 = g_cfg.drawDistance * g_cfg.drawDistance;
	const float viewportW = static_cast<float>(FctViewport()[2]);
	int guard = 0;
	for (uint8_t* spawn = FctFirstSpawn(); spawn && guard < 4096;
	     spawn = *reinterpret_cast<uint8_t**>(spawn + kSpawnNext), ++guard) {
		const uint8_t type = spawn[kSpawnType];
		if (type >= 2) continue;   // corpse
		const bool isMe = spawn == me, isTarget = spawn == target;
		if (!isMe && (!g_mayShowName(spawn) || spawn[kSpawnUntargetable] || g_isInvisibleTo(me, spawn, 0))) continue;
		const float dx = *reinterpret_cast<float*>(spawn + kSpawnY) - camX;
		const float dy = *reinterpret_cast<float*>(spawn + kSpawnX) - camY;
		if (!isTarget && dx * dx + dy * dy > drawDist2) continue;
		const int64_t hpCur = *reinterpret_cast<int64_t*>(spawn + kSpawnHpCur);
		const int64_t hpMax = *reinterpret_cast<int64_t*>(spawn + kSpawnHpMax);
		if (hpCur < 2 || hpMax <= 0) continue;
		const float hp = static_cast<float>(hpCur) / static_cast<float>(hpMax);
		const uint32_t id = *reinterpret_cast<uint32_t*>(spawn + kSpawnId);
		const bool showHp = isMe ? g_cfg.selfHealth && (hp < 1.0f || isTarget) : g_cfg.targetHealth && (hp < 1.0f || isTarget);
		const bool castOn = isMe ? g_cfg.selfCast : g_cfg.cast;
		const bool manaOn = isMe ? g_cfg.selfMana : g_cfg.mana;
		float castLeft = 0.0f;
		int spell = 0;
		const bool casting = castOn && CastProgress(id, now, &castLeft, &spell);
		const bool flashing = castOn && Flashing(id, now);
		const int32_t manaCur = *reinterpret_cast<int32_t*>(spawn + kSpawnManaCur);
		const int32_t manaMax = *reinterpret_cast<int32_t*>(spawn + kSpawnManaMax);
		const float mana = manaMax > 0 ? static_cast<float>(manaCur) / static_cast<float>(manaMax) : 1.0f;
		const bool showMana = manaOn && !casting && manaCur >= 0 && (mana < 1.0f || isTarget) && (manaMax > 0 || isTarget) && mana > 0.0f;
		const bool threat = !isMe && type == 1 && ThreatFlag(spawn, id, isTarget, now);
		if (!showHp && !casting && !flashing && !showMana && !threat) continue;
		float box[4];
		if (!NameBox(spawn, mgr, box)) continue;
		const float nameH = box[3] - box[1];
		const float h = ClampF(roundf(nameH / 9.0f * g_cfg.size), 2.0f, (6.0f * g_cfg.size > 2.0f ? 6.0f * g_cfg.size : 2.0f));
		const float w = ClampF(roundf(nameH * 4.5f * g_cfg.size), 24.0f, viewportW / 4.0f > 24.0f ? viewportW / 4.0f : 24.0f);
		const float cx = (box[0] + box[2]) * 0.5f;
		float y = box[3];
		const uint32_t threatColour = threat ? ThreatColour(sqrtf(dx * dx + dy * dy)) : 0;
		if (showHp) {
			const uint32_t fill = g_cfg.hpByCon && type == 1 ? HudConColour(spawn) : kColourHp;
			DrawBar(cx, y, w, h, hp, fill, threat, threatColour);
			if (g_cfg.hpText) {
				char text[16];
				snprintf(text, sizeof(text), "%d%%", static_cast<int>(hp * 100.0f + 0.5f));
				DrawLabel(mgr, text, static_cast<int>(cx + w * 0.5f + 4.0f), static_cast<int>(y + h * 0.5f) - 6, 0xFFFFFFFFu, false);
			}
			y += h + 1.0f;
		}
		if (flashing) {
			DrawBar(cx, y, w, h, 1.0f, kColourHp, threat, threatColour);
			y += h + 1.0f;
		}
		else if (casting) {
			DrawBar(cx, y, w, h, castLeft, kColourCast, threat, threatColour);
			y += h + 1.0f;
			if (g_cfg.castSpellName) {
				if (const char* name = SpellName(spell))
					DrawLabel(mgr, name, static_cast<int>(cx), static_cast<int>(y), 0xFFFFFFFFu, true);
			}
		}
		if (showMana) DrawBar(cx, y, w, h, mana, kColourMana, threat, threatColour);
		if (threat && !showHp && !casting && !flashing && !showMana)
			Frame(box[0] - 1, box[1] - 1, box[2] + 1, box[3] + 1, threatColour);
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
