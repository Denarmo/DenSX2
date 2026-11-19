#pragma once

#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>
#include "../GenericMipsExecutor.h"
#include "../NativeCodeCache.h"

class CVuExecutor : public CGenericMipsExecutor<BlockLookupOneWay, 8>
{
public:
	using CachedBlockKey = std::pair<uint128, uint32>;
	using BlockNoClampingSet = std::set<CachedBlockKey>;

	CVuExecutor(CMIPS&, uint32);
	virtual ~CVuExecutor();

	void SetBlockNoClamping(BlockNoClampingSet);

	void Reset() override;
	void ClearActiveBlocksInRange(uint32, uint32, bool) override;

	void InitDiskCache(const std::string& exeName);
	void SaveDiskCache();

	struct VU_CACHE_STATS
	{
		uint32 loaded = 0;
		uint32 hits = 0;
		uint32 compiled = 0;
	};
	const VU_CACHE_STATS& GetCacheStats() const { return m_cacheStats; }

protected:
	using CachedBlockMapKey = std::tuple<uint128, uint32, uint32>;
	typedef std::multimap<CachedBlockMapKey, BasicBlockPtr> CachedBlockMap;

	struct BLOCK_COMPILE_HINTS
	{
		CachedBlockKey blockKey;
		uint32 hints;
	};

	BasicBlockPtr BlockFactory(CMIPS&, uint32, uint32) override;
	void PartitionFunction(uint32) override;
	uint32 GetVu1FlagSkipHints();

	static const BLOCK_COMPILE_HINTS g_blockCompileHints[];
	CachedBlockMap m_cachedBlocks;

	BlockNoClampingSet m_blockNoClamping;

	bool m_flagScanDirty = true;
	uint32 m_vu1FlagSkipHints = 0;

	struct DiskVuKey
	{
		uint32 begin = 0;
		uint32 end = 0;
		uint32 compileHints = 0;
		uint64 hashLo = 0;
		uint64 hashHi = 0;

		bool operator<(const DiskVuKey& o) const
		{
			return std::tie(begin, end, compileHints, hashLo, hashHi) < std::tie(o.begin, o.end, o.compileHints, o.hashLo, o.hashHi);
		}
	};

	std::mutex m_diskCacheMutex;
	std::map<DiskVuKey, CACHED_NATIVE_BLOCK> m_diskCache;
	std::vector<CACHED_NATIVE_BLOCK> m_newDiskEntries;
	std::string m_diskCachePath;
	uint64 m_diskAbiFingerprint = 0;
	bool m_diskCacheDirty = false;
	VU_CACHE_STATS m_cacheStats;
};
