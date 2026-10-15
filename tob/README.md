# eq-core-dll (ToB)

A `dinput8.dll` proxy for the 64-bit EverQuest ToB client (`eqgame.exe` build `Sep 11 2025 11:54:10`).
It forwards DirectInput to the system DLL and applies opt-in client patches chosen in `dinput8.ini`.

Every patch site is found by byte signature, and its original bytes are checked before anything is written.
If a signature doesn't match on a given client build, that feature is skipped and logged to `dinput8.log`,
so a different build is never patched blindly. Features can also hook a client function (inline x64 hook:
the function's first bytes go to a trampoline and are replaced by a jump to our code); hook sites and the
client functions/globals a hook uses are found and checked the same way.

## Features (the shipped `dinput8.ini` turns the safe ones on)

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
| `MapSpawns` | Draws spawns on the map window as labelled dots (NPCs in con colour, grey cons in a darker grey that reads on the parchment; players, group members, pets, mercenaries and corpses in their own colours) plus a line to your target. Only spawns the client itself shows you: nothing untargetable, invisible to you, or a trigger/trap/object. Nearest `MaxLabels` spawns; filters, colours and font under `[Map]` in `dinput8.ini` (`LabelSize` 1-3: the map's draw only carries the first three UI fonts, so larger text would need our own draw). Labels live on map layer 3 by default, so the map's layer buttons and Z filter apply |
| `BuffTooltips` | Buff, song and target buff tooltips show the caster (when the server sent one), the remaining time as the client formats it, one line per effect in words ("Increase Hitpoints by 120 (L1) to 450 (L60) per tick", values from the client's own calculator over the spell's level range) and the cure counters a spell carries ("needs cure: 3 poison"). Text is re-set only when it changes; options under `[Buffs]` in `dinput8.ini` |
| `TargetDistance` | One line in the Target window under the HP bar: the target's level, race and class ("8 Orc Warrior") in its con colour on the left and the 3D distance ("107.01") right-aligned; colour, font and offsets under `[TargetDistance]`. Drawn with the client's own text routine, so it works with any UI skin. No line-of-sight marker: the client's LOS test has not been located |
| `Hud` | A fixed text block at `[Hud]` `X`,`Y`: your loc (Y, X, Z like `/loc`), zone long name, target name and distance, FPS (measured from frame deltas) and the local clock. Each line has its own on/off key; colour and font under `[Hud]`. Shares one per-frame hook with `TargetDistance`; both draw only while in game |
| `ItemDisplayExtras` | Adds text below the item window's own description (the client's "Effect:" lines, lore and server item information stay): for each spell on the item its name and "Spell Effect: <description>"; a block with item id, icon id, stackable count, value in pp/gp/sp/cp, tribute values, weapon ratio and efficiency, lore text (when it differs from the name), "Procs at level" / "Clickable at level" (red when above your level), required/recommended level, container details and how long a clicky still has to cool down; then for every click/proc/worn/focus/scroll/blessing effect a "Spell Info for <kind> effect" block with the spell's id and duration, recovery and recast, range, resist and one "Slot N:" line per effect in words (values from the client's own calculator at the effect's cast level). Options and colours under `[ItemDisplay]` |
| `SpellWindowExtras` | Adds to the spell window (a buff's right-click "Show Spell Information", spell links in items and chat, spell gems) below the client's own text: "Spell Effect: <description>" when the client did not print it, then one block with "ID / Duration", "RecoveryTime / RecastTime", "Range", resist (detrimental spells), one "Slot N:" line per effect in words (a level range when the value scales, or the cast level the window was opened with), the classes that get the spell with their levels, category, subcategory and spell icon. Options under `[SpellWindow]` |

Old-model horses need no patch on ToB: set `AllowPreLuclinMountRiders=1` under `[Defaults]` in `eqclient.ini`
(Options window, "Toggle Display of Pre-Luclin Characters on Mounts").

## Build

Visual Studio 2022 with the C++ workload. Run `build.bat`; output goes to `bin\`.

## Install

Copy `bin\dinput8.dll` and `dinput8.ini` next to `eqgame.exe`, then turn features on in `dinput8.ini`.

## Check a client without running it

`bin\eqcore_check.exe <path\to\eqgame.exe>` resolves every feature against the file (read-only) and prints
OK/FAIL per feature.
