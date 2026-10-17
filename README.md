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
| `TargetDistance` | One line in the Target window under the HP gauge: the target's level, race and class ("8 Orc Warrior") in its con colour on the left and the 3D distance ("107.01") right-aligned with the font's own width measure. Drawn from the window's own Draw, so windows above it cover it, clipped to its client area and placed in the first row below the gauge and its labels (`AutoPlace`); colour, font and offsets under `[TargetDistance]`. Uses the client's own text routine, so it works with any UI skin. No line-of-sight marker: the client's LOS test has not been located |
| `Hud` | A fixed text block at `[Hud]` `X`,`Y`: your loc (Y, X, Z like `/loc`), zone long name, target name and distance, FPS (measured from frame deltas) and the local clock. Each line has its own on/off key; colour and font under `[Hud]`. Draws only while in game |
| `ItemDisplayExtras` | Adds text below the item window's own description (the client's "Effect:" lines, lore and server item information stay): for each spell on the item its name and "Spell Effect: <description>" when the window did not print that description itself (it does, after its "Effect:" lines, for most items); a block with item id, icon id, stackable count, value in pp/gp/sp/cp, tribute values, weapon ratio and efficiency, lore text (when it differs from the name), "Procs at level" / "Clickable at level" (red when above your level), required/recommended level, container details and how long a clicky still has to cool down; then for every click/proc/worn/focus/scroll/blessing effect a "Spell Info for <kind> effect" block with the spell's id and duration, recovery and recast, range, resist and one "Slot N:" line per effect in words (values from the client's own calculator at the effect's cast level). Options and colours under `[ItemDisplay]` |
| `SpellWindowExtras` | Adds to the spell window (a buff's right-click "Show Spell Information", spell links in items and chat, spell gems) below the client's own text: "Spell Effect: <description>" when the client did not print it, then one block with "ID" (plus "Duration", "Range" and resist only when the client's text lacks them, or always with `ShowDuplicates=1`), "RecoveryTime / RecastTime", one "Slot N:" line per effect in words (a level range when the value scales, or the cast level the window was opened with), the classes that get the spell with their levels, category, subcategory and spell icon. Options under `[SpellWindow]` |
| `FloatingCombatText` | Floating combat text as in EverQuest Legends: damage numbers and the words Miss, Block, Parry, Riposte, Dodge, INVULNERABLE and Rune rise from the head of the spawn that was hit, in world space (they stay where the hit happened and are re-projected every frame), at a random angle and speed pulled down by gravity, with Legends' defaults. Client-only: fed by the OP_Damage packets the server already sends (hook on the OP_Damage handler, after the client has handled the packet) and drawn as 3D world text through the engine's own string sprite (the class name tags use), so it scales with distance like Legends (`[FCT]` `Render3D`, `LetterScale`, `MinPx`, `Occlude`; if the engine checks fail it falls back to 2D UI fonts and says so in `dinput8.log`). Groups as in Legends (Self: hits on you; You: your hits; Other: everyone else's, your pet's included, only within 200 units like the chat window), each with a text mode (None, Full, Abbreviated, DMG Only) and font scale; colours are your chat colours, crits twice the size, numbers live 4 s (crits 5 s) and words 2 s, nothing beyond 200 units or behind the camera, everything cleared on zoning. Pet hits the server sends twice are shown once. Settings under `[FloatingCombat]` (Legends' key names) and DLL extras under `[FCT]`; `Ctrl+Shift+F` turns it off and on. Not in yet: heals, Ding!, Out of Range, Fizzle, Missed Note, Resist, skill-up and loot/sell texts (their ToB handlers are not mapped), the pet/merc colour variants and the DoT green |
| `FCTOptionsPage` | Legends' "Combat Text" tab in the Options window (between Display and Mouse): Show Floating Combat Text, Sell/Loot Text, velocity, angle and gravity sliders with value labels, and per group (You / Self / Other) a font scale slider and a text-mode combobox. Every change is saved to `[FloatingCombat]` in `dinput8.ini` at once (`eqclient.ini` is not touched) and the page is re-filled from it whenever the tab is selected. Needs the patched `uifiles\default\EQUI_OptionsWindow.xml` (the page uses ToB templates only; apply the same block to a custom skin). Without it the feature stays silent. Requires `FloatingCombatText` |

Old-model horses need no patch on ToB: set `AllowPreLuclinMountRiders=1` under `[Defaults]` in `eqclient.ini`
(Options window, "Toggle Display of Pre-Luclin Characters on Mounts").

## Build

Visual Studio 2022 with the C++ workload. Run `build.bat`; output goes to `bin\`.

## Install

Copy `bin\dinput8.dll` and `dinput8.ini` next to `eqgame.exe`, then turn features on in `dinput8.ini`.

## Check a client without running it

`bin\eqcore_check.exe <path\to\eqgame.exe>` resolves every feature against the file (read-only) and prints
OK/FAIL per feature.
