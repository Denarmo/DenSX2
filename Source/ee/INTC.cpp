#include "INTC.h"
#include "Log.h"
#include "../states/RegisterStateFile.h"

#define LOG_NAME ("ee_intc")

#define STATE_REGS_XML ("intc/regs.xml")

CINTC::CINTC()
    : m_INTC_STAT(0)
    , m_INTC_MASK(0)
{
}

void CINTC::Reset()
{
	m_INTC_STAT.store(0, std::memory_order_relaxed);
	m_INTC_MASK = 0;
}

bool CINTC::IsInterruptPending() const
{
	return (m_INTC_STAT.load(std::memory_order_acquire) & m_INTC_MASK) != 0;
}

uint32 CINTC::GetRegister(uint32 nAddress)
{
	switch(nAddress)
	{
	case INTC_STAT:
		return m_INTC_STAT.load(std::memory_order_acquire);
		break;
	case INTC_MASK:
		return m_INTC_MASK;
		break;
	default:
		CLog::GetInstance().Warn(LOG_NAME, "Read an unhandled register (0x%08X).\r\n", nAddress);
		break;
	}

	return 0;
}

void CINTC::SetRegister(uint32 nAddress, uint32 nValue)
{
	switch(nAddress)
	{
	case INTC_STAT:
		m_INTC_STAT.fetch_and(~nValue, std::memory_order_acq_rel);
		break;
	case INTC_MASK:
		m_INTC_MASK ^= nValue;
		break;
	default:
		CLog::GetInstance().Warn(LOG_NAME, "Wrote to an unhandled register (0x%08X).\r\n", nAddress);
		break;
	}
}

void CINTC::AssertLine(uint32 nLine)
{
	m_INTC_STAT.fetch_or(1U << nLine, std::memory_order_release);
}

void CINTC::LoadState(Framework::CZipArchiveReader& archive)
{
	CRegisterStateFile registerFile(*archive.BeginReadFile(STATE_REGS_XML));
	m_INTC_STAT.store(registerFile.GetRegister32("INTC_STAT"), std::memory_order_relaxed);
	m_INTC_MASK = registerFile.GetRegister32("INTC_MASK");
}

void CINTC::SaveState(Framework::CZipArchiveWriter& archive)
{
	auto registerFile = std::make_unique<CRegisterStateFile>(STATE_REGS_XML);
	registerFile->SetRegister32("INTC_STAT", m_INTC_STAT.load(std::memory_order_relaxed));
	registerFile->SetRegister32("INTC_MASK", m_INTC_MASK);
	archive.InsertFile(std::move(registerFile));
}
