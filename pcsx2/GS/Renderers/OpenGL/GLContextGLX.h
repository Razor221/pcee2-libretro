// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// A GLX context that shares another context's objects.
//
// PCSX2 itself only ever creates contexts through EGL, so there is no general
// GLX backend here and this is not registered with GLContext::Create(). It
// exists for one case: a libretro frontend whose GL context is a GLX one --
// which is what RetroArch gives you on X11 -- where the core has to create its
// GS-thread context from the frontend's, using the same API the frontend did.
//
// libGL is opened by name and the handful of entry points needed are resolved
// at runtime, so no GLX headers are required to build this.

#pragma once

#include "GS/Renderers/OpenGL/GLContext.h"

#include <span>

class GLContextGLX final : public GLContext
{
public:
	GLContextGLX(const WindowInfo& wi);
	~GLContextGLX() override;

	// Grabs the GLX context current on the calling thread. False when the
	// thread has no GLX context (the frontend is on EGL), or libGL is absent.
	static bool CaptureCurrentContext(void** display, void** context);

	// Creates a context sharing share_context's objects, current on nothing
	// yet. The caller makes it current on the thread that will render.
	static std::unique_ptr<GLContext> CreateShared(const WindowInfo& wi, void* display, void* share_context,
		std::span<const Version> versions_to_try, Error* error);

	void* GetProcAddress(const char* name) override;
	bool ChangeSurface(const WindowInfo& new_wi) override;
	void ResizeSurface(u32 new_surface_width = 0, u32 new_surface_height = 0) override;
	bool SwapBuffers() override;
	bool IsCurrent() override;
	bool MakeCurrent() override;
	bool DoneCurrent() override;
	bool ReleaseThread() override;
	bool SupportsNegativeSwapInterval() const override;
	bool SetSwapInterval(s32 interval) override;
	std::unique_ptr<GLContext> CreateSharedContext(const WindowInfo& wi, Error* error) override;

private:
	bool CreateContext(const Version& version, void* fb_config, void* share_context, Error* error);
	void Destroy();

	void* m_display = nullptr;
	void* m_fb_config = nullptr;
	void* m_context = nullptr;
	unsigned long m_pbuffer = 0;
};
