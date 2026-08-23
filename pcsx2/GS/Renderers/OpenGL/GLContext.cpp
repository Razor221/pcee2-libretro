// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLContext.h"
#include "GS/Renderers/OpenGL/GLLibretro.h"

#if defined(_WIN32)
#include "GS/Renderers/OpenGL/GLContextWGL.h"
#else // Linux
#include "GS/Renderers/OpenGL/GLContextEGL.h"
#ifdef X11_API
#include "GS/Renderers/OpenGL/GLContextEGLX11.h"
#endif
#ifdef WAYLAND_API
#include "GS/Renderers/OpenGL/GLContextEGLWayland.h"
#endif
#endif

#include "common/Console.h"
#include "common/Error.h"

#include "glad/gl.h"

#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

// Try GL ES before desktop GL. For a device whose desktop GL driver is the
// worse of the two -- and for testing the ES path on a machine that has both.
static bool ShouldPreferESContext()
{
	const char* value = std::getenv("PREFER_GLES_CONTEXT");
	return (value && std::strcmp(value, "1") == 0);
}

GLContext::GLContext(const WindowInfo& wi)
	: m_wi(wi)
{
}

GLContext::~GLContext() = default;

std::unique_ptr<GLContext> GLContext::Create(const WindowInfo& wi, Error* error)
{
	// Desktop GL 3.3 or better, then GL ES 3.x -- the device copes with either,
	// and on a mobile GPU the ES context is often the only one on offer.
	static constexpr Version vlist[] = {
		{Profile::Core, 4, 6},
		{Profile::Core, 4, 5},
		{Profile::Core, 4, 4},
		{Profile::Core, 4, 3},
		{Profile::Core, 4, 2},
		{Profile::Core, 4, 1},
		{Profile::Core, 4, 0},
		{Profile::Core, 3, 3},
		{Profile::ES, 3, 2},
		{Profile::ES, 3, 1},
	};

	std::vector<Version> reordered;
	std::span<const Version> versions = vlist;
	if (ShouldPreferESContext())
	{
		reordered.reserve(std::size(vlist));
		for (const Version& v : vlist)
		{
			if (v.profile == Profile::ES)
				reordered.push_back(v);
		}
		for (const Version& v : vlist)
		{
			if (v.profile != Profile::ES)
				reordered.push_back(v);
		}
		versions = reordered;
	}

	std::unique_ptr<GLContext> context;
	Error local_error;

	// Libretro: the frontend owns the only context that can reach the screen,
	// so the GS thread renders in one that shares its objects rather than one
	// of its own -- see GLLibretro. Failing here is not fatal to the core, the
	// caller falls back to the readback present path.
	if (GLLibretro::Active)
	{
		context = GLLibretro::CreateSharedContext(wi, versions, error);
	}
	else
	{
#if defined(_WIN32)
		context = GLContextWGL::Create(wi, versions, error);
#else // Linux
#if defined(X11_API)
		if (wi.type == WindowInfo::Type::X11)
			context = GLContextEGLX11::Create(wi, versions, error);
#endif

#if defined(WAYLAND_API)
		if (wi.type == WindowInfo::Type::Wayland)
			context = GLContextEGLWayland::Create(wi, versions, error);
#endif

		// headless/offscreen rendering (e.g. the libretro frontend): the base EGL
		// context supports surfaceless via EGL_MESA_platform_surfaceless or a
		// pbuffer fallback
		if (wi.type == WindowInfo::Type::Surfaceless)
			context = GLContextEGL::Create(wi, versions, error);
#endif
	}

	if (!context)
		return nullptr;

	// NOTE: Not thread-safe. But this is okay, since we're not going to be creating more than one context at a time.
	static GLContext* context_being_created;
	context_being_created = context.get();

	// load up glad -- from the ES entry points when that is what we got, since
	// the two share the header but not the loader
	const auto load = [](const char* name) {
		return reinterpret_cast<GLADapiproc>(context_being_created->GetProcAddress(name));
	};
	if (!(context->IsGLES() ? gladLoadGLES2(load) : gladLoadGL(load)))
	{
		Error::SetStringFmt(error, "Failed to load {} functions for GLAD", context->IsGLES() ? "GLES" : "GL");
		return nullptr;
	}

	context_being_created = nullptr;

	return context;
}
