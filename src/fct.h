#pragma once
// FloatingCombatText: damage numbers rising from spawns in the 3D world, fed by the OP_Damage handler and
// drawn from the DrawWindows hook the Hud also uses (see fct.cpp).
#include <cstdint>
#include "patches.h"

extern const HookSite kFctHooks[2];
extern const RefSite kFctRefs[11];
const char* FctPrepare();
// Called once per frame from the shared DrawWindows hook, after the client's windows are drawn.
void FctFrame(uint8_t* mgr, bool inGame);

// FCTOptionsPage: Legends' "Combat Text" tab in the Options window (patched EQUI_OptionsWindow.xml), bound to
// the [FloatingCombat] settings. Needs FloatingCombatText applied first.
extern const HookSite kFctPageHooks[2];
extern const RefSite kFctPageRefs[6];
const char* FctPagePrepare();

// Shared with Nameplates / NameplateOptionsPage: the same hooks (signature, detour, trampoline slot), the FCT
// refs (kFctRefs) and the options page refs (kFctPageRefs) with their checks, and small accessors.
extern const char kFctDamageSig[];
extern void (*const g_fctDamageDetour)(uint8_t* pkt);
extern void (**const g_fctOrigDamageSlot)(uint8_t* pkt);
extern const char kFctRefreshSig[];
extern const char kFctNotifySig[];
extern void (*const g_fctRefreshDetour)(uint8_t* opt);
extern void (**const g_fctOrigRefreshSlot)(uint8_t* opt);
extern uint64_t (*const g_fctNotifyDetour)(uint8_t* opt, uint8_t* sender, int msg, void* data);
extern uint64_t (**const g_fctOrigNotifySlot)(uint8_t* opt, uint8_t* sender, int msg, void* data);
const char* FctSharedPrepare();        // kFctRefs checks, without turning FloatingCombatText on
const char* FctPageSharedPrepare();    // kFctPageRefs checks, without turning FCTOptionsPage on
uint8_t* FctDisplay();
uint8_t* FctCamera();                  // nullptr while zoning
void* FctRenderer();
const int32_t* FctViewport();          // x, y, w, h
uint8_t* FctLocalPlayer();
bool FctInGame();
uint8_t* FctSpawnById(uint32_t id);
uint8_t* FctFirstSpawn();              // next = *(spawn + 0x10)
bool FctProject(const float* world, float* sx, float* sy);   // false = behind the camera / no usable camera
uint8_t* OptOptionsWnd();
uint8_t* OptChild(uint8_t* parent, const char* id);           // silent GetChildItem
bool OptLooksLikeCheckbox(uint8_t* opt, uint8_t* box);
void OptSetBox(uint8_t* box, bool on);
bool OptIsChecked(uint8_t* box);
void OptSetText(uint8_t* wnd, const char* text);
void OptSliderSetValue(uint8_t* slider, int position);
void OptSliderSetTicks(uint8_t* slider, int ticks);
void OptComboSetChoice(uint8_t* combo, int row);
bool OptIsTabSwitchTo(uint8_t* opt, uint8_t* sender, int msg, void* data, uint8_t* page);
void FctShowLootText(uint32_t corpseId, const char* text, bool sale);   // Advanced Loot sale / loot text
