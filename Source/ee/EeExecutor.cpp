#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_set>
#include "EeExecutor.h"
#include "../Ps2Const.h"
#include "AlignedAlloc.h"
#include "EeBasicBlock.h"
#include "MA_EE.h"
#include "filesystem_def.h"
#include "xxhash.h"

#if defined(__unix__) || defined(__ANDROID__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

#if defined(__APPLE__)

#include <TargetConditionals.h>

#if TARGET_CPU_ARM
#define DISABLE_PROTECTION
#define STATE_FLAVOR ARM_THREAD_STATE32
#define STATE_FLAVOR_COUNT ARM_THREAD_STATE32_COUNT
#elif TARGET_CPU_ARM64
#define STATE_FLAVOR ARM_THREAD_STATE64
#define STATE_FLAVOR_COUNT ARM_THREAD_STATE64_COUNT
#elif TARGET_CPU_X86
#define STATE_FLAVOR x86_THREAD_STATE32
#define STATE_FLAVOR_COUNT x86_THREAD_STATE32_COUNT
#elif TARGET_CPU_X86_64
#define STATE_FLAVOR x86_THREAD_STATE64
#define STATE_FLAVOR_COUNT x86_THREAD_STATE64_COUNT
#else
#error Unsupported CPU architecture
#endif

#if TARGET_OS_TV
#define DISABLE_PROTECTION
#endif

#endif

static CEeExecutor* g_eeExecutor = nullptr;

//Where a block jumps when its target isn't compiled: back to the executor loop, which compiles it (jumping
//to the empty block's code would compile and run it nested in this call, without bound)
static void ReturnToExecutor(void*)
{
}

CEeExecutor::CEeExecutor(CMIPS& context, uint8* ram)
    : CGenericMipsExecutor(context, 0x20000000, BLOCK_CATEGORY_PS2_EE)
    , m_ram(ram)
{
	void* returnToExecutor = reinterpret_cast<void*>(&ReturnToExecutor);
	if(SetupBlockFixedFrame(context))
	{
		returnToExecutor = context.m_fixedFrameExit;
		//Made by the base class before the frame existed
		m_emptyBlock->Compile();
	}
	m_blockLookup.EnableCodeTable(returnToExecutor);
	context.m_blockCodeLookup = m_blockLookup.GetCodeTable();
	context.m_blockCodeLookupMask = m_addressMask;
	context.m_ramBase = m_ram;
	m_pageSize = framework_getpagesize();
	size_t pageCount = (PS2::EE_RAM_SIZE + m_pageSize - 1) / m_pageSize;
	m_pageDataFaults.assign(pageCount, 0);
	m_pageProtected.assign(pageCount, 0);
	m_dmaPageState.assign(pageCount, 0);
	m_dmaPageFaults.assign(pageCount, 0);
}

CEeExecutor::~CEeExecutor()
{
	SaveOverlayDiskCache(false);
	if(m_overlaySaveThread.joinable())
	{
		m_overlaySaveThread.join();
	}
	m_context.m_blockCodeLookup = nullptr;
	m_context.m_ramBase = nullptr;
	m_context.m_fixedFrameEntry = nullptr;
	m_context.m_fixedFrameExit = nullptr;
}

void CEeExecutor::SetBlockFpRoundingModes(BlockFpRoundingModeMap blockFpRoundingModes)
{
	m_blockFpRoundingModes = std::move(blockFpRoundingModes);
}

void CEeExecutor::SetBlockFpUseAccurateAddSub(BlockFpUseAccurateAddSubSet blockFpUseAccurateAddSub)
{
	m_blockFpUseAccurateAddSub = std::move(blockFpUseAccurateAddSub);
}

void CEeExecutor::SetIdleLoopBlocks(IdleLoopBlockMap idleLoopBlocks)
{
	m_idleLoopBlocks = std::move(idleLoopBlocks);
}

void CEeExecutor::AddExceptionHandler()
{
	assert(g_eeExecutor == nullptr);
	g_eeExecutor = this;

#ifdef DISABLE_PROTECTION
	return;
#endif

#if defined(_WIN32)
	m_handler = AddVectoredExceptionHandler(TRUE, &CEeExecutor::HandleException);
	assert(m_handler != NULL);
#elif defined(__unix__) || defined(__ANDROID__)
	struct sigaction sigAction;
	sigAction.sa_handler = nullptr;
	sigAction.sa_sigaction = &HandleException;
	sigAction.sa_flags = SA_SIGINFO;
	sigemptyset(&sigAction.sa_mask);
	int result = sigaction(SIGSEGV, &sigAction, nullptr);
	assert(result >= 0);
#elif defined(__APPLE__) && !TARGET_OS_TV
	if(!m_running)
	{
		kern_return_t result = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &m_port);
		assert(result == KERN_SUCCESS);

		m_running = true;
		m_handlerThread = std::thread([this]() { HandlerThreadProc(); });
	}

	AttachExceptionHandlerToThread();
#endif
}

void CEeExecutor::RemoveExceptionHandler()
{
#ifndef DISABLE_PROTECTION

#if defined(_WIN32)
	RemoveVectoredExceptionHandler(m_handler);
#elif defined(__APPLE__) && !TARGET_OS_TV
	m_running = false;
	m_handlerThread.join();
#endif

#endif //!DISABLE_PROTECTION

	g_eeExecutor = nullptr;
}

void CEeExecutor::AttachExceptionHandlerToThread()
{
	//Only necessary for macOS and iOS since the handler is set on a thread basis
#if defined(__APPLE__) && !TARGET_OS_TV
	assert(m_running);

	auto result = mach_port_insert_right(mach_task_self(), m_port, m_port, MACH_MSG_TYPE_MAKE_SEND);
	assert(result == KERN_SUCCESS);

	result = thread_set_exception_ports(mach_thread_self(), EXC_MASK_BAD_ACCESS, m_port, EXCEPTION_STATE | MACH_EXCEPTION_CODES, STATE_FLAVOR);
	assert(result == KERN_SUCCESS);

	result = mach_port_mod_refs(mach_task_self(), m_port, MACH_PORT_RIGHT_SEND, -1);
	assert(result == KERN_SUCCESS);
#endif
}

void CEeExecutor::Reset()
{
	SaveOverlayDiskCache(false);
	if(m_overlaySaveThread.joinable())
	{
		m_overlaySaveThread.join();
	}
	if(m_onDmaReadOnlyWriteFault)
	{
		m_onDmaReadOnlyWriteFault();
	}
	SetMemoryProtected(m_ram, PS2::EE_RAM_SIZE, false);
	std::fill(m_pageDataFaults.begin(), m_pageDataFaults.end(), 0);
	std::fill(m_dmaPageState.begin(), m_dmaPageState.end(), 0);
	std::fill(m_dmaPageFaults.begin(), m_dmaPageFaults.end(), 0);
	m_cachedBlocks.clear();
	m_blockFpRoundingModes.clear();
	m_idleLoopBlocks.clear();
	m_diskOverlayCache.clear();
	m_newOverlayEntries.clear();
	m_overlayCachePath.clear();
	m_overlayAbiFingerprint = 0;
	m_overlayDirty = false;
	m_aotStats = {};
	CGenericMipsExecutor::Reset();
}

void CEeExecutor::PrepareRamOverwrite()
{
	if(m_onDmaReadOnlyWriteFault)
	{
		m_onDmaReadOnlyWriteFault();
	}
	SetMemoryProtected(m_ram, PS2::EE_RAM_SIZE, false);
	std::fill(m_pageDataFaults.begin(), m_pageDataFaults.end(), 0);
	std::fill(m_dmaPageState.begin(), m_dmaPageState.end(), 0);
	std::fill(m_dmaPageFaults.begin(), m_dmaPageFaults.end(), 0);
}

void CEeExecutor::SetDmaReadOnlyWriteFaultHandler(std::function<void()> handler)
{
	m_onDmaReadOnlyWriteFault = std::move(handler);
}

bool CEeExecutor::TryPinDmaReadOnlyRange(uint32 offset, uint32 size)
{
#if defined(DISABLE_PROTECTION) || defined(__EMSCRIPTEN__)
	(void)offset;
	(void)size;
	return false;
#else
	if(m_pageSize == 0 || m_dmaPageState.empty() || size == 0 || offset >= PS2::EE_RAM_SIZE || (offset + size) > PS2::EE_RAM_SIZE)
	{
		return false;
	}
	size_t firstPage = offset / m_pageSize;
	size_t lastPage = (offset + size - 1) / m_pageSize;
	if(lastPage >= m_dmaPageState.size())
	{
		return false;
	}
	bool needsProtect = false;
	for(size_t p = firstPage; p <= lastPage; p++)
	{
		uint8 st = m_dmaPageState[p];
		if(st == 2) return false;
		if(st == 0) needsProtect = true;
	}
	if(!needsProtect)
	{
		return true;
	}
	size_t p = firstPage;
	while(p <= lastPage)
	{
		if(m_dmaPageState[p] != 0)
		{
			p++;
			continue;
		}
		size_t runStart = p;
		while(p <= lastPage && m_dmaPageState[p] == 0)
		{
			m_dmaPageState[p] = 1;
			p++;
		}
		SetMemoryProtected(m_ram + (runStart * m_pageSize), (p - runStart) * m_pageSize, true);
	}
	return true;
#endif
}

void CEeExecutor::ReprotectActiveBlockPages()
{
	if(m_pageSize == 0) return;
	std::vector<uint8> touched(m_pageDataFaults.size(), 0);
	for(const auto& block : m_blocks)
	{
		uint32 begin = block->GetBeginAddress();
		uint32 end = block->GetEndAddress();
		if(begin < 0x100000 || begin >= PS2::EE_RAM_SIZE) continue;
		size_t firstPage = begin / m_pageSize;
		size_t lastPage = std::min<uint32>(end, PS2::EE_RAM_SIZE - 1) / m_pageSize;
		for(size_t p = firstPage; p <= lastPage && p < touched.size(); p++)
		{
			touched[p] = 1;
		}
	}
	for(size_t p = 0; p < touched.size(); p++)
	{
		if(touched[p] && m_pageDataFaults[p] < 8)
		{
			SetMemoryProtected(m_ram + (p * m_pageSize), m_pageSize, true);
		}
	}
}

void CEeExecutor::FlushInstructionCache()
{
	if(m_trackJitStats) m_jitStats.flushes++;
	//Preserve all active blocks, links, AOT code, and overlay caches whose EE RAM bytes did not change.
	//Only invalidate blocks whose instructions actually changed (e.g. newly loaded overlays / SMC).
	std::vector<BasicBlockPtr> staleBlocks;
	for(const auto& block : m_blocks)
	{
		uint32 begin = block->GetBeginAddress();
		uint32 end = block->GetEndAddress();
		uint64 expectedHash = block->GetCodeHash64();
		if(expectedHash != 0 && begin <= end && end < PS2::EE_RAM_SIZE)
		{
			uint32 blockSize = (end - begin) + 4;
			uint64 curHash = XXH3_128bits(m_ram + begin, blockSize).low64;
			if(curHash == expectedHash)
			{
				continue;
			}
		}
		staleBlocks.push_back(block);
	}

	if(staleBlocks.empty()) return;
	if(m_trackJitStats) m_jitStats.flushInvalidated += staleBlocks.size();

	for(const auto& block : staleBlocks)
	{
		m_blockLookup.DeleteBlock(block.get());
		OrphanBlock(block.get());
		if(m_pageSize != 0)
		{
			uint32 begin = block->GetBeginAddress();
			uint32 end = block->GetEndAddress();
			if(begin < PS2::EE_RAM_SIZE)
			{
				size_t firstPage = begin / m_pageSize;
				size_t lastPage = std::min<uint32>(end, PS2::EE_RAM_SIZE - 1) / m_pageSize;
				for(size_t p = firstPage; p <= lastPage && p < m_pageDataFaults.size(); p++)
				{
					if(p < m_dmaPageState.size() && m_dmaPageState[p] == 1)
					{
						if(m_onDmaReadOnlyWriteFault) m_onDmaReadOnlyWriteFault();
						m_dmaPageState[p] = 0;
					}
					SetMemoryProtected(m_ram + (p * m_pageSize), m_pageSize, false);
					m_pageDataFaults[p] = 0;
				}
			}
		}
	}

	for(const auto& block : staleBlocks)
	{
		auto lowerBound = m_blockOutLinks.lower_bound(block->GetBeginAddress());
		auto upperBound = m_blockOutLinks.upper_bound(block->GetBeginAddress());
		for(auto it = lowerBound; it != upperBound; ++it)
		{
			auto& blockLink = it->second;
			if(!blockLink.live) continue;
			auto referringBlock = m_blockLookup.FindBlockAt(blockLink.srcAddress);
			if(referringBlock->IsEmpty()) continue;
			referringBlock->UnlinkBlock(blockLink.slot);
			blockLink.live = false;
		}
	}

	for(const auto& block : staleBlocks)
	{
		m_blocks.erase(block);
	}
}

bool CEeExecutor::IsBlockOverridden(uint32 start, uint32 end) const
{
	if(m_context.HasBreakpointInRange(start, end)) return true;
	if(m_blockFpRoundingModes.count(start) != 0) return true;
	if(m_idleLoopBlocks.count(start) != 0) return true;
	if(m_blockFpUseAccurateAddSub.count(start) != 0) return true;
	return false;
}

void CEeExecutor::PartitionRangeStatic(uint32 startAddress, uint32 segEnd, uint32& outEndAddress, uint32& outBranchAddress, bool& outHasFallthrough) const
{
	uint32 maxEnd = std::min<uint32>(startAddress + MAX_BLOCK_SIZE, segEnd - 4);
	uint32 endAddress = maxEnd;
	uint32 branchAddress = MIPS_INVALID_PC;
	bool hasFallthrough = true;

	for(uint32 address = startAddress; address < maxEnd; address += 4)
	{
		uint32 opcode = m_context.m_pMemoryMap->GetInstruction(address);
		auto branchType = m_context.m_pArch->IsInstructionBranch(&m_context, address, opcode);
		if(branchType == MIPS_BRANCH_NORMAL)
		{
			branchAddress = m_context.m_pArch->GetInstructionEffectiveAddress(&m_context, address, opcode);
			endAddress = address + 4;
			if(endAddress < segEnd)
			{
				uint32 endOpcode = m_context.m_pMemoryMap->GetInstruction(endAddress);
				auto endBranchType = m_context.m_pArch->IsInstructionBranch(&m_context, endAddress, endOpcode);
				if(endBranchType == MIPS_BRANCH_NORMAL)
				{
					endAddress = address;
				}
			}
			else
			{
				endAddress = address;
			}
			uint32 op = (opcode >> 26) & 0x3F;
			uint32 rs = (opcode >> 21) & 0x1F;
			uint32 rt = (opcode >> 16) & 0x1F;
			uint32 funct = opcode & 0x3F;
			if((op == 0x02) || ((op == 0x00) && (funct == 0x08)) ||
			   ((op == 0x04) && (rs == rt)) ||
			   ((op == 0x01) && (rs == 0) && ((rt == 0x01) || (rt == 0x03))) ||
			   ((op == 0x06) && (rs == 0)))
			{
				hasFallthrough = false;
			}
			break;
		}
		else if(branchType == MIPS_BRANCH_NODELAY)
		{
			endAddress = address;
			break;
		}
	}

	outEndAddress = endAddress;
	outBranchAddress = branchAddress;
	outHasFallthrough = hasFallthrough;
}

void CEeExecutor::InitOverlayDiskCache(const std::string& exeName)
{
	if(!CNativeCodeCache::IsSupported() || exeName.empty()) return;
	auto sanitized = CNativeCodeCache::SanitizeTag(exeName);
	if(sanitized.empty()) return;

	uint64 abiFp = CNativeCodeCache::ComputeAbiFingerprint(m_context);
	auto path = (fs::path(CNativeCodeCache::GetCacheDir()) / (sanitized + "-" + CNativeCodeCache::GetArchTag() + ".ovl.aot")).string();
	if(path == m_overlayCachePath && abiFp == m_overlayAbiFingerprint && !m_diskOverlayCache.empty())
	{
		return;
	}

	m_overlayCachePath = path;
	m_overlayAbiFingerprint = abiFp;
	m_diskOverlayCache.clear();
	m_newOverlayEntries.clear();
	m_overlayDirty = false;

	std::vector<CACHED_NATIVE_BLOCK> loaded;
	if(CNativeCodeCache::LoadFile(m_overlayCachePath, m_overlayAbiFingerprint, 0, loaded))
	{
		for(auto& entry : loaded)
		{
			DiskOverlayKey key{entry.begin, entry.end, entry.codeHashLo, entry.codeHashHi};
			m_diskOverlayCache.emplace(key, std::move(entry));
		}
		m_aotStats.overlayBlocksLoaded = static_cast<uint32>(m_diskOverlayCache.size());
	}
	WarmPreloadOverlays();
}

void CEeExecutor::WarmPreloadOverlays()
{
#if defined(_WIN32)
	return;
#else
	if(m_diskOverlayCache.empty()) return;
	struct PendingLink
	{
		uint32 begin;
		uint32 end;
		uint32 branchTarget;
	};
	std::vector<PendingLink> pendingLinks;
	pendingLinks.reserve(m_diskOverlayCache.size());
	for(const auto& kv : m_diskOverlayCache)
	{
		const auto& entry = kv.second;
		if(entry.end < entry.begin || entry.end >= PS2::EE_RAM_SIZE || (entry.begin & 3U) != 0 || (entry.end & 3U) != 0)
		{
			continue;
		}
		if(IsBlockOverridden(entry.begin, entry.end) || HasBlockAt(entry.begin))
		{
			continue;
		}
		uint32 blockSize = (entry.end - entry.begin) + 4;
		auto xxHash = XXH3_128bits(m_ram + entry.begin, blockSize);
		if(xxHash.low64 != entry.codeHashLo || xxHash.high64 != entry.codeHashHi)
		{
			continue;
		}
		auto block = std::make_shared<CEeBasicBlock>(m_context, entry.begin, entry.end, m_blockCategory);
		if(!block->LoadFromNativeCache(entry))
		{
			continue;
		}
		block->SetCodeHash64(xxHash.low64);
		uint128 hash;
		memcpy(&hash, &xxHash, sizeof(xxHash));
		m_cachedBlocks.insert(std::make_pair(std::make_pair(hash, blockSize), block));

		ResetBlockOutLinks(block.get());
		m_blockLookup.AddBlock(block.get());
		m_blocks.insert(block);

		uint32 branchTarget = MIPS_INVALID_PC;
		if(entry.end >= entry.begin + 4)
		{
			uint32 brAddr = entry.end - 4;
			uint32 brOp = m_context.m_pMemoryMap->GetInstruction(brAddr);
			if(m_context.m_pArch->IsInstructionBranch(&m_context, brAddr, brOp) == MIPS_BRANCH_NORMAL)
			{
				branchTarget = m_context.m_pArch->GetInstructionEffectiveAddress(&m_context, brAddr, brOp);
			}
		}
		pendingLinks.push_back({entry.begin, entry.end, branchTarget});
		m_aotStats.overlayHits++;
	}
	for(const auto& pl : pendingLinks)
	{
		SetupBlockLinks(pl.begin, pl.end, pl.branchTarget);
	}
#endif
}

void CEeExecutor::SaveOverlayDiskCache(bool async)
{
	if(m_overlaySaveThread.joinable())
	{
		m_overlaySaveThread.join();
	}
	if(!m_overlayDirty || m_overlayCachePath.empty() || m_newOverlayEntries.empty()) return;
	if(!CNativeCodeCache::IsSupported()) return;

	for(auto& entry : m_newOverlayEntries)
	{
		DiskOverlayKey key{entry.begin, entry.end, entry.codeHashLo, entry.codeHashHi};
		m_diskOverlayCache.emplace(key, std::move(entry));
	}
	m_newOverlayEntries.clear();

	std::vector<CACHED_NATIVE_BLOCK> allEntries;
	allEntries.reserve(m_diskOverlayCache.size());
	for(const auto& kv : m_diskOverlayCache)
	{
		allEntries.push_back(kv.second);
	}
	m_overlayDirty = false;
	std::string savePath = m_overlayCachePath;
	uint64 abiFp = m_overlayAbiFingerprint;
	if(async)
	{
		m_overlaySaveThread = std::thread([savePath = std::move(savePath), abiFp, entries = std::move(allEntries)]() {
			CNativeCodeCache::SaveFileAtomic(savePath, abiFp, 0, entries);
		});
	}
	else
	{
		CNativeCodeCache::SaveFileAtomic(savePath, abiFp, 0, allEntries);
	}
}

void CEeExecutor::InitAotForElf(const std::string& exeName, uint32 entryPoint, const std::vector<std::pair<uint32, uint32>>& execSegments)
{
	if(!CNativeCodeCache::IsSupported() || exeName.empty() || execSegments.empty()) return;
	auto t0 = std::chrono::steady_clock::now();

	InitOverlayDiskCache(exeName);

	//Filter valid RAM segments and compute a content hash of the loaded executable code
	std::vector<std::pair<uint32, uint32>> validSegs;
	XXH3_state_t* hashState = XXH3_createState();
	XXH3_64bits_reset(hashState);
	for(const auto& seg : execSegments)
	{
		uint32 s = seg.first & ~3U;
		uint32 e = std::min<uint32>(seg.second & ~3U, PS2::EE_RAM_SIZE);
		if(e <= s) continue;
		validSegs.emplace_back(s, e);
		uint32 meta[2] = {s, e};
		XXH3_64bits_update(hashState, meta, sizeof(meta));
		XXH3_64bits_update(hashState, m_ram + s, e - s);
	}
	uint64 elfHash = XXH3_64bits_digest(hashState);
	XXH3_freeState(hashState);
	if(validSegs.empty()) return;

	auto findContainingSegment = [&](uint32 addr, uint32& outSegEnd) -> bool {
		if((addr & 3U) != 0) return false;
		for(const auto& seg : validSegs)
		{
			if(addr >= seg.first && addr < seg.second)
			{
				outSegEnd = seg.second;
				return true;
			}
		}
		return false;
	};

	uint64 abiFp = CNativeCodeCache::ComputeAbiFingerprint(m_context);
	char hashBuf[24];
	snprintf(hashBuf, sizeof(hashBuf), "%016llx", static_cast<unsigned long long>(elfHash));
	auto sanitized = CNativeCodeCache::SanitizeTag(exeName);
	auto aotPath = (fs::path(CNativeCodeCache::GetCacheDir()) /
	                (sanitized + "-" + hashBuf + "-" + CNativeCodeCache::GetArchTag() + ".ee.aot")).string();

	struct PendingLink
	{
		uint32 begin;
		uint32 end;
		uint32 branchTarget;
	};
	std::vector<PendingLink> pendingLinks;
	std::vector<bool> touchedPages;
	if(m_pageSize != 0)
	{
		touchedPages.assign((PS2::EE_RAM_SIZE + m_pageSize - 1) / m_pageSize, false);
	}
	auto markPagesTouched = [&](uint32 start, uint32 end) {
		if(start >= 0x100000 && start < PS2::EE_RAM_SIZE && m_pageSize != 0)
		{
			size_t firstPage = start / m_pageSize;
			size_t lastPage = std::min<uint32>(end, PS2::EE_RAM_SIZE - 1) / m_pageSize;
			for(size_t p = firstPage; p <= lastPage && p < touchedPages.size(); p++)
			{
				touchedPages[p] = true;
			}
		}
	};

	auto computeBlockHash = [&](uint32 start, uint32 end, uint64& outLo, uint64& outHi) {
		uint32 blockSize = (end - start) + 4;
		auto xxHash = XXH3_128bits(m_ram + start, blockSize);
		outLo = xxHash.low64;
		outHi = xxHash.high64;
	};

	//Warm launch: load precompiled native machine code directly from disk
	std::vector<CACHED_NATIVE_BLOCK> loadedBlocks;
	if(CNativeCodeCache::LoadFile(aotPath, abiFp, elfHash, loadedBlocks) && !loadedBlocks.empty())
	{
#if defined(_WIN32)
		//Windows allocates per-function VirtualAlloc pages; store in overlay map for zero-JIT lazy materialization.
		for(auto& entry : loadedBlocks)
		{
			DiskOverlayKey key{entry.begin, entry.end, entry.codeHashLo, entry.codeHashHi};
			m_diskOverlayCache.emplace(key, std::move(entry));
		}
		m_aotStats.elfBlocksLoaded = static_cast<uint32>(loadedBlocks.size());
#else
		pendingLinks.reserve(loadedBlocks.size());
		uint32 loadedCount = 0;
		for(const auto& entry : loadedBlocks)
		{
			if(entry.end < entry.begin || entry.end >= PS2::EE_RAM_SIZE || (entry.begin & 3U) != 0 || (entry.end & 3U) != 0)
			{
				continue;
			}
			if(IsBlockOverridden(entry.begin, entry.end) || HasBlockAt(entry.begin))
			{
				continue;
			}
			uint64 hashLo = 0, hashHi = 0;
			computeBlockHash(entry.begin, entry.end, hashLo, hashHi);
			if(hashLo != entry.codeHashLo || hashHi != entry.codeHashHi)
			{
				continue;
			}
			auto block = std::make_shared<CEeBasicBlock>(m_context, entry.begin, entry.end, m_blockCategory);
			if(!block->LoadFromNativeCache(entry))
			{
				continue;
			}
			block->SetCodeHash64(hashLo);
			uint32 blockSize = (entry.end - entry.begin) + 4;
			uint128 hash;
			uint64 parts[2] = {hashLo, hashHi};
			memcpy(&hash, parts, sizeof(hash));
			m_cachedBlocks.insert(std::make_pair(std::make_pair(hash, blockSize), block));

			ResetBlockOutLinks(block.get());
			m_blockLookup.AddBlock(block.get());
			m_blocks.insert(block);
			markPagesTouched(entry.begin, entry.end);

			uint32 branchTarget = MIPS_INVALID_PC;
			if(entry.end >= entry.begin + 4)
			{
				uint32 brAddr = entry.end - 4;
				uint32 brOp = m_context.m_pMemoryMap->GetInstruction(brAddr);
				if(m_context.m_pArch->IsInstructionBranch(&m_context, brAddr, brOp) == MIPS_BRANCH_NORMAL)
				{
					branchTarget = m_context.m_pArch->GetInstructionEffectiveAddress(&m_context, brAddr, brOp);
				}
			}
			pendingLinks.push_back({entry.begin, entry.end, branchTarget});
			loadedCount++;
		}
		for(const auto& pl : pendingLinks)
		{
			SetupBlockLinks(pl.begin, pl.end, pl.branchTarget);
		}
		for(size_t p = 0; p < touchedPages.size(); p++)
		{
			if(touchedPages[p] && m_pageDataFaults[p] < 8)
			{
				SetMemoryProtected(m_ram + (p * m_pageSize), m_pageSize, true);
			}
		}
		m_aotStats.elfBlocksLoaded = loadedCount;
#endif
		m_aotStats.initMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		return;
	}

	//First launch: discover reachable blocks via per-function DFS so complete functions (including
	//all internal branches/fallthroughs) are compiled before moving to later functions.
	constexpr size_t MAX_AOT_BLOCKS = 131072;
	std::vector<uint32> seeds;
	seeds.reserve(65536);
	std::unordered_set<uint32> seedSet;
	seedSet.reserve(65536);

	auto addSeed = [&](uint32 addr) {
		uint32 segEnd = 0;
		if(!findContainingSegment(addr, segEnd)) return;
		if(seedSet.insert(addr).second)
		{
			seeds.push_back(addr);
		}
	};

	addSeed(entryPoint & m_addressMask);
	//Single ascending pass collects JAL targets, stack prologues, and post-JR-$ra leaf starts so
	//vtable/function-pointer leaf functions are precompiled alongside directly called functions.
	for(const auto& seg : validSegs)
	{
		for(uint32 addr = seg.first; addr < seg.second; addr += 4)
		{
			uint32 word = *reinterpret_cast<const uint32*>(m_ram + addr);
			if((word >> 26) == 0x03U)
			{
				uint32 target = (addr & 0xF0000000U) | ((word & 0x03FFFFFFU) << 2);
				addSeed(target);
			}
			else if(((word & 0xFFFF8000U) == 0x27BD8000U) || ((word & 0xFFFF8000U) == 0x67BD8000U))
			{
				addSeed(addr);
			}
			else if(word == 0x03E00008U)
			{
				//JR $ra: skip delay slot + up to 3 NOP alignment words to seed next leaf function.
				uint32 nextAddr = addr + 8;
				for(int pad = 0; pad < 3 && nextAddr < seg.second; pad++)
				{
					if(*reinterpret_cast<const uint32*>(m_ram + nextAddr) != 0) break;
					nextAddr += 4;
				}
				if(nextAddr < seg.second && (*reinterpret_cast<const uint32*>(m_ram + nextAddr) != 0))
				{
					addSeed(nextAddr);
				}
			}
		}
	}

	std::unordered_set<uint32> visited;
	visited.reserve(65536);
	std::vector<uint32> dfsStack;
	dfsStack.reserve(256);

	std::vector<CACHED_NATIVE_BLOCK> toSave;
	toSave.reserve(32768);
	pendingLinks.reserve(32768);

	for(uint32 seed : seeds)
	{
		if(toSave.size() >= MAX_AOT_BLOCKS) break;
		if(!visited.insert(seed).second) continue;
		dfsStack.push_back(seed);
		while(!dfsStack.empty() && toSave.size() < MAX_AOT_BLOCKS)
		{
			uint32 start = dfsStack.back();
			dfsStack.pop_back();

			uint32 segEnd = 0;
			if(!findContainingSegment(start, segEnd)) continue;

			uint32 end = start;
			uint32 branchTarget = MIPS_INVALID_PC;
			bool hasFallthrough = true;
			PartitionRangeStatic(start, segEnd, end, branchTarget, hasFallthrough);
			if(end < start || end >= PS2::EE_RAM_SIZE) continue;

			if(branchTarget != MIPS_INVALID_PC)
			{
				uint32 bt = branchTarget & m_addressMask;
				uint32 btSegEnd = 0;
				if(findContainingSegment(bt, btSegEnd) && visited.insert(bt).second)
				{
					dfsStack.push_back(bt);
				}
			}
			if(hasFallthrough && (end + 4) < segEnd)
			{
				uint32 ft = end + 4;
				if(visited.insert(ft).second)
				{
					dfsStack.push_back(ft);
				}
			}

			if(IsBlockOverridden(start, end) || HasBlockAt(start)) continue;

			uint64 hashLo = 0, hashHi = 0;
			computeBlockHash(start, end, hashLo, hashHi);

			auto block = std::make_shared<CEeBasicBlock>(m_context, start, end, m_blockCategory);
			CACHED_NATIVE_BLOCK cached;
			cached.begin = start;
			cached.end = end;
			cached.compileHints = 0;
			cached.flags = NATIVE_BLOCK_FLAG_LINKABLE;
			cached.codeHashLo = hashLo;
			cached.codeHashHi = hashHi;

			if(block->CompileForCache(cached.nativeCode, cached.relocs, cached.linkOffset))
			{
				block->SetCodeHash64(hashLo);
				if(block->HasCallLinkSlot())
				{
					cached.flags |= NATIVE_BLOCK_FLAG_CALL_LINK;
				}
				toSave.push_back(cached);
#if defined(_WIN32)
				DiskOverlayKey key{cached.begin, cached.end, cached.codeHashLo, cached.codeHashHi};
				m_diskOverlayCache.emplace(key, std::move(cached));
#else
				uint32 blockSize = (end - start) + 4;
				uint128 hash;
				uint64 parts[2] = {hashLo, hashHi};
				memcpy(&hash, parts, sizeof(hash));
				m_cachedBlocks.insert(std::make_pair(std::make_pair(hash, blockSize), block));

				ResetBlockOutLinks(block.get());
				m_blockLookup.AddBlock(block.get());
				m_blocks.insert(block);
				markPagesTouched(start, end);
				pendingLinks.push_back({start, end, branchTarget});
#endif
			}
		}
		dfsStack.clear();
	}

#if !defined(_WIN32)
	for(const auto& pl : pendingLinks)
	{
		SetupBlockLinks(pl.begin, pl.end, pl.branchTarget);
	}
	for(size_t p = 0; p < touchedPages.size(); p++)
	{
		if(touchedPages[p] && m_pageDataFaults[p] < 8)
		{
			SetMemoryProtected(m_ram + (p * m_pageSize), m_pageSize, true);
		}
	}
#endif

	if(!toSave.empty())
	{
		CNativeCodeCache::SaveFileAtomic(aotPath, abiFp, elfHash, toSave);
	}
	m_aotStats.elfBlocksCompiled = static_cast<uint32>(toSave.size());
	m_aotStats.initMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void CEeExecutor::ClearActiveBlocksInRange(uint32 start, uint32 end, bool executing)
{
	uint32 rangeSize = end - start;
	if(start < PS2::EE_RAM_SIZE && m_pageSize != 0)
	{
		size_t firstPage = start / m_pageSize;
		size_t lastPage = std::min<size_t>(end, PS2::EE_RAM_SIZE) / m_pageSize;
		bool needsDmaSync = false;
		for(size_t p = firstPage; p <= lastPage && p < m_dmaPageState.size(); p++)
		{
			if(m_dmaPageState[p] == 1)
			{
				needsDmaSync = true;
				m_dmaPageState[p] = 0;
			}
		}
		if(needsDmaSync && m_onDmaReadOnlyWriteFault)
		{
			m_onDmaReadOnlyWriteFault();
		}
		for(size_t p = firstPage; p <= lastPage && p < m_pageDataFaults.size(); p++)
		{
			m_pageDataFaults[p] = 0;
		}
	}
	SetMemoryProtected(m_ram + start, rangeSize, false);
	CGenericMipsExecutor::ClearActiveBlocksInRange(start, end, executing);
}

BasicBlockPtr CEeExecutor::BlockFactory(CMIPS& context, uint32 start, uint32 end)
{
	if(m_trackJitStats) m_jitStats.factoryCalls++;
	uint32 blockSize = (end - start) + 4;

	//Kernel area is below 0x100000 and isn't protected. Some games will write code in there
	//but it is safe to assume that it won't change (code writes some data just besides itself
	//so it keeps generating exceptions, making the game slower)
	if(start >= 0x100000 && start < PS2::EE_RAM_SIZE)
	{
		size_t firstPage = start / m_pageSize;
		size_t lastPage = std::min<uint32>(end, PS2::EE_RAM_SIZE - 1) / m_pageSize;
		for(size_t p = firstPage; p <= lastPage; p++)
		{
			//Skip re-protecting pages that repeatedly fault on non-code data writes;
			//DMA overlays still invalidate blocks explicitly via ClearActiveBlocksInRange.
			if(m_pageDataFaults[p] < 8)
			{
				SetMemoryProtected(m_ram + (p * m_pageSize), m_pageSize, true);
			}
		}
	}

	auto blockMemory = reinterpret_cast<uint32*>(alloca(blockSize));
	for(uint32 address = start; address <= end; address += 4)
	{
		uint32 index = (address - start) / 4;
		uint32 opcode = m_context.m_pMemoryMap->GetInstruction(address);
		blockMemory[index] = opcode;
	}

	auto xxHash = XXH3_128bits(blockMemory, blockSize);
	uint128 hash;
	memcpy(&hash, &xxHash, sizeof(xxHash));
	static_assert(sizeof(hash) == sizeof(xxHash));
	auto blockKey = std::make_pair(hash, blockSize);

	bool hasBreakpoint = m_context.HasBreakpointInRange(start, end);

	std::optional<Jitter::CJitter::ROUNDINGMODE> blockFpRoundingModeOverride;
	if(auto blockFpRoundingModeIterator = m_blockFpRoundingModes.find(start);
	   blockFpRoundingModeIterator != std::end(m_blockFpRoundingModes))
	{
		blockFpRoundingModeOverride = blockFpRoundingModeIterator->second;
	}

	bool isIdleLoopBlockOverride = false;
	if(auto idleLoopBlockIterator = m_idleLoopBlocks.find(start);
	   idleLoopBlockIterator != std::end(m_idleLoopBlocks))
	{
		const auto& checkBlockKey = idleLoopBlockIterator->second;
		if(!checkBlockKey.has_value() || (checkBlockKey.value() == blockKey))
		{
			isIdleLoopBlockOverride = true;
		}
	}

	bool fpUseAccurateAddSub = (m_blockFpUseAccurateAddSub.count(start) != 0);

	bool isCacheableBlock = !hasBreakpoint && !blockFpRoundingModeOverride.has_value() && !isIdleLoopBlockOverride && !fpUseAccurateAddSub;
	if(isCacheableBlock)
	{
		auto beginBlockIterator = m_cachedBlocks.lower_bound(blockKey);
		auto endBlockIterator = m_cachedBlocks.upper_bound(blockKey);
		for(auto blockIterator = beginBlockIterator; blockIterator != endBlockIterator; blockIterator++)
		{
			const auto& basicBlock(blockIterator->second);
			if(basicBlock->GetBeginAddress() == start && basicBlock->GetEndAddress() == end)
			{
				if(m_trackJitStats) m_jitStats.cacheHits++;
				uint32 recycleCount = basicBlock->GetRecycleCount();
				basicBlock->SetRecycleCount(std::min<uint32>(RECYCLE_NOLINK_THRESHOLD, recycleCount + 1));
				return basicBlock;
			}
		}
		if(beginBlockIterator != endBlockIterator)
		{
			if(m_trackJitStats) m_jitStats.cacheCopyHits++;
			auto result = std::make_shared<CEeBasicBlock>(context, start, end, m_blockCategory);
			result->CopyFunctionFrom(beginBlockIterator->second);
			result->SetCodeHash64(xxHash.low64);
			m_cachedBlocks.insert(std::make_pair(blockKey, result));
			return result;
		}
		if(!m_diskOverlayCache.empty())
		{
			DiskOverlayKey diskKey{start, end, xxHash.low64, xxHash.high64};
			if(auto diskIt = m_diskOverlayCache.find(diskKey); diskIt != m_diskOverlayCache.end())
			{
				auto result = std::make_shared<CEeBasicBlock>(context, start, end, m_blockCategory);
				if(result->LoadFromNativeCache(diskIt->second))
				{
					result->SetCodeHash64(xxHash.low64);
					m_aotStats.overlayHits++;
					if(m_trackJitStats) m_jitStats.overlayHits++;
					m_cachedBlocks.insert(std::make_pair(blockKey, result));
					return result;
				}
			}
		}
	}

	if(m_trackJitStats)
	{
		m_jitStats.compiles++;
		m_jitStats.compileAddrs[start]++;
	}
	auto result = std::make_shared<CEeBasicBlock>(context, start, end, m_blockCategory);
	if(blockFpRoundingModeOverride.has_value())
	{
		result->SetFpRoundingMode(blockFpRoundingModeOverride.value());
	}
	if(isIdleLoopBlockOverride)
	{
		result->SetIsIdleLoopBlock();
	}
	if(fpUseAccurateAddSub)
	{
		result->AddBlockCompileHints(CMA_EE::COMPILEHINT_FPU_USE_ACCURATE_ADD_SUB);
	}

	if(isCacheableBlock && !m_overlayCachePath.empty() && CNativeCodeCache::IsSupported() &&
	   (m_diskOverlayCache.size() + m_newOverlayEntries.size()) < 131072)
	{
		CACHED_NATIVE_BLOCK entry;
		entry.begin = start;
		entry.end = end;
		entry.compileHints = 0;
		entry.flags = NATIVE_BLOCK_FLAG_LINKABLE;
		entry.codeHashLo = xxHash.low64;
		entry.codeHashHi = xxHash.high64;
		if(result->CompileForCache(entry.nativeCode, entry.relocs, entry.linkOffset))
		{
			if(result->HasCallLinkSlot())
			{
				entry.flags |= NATIVE_BLOCK_FLAG_CALL_LINK;
			}
			m_newOverlayEntries.push_back(std::move(entry));
			m_overlayDirty = true;
			m_aotStats.overlayCompiled++;
		}
	}
	else
	{
		result->Compile();
	}
	result->SetCodeHash64(xxHash.low64);
	if(isCacheableBlock)
	{
		m_cachedBlocks.insert(std::make_pair(blockKey, result));
	}
	return result;
}

bool CEeExecutor::HandleAccessFault(intptr_t ptr)
{
#if defined(__aarch64__)
	//Fault addresses come without the tag that Android's heap puts in the top byte of pointers.
	constexpr uintptr_t addressMask = (static_cast<uintptr_t>(1) << 56) - 1;
	ptrdiff_t addr = (static_cast<uintptr_t>(ptr) & addressMask) - (reinterpret_cast<uintptr_t>(m_ram) & addressMask);
#else
	ptrdiff_t addr = reinterpret_cast<uint8*>(ptr) - m_ram;
#endif
	if(addr >= 0 && addr < PS2::EE_RAM_SIZE)
	{
		uint32 faultAddr = static_cast<uint32>(addr);
		uint32 pageAddr = faultAddr & ~(static_cast<uint32>(m_pageSize) - 1);
		size_t pageIdx = pageAddr / m_pageSize;
		bool wasDmaPinned = (pageIdx < m_dmaPageState.size() && m_dmaPageState[pageIdx] == 1);
		if(wasDmaPinned)
		{
			if(m_onDmaReadOnlyWriteFault)
			{
				m_onDmaReadOnlyWriteFault();
			}
			if(pageIdx < m_dmaPageFaults.size() && m_dmaPageFaults[pageIdx] < 255)
			{
				m_dmaPageFaults[pageIdx]++;
			}
			m_dmaPageState[pageIdx] = (pageIdx < m_dmaPageFaults.size() && m_dmaPageFaults[pageIdx] >= 2) ? 2 : 0;
		}
		//Check whether the write falls inside any compiled block (covering up to a 16-byte SQ write).
		uint32 writeStart = faultAddr & ~0xFU;
		uint32 writeEnd = writeStart + 15;
		uint32 scanStart = (writeStart >= MAX_BLOCK_SIZE) ? (writeStart - MAX_BLOCK_SIZE) : 0;
		bool hitCompiledCode = false;
		for(uint32 a = scanStart; a <= writeEnd && a < PS2::EE_RAM_SIZE; a += 4)
		{
			auto block = m_blockLookup.FindBlockAt(a);
			if(!block->IsEmpty() && (block->GetEndAddress() >= writeStart))
			{
				hitCompiledCode = true;
				break;
			}
		}
		if(!hitCompiledCode)
		{
			if(!wasDmaPinned)
			{
				if(m_trackJitStats)
				{
					m_jitStats.accessFaults++;
					m_jitStats.faultPages[pageAddr]++;
				}
				if(pageIdx < m_pageDataFaults.size() && m_pageDataFaults[pageIdx] < 255)
				{
					m_pageDataFaults[pageIdx]++;
				}
			}
			SetMemoryProtected(m_ram + pageAddr, m_pageSize, false);
			return true;
		}
		if(m_trackJitStats)
		{
			m_jitStats.accessFaults++;
			m_jitStats.faultPages[pageAddr]++;
		}
		ClearActiveBlocksInRange(pageAddr, pageAddr + static_cast<uint32>(m_pageSize), true);
		return true;
	}
	return false;
}

void CEeExecutor::SetMemoryProtected(void* addr, size_t size, bool protect)
{
#ifdef DISABLE_PROTECTION
	return;
#endif
	if(m_pageSize != 0 && !m_pageProtected.empty())
	{
		uintptr_t ramBase = reinterpret_cast<uintptr_t>(m_ram);
		uintptr_t reqAddr = reinterpret_cast<uintptr_t>(addr);
		if(reqAddr >= ramBase && reqAddr < (ramBase + PS2::EE_RAM_SIZE))
		{
			size_t firstPage = (reqAddr - ramBase) / m_pageSize;
			size_t endOff = std::min<size_t>((reqAddr - ramBase) + size, PS2::EE_RAM_SIZE);
			size_t lastPage = (endOff > 0) ? ((endOff - 1) / m_pageSize) : firstPage;
			if(firstPage == lastPage && firstPage < m_pageProtected.size())
			{
				if(m_pageProtected[firstPage] == (protect ? 1 : 0))
				{
					return;
				}
				m_pageProtected[firstPage] = protect ? 1 : 0;
			}
			else
			{
				for(size_t p = firstPage; p <= lastPage && p < m_pageProtected.size(); p++)
				{
					m_pageProtected[p] = protect ? 1 : 0;
				}
			}
		}
	}

#if defined(_WIN32)
	DWORD oldProtect = 0;
	BOOL result = VirtualProtect(addr, size, protect ? PAGE_READONLY : PAGE_READWRITE, &oldProtect);
	assert(result == TRUE);
#elif defined(__EMSCRIPTEN__)
	//Not supported on JavaScript/WebAssembly env
#elif defined(__unix__) || defined(__ANDROID__) || defined(__APPLE__)
	uintptr_t addrValue = reinterpret_cast<uintptr_t>(addr) & ~(m_pageSize - 1);
	addr = reinterpret_cast<void*>(addrValue);
	size = size + (m_pageSize - 1) & ~(m_pageSize - 1);
	int result = mprotect(addr, size, protect ? PROT_READ : PROT_READ | PROT_WRITE);
	assert(result >= 0);
#else
	assert(false);
#endif
}

#if defined(_WIN32)

LONG WINAPI CEeExecutor::HandleException(_EXCEPTION_POINTERS* exceptionInfo)
{
	return g_eeExecutor->HandleExceptionInternal(exceptionInfo);
}

LONG CEeExecutor::HandleExceptionInternal(_EXCEPTION_POINTERS* exceptionInfo)
{
	auto exceptionRecord = exceptionInfo->ExceptionRecord;
	if(exceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
	{
		if(HandleAccessFault(exceptionRecord->ExceptionInformation[1]))
		{
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

#elif defined(__unix__) || defined(__ANDROID__)

void CEeExecutor::HandleException(int sigId, siginfo_t* sigInfo, void* baseContext)
{
	g_eeExecutor->HandleExceptionInternal(sigId, sigInfo, baseContext);
}

void CEeExecutor::HandleExceptionInternal(int sigId, siginfo_t* sigInfo, void* baseContext)
{
	if(sigId != SIGSEGV) return;
	if(HandleAccessFault(reinterpret_cast<intptr_t>(sigInfo->si_addr)))
	{
		return;
	}
	signal(SIGSEGV, SIG_DFL);
}

#elif defined(__APPLE__) && !TARGET_OS_TV

void CEeExecutor::HandlerThreadProc()
{
#pragma pack(push, 4)
	struct INPUT_MESSAGE
	{
		mach_msg_header_t head;
		NDR_record_t ndr;
		exception_type_t exception;
		mach_msg_type_number_t codeCount;
		intptr_t code[2];
		int flavor;
		mach_msg_type_number_t stateCount;
		natural_t state[STATE_FLAVOR_COUNT];
		mach_msg_trailer_t trailer;
	};
	struct OUTPUT_MESSAGE
	{
		mach_msg_header_t head;
		NDR_record_t ndr;
		kern_return_t result;
		int flavor;
		mach_msg_type_number_t stateCount;
		natural_t state[STATE_FLAVOR_COUNT];
	};
#pragma pack(pop)

	while(m_running)
	{
		kern_return_t result = KERN_SUCCESS;

		INPUT_MESSAGE inMsg;
		result = mach_msg(&inMsg.head, MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_TIMEOUT, 0, sizeof(inMsg), m_port, 1000, MACH_PORT_NULL);
		if(result == MACH_RCV_TIMED_OUT) continue;
		assert(result == KERN_SUCCESS);

		assert(inMsg.head.msgh_id == 2406); //MACH_EXCEPTION_RAISE_RPC
		assert(inMsg.flavor == STATE_FLAVOR);
		assert(inMsg.stateCount == STATE_FLAVOR_COUNT);

		bool success = HandleAccessFault(inMsg.code[1]);

		OUTPUT_MESSAGE outMsg;
		outMsg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(inMsg.head.msgh_bits), 0);
		outMsg.head.msgh_remote_port = inMsg.head.msgh_remote_port;
		outMsg.head.msgh_local_port = MACH_PORT_NULL;
		outMsg.head.msgh_id = inMsg.head.msgh_id + 100;
		outMsg.head.msgh_size = sizeof(outMsg);
		outMsg.ndr = inMsg.ndr;

		if(success)
		{
			outMsg.result = KERN_SUCCESS;
			outMsg.flavor = STATE_FLAVOR;
			outMsg.stateCount = STATE_FLAVOR_COUNT;
			memcpy(outMsg.state, inMsg.state, STATE_FLAVOR_COUNT * sizeof(natural_t));
		}
		else
		{
			outMsg.result = KERN_FAILURE;
			outMsg.flavor = 0;
			outMsg.stateCount = 0;
		}

		result = mach_msg(&outMsg.head, MACH_SEND_MSG | MACH_RCV_LARGE, sizeof(outMsg), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		assert(result == KERN_SUCCESS);
	}
}

#endif
