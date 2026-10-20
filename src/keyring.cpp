// EquipmentKeyRing: Live's Equipment Key Ring on the ToB client, a seventh "Equipment" tab in the Key Rings window for
// the gear the server keeps per persona (a persona switch or an equipment set load puts what it takes off there while
// "Store Equipment Set items" is ticked, and takes it back out when that class's set loads).
//
// Storage: ToB already has a container for slot type 39 (pLocalPC +0x1460, made by the PcClient constructor
// FUN_140658f50 as FUN_1406886d0(+0x1460, capacity 0, type 0x27, dynamic 1)), and the profile's container lookup
// (vtbl +0x128, FUN_140662910) returns it for type 39, so item lookups, OP_MoveItem and OP_DeleteItem by slot already
// work on it. Two things keep it empty on a stock client: the item placement switch FUN_1401ff230 has no case 0x27,
// and its capacity is 0 (stores resize the array to the capacity, FUN_14014c920; lookups need index < capacity,
// FUN_140694f50). The placement detour stores type-39 items the way the client stores the other rings
// (FUN_140696300 on the profile base, the client's own refcounting) after setting the capacity the server announced.
//
// Wire (server commit "TOB: Equipment Key Ring (server side)"): C2S 0xbb58 {u8 store_equipment_sets} once 2 s after
// every zone-in (the server's "this client has the tab": only then does it send ring items) and whenever the box
// changes; S2C 0xbb58 {u32 capacity; u8 store_equipment_sets} in reply, optionally followed by
// {u16 count; count x {u16 index; u8 class; u8 length; set name}} for the Persona column (whole table each time). The first reply per zone is followed by the
// ring's items as OP_ItemPacket {39, index}. Capacity 0 = off: the tab stays empty and the box hidden. OP_PersonaSwap
// (S2C 0x6db0, self flag at +4) carries no ring items and the server resends them, so the ring is cleared first; it
// is not cleared at a hello (a hello the server already had in this zone gets only the state back). The hello goes out
// again after every zone-in and reconnect (OP_NewZone / OP_PlayerProfile, a new connection, or a gap in
// in-game frames while a zone loads). Sent with the client's connection send FUN_140547ef0(conn, 4, data, length).
//
// Window (hooks on KeyRingWnd::WndNotification FUN_140157100; drawing from the shared DrawWindows hook): the patched
// EQUI_KeyRingWnd.xml adds KRW_EquipmentPage (tab index 6, which the client only uses for its absent Keys page) with
// KRW_Equipment_List, KRW_Equipment_Count and KRW_Equipment_StoreSetsCheckbox. The list is rebuilt the way the
// client's ring refresh FUN_1401568e0 builds the others (row data = ring index; columns persona, icon, gap, name; rows by persona then name) whenever
// the container changes while the window is shown. Left button held on a row (0x17) picks the item up: OP_MoveItem
// {39, index} -> cursor through the client's own move FUN_14040d090 (as FUN_14040e350 does for the other rings).
// A click (1) or drop (0x23) with an item on the cursor puts it in: cursor -> {39, -1}, the server picks the slot and
// sends the item back; only equipment (Live's rule FUN_1406c2db0 on the item definition: equip slots +0xbc != 0,
// +0x590 < 2, click type +0x225 != 3, item type +400 != potion; ToB's FUN_1406990b0 reads the same offsets).
// Shift/Ctrl+click or right click (3) shows the item. The box is saved in the character's ini ([Inventory]
// StoreEquipmentSetItems).
#include "keyring.h"
#include "hud.h"
#include "nameplates.h"
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace {

const uint32_t kOpOption = 0xbb58;       // OP_EquipmentKeyRingOption, both directions
const uint32_t kOpPersonaSwap = 0x6db0;
const uint32_t kOpNewZone = 0x5ec0, kOpPlayerProfile = 0x08bf;   // every zone-in
const uint32_t kRingType = 39;           // Live eItemContainerEquipmentKeyRingItems
const uint32_t kMaxCapacity = 1000;
const int16_t kCursorSlot = 0x23;
const uint64_t kHelloDelayMs = 2000;     // after the game state turns "in game"
const uint64_t kHelloRetryMs = 8000;
const int kHelloTries = 3;
const uint64_t kFrameGapMs = 3000;      // no in-game frame for this long = a zone load
const uint64_t kPickUpGuardMs = 750;

// item placement packet types that reach the slot switch (0x6a = CharInventory on ToB)
const int kPlaceTrade = 0x67, kPlaceCharInventory = 0x6a, kPlaceOther = 0x75;

// item container (FUN_1406886d0)
const size_t kContCapacity = 0x00;       // u32
const size_t kContType = 0x04;           // u32
const size_t kContItems = 0x08;          // {item*, control block*} x size
const size_t kContSize = 0x10;           // int
// item instance
const size_t kItemSlot = 0xc8;           // u32 type, i16 slot, i16 bag, i16 aug
const size_t kItemVtblDef = 0x28;        // ItemDefinition* (its name is at +0)
// item definition (the layout Live's equipment rule and ToB's FUN_1406990b0 read)
const size_t kDefSlots = 0xbc;
const size_t kDefEvolving = 0x590;
const size_t kDefClickType = 0x225;
const size_t kDefItemType = 400;
const uint8_t kClickExpendable = 3, kItemTypePotion = 0x15;
// profile (FUN_14067c550 on the PcClient's profile list): the possessions container at +0x20
const size_t kProfileItems = 0x28;
const size_t kProfileSize = 0x30;
// local player
const size_t kSpawnName = 0xb4;
// KeyRingWnd
const size_t kKrwAutoLeaveBox = 0x3f8;   // KRW_Familiars_AutoLeaveButton, a checkbox (vtable reference for ours)
// CXWnd / CListWnd / checkbox
const size_t kWndVisible = 0x211;
const size_t kListRowCount = 0x268;
const size_t kListScroll = 0x1a8;        // int, restored after a rebuild as the client does
const size_t kVtblShow = 0x1c0;
const size_t kVtblSetText = 0x280;
const size_t kVtblListSetScroll = 0x2e8;
const size_t kVtblListDeleteAll = 0x380;
const size_t kVtblListDoneAdding = 0x398;
const size_t kVtblIsChecked = 0x348;
const size_t kVtblSetCheck = 0x380;
const size_t kVtblGetContainer = 0x128;  // on the profile base: container for an item index
const uint8_t kIsCheckedStart[] = { 0x0F, 0xB6, 0x81, 0x70, 0x02, 0x00, 0x00, 0xC3 };
const uint8_t kSetCheckStart[] = { 0x38, 0x91, 0x70, 0x02, 0x00, 0x00, 0x74 };
const int kNotifyClick = 1, kNotifyRightClick = 3, kNotifyHeld = 0x17, kNotifyDrop = 0x23;
const int kChatRefusal = 0xd;            // the colour the client's own move refusals use

struct ItemIndex {
	uint32_t type;
	int16_t slot[3];
	int16_t pad;
};

struct ItemPtr {         // the client's shared pointer {item, control block}
	uint8_t* item;
	void* control;
};

using PlaceItemFn = void (*)(int packetType, ItemPtr* item, void* stream);
using KrwNotifyFn = uint64_t (*)(uint8_t* wnd, uint8_t* sender, int code, void* data);
using StoreItemFn = void (*)(uint8_t* base, ItemIndex* slot, ItemPtr* item);
using ClearContainerFn = void (*)(uint8_t* container);
using GetProfileFn = uint8_t* (*)(uint8_t* list);
using MoveItemFn = uint8_t (*)(uint8_t* mgr, ItemIndex* from, ItemIndex* to, uint8_t a4, uint8_t a5, uint8_t a6, uint8_t a7);
using SendMessageFn = uint8_t (*)(uint8_t* connection, int channel, const uint8_t* data, int length);
using ShowItemFn = void (*)(uint8_t* mgr, ItemPtr* item, int flag);
using ChatMgrFn = uint8_t* (*)();
using ChatPrintFn = void (*)(uint8_t* mgr, const char* text, int colour, uint8_t a4, uint8_t a5, uint8_t a6, uint8_t a7);
using GetChildFn = uint8_t* (*)(uint8_t* parent, void** name, char log);
using AddRowFn = int (*)(uint8_t* list, void** text, uint32_t colour, int64_t data, void* a5, void* a6, uint8_t a7);
using SetCellIconFn = void (*)(uint8_t* list, int row, int col, void* texture);
using SetItemTextFn = void (*)(uint8_t* list, int row, int col, void** text);
using SetItemDataFn = void (*)(uint8_t* list, int row, int64_t data);
using GetCurSelFn = int (*)(uint8_t* list);
using GetItemDataFn = int64_t (*)(uint8_t* list, int row);
using SetCurSelFn = void (*)(uint8_t* list, int row);
using ListLayoutFn = void (*)(uint8_t* list);
using ItemIconFn = uint32_t (*)(uint8_t* item);
using IconTextureFn = void* (*)(uint8_t* cache, uint32_t icon);
using GetContainerFn = uint8_t* (*)(uint8_t* base, ItemIndex* index);
using ItemDefFn = const uint8_t* (*)(uint8_t* item);
using ShowFn = void (*)(uint8_t* wnd, uint8_t visible, uint8_t unknown);
using SetTextFn = void (*)(uint8_t* wnd, void** text);
using ListVoidFn = void (*)(uint8_t* list);
using ListIntFn = void (*)(uint8_t* list, int value);
using IsCheckedFn = uint8_t (*)(uint8_t* box);
using SetCheckFn = void (*)(uint8_t* box, uint8_t on, uint8_t unused);

PlaceItemFn g_origPlace = nullptr;           // FUN_1401ff230
KrwNotifyFn g_origNotify = nullptr;          // FUN_140157100
uint8_t* g_placeFn = nullptr;                // function starts, for the prepare checks
uint8_t* g_notifyFn = nullptr;
uint8_t* g_refreshFn = nullptr;              // the client's ring list refresh FUN_1401568e0
uint8_t* g_chatSite = nullptr;               // only for the reference table
StoreItemFn g_storeItem = nullptr;           // FUN_140696300
ClearContainerFn g_clearContainer = nullptr; // FUN_14010c790
MoveItemFn g_moveItem = nullptr;             // FUN_14040d090
SendMessageFn g_sendMessage = nullptr;       // FUN_140547ef0
ShowItemFn g_showItem = nullptr;             // FUN_140432200
ChatMgrFn g_chatMgr = nullptr;               // FUN_1401093c0
ChatPrintFn g_chatPrint = nullptr;           // FUN_140108b10
GetChildFn g_getChild = nullptr;             // FUN_14059f8a0
uint8_t** g_pInvMgr = nullptr;               // &DAT_140db51c8
int32_t* g_pPendingMoves = nullptr;          // &DAT_140ee2b2c (item use waiting for the server)
uint8_t** g_pConnection = nullptr;           // &DAT_140e65da8
uint8_t** g_pKeyRingWnd = nullptr;           // &DAT_140db4e40
uint8_t** g_pItemDisplayMgr = nullptr;       // &DAT_140efd330
char* g_serverName = nullptr;                // DAT_140e6f558 (the "<name>_<server>" of the character files)
// decoded from the calls in the ring refresh (prepare)
GetCurSelFn g_getCurSel = nullptr;           // FUN_1405b5f60
GetItemDataFn g_getItemData = nullptr;       // FUN_1405b62e0
AddRowFn g_addRow = nullptr;                 // FUN_1405b2250
ItemIconFn g_itemIcon = nullptr;             // FUN_14064d7e0
uint8_t** g_pIconCache = nullptr;            // &DAT_140efac20
IconTextureFn g_iconTexture = nullptr;       // FUN_1403c6490
SetCellIconFn g_setCellIcon = nullptr;       // FUN_1405b9110
SetItemTextFn g_setItemText = nullptr;       // FUN_1405b9310
SetItemDataFn g_setItemData = nullptr;       // FUN_1405b90d0
SetCurSelFn g_setCurSel = nullptr;           // FUN_1405b8dc0
GetProfileFn g_getProfile = nullptr;         // FUN_14067c550
ListLayoutFn g_listLayout = nullptr;         // FUN_1405b2590
bool g_on = false;

// ---- state ----

uint32_t g_capacity = 0;     // from the server's reply; 0 = no ring
bool g_gotState = false;
bool g_inGame = false;      // as the DrawWindows hook last saw it
bool g_storeSets = false;    // the box, saved per character
// Who each ring slot belongs to, from the optional tail of the state packet: "<class>" or "<class> (<set>)".
const int kLabelSize = 48;
char g_labels[kMaxCapacity][kLabelSize] = {};
const char* const kClassNames[] = { "", "Warrior", "Cleric", "Paladin", "Ranger", "Shadow Knight", "Druid", "Monk", "Bard",
	"Rogue", "Shaman", "Necromancer", "Wizard", "Magician", "Enchanter", "Beastlord", "Berserker" };

// State tail: u16 count, then count x {u16 index; u8 class (1-16, 0 = none); u8 length; set name (length bytes)}. The
// server sends the whole table with every state; a state without a tail leaves the labels empty.
void ReadLabels(const uint8_t* p, uint32_t size) {
	memset(g_labels, 0, sizeof(g_labels));
	if (size < 7) return;
	const uint32_t count = *reinterpret_cast<const uint16_t*>(p + 5);
	uint32_t at = 7;
	for (uint32_t i = 0; i < count && at + 4 <= size; ++i) {
		const uint16_t index = *reinterpret_cast<const uint16_t*>(p + at);
		const uint8_t cls = p[at + 2];
		const uint8_t length = p[at + 3];
		at += 4;
		if (at + length > size) break;
		if (index < kMaxCapacity) {
			char set[kLabelSize] = {};
			memcpy(set, p + at, length < kLabelSize - 1 ? length : kLabelSize - 1);
			const char* name = cls < sizeof(kClassNames) / sizeof(kClassNames[0]) ? kClassNames[cls] : "";
			if (set[0] && name[0]) snprintf(g_labels[index], kLabelSize, "%s (%s)", name, set);
			else snprintf(g_labels[index], kLabelSize, "%s", name[0] ? name : set);
		}
		at += length;
	}
}char g_charIni[MAX_PATH] = {};
char g_gameDir[MAX_PATH] = {};

enum Hello { kHelloIdle, kHelloWait, kHelloSent };
Hello g_hello = kHelloIdle;
uint64_t g_inGameAt = 0;
uint64_t g_helloAt = 0;
int g_helloTries = 0;
uint8_t* g_helloConn = nullptr;      // connection the hello went to
uint64_t g_lastFrame = 0;
uint64_t g_pickedAt = 0;

void Log(const char* fmt, ...) {
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s\\dinput8.log", g_gameDir);
	FILE* f = nullptr;
	if (fopen_s(&f, path, "a") != 0 || !f) return;
	va_list args;
	va_start(args, fmt);
	fputs("EquipmentKeyRing: ", f);
	vfprintf(f, fmt, args);
	va_end(args);
	fputc('\n', f);
	fclose(f);
}

void* Virtual(uint8_t* obj, size_t slot) {
	return (*reinterpret_cast<void***>(obj))[slot / sizeof(void*)];
}

// The PcClient's virtual base (the inventory profile owner), as the client computes it: pc + 8 + vbtable[1].
uint8_t* ProfileBase() {
	uint8_t* pc = static_cast<uint8_t*>(HudLocalPC());
	if (!pc) return nullptr;
	const uint8_t* vbtable = *reinterpret_cast<uint8_t**>(pc + 8);
	return vbtable ? pc + 8 + *reinterpret_cast<const int32_t*>(vbtable + 4) : nullptr;
}

// ToB's type-39 container (pLocalPC +0x1460), found through the client's own lookup. Null if that ever stops being a
// type-39 container.
uint8_t* RingContainer() {
	uint8_t* base = ProfileBase();
	if (!base) return nullptr;
	ItemIndex index = { kRingType, { -1, -1, -1 }, 0 };
	uint8_t* c = reinterpret_cast<GetContainerFn>(Virtual(base, kVtblGetContainer))(base, &index);
	return c && *reinterpret_cast<uint32_t*>(c + kContType) == kRingType ? c : nullptr;
}

// Stores and lookups only reach indexes below the container's capacity; the client's clear sets it back to 0.
void ApplyCapacity(uint8_t* c) {
	if (c && *reinterpret_cast<uint32_t*>(c + kContCapacity) != g_capacity) *reinterpret_cast<uint32_t*>(c + kContCapacity) = g_capacity;
}

int RingSize(uint8_t* c) {
	if (!c) return 0;
	int size = *reinterpret_cast<int*>(c + kContSize);
	const int cap = static_cast<int>(*reinterpret_cast<uint32_t*>(c + kContCapacity));
	if (size > cap) size = cap;
	return size < 0 ? 0 : size;
}

ItemPtr* RingEntry(uint8_t* c, int index) {
	if (!c || index < 0 || index >= RingSize(c)) return nullptr;
	ItemPtr* entry = *reinterpret_cast<ItemPtr**>(c + kContItems) + index;
	return entry->item ? entry : nullptr;
}

void ClearRing() {
	if (uint8_t* c = RingContainer()) {
		g_clearContainer(c);
		ApplyCapacity(c);
	}
}

uint8_t* CursorItem() {
	uint8_t* base = ProfileBase();
	uint8_t* profile = base ? g_getProfile(base + 8) : nullptr;
	if (!profile || *reinterpret_cast<int*>(profile + kProfileSize) <= kCursorSlot) return nullptr;
	return (*reinterpret_cast<ItemPtr**>(profile + kProfileItems))[kCursorSlot].item;
}

const uint8_t* ItemDef(uint8_t* item) {
	return reinterpret_cast<ItemDefFn>(Virtual(item, kItemVtblDef))(item);
}

bool IsEquipment(uint8_t* item) {
	const uint8_t* def = ItemDef(item);
	return def && *reinterpret_cast<const int32_t*>(def + kDefSlots) != 0 && *reinterpret_cast<const int32_t*>(def + kDefEvolving) < 2 &&
	       def[kDefClickType] != kClickExpendable && def[kDefItemType] != kItemTypePotion;
}

void Chat(const char* text) {
	if (uint8_t* mgr = g_chatMgr()) g_chatPrint(mgr, text, kChatRefusal, 1, 1, 0, 1);
}

// ---- configuration: [Inventory] StoreEquipmentSetItems in <character>_<server>.ini ----

void LoadCharConfig() {
	uint8_t* me = *HudLocalPlayerRef();
	if (!me || !g_serverName) return;
	snprintf(g_charIni, sizeof(g_charIni), "%s\\%s_%s.ini", g_gameDir, reinterpret_cast<const char*>(me + kSpawnName), g_serverName);
	g_storeSets = GetPrivateProfileIntA("Inventory", "StoreEquipmentSetItems", 0, g_charIni) != 0;
}

void SaveCharConfig() {
	if (g_charIni[0]) WritePrivateProfileStringA("Inventory", "StoreEquipmentSetItems", g_storeSets ? "1" : "0", g_charIni);
}

bool SendOption() {
	uint8_t* conn = *g_pConnection;
	if (!conn) return false;
	const uint8_t packet[3] = { static_cast<uint8_t>(kOpOption & 0xff), static_cast<uint8_t>(kOpOption >> 8), static_cast<uint8_t>(g_storeSets ? 1 : 0) };
	return g_sendMessage(conn, 4, packet, sizeof(packet)) != 0;
}

// ---- the Equipment page ----

struct Page {
	uint8_t* wnd;
	uint8_t* list;
	uint8_t* count;
	uint8_t* box;
	uint64_t shown;       // content signature the list was last built for
	bool missingLogged;
};
Page g_ui = {};

uint8_t* Child(uint8_t* parent, const char* id) {
	void* s = nullptr;
	HudCXStr(&s, id);
	if (!s) return nullptr;
	uint8_t* wnd = g_getChild(parent, &s, 0);
	HudCXStrRelease(&s);
	return wnd;
}

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

void SetText(uint8_t* wnd, const char* text) {
	if (!wnd) return;
	void* s = nullptr;
	HudCXStr(&s, text);
	reinterpret_cast<SetTextFn>(Virtual(wnd, kVtblSetText))(wnd, &s);
	HudCXStrRelease(&s);
}

void SetBox(uint8_t* box, bool on) {
	if (box) reinterpret_cast<SetCheckFn>(Virtual(box, kVtblSetCheck))(box, on ? 1 : 0, 0);
}

bool IsChecked(uint8_t* box) {
	return reinterpret_cast<IsCheckedFn>(Virtual(box, kVtblIsChecked))(box) != 0;
}

// Looks the page's controls up once per window (a UI reload makes a new one).
void Resolve(uint8_t* wnd) {
	if (g_ui.wnd == wnd) return;
	Page p = {};
	p.wnd = wnd;
	p.missingLogged = g_ui.missingLogged;
	p.list = Child(wnd, "KRW_Equipment_List");
	p.count = Child(wnd, "KRW_Equipment_Count");
	p.box = Child(wnd, "KRW_Equipment_StoreSetsCheckbox");
	if (p.list) {
		// The list settings the window's constructor FUN_140153190 gives the six stock ring lists: 30 px rows (the
		// icons are drawn square in them) and the selection colour.
		*reinterpret_cast<uint8_t*>(p.list + 0x2b5) = 1;
		*reinterpret_cast<uint32_t*>(p.list + 0x2bc) = 30;
		*reinterpret_cast<uint32_t*>(p.list + 0x2c8) = 0xff8aa3ffu;
		// +0x334 bit 0x40000 = the list sorts itself (column +0x2b0), bit 0x400000 = by header clicks (CListWnd
		// FUN_1405b8430). Both off: the rows stay in the order Refresh adds them (persona, then name).
		*reinterpret_cast<uint32_t*>(p.list + 0x334) &= ~(0x40000u | 0x400000u);

	}
	if (p.box && !LooksLikeCheckbox(*reinterpret_cast<uint8_t**>(wnd + kKrwAutoLeaveBox), p.box)) p.box = nullptr;
	p.shown = ~0ull;
	if (!p.list && !p.missingLogged) {
		Log("KRW_Equipment_List not found: the Key Rings window has no Equipment tab (install the patched EQUI_KeyRingWnd.xml)");
		p.missingLogged = true;
	}
	g_ui = p;
}

uint64_t Signature() {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
	uint8_t* c = RingContainer();
	mix(g_capacity);
	mix(g_storeSets);
	mix(reinterpret_cast<uint64_t>(c));
	const int size = RingSize(c);
	mix(static_cast<uint64_t>(size));
	for (int i = 0; i < size; ++i) {
		ItemPtr* e = *reinterpret_cast<ItemPtr**>(c + kContItems) + i;
		mix(reinterpret_cast<uint64_t>(e->item));
	}
	return h;
}

const char* ItemName(uint8_t* item) {
	const uint8_t* def = ItemDef(item);
	return def ? reinterpret_cast<const char*>(def) : "";
}

// Text colour per persona so each set reads as one block: the distinct labels in alphabetical order take the palette
// in turn (the same persona keeps its colour from one refresh to the next).
uint32_t PersonaColour(int index) {
	static const uint32_t kPalette[] = { 0xFFFFFFFF, 0xFFFFD866, 0xFF8CC8FF, 0xFF9CEB8C, 0xFFFFA8C8, 0xFFC8A8FF, 0xFFFFB870, 0xFF80E8DC };
	const int paletteSize = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
	if (index < 0 || index >= static_cast<int>(kMaxCapacity) || !g_labels[index][0]) return kPalette[0];
	int rank = 0;
	const char* seen[kMaxCapacity];
	int seenCount = 0;
	for (int i = 0; i < static_cast<int>(kMaxCapacity); ++i) {
		const char* label = g_labels[i];
		if (!label[0] || strcmp(label, g_labels[index]) >= 0) continue;
		bool dup = false;
		for (int j = 0; j < seenCount && !dup; ++j) dup = strcmp(seen[j], label) == 0;
		if (!dup) {
			seen[seenCount++] = label;
			++rank;
		}
	}
	return kPalette[rank % paletteSize];
}
void AddRow(uint8_t* list, int index, uint8_t* item, uint32_t colour) {
	void* s = nullptr;
	HudCXStr(&s, "");
	const int row = g_addRow(list, &s, colour, index, nullptr, nullptr, 1);
	HudCXStrRelease(&s);
	if (row < 0) return;
	HudCXStr(&s, index < static_cast<int>(kMaxCapacity) ? g_labels[index] : "");
	g_setItemText(list, row, 0, &s);
	HudCXStrRelease(&s);
	g_setCellIcon(list, row, 1, g_iconTexture(*g_pIconCache, g_itemIcon(item)));
	HudCXStr(&s, ItemName(item));
	g_setItemText(list, row, 3, &s);   // column 2 is an 8 px gap after the icon
	HudCXStrRelease(&s);
	g_setItemData(list, row, index);
}

// Rebuilds the list as the client's ring refresh does: keep the selected ring index and the scroll position.
void Refresh(uint64_t signature) {
	uint8_t* list = g_ui.list;
	int64_t selected = -1;
	const int sel = g_getCurSel(list);
	if (sel != -1) selected = g_getItemData(list, sel);
	const int scroll = *reinterpret_cast<int*>(list + kListScroll);
	reinterpret_cast<ListVoidFn>(Virtual(list, kVtblListDeleteAll))(list);
	uint8_t* c = RingContainer();
	// Persona alphabetically (unlabelled last), then item name.
	static int order[kMaxCapacity];
	int count = 0;
	for (int i = 0, n = RingSize(c); i < n; ++i)
		if (RingEntry(c, i)) order[count++] = i;
	std::sort(order, order + count, [c](int a, int b) {
		const char* la = g_labels[a];
		const char* lb = g_labels[b];
		if (!la[0] != !lb[0]) return la[0] != 0;
		if (const int byPersona = _stricmp(la, lb)) return byPersona < 0;
		if (const int byName = _stricmp(ItemName(RingEntry(c, a)->item), ItemName(RingEntry(c, b)->item))) return byName < 0;
		return a < b;
	});
	for (int k = 0; k < count; ++k) AddRow(list, order[k], RingEntry(c, order[k])->item, PersonaColour(order[k]));
	reinterpret_cast<ListIntFn>(Virtual(list, kVtblListDoneAdding))(list, 0);
	if (selected != -1) {
		const int rows = *reinterpret_cast<int*>(list + kListRowCount);
		for (int r = 0; r < rows; ++r) {
			if (g_getItemData(list, r) == selected) {
				g_setCurSel(list, r);
				break;
			}
		}
	}
	reinterpret_cast<ListIntFn>(Virtual(list, kVtblListSetScroll))(list, scroll);
	g_listLayout(list);
	char text[96];
	snprintf(text, sizeof(text), "equipment items in key ring: %d / %u", count, g_capacity);
	SetText(g_ui.count, text);
	Show(g_ui.box, g_capacity > 0);
	SetBox(g_ui.box, g_storeSets);
	g_ui.shown = signature;
}

void UpdateWindow() {
	uint8_t* wnd = *g_pKeyRingWnd;
	if (!wnd) {
		g_ui.wnd = nullptr;
		return;
	}
	Resolve(wnd);
	if (!g_ui.list || !wnd[kWndVisible]) return;
	const uint64_t signature = Signature();
	if (signature != g_ui.shown) Refresh(signature);
}

ItemPtr* SelectedEntry(int* index) {
	const int sel = g_getCurSel(g_ui.list);
	if (sel < 0) return nullptr;
	*index = static_cast<int>(g_getItemData(g_ui.list, sel));
	return RingEntry(RingContainer(), *index);
}

void Inspect() {
	int index = -1;
	ItemPtr* e = SelectedEntry(&index);
	uint8_t* mgr = *g_pItemDisplayMgr;
	if (!e || !mgr) return;
	ItemPtr copy = *e;   // the container keeps its reference for the call; the window takes its own
	g_showItem(mgr, &copy, 1);
}

void PickUp() {
	int index = -1;
	if (*g_pPendingMoves > 0 || !SelectedEntry(&index)) return;
	ItemIndex from = { kRingType, { static_cast<int16_t>(index), -1, -1 }, 0 };
	ItemIndex to = { 0, { kCursorSlot, -1, -1 }, 0 };
	g_moveItem(*g_pInvMgr, &from, &to, 0, 0, 0, 0);
	g_pickedAt = GetTickCount64();
}

void Drop(uint8_t* cursor) {
	// the click that ends the hold which picked an item up must not put it straight back
	if (*g_pPendingMoves > 0 || GetTickCount64() - g_pickedAt < kPickUpGuardMs) return;
	if (!IsEquipment(cursor)) {
		Chat("Only equipment can go in the equipment key ring.");
		return;
	}
	ItemIndex from = { 0, { kCursorSlot, -1, -1 }, 0 };
	ItemIndex to = { kRingType, { -1, -1, -1 }, 0 };   // the server picks the slot and sends the item back
	g_moveItem(*g_pInvMgr, &from, &to, 0, 0, 0, 0);
}

bool ListNotify(int code) {
	if (!g_capacity) return false;
	const bool modifier = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	uint8_t* cursor = CursorItem();
	switch (code) {
	case kNotifyClick:
		if (modifier) {
			Inspect();
			return true;
		}
		if (cursor) {
			Drop(cursor);
			return true;
		}
		return false;
	case kNotifyDrop:
		if (!cursor) return false;
		Drop(cursor);
		return true;
	case kNotifyHeld:
		if (cursor) return false;
		PickUp();
		return true;
	case kNotifyRightClick:
		Inspect();
		return true;
	default:
		return false;
	}
}

// ---- hooks ----

uint64_t NotifyDetour(uint8_t* wnd, uint8_t* sender, int code, void* data) {
	if (g_on && wnd && sender && wnd == *g_pKeyRingWnd && g_inGame) {
		Resolve(wnd);
		if (sender == g_ui.list && ListNotify(code)) return 0;
		if (sender == g_ui.box && code == kNotifyClick) {
			g_storeSets = IsChecked(sender);
			SaveCharConfig();
			if (g_gotState && g_capacity) SendOption();
			return 0;
		}
	}
	return g_origNotify(wnd, sender, code, data);
}

// After the client's own placement (which has no case for type 39 and only refreshes item windows for it), store a
// ring item the way it stores the other rings' items.
void PlaceDetour(int packetType, ItemPtr* item, void* stream) {
	g_origPlace(packetType, item, stream);
	if (!g_on || !g_capacity || !item || !item->item) return;
	if (packetType != kPlaceTrade && packetType != kPlaceCharInventory && packetType != kPlaceOther) return;
	const uint8_t* slot = item->item + kItemSlot;
	if (*reinterpret_cast<const uint32_t*>(slot) != kRingType) return;
	ItemIndex index = { kRingType, { *reinterpret_cast<const int16_t*>(slot + 4), *reinterpret_cast<const int16_t*>(slot + 6),
	                                  *reinterpret_cast<const int16_t*>(slot + 8) }, 0 };
	if (index.slot[0] < 0 || static_cast<uint32_t>(index.slot[0]) >= g_capacity || index.slot[1] != -1) return;
	uint8_t* base = ProfileBase();
	uint8_t* c = RingContainer();
	if (!base || !c) return;
	ApplyCapacity(c);
	g_storeItem(base, &index, item);
	g_ui.shown = ~0ull;   // rebuild the list on the next frame
}

template <typename T>
bool CallsWithin(const uint8_t* fn, size_t size, T target) {
	for (size_t i = 0; i + 5 <= size; ++i) {
		if (fn[i] != 0xE8) continue;
		int32_t rel;
		memcpy(&rel, fn + i + 1, 4);
		if (fn + i + 5 + rel == reinterpret_cast<const uint8_t*>(target)) return true;
	}
	return false;
}

// The target of the rel32 call at fn + offset; false if there is no call there.
template <typename T>
bool CallAt(const uint8_t* fn, size_t offset, T* out) {
	if (fn[offset] != 0xE8) return false;
	int32_t rel;
	memcpy(&rel, fn + offset + 1, 4);
	*out = reinterpret_cast<T>(const_cast<uint8_t*>(fn + offset + 5 + rel));
	return true;
}

#define REF(var) reinterpret_cast<void**>(&var)

// KeyRingWnd::WndNotification FUN_140157100(wnd, sender, code, data)
const char kNotifySig[] = "48 89 5C 24 08 4C 89 4C 24 20 44 89 44 24 18 55";
// item placement FUN_1401ff230(packet type, item, stream)
const char kPlaceSig[] = "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 F0 FE FF FF";
// FUN_140156160: mov edx,2381; call eqstr; mov rdi,rax; call chat window getter; ...; call print
const char kChatSig[] =
	"BA 81 23 00 00 E8 ?? ?? ?? ?? 48 8B F8 E8 ?? ?? ?? ?? C6 44 24 30 01 48 8B C8 C6 44 24 28 00 41 B1 01 "
	"41 B8 0F 00 00 00 C6 44 24 20 01 48 8B D7 E8 ?? ?? ?? ??";

} // namespace

namespace {

// The server keeps the hello per zone session: send it again after every zone-in and reconnect.
void ResetHello() {
	if (g_hello == kHelloIdle) return;
	g_hello = kHelloIdle;
	g_gotState = false;   // the capacity stays: ring items that arrive before the next state still go in
}

} // namespace
void KeyRingFrame(bool inGame) {
	if (!g_on) return;
	g_inGame = inGame;
	const uint64_t now = GetTickCount64();
	if (!inGame) {
		ResetHello();
		return;
	}
	// A new zone session: another connection than the hello went to, or no frame for a while (the
	// UI is not drawn while a zone loads, so the frame hook may never see the client leave the game).
	uint8_t* conn = *g_pConnection;
	// (Not the local player alone: a persona swap replaces it in the same zone session.)
	if (g_hello != kHelloIdle && (conn != g_helloConn || now - g_lastFrame > kFrameGapMs)) ResetHello();
	g_lastFrame = now;
	if (g_hello == kHelloIdle) {
		g_helloConn = conn;
		g_hello = kHelloWait;
		g_inGameAt = now;
	}
	if (g_hello == kHelloWait && now - g_inGameAt >= kHelloDelayMs) {
		LoadCharConfig();
		// no clear here: a repeated hello in the same zone session gets only the state back, not the items
		g_hello = kHelloSent;
		g_helloAt = now;
		g_helloTries = SendOption() ? 1 : 0;
	}
	else if (g_hello == kHelloSent && !g_gotState && g_helloTries < kHelloTries && now - g_helloAt >= kHelloRetryMs) {
		g_helloAt = now;
		if (SendOption()) ++g_helloTries;
		if (g_helloTries == kHelloTries) Log("no answer from the server to the hello: the server has no Equipment Key Ring");
	}
	// Something in the client's zone-in copies the stock capacity (0) back over this container while its items stay,
	// which hides them from every lookup; put the server's capacity back each frame.
	if (g_capacity) ApplyCapacity(RingContainer());
	UpdateWindow();
}

void KeyRingOnPacket(uint32_t op, const uint8_t* data, uint32_t size) {
	if (!g_on || !data) return;
	if (op == kOpOption && size >= 5) {
		uint32_t capacity = *reinterpret_cast<const uint32_t*>(data);
		if (capacity > kMaxCapacity) capacity = kMaxCapacity;
		if (!g_gotState || capacity != g_capacity) Log("server ring capacity %u", capacity);
		g_capacity = capacity;
		g_gotState = true;
		ApplyCapacity(RingContainer());
		ReadLabels(data, size);
		g_ui.shown = ~0ull;
	}
	else if (op == kOpNewZone || op == kOpPlayerProfile) {
		ResetHello();
	}
	else if (op == kOpPersonaSwap && size >= 7 && data[4] == 1) {
		ClearRing();   // the swap leaves the ring out; the server resends it right after
	}
}

const HookSite kKeyRingHooks[4] = {
	// Shared with the Hud, FCT, Nameplates and AdvLootActions (same signature, detour and trampoline).
	{ kDrawWindowsSig, 5, reinterpret_cast<void*>(&DrawWindowsDetour), reinterpret_cast<void**>(&g_origDrawWindows) },
	// Shared with Nameplates: the packet dispatcher FUN_1401e4010.
	{ kDispatchSig, 5, reinterpret_cast<void*>(g_dispatchDetour), reinterpret_cast<void**>(g_origDispatchSlot) },
	// Item placement FUN_1401ff230. Stolen (5): mov [rsp+8],rbx.
	{ kPlaceSig, 5, reinterpret_cast<void*>(&PlaceDetour), REF(g_origPlace) },
	// KeyRingWnd::WndNotification FUN_140157100. Stolen (5): mov [rsp+8],rbx.
	{ kNotifySig, 5, reinterpret_cast<void*>(&NotifyDetour), REF(g_origNotify) },
};

const RefSite kKeyRingRefs[17] = {
	{ kPlaceSig, 0, 0, REF(g_placeFn) },
	{ kNotifySig, 0, 0, REF(g_notifyFn) },
	// the client's ring list refresh FUN_1401568e0(wnd, ring): the list functions are read from its calls
	{ "48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC B8 00 00 00 4C 63 FA 4C 8B E1",
	  0, 0, REF(g_refreshFn) },
	// FUN_140696300(base, index, item): store through the profile's container lookup
	{ "40 57 48 83 EC 20 83 3A 2B", 0, 0, REF(g_storeItem) },
	// FUN_14010c790(container): clear (a dynamic container also drops its capacity)
	{ "48 89 6C 24 18 56 48 83 EC 20 80 79 26 00", 0, 0, REF(g_clearContainer) },
	// FUN_14040d090(mgr, from, to, ...): the client's item move, sends OP_MoveItem
	{ "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 90 FD FF FF", 0, 0, REF(g_moveItem) },
	// FUN_140547ef0(connection, channel, data, length)
	{ "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 40 49 63 E9", 0, 0, REF(g_sendMessage) },
	// CItemDisplayManager::ShowItem FUN_140432200(mgr, item, flag)
	{ "48 89 5C 24 10 48 89 74 24 18 41 56 48 83 EC 20 41 8B F0", 0, 0, REF(g_showItem) },
	// CXWnd::GetChildItem FUN_14059f8a0(parent, CXStr*, char log)
	{ "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC 20 02 00 00", 0, 0, REF(g_getChild) },
	// FUN_14018d4a0 (teardown): mov rcx,[pInvSlotMgr]; test; je; mov rax,[rcx]; mov edx,1; call [rax]; mov [..],rsi
	{ "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 BA 01 00 00 00 FF 10 48 89 35 ?? ?? ?? ?? 48 8B 5C 24 30",
	  3, 7, REF(g_pInvMgr) },
	// FUN_14026e670: mov eax,[pending item uses]; test; jle; dec eax; mov [..],eax
	{ "8B 05 ?? ?? ?? ?? 85 C0 7E ?? FF C8 89 05 ?? ?? ?? ?? EB ?? 89 35 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ??",
	  2, 6, REF(g_pPendingMoves) },
	// FUN_140344570: mov rdi,[pConnection]; mov [rsp+30],rdi
	{ "48 8B 3D ?? ?? ?? ?? 48 89 7C 24 30", 3, 7, REF(g_pConnection) },
	// FUN_14040e350: mov rcx,[pKeyRingWnd]; test; je; mov byte [rbx+rcx+3c0],1 (a ring's dirty flag)
	{ "48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? C6 84 0B C0 03 00 00 01", 3, 7, REF(g_pKeyRingWnd) },
	// FUN_14018d4a0: mov [pItemDisplayMgr],rsi; mov rbx,[..]; test; je; mov rcx,rbx; call; mov edx,58
	{ "48 89 35 ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 48 8B CB E8 ?? ?? ?? ?? BA 58 00 00 00",
	  3, 7, REF(g_pItemDisplayMgr) },
	// FUN_140154260 (the window's inventory dump): test rdx,rdx; je; cmp [rdx],0; jne; mov r8,[pLocalPlayer]; add r8,b4; lea r9,[server name] for
	// "%s_%s-Inventory.txt"
	{ "48 85 D2 74 05 80 3A 00 75 ?? 4C 8B 05 ?? ?? ?? ?? 49 81 C0 B4 00 00 00 4C 8D 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? "
	  "48 8D 4C 24 40 E8 ?? ?? ?? ?? 48 8D 44 24 40 48 8D 15 ?? ?? ?? ?? 48 8B C8 E8", 27, 31, REF(g_serverName) },
	// FUN_140156160 (the window's frame): its "not while switching personas" message, eqstr 0x2381 to the chat
	// window (the chat window getter, then the print with colour, r9b 1 and stack bytes 1, 0, 1)
	{ kChatSig, 14, 18, REF(g_chatMgr) },
	{ kChatSig, 49, 53, REF(g_chatPrint) },
};

const char* KeyRingPrepare() {
	if (const char* e = HudTextPrepare()) return e;
	// The ring refresh's calls, by their place in FUN_1401568e0 (each must be a rel32 call).
	const uint8_t* r = g_refreshFn;
	if (!CallAt(r, 0x64, &g_getCurSel) || !CallAt(r, 0x78, &g_getItemData) || !CallAt(r, 0x149, &g_addRow) ||
	    !CallAt(r, 0x2af, &g_itemIcon) || !CallAt(r, 0x2bd, &g_iconTexture) || !CallAt(r, 0x2d6, &g_setCellIcon) ||
	    !CallAt(r, 0x301, &g_setItemText) || !CallAt(r, 0x372, &g_setItemData) || !CallAt(r, 0x414, &g_setCurSel) ||
	    !CallAt(r, 0x5cb, &g_getProfile) || !CallAt(r, 0x645, &g_listLayout))
		return "the ring list refresh does not have the calls we expect";
	// mov rcx,[icon cache] right before the icon texture call
	if (r[0x2b6] != 0x48 || r[0x2b7] != 0x8B || r[0x2b8] != 0x0D) return "the ring list refresh does not load the icon cache";
	int32_t disp;
	memcpy(&disp, r + 0x2b9, 4);
	g_pIconCache = reinterpret_cast<uint8_t**>(const_cast<uint8_t*>(r + 0x2bd + disp));
	// The client's own ring code uses the functions resolved by signature.
	if (!CallsWithin(g_placeFn, 0xc30, g_storeItem) || !CallsWithin(g_placeFn, 0xc30, g_getProfile))
		return "the item placement does not store with the function we resolved";
	if (!CallsWithin(reinterpret_cast<const uint8_t*>(g_moveItem), 0x10e0, g_storeItem))
		return "the item move does not store with the function we resolved";
	if (!CallsWithin(g_notifyFn, 0xa20, g_sendMessage) || !CallsWithin(g_notifyFn, 0xa20, g_getCurSel))
		return "the Key Rings window does not use the send / list functions we resolved";
	GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH);
	if (char* slash = strrchr(g_gameDir, '\\')) *slash = '\0';
	g_on = true;
	return nullptr;
}
