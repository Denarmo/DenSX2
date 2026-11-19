#pragma once

#include <vector>
#include "MIPS.h"
#include "MemoryFunction.h"
#include "NativeCodeCache.h"
#ifdef AOT_BUILD_CACHE
#include "StdStream.h"
#include <mutex>
#endif

enum BLOCK_CATEGORY : uint32
{
	BLOCK_CATEGORY_UNKNOWN = 0,
	BLOCK_CATEGORY_PS2_EE = 0x65650000,
	BLOCK_CATEGORY_PS2_IOP = 0x696F7000,
	BLOCK_CATEGORY_PS2_VU = 0x76750000,
	BLOCK_CATEGORY_PSP = 0x50535000,
};

#pragma pack(push, 1)
struct AOT_BLOCK_KEY
{
	BLOCK_CATEGORY category;
	uint128 hash;
	uint32 size;

	bool operator<(const AOT_BLOCK_KEY& k2) const
	{
		const auto& k1 = (*this);
		return std::tie(k1.category, k1.hash, k1.size) <
		       std::tie(k2.category, k2.hash, k2.size);
	}
};
#pragma pack(pop)
static_assert(sizeof(AOT_BLOCK_KEY) == 0x18, "AOT_BLOCK_KEY must be 24 bytes long.");

namespace Jitter
{
	class CJitter;
};

extern "C"
{
	void EmptyBlockHandler(CMIPS*);
	void NextBlockTrampoline(CMIPS*);
	void BranchBlockTrampoline(CMIPS*);
}

//Makes the context's blocks run in a fixed frame (see CMIPS::m_fixedFrameEntry) where the code generator
//supports it. Returns whether it did; blocks compiled before must be compiled again.
bool SetupBlockFixedFrame(CMIPS&);

enum LINK_SLOT
{
	LINK_SLOT_NEXT,
	LINK_SLOT_BRANCH,
	LINK_SLOT_MAX,
};

//Block outgoing link
struct BLOCK_OUT_LINK
{
	LINK_SLOT slot;    //slot used in the source block
	uint32 srcAddress; //address of source block
	bool live;         //live if linked to another block, otherwise, link is pending
};

//Block outgoing links map (key: target link address, value: struct describing link status)
typedef std::multimap<uint32, BLOCK_OUT_LINK> BlockOutLinkMap;

//When block linking is used, each basic block will maintain pointers
//to their outgoing link definitions inside the map
typedef BlockOutLinkMap::iterator BlockOutLinkPointer;

class CBasicBlock : public std::enable_shared_from_this<CBasicBlock>
{
public:
	CBasicBlock(CMIPS&, uint32 = MIPS_INVALID_PC, uint32 = MIPS_INVALID_PC, BLOCK_CATEGORY = BLOCK_CATEGORY_UNKNOWN);
	virtual ~CBasicBlock() = default;
	void Execute();
	void Compile();
	bool CompileForCache(std::vector<uint8>& outCode, std::vector<NATIVE_RELOC>& outRelocs, uint32 outLinkOffsets[LINK_SLOT_MAX]);
	bool LoadFromNativeCache(const CACHED_NATIVE_BLOCK& cached);

	//Entry point of the compiled code, called directly by the executor's dispatch loop
	typedef void (*CodeFunction)(void*);
	CodeFunction GetCode() const
	{
		return m_code;
	}
	size_t GetCodeSize() const
	{
#ifndef AOT_USE_CACHE
		return m_function.GetSize();
#else
		return 0;
#endif
	}
	virtual void CompileRange(CMipsJitter*);
	virtual bool EndsWithDynamicBranch() const;

	void AddBlockCompileHints(uint32);

	uint32 GetBeginAddress() const
	{
		return m_begin;
	}
	uint32 GetEndAddress() const
	{
		return m_end;
	}
	bool IsCompiled() const
	{
#ifndef AOT_USE_CACHE
		return !m_function.IsEmpty();
#else
		return (m_function != nullptr);
#endif
	}
	bool IsEmpty() const
	{
		return (m_begin == MIPS_INVALID_PC) &&
		       (m_end == MIPS_INVALID_PC);
	}

	uint32 GetRecycleCount() const
	{
		return m_recycleCount;
	}
	void SetRecycleCount(uint32 recycleCount)
	{
		m_recycleCount = recycleCount;
	}

	bool HasLinkSlot(LINK_SLOT) const;
	BlockOutLinkPointer GetOutLink(LINK_SLOT) const;
	void SetOutLink(LINK_SLOT, BlockOutLinkPointer);

	void LinkBlock(LINK_SLOT, CBasicBlock*);
	void UnlinkBlock(LINK_SLOT);

#ifdef AOT_BUILD_CACHE
	static void SetAotBlockOutputStream(Framework::CStdStream*);
#endif

	void CopyFunctionFrom(const std::shared_ptr<CBasicBlock>& basicBlock);
	bool HasCallLinkSlot() const { return m_hasCallLinkSlot; }
	uint64 GetCodeHash64() const { return m_codeHash64; }
	void SetCodeHash64(uint64 hash) { m_codeHash64 = hash; }

protected:
	uint32 m_begin;
	uint32 m_end;
	BLOCK_CATEGORY m_category;
	CMIPS& m_context;

	uint32 m_blockCompileHints = 0;

	virtual void CompileProlog(CMipsJitter*);
	virtual void CompileEpilog(CMipsJitter*, bool);
	virtual bool MaySetExceptionInEpilog() const { return false; }

private:
	void HandleExternalFunctionReference(uintptr_t, uint32, Jitter::CCodeGen::SYMBOL_REF_TYPE);

#ifdef DEBUGGER_INCLUDED
	bool HasBreakpoint() const;
	static uint32 BreakpointFilter(CMIPS*);
	static void BreakpointHandler(CMIPS*);
#endif

#ifdef AOT_BUILD_CACHE
	static Framework::CStdStream* m_aotBlockOutputStream;
	static std::mutex m_aotBlockOutputStreamMutex;
#endif

#ifndef AOT_USE_CACHE
	CMemoryFunction m_function;
#else
	void (*m_function)(void*);
#endif
	CodeFunction m_code = nullptr;
	uint32 m_recycleCount = 0;
	BlockOutLinkPointer m_outLinks[LINK_SLOT_MAX];
	uint32 m_linkBlockTrampolineOffset[LINK_SLOT_MAX];
	std::vector<NATIVE_RELOC>* m_recordingRelocs = nullptr;
	bool m_recordingRelocsValid = true;
	bool m_hasCallLinkSlot = false;
	uint64 m_codeHash64 = 0;
#ifdef _DEBUG
	CBasicBlock* m_linkBlock[LINK_SLOT_MAX];
#endif
};
