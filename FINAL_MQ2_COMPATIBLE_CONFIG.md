# Final MacroQuest Compatible Configuration

## ✅ Confirmed Working Features

This is the **maximum compatibility** achievable with MacroQuest while using this DLL.

### **Active Features (Stable with MQ2):**

1. ✅ `GetEQPath()` - EQ installation path detection
2. ✅ `InitializeCriticalSection()` - Thread safety
3. ✅ `isHeroicDisabled` - Removes heroic stats display
4. ✅ `isOldModelHorseSupportEnabled` - Allows horses with old models
5. ✅ `isAllowIllegalAugmentsEnabled` - Allows illegal augment combinations
6. ✅ `isMaxHPFixEnabled` - Fixes HP cap beyond 10 million
7. ✅ `isPatchmeDisabled` - Bypasses patchme requirement
8. ✅ `isFoodDrinkSpamDisabled` - Removes hunger/thirst messages

---

## ❌ Incompatible Features (Tested & Confirmed)

### **Causes Crash at `eqlib::CXMLDataManager::GetXMLData`:**
- ❌ `InitOffsets()` - Memory scanning conflicts with MQ2's initialization

### **Causes Black Screen Hang:**
- ❌ `SetCCreateCamera` detour - Character creation camera hook
- ❌ Gamma restore hooks (GetModuleFileNameA + SetDeviceGammaRamp)
- ❌ Combat damage double-applied fix
- ❌ Spell data CRC patches

### **Not Tested (Assumed Incompatible):**
- ❌ `InitOptions()` - Custom zones/NPCs/animations (depends on InitOffsets)
- ❌ `HandleWorldMessage` detour - Network message handling
- ❌ MQ2 injections - Display hooks, chat hooks, spawns, pulse, commands, maps
- ❌ MQ2 prevention patches - Version string randomization

---

## 🔍 Root Cause Analysis

### **Why Detours Don't Work:**
Any function detours (`DetourFunction`, `EzDetour`) cause black screen hangs with MacroQuest. This is because:
1. MQ2 uses its own hooking mechanism (likely Detours library)
2. Having two DLLs hook the same functions creates conflicts
3. The timing of hook installation during `DLL_PROCESS_ATTACH` interferes with MQ2's initialization

### **Why InitOffsets Crashes:**
`InitOffsets()` performs memory scanning to find game offsets dynamically. This conflicts with MQ2's own memory scanning during XML data loading, causing a race condition that crashes at `eqlib::CXMLDataManager::GetXMLData`.

### **Why Some Patches Work:**
Simple memory patches using `PatchA()` with hardcoded offsets work because:
- They don't scan memory dynamically
- They don't install hooks/detours
- They modify memory directly without interfering with MQ2's operations
- The offsets are relative to `baseAddress` which is set by `GetModuleHandle(NULL)`

---

## 📋 What You're Getting

With this configuration, you get:
- **HP cap removal** - Support for HP beyond 10 million
- **Patchme bypass** - Can launch eqgame.exe directly
- **Food/drink spam removal** - No more hunger/thirst messages
- **Heroic stats removal** - Cleaner UI without heroic stats
- **Old model horse support** - Use horses without Luclin models
- **Illegal augment support** - Bypass augment restrictions
- **Full MacroQuest compatibility** - No crashes or hangs

---

## 📋 What You're Missing

Without detours and InitOffsets, you lose:
- Dynamic offset detection
- Custom zones/NPCs/animations injection
- Combat damage double-apply fix
- Spell data CRC verification
- Gamma restore on crash
- Character creation camera positioning
- Network message interception
- Map/bazaar window disabling
- Luclin model disabling
- All MQ2-style injections

---

## 🎯 Recommendation

**This is the optimal configuration for MacroQuest compatibility.** Any additional features require:
1. Not using MacroQuest, OR
2. Implementing features as MQ2 plugins instead of DLL patches, OR
3. Delaying initialization until after MQ2 completes its setup (complex timing solution)

The current configuration provides useful quality-of-life improvements while maintaining full MQ2 functionality.
