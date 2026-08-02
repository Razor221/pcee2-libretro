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

#include <windows.h>

#ifdef __MINGW32__
// VirtualAlloc2(), MapViewOfFile3() and UnmapViewOfFile2() are exported by
// kernelbase.dll and reached through OneCore.lib in the Windows SDK; MXE's
// mingw-w64 ships no import library that carries them, so they are resolved at
// runtime (see common/Windows/WinHostSys.cpp). Every caller goes through these
// names instead - GS.cpp uses them as well as the memory mapping code.
extern "C" {
PVOID WINAPI pcsx2_VirtualAlloc2(HANDLE Process, PVOID BaseAddress, SIZE_T Size, ULONG AllocationType,
	ULONG PageProtection, MEM_EXTENDED_PARAMETER* ExtendedParameters, ULONG ParameterCount);
PVOID WINAPI pcsx2_MapViewOfFile3(HANDLE FileMapping, HANDLE Process, PVOID BaseAddress, ULONG64 Offset,
	SIZE_T ViewSize, ULONG AllocationType, ULONG PageProtection, MEM_EXTENDED_PARAMETER* ExtendedParameters,
	ULONG ParameterCount);
BOOL WINAPI pcsx2_UnmapViewOfFile2(HANDLE Process, PVOID BaseAddress, ULONG UnmapFlags);
}
#define VirtualAlloc2 pcsx2_VirtualAlloc2
#define MapViewOfFile3 pcsx2_MapViewOfFile3
#define UnmapViewOfFile2 pcsx2_UnmapViewOfFile2
#endif

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
