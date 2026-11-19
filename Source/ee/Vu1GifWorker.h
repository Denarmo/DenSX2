#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include "Types.h"

class CVpu;
class CGIF;

namespace Ee
{
	class CVu1GifWorker
	{
	public:
		enum SYNC_REASON : uint32
		{
			SYNC_VBLANK = 0,
			SYNC_VIF1_REG = 1,
			SYNC_GIF_REG = 2,
			SYNC_VUMEM = 3,
			SYNC_GS_PRIV = 4,
			SYNC_OTHER = 5,
			SYNC_REASON_MAX = 6
		};

		struct STATS
		{
			uint64 workerBusyNs = 0;
			uint64 syncWaitNs = 0;
			uint32 vif1Packets = 0;
			uint32 gifPackets = 0;
			uint64 zcVif1Bytes = 0;
			uint64 zcGifBytes = 0;
			uint32 zcFaults = 0;
			uint32 syncs = 0;
			uint32 syncByReason[SYNC_REASON_MAX] = {};
			uint32 vif1RegSyncs[32] = {}; //low 4 bits = (addr>>4)&0xF, bit4 = isWrite
		};

		CVu1GifWorker(std::shared_ptr<CVpu>& vpu1, CGIF& gif);
		~CVu1GifWorker();

		void SetEeRam(uint8* eeRam)
		{
			m_eeRam = eeRam;
		}

		void Start();
		void Stop();

		bool IsRunning() const
		{
			return m_started;
		}

		//Returns true if enqueued asynchronously, false if caller should execute synchronously after Sync().
		bool EnqueueVif1Dma(const uint8* data, uint32 size, bool tagIncluded);
		bool EnqueueVif1DmaPtr(uint32 ramOffset, uint32 size);
		bool EnqueueGifDma(const uint8* data, uint32 size);
		bool EnqueueGifDmaPtr(uint32 ramOffset, uint32 size);
		void EnqueueVif1Fifo(uint32 address, uint32 value);
		void EnqueueGifFifo(uint32 address, uint32 value);

		void BeginDmaBatch();
		void EndDmaBatch();

		void Sync(SYNC_REASON reason);
		void WaitIdleLockFree();
		void RecordZeroCopyFault()
		{
			if(m_timingStatsEnabled) m_zcFaults++;
		}
		void ResetDeferred();
		void ReplayStalledVif1();
		void ReplayDeferredSignal();

		static void SetTimingStatsEnabled(bool enabled)
		{
			m_timingStatsEnabled = enabled;
		}
		static bool IsTimingStatsEnabled()
		{
			return m_timingStatsEnabled;
		}
		void RecordVif1RegSync(uint32 address, bool isWrite)
		{
			if(m_timingStatsEnabled)
			{
				uint32 idx = ((address >> 4) & 0xFU) | (isWrite ? 0x10U : 0U);
				m_vif1RegSyncs[idx]++;
			}
		}
		STATS TakeStats();

	private:
		enum CMD_TYPE : uint32
		{
			CMD_WRAP = 0,
			CMD_VIF1_DMA = 1,
			CMD_GIF_DMA = 2,
			CMD_VIF1_FIFO = 3,
			CMD_GIF_FIFO = 4,
			CMD_VIF1_DMA_PTR = 5,
			CMD_GIF_DMA_PTR = 6,
		};

		struct alignas(16) PACKET_HEADER
		{
			uint32 type;
			uint32 size;
			uint32 arg0;
			uint32 arg1;
		};
		static_assert(sizeof(PACKET_HEADER) == 16, "PACKET_HEADER must be 16 bytes.");

		struct STALLED_VIF1_META
		{
			uint32 offset = 0;
			uint32 size = 0;
			bool tagIncluded = false;
		};

		static constexpr uint32 RING_SIZE = 8 * 1024 * 1024;
		static constexpr uint32 RING_MASK = RING_SIZE - 1;

		bool EnqueueRaw(CMD_TYPE type, const uint8* data, uint32 size, uint32 arg0, uint32 arg1);
		void ThreadProc();
		void ProcessVif1Chunk(uint8* data, uint32 size, bool tagIncluded);
		void AppendStalledVif1(const uint8* data, uint32 size, bool tagIncluded);
		void ClearStalledVif1();
		void ProcessGifChunk(const uint8* data, uint32 size);
		void DrainDeferredPath3();
		void OnPath3Unmasked();
		void AppendDeferredPath3(const uint8* data, uint32 size);

		std::shared_ptr<CVpu>& m_vpu1;
		CGIF& m_gif;
		uint8* m_eeRam = nullptr;

		uint8* m_ring = nullptr;
		std::thread m_thread;
		bool m_started = false;
		std::atomic<bool> m_stop{false};

		alignas(64) std::atomic<uint64> m_writePos{0};
		uint64 m_cachedReadPos = 0;
		uint64 m_pendingWritePos = 0;
		uint64 m_lastCoalescePos = ~0ULL;
		uint32 m_batchDepth = 0;

		alignas(64) std::atomic<uint64> m_readPos{0};

		alignas(64) std::atomic<bool> m_workerSleeping{false};

		alignas(64) std::atomic<bool> m_producerWaitingSpace{false};
		std::atomic<bool> m_syncRequested{false};

		std::mutex m_mutex;
		std::condition_variable m_condWork;
		std::condition_variable m_condDone;

		uint8* m_deferredPath3Buf = nullptr;
		size_t m_deferredPath3Size = 0;
		size_t m_deferredPath3Cap = 0;
		size_t m_deferredPath3Offset = 0;
		std::vector<uint8> m_deferredSignalBytes;
		uint8* m_stalledVif1Buf = nullptr;
		size_t m_stalledVif1BufSize = 0;
		size_t m_stalledVif1BufCap = 0;
		std::vector<STALLED_VIF1_META> m_stalledVif1Meta;
		size_t m_stalledVif1Head = 0;

		static bool m_timingStatsEnabled;
		std::atomic<uint64> m_workerBusyNs{0};
		uint64 m_syncWaitNs = 0;
		uint32 m_vif1Packets = 0;
		uint32 m_gifPackets = 0;
		uint64 m_zcVif1Bytes = 0;
		uint64 m_zcGifBytes = 0;
		uint32 m_zcFaults = 0;
		uint32 m_syncs = 0;
		uint32 m_syncByReason[SYNC_REASON_MAX] = {};
		uint32 m_vif1RegSyncs[32] = {};
	};
}
