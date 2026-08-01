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

#endif
