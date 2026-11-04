#pragma once
// AA600: the byte rewrites are in aa600_sites.h. This adds the mirror of the first 300 owned AAs into the old profile
// slot for tools that read it directly (MacroQuest / E3Next). See aa600.cpp.
#include "patches.h"

extern const HookSite kAA600Hooks[1];
extern const RefSite kAA600Refs[3];
const char* AA600Prepare();
void AA600Frame(bool inGame);   // shared DrawWindows hook
