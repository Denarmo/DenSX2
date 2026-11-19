#include "ext/libretro.h"

#include "Log.h"
#include "AppConfig.h"
#include "PS2VM.h"
#include "NativeCodeCache.h"
#include "ui_shared/BootableUtils.h"

#include "PS2VM_Preferences.h"
#include "GSH_OpenGL_Libretro.h"
#ifdef PLAY_LIBRETRO_VULKAN
#include "GSH_Vulkan_Libretro.h"
#endif
#include "SH_LibreAudio.h"
#include "PH_Libretro_Input.h"

#include "PathUtils.h"
#include "PtrStream.h"
#include "MemStream.h"

#include "filesystem_def.h"
#include "DefaultAppConfig.h"

#include <vector>
#include <map>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <chrono>

#define LOG_NAME "LIBRETRO"

static CPS2VM* m_virtualMachine = nullptr;
static bool first_run = false;

bool libretro_supports_bitmasks = false;
retro_video_refresh_t g_video_cb;
retro_environment_t g_environ_cb;
retro_input_poll_t g_input_poll_cb;
retro_input_state_t g_input_state_cb;
retro_audio_sample_batch_t g_set_audio_sample_batch_cb;

std::map<int, int> g_ds2_to_retro_btn_map;
struct retro_hw_render_callback g_hw_render
{
};

int g_res_factor = 1;
//The frontend prefers Vulkan and gave a Vulkan device: the Vulkan GS runs instead of OpenGL's.
static bool g_useVulkan = false;

static CGSH_OpenGL_Libretro* GetGlGs()
{
	if(!m_virtualMachine || g_useVulkan) return nullptr;
	return static_cast<CGSH_OpenGL_Libretro*>(m_virtualMachine->GetGSHandler());
}

#ifdef PLAY_LIBRETRO_VULKAN
static CGSH_Vulkan_Libretro* GetVulkanGs()
{
	if(!m_virtualMachine || !g_useVulkan) return nullptr;
	return static_cast<CGSH_Vulkan_Libretro*>(m_virtualMachine->GetGSHandler());
}
#endif
//The frontend applies the aspect ratio, so the core fills the frame it reports.
CGSHandler::PRESENTATION_MODE g_presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_FILL;
bool g_forceBilinearTextures = false;

static std::vector<struct retro_variable> m_vars =
    {
        {"densx2_res_multi", "Resolution Multiplier; 1x|2x|4x|8x"},
        {"densx2_presentation_mode", "Presentation Mode; Fill Screen|Fit Screen|Original Size"},
        {"densx2_bilinear_filtering", "Force Bilinear Filtering; false|true"},
        {"densx2_widescreen", "Widescreen (games with a patch); disabled|enabled"},
        {NULL, NULL},
};

//Widescreen: the frontend shows 16:9, with the patches the game needs for it. Patches are written at every vblank on the emulation thread, so they come back
//after the game reloads its code or settings.
struct WIDESCREEN_PATCH
{
	uint32 address;
	uint32 value;
	uint8 size;
};

struct WIDESCREEN_GAME
{
	const char* executable;
	std::vector<WIDESCREEN_PATCH> patches;
};

static const std::vector<WIDESCREEN_GAME> g_widescreenGames =
    {
        //Colin McRae Rally 3 (PAL): the game's 16:9 setting.
        {"SLES_511.17;1", {{0x002D108C, 1, 1}}},
        //Gran Turismo 4 (PAL): with the game's own 16:9 option on, text drawn 3/4 as wide (from PCSX2's
        //patch by Aero_). The game sets up a race's layout when it loads, so its option is not forced.
        //The code it jumps to comes first.
        {"SCES_517.19;1",
         {
             {0x00495998, 0x3C090061, 4},
             {0x0049599C, 0x352984F0, 4},
             {0x004959A0, 0x812A0000, 4},
             {0x004959A4, 0xC65A0024, 4},
             {0x004959A8, 0x11400004, 4},
             {0x004959AC, 0x3C013F40, 4},
             {0x004959B0, 0x34210000, 4},
             {0x004959B4, 0x4481A000, 4},
             {0x004959B8, 0x4614D682, 4},
             {0x004959BC, 0x08124B83, 4},
             {0x00492E08, 0x08125666, 4},
             {0x0044DF60, 0x00000000, 4},
         }},
};

static std::atomic<bool> g_widescreen = false;
static std::atomic<const WIDESCREEN_GAME*> g_widescreenGame = nullptr;
static CPS2VM::NewFrameEvent::Connection g_widescreenConnection;
static std::string g_widescreenExecutable;
//What each patched address held before its first patch, to put back when widescreen is turned off.
static std::map<uint32, uint32> g_widescreenOriginals;

static uint32 ReadEeRam(const uint8* ram, uint32 address, uint8 size)
{
	switch(size)
	{
	case 1:
		return ram[address];
	case 2:
		return *reinterpret_cast<const uint16*>(ram + address);
	default:
		return *reinterpret_cast<const uint32*>(ram + address);
	}
}

static void WriteEeRam(uint8* ram, uint32 address, uint32 value, uint8 size)
{
	switch(size)
	{
	case 1:
		ram[address] = static_cast<uint8>(value);
		break;
	case 2:
		*reinterpret_cast<uint16*>(ram + address) = static_cast<uint16>(value);
		break;
	default:
		*reinterpret_cast<uint32*>(ram + address) = value;
		break;
	}
}

//Runs on the emulation thread. Only differing values are written: a write to compiled code makes the
//executor drop the blocks it touches.
static void ApplyWidescreenPatches()
{
	if(!m_virtualMachine) return;
	const char* executable = m_virtualMachine->m_ee->m_os->GetExecutableName();
	if(!executable) executable = "";
	if(g_widescreenExecutable != executable)
	{
		g_widescreenExecutable = executable;
		g_widescreenOriginals.clear();
		const WIDESCREEN_GAME* found = nullptr;
		for(const auto& game : g_widescreenGames)
		{
			if(g_widescreenExecutable == game.executable) found = &game;
		}
		g_widescreenGame = found;
	}
	const WIDESCREEN_GAME* game = g_widescreenGame;
	if(!game) return;
	auto ram = m_virtualMachine->m_ee->m_ram;
	const auto& patches = game->patches;
	if(g_widescreen)
	{
		for(const auto& patch : patches)
		{
			uint32 current = ReadEeRam(ram, patch.address, patch.size);
			if(current == patch.value) continue;
			g_widescreenOriginals.emplace(patch.address, current);
			WriteEeRam(ram, patch.address, patch.value, patch.size);
		}
	}
	else if(!g_widescreenOriginals.empty())
	{
		//In reverse, so a jump goes before the code it jumps to.
		for(auto patch = patches.rbegin(); patch != patches.rend(); patch++)
		{
			auto original = g_widescreenOriginals.find(patch->address);
			if(original == g_widescreenOriginals.end()) continue;
			if(ReadEeRam(ram, patch->address, patch->size) != patch->value) continue;
			WriteEeRam(ram, patch->address, original->second, patch->size);
		}
		g_widescreenOriginals.clear();
	}
}

//Whether the picture is 16:9: widescreen is on and the running game has a patch.
static bool IsWidescreenActive()
{
	return g_widescreen && g_widescreenGame;
}

enum class BootType
{
	CD,
	ELF
};

struct LastOpenCommand
{
	LastOpenCommand() = default;
	LastOpenCommand(BootType type, fs::path path)
	    : type(type)
	    , path(path)
	{
	}
	BootType type = BootType::CD;
	fs::path path;
};

LastOpenCommand m_bootCommand;

unsigned retro_api_version()
{
	return RETRO_API_VERSION;
}

void SetupVideoHandler()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	auto gsHandler = m_virtualMachine->GetGSHandler();
#ifdef PLAY_LIBRETRO_VULKAN
	if(g_useVulkan)
	{
		if(!gsHandler)
		{
			m_virtualMachine->CreateGSHandler(CGSH_Vulkan_Libretro::GetFactoryFunction());
			CLog::GetInstance().Print(LOG_NAME, "GS calls run on the Vulkan GS thread\n");
		}
		else
		{
			auto retro_gs = static_cast<CGSH_Vulkan_Libretro*>(gsHandler);
			retro_gs->SetFreeRun(true);
			retro_gs->Reset();
			retro_gs->SetFreeRun(false);
		}
		return;
	}
#endif
	if(!gsHandler)
	{
		m_virtualMachine->CreateGSHandler(CGSH_OpenGL_Libretro::GetFactoryFunction());
		auto retro_gs = static_cast<CGSH_OpenGL_Libretro*>(m_virtualMachine->GetGSHandler());
		bool threaded = retro_gs->StartWorker();
		CLog::GetInstance().Print(LOG_NAME, "GS calls run on %s\n", threaded ? "a thread of their own" : "the frontend thread");
	}
	else
	{
		auto retro_gs = static_cast<CGSH_OpenGL_Libretro*>(gsHandler);
		retro_gs->Reset();
	}
}

static void retro_context_destroy()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

static void retro_context_reset()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

#ifdef PLAY_LIBRETRO_VULKAN
	if(g_useVulkan)
	{
		const retro_hw_render_interface* hwInterface = nullptr;
		if(!g_environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &hwInterface) || !hwInterface ||
		   (hwInterface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN) ||
		   (hwInterface->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION))
		{
			CLog::GetInstance().Warn(LOG_NAME, "The frontend gave no Vulkan interface.\n");
			return;
		}
		CGSH_Vulkan_Libretro::SetFrontendInterface(reinterpret_cast<const retro_hw_render_interface_vulkan*>(hwInterface));
	}
#endif

	if(m_virtualMachine)
	{
		SetupVideoHandler();
	}
}

void SetupSoundHandler()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		m_virtualMachine->CreateSoundHandler(&CSH_LibreAudio::HandlerFactory);
	}
}

void SetupInputHandler()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(!m_virtualMachine->GetPadHandler())
	{
		static struct retro_input_descriptor descDS2[] =
		    {
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Stick X"},
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y"},
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X"},
		        {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Square"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Triangle"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Circle"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Cross"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L1"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R1"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2"},
		        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3"},
		        {0},
		    };

		g_environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descDS2);

		static const struct retro_controller_description controllers[] = {
		    {"PS2 DualShock2", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)},
		};

		static const struct retro_controller_info ports[] = {
		    {controllers, 1},
		    {NULL, 0},
		};

		g_environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

		for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
		{
			auto ds2_button = static_cast<PS2::CControllerInfo::BUTTON>(i);
			auto retro_button = descDS2[i].id;
			g_ds2_to_retro_btn_map[ds2_button] = retro_button;
		}

		m_virtualMachine->CreatePadHandler(CPH_Libretro_Input::GetFactoryFunction());
	}
}

void retro_get_system_info(struct retro_system_info* info)
{
	*info = {};
	info->library_name = "DenSX2";
	info->library_version = DENSX2_VERSION " (based on Play! " PLAY_VERSION ")";
	info->need_fullpath = true;
	info->valid_extensions = "elf|iso|cso|isz|cue|chd";
}

// The refresh rate the frontend paces retro_run at: PAL games switch the GS to 50 Hz after boot.
static unsigned g_frame_rate = 60;

void retro_get_system_av_info(struct retro_system_av_info* info)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	*info = {};
	info->timing.fps = g_frame_rate;
	info->timing.sample_rate = 44100;
	info->geometry.base_width = 640;
	info->geometry.base_height = 448;
	info->geometry.max_width = 640 * 8;
	info->geometry.max_height = 448 * 8;
	info->geometry.aspect_ratio = IsWidescreenActive() ? (16.0 / 9.0) : (4.0 / 3.0);
}

static bool g_geometryWide = false;

static void UpdateGeometry()
{
	bool wide = IsWidescreenActive();
	if(wide == g_geometryWide) return;
	g_geometryWide = wide;
	retro_system_av_info info;
	retro_get_system_av_info(&info);
	g_environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info.geometry);
}

static void UpdateFrameRate(unsigned frameRate)
{
	if(frameRate == 0 || frameRate == g_frame_rate) return;
	g_frame_rate = frameRate;
	retro_system_av_info info;
	retro_get_system_av_info(&info);
	g_environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_video_cb = cb;
}

void retro_set_environment(retro_environment_t cb)
{
	g_environ_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_input_state_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	g_set_audio_sample_batch_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

unsigned retro_get_region(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	return RETRO_REGION_NTSC;
}

size_t retro_serialize_size(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	return 40 * 1024 * 1024;
}

// The emulation thread saves and loads at the top of its loop. Meanwhile this thread runs the
// GS calls it may be waiting for, as it does in retro_run.
static bool WaitForVmCall(std::future<bool>& result)
{
#ifdef PLAY_LIBRETRO_VULKAN
	if(auto vulkanGs = GetVulkanGs())
	{
		vulkanGs->SetFreeRun(true);
		result.wait();
		vulkanGs->SetFreeRun(false);
		return result.get();
	}
#endif
	auto gsHandler = GetGlGs();
	if(gsHandler)
	{
		gsHandler->ProcessCallsUntil([&result]() { return result.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
	}
	return result.get();
}

bool retro_serialize(void* data, size_t size)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(!m_virtualMachine || !first_run) return false;

	try
	{
		Framework::CMemStream stateStream;
		auto result = m_virtualMachine->SaveState(stateStream);
		if(!WaitForVmCall(result)) return false;
		if(stateStream.GetSize() > size) return false;
		memcpy(data, stateStream.GetBuffer(), stateStream.GetSize());
		memset(reinterpret_cast<uint8*>(data) + stateStream.GetSize(), 0, size - stateStream.GetSize());
	}
	catch(...)
	{
		return false;
	}

	return true;
}

bool retro_unserialize(const void* data, size_t size)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(!m_virtualMachine || !first_run) return false;

	try
	{
		Framework::CPtrStream stateStream(data, size);
		auto result = m_virtualMachine->LoadState(stateStream);
		return WaitForVmCall(result);
	}
	catch(...)
	{
		return false;
	}
}

void* retro_get_memory_data(unsigned id)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(id == RETRO_MEMORY_SYSTEM_RAM)
	{
		return m_virtualMachine->m_ee->m_ram;
	}
	return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(id == RETRO_MEMORY_SYSTEM_RAM)
	{
		return PS2::EE_RAM_SIZE;
	}
	return 0;
}

void retro_cheat_reset(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	(void)index;
	(void)enabled;
	(void)code;
}

void updateVars()
{
	for(int i = 0; i < m_vars.size() - 1; ++i)
	{
		auto item = m_vars[i];
		if(!item.key)
			continue;

		struct retro_variable var = {nullptr};
		var.key = item.key;
		if(g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			bool videoUpdate = false;
			switch(i)
			{
			case 0:
			{
				std::string val = var.value;
				auto res_factor = std::atoi(val.substr(0, -1).c_str());
				if(res_factor != g_res_factor)
				{
					g_res_factor = res_factor;
					CAppConfig::GetInstance().SetPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR, res_factor);
					videoUpdate = true;
				}
			}
			break;
			case 1:
			{
				CGSHandler::PRESENTATION_MODE presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_FILL;

				std::string val(var.value);
				if(val == "Fit Screen")
					presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_FIT;
				else if(val == "Original Size")
					presentation_mode = CGSHandler::PRESENTATION_MODE::PRESENTATION_MODE_ORIGINAL;

				if(presentation_mode != g_presentation_mode)
				{
					g_presentation_mode = presentation_mode;
					CAppConfig::GetInstance().SetPreferenceInteger(PREF_CGSHANDLER_PRESENTATION_MODE, presentation_mode);
					videoUpdate = true;
				}
			}
			break;
			case 2:
			{
				bool forceBilinearTextures = (std::string(var.value) == "true");
				if(forceBilinearTextures != g_forceBilinearTextures)
				{
					g_forceBilinearTextures = forceBilinearTextures;
					CAppConfig::GetInstance().SetPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES, forceBilinearTextures);
					videoUpdate = true;
				}
			}
			break;
			case 3:
				g_widescreen = (std::string(var.value) == "enabled");
				break;
			}

			if(videoUpdate)
			{
				if(auto glGs = GetGlGs())
					glGs->UpdatePresentation();
			}
		}
	}
}

void checkVarsUpdates()
{
	static bool updates = true;
	if(!updates)
		g_environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updates);

	if(updates)
	{
		updateVars();
	}
	updates = false;
}

//Diagnostic, on while <save dir>/play/frame-stats.on exists: every 500 frames, a line in frame-stats.txt
//with the longest interval between retro_run calls, how many were over 34 and 50 ms, and the longest run.
struct FRAME_STATS
{
	std::string path;
	std::chrono::steady_clock::time_point firstStart, lastStart;
	uint32 frames = 0, over34 = 0, over50 = 0;
	double maxInterval = 0, maxRun = 0;
	std::string longIntervals;
};
static FRAME_STATS g_frameStats;

static void UpdateFrameStats(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
{
	auto& stats = g_frameStats;
	if(stats.lastStart.time_since_epoch().count() != 0)
	{
		double interval = std::chrono::duration<double, std::milli>(start - stats.lastStart).count();
		stats.maxInterval = (std::max)(stats.maxInterval, interval);
		stats.over34 += (interval > 34);
		stats.over50 += (interval > 50);
		if((interval > 34) && (stats.longIntervals.size() < 400))
		{
			char item[32];
			snprintf(item, sizeof(item), " %.1fs:%.0f", std::chrono::duration<double>(start - stats.firstStart).count(), interval);
			stats.longIntervals += item;
		}
	}
	else
	{
		stats.firstStart = start;
	}
	stats.maxRun = (std::max)(stats.maxRun, std::chrono::duration<double, std::milli>(end - start).count());
	stats.lastStart = start;
	if(++stats.frames < 500) return;
	if(FILE* output = fopen(stats.path.c_str(), "a"))
	{
		fprintf(output, "%.1fs: frames %u interval max %.1f ms, >34 ms %u, >50 ms %u, run max %.1f ms;%s\n",
		        std::chrono::duration<double>(start - stats.firstStart).count(), stats.frames, stats.maxInterval,
		        stats.over34, stats.over50, stats.maxRun, stats.longIntervals.c_str());
		fclose(output);
	}
	stats.frames = stats.over34 = stats.over50 = 0;
	stats.maxInterval = stats.maxRun = 0;
	stats.longIntervals.clear();
}

void retro_run()
{
	// CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
	auto runStart = std::chrono::steady_clock::now();

	checkVarsUpdates();

	if(!first_run)
	{
		if(m_virtualMachine)
		{
			// m_virtualMachine->Pause();
			m_virtualMachine->Reset();
			if(m_bootCommand.type == BootType::CD)
			{
				m_virtualMachine->m_ee->m_os->BootFromCDROM();
			}
			else
			{
				m_virtualMachine->m_ee->m_os->BootFromFile(m_bootCommand.path);
			}
			m_virtualMachine->Resume();
			first_run = true;
			CLog::GetInstance().Print(LOG_NAME, "%s\n", "Start Game");
		}
	}

	if(m_virtualMachine)
	{
		auto pad = m_virtualMachine->GetPadHandler();
		if(pad)
			static_cast<CPH_Libretro_Input*>(pad)->UpdateInputState();

		if(m_virtualMachine->GetSoundHandler())
			static_cast<CSH_LibreAudio*>(m_virtualMachine->GetSoundHandler())->ProcessBuffer();

#ifdef PLAY_LIBRETRO_VULKAN
		if(auto vulkanGs = GetVulkanGs())
		{
			vulkanGs->PresentFrame();
			UpdateFrameRate(vulkanGs->GetCrtFrameRate());
			UpdateGeometry();
		}
#endif
		if(auto gsHandler = GetGlGs())
		{
			if(gsHandler->HasWorker())
			{
				gsHandler->PresentWorkerFrame();
			}
			else if(!gsHandler->PresentHeldFrame())
			{
				gsHandler->ProcessSingleFrame();
			}
			UpdateFrameRate(gsHandler->GetCrtFrameRate());
			UpdateGeometry();
		}
	}

	if(!g_frameStats.path.empty())
	{
		UpdateFrameStats(runStart, std::chrono::steady_clock::now());
	}
}

void retro_reset(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		if(!m_virtualMachine->GetGSHandler())
			SetupVideoHandler();
		// m_virtualMachine->Pause();
		m_virtualMachine->Reset();
		m_virtualMachine->m_ee->m_os->BootFromCDROM();
		m_virtualMachine->Resume();
		CLog::GetInstance().Print(LOG_NAME, "%s\n", "Reset Game");
	}
	first_run = false;
}

bool retro_load_game(const retro_game_info* info)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

#if defined(IOS)
	bool can_jit = false;
	if(g_environ_cb(RETRO_ENVIRONMENT_GET_JIT_CAPABLE, &can_jit) && !can_jit)
	{
		// trying to run without the jit will cause a crash.
		retro_message retromsg;
		retromsg.msg = "Cannot run without JIT";
		retromsg.frames = 5000 / 17;
		g_environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &retromsg);
		return false;
	}
#endif

	fs::path filePath = info->path;
	if(BootableUtils::IsBootableExecutablePath(filePath))
	{
		m_bootCommand = LastOpenCommand(BootType::ELF, filePath);
	}
	else if(BootableUtils::IsBootableDiscImagePath(filePath))
	{
		m_bootCommand = LastOpenCommand(BootType::CD, filePath);
		CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH, filePath);
		CAppConfig::GetInstance().Save();
	}
	first_run = false;
	g_frame_rate = 60;
	g_geometryWide = false;

	auto rgb = RETRO_PIXEL_FORMAT_XRGB8888;
	g_environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb);

	g_useVulkan = false;
#ifdef PLAY_LIBRETRO_VULKAN
	{
		//Vulkan is asked for first where the GPU can run the Vulkan GS: a frontend that wants
		//OpenGL refuses it and the core goes on with OpenGL.
		unsigned preferred = 0;
		bool prefersVulkan = g_environ_cb(RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred) && (preferred == RETRO_HW_CONTEXT_VULKAN);
		if(prefersVulkan || CGSH_Vulkan_Libretro::IsSupported())
		{
			g_hw_render = {};
			g_hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
			g_hw_render.version_major = VK_API_VERSION_1_1;
			g_hw_render.context_reset = retro_context_reset;
			g_hw_render.context_destroy = retro_context_destroy;
			if(g_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hw_render))
			{
				g_useVulkan = true;
				g_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE,
				             const_cast<retro_hw_render_context_negotiation_interface_vulkan*>(CGSH_Vulkan_Libretro::GetNegotiationInterface()));
				CLog::GetInstance().Print(LOG_NAME, "Using the Vulkan GS\n");
				g_environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)m_vars.data());
				return true;
			}
		}
	}
#endif

#ifdef GLES_COMPATIBILITY
	g_hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
#else
	g_hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
#endif

	g_hw_render.version_major = 3;
	g_hw_render.version_minor = 2;
	g_hw_render.context_reset = retro_context_reset;
	g_hw_render.context_destroy = retro_context_destroy;
	g_hw_render.cache_context = false;
	g_hw_render.bottom_left_origin = true;
	g_hw_render.depth = true;
	g_environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, nullptr);

	g_environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &g_hw_render);

	g_environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, nullptr);

	g_environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)m_vars.data());

	return true;
}

void retro_unload_game(void)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	return false;
}

void retro_init()
{
#ifdef __ANDROID__
	{
		// Keep Play!'s data in the frontend's save directory: EXTERNAL_STORAGE is often not writable.
		const char* saveDir = nullptr;
		if(!g_environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDir) || !saveDir || !*saveDir)
		{
			saveDir = getenv("EXTERNAL_STORAGE");
		}
		Framework::PathUtils::SetFilesDirPath(saveDir);
		Framework::PathUtils::SetCacheDirPath(saveDir);
	}
#endif
	{
		//Compiled GL programs are kept between runs: without them every shader met again stutters.
		const char* saveDir = nullptr;
		if(g_environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDir) && saveDir && *saveDir)
		{
			std::error_code error;
			auto cacheDir = fs::path(saveDir) / "play";
			fs::create_directories(cacheDir, error);
			if(!error)
			{
				CGSH_OpenGL::SetProgramCachePath((cacheDir / "gl-programs.bin").string());
				CNativeCodeCache::SetCacheDir((cacheDir / "aot").string());
			}
			if(fs::exists(cacheDir / "frame-stats.on", error)) g_frameStats.path = (cacheDir / "frame-stats.txt").string();
#ifdef PLAY_LIBRETRO_VULKAN
			CGSH_Vulkan::SetPipelineCachePath((cacheDir / "vk-pipeline-cache.bin").string());
			if(fs::exists(cacheDir / "gs-stats.on", error))
			{
				auto statsPath = (cacheDir / "gs-stats.txt").string();
				CGSH_Vulkan::SetStatsPath(statsPath);
				CPS2VM::SetTimingStatsPath(statsPath);
			}
#endif
		}
	}
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(g_environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
		libretro_supports_bitmasks = true;

	CAppConfig::GetInstance().RegisterPreferenceInteger(PREF_AUDIO_SPUBLOCKCOUNT, 22);

	m_virtualMachine = new CPS2VM();
	m_virtualMachine->Initialize();
	g_widescreenConnection = m_virtualMachine->OnNewFrame.Connect(&ApplyWidescreenPatches);

	//Disable frame limiter, RetroArch handles this on its own
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_PS2_LIMIT_FRAMERATE, false);
	m_virtualMachine->ReloadFrameRateLimit();

	SetupInputHandler();
	SetupSoundHandler();
	first_run = false;
}

void retro_deinit()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_virtualMachine)
	{
		m_virtualMachine->PauseAsync();
		auto gsHandler = GetGlGs();
#ifdef PLAY_LIBRETRO_VULKAN
		if(auto vulkanGs = GetVulkanGs())
		{
			vulkanGs->SetFreeRun(true);
			while(m_virtualMachine->GetStatus() != CVirtualMachine::PAUSED)
			{
				std::this_thread::yield();
			}
		}
#endif
		//With the Vulkan GS there is no OpenGL handler: gsHandler is null.
		if(gsHandler && gsHandler->HasWorker())
		{
			gsHandler->SetFreeRun(true);
			while(m_virtualMachine->GetStatus() != CVirtualMachine::PAUSED)
			{
				std::this_thread::yield();
			}
			gsHandler->Release();
		}
		else if(gsHandler)
		{
			// Note: since we've forced GS into running on this/main/libretro thread
			// we need to clear its queue, to prevent it from locking up VM
			while(m_virtualMachine->GetStatus() != CVirtualMachine::PAUSED)
			{
				std::this_thread::yield();
				gsHandler->Release();
			}
		}
		m_virtualMachine->DestroyPadHandler();
		m_virtualMachine->DestroyGSHandler();
		m_virtualMachine->DestroySoundHandler();
		m_virtualMachine->Destroy();
		g_widescreenConnection.reset();
		delete m_virtualMachine;
		m_virtualMachine = nullptr;
	}
	libretro_supports_bitmasks = false;
}
