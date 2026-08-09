// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <optional>
#include <string_view>

/// Returns a resource built into the binary, keyed by its path relative to the
/// resources directory ("shaders/vulkan/convert.glsl", "fonts/promptfont.otf").
/// The libretro core ships as a single file, so the copy on disk is whatever the
/// user assembled and can be from another version; the built-in copy always
/// matches the code. Empty when no such resource was embedded.
std::optional<std::string_view> GetEmbeddedResource(std::string_view name);

/// False when PCEE2_EXTERNAL_RESOURCES is set, for working on the files on disk.
bool EmbeddedResourcesPreferred();
