#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "patches.h"

struct CodeRegion {
	uint8_t* data;   // start of .text as seen by this process (live image, or a file buffer)
	size_t size;
};

struct SiteResult {
	std::vector<uint8_t*> patchAddrs;
	std::string error;   // empty when the site resolved
};

// Finds the .text section of a PE image already laid out in memory (a loaded module).
bool FindTextSectionInImage(uint8_t* imageBase, CodeRegion* out);
// Finds the .text section in a raw PE file buffer (for the offline checker).
bool FindTextSectionInFile(uint8_t* fileData, size_t fileSize, CodeRegion* out);

// Resolves every site of a feature. Returns true only if all sites match their expected count and
// the original bytes at each patch location are as expected. Nothing is written.
bool ResolveFeature(const CodeRegion& text, const Feature& f, std::vector<SiteResult>* results, std::string* error);

// Writes the replacement bytes for an already-resolved feature (live process only).
bool ApplyFeature(const Feature& f, const std::vector<SiteResult>& results, std::string* error);
