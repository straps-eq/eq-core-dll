// AA600 mirror: the first 300 owned AAs copied back to the old PcProfile slot each frame.
//
// AA600 (aa600_sites.h) moves the owned-AA array from PcProfile+0x18cc (300 x 12) to +0x6a98 (600 x 12), and every
// client access goes to the new one. Tools that read the profile directly (MacroQuest's eqlib
// PcProfile::GetAlternateAbilityId, and E3Next through it) still read +0x18cc and found nothing. Nothing in the client
// uses the old block any more, so the shared DrawWindows hook copies entries 0..299 into it whenever they differ.
//
// Profile lookup as OP_RespondAA does it (0x140217b00): pc = *pLocalPC; GetPcProfile(pc + 0x10 + vbadj), with vbadj
// the i32 at [[pc+8]+4]. The mirror only runs once the PcProfile malloc site reads 0x86b8, i.e. AA600's writes landed.
#include "aa600.h"
#include "hud.h"
#include <cstdint>
#include <cstring>

namespace {

const size_t kOldList = 0x18cc;
const size_t kNewList = 0x6a98;
const size_t kListBytes = 300 * 12;
const uint32_t kGrownProfileSize = 0x86b8;

using GetPcProfileFn = uint8_t* (*)(uint8_t* base);

uint8_t** g_pLocalPC = nullptr;          // DAT_140e65d90
GetPcProfileFn g_getPcProfile = nullptr; // FUN_14067c550
uint32_t* g_allocSize = nullptr;   // malloc size immediate in the PcClient ctor (0x140659806 + 1)
bool g_on = false;
int g_ready = -1;                        // -1 unchecked, 0 sites not written, 1 mirror

#define REF(var) reinterpret_cast<void**>(&var)

} // namespace

// OP_RespondAA list loop head: mov rdx,[pLocalPC]; vbadj; call GetPcProfile; mov r8,rax; cmp edi,<300 or 600>
static const char kRespondAAProfileSig[] =
	"48 8B 15 ?? ?? ?? ?? 48 8B 42 08 48 83 C2 10 48 63 48 04 48 03 CA E8 ?? ?? ?? ?? 4C 8B C0 81 FF";

const HookSite kAA600Hooks[1] = {
	{ kDrawWindowsSig, 5, reinterpret_cast<void*>(&DrawWindowsDetour), reinterpret_cast<void**>(&g_origDrawWindows) },
};

const RefSite kAA600Refs[3] = {
	{ kRespondAAProfileSig, 3, 7, REF(g_pLocalPC) },
	{ kRespondAAProfileSig, 23, 27, REF(g_getPcProfile) },
	// site Z1: call ...; nop; mov ecx,<PcProfile size>; call malloc
	{ "E8 8D 2D A3 FF 90 B9 ?? ?? 00 00 E8 82 3F 06 00", 7, 0, REF(g_allocSize) },
};

const char* AA600Prepare() {
	if (const char* e = HudTextPrepare()) return e;
	g_on = true;
	return nullptr;
}

void AA600Frame(bool inGame) {
	if (!g_on || !inGame) return;
	if (g_ready < 0) g_ready = *g_allocSize == kGrownProfileSize ? 1 : 0;
	if (!g_ready) return;
	uint8_t* pc = *g_pLocalPC;
	if (!pc) return;
	uint8_t* vbtbl = *reinterpret_cast<uint8_t**>(pc + 8);
	if (!vbtbl) return;
	uint8_t* profile = g_getPcProfile(pc + 0x10 + *reinterpret_cast<int32_t*>(vbtbl + 4));
	if (!profile) return;
	if (memcmp(profile + kOldList, profile + kNewList, kListBytes) != 0)
		memcpy(profile + kOldList, profile + kNewList, kListBytes);
}
