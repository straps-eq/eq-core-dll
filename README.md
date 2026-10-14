# eq-core-dll (ToB)

A `dinput8.dll` proxy for the 64-bit EverQuest ToB client (`eqgame.exe` build `Sep 11 2025 11:54:10`).
It forwards DirectInput to the system DLL and applies opt-in client patches chosen in `eqcore.ini`.

Every patch site is found by byte signature, and its original bytes are checked before anything is written.
If a signature doesn't match on a given client build, that feature is skipped and logged to `eqcore.log`,
so a different build is never patched blindly.

## Features (all off by default)

| `eqcore.ini` key | What it does |
|---|---|
| `SoloLDoN` | LDoN adventure requests without a group (server must allow solo) |
| `MaxHPAbove10M` | Removes the client's three 10,000,000 HP caps |
| `AAPre51` | AA window usable below level 51 |
| `AAPre51Inventory` | Shows AA exp on the inventory window below 51 |
| `IllegalAugments` | Allows augment combinations the player can't use |
| `NoFoodDrinkSpam` | No client hunger/thirst processing or messages |
| `NoPatchme` | Starts `eqgame.exe` without the `patchme` argument |

Old-model horses need no patch on ToB: set `AllowPreLuclinMountRiders=1` under `[Defaults]` in `eqclient.ini`
(Options window, "Toggle Display of Pre-Luclin Characters on Mounts").

## Build

Visual Studio 2022 with the C++ workload. Run `build.bat`; output goes to `bin\`.

## Install

Copy `bin\dinput8.dll` and `eqcore.ini` next to `eqgame.exe`, then turn features on in `eqcore.ini`.

## Check a client without running it

`bin\eqcore_check.exe <path\to\eqgame.exe>` resolves every feature against the file (read-only) and prints
OK/FAIL per feature.
