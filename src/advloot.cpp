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
// AutoNoTrade=1.
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
};
const int kMaxFilters = 512;
Filter g_filters[kMaxFilters];
int g_filterCount = 0;
char g_filterFile[MAX_PATH] = {};
char g_filterChar[0x40] = {};

const char* CharacterName() {
	uint8_t* me = *HudLocalPlayerRef();
	return me ? reinterpret_cast<const char*>(me + kSpawnName) : nullptr;
}

// Loads the character's filter file the first time that character is seen.
void LoadFilters() {
	const char* name = CharacterName();
	if (!name || !name[0] || strncmp(name, g_filterChar, sizeof(g_filterChar)) == 0) return;
	strncpy_s(g_filterChar, name, _TRUNCATE);
	snprintf(g_filterFile, sizeof(g_filterFile), "%s\\userdata\\eqcore_advloot_%s.ini", g_gameDir, g_filterChar);
	g_filterCount = 0;
	static char keys[32768];
	DWORD n = GetPrivateProfileSectionA("Filters", keys, sizeof(keys), g_filterFile);
	for (const char* p = keys; n && *p && g_filterCount < kMaxFilters; p += strlen(p) + 1) {
		const char* eq = strchr(p, '=');
		if (!eq || strstr(p, "_name=")) continue;
		char* end = nullptr;
		uint64_t item = _strtoui64(p, &end, 10);
		int action = atoi(eq + 1);
		if (end != eq || !item || (action != kActionSell && action != kActionStore && action != kActionDepot))
			continue;
		g_filters[g_filterCount++] = { item, static_cast<uint8_t>(action) };
	}
}

int FindFilter(uint64_t item) {
	for (int i = 0; i < g_filterCount; ++i)
		if (g_filters[i].item == item) return i;
	return -1;
}

void SetFilter(uint64_t item, const char* name, uint8_t action) {
	if (!g_filterFile[0]) return;
	char key[32], value[16];
	snprintf(key, sizeof(key), "%llu", static_cast<unsigned long long>(item));
	const int i = FindFilter(item);
	if (!action) {
		if (i >= 0) g_filters[i] = g_filters[--g_filterCount];
		WritePrivateProfileStringA("Filters", key, nullptr, g_filterFile);
		strcat_s(key, "_name");
		WritePrivateProfileStringA("Filters", key, nullptr, g_filterFile);
		return;
	}
	if (i >= 0) g_filters[i].action = action;
	else if (g_filterCount < kMaxFilters) g_filters[g_filterCount++] = { item, action };
	char dir[MAX_PATH];
	snprintf(dir, sizeof(dir), "%s\\userdata", g_gameDir);
	CreateDirectoryA(dir, nullptr);
	snprintf(value, sizeof(value), "%u", action);
	WritePrivateProfileStringA("Filters", key, value, g_filterFile);
	strcat_s(key, "_name");
	WritePrivateProfileStringA("Filters", key, name, g_filterFile);
}

// (item, corpse) pairs the auto filters already asked for; cleared on sync and when the window empties.
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

// One request per (item, corpse) of the row, as the client's Loot button does. `autoOnly` skips pairs the
// filters already sent. Returns the number of requests.
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
		if (autoOnly) {
			if (AlreadyDone(item, corpse)) continue;
			MarkDone(item, corpse);
		}
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

bool LooksLikeCheckbox(uint8_t* wnd, uint8_t* box) {
	void** vt = *reinterpret_cast<void***>(box);
	if (!vt) return false;
	uint8_t* ref = *reinterpret_cast<uint8_t**>(wnd + kWndApplyFiltersCheck);
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
	if (c.shiftBox && !LooksLikeCheckbox(wnd, c.shiftBox)) c.shiftBox = nullptr;
	if (c.autoBox && !LooksLikeCheckbox(wnd, c.autoBox)) c.autoBox = nullptr;
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

int64_t NotifyDetour(uint8_t* wnd, uint8_t* sender, uint64_t notification, void* data) {
	if (!g_on || !wnd || !wnd[kWndInitDone] || !g_caps || !sender) return g_origNotify(wnd, sender, notification, data);
	UpdateVisibility(wnd);
	if (notification == kNotifyClick) {
		if (sender == g_ctl.shiftBox || sender == g_ctl.autoBox) {
			const bool on = IsChecked(sender);
			if (sender == g_ctl.shiftBox) {
				g_shiftForAll = on;
				SaveFlag("ShiftForAll", on);
			}
			else {
				g_autoFilters = on;
				SaveFlag("AutoFilters", on);
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
			switch (id) {
			case kMenuSell: if (Allowed(kActionSell)) RequestRow(row, kActionSell, false); break;
			case kMenuStore: if (Allowed(StoreAction())) RequestRow(row, StoreAction(), false); break;
			case kMenuAutoSell: LoadFilters(); SetFilter(item, name, kActionSell); break;
			case kMenuAutoStore: LoadFilters(); SetFilter(item, name, kActionStore); break;
			default: LoadFilters(); SetFilter(item, name, 0); break;
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
void ReleaseRequest(Sent& s) {
	s.used = false;
	uint8_t packet[13] = {};
	const uint16_t sub = kSubResult, result = 0x7fff;
	const uint32_t one = 1;
	memcpy(packet, &sub, 2);
	memcpy(packet + 2, &result, 2);
	memcpy(packet + 4, &one, 4);
	memcpy(packet + 8, &s.seq, 4);
	PacketStream stream = { packet, sizeof(packet), 2 };
	g_origResult(g_lootMgr(), &stream);
}

const uint16_t kSubClearPending = 0x10;    // {u16 0x10, u32 corpse, u64 item, u32 0}: the server refused
const uint64_t kRequestTimeoutMs = 8000;

uint64_t DispatchDetour(uint8_t* mgr, const uint8_t* data, uint32_t length) {
	if (g_on && data && length >= 2) {
		const uint16_t sub = *reinterpret_cast<const uint16_t*>(data);
		if (sub == kSubSync) {
			g_caps = 0;
			g_doneCount = 0;
		}
		else if (sub == kSubCapabilities && length >= 6) {
			g_caps = *reinterpret_cast<const uint32_t*>(data + 2);
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
				if (result != 1 && result != 2) break;
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
	return g_origResult(mgr, stream);
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

#define REF(var) reinterpret_cast<void**>(&var)

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
