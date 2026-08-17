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
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/HostSys.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"
#include "common/WindowInfo.h"

#include "EmbeddedResources.h"
#include "pcsx2/Achievements.h"
#include "pcsx2/Config.h"
#include "pcsx2/GS.h"
#include "pcsx2/Host/AudioStream.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/SPU2/spu2.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/GS/Renderers/Vulkan/GSDeviceVK.h"
#include "pcsx2/GS/Renderers/Vulkan/VKLibretro.h"
#include "pcsx2/GS/Renderers/Vulkan/VKLoader.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/MemoryTypes.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SaveState.h"
#include "pcsx2/USB/USB.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/ps2/BiosTools.h"

#include "svnrev.h"

#include "libretro.h"
#include "libretro_vulkan.h"
#include "LibretroVFS.h"

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

	// CPU thread management. The thread persists across game sessions (like
	// the Qt frontend's EmuThread) because VMManager::Internal::
	// CPUThreadInitialize may only run once per process (page fault handler,
	// COM init, ...). Games are booted in a request loop.
	static std::thread s_cpu_thread;
	static std::atomic_bool s_running{false};
	static VMBootParameters s_boot_params;
	static std::mutex s_session_mutex;
	static std::condition_variable s_session_cv;
	static bool s_boot_requested = false;
	static bool s_exit_requested = false;
	static bool s_session_active = false;

	// frame pacing: retro_run() posts a run token, CPU thread posts frame-done
	static std::mutex s_frame_mutex;
	static std::condition_variable s_frame_cv;
	static bool s_run_token = false;
	static bool s_frame_ready = false;

	// frame buffer handed from CPU thread to retro_run (guarded by s_frame_mutex)
	static std::vector<u32> s_frame_pixels;
	static u32 s_frame_width = 0;
	static u32 s_frame_height = 0;

	// Zero-copy Vulkan HW-render present path (ported from yaps2): the GS shares
	// the frontend's VkDevice (context negotiation) and hands the rendered
	// VkImage straight to the frontend via set_image, instead of the GPU->CPU
	// readback path above. Selected at retro_load_game; the readback path stays
	// as a fallback (renderer != Vulkan, frontend refuses HW render, or the
	// PCEE2_READBACK=1 env override for A/B testing).
	static bool s_hw_render_vulkan = false;
	// Set by the frontend's context_reset once the retro_hw_render_interface is
	// available; the CPU thread parks on it before booting the VM so GSDeviceVK
	// adopts the shared instance during negotiation rather than creating its own.
	static std::atomic<bool> s_context_ready{false};
	// Raised once CPUThreadInitialize() has run, so the frontend's negotiation
	// callback (which opens MTGS) doesn't race the CPU-thread global setup.
	static std::atomic<bool> s_cpu_thread_initialized{false};
	// HW-render counterpart of "s_frame_width != 0": the readback callback that
	// used to set s_frame_width isn't wired in HW mode, so UpdateInput's
	// VM-is-up gate needs this instead. Only touched on the retro_run thread.
	static bool s_hw_frame_seen = false;

	// deferred work queue for Host::RunOnCPUThread
	static std::mutex s_cpu_work_mutex;
	static std::deque<std::function<void()>> s_cpu_work;
	static std::atomic_bool s_cpu_work_pending{false};
	static std::thread::id s_cpu_thread_id;

	static constexpr u32 DEFAULT_WIDTH = 640;
	static constexpr u32 DEFAULT_HEIGHT = 480;
	static constexpr u32 MAX_UPSCALE = 4;
	static constexpr u32 MAX_WIDTH = DEFAULT_WIDTH * MAX_UPSCALE;
	static constexpr u32 MAX_HEIGHT = DEFAULT_HEIGHT * MAX_UPSCALE;

	// current output (readback) resolution; follows the upscale option
	static std::atomic<u32> s_out_width{DEFAULT_WIDTH};
	static std::atomic<u32> s_out_height{DEFAULT_HEIGHT};

	// core option state
	static std::vector<std::string> s_bios_names; // backing storage for option values
	static u32 s_opt_upscale = 1;

	// libretro port -> PCSX2 pad index (see sioConvertPadToPortAndSlot: 0=1A,
	// 1=2A, 2..4=1B..1D, 5..7=2B..2D), built from the multitap option
	static std::vector<u32> s_pad_map = {0, 1};

	// GunCon2 lightguns on USB ports (bit 0 = USB1, bit 1 = USB2)
	static u32 s_lightgun_mask = 0;

	// rumble: written by the InputManager callback (CPU thread), consumed in
	// retro_run; packed as (large << 16) | small, each 0..65535
	static std::array<std::atomic<u32>, 8> s_pad_rumble;
	static bool s_rumble_enabled = true;
	static retro_rumble_interface s_rumble_interface = {};

	static void PadVibrationCallback(u32 pad_index, float large_motor, float small_motor)
	{
		if (pad_index >= s_pad_rumble.size())
			return;
		const u32 large = static_cast<u32>(std::clamp(large_motor, 0.0f, 1.0f) * 65535.0f);
		const u32 small = static_cast<u32>(std::clamp(small_motor, 0.0f, 1.0f) * 65535.0f);
		s_pad_rumble[pad_index].store((large << 16) | small, std::memory_order_relaxed);
	}
	static constexpr u32 SAMPLE_RATE = 48000;
	static constexpr u32 MAX_AUDIO_FRAMES_PER_RUN = 2048;

	// VM timing reported to the frontend (PAL games run at 50Hz, PSX mode at
	// 44.1kHz); updated from the CPU/audio-factory threads, consumed in retro_run
	static std::atomic<u32> s_vm_fps_bits{0};
	static std::atomic<u32> s_audio_sample_rate{SAMPLE_RATE};

	// set once the frontend has been given the EE memory map for this session
	static bool s_memory_map_sent = false;

	// display aspect ratio reported to the frontend (bit_cast'd float); follows
	// the aspect option / widescreen patches
	static std::atomic<u32> s_aspect_bits{0};

	// SPU2 output stream that the frontend pulls samples from in retro_run().
	// Reads happen while the CPU thread is parked in PumpMessagesOnCPUThread(),
	// and the ring buffer is SPSC-atomic anyway, so no extra locking is needed.
	class LibretroAudioStream final : public AudioStream
	{
	public:
		LibretroAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
			: AudioStream(sample_rate, parameters)
		{
		}

		void Initialize()
		{
			BaseInitialize(&StereoSampleReaderImpl, false);
		}

		u32 PullFrames(SampleType* dest, u32 max_frames)
		{
			const u32 frames = std::min(GetBufferedFramesRelaxed(), max_frames);
			if (frames > 0)
				ReadFrames(dest, frames);
			return frames;
		}
	};

	// observer only; the stream is owned by SPU2 (s_output_stream)
	static LibretroAudioStream* s_audio_stream = nullptr;
	static std::atomic<u64> s_audio_frames_output{0};

	// Runs on the GS thread once per vsync with the previous frame's pixels
	// (see GSSetFramebufferReadback). Swizzles RGBA -> XRGB8888 into the
	// buffer retro_run() presents.
	static void FramebufferReadbackCallback(const u32* pixels, u32 pitch_px, u32 width, u32 height)
	{
		std::unique_lock lock(s_frame_mutex);
		s_frame_pixels.resize(static_cast<size_t>(width) * height);
		for (u32 y = 0; y < height; y++)
		{
			const u32* src = pixels + static_cast<size_t>(y) * pitch_px;
			u32* dst = s_frame_pixels.data() + static_cast<size_t>(y) * width;
			for (u32 x = 0; x < width; x++)
			{
				const u32 px = src[x];
				dst[x] = (px & 0xFF00FF00u) | ((px & 0xFFu) << 16) | ((px >> 16) & 0xFFu);
			}
		}
		s_frame_width = width;
		s_frame_height = height;
	}

	static std::unique_ptr<AudioStream> CreateLibretroAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	{
		std::unique_ptr<LibretroAudioStream> stream = std::make_unique<LibretroAudioStream>(sample_rate, parameters);
		stream->Initialize();
		s_audio_stream = stream.get();
		s_audio_sample_rate.store(sample_rate, std::memory_order_release);
		return stream;
	}

	static bool InitializeConfig();
	static void SettingsOverride();
	static void CPUThreadMain();
	static void DrainCPUWork();
	static void RegisterCoreOptions();
	static void ReadCoreOptions(bool startup);

	// Forward PCSX2's log to the frontend's log interface. Keeps Windows from
	// popping up a console window, and the messages land in RetroArch's log.
	static void HostLogCallback(LOGLEVEL level, ConsoleColors color, std::string_view message)
	{
		if (!s_log_cb)
		{
			std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
			return;
		}

		retro_log_level rl = RETRO_LOG_INFO;
		if (level == LOGLEVEL_ERROR)
			rl = RETRO_LOG_ERROR;
		else if (level == LOGLEVEL_WARNING)
			rl = RETRO_LOG_WARN;
		else if (level >= LOGLEVEL_DEV)
			rl = RETRO_LOG_DEBUG;
		s_log_cb(rl, "[PCSX2] %.*s\n", static_cast<int>(message.size()), message.data());
	}
} // namespace LibretroHost

using namespace LibretroHost;

// Tears the core down when the process exits or the library is unloaded:
// stops the persistent CPU thread (running VMManager's CPU-thread shutdown on
// it), and removes the process-wide page fault handler. Registered via
// atexit() from retro_init, which makes it run BEFORE any of this library's
// static destructors (atexit/destructor handlers execute in reverse
// registration order, and retro_init runs after all static initializers) —
// MTGS's global thread object asserts if it's still alive at destruction.
static void ShutdownCoreAtExit()
{
	if (s_cpu_thread.joinable())
	{
		{
			std::unique_lock lock(s_session_mutex);
			s_exit_requested = true;
		}
		s_session_cv.notify_all();
		s_cpu_thread.join();
	}

	PageFaultHandler::Uninstall();
}

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

	// crash dumps belong in our data directory, not the frontend's cwd
	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
	{
		// Not fatal any more: the shaders and fonts the core cannot start without
		// are built into it. What is left in that directory - the game database,
		// the patches - only costs the feature that wants it.
		Console.WarningFmt("No resources directory at '{}'. The core will run, but the game database and "
						   "patches will be unavailable; copy the 'resources' directory from a matching "
						   "PCSX2 build there to get them.",
			EmuFolders::Resources);
	}

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
	{
		Console.ErrorFmt("Hardware check failed: {}", error);
		return false;
	}

	{
		// ImGui keeps the span, so whatever backs it has to outlive this scope:
		// the mapping stays for the process, and the embedded copy is static data.
		std::span<const u8> roboto_data;

#ifdef PCSX2_EMBEDDED_RESOURCES
		if (EmbeddedResourcesPreferred())
		{
			if (const std::optional<std::string_view> embedded = GetEmbeddedResource("fonts/Roboto-Regular.ttf"))
				roboto_data = std::span<const u8>(reinterpret_cast<const u8*>(embedded->data()), embedded->size());
		}
#endif

		if (roboto_data.empty())
		{
			const std::string roboto_path =
				EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
			roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
			if (roboto_data.empty())
			{
				Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
				return false;
			}
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

	// content can be loaded repeatedly in one core session (RetroArch's
	// "Close Content" keeps the core resident); the base layer may only be
	// registered once
	static bool s_settings_layer_registered = false;
	if (!s_settings_layer_registered)
	{
		Host::Internal::SetBaseSettingsLayer(&si);
		s_settings_layer_registered = true;
	}

	VMManager::SetDefaultSettings(si, true, true, true, true, true);

	// If the user dropped a standalone PCSX2.ini into <system>/pcsx2/inis,
	// adopt its emulation settings as the baseline. Core options and the
	// libretro-specific overrides still apply on top, and host-side sections
	// (folders, UI, input bindings, audio device, logging, ...) are ignored.
	{
		const std::string ini_path = Path::Combine(EmuFolders::Settings, "PCSX2.ini");
		INISettingsInterface ini(ini_path);
		if (FileSystem::FileExists(ini_path.c_str()) && ini.Load())
		{
			static constexpr const char* merge_sections[] = {
				"EmuCore",
				"EmuCore/Speedhacks",
				"EmuCore/CPU",
				"EmuCore/CPU/Recompiler",
				"EmuCore/GS",
				"EmuCore/Gamefixes",
				"MemoryCards",
				"DEV9/Eth",
				"DEV9/Hdd",
			};

			u32 merged = 0;
			for (const char* section : merge_sections)
			{
				for (const auto& [key, value] : ini.GetKeyValueList(section))
				{
					s_settings_interface.SetStringValue(section, key.c_str(), value.c_str());
					merged++;
				}
			}
			Console.WriteLnFmt("Adopted {} settings from standalone config '{}'.", merged, ini_path);
		}
	}

	VMManager::Internal::LoadStartupSettings();

	EmuFolders::EnsureFoldersExist();
	return true;
}

void LibretroHost::SettingsOverride()
{
	// the frontend paces us; never block on the limiter or host vsync
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "VsyncEnable", false);

	// Renderer comes from the core options (Vulkan or SW-on-Vulkan); Vulkan is
	// the only Linux backend that supports a Surfaceless (swapchain-less)
	// device, and we read frames back anyway.

	// All input comes through the libretro API, not host devices. The default
	// keyboard bindings are left in place — we never feed host key events, so
	// they are inert, and their presence suppresses the "controller not
	// configured" OSD warning.
	s_settings_interface.SetBoolValue("InputSources", "SDL", false);
	s_settings_interface.SetBoolValue("InputSources", "XInput", false);
	s_settings_interface.ClearSection("Hotkeys");

	// v1: no audio output yet
	s_settings_interface.SetStringValue("SPU2/Output", "OutputModule", "nullout");

	// no system console: it would open a real console window on Windows; logs
	// flow through the libretro log interface instead (see HostLogCallback)
	s_settings_interface.SetBoolValue("Logging", "EnableSystemConsole", false);

	// savestates go through retro_serialize as uncompressed zips; speed over
	// size, the frontend can compress its state files itself
	s_settings_interface.SetIntValue("EmuCore", "SavestateCompressionType",
		static_cast<int>(SavestateCompressionMethod::Uncompressed));

	// pick the first valid BIOS image from <system>/pcsx2/bios if none is configured
	if (s_settings_interface.GetStringValue("Filenames", "BIOS").empty())
	{
		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(EmuFolders::Bios.c_str(), "*", FILESYSTEM_FIND_FILES, &files);
		for (const FILESYSTEM_FIND_DATA& fd : files)
		{
			u32 version, region;
			std::string description, zone;
			if (IsBIOS(fd.FileName.c_str(), version, description, region, zone))
			{
				const std::string filename(Path::GetFileName(fd.FileName));
				Console.WriteLnFmt("Auto-selected BIOS: {} ({})", filename, description);
				s_settings_interface.SetStringValue("Filenames", "BIOS", filename.c_str());
				break;
			}
		}
	}
}

void LibretroHost::RegisterCoreOptions()
{
	// scan for BIOS images so the option can list them
	s_bios_names.clear();
	s_bios_names.push_back("auto");
	{
		const char* system_dir = nullptr;
		if (s_environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
		{
			FileSystem::FindResultsArray files;
			FileSystem::FindFiles(Path::Combine(Path::Combine(system_dir, "pcsx2"), "bios").c_str(), "*",
				FILESYSTEM_FIND_FILES, &files);
			for (const FILESYSTEM_FIND_DATA& fd : files)
			{
				u32 version, region;
				std::string description, zone;
				if (IsBIOS(fd.FileName.c_str(), version, description, region, zone))
					s_bios_names.push_back(std::string(Path::GetFileName(fd.FileName)));
			}
		}
	}

	static retro_core_option_v2_category categories[] = {
		{"system", "System", "BIOS and boot behaviour."},
		{"graphics", "Graphics", "Renderer, resolution and image quality."},
		{"patches", "Patches", "Built-in game patches (widescreen, no-interlacing)."},
		{"performance", "Performance", "Speed hacks. May break games."},
		{nullptr, nullptr, nullptr},
	};

	retro_core_option_v2_definition definitions[] = {
		// system
		{"pcsx2_bios", "BIOS", nullptr, "BIOS image to use, from <system>/pcsx2/bios. Requires restart.", nullptr,
			"system", {{nullptr, nullptr}}, "auto"},
		{"pcsx2_fast_boot", "Fast Boot", nullptr, "Skip the BIOS boot animation. Requires restart.", nullptr,
			"system", {{"enabled", nullptr}, {"disabled", nullptr}, {nullptr, nullptr}}, "enabled"},
		// graphics
		{"pcsx2_renderer", "Renderer", nullptr,
			"Hardware renderer API, or the software renderer. Applies on the fly.",
			nullptr, "graphics",
			{{"vulkan", "Vulkan (Hardware)"}, {"opengl", "OpenGL (Hardware)"}, {"software", "Software"},
				{nullptr, nullptr}},
			"vulkan"},
		{"pcsx2_upscale_multiplier", "Internal Resolution", nullptr,
			"Internal rendering resolution multiplier for the hardware renderer. Also scales the output framebuffer. Applies on the fly.",
			nullptr, "graphics",
			{{"1", "1x Native (640x480)"}, {"2", "2x Native (1280x960)"}, {"3", "3x Native (1920x1440)"},
				{"4", "4x Native (2560x1920)"}, {nullptr, nullptr}},
			"1"},
		{"pcsx2_hw_download_mode", "Hardware Download Mode", nullptr,
			"How GPU->CPU readbacks are handled when a game reads rendered data back (GT3 heat haze, "
			"photo modes...). Accurate stalls the whole pipeline on tiler GPUs; Unsynchronized returns "
			"stale data without stalling (big speedup, may glitch those effects); Disabled skips them.",
			nullptr, "graphics",
			{{"accurate", "Accurate (Default)"}, {"unsynchronized", "Unsynchronized (Fast)"},
				{"disabled", "Disabled (Fastest)"}, {nullptr, nullptr}},
			"accurate"},
		{"pcsx2_blending_accuracy", "Blending Accuracy", nullptr,
			"Higher levels emulate more PS2 blending effects correctly at a GPU cost.", nullptr, "graphics",
			{{"minimum", "Minimum"}, {"basic", "Basic (Recommended)"}, {"medium", "Medium"}, {"high", "High"},
				{"full", "Full (Slow)"}, {"maximum", "Maximum (Very Slow)"}, {nullptr, nullptr}},
			"basic"},
		{"pcsx2_texture_filtering", "Texture Filtering", nullptr,
			"Bilinear (PS2) replicates the console; forced modes smooth all textures.", nullptr, "graphics",
			{{"nearest", "Nearest"}, {"bilinear_ps2", "Bilinear (PS2)"}, {"bilinear_forced", "Bilinear (Forced)"},
				{"bilinear_forced_sprite", "Bilinear (Forced excluding sprites)"}, {nullptr, nullptr}},
			"bilinear_ps2"},
		{"pcsx2_trilinear_filtering", "Trilinear Filtering", nullptr, nullptr, nullptr, "graphics",
			{{"auto", "Automatic (Default)"}, {"off", "Off"}, {"ps2", "Trilinear (PS2)"}, {"forced", "Trilinear (Forced)"},
				{nullptr, nullptr}},
			"auto"},
		{"pcsx2_anisotropic_filtering", "Anisotropic Filtering", nullptr,
			"Reduces texture aliasing at steep angles.", nullptr, "graphics",
			{{"0", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}, {nullptr, nullptr}}, "0"},
		{"pcsx2_dithering", "Dithering", nullptr,
			"Unscaled (default) replicates PS2 dithering; Off can reduce banding artifacts at high resolutions.",
			nullptr, "graphics",
			{{"0", "Off"}, {"1", "Scaled"}, {"2", "Unscaled (Default)"}, {nullptr, nullptr}}, "2"},
		{"pcsx2_mipmapping", "Hardware Mipmapping", nullptr, nullptr, nullptr, "graphics",
			{{"enabled", nullptr}, {"disabled", nullptr}, {nullptr, nullptr}}, "enabled"},
		{"pcsx2_deinterlace_mode", "Deinterlacing", nullptr,
			"Automatic uses the GameDB-recommended mode per game.", nullptr, "graphics",
			{{"0", "Automatic (Default)"}, {"1", "Off"}, {"2", "Weave (TFF)"}, {"3", "Weave (BFF)"},
				{"4", "Bob (TFF)"}, {"5", "Bob (BFF)"}, {"6", "Blend (TFF)"}, {"7", "Blend (BFF)"},
				{"8", "Adaptive (TFF)"}, {"9", "Adaptive (BFF)"}, {nullptr, nullptr}},
			"0"},
		{"pcsx2_fxaa", "FXAA", nullptr, "Cheap post-process anti-aliasing.", nullptr, "graphics",
			{{"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr}}, "disabled"},
		{"pcsx2_cas_mode", "Contrast Adaptive Sharpening", nullptr, nullptr, nullptr, "graphics",
			{{"disabled", "Disabled"}, {"sharpen", "Sharpen Only"}, {nullptr, nullptr}}, "disabled"},
		{"pcsx2_cas_sharpness", "CAS Sharpness", nullptr, nullptr, nullptr, "graphics",
			{{"10", nullptr}, {"20", nullptr}, {"30", nullptr}, {"40", nullptr}, {"50", nullptr}, {"60", nullptr},
				{"70", nullptr}, {"80", nullptr}, {"90", nullptr}, {"100", nullptr}, {nullptr, nullptr}},
			"50"},
		{"pcsx2_aspect_ratio", "Aspect Ratio", nullptr,
			"Automatic reports 16:9 when widescreen patches are enabled, 4:3 otherwise.", nullptr, "graphics",
			{{"auto", "Automatic"}, {"4:3", nullptr}, {"16:9", nullptr}, {nullptr, nullptr}}, "auto"},
		// system (continued)
		{"pcsx2_multitap", "Multitap", nullptr,
			"Enable the multitap adapter for up to 8 controllers. Player order follows the physical slots "
			"(port 1: 1A-1D, then port 2: 2A-2D). Restart recommended.",
			nullptr, "system",
			{{"disabled", "Disabled (2 players)"}, {"port1", "Port 1 (5 players)"}, {"port2", "Port 2 (5 players)"},
				{"both", "Both Ports (8 players)"}, {nullptr, nullptr}},
			"disabled"},
		{"pcsx2_lightgun", "Lightgun (GunCon 2)", nullptr,
			"Emulate a Namco GunCon 2 on a USB port, aimed with the frontend's lightgun (or mouse mapped as "
			"lightgun) on the matching controller port. Requires restart.",
			nullptr, "system",
			{{"disabled", "Disabled"}, {"usb1", "USB Port 1"}, {"usb2", "USB Port 2"}, {"both", "Both Ports"},
				{nullptr, nullptr}},
			"disabled"},
		{"pcsx2_rumble", "Rumble", nullptr, "Forward DualShock 2 vibration to the frontend's rumble support.",
			nullptr, "system", {{"enabled", nullptr}, {"disabled", nullptr}, {nullptr, nullptr}}, "enabled"},
		{"pcsx2_axis_scale", "Analog Axis Scale", nullptr,
			"Scales stick input like a real DualShock 2 (PCSX2 default 133%). Lower if diagonals feel clamped.",
			nullptr, "system",
			{{"100", "100%"}, {"115", "115%"}, {"133", "133% (Default)"}, {"150", "150%"}, {nullptr, nullptr}},
			"133"},
		{"pcsx2_axis_deadzone", "Analog Deadzone", nullptr,
			"Stick deadzone applied inside the emulated controller, on top of any frontend deadzone.", nullptr,
			"system",
			{{"0", "0% (Default)"}, {"5", "5%"}, {"10", "10%"}, {"15", "15%"}, {"20", "20%"}, {"30", "30%"},
				{nullptr, nullptr}},
			"0"},
		// patches
		{"pcsx2_widescreen_patches", "Widescreen Patches", nullptr,
			"Enable built-in 16:9 widescreen patches where available. Best applied before starting a game.", nullptr,
			"patches", {{"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr}}, "disabled"},
		{"pcsx2_no_interlacing_patches", "No-Interlacing Patches", nullptr,
			"Enable built-in progressive-output patches where available. Best applied before starting a game.", nullptr,
			"patches", {{"disabled", nullptr}, {"enabled", nullptr}, {nullptr, nullptr}}, "disabled"},
		// performance
		{"pcsx2_mtvu", "MTVU (Multi-Threaded VU1)", nullptr,
			"Runs VU1 on its own thread. Large speedup on multi-core CPUs; a small number of games hang with it.",
			nullptr, "performance",
			{{"enabled", nullptr}, {"disabled", nullptr}, {nullptr, nullptr}}, "enabled"},
		{"pcsx2_instant_vu1", "Instant VU1", nullptr,
			"Runs VU1 to completion immediately (ignored while MTVU is enabled). Usually a speedup.",
			nullptr, "performance",
			{{"enabled", nullptr}, {"disabled", nullptr}, {nullptr, nullptr}}, "enabled"},
		{"pcsx2_ee_cycle_rate", "EE Cycle Rate", nullptr,
			"Underclock or overclock the emulated Emotion Engine. Default 100%. May break games.", nullptr,
			"performance",
			{{"-3", "50% (Underclock)"}, {"-2", "60% (Underclock)"}, {"-1", "75% (Underclock)"},
				{"0", "100% (Default)"}, {"1", "130% (Overclock)"}, {"2", "180% (Overclock)"},
				{"3", "300% (Overclock)"}, {nullptr, nullptr}},
			"0"},
		{"pcsx2_ee_cycle_skip", "EE Cycle Skip", nullptr,
			"Makes the EE skip cycles. Helps some games with high VU activity, breaks others.", nullptr,
			"performance",
			{{"0", "Disabled (Default)"}, {"1", "Mild"}, {"2", "Moderate"}, {"3", "Maximum"}, {nullptr, nullptr}},
			"0"},
		{"pcsx2_cpu_recompiler", "CPU Recompiler (JIT)", nullptr,
			"Diagnostic master switch. Enabled runs the EE, IOP and VU0/VU1 dynarecs (JIT, fast, default). "
			"Disabled forces every CPU to an interpreter, which is far slower but isolates JIT bugs: if a crash "
			"still happens with this off, the recompiler is not the cause. The four per-CPU switches below only "
			"take effect while this is Enabled. Requires restart.",
			nullptr, "performance",
			{{"enabled", "Enabled (JIT, Default)"}, {"disabled", "Disabled (Interpreter)"}, {nullptr, nullptr}},
			"enabled"},
		{"pcsx2_rec_ee", "  - EE Recompiler", nullptr,
			"Diagnostic. Disable just the Emotion Engine (EE) dynarec while leaving the others on, to bisect "
			"which recompiler causes a crash. Requires restart.",
			nullptr, "performance",
			{{"enabled", "Enabled (Default)"}, {"disabled", "Disabled (Interpreter)"}, {nullptr, nullptr}},
			"enabled"},
		{"pcsx2_rec_iop", "  - IOP Recompiler", nullptr,
			"Diagnostic. Disable just the IOP (R3000) dynarec while leaving the others on, to bisect which "
			"recompiler causes a crash. Requires restart.",
			nullptr, "performance",
			{{"enabled", "Enabled (Default)"}, {"disabled", "Disabled (Interpreter)"}, {nullptr, nullptr}},
			"enabled"},
		{"pcsx2_rec_vu0", "  - VU0 Recompiler", nullptr,
			"Diagnostic. Disable just the VU0 microVU dynarec while leaving the others on, to bisect which "
			"recompiler causes a crash. Requires restart.",
			nullptr, "performance",
			{{"enabled", "Enabled (Default)"}, {"disabled", "Disabled (Interpreter)"}, {nullptr, nullptr}},
			"enabled"},
		{"pcsx2_rec_vu1", "  - VU1 Recompiler", nullptr,
			"Diagnostic. Disable just the VU1 microVU dynarec while leaving the others on, to bisect which "
			"recompiler causes a crash. Requires restart.",
			nullptr, "performance",
			{{"enabled", "Enabled (Default)"}, {"disabled", "Disabled (Interpreter)"}, {nullptr, nullptr}},
			"enabled"},
		{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, {{nullptr, nullptr}}, nullptr},
	};

	// fill in the discovered BIOS list (bounded by the option value array size)
	for (retro_core_option_v2_definition& def : definitions)
	{
		if (!def.key || std::strcmp(def.key, "pcsx2_bios") != 0)
			continue;
		const size_t max_bios = std::min(s_bios_names.size(), std::size(def.values) - 1);
		for (size_t i = 0; i < max_bios; i++)
			def.values[i] = {s_bios_names[i].c_str(), nullptr};
		def.values[max_bios] = {nullptr, nullptr};
		break;
	}

	unsigned version = 0;
	if (s_environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2)
	{
		retro_core_options_v2 options = {categories, definitions};
		s_environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
		return;
	}

	// legacy fallback: "Description; value1|value2" strings
	static std::vector<std::string> legacy_storage;
	legacy_storage.clear();
	std::vector<retro_variable> legacy;
	for (const retro_core_option_v2_definition& def : definitions)
	{
		if (!def.key)
			break;

		std::string str = fmt::format("{}; ", def.desc);
		// default first, as required by the legacy API
		str += def.default_value;
		for (const retro_core_option_value& v : def.values)
		{
			if (!v.value)
				break;
			if (std::strcmp(v.value, def.default_value) != 0)
				str += fmt::format("|{}", v.value);
		}
		legacy_storage.push_back(std::move(str));
		legacy.push_back({def.key, legacy_storage.back().c_str()});
	}
	legacy.push_back({nullptr, nullptr});
	s_environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, legacy.data());
}

void LibretroHost::ReadCoreOptions(bool startup)
{
	const auto get_option = [](const char* key, const char* fallback) -> const char* {
		retro_variable var = {key, nullptr};
		if (s_environ_cb && s_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			return var.value;
		return fallback;
	};

	const char* renderer = get_option("pcsx2_renderer", "vulkan");
	GSRendererType renderer_type = GSRendererType::VK;
	if (std::strcmp(renderer, "software") == 0)
		renderer_type = GSRendererType::SW;
	else if (std::strcmp(renderer, "opengl") == 0)
		renderer_type = GSRendererType::OGL;
	s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(renderer_type));

	const u32 upscale = std::clamp<u32>(StringUtil::FromChars<u32>(get_option("pcsx2_upscale_multiplier", "1")).value_or(1), 1, MAX_UPSCALE);
	s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(upscale));
	s_opt_upscale = upscale;
	s_out_width.store(DEFAULT_WIDTH * upscale, std::memory_order_release);
	s_out_height.store(DEFAULT_HEIGHT * upscale, std::memory_order_release);
	// The Vulkan HW-render path presents by sharing the GS VkImage (set_image);
	// only wire the GPU->CPU readback when we're actually on the readback path.
	if (!s_hw_render_vulkan)
		GSSetFramebufferReadback(&FramebufferReadbackCallback, DEFAULT_WIDTH * upscale, DEFAULT_HEIGHT * upscale);

	// graphics quality
	const auto get_int_option = [&get_option](const char* key, const char* fallback) {
		return StringUtil::FromChars<int>(get_option(key, fallback)).value_or(StringUtil::FromChars<int>(fallback).value_or(0));
	};

	static constexpr std::pair<const char*, AccBlendLevel> blend_levels[] = {
		{"minimum", AccBlendLevel::Minimum}, {"basic", AccBlendLevel::Basic}, {"medium", AccBlendLevel::Medium},
		{"high", AccBlendLevel::High}, {"full", AccBlendLevel::Full}, {"maximum", AccBlendLevel::Maximum}};
	// GSHardwareDownloadMode: Enabled=0, Unsynchronized=3, Disabled=4
	{
		const char* dl = get_option("pcsx2_hw_download_mode", "accurate");
		int dl_mode = 0;
		if (std::strcmp(dl, "unsynchronized") == 0)
			dl_mode = 3;
		else if (std::strcmp(dl, "disabled") == 0)
			dl_mode = 4;
		s_settings_interface.SetIntValue("EmuCore/GS", "HWDownloadMode", dl_mode);
	}

	const char* blend = get_option("pcsx2_blending_accuracy", "basic");
	for (const auto& [name, level] : blend_levels)
	{
		if (std::strcmp(blend, name) == 0)
		{
			s_settings_interface.SetIntValue("EmuCore/GS", "accurate_blending_unit", static_cast<int>(level));
			break;
		}
	}

	static constexpr std::pair<const char*, BiFiltering> bi_filters[] = {
		{"nearest", BiFiltering::Nearest}, {"bilinear_ps2", BiFiltering::PS2},
		{"bilinear_forced", BiFiltering::Forced}, {"bilinear_forced_sprite", BiFiltering::Forced_But_Sprite}};
	const char* bi = get_option("pcsx2_texture_filtering", "bilinear_ps2");
	for (const auto& [name, mode] : bi_filters)
	{
		if (std::strcmp(bi, name) == 0)
		{
			s_settings_interface.SetIntValue("EmuCore/GS", "filter", static_cast<int>(mode));
			break;
		}
	}

	static constexpr std::pair<const char*, TriFiltering> tri_filters[] = {
		{"auto", TriFiltering::Automatic}, {"off", TriFiltering::Off}, {"ps2", TriFiltering::PS2},
		{"forced", TriFiltering::Forced}};
	const char* tri = get_option("pcsx2_trilinear_filtering", "auto");
	for (const auto& [name, mode] : tri_filters)
	{
		if (std::strcmp(tri, name) == 0)
		{
			s_settings_interface.SetIntValue("EmuCore/GS", "TriFilter", static_cast<int>(mode));
			break;
		}
	}

	s_settings_interface.SetIntValue("EmuCore/GS", "MaxAnisotropy", get_int_option("pcsx2_anisotropic_filtering", "0"));
	s_settings_interface.SetIntValue("EmuCore/GS", "dithering_ps2", get_int_option("pcsx2_dithering", "2"));
	s_settings_interface.SetBoolValue("EmuCore/GS", "hw_mipmap",
		std::strcmp(get_option("pcsx2_mipmapping", "enabled"), "enabled") == 0);
	s_settings_interface.SetIntValue("EmuCore/GS", "deinterlace_mode", get_int_option("pcsx2_deinterlace_mode", "0"));
	s_settings_interface.SetBoolValue("EmuCore/GS", "fxaa",
		std::strcmp(get_option("pcsx2_fxaa", "disabled"), "enabled") == 0);
	s_settings_interface.SetIntValue("EmuCore/GS", "CASMode",
		std::strcmp(get_option("pcsx2_cas_mode", "disabled"), "sharpen") == 0 ?
			static_cast<int>(GSCASMode::SharpenOnly) :
			static_cast<int>(GSCASMode::Disabled));
	s_settings_interface.SetIntValue("EmuCore/GS", "CASSharpness", get_int_option("pcsx2_cas_sharpness", "50"));

	// patches
	const bool widescreen = std::strcmp(get_option("pcsx2_widescreen_patches", "disabled"), "enabled") == 0;
	s_settings_interface.SetBoolValue("EmuCore", "EnableWideScreenPatches", widescreen);
	s_settings_interface.SetBoolValue("EmuCore", "EnableNoInterlacingPatches",
		std::strcmp(get_option("pcsx2_no_interlacing_patches", "disabled"), "enabled") == 0);

	// reported display aspect
	const char* aspect = get_option("pcsx2_aspect_ratio", "auto");
	float aspect_value = 4.0f / 3.0f;
	if (std::strcmp(aspect, "16:9") == 0 || (std::strcmp(aspect, "auto") == 0 && widescreen))
		aspect_value = 16.0f / 9.0f;
	s_aspect_bits.store(std::bit_cast<u32>(aspect_value), std::memory_order_release);

	// performance
	// PCEE2_OSD=1: performance overlay (fps/speed + EE/GS/VU thread loads +
	// GPU usage) for diagnosing whether a heavy scene is CPU- or GPU-bound.
	if (std::getenv("PCEE2_OSD"))
	{
		s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowSpeed", true);
		s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowFPS", true);
		s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowResolution", true);
		s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowCPU", true);
		s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowGPU", true);
	}

	// MTVU: VU1 on its own thread — the single biggest speedup on multi-core
	// ARM (pcee2's base defaults it off; yaps2's libretro core defaults it on).
	s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "vuThread",
		std::strcmp(get_option("pcsx2_mtvu", "enabled"), "enabled") == 0);
	s_settings_interface.SetBoolValue("EmuCore/Speedhacks", "vu1Instant",
		std::strcmp(get_option("pcsx2_instant_vu1", "enabled"), "enabled") == 0);
	s_settings_interface.SetIntValue("EmuCore/Speedhacks", "EECycleRate", get_int_option("pcsx2_ee_cycle_rate", "0"));
	s_settings_interface.SetIntValue("EmuCore/Speedhacks", "EECycleSkip", get_int_option("pcsx2_ee_cycle_skip", "0"));

	// multitap: enable adapters and build the libretro-port -> pad-index map
	{
		const char* multitap = get_option("pcsx2_multitap", "disabled");
		const bool mt1 = (std::strcmp(multitap, "port1") == 0 || std::strcmp(multitap, "both") == 0);
		const bool mt2 = (std::strcmp(multitap, "port2") == 0 || std::strcmp(multitap, "both") == 0);
		s_settings_interface.SetBoolValue("Pad", "MultitapPort1", mt1);
		s_settings_interface.SetBoolValue("Pad", "MultitapPort2", mt2);

		s_pad_map.clear();
		s_pad_map.push_back(0); // 1A
		if (mt1)
		{
			s_pad_map.push_back(2); // 1B
			s_pad_map.push_back(3); // 1C
			s_pad_map.push_back(4); // 1D
		}
		s_pad_map.push_back(1); // 2A
		if (mt2)
		{
			s_pad_map.push_back(5); // 2B
			s_pad_map.push_back(6); // 2C
			s_pad_map.push_back(7); // 2D
		}

		// all mapped pads are DualShock 2s, with the configured analog response
		const float axis_scale = static_cast<float>(get_int_option("pcsx2_axis_scale", "133")) / 100.0f;
		const float axis_deadzone = static_cast<float>(get_int_option("pcsx2_axis_deadzone", "0")) / 100.0f;
		for (const u32 pad : s_pad_map)
		{
			const std::string section = fmt::format("Pad{}", pad + 1);
			s_settings_interface.SetStringValue(section.c_str(), "Type", "DualShock2");
			s_settings_interface.SetFloatValue(section.c_str(), "AxisScale", axis_scale);
			s_settings_interface.SetFloatValue(section.c_str(), "Deadzone", axis_deadzone);
		}

		s_rumble_enabled = std::strcmp(get_option("pcsx2_rumble", "enabled"), "enabled") == 0;
	}

	// lightguns: configure GunCon2 USB devices; the presence of the Relative*
	// binding keys switches the device to relative-axis aiming, which we feed
	// from the frontend's lightgun coordinates
	if (startup)
	{
		const char* lightgun = get_option("pcsx2_lightgun", "disabled");
		s_lightgun_mask = 0;
		if (std::strcmp(lightgun, "usb1") == 0 || std::strcmp(lightgun, "both") == 0)
			s_lightgun_mask |= 1;
		if (std::strcmp(lightgun, "usb2") == 0 || std::strcmp(lightgun, "both") == 0)
			s_lightgun_mask |= 2;

		for (u32 usb_port = 0; usb_port < 2; usb_port++)
		{
			const std::string section = fmt::format("USB{}", usb_port + 1);
			if (s_lightgun_mask & (1u << usb_port))
			{
				s_settings_interface.SetStringValue(section.c_str(), "Type", "guncon2");
				for (const char* bind : {"RelativeLeft", "RelativeRight", "RelativeUp", "RelativeDown"})
					s_settings_interface.SetStringValue(section.c_str(), fmt::format("guncon2_{}", bind).c_str(), "None");
			}
			else
			{
				s_settings_interface.SetStringValue(section.c_str(), "Type", "None");
			}
		}
	}

	if (startup)
	{
		const char* bios = get_option("pcsx2_bios", "auto");
		if (std::strcmp(bios, "auto") != 0)
			s_settings_interface.SetStringValue("Filenames", "BIOS", bios);

		s_settings_interface.SetBoolValue("EmuCore", "EnableFastBoot",
			std::strcmp(get_option("pcsx2_fast_boot", "enabled"), "enabled") == 0);

		// Diagnostic toggles: the master switch forces every CPU to its
		// interpreter when off; the four per-CPU switches then let a tester
		// re-enable the dynarecs one at a time to bisect which recompiler
		// causes a crash. Effective enable = master AND per-CPU. Recompiler
		// enables are latched when the CPUs are created, so this only takes
		// effect on a fresh boot.
		const bool jit = std::strcmp(get_option("pcsx2_cpu_recompiler", "enabled"), "enabled") == 0;
		const auto rec_on = [&](const char* key) {
			return jit && std::strcmp(get_option(key, "enabled"), "enabled") == 0;
		};
		const bool ee = rec_on("pcsx2_rec_ee");
		const bool iop = rec_on("pcsx2_rec_iop");
		const bool vu0 = rec_on("pcsx2_rec_vu0");
		const bool vu1 = rec_on("pcsx2_rec_vu1");
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableEE", ee);
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableIOP", iop);
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", vu0);
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", vu1);
		if (!(ee && iop && vu0 && vu1))
			Console.WriteLnFmt("Recompiler state via core options: EE={} IOP={} VU0={} VU1={} (off = interpreter).",
				ee, iop, vu0, vu1);
	}
}

void LibretroHost::CPUThreadMain()
{
	s_cpu_thread_id = std::this_thread::get_id();

	const bool init_ok = VMManager::Internal::CPUThreadInitialize();
	if (!init_ok)
		Console.Error("CPUThreadInitialize() failed.");
	s_cpu_thread_initialized.store(true, std::memory_order_release);

	for (;;)
	{
		{
			std::unique_lock lock(s_session_mutex);
			s_session_cv.wait(lock, []() { return s_boot_requested || s_exit_requested; });
			if (s_exit_requested)
				break;
			s_boot_requested = false;
		}

		// Vulkan HW render: the frontend's context negotiation opens MTGS
		// (GSDeviceVK adopts the shared instance) and then fires context_reset.
		// Booting the VM before that would open the GS against our own instance.
		while (init_ok && s_hw_render_vulkan && !s_context_ready.load(std::memory_order_acquire) &&
			s_running.load(std::memory_order_acquire))
			std::this_thread::sleep_for(std::chrono::milliseconds(1));

		if (init_ok)
		{
			VMManager::ApplySettings();

			if (VMManager::Initialize(s_boot_params) == VMBootResult::StartupSuccess)
			{
				VMManager::SetState(VMState::Running);
				// the frontend paces us through retro_run(); never wall-clock throttle
				VMManager::SetLimiterMode(LimiterModeType::Unlimited);
				while (VMManager::GetState() == VMState::Running && s_running.load(std::memory_order_acquire))
					VMManager::Execute();
				VMManager::Shutdown(false);
			}
			else
			{
				Console.Error("VMManager::Initialize() failed.");
			}
		}

		s_running.store(false, std::memory_order_release);

		// wake up a potentially waiting retro_run()
		{
			std::unique_lock lock(s_frame_mutex);
			s_frame_ready = true;
		}
		s_frame_cv.notify_all();

		// let retro_unload_game() proceed
		{
			std::unique_lock lock(s_session_mutex);
			s_session_active = false;
		}
		s_session_cv.notify_all();
	}

	if (init_ok)
		VMManager::Internal::CPUThreadShutdown();
}

void LibretroHost::DrainCPUWork()
{
	std::deque<std::function<void()>> work;
	{
		std::unique_lock lock(s_cpu_work_mutex);
		work.swap(s_cpu_work);
		s_cpu_work_pending.store(false, std::memory_order_release);
	}
	for (auto& fn : work)
		fn();
}

//////////////////////////////////////////////////////////////////////////
// Vulkan context negotiation (ported from yaps2). create_device runs on the
// frontend thread while it builds its Vulkan context: it stashes the shared
// instance/GPU + the frontend's required extensions/layers/features into
// VKLibretro::Init, then opens MTGS so GSDeviceVK constructs against the shared
// device (its vkCreateDevice is intercepted by the VKLibretro wraps, which
// capture the resulting VkDevice for the reply below).
//////////////////////////////////////////////////////////////////////////

static const VkApplicationInfo* GetVulkanApplicationInfo(void)
{
	static VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app_info.pApplicationName = "PCEE2";
	app_info.applicationVersion = VK_MAKE_VERSION(2, 0, 0);
	app_info.pEngineName = "PCEE2";
	app_info.engineVersion = VK_MAKE_VERSION(2, 0, 0);
	app_info.apiVersion = VK_API_VERSION_1_1;
	return &app_info;
}

static bool CreateVulkanDevice(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char** required_device_extensions,
	unsigned num_required_device_extensions, const char** required_device_layers,
	unsigned num_required_device_layers, const VkPhysicalDeviceFeatures* required_features)
{
	VKLibretro::Init.instance = instance;
	VKLibretro::Init.gpu = gpu;
	VKLibretro::Init.get_instance_proc_addr = get_instance_proc_addr;
	VKLibretro::Init.required_device_extensions = required_device_extensions;
	VKLibretro::Init.num_required_device_extensions = num_required_device_extensions;
	VKLibretro::Init.required_device_layers = required_device_layers;
	VKLibretro::Init.num_required_device_layers = num_required_device_layers;
	VKLibretro::Init.required_features = required_features;

	// Bring up the GS thread now: GSDeviceVK adopts Init.instance/gpu and the
	// wrapped vkCreateDevice fills Init.device with the shared device.
	if (!MTGS::IsOpen() && !MTGS::WaitForOpen())
	{
		Console.Error("MTGS::WaitForOpen failed during Vulkan negotiation.");
		return false;
	}

	GSDeviceVK* dev = GSDeviceVK::GetInstance();
	if (!dev || VKLibretro::Init.device == VK_NULL_HANDLE)
	{
		Console.Error("GS device missing after negotiation open.");
		return false;
	}

	context->gpu = dev->GetPhysicalDevice();
	context->device = VKLibretro::Init.device;
	context->queue = dev->GetGraphicsQueue();
	context->queue_family_index = dev->GetGraphicsQueueFamilyIndex();
	context->presentation_queue = context->queue;
	context->presentation_queue_family_index = context->queue_family_index;
	return true;
}

static void OnContextReset(void)
{
	retro_hw_render_interface* iface = nullptr;
	if (!s_environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) || !iface ||
		iface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN)
	{
		Console.Error("Failed to get Vulkan HW render interface.");
		return;
	}
	VKLibretro::SetHWRenderInterface(iface);
	s_context_ready.store(true, std::memory_order_release);
}

static void OnContextDestroy(void)
{
	VKLibretro::AbortPacing();
	s_context_ready.store(false, std::memory_order_release);
	VKLibretro::SetHWRenderInterface(nullptr);
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

	// Has to happen before anything touches a file - on Android the content,
	// and possibly the system and save directories too, are only reachable
	// through the frontend.
	InitializeVFS(cb);

	RegisterCoreOptions();
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
	info->library_name = "PCEE2";
	info->library_version = GIT_REV;
	info->valid_extensions = "iso|chd|cue|cso|zso|gz|bin|mdf|nrg|elf|irx";
	info->need_fullpath = true;
	info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
	const u32 fps_bits = s_vm_fps_bits.load(std::memory_order_acquire);
	const float fps = fps_bits ? std::bit_cast<float>(fps_bits) : 59.94f;

	std::memset(info, 0, sizeof(*info));
	info->geometry.base_width = s_out_width.load(std::memory_order_acquire);
	info->geometry.base_height = s_out_height.load(std::memory_order_acquire);
	// The Vulkan HW-render canvas is aspect-expanded and can exceed the plain
	// upscale rectangle, so advertise the frontend the larger backbuffer bound.
	info->geometry.max_width = s_hw_render_vulkan ? VKLibretro::kMaxCanvasWidth : MAX_WIDTH;
	info->geometry.max_height = s_hw_render_vulkan ? VKLibretro::kMaxCanvasHeight : MAX_HEIGHT;
	const u32 aspect_bits = s_aspect_bits.load(std::memory_order_acquire);
	info->geometry.aspect_ratio = aspect_bits ? std::bit_cast<float>(aspect_bits) : (4.0f / 3.0f);
	info->timing.fps = static_cast<double>(fps);
	info->timing.sample_rate = static_cast<double>(s_audio_sample_rate.load(std::memory_order_acquire));
}

void retro_init(void)
{
	// Both of these are idempotent, and retro_deinit takes them back down in
	// the reverse order, so a frontend cycling retro_deinit/retro_init ends up
	// with the same chain (page fault handler in front, crash handler behind
	// it) rather than one clobbering the other.
	CrashHandler::Install();

	static bool s_atexit_registered = false;
	if (!s_atexit_registered)
	{
		std::atexit(&ShutdownCoreAtExit);
		s_atexit_registered = true;
	}

	Log::SetHostOutputLevel(LOGLEVEL_INFO, &HostLogCallback);

	LogVFSStatus();
}

void retro_deinit(void)
{
	// Stop the CPU thread completely: leaving anything running past deinit
	// breaks the frontend's process teardown on Windows (threads are killed
	// before DLL atexit handlers run, deadlocking joins under the loader
	// lock). The idempotent signal-handler installs make the next
	// retro_init/load cycle safe again.
	if (s_cpu_thread.joinable())
	{
		{
			std::unique_lock lock(s_session_mutex);
			s_exit_requested = true;
		}
		s_session_cv.notify_all();

		// The exit request is only looked at between sessions. If we are called
		// without retro_unload_game() having wound one down, the CPU thread is
		// parked in PumpMessagesOnCPUThread() waiting for a run token nobody is
		// going to post, and the join below would wait on it forever. Break the
		// pacing handshake too, so that park always has a way out.
		s_running.store(false, std::memory_order_release);
		{
			std::unique_lock lock(s_frame_mutex);
			s_run_token = true;
		}
		s_frame_cv.notify_all();

		s_cpu_thread.join();
		{
			std::unique_lock lock(s_session_mutex);
			s_exit_requested = false;
		}
	}

	// Hand the process' fault handling back to the frontend. Nothing of ours
	// runs from here on, and the frontend unloads this module while it keeps
	// going: a SIGSEGV/exception filter still pointing in here would report the
	// frontend's own crashes as ours - writing a dump of an address space that
	// was the size of a PS2 - and, after the unload, jump into freed code.
	// Reverse order of retro_init: the page fault handler chains to the crash
	// handler, so it has to be taken down first.
	PageFaultHandler::Uninstall();
	CrashHandler::Uninstall();
}

// Redump dumps the PS2's CD-ROM titles - and most demo discs - as a cue sheet
// plus one or more binary tracks, so that is what a lot of libraries hold.
// CDVD has no cue parser (it opens the image directly, auto-detecting 2048,
// 2352 and 2448 byte sectors), so resolve the sheet here and boot the file its
// data track lives in. Audio tracks are lost in the process, which is no worse
// than the same disc converted to CHD: the ISO path reports a single-track TOC
// for every image format it supports.
static std::string ResolveCueSheet(const std::string& cue_path)
{
	const std::optional<std::string> sheet = FileSystem::ReadFileToString(cue_path.c_str());
	if (!sheet.has_value())
	{
		Console.Error(fmt::format("Failed to read cue sheet '{}'.", cue_path));
		return {};
	}

	// FILE "<name>" <format>, then the TRACK lines that belong to it. The
	// first data track is the one to boot; the first file is the fallback for
	// sheets whose first track is audio and which we therefore cannot serve
	// properly anyway.
	std::string_view first_file;
	std::string_view current_file;
	std::string_view data_file;
	for (const std::string_view line : StringUtil::SplitString(sheet.value(), '\n'))
	{
		const std::string_view trimmed = StringUtil::StripWhitespace(line);
		if (StringUtil::StartsWithNoCase(trimmed, "FILE"))
		{
			const std::string_view::size_type open_quote = trimmed.find('"');
			const std::string_view::size_type close_quote =
				(open_quote != std::string_view::npos) ? trimmed.find('"', open_quote + 1) : std::string_view::npos;
			if (close_quote == std::string_view::npos)
				continue;

			current_file = trimmed.substr(open_quote + 1, close_quote - open_quote - 1);
			if (first_file.empty())
				first_file = current_file;
		}
		else if (StringUtil::StartsWithNoCase(trimmed, "TRACK") && data_file.empty())
		{
			// "TRACK 01 MODE1/2352" - anything but AUDIO carries the filesystem.
			if (trimmed.find("AUDIO") == std::string_view::npos)
				data_file = current_file;
		}
	}

	const std::string_view chosen = data_file.empty() ? first_file : data_file;
	if (chosen.empty())
	{
		Console.Error(fmt::format("Cue sheet '{}' names no track file.", cue_path));
		return {};
	}

	// Track files are named relative to the sheet.
	std::string path = Path::Combine(Path::GetDirectory(cue_path), chosen);
	if (!FileSystem::FileExists(path.c_str()))
	{
		Console.Error(fmt::format("Cue sheet '{}' points at '{}', which does not exist.", cue_path, path));
		return {};
	}

	Console.WriteLnFmt("Cue sheet '{}': booting track file '{}'.", cue_path, path);
	return path;
}

// The frontend hands us whatever the user picked; everything except a cue
// sheet goes to the VM as-is.
static std::string ResolveContentPath(const char* path)
{
	if (StringUtil::compareNoCase(Path::GetExtension(path), "cue"))
	{
		std::string resolved = ResolveCueSheet(path);
		if (!resolved.empty())
			return resolved;
	}

	return path;
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

	// Decide the present path before SettingsOverride (which wires the readback
	// callback only when we're NOT sharing the GS VkImage). HW render needs the
	// Vulkan renderer; PCEE2_READBACK=1 forces the legacy readback path for A/B.
	{
		retro_variable var = {"pcsx2_renderer", nullptr};
		const bool renderer_is_vulkan = !s_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value ||
			std::strcmp(var.value, "vulkan") == 0;
		s_hw_render_vulkan = renderer_is_vulkan && !std::getenv("PCEE2_READBACK");
	}

	if (s_hw_render_vulkan)
	{
		static struct retro_hw_render_callback hw_render = {};
		hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
		hw_render.version_major = 1;
		hw_render.version_minor = 1;
		hw_render.context_reset = OnContextReset;
		hw_render.context_destroy = OnContextDestroy;
		hw_render.cache_context = true;
		if (!s_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
		{
			Console.Warning("Frontend refused Vulkan HW context; falling back to readback present.");
			s_hw_render_vulkan = false;
		}
		else
		{
			static const struct retro_hw_render_context_negotiation_interface_vulkan neg_iface = {
				RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
				RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
				GetVulkanApplicationInfo,
				CreateVulkanDevice,
				nullptr, // destroy_device
			};
			s_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, (void*)&neg_iface);

			Error vk_error;
			if (!Vulkan::IsVulkanLibraryLoaded() && !Vulkan::LoadVulkanLibrary(&vk_error))
			{
				Console.Error(fmt::format("LoadVulkanLibrary: {}", vk_error.GetDescription()));
				return false;
			}
			VKLibretro::InstallWraps();
			VKLibretro::Active = true;
			s_context_ready.store(false, std::memory_order_release);
		}
	}

	ReadCoreOptions(true);
	SettingsOverride();

	SPU2::CustomOutputStreamFactory = &CreateLibretroAudioStream;
	InputManager::SetPadVibrationCallback(&PadVibrationCallback);
	for (auto& r : s_pad_rumble)
		r.store(0, std::memory_order_relaxed);
	s_environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &s_rumble_interface);

	s_boot_params = VMBootParameters();
	s_boot_params.filename = ResolveContentPath(game->path);

	{
		std::unique_lock lock(s_frame_mutex);
		s_run_token = false;
		s_frame_ready = false;
		s_frame_width = 0;
		s_frame_height = 0;
	}
	s_hw_frame_seen = false;

	s_running.store(true, std::memory_order_release);
	{
		std::unique_lock lock(s_session_mutex);
		s_boot_requested = true;
		s_session_active = true;
	}
	if (!s_cpu_thread.joinable())
		s_cpu_thread = std::thread(CPUThreadMain);
	s_session_cv.notify_all();

	// The frontend calls the negotiation create_device (which opens MTGS) after
	// this returns; it depends on CPUThreadInitialize having run. Wait for it so
	// the two threads don't race on the CPU-thread global setup.
	if (s_hw_render_vulkan)
	{
		while (!s_cpu_thread_initialized.load(std::memory_order_acquire))
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
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

	// The frontend replays the last set_image indefinitely (menu background,
	// duped frames) — retract it and wait for the GPU before the VM teardown
	// below destroys the textures it points at. Abort pacing first so the GS
	// thread can't stay parked in PublishFrame.
	if (s_hw_render_vulkan)
	{
		VKLibretro::AbortPacing();
		if (auto* vulkan = static_cast<retro_hw_render_interface_vulkan*>(VKLibretro::GetHWRenderInterface()))
		{
			vulkan->set_image(vulkan->handle, nullptr, 0, nullptr, vulkan->queue_index);
			vulkan->wait_sync_index(vulkan->handle);
		}
	}

	s_running.store(false, std::memory_order_release);
	if (VMManager::HasValidVM())
		VMManager::SetState(VMState::Stopping);

	// release the CPU thread if it's blocked waiting for a run token
	{
		std::unique_lock lock(s_frame_mutex);
		s_run_token = true;
		s_frame_cv.notify_all();
	}

	// wait for the session to wind down; the CPU thread itself stays alive
	// for the next retro_load_game
	{
		std::unique_lock lock(s_session_mutex);
		s_session_cv.wait(lock, []() { return !s_session_active; });
	}

	s_audio_stream = nullptr;
	if (!s_hw_render_vulkan)
		GSSetFramebufferReadback(nullptr, 0, 0);
	if (s_hw_render_vulkan)
	{
		VKLibretro::Shutdown();
		VKLibretro::Active = false;
		s_hw_render_vulkan = false;
		s_context_ready.store(false, std::memory_order_release);
	}
	s_memory_map_sent = false;
}

void retro_reset(void)
{
	if (s_running.load(std::memory_order_acquire))
		Host::RunOnCPUThread([]() { VMManager::Reset(); });
}

// Translate libretro joypad/analog state into DualShock2 binds. Called at the
// start of retro_run(), while the CPU thread is parked, so Pad state writes
// don't race the SIO reads.
static void UpdateInput()
{
	if (!s_input_poll_cb || !s_input_state_cb)
		return;

	// don't touch Pad state until the VM is fully up (first frame produced);
	// during VMManager::Initialize() the CPU thread is still constructing it
	if (s_frame_width == 0 && !s_hw_frame_seen)
		return;

	s_input_poll_cb();

	static constexpr std::pair<unsigned, u32> button_map[] = {
		{RETRO_DEVICE_ID_JOYPAD_UP, PadDualshock2::Inputs::PAD_UP},
		{RETRO_DEVICE_ID_JOYPAD_RIGHT, PadDualshock2::Inputs::PAD_RIGHT},
		{RETRO_DEVICE_ID_JOYPAD_DOWN, PadDualshock2::Inputs::PAD_DOWN},
		{RETRO_DEVICE_ID_JOYPAD_LEFT, PadDualshock2::Inputs::PAD_LEFT},
		{RETRO_DEVICE_ID_JOYPAD_X, PadDualshock2::Inputs::PAD_TRIANGLE},
		{RETRO_DEVICE_ID_JOYPAD_A, PadDualshock2::Inputs::PAD_CIRCLE},
		{RETRO_DEVICE_ID_JOYPAD_B, PadDualshock2::Inputs::PAD_CROSS},
		{RETRO_DEVICE_ID_JOYPAD_Y, PadDualshock2::Inputs::PAD_SQUARE},
		{RETRO_DEVICE_ID_JOYPAD_SELECT, PadDualshock2::Inputs::PAD_SELECT},
		{RETRO_DEVICE_ID_JOYPAD_START, PadDualshock2::Inputs::PAD_START},
		{RETRO_DEVICE_ID_JOYPAD_L, PadDualshock2::Inputs::PAD_L1},
		{RETRO_DEVICE_ID_JOYPAD_L2, PadDualshock2::Inputs::PAD_L2},
		{RETRO_DEVICE_ID_JOYPAD_R, PadDualshock2::Inputs::PAD_R1},
		{RETRO_DEVICE_ID_JOYPAD_R2, PadDualshock2::Inputs::PAD_R2},
		{RETRO_DEVICE_ID_JOYPAD_L3, PadDualshock2::Inputs::PAD_L3},
		{RETRO_DEVICE_ID_JOYPAD_R3, PadDualshock2::Inputs::PAD_R3},
	};

	for (u32 port = 0; port < static_cast<u32>(s_pad_map.size()); port++)
	{
		const u32 pad = s_pad_map[port];

		for (const auto& [retro_id, ds2_bind] : button_map)
		{
			const int16_t state = s_input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, retro_id);
			Pad::SetControllerState(pad, ds2_bind, state ? 1.0f : 0.0f);
		}

		// analog sticks: split each axis into the two directional binds
		static constexpr auto axis_value = [](int16_t v, bool negative) {
			const float f = static_cast<float>(v) / 32767.0f;
			return negative ? std::max(-f, 0.0f) : std::max(f, 0.0f);
		};

		const int16_t lx = s_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
		const int16_t ly = s_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
		const int16_t rx = s_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
		const int16_t ry = s_input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);

		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_L_LEFT, axis_value(lx, true));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_L_RIGHT, axis_value(lx, false));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_L_UP, axis_value(ly, true));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_L_DOWN, axis_value(ly, false));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_R_LEFT, axis_value(rx, true));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_R_RIGHT, axis_value(rx, false));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_R_UP, axis_value(ry, true));
		Pad::SetControllerState(pad, PadDualshock2::Inputs::PAD_R_DOWN, axis_value(ry, false));
	}

	// GunCon2 lightguns (bind indices from usb-lightgun/guncon2.cpp)
	for (u32 usb_port = 0; usb_port < 2; usb_port++)
	{
		if (!(s_lightgun_mask & (1u << usb_port)))
			continue;

		const auto gun = [&](unsigned id) {
			return s_input_state_cb(usb_port, RETRO_DEVICE_LIGHTGUN, 0, id);
		};

		// aim: [-0x8000,0x7fff] across the visible video -> relative half-axes
		const float x = std::clamp(static_cast<float>(gun(RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X)) / 32767.0f, -1.0f, 1.0f);
		const float y = std::clamp(static_cast<float>(gun(RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y)) / 32767.0f, -1.0f, 1.0f);
		USB::SetDeviceBindValue(usb_port, 18 /* BID_RELATIVE_LEFT  */, (x < 0.0f) ? -x : 0.0f);
		USB::SetDeviceBindValue(usb_port, 19 /* BID_RELATIVE_RIGHT */, (x > 0.0f) ? x : 0.0f);
		USB::SetDeviceBindValue(usb_port, 20 /* BID_RELATIVE_UP    */, (y < 0.0f) ? -y : 0.0f);
		USB::SetDeviceBindValue(usb_port, 21 /* BID_RELATIVE_DOWN  */, (y > 0.0f) ? y : 0.0f);

		static constexpr std::pair<unsigned, u32> gun_buttons[] = {
			{RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, 13 /* BID_TRIGGER */},
			{RETRO_DEVICE_ID_LIGHTGUN_RELOAD, 16 /* BID_SHOOT_OFFSCREEN */},
			{RETRO_DEVICE_ID_LIGHTGUN_AUX_A, 3 /* BID_A */},
			{RETRO_DEVICE_ID_LIGHTGUN_AUX_B, 2 /* BID_B */},
			{RETRO_DEVICE_ID_LIGHTGUN_AUX_C, 1 /* BID_C */},
			{RETRO_DEVICE_ID_LIGHTGUN_START, 15 /* BID_START */},
			{RETRO_DEVICE_ID_LIGHTGUN_SELECT, 14 /* BID_SELECT */},
			{RETRO_DEVICE_ID_LIGHTGUN_DPAD_UP, 4 /* BID_DPAD_UP */},
			{RETRO_DEVICE_ID_LIGHTGUN_DPAD_DOWN, 6 /* BID_DPAD_DOWN */},
			{RETRO_DEVICE_ID_LIGHTGUN_DPAD_LEFT, 7 /* BID_DPAD_LEFT */},
			{RETRO_DEVICE_ID_LIGHTGUN_DPAD_RIGHT, 5 /* BID_DPAD_RIGHT */},
		};
		for (const auto& [retro_id, bid] : gun_buttons)
			USB::SetDeviceBindValue(usb_port, bid, gun(retro_id) ? 1.0f : 0.0f);
	}
}

static void OutputAudio()
{
	if (!s_audio_batch_cb)
		return;

	static float float_buffer[MAX_AUDIO_FRAMES_PER_RUN * 2];
	static int16_t s16_buffer[MAX_AUDIO_FRAMES_PER_RUN * 2];

	u32 frames = 0;
	if (s_audio_stream)
		frames = s_audio_stream->PullFrames(float_buffer, MAX_AUDIO_FRAMES_PER_RUN);

	if (frames == 0)
	{
		// keep the frontend's audio pipeline fed during boot
		std::memset(s16_buffer, 0, (SAMPLE_RATE / 60) * 2 * sizeof(int16_t));
		s_audio_batch_cb(s16_buffer, SAMPLE_RATE / 60);
		return;
	}

	for (u32 i = 0; i < frames * 2; i++)
	{
		const float v = std::clamp(float_buffer[i], -1.0f, 1.0f);
		s16_buffer[i] = static_cast<int16_t>(v * 32767.0f);
	}
	s_audio_batch_cb(s16_buffer, frames);
	s_audio_frames_output.fetch_add(frames, std::memory_order_relaxed);
}

// Re-announce av_info whenever the VM's timing or our output size changes
// (PAL 50Hz detection after boot, PSX-mode 44.1kHz, upscale option, ...).
static void UpdateAVInfoIfChanged()
{
	// Initialized to what retro_get_system_av_info reported at startup, so the
	// first VM report only announces when it genuinely differs.
	static u32 last_fps_bits = 0; // 0 = "still the 59.94f startup default"
	static u32 last_sample_rate = SAMPLE_RATE;
	static u32 last_width = 0;
	static u32 last_height = 0;
	static u32 last_aspect_bits = 0;

	const u32 fps_bits = s_vm_fps_bits.load(std::memory_order_acquire);
	const u32 sample_rate = s_audio_sample_rate.load(std::memory_order_acquire);
	const u32 width = s_out_width.load(std::memory_order_acquire);
	const u32 height = s_out_height.load(std::memory_order_acquire);
	const u32 aspect_bits = s_aspect_bits.load(std::memory_order_acquire);

	// SET_SYSTEM_AV_INFO makes the frontend reinit the whole video driver
	// (context_destroy + re-negotiation on the HW-render path) — only worth it
	// for a real timing change. In HW-render mode geometry/aspect are handled
	// per-frame via SET_GEOMETRY (no reinit), so ignore them here. The fps
	// compare needs a tolerance: NTSC reports 59.94005994Hz vs the 59.94f
	// startup default, and that 0.00006Hz delta must not trigger a reinit.
	const float fps = fps_bits ? std::bit_cast<float>(fps_bits) : 0.0f;
	const float last_fps = last_fps_bits ? std::bit_cast<float>(last_fps_bits) : 59.94f;
	const bool timing_changed = std::abs(fps - last_fps) > 0.25f || sample_rate != last_sample_rate;
	const bool geometry_changed = width != last_width || height != last_height || aspect_bits != last_aspect_bits;
	if (!timing_changed && (s_hw_render_vulkan || !geometry_changed))
		return;

	// don't announce anything until the VM has reported a real frame rate
	if (fps_bits == 0)
		return;

	last_fps_bits = fps_bits;
	last_sample_rate = sample_rate;
	last_width = width;
	last_height = height;
	last_aspect_bits = aspect_bits;

	// The frontend's video reinit runs inside this environment call while the
	// GS thread may still be chewing queued work that submits to the shared
	// Vulkan queue — drain it first so the reinit doesn't race those submits.
	// (The CPU thread is parked here: retro_run hasn't posted its run token.)
	if (s_hw_render_vulkan && MTGS::IsOpen())
	{
		std::atomic_bool gs_drained{false};
		MTGS::RunOnGSThread([&gs_drained]() { gs_drained.store(true, std::memory_order_release); });
		for (int i = 0; i < 1000 && !gs_drained.load(std::memory_order_acquire); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	retro_system_av_info av_info;
	retro_get_system_av_info(&av_info);
	s_environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
	INFO_LOG("libretro av_info: {}x{} @ {:.2f}Hz, {}Hz audio", av_info.geometry.base_width,
		av_info.geometry.base_height, av_info.timing.fps, sample_rate);
}

void retro_run(void)
{
	// apply core option changes on the fly
	bool options_updated = false;
	if (s_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_updated) && options_updated &&
		s_running.load(std::memory_order_acquire))
	{
		ReadCoreOptions(false);
		Host::RunOnCPUThread([]() { VMManager::ApplySettings(); });
	}

	UpdateAVInfoIfChanged();

	// announce the memory map once the VM has its memory allocated, so the
	// frontend's achievements/cheats can read EE RAM
	if (!s_memory_map_sent && eeMem && s_running.load(std::memory_order_acquire))
	{
		static retro_memory_descriptor descs[2];
		descs[0] = {RETRO_MEMDESC_SYSTEM_RAM, eeMem->Main, 0, 0x00000000u, 0, 0, Ps2MemSize::MainRam, "EE RAM"};
		descs[1] = {RETRO_MEMDESC_SYSTEM_RAM, eeMem->Scratch, 0, 0x70000000u, 0, 0, sizeof(eeMem->Scratch), "Scratchpad"};
		retro_memory_map mmap = {descs, 2};
		s_memory_map_sent = s_environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap);
	}

	if (s_running.load(std::memory_order_acquire))
		UpdateInput();

	// forward DS2 vibration to the frontend
	if (s_rumble_interface.set_rumble_state)
	{
		for (u32 port = 0; port < static_cast<u32>(s_pad_map.size()); port++)
		{
			const u32 packed = s_rumble_enabled ? s_pad_rumble[s_pad_map[port]].load(std::memory_order_relaxed) : 0;
			s_rumble_interface.set_rumble_state(port, RETRO_RUMBLE_STRONG, static_cast<u16>(packed >> 16));
			s_rumble_interface.set_rumble_state(port, RETRO_RUMBLE_WEAK, static_cast<u16>(packed & 0xFFFF));
		}
	}

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

	if (s_hw_render_vulkan)
	{
		// Zero-copy present: consume the frame the GS just published and hand
		// its VkImage to the frontend. The retro_vulkan_image storage must
		// outlive this call (the frontend replays it for cached/duped frames).
		VKLibretro::Frame frame;
		auto* vulkan = static_cast<retro_hw_render_interface_vulkan*>(VKLibretro::GetHWRenderInterface());
		if (vulkan && VKLibretro::ConsumeFrame(&frame))
		{
			s_hw_frame_seen = true;
			// The GS present path sizes the canvas to the aspect-expanded merged
			// frame, so it tracks the internal resolution — keep the frontend's
			// geometry in sync so scaling stays correct.
			static u32 last_geom_w = 0, last_geom_h = 0;
			if (frame.width != last_geom_w || frame.height != last_geom_h)
			{
				last_geom_w = frame.width;
				last_geom_h = frame.height;
				retro_game_geometry geometry = {};
				geometry.base_width = frame.width;
				geometry.base_height = frame.height;
				geometry.max_width = VKLibretro::kMaxCanvasWidth;
				geometry.max_height = VKLibretro::kMaxCanvasHeight;
				// The canvas is already aspect-corrected; display it 1:1.
				geometry.aspect_ratio = static_cast<float>(frame.width) / static_cast<float>(frame.height);
				s_environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
			}
			static retro_vulkan_image vkimage;
			vkimage = {};
			vkimage.image_view = frame.view;
			vkimage.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkimage.create_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, frame.image,
				VK_IMAGE_VIEW_TYPE_2D, frame.format,
				{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
					VK_COMPONENT_SWIZZLE_IDENTITY},
				{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
			vulkan->set_image(vulkan->handle, &vkimage, 0, nullptr, vulkan->queue_index);
			if (s_video_cb)
				s_video_cb(RETRO_HW_FRAME_BUFFER_VALID, frame.width, frame.height, 0);
		}
		else if (s_video_cb)
		{
			// nothing new (still booting, or a duplicate frame) — dupe
			s_video_cb(nullptr, s_frame_width ? s_frame_width : DEFAULT_WIDTH,
				s_frame_height ? s_frame_height : DEFAULT_HEIGHT, 0);
		}
	}
	else if (got_frame && s_frame_width > 0 && s_frame_height > 0 && s_video_cb)
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

	// CPU thread is parked again at this point; safe to drain the audio buffer
	OutputAudio();
}

// Fixed upper bound for uncompressed PS2 state data (EE 32MB + IOP 2MB + GS +
// SPU2 + VU + zip overhead). libretro requires a stable serialize size, while
// the real state size varies per frame, so we pad up to this and store the
// actual length in a small header.
static constexpr size_t SERIALIZE_BUFFER_SIZE = 96 * 1024 * 1024;
static constexpr u32 SERIALIZE_MAGIC = 0x50325253; // 'P2RS'

struct SerializeHeader
{
	u32 magic;
	u32 reserved;
	u64 zip_size;
};

size_t retro_serialize_size(void)
{
	return SERIALIZE_BUFFER_SIZE;
}

bool retro_serialize(void* data, size_t size)
{
	if (!s_running.load(std::memory_order_acquire) || size < sizeof(SerializeHeader))
		return false;

	bool result = false;
	Host::RunOnCPUThread(
		[data, size, &result]() {
			if (VMManager::GetState() != VMState::Running && VMManager::GetState() != VMState::Paused)
				return;

			Error error;
			std::unique_ptr<ArchiveEntryList> entries = SaveState_DownloadState(&error);
			if (!entries)
			{
				ERROR_LOG("retro_serialize: DownloadState failed: {}", error.GetDescription());
				return;
			}

			std::vector<u8> buffer;
			if (!SaveState_ZipToBuffer(std::move(entries), &buffer, &error))
			{
				ERROR_LOG("retro_serialize: ZipToBuffer failed: {}", error.GetDescription());
				return;
			}

			if (sizeof(SerializeHeader) + buffer.size() > size)
			{
				ERROR_LOG("retro_serialize: state too large ({} bytes)", buffer.size());
				return;
			}

			SerializeHeader header = {SERIALIZE_MAGIC, 0, buffer.size()};
			std::memcpy(data, &header, sizeof(header));
			std::memcpy(static_cast<u8*>(data) + sizeof(header), buffer.data(), buffer.size());
			result = true;
		},
		true);

	return result;
}

bool retro_unserialize(const void* data, size_t size)
{
	if (!s_running.load(std::memory_order_acquire) || size < sizeof(SerializeHeader))
		return false;

	SerializeHeader header;
	std::memcpy(&header, data, sizeof(header));
	if (header.magic != SERIALIZE_MAGIC || sizeof(SerializeHeader) + header.zip_size > size)
		return false;

	bool result = false;
	Host::RunOnCPUThread(
		[data, &header, &result]() {
			if (VMManager::GetState() != VMState::Running && VMManager::GetState() != VMState::Paused)
				return;

			Error error;
			if (!SaveState_UnzipFromBuffer(
					static_cast<const u8*>(data) + sizeof(SerializeHeader), header.zip_size, &error))
			{
				ERROR_LOG("retro_unserialize: {}", error.GetDescription());
				return;
			}

			result = true;
		},
		true);

	return result;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) {}

unsigned retro_get_region(void)
{
	const u32 fps_bits = s_vm_fps_bits.load(std::memory_order_acquire);
	return (fps_bits && std::bit_cast<float>(fps_bits) < 55.0f) ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {}

// EE main memory, exposed so the frontend's achievements, cheats and memory
// inspection work. eeMem is allocated during CPU thread startup; until then
// (or after shutdown) report no memory.
void* retro_get_memory_data(unsigned id)
{
	if (id == RETRO_MEMORY_SYSTEM_RAM && eeMem && s_running.load(std::memory_order_acquire))
		return eeMem->Main;
	return nullptr;
}

size_t retro_get_memory_size(unsigned id)
{
	if (id == RETRO_MEMORY_SYSTEM_RAM)
		return Ps2MemSize::MainRam;
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

std::string Host::GetTextFromClipboard()
{
	// No host clipboard access from within the libretro core.
	return std::string();
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
		{
			std::unique_lock lock(s_cpu_work_mutex);
			s_cpu_work.push_back(std::move(function));
			s_cpu_work_pending.store(true, std::memory_order_release);
		}
		s_frame_cv.notify_all(); // wake the CPU thread if it's parked
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
		s_cpu_work_pending.store(true, std::memory_order_release);
	}
	s_frame_cv.notify_all();
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

	// track the VM's vertical frequency for PAL/NTSC av_info reporting
	const float fps = VMManager::GetFrameRate();
	if (fps > 0.0f)
		s_vm_fps_bits.store(std::bit_cast<u32>(fps), std::memory_order_release);

	// frames arrive asynchronously via FramebufferReadbackCallback on the GS
	// thread; nothing to read back here
	static u32 s_frame_counter = 0;
	if ((s_frame_counter++ % 120) == 0)
	{
		u32 width, height;
		{
			std::unique_lock lock(s_frame_mutex);
			width = s_frame_width;
			height = s_frame_height;
		}
		INFO_LOG("libretro frame {}: {}x{} audio_frames={}", s_frame_counter - 1, width, height,
			s_audio_frames_output.load(std::memory_order_relaxed));
	}

	// pacing: signal frame done, then wait for the next run token, servicing
	// queued CPU work (savestates, resets, ...) while parked
	std::unique_lock lock(s_frame_mutex);
	s_frame_ready = true;
	s_frame_cv.notify_all();
	for (;;)
	{
		s_frame_cv.wait(lock, []() {
			return s_run_token || s_cpu_work_pending.load(std::memory_order_acquire) ||
				   !s_running.load(std::memory_order_acquire);
		});

		if (s_cpu_work_pending.load(std::memory_order_acquire))
		{
			lock.unlock();
			DrainCPUWork();
			lock.lock();
			continue;
		}

		break;
	}
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
