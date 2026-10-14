# eq-core-dll (ToB)

A `dinput8.dll` proxy for the 64-bit EverQuest ToB client (`eqgame.exe` build `Sep 11 2025 11:54:10`).
It forwards DirectInput to the system DLL and applies opt-in client patches chosen in `dinput8.ini`.

Every patch site is found by byte signature, and its original bytes are checked before anything is written.
If a signature doesn't match on a given client build, that feature is skipped and logged to `dinput8.log`,
so a different build is never patched blindly. Features can also hook a client function (inline x64 hook:
the function's first bytes go to a trampoline and are replaced by a jump to our code); hook sites and the
client functions/globals a hook uses are found and checked the same way.

## Features (all off by default)

| `dinput8.ini` key | What it does |
|---|---|
| `SoloLDoN` | LDoN adventure requests without a group (server must allow solo) |
| `NoDoubleDamage` | Stops the client applying melee hits to your own HP twice (bouncing HP bar, knocked out at about half health) |
| `MaxHPAbove10M` | Removes the client's three 10,000,000 HP caps |
| `AAPre51` | AA window usable below level 51 |
| `AAPre51Inventory` | Shows AA exp on the inventory window below 51 |
| `IllegalAugments` | Allows augment combinations the player can't use |
| `NoFoodDrinkSpam` | No client hunger/thirst processing or messages |
| `NoPatchme` | Starts `eqgame.exe` without the `patchme` argument |
| `ConNameTags` | Colours NPC name tags by con colour (pets, mercenaries, PCs and GM mode keep the client's colours). Tags refresh when a spawn's name is rebuilt or the target changes, and all NPCs are re-coloured after you level |
| `MapSpawns` | Draws spawns on the map window as labelled dots (NPCs in con colour; players, group members, pets, mercenaries and corpses in their own colours) plus a line to your target. Only spawns the client itself shows you: nothing untargetable, invisible to you, or a trigger/trap/object. Nearest `MaxLabels` spawns; filters, colours and font under `[Map]` in `dinput8.ini`. Labels live on map layer 3 by default, so the map's layer buttons and Z filter apply |
| `TargetDistance` | Draws the distance to your target (3D) inside the Target window, right-aligned at the top; colour, font and offsets under `[TargetDistance]`. Drawn with the client's own text routine, so it works with any UI skin |
| `Hud` | A fixed text block at `[Hud]` `X`,`Y`: your loc (Y, X, Z like `/loc`), zone long name, target name and distance, FPS (measured from frame deltas) and the local clock. Each line has its own on/off key; colour and font under `[Hud]`. Shares one per-frame hook with `TargetDistance`; both draw only while in game |

Old-model horses need no patch on ToB: set `AllowPreLuclinMountRiders=1` under `[Defaults]` in `eqclient.ini`
(Options window, "Toggle Display of Pre-Luclin Characters on Mounts").

## Build

Visual Studio 2022 with the C++ workload. Run `build.bat`; output goes to `bin\`.

## Install

Copy `bin\dinput8.dll` and `dinput8.ini` next to `eqgame.exe`, then turn features on in `dinput8.ini`.

## Check a client without running it

`bin\eqcore_check.exe <path\to\eqgame.exe>` resolves every feature against the file (read-only) and prints
OK/FAIL per feature.
