#include <cstring>
#include "BasicBlock.h"
#include "MemStream.h"
#include "offsetof_def.h"
#include "MipsJitter.h"
#include "Jitter_CodeGenFactory.h"
#if defined(__aarch64__) && !defined(__APPLE__) && !defined(AOT_ENABLED)
#include "Jitter_CodeGen_AArch64.h"
#endif

#if defined(AOT_BUILD_CACHE) || defined(AOT_USE_CACHE)
#define AOT_ENABLED
#endif

#ifdef AOT_ENABLED

#include "xxhash.h"

#include "StdStream.h"
#include "StdStreamUtils.h"

#endif

#ifdef VTUNE_ENABLED
#include <jitprofiling.h>
#include "string_format.h"
#endif

#ifdef AOT_USE_CACHE

#pragma pack(push, 1)
struct AOT_BLOCK
{
	AOT_BLOCK_KEY key;
	void* fct;
};
#pragma pack(pop)

extern "C"
{
	extern AOT_BLOCK _aot_firstBlock;
	extern uint32 _aot_blockCount;
}

#endif

#define INVALID_LINK_SLOT (~0U)

CBasicBlock::CBasicBlock(CMIPS& context, uint32 begin, uint32 end, BLOCK_CATEGORY category)
    : m_begin(begin)
    , m_end(end)
    , m_category(category)
    , m_context(context)
#ifdef AOT_USE_CACHE
    , m_function(nullptr)
#endif
{
	assert(m_end >= m_begin);
	for(uint32 i = 0; i < LINK_SLOT_MAX; i++)
	{
#ifdef _DEBUG
		m_linkBlock[i] = nullptr;
#endif
		m_linkBlockTrampolineOffset[i] = INVALID_LINK_SLOT;
	}
}

#ifdef AOT_BUILD_CACHE

Framework::CStdStream* CBasicBlock::m_aotBlockOutputStream(nullptr);
std::mutex CBasicBlock::m_aotBlockOutputStreamMutex;

void CBasicBlock::SetAotBlockOutputStream(Framework::CStdStream* outputStream)
{
	assert(m_aotBlockOutputStream == nullptr || outputStream == nullptr);
	m_aotBlockOutputStream = outputStream;
}

#endif

void CBasicBlock::Compile()
{
#ifndef AOT_USE_CACHE

	Framework::CMemStream stream;
	m_hasCallLinkSlot = false;
	{
		static thread_local std::unique_ptr<CMipsJitter> jitter;
		if(!jitter)
		{
			Jitter::CCodeGen* codeGen = Jitter::CreateCodeGen();
			jitter = std::make_unique<CMipsJitter>(codeGen);
		}

		jitter->GetCodeGen()->SetFixedFrameExit(m_context.m_fixedFrameExit);
		jitter->GetCodeGen()->SetFixedFrameRamBasePinned((m_context.m_fixedFrameExit != nullptr) && (m_context.m_ramBase != nullptr));
		jitter->GetCodeGen()->SetExternalSymbolReferencedHandler([&](auto symbol, auto offset, auto refType) { this->HandleExternalFunctionReference(symbol, offset, refType); });
		jitter->SetStream(&stream);
		jitter->Begin();
		CompileRange(jitter.get());
		jitter->End();
	}

	if(m_context.m_fixedFrameExit)
	{
		//Unlinked exits leave the frame instead of calling the trampolines
		auto* rawCode = reinterpret_cast<uint8*>(stream.GetBuffer());
		uintptr_t exitAddr = reinterpret_cast<uintptr_t>(m_context.m_fixedFrameExit);
		for(uint32 i = 0; i < LINK_SLOT_MAX; i++)
		{
			if(HasLinkSlot(static_cast<LINK_SLOT>(i)))
			{
				memcpy(rawCode + m_linkBlockTrampolineOffset[i], &exitAddr, sizeof(uintptr_t));
			}
		}
	}
	m_function = CMemoryFunction(stream.GetBuffer(), stream.GetSize());
	m_code = reinterpret_cast<CodeFunction>(m_function.GetCode());

#ifdef VTUNE_ENABLED
	if(iJIT_IsProfilingActive() == iJIT_SAMPLING_ON)
	{
		iJIT_Method_Load jmethod = {};
		jmethod.method_id = iJIT_GetNewMethodID();
		jmethod.class_file_name = "";
		jmethod.source_file_name = __FILE__;

		jmethod.method_load_address = m_function.GetCode();
		jmethod.method_size = m_function.GetSize();
		jmethod.line_number_size = 0;

		auto functionName = string_format("BasicBlock_0x%08X_0x%08X", m_begin, m_end);

		jmethod.method_name = const_cast<char*>(functionName.c_str());
		iJIT_NotifyEvent(iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED, reinterpret_cast<void*>(&jmethod));
	}
#endif

#endif

#ifdef AOT_ENABLED

	size_t blockSize = ((m_end - m_begin) / 4) + 1;
	size_t blockSizeByte = blockSize * 4;
	auto blockData = new uint32[blockSize];

	if(!IsEmpty())
	{
		for(uint32 i = 0; i < blockSize; i++)
		{
			blockData[i] = m_context.m_pMemoryMap->GetInstruction(m_begin + (i * 4));
		}
	}
	else
	{
		//The empty block has no data per se
		assert(blockSize == 1);
		blockData[0] = ~0;
	}

	auto xxHash = XXH3_128bits(blockData, blockSizeByte);
	uint128 hash;
	memcpy(&hash, &xxHash, sizeof(xxHash));

#endif

#ifdef AOT_USE_CACHE

	AOT_BLOCK* blocksBegin = &_aot_firstBlock;
	AOT_BLOCK* blocksEnd = blocksBegin + _aot_blockCount;

	AOT_BLOCK blockRef = {{m_category, hash, blockSizeByte}, nullptr};

	static const auto blockComparer =
	    [](const AOT_BLOCK& item1, const AOT_BLOCK& item2) {
		    return item1.key < item2.key;
	    };

	//	assert(std::is_sorted(blocksBegin, blocksEnd, blockComparer));

	bool blockExists = std::binary_search(blocksBegin, blocksEnd, blockRef, blockComparer);
	auto blockIterator = std::lower_bound(blocksBegin, blocksEnd, blockRef, blockComparer);

	assert(blockExists);
	assert(blockIterator != blocksEnd);
	assert(blockIterator->key.hash == hash);
	assert(blockIterator->key.size == blockSizeByte);

	m_function = reinterpret_cast<void (*)(void*)>(blockIterator->fct);
	m_code = m_function;

#endif

#ifdef AOT_BUILD_CACHE
	if(m_aotBlockOutputStream)
	{
		std::lock_guard<std::mutex> lock(m_aotBlockOutputStreamMutex);

		m_aotBlockOutputStream->Write32(m_category);
		m_aotBlockOutputStream->Write(&hash, sizeof(hash));
		m_aotBlockOutputStream->Write32(blockSizeByte);
		m_aotBlockOutputStream->Write(blockData, blockSizeByte);
	}
#endif
	return;
}

bool CBasicBlock::CompileForCache(std::vector<uint8>& outCode, std::vector<NATIVE_RELOC>& outRelocs, uint32 outLinkOffsets[LINK_SLOT_MAX])
{
	outCode.clear();
	outRelocs.clear();
	for(uint32 i = 0; i < LINK_SLOT_MAX; i++)
	{
		outLinkOffsets[i] = INVALID_LINK_SLOT;
		m_linkBlockTrampolineOffset[i] = INVALID_LINK_SLOT;
	}

#if !defined(AOT_USE_CACHE) && !defined(AOT_BUILD_CACHE) && !defined(__EMSCRIPTEN__)
	Framework::CMemStream stream;
	m_recordingRelocs = &outRelocs;
	m_recordingRelocsValid = true;
	m_hasCallLinkSlot = false;
	{
		static thread_local std::unique_ptr<CMipsJitter> jitter;
		if(!jitter)
		{
			Jitter::CCodeGen* codeGen = Jitter::CreateCodeGen();
			jitter = std::make_unique<CMipsJitter>(codeGen);
		}

		jitter->GetCodeGen()->SetFixedFrameExit(m_context.m_fixedFrameExit);
		jitter->GetCodeGen()->SetFixedFrameRamBasePinned((m_context.m_fixedFrameExit != nullptr) && (m_context.m_ramBase != nullptr));
		jitter->GetCodeGen()->SetExternalSymbolReferencedHandler([&](auto symbol, auto offset, auto refType) { this->HandleExternalFunctionReference(symbol, offset, refType); });
		jitter->SetStream(&stream);
		jitter->Begin();
		CompileRange(jitter.get());
		jitter->End();
	}
	m_recordingRelocs = nullptr;

	size_t codeSize = stream.GetSize();
	auto* rawCode = reinterpret_cast<uint8*>(stream.GetBuffer());
	if(m_recordingRelocsValid && codeSize > 0)
	{
		outCode.assign(rawCode, rawCode + codeSize);
		for(uint32 i = 0; i < LINK_SLOT_MAX; i++)
		{
			outLinkOffsets[i] = m_linkBlockTrampolineOffset[i];
		}
	}

	if(m_context.m_fixedFrameExit)
	{
		uintptr_t exitAddr = reinterpret_cast<uintptr_t>(m_context.m_fixedFrameExit);
		for(uint32 i = 0; i < LINK_SLOT_MAX; i++)
		{
			if(HasLinkSlot(static_cast<LINK_SLOT>(i)))
			{
				memcpy(rawCode + m_linkBlockTrampolineOffset[i], &exitAddr, sizeof(uintptr_t));
			}
		}
	}
	m_function = CMemoryFunction(rawCode, codeSize);
	m_code = reinterpret_cast<CodeFunction>(m_function.GetCode());
	return m_recordingRelocsValid && !outCode.empty();
#else
	Compile();
	return false;
#endif
}

bool CBasicBlock::LoadFromNativeCache(const CACHED_NATIVE_BLOCK& cached)
{
#if !defined(AOT_USE_CACHE) && !defined(AOT_BUILD_CACHE) && !defined(__EMSCRIPTEN__)
	if(cached.nativeCode.empty()) return false;
	for(uint32 i = 0; i < LINK_SLOT_MAX; i++)
	{
		if(cached.linkOffset[i] != INVALID_LINK_SLOT &&
		   static_cast<size_t>(cached.linkOffset[i]) + sizeof(uintptr_t) > cached.nativeCode.size())
		{
			return false;
		}
	}

	std::vector<uint8> patched = cached.nativeCode;
	if(!CNativeCodeCache::ApplyRelocations(patched.data(), patched.size(), cached.relocs.data(), cached.relocs.size(), m_context))
	{
		return false;
	}

	for(uint32 i = 0; i < LINK_SLOT_MAX; i++)
	{
		m_linkBlockTrampolineOffset[i] = cached.linkOffset[i];
#ifdef _DEBUG
		m_linkBlock[i] = nullptr;
#endif
	}

	if(m_context.m_fixedFrameExit)
	{
		uintptr_t exitAddr = reinterpret_cast<uintptr_t>(m_context.m_fixedFrameExit);
		for(uint32 i = 0; i < LINK_SLOT_MAX; i++)
		{
			if(HasLinkSlot(static_cast<LINK_SLOT>(i)))
			{
				memcpy(patched.data() + m_linkBlockTrampolineOffset[i], &exitAddr, sizeof(uintptr_t));
			}
		}
	}

	m_hasCallLinkSlot = (cached.flags & NATIVE_BLOCK_FLAG_CALL_LINK) != 0;
	m_function = CMemoryFunction(patched.data(), patched.size());
	m_code = reinterpret_cast<CodeFunction>(m_function.GetCode());
	return true;
#else
	return false;
#endif
}

void CBasicBlock::AddBlockCompileHints(uint32 compileHints)
{
	m_blockCompileHints |= compileHints;
}

void CBasicBlock::CompileRange(CMipsJitter* jitter)
{
	if(IsEmpty())
	{
		jitter->JumpTo(reinterpret_cast<void*>(&EmptyBlockHandler));
		return;
	}

	bool loopsOnItself = [&]() {
		if(m_begin == m_end)
		{
			return false;
		}
		uint32 branchInstAddr = m_end - 4;
		uint32 inst = m_context.m_pMemoryMap->GetInstruction(branchInstAddr);
		if(m_context.m_pArch->IsInstructionBranch(&m_context, branchInstAddr, inst) != MIPS_BRANCH_NORMAL)
		{
			return false;
		}
		uint32 target = m_context.m_pArch->GetInstructionEffectiveAddress(&m_context, branchInstAddr, inst);
		if(target == MIPS_INVALID_PC)
		{
			return false;
		}
		return target == m_begin;
	}();

	m_context.m_pArch->SetCompileHints(m_blockCompileHints);

	CompileProlog(jitter);
	jitter->MarkFirstBlockLabel();

	for(uint32 address = m_begin; address <= m_end; address += 4)
	{
		m_context.m_pArch->CompileInstruction(
		    address,
		    jitter,
		    &m_context, address - m_begin);
		//Sanity check
		assert(jitter->IsStackEmpty());
	}

	jitter->MarkLastBlockLabel();
	CompileEpilog(jitter, loopsOnItself);
}

bool CBasicBlock::EndsWithDynamicBranch() const
{
	if(m_begin == m_end) return false;
	uint32 branchInstAddr = m_end - 4;
	uint32 inst = m_context.m_pMemoryMap->GetInstruction(branchInstAddr);
	if(m_context.m_pArch->IsInstructionBranch(&m_context, branchInstAddr, inst) != MIPS_BRANCH_NORMAL)
	{
		return false;
	}
	return m_context.m_pArch->GetInstructionEffectiveAddress(&m_context, branchInstAddr, inst) == MIPS_INVALID_PC;
}

void CBasicBlock::CompileProlog(CMipsJitter* jitter)
{
#ifdef DEBUGGER_INCLUDED
	if(HasBreakpoint())
	{
		jitter->PushCtx();
		jitter->Call(reinterpret_cast<void*>(&BreakpointFilter), 1, Jitter::CJitter::RETURN_VALUE_32);

		jitter->PushCst(0);
		jitter->BeginIf(Jitter::CONDITION_EQ);
		{
			jitter->JumpTo(reinterpret_cast<void*>(&BreakpointHandler));
		}
		jitter->EndIf();
	}
#endif
}

void CBasicBlock::CompileEpilog(CMipsJitter* jitter, bool loopsOnItself)
{
	uint32 blockInstrCount = ((m_end - m_begin) / 4) + 1;

	//Analyze EE block termination statically so fallthrough and unconditional-branch blocks
	//avoid dynamic nDelayedJumpAddr checks, dead epilog code, and per-block quota branches on short forward blocks.
	bool hasBranch = true;
	bool isUnconditionalBranch = false;
	bool isJalCall = false;
	bool isJalrCall = false;
	bool isJrRa = false;
	bool needsQuotaCheck = true;
	bool blockCanRaiseException = true;
	if(m_category == BLOCK_CATEGORY_PS2_EE)
	{
		hasBranch = false;
		needsQuotaCheck = (blockInstrCount >= 48);
		blockCanRaiseException = false;
		for(uint32 address = m_begin; address <= m_end; address += 4)
		{
			uint32 inst = m_context.m_pMemoryMap->GetInstruction(address);
			uint32 instOp = (inst >> 26) & 0x3F;
			if((instOp == 0x10) || (instOp == 0x12) || ((instOp >= 0x1A) && (instOp != 0x1C)))
			{
				blockCanRaiseException = true;
			}
			else if(instOp == 0x00)
			{
				uint32 instFunct = inst & 0x3F;
				if((instFunct == 0x0C) || (instFunct == 0x0D) || ((instFunct >= 0x30) && (instFunct <= 0x37)))
				{
					blockCanRaiseException = true;
				}
			}
			else if(instOp == 0x01)
			{
				uint32 instRt = (inst >> 16) & 0x1F;
				if((instRt >= 0x08) && (instRt <= 0x0F))
				{
					blockCanRaiseException = true;
				}
			}
			auto branchType = m_context.m_pArch->IsInstructionBranch(&m_context, address, inst);
			if(branchType == MIPS_BRANCH_NODELAY)
			{
				hasBranch = true;
				needsQuotaCheck = true;
				break;
			}
			else if(branchType == MIPS_BRANCH_NORMAL)
			{
				hasBranch = true;
				if(address == (m_end - 4))
				{
					uint32 delayInst = m_context.m_pMemoryMap->GetInstruction(m_end);
					bool delayIsBranch = m_context.m_pArch->IsInstructionBranch(&m_context, m_end, delayInst) != MIPS_BRANCH_NONE;
					if(!delayIsBranch)
					{
						uint32 op = (inst >> 26) & 0x3F;
						uint32 rs = (inst >> 21) & 0x1F;
						uint32 rt = (inst >> 16) & 0x1F;
						uint32 rd = (inst >> 11) & 0x1F;
						uint32 funct = inst & 0x3F;
						if((op == 0x02) || (op == 0x03) ||
						   ((op == 0x00) && ((funct == 0x08) || (funct == 0x09))) ||
						   ((op == 0x04) && (rs == rt)) ||
						   ((op == 0x01) && (rs == 0) && ((rt == 0x01) || (rt == 0x03) || (rt == 0x11) || (rt == 0x13))) ||
						   ((op == 0x06) && (rs == 0)))
						{
							isUnconditionalBranch = true;
						}
						bool isCallOp = (op == 0x03) || ((op == 0x00) && (funct == 0x09) && (rd == CMIPS::RA));
						if(isCallOp && (m_end < 0x01FFFFFCU))
						{
							uint32 dOp = (delayInst >> 26) & 0x3F;
							uint32 dRd = (delayInst >> 11) & 0x1F;
							uint32 dRt = (delayInst >> 16) & 0x1F;
							bool delaySafeForRa =
							    (delayInst == 0) ||
							    ((dOp == 0x00) && (dRd != CMIPS::RA)) ||
							    ((dOp == 0x1C) && (dRd != CMIPS::RA)) || //MMI
							    ((dOp >= 0x28) && (dOp <= 0x2F)) ||      //SB/SH/SW/SWL/SWR/SD/SDL/SDR
							    ((dOp >= 0x38) && (dOp <= 0x3F)) ||      //SC/SWC1/SQC2/SD/SQ
							    ((dOp >= 0x08) && (dOp <= 0x0F) && (dRt != CMIPS::RA)) || //ALU imm
							    ((dOp >= 0x18) && (dOp <= 0x1B) && (dRt != CMIPS::RA)) || //DADDIU/LDL/LDR
							    ((dOp >= 0x20) && (dOp <= 0x27) && (dRt != CMIPS::RA)) || //Loads
							    ((dOp == 0x37) && (dRt != CMIPS::RA));                    //LD
							if(delaySafeForRa)
							{
								if(op == 0x03)
								{
									isJalCall = true;
								}
								else
								{
									isJalrCall = true;
								}
							}
						}
						else if((op == 0x00) && (funct == 0x08) && (rs == CMIPS::RA))
						{
							isJrRa = true;
						}
						if((op == 0x00) && ((funct == 0x08) || (funct == 0x09)))
						{
							needsQuotaCheck = true;
						}
						else
						{
							uint32 target = m_context.m_pArch->GetInstructionEffectiveAddress(&m_context, address, inst);
							if((target == MIPS_INVALID_PC) || (target <= m_end))
							{
								needsQuotaCheck = true;
							}
						}
					}
					else
					{
						needsQuotaCheck = true;
					}
				}
				else
				{
					needsQuotaCheck = true;
				}
				break;
			}
		}
	}

	//Update cycle quota
	jitter->PushRel(offsetof(CMIPS, m_State.cycleQuota));
	jitter->PushCst(blockInstrCount);
	jitter->Sub();
	jitter->PullRel(offsetof(CMIPS, m_State.cycleQuota));

	if(needsQuotaCheck)
	{
		jitter->PushRel(offsetof(CMIPS, m_State.cycleQuota));
		jitter->PushCst(0);
		jitter->BeginIf(Jitter::CONDITION_LE);
		{
			jitter->PushRel(offsetof(CMIPS, m_State.nHasException));
			jitter->PushCst(MIPS_EXCEPTION_STATUS_QUOTADONE);
			jitter->Or();
			jitter->PullRel(offsetof(CMIPS, m_State.nHasException));
		}
		jitter->EndIf();
	}

	bool skipExceptionCheck = (m_category == BLOCK_CATEGORY_PS2_EE) && !needsQuotaCheck && !blockCanRaiseException && !MaySetExceptionInEpilog();
#ifdef DEBUGGER_INCLUDED
	if(HasBreakpoint()) skipExceptionCheck = false;
#endif

	auto emitTakenBranch = [&]() {
		jitter->PushRel(offsetof(CMIPS, m_State.nDelayedJumpAddr));
		jitter->PullRel(offsetof(CMIPS, m_State.nPC));

		jitter->PushCst(MIPS_INVALID_PC);
		jitter->PullRel(offsetof(CMIPS, m_State.nDelayedJumpAddr));

		if(loopsOnItself)
		{
			jitter->PushRel(offsetof(CMIPS, m_State.nHasException));
			jitter->PushCst(0);
			jitter->BeginIf(Jitter::CONDITION_EQ);
			{
				jitter->Goto(jitter->GetFirstBlockLabel());
			}
			jitter->EndIf();
		}
		else
		{
#if !defined(AOT_BUILD_CACHE) && !defined(__EMSCRIPTEN__)
			auto emitTakenDispatch = [&]() {
				if(EndsWithDynamicBranch() && m_context.m_blockCodeLookup && jitter->SupportsExternJmpRef())
				{
					if(isJrRa && m_context.m_fixedFrameExit)
					{
						jitter->PushRel(offsetof(CMIPS, m_State.nPC));
						jitter->CheckRasReturn();
					}
					//Find the target's code here: each such exit gets its own branch prediction
					jitter->PushRelRef(offsetof(CMIPS, m_blockCodeLookup));
					jitter->PushRel(offsetof(CMIPS, m_State.nPC));
					jitter->PushCst(m_context.m_blockCodeLookupMask);
					jitter->And();
					jitter->Srl(16);
					jitter->LoadRefFromRefIdx();
					jitter->PushRel(offsetof(CMIPS, m_State.nPC));
					jitter->PushCst(m_context.m_blockCodeLookupMask & 0xFFFCU);
					jitter->And();
					jitter->Srl(2);
					jitter->LoadRefFromRefIdx();
					if(isJalrCall && m_context.m_fixedFrameExit)
					{
						jitter->PushRel(offsetof(CMIPS, m_State.nGPR[CMIPS::RA].nV[0]));
						jitter->CallToRef(reinterpret_cast<void*>(&NextBlockTrampoline));
					}
					else
					{
						jitter->JumpToRef();
					}
				}
				else if(isJalCall && m_context.m_fixedFrameExit && jitter->SupportsExternJmpRef())
				{
					m_hasCallLinkSlot = true;
					jitter->PushRel(offsetof(CMIPS, m_State.nGPR[CMIPS::RA].nV[0]));
					jitter->CallToDynamic(reinterpret_cast<void*>(&BranchBlockTrampoline), reinterpret_cast<void*>(&NextBlockTrampoline));
				}
				else
				{
					jitter->JumpToDynamic(reinterpret_cast<void*>(&BranchBlockTrampoline));
				}
			};
			if(skipExceptionCheck)
			{
				emitTakenDispatch();
			}
			else
			{
				jitter->PushRel(offsetof(CMIPS, m_State.nHasException));
				jitter->PushCst(0);
				jitter->BeginIf(Jitter::CONDITION_EQ);
				{
					emitTakenDispatch();
				}
				jitter->EndIf();
			}
#endif
		}
	};

	auto emitFallthrough = [&]() {
		jitter->PushRel(offsetof(CMIPS, m_State.nPC));
		jitter->PushCst(m_end - m_begin + 4);
		jitter->Add();
		jitter->PullRel(offsetof(CMIPS, m_State.nPC));

#if !defined(AOT_BUILD_CACHE) && !defined(__EMSCRIPTEN__)
		if(skipExceptionCheck)
		{
			jitter->JumpToDynamic(reinterpret_cast<void*>(&NextBlockTrampoline));
		}
		else
		{
			jitter->PushRel(offsetof(CMIPS, m_State.nHasException));
			jitter->PushCst(0);
			jitter->BeginIf(Jitter::CONDITION_EQ);
			{
				jitter->JumpToDynamic(reinterpret_cast<void*>(&NextBlockTrampoline));
			}
			jitter->EndIf();
		}
#endif
	};

	if(!hasBranch)
	{
		emitFallthrough();
	}
	else if(isUnconditionalBranch)
	{
		emitTakenBranch();
	}
	else
	{
		jitter->PushCst(MIPS_INVALID_PC);
		jitter->PushRel(offsetof(CMIPS, m_State.nDelayedJumpAddr));
		jitter->BeginIf(Jitter::CONDITION_NE);
		{
			emitTakenBranch();
		}
		jitter->Else();
		{
			emitFallthrough();
		}
		jitter->EndIf();
	}
}

void CBasicBlock::Execute()
{
	if(m_context.m_fixedFrameEntry)
	{
		m_context.m_fixedFrameEntry(&m_context, m_function.GetCode());
	}
	else
	{
		m_function(&m_context);
	}

	assert(m_context.m_State.nGPR[0].nV0 == 0);
	assert(m_context.m_State.nGPR[0].nV1 == 0);
	assert(m_context.m_State.nGPR[0].nV2 == 0);
	assert(m_context.m_State.nGPR[0].nV3 == 0);
	assert((m_context.m_State.nPC & 3) == 0);
	assert(m_context.m_State.nCOP2[0].nV0 == 0x00000000);
	assert(m_context.m_State.nCOP2[0].nV1 == 0x00000000);
	assert(m_context.m_State.nCOP2[0].nV2 == 0x00000000);
	assert(m_context.m_State.nCOP2[0].nV3 == 0x3F800000);
	assert(m_context.m_State.nCOP2VI[0] == 0);
}

bool CBasicBlock::HasLinkSlot(LINK_SLOT linkSlot) const
{
	return m_linkBlockTrampolineOffset[linkSlot] != INVALID_LINK_SLOT;
}

BlockOutLinkPointer CBasicBlock::GetOutLink(LINK_SLOT linkSlot) const
{
	assert(linkSlot < LINK_SLOT_MAX);
	return m_outLinks[linkSlot];
}

void CBasicBlock::SetOutLink(LINK_SLOT linkSlot, BlockOutLinkPointer link)
{
	assert(linkSlot < LINK_SLOT_MAX);
	m_outLinks[linkSlot] = link;
}

void CBasicBlock::LinkBlock(LINK_SLOT linkSlot, CBasicBlock* otherBlock)
{
#if !defined(AOT_ENABLED) && !defined(__EMSCRIPTEN__)
	assert(!IsEmpty());
	assert(!otherBlock->IsEmpty());
	assert(linkSlot < LINK_SLOT_MAX);
	assert(m_linkBlockTrampolineOffset[linkSlot] != INVALID_LINK_SLOT);
#ifdef _DEBUG
	assert(m_linkBlock[linkSlot] == nullptr);
	m_linkBlock[linkSlot] = otherBlock;
#endif
	auto patchValue = reinterpret_cast<uintptr_t>(otherBlock->m_function.GetCode());
	auto code = reinterpret_cast<uint8*>(m_function.GetCode());
	m_function.BeginModify();
#if defined(__aarch64__) && !defined(__APPLE__)
	if(m_context.m_fixedFrameExit && (m_linkBlockTrampolineOffset[linkSlot] >= 8))
	{
		auto* instrPtr = reinterpret_cast<uint32*>(code + m_linkBlockTrampolineOffset[linkSlot] - 8);
		intptr_t delta = static_cast<intptr_t>(patchValue) - static_cast<intptr_t>(reinterpret_cast<uintptr_t>(instrPtr));
		if(((delta & 3) == 0) && (delta >= -0x08000000) && (delta <= 0x07FFFFFC))
		{
			//Direct PC-relative B target (0-cycle taken branch, no I-cache literal load or indirect BR)
			*instrPtr = 0x14000000U | (static_cast<uint32>(delta >> 2) & 0x03FFFFFFU);
		}
		else
		{
			*instrPtr = 0x58000050U; //ldr x16, [pc, #8]
		}
		if(m_hasCallLinkSlot && (linkSlot == LINK_SLOT_BRANCH) && (m_linkBlockTrampolineOffset[linkSlot] >= 28))
		{
			auto* blPtr = reinterpret_cast<uint32*>(code + m_linkBlockTrampolineOffset[linkSlot] - 28);
			intptr_t blDelta = static_cast<intptr_t>(patchValue) - static_cast<intptr_t>(reinterpret_cast<uintptr_t>(blPtr));
			if(((blDelta & 3) == 0) && (blDelta >= -0x08000000) && (blDelta <= 0x07FFFFFC))
			{
				//Direct PC-relative BL target (pushes return_here onto CPU hardware RAS)
				*blPtr = 0x94000000U | (static_cast<uint32>(blDelta >> 2) & 0x03FFFFFFU);
			}
			else
			{
				*blPtr = 0x94000005U; //bl +20 (unlinked_branch_stub)
			}
		}
	}
#endif
	*reinterpret_cast<uintptr_t*>(code + m_linkBlockTrampolineOffset[linkSlot]) = patchValue;
	m_function.EndModify();
#endif //!AOT_ENABLED && !__EMSCRIPTEN__
}

void CBasicBlock::UnlinkBlock(LINK_SLOT linkSlot)
{
#if !defined(AOT_ENABLED) && !defined(__EMSCRIPTEN__)
	assert(!IsEmpty());
	assert(linkSlot < LINK_SLOT_MAX);
	assert(m_linkBlockTrampolineOffset[linkSlot] != INVALID_LINK_SLOT);
#ifdef _DEBUG
	m_linkBlock[linkSlot] = nullptr;
#endif
	auto patchValue = (linkSlot == LINK_SLOT_NEXT) ? reinterpret_cast<uintptr_t>(&NextBlockTrampoline) : reinterpret_cast<uintptr_t>(&BranchBlockTrampoline);
	if(m_context.m_fixedFrameExit)
	{
		patchValue = reinterpret_cast<uintptr_t>(m_context.m_fixedFrameExit);
	}
	auto code = reinterpret_cast<uint8*>(m_function.GetCode());
	m_function.BeginModify();
#if defined(__aarch64__) && !defined(__APPLE__)
	if(m_context.m_fixedFrameExit && (m_linkBlockTrampolineOffset[linkSlot] >= 8))
	{
		auto* instrPtr = reinterpret_cast<uint32*>(code + m_linkBlockTrampolineOffset[linkSlot] - 8);
		*instrPtr = 0x58000050U; //ldr x16, [pc, #8]
		if(m_hasCallLinkSlot && (linkSlot == LINK_SLOT_BRANCH) && (m_linkBlockTrampolineOffset[linkSlot] >= 28))
		{
			auto* blPtr = reinterpret_cast<uint32*>(code + m_linkBlockTrampolineOffset[linkSlot] - 28);
			*blPtr = 0x94000005U; //bl +20 (unlinked_branch_stub)
		}
	}
#endif
	*reinterpret_cast<uintptr_t*>(code + m_linkBlockTrampolineOffset[linkSlot]) = patchValue;
	m_function.EndModify();
#endif //!AOT_ENABLED && !__EMSCRIPTEN__
}

void CBasicBlock::HandleExternalFunctionReference(uintptr_t symbol, uint32 offset, Jitter::CCodeGen::SYMBOL_REF_TYPE refType)
{
	if(symbol == reinterpret_cast<uintptr_t>(&NextBlockTrampoline))
	{
		assert(refType == Jitter::CCodeGen::SYMBOL_REF_TYPE::NATIVE_POINTER);
		assert(m_linkBlockTrampolineOffset[LINK_SLOT_NEXT] == INVALID_LINK_SLOT);
		m_linkBlockTrampolineOffset[LINK_SLOT_NEXT] = offset;
	}
	else if(symbol == reinterpret_cast<uintptr_t>(&BranchBlockTrampoline))
	{
		assert(refType == Jitter::CCodeGen::SYMBOL_REF_TYPE::NATIVE_POINTER);
		assert(m_linkBlockTrampolineOffset[LINK_SLOT_BRANCH] == INVALID_LINK_SLOT);
		m_linkBlockTrampolineOffset[LINK_SLOT_BRANCH] = offset;
	}

	if(m_recordingRelocs != nullptr)
	{
		if(refType == Jitter::CCodeGen::SYMBOL_REF_TYPE::NATIVE_POINTER ||
		   refType == Jitter::CCodeGen::SYMBOL_REF_TYPE::ARMV8_MOVZ_MOVK_64)
		{
			uint16 symbolId = 0;
			if(CNativeCodeCache::LookupSymbolId(symbol, m_context, symbolId))
			{
				NATIVE_RELOC reloc = {};
				reloc.offset = offset;
				reloc.symbolId = symbolId;
				reloc.refType = static_cast<uint8>(refType);
				m_recordingRelocs->push_back(reloc);
			}
			else
			{
				m_recordingRelocsValid = false;
			}
		}
		else
		{
			m_recordingRelocsValid = false;
		}
	}
}

void CBasicBlock::CopyFunctionFrom(const std::shared_ptr<CBasicBlock>& other)
{
#ifndef AOT_USE_CACHE
	m_function = other->m_function.CreateInstance();
	m_code = reinterpret_cast<CodeFunction>(m_function.GetCode());
	m_hasCallLinkSlot = other->m_hasCallLinkSlot;
	std::copy(std::begin(other->m_linkBlockTrampolineOffset), std::end(other->m_linkBlockTrampolineOffset), m_linkBlockTrampolineOffset);
#ifdef _DEBUG
	std::copy(std::begin(other->m_linkBlock), std::end(other->m_linkBlock), m_linkBlock);
#endif
	if(
	    HasLinkSlot(LINK_SLOT_NEXT)
#ifdef _DEBUG
	    && m_linkBlock[LINK_SLOT_NEXT]
#endif
	)
	{
		UnlinkBlock(LINK_SLOT_NEXT);
	}
	if(
	    HasLinkSlot(LINK_SLOT_BRANCH)
#ifdef _DEBUG
	    && m_linkBlock[LINK_SLOT_BRANCH]
#endif
	)
	{
		UnlinkBlock(LINK_SLOT_BRANCH);
	}
#else
	m_function = basicBlock->m_function;
#endif
}

#ifdef DEBUGGER_INCLUDED

bool CBasicBlock::HasBreakpoint() const
{
	return m_context.HasBreakpointInRange(GetBeginAddress(), GetEndAddress());
}

uint32 CBasicBlock::BreakpointFilter(CMIPS* context)
{
	return context->m_executor->FilterBreakpoint();
}

void CBasicBlock::BreakpointHandler(CMIPS* context)
{
	assert(context->m_State.nHasException == MIPS_EXCEPTION_NONE);
	context->m_State.nHasException = MIPS_EXCEPTION_BREAKPOINT;
}

#endif

bool SetupBlockFixedFrame(CMIPS& context)
{
#if defined(__aarch64__) && !defined(__APPLE__) && !defined(AOT_ENABLED)
	//One frame for all: blocks can outlive the executors that made them
	static const auto frame = []() {
		Framework::CMemStream stream;
		uint32 exitOffset = 0;
		Jitter::CCodeGen_AArch64::GenerateFixedFrame(stream, exitOffset, static_cast<uint32>(offsetof(CMIPS, m_ramBase)));
		auto function = new CMemoryFunction(stream.GetBuffer(), stream.GetSize());
		return std::make_pair(function->GetCode(), reinterpret_cast<uint8*>(function->GetCode()) + exitOffset);
	}();
	context.m_fixedFrameEntry = reinterpret_cast<void (*)(void*, void*)>(frame.first);
	context.m_fixedFrameExit = frame.second;
	return true;
#else
	return false;
#endif
}

void EmptyBlockHandler(CMIPS* context)
{
	context->m_emptyBlockHandler(context);
}

//The trampolines are told apart by address when linking, so their bodies must differ:
//identical code folding (MSVC /OPT:ICF) would otherwise merge them and branch links would never be made.
static volatile uint32 g_lastUnlinkedExit = 0;

void NextBlockTrampoline(CMIPS* context)
{
	g_lastUnlinkedExit = LINK_SLOT_NEXT;
}

void BranchBlockTrampoline(CMIPS* context)
{
	g_lastUnlinkedExit = LINK_SLOT_BRANCH;
}
