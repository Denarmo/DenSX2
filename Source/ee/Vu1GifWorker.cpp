#include "Vu1GifWorker.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fenv.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "AlignedAlloc.h"
#include "FpUtils.h"
#include "ThreadUtils.h"
#include "../ThreadAffinity.h"
#include "Vpu.h"
#include "Vif.h"
#include "GIF.h"

using namespace Ee;

namespace
{
	static inline void CopyToRing(uint8* __restrict dst, const uint8* __restrict src, uint32 size)
	{
		if((size <= 256) && ((size & 15U) == 0))
		{
#if defined(__aarch64__)
			uint32 i = 0;
			for(; i + 64 <= size; i += 64)
			{
				uint8x16_t v0 = vld1q_u8(src + i);
				uint8x16_t v1 = vld1q_u8(src + i + 16);
				uint8x16_t v2 = vld1q_u8(src + i + 32);
				uint8x16_t v3 = vld1q_u8(src + i + 48);
				vst1q_u8(dst + i, v0);
				vst1q_u8(dst + i + 16, v1);
				vst1q_u8(dst + i + 32, v2);
				vst1q_u8(dst + i + 48, v3);
			}
			for(; i < size; i += 16)
			{
				vst1q_u8(dst + i, vld1q_u8(src + i));
			}
#else
			auto* d = reinterpret_cast<uint64*>(dst);
			const auto* s = reinterpret_cast<const uint64*>(src);
			for(uint32 i = 0; i < (size >> 3); i += 2)
			{
				d[i] = s[i];
				d[i + 1] = s[i + 1];
			}
#endif
		}
		else
		{
			memcpy(dst, src, size);
		}
	}
}

bool CVu1GifWorker::m_timingStatsEnabled = false;

CVu1GifWorker::CVu1GifWorker(std::shared_ptr<CVpu>& vpu1, CGIF& gif)
    : m_vpu1(vpu1)
    , m_gif(gif)
{
	m_ring = reinterpret_cast<uint8*>(framework_aligned_alloc(RING_SIZE, 64));
	m_deferredPath3Cap = 6 * 1024 * 1024;
	m_deferredPath3Buf = reinterpret_cast<uint8*>(framework_aligned_alloc(m_deferredPath3Cap, 64));
	m_stalledVif1BufCap = 2 * 1024 * 1024;
	m_stalledVif1Buf = reinterpret_cast<uint8*>(framework_aligned_alloc(m_stalledVif1BufCap, 64));
	m_stalledVif1Meta.reserve(2048);
}

CVu1GifWorker::~CVu1GifWorker()
{
	Stop();
	if(m_ring)
	{
		framework_aligned_free(m_ring);
		m_ring = nullptr;
	}
	if(m_deferredPath3Buf)
	{
		framework_aligned_free(m_deferredPath3Buf);
		m_deferredPath3Buf = nullptr;
	}
	if(m_stalledVif1Buf)
	{
		framework_aligned_free(m_stalledVif1Buf);
		m_stalledVif1Buf = nullptr;
	}
}

void CVu1GifWorker::Start()
{
	if(m_started) return;
	m_stop.store(false, std::memory_order_relaxed);
	m_writePos.store(0, std::memory_order_relaxed);
	m_cachedReadPos = 0;
	m_pendingWritePos = 0;
	m_lastCoalescePos = ~0ULL;
	m_batchDepth = 0;
	m_readPos.store(0, std::memory_order_relaxed);
	m_workerSleeping.store(false, std::memory_order_relaxed);
	m_producerWaitingSpace.store(false, std::memory_order_relaxed);
	m_syncRequested.store(false, std::memory_order_relaxed);
	m_gif.SetPath3UnmaskedCallback([this]() { OnPath3Unmasked(); });
	m_started = true;
	m_thread = std::thread([this]() { ThreadProc(); });
	Framework::ThreadUtils::SetThreadName(m_thread, "VU1/GIF Thread");
}

void CVu1GifWorker::Stop()
{
	if(!m_started) return;
	Sync(SYNC_OTHER);
	{
		std::lock_guard lk(m_mutex);
		m_stop.store(true, std::memory_order_release);
		m_condWork.notify_all();
	}
	if(m_thread.joinable())
	{
		m_thread.join();
	}
	m_gif.SetPath3UnmaskedCallback({});
	m_started = false;
}

void CVu1GifWorker::BeginDmaBatch()
{
	if(!m_started) return;
	if(m_batchDepth == 0)
	{
		m_pendingWritePos = m_writePos.load(std::memory_order_relaxed);
		m_lastCoalescePos = ~0ULL;
	}
	m_batchDepth++;
}

void CVu1GifWorker::EndDmaBatch()
{
	if(!m_started || m_batchDepth == 0) return;
	m_batchDepth--;
	if(m_batchDepth == 0)
	{
		m_lastCoalescePos = ~0ULL;
		uint64 published = m_writePos.load(std::memory_order_relaxed);
		if(m_pendingWritePos != published)
		{
			m_writePos.store(m_pendingWritePos, std::memory_order_seq_cst);
			if(m_workerSleeping.load(std::memory_order_seq_cst))
			{
				std::lock_guard lk(m_mutex);
				m_condWork.notify_one();
			}
		}
	}
}

bool CVu1GifWorker::EnqueueRaw(CMD_TYPE type, const uint8* data, uint32 size, uint32 arg0, uint32 arg1)
{
	uint32 alignedSize = (size + 15U) & ~15U;
	uint32 need = sizeof(PACKET_HEADER) + alignedSize;
	if(need > (RING_SIZE / 2))
	{
		Sync(SYNC_OTHER);
		return false;
	}

	uint64 w = (m_batchDepth > 0) ? m_pendingWritePos : m_writePos.load(std::memory_order_relaxed);

	//Coalesce consecutive unpublished VIF1/GIF DMA chunks (arg0 == 0, 16-byte aligned) into the previous matching ring packet
	//(including appending VIF1 payload directly onto a preceding TTE=1 16-byte tag header packet).
	if(m_batchDepth > 0 && (type == CMD_VIF1_DMA || type == CMD_GIF_DMA) && arg0 == 0 && size != 0 && ((size & 15U) == 0) && m_lastCoalescePos != ~0ULL)
	{
		uint32 off = static_cast<uint32>(w & RING_MASK);
		uint64 nextW = w + size;
		bool sameEpoch = (((nextW - 1ULL) & ~static_cast<uint64>(RING_MASK)) == (m_lastCoalescePos & ~static_cast<uint64>(RING_MASK)));
		if(sameEpoch && (nextW - m_cachedReadPos) <= RING_SIZE)
		{
			auto* prevHdr = reinterpret_cast<PACKET_HEADER*>(m_ring + static_cast<uint32>(m_lastCoalescePos & RING_MASK));
			if(prevHdr->type == type && (prevHdr->size + size) <= (RING_SIZE / 4))
			{
				CopyToRing(m_ring + off, data, size);
				prevHdr->size += size;
				m_pendingWritePos = nextW;
				if((nextW & RING_MASK) == 0)
				{
					m_lastCoalescePos = ~0ULL;
				}
				if((nextW - m_writePos.load(std::memory_order_relaxed)) >= 65536)
				{
					m_lastCoalescePos = ~0ULL;
					m_writePos.store(nextW, std::memory_order_seq_cst);
					if(m_workerSleeping.load(std::memory_order_seq_cst))
					{
						std::lock_guard lk(m_mutex);
						m_condWork.notify_one();
					}
				}
				return true;
			}
		}
	}

	uint32 off = static_cast<uint32>(w & RING_MASK);
	uint32 tail = RING_SIZE - off;
	bool needWrap = (tail < need);
	uint64 totalNeed = needWrap ? (static_cast<uint64>(tail) + need) : need;

	if((w + totalNeed) - m_cachedReadPos > RING_SIZE)
	{
		m_cachedReadPos = m_readPos.load(std::memory_order_acquire);
		if((w + totalNeed) - m_cachedReadPos > RING_SIZE)
		{
			//Publish any batched writes so the consumer can drain and free space.
			if(m_batchDepth > 0 && w != m_writePos.load(std::memory_order_relaxed))
			{
				m_lastCoalescePos = ~0ULL;
				m_writePos.store(w, std::memory_order_release);
			}
			while((w + totalNeed) - m_cachedReadPos > RING_SIZE)
			{
				std::unique_lock lk(m_mutex);
				m_producerWaitingSpace.store(true, std::memory_order_seq_cst);
				if(m_workerSleeping.load(std::memory_order_acquire))
				{
					m_condWork.notify_one();
				}
				m_cachedReadPos = m_readPos.load(std::memory_order_seq_cst);
				if((w + totalNeed) - m_cachedReadPos > RING_SIZE)
				{
					m_condDone.wait(lk);
					m_cachedReadPos = m_readPos.load(std::memory_order_acquire);
				}
				m_producerWaitingSpace.store(false, std::memory_order_relaxed);
			}
		}
	}

	if(needWrap)
	{
		auto* wrapHdr = reinterpret_cast<PACKET_HEADER*>(m_ring + off);
		wrapHdr->type = CMD_WRAP;
		wrapHdr->size = tail - sizeof(PACKET_HEADER);
		wrapHdr->arg0 = 0;
		wrapHdr->arg1 = 0;
		w += tail;
		off = 0;
	}

	auto* hdr = reinterpret_cast<PACKET_HEADER*>(m_ring + off);
	hdr->type = type;
	hdr->size = size;
	hdr->arg0 = arg0;
	hdr->arg1 = arg1;
	if(size != 0)
	{
		CopyToRing(m_ring + off + sizeof(PACKET_HEADER), data, size);
	}

	uint64 nextW = w + need;
	if(m_batchDepth > 0)
	{
		m_pendingWritePos = nextW;
		if((nextW - m_writePos.load(std::memory_order_relaxed)) >= 65536)
		{
			m_lastCoalescePos = ~0ULL;
			m_writePos.store(nextW, std::memory_order_seq_cst);
			if(m_workerSleeping.load(std::memory_order_seq_cst))
			{
				std::lock_guard lk(m_mutex);
				m_condWork.notify_one();
			}
		}
		else if(((type == CMD_VIF1_DMA && (arg0 == 0 || (arg0 == 1 && size == 16))) ||
		         (type == CMD_GIF_DMA && arg0 == 0)) &&
		        ((size & 15U) == 0) && ((nextW & RING_MASK) != 0))
		{
			m_lastCoalescePos = w;
		}
		else
		{
			m_lastCoalescePos = ~0ULL;
		}
	}
	else
	{
		m_lastCoalescePos = ~0ULL;
		m_writePos.store(nextW, std::memory_order_seq_cst);
		if(m_workerSleeping.load(std::memory_order_seq_cst))
		{
			std::lock_guard lk(m_mutex);
			m_condWork.notify_one();
		}
	}
	return true;
}

bool CVu1GifWorker::EnqueueVif1Dma(const uint8* data, uint32 size, bool tagIncluded)
{
	if(!m_started) return false;
	if(m_timingStatsEnabled) m_vif1Packets++;
	if(m_batchDepth > 0 && tagIncluded && size == 16)
	{
		uint64 w = m_pendingWritePos;
		uint32 off = static_cast<uint32>(w & RING_MASK);
		uint64 nextW = w + 32;
		if((off <= (RING_SIZE - 32)) && ((nextW - m_cachedReadPos) <= RING_SIZE))
		{
			uint8* dst = m_ring + off;
			auto* hdr = reinterpret_cast<PACKET_HEADER*>(dst);
			hdr->type = CMD_VIF1_DMA;
			hdr->size = 16;
			hdr->arg0 = 1U;
			hdr->arg1 = 0;
#if defined(__aarch64__)
			vst1q_u8(dst + sizeof(PACKET_HEADER), vld1q_u8(data));
#else
			memcpy(dst + sizeof(PACKET_HEADER), data, 16);
#endif
			m_pendingWritePos = nextW;
			if((nextW - m_writePos.load(std::memory_order_relaxed)) >= 65536)
			{
				m_lastCoalescePos = ~0ULL;
				m_writePos.store(nextW, std::memory_order_seq_cst);
				if(m_workerSleeping.load(std::memory_order_seq_cst))
				{
					std::lock_guard lk(m_mutex);
					m_condWork.notify_one();
				}
			}
			else
			{
				m_lastCoalescePos = ((nextW & RING_MASK) != 0) ? w : ~0ULL;
			}
			return true;
		}
	}
	return EnqueueRaw(CMD_VIF1_DMA, data, size, tagIncluded ? 1U : 0U, 0);
}

bool CVu1GifWorker::EnqueueVif1DmaPtr(uint32 ramOffset, uint32 size)
{
	if(!m_started || !m_eeRam) return false;
	if(m_timingStatsEnabled)
	{
		m_vif1Packets++;
		m_zcVif1Bytes += size;
	}
	if(m_batchDepth > 0)
	{
		uint64 w = m_pendingWritePos;
		uint32 off = static_cast<uint32>(w & RING_MASK);
		uint64 nextW = w + sizeof(PACKET_HEADER);
		if((off <= (RING_SIZE - sizeof(PACKET_HEADER))) && ((nextW - m_cachedReadPos) <= RING_SIZE))
		{
			auto* hdr = reinterpret_cast<PACKET_HEADER*>(m_ring + off);
			hdr->type = CMD_VIF1_DMA_PTR;
			hdr->size = 0;
			hdr->arg0 = ramOffset;
			hdr->arg1 = size;
			m_pendingWritePos = nextW;
			m_lastCoalescePos = ~0ULL;
			if((nextW - m_writePos.load(std::memory_order_relaxed)) >= 4096)
			{
				m_writePos.store(nextW, std::memory_order_seq_cst);
				if(m_workerSleeping.load(std::memory_order_seq_cst))
				{
					std::lock_guard lk(m_mutex);
					m_condWork.notify_one();
				}
			}
			return true;
		}
	}
	return EnqueueRaw(CMD_VIF1_DMA_PTR, nullptr, 0, ramOffset, size);
}

bool CVu1GifWorker::EnqueueGifDma(const uint8* data, uint32 size)
{
	if(!m_started) return false;
	if(m_timingStatsEnabled) m_gifPackets++;
	return EnqueueRaw(CMD_GIF_DMA, data, size, 0, 0);
}

bool CVu1GifWorker::EnqueueGifDmaPtr(uint32 ramOffset, uint32 size)
{
	if(!m_started || !m_eeRam) return false;
	if(m_timingStatsEnabled)
	{
		m_gifPackets++;
		m_zcGifBytes += size;
	}
	if(m_batchDepth > 0)
	{
		uint64 w = m_pendingWritePos;
		uint32 off = static_cast<uint32>(w & RING_MASK);
		uint64 nextW = w + sizeof(PACKET_HEADER);
		if((off <= (RING_SIZE - sizeof(PACKET_HEADER))) && ((nextW - m_cachedReadPos) <= RING_SIZE))
		{
			auto* hdr = reinterpret_cast<PACKET_HEADER*>(m_ring + off);
			hdr->type = CMD_GIF_DMA_PTR;
			hdr->size = 0;
			hdr->arg0 = ramOffset;
			hdr->arg1 = size;
			m_pendingWritePos = nextW;
			m_lastCoalescePos = ~0ULL;
			if((nextW - m_writePos.load(std::memory_order_relaxed)) >= 4096)
			{
				m_writePos.store(nextW, std::memory_order_seq_cst);
				if(m_workerSleeping.load(std::memory_order_seq_cst))
				{
					std::lock_guard lk(m_mutex);
					m_condWork.notify_one();
				}
			}
			return true;
		}
	}
	return EnqueueRaw(CMD_GIF_DMA_PTR, nullptr, 0, ramOffset, size);
}

void CVu1GifWorker::EnqueueVif1Fifo(uint32 address, uint32 value)
{
	if(!m_started) return;
	EnqueueRaw(CMD_VIF1_FIFO, nullptr, 0, address, value);
}

void CVu1GifWorker::EnqueueGifFifo(uint32 address, uint32 value)
{
	if(!m_started) return;
	EnqueueRaw(CMD_GIF_FIFO, nullptr, 0, address, value);
}

void CVu1GifWorker::Sync(SYNC_REASON reason)
{
	if(!m_started) return;
	uint64 target = m_writePos.load(std::memory_order_relaxed);
	if(m_batchDepth > 0 && m_pendingWritePos != target)
	{
		m_lastCoalescePos = ~0ULL;
		target = m_pendingWritePos;
		m_writePos.store(target, std::memory_order_release);
		if(m_workerSleeping.load(std::memory_order_acquire))
		{
			std::lock_guard lk(m_mutex);
			m_condWork.notify_one();
		}
	}
	if(m_cachedReadPos >= target)
	{
		return;
	}
	m_cachedReadPos = m_readPos.load(std::memory_order_acquire);
	if(m_cachedReadPos >= target)
	{
		return;
	}

	auto t0 = m_timingStatsEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
	{
		std::unique_lock lk(m_mutex);
		m_syncRequested.store(true, std::memory_order_seq_cst);
		if(m_workerSleeping.load(std::memory_order_acquire))
		{
			m_condWork.notify_one();
		}
		m_condDone.wait(lk, [&]() { return m_readPos.load(std::memory_order_acquire) >= target; });
		m_syncRequested.store(false, std::memory_order_relaxed);
	}
	if(m_timingStatsEnabled)
	{
		m_syncWaitNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
		m_syncs++;
		if(reason < SYNC_REASON_MAX) m_syncByReason[reason]++;
	}
}

void CVu1GifWorker::WaitIdleLockFree()
{
	if(!m_started) return;
	uint64 target = m_writePos.load(std::memory_order_seq_cst);
	if(m_cachedReadPos >= target) return;
	while(m_readPos.load(std::memory_order_acquire) < target)
	{
		std::this_thread::yield();
	}
	m_cachedReadPos = target;
}

void CVu1GifWorker::ResetDeferred()
{
	m_deferredPath3Size = 0;
	m_deferredPath3Offset = 0;
	m_deferredSignalBytes.clear();
	ClearStalledVif1();
}

void CVu1GifWorker::ClearStalledVif1()
{
	m_stalledVif1Head = 0;
	m_stalledVif1Meta.clear();
	m_stalledVif1BufSize = 0;
}

void CVu1GifWorker::AppendStalledVif1(const uint8* data, uint32 size, bool tagIncluded)
{
	if(size == 0) return;
	if(m_stalledVif1Head >= m_stalledVif1Meta.size())
	{
		ClearStalledVif1();
	}
	if((m_stalledVif1BufSize + size) > m_stalledVif1BufCap)
	{
		if(m_stalledVif1Head > 0)
		{
			uint32 baseOff = m_stalledVif1Meta[m_stalledVif1Head].offset;
			size_t rem = m_stalledVif1BufSize - baseOff;
			if(rem > 0)
			{
				memmove(m_stalledVif1Buf, m_stalledVif1Buf + baseOff, rem);
			}
			size_t remCount = m_stalledVif1Meta.size() - m_stalledVif1Head;
			for(size_t i = 0; i < remCount; i++)
			{
				m_stalledVif1Meta[i] = m_stalledVif1Meta[m_stalledVif1Head + i];
				m_stalledVif1Meta[i].offset -= baseOff;
			}
			m_stalledVif1Meta.resize(remCount);
			m_stalledVif1Head = 0;
			m_stalledVif1BufSize = rem;
		}
		if((m_stalledVif1BufSize + size) > m_stalledVif1BufCap)
		{
			size_t newCap = (std::max)(m_stalledVif1BufCap * 2, m_stalledVif1BufSize + size);
			auto* newBuf = reinterpret_cast<uint8*>(framework_aligned_alloc(newCap, 64));
			if(m_stalledVif1BufSize > 0)
			{
				memcpy(newBuf, m_stalledVif1Buf, m_stalledVif1BufSize);
			}
			framework_aligned_free(m_stalledVif1Buf);
			m_stalledVif1Buf = newBuf;
			m_stalledVif1BufCap = newCap;
		}
	}
	uint32 off = static_cast<uint32>(m_stalledVif1BufSize);
	memcpy(m_stalledVif1Buf + off, data, size);
	m_stalledVif1BufSize += size;
	m_stalledVif1Meta.push_back(STALLED_VIF1_META{off, size, tagIncluded});
}

void CVu1GifWorker::AppendDeferredPath3(const uint8* data, uint32 size)
{
	if(size == 0) return;
	if((m_deferredPath3Size + size) > m_deferredPath3Cap)
	{
		if(m_deferredPath3Offset > 0)
		{
			size_t rem = m_deferredPath3Size - m_deferredPath3Offset;
			if(rem > 0)
			{
				memmove(m_deferredPath3Buf, m_deferredPath3Buf + m_deferredPath3Offset, rem);
			}
			m_deferredPath3Size = rem;
			m_deferredPath3Offset = 0;
		}
		if((m_deferredPath3Size + size) > m_deferredPath3Cap)
		{
			size_t newCap = (std::max)(m_deferredPath3Cap * 2, m_deferredPath3Size + size);
			auto* newBuf = reinterpret_cast<uint8*>(framework_aligned_alloc(newCap, 64));
			if(m_deferredPath3Size > 0)
			{
				memcpy(newBuf, m_deferredPath3Buf, m_deferredPath3Size);
			}
			framework_aligned_free(m_deferredPath3Buf);
			m_deferredPath3Buf = newBuf;
			m_deferredPath3Cap = newCap;
		}
	}
	memcpy(m_deferredPath3Buf + m_deferredPath3Size, data, size);
	m_deferredPath3Size += size;
}

void CVu1GifWorker::ReplayStalledVif1()
{
	while(m_stalledVif1Head < m_stalledVif1Meta.size())
	{
		auto& meta = m_stalledVif1Meta[m_stalledVif1Head];
		uint32 consumed = m_vpu1->GetVif().ProcessBuffer(m_stalledVif1Buf + meta.offset, meta.size, meta.tagIncluded);
		if(consumed < meta.size)
		{
			meta.offset += consumed;
			meta.size -= consumed;
			if(consumed != 0)
			{
				meta.tagIncluded = false;
			}
			return;
		}
		m_stalledVif1Head++;
	}
	ClearStalledVif1();
}

void CVu1GifWorker::ReplayDeferredSignal()
{
	if(m_deferredSignalBytes.empty()) return;
	auto bytes = std::move(m_deferredSignalBytes);
	m_deferredSignalBytes.clear();
	ProcessGifChunk(bytes.data(), static_cast<uint32>(bytes.size()));
}

CVu1GifWorker::STATS CVu1GifWorker::TakeStats()
{
	STATS s;
	s.workerBusyNs = m_workerBusyNs.exchange(0, std::memory_order_relaxed);
	s.syncWaitNs = m_syncWaitNs;
	s.vif1Packets = m_vif1Packets;
	s.gifPackets = m_gifPackets;
	s.zcVif1Bytes = m_zcVif1Bytes;
	s.zcGifBytes = m_zcGifBytes;
	s.zcFaults = m_zcFaults;
	s.syncs = m_syncs;
	for(uint32 i = 0; i < SYNC_REASON_MAX; i++)
	{
		s.syncByReason[i] = m_syncByReason[i];
		m_syncByReason[i] = 0;
	}
	for(uint32 i = 0; i < 32; i++)
	{
		s.vif1RegSyncs[i] = m_vif1RegSyncs[i];
		m_vif1RegSyncs[i] = 0;
	}
	m_syncWaitNs = 0;
	m_vif1Packets = 0;
	m_gifPackets = 0;
	m_zcVif1Bytes = 0;
	m_zcGifBytes = 0;
	m_zcFaults = 0;
	m_syncs = 0;
	return s;
}

void CVu1GifWorker::DrainDeferredPath3()
{
	if(m_deferredPath3Offset >= m_deferredPath3Size)
	{
		m_deferredPath3Size = 0;
		m_deferredPath3Offset = 0;
		return;
	}
	if(m_gif.IsPath3Masked() || !m_deferredSignalBytes.empty())
	{
		return;
	}
	size_t avail = m_deferredPath3Size - m_deferredPath3Offset;
	uint32 consumed = m_gif.ProcessPath3Buffer(
	    m_deferredPath3Buf + m_deferredPath3Offset,
	    static_cast<uint32>(avail));
	m_deferredPath3Offset += consumed;
	if(m_deferredPath3Offset >= m_deferredPath3Size)
	{
		m_deferredPath3Size = 0;
		m_deferredPath3Offset = 0;
	}
	else if(m_gif.IsSignalPending())
	{
		m_deferredSignalBytes.insert(m_deferredSignalBytes.end(),
		                             m_deferredPath3Buf + m_deferredPath3Offset,
		                             m_deferredPath3Buf + m_deferredPath3Size);
		m_deferredPath3Size = 0;
		m_deferredPath3Offset = 0;
	}
}

void CVu1GifWorker::OnPath3Unmasked()
{
	if(m_deferredPath3Offset >= m_deferredPath3Size) return;
	size_t avail = m_deferredPath3Size - m_deferredPath3Offset;
	uint32 consumed = m_gif.ProcessOneMaskedPath3Packet(
	    m_deferredPath3Buf + m_deferredPath3Offset,
	    static_cast<uint32>(avail));
	m_deferredPath3Offset += consumed;
	if(m_deferredPath3Offset >= m_deferredPath3Size)
	{
		m_deferredPath3Size = 0;
		m_deferredPath3Offset = 0;
	}
	else if(m_deferredPath3Offset >= (3 * 1024 * 1024))
	{
		size_t rem = m_deferredPath3Size - m_deferredPath3Offset;
		memmove(m_deferredPath3Buf, m_deferredPath3Buf + m_deferredPath3Offset, rem);
		m_deferredPath3Size = rem;
		m_deferredPath3Offset = 0;
	}
}

void CVu1GifWorker::ProcessVif1Chunk(uint8* data, uint32 size, bool tagIncluded)
{
	if(m_stalledVif1Head < m_stalledVif1Meta.size())
	{
		AppendStalledVif1(data, size, tagIncluded);
		return;
	}
	uint32 consumed = m_vpu1->GetVif().ProcessBuffer(data, size, tagIncluded);
	if(consumed < size)
	{
		AppendStalledVif1(data + consumed, size - consumed, (consumed == 0) ? tagIncluded : false);
	}
}

void CVu1GifWorker::ProcessGifChunk(const uint8* data, uint32 size)
{
	if(m_gif.IsPath3Masked())
	{
		AppendDeferredPath3(data, size);
		return;
	}
	if(m_deferredPath3Offset < m_deferredPath3Size)
	{
		AppendDeferredPath3(data, size);
		DrainDeferredPath3();
		return;
	}
	if(!m_deferredSignalBytes.empty())
	{
		m_deferredSignalBytes.insert(m_deferredSignalBytes.end(), data, data + size);
		return;
	}
	uint32 consumed = m_gif.ProcessPath3Buffer(data, size);
	if(consumed < size)
	{
		m_deferredSignalBytes.insert(m_deferredSignalBytes.end(), data + consumed, data + size);
	}
}

void CVu1GifWorker::ThreadProc()
{
	ThreadAffinity::ApplyCurrentThread(ThreadAffinity::Role::Vu1Worker);
	fesetround(FE_TOWARDZERO);
	FpUtils::SetDenormalHandlingMode();

	uint64 r = 0;
	while(true)
	{
		uint64 w = m_writePos.load(std::memory_order_acquire);
		if(r < w)
		{
			auto t0 = m_timingStatsEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			uint64 publishedR = r;
			uint32 batchPkts = 0;
			while(r < w)
			{
				uint32 off = static_cast<uint32>(r & RING_MASK);
				auto* hdr = reinterpret_cast<PACKET_HEADER*>(m_ring + off);
				uint32 type = hdr->type;
				uint32 size = hdr->size;
				uint32 arg0 = hdr->arg0;
				uint32 arg1 = hdr->arg1;
				uint32 alignedSize = (size + 15U) & ~15U;
				uint8* payload = m_ring + off + sizeof(PACKET_HEADER);

				switch(type)
				{
				case CMD_WRAP:
					break;
				case CMD_VIF1_DMA:
					ProcessVif1Chunk(payload, size, arg0 != 0);
					break;
				case CMD_GIF_DMA:
					ProcessGifChunk(payload, size);
					break;
				case CMD_VIF1_FIFO:
					m_vpu1->GetVif().SetRegister(arg0, arg1);
					break;
				case CMD_GIF_FIFO:
					m_gif.SetRegister(arg0, arg1);
					break;
				case CMD_VIF1_DMA_PTR:
					ProcessVif1Chunk(m_eeRam + arg0, arg1, false);
					break;
				case CMD_GIF_DMA_PTR:
					ProcessGifChunk(m_eeRam + arg0, arg1);
					break;
				default:
					assert(false);
					break;
				}

				r += sizeof(PACKET_HEADER) + alignedSize;
				batchPkts++;
				if((batchPkts >= 32) || ((r - publishedR) >= 65536) || (r >= w))
				{
					m_readPos.store(r, std::memory_order_release);
					publishedR = r;
					batchPkts = 0;
					if(m_producerWaitingSpace.load(std::memory_order_acquire))
					{
						std::lock_guard lk(m_mutex);
						m_condDone.notify_one();
					}
					w = m_writePos.load(std::memory_order_acquire);
				}
			}
			if(!m_gif.IsPath3Masked() && (m_deferredPath3Offset < m_deferredPath3Size))
			{
				DrainDeferredPath3();
			}
			if(m_timingStatsEnabled)
			{
				uint64 dt = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
				m_workerBusyNs.fetch_add(dt, std::memory_order_relaxed);
			}
			if(m_syncRequested.load(std::memory_order_acquire))
			{
				std::lock_guard lk(m_mutex);
				m_condDone.notify_all();
			}
			continue;
		}

		if(m_stop.load(std::memory_order_acquire))
		{
			break;
		}

		//Short spin before sleeping to avoid futex latency between bursts within a frame.
		bool gotWork = false;
		for(int spin = 0; spin < 128; spin++)
		{
			if(m_writePos.load(std::memory_order_acquire) != r ||
			   m_stop.load(std::memory_order_acquire))
			{
				gotWork = true;
				break;
			}
			std::this_thread::yield();
		}
		if(gotWork) continue;

		std::unique_lock lk(m_mutex);
		m_workerSleeping.store(true, std::memory_order_seq_cst);
		if(m_syncRequested.load(std::memory_order_acquire))
		{
			m_condDone.notify_all();
		}
		if(!m_stop.load(std::memory_order_acquire) &&
		   (r == m_writePos.load(std::memory_order_seq_cst)))
		{
			m_condWork.wait(lk);
		}
		m_workerSleeping.store(false, std::memory_order_relaxed);
	}
}
