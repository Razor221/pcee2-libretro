// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLContextGLX.h"

#include "common/Console.h"
#include "common/DynamicLibrary.h"
#include "common/Error.h"

#include <mutex>

// GLX types, spelled out so this doesn't need GL/glx.h (which only ships with
// the desktop GL dev packages, and this file has to build wherever X11 does).
// Every one of these is a pointer or an XID, so void*/unsigned long suffice.
using GLXDisplay = void;
using GLXContextHandle = void*;
using GLXFBConfigHandle = void*;
using GLXDrawableHandle = unsigned long;

namespace
{
	constexpr int GLX_NONE_ATTRIB = 0; // X11's None
	constexpr int GLX_SCREEN_ATTRIB = 0x800C;
	constexpr int GLX_FBCONFIG_ID_ATTRIB = 0x8013;
	constexpr int GLX_RGBA_TYPE_ATTRIB = 0x8014;
	constexpr int GLX_PBUFFER_HEIGHT_ATTRIB = 0x8040;
	constexpr int GLX_PBUFFER_WIDTH_ATTRIB = 0x8041;
	constexpr int GLX_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
	constexpr int GLX_CONTEXT_MINOR_VERSION_ARB = 0x2092;
	constexpr int GLX_CONTEXT_PROFILE_MASK_ARB = 0x9126;
	constexpr int GLX_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;

	struct GLXFunctions
	{
		GLXDisplay* (*GetCurrentDisplay)() = nullptr;
		GLXContextHandle (*GetCurrentContext)() = nullptr;
		int (*QueryContext)(GLXDisplay*, GLXContextHandle, int, int*) = nullptr;
		GLXFBConfigHandle* (*ChooseFBConfig)(GLXDisplay*, int, const int*, int*) = nullptr;
		GLXContextHandle (*CreateNewContext)(GLXDisplay*, GLXFBConfigHandle, int, GLXContextHandle, int) = nullptr;
		GLXDrawableHandle (*CreatePbuffer)(GLXDisplay*, GLXFBConfigHandle, const int*) = nullptr;
		void (*DestroyPbuffer)(GLXDisplay*, GLXDrawableHandle) = nullptr;
		int (*MakeContextCurrent)(GLXDisplay*, GLXDrawableHandle, GLXDrawableHandle, GLXContextHandle) = nullptr;
		void (*DestroyContext)(GLXDisplay*, GLXContextHandle) = nullptr;
		void (*SwapBuffers)(GLXDisplay*, GLXDrawableHandle) = nullptr;
		void* (*GetProcAddress)(const unsigned char*) = nullptr;
		void (*XFree)(void*) = nullptr;
	};

	GLXFunctions s_glx;
	DynamicLibrary s_gl_library;
	DynamicLibrary s_x11_library;
	bool s_loaded = false;
	std::once_flag s_load_once;

	void LoadGLX()
	{
		std::string gl_libname = DynamicLibrary::GetVersionedFilename("libGL", 1);
		Error error;
		if (!s_gl_library.Open(gl_libname.c_str(), &error))
		{
			gl_libname = DynamicLibrary::GetVersionedFilename("libGL");
			if (!s_gl_library.Open(gl_libname.c_str(), &error))
			{
				Console.WarningFmt("GLX: Failed to load libGL: {}", error.GetDescription());
				return;
			}
		}

		// XFree() releases what glXChooseFBConfig() hands back. Not fatal if
		// it's missing -- a leaked FBConfig array is a few dozen bytes, once.
		const std::string x11_libname = DynamicLibrary::GetVersionedFilename("libX11", 6);
		if (s_x11_library.Open(x11_libname.c_str(), nullptr))
			s_x11_library.GetSymbol("XFree", &s_glx.XFree);

		const bool ok =
			s_gl_library.GetSymbol("glXGetCurrentDisplay", &s_glx.GetCurrentDisplay) &&
			s_gl_library.GetSymbol("glXGetCurrentContext", &s_glx.GetCurrentContext) &&
			s_gl_library.GetSymbol("glXQueryContext", &s_glx.QueryContext) &&
			s_gl_library.GetSymbol("glXChooseFBConfig", &s_glx.ChooseFBConfig) &&
			s_gl_library.GetSymbol("glXCreateNewContext", &s_glx.CreateNewContext) &&
			s_gl_library.GetSymbol("glXCreatePbuffer", &s_glx.CreatePbuffer) &&
			s_gl_library.GetSymbol("glXDestroyPbuffer", &s_glx.DestroyPbuffer) &&
			s_gl_library.GetSymbol("glXMakeContextCurrent", &s_glx.MakeContextCurrent) &&
			s_gl_library.GetSymbol("glXDestroyContext", &s_glx.DestroyContext) &&
			s_gl_library.GetSymbol("glXSwapBuffers", &s_glx.SwapBuffers) &&
			s_gl_library.GetSymbol("glXGetProcAddress", &s_glx.GetProcAddress);
		if (!ok)
		{
			Console.Warning("GLX: libGL is missing GLX 1.3 entry points.");
			s_gl_library.Close();
			return;
		}

		s_loaded = true;
	}

	bool EnsureGLX()
	{
		std::call_once(s_load_once, LoadGLX);
		return s_loaded;
	}

	// The FBConfig a context was created against, which every drawable it is
	// made current on has to be compatible with.
	GLXFBConfigHandle GetContextFBConfig(GLXDisplay* display, GLXContextHandle context)
	{
		int fb_config_id = 0;
		int screen = 0;
		if (s_glx.QueryContext(display, context, GLX_FBCONFIG_ID_ATTRIB, &fb_config_id) != 0 ||
			s_glx.QueryContext(display, context, GLX_SCREEN_ATTRIB, &screen) != 0)
		{
			return nullptr;
		}

		const int attribs[] = {GLX_FBCONFIG_ID_ATTRIB, fb_config_id, GLX_NONE_ATTRIB};
		int num_configs = 0;
		GLXFBConfigHandle* configs = s_glx.ChooseFBConfig(display, screen, attribs, &num_configs);
		if (!configs)
			return nullptr;

		GLXFBConfigHandle config = (num_configs > 0) ? configs[0] : nullptr;
		if (s_glx.XFree)
			s_glx.XFree(configs);
		return config;
	}
} // namespace

GLContextGLX::GLContextGLX(const WindowInfo& wi)
	: GLContext(wi)
{
}

GLContextGLX::~GLContextGLX()
{
	Destroy();
}

bool GLContextGLX::CaptureCurrentContext(void** display, void** context)
{
	if (!EnsureGLX())
		return false;

	GLXDisplay* const dpy = s_glx.GetCurrentDisplay();
	GLXContextHandle const ctx = s_glx.GetCurrentContext();
	if (!dpy || !ctx)
		return false;

	*display = dpy;
	*context = ctx;
	return true;
}

std::unique_ptr<GLContext> GLContextGLX::CreateShared(const WindowInfo& wi, void* display, void* share_context,
	std::span<const Version> versions_to_try, Error* error)
{
	if (!EnsureGLX())
	{
		Error::SetStringView(error, "GLX is not available");
		return nullptr;
	}

	GLXFBConfigHandle fb_config = GetContextFBConfig(display, share_context);
	if (!fb_config)
	{
		Error::SetStringView(error, "glXQueryContext() could not report the frontend context's FBConfig");
		return nullptr;
	}

	std::unique_ptr<GLContextGLX> context = std::make_unique<GLContextGLX>(wi);
	context->m_display = display;
	context->m_fb_config = fb_config;

	// Made current on the calling thread, which is the GS thread that renders
	// through it -- and the thread GLAD is loaded on, right after this returns.
	for (const Version& cv : versions_to_try)
	{
		if (!context->CreateContext(cv, fb_config, share_context, error))
			continue;

		if (context->MakeCurrent())
			return context;

		context->Destroy();
	}

	Error::SetStringView(error, "Failed to create any shared GLX context version");
	return nullptr;
}

bool GLContextGLX::CreateContext(const Version& version, void* fb_config, void* share_context, Error* error)
{
	// Prefer an explicitly versioned core context; glXCreateNewContext() only
	// ever gives back a compatibility one, whose version is the driver's
	// choice, and PCSX2 wants 3.3 core or better.
	using PFNCreateContextAttribs = GLXContextHandle (*)(GLXDisplay*, GLXFBConfigHandle, GLXContextHandle, int, const int*);
	const auto create_context_attribs = reinterpret_cast<PFNCreateContextAttribs>(
		s_glx.GetProcAddress(reinterpret_cast<const unsigned char*>("glXCreateContextAttribsARB")));

	if (create_context_attribs)
	{
		const int attribs[] = {
			GLX_CONTEXT_MAJOR_VERSION_ARB, version.major_version,
			GLX_CONTEXT_MINOR_VERSION_ARB, version.minor_version,
			GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
			GLX_NONE_ATTRIB};
		m_context = create_context_attribs(m_display, fb_config, share_context, 1, attribs);
	}
	else if (version.major_version == 3 && version.minor_version == 3)
	{
		// No ARB_create_context: one attempt at whatever the driver defaults
		// to, on the last version in the list rather than once per entry.
		m_context = s_glx.CreateNewContext(m_display, fb_config, GLX_RGBA_TYPE_ATTRIB, share_context, 1);
	}

	if (!m_context)
		return false;

	// GLX has no surfaceless contexts: rendering happens entirely in FBOs, but
	// something has to be current, so a 1x1 pbuffer stands in for the window.
	const int pbuffer_attribs[] = {
		GLX_PBUFFER_WIDTH_ATTRIB, 1, GLX_PBUFFER_HEIGHT_ATTRIB, 1, GLX_NONE_ATTRIB};
	m_pbuffer = s_glx.CreatePbuffer(m_display, fb_config, pbuffer_attribs);
	if (!m_pbuffer)
	{
		// Some FBConfigs (plain window ones) can't back a pbuffer. Mesa and
		// NVIDIA both accept a None drawable for an ARB-created context, so
		// try that before giving up.
		if (!s_glx.MakeContextCurrent(m_display, 0, 0, m_context))
		{
			Error::SetStringView(error, "Neither a pbuffer nor a drawable-less context could be made current");
			s_glx.DestroyContext(m_display, m_context);
			m_context = nullptr;
			return false;
		}
		s_glx.MakeContextCurrent(m_display, 0, 0, nullptr);
	}

	Console.WriteLnFmt("GLX: Created a shared {}.{} context for the GS thread.",
		version.major_version, version.minor_version);
	m_version = version;
	return true;
}

void GLContextGLX::Destroy()
{
	if (!s_loaded || m_abandoned)
	{
		m_pbuffer = 0;
		m_context = nullptr;
		return;
	}

	if (m_context && s_glx.GetCurrentContext() == m_context)
		s_glx.MakeContextCurrent(m_display, 0, 0, nullptr);

	if (m_pbuffer)
	{
		s_glx.DestroyPbuffer(m_display, m_pbuffer);
		m_pbuffer = 0;
	}

	if (m_context)
	{
		s_glx.DestroyContext(m_display, m_context);
		m_context = nullptr;
	}
}

void* GLContextGLX::GetProcAddress(const char* name)
{
	return s_glx.GetProcAddress(reinterpret_cast<const unsigned char*>(name));
}

bool GLContextGLX::ChangeSurface(const WindowInfo& new_wi)
{
	// Surfaceless by construction; there is no window to move to.
	m_wi = new_wi;
	return true;
}

void GLContextGLX::ResizeSurface(u32 new_surface_width, u32 new_surface_height)
{
	m_wi.surface_width = new_surface_width;
	m_wi.surface_height = new_surface_height;
}

bool GLContextGLX::SwapBuffers()
{
	if (m_abandoned || !m_pbuffer)
		return false;

	s_glx.SwapBuffers(m_display, m_pbuffer);
	return true;
}

bool GLContextGLX::IsCurrent()
{
	return m_context && s_glx.GetCurrentContext() == m_context;
}

bool GLContextGLX::MakeCurrent()
{
	if (m_abandoned)
		return false;

	if (!s_glx.MakeContextCurrent(m_display, m_pbuffer, m_pbuffer, m_context))
	{
		Console.Error("GLX: glXMakeContextCurrent() failed");
		return false;
	}

	return true;
}

bool GLContextGLX::ReleaseThread()
{
	// GLX has no equivalent of eglTerminate, so the frontend dropping its own
	// context leaves the display -- and this unbind -- perfectly valid.
	if (!s_loaded)
		return false;

	s_glx.MakeContextCurrent(m_display, 0, 0, nullptr);
	return true;
}

bool GLContextGLX::DoneCurrent()
{
	if (m_abandoned)
		return true;

	return s_glx.MakeContextCurrent(m_display, 0, 0, nullptr) != 0;
}

bool GLContextGLX::SupportsNegativeSwapInterval() const
{
	return false;
}

bool GLContextGLX::SetSwapInterval(s32 interval)
{
	// Nothing is presented through this context; the frontend paces us.
	return true;
}

std::unique_ptr<GLContext> GLContextGLX::CreateSharedContext(const WindowInfo& wi, Error* error)
{
	// GLX is the desktop-only path; a GLES frontend on this platform is on EGL.
	static constexpr Version vlist[] = {{Profile::Core, 4, 6}, {Profile::Core, 4, 5}, {Profile::Core, 4, 4},
		{Profile::Core, 4, 3}, {Profile::Core, 4, 2}, {Profile::Core, 4, 1}, {Profile::Core, 4, 0},
		{Profile::Core, 3, 3}};
	return CreateShared(wi, m_display, m_context, vlist, error);
}
