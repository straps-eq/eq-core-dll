// DPS ADV CREATED BY WARNEN 2008-2009
// MQ2DPSAdv.cpp

using namespace std;
#include <vector>
#include <sstream>
#include <list>
#include "MQ2AdvDps.h"

#define DPSVERSION "1.2.06"
//#define DPSDEV

extern std::map<uint32_t, EdgeDPSEntry> DamageEntries;

// ############################### CDPSAdvWnd START ############################################

CDPSAdvWnd::CDPSAdvWnd() :CCustomWnd("DPSAdvWnd") {
	int CheckUI = false;
	if (!(Tabs = (CTabWnd*)GetChildItem("DPS_Tabs"))) CheckUI = true;
	if (!(LTopList = (CListWnd*)GetChildItem("DPS_TopList"))) CheckUI = true;
	if (!(CMobList = (CComboWnd*)GetChildItem("DPS_MobList"))) CheckUI = true;
	if (!(CShowMeTop = (CCheckBoxWnd*)GetChildItem("DPS_ShowMeTopBox"))) CheckUI = true;
	if (!(CShowMeMin = (CCheckBoxWnd*)GetChildItem("DPS_ShowMeMinBox"))) CheckUI = true;
	if (!(TShowMeMin = (CTextEntryWnd*)GetChildItem("DPS_ShowMeMinInput"))) CheckUI = true;
	if (!(CUseRaidColors = (CCheckBoxWnd*)GetChildItem("DPS_UseRaidColorsBox"))) CheckUI = true;
	if (!(CLiveUpdate = (CCheckBoxWnd*)GetChildItem("DPS_LiveUpdateBox"))) CheckUI = true;
	if (!(TFightIA = (CTextEntryWnd*)GetChildItem("DPS_FightIAInput"))) CheckUI = true;
	if (!(TFightTO = (CTextEntryWnd*)GetChildItem("DPS_FightTOInput"))) CheckUI = true;
	if (!(TEntTO = (CTextEntryWnd*)GetChildItem("DPS_EntTOInput"))) CheckUI = true;
	if (!(CShowTotal = (CComboWnd*)GetChildItem("DPS_ShowTotal"))) CheckUI = true;
	//if (!(LFightList = (CListWnd*)GetChildItem("DPS_FightList"))) CheckUI = true;
	this->BGColor.ARGB = 0xFF000000;
	Tabs->BGColor.ARGB = 0xFF000000;
	LTopList->BGColor.ARGB = 0xFF000000;
	CShowMeTop->BGColor.ARGB = 0xFF000000;
	CShowMeMin->BGColor.ARGB = 0xFF000000;
	TShowMeMin->BGColor.ARGB = 0xFF000000;
	CUseRaidColors->BGColor.ARGB = 0xFF000000;
	CLiveUpdate->BGColor.ARGB = 0xFF000000;
	TFightIA->BGColor.ARGB = 0xFF000000;
	TFightTO->BGColor.ARGB = 0xFF000000;
	TEntTO->BGColor.ARGB = 0xFF000000;
	CShowTotal->BGColor.ARGB = 0xFF000000;
	if (CheckUI) {
		WriteChatf("\ar[DPSAdv] Incorrect UI File in use. Please update to the latest client files.");
		WrongUI = true;
	}
	else WrongUI = false;

	LoadLoc();
	SetWndNotification(CDPSAdvWnd);
	//LTopList->SetColors(0xFFFFFFFF, 0xFFCC3333, 0xFF666666);
	//LFightList->SetColors(0xFFFFFFFF, 0xFFCC3333, 0xFF666666);
	CMobList->SetColors(0xFFCC3333, 0xFF666666, 0xFF000000);
	Tabs->UpdatePage();
	DrawCombo();
	//LFightList->AddString(&CXStr("1"), 0, 0, 0);
	//LFightList->AddString(&CXStr("2"), 0, 0, 0);
	//LFightList->AddString(&CXStr("3"), 0, 0, 0);
	//LFightList->ExtendSel(1);
	//LFightList->ExtendSel(2);
}

CDPSAdvWnd::~CDPSAdvWnd() {}

void CDPSAdvWnd::DrawCombo() {
	int CurSel = CMobList->GetCurChoice();
	CMobList->DeleteAll();
	CHAR szTemp[MAX_STRING] = { 0 };
	//sprintf_s(szTemp, "<Target> %s", CurListMob ? CurListMob->Name : "None");
	sprintf_s(szTemp, "<Target> %s", CListType == CLISTTARGET && CurListMob ? CurListMob->Name : "None");
	CMobList->InsertChoice(szTemp);
	sprintf_s(szTemp, "<MaxDmg> %s", CListType == CLISTMAXDMG && CurMaxMob ? CurListMob->Name : "None");
	CMobList->InsertChoice(szTemp);
	int i = 0, ListSize = 0;
	for (auto mob : DamageEntries) {

		auto entrySpawn = mob.second.SpawnID > 0 ? (PSPAWNINFO)GetSpawnByID(mob.second.SpawnID) : nullptr;

		if (entrySpawn && entrySpawn->Type == 1) {
			ListSize++;
			sprintf_s(szTemp, "(%i) %s", ListSize, entrySpawn->Name);
			CMobList->InsertChoice(szTemp);
		}
	}

	if (ListSize < 6) CMobList->InsertChoice("");

	if (CListType == CLISTTARGET) CMobList->SetChoice(0);
	else if (CListType == CLISTMAXDMG) CMobList->SetChoice(1);
	else CMobList->SetChoice(CurSel >= 0 ? CurSel : 0);
}

void CDPSAdvWnd::SetTotal(int LineNum, PSPAWNINFO Mob) {
	CHAR szTemp[MAX_STRING] = { 0 };

	if (!Mob)
		return;

	EdgeDPSEntry Entry = GetEdgeDPSEntryByID(Mob->SpawnID, false);

	if (Entry.SpawnID)
	{
		SetLineColors(LineNum, Entry, true, false, false);
		LTopList->SetItemText(LineNum, 0, &CXStr("-"));
		LTopList->SetItemText(LineNum, 1, &CXStr("Total"));
		sprintf_s(szTemp, "%i", Entry.TotalIncomingDamage);
		LTopList->SetItemText(LineNum, 2, &CXStr(szTemp));

		auto timestamp = std::chrono::high_resolution_clock::now();

		std::chrono::duration<double> elapsed_seconds = timestamp - Entry.BeginningTimestamp;

		auto dpsTOTAL = 0;
		auto dpsAVRGE = 0;

		DWORD damages = Entry.TotalIncomingDamage;
		DOUBLE estimate = damages / elapsed_seconds.count();
		dpsTOTAL++;
		DOUBLE evaluate = (estimate - dpsAVRGE) / dpsTOTAL + dpsAVRGE; dpsAVRGE = evaluate;
		if (elapsed_seconds.count() == 0.0)
		{
			evaluate = 0.f;
		}
		std::ostringstream outEstimate;
		outEstimate.precision(3);
		outEstimate << std::fixed << estimate;
		LTopList->SetItemText(LineNum, 3, &CXStr(outEstimate.str().c_str()));


		sprintf_s(szTemp, "%i", Entry.TotalOutgoingDamage);
		LTopList->SetItemText(LineNum, 4, &CXStr(szTemp));

		dpsTOTAL = 0;
		dpsAVRGE = 0;

		damages = Entry.TotalOutgoingDamage;
		estimate = damages / elapsed_seconds.count();
		dpsTOTAL++;
		evaluate = (estimate - dpsAVRGE) / dpsTOTAL + dpsAVRGE; dpsAVRGE = evaluate;
		if (elapsed_seconds.count() == 0.0)
		{
			evaluate = 0.f;
		}
		std::ostringstream inEstimate;
		inEstimate.precision(3);
		inEstimate << std::fixed << estimate;
		LTopList->SetItemText(LineNum, 5, &CXStr(inEstimate.str().c_str()));
	}
}

void CDPSAdvWnd::DrawList() {
	int ScrollPos = LTopList->VScrollPos;
	int CurSel = LTopList->GetCurSel();
	CHAR szTemp[MAX_STRING] = { 0 };
	LTopList->DeleteAll();
	int i = 0, LineNum = 0, RankAdj = 0, ShowMeLineNum = 0;
	bool FoundMe = false, ThisMe = false;
	if (ShowTotal == TOTALABOVE) {
		LineNum = LTopList->AddString(CXStr(" "), 0, 0, 0);
		SetTotal(LineNum, CurListMob);
		RankAdj++;
	}
	if (ShowMeTop) {
		ShowMeLineNum = LTopList->AddString(CXStr(" "), 0, 0, 0);
		SetLineColors(ShowMeLineNum, EdgeDPSEntry(), false, true, false);
		RankAdj++;
	}
	if (ShowTotal == TOTALSECOND) {
		LineNum = LTopList->AddString(CXStr(" "), 0, 0, 0);
		SetTotal(LineNum, CurListMob);
		RankAdj++;
	}

	for (auto entry : DamageEntries) {
		auto spawnEntry = (PSPAWNINFO)GetSpawnByID(entry.first);

		if (ShowMeTop && spawnEntry && !strcmp(spawnEntry->Name, ((PSPAWNINFO)pCharSpawn)->DisplayedName)) {
			if (!ShowMeMin || (LineNum - RankAdj + 1) > ShowMeMinNum) FoundMe = true;
			ThisMe = true;
		}
		else ThisMe = false;
		LineNum = LTopList->AddString(CXStr(" "), 0, 0, 0);
		SetLineColors(LineNum, entry.second);
		sprintf_s(szTemp, "%i", LineNum - RankAdj + 1);
		LTopList->SetItemText(LineNum, 0, &CXStr(szTemp));
		if (ThisMe) LTopList->SetItemText(ShowMeLineNum, 0, &CXStr(szTemp));
		sprintf_s(szTemp, "%s%s", spawnEntry ? spawnEntry->Name : " ", spawnEntry && spawnEntry->PetID > 0 ? "*" : " ");
		LTopList->SetItemText(LineNum, 1, &CXStr(szTemp));
		if (ThisMe) LTopList->SetItemText(ShowMeLineNum, 1, &CXStr(szTemp));
		sprintf_s(szTemp, "%i", entry.second.TotalIncomingDamage);
		LTopList->SetItemText(LineNum, 2, &CXStr(szTemp));
		if (ThisMe) LTopList->SetItemText(ShowMeLineNum, 2, &CXStr(szTemp));
		auto timestamp = std::chrono::high_resolution_clock::now();

		std::chrono::duration<double> elapsed_seconds = timestamp - entry.second.BeginningTimestamp;

		auto dpsTOTAL = 0;
		auto dpsAVRGE = 0;

		DWORD damages = entry.second.TotalIncomingDamage;
		DOUBLE estimate = damages / elapsed_seconds.count();
		dpsTOTAL++;
		DOUBLE evaluate = (estimate - dpsAVRGE) / dpsTOTAL + dpsAVRGE; dpsAVRGE = evaluate;
		if (elapsed_seconds.count() == 0.0)
		{
			evaluate = 0.f;
		}
		std::ostringstream outEstimate;
		outEstimate.precision(3);
		outEstimate << std::fixed << estimate;
		LTopList->SetItemText(LineNum, 3, &CXStr(outEstimate.str().c_str()));
		if (ThisMe) LTopList->SetItemText(ShowMeLineNum, 3, &CXStr(outEstimate.str().c_str()));

		sprintf_s(szTemp, "%i", entry.second.TotalOutgoingDamage);
		LTopList->SetItemText(LineNum, 4, &CXStr(szTemp));
		
		dpsTOTAL = 0;
		dpsAVRGE = 0;

		damages = entry.second.TotalOutgoingDamage;
		estimate = damages / elapsed_seconds.count();
		dpsTOTAL++;
		evaluate = (estimate - dpsAVRGE) / dpsTOTAL + dpsAVRGE; dpsAVRGE = evaluate;
		if (elapsed_seconds.count() == 0.0)
		{
			evaluate = 0.f;
		}
		std::ostringstream inEstimate;
		inEstimate.precision(3);
		inEstimate << std::fixed << estimate;
		
		LTopList->SetItemText(LineNum, 5, &CXStr(inEstimate.str().c_str()));
		if (ThisMe) LTopList->SetItemText(ShowMeLineNum, 5, &CXStr(inEstimate.str().c_str()));

	}
	if (ShowTotal == TOTALBOTTOM) {
		LineNum = LTopList->AddString(CXStr(" "), 0, 0, 0);
		SetTotal(LineNum, CurListMob);
	}
	if (ShowMeTop && !FoundMe) LTopList->RemoveLine(ShowMeLineNum);
	LTopList->VScrollPos = ScrollPos;
	LTopList->CalculateFirstVisibleLine();
	LTopList->SetCurSel(CurSel);
}

void CDPSAdvWnd::SetLineColors(int LineNum, EdgeDPSEntry ent, bool Total, bool MeTop, bool UseEnt) {
	
	auto entrySpawn = ent.SpawnID > 0 && UseEnt ? (PSPAWNINFO)GetSpawnByID(ent.SpawnID) : nullptr;
	
	if (MeTop) {
		LTopList->SetItemColor(LineNum, 0, MeTopColor);
		LTopList->SetItemColor(LineNum, 1, MeTopColor);
		LTopList->SetItemColor(LineNum, 2, MeTopColor);
		LTopList->SetItemColor(LineNum, 3, MeTopColor);
		LTopList->SetItemColor(LineNum, 4, MeTopColor);
		LTopList->SetItemColor(LineNum, 5, MeTopColor);

	}
	else if (Total) {
		LTopList->SetItemColor(LineNum, 0, TotalColor);
		LTopList->SetItemColor(LineNum, 1, TotalColor);
		LTopList->SetItemColor(LineNum, 2, TotalColor);
		LTopList->SetItemColor(LineNum, 3, TotalColor);
		LTopList->SetItemColor(LineNum, 4, TotalColor);
		LTopList->SetItemColor(LineNum, 5, TotalColor);

	}
	 if (entrySpawn && entrySpawn->Type == SPAWN_PLAYER) {
		//WriteChatf("Setting Raid Color: %i, %i, %i", Ent->Class, Coloring[Ent->Class], ((PEQRAIDWINDOW)pRaidWnd)->ClassColors[ClassInfo.RaidColorOrder[Ent->Class]]);
		LTopList->SetItemColor(LineNum, 0, MeColor);
		LTopList->SetItemColor(LineNum, 1, MeColor);
		LTopList->SetItemColor(LineNum, 2, MeColor);
		LTopList->SetItemColor(LineNum, 3, MeColor);
		LTopList->SetItemColor(LineNum, 4, MeColor);
		LTopList->SetItemColor(LineNum, 5, MeColor);

	}
	else {
		LTopList->SetItemColor(LineNum, 0, NormalColor);
		LTopList->SetItemColor(LineNum, 1, NPCColor);
		LTopList->SetItemColor(LineNum, 2, NormalColor);
		LTopList->SetItemColor(LineNum, 3, NormalColor);
		LTopList->SetItemColor(LineNum, 4, NormalColor);
		LTopList->SetItemColor(LineNum, 5, NormalColor);
	}
}
/*
void CDPSAdvWnd::SaveSetting(PCHAR Key, PCHAR Value, ...) {
   char zOutput[MAX_STRING]; va_list vaList; va_start(vaList,Value);
   vsprintf_s(zOutput,Value,vaList);
   WritePrivateProfileString(GetCharInfo()->Name, Key, zOutput, "Edge.ini");
   if (!Saved) {
	  Saved = true;
	  WritePrivateProfileString(GetCharInfo()->Name, "Saved", "1", "Edge.ini");
   }
}
*/
void CDPSAdvWnd::SaveLoc() {
	if (!GetCharInfo()) return;
	CHAR szTemp[MAX_STRING] = { 0 };
	WritePrivateProfileString(GetCharInfo()->Name, "Saved", "1", "Edge.ini");
	sprintf_s(szTemp, "%i", Location.top);
	WritePrivateProfileString(GetCharInfo()->Name, "Top", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", Location.bottom);
	WritePrivateProfileString(GetCharInfo()->Name, "Bottom", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", Location.left);
	WritePrivateProfileString(GetCharInfo()->Name, "Left", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", Location.right);
	WritePrivateProfileString(GetCharInfo()->Name, "Right", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", Alpha);
	WritePrivateProfileString(GetCharInfo()->Name, "Alpha", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", FadeToAlpha);
	WritePrivateProfileString(GetCharInfo()->Name, "FadeToAlpha", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", CListType);
	WritePrivateProfileString(GetCharInfo()->Name, "CListType", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", LiveUpdate ? 1 : 0);
	WritePrivateProfileString(GetCharInfo()->Name, "LiveUpdate", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", dShow);
	WritePrivateProfileString(GetCharInfo()->Name, "Show", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", ShowMeTop ? 1 : 0);
	WritePrivateProfileString(GetCharInfo()->Name, "ShowMeTop", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", ShowMeMin ? 1 : 0);
	WritePrivateProfileString(GetCharInfo()->Name, "ShowMeMin", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", ShowMeMinNum);
	WritePrivateProfileString(GetCharInfo()->Name, "ShowMeMinNum", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", UseRaidColors ? 1 : 0);
	WritePrivateProfileString(GetCharInfo()->Name, "UseRaidColors", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", ShowTotal);
	WritePrivateProfileString(GetCharInfo()->Name, "ShowTotal", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", FightIA);
	WritePrivateProfileString(GetCharInfo()->Name, "FightIA", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", FightTO);
	WritePrivateProfileString(GetCharInfo()->Name, "FightTO", szTemp, "Edge.ini");
	sprintf_s(szTemp, "%i", EntTO);
	WritePrivateProfileString(GetCharInfo()->Name, "EntTO", szTemp, "Edge.ini");
}

void CDPSAdvWnd::LoadSettings() {
	CHAR szTemp[MAX_STRING] = { 0 };
	CShowMeTop->Checked = ShowMeTop ? 1 : 0;
	CShowMeMin->Checked = ShowMeMin ? 1 : 0;
	sprintf_s(szTemp, "%i", ShowMeMinNum);
	SetCXStr(&TShowMeMin->InputText, szTemp);
	CUseRaidColors->Checked = UseRaidColors ? 1 : 0;
	CLiveUpdate->Checked = LiveUpdate ? 1 : 0;
	sprintf_s(szTemp, "%i", FightIA);
	SetCXStr(&TFightIA->InputText, szTemp);
	sprintf_s(szTemp, "%i", FightTO);
	SetCXStr(&TFightTO->InputText, szTemp);
	sprintf_s(szTemp, "%i", EntTO);
	SetCXStr(&TEntTO->InputText, szTemp);
	CShowTotal->DeleteAll();
	CShowTotal->InsertChoice("Don't Show Total");
	CShowTotal->InsertChoice("Above ShowMeTop");
	CShowTotal->InsertChoice("Below ShowMeTop");
	CShowTotal->InsertChoice("Show Bottom");
	CShowTotal->InsertChoice("");
	CShowTotal->SetChoice(ShowTotal);
}

void CDPSAdvWnd::LoadLoc(char szChar[256]) {
	if (!GetCharInfo()) return;
	char szName[256] = { 0 };
	if (!szChar) strcpy_s(szName, GetCharInfo()->Name);
	else strcpy_s(szName, szChar);
	Saved = (GetPrivateProfileInt(szName, "Saved", 0, "Edge.ini") > 0 ? true : false);
	if (Saved && DPSWnd) {
		DPSWnd->Location = { (LONG)GetPrivateProfileInt(szName, "Left", 0, "Edge.ini"),
			(LONG)GetPrivateProfileInt(szName, "Top", 0, "Edge.ini"),
			(LONG)GetPrivateProfileInt(szName, "Right", 0, "Edge.ini"),
			(LONG)GetPrivateProfileInt(szName, "Bottom", 0, "Edge.ini") };

		DPSWnd->Alpha = (BYTE)GetPrivateProfileInt(szName, "Alpha", 0, "Edge.ini");
		
		DPSWnd->FadeToAlpha = (BYTE)GetPrivateProfileInt(szName, "FadeToAlpha", 0, "Edge.ini");
	}
	CListType = GetPrivateProfileInt(szName, "CListType", 0, "Edge.ini");
	LiveUpdate = (GetPrivateProfileInt(szName, "LiveUpdate", 0, "Edge.ini") > 0 ? true : false);
	Debug = (GetPrivateProfileInt(szName, "Debug", 0, "Edge.ini") > 0 ? true : false);
	if(DPSWnd)
		DPSWnd->dShow = (GetPrivateProfileInt(szName, "Show", 1, "Edge.ini") > 0 ? true : false);
	ShowMeTop = (GetPrivateProfileInt(szName, "ShowMeTop", 0, "Edge.ini") > 0 ? true : false);
	ShowMeMin = (GetPrivateProfileInt(szName, "ShowMeMin", 0, "Edge.ini") > 0 ? true : false);
	ShowMeMinNum = GetPrivateProfileInt(szName, "ShowMeMinNum", 0, "Edge.ini");
	UseRaidColors = (GetPrivateProfileInt(szName, "UseRaidColors", 0, "Edge.ini") > 0 ? true : false);
	ShowTotal = GetPrivateProfileInt(szName, "ShowTotal", 0, "Edge.ini");
	FightIA = GetPrivateProfileInt(szName, "FightIA", 8, "Edge.ini");
	FightTO = GetPrivateProfileInt(szName, "FightTO", 30, "Edge.ini");
	EntTO = GetPrivateProfileInt(szName, "EntTO", 8, "Edge.ini");
	MeColor = GetPrivateProfileInt(szName, "MeColor", 0xFF00CC00, "Edge.ini");
	MeTopColor = GetPrivateProfileInt(szName, "MeTopColor", 0xFF00CC00, "Edge.ini");
	NormalColor = GetPrivateProfileInt(szName, "NormalColor", 0xFFFFFFFF, "Edge.ini");
	NPCColor = GetPrivateProfileInt(szName, "NPCColor", 0xFFFFFFFF, "Edge.ini");
	TotalColor = GetPrivateProfileInt(szName, "TotalColor", 0xFF66FFFF, "Edge.ini");
	EntHover = GetPrivateProfileInt(szName, "EntHover", 0xFFCC3333, "Edge.ini");
	EntHighlight = GetPrivateProfileInt(szName, "EntHighlight", 0xFF666666, "Edge.ini");
	FightNormal = GetPrivateProfileInt(szName, "FightNormal", NormalColor, "Edge.ini");
	FightHover = GetPrivateProfileInt(szName, "FightHover", EntHover, "Edge.ini");
	FightHighlight = GetPrivateProfileInt(szName, "FightHighlight", EntHighlight, "Edge.ini");
	FightActive = GetPrivateProfileInt(szName, "FightActive", 0xFF00CC00, "Edge.ini");
	FightInActive = GetPrivateProfileInt(szName, "FightInActive", 0xFF777777, "Edge.ini");
	FightDead = GetPrivateProfileInt(szName, "FightDead", 0xFF330000, "Edge.ini");
	if (FightIA < 3) FightIA = 8;
	if (FightTO < 3) FightTO = 30;
	if (EntTO < 3) EntTO = 8;
	if (Debug) gSpewToFile = TRUE;
	if (CListType > 1) CListType = CLISTTARGET;
	LTopList->SetColors(NormalColor, EntHover, EntHighlight);
	//   LFightList->SetColors(FightNormal, FightHover, FightHighlight);
	CMobList->SetChoice(CListType);
	LoadSettings();
}

int CDPSAdvWnd::WndNotification(CXWnd *pWnd, unsigned int Message, void *unknown) {
	if (Debug && Message != 21) WriteChatf("Notify: %i", Message);
	if (Message == 10) CheckActive();
	if (Message == 3 && pWnd == (CXWnd*)LTopList) LTopList->SetCurSel(-1);
	else if (Message == 10 && pWnd == (CXWnd*)DPSWnd) CheckActive();
	else if (Message == 1) {
		if (pWnd == (CXWnd*)Tabs) LoadSettings();
		else if (pWnd == (CXWnd*)CShowMeTop) ShowMeTop = CShowMeTop->Checked ? true : false;
		else if (pWnd == (CXWnd*)CShowMeMin) ShowMeMin = CShowMeMin->Checked ? true : false;
		else if (pWnd == (CXWnd*)CUseRaidColors) UseRaidColors = CUseRaidColors->Checked ? true : false;
		else if (pWnd == (CXWnd*)CLiveUpdate) LiveUpdate = CLiveUpdate->Checked ? true : false;
		//else if (pWnd == (CXWnd*)LTopList) WriteChatf("CurSel: %i", LTopList->GetCurSel());
		else if (pWnd == (CXWnd*)CShowTotal) {
			ShowTotal = CShowTotal->GetCurChoice();
			if (ShowTotal == 4) ShowTotal = 0;
			LoadSettings();
		}
		else if (pWnd == (CXWnd*)CMobList) {
			CurListMob = 0;
			LTopList->DeleteAll();
			bool FoundMob = false;
			if ((int)CMobList->GetCurChoice() > 1) {
				CListType = 2;
				int i = 0, x = 0;
				for (auto mob : DamageEntries) {
					auto ListMob = (PSPAWNINFO)GetSpawnByID(mob.first);
					if (ListMob && ListMob->Type == 1 && !ListMob->PetID != 0) {
						if (x + 2 == (int)CMobList->GetCurChoice()) {
							FoundMob = true;
							ListSwitch(ListMob);
							break;
						}
						x++;
					}
				}
				if (!FoundMob) {
					CListType = 0;
					DPSWnd->DrawCombo();
				}
			}
			else CListType = (int)CMobList->GetCurChoice();
			Intervals -= 1; // Force update next Pulse.
		}
	}
	else if (Message == 14) {
		CHAR szTemp[MAX_STRING] = { 0 };
		GetCXStr(((CEditWnd*)pWnd)->InputText, szTemp);
		if (pWnd == (CXWnd*)TShowMeMin) {
			if (strlen(szTemp)) {
				szTemp[2] = 0;
				ShowMeMinNum = atoi(szTemp);
				sprintf_s(szTemp, "%i", ShowMeMinNum);
				SetCXStr(&TShowMeMin->InputText, szTemp);
				TShowMeMin->SetSel(strlen(szTemp), 0);
			}
		}
		else if (pWnd == (CXWnd*)TFightIA) {
			if (strlen(szTemp)) {
				szTemp[2] = 0;
				FightIA = atoi(szTemp);
				if (FightIA < 3) FightIA = 8;
				sprintf_s(szTemp, "%i", FightIA);
				//SetCXStr(&TFightTO->InputText, szTemp);
				//TFightTO->SetSel(strlen(szTemp), 0);
			}
		}
		else if (pWnd == (CXWnd*)TFightTO) {
			if (strlen(szTemp)) {
				szTemp[2] = 0;
				FightTO = atoi(szTemp);
				if (FightTO < 3) FightTO = 30;
				sprintf_s(szTemp, "%i", FightTO);
				//SetCXStr(&TFightTO->InputText, szTemp);
				//TFightTO->SetSel(strlen(szTemp), 0);
			}
		}
		else if (pWnd == (CXWnd*)TEntTO) {
			if (strlen(szTemp)) {
				szTemp[2] = 0;
				EntTO = atoi(szTemp);
				if (EntTO < 3) EntTO = 8;
				sprintf_s(szTemp, "%i", EntTO);
				//SetCXStr(&TEntTO->InputText, szTemp);
				//TEntTO->SetSel(strlen(szTemp), 0);
			}
		}
	}


	return CSidlScreenWnd::WndNotification(pWnd, Message, unknown);
};


#ifdef DPSDEV
void DPSTestCmd(PSPAWNINFO pChar, PCHAR szLine) {

}
#endif

void DPSAdvCmd(PSPAWNINFO pChar, PCHAR szLine) {
	char Arg1[MAX_STRING];
	GetArg(Arg1, szLine, 1);
	if (!_stricmp(Arg1, "show"))
		if (!DPSWnd) WriteChatf("\arDPSWnd does not exist. Try reloading your UI (/loadskin default).");
		else DPSWnd->dShow  = !DPSWnd->dShow;
	else if (!_stricmp(Arg1, "colors"))
		((CXWnd*)pRaidOptionsWnd)->Show(1, 1);
	else if (DPSWnd && !_stricmp(Arg1, "reload"))
		DPSWnd->LoadLoc();
	else if (DPSWnd && !_stricmp(Arg1, "save"))
		DPSWnd->SaveLoc();
	else if (!_stricmp(Arg1, "listsize"))
		WriteChatf("\ayMobList Size: %i", DamageEntries.size());
	else if (!_stricmp(Arg1, "copy")) {
		char szCopy[MAX_STRING];
		GetArg(szCopy, szLine, 2);
		if (DPSWnd) {
			DPSWnd->LoadLoc(szCopy);
			DPSWnd->SaveLoc();
		}
		else WriteChatf("\arFailed to Copy: DPS Window not loaded.");
	}
	else if (!_stricmp(Arg1, "Debug")) {
		Debug = Debug ? false : true;
		WriteChatf("Debug is now: %s", Debug ? "\agOn" : "\arOff");
	}
	CheckActive();
}

void CreateDPSWindow() {
	if (DPSWnd) DestroyDPSWindow();
	if (pSidlMgr->FindScreenPieceTemplate("DPSAdvWnd")) {
		DPSWnd = new CDPSAdvWnd();
		if (DPSWnd->dShow) ((CXWnd*)DPSWnd)->Show(1, 1);
		char szTitle[MAX_STRING];
		sprintf_s(szTitle, "DPS Window", DPSVERSION);
		SetCCXStr(&DPSWnd->WindowText, szTitle);
	}
	CheckActive();
}

void DestroyDPSWindow() {
	if (DPSWnd) {
		DPSWnd->SaveLoc();
		delete DPSWnd;
		DPSWnd = 0;
	}
	CheckActive();
}

PLUGIN_API VOID SetDPSGameState(DWORD GameState) {
	DebugSpewAlways("GameState Change: %i", GameState);
	if (GameState == GAMESTATE_INGAME) {
		if (!DPSWnd) CreateDPSWindow();
	}
}
bool bCleaned = false;
PLUGIN_API VOID OnDPSCleanUI(VOID) { DestroyDPSWindow(); bCleaned = true; }
PLUGIN_API VOID OnDPSReloadUI(VOID) { if (gGameState == GAMESTATE_INGAME && pCharSpawn) CreateDPSWindow(); }

PLUGIN_API VOID InitializeDPSPlugin(VOID) {
	LastMob = 0;
	CurTarget = 0;
	CurTarMob = 0;
	CurListMob = 0;
	CurMaxMob = 0;
	Zoning = false;
	ShowMeTop = false;
	WrongUI = false;
	AddXMLFile("EQUI_DPSAdvWnd.xml");
	AddCommand("/advdps", DPSAdvCmd);
	CheckActive();
	if (gGameState != GAMESTATE_INGAME || !pCharSpawn) return;
	else CreateDPSWindow();
}

PLUGIN_API VOID ShutdownDPSPlugin(VOID) {
	DestroyDPSWindow();
	RemoveCommand("/advdps");
#ifdef DPSDEV
	RemoveCommand("/dpstest");
#endif
}

bool CheckInterval() {
	if (!Intervals) Intervals = time(NULL);
	else if (Intervals != time(NULL)) {
		Intervals = time(NULL);
		return true;
	}
	return false;
}

void CheckActive() {
	if (DPSWnd && DPSWnd->dShow && !Zoning && !WrongUI) Active = true;
	else Active = false;
}

void ListSwitch(PSPAWNINFO Switcher) {
	if (Switcher)
	{
		CurListMob = Switcher;
	}
	DPSWnd->LTopList->SetCurSel(-1);
	DPSWnd->LTopList->VScrollPos = 0;
	DPSWnd->DrawList();
	DPSWnd->DrawCombo();
}

void TargetSwitch() {
	CurTarget = (PSPAWNINFO)pTarget;
	ListSwitch(CurTarget);
}

void HandleDeath(EdgeDPSEntry DeadMob) {

	DPSWnd->DrawList();
}

void IntPulse() {
	bool CChange = false;
	for (auto mob : DamageEntries) {
		auto spawnEntry = (PSPAWNINFO)GetSpawnByID(mob.first);
	}
	if (CListType == CLISTMAXDMG && CurMaxMob && CurMaxMob != CurListMob) 
		ListSwitch(CurMaxMob);
	if (CChange) 
		DPSWnd->DrawCombo();
	DPSWnd->DrawList();
	//WriteChatf("Active: %s", Active ? "Yes" : "No");
}

PLUGIN_API VOID OnDPSPulse(VOID) {
	if (gGameState != GAMESTATE_INGAME || !pCharSpawn) return;

	if (gGameState == GAMESTATE_INGAME) {
		if (!DPSWnd)
			CreateDPSWindow();
	}

	if (Active) {

		if (DPSWnd->dShow) ((CXWnd*)DPSWnd)->Show(1, 1);
		if ((PSPAWNINFO)pTarget && (PSPAWNINFO)pTarget != CurTarget) TargetSwitch();
		if (CListType == CLISTTARGET && CurTarMob && CurTarMob != CurListMob)
		{
			auto entry = GetEdgeDPSEntryByID(CurTarMob->SpawnID, false);
			if (entry.SpawnID && entry.weDamaged)
			{
				ListSwitch(CurTarMob);
			}
		}
		if (CheckInterval()) IntPulse();
	}
}

void ZoneProcess() {
	LastMob = 0;
	CurTarget = 0;
	CurTarMob = 0;
	CurListMob = 0;
	CurMaxMob = 0;
	DPSMob *pMob = 0;
	DPSMob::DPSEntry *pEnt = 0;
}

PLUGIN_API VOID OnDPSBeginZone(VOID) {
	//DebugSpewAlways("START ZONING");
	ZoneProcess();
	Zoning = true;
	CheckActive();
}

PLUGIN_API VOID OnDPSEndZone(VOID) {
	//DebugSpewAlways("END ZONING");
	Zoning = false;
	CheckActive();
}
