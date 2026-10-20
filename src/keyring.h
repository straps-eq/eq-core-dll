#pragma once
// EquipmentKeyRing: a seventh "Equipment" tab in the Key Rings window (Live's Equipment Key Ring on ToB), for the
// server's persona / equipment set storage. See keyring.cpp.
#include <cstdint>
#include "patches.h"

extern const HookSite kKeyRingHooks[4];
extern const RefSite kKeyRingRefs[18];
const char* KeyRingPrepare();

// Called from the shared hooks (no-ops while the feature is off).
void KeyRingFrame(bool inGame);                                        // DrawWindows, once per frame
void KeyRingOnPacket(uint32_t op, const uint8_t* data, uint32_t size); // packet dispatcher, before the client
