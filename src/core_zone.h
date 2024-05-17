#pragma once

#include "MQ2Main.h"
#include "core_models.h"
#include "_options.h"

static bool isCustomZonesReplacingExisting = false;

char* __fastcall InjectCustomZones_Trampoline(char* pThis, char* pPtr, unsigned __int32 zoneType, unsigned __int32 zoneID, char* zoneShortName, char* zoneLongName, unsigned __int32 eqStrID, __int32 zoneFlags2, __int32 x, __int32 y, __int32 z);
char* __fastcall InjectCustomZones_Detour(char* pThis, char* pPtr, unsigned __int32 zoneType, unsigned __int32 zoneID, char* zoneShortName, char* zoneLongName, unsigned __int32 eqStrID, __int32 zoneFlags2, __int32 x, __int32 y, __int32 z)
{
	if (isCustomZonesReplacingExisting) {
		for (auto&& zone : Zones) {
			if (zoneID != zone.zoneID) {
				continue;
			}
			DebugSpew("replacing zone %s id %d", zone.zoneShortName, zone.zoneID);
			return InjectCustomZones_Trampoline(pThis, pPtr, zone.zoneType, zone.zoneID, zone.zoneShortName, zone.zoneLongName, zone.eqStrID, zone.zoneFlags2, zone.x, zone.y, zone.z);
		}
	}
	if (!strcmp(zoneShortName, "interiorwalltest")) {
		for (auto&& zone : Zones) {
			DebugSpew("injecting zone %s id %d", zone.zoneShortName, zone.zoneID);			
			InjectCustomZones_Trampoline(pThis, pPtr, zone.zoneType, zone.zoneID, zone.zoneShortName, zone.zoneLongName, zone.eqStrID, zone.zoneFlags2, zone.x, zone.y, zone.z);
		}
	}
	DebugSpew("loaded zone %s id %d", zoneShortName, zoneID);
	return InjectCustomZones_Trampoline(pThis, pPtr, zoneType, zoneID, zoneShortName, zoneLongName, eqStrID, zoneFlags2, x, y, z);
}

DETOUR_TRAMPOLINE_EMPTY(char* __fastcall InjectCustomZones_Trampoline(char* pThis, char* pPtr, unsigned __int32 zoneType, unsigned __int32 zoneID, char* zoneShortName, char* zoneLongName, unsigned __int32 eqStrID, __int32 zoneFlags2, __int32 x, __int32 y, __int32 z));
// Hooks to EQWorldData::AddZone
void InjectCustomZones() { 
	for (auto&& zone : Zones) {
		if (zone.zoneID >= 787 ) {
			continue;
		}
		isCustomZonesReplacingExisting = true;
		break;
	}
	EzDetour((((DWORD)0x007DC430 - 0x400000) + baseAddress), InjectCustomZones_Detour, InjectCustomZones_Trampoline); 
};
//bool __thiscall sub_7DC430(_DWORD *this, int a2, int a3, char *a4, char *a5, int a6, int a7, int a8, int a9, int a10)
