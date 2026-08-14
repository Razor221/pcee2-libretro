// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

namespace LibretroHost
{
	/// Asks the frontend for its virtual file system interface and, when it has
	/// one, makes common/FileSystem use it. Safe to call more than once; only
	/// the first call does anything.
	void InitializeVFS(retro_environment_t environ_cb);

	/// Reports what InitializeVFS() settled on. Separate because it runs before
	/// the frontend's log sink is hooked up, so the message would be lost.
	void LogVFSStatus();
} // namespace LibretroHost
