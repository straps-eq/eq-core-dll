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
		return true;
	}
	return false;
}

} // namespace

bool FindTextSectionInImage(uint8_t* imageBase, CodeRegion* out) {
	return FindText(imageBase, SIZE_MAX, false, out);
}

bool FindTextSectionInFile(uint8_t* fileData, size_t fileSize, CodeRegion* out) {
	return FindText(fileData, fileSize, true, out);
}

bool ResolveFeature(const CodeRegion& text, const Feature& f, std::vector<SiteResult>* results, std::string* error) {
	results->clear();
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
		results->push_back(r);
	}
	return ok;
}

bool ApplyFeature(const Feature& f, const std::vector<SiteResult>& results, std::string* error) {
	for (size_t s = 0; s < f.siteCount; ++s) {
		std::vector<uint8_t> repl;
		ParseHex(f.sites[s].replacement, &repl, nullptr);
		for (uint8_t* at : results[s].patchAddrs) {
			DWORD old = 0;
			if (!VirtualProtect(at, repl.size(), PAGE_EXECUTE_READWRITE, &old)) {
				*error = "VirtualProtect failed (" + std::to_string(GetLastError()) + ")";
				return false;
			}
			memcpy(at, repl.data(), repl.size());
			VirtualProtect(at, repl.size(), old, &old);
			FlushInstructionCache(GetCurrentProcess(), at, repl.size());
		}
	}
	return true;
}
