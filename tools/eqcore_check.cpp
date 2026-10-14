// Offline checker: resolves every feature against an eqgame.exe file without running or modifying it.
// Usage: eqcore_check.exe <path to eqgame.exe>
#include <cstdio>
#include <vector>
#include "../src/patcher.h"

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: %s <eqgame.exe>\n", argv[0]);
		return 2;
	}
	FILE* f = nullptr;
	if (fopen_s(&f, argv[1], "rb") != 0 || !f) {
		printf("cannot open %s\n", argv[1]);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> data(static_cast<size_t>(size));
	fread(data.data(), 1, data.size(), f);
	fclose(f);

	CodeRegion text = {};
	if (!FindTextSectionInFile(data.data(), data.size(), &text)) {
		printf("not an x64 PE with a .text section\n");
		return 2;
	}

	size_t count = 0;
	const Feature* features = GetFeatures(&count);
	int failed = 0;
	for (size_t i = 0; i < count; ++i) {
		std::vector<SiteResult> results;
		std::string error;
		bool ok = ResolveFeature(text, features[i], &results, &error);
		printf("%-18s %s", features[i].iniKey, ok ? "OK  " : "FAIL");
		if (ok) {
			for (const auto& r : results)
				for (uint8_t* at : r.patchAddrs)
					printf(" 0x%llx", static_cast<unsigned long long>(at - data.data()));
			printf("  (file offsets)\n");
		}
		else {
			printf("  %s\n", error.c_str());
			++failed;
		}
	}
	return failed ? 1 : 0;
}
