// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// PCSX2 libretro core frontend.
//
// Threading model:
//  - retro_load_game() spawns a dedicated CPU thread which runs the usual
//    VMManager::Execute() loop (same shape as pcsx2-gsrunner's CPUThreadMain).
//  - Host::PumpMessagesOnCPUThread() is invoked by the core once per emulated
//    frame (at CPU vsync). We use it as the pacing point: the CPU thread grabs
//    the presented frame into a buffer, signals retro_run(), then blocks until
//    the frontend asks for the next frame.
//  - retro_run() hands one "run token" to the CPU thread, waits (with timeout,
//    so slow boots don't freeze the frontend) for the frame, and uploads it.
//
// v1 scope: software renderer + MTGS::SaveMemorySnapshot readback,
// null audio output, no pad input, no savestates. Enough to boot and render.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"
#include "common/WindowInfo.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/Config.h"
#include "pcsx2/GS.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/VMManager.h"

#include "svnrev.h"

#include "libretro.h"

#include "fmt/format.h"

namespace LibretroHost
{
	// libretro callbacks
	static retro_environment_t s_environ_cb;
	static retro_video_refresh_t s_video_cb;
	static retro_audio_sample_batch_t s_audio_batch_cb;
	static retro_input_poll_t s_input_poll_cb;
	static retro_input_state_t s_input_state_cb;
	static retro_log_printf_t s_log_cb;

	// configuration
	static MemorySettingsInterface s_settings_interface;
	static std::string s_system_dir;

	// CPU thread management
	static std::thread s_cpu_thread;
	static std::atomic_bool s_running{false};
	static VMBootParameters s_boot_params;

	// frame pacing: retro_run() posts a run token, CPU thread posts frame-done
	static std::mutex s_frame_mutex;
	static std::condition_variable s_frame_cv;
	static bool s_run_token = false;
	static bool s_frame_ready = false;

	// frame buffer handed from CPU thread to retro_run (guarded by s_frame_mutex)
	static std::vector<u32> s_frame_pixels;
	static u32 s_frame_width = 0;
	static u32 s_frame_height = 0;

	// deferred work queue for Host::RunOnCPUThread
	static std::mutex s_cpu_work_mutex;
	static std::deque<std::function<void()>> s_cpu_work;
	static std::thread::id s_cpu_thread_id;

	static constexpr u32 DEFAULT_WIDTH = 640;
	static constexpr u32 DEFAULT_HEIGHT = 480;
	static constexpr u32 MAX_WIDTH = 1920;
	static constexpr u32 MAX_HEIGHT = 1080;
	static constexpr u32 SAMPLE_RATE = 48000;

	static bool InitializeConfig();
	static void SettingsOverride();
	static void CPUThreadMain();
	static void DrainCPUWork();

	static void LogCallback(const char* fmt_str, ...)
	{
		// placeholder; we rely on PCSX2's Console going to stdout for now
	}
} // namespace LibretroHost

using namespace LibretroHost;

//////////////////////////////////////////////////////////////////////////
// Config / boot
//////////////////////////////////////////////////////////////////////////

bool LibretroHost::InitializeConfig()
{
	// Map PCSX2's folder layout into <retro_system_directory>/pcsx2/.
	// BIOS goes to <system>/pcsx2/bios, resources to <system>/pcsx2/resources.
	const char* system_dir = nullptr;
	if (s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
		s_system_dir = Path::Combine(system_dir, "pcsx2");
	else
		s_system_dir = "pcsx2";

	EmuFolders::AppRoot = s_system_dir;
	EmuFolders::DataRoot = s_system_dir;
	EmuFolders::Resources = Path::Combine(s_system_dir, "resources");
	EmuFolders::UserResources = EmuFolders::Resources;
	EmuFolders::Settings = Path::Combine(s_system_dir, "inis");

	if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
	{
		Console.ErrorFmt("PCSX2 resources directory not found at '{}'. Copy the 'resources' directory from a "
						 "PCSX2 installation there.",
			EmuFolders::Resources);
		return false;
	}

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
	{
		Console.ErrorFmt("Hardware check failed: {}", error);
		return false;
	}

	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
			return false;
		}

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);

		ImGuiManager::SetFonts(std::move(fonts));
	}

	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);

	VMManager::SetDefaultSettings(si, true, true, true, true, true);
	VMManager::Internal::LoadStartupSettings();

	EmuFolders::EnsureFoldersExist();
	return true;
}

void LibretroHost::SettingsOverride()
{
	// the frontend paces us; never block on the limiter or host vsync
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "VsyncEnable", false);

	// v1: software renderer + memory readback
	s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::SW));

	// all input comes through the libretro API, not host devices
	s_settings_interface.SetBoolValue("InputSources", "SDL", false);
	s_settings_interface.SetBoolValue("InputSources", "XInput", false);
	Pad::ClearPortBindings(s_settings_interface, 0);
	s_settings_interface.ClearSection("Hotkeys");

	// v1: no audio output yet
	s_settings_interface.SetStringValue("SPU2/Output", "OutputModule", "nullout");

	s_settings_interface.SetBoolValue("Logging", "EnableSystemConsole", true);
}

void LibretroHost::CPUThreadMain()
{
	s_cpu_thread_id = std::this_thread::get_id();

	if (VMManager::Internal::CPUThreadInitialize())
	{
		VMManager::ApplySettings();

		if (VMManager::Initialize(s_boot_params) == VMBootResult::StartupSuccess)
		{
			VMManager::SetState(VMState::Running);
			while (VMManager::GetState() == VMState::Running && s_running.load(std::memory_order_acquire))
				VMManager::Execute();
			VMManager::Shutdown(false);
		}
		else
		{
			Console.Error("VMManager::Initialize() failed.");
		}
	}

	VMManager::Internal::CPUThreadShutdown();
	s_running.store(false, std::memory_order_release);

	// wake up a potentially waiting retro_run()
	std::unique_lock lock(s_frame_mutex);
	s_frame_ready = true;
	s_frame_cv.notify_all();
}

void LibretroHost::DrainCPUWork()
{
	std::deque<std::function<void()>> work;
	{
		std::unique_lock lock(s_cpu_work_mutex);
		work.swap(s_cpu_work);
	}
	for (auto& fn : work)
		fn();
}

//////////////////////////////////////////////////////////////////////////
// libretro entry points
//////////////////////////////////////////////////////////////////////////

void retro_set_environment(retro_environment_t cb)
{
	s_environ_cb = cb;

	bool no_game = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

	retro_log_callback log_cb{};
	if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_cb))
		s_log_cb = log_cb.log;
}

void retro_set_video_refresh(retro_video_refresh_t cb) { s_video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { s_audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { s_input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { s_input_state_cb = cb; }

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info* info)
{
	std::memset(info, 0, sizeof(*info));
	info->library_name = "PCSX2";
	info->library_version = GIT_REV;
	info->valid_extensions = "iso|chd|cso|zso|gz|bin|mdf|nrg|elf|irx";
	info->need_fullpath = true;
	info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
	std::memset(info, 0, sizeof(*info));
	info->geometry.base_width = DEFAULT_WIDTH;
	info->geometry.base_height = DEFAULT_HEIGHT;
	info->geometry.max_width = MAX_WIDTH;
	info->geometry.max_height = MAX_HEIGHT;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = 59.94;
	info->timing.sample_rate = static_cast<double>(SAMPLE_RATE);
}

void retro_init(void)
{
	CrashHandler::Install();
	Log::SetConsoleOutputLevel(LOGLEVEL_INFO);
}

void retro_deinit(void)
{
}

bool retro_load_game(const struct retro_game_info* game)
{
	if (!game || !game->path)
		return false;

	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!s_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
	{
		Console.Error("XRGB8888 pixel format not supported by frontend.");
		return false;
	}

	if (!InitializeConfig())
		return false;

	SettingsOverride();

	s_boot_params = VMBootParameters();
	s_boot_params.filename = game->path;

	{
		std::unique_lock lock(s_frame_mutex);
		s_run_token = false;
		s_frame_ready = false;
		s_frame_width = 0;
		s_frame_height = 0;
	}

	s_running.store(true, std::memory_order_release);
	s_cpu_thread = std::thread(CPUThreadMain);
	return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
	return false;
}

void retro_unload_game(void)
{
	if (!s_cpu_thread.joinable())
		return;

	s_running.store(false, std::memory_order_release);
	VMManager::SetState(VMState::Stopping);

	// release the CPU thread if it's blocked waiting for a run token
	{
		std::unique_lock lock(s_frame_mutex);
		s_run_token = true;
		s_frame_cv.notify_all();
	}

	s_cpu_thread.join();
}

void retro_reset(void)
{
	if (s_running.load(std::memory_order_acquire))
		Host::RunOnCPUThread([]() { VMManager::Reset(); });
}

void retro_run(void)
{
	if (s_input_poll_cb)
		s_input_poll_cb();

	if (!s_running.load(std::memory_order_acquire))
	{
		// VM is gone (failed boot or shutdown); present black
		static std::vector<u32> black(DEFAULT_WIDTH * DEFAULT_HEIGHT, 0);
		if (s_video_cb)
			s_video_cb(black.data(), DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_WIDTH * sizeof(u32));
		return;
	}

	// hand the CPU thread one frame of execution, wait for the result
	std::unique_lock lock(s_frame_mutex);
	s_run_token = true;
	s_frame_cv.notify_all();

	const bool got_frame = s_frame_cv.wait_for(lock, std::chrono::milliseconds(200), []() { return s_frame_ready; });
	s_frame_ready = false;

	if (got_frame && s_frame_width > 0 && s_frame_height > 0 && s_video_cb)
	{
		s_video_cb(s_frame_pixels.data(), s_frame_width, s_frame_height, s_frame_width * sizeof(u32));
	}
	else if (s_video_cb)
	{
		// no frame produced yet (still booting); duplicate previous frame
		s_video_cb(nullptr, s_frame_width ? s_frame_width : DEFAULT_WIDTH,
			s_frame_height ? s_frame_height : DEFAULT_HEIGHT,
			(s_frame_width ? s_frame_width : DEFAULT_WIDTH) * sizeof(u32));
	}

	// v1: silence
	if (s_audio_batch_cb)
	{
		static int16_t silence[2 * (SAMPLE_RATE / 50)] = {};
		s_audio_batch_cb(silence, SAMPLE_RATE / 60);
	}
}

size_t retro_serialize_size(void)
{
	return 0;
}

bool retro_serialize(void* data, size_t size)
{
	return false;
}

bool retro_unserialize(const void* data, size_t size)
{
	return false;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) {}

unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void* retro_get_memory_data(unsigned id)
{
	return nullptr;
}

size_t retro_get_memory_size(unsigned id)
{
	return 0;
}

//////////////////////////////////////////////////////////////////////////
// Host implementation
//////////////////////////////////////////////////////////////////////////

void Host::CommitBaseSettingChanges()
{
	// in-memory settings; nothing to persist
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = DEFAULT_WIDTH;
	wi.surface_height = DEFAULT_HEIGHT;
	wi.surface_scale = 1.0f;
	return wi;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	// v1: surfaceless; the SW renderer draws into memory and we read it back
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = DEFAULT_WIDTH;
	wi.surface_height = DEFAULT_HEIGHT;
	wi.surface_scale = 1.0f;
	return wi;
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
	INFO_LOG("Game changed: {} ({})", title, disc_serial);
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	if (std::this_thread::get_id() == s_cpu_thread_id)
	{
		function();
		return;
	}

	if (!block)
	{
		std::unique_lock lock(s_cpu_work_mutex);
		s_cpu_work.push_back(std::move(function));
		return;
	}

	// blocking variant: wait for the CPU thread to drain the queue
	std::mutex done_mutex;
	std::condition_variable done_cv;
	bool done = false;
	{
		std::unique_lock lock(s_cpu_work_mutex);
		s_cpu_work.push_back([&]() {
			function();
			std::unique_lock dlock(done_mutex);
			done = true;
			done_cv.notify_all();
		});
	}
	std::unique_lock dlock(done_mutex);
	done_cv.wait(dlock, [&]() { return done; });
}

void Host::RunOnGSThread(std::function<void()> function)
{
	MTGS::RunOnGSThread(std::move(function));
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

void Host::OnAchievementsRefreshed()
{
}

void Host::OnCoverDownloaderOpenRequested()
{
}

void Host::OnCreateMemoryCardOpenRequested()
{
}

bool Host::InBatchMode()
{
	return true;
}

bool Host::InNoGUIMode()
{
	return true;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

void Host::PumpMessagesOnCPUThread()
{
	// run deferred work first (settings changes, reset requests, ...)
	DrainCPUWork();

	if (!s_running.load(std::memory_order_acquire))
		return;

	// read back the presented frame for the frontend
	u32 width = 0, height = 0;
	std::vector<u32> pixels;
	if (MTGS::SaveMemorySnapshot(DEFAULT_WIDTH, DEFAULT_HEIGHT, true, false, &width, &height, &pixels))
	{
		std::unique_lock lock(s_frame_mutex);
		s_frame_pixels = std::move(pixels);
		s_frame_width = width;
		s_frame_height = height;
	}

	// pacing: signal frame done, then wait for the next run token
	std::unique_lock lock(s_frame_mutex);
	s_frame_ready = true;
	s_frame_cv.notify_all();
	s_frame_cv.wait(lock, []() { return s_run_token || !s_running.load(std::memory_order_acquire); });
	s_run_token = false;
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
