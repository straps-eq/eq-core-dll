// dinput8.dll proxy for the x64 EverQuest ToB client.
// Forwards DirectInput to the system dinput8.dll and applies opt-in client patches (see patches.cpp)
// when loaded into eqgame.exe. Features are chosen in dinput8.ini next to eqgame.exe.
#include <windows.h>
#include <unknwn.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include "patcher.h"

namespace {

HMODULE g_realDInput = nullptr;
char g_gameDir[MAX_PATH] = {};

void Log(const char* fmt, ...) {
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s\\dinput8.log", g_gameDir);
	FILE* f = nullptr;
	if (fopen_s(&f, path, "a") != 0 || !f) return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	va_list args;
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);
	fputc('\n', f);
	fclose(f);
}

FARPROC RealProc(const char* name) {
	if (!g_realDInput) {
		char path[MAX_PATH];
		GetSystemDirectoryA(path, MAX_PATH);
		strcat_s(path, "\\dinput8.dll");
		g_realDInput = LoadLibraryA(path);
		if (!g_realDInput) return nullptr;
	}
	return GetProcAddress(g_realDInput, name);
}

bool IsEqGame() {
	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	const char* name = strrchr(exe, '\\');
	name = name ? name + 1 : exe;
	return _stricmp(name, "eqgame.exe") == 0;
}

void ApplyPatches() {
	char ini[MAX_PATH];
	snprintf(ini, sizeof(ini), "%s\\dinput8.ini", g_gameDir);

	CodeRegion text = {};
	if (!FindTextSectionInImage(reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr)), &text)) {
		Log("eqgame.exe .text section not found; no patches applied");
		return;
	}

	size_t count = 0;
	const Feature* features = GetFeatures(&count);
	for (size_t i = 0; i < count; ++i) {
		const Feature& f = features[i];
		bool on = GetPrivateProfileIntA("Features", f.iniKey, f.defaultOn ? 1 : 0, ini) != 0;
		if (!on) continue;
		Resolved resolved;
		std::string error;
		if (!ResolveFeature(text, f, &resolved, &error)) {
			Log("%s: SKIPPED, %s", f.iniKey, error.c_str());
			continue;
		}
		if (!ApplyFeature(f, resolved, &error)) {
			Log("%s: FAILED while patching, %s", f.iniKey, error.c_str());
			continue;
		}
		Log("%s: applied (%s)", f.iniKey, f.description);
	}
}

} // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
	using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
	auto fn = reinterpret_cast<Fn>(RealProc("DirectInput8Create"));
	return fn ? fn(hinst, ver, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
	using Fn = HRESULT(WINAPI*)();
	auto fn = reinterpret_cast<Fn>(RealProc("DllCanUnloadNow"));
	return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
	using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
	auto fn = reinterpret_cast<Fn>(RealProc("DllGetClassObject"));
	return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
	using Fn = HRESULT(WINAPI*)();
	auto fn = reinterpret_cast<Fn>(RealProc("DllRegisterServer"));
	return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
	using Fn = HRESULT(WINAPI*)();
	auto fn = reinterpret_cast<Fn>(RealProc("DllUnregisterServer"));
	return fn ? fn() : E_FAIL;
}

const void* WINAPI Proxy_GetdfDIJoystick() {
	using Fn = const void*(WINAPI*)();
	auto fn = reinterpret_cast<Fn>(RealProc("GetdfDIJoystick"));
	return fn ? fn() : nullptr;
}

} // extern "C"

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(module);
		GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH);
		char* slash = strrchr(g_gameDir, '\\');
		if (slash) *slash = '\0';
		// Patching is plain memory writes plus VirtualAlloc for hook trampolines (no thread creation,
		// no library loads), which is safe under the loader lock. eqgame.exe is fully mapped because
		// it imports this DLL.
		if (IsEqGame()) ApplyPatches();
	}
	else if (reason == DLL_PROCESS_DETACH && g_realDInput) {
		FreeLibrary(g_realDInput);
	}
	return TRUE;
}
