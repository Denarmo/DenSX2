//Runs a game headless from a save state and reports how fast the PS2VM thread emulates it.
//  vmbench <disc image> <save state> <frames> [button to hold]
//The save state is Play!'s own (an EmuDEN state file is accepted too: its header is skipped). The GS is the
//null one, sound is discarded, and the pad holds one button (cross by default) with the sticks centred.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "AppConfig.h"
#include "PathUtils.h"
#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "PtrStream.h"
#include "ee/PS2OS.h"
#include "ee/EeExecutor.h"
#include "gs/GSH_Null.h"
#include "PadHandler.h"
#include "ControllerInfo.h"
#include "sound/SoundHandler.h"

//Play!'s data (memory cards, configuration) go in the working directory
fs::path CAppConfig::GetBasePath() const
{
	static const auto basePath = []() {
		auto result = fs::current_path() / "vmbench-data";
		Framework::PathUtils::EnsurePathExists(result);
		return result;
	}();
	return basePath;
}

namespace
{
	class CPH_Hold : public CPadHandler
	{
	public:
		explicit CPH_Hold(PS2::CControllerInfo::BUTTON button)
		    : m_button(button)
		{
		}

		void Update(uint8* ram) override
		{
			for(auto* listener : m_interfaces)
			{
				for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
				{
					auto button = static_cast<PS2::CControllerInfo::BUTTON>(i);
					if(PS2::CControllerInfo::IsAxis(button))
					{
						listener->SetAxisState(0, button, 0x7F, ram);
					}
					else
					{
						listener->SetButtonState(0, button, button == m_button, ram);
					}
				}
			}
		}

	private:
		PS2::CControllerInfo::BUTTON m_button;
	};

	class CSH_Discard : public CSoundHandler
	{
	public:
		void Reset() override
		{
		}
		void Write(int16*, unsigned int, unsigned int) override
		{
		}
		bool HasFreeBuffers() override
		{
			return true;
		}
		void RecycleBuffers() override
		{
		}
	};

	double NowSeconds()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void Step(const char* name)
	{
		fprintf(stderr, "vmbench: %s\n", name);
		fflush(stderr);
	}
}

static int Run(int argc, const char** argv)
{
	if(argc < 4)
	{
		printf("usage: vmbench <disc image> <save state> <frames> [hold button:cross|square|circle|triangle|none]\n");
		return 1;
	}
	std::string discPath = argv[1];
	std::string statePath = argv[2];
	int frames = atoi(argv[3]);
	auto holdButton = PS2::CControllerInfo::CROSS;
	if(argc >= 5)
	{
		std::string name = argv[4];
		if(name == "square") holdButton = PS2::CControllerInfo::SQUARE;
		else if(name == "circle") holdButton = PS2::CControllerInfo::CIRCLE;
		else if(name == "triangle") holdButton = PS2::CControllerInfo::TRIANGLE;
		else if(name == "none") holdButton = PS2::CControllerInfo::MAX_BUTTONS;
	}

	std::vector<uint8> state;
	{
		std::ifstream file(statePath, std::ios::binary);
		state.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}
	size_t stateOffset = 0;
	if((state.size() > 9) && (memcmp(state.data(), "DENSTAT1", 8) == 0))
	{
		stateOffset = 9 + state[8];
	}
	if(state.size() <= stateOffset)
	{
		printf("can't read the save state %s\n", statePath.c_str());
		return 1;
	}

	Step("initialize");
	CPS2VM virtualMachine;
	virtualMachine.Initialize();
	//The VM registers these preferences
	CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH, discPath);
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_PS2_LIMIT_FRAMERATE, getenv("VMBENCH_LIMIT") != nullptr);
	virtualMachine.ReloadFrameRateLimit();
	virtualMachine.CreateGSHandler(CGSH_Null::GetFactoryFunction());
	virtualMachine.CreatePadHandler([holdButton]() { return new CPH_Hold(holdButton); });
	virtualMachine.CreateSoundHandler([]() { return new CSH_Discard(); });

	Step("boot");
	virtualMachine.Reset();
	virtualMachine.m_ee->m_os->BootFromCDROM();

	Step("load state");
	{
		Framework::CPtrStream stateStream(state.data() + stateOffset, state.size() - stateOffset);
		auto result = virtualMachine.LoadState(stateStream);
		if(!result.get())
		{
			printf("loading the save state failed\n");
			return 1;
		}
	}

	std::mutex mutex;
	std::condition_variable done;
	int frameCount = 0;
	double start = 0;
	const int warmup = 100;
	uint64 eeTotalTicks = 0, eeIdleTicks = 0, iopTotalTicks = 0, iopIdleTicks = 0;
	std::atomic<bool> benchActive{false};
	auto connection = virtualMachine.OnNewFrame.Connect([&]() {
		std::lock_guard<std::mutex> lock(mutex);
		frameCount++;
		if(frameCount == warmup)
		{
			start = NowSeconds();
			if(getenv("VMBENCH_SAMPLE") != nullptr)
			{
				auto* eeExec = static_cast<CEeExecutor*>(virtualMachine.m_ee->m_EE.m_executor.get());
				eeExec->ResetJitStats();
				eeExec->SetTrackJitStats(true);
			}
			benchActive = true;
		}
		if(frameCount > warmup && frameCount <= warmup + frames)
		{
			auto u = virtualMachine.GetCpuUtilisationInfo();
			eeTotalTicks += u.eeTotalTicks;
			eeIdleTicks += u.eeIdleTicks;
			iopTotalTicks += u.iopTotalTicks;
			iopIdleTicks += u.iopIdleTicks;
		}
		if(frameCount == warmup + frames)
		{
			benchActive = false;
			if(getenv("VMBENCH_SAMPLE") != nullptr)
			{
				auto* eeExec = static_cast<CEeExecutor*>(virtualMachine.m_ee->m_EE.m_executor.get());
				eeExec->SetTrackJitStats(false);
			}
			done.notify_all();
		}
	});

	bool samplePc = getenv("VMBENCH_SAMPLE") != nullptr;
	std::unordered_map<uint32, uint32> eePcHist;
	uint32 vu1RunningSamples = 0, totalSamples = 0;
	std::atomic<bool> stopSampler{false};
	std::thread samplerThread;
	if(samplePc)
	{
		samplerThread = std::thread([&]() {
			while(!stopSampler.load(std::memory_order_relaxed))
			{
				if(benchActive.load(std::memory_order_relaxed))
				{
					uint32 pc = virtualMachine.m_ee->m_EE.m_State.nPC;
					eePcHist[pc & ~0x3FU]++;
					if(virtualMachine.m_ee->m_vpu1->IsVuRunning()) vu1RunningSamples++;
					totalSamples++;
				}
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}
		});
	}

	Step("run");
	virtualMachine.Resume();
	{
		std::unique_lock<std::mutex> lock(mutex);
		done.wait(lock, [&]() { return frameCount >= warmup + frames; });
	}
	stopSampler = true;
	if(samplerThread.joinable()) samplerThread.join();
	double elapsed = NowSeconds() - start;
	virtualMachine.Pause();
	printf("%d frames in %.3f s: %.2f fps, %.3f ms per frame (exe=%s, eeIdle=%.1f%%, iopIdle=%.1f%%)\n",
	       frames, elapsed, frames / elapsed, 1000.0 * elapsed / frames,
	       virtualMachine.m_ee->m_os->GetExecutableName(),
	       eeTotalTicks ? (100.0 * eeIdleTicks / eeTotalTicks) : 0.0,
	       iopTotalTicks ? (100.0 * iopIdleTicks / iopTotalTicks) : 0.0);
	if(samplePc && totalSamples > 0)
	{
		std::vector<std::pair<uint32, uint32>> top(eePcHist.begin(), eePcHist.end());
		std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		printf("PC samples=%u vu1Running=%.1f%% top EE 64B buckets:\n", totalSamples, 100.0 * vu1RunningSamples / totalSamples);
		for(size_t i = 0; i < std::min<size_t>(15, top.size()); i++)
		{
			uint32 addr = top[i].first;
			printf("  0x%08X: %.2f%% (%u)", addr, 100.0 * top[i].second / totalSamples, top[i].second);
			if(addr + 64 <= PS2::EE_RAM_SIZE)
			{
				const uint32* words = reinterpret_cast<const uint32*>(virtualMachine.m_ee->m_ram + addr);
				printf(" words:");
				for(int k = 0; k < 8; k++) printf(" %08X", words[k]);
			}
			printf("\n");
		}
		auto* eeExec = static_cast<CEeExecutor*>(virtualMachine.m_ee->m_EE.m_executor.get());
		const auto& js = eeExec->GetJitStats();
		printf("EE JIT stats: faults=%llu factory=%llu cacheHits=%llu cacheCopy=%llu compiles=%llu\n",
		       (unsigned long long)js.accessFaults, (unsigned long long)js.factoryCalls,
		       (unsigned long long)js.cacheHits, (unsigned long long)js.cacheCopyHits,
		       (unsigned long long)js.compiles);
		{
			std::vector<std::pair<uint32, uint32>> fp(js.faultPages.begin(), js.faultPages.end());
			std::sort(fp.begin(), fp.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			printf("Top fault pages:\n");
			for(size_t i = 0; i < std::min<size_t>(10, fp.size()); i++)
				printf("  page 0x%08X: %u faults\n", fp[i].first, fp[i].second);
		}
		{
			std::vector<std::pair<uint32, uint32>> ca(js.compileAddrs.begin(), js.compileAddrs.end());
			std::sort(ca.begin(), ca.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			printf("Top compiled EE blocks:\n");
			for(size_t i = 0; i < std::min<size_t>(15, ca.size()); i++)
				printf("  addr 0x%08X: %u compiles\n", ca[i].first, ca[i].second);
		}
	}
	fflush(stdout);

	connection.reset();
	virtualMachine.DestroyPadHandler();
	virtualMachine.DestroySoundHandler();
	virtualMachine.DestroyGSHandler();
	virtualMachine.Destroy();
	return 0;
}

int main(int argc, const char** argv)
{
	try
	{
		return Run(argc, argv);
	}
	catch(const std::exception& exception)
	{
		printf("error: %s\n", exception.what());
		fflush(stdout);
		return 1;
	}
}
