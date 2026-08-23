// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/OpenGL/GLLibretro.h"

#include "common/Console.h"
#include "common/Error.h"

#if defined(_WIN32)
#include "GS/Renderers/OpenGL/GLContextWGL.h"
#else
#include "GS/Renderers/OpenGL/GLContextEGL.h"
#include "GS/Renderers/OpenGL/GLContextGLX.h"
#endif

#include <condition_variable>
#include <mutex>

namespace GLLibretro
{
	bool Active = false;

	namespace
	{
		enum class Backend
		{
			None,
			EGL,
			GLX,
			WGL,
		};

		// Written by the frontend thread (context_reset/context_destroy), read
		// by the GS thread when it builds its context. The two are ordered by
		// the core: the CPU thread does not boot the VM -- and so the GS thread
		// does not exist -- until context_reset has run.
		Backend s_backend = Backend::None;
		void* s_display = nullptr;
		void* s_share_context = nullptr;
	} // namespace

	bool CaptureFrontendContext(Error* error)
	{
		ReleaseFrontendContext();

#if defined(_WIN32)
		void* const rc = GLContextWGL::CaptureCurrentContext();
		if (rc)
		{
			s_backend = Backend::WGL;
			s_share_context = rc;
			Console.WriteLn("GL: Captured the frontend's WGL context.");
			return true;
		}
#else
		// EGL first: it is what RetroArch uses on Wayland and Android, and
		// asking it about a thread that is really on GLX is harmless.
		EGLDisplay egl_display = EGL_NO_DISPLAY;
		EGLContext egl_context = EGL_NO_CONTEXT;
		if (GLContextEGL::CaptureCurrentContext(&egl_display, &egl_context))
		{
			s_backend = Backend::EGL;
			s_display = egl_display;
			s_share_context = egl_context;
			Console.WriteLn("GL: Captured the frontend's EGL context.");
			return true;
		}

		void* glx_display = nullptr;
		void* glx_context = nullptr;
		if (GLContextGLX::CaptureCurrentContext(&glx_display, &glx_context))
		{
			s_backend = Backend::GLX;
			s_display = glx_display;
			s_share_context = glx_context;
			Console.WriteLn("GL: Captured the frontend's GLX context.");
			return true;
		}
#endif

		Error::SetStringView(error,
			"No GL context was current on the frontend thread (tried EGL, GLX and WGL)");
		return false;
	}

	void ReleaseFrontendContext()
	{
		s_backend = Backend::None;
		s_display = nullptr;
		s_share_context = nullptr;
	}

	bool HasFrontendContext()
	{
		return s_backend != Backend::None;
	}

	std::unique_ptr<GLContext> CreateSharedContext(
		const WindowInfo& wi, std::span<const GLContext::Version> versions_to_try, Error* error)
	{
		switch (s_backend)
		{
#if defined(_WIN32)
			case Backend::WGL:
				return GLContextWGL::CreateShared(
					wi, static_cast<HGLRC>(s_share_context), versions_to_try, error);
#else
			case Backend::EGL:
				return GLContextEGL::CreateShared(wi, s_display, s_share_context, versions_to_try, error);

			case Backend::GLX:
				return GLContextGLX::CreateShared(wi, s_display, s_share_context, versions_to_try, error);
#endif

			default:
				Error::SetStringView(error, "The frontend's GL context was never captured");
				return nullptr;
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// GS thread -> retro_run frame handoff
	//////////////////////////////////////////////////////////////////////////

	namespace
	{
		std::mutex s_frame_mutex;
		std::condition_variable s_frame_consumed_cv;
		Frame s_frame;
		u64 s_frame_serial = 0;
		u64 s_frame_consumed = 0;
		bool s_pacing = false;
	} // namespace

	void PublishFrame(const Frame& frame)
	{
		std::unique_lock<std::mutex> lock(s_frame_mutex);

		// A frame nobody picked up (pacing off, or the frontend tore its
		// context down mid-flight) still owns a fence. Retire it here, on the
		// thread that made it, rather than leaking one per dropped frame.
		if (s_frame.fence && s_frame_serial != s_frame_consumed)
			glDeleteSync(s_frame.fence);

		s_frame = frame;
		s_frame_serial++;

		// Block the GS thread until retro_run picks the frame up (or pacing
		// gets aborted for shutdown): one presented frame per retro_run.
		s_frame_consumed_cv.wait(
			lock, []() { return !s_pacing || s_frame_consumed == s_frame_serial; });
	}

	bool ConsumeFrame(Frame* out_frame)
	{
		std::lock_guard<std::mutex> lock(s_frame_mutex);
		if (s_frame_serial == s_frame_consumed || s_frame.texture == 0)
			return false;

		s_frame_consumed = s_frame_serial;
		*out_frame = s_frame;
		// The caller owns the fence now; clearing it here keeps PublishFrame
		// from also retiring it.
		s_frame.fence = nullptr;
		s_frame_consumed_cv.notify_all();
		return true;
	}

	void SetPacing(bool enabled)
	{
		std::lock_guard<std::mutex> lock(s_frame_mutex);
		s_pacing = enabled;
		if (!enabled)
			s_frame_consumed_cv.notify_all();
	}

	void AbortPacing()
	{
		SetPacing(false);
	}

	void Shutdown()
	{
		ReleaseFrontendContext();

		std::lock_guard<std::mutex> lock(s_frame_mutex);
		// Not deleted: by the time a session shuts down the context that owns
		// this fence may already be gone, and glDeleteSync on a dead share
		// group is undefined. Dropping the handle leaks nothing that outlives
		// the context itself.
		s_frame = Frame();
		s_frame_serial = 0;
		s_frame_consumed = 0;
		s_pacing = false;
		s_frame_consumed_cv.notify_all();
	}
} // namespace GLLibretro
