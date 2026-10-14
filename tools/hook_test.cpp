// Self-test for the inline hook: hooks a small function built in executable memory and calls it.
// Usage: hook_test.exe (exit code 0 = pass)
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "../src/patcher.h"

using Fn = int (*)();
static Fn g_original = nullptr;

static int Detour() {
	return g_original() + 1;
}

int main() {
	// mov [rsp+8],rbx; mov eax,42; ret
	static const uint8_t kCode[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };
	auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
	if (!code) return 2;
	memset(code, 0xCC, 0x1000);
	memcpy(code + 0x100, kCode, sizeof(kCode));
	FlushInstructionCache(GetCurrentProcess(), code, 0x1000);

	auto target = reinterpret_cast<Fn>(code + 0x100);
	if (target() != 42) return 3;

	const HookSite hooks[] = { { "48 89 5C 24 08 B8 2A 00 00 00 C3", 5, reinterpret_cast<void*>(&Detour),
	                             reinterpret_cast<void**>(&g_original) } };
	Feature f = {};
	f.iniKey = "HookTest";
	f.description = "self-test";
	f.hooks = hooks;
	f.hookCount = 1;

	CodeRegion text = { code, 0x1000, reinterpret_cast<uint64_t>(code) };
	Resolved r;
	std::string error;
	if (!ResolveFeature(text, f, &r, &error)) { printf("resolve failed: %s\n", error.c_str()); return 4; }
	if (!ApplyFeature(f, r, &error)) { printf("apply failed: %s\n", error.c_str()); return 5; }

	int v = target();
	printf("hooked call returned %d (expected 43)\n", v);
	return v == 43 ? 0 : 1;
}
