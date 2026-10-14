// MQ2Labels.cpp : Defines the entry point for the DLL application.
//

// MQ2 Custom Labels


#include "MQ2Main.h"
#include <map>

typedef string(*pEqTypesFunc)();

map<DWORD, pEqTypesFunc> eqTypesMap;

// CSidlManager::CreateLabel 0x5F2470

// the tool tip is already copied out of the 
// in class CControlTemplate.  use this struct
// to mock up the class, so we don't have to
// worry about class instatiation and crap

struct _CControl {
    /*0x000*/    DWORD Fluff[0x24]; // if this changes update ISXEQLabels.cpp too
    /*0x090*/    CXSTR * EQType;
};

// optimize off because the tramp looks blank to the compiler
// and it doesn't respect the fact the it will be a real routine
#pragma optimize ("g", off)

class CSidlManagerHook {
public:
    class CXWnd * CreateLabel_Trampoline(class CXWnd *, struct _CControl *);
    class CXWnd * CreateLabel_Detour(class CXWnd *CWin, struct _CControl *CControl)
    {
        CLABELWND *p;
        class CXWnd *tmp = CreateLabel_Trampoline(CWin, CControl);
        p = (CLABELWND *)tmp;
        if (CControl->EQType) {
            *((DWORD *)&p->SidlPiece) = atoi(CControl->EQType->Text);
        } else {
            *((DWORD *)&p->SidlPiece) = 0;
        }

        return tmp;
    }
};

DETOUR_TRAMPOLINE_EMPTY(class CXWnd * CSidlManagerHook::CreateLabel_Trampoline(class CXWnd *, struct _CControl *));

#pragma optimize ("g", on)

// CLabelHook::Draw_Detour

class CLabelHook {
public:
    VOID Draw_Trampoline(VOID);
    VOID Draw_Detour(VOID)
    {
        PCLABELWND pThisLabel;
        __asm {mov [pThisLabel], ecx};
        //          (PCLABELWND)this;
        Draw_Trampoline();
        CHAR Buffer[MAX_STRING] = {0};
        BOOL Found=FALSE;
        DWORD index;

		std::string eqtypesString = "";


        if ((DWORD)pThisLabel->SidlPiece==9999) {
            if (!pThisLabel->Wnd.XMLToolTip) {
                strcpy(Buffer,"BadCustom");
                Found=TRUE;
            } else {
                //strcpy(Buffer,&pThisLabel->XMLToolTip->Text[0]);
                STMLToPlainText(&pThisLabel->Wnd.XMLToolTip->Text[0],Buffer);
                ParseMacroParameter(((PCHARINFO)pCharData)->pSpawn,Buffer);
                if (!strcmp(Buffer,"NULL"))
                    Buffer[0]=0;
                Found=TRUE;
            }
        } else if ((DWORD)pThisLabel->SidlPiece>=1000) {
            for (auto eqtype : eqTypesMap) {
                if (eqtype.first==(DWORD)pThisLabel->SidlPiece) {

					auto func = eqtype.second;
					if (func)
					{
						eqtypesString = (*func)();
						Found = TRUE;
						break;
					}
                }
            }
        }
        if (Found) SetCXStr(&(pThisLabel->Wnd.WindowText),(PCHAR)eqtypesString.c_str());
    }
}; 

DETOUR_TRAMPOLINE_EMPTY(VOID CLabelHook::Draw_Trampoline(VOID));

BOOL StealNextGauge=FALSE;
DWORD NextGauge=0;

std::string testDisplayFunction()
{
	return "Test";
}

// Called once, when the plugin is to initialize
PLUGIN_API VOID InitializeMQ2Labels(VOID)
{
 //   DebugSpewAlways("Initializing MQ2Labels");
	eqTypesMap[1000] = testDisplayFunction; //and so forth 

    // Add commands, macro parameters, hooks, etc.
    //EasyClassDetour(CLabel__Draw,CLabelHook,Draw_Detour,VOID,(VOID),Draw_Trampoline);
    EzDetour(CLabel__Draw,&CLabelHook::Draw_Detour,&CLabelHook::Draw_Trampoline);
    EzDetour(CSidlManager__CreateLabel,&CSidlManagerHook::CreateLabel_Detour,&CSidlManagerHook::CreateLabel_Trampoline);


    // currently in testing:
    //    EasyClassDetour(CGauge__Draw,CGaugeHook,Draw_Detour,VOID,(VOID),Draw_Trampoline);
    //    EasyDetour(__GetGaugeValueFromEQ,GetGaugeValueFromEQ_Hook,int,(int,class CXStr *,bool *),GetGaugeValueFromEQ_Trampoline);
}

// Called once, when the plugin is to shutdown
PLUGIN_API VOID ShutdownLabelsPlugin(VOID)
{
   // DebugSpewAlways("Shutting down MQ2Labels");

    // Remove commands, macro parameters, hooks, etc.
    RemoveDetour(CSidlManager__CreateLabel);
    RemoveDetour(CLabel__Draw);
    //RemoveDetour(CGaugeWnd__Draw);
    //RemoveDetour(__GetGaugeValueFromEQ);
}

