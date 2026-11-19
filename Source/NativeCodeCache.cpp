#include "NativeCodeCache.h"
#include <cstdio>
#include <cstring>
#include <cstddef>
#include "filesystem_def.h"
#include "AppConfig.h"
#include "BasicBlock.h"
#include "MemoryUtils.h"
#include "COP_SCU.h"
#include "ee/PS2OS.h"
#include "ee/FpAddTruncate.h"
#include "offsetof_def.h"
#include "xxhash.h"

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

extern "C"
{
	uint32 LWL_Proxy(uint32, uint32, CMIPS*);
	uint32 LWR_Proxy(uint32, uint32, CMIPS*);
	uint64 LDL_Proxy(uint32, uint64, CMIPS*);
	uint64 LDR_Proxy(uint32, uint64, CMIPS*);
	void SWL_Proxy(uint32, uint32, CMIPS*);
	void SWR_Proxy(uint32, uint32, CMIPS*);
	void SDL_Proxy(uint32, uint64, CMIPS*);
	void SDR_Proxy(uint32, uint64, CMIPS*);
	void TrapHandler(CMIPS*);
	void HandleTLBException(CMIPS*);
}

std::string CNativeCodeCache::m_cacheDir;

namespace
{
	constexpr char AOT_MAGIC[8] = {'D', 'N', 'S', 'X', '2', 'A', 'O', 'T'};
	constexpr uint32 AOT_FORMAT_VERSION = 21;
	constexpr uint32 MAX_CACHED_BLOCKS_PER_FILE = 262144;
	constexpr uint32 MAX_BLOCK_CODE_SIZE = 2 * 1024 * 1024;
	constexpr uint32 MAX_BLOCK_RELOCS = 65536;

	enum EXTERNAL_SYMBOL_ID : uint16
	{
		SYM_NONE = 0,
		SYM_FIXED_FRAME_EXIT = 1,
		SYM_EMPTY_BLOCK_HANDLER = 2,
		SYM_NEXT_BLOCK_TRAMPOLINE = 3,
		SYM_BRANCH_BLOCK_TRAMPOLINE = 4,
		SYM_MEM_GET_BYTE = 5,
		SYM_MEM_GET_HALF = 6,
		SYM_MEM_GET_WORD = 7,
		SYM_MEM_GET_DOUBLE = 8,
		SYM_MEM_GET_QUAD = 9,
		SYM_MEM_SET_BYTE = 10,
		SYM_MEM_SET_HALF = 11,
		SYM_MEM_SET_WORD = 12,
		SYM_MEM_SET_DOUBLE = 13,
		SYM_MEM_SET_QUAD = 14,
		SYM_LWL_PROXY = 15,
		SYM_LWR_PROXY = 16,
		SYM_LDL_PROXY = 17,
		SYM_LDR_PROXY = 18,
		SYM_SWL_PROXY = 19,
		SYM_SWR_PROXY = 20,
		SYM_SDL_PROXY = 21,
		SYM_SDR_PROXY = 22,
		SYM_FP_ADD_TRUNCATE = 23,
		SYM_TRAP_HANDLER = 24,
		SYM_HANDLE_TLB_EXCEPTION = 25,
		SYM_HANDLE_TLB_READ = 26,
		SYM_HANDLE_TLB_WRITE = 27,
		SYM_PS2OS_TRANSLATE_ADDR = 28,
		SYM_PS2OS_TRANSLATE_ADDR_TLB = 29,
		SYM_PS2OS_CHECK_TLB_EX = 30,
		SYM_MIPS_TRANSLATE_ADDR64 = 31,
	};

	struct FILE_HEADER
	{
		char magic[8];
		uint32 version;
		uint32 blockCount;
		uint64 abiFingerprint;
		uint64 elfContentHash;
	};

	struct BLOCK_HEADER
	{
		uint32 begin;
		uint32 end;
		uint32 compileHints;
		uint32 flags;
		uint32 linkOffset[2];
		uint64 codeHashLo;
		uint64 codeHashHi;
		uint32 relocCount;
		uint32 nativeCodeSize;
	};

	uint64 GetHostCpuFeatureMask()
	{
#if defined(__aarch64__)
		return 0xA6400001ULL;
#elif defined(__x86_64__) || defined(_M_X64)
		uint32 eax1 = 0, ebx1 = 0, ecx1 = 0, edx1 = 0;
		uint32 eax7 = 0, ebx7 = 0, ecx7 = 0, edx7 = 0;
#if defined(_MSC_VER)
		int regs1[4] = {};
		int regs7[4] = {};
		__cpuid(regs1, 1);
		__cpuidex(regs7, 7, 0);
		ecx1 = static_cast<uint32>(regs1[2]);
		ebx7 = static_cast<uint32>(regs7[1]);
#else
		__cpuid(1, eax1, ebx1, ecx1, edx1);
		__cpuid_count(7, 0, eax7, ebx7, ecx7, edx7);
#endif
		uint64 mask = 0x86640000ULL;
		if(ecx1 & (1U << 9)) mask |= (1ULL << 0);  //SSSE3
		if(ecx1 & (1U << 19)) mask |= (1ULL << 1); //SSE4.1
		if(ecx1 & (1U << 27)) mask |= (1ULL << 2); //OSXSAVE
		if(ecx1 & (1U << 28)) mask |= (1ULL << 3); //AVX
		if(ebx7 & (1U << 5)) mask |= (1ULL << 4);  //AVX2
		return mask;
#else
		return 0;
#endif
	}

	uintptr_t GetStaticSymbolAddr(uint16 symbolId)
	{
		switch(symbolId)
		{
		case SYM_EMPTY_BLOCK_HANDLER: return reinterpret_cast<uintptr_t>(&EmptyBlockHandler);
		case SYM_NEXT_BLOCK_TRAMPOLINE: return reinterpret_cast<uintptr_t>(&NextBlockTrampoline);
		case SYM_BRANCH_BLOCK_TRAMPOLINE: return reinterpret_cast<uintptr_t>(&BranchBlockTrampoline);
		case SYM_MEM_GET_BYTE: return reinterpret_cast<uintptr_t>(&MemoryUtils_GetByteProxy);
		case SYM_MEM_GET_HALF: return reinterpret_cast<uintptr_t>(&MemoryUtils_GetHalfProxy);
		case SYM_MEM_GET_WORD: return reinterpret_cast<uintptr_t>(&MemoryUtils_GetWordProxy);
		case SYM_MEM_GET_DOUBLE: return reinterpret_cast<uintptr_t>(&MemoryUtils_GetDoubleProxy);
		case SYM_MEM_GET_QUAD: return reinterpret_cast<uintptr_t>(&MemoryUtils_GetQuadProxy);
		case SYM_MEM_SET_BYTE: return reinterpret_cast<uintptr_t>(&MemoryUtils_SetByteProxy);
		case SYM_MEM_SET_HALF: return reinterpret_cast<uintptr_t>(&MemoryUtils_SetHalfProxy);
		case SYM_MEM_SET_WORD: return reinterpret_cast<uintptr_t>(&MemoryUtils_SetWordProxy);
		case SYM_MEM_SET_DOUBLE: return reinterpret_cast<uintptr_t>(&MemoryUtils_SetDoubleProxy);
		case SYM_MEM_SET_QUAD: return reinterpret_cast<uintptr_t>(&MemoryUtils_SetQuadProxy);
		case SYM_LWL_PROXY: return reinterpret_cast<uintptr_t>(&LWL_Proxy);
		case SYM_LWR_PROXY: return reinterpret_cast<uintptr_t>(&LWR_Proxy);
		case SYM_LDL_PROXY: return reinterpret_cast<uintptr_t>(&LDL_Proxy);
		case SYM_LDR_PROXY: return reinterpret_cast<uintptr_t>(&LDR_Proxy);
		case SYM_SWL_PROXY: return reinterpret_cast<uintptr_t>(&SWL_Proxy);
		case SYM_SWR_PROXY: return reinterpret_cast<uintptr_t>(&SWR_Proxy);
		case SYM_SDL_PROXY: return reinterpret_cast<uintptr_t>(&SDL_Proxy);
		case SYM_SDR_PROXY: return reinterpret_cast<uintptr_t>(&SDR_Proxy);
		case SYM_FP_ADD_TRUNCATE: return reinterpret_cast<uintptr_t>(&FpAddTruncate);
		case SYM_TRAP_HANDLER: return reinterpret_cast<uintptr_t>(&TrapHandler);
		case SYM_HANDLE_TLB_EXCEPTION: return reinterpret_cast<uintptr_t>(&HandleTLBException);
		case SYM_HANDLE_TLB_READ: return reinterpret_cast<uintptr_t>(&CCOP_SCU::HandleTLBRead);
		case SYM_HANDLE_TLB_WRITE: return reinterpret_cast<uintptr_t>(&CCOP_SCU::HandleTLBWrite);
		case SYM_PS2OS_TRANSLATE_ADDR: return reinterpret_cast<uintptr_t>(&CPS2OS::TranslateAddress);
		case SYM_PS2OS_TRANSLATE_ADDR_TLB: return reinterpret_cast<uintptr_t>(&CPS2OS::TranslateAddressTLB);
		case SYM_PS2OS_CHECK_TLB_EX: return reinterpret_cast<uintptr_t>(&CPS2OS::CheckTLBExceptions);
		case SYM_MIPS_TRANSLATE_ADDR64: return reinterpret_cast<uintptr_t>(&CMIPS::TranslateAddress64);
		default: return 0;
		}
	}
}

bool CNativeCodeCache::IsSupported()
{
#if (defined(__aarch64__) && !defined(__APPLE__)) || defined(__x86_64__) || defined(_M_X64)
#if !defined(AOT_BUILD_CACHE) && !defined(AOT_USE_CACHE) && !defined(__EMSCRIPTEN__) && !defined(DEBUGGER_INCLUDED)
	return true;
#else
	return false;
#endif
#else
	return false;
#endif
}

const char* CNativeCodeCache::GetArchTag()
{
#if defined(__aarch64__)
	return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
	return "x86_64";
#else
	return "unknown";
#endif
}

void CNativeCodeCache::SetCacheDir(std::string dir)
{
	m_cacheDir = std::move(dir);
}

std::string CNativeCodeCache::GetCacheDir()
{
	if(!m_cacheDir.empty())
	{
		return m_cacheDir;
	}
	return (CAppConfig::GetInstance().GetBasePath() / "aot").string();
}

std::string CNativeCodeCache::SanitizeTag(const std::string& raw)
{
	std::string out;
	out.reserve(raw.size());
	for(char c : raw)
	{
		if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
		{
			out.push_back(c);
		}
		else
		{
			out.push_back('_');
		}
	}
	return out;
}

uint64 CNativeCodeCache::ComputeAbiFingerprint(const CMIPS& ctx)
{
	const uint64 fields[] = {
	    AOT_FORMAT_VERSION,
	    GetHostCpuFeatureMask(),
	    sizeof(CMIPS),
	    sizeof(MIPSSTATE),
	    offsetof(CMIPS, m_State),
	    offsetof(CMIPS, m_vuMem),
	    offsetof(CMIPS, m_pageLookup),
	    offsetof(CMIPS, m_ramBase),
	    offsetof(CMIPS, m_blockCodeLookup),
	    offsetof(CMIPS, m_State.nPC),
	    offsetof(CMIPS, m_State.nDelayedJumpAddr),
	    offsetof(CMIPS, m_State.cycleQuota),
	    offsetof(CMIPS, m_State.nHasException),
	    offsetof(CMIPS, m_State.nGPR),
	    offsetof(CMIPS, m_State.nCOP2),
	    offsetof(CMIPS, m_State.nCOP2VI),
	    offsetof(CMIPS, m_State.nCOP0),
	    offsetof(CMIPS, m_State.nFCSR),
	    offsetof(CMIPS, m_State.pipeTime),
	    offsetof(CMIPS, m_State.pipeFmacWrite),
	    ctx.m_fixedFrameExit ? 1ULL : 0ULL,
	    ctx.m_pageLookup ? 1ULL : 0ULL,
	    ctx.m_ramBase ? 1ULL : 0ULL,
	    ctx.m_vuMem ? 1ULL : 0ULL,
	    ctx.m_blockCodeLookup ? 1ULL : 0ULL,
	    static_cast<uint64>(ctx.m_blockCodeLookupMask),
	    (ctx.m_pAddrTranslator == &CMIPS::TranslateAddress64) ? 1ULL : 0ULL,
	    (ctx.m_TLBExceptionChecker != nullptr) ? 1ULL : 0ULL,
	};
	return XXH3_64bits(fields, sizeof(fields));
}

bool CNativeCodeCache::LookupSymbolId(uintptr_t addr, const CMIPS& ctx, uint16& outSymbolId)
{
	if(ctx.m_fixedFrameExit && (addr == reinterpret_cast<uintptr_t>(ctx.m_fixedFrameExit)))
	{
		outSymbolId = SYM_FIXED_FRAME_EXIT;
		return true;
	}
	for(uint16 id = SYM_EMPTY_BLOCK_HANDLER; id <= SYM_MIPS_TRANSLATE_ADDR64; id++)
	{
		if(GetStaticSymbolAddr(id) == addr)
		{
			outSymbolId = id;
			return true;
		}
	}
	return false;
}

bool CNativeCodeCache::ResolveSymbolAddress(uint16 symbolId, const CMIPS& ctx, uintptr_t& outAddr)
{
	if(symbolId == SYM_FIXED_FRAME_EXIT)
	{
		if(!ctx.m_fixedFrameExit) return false;
		outAddr = reinterpret_cast<uintptr_t>(ctx.m_fixedFrameExit);
		return true;
	}
	uintptr_t addr = GetStaticSymbolAddr(symbolId);
	if(addr == 0) return false;
	outAddr = addr;
	return true;
}

bool CNativeCodeCache::ApplyRelocations(uint8* code, size_t codeSize, const NATIVE_RELOC* relocs, size_t relocCount, const CMIPS& ctx)
{
	for(size_t i = 0; i < relocCount; i++)
	{
		const auto& reloc = relocs[i];
		uintptr_t targetAddr = 0;
		if(!ResolveSymbolAddress(reloc.symbolId, ctx, targetAddr))
		{
			return false;
		}
		auto refType = static_cast<Jitter::CCodeGen::SYMBOL_REF_TYPE>(reloc.refType);
		if(refType == Jitter::CCodeGen::SYMBOL_REF_TYPE::NATIVE_POINTER)
		{
			if(static_cast<size_t>(reloc.offset) + sizeof(uintptr_t) > codeSize) return false;
			memcpy(code + reloc.offset, &targetAddr, sizeof(uintptr_t));
		}
		else if(refType == Jitter::CCodeGen::SYMBOL_REF_TYPE::ARMV8_MOVZ_MOVK_64)
		{
			if(static_cast<size_t>(reloc.offset) + 16 > codeSize) return false;
			uint64 val = static_cast<uint64>(targetAddr);
			for(int k = 0; k < 4; k++)
			{
				uint32 insn = 0;
				memcpy(&insn, code + reloc.offset + (k * 4), sizeof(uint32));
				uint32 imm16 = static_cast<uint32>((val >> (k * 16)) & 0xFFFFU);
				insn = (insn & ~(0xFFFFU << 5)) | (imm16 << 5);
				memcpy(code + reloc.offset + (k * 4), &insn, sizeof(uint32));
			}
		}
		else
		{
			return false;
		}
	}
	return true;
}

bool CNativeCodeCache::LoadFile(const std::string& path, uint64 expectedAbiFingerprint, uint64 expectedElfHash, std::vector<CACHED_NATIVE_BLOCK>& outBlocks)
{
	outBlocks.clear();
	FILE* fp = fopen(path.c_str(), "rb");
	if(!fp) return false;

	FILE_HEADER hdr = {};
	if(fread(&hdr, sizeof(hdr), 1, fp) != 1 ||
	   memcmp(hdr.magic, AOT_MAGIC, sizeof(AOT_MAGIC)) != 0 ||
	   hdr.version != AOT_FORMAT_VERSION ||
	   hdr.abiFingerprint != expectedAbiFingerprint ||
	   (expectedElfHash != 0 && hdr.elfContentHash != expectedElfHash) ||
	   hdr.blockCount > MAX_CACHED_BLOCKS_PER_FILE)
	{
		fclose(fp);
		return false;
	}

	outBlocks.reserve(hdr.blockCount);
	for(uint32 i = 0; i < hdr.blockCount; i++)
	{
		BLOCK_HEADER bh = {};
		if(fread(&bh, sizeof(bh), 1, fp) != 1 ||
		   bh.relocCount > MAX_BLOCK_RELOCS ||
		   bh.nativeCodeSize == 0 ||
		   bh.nativeCodeSize > MAX_BLOCK_CODE_SIZE)
		{
			outBlocks.clear();
			fclose(fp);
			return false;
		}
		CACHED_NATIVE_BLOCK blk;
		blk.begin = bh.begin;
		blk.end = bh.end;
		blk.compileHints = bh.compileHints;
		blk.flags = bh.flags;
		blk.linkOffset[0] = bh.linkOffset[0];
		blk.linkOffset[1] = bh.linkOffset[1];
		blk.codeHashLo = bh.codeHashLo;
		blk.codeHashHi = bh.codeHashHi;
		blk.relocs.resize(bh.relocCount);
		blk.nativeCode.resize(bh.nativeCodeSize);
		if((bh.relocCount > 0 && fread(blk.relocs.data(), sizeof(NATIVE_RELOC), bh.relocCount, fp) != bh.relocCount) ||
		   fread(blk.nativeCode.data(), 1, bh.nativeCodeSize, fp) != bh.nativeCodeSize)
		{
			outBlocks.clear();
			fclose(fp);
			return false;
		}
		outBlocks.push_back(std::move(blk));
	}

	fclose(fp);
	return true;
}

bool CNativeCodeCache::SaveFileAtomic(const std::string& path, uint64 abiFingerprint, uint64 elfHash, const std::vector<CACHED_NATIVE_BLOCK>& blocks)
{
	if(blocks.empty() || blocks.size() > MAX_CACHED_BLOCKS_PER_FILE) return false;

	std::error_code ec;
	auto fsPath = fs::path(path);
	if(fsPath.has_parent_path())
	{
		fs::create_directories(fsPath.parent_path(), ec);
	}

	std::string tmpPath = path + ".tmp";
	FILE* fp = fopen(tmpPath.c_str(), "wb");
	if(!fp) return false;

	FILE_HEADER hdr = {};
	memcpy(hdr.magic, AOT_MAGIC, sizeof(AOT_MAGIC));
	hdr.version = AOT_FORMAT_VERSION;
	hdr.blockCount = static_cast<uint32>(blocks.size());
	hdr.abiFingerprint = abiFingerprint;
	hdr.elfContentHash = elfHash;

	bool ok = (fwrite(&hdr, sizeof(hdr), 1, fp) == 1);
	for(const auto& blk : blocks)
	{
		if(!ok) break;
		BLOCK_HEADER bh = {};
		bh.begin = blk.begin;
		bh.end = blk.end;
		bh.compileHints = blk.compileHints;
		bh.flags = blk.flags;
		bh.linkOffset[0] = blk.linkOffset[0];
		bh.linkOffset[1] = blk.linkOffset[1];
		bh.codeHashLo = blk.codeHashLo;
		bh.codeHashHi = blk.codeHashHi;
		bh.relocCount = static_cast<uint32>(blk.relocs.size());
		bh.nativeCodeSize = static_cast<uint32>(blk.nativeCode.size());

		ok = (fwrite(&bh, sizeof(bh), 1, fp) == 1);
		if(ok && bh.relocCount > 0)
		{
			ok = (fwrite(blk.relocs.data(), sizeof(NATIVE_RELOC), bh.relocCount, fp) == bh.relocCount);
		}
		if(ok && bh.nativeCodeSize > 0)
		{
			ok = (fwrite(blk.nativeCode.data(), 1, bh.nativeCodeSize, fp) == bh.nativeCodeSize);
		}
	}

	if(fclose(fp) != 0) ok = false;
	if(!ok)
	{
		fs::remove(tmpPath, ec);
		return false;
	}

	fs::rename(tmpPath, path, ec);
	if(ec)
	{
		ec.clear();
		fs::remove(path, ec);
		ec.clear();
		fs::rename(tmpPath, path, ec);
		if(ec)
		{
			fs::remove(tmpPath, ec);
			return false;
		}
	}
	return true;
}
