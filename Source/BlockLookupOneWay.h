#pragma once

#include <algorithm>
#include "Types.h"
#include "BasicBlock.h"

class BlockLookupOneWay
{
public:
	typedef CBasicBlock* BlockType;

	BlockLookupOneWay(BlockType emptyBlock, uint32 maxAddress)
	    : m_emptyBlock(emptyBlock)
	    , m_maxAddress(maxAddress)
	{
		m_tableSize = maxAddress / INSTRUCTION_SIZE;
		m_blockTable = new BlockType[m_tableSize];
	}

	~BlockLookupOneWay()
	{
		delete[] m_blockTable;
		delete[] m_codeTable;
		delete[] m_codePages;
	}

	void EnableCodeTable(void* emptyCode)
	{
		assert(!m_codeTable);
		m_emptyCode = emptyCode;
		m_codeTable = new void*[m_tableSize];
		std::fill(m_codeTable, m_codeTable + m_tableSize, m_emptyCode);
		uint32 numPages = (m_maxAddress + 0xFFFFU) >> 16;
		m_codePages = new void**[numPages];
		for(uint32 i = 0; i < numPages; i++)
		{
			m_codePages[i] = m_codeTable + (i * (0x10000U / INSTRUCTION_SIZE));
		}
	}

	void*** GetCodeTable() const
	{
		return m_codePages;
	}

	void Clear()
	{
		for(unsigned int i = 0; i < m_tableSize; i++)
		{
			m_blockTable[i] = m_emptyBlock;
		}
		if(m_codeTable)
		{
			std::fill(m_codeTable, m_codeTable + m_tableSize, m_emptyCode);
		}
	}

	void AddBlock(BlockType block)
	{
		uint32 address = block->GetBeginAddress();
		assert(m_blockTable[address / INSTRUCTION_SIZE] == m_emptyBlock);
		m_blockTable[address / INSTRUCTION_SIZE] = block;
		if(m_codeTable)
		{
			m_codeTable[address / INSTRUCTION_SIZE] = reinterpret_cast<void*>(block->GetCode());
		}
	}

	void DeleteBlock(BlockType block)
	{
		uint32 address = block->GetBeginAddress();
		assert(m_blockTable[address / INSTRUCTION_SIZE] != m_emptyBlock);
		m_blockTable[address / INSTRUCTION_SIZE] = m_emptyBlock;
		if(m_codeTable)
		{
			m_codeTable[address / INSTRUCTION_SIZE] = m_emptyCode;
		}
	}

	BlockType FindBlockAt(uint32 address) const
	{
		assert((address / INSTRUCTION_SIZE) < m_tableSize);
		return m_blockTable[address / INSTRUCTION_SIZE];
	}

	inline void* FindCodeAt(uint32 address) const
	{
		void* code = m_codeTable[address / INSTRUCTION_SIZE];
		return (code != m_emptyCode) ? code : reinterpret_cast<void*>(m_emptyBlock->GetCode());
	}

private:
	enum
	{
		INSTRUCTION_SIZE = 4,
	};

	BlockType m_emptyBlock = nullptr;
	BlockType* m_blockTable = nullptr;
	uint32 m_maxAddress = 0;
	uint32 m_tableSize = 0;
	void* m_emptyCode = nullptr;
	void** m_codeTable = nullptr;
	void*** m_codePages = nullptr;
};
