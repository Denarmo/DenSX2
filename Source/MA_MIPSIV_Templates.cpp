#include "MA_MIPSIV.h"
#include "Jitter.h"
#include "MIPS.h"
#include "offsetof_def.h"

void CMA_MIPSIV::Template_Add32(bool isSigned)
{
	if(m_nRD == 0) return;

	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));

	m_codeGen->Add();

	if(m_regSize == MIPS_REGSIZE_64)
	{
		m_codeGen->PushTop();
		m_codeGen->SignExt();
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[1]));
	}

	m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));
}

void CMA_MIPSIV::Template_Add64(bool isSigned)
{
	if(!Ensure64BitRegs()) return;
	if(m_nRD == 0) return;

	//PS2 compilers emit DADDU rd, rs, $zero (or $zero, rt) for register-to-register moves.
	//Emit 32-bit half moves so rs/rt/rd stay unaliased for register allocation and copy propagation.
	if((m_nRT == 0) || (m_nRS == 0))
	{
		uint8 src = (m_nRT == 0) ? m_nRS : m_nRT;
		if(m_nRD == src) return;
		for(unsigned int i = 0; i < 2; i++)
		{
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[src].nV[i]));
			m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[i]));
		}
		return;
	}

	m_codeGen->PushRel64(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
	m_codeGen->PushRel64(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
	m_codeGen->Add64();
	m_codeGen->PullRel64(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));
}

void CMA_MIPSIV::Template_Sub32(bool isSigned)
{
	if(m_nRD == 0) return;

	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));

	m_codeGen->Sub();

	if(m_regSize == MIPS_REGSIZE_64)
	{
		m_codeGen->PushTop();
		m_codeGen->SignExt();
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[1]));
	}

	m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));
}

void CMA_MIPSIV::Template_Sub64(bool isSigned)
{
	if(!Ensure64BitRegs()) return;
	if(m_nRD == 0) return;

	if(m_nRT == 0)
	{
		if(m_nRD == m_nRS) return;
		for(unsigned int i = 0; i < 2; i++)
		{
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[i]));
			m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[i]));
		}
		return;
	}

	m_codeGen->PushRel64(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
	m_codeGen->PushRel64(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
	m_codeGen->Sub64();
	m_codeGen->PullRel64(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));
}

void CMA_MIPSIV::Template_Load32Idx(const MemoryAccessIdxTraits& traits)
{
	CheckTLBExceptions(false);

	if(m_nRT == 0) return;

	const auto finishLoad =
	    [&]() {
		    if(traits.signExtFunction)
		    {
			    //SignExt to 32-bits (if needed)
			    ((m_codeGen)->*(traits.signExtFunction))();
		    }

		    //Sign extend to whole 64-bits
		    if(m_regSize == MIPS_REGSIZE_64)
		    {
			    if(traits.upper64BitSignExtend)
			    {
				    m_codeGen->PushTop();
				    m_codeGen->SignExt();
			    }
			    else
			    {
				    m_codeGen->PushCst(0);
			    }
			    m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[1]));
		    }
		    m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
	    };

	bool usePageLookup = (m_pCtx->m_pageLookup != nullptr);

	//One statement with direct 32 MiB RAM (or page lookup) inline and proxy out of line, so the block isn't split
	if(usePageLookup && m_codeGen->SupportsPageAccess())
	{
		bool useDirectRam = (m_pCtx->m_ramBase != nullptr);
		if(useDirectRam && m_pCtx->m_fixedFrameExit)
		{
			m_codeGen->PushCstPtr(0);
		}
		else
		{
			m_codeGen->PushRelRef(useDirectRam ? offsetof(CMIPS, m_ramBase) : offsetof(CMIPS, m_pageLookup));
		}
		ComputeMemAccessAddrNoXlat();
		m_codeGen->LoadFromPage(traits.elementSize, traits.getProxyFunction, useDirectRam ? static_cast<uint32>(offsetof(CMIPS, m_pageLookup)) : 0);
		finishLoad();
		return;
	}

	if(usePageLookup)
	{
		ComputeMemAccessPageRef();

		m_codeGen->PushCst(0);
		m_codeGen->BeginIf(Jitter::CONDITION_NE);
		{
			ComputeMemAccessRefIdx(traits.elementSize);
			((m_codeGen)->*(traits.loadFunction))(1);
			finishLoad();
		}
		m_codeGen->Else();
	}

	//Standard memory access
	{
		ComputeMemAccessAddrNoXlat();

		m_codeGen->PushCtx();
		m_codeGen->PushIdx(1);
		m_codeGen->Call(traits.getProxyFunction, 2, Jitter::CJitter::RETURN_VALUE_32);

		finishLoad();

		m_codeGen->PullTop();
	}

	if(usePageLookup)
	{
		m_codeGen->EndIf();
	}
}

void CMA_MIPSIV::Template_Store32Idx(const MemoryAccessIdxTraits& traits)
{
	CheckTLBExceptions(true);

	bool usePageLookup = (m_pCtx->m_pageLookup != nullptr);

	if(usePageLookup && m_codeGen->SupportsPageAccess())
	{
		bool useDirectRam = (m_pCtx->m_ramBase != nullptr);
		if(useDirectRam && m_pCtx->m_fixedFrameExit)
		{
			m_codeGen->PushCstPtr(0);
		}
		else
		{
			m_codeGen->PushRelRef(useDirectRam ? offsetof(CMIPS, m_ramBase) : offsetof(CMIPS, m_pageLookup));
		}
		ComputeMemAccessAddrNoXlat();
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
		m_codeGen->StoreAtPage(traits.elementSize, traits.setProxyFunction, useDirectRam ? static_cast<uint32>(offsetof(CMIPS, m_pageLookup)) : 0);
		return;
	}

	if(usePageLookup)
	{
		ComputeMemAccessPageRef();

		m_codeGen->PushCst(0);
		m_codeGen->BeginIf(Jitter::CONDITION_NE);
		{
			ComputeMemAccessRefIdx(traits.elementSize);

			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
			((m_codeGen)->*(traits.storeFunction))(1);
		}
		m_codeGen->Else();
	}

	//Standard memory access
	{
		ComputeMemAccessAddrNoXlat();

		m_codeGen->PushCtx();
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
		m_codeGen->PushIdx(2);
		m_codeGen->Call(traits.setProxyFunction, 3, Jitter::CJitter::RETURN_VALUE_NONE);

		m_codeGen->PullTop();
	}

	if(usePageLookup)
	{
		m_codeGen->EndIf();
	}
}

void CMA_MIPSIV::Template_ShiftCst32(const TemplateParamedOperationFunctionType& Function)
{
	if(m_nRD == 0) return;

	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
	Function(m_nSA);

	if(m_regSize == MIPS_REGSIZE_64)
	{
		m_codeGen->PushTop();
		m_codeGen->SignExt();
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[1]));
	}

	m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));
}

void CMA_MIPSIV::Template_ShiftVar32(const TemplateOperationFunctionType& function)
{
	if(m_nRD == 0) return;

	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
	function();

	if(m_regSize == MIPS_REGSIZE_64)
	{
		m_codeGen->PushTop();
		m_codeGen->SignExt();
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[1]));
	}

	m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));
}

void CMA_MIPSIV::Template_Mult32(bool isSigned, unsigned int unit)
{
	auto function = isSigned ? &CMipsJitter::MultS : &CMipsJitter::Mult;

	size_t lo[2];
	size_t hi[2];

	switch(unit)
	{
	case 0:
		lo[0] = offsetof(CMIPS, m_State.nLO[0]);
		lo[1] = offsetof(CMIPS, m_State.nLO[1]);
		hi[0] = offsetof(CMIPS, m_State.nHI[0]);
		hi[1] = offsetof(CMIPS, m_State.nHI[1]);
		break;
	case 1:
		lo[0] = offsetof(CMIPS, m_State.nLO1[0]);
		lo[1] = offsetof(CMIPS, m_State.nLO1[1]);
		hi[0] = offsetof(CMIPS, m_State.nHI1[0]);
		hi[1] = offsetof(CMIPS, m_State.nHI1[1]);
		break;
	default:
		throw std::runtime_error("Invalid unit number.");
		break;
	}

	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
	((m_codeGen)->*(function))();

	m_codeGen->PushTop();

	m_codeGen->ExtLow64();
	if(m_regSize == MIPS_REGSIZE_64)
	{
		m_codeGen->PushTop();
		m_codeGen->SignExt();
		m_codeGen->PullRel(lo[1]);
	}
	m_codeGen->PullRel(lo[0]);

	m_codeGen->ExtHigh64();
	if(m_regSize == MIPS_REGSIZE_64)
	{
		m_codeGen->PushTop();
		m_codeGen->SignExt();
		m_codeGen->PullRel(hi[1]);
	}
	m_codeGen->PullRel(hi[0]);

	if(m_nRD != 0)
	{
		//Wierd EE MIPS spinoff...
		m_codeGen->PushRel(lo[0]);
		m_codeGen->PushRel(lo[1]);

		m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[1]));
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));
	}
}

void CMA_MIPSIV::Template_Div32(bool isSigned, unsigned int unit, unsigned int regOffset)
{
	auto function = isSigned ? &CMipsJitter::DivS : &CMipsJitter::Div;

	size_t lo[2];
	size_t hi[2];

	switch(unit)
	{
	case 0:
		lo[0] = offsetof(CMIPS, m_State.nLO[0]);
		lo[1] = offsetof(CMIPS, m_State.nLO[1]);
		hi[0] = offsetof(CMIPS, m_State.nHI[0]);
		hi[1] = offsetof(CMIPS, m_State.nHI[1]);
		break;
	case 1:
		lo[0] = offsetof(CMIPS, m_State.nLO1[0]);
		lo[1] = offsetof(CMIPS, m_State.nLO1[1]);
		hi[0] = offsetof(CMIPS, m_State.nHI1[0]);
		hi[1] = offsetof(CMIPS, m_State.nHI1[1]);
		break;
	default:
		throw std::runtime_error("Invalid unit number.");
		break;
	}

	//Check for zero
	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[regOffset]));
	m_codeGen->PushCst(0);
	m_codeGen->BeginIf(Jitter::CONDITION_EQ);
	{
		if(isSigned)
		{
			//If r[rs] < 0, then lo = 1 else lo = ~0
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[regOffset]));
			m_codeGen->PushCst(0);
			m_codeGen->BeginIf(Jitter::CONDITION_LT);
			{
				m_codeGen->PushCst(1);
				m_codeGen->PullRel(lo[0]);
			}
			m_codeGen->Else();
			{
				m_codeGen->PushCst(~0);
				m_codeGen->PullRel(lo[0]);
			}
			m_codeGen->EndIf();
		}
		else
		{
			m_codeGen->PushCst(~0);
			m_codeGen->PullRel(lo[0]);
		}

		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[regOffset]));
		m_codeGen->PullRel(hi[0]);
	}
	m_codeGen->Else();
	{
		if(isSigned)
		{
			//Check for overflow condition (0x80000000 / 0xFFFFFFFF)
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[regOffset]));
			m_codeGen->PushCst(0x80000000);
			m_codeGen->Cmp(Jitter::CONDITION_EQ);

			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[regOffset]));
			m_codeGen->PushCst(0xFFFFFFFF);
			m_codeGen->Cmp(Jitter::CONDITION_EQ);

			m_codeGen->And();
		}
		else
		{
			m_codeGen->PushCst(0);
		}

		m_codeGen->PushCst(0);

		m_codeGen->BeginIf(Jitter::CONDITION_NE);
		{
			//Overflow
			m_codeGen->PushCst(0x80000000);
			m_codeGen->PullRel(lo[0]);

			m_codeGen->PushCst(0);
			m_codeGen->PullRel(hi[0]);
		}
		m_codeGen->Else();
		{
			//No overflow, carry on
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[regOffset]));
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[regOffset]));
			((m_codeGen)->*(function))();

			m_codeGen->PushTop();

			m_codeGen->ExtLow64();
			m_codeGen->PullRel(lo[0]);

			m_codeGen->ExtHigh64();
			m_codeGen->PullRel(hi[0]);
		}
		m_codeGen->EndIf();
	}
	m_codeGen->EndIf();

	if(m_regSize == MIPS_REGSIZE_64)
	{
		m_codeGen->PushRel(hi[0]);
		m_codeGen->SignExt();
		m_codeGen->PullRel(hi[1]);

		m_codeGen->PushRel(lo[0]);
		m_codeGen->SignExt();
		m_codeGen->PullRel(lo[1]);
	}
}

void CMA_MIPSIV::Template_MovEqual(bool isEqual)
{
	if(m_nRD == 0) return;

	if(m_nRT == 0)
	{
		if(isEqual && (m_nRS != m_nRD))
		{
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
			m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));
			if(m_regSize == MIPS_REGSIZE_64)
			{
				m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
				m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[1]));
			}
		}
		return;
	}

	if(m_codeGen->GetCodeGen()->SupportsCmpSelect())
	{
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
		if(m_regSize == MIPS_REGSIZE_64)
		{
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[1]));
			m_codeGen->Or();
		}
		m_codeGen->PushCst(0);
		m_codeGen->Cmp(isEqual ? Jitter::CONDITION_EQ : Jitter::CONDITION_NE);
		if(m_regSize == MIPS_REGSIZE_64)
		{
			//Duplicate cond before overwriting nV[0] (handles m_nRD == m_nRT and avoids recomputing cond)
			m_codeGen->PushTop();
		}

		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));
		m_codeGen->Select();
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));

		if(m_regSize == MIPS_REGSIZE_64)
		{
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[1]));
			m_codeGen->Select();
			m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[1]));
		}
		return;
	}

	m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
	if(m_regSize == MIPS_REGSIZE_64)
	{
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[1]));
		m_codeGen->Or();
	}

	m_codeGen->PushCst(0);
	m_codeGen->BeginIf(isEqual ? Jitter::CONDITION_EQ : Jitter::CONDITION_NE);
	{
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));

		if(m_regSize == MIPS_REGSIZE_64)
		{
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
			m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[1]));
		}
	}
	m_codeGen->EndIf();
}

void CMA_MIPSIV::Template_SetLessThanImm(bool isSigned)
{
	if(m_nRT == 0) return;

	Jitter::CONDITION condition = isSigned ? Jitter::CONDITION_LT : Jitter::CONDITION_BL;

	if(m_regSize == MIPS_REGSIZE_32)
	{
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
		m_codeGen->PushCst(static_cast<int16>(m_nImmediate));

		m_codeGen->Cmp(condition);
	}
	else
	{
		int32 imm32 = static_cast<int32>(static_cast<int16>(m_nImmediate));
		uint32 immLo = static_cast<uint32>(imm32);
		uint32 immHi = (imm32 < 0) ? 0xFFFFFFFFU : 0U;

		if(m_nRS == 0)
		{
			uint32 res = isSigned ? (0 < static_cast<int64>(imm32) ? 1U : 0U)
			                      : (0ULL < static_cast<uint64>(static_cast<int64>(imm32)) ? 1U : 0U);
			m_codeGen->PushCst(res);
		}
		else if(!isSigned && (immHi == 0))
		{
			//Unsigned compare against [0, 32767]: (rs.hi == 0) & (rs.lo <u immLo)
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
			m_codeGen->PushCst(0);
			m_codeGen->Cmp(Jitter::CONDITION_EQ);

			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
			m_codeGen->PushCst(immLo);
			m_codeGen->Cmp(Jitter::CONDITION_BL);

			m_codeGen->And();
		}
		else
		{
			//Exact 64-bit compare using 32-bit halves so rs stays unaliased for register allocation:
			//(rs.hi < immHi) | ((rs.hi == immHi) & (rs.lo <u immLo))
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
			m_codeGen->PushCst(immHi);
			m_codeGen->Cmp(condition);

			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
			m_codeGen->PushCst(immHi);
			m_codeGen->Cmp(Jitter::CONDITION_EQ);

			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
			m_codeGen->PushCst(immLo);
			m_codeGen->Cmp(Jitter::CONDITION_BL);

			m_codeGen->And();
			m_codeGen->Or();
		}
	}

	m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));

	if(m_regSize == MIPS_REGSIZE_64)
	{
		//Clear higher 32-bits
		m_codeGen->PushCst(0);
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[1]));
	}
}

void CMA_MIPSIV::Template_SetLessThanReg(bool isSigned)
{
	if(m_nRD == 0) return;

	Jitter::CONDITION condition = isSigned ? Jitter::CONDITION_LT : Jitter::CONDITION_BL;

	if(m_regSize == MIPS_REGSIZE_32)
	{
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
		m_codeGen->Cmp(condition);
	}
	else
	{
		if(m_nRS == m_nRT)
		{
			m_codeGen->PushCst(0);
		}
		else if(!isSigned && (m_nRS == 0))
		{
			//SLTU rd, $zero, rt => (rt != 0)
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[1]));
			m_codeGen->Or();
			m_codeGen->PushCst(0);
			m_codeGen->Cmp(Jitter::CONDITION_NE);
		}
		else if(!isSigned && (m_nRT == 0))
		{
			//SLTU rd, rs, $zero => always 0
			m_codeGen->PushCst(0);
		}
		else if(isSigned && (m_nRT == 0))
		{
			//SLT rd, rs, $zero => (rs.hi <s 0)
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
			m_codeGen->PushCst(0);
			m_codeGen->Cmp(Jitter::CONDITION_LT);
		}
		else
		{
			//Exact 64-bit compare using 32-bit halves so rs/rt stay unaliased for register allocation:
			//(rs.hi < rt.hi) | ((rs.hi == rt.hi) & (rs.lo <u rt.lo))
			auto pushHalf = [&](uint8 reg, uint32 half) {
				if(reg == 0)
					m_codeGen->PushCst(0);
				else
					m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[reg].nV[half]));
			};

			pushHalf(m_nRS, 1);
			pushHalf(m_nRT, 1);
			m_codeGen->Cmp(condition);

			pushHalf(m_nRS, 1);
			pushHalf(m_nRT, 1);
			m_codeGen->Cmp(Jitter::CONDITION_EQ);

			pushHalf(m_nRS, 0);
			pushHalf(m_nRT, 0);
			m_codeGen->Cmp(Jitter::CONDITION_BL);

			m_codeGen->And();
			m_codeGen->Or();
		}
	}

	m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[0]));

	if(m_regSize == MIPS_REGSIZE_64)
	{
		//Clear higher 32-bits
		m_codeGen->PushCst(0);
		m_codeGen->PullRel(offsetof(CMIPS, m_State.nGPR[m_nRD].nV[1]));
	}
}

void CMA_MIPSIV::Template_BranchEq(bool condition, bool likely)
{
	if(!likely && (m_nRS == m_nRT))
	{
		if(condition)
		{
			uint16 nImmediate = static_cast<uint16>(m_nOpcode & 0xFFFF);
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nPC));
			m_codeGen->PushCst((m_instrPosition + 4) + CMIPS::GetBranch(nImmediate));
			m_codeGen->Add();
			m_codeGen->PullRel(offsetof(CMIPS, m_State.nDelayedJumpAddr));
		}
		return;
	}

	if(m_regSize == MIPS_REGSIZE_32)
	{
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
	}
	else if(m_regSize == MIPS_REGSIZE_64)
	{
		if(m_nRS == m_nRT)
		{
			m_codeGen->PushCst(0);
			m_codeGen->PushCst(0);
		}
		else if((m_nRS == 0) || (m_nRT == 0))
		{
			uint8 reg = (m_nRS == 0) ? m_nRT : m_nRS;
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[reg].nV[0]));
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[reg].nV[1]));
			m_codeGen->Or();
			m_codeGen->PushCst(0);
		}
		else
		{
			//Exact 64-bit equality without SYM_RELATIVE64 aliasing: ((rs.lo ^ rt.lo) | (rs.hi ^ rt.hi)) == 0
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[0]));
			m_codeGen->Xor();
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRT].nV[1]));
			m_codeGen->Xor();
			m_codeGen->Or();
			m_codeGen->PushCst(0);
		}
	}

	auto branchCondition = condition ? Jitter::CONDITION_EQ : Jitter::CONDITION_NE;

	if(likely)
	{
		BranchLikely(branchCondition);
	}
	else
	{
		Branch(branchCondition);
	}
}

void CMA_MIPSIV::Template_BranchGez(bool condition, bool likely)
{
	if(m_regSize == MIPS_REGSIZE_32)
	{
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
	}
	else
	{
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
	}

	m_codeGen->PushCst(0);

	Jitter::CONDITION branchCondition = condition ? Jitter::CONDITION_GE : Jitter::CONDITION_LT;

	if(likely)
	{
		BranchLikely(branchCondition);
	}
	else
	{
		Branch(branchCondition);
	}
}

void CMA_MIPSIV::Template_BranchLez(bool condition, bool likely)
{
	Jitter::CONDITION branchCondition;

	if(m_regSize == MIPS_REGSIZE_32)
	{
		m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
		m_codeGen->PushCst(0);

		branchCondition = condition ? Jitter::CONDITION_LE : Jitter::CONDITION_GT;
	}
	else
	{
		if(m_nRS == 0)
		{
			m_codeGen->PushCst(1);
			m_codeGen->PushCst(0);
			branchCondition = condition ? Jitter::CONDITION_NE : Jitter::CONDITION_EQ;
		}
		else
		{
			//Signed 64-bit (rs <= 0) without SYM_RELATIVE64 aliasing: (rs.hi <s 0) | ((rs.lo | rs.hi) == 0)
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
			m_codeGen->PushCst(0);
			m_codeGen->Cmp(Jitter::CONDITION_LT);

			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[0]));
			m_codeGen->PushRel(offsetof(CMIPS, m_State.nGPR[m_nRS].nV[1]));
			m_codeGen->Or();
			m_codeGen->PushCst(0);
			m_codeGen->Cmp(Jitter::CONDITION_EQ);

			m_codeGen->Or();
			m_codeGen->PushCst(0);
			branchCondition = condition ? Jitter::CONDITION_NE : Jitter::CONDITION_EQ;
		}
	}

	if(likely)
	{
		BranchLikely(branchCondition);
	}
	else
	{
		Branch(branchCondition);
	}
}
