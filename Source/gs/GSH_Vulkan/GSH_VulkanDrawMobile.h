#pragma once

#include <cfloat>
#include "GSH_VulkanDraw.h"

namespace GSH_Vulkan
{
	class CDrawMobile : public CDraw
	{
	public:
		CDrawMobile(const ContextPtr&, const FrameCommandBufferPtr&);
		virtual ~CDrawMobile();

		void SetPipelineCaps(const PIPELINE_CAPS&) override;
		void SetFramebufferParams(uint32, uint32, uint32) override;
		void SetDepthbufferParams(uint32, uint32) override;
		void SetScissor(uint32, uint32, uint32, uint32) override;

		void FlushVertices() override;
		void FlushRenderPass() override;
		void PostFlushFrameCommandBuffer() override;
		bool CanLoadClutDuringPass(uint32, uint32, uint32) override;
		bool CanHostTransferDuringPass(uint32, uint32) override;
		void SyncMemoryForRead(uint32 address, uint32 size) override;
		void SyncMemoryForWrite(uint32 address, uint32 size) override;
		void FlushDirtyColorSlots() override;
		void FlushAllResidentMemory() override;
		void InvalidateAllResidentMemory() override;
		void SetStatsEnabled(bool enabled) override
		{
			m_statsEnabled = enabled;
		}
		STATS TakeStats() override
		{
			auto s = m_stats;
			m_stats = {};
			return s;
		}

	private:
		enum PASS_MODE : uint32
		{
			PASS_MODE_NO_DEPTH = 0,          //1 att: color only (COLOR_ATTACHMENT_OPTIMAL, LOAD/STORE -> GMEM+UBWC)
			PASS_MODE_DEPTH = 1,             //2 atts: color + hwDepth (OPTIMAL, LOAD/STORE -> GMEM+UBWC+LRZ)
			PASS_MODE_FEEDBACK_NO_DEPTH = 2, //1 att: color (GENERAL, input att 0)
			PASS_MODE_FEEDBACK_DEPTH = 3,    //2 atts: color + hwDepth (GENERAL, input att 0+1)
			PASS_MODE_COUNT = 4,
		};

		static constexpr uint32 RESIDENT_COLOR_SLOTS = 8;
		static constexpr uint32 RESIDENT_DEPTH_SLOTS = 8;

		struct RESIDENT_COLOR_SLOT
		{
			bool allocated = false;
			Framework::Vulkan::CImage image;
			VkImageView imageView = VK_NULL_HANDLE;
			VkFramebuffer drawFbs[RESIDENT_DEPTH_SLOTS][PASS_MODE_COUNT] = {};
			VkFramebuffer loadFb = VK_NULL_HANDLE;
			VkFramebuffer storeFb = VK_NULL_HANDLE;
			VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			bool valid = false;
			bool dirty = false;
			uint32 addr = 0;
			uint32 width = 0;
			uint32 psm = 0; //Canonical format (PSMCT24 normalized to PSMCT32)
			int32 loadedX0 = 0, loadedY0 = 0, loadedX1 = 0, loadedY1 = 0;
			int32 dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;
			std::pair<uint64, uint64> gsRange = {0, 0};
			uint64 lastUsedSeq = 0;
		};

		struct RESIDENT_DEPTH_STATE
		{
			bool allocated = false;
			Framework::Vulkan::CImage image;
			VkImageView imageView = VK_NULL_HANDLE;
			VkFramebuffer loadFb = VK_NULL_HANDLE;
			VkFramebuffer storeFb = VK_NULL_HANDLE;
			VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

			bool valid = false;
			bool dirty = false;
			uint32 addr = 0;
			uint32 width = 0;
			uint32 psm = 0;
			int32 loadedX0 = 0, loadedY0 = 0, loadedX1 = 0, loadedY1 = 0;
			int32 dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;
			std::pair<uint64, uint64> gsRange = {0, 0};
			uint64 lastUsedSeq = 0;
		};

		static uint32 CanonicalFbFormat(uint32 psm);

		void EnsureColorSlotAllocated(uint32 slotIdx);
		void EnsureDepthSlotAllocated(uint32 slotIdx);
		VkFramebuffer GetDrawFramebuffer(uint32 colorSlotIdx, uint32 depthSlotIdx, uint32 passMode);
		uint32 AcquireColorSlot(uint32 addr, uint32 width, uint32 psm, int32 x0, int32 y0, int32 x1, int32 y1, bool skipColorLoad);
		uint32 AcquireDepthSlot(uint32 addr, uint32 width, uint32 psm, int32 x0, int32 y0, int32 x1, int32 y1, bool skipDepthLoad, uint32 activeColorSlotIdx);
		void LoadColorSlotFromMemory(uint32 slotIdx, int32 x0, int32 y0, int32 x1, int32 y1, bool preserve = false);
		void FlushColorSlotToMemory(uint32 slotIdx);
		void LoadDepthFromMemory(uint32 depthSlotIdx, int32 x0, int32 y0, int32 x1, int32 y1, bool preserve = false);
		void FlushDepthToMemory(uint32 depthSlotIdx);
		void TransitionColorImage(VkCommandBuffer cmdBuf, RESIDENT_COLOR_SLOT& slot, VkImageLayout newLayout);
		void TransitionDepthImage(VkCommandBuffer cmdBuf, RESIDENT_DEPTH_STATE& slot, VkImageLayout newLayout);
		void EmitMemoryStoreBarrier(VkCommandBuffer cmdBuf);

		void RecordRenderPass(int32, int32, int32, int32);
		VkDescriptorSet PrepareDescriptorSet(VkDescriptorSetLayout, const DESCRIPTORSET_CAPS&);

		void CreateRenderPasses();
		void CreateSharedLayouts();

		struct SHARED_LAYOUT
		{
			VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
			VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		};
		static constexpr uint32 DRAW_LAYOUT_COUNT = 8; //bit0=hasTex, bit1=isIdtex, bit2=dynMip
		SHARED_LAYOUT m_drawLayouts[DRAW_LAYOUT_COUNT];
		SHARED_LAYOUT m_loadStoreLayout;
		uint32 GetDrawLayoutIndex(const PIPELINE_CAPS&) const;

		PIPELINE CreateDrawPipeline(const PIPELINE_CAPS&, uint32 passMode);
		Framework::Vulkan::CShaderModule CreateDrawFragmentShader(const PIPELINE_CAPS&, uint32 passMode);

		struct HW_BLEND_CONFIG
		{
			bool supported = false;
			bool blendEnable = false;
			bool dualSrc = false;
			bool usesConstColor = false;
			bool premulSrcByC = false;
			bool premulSrcByOneMinusC = false;
			bool zeroSrcColor = false;
			VkBlendFactor srcColor = VK_BLEND_FACTOR_ONE;
			VkBlendFactor dstColor = VK_BLEND_FACTOR_ZERO;
			VkBlendOp colorOp = VK_BLEND_OP_ADD;
		};

		static HW_BLEND_CONFIG GetHardwareBlendConfig(const PIPELINE_CAPS&, bool hasDualSrcBlend);
		static uint8 GetHardwareColorWriteMask(uint32 framebufferFormat, uint32 fbWriteMask);
		static bool WritesFragDepth(const PIPELINE_CAPS&);
		static bool UsesSoftwareColorRead(const PIPELINE_CAPS&, const HW_BLEND_CONFIG&);
		static VkCompareOp MapDepthCompareOp(uint32 depthTestFunction);

		static PIPELINE_CAPS MakeLoadStorePipelineCaps(const PIPELINE_CAPS&);
		static std::pair<uint64, uint64> GetBufferRange(uint32 bufAddr, uint32 bufWidth, uint32 psm, uint32 rows);
		static bool RangesOverlap(const std::pair<uint64, uint64>&, const std::pair<uint64, uint64>&);
		bool FramebufferOverlapsDepthbuffer() const;
		bool TextureOverlapsDepthbuffer() const;
		bool TextureOverlapsFramebuffer() const;

		PIPELINE CreateLoadColorPipeline(uint32 fbFormat);
		PIPELINE CreateStoreColorPipeline(uint32 fbFormat);
		PIPELINE CreateLoadDepthPipeline(uint32 depthFormat);
		PIPELINE CreateStoreDepthPipeline(uint32 depthFormat);
		Framework::Vulkan::CShaderModule CreateLoadStoreVertexShader();
		Framework::Vulkan::CShaderModule CreateLoadColorFragmentShader(uint32 fbFormat);
		Framework::Vulkan::CShaderModule CreateStoreColorFragmentShader(uint32 fbFormat);
		Framework::Vulkan::CShaderModule CreateLoadDepthFragmentShader(uint32 depthFormat);
		Framework::Vulkan::CShaderModule CreateStoreDepthFragmentShader(uint32 depthFormat);

		PipelineCache m_drawPipelineCaches[PASS_MODE_COUNT];
		PipelineCache m_loadColorPipelineCache;
		PipelineCache m_storeColorPipelineCache;
		PipelineCache m_loadDepthPipelineCache;
		PipelineCache m_storeDepthPipelineCache;

		VkRenderPass m_renderPasses[PASS_MODE_COUNT] = {};
		VkRenderPass m_rpLoadColor = VK_NULL_HANDLE;
		VkRenderPass m_rpLoadColorPreserve = VK_NULL_HANDLE;
		VkRenderPass m_rpStoreColor = VK_NULL_HANDLE;
		VkRenderPass m_rpLoadDepth = VK_NULL_HANDLE;
		VkRenderPass m_rpLoadDepthPreserve = VK_NULL_HANDLE;
		VkRenderPass m_rpStoreDepth = VK_NULL_HANDLE;

		RESIDENT_COLOR_SLOT m_residentColors[RESIDENT_COLOR_SLOTS];
		RESIDENT_DEPTH_STATE m_residentDepths[RESIDENT_DEPTH_SLOTS];
		uint64 m_residentSeq = 0;

		//Draws are recorded when the render pass ends, once the area they touch is known: the pass
		//then loads, renders and stores only that area instead of the whole scissor.
		struct VERTEX_SLICE
		{
			uint32 start = 0;
			uint32 count = 0;
			uint32 next = ~0U;
		};

		struct PENDING_DRAW
		{
			PIPELINE_CAPS drawPipelineCaps;
			DESCRIPTORSET_CAPS descriptorSetCaps;
			bool hasTexture = false;
			uint32 textureFormat = 0;
			std::pair<uint64, uint64> texRange = {0, 0};
			bool hasClutOffset = false;
			uint32 clutOffset = 0;
			bool hasMipParams = false;
			uint32 mipParamsIndex = 0;
			bool readsColor = false;
			bool readsDepth = false;
			bool usesConstColor = false;
			uint8 readReason = 0; //1=alpha, 2=depthOnly, 3+=other
			int32 minX = 0;
			int32 minY = 0;
			int32 maxX = 0;
			int32 maxY = 0;
			uint32 vertexStart = 0;
			uint32 vertexCount = 0;
			uint32 firstSlice = ~0U;
			uint32 lastSlice = ~0U;
			uint32 sliceCount = 1;
			DRAW_PIPELINE_PUSHCONSTANTS pushConstants;
		};

		static bool CanDrawsCommute(const PENDING_DRAW& a, const PENDING_DRAW& b);

		static constexpr uint32 MAX_INDEX_COUNT = MAX_VERTEX_COUNT;
		Framework::Vulkan::CBuffer m_indexBuffers[MAX_FRAMES];
		uint32* m_indexBufferPtrs[MAX_FRAMES] = {};
		uint32 m_indexBufferCursor = 0;

		std::vector<PENDING_DRAW> m_pendingDraws;
		std::vector<VERTEX_SLICE> m_pendingSlices;
		DRAW_PIPELINE_PUSHCONSTANTS m_passLoadPushConstants;

		//The pass stores depth back to memory if any of its draws writes depth. Draws that don't
		//write it output the depth they loaded, so storing it is harmless for them.
		bool m_passWriteDepth = false;
		bool m_passReadDepth = false;
		bool m_passHasDepth = false;
		bool m_passNeedsColorFeedback = false;
		bool m_passNeedsDepthFeedback = false;
		bool m_passHasDualSrc = false;
		uint32 m_passFbFormat = 0;
		uint32 m_passDepthFormat = 0;
		uint32 m_passDepthBufAddr = 0;
		uint32 m_passDepthBufWidth = 0;

		static constexpr uint32 FAST_CACHE_SIZE = 64;
		struct PIPELINE_FAST_ENTRY
		{
			PipelineCapsInt caps = ~0ULL;
			uint32 passMode = PASS_MODE_COUNT;
			const PIPELINE* pipeline = nullptr;
		};
		struct DESC_FAST_ENTRY
		{
			DescriptorSetCapsInt caps = ~0U;
			VkDescriptorSet descSet = VK_NULL_HANDLE;
		};
		PIPELINE_FAST_ENTRY m_pipelineFastCache[FAST_CACHE_SIZE];
		DESC_FAST_ENTRY m_descFastCache[FAST_CACHE_SIZE];

		float m_renderPassMinX = FLT_MAX;
		float m_renderPassMinY = FLT_MAX;
		float m_renderPassMaxX = -FLT_MAX;
		float m_renderPassMaxY = -FLT_MAX;
		bool m_statsEnabled = false;
		STATS m_stats;
	};
}
