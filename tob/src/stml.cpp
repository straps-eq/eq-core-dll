#include "stml.h"
#include <cstdint>
#include <cstring>

namespace {

// CStmlWnd
const size_t kStmlText = 0x260;        // CXStr: the string the window displays
// CXStr rep (FUN_1405853f0 writes +8 and +0x18; FUN_140585f90 switches on +0xc)
const size_t kRepLength = 0x08;        // int, characters
const size_t kRepEncoding = 0x0c;      // int: 0 = char, 1 = wchar_t
const size_t kRepChars = 0x18;

} // namespace

size_t StmlCurrentText(void* stml, char* out, size_t cap) {
	if (!out || !cap) return 0;
	out[0] = '\0';
	if (!stml) return 0;
	const uint8_t* rep = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(stml) + kStmlText);
	if (!rep) return 0;
	int n = *reinterpret_cast<const int*>(rep + kRepLength);
	int enc = *reinterpret_cast<const int*>(rep + kRepEncoding);
	if (n <= 0) return 0;
	size_t count = static_cast<size_t>(n);
	if (count > cap - 1) count = cap - 1;
	if (enc == 1) {
		const wchar_t* w = reinterpret_cast<const wchar_t*>(rep + kRepChars);
		for (size_t i = 0; i < count; ++i) out[i] = (w[i] > 0 && w[i] < 0x100) ? static_cast<char>(w[i]) : '?';
	}
	else {
		memcpy(out, rep + kRepChars, count);
	}
	out[count] = '\0';
	return strlen(out);   // an embedded terminator ends the text early
}

void StmlSetText(void* stml, const char* text, StmlCXStrCtorFn ctor, StmlSetTextFn setText) {
	if (!stml || !text || !ctor || !setText) return;
	void* cxstr = nullptr;
	ctor(&cxstr, text);
	if (!cxstr) return;
	setText(stml, &cxstr, 0, nullptr);   // consumes cxstr
}
