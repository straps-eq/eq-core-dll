#include "patcher.h"
#include <windows.h>
#include <cstring>

namespace {

// Parses "48 8B ?? 30" into bytes plus a mask (false = wildcard).
bool ParseHex(const char* text, std::vector<uint8_t>* bytes, std::vector<bool>* mask) {
	bytes->clear();
	if (mask) mask->clear();
	const char* p = text;
	while (*p) {
		while (*p == ' ') ++p;
		if (!*p) break;
		if (p[0] == '?' && p[1] == '?') {
			if (!mask) return false;
			bytes->push_back(0);
			mask->push_back(false);
			p += 2;
			continue;
		}
		char hex[3] = { p[0], p[1], 0 };
		char* end = nullptr;
		unsigned long v = strtoul(hex, &end, 16);
		if (end != hex + 2) return false;
		bytes->push_back(static_cast<uint8_t>(v));
		if (mask) mask->push_back(true);
		p += 2;
	}
	return !bytes->empty();
}

std::vector<uint8_t*> FindAll(const CodeRegion& text, const std::vector<uint8_t>& sig, const std::vector<bool>& mask) {
	std::vector<uint8_t*> hits;
	if (sig.size() > text.size) return hits;
	const size_t last = text.size - sig.size();
	for (size_t i = 0; i <= last; ++i) {
		size_t j = 0;
		for (; j < sig.size(); ++j) {
			if (mask[j] && text.data[i + j] != sig[j]) break;
		}
		if (j == sig.size()) hits.push_back(text.data + i);
	}
	return hits;
}

// Finds a signature that must match exactly once. Returns nullptr and sets *error otherwise.
uint8_t* FindOne(const CodeRegion& text, const char* signature, std::vector<bool>* mask, std::string* error) {
	std::vector<uint8_t> sig;
	if (!ParseHex(signature, &sig, mask)) {
		*error = "bad signature";
		return nullptr;
	}
	auto hits = FindAll(text, sig, *mask);
	if (hits.size() != 1) {
		*error = "signature matched " + std::to_string(hits.size()) + " times, expected 1";
		return nullptr;
	}
	return hits[0];
}

bool FindText(uint8_t* base, size_t limit, bool fileLayout, CodeRegion* out) {
	if (limit < sizeof(IMAGE_DOS_HEADER)) return false;
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
	if (static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > limit) return false;
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
	auto* sec = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
		if (memcmp(sec->Name, ".text", 6) != 0) continue;
		size_t start = fileLayout ? sec->PointerToRawData : sec->VirtualAddress;
		size_t size = fileLayout ? sec->SizeOfRawData : sec->Misc.VirtualSize;
		if (fileLayout && start + size > limit) return false;
		out->data = base + start;
		out->size = size;
		out->va = fileLayout ? nt->OptionalHeader.ImageBase + sec->VirtualAddress
		                     : reinterpret_cast<uint64_t>(out->data);
		return true;
	}
	return false;
}

// ---- hook installation (live process only) ----
//
// Each hook gets a 16-byte-aligned block in an executable page allocated within +/-2GB of the hooked
// code, so a rel32 JMP can reach it from the target and the trampoline can jump back with another:
//   stub:        FF 25 00 00 00 00 <8-byte detour address>   (jmp [rip]; detour may be >2GB away)
//   trampoline:  <stolen bytes> E9 <rel32 to target+stolen>
// The target's first `stolen` bytes become E9 <rel32 to stub> followed by NOPs.

const size_t kStubSize = 16;
const size_t kBlockSize = 64;   // stub + up to kMaxStolen bytes + 5-byte jmp, rounded up

uint8_t* g_page = nullptr;      // current executable page
size_t g_pageUsed = 0;

bool Within2GB(const void* a, const void* b) {
	int64_t d = reinterpret_cast<int64_t>(a) - reinterpret_cast<int64_t>(b);
	return d > -0x7FF00000LL && d < 0x7FF00000LL;
}

// Allocates one page of free address space within +/-2GB of `anchor`, searching upward from
// `anchor` first and then upward from the lower bound.
uint8_t* AllocPageNear(const uint8_t* anchor) {
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	const uint64_t gran = si.dwAllocationGranularity;
	const uint64_t target = reinterpret_cast<uint64_t>(anchor);
	const uint64_t lo = target > 0x7FF00000ULL + gran ? target - 0x7FF00000ULL : gran;
	const uint64_t hi = target + 0x7FF00000ULL;
	const uint64_t starts[2] = { target, lo };
	for (uint64_t start : starts) {
		uint64_t addr = (start + gran - 1) / gran * gran;
		while (addr < hi) {
			MEMORY_BASIC_INFORMATION mbi;
			if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) break;
			uint64_t regionEnd = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
			if (mbi.State == MEM_FREE) {
				uint64_t candidate = (reinterpret_cast<uint64_t>(mbi.BaseAddress) + gran - 1) / gran * gran;
				if (candidate + si.dwPageSize <= regionEnd && candidate >= lo && candidate < hi) {
					void* p = VirtualAlloc(reinterpret_cast<void*>(candidate), si.dwPageSize,
					                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
					if (p) return static_cast<uint8_t*>(p);
				}
			}
			addr = (regionEnd + gran - 1) / gran * gran;
		}
	}
	return nullptr;
}

// Returns a writable block for a hook on `target`, or nullptr. The page is left PAGE_READWRITE;
// SealPage() makes it executable once a feature's hooks are written.
uint8_t* AllocBlock(const uint8_t* target) {
	if (g_page && (g_pageUsed + kBlockSize > 0x1000 || !Within2GB(g_page, target))) {
		g_page = nullptr;
	}
	if (!g_page) {
		g_page = AllocPageNear(target);
		g_pageUsed = 0;
		if (!g_page) return nullptr;
	}
	else {
		DWORD old = 0;
		if (!VirtualProtect(g_page, 0x1000, PAGE_READWRITE, &old)) return nullptr;
	}
	uint8_t* block = g_page + g_pageUsed;
	g_pageUsed += kBlockSize;
	return block;
}

void SealPage() {
	if (!g_page) return;
	DWORD old = 0;
	VirtualProtect(g_page, 0x1000, PAGE_EXECUTE_READ, &old);
	FlushInstructionCache(GetCurrentProcess(), g_page, 0x1000);
}

void WriteRel32(uint8_t* at, const uint8_t* dest) {
	int64_t rel = reinterpret_cast<int64_t>(dest) - reinterpret_cast<int64_t>(at + 4);
	int32_t rel32 = static_cast<int32_t>(rel);
	memcpy(at, &rel32, 4);
}

bool WriteCode(uint8_t* at, const uint8_t* bytes, size_t size, std::string* error) {
	DWORD old = 0;
	if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) {
		*error = "VirtualProtect failed (" + std::to_string(GetLastError()) + ")";
		return false;
	}
	memcpy(at, bytes, size);
	VirtualProtect(at, size, old, &old);
	FlushInstructionCache(GetCurrentProcess(), at, size);
	return true;
}

bool InstallHook(const HookSite& hook, uint8_t* target, std::string* error) {
	uint8_t* block = AllocBlock(target);
	if (!block) {
		*error = "no executable memory within 2GB of the hook target";
		return false;
	}
	uint8_t* stub = block;
	uint8_t* trampoline = block + kStubSize;

	// stub: jmp [rip+0]; dq detour
	static const uint8_t kJmpRipRel[6] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
	memcpy(stub, kJmpRipRel, 6);
	uint64_t detour = reinterpret_cast<uint64_t>(hook.detour);
	memcpy(stub + 6, &detour, 8);

	// trampoline: stolen bytes, then jmp back to the rest of the original
	memcpy(trampoline, target, hook.stolen);
	trampoline[hook.stolen] = 0xE9;
	WriteRel32(trampoline + hook.stolen + 1, target + hook.stolen);

	// target: jmp stub, NOP padding
	uint8_t jmp[kMaxStolen];
	memset(jmp, 0x90, sizeof(jmp));
	// The displacement is relative to where the JMP ends up (target + 5), not to this local buffer.
	jmp[0] = 0xE9;
	int32_t rel32 = static_cast<int32_t>(reinterpret_cast<int64_t>(stub) - reinterpret_cast<int64_t>(target + 5));
	memcpy(jmp + 1, &rel32, 4);
	if (!WriteCode(target, jmp, hook.stolen, error)) return false;
	int32_t written = 0;
	memcpy(&written, target + 1, 4);
	if (target[0] != 0xE9 || target + 5 + written != stub) {
		*error = "hook jump does not land on its stub";
		return false;
	}

	*hook.original = trampoline;
	return true;
}

} // namespace

bool FindTextSectionInImage(uint8_t* imageBase, CodeRegion* out) {
	return FindText(imageBase, SIZE_MAX, false, out);
}

bool FindTextSectionInFile(uint8_t* fileData, size_t fileSize, CodeRegion* out) {
	return FindText(fileData, fileSize, true, out);
}

bool ResolveFeature(const CodeRegion& text, const Feature& f, Resolved* out, std::string* error) {
	out->sites.clear();
	out->hookAddrs.clear();
	out->refAddrs.clear();
	bool ok = true;
	for (size_t s = 0; s < f.siteCount; ++s) {
		const PatchSite& site = f.sites[s];
		SiteResult r;
		std::vector<uint8_t> sig, orig, repl;
		std::vector<bool> mask;
		if (!ParseHex(site.signature, &sig, &mask) || !ParseHex(site.original, &orig, nullptr) ||
			!ParseHex(site.replacement, &repl, nullptr) || orig.size() != repl.size() ||
			site.offset + orig.size() > sig.size()) {
			r.error = "bad patch table entry";
		}
		else {
			auto hits = FindAll(text, sig, mask);
			if (static_cast<int>(hits.size()) != site.expectedMatches) {
				r.error = "signature matched " + std::to_string(hits.size()) + " times, expected " +
					std::to_string(site.expectedMatches);
			}
			else {
				for (uint8_t* hit : hits) {
					uint8_t* at = hit + site.offset;
					if (memcmp(at, orig.data(), orig.size()) != 0) {
						r.error = "original bytes differ (already patched, or a different client build)";
						break;
					}
					r.patchAddrs.push_back(at);
				}
			}
		}
		if (!r.error.empty()) {
			ok = false;
			if (error->empty()) *error = "site " + std::to_string(s + 1) + ": " + r.error;
		}
		out->sites.push_back(r);
	}
	for (size_t h = 0; h < f.hookCount; ++h) {
		const HookSite& hook = f.hooks[h];
		std::vector<bool> mask;
		std::string e;
		uint8_t* at = nullptr;
		if (hook.stolen < 5 || hook.stolen > kMaxStolen || !hook.detour || !hook.original) {
			e = "bad hook table entry";
		}
		else if ((at = FindOne(text, hook.signature, &mask, &e)) != nullptr) {
			// Every stolen byte must be spelled out, so a changed prologue is never copied blindly.
			if (mask.size() < hook.stolen) e = "signature shorter than the stolen bytes";
			for (size_t i = 0; e.empty() && i < hook.stolen; ++i)
				if (!mask[i]) e = "wildcard inside the stolen bytes";
			// A hook two features share (same detour and `original`) is installed by the first of them;
			// the second finds its trampoline already set and skips the install.
			if (e.empty() && at[0] == 0xE9 && !*hook.original) e = "already hooked";
		}
		if (!e.empty()) {
			ok = false;
			at = nullptr;
			if (error->empty()) *error = "hook " + std::to_string(h + 1) + ": " + e;
		}
		out->hookAddrs.push_back(at);
	}
	const size_t refTotal = f.refCount + f.moreRefCount + f.extraRefCount;
	for (size_t i = 0; i < refTotal; ++i) {
		const RefSite& ref = i < f.refCount ? f.refs[i]
		                   : i < f.refCount + f.moreRefCount ? f.moreRefs[i - f.refCount]
		                   : f.extraRefs[i - f.refCount - f.moreRefCount];
		std::vector<bool> mask;
		std::string e;
		uint64_t addr = 0;
		uint8_t* at = ref.target ? FindOne(text, ref.signature, &mask, &e) : nullptr;
		if (!ref.target) e = "bad ref table entry";
		if (at) {
			uint64_t matchVa = text.va + static_cast<uint64_t>(at - text.data);
			if (ref.nextInstr == 0) {
				if (ref.offset > mask.size()) e = "ref offset outside the signature";
				else addr = matchVa + ref.offset;
			}
			else if (ref.offset + 4 > ref.nextInstr || ref.nextInstr > mask.size()) {
				e = "ref displacement outside its instruction";
			}
			else {
				int32_t disp = 0;
				memcpy(&disp, at + ref.offset, 4);
				addr = matchVa + ref.nextInstr + static_cast<int64_t>(disp);
			}
		}
		if (!e.empty()) {
			ok = false;
			if (error->empty()) *error = "ref " + std::to_string(i + 1) + ": " + e;
		}
		out->refAddrs.push_back(addr);
	}
	return ok;
}

bool ApplyFeature(const Feature& f, const Resolved& r, std::string* error) {
	for (size_t i = 0; i < f.refCount; ++i)
		*f.refs[i].target = reinterpret_cast<void*>(r.refAddrs[i]);
	for (size_t i = 0; i < f.moreRefCount; ++i)
		*f.moreRefs[i].target = reinterpret_cast<void*>(r.refAddrs[f.refCount + i]);
	for (size_t i = 0; i < f.extraRefCount; ++i)
		*f.extraRefs[i].target = reinterpret_cast<void*>(r.refAddrs[f.refCount + f.moreRefCount + i]);
	if (f.prepare) {
		const char* e = f.prepare();
		if (e) {
			*error = e;
			return false;
		}
	}
	for (size_t h = 0; h < f.hookCount; ++h) {
		if (*f.hooks[h].original) continue;   // shared hook, installed by an earlier feature
		if (!InstallHook(f.hooks[h], r.hookAddrs[h], error)) {
			SealPage();
			return false;
		}
	}
	SealPage();
	for (size_t s = 0; s < f.siteCount; ++s) {
		std::vector<uint8_t> repl;
		ParseHex(f.sites[s].replacement, &repl, nullptr);
		for (uint8_t* at : r.sites[s].patchAddrs) {
			if (!WriteCode(at, repl.data(), repl.size(), error)) return false;
		}
	}
	return true;
}
