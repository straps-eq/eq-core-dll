// ConNameTags: colour NPC name tags by con colour (grey/green/light blue/blue/white/yellow/red).
//
// The ToB client never cons a name tag: EQPlayer::SetNameSpriteTint (FUN_1403014a0) picks a fixed
// 0x00RRGGBB colour per category (NPC 0x17b8ad, merc, pet by owner, PC/PvP/LD/GM, target blink) and
// pushes it to the actor with actor->vtbl[+0x328](actor, &colour). We hook that function: run the
// original, then for plain NPCs compute the con colour with the client's own functions
// (GetConLevel FUN_1402d7fa0 -> con code 1..7, FUN_140661f70 -> user colour index 0x186..0x18c,
// FUN_14050b6d0 -> colour) and push it through the same actor call, exactly as the original does
// for a pet owner's colour.
//
// The client only re-tints a spawn when its name sprite is rebuilt (spawn appearance, /shownames,
// name options) or when the target changes; it is not per frame. The local player's level-up does
// not re-tint anybody natively, so we also watch the local level from the hook and, when it has
// changed, re-colour every NPC in the spawn list on the next tint (usually within a second: spawns
// coming into view rebuild their sprites constantly).
#include "nametags.h"
#include <cstdint>

namespace {

// EQPlayer (spawn) fields, verified in SetNameSpriteTint / SetNameSpriteState / GetConLevel.
const size_t kSpawnListNode = 0x10;      // object whose vtbl[+0x88]() returns the next spawn
const size_t kSpawnType = 0x135;         // u8: 0 PC, 1 NPC, 2+ corpse
const size_t kSpawnPetOwnerType = 0x1c0; // int: non-zero = pet (the client colours pets by owner)
const size_t kSpawnLevel = 0x494;        // u8 (what OP_SpawnAppearance type 1 writes)
const size_t kSpawnActor = 0x1170;       // CActorInterface*
const size_t kVtblGetActorDef = 0x78;    // EQPlayer::GetActorDef(); +0x5f6 = mercenary flag
const size_t kVtblGetRace = 0x110;       // EQPlayer::GetRace(); >= 0x8cd are objects/banners
const size_t kActorDefMercenary = 0x5f6;
const size_t kActorVtblSetNameColour = 0x328; // (actor, uint32_t* colour)
const size_t kSpawnMgrVtblFirst = 0x8;
const size_t kSpawnNodeVtblNext = 0x88;
const size_t kGmModeOwnerFlag = 0x5e4;   // *(int*)(DAT_140ee7010 + 0x5e4) == 1 = GM mode (yellow names)

using TintFn = uint64_t (*)(uint8_t* spawn);
using GetConLevelFn = int (*)(void* pEverQuest, uint8_t* spawn);
using ConColourIndexFn = int (*)(int con);
using UserColourFn = uint32_t (*)(int index);

TintFn g_origTint = nullptr;               // trampoline into FUN_1403014a0
GetConLevelFn g_getConLevel = nullptr;     // FUN_1402d7fa0
ConColourIndexFn g_conColourIndex = nullptr; // FUN_140661f70
UserColourFn g_userColour = nullptr;       // FUN_14050b6d0
void* g_getConLevelCalled = nullptr;       // target of the GetConLevel call next to the pEverQuest load
void** g_pEverQuest = nullptr;             // &DAT_140e65d90
uint64_t* g_gmModeFlag = nullptr;          // &DAT_140db4f38
uint8_t** g_gmModeOwner = nullptr;         // &DAT_140ee7010
uint8_t** g_pLocalPlayer = nullptr;        // &DAT_140e66228
uint8_t** g_spawnManager = nullptr;        // &DAT_140ee4b40

template <typename R, typename... A>
R VCall(void* obj, size_t slot, A... args) {
	auto fn = *reinterpret_cast<R (**)(void*, A...)>(*reinterpret_cast<uint8_t**>(obj) + slot);
	return fn(obj, args...);
}

// Overrides the tint of a plain NPC with its con colour. Leaves PCs, corpses, pets, mercenaries,
// objects and GM mode to the original. The target gets the plain con colour (no blink brightening).
void ApplyConColour(uint8_t* spawn) {
	void* actor = *reinterpret_cast<void**>(spawn + kSpawnActor);
	if (!actor || spawn[kSpawnType] != 1) return;
	if (*reinterpret_cast<int*>(spawn + kSpawnPetOwnerType) != 0) return;
	if (*g_gmModeFlag && *g_gmModeOwner && *reinterpret_cast<int*>(*g_gmModeOwner + kGmModeOwnerFlag) == 1) return;
	if (VCall<int>(spawn, kVtblGetRace) >= 0x8cd) return;
	uint8_t* actorDef = VCall<uint8_t*>(spawn, kVtblGetActorDef);
	if (actorDef && actorDef[kActorDefMercenary]) return;
	void* eq = *g_pEverQuest;
	if (!eq) return;
	int con = g_getConLevel(eq, spawn);   // 0 = corpse/untargetable
	if (con < 1 || con > 7) return;
	uint32_t colour = g_userColour(g_conColourIndex(con));
	VCall<void>(actor, kActorVtblSetNameColour, &colour);
}

// Re-colours every NPC when the local player's level has changed since we last looked.
void RetintOnLocalLevelChange() {
	static int lastLevel = -1;
	static bool walking = false;
	uint8_t* me = *g_pLocalPlayer;
	if (!me || walking) return;
	int level = me[kSpawnLevel];
	if (level == lastLevel) return;
	bool first = lastLevel < 0;
	lastLevel = level;
	if (first) return;
	walking = true;
	uint8_t* mgr = *g_spawnManager;
	uint8_t* spawn = mgr ? VCall<uint8_t*>(mgr, kSpawnMgrVtblFirst) : nullptr;
	while (spawn) {
		if (spawn != me) ApplyConColour(spawn);
		void* node = *reinterpret_cast<void**>(spawn + kSpawnListNode);
		spawn = node ? VCall<uint8_t*>(node, kSpawnNodeVtblNext) : nullptr;
	}
	walking = false;
}

uint64_t SetNameSpriteTintDetour(uint8_t* spawn) {
	uint64_t result = g_origTint(spawn);
	ApplyConColour(spawn);
	RetintOnLocalLevelChange();
	return result;
}

#define REF(var) reinterpret_cast<void**>(&var)

// SetNameSpriteTint FUN_1403014a0 prologue and GM-mode check. Stolen (20 bytes, all position
// independent): push rbp; push rdi; mov rbp,rsp; sub rsp,38; mov rdi,rcx; mov rcx,[rcx+1170].
// The same match also carries the two GM-mode globals.
const char* kTintSig =
	"40 55 57 48 8B EC 48 83 EC 38 48 8B F9 48 8B 89 70 11 00 00 48 85 C9 0F 84 ?? ?? ?? ?? "
	"48 83 3D ?? ?? ?? ?? 00 74 30 48 8B 05 ?? ?? ?? ?? 83 B8 E4 05 00 00 01";

// CTargetWnd per-frame FUN_14050e150: mov [rax+13bc],-1; mov rdx,[pTarget]; mov rcx,[pEverQuest];
// call GetConLevel. The call target is cross-checked against the GetConLevel found by prologue.
const char* kTargetWndConSig =
	"C7 80 BC 13 00 00 FF FF FF FF 48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ??";

// Pet-owner lookup inside SetNameSpriteTint: mov rcx,[spawnManager]; ... call [rax+18] (GetSpawnByID
// with [rdi+1b8] = owner id); ... mov rcx,[pLocalPlayer]; cmp [rax+168],[rcx+168] (group id).
const char* kSpawnMgrSig =
	"48 8B 0D ?? ?? ?? ?? 48 85 C9 74 6E 48 8B 01 48 8B 97 B8 01 00 00 FF 50 18 48 8B D8 48 85 C0 74 59 "
	"48 8B 0D ?? ?? ?? ?? 8B 91 68 01 00 00 39 90 68 01 00 00";

} // namespace

const HookSite kConNameTagsHooks[1] = {
	{ kTintSig, 20, reinterpret_cast<void*>(&SetNameSpriteTintDetour), REF(g_origTint) },
};

const RefSite kConNameTagsRefs[9] = {
	// GetConLevel FUN_1402d7fa0(pEverQuest, spawn): corpse test on +0x135, vtbl+0xa0 untargetable test
	{ "48 89 5C 24 18 57 48 83 EC 20 80 BA 35 01 00 00 02 48 8B FA 48 8B D9 0F 83 ?? ?? ?? ?? "
	  "48 8B 02 48 8B CA FF 90 A0 00 00 00 84 C0", 0, 0, REF(g_getConLevel) },
	// FUN_140661f70(con): jump table returning 0x186..0x18c
	{ "FF C9 83 F9 06 77 3A 48 63 C1 48 8D 15 ?? ?? ?? ?? 8B 8C 82 ?? ?? ?? ?? 48 03 CA FF E1 "
	  "B8 86 01 00 00 C3 B8 87 01 00 00 C3", 0, 0, REF(g_conColourIndex) },
	// FUN_14050b6d0(index): user colour table lookup, 0xf0f0f0 fallback, R/B swap
	{ "48 83 EC 28 81 F9 FF 00 00 00 7E 32 E8 ?? ?? ?? ?? 85 C0 B9 F0 F0 F0 00 0F 44 C1 8B C8 C1 E9 10",
	  0, 0, REF(g_userColour) },
	{ kTargetWndConSig, 20, 24, REF(g_pEverQuest) },
	{ kTargetWndConSig, 25, 29, REF(g_getConLevelCalled) },
	{ kTintSig, 32, 37, REF(g_gmModeFlag) },
	{ kTintSig, 42, 46, REF(g_gmModeOwner) },
	{ kSpawnMgrSig, 3, 7, REF(g_spawnManager) },
	{ kSpawnMgrSig, 36, 40, REF(g_pLocalPlayer) },
};

const char* ConNameTagsPrepare() {
	if (g_getConLevelCalled != reinterpret_cast<void*>(g_getConLevel))
		return "the target window's con call does not point at GetConLevel";
	return nullptr;
}
