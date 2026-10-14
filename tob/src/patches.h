#pragma once
// Patch table for the ToB client (eqgame.exe, build "Sep 11 2025 11:54:10", x64).
// Each site is located by a byte signature (?? = wildcard) inside eqgame.exe's .text section.
// A feature is applied only if every one of its signatures matches exactly the expected number of
// times AND the original bytes at each patch offset are what we expect. Otherwise the feature is
// skipped and the reason is logged, so a different client build never gets patched blindly.
//
// Function names (FUN_...) and globals (DAT_...) are the Ghidra auto-names for this build.
//
// Adding a hooked feature (nametags.cpp is a complete example):
//  1. Put the detour, its tables and any state in its own src/<feature>.cpp (added to both targets
//     in CMakeLists.txt) and expose them through a small header included by patches.cpp.
//  2. HookSite: signature starting at the function's first byte; `stolen` = whole instructions
//     (>= 5 bytes) that are position-independent, checked with a disassembler for this build and
//     spelled out exactly in the signature. The detour gets the same arguments as the original and
//     calls the original through `*original` (the trampoline).
//  3. RefSite: every client function or global the detour uses, found by signature (function start)
//     or from a RIP-relative operand inside a match. Never hard-code addresses.
//  4. Optional `prepare`: cross-checks on the resolved refs (e.g. a call next to a global must target
//     the function resolved separately). Returning an error skips the feature before anything is written.
//  5. Add the Feature entry (default off), a dinput8.ini key and a README row, then confirm
//     eqcore_check prints OK with the expected hook offset and ref addresses.
//  Two features may share one hook (same tables, detour and `original`; hud.cpp): the first one
//  applied installs it, the second finds the trampoline already set and only runs its prepare.

#include <cstddef>

struct PatchSite {
	const char* signature;   // hex bytes, "??" wildcard
	int expectedMatches;     // usually 1; a shared signature may legitimately match N times
	size_t offset;           // offset from match start to the bytes we change
	const char* original;    // hex bytes expected at offset
	const char* replacement; // hex bytes written at offset (same length as original)
};

// Inline hook on a function found by signature (the match is the function start). The first
// `stolen` bytes are copied to a trampoline (which then jumps back into the original) and replaced
// by a 5-byte JMP to `detour`. The stolen bytes must be whole instructions that don't depend on their
// own address (no RIP-relative operands, no relative jumps/calls), and the signature must spell them
// out exactly (no ?? within the first `stolen` bytes), so a different build is never hooked blindly.
struct HookSite {
	const char* signature;   // must match exactly once
	size_t stolen;           // 5..kMaxStolen bytes, ending on an instruction boundary
	void* detour;            // our replacement; same arguments and return as the original
	void** original;         // receives the trampoline: call it to run the original function
};

// Address of a client function or global, found inside a signature match (which must match exactly
// once). nextInstr == 0: the address is match + offset (e.g. a function start). Otherwise `offset`
// is a 4-byte RIP-relative displacement whose instruction ends at match + nextInstr.
struct RefSite {
	const char* signature;
	size_t offset;
	size_t nextInstr;
	void** target;           // receives the address (live process only)
};

const size_t kMaxStolen = 32;

struct Feature {
	const char* iniKey;      // [Features] key in dinput8.ini
	bool defaultOn;          // used when the key is absent
	const char* description;
	const PatchSite* sites;
	size_t siteCount;
	const HookSite* hooks;   // optional
	size_t hookCount;
	const RefSite* refs;     // optional; resolved before `prepare` and before anything is written
	size_t refCount;
	const char* (*prepare)(); // optional cross-checks on the resolved refs; returns an error or nullptr
};

const Feature* GetFeatures(size_t* count);
