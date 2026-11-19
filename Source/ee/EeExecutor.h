#pragma once

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <thread>
#elif defined(__unix__)
#include <signal.h>
#endif

#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../GenericMipsExecutor.h"
#include "../NativeCodeCache.h"

class CEeExecutor : public CGenericMipsExecutor<BlockLookupTwoWay>
{
public:
	using CachedBlockKey = std::pair<uint128, uint32>;
	using IdleLoopBlockMap = std::map<uint32, std::optional<CachedBlockKey>>;
	using BlockFpUseAccurateAddSubSet = std::set<uint32>;
	using BlockFpRoundingModeMap = std::map<uint32, Jitter::CJitter::ROUNDINGMODE>;

	CEeExecutor(CMIPS&, uint8*);
	virtual ~CEeExecutor();

	void SetBlockFpRoundingModes(BlockFpRoundingModeMap);
	void SetBlockFpUseAccurateAddSub(BlockFpUseAccurateAddSubSet);
	void SetIdleLoopBlocks(IdleLoopBlockMap);

	void AddExceptionHandler();
	void RemoveExceptionHandler();

	void AttachExceptionHandlerToThread();

	void Reset() override;
	void FlushInstructionCache() override;
	void ClearActiveBlocksInRange(uint32, uint32, bool) override;
	void PrepareRamOverwrite();
	void ReprotectActiveBlockPages();
	void SetDmaReadOnlyWriteFaultHandler(std::function<void()> handler);
	bool TryPinDmaReadOnlyRange(uint32 offset, uint32 size);
	size_t GetPageSize() const { return m_pageSize; }

	BasicBlockPtr BlockFactory(CMIPS&, uint32, uint32) override;

	void InitAotForElf(const std::string& exeName, uint32 entryPoint, const std::vector<std::pair<uint32, uint32>>& execSegments);
	void InitOverlayDiskCache(const std::string& exeName);
	void WarmPreloadOverlays();
	void SaveOverlayDiskCache(bool async = false);
	size_t GetPendingOverlayCount() const { return m_newOverlayEntries.size(); }

	struct AOT_STATS
	{
		uint32 elfBlocksLoaded = 0;
		uint32 elfBlocksCompiled = 0;
		uint32 overlayBlocksLoaded = 0;
		uint32 overlayHits = 0;
		uint32 overlayCompiled = 0;
		double initMs = 0.0;
	};
	const AOT_STATS& GetAotStats() const { return m_aotStats; }

	struct JIT_STATS
	{
		uint64 accessFaults = 0;
		uint64 factoryCalls = 0;
		uint64 cacheHits = 0;
		uint64 cacheCopyHits = 0;
		uint64 overlayHits = 0;
		uint64 compiles = 0;
		uint64 flushes = 0;
		uint64 flushInvalidated = 0;
		std::map<uint32, uint32> faultPages;
		std::map<uint32, uint32> compileAddrs;
	};
	void SetTrackJitStats(bool enable) { m_trackJitStats = enable; }
	void ResetJitStats() { m_jitStats = {}; }
	const JIT_STATS& GetJitStats() const { return m_jitStats; }

private:
	bool m_trackJitStats = false;
	JIT_STATS m_jitStats;
	typedef std::multimap<CachedBlockKey, BasicBlockPtr> CachedBlockMap;
	CachedBlockMap m_cachedBlocks;

	IdleLoopBlockMap m_idleLoopBlocks;
	BlockFpUseAccurateAddSubSet m_blockFpUseAccurateAddSub;
	BlockFpRoundingModeMap m_blockFpRoundingModes;

	uint8* m_ram = nullptr;
	size_t m_pageSize = 0;
	std::vector<uint8> m_pageDataFaults;
	std::vector<uint8> m_pageProtected;
	std::vector<uint8> m_dmaPageState; //0=writable, 1=pinned PROT_READ for zero-copy DMA, 2=dynamic
	std::vector<uint8> m_dmaPageFaults;
	std::function<void()> m_onDmaReadOnlyWriteFault;

	struct DiskOverlayKey
	{
		uint32 begin = 0;
		uint32 end = 0;
		uint64 hashLo = 0;
		uint64 hashHi = 0;

		bool operator<(const DiskOverlayKey& o) const
		{
			return std::tie(begin, end, hashLo, hashHi) < std::tie(o.begin, o.end, o.hashLo, o.hashHi);
		}
	};

	std::map<DiskOverlayKey, CACHED_NATIVE_BLOCK> m_diskOverlayCache;
	std::vector<CACHED_NATIVE_BLOCK> m_newOverlayEntries;
	std::string m_overlayCachePath;
	uint64 m_overlayAbiFingerprint = 0;
	bool m_overlayDirty = false;
	std::thread m_overlaySaveThread;
	AOT_STATS m_aotStats;

	bool IsBlockOverridden(uint32 start, uint32 end) const;
	void PartitionRangeStatic(uint32 startAddress, uint32 segEnd, uint32& outEndAddress, uint32& outBranchAddress, bool& outHasFallthrough) const;

	bool HandleAccessFault(intptr_t);
	void SetMemoryProtected(void*, size_t, bool);

#if defined(_WIN32)
	static LONG CALLBACK HandleException(_EXCEPTION_POINTERS*);
	LONG HandleExceptionInternal(_EXCEPTION_POINTERS*);

	LPVOID m_handler = NULL;
#elif defined(__unix__) || defined(__ANDROID__)
	static void HandleException(int, siginfo_t*, void*);
	void HandleExceptionInternal(int, siginfo_t*, void*);
#elif defined(__APPLE__) && !TARGET_OS_TV
	void HandlerThreadProc();

	mach_port_t m_port = MACH_PORT_NULL;
	std::thread m_handlerThread;
	std::atomic<bool> m_running = false;
#endif
};
