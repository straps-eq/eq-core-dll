#pragma once

#include "MQ2Main.h"
#include "core_models.h"
#include "_options.h"

signed int __stdcall InjectIsShield_Trampoline(signed int itemID);
signed int __stdcall InjectIsShield_Detour(signed int itemID) {
	for (auto&& shieldID : Shields) {
		if (itemID != shieldID) {
			continue;
		}
		return 1;
	}

	if (areDefaultShieldsIgnored) {
		return 0;
	}
	return InjectIsShield_Trampoline(itemID);
}

DETOUR_TRAMPOLINE_EMPTY(signed int __stdcall InjectIsShield_Trampoline(signed int itemID));
void InjectCustomShields() { EzDetour((((DWORD)0x00489D70 - 0x400000) + baseAddress), InjectIsShield_Detour, InjectIsShield_Trampoline); };
// signed int __thiscall sub_489D70(_DWORD *this, signed int a2)
