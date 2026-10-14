#pragma once
// Patch table for the ToB client (eqgame.exe, build "Sep 11 2025 11:54:10", x64).
// Each site is located by a byte signature (?? = wildcard) inside eqgame.exe's .text section.
// A feature is applied only if every one of its signatures matches exactly the expected number of
// times AND the original bytes at each patch offset are what we expect. Otherwise the feature is
// skipped and the reason is logged, so a different client build never gets patched blindly.
//
// Function names (FUN_...) are the Ghidra auto-names for this build.

#include <cstddef>

struct PatchSite {
	const char* signature;   // hex bytes, "??" wildcard
	int expectedMatches;     // usually 1; a shared signature may legitimately match N times
	size_t offset;           // offset from match start to the bytes we change
	const char* original;    // hex bytes expected at offset
	const char* replacement; // hex bytes written at offset (same length as original)
};

struct Feature {
	const char* iniKey;      // [Features] key in eqcore.ini
	bool defaultOn;          // used when the key is absent
	const char* description;
	const PatchSite* sites;
	size_t siteCount;
};

const Feature* GetFeatures(size_t* count);
