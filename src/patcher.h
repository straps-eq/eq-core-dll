#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "patches.h"

struct CodeRegion {
	uint8_t* data;   // start of .text as seen by this process (live image, or a file buffer)
	size_t size;
	uint64_t va;     // virtual address of .text in the image (live: equals data)
};

struct SiteResult {
	std::vector<uint8_t*> patchAddrs;
	std::string error;   // empty when the site resolved
};

struct Resolved {
	std::vector<SiteResult> sites;    // one per PatchSite
	std::vector<uint8_t*> hookAddrs;  // one per HookSite (function start)
	std::vector<uint64_t> refAddrs;   // one per RefSite (live: the address; file: the image VA)
};

// Finds the .text section of a PE image already laid out in memory (a loaded module).
bool FindTextSectionInImage(uint8_t* imageBase, CodeRegion* out);
// Finds the .text section in a raw PE file buffer (for the offline checker).
bool FindTextSectionInFile(uint8_t* fileData, size_t fileSize, CodeRegion* out);

// Resolves every patch, hook and ref site of a feature. Returns true only if all signatures match
// their expected count, the original bytes at each patch location are as expected, and every hook's
// stolen bytes are spelled out by its signature. Nothing is written.
bool ResolveFeature(const CodeRegion& text, const Feature& f, Resolved* out, std::string* error);

// Live process only: stores the refs, runs the feature's prepare check, installs the hooks and
// writes the patches. Nothing is written if the refs or the prepare check fail.
bool ApplyFeature(const Feature& f, const Resolved& r, std::string* error);
