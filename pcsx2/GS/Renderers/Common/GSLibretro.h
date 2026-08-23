// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// State shared by every libretro present path, whichever graphics API is
// driving it. A libretro frontend never gives the core a window: the GS
// renders into a backbuffer this side sizes and hands over (a VkImage through
// set_image, a GL texture blitted into the frontend's FBO), so the parts of
// the present path that ask "is there a real window behind this?" need one
// answer that does not depend on which backend is loaded.

#pragma once

#include "common/Pcsx2Types.h"

namespace GSLibretro
{
	// True when a libretro frontend owns presentation. Set by the core before
	// the GS opens, cleared when the session ends.
	extern bool Active;

	// Largest output canvas the core will produce: 4x-upscaled PAL expanded to
	// 4:3. Advertised to the frontend as retro_game_geometry max_width/height
	// and enforced when the present path sizes the canvas to the merged frame.
	static constexpr u32 kMaxCanvasWidth = 2732;
	static constexpr u32 kMaxCanvasHeight = 2048;
} // namespace GSLibretro
