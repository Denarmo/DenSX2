#include "VuExecutor.h"
#include "VuBasicBlock.h"
#include "VUShared.h"
#include "../Ps2Const.h"
#include "filesystem_def.h"
#include "xxhash.h"

static void ReturnToVuExecutor(void*)
{
}

// clang-format off
const CVuExecutor::BLOCK_COMPILE_HINTS CVuExecutor::g_blockCompileHints[] =
{
	// Tri-Ace VU0 decompression code
	{ std::make_pair(uint128{0x3aa43f7c, 0xe932516c, 0x6786472d, 0xf5333e12}, 0x58U), VUShared::COMPILEHINT_USE_ACCURATE_ADDI },
};
// clang-format on

CVuExecutor::CVuExecutor(CMIPS& context, uint32 maxAddress)
    : CGenericMipsExecutor(context, maxAddress, BLOCK_CATEGORY_PS2_VU)
{
	void* returnToExecutor = reinterpret_cast<void*>(&ReturnToVuExecutor);
	if(SetupBlockFixedFrame(context))
	{
		returnToExecutor = context.m_fixedFrameExit;
		//Made by the base class before the frame existed
		m_emptyBlock->Compile();
	}
	m_blockLookup.EnableCodeTable(returnToExecutor);
	context.m_blockCodeLookup = m_blockLookup.GetCodeTable();
	context.m_blockCodeLookupMask = m_addressMask;
}

CVuExecutor::~CVuExecutor()
{
	m_context.m_blockCodeLookup = nullptr;
	m_context.m_fixedFrameEntry = nullptr;
	m_context.m_fixedFrameExit = nullptr;
}

void CVuExecutor::SetBlockNoClamping(BlockNoClampingSet blockNoClamping)
{
	m_blockNoClamping = std::move(blockNoClamping);
}

void CVuExecutor::Reset()
{
	SaveDiskCache();
	{
		std::lock_guard<std::mutex> lock(m_diskCacheMutex);
		m_diskCache.clear();
		m_newDiskEntries.clear();
		m_diskCachePath.clear();
		m_diskAbiFingerprint = 0;
		m_diskCacheDirty = false;
		m_cacheStats = {};
	}
	m_cachedBlocks.clear();
	m_blockNoClamping.clear();
	m_flagScanDirty = true;
	m_vu1FlagSkipHints = 0;
	CGenericMipsExecutor::Reset();
}

void CVuExecutor::InitDiskCache(const std::string& exeName)
{
	if(!CNativeCodeCache::IsSupported() || exeName.empty()) return;
	auto sanitized = CNativeCodeCache::SanitizeTag(exeName);
	if(sanitized.empty()) return;

	const char* suffix = (m_maxAddress == PS2::MICROMEM1SIZE) ? ".vu1.aot" : ".vu0.aot";
	uint64 abiFp = CNativeCodeCache::ComputeAbiFingerprint(m_context);
	auto path = (fs::path(CNativeCodeCache::GetCacheDir()) /
	             (sanitized + "-" + CNativeCodeCache::GetArchTag() + suffix)).string();

	std::lock_guard<std::mutex> lock(m_diskCacheMutex);
	if(path == m_diskCachePath && abiFp == m_diskAbiFingerprint && !m_diskCache.empty())
	{
		return;
	}
	m_diskCachePath = path;
	m_diskAbiFingerprint = abiFp;
	m_diskCache.clear();
	m_newDiskEntries.clear();
	m_diskCacheDirty = false;

	std::vector<CACHED_NATIVE_BLOCK> loaded;
	if(CNativeCodeCache::LoadFile(m_diskCachePath, m_diskAbiFingerprint, 0, loaded))
	{
		for(auto& entry : loaded)
		{
			DiskVuKey key{entry.begin, entry.end, entry.compileHints, entry.codeHashLo, entry.codeHashHi};
			m_diskCache.emplace(key, std::move(entry));
		}
		m_cacheStats.loaded = static_cast<uint32>(m_diskCache.size());
	}
}

void CVuExecutor::SaveDiskCache()
{
	std::vector<CACHED_NATIVE_BLOCK> toSave;
	std::string savePath;
	uint64 abiFp = 0;
	{
		std::lock_guard<std::mutex> lock(m_diskCacheMutex);
		if(!m_diskCacheDirty || m_diskCachePath.empty() || m_newDiskEntries.empty()) return;
		for(auto& entry : m_newDiskEntries)
		{
			DiskVuKey key{entry.begin, entry.end, entry.compileHints, entry.codeHashLo, entry.codeHashHi};
			m_diskCache.emplace(key, std::move(entry));
		}
		m_newDiskEntries.clear();
		toSave.reserve(m_diskCache.size());
		for(const auto& kv : m_diskCache)
		{
			toSave.push_back(kv.second);
		}
		savePath = m_diskCachePath;
		abiFp = m_diskAbiFingerprint;
		m_diskCacheDirty = false;
	}
	if(!toSave.empty())
	{
		CNativeCodeCache::SaveFileAtomic(savePath, abiFp, 0, toSave);
	}
}

void CVuExecutor::ClearActiveBlocksInRange(uint32 start, uint32 end, bool executing)
{
	m_flagScanDirty = true;
	CGenericMipsExecutor::ClearActiveBlocksInRange(start, end, executing);
}

uint32 CVuExecutor::GetVu1FlagSkipHints()
{
	//Only VU1 micro-memory is isolated from EE COP2 flag reads (CFC2 reads VU0).
	if(m_maxAddress != PS2::MICROMEM1SIZE)
	{
		return 0;
	}
	if(!m_flagScanDirty)
	{
		return m_vu1FlagSkipHints;
	}
	m_flagScanDirty = false;

	auto map = m_context.m_pMemoryMap->GetInstructionMap(0);
	const auto* words = reinterpret_cast<const uint32*>(map->pPointer);
	bool hasStatusRead = false;
	bool hasMacRead = false;
	for(uint32 addr = 0; addr < m_maxAddress; addr += 8)
	{
		uint32 upper = words[(addr / 4) + 1];
		if(upper & 0x80000000U) continue; //LOI: lower 32 bits are an immediate float
		uint32 op = words[addr / 4] >> 25;
		if(op == 0x14 || op == 0x16 || op == 0x17) //FSEQ, FSAND, FSOR
		{
			hasStatusRead = true;
			hasMacRead = true;
			break;
		}
		if(op == 0x18 || op == 0x1A || op == 0x1B) //FMEQ, FMAND, FMOR
		{
			hasMacRead = true;
		}
	}

	uint32 newHints = 0;
	if(!hasStatusRead) newHints |= VUShared::COMPILEHINT_SKIP_STICKY_UPDATE;
	if(!hasMacRead) newHints |= VUShared::COMPILEHINT_SKIP_FMAC_UPDATE;

	//If a partial micro-memory upload introduced a flag reader, discard any active blocks that skipped those flags.
	if((m_vu1FlagSkipHints & ~newHints) != 0)
	{
		CGenericMipsExecutor::ClearActiveBlocksInRange(0, m_maxAddress, false);
	}
	m_vu1FlagSkipHints = newHints;
	return m_vu1FlagSkipHints;
}

BasicBlockPtr CVuExecutor::BlockFactory(CMIPS& context, uint32 begin, uint32 end)
{
	uint32 vu1FlagHints = GetVu1FlagSkipHints();
	uint32 blockSize = ((end - begin) + 4) / 4;
	uint32 blockSizeByte = blockSize * 4;

	auto map = m_context.m_pMemoryMap->GetInstructionMap(begin);
	assert(m_context.m_pMemoryMap->GetInstructionMap(end) == map);
	uint32 localBegin = begin - map->nStart;
	auto blockMemory = reinterpret_cast<const uint32*>(reinterpret_cast<uint8*>(map->pPointer) + localBegin);

	auto xxHash = XXH3_128bits(blockMemory, blockSizeByte);
	uint128 hash;
	memcpy(&hash, &xxHash, sizeof(xxHash));
	static_assert(sizeof(hash) == sizeof(xxHash));
	auto blockKey = std::make_pair(hash, blockSizeByte);
	auto cacheKey = std::make_tuple(hash, blockSizeByte, vu1FlagHints);

	//Don't use the cached blocks of we have a breakpoint in our block range.
	bool hasBreakpoint = m_context.HasBreakpointInRange(begin, end);
	if(!hasBreakpoint)
	{
		auto beginBlockIterator = m_cachedBlocks.lower_bound(cacheKey);
		auto endBlockIterator = m_cachedBlocks.upper_bound(cacheKey);
		//Check if we have a block that has the same contents and the same range.
		for(auto blockIterator = beginBlockIterator; blockIterator != endBlockIterator; blockIterator++)
		{
			const auto& basicBlock(blockIterator->second);
			if(basicBlock->GetBeginAddress() == begin && basicBlock->GetEndAddress() == end)
			{
				return basicBlock;
			}
		}
		//Check if we have a block that has the same contents but not the same range. Reuse the code of that block if that's the case.
		if(beginBlockIterator != endBlockIterator)
		{
			auto result = std::make_shared<CVuBasicBlock>(context, begin, end, m_blockCategory);
			result->CopyFunctionFrom(beginBlockIterator->second);
			m_cachedBlocks.insert(std::make_pair(cacheKey, result));
			return result;
		}
	}

	//Compute full compile hints for this block so disk cache keys match exact codegen flags
	uint32 totalHints = vu1FlagHints;
	auto blockCompileHintsIterator = std::find_if(std::begin(g_blockCompileHints), std::end(g_blockCompileHints),
	                                              [&](const auto& item) { return item.blockKey == blockKey; });
	if(blockCompileHintsIterator != std::end(g_blockCompileHints))
	{
		totalHints |= blockCompileHintsIterator->hints;
	}
	if(auto blockNoClamping = m_blockNoClamping.find(blockKey);
	   blockNoClamping != std::end(m_blockNoClamping))
	{
		totalHints |= VUShared::COMPILEHINT_NO_CLAMPING;
	}

	if(!hasBreakpoint)
	{
		std::lock_guard<std::mutex> lock(m_diskCacheMutex);
		if(!m_diskCache.empty())
		{
			DiskVuKey diskKey{begin, end, totalHints, xxHash.low64, xxHash.high64};
			if(auto diskIt = m_diskCache.find(diskKey); diskIt != m_diskCache.end())
			{
				auto result = std::make_shared<CVuBasicBlock>(context, begin, end, m_blockCategory);
				if(totalHints != 0)
				{
					result->AddBlockCompileHints(totalHints);
				}
				if(result->LoadFromNativeCache(diskIt->second))
				{
					m_cacheStats.hits++;
					m_cachedBlocks.insert(std::make_pair(cacheKey, result));
					return result;
				}
			}
		}
	}

	//Totally new block, build it from scratch
	auto result = std::make_shared<CVuBasicBlock>(context, begin, end, m_blockCategory);
	if(totalHints != 0)
	{
		result->AddBlockCompileHints(totalHints);
	}

	bool hasTrailingEBitInDelaySlot = false;
	if((end - begin) >= 0xC)
	{
		uint32 delayUpperOp = m_context.m_pMemoryMap->GetInstruction(end);
		if(delayUpperOp & VUShared::VU_UPPEROP_BIT_E)
		{
			hasTrailingEBitInDelaySlot = true;
		}
	}

	bool tryCache = !hasBreakpoint && !hasTrailingEBitInDelaySlot && CNativeCodeCache::IsSupported();
	if(tryCache)
	{
		CACHED_NATIVE_BLOCK entry;
		entry.begin = begin;
		entry.end = end;
		entry.compileHints = totalHints;
		entry.flags = NATIVE_BLOCK_FLAG_LINKABLE;
		entry.codeHashLo = xxHash.low64;
		entry.codeHashHi = xxHash.high64;
		bool compiledOk = result->CompileForCache(entry.nativeCode, entry.relocs, entry.linkOffset);
		if(compiledOk && result->IsLinkable())
		{
			std::lock_guard<std::mutex> lock(m_diskCacheMutex);
			if(!m_diskCachePath.empty() && (m_diskCache.size() + m_newDiskEntries.size()) < 8192)
			{
				m_newDiskEntries.push_back(std::move(entry));
				m_diskCacheDirty = true;
				m_cacheStats.compiled++;
			}
		}
	}
	else
	{
		result->Compile();
	}
	if(!hasBreakpoint)
	{
		m_cachedBlocks.insert(std::make_pair(cacheKey, result));
	}
	return result;
}

void CVuExecutor::PartitionFunction(uint32 startAddress)
{
	uint32 endAddress = std::min<uint32>(startAddress + MAX_BLOCK_SIZE - 4, m_maxAddress - 4);
	uint32 branchAddress = MIPS_INVALID_PC;
	for(uint32 address = startAddress; address < endAddress; address += 8)
	{
		uint32 addrLo = address + 0;
		uint32 addrHi = address + 4;
		uint32 lowerOp = m_context.m_pMemoryMap->GetInstruction(addrLo);
		uint32 upperOp = m_context.m_pMemoryMap->GetInstruction(addrHi);
		auto branchType = m_context.m_pArch->IsInstructionBranch(&m_context, addrLo, lowerOp);
		if(upperOp & VUShared::VU_UPPEROP_BIT_E)
		{
			endAddress = address + 0xC;
			break;
		}
		else if((branchType == MIPS_BRANCH_NONE) && (upperOp & (VUShared::VU_UPPEROP_BIT_D | VUShared::VU_UPPEROP_BIT_T)))
		{
			endAddress = address + 0x04;
			break;
		}
		else if(branchType == MIPS_BRANCH_NORMAL)
		{
			branchAddress = m_context.m_pArch->GetInstructionEffectiveAddress(&m_context, addrLo, lowerOp);
			endAddress = address + 0xC;
			break;
		}
		else if(branchType == MIPS_BRANCH_NODELAY)
		{
			//Should never happen
			assert(false);
		}
	}
	assert((endAddress - startAddress) <= MAX_BLOCK_SIZE);
	CreateBlock(startAddress, endAddress);
	auto block = static_cast<CVuBasicBlock*>(FindBlockStartingAt(startAddress));
	if(block->IsLinkable())
	{
		SetupBlockLinks(startAddress, endAddress, branchAddress);
	}
}
