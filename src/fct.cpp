// FloatingCombatText: damage numbers and miss words rising from characters and NPCs in the 3D world,
// matching the Legends client's floating combat text (analysis\legends\fct-legends-parity\behaviour-spec.md).
// Client-only, fed by the OP_Damage packets the server already sends.
//
// Events: detour on the OP_Damage handler FUN_140216190(pkt). The original runs first (HP, chat message),
// then we read the packet the way it does: target id u16 @0, source id u16 @2, damage int64 @8 (0 miss,
// -1 block, -2 parry, -3 riposte, -4 dodge, -5 invulnerable, -6 rune), special u32 @0x10 (bit 7 = crit),
// spell id int @0x14, type u8 @0x28 (0xE7 spell, below it a melee skill, 0xEC+ damage shield). Hits between
// two other spawns are shown only within OtherRange of the target, measured with the same function and
// arguments the handler uses for its own chat gate (FUN_14019b2c0(pDisplay, me, target, 1.0) = |dx|+|dy|+
// |dz|, compared with 200.0). Spawns come from the spawn manager's GetSpawnByID (vtbl+0x10), the call the
// handler itself makes.
//
// As in Legends: category Self = the target is me, else You = the attacker is me, else Other (so my pet's
// hits are Other). Per category a text mode (0 None, 1 Full, 2 Abbreviated, 3 DMG Only): numbers show in
// 1-3, words (misses, heal text) in Full and, cut to three characters, in Abbreviated; plain texts ignore
// the mode. Colours are the chat colours of the message the client prints (user colours through
// FUN_14050b6d0): hits 0x109 / 0x10a / 0x117, damage shields 0x11b, misses 0x10b / 0x10c / 0x118, Block the
// fixed white 10. A crit keeps its colour and doubles the category's font scale. Numbers live 4 s (crit
// 5 s), words 2 s, no fade. The pet/merc colour variants and the DoT green of the Legends builder are not
// ported (which case is which is not settled). Extras Legends does not have: the server sends a pet owner
// each pet hit twice, so an identical {source, target, damage, type} involving my pet within DedupMs is
// dropped; MaxEntries; a 1 px outline; FadeMs (0 = off); the toggle key.
// Other feeders (heals, Ding!, Out of Range, Fizzle, Missed Note, Resist, skill-up, loot/sell) are not
// hooked yet: their ToB handlers are not mapped. Each only has to call AddText() with its kind.
//
// Motion, in world space like Legends: the text starts at the spawn's head point {spawn+0x74, +0x78,
// +0x7c + size(+0x154) * HeadHeight} (memory order) moved 0.5 x size toward the camera, and does not follow
// the spawn. Direction = cameraRight x cos(A - 90) + cameraUp x sin(A - 90) for a random A in MinAngle..
// MaxAngle (0 down, 90 right, 180 up, 270 left), speed MinVelocity..MaxVelocity world units/s; fixed 1/60 s
// steps (at most 5 per frame): vz -= Gravity * dt, horizontal velocity x 0.98 per step, pos += v * dt. The
// camera axes are the rows of the camera's view matrix (the fields WorldToScreen reads). Not drawn beyond
// 200 units from the camera.
//
// Drawing, 3D route (default): the engine's own world string sprite, the class name tags use, so the text
// scales with distance like Legends'; see the "3D route" section below. 2D route (Render3D=0, or when a
// check of the engine fails):
// Drawing: from the CXWndManager::DrawWindows hook shared with the Hud (hud.cpp), after the client's
// windows, while in game. Each frame every entry is projected with the camera's WorldToScreen (EQGraphics
// CCameraInterface vtbl+0xe8; camera = *(pDisplay + 0x118), the offset read from the "Camera created."
// site). A point behind the camera is not drawn that frame. The camera's vtable and the slot's first bytes
// are checked against the EQGraphics.dll this was mapped on (vtable at +0x2b2138); otherwise the camera's
// own matrices are used (the same arithmetic as the slot) when the camera's viewport width matches, else
// nothing is drawn. Text goes through the Hud's helpers (CTextureFont::DrawWrappedText FUN_1405d1990),
// centred on the point; the UI font is FontStep x scale rounded (capped at the largest). Entries are dropped when their spawn is gone,
// and all are dropped when the client leaves the game state or the display, camera or renderer is missing.
// Settings: Legends' keys under [FloatingCombat] in dinput8.ini, the DLL extras under [FCT]. ToggleKey
// (default Ctrl+Shift+F, game window focused) flips Enabled and shows the new state for two seconds.
//
// FCTOptionsPage: Legends' "Combat Text" tab in the Options window (OptionsCombatTextPage from the patched
// EQUI_OptionsWindow.xml; without it nothing happens). COptionsWnd (g_pOptionsWnd DAT_140db5348, 0x28d0
// bytes); the page and its controls are found with CXWnd::GetChildItem (FUN_14059f8a0, log = 0, silent),
// once per COptionsWnd instance (a UI reload builds a new one). Hooks: RefreshDisplayPage FUN_14049db70 (end
// of the constructor and every 2 s on the Display tab: resolve and fill the page the first time) and
// COptionsWnd::WndNotification FUN_14049f110 (our page is unknown to the client's page router, so its
// messages arrive here): tab box (+0x2d8) msg 1 with data+8 = our page -> fill the controls from the
// settings (the client never refreshes an unknown page); checkbox msg 1 (already toggled), slider msg 0xe
// (data = new position), combobox msg 1 (data = new row) -> update the setting, save it, return 0 as the
// client's page handlers do. Checkboxes go through their own vtable (+0x348 IsChecked, +0x380 SetCheck),
// used only when it is ODP_NPCNamesCheckbox's (+0x508) or the two slots start with the expected bytes.
// Sliders: SetNumTicks FUN_1405ba8a0 (100), SetValue FUN_1405baa20, value = min + pos/100 x (max - min);
// value labels via SetWindowText (vtbl +0x280). Comboboxes: SetChoice FUN_1405ae150.
#include "fct.h"
#include "hud.h"
#include "nameplates.h"
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
const uint8_t kTypeSpell = 0xE7;           // below: a melee skill
const uint8_t kTypeDamageShield = 0xEC;

// CEverQuest game state, as in hud.cpp.
const size_t kEverQuestGameState = 0x5e4;

// The EQGraphics.dll camera this was mapped on (PE stamp 0x68c31dea): CCameraInterface vtable and the
// first bytes of its WorldToScreen slot.
const size_t kCameraVtblRva = 0x2b2138;
const size_t kVtblWorldToScreen = 0xe8;
const uint8_t kWorldToScreenStart[] = {
	0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x01,
};
// Camera fields WorldToScreen reads (dll-spec-w2s.md section 4): origin and the view matrix rows.
const size_t kCamOrigin = 0xc0;      // float[3]
const size_t kCamRight = 0x70;       // float[3]
const size_t kCamUp = 0x7c;          // float[3]
const size_t kCamFwdX = 0x4c, kCamFwdY = 0x58, kCamFwdZ = 0x64;
const size_t kCamHalfW = 0x3c;       // float, viewport width / 2
const size_t kCamHalfH = 0x40;       // float
const size_t kCamWidth = 0xcc;       // u32 viewport width

// User colour indices (the chat colours of the client's hit and miss messages) and fixed colours.
const int kColourYouHit = 0x109, kColourSelfHit = 0x10a, kColourOtherHit = 0x117, kColourNonMelee = 0x11b;
const int kColourYouMiss = 0x10b, kColourSelfMiss = 0x10c, kColourOtherMiss = 0x118;
const int kColourWhite = 10;         // Block

const float kStep = 1.0f / 60.0f;    // Legends' fixed animation step
const int kMaxStepsPerFrame = 5;
const float kDrag = 0.98f;           // horizontal velocity factor per step
const float kMaxDrawDistance = 200.0f;

using DamageFn = void (*)(uint8_t* pkt);
using GetSpawnByIdFn = uint8_t* (*)(uint8_t* mgr, uint32_t id);
using DistanceFn = float (*)(uint8_t* display, uint8_t* a, uint8_t* b, float zScale);
using WorldToScreenFn = bool (*)(uint8_t* cam, const float* world, float* sx, float* sy);

DamageFn g_origDamage = nullptr;      // trampoline into FUN_140216190
uint8_t** g_spawnMgr = nullptr;       // &DAT_140ee4b40
uint8_t** g_pLocalPlayer = nullptr;   // &DAT_140e66228, as the damage handler loads it
uint8_t** g_pDisplay = nullptr;       // &DAT_140e65d60, as the damage handler loads it
DistanceFn g_distance = nullptr;      // FUN_14019b2c0
// The Chat tab's Show / Hide: the chat manager FUN_1401093c0() and its "is this colour shown" FUN_140108b00(mgr,
// colour) (filter value != 1 for the filter the colour maps to), the check the damage message makes before printing.
using ChatMgrFn = uint8_t* (*)();
using ChatShowsFn = bool (*)(uint8_t* mgr, int colour);
ChatMgrFn g_chatMgr = nullptr;
ChatShowsFn g_chatShows = nullptr;
uint8_t** g_pDisplayCheck = nullptr;  // &DAT_140e65d60 from the game time read, for the cross-check
int32_t* g_cameraOffsetAt = nullptr;  // the 0x118 inside `lea rdx,[rdi+118]` at the "Camera created." site
void** g_pRenderer = nullptr;         // &DAT_140f0e910
int32_t* g_viewport = nullptr;        // &DAT_140e6d8f0: x, y, w, h
uint8_t** g_pEverQuest = nullptr;     // &DAT_140ee7010

size_t g_cameraOffset = 0;
bool g_on = false;       // FloatingCombatText applied
char g_iniPath[MAX_PATH] = {};

// ---- configuration ----

enum Category { kYou, kSelf, kOther, kCategoryCount };          // the order of Legends' three tabs
enum TextMode { kModeNone = 0, kModeFull = 1, kModeAbbreviated = 2, kModeDamageOnly = 3 };
enum Kind { kNumber, kWord, kPlain };                           // what the text mode applies to
// Legends' float option indices.
enum FloatOption { kMinVelocity, kMaxVelocity, kGravity, kMinAngle, kMaxAngle, kYouFontScale, kSelfFontScale,
                   kOtherFontScale, kFloatCount };

struct FloatInfo {
	const char* key;
	float def, min, max;
};
const FloatInfo kFloats[kFloatCount] = {
	{ "MinVelocity", 2.0f, 0.0f, 50.0f },
	{ "MaxVelocity", 4.0f, 0.0f, 50.0f },
	{ "Gravity", 2.5f, 0.0f, 50.0f },
	{ "MinAngle", 75.0f, 0.0f, 359.0f },
	{ "MaxAngle", 250.0f, 0.0f, 359.0f },
	{ "YouFontScale", 1.25f, 0.1f, 2.0f },
	{ "SelfFontScale", 1.25f, 0.1f, 2.0f },
	{ "OtherFontScale", 1.25f, 0.1f, 2.0f },
};
const char* const kHitTextKeys[kCategoryCount] = { "YouHitText", "SelfHitText", "OtherHitText" };
const char* const kSection = "FloatingCombat";   // Legends' keys
const char* const kExtras = "FCT";               // DLL extras

struct Config {
	// Legends
	bool enabled = true;
	bool sellText = true;
	bool lootText = true;
	float f[kFloatCount] = {};
	int hitText[kCategoryCount] = { kModeFull, kModeFull, kModeFull };
	// extras
	uint32_t colour[kCategoryCount] = {};   // 0 = the client's chat colour
	int fadeMs = 0;
	float headHeight = 1.0f;
	float fontStep = 4.0f;                  // 2D route: UI font index per unit of font scale
	bool render3D = true;                   // world text through the engine's string sprite when its checks pass
	float letterScale = 0.4f;               // 3D: sprite letter scale per unit of font scale (Legends mapping)
	int minPx = 12;                         // 3D: minimum text height in pixels (Legends)
	bool occlude = false;                   // 3D: false = drawn over walls (Legends' default)
	bool followChatFilters = true;          // a hit / miss hidden in the Chat tab gets no floating text either
	bool valueLabels = false;               // SliderLabels=value: the DLL-only sliders show their value, not N%
	float otherRange = 200.0f;
	int maxEntries = 32;
	int dedupMs = 100;
	bool outline = true;
	int toggleKey = 'F';                    // virtual key; 0 = no hotkey
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
	strcpy_s(g_iniPath, ini);
	auto Int = [&](const char* s, const char* key, int def) { return static_cast<int>(GetPrivateProfileIntA(s, key, def, ini)); };
	auto Float = [&](const char* s, const char* key, float def) {
		char buf[32] = {};
		GetPrivateProfileStringA(s, key, "", buf, sizeof(buf), ini);
		char* end = nullptr;
		float v = strtof(buf, &end);
		return (buf[0] && end != buf && std::isfinite(v)) ? v : def;
	};
	auto Colour = [&](const char* s, const char* key) {
		char buf[32] = {};
		GetPrivateProfileStringA(s, key, "", buf, sizeof(buf), ini);
		char* end = nullptr;
		unsigned long v = strtoul(buf, &end, 16);
		return end != buf ? static_cast<uint32_t>(v) | 0xFF000000u : 0u;
	};
	Config c;
	c.enabled = Int(kSection, "Enabled", c.enabled) != 0;
	c.sellText = Int(kSection, "SellText", c.sellText) != 0;
	c.lootText = Int(kSection, "LootText", c.lootText) != 0;
	for (int i = 0; i < kFloatCount; ++i) c.f[i] = ClampF(Float(kSection, kFloats[i].key, kFloats[i].def), kFloats[i].min, kFloats[i].max);
	for (int i = 0; i < kCategoryCount; ++i) c.hitText[i] = Clamp(Int(kSection, kHitTextKeys[i], c.hitText[i]), kModeNone, kModeDamageOnly);
	c.colour[kYou] = Colour(kExtras, "YouColour");
	c.colour[kSelf] = Colour(kExtras, "SelfColour");
	c.colour[kOther] = Colour(kExtras, "OtherColour");
	c.fadeMs = Clamp(Int(kExtras, "FadeMs", c.fadeMs), 0, 5000);
	c.headHeight = ClampF(Float(kExtras, "HeadHeight", c.headHeight), -5.0f, 5.0f);
	c.otherRange = ClampF(Float(kExtras, "OtherRange", c.otherRange), 0.0f, 100000.0f);
	c.maxEntries = Clamp(Int(kExtras, "MaxEntries", c.maxEntries), 1, 64);
	c.dedupMs = Clamp(Int(kExtras, "DedupMs", c.dedupMs), 0, 2000);
	c.outline = Int(kExtras, "Outline", c.outline) != 0;
	c.fontStep = ClampF(Float(kExtras, "FontStep", c.fontStep), 0.5f, 16.0f);
	c.render3D = Int(kExtras, "Render3D", c.render3D) != 0;
	c.letterScale = ClampF(Float(kExtras, "LetterScale", c.letterScale), 0.01f, 10.0f);
	c.minPx = Clamp(Int(kExtras, "MinPx", c.minPx), 0, 200);
	c.occlude = Int(kExtras, "Occlude", c.occlude) != 0;
	c.followChatFilters = Int(kExtras, "FollowChatFilters", c.followChatFilters) != 0;
	char labels[16] = {};
	GetPrivateProfileStringA(kExtras, "SliderLabels", "percent", labels, sizeof(labels), ini);
	c.valueLabels = _stricmp(labels, "value") == 0;
	char key[64] = {};
	GetPrivateProfileStringA(kExtras, "ToggleKey", "Ctrl+Shift+F", key, sizeof(key), ini);
	ParseHotkey(key, &c);
	g_cfg = c;
}

// Writes Legends' settings back to [FloatingCombat] (the options page and the toggle key change them).
void SaveConfig() {
	if (!g_iniPath[0]) return;
	char buf[32];
	WritePrivateProfileStringA(kSection, "Enabled", g_cfg.enabled ? "1" : "0", g_iniPath);
	WritePrivateProfileStringA(kSection, "SellText", g_cfg.sellText ? "1" : "0", g_iniPath);
	WritePrivateProfileStringA(kSection, "LootText", g_cfg.lootText ? "1" : "0", g_iniPath);
	for (int i = 0; i < kFloatCount; ++i) {
		snprintf(buf, sizeof(buf), "%.2f", g_cfg.f[i]);
		WritePrivateProfileStringA(kSection, kFloats[i].key, buf, g_iniPath);
	}
	for (int i = 0; i < kCategoryCount; ++i) {
		snprintf(buf, sizeof(buf), "%d", g_cfg.hitText[i]);
		WritePrivateProfileStringA(kSection, kHitTextKeys[i], buf, g_iniPath);
	}
}

// ---- entries ----

const int kMaxEntries = 64;
const float kPi = 3.14159265f;

struct Entry {
	uint32_t spawnId;
	char text[48];
	uint32_t colour;   // opaque ARGB
	int category;      // font scale group
	bool crit;         // x2 font scale
	int64_t born;      // QPC ticks
	float life;        // seconds
	float pos[3];      // world, spawn memory order
	float vel[3];      // world units per second
	float pending;     // time not yet integrated
	int width;         // text width for `font` (2D route)
	uint8_t* font;
	int sprite;        // 3D route: pool slot, -1 = none
};
Entry g_entries[kMaxEntries];
int g_count = 0;
int64_t g_lastFrame = 0;

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

float RandomBetween(float a, float b) {
	return a + (b - a) * Random01();
}

uint8_t* SpawnById(uint32_t id) {
	uint8_t* mgr = *g_spawnMgr;
	if (!mgr || !id) return nullptr;
	auto getById = reinterpret_cast<GetSpawnByIdFn>((*reinterpret_cast<void***>(mgr))[0x10 / sizeof(void*)]);
	return getById(mgr, id);
}

uint8_t* CurrentCamera() {
	uint8_t* display = *g_pDisplay;
	uint8_t* cam = display ? *reinterpret_cast<uint8_t**>(display + g_cameraOffset) : nullptr;
	return (cam && *reinterpret_cast<void**>(cam)) ? cam : nullptr;
}

float CamF(const uint8_t* cam, size_t off) { return *reinterpret_cast<const float*>(cam + off); }

// A camera axis (a view matrix row), normalised; false when degenerate.
bool CamAxis(const uint8_t* cam, size_t x, size_t y, size_t z, float* out) {
	out[0] = CamF(cam, x);
	out[1] = CamF(cam, y);
	out[2] = CamF(cam, z);
	float len = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
	if (!(len > 1e-4f) || !std::isfinite(len)) return false;
	for (int i = 0; i < 3; ++i) out[i] /= len;
	return true;
}

// ---- 3D route: the engine's world string sprite (EQGraphics CStringSprite, the name-tag class) ----
//
// analysis\legends\fct-3dtext\dll-spec.md. A pool of sprites in our own memory, constructed once when the
// EQGraphics.dll functions and vtables check out. Per entry: SetText/SetColor once; per frame the world
// position (+0x98, spawn memory order) and letter scale (+0x94), then Draw, which queues the text into the
// engine's name-tag text list; renderer slot +0x158 draws it at the end of this frame's 3D pass. A text
// smaller than MinPx gets a second Draw with a scaled-up letter scale; without Occlude the depth (+0x58) is
// replaced after Draw so the text sits over walls. A free sprite holds "" (Draw returns -1). If a check
// fails, or no sprite produces a rect for two seconds while texts exist, the 2D route takes over.

const size_t kSpriteSize = 0xc0;
const size_t kSpriteVtblRva = 0x2c5498;
const size_t kSpriteTextItemVtblRva = 0x2c5240;   // vtable of the text item at sprite+8
const size_t kRendererVtblRva = 0x2c0830;
const size_t kRendererEndScene3D = 0x158, kRendererEndScene3DRva = 0xd7820;
const size_t kSpriteRect = 0x1c;                  // int l, t, r, b (viewport px) after Draw
const size_t kSpriteDepth = 0x58;                 // float, written into the vertices
const size_t kSpriteBoneOffset = 0x8c;            // float, vertical offset (world units)
const size_t kSpriteScale = 0x90;                 // float
const size_t kSpriteLetterScale = 0x94;           // float
const size_t kSpritePos = 0x98;                   // float[3]
const float kDepthOnTop = 1e-4f;

using SpriteCtorFn = void* (*)(void* mem);
using SpriteSetTextFn = void (*)(void* s, const char* text, const char* text2);
using SpriteSetColorFn = void (*)(void* s, const uint32_t* rgb);
using SpriteSetFadeFn = void (*)(void* s, float maxDist, float nearDist, float farBand, float minAlpha);
using SpriteDrawFn = int (*)(void* s);

struct EngineFn {
	size_t rva;
	uint8_t start[16];
	size_t len;
	void** target;
};
SpriteCtorFn g_spriteCtor = nullptr;
SpriteSetTextFn g_spriteSetText = nullptr;
SpriteSetColorFn g_spriteSetColor = nullptr;
SpriteSetFadeFn g_spriteSetFade = nullptr;
SpriteDrawFn g_spriteDraw = nullptr;
const EngineFn kEngineFns[] = {
	{ 0xfcff0, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8 }, 10, reinterpret_cast<void**>(&g_spriteCtor) },
	{ 0xfd890, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xD2, 0x74 }, 13, reinterpret_cast<void**>(&g_spriteSetText) },
	{ 0xfd880, { 0x8B, 0x02, 0x89, 0x81, 0x80, 0x00, 0x00, 0x00, 0xC3 }, 9, reinterpret_cast<void**>(&g_spriteSetColor) },
	{ 0xfd840, { 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x28, 0xF3, 0x0F, 0x11, 0x89, 0xA4, 0x00, 0x00, 0x00 }, 14, reinterpret_cast<void**>(&g_spriteSetFade) },
	{ 0xfd300, { 0x40, 0x53, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00 }, 12, reinterpret_cast<void**>(&g_spriteDraw) },
};

const int kPoolSize = 64;   // >= MaxEntries
struct Sprite3D {
	alignas(16) uint8_t mem[kSpriteSize];
	bool used;
};
Sprite3D g_pool[kPoolSize];
enum Route3D { k3DUnchecked, k3DReady, k3DOff };
Route3D g_route3D = k3DUnchecked;
int64_t g_3dNoRectSince = 0;    // first frame with live 3D texts but no drawn rect
bool g_3dRectSeen = false;

void LogMessage(const char* text) {
	if (!g_iniPath[0]) return;
	char path[MAX_PATH];
	strcpy_s(path, g_iniPath);
	char* slash = strrchr(path, '\\');
	if (!slash) return;
	strcpy_s(slash + 1, static_cast<size_t>(path + sizeof(path) - (slash + 1)), "dinput8.log");
	FILE* f = nullptr;
	if (fopen_s(&f, path, "a") != 0 || !f) return;
	fprintf(f, "FloatingCombatText: %s\n", text);
	fclose(f);
}

void Disable3D(const char* why) {
	g_route3D = k3DOff;
	char text[160];
	snprintf(text, sizeof(text), "3D text off, using 2D UI fonts (%s)", why);
	LogMessage(text);
}

// Checks EQGraphics.dll once and builds the pool. Only from the draw hook (the module is loaded by then).
bool Ready3D() {
	if (g_route3D != k3DUnchecked) return g_route3D == k3DReady;
	if (!g_cfg.render3D) {
		g_route3D = k3DOff;
		return false;
	}
	uint8_t* gfx = reinterpret_cast<uint8_t*>(GetModuleHandleA("EQGraphics.dll"));
	if (!gfx) {
		Disable3D("EQGraphics.dll not loaded");
		return false;
	}
	for (const EngineFn& fn : kEngineFns) {
		if (memcmp(gfx + fn.rva, fn.start, fn.len) != 0) {
			Disable3D("an EQGraphics function does not match this build");
			return false;
		}
		*fn.target = gfx + fn.rva;
	}
	void** rVt = *g_pRenderer ? *reinterpret_cast<void***>(*g_pRenderer) : nullptr;
	if (!rVt || reinterpret_cast<uint8_t*>(rVt) != gfx + kRendererVtblRva ||
	    reinterpret_cast<uint8_t*>(rVt[kRendererEndScene3D / sizeof(void*)]) != gfx + kRendererEndScene3DRva) {
		Disable3D("the renderer is not the expected class");
		return false;
	}
	for (int i = 0; i < kPoolSize; ++i) {
		uint8_t* s = g_pool[i].mem;
		g_spriteCtor(s);
		if (i == 0 && (*reinterpret_cast<uint8_t**>(s) != gfx + kSpriteVtblRva ||
		               *reinterpret_cast<uint8_t**>(s + 8) != gfx + kSpriteTextItemVtblRva)) {
			Disable3D("the string sprite is not the expected class");   // nothing linked yet that we must undo
			return false;
		}
		g_spriteSetFade(s, 400.0f, 0.001f, 0.001f, 1.0f);   // full alpha everywhere, 1 + d/400 size compensation
		*reinterpret_cast<float*>(s + kSpriteBoneOffset) = 0.0f;
		*reinterpret_cast<float*>(s + kSpriteScale) = 1.0f;
		g_pool[i].used = false;
	}
	g_route3D = k3DReady;
	LogMessage("3D text on (engine string sprites)");
	return true;
}

int AcquireSprite(const char* text, uint32_t colour) {
	for (int i = 0; i < kPoolSize; ++i) {
		if (g_pool[i].used) continue;
		g_pool[i].used = true;
		g_spriteSetText(g_pool[i].mem, text, text);
		const uint32_t rgb = colour & 0x00FFFFFFu;
		g_spriteSetColor(g_pool[i].mem, &rgb);
		return i;
	}
	return -1;
}

void ReleaseSprite(Entry& e) {
	if (e.sprite < 0) return;
	if (g_route3D == k3DReady) g_spriteSetText(g_pool[e.sprite].mem, "", "");
	g_pool[e.sprite].used = false;
	e.sprite = -1;
}

void ClearEntries() {
	for (int i = 0; i < g_count; ++i) ReleaseSprite(g_entries[i]);
	g_count = 0;
}

// Queues one entry's text for this frame's name-tag pass.
void Draw3D(Entry& e) {
	uint8_t* s = g_pool[e.sprite].mem;
	float* pos = reinterpret_cast<float*>(s + kSpritePos);
	pos[0] = e.pos[0];
	pos[1] = e.pos[1];
	pos[2] = e.pos[2];
	const float letter = g_cfg.letterScale * g_cfg.f[kYouFontScale + e.category] * (e.crit ? 2.0f : 1.0f);
	*reinterpret_cast<float*>(s + kSpriteLetterScale) = letter;
	if (g_spriteDraw(s) != 0) return;
	const int* rect = reinterpret_cast<const int*>(s + kSpriteRect);
	const int h = rect[3] - rect[1];
	if (rect[2] > rect[0] && h > 0) g_3dRectSeen = true;
	if (h > 0 && h < g_cfg.minPx) {
		*reinterpret_cast<float*>(s + kSpriteLetterScale) = letter * static_cast<float>(g_cfg.minPx) / static_cast<float>(h);
		g_spriteDraw(s);
	}
	if (!g_cfg.occlude) *reinterpret_cast<float*>(s + kSpriteDepth) = kDepthOnTop;
}

// Adds a text above `spawn`, dropping the oldest entry when full. The entry point for every feeder.
// Numbers and words obey the category's text mode; plain texts only Enabled.
void AddText(uint8_t* spawn, const char* text, uint32_t colour, int category, bool crit, int lifeMs, Kind kind) {
	if (!g_cfg.enabled || !spawn) return;
	const int mode = g_cfg.hitText[category];
	char shown[48];
	strncpy_s(shown, text, _TRUNCATE);
	if (kind == kNumber && mode == kModeNone) return;
	if (kind == kWord) {
		if (mode == kModeNone || mode == kModeDamageOnly) return;
		if (mode == kModeAbbreviated) shown[3] = '\0';
	}
	uint8_t* cam = CurrentCamera();
	float right[3], up[3], fwd[3];
	if (!cam || !CamAxis(cam, kCamRight, kCamRight + 4, kCamRight + 8, right) ||
	    !CamAxis(cam, kCamUp, kCamUp + 4, kCamUp + 8, up) || !CamAxis(cam, kCamFwdX, kCamFwdY, kCamFwdZ, fwd))
		return;
	if (g_count >= g_cfg.maxEntries) {
		int drop = g_count - g_cfg.maxEntries + 1;
		for (int i = 0; i < drop; ++i) ReleaseSprite(g_entries[i]);
		memmove(g_entries, g_entries + drop, sizeof(Entry) * (g_count - drop));
		g_count -= drop;
	}
	Entry& e = g_entries[g_count++];
	e.spawnId = *reinterpret_cast<uint32_t*>(spawn + kSpawnId);
	strcpy_s(e.text, shown);
	e.colour = colour;
	e.category = category;
	e.crit = crit;
	e.born = Now();
	e.life = lifeMs / 1000.0f;
	float size = *reinterpret_cast<float*>(spawn + kSpawnSize);
	if (!(size > 0.0f && size < 200.0f)) size = 6.0f;
	e.pos[0] = *reinterpret_cast<float*>(spawn + kSpawnY);
	e.pos[1] = *reinterpret_cast<float*>(spawn + kSpawnX);
	e.pos[2] = *reinterpret_cast<float*>(spawn + kSpawnZ) + size * g_cfg.headHeight;
	for (int i = 0; i < 3; ++i) e.pos[i] -= fwd[i] * size * 0.5f;   // toward the camera
	const float* f = g_cfg.f;
	const float a = (RandomBetween(f[kMinAngle], f[kMaxAngle]) - 90.0f) * kPi / 180.0f;
	const float speed = RandomBetween(f[kMinVelocity], f[kMaxVelocity]);
	for (int i = 0; i < 3; ++i) e.vel[i] = (right[i] * cosf(a) + up[i] * sinf(a)) * speed;
	e.pending = 0.0f;
	e.width = -1;
	e.font = nullptr;
	e.sprite = g_route3D == k3DReady ? AcquireSprite(e.text, colour) : -1;
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

// Legends' words (eqstr 15557-15564).
const char* MissWord(int64_t damage) {
	switch (damage) {
	case 0: return "Miss";
	case -1: return "Block";
	case -2: return "Parry";
	case -3: return "Riposte";
	case -4: return "Dodge";
	case -5: return "INVULNERABLE";
	case -6: return "Rune";
	default: return nullptr;
	}
}

bool InGame() {
	uint8_t* eq = *g_pEverQuest;
	return eq && *reinterpret_cast<int*>(eq + kEverQuestGameState) == 5 && *g_pLocalPlayer;
}

// The client prints this chat colour (Chat tab filter not set to Hide).
bool ChatShows(int colour) {
	if (!g_cfg.followChatFilters) return true;
	uint8_t* mgr = g_chatMgr();
	return !mgr || g_chatShows(mgr, colour);
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
	else if (sourceId == myId) cat = kYou;
	if (cat == kOther) {
		uint8_t* display = *g_pDisplay;
		if (!display || g_distance(display, me, target, 1.0f) > g_cfg.otherRange) return;
	}
	if ((sourceIsMyPet || targetIsMyPet) && g_cfg.dedupMs > 0 && IsDuplicate(sourceId, targetId, damage, type)) return;

	if (damage > 0) {
		static const int kHit[kCategoryCount] = { kColourYouHit, kColourSelfHit, kColourOtherHit };
		const bool crit = (special & kSpecialCrit) != 0;
		const int chat = type >= kTypeDamageShield ? kColourNonMelee : kHit[cat];
		if (!ChatShows(chat)) return;
		uint32_t colour = g_cfg.colour[cat] ? g_cfg.colour[cat] : HudUserColour(chat);
		char text[24];
		snprintf(text, sizeof(text), "%lld", static_cast<long long>(damage));
		AddText(target, text, colour, cat, crit, crit ? 5000 : 4000, kNumber);
		return;
	}
	// Spells (beneficial ones land as damage 0, type 0xE7) and damage shields have no miss: the client prints none.
	if (type >= kTypeSpell) return;
	const char* word = MissWord(damage);
	if (!word) return;
	static const int kMiss[kCategoryCount] = { kColourYouMiss, kColourSelfMiss, kColourOtherMiss };
	if (!ChatShows(kMiss[cat])) return;
	uint32_t colour = HudUserColour(damage == -1 ? kColourWhite : kMiss[cat]);
	AddText(target, word, colour, cat, false, 2000, kWord);
}

void DamageDetour(uint8_t* pkt) {
	g_origDamage(pkt);
	if (g_on && pkt) OnDamage(pkt);
	if (pkt) NameplatesOnDamage(pkt);
}

// ---- projection ----

enum Projection { kProjNone, kProjSlot, kProjManual };
uint8_t* g_checkedCam = nullptr;
void* g_checkedVtbl = nullptr;
Projection g_proj = kProjNone;

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

// Font for a scale: FontStep x scale rounded, capped at the largest UI font. The UI fonts have fixed sizes, so
// this stands in for Legends' perspective text. The first time, the font count and heights go to dinput8.log.
void LogFonts(uint8_t* mgr) {
	static bool logged = false;
	if (logged || !g_iniPath[0]) return;
	logged = true;
	char path[MAX_PATH];
	strcpy_s(path, g_iniPath);
	char* slash = strrchr(path, '\\');
	if (!slash) return;
	strcpy_s(slash + 1, static_cast<size_t>(path + sizeof(path) - (slash + 1)), "dinput8.log");
	FILE* f = nullptr;
	if (fopen_s(&f, path, "a") != 0 || !f) return;
	const int count = HudFontCount(mgr);
	fprintf(f, "FloatingCombatText: %d UI fonts, line heights:", count);
	for (int i = 0; i < count && i < 32; ++i) {
		uint8_t* font = HudFont(mgr, i);
		fprintf(f, " %d=%d", i, font ? HudLineHeight(font) : 0);
	}
	fputc('\n', f);
	fclose(f);
}

uint8_t* FontForScale(uint8_t* mgr, float scale) {
	int count = HudFontCount(mgr);
	if (count <= 0) return nullptr;
	int index = Clamp(static_cast<int>(lroundf(scale * g_cfg.fontStep)), 0, count - 1);
	return HudFont(mgr, index);
}

// Legends' fixed-step motion: gravity on world Z, drag on the horizontal velocity.
void Step(Entry& e, float dt) {
	e.pending += dt;
	int steps = 0;
	while (e.pending >= kStep && steps < kMaxStepsPerFrame) {
		e.vel[2] -= g_cfg.f[kGravity] * kStep;
		e.vel[0] *= kDrag;
		e.vel[1] *= kDrag;
		for (int i = 0; i < 3; ++i) e.pos[i] += e.vel[i] * kStep;
		e.pending -= kStep;
		++steps;
	}
	if (e.pending > kStep) e.pending = kStep;   // a long frame does not catch up beyond five steps
}

void DrawEntries(uint8_t* mgr) {
	const int64_t now = Now();
	const float freq = static_cast<float>(Frequency());
	const float dt = g_lastFrame ? static_cast<float>(now - g_lastFrame) / freq : 0.0f;
	g_lastFrame = now;
	uint8_t* cam = CurrentCamera();
	if (!cam || !*g_pRenderer) {
		ClearEntries();
		return;
	}
	const bool use3D = Ready3D();
	if (!g_count) return;
	const Projection proj = CheckCamera(cam);
	if (!use3D) LogFonts(mgr);
	int screenH = 0;
	const int screenW = HudScreenSize(mgr, &screenH);
	const int clip[4] = { 0, 0, screenW, screenH };
	const float origin[3] = { CamF(cam, kCamOrigin), CamF(cam, kCamOrigin + 4), CamF(cam, kCamOrigin + 8) };
	int kept = 0;
	for (int i = 0; i < g_count; ++i) {
		Entry e = g_entries[i];
		const float t = static_cast<float>(now - e.born) / freq;
		if (t >= e.life || !SpawnById(e.spawnId)) {
			ReleaseSprite(e);
			continue;
		}
		Step(e, dt);
		g_entries[kept++] = e;
		Entry& k = g_entries[kept - 1];
		const float dx = k.pos[0] - origin[0], dy = k.pos[1] - origin[1], dz = k.pos[2] - origin[2];
		if (dx * dx + dy * dy + dz * dz > kMaxDrawDistance * kMaxDrawDistance) continue;
		if (use3D) {
			if (k.sprite < 0) k.sprite = AcquireSprite(k.text, k.colour);   // texts added before the pool existed
			if (k.sprite >= 0) Draw3D(k);
			continue;
		}
		if (proj == kProjNone) continue;
		float sx = 0.0f, sy = 0.0f;
		if (!Project(cam, proj, k.pos, &sx, &sy)) continue;   // behind the camera
		uint8_t* font = FontForScale(mgr, g_cfg.f[kYouFontScale + k.category] * (k.crit ? 2.0f : 1.0f));
		if (!font) continue;
		if (font != k.font) {
			k.font = font;
			k.width = HudTextWidth(font, k.text);
		}
		const int x = static_cast<int>(sx) - k.width / 2;
		const int y = static_cast<int>(sy) - HudLineHeight(font) / 2;
		if (x < 1 || y < 1 || x + k.width >= screenW || y >= screenH) continue;
		float fade = 1.0f;
		if (g_cfg.fadeMs > 0) fade = ClampF((k.life - t) / (g_cfg.fadeMs / 1000.0f), 0.0f, 1.0f);
		const uint32_t alpha = static_cast<uint32_t>(fade * 255.0f) << 24;
		if (!alpha) continue;
		if (g_cfg.outline) {
			static const int kOffsets[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
			for (const auto& o : kOffsets) HudDrawText(font, k.text, x + o[0], y + o[1], clip, alpha);
		}
		HudDrawText(font, k.text, x, y, clip, alpha | (k.colour & 0x00FFFFFFu));
	}
	g_count = kept;
	// No rect from any sprite for two seconds while texts were queued: the engine is not drawing them.
	if (use3D) {
		if (g_3dRectSeen || !g_count) {
			g_3dNoRectSince = 0;
		}
		else if (!g_3dNoRectSince) {
			g_3dNoRectSince = now;
		}
		else if (now - g_3dNoRectSince > Frequency() * 2) {
			ClearEntries();
			Disable3D("the engine drew no text for two seconds");
		}
	}
}

void SyncOptionsPage();   // FCTOptionsPage, below

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// The toggle hotkey, edge-triggered and only while the game window has focus.
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
		g_cfg.enabled = !g_cfg.enabled;
		ClearEntries();
		SaveConfig();
		SyncOptionsPage();
		g_toggleMessageUntil = Now() + Frequency() * 2;
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
	const char* text = g_cfg.enabled ? "Floating combat text: on" : "Floating combat text: off";
	const int x = (screenW - HudTextWidth(font, text)) / 2;
	const int y = screenH / 6;
	if (x < 1 || y < 1) return;
	HudDrawText(font, text, x + 1, y + 1, clip, 0xFF000000u);
	HudDrawText(font, text, x, y, clip, 0xFFFFFF80u);
}

#define REF(var) reinterpret_cast<void**>(&var)

// OP_Damage handler FUN_140216190(pkt): mov [rsp+8],rsi; push rdi; sub rsp,30; movzx edx,[rcx+2];
// mov rdi,rcx; mov rax,[pLocalPlayer]; mov sil,1; mov ecx,[rax+168]; cmp edx,ecx; je.
// Stolen (5 bytes, position independent): mov [rsp+8],rsi.

// Same function, the 200-unit gate for other people's hits: mov rcx,[spawnMgr]; mov edx,r8d; mov rax,[rcx];
// call [rax+10] (GetSpawnByID); movss xmm3,[1.0f]; mov r8,rax; mov rdx,[pLocalPlayer]; mov rcx,[pDisplay];
// call FUN_14019b2c0.
const char* kDamageGateSig =
	"48 8B 0D ?? ?? ?? ?? 41 8B D0 48 8B 01 FF 50 10 F3 0F 10 1D ?? ?? ?? ?? 4C 8B C0 "
	"48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";

} // namespace

extern const char kFctDamageSig[] =
	"48 89 74 24 08 57 48 83 EC 30 0F B7 51 02 48 8B F9 48 8B 05 ?? ?? ?? ?? 40 B6 01 8B 88 68 01 00 00 3B D1 74 4C";

// Shared with Nameplates (same signature, detour and trampoline as kFctHooks[1]).
void (*const g_fctDamageDetour)(uint8_t* pkt) = &DamageDetour;
void (**const g_fctOrigDamageSlot)(uint8_t* pkt) = &g_origDamage;

const HookSite kFctHooks[2] = {
	// Shared with the Hud (same signature, detour and trampoline): whichever feature is applied first installs it.
	{ kDrawWindowsSig, 5, reinterpret_cast<void*>(&DrawWindowsDetour), reinterpret_cast<void**>(&g_origDrawWindows) },
	{ kFctDamageSig, 5, reinterpret_cast<void*>(&DamageDetour), REF(g_origDamage) },
};

const RefSite kFctRefs[11] = {
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
	// The damage message's chat gate: call ChatMgr; mov rcx,rax; mov ebx,[rbp-58]; mov edx,ebx; call Shows; test al,al
	{ "E8 ?? ?? ?? ?? 48 8B C8 8B 5D A8 8B D3 E8 ?? ?? ?? ?? 84 C0 0F", 1, 5, REF(g_chatMgr) },
	{ "E8 ?? ?? ?? ?? 48 8B C8 8B 5D A8 8B D3 E8 ?? ?? ?? ?? 84 C0 0F", 14, 18, REF(g_chatShows) },
};

const char* FctSharedPrepare() {
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
	// FUN_140108b00: mov r8d,edx; lea rdx,[filters]; jmp (the colour -> filter lookup)
	static const uint8_t kChatShowsStart[] = { 0x44, 0x8B, 0xC2, 0x48, 0x8D, 0x15 };
	if (memcmp(reinterpret_cast<const void*>(g_chatShows), kChatShowsStart, sizeof(kChatShowsStart)) != 0)
		return "the damage message's chat filter check is not the expected function";
	g_cameraOffset = static_cast<size_t>(*g_cameraOffsetAt);
	return nullptr;
}

const char* FctPrepare() {
	if (const char* e = FctSharedPrepare()) return e;
	LoadConfig();
	g_on = true;
	return nullptr;
}

// ---- shared with Nameplates (valid after FctSharedPrepare) ----

// Advanced Loot results (advloot.cpp): the item name over the corpse, as Legends does (sale 0xFFFFDF00, loot
// 0xFF9090FF, 2500 ms, You font scale), gated by [FloatingCombat] SellText / LootText. Over me when the corpse is
// already gone (its last item was taken).
void FctShowLootText(uint32_t corpseId, const char* text, bool sale) {
	if (!g_on || !InGame() || !text || !text[0]) return;
	if (sale ? !g_cfg.sellText : !g_cfg.lootText) return;
	uint8_t* spawn = SpawnById(corpseId);
	if (!spawn) spawn = *g_pLocalPlayer;
	AddText(spawn, text, sale ? 0xFFFFDF00u : 0xFF9090FFu, kYou, false, 2500, kPlain);
}

uint8_t* FctDisplay() {
	return *g_pDisplay;
}

uint8_t* FctCamera() {
	return CurrentCamera();
}

void* FctRenderer() {
	return *g_pRenderer;
}

const int32_t* FctViewport() {
	return g_viewport;
}

uint8_t* FctLocalPlayer() {
	return *g_pLocalPlayer;
}

bool FctInGame() {
	return InGame();
}

uint8_t* FctSpawnById(uint32_t id) {
	return SpawnById(id);
}

uint8_t* FctFirstSpawn() {
	uint8_t* mgr = *g_spawnMgr;
	if (!mgr) return nullptr;
	auto first = reinterpret_cast<uint8_t* (*)(uint8_t*)>((*reinterpret_cast<void***>(mgr))[0x8 / sizeof(void*)]);
	return first(mgr);
}

bool FctProject(const float* world, float* sx, float* sy) {
	uint8_t* cam = CurrentCamera();
	if (!cam) return false;
	const Projection proj = CheckCamera(cam);
	return proj != kProjNone && Project(cam, proj, world, sx, sy);
}

void FctFrame(uint8_t* mgr, bool inGame) {
	if (!g_on) return;
	if (!inGame) {
		ClearEntries();
		g_lastFrame = 0;
		g_toggleHeld = false;
		return;
	}
	PollToggle();
	DrawEntries(mgr);
	DrawToggleMessage(mgr);
}

// ---- FCTOptionsPage ----

namespace {

// COptionsWnd
const size_t kOptTabBox = 0x2d8;         // CTabWnd* OPTW_OptionsSubwindows
const size_t kOptNpcNamesBox = 0x508;    // CCheckBoxWnd*, ODP_NPCNamesCheckbox (same class as our boxes)
const int32_t kOptionsWndSize = 0x28d0;
const size_t kTabNotifyNewPage = 0x08;   // tab change data: CXWnd* new page
const size_t kVtblIsChecked = 0x348;
const size_t kVtblSetCheck = 0x380;
const size_t kVtblSetWindowText = 0x280;
const uint8_t kIsCheckedStart[] = { 0x0F, 0xB6, 0x81, 0x70, 0x02, 0x00, 0x00, 0xC3 };   // movzx eax,[rcx+270]; ret
const uint8_t kSetCheckStart[] = { 0x38, 0x91, 0x70, 0x02, 0x00, 0x00, 0x74 };          // cmp [rcx+270],dl; je
const int kMsgClick = 1;                 // checkbox click (already toggled), combobox pick (data = row), tab change
const int kMsgSliderMoved = 0xe;         // data = new position
const int kSliderSteps = 100;            // Legends: value = min + pos / 100 x (max - min); 101 ticks reach the max

using RefreshFn = void (*)(uint8_t* opt);
using NotifyFn = uint64_t (*)(uint8_t* opt, uint8_t* sender, int msg, void* data);
using GetChildFn = uint8_t* (*)(uint8_t* parent, void** name, char log);
using IsCheckedFn = uint8_t (*)(uint8_t* box);
using SetCheckFn = void (*)(uint8_t* box, uint8_t on, uint8_t unused);
using SetWindowTextFn = void (*)(uint8_t* wnd, void** text);
using SliderSetFn = void (*)(uint8_t* slider, int value);
using ComboSetFn = void (*)(uint8_t* combo, int index);

RefreshFn g_origRefresh = nullptr;       // trampoline into FUN_14049db70
NotifyFn g_origNotify = nullptr;         // trampoline into FUN_14049f110
uint8_t** g_pOptionsWnd = nullptr;       // &DAT_140db5348
int32_t* g_optionsWndSizeAt = nullptr;   // imm32 of `mov ecx,28d0` next to the global's null test
GetChildFn g_getChild = nullptr;         // FUN_14059f8a0
SliderSetFn g_sliderSetValue = nullptr;  // FUN_1405baa20
SliderSetFn g_sliderSetTicks = nullptr;  // FUN_1405ba8a0
ComboSetFn g_comboSetChoice = nullptr;   // FUN_1405ae150
bool g_pageOn = false;

const char* const kSliderIds[kFloatCount] = {
	"FCT_MinVelocitySlider", "FCT_MaxVelocitySlider", "FCT_GravitySlider", "FCT_MinAngleSlider", "FCT_MaxAngleSlider",
	"FCT_YourFontSlider", "FCT_SelfFontSlider", "FCT_OtherFontSlider",
};
const char* const kValueIds[kFloatCount] = {
	"FCT_MinVelocityValueLabel", "FCT_MaxVelocityValueLabel", "FCT_GravityValueLabel", "FCT_MinAngleValueLabel",
	"FCT_MaxAngleValueLabel", "FCT_YourFontValueLabel", "FCT_SelfFontValueLabel", "FCT_OtherFontValueLabel",
};
const char* const kComboIds[kCategoryCount] = { "FCT_YourTextCombobox", "FCT_SelfTextCombobox", "FCT_OtherTextCombobox" };

// DLL extras on the same page ([FCT]): sliders with N positions, value = min + pos x step.
enum ExtraSlider { kXFadeMs, kXDedupMs, kXMaxEntries, kXOtherRange, kXFontStep, kXHeadHeight, kXCount };
struct ExtraSliderInfo {
	const char* id;
	const char* valueId;
	const char* key;
	float min, step;
	int ticks;
	const char* fmt;
};
const ExtraSliderInfo kExtraSliders[kXCount] = {
	{ "FCT_FadeMsSlider", "FCT_FadeMsValueLabel", "FadeMs", 0.0f, 50.0f, 101, "%.0f" },
	{ "FCT_DedupMsSlider", "FCT_DedupMsValueLabel", "DedupMs", 0.0f, 20.0f, 101, "%.0f" },
	{ "FCT_MaxEntriesSlider", "FCT_MaxEntriesValueLabel", "MaxEntries", 1.0f, 1.0f, 64, "%.0f" },
	{ "FCT_OtherRangeSlider", "FCT_OtherRangeValueLabel", "OtherRange", 0.0f, 10.0f, 101, "%.0f" },
	{ "FCT_FontStepSlider", "FCT_FontStepValueLabel", "FontStep", 0.5f, 0.25f, 63, "%.2f" },
	{ "FCT_HeadHeightSlider", "FCT_HeadHeightValueLabel", "HeadHeight", -5.0f, 0.1f, 101, "%.1f" },
};
const char* const kColourComboIds[kCategoryCount] = { "FCT_YourColourCombobox", "FCT_SelfColourCombobox", "FCT_OtherColourCombobox" };
const char* const kColourKeys[kCategoryCount] = { "YouColour", "SelfColour", "OtherColour" };
// Colour combobox rows (the XML's order): 0 = the client's chat colour, then presets, last = Custom (ini).
const uint32_t kColourPresets[] = { 0, 0xF0F0F0, 0xF00000, 0xFF8000, 0xF0F000, 0x00F000, 0x00F0F0, 0x0080FF, 0xC000FF, 0xFF80C0 };
const int kColourPresetCount = static_cast<int>(sizeof(kColourPresets) / sizeof(kColourPresets[0]));   // Custom = this row
// Toggle key combobox rows; the last one is Custom (ini).
const char* const kKeyPresets[] = { "none", "Ctrl+Shift+F", "Ctrl+Shift+C", "Ctrl+Shift+T", "Ctrl+Alt+F", "Shift+F12" };
const int kKeyPresetCount = static_cast<int>(sizeof(kKeyPresets) / sizeof(kKeyPresets[0]));

struct Page {
	uint8_t* owner;                      // the COptionsWnd these were looked up on
	uint8_t* page;
	uint8_t* toggle;
	uint8_t* sell;
	uint8_t* loot;
	uint8_t* slider[kFloatCount];
	uint8_t* value[kFloatCount];
	uint8_t* combo[kCategoryCount];
	uint8_t* outline;
	uint8_t* xslider[kXCount];
	uint8_t* xvalue[kXCount];
	uint8_t* keyCombo;
	uint8_t* keyValue;
	uint8_t* colourCombo[kCategoryCount];
};
Page g_page = {};

uint8_t* Child(uint8_t* parent, const char* id) {
	void* name = nullptr;
	HudCXStr(&name, id);
	if (!name) return nullptr;
	uint8_t* wnd = g_getChild(parent, &name, 0);
	HudCXStrRelease(&name);
	return wnd;
}

bool LooksLikeCheckbox(uint8_t* opt, uint8_t* box) {
	void** vt = *reinterpret_cast<void***>(box);
	if (!vt) return false;
	uint8_t* npc = *reinterpret_cast<uint8_t**>(opt + kOptNpcNamesBox);
	if (npc && *reinterpret_cast<void***>(npc) == vt) return true;
	return memcmp(vt[kVtblIsChecked / sizeof(void*)], kIsCheckedStart, sizeof(kIsCheckedStart)) == 0 &&
	       memcmp(vt[kVtblSetCheck / sizeof(void*)], kSetCheckStart, sizeof(kSetCheckStart)) == 0;
}

void* Virtual(uint8_t* wnd, size_t slot) {
	return (*reinterpret_cast<void***>(wnd))[slot / sizeof(void*)];
}

void SetBox(uint8_t* box, bool on) {
	if (box) reinterpret_cast<SetCheckFn>(Virtual(box, kVtblSetCheck))(box, on ? 1 : 0, 0);
}

bool IsChecked(uint8_t* box) {
	return reinterpret_cast<IsCheckedFn>(Virtual(box, kVtblIsChecked))(box) != 0;
}

void SetText(uint8_t* wnd, const char* text) {
	if (!wnd) return;
	void* str = nullptr;
	HudCXStr(&str, text);
	if (!str) return;
	reinterpret_cast<SetWindowTextFn>(Virtual(wnd, kVtblSetWindowText))(wnd, &str);
	HudCXStrRelease(&str);
}

// Legends' slider label: the position as a percentage of the track (its CSliderWnd draw), not the setting.
void PercentText(int position, int ticks, char* out, size_t size) {
	const float pct = ticks > 1 ? static_cast<float>(Clamp(position, 0, ticks - 1)) / (static_cast<float>(ticks) - 1.0f) * 100.0f : 0.0f;
	snprintf(out, size, "%.0f%%", static_cast<double>(pct));
}

void SetLabel(uint8_t* label, int position) {
	char text[32];
	PercentText(position, kSliderSteps + 1, text, sizeof(text));
	SetText(label, text);
}

int PositionOf(int option) {
	const FloatInfo& fi = kFloats[option];
	return Clamp(static_cast<int>((g_cfg.f[option] - fi.min) / (fi.max - fi.min) * kSliderSteps + 0.5f), 0, kSliderSteps);
}

float ValueOf(int option, int position) {
	const FloatInfo& fi = kFloats[option];
	return fi.min + static_cast<float>(Clamp(position, 0, kSliderSteps)) / kSliderSteps * (fi.max - fi.min);
}

float ExtraValue(int i) {
	switch (i) {
	case kXFadeMs: return static_cast<float>(g_cfg.fadeMs);
	case kXDedupMs: return static_cast<float>(g_cfg.dedupMs);
	case kXMaxEntries: return static_cast<float>(g_cfg.maxEntries);
	case kXOtherRange: return g_cfg.otherRange;
	case kXFontStep: return g_cfg.fontStep;
	default: return g_cfg.headHeight;
	}
}

void SetExtraValue(int i, float v) {
	switch (i) {
	case kXFadeMs: g_cfg.fadeMs = static_cast<int>(lroundf(v)); break;
	case kXDedupMs: g_cfg.dedupMs = static_cast<int>(lroundf(v)); break;
	case kXMaxEntries: g_cfg.maxEntries = Clamp(static_cast<int>(lroundf(v)), 1, kMaxEntries); break;
	case kXOtherRange: g_cfg.otherRange = v; break;
	case kXFontStep: g_cfg.fontStep = v; break;
	default: g_cfg.headHeight = v; break;
	}
}

void SetExtraLabel(int i, int position) {
	char text[32];
	if (g_cfg.valueLabels) snprintf(text, sizeof(text), kExtraSliders[i].fmt, ExtraValue(i));
	else PercentText(position, kExtraSliders[i].ticks, text, sizeof(text));
	SetText(g_page.xvalue[i], text);
}

int ExtraPosition(int i) {
	const ExtraSliderInfo& x = kExtraSliders[i];
	return Clamp(static_cast<int>(lroundf((ExtraValue(i) - x.min) / x.step)), 0, x.ticks - 1);
}

// One [FCT] key, written only when the player changed it (the line's ini comment goes with it).
void SaveExtra(const char* key, const char* text) {
	if (g_iniPath[0]) WritePrivateProfileStringA(kExtras, key, text, g_iniPath);
}

// "Ctrl+Shift+F", "F9", "none": the inverse of ParseHotkey.
void FormatHotkey(const Config& c, char* out, size_t size) {
	if (!c.toggleKey) {
		strcpy_s(out, size, "none");
		return;
	}
	char key[8];
	if (c.toggleKey >= VK_F1 && c.toggleKey <= VK_F24) snprintf(key, sizeof(key), "F%d", c.toggleKey - VK_F1 + 1);
	else snprintf(key, sizeof(key), "%c", static_cast<char>(c.toggleKey));
	snprintf(out, size, "%s%s%s%s", c.toggleCtrl ? "Ctrl+" : "", c.toggleAlt ? "Alt+" : "", c.toggleShift ? "Shift+" : "", key);
}

int KeyRow() {
	for (int i = 0; i < kKeyPresetCount; ++i) {
		Config c;
		ParseHotkey(kKeyPresets[i], &c);
		if (c.toggleKey == g_cfg.toggleKey && (!c.toggleKey || (c.toggleCtrl == g_cfg.toggleCtrl &&
		    c.toggleShift == g_cfg.toggleShift && c.toggleAlt == g_cfg.toggleAlt)))
			return i;
	}
	return kKeyPresetCount;   // Custom (ini)
}

void SetKeyLabel() {
	char key[48], text[64];
	FormatHotkey(g_cfg, key, sizeof(key));
	snprintf(text, sizeof(text), "Current: %s", key);
	SetText(g_page.keyValue, text);
}

int ColourRow(uint32_t colour) {
	const uint32_t rgb = colour & 0x00FFFFFFu;
	if (!colour) return 0;
	for (int i = 1; i < kColourPresetCount; ++i)
		if (kColourPresets[i] == rgb) return i;
	return kColourPresetCount;   // Custom (ini)
}

// Settings -> controls (what Legends' page refresh does).
void Fill() {
	SetBox(g_page.toggle, g_cfg.enabled);
	SetBox(g_page.sell, g_cfg.sellText);
	SetBox(g_page.loot, g_cfg.lootText);
	for (int i = 0; i < kFloatCount; ++i) {
		if (g_page.slider[i]) g_sliderSetValue(g_page.slider[i], PositionOf(i));
		SetLabel(g_page.value[i], PositionOf(i));
	}
	for (int i = 0; i < kCategoryCount; ++i)
		if (g_page.combo[i]) g_comboSetChoice(g_page.combo[i], g_cfg.hitText[i]);
	SetBox(g_page.outline, g_cfg.outline);
	for (int i = 0; i < kXCount; ++i) {
		if (g_page.xslider[i]) g_sliderSetValue(g_page.xslider[i], ExtraPosition(i));
		SetExtraLabel(i, ExtraPosition(i));
	}
	if (g_page.keyCombo) g_comboSetChoice(g_page.keyCombo, KeyRow());
	SetKeyLabel();
	for (int i = 0; i < kCategoryCount; ++i)
		if (g_page.colourCombo[i]) g_comboSetChoice(g_page.colourCombo[i], ColourRow(g_cfg.colour[i]));
}

// Looks the page and its controls up once per COptionsWnd; false when the skin has no page.
bool Resolve() {
	uint8_t* opt = *g_pOptionsWnd;
	if (!opt) {
		g_page = {};
		return false;
	}
	if (g_page.owner == opt) return g_page.page != nullptr;
	g_page = {};
	g_page.owner = opt;
	uint8_t* page = Child(opt, "OptionsCombatTextPage");
	if (!page) return false;
	Page p = {};
	p.owner = opt;
	p.page = page;
	p.toggle = Child(page, "FCT_Toggle");
	p.sell = Child(page, "FCT_SellToggle");
	p.loot = Child(page, "FCT_LootToggle");
	p.outline = Child(page, "FCT_OutlineToggle");
	uint8_t* const boxes[] = { p.toggle, p.sell, p.loot, p.outline };
	for (uint8_t* box : boxes)
		if (box && !LooksLikeCheckbox(opt, box)) return false;
	for (int i = 0; i < kFloatCount; ++i) {
		p.slider[i] = Child(page, kSliderIds[i]);
		p.value[i] = Child(page, kValueIds[i]);
		if (p.slider[i]) g_sliderSetTicks(p.slider[i], kSliderSteps + 1);
	}
	for (int i = 0; i < kCategoryCount; ++i) {
		p.combo[i] = Child(page, kComboIds[i]);
		p.colourCombo[i] = Child(page, kColourComboIds[i]);
	}
	for (int i = 0; i < kXCount; ++i) {
		p.xslider[i] = Child(page, kExtraSliders[i].id);
		p.xvalue[i] = Child(page, kExtraSliders[i].valueId);
		if (p.xslider[i]) g_sliderSetTicks(p.xslider[i], kExtraSliders[i].ticks);
	}
	p.keyCombo = Child(page, "FCT_ToggleKeyCombobox");
	p.keyValue = Child(page, "FCT_ToggleKeyValueLabel");
	g_page = p;
	Fill();   // first sync (also after a UI reload while the window is open)
	return true;
}

void RefreshDetour(uint8_t* opt) {
	g_origRefresh(opt);
	if (g_pageOn) Resolve();
	NameplatePageRefresh(opt);
}

uint64_t NotifyDetour(uint8_t* opt, uint8_t* sender, int msg, void* data) {
	if (sender && NameplatePageNotify(opt, sender, msg, data)) return 0;
	if (!g_pageOn || !sender || opt != *g_pOptionsWnd || !Resolve()) return g_origNotify(opt, sender, msg, data);
	if (msg == kMsgClick && sender == *reinterpret_cast<uint8_t**>(opt + kOptTabBox)) {
		if (data && *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(data) + kTabNotifyNewPage) == g_page.page) Fill();
		return g_origNotify(opt, sender, msg, data);
	}
	const int arg = static_cast<int>(reinterpret_cast<intptr_t>(data));
	if (msg == kMsgClick && (sender == g_page.toggle || sender == g_page.sell || sender == g_page.loot)) {
		const bool on = IsChecked(sender);
		if (sender == g_page.toggle) {
			g_cfg.enabled = on;
			if (!on) ClearEntries();
		}
		else if (sender == g_page.sell) g_cfg.sellText = on;
		else g_cfg.lootText = on;
		SaveConfig();
		return 0;
	}
	if (msg == kMsgClick && sender == g_page.outline) {
		g_cfg.outline = IsChecked(sender);
		SaveExtra("Outline", g_cfg.outline ? "1" : "0");
		return 0;
	}
	if (msg == kMsgSliderMoved) {
		for (int i = 0; i < kFloatCount; ++i) {
			if (sender != g_page.slider[i]) continue;
			g_cfg.f[i] = ValueOf(i, arg);
			SetLabel(g_page.value[i], arg);
			SaveConfig();
			return 0;
		}
		for (int i = 0; i < kXCount; ++i) {
			if (sender != g_page.xslider[i]) continue;
			const ExtraSliderInfo& x = kExtraSliders[i];
			SetExtraValue(i, x.min + static_cast<float>(Clamp(arg, 0, x.ticks - 1)) * x.step);
			SetExtraLabel(i, arg);
			if (i == kXMaxEntries && g_count > g_cfg.maxEntries) {
				// keep the newest
				const int drop = g_count - g_cfg.maxEntries;
				for (int k = 0; k < drop; ++k) ReleaseSprite(g_entries[k]);
				memmove(g_entries, g_entries + drop, sizeof(Entry) * (g_count - drop));
				g_count -= drop;
			}
			char text[32];
			snprintf(text, sizeof(text), x.fmt, ExtraValue(i));
			SaveExtra(x.key, text);
			return 0;
		}
	}
	if (msg == kMsgClick) {
		for (int i = 0; i < kCategoryCount; ++i) {
			if (sender == g_page.combo[i]) {
				g_cfg.hitText[i] = Clamp(arg, kModeNone, kModeDamageOnly);
				SaveConfig();
				return 0;
			}
			if (sender == g_page.colourCombo[i]) {
				if (arg < 0 || arg >= kColourPresetCount) {
					g_comboSetChoice(sender, ColourRow(g_cfg.colour[i]));   // Custom (ini): keep the ini colour
					return 0;
				}
				const uint32_t rgb = kColourPresets[arg];
				g_cfg.colour[i] = rgb ? (rgb | 0xFF000000u) : 0;
				char text[16] = "";
				if (rgb) snprintf(text, sizeof(text), "%06X", rgb);
				SaveExtra(kColourKeys[i], text);
				return 0;
			}
		}
		if (sender == g_page.keyCombo) {
			if (arg < 0 || arg >= kKeyPresetCount) {
				g_comboSetChoice(sender, KeyRow());   // Custom (ini): keep the ini binding
				return 0;
			}
			ParseHotkey(kKeyPresets[arg], &g_cfg);
			g_toggleHeld = false;
			SaveExtra("ToggleKey", kKeyPresets[arg]);
			SetKeyLabel();
			return 0;
		}
	}
	return g_origNotify(opt, sender, msg, data);
}

// Keeps the page in step after the toggle key changed Enabled (no-op when the page is not resolved).
void SyncOptionsPage() {
	if (g_pageOn && g_page.page && g_page.owner == *g_pOptionsWnd) SetBox(g_page.toggle, g_cfg.enabled);
}

const char* kOptionsWndSig = "48 83 3D ?? ?? ?? ?? 00 75 ?? B9 D0 28 00 00";

} // namespace

extern const char kFctRefreshSig[] = "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 56 41 57 48 8B EC";
extern const char kFctNotifySig[] =
	"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 49 8B E9 45 8B F0";
// Shared with NameplateOptionsPage (same signatures, detours and trampolines as kFctPageHooks).
void (*const g_fctRefreshDetour)(uint8_t* opt) = &RefreshDetour;
void (**const g_fctOrigRefreshSlot)(uint8_t* opt) = &g_origRefresh;
uint64_t (*const g_fctNotifyDetour)(uint8_t* opt, uint8_t* sender, int msg, void* data) = &NotifyDetour;
uint64_t (**const g_fctOrigNotifySlot)(uint8_t* opt, uint8_t* sender, int msg, void* data) = &g_origNotify;

const HookSite kFctPageHooks[2] = {
	// COptionsWnd::RefreshDisplayPage FUN_14049db70 (end of the constructor = UI load). Stolen (7 bytes,
	// position independent): mov rax,rsp; mov [rax+10],rbx.
	{ kFctRefreshSig, 7, reinterpret_cast<void*>(&RefreshDetour), REF(g_origRefresh) },
	// COptionsWnd::WndNotification FUN_14049f110(opt, sender, msg, data), vtbl +0x120. Stolen (5 bytes,
	// position independent): mov [rsp+8],rbx.
	{ kFctNotifySig, 5, reinterpret_cast<void*>(&NotifyDetour), REF(g_origNotify) },
};

const RefSite kFctPageRefs[6] = {
	// Options window factory FUN_140193360: cmp qword [pOptionsWnd],0; jne; mov ecx,28d0 (sizeof COptionsWnd)
	{ kOptionsWndSig, 3, 8, REF(g_pOptionsWnd) },
	{ kOptionsWndSig, 11, 0, REF(g_optionsWndSizeAt) },
	// CXWnd::GetChildItem FUN_14059f8a0(parent, CXStr*, char log)
	{ "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC 20 02 00 00", 0, 0, REF(g_getChild) },
	// CSliderWnd::SetValue FUN_1405baa20(slider, pos): mov r8,rcx; test edx,edx; jns; xor edx,edx
	{ "4C 8B C1 85 D2 79 ?? 33 D2", 0, 0, REF(g_sliderSetValue) },
	// CSliderWnd::SetNumTicks FUN_1405ba8a0(slider, n): push rbx; sub rsp,40; mov eax,2
	{ "40 53 48 83 EC 40 B8 02 00 00 00", 0, 0, REF(g_sliderSetTicks) },
	// CComboWnd::SetChoice FUN_1405ae150(combo, idx): mov rcx,[rcx+260] (list); mov eax,[rcx+268] (count)
	{ "48 8B 89 60 02 00 00 8B 81 68 02 00 00", 0, 0, REF(g_comboSetChoice) },
};

const char* FctPageSharedPrepare() {
	if (const char* e = HudTextPrepare()) return e;
	if (*g_optionsWndSizeAt != kOptionsWndSize) return "the options window is not the expected size (0x28d0)";
	return nullptr;
}

const char* FctPagePrepare() {
	if (!g_on) return "needs FloatingCombatText";
	if (const char* e = FctPageSharedPrepare()) return e;
	g_pageOn = true;
	return nullptr;
}

// ---- options page helpers shared with NameplateOptionsPage (valid after FctPageSharedPrepare) ----

uint8_t* OptOptionsWnd() {
	return *g_pOptionsWnd;
}

uint8_t* OptChild(uint8_t* parent, const char* id) {
	return Child(parent, id);
}

bool OptLooksLikeCheckbox(uint8_t* opt, uint8_t* box) {
	return LooksLikeCheckbox(opt, box);
}

void OptSetBox(uint8_t* box, bool on) {
	SetBox(box, on);
}

bool OptIsChecked(uint8_t* box) {
	return IsChecked(box);
}

void OptSetText(uint8_t* wnd, const char* text) {
	SetText(wnd, text);
}

void OptSliderSetValue(uint8_t* slider, int position) {
	if (slider) g_sliderSetValue(slider, position);
}

void OptSliderSetTicks(uint8_t* slider, int ticks) {
	if (slider) g_sliderSetTicks(slider, ticks);
}

void OptComboSetChoice(uint8_t* combo, int row) {
	if (combo) g_comboSetChoice(combo, row);
}

bool OptIsTabSwitchTo(uint8_t* opt, uint8_t* sender, int msg, void* data, uint8_t* page) {
	return msg == kMsgClick && page && sender == *reinterpret_cast<uint8_t**>(opt + kOptTabBox) && data &&
	       *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(data) + kTabNotifyNewPage) == page;
}
