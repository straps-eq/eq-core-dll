#pragma once
// Nameplates: HP / mana / cast bars and a threat flash under spawn names. NameplateOptionsPage: a "Nameplates"
// tab in the Options window. See nameplates.cpp.
#include <cstdint>
#include "patches.h"

extern const HookSite kNameplatesHooks[5];
extern const RefSite kNameplatesRefs[4];
const char* NameplatesPrepare();
extern const HookSite kNameplatePageHooks[3];
extern const RefSite kNameplatePageRefs[7];
const char* NameplatePagePrepare();

// Called from the shared hooks (no-ops while the features are off).
void NameplatesFrame(uint8_t* mgr, bool inGame);              // DrawWindows, after the client's windows
void NameplatesOnDamage(const uint8_t* pkt);                   // OP_Damage, after the client handled it
void NameplatePageRefresh(uint8_t* opt);                       // COptionsWnd::RefreshDisplayPage
bool NameplatePageNotify(uint8_t* opt, uint8_t* sender, int msg, void* data);   // true = ours, return 0

// Shared with EquipmentKeyRing: the packet dispatcher hook FUN_1401e4010 (signature, detour, trampoline slot). The
// detour hands every packet to KeyRingOnPacket first.
extern const char kDispatchSig[];
extern uint64_t (*const g_dispatchDetour)(void* self, void* a2, uint32_t op, uint8_t* data, uint32_t size);
extern uint64_t (**const g_origDispatchSlot)(void* self, void* a2, uint32_t op, uint8_t* data, uint32_t size);
