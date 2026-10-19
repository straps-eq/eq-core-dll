#pragma once
// Hud: on-screen text drawn from CXWndManager::DrawWindows. TargetDistance: a line drawn from the
// Target window's own Draw. Both share the ref table and the prepare checks; each has its own hook.
#include <cstdint>
#include "patches.h"

extern const HookSite kHudHooks[1];
extern const HookSite kTargetDistanceHooks[1];
extern const RefSite kHudRefs[22];
const char* HudPrepare();
const char* TargetDistancePrepare();

// Shared with FloatingCombatText (fct.cpp), which installs the same DrawWindows hook when the Hud is off
// and reuses kHudRefs. The helpers are valid once HudTextPrepare() has succeeded.
using DrawWindowsFn = int (*)(uint8_t* mgr);
extern const char kDrawWindowsSig[];
extern DrawWindowsFn g_origDrawWindows;
int DrawWindowsDetour(uint8_t* mgr);
const char* HudTextPrepare();
uint8_t* HudFont(uint8_t* mgr, int index);           // nullptr when the index is out of range
int HudFontCount(uint8_t* mgr);
int HudScreenSize(uint8_t* mgr, int* height);       // returns the width
int HudDrawText(uint8_t* font, const char* text, int x, int y, const int* clip, uint32_t argb);
int HudTextWidth(uint8_t* font, const char* text);
int HudLineHeight(uint8_t* font);
uint32_t HudUserColour(int index);                  // the client's user colour (Options > Colors), opaque ARGB
uint8_t** HudLocalPlayerRef();                      // &pLocalPlayer as kHudRefs resolved it
uint8_t* HudTarget();                               // the current target, or nullptr
void HudCXStr(void** str, const char* text);        // CXStr::CXStr(const char*)
void HudCXStrRelease(void** str);
void* HudLocalPC();                                 // *pLocalPC
uint32_t HudConColour(uint8_t* spawn);              // the con colour the Target window uses, opaque ARGB
