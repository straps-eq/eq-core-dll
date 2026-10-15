#pragma once
// Adding text to a CStmlWnd without losing what the client put there.
//
// CStmlWnd::SetSTMLText (FUN_1405ca040(stml, CXStr*, flag, fmt)) always makes the window display the one
// string it is given: both flag values end with `stml+0x2e0 = 1` (re-parse) and `stml+0x260 = str`, and the
// re-parse (FUN_1405c0170 -> FUN_1405c3560) reads only +0x260. The flag selects how the string is recorded
// in the window's run table (+0x320/+0x330: flag 0 overwrites the current entry, 1 adds one), which the
// display does not use. So "append" mode replaces the visible text, which is how the first version of the
// item and spell windows lost the client's own lines. The client itself never appends: it concatenates
// CXStrs and sets the result once.
//
// Usage: StmlCurrentText copies what the window shows (+0x260; stored wide, encoding 1 at rep+0xc, after
// SetSTMLText converted it) into a buffer, the caller adds its lines after it, StmlSetText sets the whole
// text again with flag 0.
#include <cstddef>

using StmlCXStrCtorFn = void* (*)(void** out, const char* text);                    // FUN_1405853f0
using StmlSetTextFn = void (*)(void* stml, void** text, char flag, void* fmt);     // FUN_1405ca040

// The window's current text, narrowed; returns the length written (always < cap, terminated).
size_t StmlCurrentText(void* stml, char* out, size_t cap);
// Makes `text` the window's text (the client's constructor builds the CXStr, SetSTMLText consumes it).
void StmlSetText(void* stml, const char* text, StmlCXStrCtorFn ctor, StmlSetTextFn setText);
// Removes trailing line breaks and blanks from `text` (the client's text usually ends in a few <BR>s),
// so that one "<BR>" added after it is exactly one blank line. Returns the new length.
size_t StmlTrimBreaks(char* text, size_t len);
// Whether `text` already contains the start of `needle` (its first 47 characters: enough to recognise a
// spell description the client printed itself).
bool StmlHasText(const char* text, const char* needle);
// The parser collapses runs of spaces; "&...;" entities (FUN_1405c3560, '&' branch) go through its entity
// table, and "&NBSP;" is what the client itself writes between columns ("&NBSP;&NBSP;&NBSP;master=%d").
const char* const kStmlGap = "&NBSP;&NBSP;&NBSP;";
