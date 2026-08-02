// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// We require Windows 10+.
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0A00 // Windows 10

// mingw-w64 gates the Windows 10 1803 APIs the emulator uses - VirtualAlloc2(),
// UnmapViewOfFile2(), MEM_*_PLACEHOLDER, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
// - behind NTDDI_VERSION, which otherwise defaults to plain Windows 10 and
// hides them. The Windows SDK exposes them from _WIN32_WINNT alone, so MSVC
// never needed this.
#ifdef NTDDI_VERSION
#undef NTDDI_VERSION
#endif
#define NTDDI_VERSION 0x0A000005 // NTDDI_WIN10_RS4 (1803)

#ifdef __MINGW32__
// mingw's own headers declare their small helpers - GetCurrentFiber(),
// NtCurrentTeb(), InterlockedExchangeSubtract(), the Tp* and SH* families - as
// FORCEINLINE, which resolves to __forceinline, which Pcsx2Defs.h defines
// without the inline keyword on purpose (that is what __forceinline_odr is
// for). Every translation unit then emitted an external definition and the
// link failed with thousands of "multiple definition" errors. Windows headers
// get their own spelling back.
#ifdef FORCEINLINE
#undef FORCEINLINE
#endif
#define FORCEINLINE inline __attribute__((__always_inline__))
#endif

#include <windows.h>

// NTDDI_VERSION above is enough to get VirtualAlloc2() and UnmapViewOfFile2()
// declared, but mingw-w64's headers do not define these constants at all.
// Values from the Windows SDK (winnt.h and synchapi.h).
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#endif
