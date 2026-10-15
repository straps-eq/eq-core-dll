// Hud and TargetDistance: screen-space text drawn once per frame on top of the client's windows.
//
// Hook: CXWndManager::DrawWindows (FUN_1405cc3a0), called once per frame by every game-state loop
// (char select, loading, in game). We run the original, then, only while the client is in game
// (CEverQuest::GameState == 5, the value SetGameState FUN_140291d60 handles as "in game"; pLocalPlayer
// and pLocalPC set), draw our text exactly the way the client's own on-screen text helper
// FUN_1401a4620 does: CXStr from a C string (FUN_1405853f0), CTextureFont::DrawWrappedText
// (FUN_1405d1990) with the window manager's font, clip rect = {x, y, screenW, screenH}, then release
// the CXStr (FUN_140587670). Both functions and the globals are cross-checked against the calls inside
// FUN_1401a4620 itself before anything is hooked.
//
// TargetDistance: one line inside the Target window, drawn from the window's own Draw virtual
// (CTargetWnd vtbl+0x20 = FUN_14050db60, which paints the HP box animations; the window driver
// FUN_1405a5190 calls +0x18, then +0x20, then the children, then +0x28), so windows above it in the
// z-order paint over the line like over the rest of the window. Placement: the client rect (vtbl+0x210,
// checked against GetClientRect FUN_1405a7ce0) plus OffsetY, pushed below any visible child (first child
// +0x20, next sibling +0x10, visible +0x211; screen rect from vtbl+0x218 / GetScreenRect FUN_1405a7bb0)
// that overlaps that row from above, i.e. the HP gauge and its labels; clipped to the client rect.
// "<level> <race> <class>" on the left in the target's con colour (GetConLevel FUN_1402d7fa0 -> colour
// index FUN_140661f70 -> user colour FUN_14050b6d0, the chain the window uses for its own HP bar tint),
// the 3D distance (like the client's own range checks) right-aligned with the font's width function
// (FUN_14059bf40(fontIndex, CXStr*), the target of the CTextureFont::GetWidth thunk FUN_1405d1e70). Race
// names come from the dbstr table (type 11, keyed by the race id the spawn's vtbl+0x110 returns), the
// class name from a table of the client's class ids (spawn+0xfd4, the field the GM name tag prints).
// Hud: a fixed block at [Hud] X,Y: /loc (Y, X, Z), zone long name (OP_NewZone data at DAT_140e6dad4),
// target name and distance, FPS from frame deltas, local clock. One draw call per enabled line.
// Text goes into fixed buffers; the only runtime allocation is the client's own CXStr, released on
// the same call. Spawn pointers are read from the globals on every frame and never kept.
#include "hud.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace {

// PlayerClient (spawn) fields, see map.cpp.
const size_t kSpawnY = 0x74;
const size_t kSpawnX = 0x78;
const size_t kSpawnZ = 0x7c;
const size_t kSpawnName = 0xb4;          // char[0x40]
const size_t kSpawnDisplayedName = 0xf4; // char[0x40] cleaned name
const size_t kSpawnLevel = 0x494;        // u8
const size_t kSpawnClass = 0xfd4;        // int (GM name tag: FUN_1402772a0(class) -> eqstr)
const size_t kVtblGetRace = 0x110;       // int GetRace()
const int kDbstrRaceName = 11;

// CEverQuest (DAT_140ee7010): game state written by SetGameState FUN_140291d60.
const size_t kEverQuestGameState = 0x5e4;
const int kGameStateInGame = 5;

// CXWndManager (DAT_140f0f8c8), as used by FUN_1401a4620.
const size_t kWndMgrScreenW = 0x188;
const size_t kWndMgrScreenH = 0x18c;
const size_t kWndMgrFontCount = 0x190;
const size_t kWndMgrFonts = 0x198;       // CTextureFont** ; fonts[2] is the client's HUD font

// CXWnd
const size_t kWndFirstChild = 0x20;      // CXWnd*, the list FUN_1405a5190 walks after the window's Draw
const size_t kWndNextSibling = 0x10;     // CXWnd*
const size_t kWndScreenRectCache = 0x5c; // int[4], what GetScreenRect returns
const size_t kWndVisible = 0x211;        // u8, the DrawWindows gate
const size_t kVtblGetClientRect = 0x210; // (CXWnd*, int rect[4]) -> rect, screen coordinates
const size_t kVtblGetScreenRect = 0x218; // (CXWnd*, int rect[4]) -> rect
// CTextureFont
const size_t kFontIndex = 0x08;          // int, what the GetWidth / GetHeight thunks pass on

// Zone info (DAT_140e6dad4), filled from OP_NewZone by FUN_140203d30.
const size_t kZoneShortName = 0x0;       // char[0x80]
const size_t kZoneLongName = 0x80;       // char[0x80]

using DrawWindowsFn = int (*)(uint8_t* mgr);
using CXStrCtorFn = void* (*)(void** str, const char* text);
using CXStrReleaseFn = void (*)(void** str);
using DrawWrappedTextFn = int (*)(uint8_t* font, void** str, int x, int y, int maxWidth, int* clip,
                                  uint32_t argb, uint16_t flags, int offset);
using FontHeightFn = int (*)(uint8_t* font);
using TextWidthFn = int (*)(int fontIndex, void** str);
using GetScreenRectFn = int* (*)(uint8_t* wnd, int* rect);
using TargetDrawFn = int64_t (*)(uint8_t* wnd);
using GetConLevelFn = int (*)(void* pLocalPC, uint8_t* spawn);
using ConColourIndexFn = int (*)(int con);
using UserColourFn = uint32_t (*)(int index);
using DbStrFn = const char* (*)(void* table, unsigned id, unsigned type, bool* found);

DrawWindowsFn g_origDrawWindows = nullptr; // trampoline into FUN_1405cc3a0
CXStrCtorFn g_cxstrCtor = nullptr;         // FUN_1405853f0
CXStrReleaseFn g_cxstrRelease = nullptr;   // FUN_140587670
DrawWrappedTextFn g_drawWrapped = nullptr; // FUN_1405d1990
FontHeightFn g_fontHeight = nullptr;       // FUN_1405d1e30 (thunk to FUN_14059bdc0)
TextWidthFn g_textWidth = nullptr;         // FUN_14059bf40 (behind the GetWidth thunk FUN_1405d1e70)
GetScreenRectFn g_getScreenRect = nullptr; // FUN_1405a7bb0
GetScreenRectFn g_getClientRect = nullptr; // FUN_1405a7ce0
TargetDrawFn g_origTargetDraw = nullptr;   // trampoline into FUN_14050db60
GetConLevelFn g_getConLevel = nullptr;     // FUN_1402d7fa0
void* g_getConLevelCalled = nullptr;       // the call next to the pLocalPC load in CTargetWnd, for the cross-check
ConColourIndexFn g_conColourIndex = nullptr; // FUN_140661f70
UserColourFn g_userColour = nullptr;       // FUN_14050b6d0
DbStrFn g_dbstr = nullptr;                 // FUN_14018b660
void** g_dbstrTable = nullptr;             // &DAT_140db4de0
void* g_cxstrCtorCalled = nullptr;         // the three calls inside FUN_1401a4620, for cross-checks
void* g_drawWrappedCalled = nullptr;
void* g_cxstrReleaseCalled = nullptr;
uint8_t** g_wndMgr = nullptr;              // &DAT_140f0f8c8
uint8_t** g_pEverQuest = nullptr;          // &DAT_140ee7010
uint8_t** g_pLocalPlayer = nullptr;        // &DAT_140e66228
void** g_pLocalPC = nullptr;               // &DAT_140e65d90
uint8_t** g_pTarget = nullptr;             // &DAT_140e662d0
uint8_t* g_zoneInfo = nullptr;             // &DAT_140e6dad4

// ---- configuration ([Hud] and [TargetDistance] in dinput8.ini) ----

struct Config {
	int x = 10;
	int y = 10;
	uint32_t colour = 0xFFF0F0F0;
	int font = 2;
	bool loc = true;
	bool zone = true;
	bool target = true;
	bool fps = true;
	bool clock = true;
	uint32_t distColour = 0xFFF0F0F0;
	int distFont = 2;
	int distOffsetX = -2;       // from the client area's right edge (text is right-aligned)
	int distOffsetY = 2;        // from the client area's top edge; the line is then pushed below the HP gauge
	bool distAutoPlace = true;  // push the row below the gauge and labels that overlap it
	bool distConLine = true;    // "<level> <race> <class>" in con colour on the left of the same line
	int distConOffsetX = 2;     // from the client area's left edge
};
Config g_cfg;
bool g_configLoaded = false;
bool g_hudOn = false;
bool g_distOn = false;

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void LoadConfig() {
	if (g_configLoaded) return;
	g_configLoaded = true;
	char ini[MAX_PATH];
	GetModuleFileNameA(nullptr, ini, MAX_PATH);
	char* slash = strrchr(ini, '\\');
	if (slash) *slash = '\0';
	strcat_s(ini, "\\dinput8.ini");
	auto Int = [&](const char* section, const char* key, int def) {
		return static_cast<int>(GetPrivateProfileIntA(section, key, def, ini));
	};
	auto Colour = [&](const char* section, const char* key, uint32_t def) {
		char buf[32] = {};
		GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), ini);
		if (!buf[0]) return def;
		return static_cast<uint32_t>(strtoul(buf, nullptr, 16)) | 0xFF000000u;
	};
	Config c;
	c.x = Clamp(Int("Hud", "X", c.x), 0, 16384);
	c.y = Clamp(Int("Hud", "Y", c.y), 0, 16384);
	c.colour = Colour("Hud", "Colour", c.colour);
	c.font = Clamp(Int("Hud", "Font", c.font), 0, 15);
	c.loc = Int("Hud", "Loc", c.loc) != 0;
	c.zone = Int("Hud", "Zone", c.zone) != 0;
	c.target = Int("Hud", "Target", c.target) != 0;
	c.fps = Int("Hud", "FPS", c.fps) != 0;
	c.clock = Int("Hud", "Clock", c.clock) != 0;
	c.distColour = Colour("TargetDistance", "Colour", c.distColour);
	c.distFont = Clamp(Int("TargetDistance", "Font", c.distFont), 0, 15);
	c.distOffsetX = Clamp(Int("TargetDistance", "OffsetX", c.distOffsetX), -4096, 4096);
	c.distOffsetY = Clamp(Int("TargetDistance", "OffsetY", c.distOffsetY), -4096, 4096);
	c.distAutoPlace = Int("TargetDistance", "AutoPlace", c.distAutoPlace) != 0;
	c.distConLine = Int("TargetDistance", "ConLine", c.distConLine) != 0;
	c.distConOffsetX = Clamp(Int("TargetDistance", "ConOffsetX", c.distConOffsetX), -4096, 4096);
	g_cfg = c;
}

// The target of the first RIP-relative `mov rcx,[global]` (48 8B 0D disp32) in a function body.
const void* FirstGlobalLoad(const uint8_t* fn, size_t size) {
	for (size_t i = 0; i + 7 <= size; ++i) {
		if (fn[i] != 0x48 || fn[i + 1] != 0x8B || fn[i + 2] != 0x0D) continue;
		int32_t rel;
		memcpy(&rel, fn + i + 3, 4);
		return fn + i + 7 + rel;
	}
	return nullptr;
}

// ---- drawing ----

const int kTextMax = 192;
bool g_inDraw = false;
bool g_inTargetDraw = false;

// FPS from frame deltas, refreshed twice a second.
LARGE_INTEGER g_fpsStart = {};
int g_fpsFrames = 0;
float g_fps = 0.0f;

void TickFps() {
	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	if (g_fpsStart.QuadPart == 0) g_fpsStart = now;
	++g_fpsFrames;
	LONGLONG elapsed = now.QuadPart - g_fpsStart.QuadPart;
	if (elapsed * 2 >= freq.QuadPart) {
		g_fps = static_cast<float>(g_fpsFrames) * static_cast<float>(freq.QuadPart) / static_cast<float>(elapsed);
		g_fpsFrames = 0;
		g_fpsStart = now;
	}
}

uint8_t* Font(uint8_t* mgr, int index) {
	if (index >= *reinterpret_cast<int*>(mgr + kWndMgrFontCount)) return nullptr;
	uint8_t** fonts = *reinterpret_cast<uint8_t***>(mgr + kWndMgrFonts);
	return fonts ? fonts[index] : nullptr;
}

// The client's own text draw (FUN_1401a4620) with an ARGB colour, a font and a clip rect. Returns the width.
int DrawStringClipped(uint8_t* font, const char* text, int x, int y, const int* clip, uint32_t argb) {
	if (x >= clip[2] || y >= clip[3]) return 0;
	void* str = nullptr;
	g_cxstrCtor(&str, text);
	if (!str) return 0;
	int rect[4] = { clip[0], clip[1], clip[2], clip[3] };
	int width = g_drawWrapped(font, &str, x, y, clip[2] - x, rect, argb, 1, 0);
	if (str) g_cxstrRelease(&str);
	return width;
}

int DrawString(uint8_t* mgr, uint8_t* font, const char* text, int x, int y, uint32_t argb) {
	int clip[4] = { x, y, *reinterpret_cast<int*>(mgr + kWndMgrScreenW), *reinterpret_cast<int*>(mgr + kWndMgrScreenH) };
	return DrawStringClipped(font, text, x, y, clip, argb);
}

// Width of `text` in `font`: the font's own measure, as the client's label layout calls it.
int TextWidth(uint8_t* font, const char* text) {
	void* str = nullptr;
	g_cxstrCtor(&str, text);
	if (!str) return 0;
	int width = g_textWidth(*reinterpret_cast<int*>(font + kFontIndex), &str);
	if (str) g_cxstrRelease(&str);
	return (width > 0 && width < 4096) ? width : 0;
}

int LineHeight(uint8_t* font) {
	int h = g_fontHeight(font);
	return (h >= 6 && h <= 64) ? h + 1 : 14;
}

// In game with a drawable UI: game state 5, local player and PC present, window manager with fonts.
bool InGame(uint8_t* mgr) {
	uint8_t* eq = *g_pEverQuest;
	if (!eq || *reinterpret_cast<int*>(eq + kEverQuestGameState) != kGameStateInGame) return false;
	if (!*g_pLocalPlayer || !*g_pLocalPC) return false;
	return mgr != nullptr && *reinterpret_cast<uint8_t***>(mgr + kWndMgrFonts) != nullptr;
}

float Distance(const uint8_t* a, const uint8_t* b) {
	float dx = *reinterpret_cast<const float*>(a + kSpawnX) - *reinterpret_cast<const float*>(b + kSpawnX);
	float dy = *reinterpret_cast<const float*>(a + kSpawnY) - *reinterpret_cast<const float*>(b + kSpawnY);
	float dz = *reinterpret_cast<const float*>(a + kSpawnZ) - *reinterpret_cast<const float*>(b + kSpawnZ);
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

const char* SpawnName(const uint8_t* spawn) {
	const char* name = reinterpret_cast<const char*>(spawn + kSpawnDisplayedName);
	return name[0] ? name : reinterpret_cast<const char*>(spawn + kSpawnName);
}

// Class ids as the client numbers them (player classes 1-16, then the NPC roles).
const char* ClassName(int id) {
	static const char* const kPlayer[] = {
		"", "Warrior", "Cleric", "Paladin", "Ranger", "Shadow Knight", "Druid", "Monk", "Bard", "Rogue", "Shaman",
		"Necromancer", "Wizard", "Magician", "Enchanter", "Beastlord", "Berserker",
	};
	if (id >= 1 && id <= 16) return kPlayer[id];
	switch (id) {
	case 20: return "GM Warrior"; case 21: return "GM Cleric"; case 22: return "GM Paladin"; case 23: return "GM Ranger";
	case 24: return "GM Shadow Knight"; case 25: return "GM Druid"; case 26: return "GM Monk"; case 27: return "GM Bard";
	case 28: return "GM Rogue"; case 29: return "GM Shaman"; case 30: return "GM Necromancer"; case 31: return "GM Wizard";
	case 32: return "GM Magician"; case 33: return "GM Enchanter"; case 34: return "GM Beastlord"; case 35: return "GM Berserker";
	case 40: return "Banker"; case 41: return "Merchant"; case 59: return "Discord Merchant"; case 60: return "Adventure Recruiter";
	case 61: return "Adventure Merchant"; case 63: return "Tribute Master"; case 64: return "Guild Tribute Master";
	case 66: return "Guild Bank"; case 67: return "Radiant Crystal Merchant"; case 68: return "Ebon Crystal Merchant";
	case 69: return "Fellowship Registrar"; case 70: return "Alternate Currency Merchant"; case 71: return "Mercenary Liaison";
	case 72: return "Real Estate"; case 73: return "Loyalty Merchant"; case 74: return "Tribute Master";
	default: return nullptr;
	}
}

const char* RaceName(uint8_t* spawn) {
	void** vtbl = *reinterpret_cast<void***>(spawn);
	if (!vtbl || !*g_dbstrTable) return nullptr;
	int race = reinterpret_cast<int (*)(uint8_t*)>(vtbl[kVtblGetRace / sizeof(void*)])(spawn);
	if (race <= 0) return nullptr;
	bool found = false;
	const char* name = g_dbstr(*g_dbstrTable, static_cast<unsigned>(race), kDbstrRaceName, &found);
	return (found && name && name[0]) ? name : nullptr;
}

// The con colour the Target window itself uses for its HP bar tint.
uint32_t ConColour(uint8_t* target) {
	int con = g_getConLevel(*g_pLocalPC, target);
	return g_userColour(g_conColourIndex(con)) | 0xFF000000u;
}

// A child window's screen rect: the GetScreenRect virtual when its slot holds the one we resolved, else
// the cached rect that function returns (fresh: the window was just laid out and drawn).
void ChildScreenRect(uint8_t* child, int* rect) {
	void** vtbl = *reinterpret_cast<void***>(child);
	if (vtbl && vtbl[kVtblGetScreenRect / sizeof(void*)] == reinterpret_cast<void*>(g_getScreenRect)) {
		g_getScreenRect(child, rect);
		return;
	}
	memcpy(rect, child + kWndScreenRectCache, 4 * sizeof(int));
}

// The first row of `lineH` pixels at or below `y` inside `client` that no visible child of `wnd` starting
// above the row overlaps (the HP gauge and its labels); a child that starts below the row (the buff box)
// is left alone, since it is drawn after us.
int FreeRow(uint8_t* wnd, const int* client, int y, int lineH) {
	for (int pass = 0; pass < 16; ++pass) {
		bool moved = false;
		for (uint8_t* child = *reinterpret_cast<uint8_t**>(wnd + kWndFirstChild); child && !moved;
		     child = *reinterpret_cast<uint8_t**>(child + kWndNextSibling)) {
			if (!child[kWndVisible]) continue;
			int r[4] = {};
			ChildScreenRect(child, r);
			if (r[2] <= r[0] || r[3] <= r[1]) continue;
			if (r[2] <= client[0] || r[0] >= client[2]) continue;    // beside the client area
			if (r[1] > y || r[3] <= y) continue;                      // starts below the row, or ends above it
			y = r[3] + 1;
			moved = true;
		}
		if (!moved) break;
	}
	return y + lineH <= client[3] ? y : client[1] + g_cfg.distOffsetY;
}

// Inside CTargetWnd::Draw, after the window painted its HP box.
void DrawTargetLine(uint8_t* mgr, uint8_t* wnd) {
	uint8_t* me = *g_pLocalPlayer;
	uint8_t* target = *g_pTarget;
	if (!target || target == me || !wnd[kWndVisible]) return;
	// Only call the rect virtuals when the slots still hold the functions we resolved.
	void** vtbl = *reinterpret_cast<void***>(wnd);
	if (!vtbl || vtbl[kVtblGetScreenRect / sizeof(void*)] != reinterpret_cast<void*>(g_getScreenRect)) return;
	if (vtbl[kVtblGetClientRect / sizeof(void*)] != reinterpret_cast<void*>(g_getClientRect)) return;
	uint8_t* font = Font(mgr, g_cfg.distFont);
	if (!font) return;
	int client[4] = {};
	g_getClientRect(wnd, client);
	if (client[2] - client[0] < 16 || client[3] - client[1] < 8) return;
	int lineH = LineHeight(font);
	int y = client[1] + g_cfg.distOffsetY;
	if (g_cfg.distAutoPlace) y = FreeRow(wnd, client, y, lineH);
	char text[160];
	snprintf(text, sizeof(text), "%.2f", Distance(me, target));
	int distWidth = TextWidth(font, text);
	int distX = client[2] + g_cfg.distOffsetX - distWidth;
	if (distX < client[0]) distX = client[0];
	if (g_cfg.distConLine) {
		const char* race = RaceName(target);
		const char* cls = ClassName(*reinterpret_cast<int*>(target + kSpawnClass));
		char con[160];
		snprintf(con, sizeof(con), "%d%s%.47s%s%.47s", target[kSpawnLevel], race ? " " : "", race ? race : "",
		         cls ? " " : "", cls ? cls : "");
		int clip[4] = { client[0], client[1], distX - 4, client[3] };   // never under the distance
		if (clip[2] > clip[0]) DrawStringClipped(font, con, client[0] + g_cfg.distConOffsetX, y, clip, ConColour(target));
	}
	DrawStringClipped(font, text, distX, y, client, g_cfg.distColour);
}

int64_t TargetDrawDetour(uint8_t* wnd) {
	int64_t result = g_origTargetDraw(wnd);
	if (g_inTargetDraw || !g_distOn) return result;
	g_inTargetDraw = true;
	uint8_t* mgr = *g_wndMgr;
	if (InGame(mgr)) DrawTargetLine(mgr, wnd);
	g_inTargetDraw = false;
	return result;
}

void DrawHud(uint8_t* mgr, uint8_t* me) {
	uint8_t* font = Font(mgr, g_cfg.font);
	if (!font) return;
	const int lineH = LineHeight(font);
	int y = g_cfg.y;
	char text[kTextMax];
	if (g_cfg.loc) {
		snprintf(text, sizeof(text), "Loc: %.2f, %.2f, %.2f", *reinterpret_cast<float*>(me + kSpawnY),
		         *reinterpret_cast<float*>(me + kSpawnX), *reinterpret_cast<float*>(me + kSpawnZ));
		DrawString(mgr, font, text, g_cfg.x, y, g_cfg.colour);
		y += lineH;
	}
	if (g_cfg.zone) {
		const char* name = reinterpret_cast<const char*>(g_zoneInfo + kZoneLongName);
		if (!name[0]) name = reinterpret_cast<const char*>(g_zoneInfo + kZoneShortName);
		snprintf(text, sizeof(text), "Zone: %.*s", 0x7f, name);
		DrawString(mgr, font, text, g_cfg.x, y, g_cfg.colour);
		y += lineH;
	}
	if (g_cfg.target) {
		uint8_t* target = *g_pTarget;
		if (target) snprintf(text, sizeof(text), "Target: %.*s (%.1f)", 0x3f, SpawnName(target), Distance(me, target));
		else snprintf(text, sizeof(text), "Target: -");
		DrawString(mgr, font, text, g_cfg.x, y, g_cfg.colour);
		y += lineH;
	}
	if (g_cfg.fps) {
		snprintf(text, sizeof(text), "FPS: %.0f", g_fps);
		DrawString(mgr, font, text, g_cfg.x, y, g_cfg.colour);
		y += lineH;
	}
	if (g_cfg.clock) {
		SYSTEMTIME t;
		GetLocalTime(&t);
		snprintf(text, sizeof(text), "%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
		DrawString(mgr, font, text, g_cfg.x, y, g_cfg.colour);
	}
}

int DrawWindowsDetour(uint8_t* mgr) {
	int result = g_origDrawWindows(mgr);
	if (g_inDraw) return result;
	g_inDraw = true;
	TickFps();
	if (g_hudOn && InGame(mgr)) DrawHud(mgr, *g_pLocalPlayer);
	g_inDraw = false;
	return result;
}

#define REF(var) reinterpret_cast<void**>(&var)

// CXWndManager::DrawWindows FUN_1405cc3a0. Stolen (5 bytes, position independent): mov [rsp+18],rbx.
const char* kDrawWindowsSig =
	"48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 50";

// Inside FUN_1401a4620: mov rdx,r10; lea rcx,[rsp+50]; call CXStr::CXStr(const char*); nop;
// mov rax,[CXWndManager]; lea rcx,[rax+190]; cmp [rcx],2; jle.
const char* kWriteTextCtorSig =
	"49 8B D2 48 8D 4C 24 50 E8 ?? ?? ?? ?? 90 48 8B 05 ?? ?? ?? ?? 48 8D 88 90 01 00 00 83 39 02 7E 71";

// Same function: mov rcx,[r10+10] (fonts[2]); call DrawWrappedText; nop; mov rdx,[rsp+50]; test rdx,rdx;
// je; lea rcx,[rsp+50]; call CXStr release; nop; mov eax,ebp.
const char* kWriteTextDrawSig =
	"49 8B 4A 10 E8 ?? ?? ?? ?? 90 48 8B 54 24 50 48 85 D2 74 0B 48 8D 4C 24 50 E8 ?? ?? ?? ?? 90 8B C5";

// Inside DrawWindows (tooltip): mov rcx,[rbx+1a8]; call FontHeight; add eax,2; mov [rbp+3c],eax; lea rdx,[rbp+38].
const char* kFontHeightSig = "48 8B 8B A8 01 00 00 E8 ?? ?? ?? ?? 83 C0 02 89 45 3C 48 8D 55 38";

// mov rcx,[pEverQuest]; cmp dword [rcx+5e4],5 (the in-game test, FUN_140109128).
const char* kGameStateSig = "48 8B 0D ?? ?? ?? ?? 83 B9 E4 05 00 00 05";

// CTargetWnd per-frame FUN_14050e150: mov [rax+13bc],-1; mov rdx,[pTarget]; mov rcx,[pLocalPC]; call GetConLevel.
const char* kTargetWndConSig =
	"C7 80 BC 13 00 00 FF FF FF FF 48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";

// Pet-owner lookup inside SetNameSpriteTint FUN_1403014a0: ... mov rcx,[pLocalPlayer]; cmp [rax+168],[rcx+168].
const char* kSpawnMgrSig =
	"48 8B 0D ?? ?? ?? ?? 48 85 C9 74 6E 48 8B 01 48 8B 97 B8 01 00 00 FF 50 18 48 8B D8 48 85 C0 74 59 "
	"48 8B 0D ?? ?? ?? ?? 8B 91 68 01 00 00 39 90 68 01 00 00";

// Zone init: mov byte [zoneInfo],0; mov dword [..],1.0f; mov qword [..],0.
const char* kZoneInfoSig =
	"C6 05 ?? ?? ?? ?? 00 C7 05 ?? ?? ?? ?? 00 00 80 3F 48 C7 05 ?? ?? ?? ?? 00 00 00 00";

// CTargetWnd::Draw FUN_14050db60 (vtbl+0x20): push rbx; sub rsp,20; cmp [pTarget],0; mov rbx,rcx;
// mov rdx,[rcx+388] (the first HP box animation). Stolen (6 bytes, position independent): push rbx; sub rsp,20.
const char* kTargetDrawSig = "40 53 48 83 EC 20 48 83 3D ?? ?? ?? ?? 00 48 8B D9 48 8B 91 88 03 00 00";

} // namespace

const HookSite kHudHooks[1] = {
	{ kDrawWindowsSig, 5, reinterpret_cast<void*>(&DrawWindowsDetour), REF(g_origDrawWindows) },
};

const HookSite kTargetDistanceHooks[1] = {
	{ kTargetDrawSig, 6, reinterpret_cast<void*>(&TargetDrawDetour), REF(g_origTargetDraw) },
};

const RefSite kHudRefs[22] = {
	// CXStr::CXStr(CXStr*, const char*) FUN_1405853f0: strlen loop, alloc rep, copy to rep+0x18
	{ "48 89 74 24 18 57 48 83 EC 20 48 8B F2 48 8B F9 48 85 D2 74 ?? 48 89 5C 24 30", 0, 0, REF(g_cxstrCtor) },
	// CXStr release FUN_140587670: critical-section guarded FreeRep
	{ "48 89 5C 24 08 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B DA 48 8B F9 74 ?? 8B 05 ?? ?? ?? ??",
	  0, 0, REF(g_cxstrRelease) },
	// CTextureFont::DrawWrappedText FUN_1405d1990(font, CXStr*, x, y, maxWidth, clip, argb, flags, offset)
	{ "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B DA", 0, 0, REF(g_drawWrapped) },
	// CXWnd::GetScreenRect FUN_1405a7bb0 (vtbl+0x218): cached at +0x5c..+0x68, recomputed when +0x228 set
	{ "48 89 5C 24 08 57 48 83 EC 50 80 B9 28 02 00 00 00", 0, 0, REF(g_getScreenRect) },
	{ kWriteTextCtorSig, 9, 13, REF(g_cxstrCtorCalled) },
	{ kWriteTextCtorSig, 17, 21, REF(g_wndMgr) },
	{ kWriteTextDrawSig, 5, 9, REF(g_drawWrappedCalled) },
	{ kWriteTextDrawSig, 26, 30, REF(g_cxstrReleaseCalled) },
	{ kFontHeightSig, 8, 12, REF(g_fontHeight) },
	{ kGameStateSig, 3, 7, REF(g_pEverQuest) },
	{ kTargetWndConSig, 13, 17, REF(g_pTarget) },
	{ kTargetWndConSig, 20, 24, REF(g_pLocalPC) },
	{ kTargetWndConSig, 25, 29, REF(g_getConLevelCalled) },
	// GetConLevel FUN_1402d7fa0(pLocalPC, spawn): corpse test on +0x135, vtbl+0xa0 untargetable test
	{ "48 89 5C 24 18 57 48 83 EC 20 80 BA 35 01 00 00 02 48 8B FA 48 8B D9 0F 83 ?? ?? ?? ?? "
	  "48 8B 02 48 8B CA FF 90 A0 00 00 00 84 C0", 0, 0, REF(g_getConLevel) },
	// FUN_140661f70(con): jump table returning 0x186..0x18c
	{ "FF C9 83 F9 06 77 3A 48 63 C1 48 8D 15 ?? ?? ?? ?? 8B 8C 82 ?? ?? ?? ?? 48 03 CA FF E1 "
	  "B8 86 01 00 00 C3 B8 87 01 00 00 C3", 0, 0, REF(g_conColourIndex) },
	// FUN_14050b6d0(index): user colour table lookup, 0xf0f0f0 fallback, R/B swap
	{ "48 83 EC 28 81 F9 FF 00 00 00 7E 32 E8 ?? ?? ?? ?? 85 C0 B9 F0 F0 F0 00 0F 44 C1 8B C8 C1 E9 10",
	  0, 0, REF(g_userColour) },
	// FUN_14018b660(table, id, type, bool* found): dbstr lookup
	{ "48 89 5C 24 08 57 48 83 EC 20 44 8B DA", 0, 0, REF(g_dbstr) },
	// dbstr table creation in FUN_14027cea0: mov [dbstrTable],rdi; mov ecx,20
	{ "48 89 3D ?? ?? ?? ?? B9 20 00 00 00", 3, 7, REF(g_dbstrTable) },
	{ kSpawnMgrSig, 36, 40, REF(g_pLocalPlayer) },
	{ kZoneInfoSig, 2, 7, REF(g_zoneInfo) },
	// CXWnd::GetClientRect FUN_1405a7ce0 (vtbl+0x210): cached at +0x1e8, recomputed when +0xe4 set
	{ "48 89 5C 24 08 57 48 83 EC 30 80 B9 E4 00 00 00 00", 0, 0, REF(g_getClientRect) },
	// FUN_14059bf40(fontIndex, CXStr*): text width through the font object the renderer hands out (the
	// target of the 8-byte CTextureFont::GetWidth thunk FUN_1405d1e70, which is not signaturable itself)
	{ "40 53 48 83 EC 20 48 8B DA 48 C7 44 24 40 00 00 00 00", 0, 0, REF(g_textWidth) },
};

namespace {

const char* CommonPrepare() {
	if (g_cxstrCtorCalled != reinterpret_cast<void*>(g_cxstrCtor))
		return "the client's text helper does not call the CXStr constructor we resolved";
	if (g_drawWrappedCalled != reinterpret_cast<void*>(g_drawWrapped))
		return "the client's text helper does not call the DrawWrappedText we resolved";
	if (g_cxstrReleaseCalled != reinterpret_cast<void*>(g_cxstrRelease))
		return "the client's text helper does not call the CXStr release we resolved";
	// FUN_1405d1e30 is the thunk `mov ecx,[rcx+8]; jmp FUN_14059bdc0`.
	static const uint8_t kThunk[4] = { 0x8B, 0x49, 0x08, 0xE9 };
	if (memcmp(reinterpret_cast<const void*>(g_fontHeight), kThunk, sizeof(kThunk)) != 0)
		return "the font height call inside DrawWindows is not the expected thunk";
	if (g_getConLevelCalled != reinterpret_cast<void*>(g_getConLevel))
		return "the target window's con call does not point at GetConLevel";
	// The width function and the height function behind the thunk both start by loading the same
	// renderer global (the object whose vtbl+0x140 hands out the font by index).
	const uint8_t* heightFn = reinterpret_cast<const uint8_t*>(g_fontHeight);
	int32_t rel;
	memcpy(&rel, heightFn + 4, 4);
	const uint8_t* heightTarget = heightFn + 8 + rel;
	const void* a = FirstGlobalLoad(reinterpret_cast<const uint8_t*>(g_textWidth), 73);
	const void* b = FirstGlobalLoad(heightTarget, 96);
	if (!a || a != b)
		return "the text width function does not read the renderer global the height function reads";
	LoadConfig();
	return nullptr;
}

} // namespace

const char* HudPrepare() {
	const char* e = CommonPrepare();
	if (!e) g_hudOn = true;
	return e;
}

const char* TargetDistancePrepare() {
	const char* e = CommonPrepare();
	if (!e) g_distOn = true;
	return e;
}
