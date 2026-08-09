// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <optional>
#include <string_view>

/// Returns a shader built into the binary, keyed by its path relative to the
/// resources directory ("shaders/vulkan/convert.glsl"). The libretro core ships
/// as a single file, so the copy on disk is whatever the user assembled and can
/// be from another version; the built-in copy always matches the code. Empty
/// when no such shader was embedded.
std::optional<std::string_view> GSGetEmbeddedShader(std::string_view name);
