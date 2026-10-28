// dinput8.dll proxy for the x64 EverQuest ToB client.
// Forwards DirectInput to the system dinput8.dll and applies opt-in client patches (see patches.cpp)
// when loaded into eqgame.exe. Features are chosen in dinput8.ini next to eqgame.exe.
#include <windows.h>
#include <unknwn.h>
#include <intrin.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
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

// Describes an address as module+offset, e.g. "eqgame.exe+0x455a60".
void DescribeAddress(DWORD64 addr, char* out, size_t size) {
	HMODULE mod = nullptr;
	char name[MAX_PATH] = "?";
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
		char path[MAX_PATH];
		GetModuleFileNameA(mod, path, MAX_PATH);
		const char* slash = strrchr(path, '\\');
		strcpy_s(name, slash ? slash + 1 : path);
		snprintf(out, size, "%s+0x%llx", name, static_cast<unsigned long long>(addr - reinterpret_cast<DWORD64>(mod)));
	}
	else {
		snprintf(out, size, "0x%llx", static_cast<unsigned long long>(addr));
	}
}

// Logs the first few access violations (module+offset and an unwound call stack) so a crash in the
// field can be traced to a feature without a dump. First-chance: some of these may be handled.
LONG CALLBACK CrashLogHandler(PEXCEPTION_POINTERS info) {
	static volatile LONG logged = 0;
	DWORD code = info->ExceptionRecord->ExceptionCode;
	if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
	if (InterlockedIncrement(&logged) > 5) return EXCEPTION_CONTINUE_SEARCH;
	char where[MAX_PATH + 32];
	DescribeAddress(info->ContextRecord->Rip, where, sizeof(where));
	ULONG_PTR rw = info->ExceptionRecord->NumberParameters >= 2 ? info->ExceptionRecord->ExceptionInformation[0] : 0;
	ULONG_PTR target = info->ExceptionRecord->NumberParameters >= 2 ? info->ExceptionRecord->ExceptionInformation[1] : 0;
	Log("EXCEPTION 0x%08lx at %s (%s 0x%llx) rcx=0x%llx rdx=0x%llx", code, where, rw == 1 ? "write" : rw == 8 ? "exec" : "read",
		static_cast<unsigned long long>(target), info->ContextRecord->Rcx, info->ContextRecord->Rdx);
	CONTEXT ctx = *info->ContextRecord;
	for (int frame = 0; frame < 12 && ctx.Rip; ++frame) {
		DWORD64 imageBase = 0;
		PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
		if (!fn) {
			ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);   // leaf function
			ctx.Rsp += 8;
		}
		else {
			void* handlerData = nullptr;
			DWORD64 establisher = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
		}
		if (!ctx.Rip) break;
		DescribeAddress(ctx.Rip, where, sizeof(where));
		Log("  called from %s", where);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// The joystick data format (DIJOYSTATE), built from its documented layout so GetdfDIJoystick never
// has to load or enter the system DLL. Callers other than eqgame.exe (e.g. GameInput) ask for it on
// their own threads. Field for field the same as the system DLL's c_dfDIJoystick.
constexpr DWORD kOptional = 0x80000000;   // DIDFT_OPTIONAL: documented for DIOBJECTDATAFORMAT, absent from the SDK header
constexpr DWORD kAxis = kOptional | DIDFT_AXIS | DIDFT_ANYINSTANCE;
constexpr DWORD kPov = kOptional | DIDFT_POV | DIDFT_ANYINSTANCE;
constexpr DWORD kButton = kOptional | DIDFT_BUTTON | DIDFT_ANYINSTANCE;

#define JOY_BUTTON(n) { nullptr, DIJOFS_BUTTON(n), kButton, 0 }
DIOBJECTDATAFORMAT g_joystickObjects[] = {
	{ &GUID_XAxis, DIJOFS_X, kAxis, DIDOI_ASPECTPOSITION },
	{ &GUID_YAxis, DIJOFS_Y, kAxis, DIDOI_ASPECTPOSITION },
	{ &GUID_ZAxis, DIJOFS_Z, kAxis, DIDOI_ASPECTPOSITION },
	{ &GUID_RxAxis, DIJOFS_RX, kAxis, DIDOI_ASPECTPOSITION },
	{ &GUID_RyAxis, DIJOFS_RY, kAxis, DIDOI_ASPECTPOSITION },
	{ &GUID_RzAxis, DIJOFS_RZ, kAxis, DIDOI_ASPECTPOSITION },
	{ &GUID_Slider, DIJOFS_SLIDER(0), kAxis, DIDOI_ASPECTPOSITION },
	{ &GUID_Slider, DIJOFS_SLIDER(1), kAxis, DIDOI_ASPECTPOSITION },
	{ &GUID_POV, DIJOFS_POV(0), kPov, 0 },
	{ &GUID_POV, DIJOFS_POV(1), kPov, 0 },
	{ &GUID_POV, DIJOFS_POV(2), kPov, 0 },
	{ &GUID_POV, DIJOFS_POV(3), kPov, 0 },
	JOY_BUTTON(0), JOY_BUTTON(1), JOY_BUTTON(2), JOY_BUTTON(3), JOY_BUTTON(4), JOY_BUTTON(5), JOY_BUTTON(6), JOY_BUTTON(7),
	JOY_BUTTON(8), JOY_BUTTON(9), JOY_BUTTON(10), JOY_BUTTON(11), JOY_BUTTON(12), JOY_BUTTON(13), JOY_BUTTON(14), JOY_BUTTON(15),
	JOY_BUTTON(16), JOY_BUTTON(17), JOY_BUTTON(18), JOY_BUTTON(19), JOY_BUTTON(20), JOY_BUTTON(21), JOY_BUTTON(22), JOY_BUTTON(23),
	JOY_BUTTON(24), JOY_BUTTON(25), JOY_BUTTON(26), JOY_BUTTON(27), JOY_BUTTON(28), JOY_BUTTON(29), JOY_BUTTON(30), JOY_BUTTON(31),
};
#undef JOY_BUTTON

const DIDATAFORMAT g_joystickFormat = {
	sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, sizeof(DIJOYSTATE),
	ARRAYSIZE(g_joystickObjects), g_joystickObjects,
};

// Logs who called an export (module+offset of the return address and the thread), once per export.
void LogCaller(const char* exportName, void* returnAddress, volatile LONG* logged) {
	if (InterlockedExchange(logged, 1)) return;
	char where[MAX_PATH + 32];
	DescribeAddress(reinterpret_cast<DWORD64>(returnAddress), where, sizeof(where));
	Log("%s first called from %s on thread %lu", exportName, where, GetCurrentThreadId());
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

	// Resolve every enabled feature against the untouched image first, then apply. Applying as we go
	// would let one feature's hook bytes break another feature's signature (shared hooks, overlapping refs).
	size_t count = 0;
	const Feature* features = GetFeatures(&count);
	std::vector<Resolved> resolved(count);
	std::vector<bool> ready(count, false);
	for (size_t i = 0; i < count; ++i) {
		const Feature& f = features[i];
		bool on = GetPrivateProfileIntA("Features", f.iniKey, f.defaultOn ? 1 : 0, ini) != 0;
		if (!on) continue;
		std::string error;
		if (!ResolveFeature(text, f, &resolved[i], &error)) {
			Log("%s: SKIPPED, %s", f.iniKey, error.c_str());
			continue;
		}
		ready[i] = true;
	}
	for (size_t i = 0; i < count; ++i) {
		if (!ready[i]) continue;
		const Feature& f = features[i];
		std::string error;
		if (!ApplyFeature(f, resolved[i], &error)) {
			Log("%s: FAILED while patching, %s", f.iniKey, error.c_str());
			continue;
		}
		Log("%s: applied (%s)", f.iniKey, f.description);
	}
}

} // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
	static volatile LONG logged = 0;
	static thread_local int depth = 0;
	LogCaller("DirectInput8Create", _ReturnAddress(), &logged);
	if (depth > 0) Log("DirectInput8Create re-entered on thread %lu (depth %d)", GetCurrentThreadId(), depth);
	using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
	auto fn = reinterpret_cast<Fn>(RealProc("DirectInput8Create"));
	if (!fn) return E_FAIL;
	++depth;
	HRESULT hr = fn(hinst, ver, riid, out, outer);
	--depth;
	return hr;
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
	static volatile LONG logged = 0;
	LogCaller("GetdfDIJoystick", _ReturnAddress(), &logged);
	return &g_joystickFormat;
}

} // extern "C"

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(module);
		GetModuleFileNameA(nullptr, g_gameDir, MAX_PATH);
		char* slash = strrchr(g_gameDir, '\\');
		if (slash) *slash = '\0';
		// Patching is plain memory writes plus VirtualAlloc for hook trampolines (no thread creation,
		// no library loads), which is safe under the loader lock. eqgame.exe is fully mapped because
		// it imports this DLL.
		if (IsEqGame()) {
			// Hooks jump into this DLL for the life of the process, so it must never be unloaded,
			// even if the game frees its DirectInput library.
			HMODULE pinned = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			                   reinterpret_cast<LPCSTR>(&DllMain), &pinned);
			Log("dinput8.dll loaded at 0x%llx", reinterpret_cast<unsigned long long>(module));
			char ini[MAX_PATH];
			snprintf(ini, sizeof(ini), "%s\\dinput8.ini", g_gameDir);
			if (GetPrivateProfileIntA("Debug", "CrashLog", 1, ini)) AddVectoredExceptionHandler(1, CrashLogHandler);
			ApplyPatches();
		}
	}
	else if (reason == DLL_PROCESS_DETACH) {
		if (!reserved) Log("dinput8.dll is being unloaded by FreeLibrary");
		if (g_realDInput && !reserved) FreeLibrary(g_realDInput);
	}
	return TRUE;
}
