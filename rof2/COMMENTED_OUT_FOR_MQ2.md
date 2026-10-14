# Code Commented Out for MacroQuest Compatibility

This document tracks all code that has been commented out to prevent crashes with MacroQuest.

## Files Modified

### eqgame.cpp - InitHooks() function

**ALL patches and detours have been commented out:**

1. **MQ2 Injections** (lines ~771-782)
   - InitializeDisplayHook()
   - InitializeChatHook()
   - InitializeMQ2Commands()
   - InitializeMQ2Pulse()
   - InitializeMQ2Spawns()
   - InitializeMapPlugin()
   - InitializeMQ2ItemDisplay()
   - InitializeMQ2Labels()

2. **InitOptions()** (line ~786)
   - This disables ALL custom zones, NPCs, animations, shields, map/bazaar window disabling, luclin model disabling, etc.

3. **Heroic Stats Patches** (lines ~817-827)

4. **Old Model Horse Support** (lines ~829-834)

5. **Illegal Augments Patch** (lines ~836-840)

6. **HandleWorldMessage Detour** (lines ~842-844)

7. **Spell Data CRC** (lines ~846-858)

8. **Combat Damage Fix** (lines ~859-863)

9. **Max HP Fix** (lines ~868-875)

10. **SetCCreateCamera Detour** (lines ~920-922)

11. **Patchme Bypass** (lines ~924-929)

12. **Food/Drink Spam Disable** (lines ~931-937)

13. **MQ2 Prevention Patches** (lines ~939-962)

14. **Gamma Restore Detours** (lines ~964-974)
    - GetModuleFileNameA detour
    - SetDeviceGammaRamp detour

### MQ2Main.cpp - MQ2Initialize() function

1. **MQ2 Injections** (lines ~311-320)
   - InitializeDisplayHook()
   - InitializeChatHook()
   - InitializeMQ2Spawns()
   - InitializeMQ2Pulse()
   - InitializeMQ2Commands()
   - InitializeMQ2KeyBinds()

2. **MQ2 Plugins** (line ~323)
   - InitializeMQ2Plugins()

## Current State

The DLL now performs minimal operations:
- Loads the real dinput8.dll
- Initializes critical sections
- Does NOT apply any memory patches
- Does NOT hook any functions
- Does NOT inject any custom content

## To Re-enable Features

Uncomment sections one at a time in reverse order (from bottom to top) and test after each change to identify which specific feature causes the crash.

## Crash Details

Last crash occurred at:
- Location: eqlib::CXMLDataManager::GetXMLData+0
- This suggests MQ2 was crashing when accessing XML data, likely due to memory patches interfering with its initialization.
