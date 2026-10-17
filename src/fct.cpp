// FloatingCombatText: damage numbers rising from characters and NPCs in the 3D world, like the
// Legends client's floating combat text. Client-only, fed by the OP_Damage packets the server already sends.
//
// Events: detour on the OP_Damage handler FUN_140216190(pkt). The original runs first (HP, chat message),
// then we read the packet the way it does: target id u16 @0, source id u16 @2, damage int64 @8 (0 miss,
// -1 block, -2 parry, -3 riposte, -4 dodge, -5 invulnerable, -6 rune/absorb), special u32 @0x10 (bit 7 =
// crit), spell id int @0x14, type u8 @0x28 (0xE7 spell, below it a melee skill, 0xEC+ damage shield).
// Hits between two other spawns are shown only within OtherRange of the target, measured with the same
// function and arguments the handler uses for its own chat gate (FUN_14019b2c0(pDisplay, me, target, 1.0)
// = |dx|+|dy|+|dz|, compared with 200.0). Spawns come from the spawn manager's GetSpawnByID (vtbl+0x10),
// the call the handler itself makes. Categories as in Legends: You = source is me or my pet (spawn
// +0x2d8 = my id), Self = target is me, Other = everything else. The server sends a pet owner the same
// OP_Damage twice for pet hits, so an identical {source, target, damage, type} involving my pet within
// DedupMs is dropped (only for my pet: two real double-attack hits of equal size must both show).
// Heal numbers: the heal-message handler is not mapped for ToB yet; when it is, its detour only has to
// call AddEntry() with the healed spawn and the text (a Heal category would sit next to You/Self/Other).
//
// Drawing: from the CXWndManager::DrawWindows hook shared with the Hud (hud.cpp), after the client's
// windows, while in game. Each frame every entry re-projects its spawn's head point {spawn+0x74, +0x78,
// +0x7c + size(+0x154) * HeadHeight} (memory order, no swap) with the camera's WorldToScreen (EQGraphics
// CCameraInterface vtbl+0xe8; camera = *(pDisplay + 0x118), the offset read from the "Camera created."
// site) and adds a ballistic offset in pixels: a random angle in MinAngle..MaxAngle (0 = right, 90 = up)
// and speed in MinVelocity..MaxVelocity, pulled down by Gravity, both scaled by PixelsPerUnit. A point
// behind the camera is not drawn that frame. The camera's vtable and the slot's first bytes are checked
// against the EQGraphics.dll this was mapped on (vtable at +0x2b2138); otherwise the camera's own matrices
// are used (the same arithmetic as the slot) when the camera's viewport width matches, else nothing is
// drawn. Text goes through the Hud's helpers (CTextureFont::DrawWrappedText FUN_1405d1990), centred on
// the point, with an optional 1 px black outline drawn first, and fades out over the last FadeMs.
// Entries are dropped when their spawn is gone, and all are dropped when the client leaves the game
// state or the display, camera or renderer is missing (zoning).
// In-game toggle: ToggleKey (default Ctrl+Shift+F) while the game window has focus turns the numbers off
// and on, shows "Floating combat text: on/off" for two seconds and saves the state as [FCT] Show.
#include "fct.h"
#include "hud.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cctype>

namespace {

// PlayerClient (spawn) fields, see map.cpp.
const size_t kSpawnY = 0x74;
const size_t kSpawnX = 0x78;
const size_t kSpawnZ = 0x7c;
const size_t kSpawnSize = 0x154;     // float model height; -1 before the server sets it
const size_t kSpawnId = 0x168;       // u32
const size_t kSpawnMasterId = 0x2d8; // u32 pet owner spawn id

// OP_Damage packet as FUN_140216190 reads it.
const size_t kDmgTarget = 0x00;      // u16
const size_t kDmgSource = 0x02;      // u16
const size_t kDmgAmount = 0x08;      // int64
const size_t kDmgSpecial = 0x10;     // u32, bit 7 = critical
const size_t kDmgType = 0x28;        // u8
const uint32_t kSpecialCrit = 0x80;

// CEverQuest game state, as in hud.cpp.
const size_t kEverQuestGameState = 0x5e4;

// The EQGraphics.dll camera this was mapped on (PE stamp 0x68c31dea): CCameraInterface vtable and the
// first bytes of its WorldToScreen slot.
const size_t kCameraVtblRva = 0x2b2138;
const size_t kVtblWorldToScreen = 0xe8;
const uint8_t kWorldToScreenStart[] = {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x01,
};
// Camera fields WorldToScreen reads (dll-spec-w2s.md section 4).
const size_t kCamOrigin = 0xc0;      // float[3]
const size_t kCamRight = 0x70;       // float[3]
const size_t kCamUp = 0x7c;          // float[3]
const size_t kCamFwdX = 0x4c, kCamFwdY = 0x58, kCamFwdZ = 0x64;
const size_t kCamHalfW = 0x3c;       // float, viewport width / 2
const size_t kCamHalfH = 0x40;       // float
const size_t kCamWidth = 0xcc;       // u32 viewport width

using DamageFn = void (*)(uint8_t* pkt);
using GetSpawnByIdFn = uint8_t* (*)(uint8_t* mgr, uint32_t id);
using DistanceFn = float (*)(uint8_t* display, uint8_t* a, uint8_t* b, float zScale);
using WorldToScreenFn = bool (*)(uint8_t* cam, const float* world, float* sx, float* sy);

DamageFn g_origDamage = nullptr;      // trampoline into FUN_140216190
uint8_t** g_spawnMgr = nullptr;       // &DAT_140ee4b40
uint8_t** g_pLocalPlayer = nullptr;   // &DAT_140e66228, as the damage handler loads it
uint8_t** g_pDisplay = nullptr;       // &DAT_140e65d60, as the damage handler loads it
DistanceFn g_distance = nullptr;      // FUN_14019b2c0
uint8_t** g_pDisplayCheck = nullptr;  // &DAT_140e65d60 from the game time read, for the cross-check
int32_t* g_cameraOffsetAt = nullptr;  // the 0x118 inside `lea rdx,[rdi+118]` at the "Camera created." site
void** g_pRenderer = nullptr;         // &DAT_140f0e910
int32_t* g_viewport = nullptr;        // &DAT_140e6d8f0: x, y, w, h
uint8_t** g_pEverQuest = nullptr;     // &DAT_140ee7010 (from the Hud refs, via the game state site)

size_t g_cameraOffset = 0;
bool g_on = false;       // feature applied
bool g_shown = true;     // in-game toggle ([FCT] Show, flipped by ToggleKey)
char g_iniPath[MAX_PATH] = {};

// ---- configuration ([FCT] in dinput8.ini) ----

enum Category { kYou, kSelf, kOther, kCategoryCount };
enum TextMode { kTextOff = 0, kTextNumbers = 1, kTextNumbersAndWords = 2 };

struct CategoryConfig {
	bool on = true;
	int text = kTextNumbersAndWords;
	float scale = 1.25f;
	int colourIndex = 0;     // client user colour used when no Colour is set
	uint32_t colour = 0;     // 0 = use the client's user colour
};

struct Config {
	CategoryConfig cat[kCategoryCount];
	float critScale = 1.75f;
	uint32_t critColour = 0xFFFFE040;
	float minVelocity = 2.0f;
	float maxVelocity = 4.0f;
	float gravity = 2.5f;
	float minAngle = 75.0f;
	float maxAngle = 250.0f;
	float pixelsPerUnit = 30.0f;
	int lifeMs = 2500;
	int fadeMs = 500;
	float headHeight = 1.0f;
	float otherRange = 200.0f;
	int maxEntries = 32;
	int dedupMs = 100;
	bool outline = true;
	int toggleKey = 'F';     // virtual key; 0 = no hotkey
	bool toggleCtrl = true, toggleShift = true, toggleAlt = false;
};
Config g_cfg;

float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// "Ctrl+Shift+F", "Alt+F9", "none": modifiers plus one key (A-Z, 0-9, F1-F24). Unknown text = no hotkey.
void ParseHotkey(const char* text, Config* c) {
	c->toggleKey = 0;
	c->toggleCtrl = c->toggleShift = c->toggleAlt = false;
	char buf[64];
	strcpy_s(buf, text);
	if (char* comment = strchr(buf, ';')) *comment = '\0';   // GetPrivateProfileString keeps inline comments
	char* next = nullptr;
	for (char* part = strtok_s(buf, "+ ", &next); part; part = strtok_s(nullptr, "+ ", &next)) {
		if (_stricmp(part, "Ctrl") == 0) c->toggleCtrl = true;
		else if (_stricmp(part, "Shift") == 0) c->toggleShift = true;
		else if (_stricmp(part, "Alt") == 0) c->toggleAlt = true;
		else if (strlen(part) == 1 && isalnum(static_cast<unsigned char>(part[0]))) c->toggleKey = toupper(static_cast<unsigned char>(part[0]));
		else if ((part[0] == 'F' || part[0] == 'f') && atoi(part + 1) >= 1 && atoi(part + 1) <= 24) c->toggleKey = VK_F1 + atoi(part + 1) - 1;
		else {
			c->toggleKey = 0;
			return;
		}
	}
}

void LoadConfig() {
	char ini[MAX_PATH];
	GetModuleFileNameA(nullptr, ini, MAX_PATH);
	char* slash = strrchr(ini, '\\');
	if (slash) *slash = '\0';
	strcat_s(ini, "\\dinput8.ini");
	const char* s = "FCT";
	auto Int = [&](const char* key, int def) { return static_cast<int>(GetPrivateProfileIntA(s, key, def, ini)); };
	auto Float = [&](const char* key, float def) {
		char buf[32] = {};
		GetPrivateProfileStringA(s, key, "", buf, sizeof(buf), ini);
		char* end = nullptr;
		float v = strtof(buf, &end);
		return (buf[0] && end != buf && std::isfinite(v)) ? v : def;
	};
	auto Colour = [&](const char* key, uint32_t def) {
		char buf[32] = {};
		GetPrivateProfileStringA(s, key, "", buf, sizeof(buf), ini);
		if (!buf[0]) return def;
		return static_cast<uint32_t>(strtoul(buf, nullptr, 16)) | 0xFF000000u;
	};
	Config c;
	// The client's own chat colours for the three kinds of hit message (Options > Colors).
	c.cat[kYou].colourIndex = 0x109;    // your hits
	c.cat[kSelf].colourIndex = 0x10a;   // hits on you
	c.cat[kOther].colourIndex = 0x117;  // other people's hits
	static const char* const kNames[kCategoryCount] = { "You", "Self", "Other" };
	for (int i = 0; i < kCategoryCount; ++i) {
		char key[32];
		CategoryConfig& cc = c.cat[i];
		cc.on = Int(kNames[i], cc.on) != 0;
		snprintf(key, sizeof(key), "%sText", kNames[i]);
		cc.text = Clamp(Int(key, cc.text), kTextOff, kTextNumbersAndWords);
		snprintf(key, sizeof(key), "%sScale", kNames[i]);
		cc.scale = ClampF(Float(key, cc.scale), 0.0f, 8.0f);
		snprintf(key, sizeof(key), "%sColour", kNames[i]);
		cc.colour = Colour(key, cc.colour);
	}
	c.critScale = ClampF(Float("CritScale", c.critScale), 0.0f, 8.0f);
	c.critColour = Colour("CritColour", c.critColour);
	c.minVelocity = ClampF(Float("MinVelocity", c.minVelocity), 0.0f, 50.0f);
	c.maxVelocity = ClampF(Float("MaxVelocity", c.maxVelocity), c.minVelocity, 50.0f);
	c.gravity = ClampF(Float("Gravity", c.gravity), -50.0f, 50.0f);
	c.minAngle = ClampF(Float("MinAngle", c.minAngle), -360.0f, 720.0f);
	c.maxAngle = ClampF(Float("MaxAngle", c.maxAngle), c.minAngle, 720.0f);
	c.pixelsPerUnit = ClampF(Float("PixelsPerUnit", c.pixelsPerUnit), 0.0f, 500.0f);
	c.lifeMs = Clamp(Int("LifeMs", c.lifeMs), 200, 20000);
	c.fadeMs = Clamp(Int("FadeMs", c.fadeMs), 0, c.lifeMs);
	c.headHeight = ClampF(Float("HeadHeight", c.headHeight), -5.0f, 5.0f);
	c.otherRange = ClampF(Float("OtherRange", c.otherRange), 0.0f, 100000.0f);
	c.maxEntries = Clamp(Int("MaxEntries", c.maxEntries), 1, 64);
	c.dedupMs = Clamp(Int("DedupMs", c.dedupMs), 0, 2000);
	c.outline = Int("Outline", c.outline) != 0;
	char key[64] = {};
	GetPrivateProfileStringA(s, "ToggleKey", "Ctrl+Shift+F", key, sizeof(key), ini);
	ParseHotkey(key, &c);
	g_shown = Int("Show", 1) != 0;
	strcpy_s(g_iniPath, ini);
	g_cfg = c;
}

// ---- entries ----

const int kMaxEntries = 64;
const float kPi = 3.14159265f;

struct Entry {
	uint32_t spawnId;
	char text[24];
	uint32_t colour;   // opaque ARGB
	float scale;
	int64_t born;      // QPC ticks
	float vx, vy;      // initial velocity, pixels per second (screen y down)
	int width;         // cached text width for `font`, -1 until first drawn
	uint8_t* font;
};
Entry g_entries[kMaxEntries];
int g_count = 0;

struct Recent {
	int64_t when;
	uint16_t source, target;
	uint8_t type;
	int64_t damage;
};
Recent g_recent[16];
int g_recentNext = 0;

uint32_t g_rng = 0;

int64_t Now() {
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

int64_t Frequency() {
	static int64_t f = 0;
	if (!f) {
		LARGE_INTEGER q;
		QueryPerformanceFrequency(&q);
		f = q.QuadPart;
	}
	return f;
}

float Random01() {
	if (!g_rng) g_rng = static_cast<uint32_t>(Now()) | 1u;
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return static_cast<float>(g_rng >> 8) / 16777216.0f;
}

uint8_t* SpawnById(uint32_t id) {
	uint8_t* mgr = *g_spawnMgr;
	if (!mgr || !id) return nullptr;
	auto getById = reinterpret_cast<GetSpawnByIdFn>((*reinterpret_cast<void***>(mgr))[0x10 / sizeof(void*)]);
	return getById(mgr, id);
}

// Adds a floating text above `spawnId`, dropping the oldest entry when full. Also the entry point for
// future feeders (heals).
void AddEntry(uint32_t spawnId, const char* text, uint32_t colour, float scale) {
	if (g_count >= g_cfg.maxEntries) {
		int drop = g_count - g_cfg.maxEntries + 1;
		memmove(g_entries, g_entries + drop, sizeof(Entry) * (g_count - drop));
		g_count -= drop;
	}
	Entry& e = g_entries[g_count++];
	e.spawnId = spawnId;
	strncpy_s(e.text, text, _TRUNCATE);
	e.colour = colour;
	e.scale = scale;
	e.born = Now();
	float angle = (g_cfg.minAngle + (g_cfg.maxAngle - g_cfg.minAngle) * Random01()) * kPi / 180.0f;
	float speed = (g_cfg.minVelocity + (g_cfg.maxVelocity - g_cfg.minVelocity) * Random01()) * g_cfg.pixelsPerUnit;
	e.vx = cosf(angle) * speed;
	e.vy = -sinf(angle) * speed;
	e.width = -1;
	e.font = nullptr;
}

// True when the same hit was already seen within DedupMs (the pet owner's second copy).
bool IsDuplicate(uint16_t source, uint16_t target, int64_t damage, uint8_t type) {
	int64_t now = Now();
	int64_t window = Frequency() * g_cfg.dedupMs / 1000;
	for (Recent& r : g_recent) {
		if (r.when && now - r.when <= window && r.source == source && r.target == target && r.damage == damage &&
		    r.type == type) {
			r.when = 0;   // pair it off, so a third identical hit shows again
			return true;
		}
	}
	g_recent[g_recentNext] = { now, source, target, type, damage };
	g_recentNext = (g_recentNext + 1) % static_cast<int>(sizeof(g_recent) / sizeof(g_recent[0]));
	return false;
}

const char* MissWord(int64_t damage) {
	switch (damage) {
	case 0: return "Miss";
	case -1: return "Block";
	case -2: return "Parry";
	case -3: return "Riposte";
	case -4: return "Dodge";
	case -5: return "Invulnerable";
	case -6: return "Absorb";
	default: return nullptr;
	}
}

bool InGame() {
	uint8_t* eq = *g_pEverQuest;
	return eq && *reinterpret_cast<int*>(eq + kEverQuestGameState) == 5 && *g_pLocalPlayer;
}

void OnDamage(const uint8_t* pkt) {
	if (!InGame()) return;
	uint8_t* me = *g_pLocalPlayer;
	const uint16_t targetId = *reinterpret_cast<const uint16_t*>(pkt + kDmgTarget);
	const uint16_t sourceId = *reinterpret_cast<const uint16_t*>(pkt + kDmgSource);
	const int64_t damage = *reinterpret_cast<const int64_t*>(pkt + kDmgAmount);
	const uint32_t special = *reinterpret_cast<const uint32_t*>(pkt + kDmgSpecial);
	const uint8_t type = pkt[kDmgType];
	const uint32_t myId = *reinterpret_cast<uint32_t*>(me + kSpawnId);
	uint8_t* target = SpawnById(targetId);
	if (!target) return;
	uint8_t* source = sourceId ? SpawnById(sourceId) : nullptr;
	const bool sourceIsMyPet = source && sourceId != myId && *reinterpret_cast<uint32_t*>(source + kSpawnMasterId) == myId;
	const bool targetIsMyPet = targetId != myId && *reinterpret_cast<uint32_t*>(target + kSpawnMasterId) == myId;

	Category cat = kOther;
	if (targetId == myId) cat = kSelf;
	else if (sourceId == myId || sourceIsMyPet) cat = kYou;
	if (cat == kOther) {
		uint8_t* display = *g_pDisplay;
		if (!display || g_distance(display, me, target, 1.0f) > g_cfg.otherRange) return;
	}
	const CategoryConfig& cc = g_cfg.cat[cat];
	if (!cc.on || cc.text == kTextOff) return;

	char text[24];
	const bool crit = damage > 0 && (special & kSpecialCrit) != 0;
	if (damage > 0) {
		snprintf(text, sizeof(text), "%lld", static_cast<long long>(damage));
	}
	else {
		const char* word = MissWord(damage);
		if (!word || cc.text != kTextNumbersAndWords) return;
		strcpy_s(text, word);
	}
	if ((sourceIsMyPet || targetIsMyPet) && g_cfg.dedupMs > 0 && IsDuplicate(sourceId, targetId, damage, type)) return;

	uint32_t colour = cc.colour ? cc.colour : HudUserColour(cc.colourIndex);
	AddEntry(targetId, text, crit ? g_cfg.critColour : colour, crit ? g_cfg.critScale : cc.scale);
}

void DamageDetour(uint8_t* pkt) {
	g_origDamage(pkt);
	if (g_on && g_shown && pkt) OnDamage(pkt);
}

// ---- projection ----

enum Projection { kProjNone, kProjSlot, kProjManual };
uint8_t* g_checkedCam = nullptr;
void* g_checkedVtbl = nullptr;
Projection g_proj = kProjNone;

float CamF(const uint8_t* cam, size_t off) { return *reinterpret_cast<const float*>(cam + off); }

// The camera's WorldToScreen arithmetic (EQGraphics FUN_1800082e0), for when the slot does not match.
bool ManualWorldToScreen(const uint8_t* cam, const float* w, float* sx, float* sy) {
	float dx = w[0] - CamF(cam, kCamOrigin), dy = w[1] - CamF(cam, kCamOrigin + 4), dz = w[2] - CamF(cam, kCamOrigin + 8);
	float x = dx * CamF(cam, kCamRight) + dy * CamF(cam, kCamRight + 4) + dz * CamF(cam, kCamRight + 8);
	float y = dx * CamF(cam, kCamUp) + dy * CamF(cam, kCamUp + 4) + dz * CamF(cam, kCamUp + 8);
	float z = dx * CamF(cam, kCamFwdX) + dy * CamF(cam, kCamFwdY) + dz * CamF(cam, kCamFwdZ);
	float d = fabsf(z);
	if (d < 1.0f) d = 1.0f;
	*sx = CamF(cam, kCamHalfW) * x / d + CamF(cam, kCamHalfW);
	*sy = CamF(cam, kCamHalfH) - CamF(cam, kCamHalfH) * y / d;
	return z >= 0.0f;
}

// Chooses how to project with this camera; re-checked when the camera object or its vtable changes.
Projection CheckCamera(uint8_t* cam) {
	void* vtbl = *reinterpret_cast<void**>(cam);
	if (cam == g_checkedCam && vtbl == g_checkedVtbl) return g_proj;
	g_checkedCam = cam;
	g_checkedVtbl = vtbl;
	g_proj = kProjNone;
	HMODULE gfx = GetModuleHandleA("EQGraphics.dll");
	if (gfx && vtbl == reinterpret_cast<uint8_t*>(gfx) + kCameraVtblRva) {
		const void* fn = reinterpret_cast<void* const*>(vtbl)[kVtblWorldToScreen / sizeof(void*)];
		if (fn && memcmp(fn, kWorldToScreenStart, sizeof(kWorldToScreenStart)) == 0) return g_proj = kProjSlot;
	}
	const int width = g_viewport[2];
	if (width > 0 && *reinterpret_cast<const uint32_t*>(cam + kCamWidth) == static_cast<uint32_t>(width) &&
	    fabsf(CamF(cam, kCamHalfW) - width * 0.5f) < 0.5f)
		g_proj = kProjManual;
	return g_proj;
}

bool Project(uint8_t* cam, Projection proj, const float* world, float* sx, float* sy) {
	if (proj == kProjSlot) {
		auto w2s = reinterpret_cast<WorldToScreenFn>((*reinterpret_cast<void***>(cam))[kVtblWorldToScreen / sizeof(void*)]);
		if (!w2s(cam, world, sx, sy)) return false;
	}
	else if (!ManualWorldToScreen(cam, world, sx, sy)) {
		return false;
	}
	*sx += static_cast<float>(g_viewport[0]);
	*sy += static_cast<float>(g_viewport[1]);
	return std::isfinite(*sx) && std::isfinite(*sy);
}

// ---- drawing ----

// Font for a scale: 2 x scale rounded (1.0 = font 2, the Hud's font; each +0.5 one size up), capped at the
// largest UI font.
uint8_t* FontForScale(uint8_t* mgr, float scale) {
	int count = HudFontCount(mgr);
	if (count <= 0) return nullptr;
	int index = Clamp(static_cast<int>(lroundf(scale * 2.0f)), 0, count - 1);
	return HudFont(mgr, index);
}

void DrawEntries(uint8_t* mgr) {
	uint8_t* display = *g_pDisplay;
	uint8_t* cam = display ? *reinterpret_cast<uint8_t**>(display + g_cameraOffset) : nullptr;
	if (!cam || !*g_pRenderer || !*reinterpret_cast<void**>(cam)) {
		g_count = 0;
		return;
	}
	if (!g_count) return;
	const Projection proj = CheckCamera(cam);
	int screenH = 0;
	const int screenW = HudScreenSize(mgr, &screenH);
	const int clip[4] = { 0, 0, screenW, screenH };
	const int64_t now = Now();
	const float freq = static_cast<float>(Frequency());
	int kept = 0;
	for (int i = 0; i < g_count; ++i) {
		Entry& e = g_entries[i];
		const float t = static_cast<float>(now - e.born) / freq;
		const float life = g_cfg.lifeMs / 1000.0f;
		if (t >= life) continue;
		uint8_t* spawn = SpawnById(e.spawnId);
		if (!spawn) continue;
		g_entries[kept++] = e;
		Entry& k = g_entries[kept - 1];
		if (proj == kProjNone) continue;
		float size = *reinterpret_cast<float*>(spawn + kSpawnSize);
		if (!(size > 0.0f && size < 200.0f)) size = 6.0f;
		const float world[3] = {
			*reinterpret_cast<float*>(spawn + kSpawnY),
			*reinterpret_cast<float*>(spawn + kSpawnX),
			*reinterpret_cast<float*>(spawn + kSpawnZ) + size * g_cfg.headHeight,
		};
		float sx = 0.0f, sy = 0.0f;
		if (!Project(cam, proj, world, &sx, &sy)) continue;   // behind the camera
		sx += k.vx * t;
		sy += k.vy * t + 0.5f * g_cfg.gravity * g_cfg.pixelsPerUnit * t * t;
		if (!k.font) {
			k.font = FontForScale(mgr, k.scale);
			if (!k.font) continue;
		}
		if (k.width < 0) k.width = HudTextWidth(k.font, k.text);
		const int x = static_cast<int>(sx) - k.width / 2;
		const int y = static_cast<int>(sy) - HudLineHeight(k.font) / 2;
		if (x < 1 || y < 1 || x + k.width >= screenW || y >= screenH) continue;
		const float fade = g_cfg.fadeMs > 0 ? (life - t) / (g_cfg.fadeMs / 1000.0f) : 1.0f;
		const uint32_t alpha = static_cast<uint32_t>(ClampF(fade, 0.0f, 1.0f) * 255.0f) << 24;
		if (!alpha) continue;
		if (g_cfg.outline) {
			static const int kOffsets[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
			for (const auto& o : kOffsets) HudDrawText(k.font, k.text, x + o[0], y + o[1], clip, alpha);
		}
		HudDrawText(k.font, k.text, x, y, clip, alpha | (k.colour & 0x00FFFFFFu));
	}
	g_count = kept;
}

#define REF(var) reinterpret_cast<void**>(&var)

// OP_Damage handler FUN_140216190(pkt): mov [rsp+8],rsi; push rdi; sub rsp,30; movzx edx,[rcx+2];
// mov rdi,rcx; mov rax,[pLocalPlayer]; mov sil,1; mov ecx,[rax+168]; cmp edx,ecx; je.
// Stolen (5 bytes, position independent): mov [rsp+8],rsi.
const char* kDamageSig =
	"48 89 74 24 08 57 48 83 EC 30 0F B7 51 02 48 8B F9 48 8B 05 ?? ?? ?? ?? 40 B6 01 8B 88 68 01 00 00 3B D1 74 4C";

// Same function, the 200-unit gate for other people's hits: mov rcx,[spawnMgr]; mov edx,r8d; mov rax,[rcx];
// call [rax+10] (GetSpawnByID); movss xmm3,[1.0f]; mov r8,rax; mov rdx,[pLocalPlayer]; mov rcx,[pDisplay];
// call FUN_14019b2c0.
const char* kDamageGateSig =
	"48 8B 0D ?? ?? ?? ?? 41 8B D0 48 8B 01 FF 50 10 F3 0F 10 1D ?? ?? ?? ?? 4C 8B C0 "
	"48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";

} // namespace

const HookSite kFctHooks[2] = {
	// Shared with the Hud (same signature, detour and trampoline): whichever feature is applied first installs it.
	{ kDrawWindowsSig, 5, reinterpret_cast<void*>(&DrawWindowsDetour), reinterpret_cast<void**>(&g_origDrawWindows) },
	{ kDamageSig, 5, reinterpret_cast<void*>(&DamageDetour), REF(g_origDamage) },
};

const RefSite kFctRefs[9] = {
	{ kDamageGateSig, 3, 7, REF(g_spawnMgr) },
	{ kDamageGateSig, 30, 34, REF(g_pLocalPlayer) },
	{ kDamageGateSig, 37, 41, REF(g_pDisplay) },
	{ kDamageGateSig, 42, 46, REF(g_distance) },
	// Game time read: mov rax,[pDisplay]; mov ecx,[rax+16c]; mov [rbx+45c],ecx
	{ "48 8B 05 ?? ?? ?? ?? 8B 88 6C 01 00 00 89 8B 5C 04 00 00", 3, 7, REF(g_pDisplayCheck) },
	// "Camera created.": mov rcx,[scene]; lea rdx,[rdi+118] (rdi = CDisplay); the address of the imm32
	{ "48 8B 0D ?? ?? ?? ?? 48 8D 97 18 01 00 00", 10, 0, REF(g_cameraOffsetAt) },
	// Renderer: mov rcx,[renderer]; mov rax,[rcx]; xor edx,edx; call [rax+170]
	{ "48 8B 0D ?? ?? ?? ?? 48 8B 01 33 D2 FF 90 70 01 00 00", 3, 7, REF(g_pRenderer) },
	// Viewport: mov eax,[viewport.x]; lea rdx,[rbx+54a4]; movaps xmm6,[rsp+30]
	{ "8B 05 ?? ?? ?? ?? 48 8D 93 A4 54 00 00 0F 28 74 24 30", 2, 6, REF(g_viewport) },
	// mov rcx,[pEverQuest]; cmp dword [rcx+5e4],5 (the in-game test, as in hud.cpp)
	{ "48 8B 0D ?? ?? ?? ?? 83 B9 E4 05 00 00 05", 3, 7, REF(g_pEverQuest) },
};

const char* FctPrepare() {
	if (const char* e = HudTextPrepare()) return e;
	if (g_pLocalPlayer != HudLocalPlayerRef())
		return "the damage handler's local player is not the one the Hud refs resolved";
	if (g_pDisplay != g_pDisplayCheck)
		return "the damage handler's display global is not the one the game time read uses";
	// FUN_14019b2c0 starts with its null checks: test rdx,rdx; je; test r8,r8; je.
	static const uint8_t kDistanceStart[] = { 0x48, 0x85, 0xD2, 0x74, 0x5C, 0x4D, 0x85, 0xC0, 0x74, 0x57 };
	if (memcmp(reinterpret_cast<const void*>(g_distance), kDistanceStart, sizeof(kDistanceStart)) != 0)
		return "the damage handler's distance call is not the expected function";
	if (*g_cameraOffsetAt != 0x118) return "the camera is not at CDisplay+0x118";
	g_cameraOffset = static_cast<size_t>(*g_cameraOffsetAt);
	LoadConfig();
	g_on = true;
	return nullptr;
}

namespace {

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// The toggle hotkey, edge-triggered and only while the game window has focus. The new state is saved
// as [FCT] Show so it survives a restart.
int64_t g_toggleMessageUntil = 0;
bool g_toggleHeld = false;

void PollToggle() {
	if (!g_cfg.toggleKey) return;
	DWORD pid = 0;
	HWND fg = GetForegroundWindow();
	if (!fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId()) {
		g_toggleHeld = false;
		return;
	}
	bool down = KeyDown(g_cfg.toggleKey) && KeyDown(VK_CONTROL) == g_cfg.toggleCtrl &&
	            KeyDown(VK_SHIFT) == g_cfg.toggleShift && KeyDown(VK_MENU) == g_cfg.toggleAlt;
	if (down && !g_toggleHeld) {
		g_shown = !g_shown;
		g_count = 0;
		g_toggleMessageUntil = Now() + Frequency() * 2;
		if (g_iniPath[0]) WritePrivateProfileStringA("FCT", "Show", g_shown ? "1" : "0", g_iniPath);
	}
	g_toggleHeld = down;
}

// "Floating combat text: on/off" for two seconds after a toggle, centred near the top of the screen.
void DrawToggleMessage(uint8_t* mgr) {
	if (!g_toggleMessageUntil) return;
	if (Now() >= g_toggleMessageUntil) {
		g_toggleMessageUntil = 0;
		return;
	}
	uint8_t* font = HudFont(mgr, 3);
	if (!font) font = HudFont(mgr, 2);
	if (!font) return;
	int screenH = 0;
	const int screenW = HudScreenSize(mgr, &screenH);
	const int clip[4] = { 0, 0, screenW, screenH };
	const char* text = g_shown ? "Floating combat text: on" : "Floating combat text: off";
	const int x = (screenW - HudTextWidth(font, text)) / 2;
	const int y = screenH / 6;
	if (x < 1 || y < 1) return;
	HudDrawText(font, text, x + 1, y + 1, clip, 0xFF000000u);
	HudDrawText(font, text, x, y, clip, 0xFFFFFF80u);
}

} // namespace

void FctFrame(uint8_t* mgr, bool inGame) {
	if (!g_on) return;
	if (!inGame) {
		g_count = 0;
		g_toggleHeld = false;
		return;
	}
	PollToggle();
	if (g_shown) DrawEntries(mgr);
	DrawToggleMessage(mgr);
}
