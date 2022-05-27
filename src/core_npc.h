#pragma once

#include "MQ2Main.h"
#include "core_models.h"
#include "_options.h"

char* __fastcall InjectCustomNPCs_Trampoline(char* pThis, char* pPtr, int raceID, int genderID, const char* modelName, int raceMask, int dbStrID);
char* __fastcall InjectCustomNPCs_Detour(char*pThis, char* pPtr, int raceID, int genderID, const char* modelName, int raceMask, int dbStrID)
{
	if (!raceID) return InjectCustomNPCs_Trampoline(pThis, pPtr, raceID, genderID, modelName, raceMask, dbStrID);
	if (raceID == 723) {
		for (auto&& npc : NPCs) {
			DebugSpew("injecting race %s gender %d id %d", npc.modelName , npc.genderID, npc.raceID);
			InjectCustomNPCs_Trampoline(pThis, pPtr, npc.raceID, npc.genderID, npc.modelName, npc.raceMask, npc.dbStrID);
		}
	}
	DebugSpew("loaded race %s gender %d as id %d", modelName, genderID, raceID);
	return InjectCustomNPCs_Trampoline(pThis, pPtr, raceID, genderID, modelName, raceMask, dbStrID);
}
DETOUR_TRAMPOLINE_EMPTY(char* __fastcall InjectCustomNPCs_Trampoline(char* pThis, char* pPtr, int raceID, int genderID, const char* modelName, int raceMask, int dbStrID));
// Hooks to CRaceGenderInfoManager::AddRaceGender
void InjectCustomNPCs() { EzDetour((((DWORD)0x0050A440 - 0x400000) + baseAddress), InjectCustomNPCs_Detour, InjectCustomNPCs_Trampoline); };
//signed int __thiscall CRaceGenderInfoManager::AddRaceGender(signed int* this, int a2, int a3, const char* a4, int a5, int a6)
