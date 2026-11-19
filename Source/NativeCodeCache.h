#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "Types.h"
#include "MIPS.h"
#include "Jitter_CodeGen.h"

struct NATIVE_RELOC
{
	uint32 offset = 0;
	uint16 symbolId = 0;
	uint8 refType = 0;
	uint8 pad = 0;
};
static_assert(sizeof(NATIVE_RELOC) == 8, "NATIVE_RELOC must be 8 bytes.");

enum NATIVE_BLOCK_FLAGS : uint32
{
	NATIVE_BLOCK_FLAG_LINKABLE = (1U << 0),
	NATIVE_BLOCK_FLAG_CALL_LINK = (1U << 1),
};

struct CACHED_NATIVE_BLOCK
{
	uint32 begin = 0;
	uint32 end = 0;
	uint32 compileHints = 0;
	uint32 flags = NATIVE_BLOCK_FLAG_LINKABLE;
	uint32 linkOffset[2] = {~0U, ~0U};
	uint64 codeHashLo = 0;
	uint64 codeHashHi = 0;
	std::vector<NATIVE_RELOC> relocs;
	std::vector<uint8> nativeCode;
};

class CNativeCodeCache
{
public:
	static bool IsSupported();
	static const char* GetArchTag();

	static void SetCacheDir(std::string dir);
	static std::string GetCacheDir();
	static std::string SanitizeTag(const std::string& raw);

	static uint64 ComputeAbiFingerprint(const CMIPS& ctx);

	static bool LookupSymbolId(uintptr_t addr, const CMIPS& ctx, uint16& outSymbolId);
	static bool ResolveSymbolAddress(uint16 symbolId, const CMIPS& ctx, uintptr_t& outAddr);
	static bool ApplyRelocations(uint8* code, size_t codeSize, const NATIVE_RELOC* relocs, size_t relocCount, const CMIPS& ctx);

	static bool LoadFile(const std::string& path, uint64 expectedAbiFingerprint, uint64 expectedElfHash, std::vector<CACHED_NATIVE_BLOCK>& outBlocks);
	static bool SaveFileAtomic(const std::string& path, uint64 abiFingerprint, uint64 elfHash, const std::vector<CACHED_NATIVE_BLOCK>& blocks);

private:
	static std::string m_cacheDir;
};
