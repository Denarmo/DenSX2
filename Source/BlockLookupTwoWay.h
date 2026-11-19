#pragma once

#include <cstring>
#include "Types.h"
#include "BasicBlock.h"

class BlockLookupTwoWay
{
public:
	typedef CBasicBlock* BlockType;

	BlockLookupTwoWay(BlockType emptyBlock, uint32 maxAddress)
	    : m_emptyBlock(emptyBlock)
	{
		m_subTableCount = (maxAddress + SUBTABLE_MASK) / SUBTABLE_SIZE;
		assert(m_subTableCount != 0);
		m_blockTable = new BlockType*[m_subTableCount];
		memset(m_blockTable, 0, sizeof(BlockType*) * m_subTableCount);
	}

	~BlockLookupTwoWay()
	{
		if(m_codeTable)
		{
			for(unsigned int i = 0; i < m_subTableCount; i++)
			{
				if(m_codeTable[i] != m_emptyCodeSubTable) delete[] m_codeTable[i];
			}
			delete[] m_codeTable;
			delete[] m_emptyCodeSubTable;
		}
		for(unsigned int i = 0; i < m_subTableCount; i++)
		{
			auto subTable = m_blockTable[i];
			if(subTable)
			{
				delete[] subTable;
			}
		}
		delete[] m_blockTable;
	}

	void Clear()
	{
		for(unsigned int i = 0; i < m_subTableCount; i++)
		{
			auto subTable = m_blockTable[i];
			if(subTable)
			{
				delete[] subTable;
				m_blockTable[i] = nullptr;
			}
			if(m_codeTable && (m_codeTable[i] != m_emptyCodeSubTable))
			{
				delete[] m_codeTable[i];
				m_codeTable[i] = m_emptyCodeSubTable;
			}
		}
	}

	//Keep a table of block code next to the block table, for generated code to look up. Empty entries hold
	//the empty block's code (it compiles the block).
	void EnableCodeTable(void* emptyCode)
	{
		assert(!m_codeTable);
		m_emptyCode = emptyCode;
		const uint32 subTableSize = SUBTABLE_SIZE / INSTRUCTION_SIZE;
		m_emptyCodeSubTable = new void*[subTableSize];
		for(uint32 i = 0; i < subTableSize; i++)
		{
			m_emptyCodeSubTable[i] = emptyCode;
		}
		m_codeTable = new void**[m_subTableCount];
		for(uint32 i = 0; i < m_subTableCount; i++)
		{
			m_codeTable[i] = m_emptyCodeSubTable;
		}
	}

	void*** GetCodeTable() const
	{
		return m_codeTable;
	}

	void AddBlock(BlockType block)
	{
		uint32 address = block->GetBeginAddress();
		uint32 hiAddress = address >> SUBTABLE_BITS;
		uint32 loAddress = address & SUBTABLE_MASK;
		assert(hiAddress < m_subTableCount);
		auto& subTable = m_blockTable[hiAddress];
		if(!subTable)
		{
			const uint32 subTableSize = SUBTABLE_SIZE / INSTRUCTION_SIZE;
			subTable = new BlockType[subTableSize];
			for(uint32 i = 0; i < subTableSize; i++)
			{
				subTable[i] = m_emptyBlock;
			}
		}
		assert(subTable[loAddress / INSTRUCTION_SIZE] == m_emptyBlock);
		subTable[loAddress / INSTRUCTION_SIZE] = block;
		if(m_codeTable)
		{
			auto& codeSubTable = m_codeTable[hiAddress];
			if(codeSubTable == m_emptyCodeSubTable)
			{
				const uint32 subTableSize = SUBTABLE_SIZE / INSTRUCTION_SIZE;
				codeSubTable = new void*[subTableSize];
				for(uint32 i = 0; i < subTableSize; i++)
				{
					codeSubTable[i] = m_emptyCode;
				}
			}
			codeSubTable[loAddress / INSTRUCTION_SIZE] = reinterpret_cast<void*>(block->GetCode());
		}
	}

	void DeleteBlock(BlockType block)
	{
		uint32 address = block->GetBeginAddress();
		uint32 hiAddress = address >> SUBTABLE_BITS;
		uint32 loAddress = address & SUBTABLE_MASK;
		assert(hiAddress < m_subTableCount);
		auto& subTable = m_blockTable[hiAddress];
		assert(subTable);
		assert(subTable[loAddress / INSTRUCTION_SIZE] != m_emptyBlock);
		subTable[loAddress / INSTRUCTION_SIZE] = m_emptyBlock;
		if(m_codeTable)
		{
			assert(m_codeTable[hiAddress] != m_emptyCodeSubTable);
			m_codeTable[hiAddress][loAddress / INSTRUCTION_SIZE] = m_emptyCode;
		}
	}

	BlockType FindBlockAt(uint32 address) const
	{
		uint32 hiAddress = address >> SUBTABLE_BITS;
		uint32 loAddress = address & SUBTABLE_MASK;
		assert(hiAddress < m_subTableCount);
		auto& subTable = m_blockTable[hiAddress];
		if(!subTable) return m_emptyBlock;
		auto result = subTable[loAddress / INSTRUCTION_SIZE];
		return result;
	}

	inline void* FindCodeAt(uint32 address) const
	{
		void* code = m_codeTable[address >> SUBTABLE_BITS][(address & SUBTABLE_MASK) / INSTRUCTION_SIZE];
		return (code != m_emptyCode) ? code : reinterpret_cast<void*>(m_emptyBlock->GetCode());
	}

private:
	enum
	{
		SUBTABLE_BITS = 16,
		SUBTABLE_SIZE = (1 << SUBTABLE_BITS),
		SUBTABLE_MASK = (SUBTABLE_SIZE - 1),
		INSTRUCTION_SIZE = 4,
	};

	BlockType m_emptyBlock = nullptr;
	BlockType** m_blockTable = nullptr;
	uint32 m_subTableCount = 0;
	void*** m_codeTable = nullptr;
	void** m_emptyCodeSubTable = nullptr;
	void* m_emptyCode = nullptr;
};
