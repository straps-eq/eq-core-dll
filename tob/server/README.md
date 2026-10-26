# Server side for the ToB dinput8.dll

Most of the DLL is client-only. Three features need the server to send or accept something stock EQEmu doesn't:

| DLL feature | Server change | Rules (all off by default) |
|---|---|---|
| `AdvLootActions` (Sell / Store in the Advanced Loot window, auto actions) | `advloot/` | `Character:AdvancedLootActions` |
| `Nameplates` (HP / mana bars on NPCs you haven't targeted) | `nameplate-vitals/` | `Character:ToBNameplateVitalsRange` |
| `FloatingCombatText` (heal numbers with crits, overheal and other people's heals) | `heal-messages/` | `Spells:ToBHealMessages`, `Spells:ToBOtherHealMessageRange`, `Character:ToBLevelUpAppearance` |

Without the rules on, the server behaves exactly as before, and the DLL falls back:
- The loot window stays stock.
- Bars only fill for your target, group and extended targets.
- Heal numbers come from the old heal chat strings.

RoF2 and older clients never get anything new.

The patches are `git format-patch` output against an EQEmu tree with the ToB client patch (the `tob` component
patch system, `common/patches/tob*`, `utils/patches/patch_TOB.conf`). Apply each folder in numbered order with
`git am`. The folders are independent of each other. On a tree that has moved on, read them as a reference: every
change is small and self-contained.

## Advanced Loot actions (`advloot/`)

Players can sell a corpse item for its merchant value, or store it in the Tradeskill Depot (tradeskill items) or the
Dragon's Hoard (everything else), straight from the Advanced Loot window. No new opcode, no database change.

- `01-actions`: the sell / store paths, plus `Client::StoreInDragonHoard` / `StoreInTradeskillDepot` split out of the
  cursor deposit so a corpse item can be stored without going through the cursor.
- `02-refusals-free-the-request`: a refused action answers with result 6 (see below), so the client's row isn't left
  busy. Also drops the merge action.
- `03-chat-lines`: "You sold [N ]<item> for <coins>." and "You have added <item> to your Dragon's Hoard."
- `04-quest-loot-events`: sell / store run the quest `EVENT_LOOT` checks like a normal loot.
- `05-store-room-check`: storing checks for room before the quest event runs.
- `06-coin-sound`: the coin sound plays again for coin loot.

Wire contract (`OP_AdvLoot`, ToB only):

- **C2S sub 8, loot request:** `u16 8, u64 item, u32 corpse, u32 qty, u32 seq, u8 action`. The action byte at offset
  22 is what's new:
  - 0 / 1: loot
  - 3: sell
  - 5: store (the depot if the item fits there, else the hoard)
  - 6: depot only
  - 7: depot only, onto an existing stack
  - With the rule off, the server ignores this byte.
- **S2C sub 9, result:** `u16 9, u16 result, u32 1, u32 seq, u8 0`.
  - A successful sale (result 1) appends `u64 copper` at offset 13, making the packet 21 bytes.
  - A refusal sends result 6 for that `seq`, then sub `0x10` (clear pending), plus a chat line that names the item.
- **S2C sub `0x20`, capabilities:** `u16 0x20, u32 flags`.
  - Bits: 0 = sell, 1 = depot store, 2 = hoard store.
  - Sent right after the reply to every sub 3 sync, only when the rule is on.
  - The DLL shows its buttons only after it gets this packet.

Sell pays `Price * qty * Merchant:BuyCostMod` (no faction modifier). These are refused with a red line: NO DROP,
Price 0, attuned, augmented, a bag with contents, alt-currency items, Krono. A sale is recorded as a `MERCHANT_SELL`
player event ("Advanced Loot"). Store follows the depot and hoard rules (lore, no-rent, capacity, stack merge) and
needs `TradeskillDepotSlots` / `DragonHoardSlots` above 0.

## Nameplate vitals (`nameplate-vitals/`)

Stock EQEmu sends an NPC's HP only to clients that target it, have it on extended target, or own it. This sends HP
and mana percent to ToB clients within `Character:ToBNameplateVitalsRange` units, reusing the existing ToB encoders:
- `OP_MobHealth`: `int16 spawn_id, int32 percent`
- `OP_MobManaUpdate`: `int16 spawn_id, int32 percent`

Limits:
- At most one send per NPC every 500 ms, and the last value is always delivered.
- Clients that already get the HP through the normal paths are skipped.
- The work is skipped when the rule is 0, and in zones with no ToB player.

Keep the range at or below `Range:MobCloseScanDistance` (600). Above that, the close list is the whole zone. 200
matches the DLL's default draw distance.

## Heal messages (`heal-messages/`)

ToB has its own heal message, `OP_CombatText64`. The client prints the heal line itself ("X healed Y for N hit points
by Spell.", with "Critical" and "over time"), and the DLL draws the floating number from it. Layout (28 bytes):

```cpp
struct HealMessage_Struct {
	uint16 target_id;
	uint16 caster_id;
	int32  spell_id;   // -1 leaves the "by <spell>" part out
	int64  healed;
	int64  total;      // the client shows "healed (total)" when they differ
	uint32 flags;      // 1 over time, 4 critical
};
```

- `Spells:ToBHealMessages`: the healer and the target get this message instead of the old heal strings.
- `Spells:ToBOtherHealMessageRange`: other ToB clients within this range also get it, filtered by their PC / NPC
  spell filters.
- `Character:ToBLevelUpAppearance`: nearby ToB clients get `OP_LevelAppearance` when a player levels, so the DLL can
  show "Ding!" over them. The DLL doesn't draw this yet.

The `Spells:HealAmountMessageFilterThreshold` rule (heals under 100 send nothing) still applies.

## Client files

The DLL's window changes need the patched UI files in `../uifiles/default/`. Copy them into the client's
`uifiles\default` folder, or merge the marked blocks into a custom skin:

- `EQUI_OptionsWindow.xml`: the Combat Text and Nameplates tabs.
- `EQUI_AdvancedLootWnd.xml`: Store All / Sell All, the two checkboxes, and the AS / ST columns.
- `EQUI_LootFiltersWnd.xml`: the AS / ST columns in Edit Loot Filters.

## Item stats ToB cannot show (DLL feature ItemHiddenStats)

Opcode 0x7e51, unused by the ToB client. C2S `{u32 item_id}`: the DLL asks once per item id per session when an item window shows the item. S2C `{u32 item_id; u8 count; count x {u8 flags; i32 value; u8 length; char label[length]}}`: one entry per non-zero stat, `flags` bit 0 = show a % sign, `label` without a trailing NUL (e.g. `Shielding`, `Faction (Coldain)`). Answer every request, with `count` 0 when there is nothing to add; the DLL does not ask again for that id. Values are the base item's (no augments).
