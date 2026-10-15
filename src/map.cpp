// MapSpawns: spawn dots and name labels on the native map window, plus a line to the current target.
//
// The map (MapViewMap, embedded in CMapViewWnd at +0x4B8) keeps its lines and labels in two intrusive
// lists (+0x2F8 lines, +0x300 labels) and draws them from MapViewMap::PostDraw (FUN_140455a60, vtable
// slot 5): layer mask, Z filter, hover highlight, dot and text all come from the node (MAPLABEL 0x50 /
// MAPLINE 0x30, same shape as RoF2 with 8-byte pointers). We hook PostDraw with an inline hook, and per
// frame:
//   1. walk the live spawn list (SpawnManager FUN_140309350 / spawn+0x10), filter with the client's own
//      tests (targetable flags, FUN_140315770 "is invisible to me", body types, object races), pick the
//      nearest N, and refresh our fixed pool of label nodes (text rebuilt and re-measured with the
//      client's FUN_140455db0 only when it changes);
//   2. splice our nodes in front of the client's list heads, run the original PostDraw, and restore the
//      heads before returning. The client's clear/remove/select code never sees our nodes, so it can never
//      free them; our nodes never point at client memory after the call. If the draw left one of our
//      nodes in the hover globals (DAT_140efd758/DAT_140efd760, read by the mouse handlers), we clear it.
// Spawn pointers are never kept across frames; the pool is keyed by spawn id and reset on zone change.
// Nothing is allocated at runtime.
#include "map.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// PlayerClient (spawn) fields, verified in the Spawn_Struct reader FUN_1403040d0 and the map draw.
const size_t kSpawnNext = 0x10;          // next PlayerClient* in the manager's list
const size_t kSpawnY = 0x74;
const size_t kSpawnX = 0x78;
const size_t kSpawnZ = 0x7c;
const size_t kSpawnName = 0xb4;          // char[0x40]
const size_t kSpawnDisplayedName = 0xf4; // char[0x40] cleaned name (FUN_140314690)
const size_t kSpawnType = 0x135;         // u8: 0 PC, 1 NPC, 2+ corpse
const size_t kSpawnId = 0x168;           // u32
const size_t kSpawnTargetable = 0x18c;   // u8 (vtbl+0x170 setter)
const size_t kSpawnUntargetable = 0x18e; // u8 click-through (vtbl+0x180 setter)
const size_t kSpawnAnon = 0x244;         // u32: 0 normal, 1 anon, 2 roleplay
const size_t kSpawnIsPet = 0x24c;        // u8
const size_t kSpawnMasterId = 0x2d8;     // u32 pet owner spawn id
const size_t kSpawnDeity = 0x370;        // u32: 0 for NPC corpses
const size_t kSpawnLevel = 0x494;        // u8
const size_t kSpawnIsMercenary = 0x5f6;  // u8
const size_t kSpawnZoneId = 0x62c;       // u32
const size_t kVtblGetRace = 0x110;       // >= 0x8cd are objects/banners
const size_t kSpawnMgrVtblFirst = 0x8;   // SpawnManager::GetFirstSpawn()

// MapViewMap fields (relative to the object PostDraw receives).
const size_t kMapLines = 0x2f8;          // MAPLINE* head
const size_t kMapLabels = 0x300;         // MAPLABEL* head
const size_t kMapRefit = 0x37c;          // u8: re-fit the view to the labels on this draw
const size_t kMapZoneId = 0x380;         // u32 zone the loaded map belongs to
const size_t kMapVtblPostDraw = 5;

// pLocalPC (DAT_140e65d90): group at +0x2E30, members at +8+i*8, member spawn at +0x58 (map draw).
const size_t kPcGroup = 0x2e30;
const size_t kGroupMemberSpawn = 0x58;
const int kGroupMembers = 6;

// Body types that the client treats as triggers/traps/untargetable markers.
const int kHiddenBodyTypes[] = { 11, 65, 66, 67, 100, 101, 102, 103 };

#pragma pack(push, 1)
struct MapLabel {                 // client MAPLABEL, 0x50 bytes
	uint32_t id;
	uint32_t pad0;
	MapLabel* next;
	MapLabel* prev;
	float loc[3];                 // map space: x = -spawn.X, y = -spawn.Y, z = spawn.Z
	uint32_t colour;              // ARGB, dot and text
	int size;                     // 1..3 = font index + 1; FUN_140455db0 aborts on anything else
	uint32_t pad1;
	char* text;
	uint32_t layer;               // 0..3
	int width;                    // filled by FUN_140455db0
	int height;
	int offsetX;                  // 0,0 = text centred above the dot
	int offsetY;
	uint32_t pad2;
};
struct MapLine {                  // client MAPLINE, 0x30 bytes
	MapLine* next;
	MapLine* prev;
	float start[3];
	float end[3];
	uint32_t colour;
	uint32_t layer;
};
#pragma pack(pop)
static_assert(sizeof(MapLabel) == 0x50, "MAPLABEL layout");
static_assert(offsetof(MapLabel, next) == 0x08 && offsetof(MapLabel, loc) == 0x18 &&
              offsetof(MapLabel, colour) == 0x24 && offsetof(MapLabel, text) == 0x30 &&
              offsetof(MapLabel, layer) == 0x38 && offsetof(MapLabel, offsetY) == 0x48, "MAPLABEL layout");
static_assert(sizeof(MapLine) == 0x30 && offsetof(MapLine, colour) == 0x28, "MAPLINE layout");

using PostDrawFn = uint64_t (*)(uint8_t* map);
using GetConLevelFn = int (*)(void* pLocalPC, uint8_t* spawn);
using ConColourIndexFn = int (*)(int con);
using UserColourFn = uint32_t (*)(int index);
using IsInvisibleToFn = char (*)(uint8_t* me, uint8_t* spawn, char lenient);
using MeasureLabelFn = void (*)(uint8_t* map, MapLabel* label);
using HasBodyTypeFn = char (*)(uint8_t* spawn, int bodyType, int zero1, int zero2); // client passes (bt, 0, 0)

PostDrawFn g_origPostDraw = nullptr;       // trampoline into FUN_140455a60
uint8_t** g_everQuest = nullptr;           // &DAT_140ee7010; +0x5e4 = game state, 5 = in a zone

// Only touch game data in a zone: at character select and while loading, windows and spawns exist
// but the data behind them isn't ready.
bool InGame() {
	return *g_everQuest && *reinterpret_cast<int*>(*g_everQuest + 0x5e4) == 5;
}
GetConLevelFn g_getConLevel = nullptr;     // FUN_1402d7fa0
ConColourIndexFn g_conColourIndex = nullptr; // FUN_140661f70
UserColourFn g_userColour = nullptr;       // FUN_14050b6d0
IsInvisibleToFn g_isInvisibleTo = nullptr; // FUN_140315770: non-zero = the client hides the spawn from me
MeasureLabelFn g_measureLabel = nullptr;   // FUN_140455db0
HasBodyTypeFn g_hasBodyType = nullptr;     // FUN_140637a50
void* g_getConLevelCalled = nullptr;       // target of the GetConLevel call next to the pLocalPC load
void* g_postDrawFn = nullptr;              // FUN_140455a60, to cross-check against the vtable
void** g_mapVtbl = nullptr;                // PTR_LAB_140a26210 (MapViewMap vtable)
void** g_pLocalPC = nullptr;               // &DAT_140e65d90
uint8_t** g_pLocalPlayer = nullptr;        // &DAT_140e66228
uint8_t** g_pTarget = nullptr;             // &DAT_140e66230
uint8_t** g_spawnManager = nullptr;        // &DAT_140ee4b40
MapLine** g_hoverLine = nullptr;           // &DAT_140efd758, nearest line to the cursor (set by the draw)
MapLabel** g_hoverLabel = nullptr;         // &DAT_140efd760, nearest label to the cursor
uint8_t* g_selectBoxActive = nullptr;      // &DAT_140efd6fc, drag-select box in progress

template <typename R, typename... A>
R VCall(void* obj, size_t slot, A... args) {
	auto fn = *reinterpret_cast<R (**)(void*, A...)>(*reinterpret_cast<uint8_t**>(obj) + slot);
	return fn(obj, args...);
}

// ---- configuration ([Map] in dinput8.ini) ----

const int kMaxLabels = 256;   // pool size; MaxLabels is clamped to it
const int kTextMax = 96;

struct Config {
	int maxLabels = 100;
	int maxDistance = 0;        // 0 = no limit
	int labelSize = 3;          // 1..3: the draw keeps a three-entry font table (fonts[0..2]) on its stack
	int layer = 3;              // 0..3, toggled with the map's layer buttons
	bool showLevel = false;
	bool showNPCs = true;
	bool showPCs = true;
	bool showGroup = true;
	bool showPets = true;
	bool showMercs = true;
	int showCorpses = 1;        // 0 none, 1 player corpses, 2 all
	bool targetLine = true;
	uint32_t colourPC = 0xFFFF00FF;
	uint32_t colourGroup = 0xFF00FF00;
	uint32_t colourPet = 0xFFFF8000;
	uint32_t colourMerc = 0xFFE1B511;
	uint32_t colourPCCorpse = 0xFFFFC0FF;
	uint32_t colourNPCCorpse = 0xFF808080;
	uint32_t colourTargetLine = 0xFFFF0000;
	uint32_t colourGreyCon = 0xFF505050; // grey-con NPCs (the Options grey is too light on the parchment)
};
Config g_cfg;

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void LoadConfig() {
	char ini[MAX_PATH];
	GetModuleFileNameA(nullptr, ini, MAX_PATH);
	char* slash = strrchr(ini, '\\');
	if (slash) *slash = '\0';
	strcat_s(ini, "\\dinput8.ini");
	auto Int = [&](const char* key, int def) { return static_cast<int>(GetPrivateProfileIntA("Map", key, def, ini)); };
	auto Colour = [&](const char* key, uint32_t def) {
		char buf[32] = {};
		GetPrivateProfileStringA("Map", key, "", buf, sizeof(buf), ini);
		if (!buf[0]) return def;
		return static_cast<uint32_t>(strtoul(buf, nullptr, 16)) | 0xFF000000u;
	};
	Config c;
	c.maxLabels = Clamp(Int("MaxLabels", c.maxLabels), 1, kMaxLabels);
	c.maxDistance = Clamp(Int("MaxDistance", c.maxDistance), 0, 100000);
	c.labelSize = Clamp(Int("LabelSize", c.labelSize), 1, 3);
	c.layer = Clamp(Int("Layer", c.layer), 0, 3);
	c.showLevel = Int("ShowLevel", c.showLevel) != 0;
	c.showNPCs = Int("ShowNPCs", c.showNPCs) != 0;
	c.showPCs = Int("ShowPCs", c.showPCs) != 0;
	c.showGroup = Int("ShowGroup", c.showGroup) != 0;
	c.showPets = Int("ShowPets", c.showPets) != 0;
	c.showMercs = Int("ShowMercs", c.showMercs) != 0;
	c.showCorpses = Clamp(Int("ShowCorpses", c.showCorpses), 0, 2);
	c.targetLine = Int("TargetLine", c.targetLine) != 0;
	c.colourPC = Colour("ColourPC", c.colourPC);
	c.colourGroup = Colour("ColourGroup", c.colourGroup);
	c.colourPet = Colour("ColourPet", c.colourPet);
	c.colourMerc = Colour("ColourMerc", c.colourMerc);
	c.colourPCCorpse = Colour("ColourPCCorpse", c.colourPCCorpse);
	c.colourNPCCorpse = Colour("ColourNPCCorpse", c.colourNPCCorpse);
	c.colourTargetLine = Colour("ColourTargetLine", c.colourTargetLine);
	c.colourGreyCon = Colour("ColourGreyCon", c.colourGreyCon);
	g_cfg = c;
}

// ---- node pool ----

struct Slot {
	MapLabel node;
	char text[kTextMax];
	uint32_t spawnId;    // 0 = free
	bool measured;       // width/height valid for the current text
	bool used;           // claimed this frame
	bool chained;        // already in this frame's list (guards against a repeated spawn id)
};
Slot g_slots[kMaxLabels];
MapLine g_targetLine;
Slot* g_chain[kMaxLabels];   // slots spliced in this frame, in list order
int g_chainCount = 0;
uint32_t g_lastZone = 0xFFFFFFFF;
bool g_inDraw = false;

struct Candidate {
	uint8_t* spawn;
	float dist2;
	uint32_t colour;
	bool withLevel;
};
Candidate g_candidates[kMaxLabels];
int g_candidateCount = 0;

void ResetPool() {
	for (Slot& s : g_slots) {
		s.spawnId = 0;
		s.measured = false;
		s.used = false;
		s.text[0] = '\0';
		s.node.next = nullptr;
		s.node.prev = nullptr;
	}
	g_chainCount = 0;
}

void InitPool() {
	memset(g_slots, 0, sizeof(g_slots));
	for (Slot& s : g_slots) {
		s.node.text = s.text;
		s.node.size = g_cfg.labelSize;
		s.node.layer = g_cfg.layer;
	}
	memset(&g_targetLine, 0, sizeof(g_targetLine));
	g_targetLine.colour = g_cfg.colourTargetLine;
	g_targetLine.layer = g_cfg.layer;
	g_lastZone = 0xFFFFFFFF;
}

bool IsOurLabel(const MapLabel* p) {
	return p >= &g_slots[0].node && p <= &g_slots[kMaxLabels - 1].node;
}

// Keeps the nearest MaxLabels candidates (replace the farthest once full).
void AddCandidate(const Candidate& c) {
	if (g_candidateCount < g_cfg.maxLabels) {
		g_candidates[g_candidateCount++] = c;
		return;
	}
	int farthest = 0;
	for (int i = 1; i < g_candidateCount; ++i)
		if (g_candidates[i].dist2 > g_candidates[farthest].dist2) farthest = i;
	if (c.dist2 < g_candidates[farthest].dist2) g_candidates[farthest] = c;
}

// ---- spawn classification ----

bool IsGroupMember(uint8_t* spawn) {
	uint8_t* pc = static_cast<uint8_t*>(*g_pLocalPC);
	if (!pc) return false;
	uint8_t* group = *reinterpret_cast<uint8_t**>(pc + kPcGroup);
	if (!group) return false;
	for (int i = 0; i < kGroupMembers; ++i) {
		uint8_t* member = *reinterpret_cast<uint8_t**>(group + 8 + i * 8);
		if (member && *reinterpret_cast<uint8_t**>(member + kGroupMemberSpawn) == spawn) return true;
	}
	return false;
}

// Decides whether a spawn goes on the map and in which colour. Uses the client's own visibility
// decision (FUN_140315770) so nothing the client hides from this player is shown.
bool Classify(uint8_t* spawn, uint8_t* me, Candidate* out) {
	if (spawn == me) return false;
	if (!spawn[kSpawnTargetable] || spawn[kSpawnUntargetable]) return false;
	if (VCall<int>(spawn, kVtblGetRace) >= 0x8cd) return false;
	if (g_isInvisibleTo(me, spawn, 0)) return false;
	for (int bt : kHiddenBodyTypes)
		if (g_hasBodyType(spawn, bt, 0, 0)) return false;

	uint8_t type = spawn[kSpawnType];
	out->withLevel = g_cfg.showLevel;
	if (type >= 2) {
		bool pcCorpse = *reinterpret_cast<uint32_t*>(spawn + kSpawnDeity) != 0;
		if (g_cfg.showCorpses == 0 || (g_cfg.showCorpses == 1 && !pcCorpse)) return false;
		out->colour = pcCorpse ? g_cfg.colourPCCorpse : g_cfg.colourNPCCorpse;
		out->withLevel = false;
	}
	else if (type == 0) {
		if (IsGroupMember(spawn)) {
			if (!g_cfg.showGroup) return false;
			out->colour = g_cfg.colourGroup;
		}
		else {
			if (!g_cfg.showPCs) return false;
			out->colour = g_cfg.colourPC;
		}
		if (*reinterpret_cast<uint32_t*>(spawn + kSpawnAnon) != 0) out->withLevel = false;
	}
	else if (spawn[kSpawnIsMercenary]) {
		if (!g_cfg.showMercs) return false;
		out->colour = g_cfg.colourMerc;
	}
	else if (spawn[kSpawnIsPet] || *reinterpret_cast<uint32_t*>(spawn + kSpawnMasterId) != 0) {
		if (!g_cfg.showPets) return false;
		out->colour = g_cfg.colourPet;
	}
	else {
		if (!g_cfg.showNPCs) return false;
		void* pc = *g_pLocalPC;
		if (!pc) return false;
		int con = g_getConLevel(pc, spawn);   // 0 = not conable
		if (con < 1 || con > 7) return false;
		out->colour = con == 1 ? g_cfg.colourGreyCon : (g_userColour(g_conColourIndex(con)) | 0xFF000000u);
	}
	out->spawn = spawn;
	return true;
}

void CollectCandidates(uint8_t* me) {
	g_candidateCount = 0;
	uint8_t* mgr = *g_spawnManager;
	uint8_t* spawn = mgr ? VCall<uint8_t*>(mgr, kSpawnMgrVtblFirst) : nullptr;
	const float meX = *reinterpret_cast<float*>(me + kSpawnX);
	const float meY = *reinterpret_cast<float*>(me + kSpawnY);
	const float maxDist2 = static_cast<float>(g_cfg.maxDistance) * static_cast<float>(g_cfg.maxDistance);
	for (int guard = 0; spawn && guard < 100000; ++guard) {
		Candidate c;
		float dx = *reinterpret_cast<float*>(spawn + kSpawnX) - meX;
		float dy = *reinterpret_cast<float*>(spawn + kSpawnY) - meY;
		c.dist2 = dx * dx + dy * dy;
		if ((g_cfg.maxDistance == 0 || c.dist2 <= maxDist2) && Classify(spawn, me, &c)) AddCandidate(c);
		spawn = *reinterpret_cast<uint8_t**>(spawn + kSpawnNext);
	}
}

Slot* FindSlot(uint32_t spawnId) {
	for (Slot& s : g_slots)
		if (s.spawnId == spawnId) return &s;
	return nullptr;
}

Slot* FreeSlot() {
	for (Slot& s : g_slots)
		if (!s.used) return &s;
	return nullptr;
}

void BuildText(const Candidate& c, char* out) {
	const char* name = reinterpret_cast<const char*>(c.spawn + kSpawnDisplayedName);
	if (!name[0]) name = reinterpret_cast<const char*>(c.spawn + kSpawnName);
	if (c.withLevel) snprintf(out, kTextMax, "%.*s (%d)", 0x3f, name, c.spawn[kSpawnLevel]);
	else snprintf(out, kTextMax, "%.*s", 0x3f, name);
	if (!out[0]) strcpy_s(out, kTextMax, "?");   // the client never measures or draws an empty label
}

// Refreshes the pool from this frame's candidates: existing slots keep their text (and measurement)
// when nothing changed; spawns that left are released.
void UpdateSlots(uint8_t* map) {
	for (Slot& s : g_slots) s.used = s.chained = false;
	Slot* chosen[kMaxLabels];
	for (int i = 0; i < g_candidateCount; ++i) {
		uint32_t id = *reinterpret_cast<uint32_t*>(g_candidates[i].spawn + kSpawnId);
		Slot* s = id ? FindSlot(id) : nullptr;
		if (s) s->used = true;
		chosen[i] = s;
	}
	g_chainCount = 0;
	for (int i = 0; i < g_candidateCount; ++i) {
		const Candidate& c = g_candidates[i];
		Slot* s = chosen[i];
		if (!s) {
			s = FreeSlot();
			if (!s) break;
			s->used = true;
			s->spawnId = *reinterpret_cast<uint32_t*>(c.spawn + kSpawnId);
			s->text[0] = '\0';
			s->measured = false;
		}
		if (s->chained) continue;
		s->chained = true;
		char text[kTextMax];
		BuildText(c, text);
		if (strcmp(text, s->text) != 0) {
			strcpy_s(s->text, text);
			s->measured = false;
		}
		if (!s->measured) {
			g_measureLabel(map, &s->node);
			s->measured = true;
		}
		s->node.loc[0] = -*reinterpret_cast<float*>(c.spawn + kSpawnX);
		s->node.loc[1] = -*reinterpret_cast<float*>(c.spawn + kSpawnY);
		s->node.loc[2] = *reinterpret_cast<float*>(c.spawn + kSpawnZ);
		s->node.colour = c.colour;
		s->node.offsetX = 0;
		s->node.offsetY = 0;
		g_chain[g_chainCount++] = s;
	}
	for (Slot& s : g_slots)
		if (!s.used) s.spawnId = 0;
}

// ---- splice / unsplice around the original draw ----

uint64_t PostDrawDetour(uint8_t* map) {
	uint8_t* me = *g_pLocalPlayer;
	if (g_inDraw || !me || !*g_pLocalPC || !InGame()) return g_origPostDraw(map);
	uint32_t zone = *reinterpret_cast<uint32_t*>(me + kSpawnZoneId);
	if (zone != g_lastZone) {
		ResetPool();
		g_lastZone = zone;
	}
	// Only the current zone's map, and not on the draw that re-fits the view to the labels. The draw only
	// clears the re-fit flag when the map has labels of its own, so ignore it on a label-less map.
	bool refit = map[kMapRefit] == 1 && *reinterpret_cast<MapLabel**>(map + kMapLabels);
	if (*reinterpret_cast<uint32_t*>(map + kMapZoneId) != zone || refit) return g_origPostDraw(map);

	g_inDraw = true;
	CollectCandidates(me);
	UpdateSlots(map);

	MapLabel** labelHead = reinterpret_cast<MapLabel**>(map + kMapLabels);
	MapLine** lineHead = reinterpret_cast<MapLine**>(map + kMapLines);
	MapLabel* savedLabels = *labelHead;
	MapLine* savedLines = *lineHead;
	if (g_chainCount) {
		for (int i = 0; i < g_chainCount; ++i) {
			g_chain[i]->node.prev = i ? &g_chain[i - 1]->node : nullptr;
			g_chain[i]->node.next = i + 1 < g_chainCount ? &g_chain[i + 1]->node : savedLabels;
		}
		*labelHead = &g_chain[0]->node;
	}
	// The target line is left out while a drag-select box is active: the draw would record every line
	// inside the box in the map's selection list, which it later frees.
	uint8_t* target = *g_pTarget;
	bool lineOn = g_cfg.targetLine && target && target != me && !*g_selectBoxActive;
	if (lineOn) {
		g_targetLine.start[0] = -*reinterpret_cast<float*>(me + kSpawnX);
		g_targetLine.start[1] = -*reinterpret_cast<float*>(me + kSpawnY);
		g_targetLine.start[2] = *reinterpret_cast<float*>(me + kSpawnZ);
		g_targetLine.end[0] = -*reinterpret_cast<float*>(target + kSpawnX);
		g_targetLine.end[1] = -*reinterpret_cast<float*>(target + kSpawnY);
		g_targetLine.end[2] = *reinterpret_cast<float*>(target + kSpawnZ);
		g_targetLine.prev = nullptr;
		g_targetLine.next = savedLines;
		*lineHead = &g_targetLine;
	}

	uint64_t result = g_origPostDraw(map);

	*labelHead = savedLabels;
	*lineHead = savedLines;
	if (g_chainCount) g_chain[g_chainCount - 1]->node.next = nullptr;
	g_targetLine.next = nullptr;
	if (IsOurLabel(*g_hoverLabel)) *g_hoverLabel = nullptr;
	if (*g_hoverLine == &g_targetLine) *g_hoverLine = nullptr;
	g_inDraw = false;
	return result;
}

#define REF(var) reinterpret_cast<void**>(&var)

// MapViewMap::PostDraw FUN_140455a60. Stolen (6 bytes, position independent): push rbx; sub rsp,50.
// The next instruction loads the stack cookie RIP-relatively, so nothing more can be stolen.
const char* kPostDrawSig =
	"40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 41 "
	"48 8B 03 48 8D 54 24 30 48 8B CB FF 90 08 02 00 00";

// CTargetWnd per-frame FUN_14050e150: mov [rax+13bc],-1; mov rdx,[pTarget]; mov rcx,[pLocalPC];
// call GetConLevel. The call target is cross-checked against the GetConLevel found by prologue.
const char* kTargetWndConSig =
	"C7 80 BC 13 00 00 FF FF FF FF 48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";

// Pet-owner lookup inside SetNameSpriteTint FUN_1403014a0: mov rcx,[spawnManager]; ... call [rax+18]
// (GetSpawnByID); ... mov rcx,[pLocalPlayer]; cmp [rax+168],[rcx+168].
const char* kSpawnMgrSig =
	"48 8B 0D ?? ?? ?? ?? 48 85 C9 74 6E 48 8B 01 48 8B 97 B8 01 00 00 FF 50 18 48 8B D8 48 85 C0 74 59 "
	"48 8B 0D ?? ?? ?? ?? 8B 91 68 01 00 00 39 90 68 01 00 00";

// Start of the map draw FUN_1404507e0's hover pass: xor edx,edx; mov [hoverLine],rdx; mov [hoverLabel],rdx.
const char* kHoverSig = "33 D2 48 89 15 ?? ?? ?? ?? 48 89 15 ?? ?? ?? ?? 44 8B EA F3 44 0F 10 35";

// Same function, earlier: cmp byte [selectBoxActive],0; je; mov rax,[selectBoxStart]; mov [rbp+1e0],eax.
const char* kSelectBoxSig = "80 3D ?? ?? ?? ?? 00 74 45 48 8B 05 ?? ?? ?? ?? 89 85 E0 01 00 00 8B 0D";

// MapViewMap dtor FUN_14044fb20: lea rax,[vtable]; mov [rcx],rax; call clear; xor esi,esi; mov [rbx+358],rsi.
const char* kMapVtblSig = "48 8D 05 ?? ?? ?? ?? 48 89 01 E8 ?? ?? ?? ?? 33 F6 48 89 B3 58 03 00 00";

} // namespace

const HookSite kMapSpawnsHooks[1] = {
	{ kPostDrawSig, 6, reinterpret_cast<void*>(&PostDrawDetour), REF(g_origPostDraw) },
};

const RefSite kMapSpawnsRefs[17] = {
	// mov rcx,[pEverQuest]; cmp dword [rcx+5e4],5 (the client's in-game test)
	{ "48 8B 0D ?? ?? ?? ?? 83 B9 E4 05 00 00 05", 3, 7, REF(g_everQuest) },
	// GetConLevel FUN_1402d7fa0(pLocalPC, spawn): corpse test on +0x135, vtbl+0xa0 untargetable test
	{ "48 89 5C 24 18 57 48 83 EC 20 80 BA 35 01 00 00 02 48 8B FA 48 8B D9 0F 83 ?? ?? ?? ?? "
	  "48 8B 02 48 8B CA FF 90 A0 00 00 00 84 C0", 0, 0, REF(g_getConLevel) },
	// FUN_140661f70(con): jump table returning 0x186..0x18c
	{ "FF C9 83 F9 06 77 3A 48 63 C1 48 8D 15 ?? ?? ?? ?? 8B 8C 82 ?? ?? ?? ?? 48 03 CA FF E1 "
	  "B8 86 01 00 00 C3 B8 87 01 00 00 C3", 0, 0, REF(g_conColourIndex) },
	// FUN_14050b6d0(index): user colour table lookup, 0xf0f0f0 fallback, R/B swap
	{ "48 83 EC 28 81 F9 FF 00 00 00 7E 32 E8 ?? ?? ?? ?? 85 C0 B9 F0 F0 F0 00 0F 44 C1 8B C8 C1 E9 10",
	  0, 0, REF(g_userColour) },
	{ kTargetWndConSig, 13, 17, REF(g_pTarget) },
	{ kTargetWndConSig, 20, 24, REF(g_pLocalPC) },
	{ kTargetWndConSig, 25, 29, REF(g_getConLevelCalled) },
	{ kSpawnMgrSig, 3, 7, REF(g_spawnManager) },
	{ kSpawnMgrSig, 36, 40, REF(g_pLocalPlayer) },
	// FUN_140315770(me, spawn, lenient) -> non-zero when the client hides `spawn` from `me` (GM invis
	// 3001, body type 100, see-invis level vs +0x600, bazaar trader/buyer exceptions)
	{ "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 45 33 F6 40 32 F6 "
	  "45 0F B6 E0 48 8B DA 48 8B F9 45 8B FE 48 3B D1 75 31 45 84 C0", 0, 0, REF(g_isInvisibleTo) },
	// FUN_140455db0(map, label): Width/Height from CXWndManager fonts[Size-1]
	{ "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 4C 63 42 28 41 8D 50 FF 48 8B 0D ?? ?? ?? ?? "
	  "48 81 C1 90 01 00 00 3B 11 7D 5C 85 D2 78 58", 0, 0, REF(g_measureLabel) },
	// FUN_140637a50(spawn, bodyType, 0): add rcx,138; jmp set-lookup (followed by the +0x135 corpse test)
	{ "48 81 C1 38 01 00 00 E9 ?? ?? ?? ?? CC CC CC CC 80 B9 35 01 00 00 02", 0, 0, REF(g_hasBodyType) },
	{ kHoverSig, 5, 9, REF(g_hoverLine) },
	{ kHoverSig, 12, 16, REF(g_hoverLabel) },
	{ kSelectBoxSig, 2, 7, REF(g_selectBoxActive) },
	{ kMapVtblSig, 3, 7, REF(g_mapVtbl) },
	{ kPostDrawSig, 0, 0, REF(g_postDrawFn) },
};

const char* MapSpawnsPrepare() {
	if (g_getConLevelCalled != reinterpret_cast<void*>(g_getConLevel))
		return "the target window's con call does not point at GetConLevel";
	if (g_mapVtbl[kMapVtblPostDraw] != g_postDrawFn)
		return "MapViewMap vtable slot 5 is not the PostDraw we hook";
	LoadConfig();
	InitPool();
	return nullptr;
}
