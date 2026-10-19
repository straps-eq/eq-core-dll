#pragma once
// Nameplates: Legends-style HP / mana / cast bars and a threat frame under spawn names. NameplateOptionsPage:
// Legends' "Nameplates" tab in the Options window. See nameplates.cpp.
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
