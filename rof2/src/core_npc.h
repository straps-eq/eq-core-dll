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
			char* modelNameUpper = _strdup(npc.modelName);
			DebugSpew("injecting race %s gender %d id %d", modelNameUpper, npc.genderID, npc.raceID);
			InjectCustomNPCs_Trampoline(pThis, pPtr, npc.raceID, npc.genderID, modelNameUpper, npc.raceMask, npc.dbStrID);
		}
	}
	DebugSpew("loaded race %s gender %d as id %d", modelName, genderID, raceID);
	return InjectCustomNPCs_Trampoline(pThis, pPtr, raceID, genderID, modelName, raceMask, dbStrID);
}
DETOUR_TRAMPOLINE_EMPTY(char* __fastcall InjectCustomNPCs_Trampoline(char* pThis, char* pPtr, int raceID, int genderID, const char* modelName, int raceMask, int dbStrID));
// Hooks to CRaceGenderInfoManager::AddRaceGender
void InjectCustomNPCs() { EzDetour((((DWORD)0x0050A440 - 0x400000) + baseAddress), InjectCustomNPCs_Detour, InjectCustomNPCs_Trampoline); };
//signed int __thiscall CRaceGenderInfoManager::AddRaceGender(signed int* this, int a2, int a3, const char* a4, int a5, int a6)


char* __stdcall InjectCustomOldAnimations_Trampoline(char* pOriginalModel, char* pReplacementModel, char isReplaced);
char* __stdcall InjectCustomOldAnimations_Detour(char* pOriginalModel, char* pReplacementModel, char isReplaced) {
	for (auto&& anim : CustomAnimations) {
		if (!anim.originalName || !anim.replacementName) continue;
		if (strncmp(anim.originalName, pOriginalModel, strlen(anim.originalName)) != 0) continue;
		if (strlen(anim.replacementName) != 2 && strlen(anim.replacementName) != 3) continue;
		pReplacementModel[0] = anim.replacementName[0];
		pReplacementModel[1] = anim.replacementName[1];
		if (strlen(anim.replacementName) == 2) {
			pReplacementModel[2] = pOriginalModel[2];
		}
		else {
			pReplacementModel[2] = anim.replacementName[2];
		}

		DebugSpew("injecting animation %s => %s", anim.originalName, pReplacementModel);
		isReplaced = 0x0;
		return &isReplaced;
	}
	auto ret = InjectCustomOldAnimations_Trampoline(pOriginalModel, pReplacementModel, isReplaced);
	DebugSpew("animation event %s => %s", pOriginalModel, pReplacementModel);
	return ret;
}
DETOUR_TRAMPOLINE_EMPTY(char* __stdcall InjectCustomOldAnimations_Trampoline(char* pOriginalModel, char* pReplacementModel, char isReplaced));
// Hooks to GetAlternateAnimTag
void InjectCustomOldAnimations() { EzDetour((((DWORD)0x00406a60 - 0x400000) + baseAddress), InjectCustomOldAnimations_Detour, InjectCustomOldAnimations_Trampoline); };
//char __stdcall ActorAnimation::GetAlternateAnimTag(char* a2, char* a3, bool a4)
