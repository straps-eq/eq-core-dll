# Working Configuration with MacroQuest

## ✅ Confirmed Working Setup

This configuration allows the DLL to coexist with MacroQuest without crashes.

### **Critical - Must Stay Commented Out:**

1. **`InitOffsets()`** - Line 768 in eqgame.cpp
   - This function scans game memory for offsets
   - **Conflicts directly with MQ2's XML data access during initialization**
   - Causes crash at: `eqlib::CXMLDataManager::GetXMLData+0`

2. **`InitOptions()`** - Line 787 in eqgame.cpp
   - Applies custom zones, NPCs, animations, shields
   - May depend on offsets from InitOffsets()
   - Keep commented out for stability

3. **All MQ2 Injections** - Lines 772-782
   - Display hooks, chat hooks, spawns, pulse, commands, maps, item display, labels
   - These duplicate MQ2's own functionality and cause conflicts

4. **All Detours** - Various lines
   - HandleWorldMessage, SetCCreateCamera, gamma hooks
   - Network and function detours conflict with MQ2's hooks

5. **MQ2 Prevention/Checksum Patches**
   - Version string randomization
   - Checksum modifications
   - Spell data CRC

### **Currently Active (Safe with MQ2):**

✅ `GetEQPath()` - Gets EQ installation path
✅ `InitializeCriticalSection()` - Thread safety
✅ `isHeroicDisabled` - Removes heroic stats display
✅ `isOldModelHorseSupportEnabled` - Allows horses with old models
✅ `isAllowIllegalAugmentsEnabled` - Allows illegal augment combinations
✅ `isMaxHPFixEnabled` - Fixes HP cap beyond 10 million
✅ `isPatchmeDisabled` - Bypasses patchme requirement
✅ `isFoodDrinkSpamDisabled` - Removes hunger/thirst messages

### **Why This Works:**

The key insight is that **`InitOffsets()` performs memory scanning during `DLL_PROCESS_ATTACH`**, which happens at the same time MacroQuest is initializing and accessing XML data. This creates a race condition/conflict.

By skipping `InitOffsets()`:
- The DLL doesn't scan memory during initialization
- MQ2 can complete its XML data loading without interference
- Simple memory patches (heroic, HP, patchme, etc.) still work because they use hardcoded offsets relative to `baseAddress`
- `baseAddress` is set automatically by `GetModuleHandle(NULL)` in MQ2Globals.cpp

### **What You're Missing:**

Without `InitOffsets()` and `InitOptions()`, you lose:
- Dynamic offset detection (but hardcoded offsets still work)
- Custom zones injection
- Custom NPCs injection
- Custom animations injection
- Custom shields injection
- Map window disabling
- Bazaar window disabling
- Luclin model disabling

### **If You Need Those Features:**

You would need to delay the initialization until after MQ2 completes its setup. Possible approaches:
1. Move `InitOffsets()` and `InitOptions()` to a later hook (after character select)
2. Add a delay/wait mechanism to ensure MQ2 finishes first
3. Use MQ2's plugin system instead of a separate DLL

### **Bottom Line:**

This configuration gives you the essential patches (HP fix, patchme bypass, food/drink spam, heroic stats, etc.) while maintaining full MacroQuest compatibility.
