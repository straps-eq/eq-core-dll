// AdvLootActions: Sell / Store in the ToB Advanced Loot window, like EverQuest Legends (without its Merge)
// (analysis\legends\advloot-buttons\dll-spec.md, ui-all-options\dll-spec-addendum.md section 3, server contract in
// analysis\legends\server\README.md).
//
// Buttons Store All / Sell All (ADLW_PLL_*AllBtn from the patched EQUI_AdvancedLootWnd.xml) and right-click
// entries "Sell / Store this item" (ids 0x40-0x41) on the Personal list. Every request goes
// through the client's own FUN_140161230 (it fills the sequence number and tracks the pending row, so the row
// disappears on the result): OP_AdvLoot sub 8 {u16 8, u64 item, u32 corpse, u32 qty, u32 seq, u8 action}, action
// 3 sell, 5 store (the server picks Tradeskill Depot or Dragon's Hoard), 6 depot only. One request per
// (item, corpse) and only for rows the client would loot itself (not pending, corpse entry not locked), the row
// marked pending first as the Loot button does.
//
// Capability gate (mandatory): the server sends S2C sub 0x20 {u16 0x20, u32 flags} after every sub 3 sync (bit0
// sell, bit1 depot, bit2 hoard, bit3 merge); the flags are cleared on sub 3. With flags 0 the buttons and
// checkboxes are hidden, no menu entry is added and no action byte is ever sent (an older server would treat it as
// a plain loot).
//
// Hooks: A CAdvancedLootWnd::WndNotification FUN_1400adc10 (button clicks, checkbox clicks, menu selections
// 0x14), B CContextMenuManager::PopupMenu FUN_1405bca80 (adds our entries to the Personal menu after the client's
// own), C the AdvLoot subtype dispatcher FUN_14015dec0 (capabilities), D the sub 9 result handler FUN_14015da60
// and E the request sender FUN_140161230 (seq -> item name and corpse, for the floating text). Sale results carry a
// trailing u64 copper at offset 13; a sale shows "<item> (Xp Yg Zs Wc)" and a loot the item name over the corpse
// through FloatingCombatText ([FloatingCombat] SellText / LootText). Refusals come as sub 0x10 plus a chat line.
//
// Loot-window checkboxes ADLW_ShiftForAllCheck (the All buttons act only while Shift is held) and
// ADLW_AutoActionsCheck (auto filters), saved in dinput8.ini [AdvLoot]. Auto filters: per character in
// userdata\eqcore_advloot_<Character>.ini, [Filters] item_id=action (3 sell, 5 store, 6 depot) plus
// item_id_name=<name>; toggled from the right-click menu (0x43 Auto-sell, 0x44 Auto-store, 0x46
// Clear auto action) and applied once per frame from the DrawWindows hook while the window has rows and the
// server's flags are set: once per (item, corpse), never on pending rows, never on NO TRADE rows unless
// AutoNoTrade=1. The file also keeps item_id_icon=<icon id> for the Edit Loot Filters rows.
//
// Edit Loot Filters window (CLootFiltersWnd, second feature table, same ini key): the patched EQUI_LootFiltersWnd.xml
// adds columns 8 AS and 9 ST after Remove. The client hard-codes columns 3-7 (AN, AG, NV, auto-roll, Remove) and
// never looks at 8+, so the DLL fills them: H2 post-hook on AddOrUpdateRow FUN_1401ad5f0 clones LF_CheckBoxTemplate
// into the two cells the way the client does for AN/AG/NV (SidlFindDef, SidlCreate, SetCellWnd) and sets them from the
// DLL table; H3 post-hook on Rebuild FUN_1401aea30 adds rows for items that only have a DLL action (a stack record
// {0, item, icon, name} through AddOrUpdateRow in search mode, so the search text and "Max search results" apply);
// H1 detour on CLootFiltersWnd::WndNotification FUN_1401af130 handles clicks on AS/ST (mutually exclusive; both off
// clears the action) and, for DLL-only rows, Remove and AN/AG/NV/auto-roll (creating the client's record with
// FUN_14015a770 turns the row into a stock one); H4 pre-hook on OnProcessFrame FUN_1401aee00: the client deletes the
// row for the item in wnd+0x324 on this path, so Remove also clears the DLL action and a row whose stock flags fell to
// 0 while a DLL action remains is kept; changes from the Advanced Loot menu are synced on the next visible frame.
// H5 on the Merge / Replace confirm callback FUN_140158490 merges / replaces the DLL file from the other character's.
// Cells are only attached when the list reports at least 10 columns, AutoFilters is on and the server's flags are set.
#include "advloot.h"
#include "hud.h"
#include "fct.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// CAdvancedLootWnd
const size_t kWndPllList = 0x2e0;        // CListWnd* Personal list
const size_t kWndApplyFiltersCheck = 0x330;   // the window's own checkbox (vtable reference for ours)
const size_t kWndPllRows = 0x3e0;        // row container*
const size_t kWndPllMenuIndex = 0x410;   // int, -1 until the first right-click
const size_t kWndInitDone = 0x414;       // u8
const size_t kWndVisible = 0x211;        // u8 (CXWnd)
// row container
const size_t kRowsBase = 0x08;
const size_t kRowsCount = 0x10;
const size_t kRowsList = 0x20;           // CListWnd*
const size_t kRowSize = 0xa8;
// CListWnd
const size_t kListRowCount = 0x268;
// Personal row
const size_t kRowItemId = 0x00;          // u64
const size_t kRowName = 0x08;            // char[64]
const size_t kRowIcon = 0x48;            // u32 (the client's auto-roll box passes it as the filter record's icon)
const size_t kRowNoTrade = 0x54;         // u8
const size_t kRowMenuOpen = 0x6a;        // u8
const size_t kRowPending = 0x6d;         // u8
const size_t kRowCorpseBase = 0x78;      // sub-entry array
const size_t kRowCorpseCount = 0x80;     // int
const size_t kSubSize = 0x50;
const size_t kSubCorpseId = 0x00;        // u32
const size_t kSubQty = 0x04;             // u16
const size_t kSubLocked = 0x0c;          // u8
// CContextMenuManager / CContextMenu
const size_t kMenuArray = 0x2b0;
const size_t kMenuCount = 0x22b0;
const size_t kVtblAddMenuItem = 0x3a0;
// CXWnd / checkbox
const size_t kVtblShow = 0x1c0;
const size_t kVtblIsChecked = 0x348;
const size_t kVtblSetCheck = 0x380;
const uint8_t kIsCheckedStart[] = { 0x0F, 0xB6, 0x81, 0x70, 0x02, 0x00, 0x00, 0xC3 };
const uint8_t kSetCheckStart[] = { 0x38, 0x91, 0x70, 0x02, 0x00, 0x00, 0x74 };
// local player
const size_t kSpawnName = 0xb4;          // char[0x40]

const int kNotifyClick = 1, kNotifyMenu = 0x14;
// Merge (Legends' 0x42 / 0x45, action 4, capability bit 3) is left out: this server has no such gameplay.
const int kMenuSell = 0x40, kMenuStore = 0x41;
const int kMenuAutoSell = 0x43, kMenuAutoStore = 0x44, kMenuAutoClear = 0x46;
const uint8_t kActionSell = 3, kActionStore = 5, kActionDepot = 6;
const uint32_t kCapSell = 1, kCapDepot = 2, kCapHoard = 4;
const uint16_t kSubSync = 3, kSubResult = 9, kSubCapabilities = 0x20;

#pragma pack(push, 1)
struct PacketStream {
	const uint8_t* data;
	uint32_t length;
	uint32_t position;
};
#pragma pack(pop)
struct AdvLootRequest {
	uint64_t item;
	uint32_t corpse;
	uint32_t qty;
	uint32_t seq;
	uint8_t action;
	uint8_t pad[3];
};

using WndNotifyFn = int64_t (*)(uint8_t* wnd, uint8_t* sender, uint64_t notification, void* data);
using PopupFn = int64_t (*)(uint8_t* mgr, int menuIndex, void* point, uint8_t* wnd, uint8_t flag);
using DispatchFn = uint64_t (*)(uint8_t* mgr, const uint8_t* data, uint32_t length);
using ResultFn = uint64_t (*)(uint8_t* mgr, PacketStream* stream);
using SendFn = uint64_t (*)(uint8_t* mgr, AdvLootRequest* req);
using LootMgrFn = uint8_t* (*)();
using GetChildFn = uint8_t* (*)(uint8_t* parent, void** name, char log);
using ListItemDataFn = int64_t (*)(uint8_t* list, int row);
// As the client calls it (FUN_1400ad9c0): r9 = 0, then -1 and 1 on the stack.
using AddMenuItemFn = int (*)(uint8_t* menu, void** text, int id, void* zero, int minusOne, uint8_t one);
using ShowFn = void (*)(uint8_t* wnd, uint8_t visible, uint8_t unknown);
using IsCheckedFn = uint8_t (*)(uint8_t* box);
using SetCheckFn = void (*)(uint8_t* box, uint8_t on, uint8_t unused);

WndNotifyFn g_origNotify = nullptr;      // FUN_1400adc10
PopupFn g_origPopup = nullptr;           // FUN_1405bca80
DispatchFn g_origDispatch = nullptr;     // FUN_14015dec0
ResultFn g_origResult = nullptr;         // FUN_14015da60
SendFn g_origSend = nullptr;             // FUN_140161230 (trampoline)
SendFn g_send = nullptr;                 // FUN_140161230 (through our detour)
LootMgrFn g_lootMgr = nullptr;           // FUN_14015d480
GetChildFn g_getChild = nullptr;         // FUN_14059f8a0
ListItemDataFn g_listItemData = nullptr; // FUN_1405b62e0
uint8_t** g_pAdvLootWnd = nullptr;       // &DAT_140db4e70
uint8_t** g_menuMgr = nullptr;           // &DAT_140f0f5b0
uint8_t* g_notifyFn = nullptr;           // FUN_1400adc10, for the prepare check
bool g_on = false;
uint32_t g_caps = 0;                     // the server's capability flags (0 = feature absent)

// ---- configuration ([AdvLoot]) ----

char g_iniPath[MAX_PATH] = {};
char g_gameDir[MAX_PATH] = {};
bool g_shiftForAll = true;
bool g_autoFilters = false;
bool g_autoNoTrade = false;

void LoadConfig() {
	GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH);
	char* slash = strrchr(g_gameDir, '\\');
	if (slash) *slash = '\0';
	snprintf(g_iniPath, sizeof(g_iniPath), "%s\\dinput8.ini", g_gameDir);
	g_shiftForAll = GetPrivateProfileIntA("AdvLoot", "ShiftForAll", 1, g_iniPath) != 0;
	g_autoFilters = GetPrivateProfileIntA("AdvLoot", "AutoFilters", 0, g_iniPath) != 0;
	g_autoNoTrade = GetPrivateProfileIntA("AdvLoot", "AutoNoTrade", 0, g_iniPath) != 0;
}

void SaveFlag(const char* key, bool on) {
	if (g_iniPath[0]) WritePrivateProfileStringA("AdvLoot", key, on ? "1" : "0", g_iniPath);
}

// ---- auto filters (userdata\eqcore_advloot_<Character>.ini) ----

struct Filter {
	uint64_t item;
	uint8_t action;
	uint32_t icon;      // item icon id, for the Edit Loot Filters row
	char name[64];
};
const int kMaxFilters = 512;
Filter g_filters[kMaxFilters];
int g_filterCount = 0;
char g_filterFile[MAX_PATH] = {};
char g_filterChar[0x40] = {};
bool g_lfDirty = false;      // the Edit Loot Filters window has to be brought in line with the table

const char* CharacterName() {
	uint8_t* me = *HudLocalPlayerRef();
	return me ? reinterpret_cast<const char*>(me + kSpawnName) : nullptr;
}

// Reads a [Filters] section into the table: item_id=action, then item_id_name / item_id_icon for those items.
int ReadFilterFile(const char* file, Filter* out, int max) {
	int count = 0;
	static char keys[32768];
	DWORD n = GetPrivateProfileSectionA("Filters", keys, sizeof(keys), file);
	for (const char* p = keys; n && *p && count < max; p += strlen(p) + 1) {
		const char* eq = strchr(p, '=');
		char* end = nullptr;
		uint64_t item = _strtoui64(p, &end, 10);
		int action = eq ? atoi(eq + 1) : 0;
		if (!eq || end != eq || !item || (action != kActionSell && action != kActionStore && action != kActionDepot))
			continue;
		out[count] = {};
		out[count].item = item;
		out[count++].action = static_cast<uint8_t>(action);
	}
	for (const char* p = keys; n && *p; p += strlen(p) + 1) {
		const char* eq = strchr(p, '=');
		char* end = nullptr;
		uint64_t item = _strtoui64(p, &end, 10);
		if (!eq || !item || end == eq || *end != '_') continue;
		int i = 0;
		while (i < count && out[i].item != item) ++i;
		if (i == count) continue;
		if (strncmp(end, "_name=", 6) == 0) strncpy_s(out[i].name, eq + 1, _TRUNCATE);
		else if (strncmp(end, "_icon=", 6) == 0) out[i].icon = static_cast<uint32_t>(strtoul(eq + 1, nullptr, 10));
	}
	return count;
}

// Loads the character's filter file the first time that character is seen.
void LoadFilters() {
	const char* name = CharacterName();
	if (!name || !name[0] || strncmp(name, g_filterChar, sizeof(g_filterChar)) == 0) return;
	strncpy_s(g_filterChar, name, _TRUNCATE);
	snprintf(g_filterFile, sizeof(g_filterFile), "%s\\userdata\\eqcore_advloot_%s.ini", g_gameDir, g_filterChar);
	g_filterCount = ReadFilterFile(g_filterFile, g_filters, kMaxFilters);
	g_lfDirty = true;
}

int FindFilter(uint64_t item) {
	for (int i = 0; i < g_filterCount; ++i)
		if (g_filters[i].item == item) return i;
	return -1;
}

void WriteFilterKey(const char* file, uint64_t item, const char* suffix, const char* value) {
	char key[32];
	snprintf(key, sizeof(key), "%llu%s", static_cast<unsigned long long>(item), suffix);
	WritePrivateProfileStringA("Filters", key, value, file);
}

void SetFilter(uint64_t item, const char* name, uint32_t icon, uint8_t action) {
	if (!g_filterFile[0]) return;
	g_lfDirty = true;
	const int i = FindFilter(item);
	if (!action) {
		if (i >= 0) g_filters[i] = g_filters[--g_filterCount];
		WriteFilterKey(g_filterFile, item, "", nullptr);
		WriteFilterKey(g_filterFile, item, "_name", nullptr);
		WriteFilterKey(g_filterFile, item, "_icon", nullptr);
		return;
	}
	Filter* f = i >= 0 ? &g_filters[i] : g_filterCount < kMaxFilters ? &g_filters[g_filterCount++] : nullptr;
	if (f) {
		f->item = item;
		f->action = action;
		f->icon = icon;
		strncpy_s(f->name, name ? name : "", _TRUNCATE);
	}
	char dir[MAX_PATH], value[16];
	snprintf(dir, sizeof(dir), "%s\\userdata", g_gameDir);
	CreateDirectoryA(dir, nullptr);
	snprintf(value, sizeof(value), "%u", action);
	WriteFilterKey(g_filterFile, item, "", value);
	WriteFilterKey(g_filterFile, item, "_name", name);
	snprintf(value, sizeof(value), "%u", icon);
	WriteFilterKey(g_filterFile, item, "_icon", value);
}

// (item, corpse) pairs the server refused (or never answered): the auto filters don't ask for them again. Pairs that
// succeed are not recorded, so the next copy of the same item on the same corpse is handled too. Cleared on sync
// and when the window empties.
struct Done {
	uint64_t item;
	uint32_t corpse;
};
const int kMaxDone = 256;
Done g_done[kMaxDone];
int g_doneCount = 0;

bool AlreadyDone(uint64_t item, uint32_t corpse) {
	for (int i = 0; i < g_doneCount; ++i)
		if (g_done[i].item == item && g_done[i].corpse == corpse) return true;
	return false;
}

void MarkDone(uint64_t item, uint32_t corpse) {
	if (g_doneCount == kMaxDone) {
		memmove(g_done, g_done + 1, sizeof(Done) * (kMaxDone - 1));
		--g_doneCount;
	}
	g_done[g_doneCount++] = { item, corpse };
}

// ---- requests ----

// What each request was for, by sequence number, for the result's floating text.
struct Sent {
	uint32_t seq;
	uint32_t corpse;
	uint64_t item;
	uint64_t when;      // GetTickCount64 at send
	uint8_t action;
	bool used;          // no result yet
	char name[64];
};
const int kMaxSent = 64;
Sent g_sent[kMaxSent];
int g_sentNext = 0;

uint8_t* PersonalRows(uint8_t* wnd) {
	return *reinterpret_cast<uint8_t**>(wnd + kWndPllRows);
}

uint8_t* RowAt(uint8_t* rows, int listRow) {
	uint8_t* list = *reinterpret_cast<uint8_t**>(rows + kRowsList);
	if (!list) return nullptr;
	const int idx = static_cast<int>(g_listItemData(list, listRow));
	if (idx < 0 || idx >= *reinterpret_cast<int*>(rows + kRowsCount)) return nullptr;
	return *reinterpret_cast<uint8_t**>(rows + kRowsBase) + static_cast<size_t>(idx) * kRowSize;
}

int ListRows(uint8_t* rows) {
	uint8_t* list = *reinterpret_cast<uint8_t**>(rows + kRowsList);
	return list ? *reinterpret_cast<int*>(list + kListRowCount) : 0;
}

const char* NameOfItem(uint64_t item) {
	uint8_t* wnd = *g_pAdvLootWnd;
	uint8_t* rows = wnd ? PersonalRows(wnd) : nullptr;
	if (!rows) return nullptr;
	const int count = *reinterpret_cast<int*>(rows + kRowsCount);
	uint8_t* base = *reinterpret_cast<uint8_t**>(rows + kRowsBase);
	for (int i = 0; base && i < count; ++i) {
		uint8_t* row = base + static_cast<size_t>(i) * kRowSize;
		if (*reinterpret_cast<uint64_t*>(row + kRowItemId) == item) return reinterpret_cast<const char*>(row + kRowName);
	}
	return nullptr;
}

// One request per (item, corpse) of the row, as the client's Loot button does. `autoOnly` skips pairs the server
// refused before. Returns the number of requests.
int RequestRow(uint8_t* row, uint8_t action, bool autoOnly) {
	if (!g_caps || row[kRowPending]) return 0;
	const uint64_t item = *reinterpret_cast<uint64_t*>(row + kRowItemId);
	const int corpses = *reinterpret_cast<int*>(row + kRowCorpseCount);
	uint8_t* base = *reinterpret_cast<uint8_t**>(row + kRowCorpseBase);
	int sent = 0;
	for (int j = 0; base && j < corpses; ++j) {
		uint8_t* sub = base + static_cast<size_t>(j) * kSubSize;
		if (sub[kSubLocked]) continue;
		const uint32_t corpse = *reinterpret_cast<uint32_t*>(sub + kSubCorpseId);
		const uint16_t qty = *reinterpret_cast<uint16_t*>(sub + kSubQty);
		if (!qty) continue;
		if (autoOnly && AlreadyDone(item, corpse)) continue;   // refused before: don't ask again
		AdvLootRequest req = { item, corpse, qty, 0, action, {} };
		row[kRowPending] = 1;
		g_send(g_lootMgr(), &req);
		++sent;
	}
	return sent;
}

int ForEachPersonalRow(uint8_t* wnd, uint8_t action) {
	uint8_t* rows = PersonalRows(wnd);
	if (!rows) return 0;
	int sent = 0;
	for (int i = 0, n = ListRows(rows); i < n; ++i) {
		if (uint8_t* row = RowAt(rows, i)) sent += RequestRow(row, action, false);
	}
	return sent;
}

// The row whose context menu is open (the client's FUN_1400a5540 logic), or nullptr.
uint8_t* RowWithMenuOpen(uint8_t* wnd) {
	uint8_t* rows = PersonalRows(wnd);
	if (!rows) return nullptr;
	for (int i = 0, n = ListRows(rows); i < n; ++i) {
		uint8_t* row = RowAt(rows, i);
		if (row && row[kRowMenuOpen]) return row;
	}
	return nullptr;
}

// ---- window controls ----

struct Controls {
	uint8_t* wnd;
	uint8_t* merge;
	uint8_t* store;
	uint8_t* sell;
	uint8_t* shiftBox;
	uint8_t* autoBox;
	uint32_t shownCaps;     // the flags the visibility was last set for (+1 so 0 is "never")
};
Controls g_ctl = {};

uint8_t* Child(uint8_t* parent, const char* id) {
	void* s = nullptr;
	HudCXStr(&s, id);
	if (!s) return nullptr;
	uint8_t* wnd = g_getChild(parent, &s, 0);
	HudCXStrRelease(&s);
	return wnd;
}

void* Virtual(uint8_t* wnd, size_t slot) {
	return (*reinterpret_cast<void***>(wnd))[slot / sizeof(void*)];
}

// `ref` is a window known to be a checkbox (or null): same vtable, or the client's IsChecked / SetCheck bodies.
bool LooksLikeCheckbox(uint8_t* ref, uint8_t* box) {
	void** vt = *reinterpret_cast<void***>(box);
	if (!vt) return false;
	if (ref && *reinterpret_cast<void***>(ref) == vt) return true;
	return memcmp(vt[kVtblIsChecked / sizeof(void*)], kIsCheckedStart, sizeof(kIsCheckedStart)) == 0 &&
	       memcmp(vt[kVtblSetCheck / sizeof(void*)], kSetCheckStart, sizeof(kSetCheckStart)) == 0;
}

void Show(uint8_t* wnd, bool visible) {
	if (wnd) reinterpret_cast<ShowFn>(Virtual(wnd, kVtblShow))(wnd, visible ? 1 : 0, 1);
}

void SetBox(uint8_t* box, bool on) {
	if (box) reinterpret_cast<SetCheckFn>(Virtual(box, kVtblSetCheck))(box, on ? 1 : 0, 0);
}

bool IsChecked(uint8_t* box) {
	return reinterpret_cast<IsCheckedFn>(Virtual(box, kVtblIsChecked))(box) != 0;
}

// Looks the controls up once per window (a UI reload builds a new one); missing controls stay null.
void Resolve(uint8_t* wnd) {
	if (g_ctl.wnd == wnd) return;
	Controls c = {};
	c.wnd = wnd;
	c.merge = Child(wnd, "ADLW_PLL_MergeAllBtn");
	c.store = Child(wnd, "ADLW_PLL_StoreAllBtn");
	c.sell = Child(wnd, "ADLW_PLL_SellAllBtn");
	c.shiftBox = Child(wnd, "ADLW_ShiftForAllCheck");
	c.autoBox = Child(wnd, "ADLW_AutoActionsCheck");
	uint8_t* ref = *reinterpret_cast<uint8_t**>(wnd + kWndApplyFiltersCheck);
	if (c.shiftBox && !LooksLikeCheckbox(ref, c.shiftBox)) c.shiftBox = nullptr;
	if (c.autoBox && !LooksLikeCheckbox(ref, c.autoBox)) c.autoBox = nullptr;
	g_ctl = c;
	SetBox(c.shiftBox, g_shiftForAll);
	SetBox(c.autoBox, g_autoFilters);
}

// Buttons and boxes follow the server's flags (hidden without the feature).
void UpdateVisibility(uint8_t* wnd) {
	Resolve(wnd);
	if (g_ctl.shownCaps == g_caps + 1) return;
	g_ctl.shownCaps = g_caps + 1;
	Show(g_ctl.sell, (g_caps & kCapSell) != 0);
	Show(g_ctl.store, (g_caps & (kCapDepot | kCapHoard)) != 0);
	Show(g_ctl.merge, false);   // an older XML may still have the Merge All button
	Show(g_ctl.shiftBox, g_caps != 0);
	Show(g_ctl.autoBox, g_caps != 0);
}

uint8_t StoreAction() {
	return kActionStore;   // the server picks the depot for tradeskill stacks, else the hoard
}

bool Allowed(uint8_t action) {
	switch (action) {
	case kActionSell: return (g_caps & kCapSell) != 0;
	case kActionStore: return (g_caps & (kCapDepot | kCapHoard)) != 0;
	case kActionDepot: return (g_caps & kCapDepot) != 0;
	default: return false;
	}
}

// ---- hooks ----

bool PllCellClick(uint8_t* wnd, uint8_t* sender);

int64_t NotifyDetour(uint8_t* wnd, uint8_t* sender, uint64_t notification, void* data) {
	if (!g_on || !wnd || !wnd[kWndInitDone] || !g_caps || !sender) return g_origNotify(wnd, sender, notification, data);
	UpdateVisibility(wnd);
	if (notification == kNotifyClick) {
		if (PllCellClick(wnd, sender)) return 0;
		if (sender == g_ctl.shiftBox || sender == g_ctl.autoBox) {
			const bool on = IsChecked(sender);
			if (sender == g_ctl.shiftBox) {
				g_shiftForAll = on;
				SaveFlag("ShiftForAll", on);
			}
			else {
				g_autoFilters = on;
				SaveFlag("AutoFilters", on);
				g_lfDirty = true;
			}
			return 0;
		}
		uint8_t action = 0;
		if (sender == g_ctl.sell) action = kActionSell;
		else if (sender == g_ctl.store) action = StoreAction();
		if (action) {
			if (Allowed(action) && (!g_shiftForAll || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0))
				ForEachPersonalRow(wnd, action);
			return 0;
		}
	}
	else if (notification == kNotifyMenu) {
		const int id = static_cast<int>(reinterpret_cast<intptr_t>(data));
		uint8_t* mgr = *g_menuMgr;
		const int index = *reinterpret_cast<int*>(wnd + kWndPllMenuIndex);
		if (id >= kMenuSell && id <= kMenuAutoClear && id != 0x42 && id != 0x45 && mgr && index >= 0 && index < *reinterpret_cast<int*>(mgr + kMenuCount) &&
		    sender == reinterpret_cast<uint8_t**>(mgr + kMenuArray)[index]) {
			uint8_t* row = RowWithMenuOpen(wnd);
			if (!row) return 0;
			row[kRowMenuOpen] = 0;
			const uint64_t item = *reinterpret_cast<uint64_t*>(row + kRowItemId);
			const char* name = reinterpret_cast<const char*>(row + kRowName);
			const uint32_t icon = *reinterpret_cast<uint32_t*>(row + kRowIcon);
			switch (id) {
			case kMenuSell: if (Allowed(kActionSell)) RequestRow(row, kActionSell, false); break;
			case kMenuStore: if (Allowed(StoreAction())) RequestRow(row, StoreAction(), false); break;
			case kMenuAutoSell: LoadFilters(); SetFilter(item, name, icon, kActionSell); break;
			case kMenuAutoStore: LoadFilters(); SetFilter(item, name, icon, kActionStore); break;
			default: LoadFilters(); SetFilter(item, name, icon, 0); break;
			}
			return 0;
		}
	}
	return g_origNotify(wnd, sender, notification, data);
}

void AddItem(uint8_t* menu, const char* text, int id) {
	void* s = nullptr;
	HudCXStr(&s, text);
	if (!s) return;
	reinterpret_cast<AddMenuItemFn>(Virtual(menu, kVtblAddMenuItem))(menu, &s, id, nullptr, -1, 1);
	HudCXStrRelease(&s);
}

void Log(const char* text) {
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s\\dinput8.log", g_gameDir);
	FILE* f = nullptr;
	if (fopen_s(&f, path, "a") != 0 || !f) return;
	fprintf(f, "AdvLootActions: %s\n", text);
	fclose(f);
}

bool g_menuBroken = false;   // adding our menu entries faulted once: stop adding them

void AddEntries(uint8_t* menu, uint8_t* wnd) {
	if (g_caps & kCapSell) AddItem(menu, "Sell this item", kMenuSell);
	if (g_caps & (kCapDepot | kCapHoard)) AddItem(menu, "Store this item", kMenuStore);
	if (g_autoFilters) {
		LoadFilters();
		if (g_caps & kCapSell) AddItem(menu, "Auto-sell this item", kMenuAutoSell);
		if (g_caps & (kCapDepot | kCapHoard)) AddItem(menu, "Auto-store this item", kMenuAutoStore);
		uint8_t* row = RowWithMenuOpen(wnd);
		if (row && FindFilter(*reinterpret_cast<uint64_t*>(row + kRowItemId)) >= 0)
			AddItem(menu, "Clear auto action", kMenuAutoClear);
	}
}

// The entries are added under a structured exception guard: if the client's menu call ever faults (a different
// argument layout than the one read from FUN_1400ad9c0), the menu stays stock and the log says so.
void AddEntriesGuarded(uint8_t* menu, uint8_t* wnd) {
	DWORD code = 0;
	__try {
		AddEntries(menu, wnd);
	}
	__except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
		g_menuBroken = true;
	}
	if (g_menuBroken) {
		char text[96];
		snprintf(text, sizeof(text), "menu entries off: adding them faulted (exception 0x%08lx)", code);
		Log(text);
	}
}

int64_t PopupDetour(uint8_t* mgr, int menuIndex, void* point, uint8_t* wnd, uint8_t flag) {
	if (g_on && g_caps && !g_menuBroken && wnd && wnd == *g_pAdvLootWnd && mgr &&
	    menuIndex == *reinterpret_cast<int*>(wnd + kWndPllMenuIndex) && menuIndex >= 0 &&
	    menuIndex < *reinterpret_cast<int*>(mgr + kMenuCount)) {
		uint8_t* menu = reinterpret_cast<uint8_t**>(mgr + kMenuArray)[menuIndex];
		if (menu) AddEntriesGuarded(menu, wnd);
	}
	return g_origPopup(mgr, menuIndex, point, wnd, flag);
}

// Ends a request the server refused (or never answered) the way a result does: the client's own result handler with
// an unknown result code clears the row's pending flag and drops its pending node, so the row can be looted or left
// again. Without this a refused row stays "busy" and Leave All answers "You cannot perform that action right now".
void SendReleaseResult(uint32_t seq) {
	uint8_t packet[13] = {};
	const uint16_t sub = kSubResult, result = 0x7fff;
	const uint32_t one = 1;
	memcpy(packet, &sub, 2);
	memcpy(packet + 2, &result, 2);
	memcpy(packet + 4, &one, 4);
	memcpy(packet + 8, &seq, 4);
	PacketStream stream = { packet, sizeof(packet), 2 };
	g_origResult(g_lootMgr(), &stream);
}

void ReleaseRequest(Sent& s) {
	s.used = false;
	MarkDone(s.item, s.corpse);   // refused or unanswered: the auto filters leave this pair alone from now on
	SendReleaseResult(s.seq);
}

// The loot manager's pending request node for `seq` (buckets at +0x38, seq % 30; node +0 seq, +0x38 next).
bool RequestPending(uint8_t* mgr, uint32_t seq) {
	const uint8_t* node = reinterpret_cast<uint8_t**>(mgr + 0x38)[seq % 0x1e];
	for (; node; node = *reinterpret_cast<uint8_t* const*>(node + 0x38))
		if (*reinterpret_cast<const uint32_t*>(node) == seq) return true;
	return false;
}

const uint16_t kSubClearPending = 0x10;    // {u16 0x10, u32 corpse, u64 item, u32 0}: the server refused
const uint64_t kRequestTimeoutMs = 8000;

uint64_t DispatchDetour(uint8_t* mgr, const uint8_t* data, uint32_t length) {
	if (g_on && data && length >= 2) {
		const uint16_t sub = *reinterpret_cast<const uint16_t*>(data);
		if (sub == kSubSync) {
			if (g_caps) g_lfDirty = true;
			g_caps = 0;
			g_doneCount = 0;
		}
		else if (sub == kSubCapabilities && length >= 6) {
			const uint32_t caps = *reinterpret_cast<const uint32_t*>(data + 2);
			if (caps != g_caps) g_lfDirty = true;
			g_caps = caps;
			return 0;
		}
		else if (sub == kSubClearPending && length >= 14) {
			const uint64_t result = g_origDispatch(mgr, data, length);
			const uint32_t corpse = *reinterpret_cast<const uint32_t*>(data + 2);
			const uint64_t item = *reinterpret_cast<const uint64_t*>(data + 6);
			for (Sent& s : g_sent)
				if (s.used && s.corpse == corpse && s.item == item) ReleaseRequest(s);
			return result;
		}
	}
	return g_origDispatch(mgr, data, length);
}

uint64_t SendDetour(uint8_t* mgr, AdvLootRequest* req) {
	const uint64_t result = g_origSend(mgr, req);
	if (g_on && req) {
		Sent& s = g_sent[g_sentNext];
		g_sentNext = (g_sentNext + 1) % kMaxSent;
		s.seq = req->seq;
		s.corpse = req->corpse;
		s.item = req->item;
		s.when = GetTickCount64();
		s.action = req->action;
		s.used = true;
		const char* name = NameOfItem(req->item);
		strncpy_s(s.name, name ? name : "", _TRUNCATE);
	}
	return result;
}

uint64_t ResultDetour(uint8_t* mgr, PacketStream* stream) {
	if (g_on && stream && stream->data) {
		const uint32_t p = stream->position;
		if (stream->length >= p + 11) {
			const uint16_t result = *reinterpret_cast<const uint16_t*>(stream->data + p);
			const uint32_t seq = *reinterpret_cast<const uint32_t*>(stream->data + p + 6);
			const uint64_t copper = stream->length >= p + 19 ? *reinterpret_cast<const uint64_t*>(stream->data + p + 11) : 0;
			for (Sent& s : g_sent) {
				if (!s.used || s.seq != seq) continue;
				s.used = false;
				if (result != 1 && result != 2) {
					MarkDone(s.item, s.corpse);   // refused (the server's result 6): no auto retry
					break;
				}
				char text[128];
				if (copper) {
					const unsigned long long pp = copper / 1000, gp = copper / 100 % 10, sp = copper / 10 % 10, cp = copper % 10;
					snprintf(text, sizeof(text), "%s (%llup %llug %llus %lluc)", s.name, pp, gp, sp, cp);
					FctShowLootText(s.corpse, text, true);
				}
				else if (s.action <= 1 && result == 1) {
					FctShowLootText(s.corpse, s.name, false);
				}
				break;
			}
		}
	}
	uint32_t seq = 0;
	const bool known = g_on && stream && stream->data && stream->length >= stream->position + 10;
	if (known) seq = *reinterpret_cast<const uint32_t*>(stream->data + stream->position + 6);
	const uint64_t ret = g_origResult(mgr, stream);
	// A success for a corpse the client no longer lists (an earlier result emptied it) makes the handler return early,
	// leaving the request's node and the client's "waiting on the server" count behind. That count blocks the inventory
	// and other item actions until something resets it, so finish the request the way an unknown result code does.
	if (known && RequestPending(mgr, seq)) SendReleaseResult(seq);
	return ret;
}

// Auto filters: one pass over the Personal rows per frame.
void ApplyFilters(uint8_t* wnd) {
	LoadFilters();
	if (!g_filterCount) return;
	uint8_t* rows = PersonalRows(wnd);
	if (!rows) return;
	const int n = ListRows(rows);
	if (!n) {
		g_doneCount = 0;
		return;
	}
	for (int i = 0; i < n; ++i) {
		uint8_t* row = RowAt(rows, i);
		if (!row || row[kRowPending]) continue;
		if (row[kRowNoTrade] && !g_autoNoTrade) continue;
		const int f = FindFilter(*reinterpret_cast<uint64_t*>(row + kRowItemId));
		if (f < 0 || !Allowed(g_filters[f].action)) continue;
		RequestRow(row, g_filters[f].action, true);
	}
}

// ---- Edit Loot Filters window: AS / ST columns ----

// CLootFiltersWnd
const size_t kLfList = 0x2e0;            // CListWnd* LF_ItemOptionsList
const size_t kLfCheckTemplate = 0x2f8;   // CXWnd* LF_CheckBoxTemplate (hidden; cloned per cell)
const size_t kLfRemoveItem = 0x324;      // int item id whose row OnProcessFrame deletes, -1 none
// CListWnd
const size_t kListColCount = 0x280;      // int
const int kLfColumns = 10;               // the shipped XML: 8 stock columns plus AS and ST
const int kColAN = 3, kColAR = 6, kColRemove = 7, kColAS = 8, kColST = 9;
const uint32_t kBitAR = 1, kBitAN = 2, kBitAG = 4, kBitNV = 8;
// CXWnd
const size_t kWndXmlIndex = 0x108;       // u32 SIDL id of the template, passed to SidlFindDef
const size_t kWndUnk1e0 = 0x1e0;         // u64, zeroed on every clone like the client does
// LootFiltersCopyWnd confirm callback interface: entries "<server>_<character>" (CXStr at the start of each)
const size_t kCopyEntryCount = 0x10;
const size_t kCopyEntries = 0x18;
const size_t kCopyEntrySize = 0x20;
const int kCopyMerge = 100, kCopyReplace = 0x65;

// The client's filter record (store node + 4).
struct FilterRecord {
	uint32_t flags;
	uint32_t item;
	uint32_t icon;
	char name[64];
};

using LfNotifyFn = int64_t (*)(uint8_t* wnd, uint8_t* sender, uint32_t code, void* data);
using LfAddRowFn = void (*)(uint8_t* wnd, FilterRecord* rec, char searchMode);
using LfRebuildFn = void (*)(uint8_t* wnd, void** search);
using LfFrameFn = void (*)(uint8_t* wnd);
using CopyConfirmFn = void (*)(uint8_t* iface, int id, uint64_t result, int* entry);
using FilterFindFn = FilterRecord* (*)(uint8_t* store, uint32_t item);
using FilterSetFn = uint8_t (*)(uint8_t* store, uint32_t item, uint32_t icon, const char* name, uint32_t flags,
                                char keepBit0, char select);
using CellFromWndFn = void (*)(uint8_t* list, uint8_t* wnd, int* row, int* col);
using GetCellWndFn = uint8_t* (*)(uint8_t* list, int row, int col);
using SetCellWndFn = void (*)(uint8_t* list, int row, int col, uint8_t* wnd);
using ListLayoutFn = void (*)(uint8_t* list);
using SidlFindDefFn = void* (*)(uint8_t* mgr, uint32_t xmlIndex);
using SidlCreateFn = uint8_t* (*)(uint8_t* mgr, uint8_t* parent, void* def, uint8_t flag);
using SetByte70Fn = void (*)(uint8_t* wnd, uint8_t value, int depth);
using CXStrCStrFn = const char* (*)(void** str);

LfNotifyFn g_origLfNotify = nullptr;         // FUN_1401af130
LfAddRowFn g_origLfAddRow = nullptr;         // FUN_1401ad5f0 (trampoline)
LfAddRowFn g_lfAddRow = nullptr;             // FUN_1401ad5f0 (through our detour, so the cells follow)
LfRebuildFn g_origLfRebuild = nullptr;       // FUN_1401aea30
LfRebuildFn g_lfRebuild = nullptr;
LfFrameFn g_origLfFrame = nullptr;           // FUN_1401aee00
CopyConfirmFn g_origCopyConfirm = nullptr;   // FUN_140158490
FilterFindFn g_filterFind = nullptr;         // FUN_140159c20
FilterSetFn g_filterSet = nullptr;           // FUN_14015a770
CellFromWndFn g_listCellFromWnd = nullptr;   // FUN_1405b6d20
GetCellWndFn g_listGetCellWnd = nullptr;     // FUN_1405b69c0
SetCellWndFn g_listSetCellWnd = nullptr;     // FUN_1405b9440
ListLayoutFn g_listLayout = nullptr;         // FUN_1405b2590
SidlFindDefFn g_sidlFindDef = nullptr;       // FUN_1405da5c0
SidlCreateFn g_sidlCreate = nullptr;         // FUN_1405d92c0
SetByte70Fn g_wndSetByte70 = nullptr;        // FUN_1405a77b0
CXStrCStrFn g_cxstrCStr = nullptr;           // FUN_1405859f0
uint8_t** g_pFilterStore = nullptr;          // &DAT_140db4828
uint8_t** g_pSidlMgr = nullptr;              // &DAT_140f0f8d0
uint8_t* g_lfNotifyFn = nullptr;             // function starts, for the prepare checks
uint8_t* g_lfFrameFn = nullptr;
uint8_t* g_copyConfirmFn = nullptr;
bool g_lfOn = false;
int g_pendingRemove = -1;                    // item whose Remove was pressed; its deferred row removal clears our action

bool LfActive(uint8_t* wnd) {
	if (!g_lfOn || !g_caps || !g_autoFilters || !wnd) return false;
	uint8_t* list = *reinterpret_cast<uint8_t**>(wnd + kLfList);
	return list && *reinterpret_cast<int*>(list + kListColCount) >= kLfColumns &&
	       *reinterpret_cast<uint8_t**>(wnd + kLfCheckTemplate) != nullptr;
}

FilterRecord* StoreRec(uint32_t item) {
	uint8_t* store = *g_pFilterStore;
	return store ? g_filterFind(store, item) : nullptr;
}

int ListRowCount(uint8_t* list) {
	return *reinterpret_cast<int*>(list + kListRowCount);
}

int RowOfItem(uint8_t* list, uint32_t item) {
	for (int r = 0, n = ListRowCount(list); r < n; ++r)
		if (static_cast<uint32_t>(g_listItemData(list, r)) == item) return r;
	return -1;
}

bool ActionShownIn(int col, uint8_t action) {
	return col == kColAS ? action == kActionSell : (action == kActionStore || action == kActionDepot);
}

// A checkbox from LF_CheckBoxTemplate, as AddOrUpdateRow makes them for AN / AG / NV.
uint8_t* CloneCheckbox(uint8_t* wnd) {
	uint8_t* tmpl = *reinterpret_cast<uint8_t**>(wnd + kLfCheckTemplate);
	uint8_t* mgr = *g_pSidlMgr;
	if (!tmpl || !mgr) return nullptr;
	void* def = g_sidlFindDef(mgr, *reinterpret_cast<uint32_t*>(tmpl + kWndXmlIndex));
	uint8_t* box = def ? g_sidlCreate(mgr, wnd, def, 1) : nullptr;
	if (!box) return nullptr;
	if (!LooksLikeCheckbox(tmpl, box)) {
		g_lfOn = false;   // not the client's checkbox class: leave the columns alone from now on
		Log("filter columns off: the cloned template is not a checkbox");
		return nullptr;
	}
	g_wndSetByte70(box, 1, 0);
	Show(box, true);
	*reinterpret_cast<uint64_t*>(box + kWndUnk1e0) = 0;
	return box;
}

// Attaches AS / ST to row r if missing and sets them from the DLL table. A box whose action the server does not
// offer is hidden.
void EnsureCells(uint8_t* wnd, int r, bool* attached) {
	uint8_t* list = *reinterpret_cast<uint8_t**>(wnd + kLfList);
	const int f = FindFilter(static_cast<uint32_t>(g_listItemData(list, r)));
	const uint8_t action = f >= 0 ? g_filters[f].action : 0;
	for (int c = kColAS; c <= kColST; ++c) {
		uint8_t* box = g_listGetCellWnd(list, r, c);
		if (!box) {
			box = CloneCheckbox(wnd);
			if (!box) return;
			g_listSetCellWnd(list, r, c, box);
			*attached = true;
		}
		SetBox(box, ActionShownIn(c, action));
		Show(box, Allowed(c == kColAS ? kActionSell : kActionStore));
	}
}

// A row for an item that only has a DLL action: a record with no stock flags through the client's own AddOrUpdateRow
// (our H2 detour attaches the cells). Search mode 1 applies the window's search text and row limit.
void AddDllRow(uint8_t* wnd, const Filter& f, char searchMode) {
	FilterRecord rec = {};
	rec.item = static_cast<uint32_t>(f.item);
	rec.icon = f.icon;
	strncpy_s(rec.name, f.name, _TRUNCATE);
	g_lfAddRow(wnd, &rec, searchMode);
}

// Brings the list in line with the DLL table: cells on every row, rows for DLL-only items, and the deferred removal
// of a DLL-only row whose action was cleared elsewhere (one per frame, the client deletes it on this frame's path).
void Sync(uint8_t* wnd) {
	uint8_t* list = *reinterpret_cast<uint8_t**>(wnd + kLfList);
	bool attached = false;
	int stale = 0;
	for (int r = 0, n = ListRowCount(list); r < n; ++r) {
		const uint32_t item = static_cast<uint32_t>(g_listItemData(list, r));
		if (FindFilter(item) < 0 && !StoreRec(item)) {
			int* removal = reinterpret_cast<int*>(wnd + kLfRemoveItem);
			if (!stale++ && *removal == -1) *removal = static_cast<int>(item);
			continue;
		}
		EnsureCells(wnd, r, &attached);
	}
	for (int i = 0; i < g_filterCount; ++i) {
		const uint32_t item = static_cast<uint32_t>(g_filters[i].item);
		if (StoreRec(item) || RowOfItem(list, item) >= 0) continue;
		AddDllRow(wnd, g_filters[i], 1);
	}
	if (attached) g_listLayout(list);
	g_lfDirty = stale > 1;
}

// H1: clicks (notification 1) on our cells, and on the stock cells of rows the client has no record for.
int64_t LfNotifyDetour(uint8_t* wnd, uint8_t* sender, uint32_t code, void* data) {
	if (code != kNotifyClick || !sender || !LfActive(wnd)) return g_origLfNotify(wnd, sender, code, data);
	uint8_t* list = *reinterpret_cast<uint8_t**>(wnd + kLfList);
	if (sender == list) return g_origLfNotify(wnd, sender, code, data);
	int r = -1, c = -1;
	g_listCellFromWnd(list, sender, &r, &c);
	if (r < 0) {
		g_pendingRemove = -1;
		return g_origLfNotify(wnd, sender, code, data);
	}
	const uint32_t item = static_cast<uint32_t>(g_listItemData(list, r));
	LoadFilters();
	FilterRecord* rec = StoreRec(item);
	const int f = FindFilter(item);
	if (c == kColAS || c == kColST) {
		const bool on = IsChecked(sender);
		const uint8_t action = on ? (c == kColAS ? kActionSell : kActionStore) : 0;
		if (action && !Allowed(action)) {
			SetBox(sender, false);
			return 0;
		}
		if (on) SetBox(g_listGetCellWnd(list, r, c == kColAS ? kColST : kColAS), false);
		const char* name = rec ? rec->name : (f >= 0 ? g_filters[f].name : "");
		const uint32_t icon = rec ? rec->icon : (f >= 0 ? g_filters[f].icon : 0);
		SetFilter(item, name, icon, action);
		if (!action && !rec) {   // a DLL-only row with nothing left: the client removes it on the next frame
			g_pendingRemove = static_cast<int>(item);
			*reinterpret_cast<int*>(wnd + kLfRemoveItem) = static_cast<int>(item);
		}
		return 0;
	}
	if (c == kColRemove) {
		g_pendingRemove = static_cast<int>(item);   // consumed by H4 when the row goes
		if (rec) return g_origLfNotify(wnd, sender, code, data);
		SetFilter(item, "", 0, 0);                  // DLL-only row: the stock Remove has nothing to remove
		*reinterpret_cast<int*>(wnd + kLfRemoveItem) = static_cast<int>(item);
		return 0;
	}
	if (c >= kColAN && c <= kColAR && !rec && f >= 0) {
		// AN / AG / NV / auto-roll on a DLL-only row: the client's rule (AN, AG and NV exclusive, auto-roll combines),
		// then its record is created, which refreshes the row and makes it a stock row.
		if (IsChecked(sender)) {
			for (int o = kColAN; o <= kColAR; ++o) {
				uint8_t* box = o != c ? g_listGetCellWnd(list, r, o) : nullptr;
				if (box && IsChecked(box)) SetBox(box, c == kColAR || o == kColAR);
			}
		}
		static const uint32_t kBits[] = { kBitAN, kBitAG, kBitNV, kBitAR };
		uint32_t flags = 0;
		for (int o = kColAN; o <= kColAR; ++o) {
			uint8_t* box = g_listGetCellWnd(list, r, o);
			if (box && IsChecked(box)) flags |= kBits[o - kColAN];
		}
		if (flags && *g_pFilterStore) g_filterSet(*g_pFilterStore, item, g_filters[f].icon, g_filters[f].name, flags, 0, 0);
		return 0;
	}
	g_pendingRemove = -1;
	return g_origLfNotify(wnd, sender, code, data);
}

// H2: after the client added or updated a row, give it our cells. Covers file load, Merge / Replace, Advanced Loot
// changes, cursor drops and our own rows.
void LfAddRowDetour(uint8_t* wnd, FilterRecord* rec, char searchMode) {
	g_origLfAddRow(wnd, rec, searchMode);
	if (!rec || !LfActive(wnd)) return;
	LoadFilters();
	uint8_t* list = *reinterpret_cast<uint8_t**>(wnd + kLfList);
	const int r = RowOfItem(list, rec->item);
	if (r < 0) return;
	bool attached = false;
	EnsureCells(wnd, r, &attached);
	if (attached) g_listLayout(list);
}

// H3: after a rebuild (file load, Search, Enter in the search box, limit edits, cursor drop), add the DLL-only rows.
// AddOrUpdateRow in search mode tests the search box text and the row limit itself (the text the rebuild was called
// with: a cursor drop writes the dropped item's name into the box first).
void LfRebuildDetour(uint8_t* wnd, void** search) {
	g_origLfRebuild(wnd, search);
	if (!LfActive(wnd)) return;
	LoadFilters();
	uint8_t* list = *reinterpret_cast<uint8_t**>(wnd + kLfList);
	for (int i = 0; i < g_filterCount; ++i) {
		const uint32_t item = static_cast<uint32_t>(g_filters[i].item);
		if (StoreRec(item) || RowOfItem(list, item) >= 0) continue;
		AddDllRow(wnd, g_filters[i], 1);
	}
}

// H4: before the client's per-frame work, which deletes the row of the item in wnd+0x324.
void LfFrameDetour(uint8_t* wnd) {
	if (LfActive(wnd)) {
		int* removal = reinterpret_cast<int*>(wnd + kLfRemoveItem);
		const int item = *removal;
		if (item != -1) {
			LoadFilters();
			if (item == g_pendingRemove) {   // Remove pressed (and confirmed, if the client asked)
				if (FindFilter(static_cast<uint32_t>(item)) >= 0) SetFilter(static_cast<uint32_t>(item), "", 0, 0);
				g_pendingRemove = -1;
			}
			else if (FindFilter(static_cast<uint32_t>(item)) >= 0 && !StoreRec(static_cast<uint32_t>(item))) {
				*removal = -1;   // the stock flags fell to 0 but our action remains: keep the row
			}
		}
		if (g_lfDirty && wnd[kWndVisible]) Sync(wnd);
	}
	g_origLfFrame(wnd);
}

// H5: Merge / Replace Filters confirmed for another character: do the same for the DLL file. The entry reads
// "<server>_<character>"; the part that names an existing DLL file is taken as the character.
void MergeDllFile(const char* entry, bool replace) {
	static Filter other[kMaxFilters];
	const char* first = strchr(entry, '_');
	const char* last = strrchr(entry, '_');
	const char* parts[2] = { last ? last + 1 : entry, entry };
	char file[MAX_PATH];
	int n = -1;
	for (int i = 0; i < 2 && n < 0; ++i) {
		const size_t len = i == 0 ? strlen(parts[0]) : first ? static_cast<size_t>(first - entry) : strlen(entry);
		if (!len || len >= 64) continue;
		snprintf(file, sizeof(file), "%s\\userdata\\eqcore_advloot_%.*s.ini", g_gameDir, static_cast<int>(len), parts[i]);
		if (GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES) n = ReadFilterFile(file, other, kMaxFilters);
	}
	if (n < 0 || _stricmp(file, g_filterFile) == 0) return;
	if (replace)
		while (g_filterCount) SetFilter(g_filters[g_filterCount - 1].item, "", 0, 0);
	for (int i = 0; i < n; ++i)
		if (FindFilter(other[i].item) < 0) SetFilter(other[i].item, other[i].name, other[i].icon, other[i].action);
}

void CopyConfirmDetour(uint8_t* iface, int id, uint64_t result, int* entry) {
	if (g_lfOn && g_autoFilters && entry && (result & 5) && (id == kCopyMerge || id == kCopyReplace) && iface) {
		const int i = *entry;
		uint8_t* entries = *reinterpret_cast<uint8_t**>(iface + kCopyEntries);
		if (i >= 0 && i < *reinterpret_cast<int*>(iface + kCopyEntryCount) && entries) {
			LoadFilters();
			const char* name = g_cxstrCStr(reinterpret_cast<void**>(entries + static_cast<size_t>(i) * kCopyEntrySize));
			if (name && g_filterFile[0]) MergeDllFile(name, id == kCopyReplace);
		}
	}
	g_origCopyConfirm(iface, id, result, entry);
}

// True if the function body starting at `fn` has a rel32 call (E8) to `target` within `size` bytes.
template <class T>
bool CallsWithin(const uint8_t* fn, size_t size, T target) {
	for (size_t i = 0; i + 5 <= size; ++i) {
		if (fn[i] != 0xE8) continue;
		int32_t rel;
		memcpy(&rel, fn + i + 1, 4);
		if (fn + i + 5 + rel == reinterpret_cast<const uint8_t*>(target)) return true;
	}
	return false;
}

// ---- AS / ST columns in the Advanced Loot window's Personal list ----
// The shipped EQUI_AdvancedLootWnd.xml puts AS and ST in columns 7 and 8; the client fills 0-6 only (FUN_1400a2460).
// Cells are clones of ADLW_CheckBoxTemplate made the way the client's FUN_1400a5250 makes the AN / AG boxes.

const size_t kWndCheckTemplate = 0x388;  // CXWnd* ADLW_CheckBoxTemplate (FUN_1400a5250 clones it)
const int kPllColAS = 7, kPllColST = 8, kPllColumns = 9;
bool g_pllShown = false;                 // cells were made while the columns were active

uint8_t* PllList(uint8_t* wnd) {
	uint8_t* rows = PersonalRows(wnd);
	uint8_t* list = rows ? *reinterpret_cast<uint8_t**>(rows + kRowsList) : nullptr;
	return list && *reinterpret_cast<int*>(list + kListColCount) >= kPllColumns ? list : nullptr;
}

bool PllActive() {
	return g_lfOn && g_caps && g_autoFilters;
}

uint8_t* PllCloneCheckbox(uint8_t* wnd) {
	uint8_t* tmpl = *reinterpret_cast<uint8_t**>(wnd + kWndCheckTemplate);
	uint8_t* mgr = *g_pSidlMgr;
	if (!tmpl || !mgr) return nullptr;
	void* def = g_sidlFindDef(mgr, *reinterpret_cast<uint32_t*>(tmpl + kWndXmlIndex));
	uint8_t* box = def ? g_sidlCreate(mgr, wnd, def, 1) : nullptr;
	if (!box) return nullptr;
	if (!LooksLikeCheckbox(*reinterpret_cast<uint8_t**>(wnd + kWndApplyFiltersCheck), box)) {
		g_lfOn = false;
		Log("loot list columns off: the cloned template is not a checkbox");
		return nullptr;
	}
	g_wndSetByte70(box, 1, 0);
	SetBox(box, false);
	Show(box, true);
	*reinterpret_cast<uint64_t*>(box + kWndUnk1e0) = 0;
	return box;
}

// Each frame: every Personal row gets its AS / ST boxes (only for actions the server offers) and they follow the table.
void PllSync(uint8_t* wnd) {
	uint8_t* rows = PersonalRows(wnd);
	uint8_t* list = PllList(wnd);
	if (!list) return;
	const bool active = PllActive();
	if (!active && !g_pllShown) return;
	const bool reshow = active && !g_pllShown;   // boxes hidden while the columns were off come back
	bool attached = false;
	for (int r = 0, n = ListRows(rows); r < n; ++r) {
		uint8_t* row = RowAt(rows, r);
		if (!row) continue;
		const int f = active ? FindFilter(*reinterpret_cast<uint64_t*>(row + kRowItemId)) : -1;
		const uint8_t action = f >= 0 ? g_filters[f].action : 0;
		for (int c = kPllColAS; c <= kPllColST; ++c) {
			uint8_t* box = g_listGetCellWnd(list, r, c);
			if (!active) {
				Show(box, false);
				continue;
			}
			if (!Allowed(c == kPllColAS ? kActionSell : kActionStore)) continue;
			if (!box) {
				box = PllCloneCheckbox(wnd);
				if (!box) return;
				g_listSetCellWnd(list, r, c, box);
				attached = true;
			}
			else if (reshow) Show(box, true);
			const bool want = c == kPllColAS ? action == kActionSell : (action == kActionStore || action == kActionDepot);
			if (IsChecked(box) != want) SetBox(box, want);
		}
	}
	if (attached) g_listLayout(list);
	g_pllShown = active;
}

// A click on one of our cells: set or clear the row's auto action (AS and ST exclude each other).
bool PllCellClick(uint8_t* wnd, uint8_t* sender) {
	if (!PllActive()) return false;
	uint8_t* list = PllList(wnd);
	if (!list || sender == list) return false;
	int r = -1, c = -1;
	g_listCellFromWnd(list, sender, &r, &c);
	if (r < 0 || (c != kPllColAS && c != kPllColST)) return false;
	uint8_t* row = RowAt(PersonalRows(wnd), r);
	if (!row) return true;
	const bool on = IsChecked(sender);
	const uint8_t action = on ? (c == kPllColAS ? kActionSell : StoreAction()) : 0;
	if (action && !Allowed(action)) {
		SetBox(sender, false);
		return true;
	}
	if (on) SetBox(g_listGetCellWnd(list, r, c == kPllColAS ? kPllColST : kPllColAS), false);
	LoadFilters();
	SetFilter(*reinterpret_cast<uint64_t*>(row + kRowItemId), reinterpret_cast<const char*>(row + kRowName),
	          *reinterpret_cast<uint32_t*>(row + kRowIcon), action);
	return true;
}

#define REF(var) reinterpret_cast<void**>(&var)

const char* kLfNotifySig = "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 30 4D 8B F1";
const char* kLfAddRowSig = "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 70 45 0F B6 E0";
const char* kLfRebuildSig = "48 89 5C 24 18 48 89 54 24 10 55 56 57 41 56 41 57 48 83 EC 20";
const char* kLfFrameSig = "40 57 48 83 EC 20 48 83 B9 70 02 00 00 00 48 8B F9 0F 84 ?? ?? ?? ??";
const char* kCopyConfirmSig = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 4D 8B F1";

const char* kNotifySig =
	"40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 F8 48 81 EC 08 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F0 4D 8B F1";
const char* kSendSig = "48 89 5C 24 20 48 89 54 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC";

} // namespace

void AdvLootFrame(bool inGame) {
	if (!g_on) return;
	if (!inGame) {
		g_ctl = {};
		return;
	}
	// Requests nobody answered (a lost refusal): release them after a while, like Legends' pending expiry.
	const uint64_t now = GetTickCount64();
	for (Sent& s : g_sent)
		if (s.used && now - s.when > kRequestTimeoutMs) ReleaseRequest(s);
	uint8_t* wnd = *g_pAdvLootWnd;
	if (!wnd || !wnd[kWndInitDone]) return;
	UpdateVisibility(wnd);
	if (g_lfOn) PllSync(wnd);
	if (g_caps && g_autoFilters) ApplyFilters(wnd);
}

const HookSite kAdvLootHooks[6] = {
	// Shared with the Hud, FCT and Nameplates (same signature, detour and trampoline).
	{ kDrawWindowsSig, 5, reinterpret_cast<void*>(&DrawWindowsDetour), reinterpret_cast<void**>(&g_origDrawWindows) },
	// A: CAdvancedLootWnd::WndNotification FUN_1400adc10. Stolen (5): push rbp; push rbx; push rsi; push rdi.
	{ kNotifySig, 5, reinterpret_cast<void*>(&NotifyDetour), REF(g_origNotify) },
	// B: CContextMenuManager::PopupMenu FUN_1405bca80. Stolen (5): mov [rsp+10],rbx.
	{ "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 49 8B F9",
	  5, reinterpret_cast<void*>(&PopupDetour), REF(g_origPopup) },
	// C: AdvLoot subtype dispatcher FUN_14015dec0(mgr, data, length). Stolen (5): mov [rsp+10],rbx.
	{ "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 48 8D AC 24 70 FA FF FF",
	  5, reinterpret_cast<void*>(&DispatchDetour), REF(g_origDispatch) },
	// D: sub 9 result handler FUN_14015da60(mgr, stream). Stolen (7): mov rax,rsp; mov [rax+8],rbx.
	{ "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 20 57 41 54",
	  7, reinterpret_cast<void*>(&ResultDetour), REF(g_origResult) },
	// E: request sender FUN_140161230(mgr, req). Stolen (5): mov [rsp+20],rbx.
	{ kSendSig, 5, reinterpret_cast<void*>(&SendDetour), REF(g_origSend) },
};

const RefSite kAdvLootRefs[7] = {
	{ kNotifySig, 0, 0, REF(g_notifyFn) },
	{ kSendSig, 0, 0, REF(g_send) },
	// FUN_14015d480: the loot manager singleton
	{ "40 53 48 83 EC 20 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 ?? ?? ?? ?? BA 04 00 00 00 48 8B 0C C8 8B 04 0A 39 05 ?? ?? ?? ?? 0F 8F ?? ?? ?? ??",
	  0, 0, REF(g_lootMgr) },
	// CXWnd::GetChildItem FUN_14059f8a0(parent, CXStr*, char log)
	{ "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC 20 02 00 00", 0, 0, REF(g_getChild) },
	// CListWnd::GetItemData FUN_1405b62e0(list, row)
	{ "48 83 EC 28 85 D2 78 ?? 48 81 C1 68 02 00 00 3B 11 7D ?? 4C 8B 41 08 4D 85 C0 74 ?? 48 63 CA 48 69 C1 38 01 00 00 4A 8B 44 00 18",
	  0, 0, REF(g_listItemData) },
	// FUN_14015fab0: mov rcx,[pAdvLootWnd]; test rcx,rcx; je; mov rax,[rcx]; call [rax+3b0]
	{ "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 B0 03 00 00 44 89 AB E8 01 00 00", 3, 7, REF(g_pAdvLootWnd) },
	// FUN_1401900e0: mov rcx,[CContextMenuManager]; ...
	{ "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 B2 01 FF 90 38 01 00 00 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 80 B9 11 02 00 00 00",
	  3, 7, REF(g_menuMgr) },
};

const char* AdvLootPrepare() {
	if (const char* e = HudTextPrepare()) return e;
	// The loot window's WndNotification calls the loot manager getter (its "Link All" branch): find that rel32 call.
	bool found = false;
	for (size_t i = 0; i + 5 <= 0x2000 && !found; ++i) {
		if (g_notifyFn[i] != 0xE8) continue;
		int32_t rel;
		memcpy(&rel, g_notifyFn + i + 1, 4);
		found = g_notifyFn + i + 5 + rel == reinterpret_cast<const uint8_t*>(g_lootMgr);
	}
	if (!found) return "the loot window does not call the loot manager getter we resolved";
	LoadConfig();
	g_on = true;
	return nullptr;
}

const HookSite kAdvLootFilterHooks[5] = {
	// H1: CLootFiltersWnd::WndNotification FUN_1401af130. Stolen (5): mov [rsp+8],rbx.
	{ kLfNotifySig, 5, reinterpret_cast<void*>(&LfNotifyDetour), REF(g_origLfNotify) },
	// H2: AddOrUpdateRow FUN_1401ad5f0(wnd, record, searchMode). Stolen (5): mov [rsp+10],rbx.
	{ kLfAddRowSig, 5, reinterpret_cast<void*>(&LfAddRowDetour), REF(g_origLfAddRow) },
	// H3: Rebuild FUN_1401aea30(wnd, CXStr* search). Stolen (5): mov [rsp+18],rbx.
	{ kLfRebuildSig, 5, reinterpret_cast<void*>(&LfRebuildDetour), REF(g_origLfRebuild) },
	// H4: CLootFiltersWnd::OnProcessFrame FUN_1401aee00. Stolen (6): push rdi; sub rsp,20.
	{ kLfFrameSig, 6, reinterpret_cast<void*>(&LfFrameDetour), REF(g_origLfFrame) },
	// H5: LootFiltersCopyWnd confirm callback FUN_140158490(iface, id, result, int* entry). Stolen (5): mov [rsp+8],rbx.
	{ kCopyConfirmSig, 5, reinterpret_cast<void*>(&CopyConfirmDetour), REF(g_origCopyConfirm) },
};

const RefSite kAdvLootFilterRefs[17] = {
	{ kLfNotifySig, 0, 0, REF(g_lfNotifyFn) },
	{ kLfAddRowSig, 0, 0, REF(g_lfAddRow) },
	{ kLfRebuildSig, 0, 0, REF(g_lfRebuild) },
	{ kLfFrameSig, 0, 0, REF(g_lfFrameFn) },
	{ kCopyConfirmSig, 0, 0, REF(g_copyConfirmFn) },
	// FUN_140159c20(store, item) -> FilterRecord*
	{ "8B C2 45 33 C0 83 E0 1F 48 8B 44 C1 20 48 85 C0 74 0D 39 10 74 0C 48 8B 40 68 48 85 C0 75 F3", 0, 0, REF(g_filterFind) },
	// FUN_14015a770(store, item, icon, name, flags, keepBit0, select)
	{ "4C 8B DC 55 57 41 57 49 8D AB C8 FD FF FF", 0, 0, REF(g_filterSet) },
	// CListWnd::CellFromWnd FUN_1405b6d20(list, wnd, int* row, int* col)
	{ "48 89 5C 24 08 4C 89 4C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 20 49 8B C1",
	  0, 0, REF(g_listCellFromWnd) },
	// CListWnd::GetCellWnd FUN_1405b69c0(list, row, col)
	{ "48 83 EC 28 85 D2 78 ?? 48 81 C1 68 02 00 00 3B 11 7D ?? 4C 8B 49 08 4D 85 C9 74 ?? 48 63 C2 48 69 C8 38 01 00 00 49 03 C9 45 85 C0 78 ?? 44 3B 01 7D ?? 48 8B 51 08 48 85 D2 74 ?? 49 63 C0",
	  0, 0, REF(g_listGetCellWnd) },
	// CListWnd::SetCellWnd FUN_1405b9440(list, row, col, wnd)
	{ "85 D2 0F 88 ?? ?? ?? ?? 48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54", 0, 0, REF(g_listSetCellWnd) },
	// CListWnd cell layout FUN_1405b2590(list); short, so the prepare check that AddOrUpdateRow calls it is required
	{ "40 55 41 54 41 56 48 83 EC 60", 0, 0, REF(g_listLayout) },
	// CSidlManager::FindDef FUN_1405da5c0(mgr, xmlIndex)
	{ "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 8B EA 48 8B F1 44 0F B7 F5", 0, 0, REF(g_sidlFindDef) },
	// CSidlManager::Create FUN_1405d92c0(mgr, parent, def, flag)
	{ "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 57 48 83 EC 50 41 0F B6 F9", 0, 0, REF(g_sidlCreate) },
	// FUN_1405a77b0(wnd, u8, depth): sets wnd+0x70 as AddOrUpdateRow does on every cell it clones
	{ "48 89 74 24 10 57 48 83 EC 20 88 51 70", 0, 0, REF(g_wndSetByte70) },
	// CXStr -> const char* FUN_1405859f0 (the copy callback reads the entry names with it)
	{ "40 53 48 83 EC 20 48 8B 01 48 8B D9 48 85 C0 74 ?? 83 78 0C 00", 0, 0, REF(g_cxstrCStr) },
	// FUN_140281e50: mov rcx,[pFilterStore]; test rcx,rcx; je; mov dl,1; call; mov rcx,[..]; test; je; xor edx,edx
	{ "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? B2 01 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 33 D2", 3, 7, REF(g_pFilterStore) },
	// FUN_140196d10: mov rcx,[pSidlMgr]; call; mov rdx,rax; lea rcx,[rsp+130]
	{ "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 48 8D 8C 24 30 01 00 00", 3, 7, REF(g_pSidlMgr) },
};

const char* AdvLootFiltersPrepare() {
	if (!g_on) return "AdvLootActions itself is off";
	const uint8_t* addRow = reinterpret_cast<const uint8_t*>(g_lfAddRow);
	if (!CallsWithin(addRow, 0x760, g_sidlFindDef) || !CallsWithin(addRow, 0x760, g_sidlCreate) ||
	    !CallsWithin(addRow, 0x760, g_wndSetByte70) || !CallsWithin(addRow, 0x760, g_listSetCellWnd))
		return "AddOrUpdateRow does not clone cells with the functions we resolved";
	if (!CallsWithin(addRow, 0x760, g_listLayout) || !CallsWithin(addRow, 0x760, g_listGetCellWnd))
		return "AddOrUpdateRow does not use the list functions we resolved";
	if (!CallsWithin(reinterpret_cast<const uint8_t*>(g_lfRebuild), 0x270, g_lfAddRow))
		return "the rebuild does not call AddOrUpdateRow";
	if (!CallsWithin(g_lfNotifyFn, 0x3d0, g_lfRebuild) || !CallsWithin(g_lfNotifyFn, 0x3d0, g_listCellFromWnd))
		return "the filters window's WndNotification does not call the functions we resolved";
	// The click handler the window calls at +0xdf (FUN_1401adf60) writes records with the set / find we resolved.
	int32_t rel = 0;
	if (g_lfNotifyFn[0xdf] != 0xE8) return "no call where the filters window hands clicks on";
	memcpy(&rel, g_lfNotifyFn + 0xe0, 4);
	const uint8_t* clicks = g_lfNotifyFn + 0xe4 + rel;
	if (!CallsWithin(clicks, 0x650, g_filterSet) || !CallsWithin(clicks, 0x650, g_filterFind))
		return "the click handler does not use the filter store functions we resolved";
	if (!CallsWithin(g_copyConfirmFn, 0x140, g_cxstrCStr))
		return "the copy confirm callback does not read its entries with the CXStr function we resolved";
	g_lfOn = true;
	return nullptr;
}
