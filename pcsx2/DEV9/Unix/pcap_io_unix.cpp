// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Unix builds resolve libpcap at runtime rather than linking against it, the
// same way the Windows build handles wpcap.dll. Linking it directly would put
// a DT_NEEDED on whatever SONAME the build machine happens to have
// (libpcap.so.0.8 on Debian/Ubuntu, libpcap.so.1 on Arch and friends), and the
// whole core then fails to load on any system carrying the other one - even
// though DEV9 ethernet is off by default. Missing libpcap now just means no
// PCAP adapter.

#include "DEV9/pcap_io.h"

#include "common/Console.h"
#include "common/DynamicLibrary.h"

static DynamicLibrary s_pcap_library;

#define FUNCTION_SHIM_HEAD_ARGS(retType, name, ...) \
	typedef retType (*fp_##name##_t)(__VA_ARGS__);  \
	static fp_##name##_t fp_##name;                 \
	retType name(__VA_ARGS__)

#define FUNCTION_SHIM_BODY_ARGS(retType, name, ...) \
	{                                               \
		return fp_##name(__VA_ARGS__);              \
	}

#define FUNCTION_SHIM_1_ARG(retType, name, type1)    \
	FUNCTION_SHIM_HEAD_ARGS(retType, name, type1 a1) \
	FUNCTION_SHIM_BODY_ARGS(retType, name, a1)

#define FUNCTION_SHIM_2_ARG(retType, name, type1, type2)       \
	FUNCTION_SHIM_HEAD_ARGS(retType, name, type1 a1, type2 a2) \
	FUNCTION_SHIM_BODY_ARGS(retType, name, a1, a2)

#define FUNCTION_SHIM_3_ARG(retType, name, type1, type2, type3)          \
	FUNCTION_SHIM_HEAD_ARGS(retType, name, type1 a1, type2 a2, type3 a3) \
	FUNCTION_SHIM_BODY_ARGS(retType, name, a1, a2, a3)

#define FUNCTION_SHIM_5_ARG(retType, name, type1, type2, type3, type4, type5)                \
	FUNCTION_SHIM_HEAD_ARGS(retType, name, type1 a1, type2 a2, type3 a3, type4 a4, type5 a5) \
	FUNCTION_SHIM_BODY_ARGS(retType, name, a1, a2, a3, a4, a5)

#include "pcap_io_unix_funcs.h"

bool load_pcap()
{
	if (s_pcap_library.IsOpen())
		return true;

	// Distributions disagree on the SONAME, and the unversioned symlink only
	// exists when the -dev/-devel package is installed, so try them all.
	static constexpr const char* libraries[] = {
#ifdef __APPLE__
		"libpcap.A.dylib",
		"libpcap.dylib",
#else
		"libpcap.so.1",
		"libpcap.so.0.8",
		"libpcap.so",
#endif
	};

	for (const char* library : libraries)
	{
		if (s_pcap_library.Open(library, nullptr))
			break;
	}

	if (!s_pcap_library.IsOpen())
	{
		Console.Error("DEV9: libpcap not found, PCAP adapters are unavailable");
		return false;
	}

#define FUNCTION_SHIM_ANY_ARG(retType, name, ...)                 \
	if (!s_pcap_library.GetSymbol(#name, &fp_##name))             \
	{                                                             \
		Console.Error("DEV9: %s not found in libpcap", #name);    \
		s_pcap_library.Close();                                   \
		return false;                                             \
	}

#include "pcap_io_unix_funcs.h"

	return true;
}

void unload_pcap()
{
	s_pcap_library.Close();
}
