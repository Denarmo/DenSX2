#include "GSH_VulkanDrawMobile.h"
#include <cmath>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "GSH_VulkanDrawUtils.h"
#include "GSH_VulkanMemoryUtils.h"
#include "string_format.h"
#include "MemStream.h"
#include "vulkan/StructDefs.h"
#include "vulkan/Utils.h"
#include "nuanceur/Builder.h"
#include "nuanceur/generators/SpirvShaderGenerator.h"
#include "../GSHandler.h"
#include "../GsPixelFormats.h"

using namespace GSH_Vulkan;

#define VERTEX_ATTRIB_LOCATION_POSITION 0
#define VERTEX_ATTRIB_LOCATION_DEPTH 1
#define VERTEX_ATTRIB_LOCATION_COLOR 2
#define VERTEX_ATTRIB_LOCATION_TEXCOORD 3
#define VERTEX_ATTRIB_LOCATION_FOG 4

#define DESCRIPTOR_LOCATION_BUFFER_MEMORY 0
#define DESCRIPTOR_LOCATION_IMAGE_CLUT 1
#define DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_TEX 2
#define DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_FB 3
#define DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_DEPTH 4
#define DESCRIPTOR_LOCATION_IMAGE_INPUT_COLOR 5
#define DESCRIPTOR_LOCATION_IMAGE_INPUT_DEPTH 6
#define DESCRIPTOR_LOCATION_UNIFORM_MIPPARAMS 7
#define DESCRIPTOR_LOCATION_BUFFER_MEMORY_16BIT 8

#define DRAW_AREA_SIZE 2048
#define MAX_VERTEX_COUNT 1024 * 512

#define DEPTH_MAX (4294967296.0f)

CDrawMobile::CDrawMobile(const ContextPtr& context, const FrameCommandBufferPtr& frameCommandBuffer)
    : CDraw(context, frameCommandBuffer)
    , m_drawPipelineCaches{PipelineCache(context->device), PipelineCache(context->device), PipelineCache(context->device), PipelineCache(context->device)}
    , m_loadColorPipelineCache(context->device)
    , m_storeColorPipelineCache(context->device)
    , m_loadDepthPipelineCache(context->device)
    , m_storeDepthPipelineCache(context->device)
{
	CreateSharedLayouts();
	CreateRenderPasses();
	EnsureDepthSlotAllocated(0);
	EnsureColorSlotAllocated(0);

	for(uint32 i = 0; i < MAX_FRAMES; i++)
	{
		m_indexBuffers[i] = Framework::Vulkan::CBuffer(
		    m_context->device, m_context->physicalDeviceMemoryProperties,
		    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		    sizeof(uint32) * MAX_INDEX_COUNT);
		auto result = m_context->device.vkMapMemory(m_context->device, m_indexBuffers[i].GetMemory(),
		                                            0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&m_indexBufferPtrs[i]));
		CHECKVULKANERROR(result);
	}
	m_pendingDraws.reserve(256);
	m_pendingSlices.reserve(512);
}

CDrawMobile::~CDrawMobile()
{
	for(uint32 i = 0; i < MAX_FRAMES; i++)
	{
		if(m_indexBufferPtrs[i])
		{
			m_context->device.vkUnmapMemory(m_context->device, m_indexBuffers[i].GetMemory());
			m_indexBufferPtrs[i] = nullptr;
		}
	}
	for(uint32 s = 0; s < RESIDENT_COLOR_SLOTS; s++)
	{
		auto& slot = m_residentColors[s];
		if(!slot.allocated) continue;
		for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
		{
			for(uint32 i = 0; i < PASS_MODE_COUNT; i++)
			{
				if(slot.drawFbs[d][i] != VK_NULL_HANDLE)
					m_context->device.vkDestroyFramebuffer(m_context->device, slot.drawFbs[d][i], nullptr);
			}
		}
		if(slot.loadFb != VK_NULL_HANDLE)
			m_context->device.vkDestroyFramebuffer(m_context->device, slot.loadFb, nullptr);
		if(slot.storeFb != VK_NULL_HANDLE)
			m_context->device.vkDestroyFramebuffer(m_context->device, slot.storeFb, nullptr);
		if(slot.imageView != VK_NULL_HANDLE)
			m_context->device.vkDestroyImageView(m_context->device, slot.imageView, nullptr);
	}
	for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
	{
		auto& dslot = m_residentDepths[d];
		if(!dslot.allocated) continue;
		if(dslot.loadFb != VK_NULL_HANDLE)
			m_context->device.vkDestroyFramebuffer(m_context->device, dslot.loadFb, nullptr);
		if(dslot.storeFb != VK_NULL_HANDLE)
			m_context->device.vkDestroyFramebuffer(m_context->device, dslot.storeFb, nullptr);
		if(dslot.imageView != VK_NULL_HANDLE)
			m_context->device.vkDestroyImageView(m_context->device, dslot.imageView, nullptr);
	}

	for(uint32 i = 0; i < PASS_MODE_COUNT; i++)
	{
		if(m_renderPasses[i] != VK_NULL_HANDLE)
			m_context->device.vkDestroyRenderPass(m_context->device, m_renderPasses[i], nullptr);
	}
	if(m_rpLoadColor != VK_NULL_HANDLE)
		m_context->device.vkDestroyRenderPass(m_context->device, m_rpLoadColor, nullptr);
	if(m_rpLoadColorPreserve != VK_NULL_HANDLE)
		m_context->device.vkDestroyRenderPass(m_context->device, m_rpLoadColorPreserve, nullptr);
	if(m_rpStoreColor != VK_NULL_HANDLE)
		m_context->device.vkDestroyRenderPass(m_context->device, m_rpStoreColor, nullptr);
	if(m_rpLoadDepth != VK_NULL_HANDLE)
		m_context->device.vkDestroyRenderPass(m_context->device, m_rpLoadDepth, nullptr);
	if(m_rpLoadDepthPreserve != VK_NULL_HANDLE)
		m_context->device.vkDestroyRenderPass(m_context->device, m_rpLoadDepthPreserve, nullptr);
	if(m_rpStoreDepth != VK_NULL_HANDLE)
		m_context->device.vkDestroyRenderPass(m_context->device, m_rpStoreDepth, nullptr);

	for(uint32 i = 0; i < DRAW_LAYOUT_COUNT; i++)
	{
		if(m_drawLayouts[i].pipelineLayout != VK_NULL_HANDLE)
			m_context->device.vkDestroyPipelineLayout(m_context->device, m_drawLayouts[i].pipelineLayout, nullptr);
		if(m_drawLayouts[i].descriptorSetLayout != VK_NULL_HANDLE)
			m_context->device.vkDestroyDescriptorSetLayout(m_context->device, m_drawLayouts[i].descriptorSetLayout, nullptr);
	}
}

uint32 CDrawMobile::CanonicalFbFormat(uint32 psm)
{
	return (psm == CGSHandler::PSMCT24) ? CGSHandler::PSMCT32 : psm;
}

void CDrawMobile::SetPipelineCaps(const PIPELINE_CAPS& caps)
{
	bool changed = static_cast<uint64>(caps) != static_cast<uint64>(m_pipelineCaps);
	if(!changed) return;
	bool nextUsesDepth = (caps.depthTestFunction != CGSHandler::DEPTH_TEST_ALWAYS) || (caps.writeDepth != 0);
	bool hasActivePass = !m_pendingDraws.empty() || (m_passVertexEnd != m_passVertexStart);
	uint32 activeFbFmt = !m_pendingDraws.empty() ? m_passFbFormat : CanonicalFbFormat(m_pipelineCaps.framebufferFormat);
	bool fbFmtChanged = hasActivePass && (CanonicalFbFormat(caps.framebufferFormat) != activeFbFmt);
	bool zbFmtConflict = m_passHasDepth && nextUsesDepth && (caps.depthbufferFormat != m_passDepthFormat);
	bool depthOverlapBlocked = false;
	if((caps.writeDepth != m_pipelineCaps.writeDepth) && FramebufferOverlapsDepthbuffer())
	{
		depthOverlapBlocked = true;
	}

	if(fbFmtChanged || zbFmtConflict || depthOverlapBlocked)
	{
		if(m_statsEnabled && (!m_pendingDraws.empty() || m_passVertexEnd != m_passVertexStart))
		{
			if(depthOverlapBlocked && !fbFmtChanged && !zbFmtConflict)
				m_stats.flushCapsDepthOverlap++;
			else
				m_stats.flushCapsFmt++;
		}
		FlushRenderPass();
	}
	if(caps.textureUseMemoryCopy || m_pipelineCaps.textureUseMemoryCopy)
	{
		if(m_statsEnabled && (!m_pendingDraws.empty() || m_passVertexEnd != m_passVertexStart))
		{
			m_stats.flushCapsMemCopy++;
		}
		FlushRenderPass();
	}
	FlushVertices();
	m_pipelineCaps = caps;
}

void CDrawMobile::SetFramebufferParams(uint32 addr, uint32 width, uint32 writeMask)
{
	bool storeChanged =
	    (m_pushConstants.fbBufAddr != addr) ||
	    (m_pushConstants.fbBufWidth != width);
	bool drawChanged =
	    (m_pushConstants.fbWriteMask != writeMask);
	if(!storeChanged && !drawChanged) return;
	if(storeChanged)
	{
		if(m_statsEnabled && (!m_pendingDraws.empty() || m_passVertexEnd != m_passVertexStart))
		{
			m_stats.flushFbAddr++;
		}
		FlushRenderPass();
	}
	if(drawChanged)
	{
		FlushVertices();
	}
	m_pushConstants.fbBufAddr = addr;
	m_pushConstants.fbBufWidth = width;
	m_pushConstants.fbWriteMask = writeMask;
}

void CDrawMobile::SetDepthbufferParams(uint32 addr, uint32 width)
{
	bool changed =
	    (m_pushConstants.depthBufAddr != addr) ||
	    (m_pushConstants.depthBufWidth != width);
	if(!changed) return;
	if(m_passHasDepth && ((m_passDepthBufAddr != addr) || (m_passDepthBufWidth != width)))
	{
		if(m_statsEnabled && (!m_pendingDraws.empty() || m_passVertexEnd != m_passVertexStart))
		{
			m_stats.flushZbAddr++;
		}
		FlushRenderPass();
	}
	else if(!m_passHasDepth && (m_passVertexEnd != m_passVertexStart))
	{
		FlushVertices();
	}
	m_pushConstants.depthBufAddr = addr;
	m_pushConstants.depthBufWidth = width;
}

void CDrawMobile::SetScissor(uint32 scissorX, uint32 scissorY, uint32 scissorWidth, uint32 scissorHeight)
{
	bool changed =
	    (m_scissorX != scissorX) ||
	    (m_scissorY != scissorY) ||
	    (m_scissorWidth != scissorWidth) ||
	    (m_scissorHeight != scissorHeight);
	if(!changed) return;
	if(m_statsEnabled && (!m_pendingDraws.empty() || m_passVertexEnd != m_passVertexStart))
	{
		m_stats.flushScissor++;
	}
	FlushRenderPass();
	m_scissorX = scissorX;
	m_scissorY = scissorY;
	m_scissorWidth = scissorWidth;
	m_scissorHeight = scissorHeight;
}

void CDrawMobile::FlushVertices()
{
	uint32 vertexCount = m_passVertexEnd - m_passVertexStart;
	if(vertexCount == 0) return;

	float drawMinX = m_curDrawMinX;
	float drawMinY = m_curDrawMinY;
	float drawMaxX = m_curDrawMaxX;
	float drawMaxY = m_curDrawMaxY;
	m_curDrawMinX = m_curDrawMinY = FLT_MAX;
	m_curDrawMaxX = m_curDrawMaxY = -FLT_MAX;

	if((m_pipelineCaps.depthTestFunction == CGSHandler::DEPTH_TEST_NEVER) ||
	   ((m_pipelineCaps.alphaTestFunction == CGSHandler::ALPHA_TEST_NEVER) &&
	    ((m_pipelineCaps.alphaTestFailAction == CGSHandler::ALPHA_TEST_FAIL_KEEP) ||
	     ((m_pipelineCaps.alphaTestFailAction == CGSHandler::ALPHA_TEST_FAIL_ZBONLY) && !m_pipelineCaps.writeDepth))))
	{
		if(m_statsEnabled) m_stats.drawSkipped++;
		m_passVertexStart = m_passVertexEnd;
		return;
	}

	uint8 hwColorMask = 0xF;
	bool promotedMaskToHw = false;
	if(m_pipelineCaps.maskColor)
	{
		hwColorMask = GetHardwareColorWriteMask(m_pipelineCaps.framebufferFormat, m_pushConstants.fbWriteMask);
		if(hwColorMask == 0 && !m_pipelineCaps.writeDepth)
		{
			if(m_statsEnabled) m_stats.drawSkipped++;
			m_passVertexStart = m_passVertexEnd;
			return;
		}
		if(hwColorMask != 0xFF)
		{
			promotedMaskToHw = true;
		}
	}

	//Draws only see memory as it was when the pass began: one that samples the depth buffer after
	//draws of this pass wrote depth, or samples memory overlapping the framebuffer, goes to a new
	//pass so it sees what the previous draws wrote.
	if(m_pipelineCaps.hasTexture && !m_pendingDraws.empty())
	{
		bool needsFlush = false;
		if(m_pipelineCaps.textureUseMemoryCopy)
		{
			if(m_statsEnabled) m_stats.flushCapsMemCopy++;
			needsFlush = true;
		}
		else if(m_passWriteDepth && TextureOverlapsDepthbuffer())
		{
			if(m_statsEnabled) m_stats.flushTexDepthOverlap++;
			needsFlush = true;
		}
		else if(TextureOverlapsFramebuffer())
		{
			if(m_statsEnabled) m_stats.drawTexOverlapsFb++;
			needsFlush = true;
		}
		if(needsFlush)
		{
			uint32 vertexStart = m_passVertexStart;
			uint32 vertexEnd = m_passVertexEnd;
			m_passVertexEnd = m_passVertexStart;
			FlushRenderPass();
			m_passVertexStart = vertexStart;
			m_passVertexEnd = vertexEnd;
		}
	}

	auto& frame = m_frames[m_frameCommandBuffer->GetCurrentFrame()];
	if(m_pipelineCaps.textureUseDynamicMipLOD && (m_mipParamsIndex == 0) && !m_mipParamsSlot0Valid)
	{
		frame.mipParamsBufferPtr[0] = m_lastMipParams;
		m_mipParamsCpuCache[0] = m_lastMipParams;
		m_mipParamsSlot0Valid = true;
	}

	m_renderPassMinX = std::min(drawMinX, m_renderPassMinX);
	m_renderPassMinY = std::min(drawMinY, m_renderPassMinY);
	m_renderPassMaxX = std::max(drawMaxX, m_renderPassMaxX);
	m_renderPassMaxY = std::max(drawMaxY, m_renderPassMaxY);

	if(m_pipelineCaps.textureUseMemoryCopy)
	{
		assert(m_pendingDraws.empty());
		m_memoryCopyRegion.Reset();
	}

	if(m_pendingDraws.empty())
	{
		m_passLoadPushConstants = m_pushConstants;
		m_passFbFormat = CanonicalFbFormat(m_pipelineCaps.framebufferFormat);
	}

	if(m_statsEnabled)
	{
		m_stats.draws++;
		m_stats.verts += vertexCount;
		if(m_pipelineCaps.textureUseDynamicMipLOD) m_stats.drawDynMip++;
		if(m_pipelineCaps.hasTexture && m_pushConstants.texMipLevel > 0) m_stats.drawStatMip++;
		if(m_pipelineCaps.hasTexture && m_pipelineCaps.textureUseLinearFiltering) m_stats.drawLinear++;
		if(m_pipelineCaps.textureUseMemoryCopy) m_stats.drawMemCopy++;
		if(m_pipelineCaps.primitiveType == PIPELINE_PRIMITIVE_POINT) m_stats.drawPoints++;
		if(m_pipelineCaps.primitiveType == PIPELINE_PRIMITIVE_LINE) m_stats.drawLines++;
		if(m_pipelineCaps.hasAlphaBlending)
		{
			m_stats.drawAlphaBlend++;
			uint32 abCode = ((m_pipelineCaps.alphaA & 3) << 6) | ((m_pipelineCaps.alphaB & 3) << 4) |
			                ((m_pipelineCaps.alphaC & 3) << 2) | (m_pipelineCaps.alphaD & 3);
			m_stats.abModes[abCode]++;
			if(m_pipelineCaps.alphaC == CGSHandler::ALPHABLEND_C_FIX)
				m_stats.abFix++;
			else if(m_pipelineCaps.alphaA == CGSHandler::ALPHABLEND_ABD_CS &&
			        m_pipelineCaps.alphaB == CGSHandler::ALPHABLEND_ABD_CD &&
			        m_pipelineCaps.alphaC == CGSHandler::ALPHABLEND_C_AS &&
			        m_pipelineCaps.alphaD == CGSHandler::ALPHABLEND_ABD_CD)
				m_stats.abStd++;
			else if(m_pipelineCaps.alphaA == CGSHandler::ALPHABLEND_ABD_CS &&
			        m_pipelineCaps.alphaB == CGSHandler::ALPHABLEND_ABD_ZERO &&
			        m_pipelineCaps.alphaC == CGSHandler::ALPHABLEND_C_AS &&
			        m_pipelineCaps.alphaD == CGSHandler::ALPHABLEND_ABD_CD)
				m_stats.abAdd++;
			else
				m_stats.abOther++;
		}
		if(m_pipelineCaps.hasDstAlphaTest) m_stats.drawDstAlphaTest++;
		m_stats.fbPsmMask |= (1ULL << m_pipelineCaps.framebufferFormat);
		m_stats.zbPsmMask |= (1ULL << m_pipelineCaps.depthbufferFormat);
		if(m_pipelineCaps.hasTexture) m_stats.texPsmMask |= (1ULL << m_pipelineCaps.textureFormat);
	}

	//The draw fragment shader does not depend on depthbufferFormat (load/store passes handle it),
	//so repurpose those bits in the draw pipeline key to carry the 4-bit Vulkan colorWriteMask.
	bool isPsm24 = (m_pipelineCaps.framebufferFormat == CGSHandler::PSMCT24);
	auto drawPipelineCaps = m_pipelineCaps;
	drawPipelineCaps.framebufferFormat = m_passFbFormat;
	if(isPsm24 && !m_pipelineCaps.maskColor)
	{
		promotedMaskToHw = true;
		hwColorMask = 0x7;
	}
	if(promotedMaskToHw)
	{
		drawPipelineCaps.maskColor = 0;
		drawPipelineCaps.depthbufferFormat = hwColorMask;
		if((hwColorMask & 0x7) == 0)
		{
			//RGB write is completely masked off; PS2 alpha blending only affects RGB.
			drawPipelineCaps.hasAlphaBlending = 0;
		}
	}
	else
	{
		drawPipelineCaps.depthbufferFormat = 0xF;
	}

	auto descriptorSetCaps = make_convertible<DESCRIPTORSET_CAPS>(0);
	descriptorSetCaps.hasTexture = m_pipelineCaps.hasTexture;
	descriptorSetCaps.framebufferFormat = m_passFbFormat;
	descriptorSetCaps.depthbufferFormat = CGSHandler::PSMZ32;
	descriptorSetCaps.textureFormat = m_pipelineCaps.textureFormat;
	descriptorSetCaps.textureUseDynamicMipLOD = m_pipelineCaps.textureUseDynamicMipLOD;
	descriptorSetCaps.frameNumber = m_pipelineCaps.textureUseDynamicMipLOD ? m_frameCommandBuffer->GetCurrentFrame() : 0;

	bool drawUsesDepth = (m_pipelineCaps.depthTestFunction != CGSHandler::DEPTH_TEST_ALWAYS) || (m_pipelineCaps.writeDepth != 0);
	if(drawUsesDepth && !m_passHasDepth)
	{
		m_passHasDepth = true;
		m_passDepthFormat = m_pipelineCaps.depthbufferFormat;
		m_passDepthBufAddr = m_pushConstants.depthBufAddr;
		m_passDepthBufWidth = m_pushConstants.depthBufWidth;
		m_passLoadPushConstants.depthBufAddr = m_passDepthBufAddr;
		m_passLoadPushConstants.depthBufWidth = m_passDepthBufWidth;
	}
	m_passWriteDepth |= m_pipelineCaps.writeDepth;
	m_passReadDepth |= (m_pipelineCaps.depthTestFunction != CGSHandler::DEPTH_TEST_ALWAYS);

	PENDING_DRAW draw;
	draw.drawPipelineCaps = drawPipelineCaps;
	draw.descriptorSetCaps = descriptorSetCaps;
	draw.hasTexture = m_pipelineCaps.hasTexture != 0;
	draw.textureFormat = m_pipelineCaps.textureFormat;
	if(draw.hasTexture)
	{
		uint32 effWidth = std::max(m_pushConstants.texBufWidth, m_pushConstants.texWidth);
		draw.texRange = GetBufferRange(m_pushConstants.texBufAddr, effWidth, draw.textureFormat, m_pushConstants.texHeight);
	}
	draw.hasClutOffset = m_pipelineCaps.hasTexture && CGsPixelFormats::IsPsmIDTEX(m_pipelineCaps.textureFormat);
	draw.clutOffset = m_clutBufferOffset;
	draw.hasMipParams = m_pipelineCaps.hasTexture && m_pipelineCaps.textureUseDynamicMipLOD;
	draw.mipParamsIndex = m_mipParamsIndex;
	auto hwBlend = GetHardwareBlendConfig(drawPipelineCaps, m_context->hasDualSrcBlend);
	bool writesFragDepth = WritesFragDepth(drawPipelineCaps);
	bool readsColor = UsesSoftwareColorRead(drawPipelineCaps, hwBlend) || writesFragDepth;
	draw.readsColor = readsColor;
	draw.readsDepth = writesFragDepth;
	draw.usesConstColor = !readsColor && hwBlend.usesConstColor;
	m_passNeedsColorFeedback |= readsColor;
	m_passNeedsDepthFeedback |= writesFragDepth;
	m_passHasDualSrc |= (!readsColor && hwBlend.dualSrc);
	draw.readReason = (!hwBlend.supported) ? 1 :
	                  (drawPipelineCaps.maskColor ? 4 : 0) | ((drawPipelineCaps.alphaTestFunction != CGSHandler::ALPHA_TEST_ALWAYS) ? 8 : 0) | 16;
	if(m_statsEnabled)
	{
		if(promotedMaskToHw) m_stats.drawHwMask++;
		if(readsColor)
		{
			m_stats.drawSwFb++;
			if(writesFragDepth) m_stats.drawSwDepth++;
		}
		else
		{
			m_stats.drawHwFast++;
			if(hwBlend.dualSrc) m_stats.drawHwDual++;
		}
	}
	draw.minX = static_cast<int32>(std::floor(drawMinX));
	draw.minY = static_cast<int32>(std::floor(drawMinY));
	draw.maxX = static_cast<int32>(std::ceil(drawMaxX + 0.5f));
	draw.maxY = static_cast<int32>(std::ceil(drawMaxY + 0.5f));
	draw.vertexStart = m_passVertexStart;
	draw.vertexCount = vertexCount;
	draw.pushConstants = m_pushConstants;
	//Draw fragment shaders never read fbBufAddr/Width or depthBufAddr/Width (only load/store passes do);
	//zero them and any inactive state so push-constant diffing and draw merging match maximally.
	draw.pushConstants.fbBufAddr = 0;
	draw.pushConstants.fbBufWidth = 0;
	draw.pushConstants.depthBufAddr = 0;
	draw.pushConstants.depthBufWidth = 0;
	if(!drawPipelineCaps.hasTexture)
	{
		draw.pushConstants.texBufAddr = 0;
		draw.pushConstants.texBufWidth = 0;
		draw.pushConstants.texWidth = 0;
		draw.pushConstants.texHeight = 0;
		draw.pushConstants.texMipLevel = 0;
		draw.pushConstants.texCsa = 0;
		draw.pushConstants.texA0 = 0;
		draw.pushConstants.texA1 = 0;
		draw.pushConstants.clampMin[0] = 0;
		draw.pushConstants.clampMin[1] = 0;
		draw.pushConstants.clampMax[0] = 0;
		draw.pushConstants.clampMax[1] = 0;
	}
	else
	{
		if(!CGsPixelFormats::IsPsmIDTEX4(drawPipelineCaps.textureFormat))
		{
			draw.pushConstants.texCsa = 0;
		}
		bool usesTexAlpha = false;
		if(drawPipelineCaps.textureHasAlpha)
		{
			if(CGsPixelFormats::IsPsmIDTEX(drawPipelineCaps.textureFormat))
			{
				usesTexAlpha = (drawPipelineCaps.clutFormat == CGSHandler::PSMCT16) ||
				               (drawPipelineCaps.clutFormat == CGSHandler::PSMCT16S);
			}
			else
			{
				usesTexAlpha = (drawPipelineCaps.textureFormat == CGSHandler::PSMCT24) ||
				               (drawPipelineCaps.textureFormat == CGSHandler::PSMCT16) ||
				               (drawPipelineCaps.textureFormat == CGSHandler::PSMCT16S) ||
				               (drawPipelineCaps.textureFormat == CGSHandler::PSMZ16) ||
				               (drawPipelineCaps.textureFormat == CGSHandler::PSMZ16S);
			}
		}
		if(!usesTexAlpha)
		{
			draw.pushConstants.texA0 = 0;
			draw.pushConstants.texA1 = 0;
		}
		if(drawPipelineCaps.texClampU < CGSHandler::CLAMP_MODE_REGION_CLAMP)
		{
			draw.pushConstants.clampMin[0] = 0;
			draw.pushConstants.clampMax[0] = 0;
		}
		if(drawPipelineCaps.texClampV < CGSHandler::CLAMP_MODE_REGION_CLAMP)
		{
			draw.pushConstants.clampMin[1] = 0;
			draw.pushConstants.clampMax[1] = 0;
		}
	}
	if(!drawPipelineCaps.hasFog)
	{
		draw.pushConstants.fogColor[0] = 0.0f;
		draw.pushConstants.fogColor[1] = 0.0f;
		draw.pushConstants.fogColor[2] = 0.0f;
		draw.pushConstants.fogColor[3] = 0.0f;
	}
	if(!drawPipelineCaps.hasAlphaBlending || (drawPipelineCaps.alphaC != CGSHandler::ALPHABLEND_C_FIX))
	{
		draw.pushConstants.alphaFix = 0;
	}
	if(drawPipelineCaps.alphaTestFunction == CGSHandler::ALPHA_TEST_ALWAYS)
	{
		draw.pushConstants.alphaRef = 0;
	}
	if(promotedMaskToHw || !drawPipelineCaps.maskColor)
	{
		//Hardware colorWriteMask (or unmasked write) handles masking; normalize so pushConstants memcmp & draw merging match.
		draw.pushConstants.fbWriteMask = 0xFFFFFFFFU;
	}
	if(!m_pendingDraws.empty() && !draw.readsColor && !draw.readsDepth)
	{
		auto drawCapsInt = static_cast<uint64>(draw.drawPipelineCaps);
		auto drawDescInt = static_cast<DescriptorSetCapsInt>(draw.descriptorSetCaps);
		int bestPipeIdx = -1;
		int count = static_cast<int>(m_pendingDraws.size());
		int minK = std::max(0, count - 64);
		for(int k = count - 1; k >= minK; k--)
		{
			auto& cand = m_pendingDraws[k];
			if(!cand.readsColor && !cand.readsDepth &&
			   (static_cast<uint64>(cand.drawPipelineCaps) == drawCapsInt))
			{
				if((static_cast<DescriptorSetCapsInt>(cand.descriptorSetCaps) == drawDescInt) &&
				   (cand.hasClutOffset == draw.hasClutOffset) && (!draw.hasClutOffset || (cand.clutOffset == draw.clutOffset)) &&
				   (cand.hasMipParams == draw.hasMipParams) && (!draw.hasMipParams || (cand.mipParamsIndex == draw.mipParamsIndex)) &&
				   (memcmp(&cand.pushConstants, &draw.pushConstants, sizeof(DRAW_PIPELINE_PUSHCONSTANTS)) == 0))
				{
					if(cand.sliceCount == 1)
					{
						if(cand.vertexStart + cand.vertexCount == draw.vertexStart)
						{
							cand.vertexCount += draw.vertexCount;
						}
						else
						{
							uint32 s0 = static_cast<uint32>(m_pendingSlices.size());
							uint32 s1 = s0 + 1;
							m_pendingSlices.push_back({cand.vertexStart, cand.vertexCount, s1});
							m_pendingSlices.push_back({draw.vertexStart, draw.vertexCount, ~0U});
							cand.firstSlice = s0;
							cand.lastSlice = s1;
							cand.sliceCount = 2;
							cand.vertexCount += draw.vertexCount;
						}
					}
					else
					{
						auto& tail = m_pendingSlices[cand.lastSlice];
						if(tail.start + tail.count == draw.vertexStart)
						{
							tail.count += draw.vertexCount;
						}
						else
						{
							uint32 sNew = static_cast<uint32>(m_pendingSlices.size());
							m_pendingSlices.push_back({draw.vertexStart, draw.vertexCount, ~0U});
							m_pendingSlices[cand.lastSlice].next = sNew;
							cand.lastSlice = sNew;
							cand.sliceCount++;
						}
						cand.vertexCount += draw.vertexCount;
					}
					cand.minX = std::min(cand.minX, draw.minX);
					cand.minY = std::min(cand.minY, draw.minY);
					cand.maxX = std::max(cand.maxX, draw.maxX);
					cand.maxY = std::max(cand.maxY, draw.maxY);
					m_passVertexStart = m_passVertexEnd;
					return;
				}
				if(bestPipeIdx < 0)
				{
					bestPipeIdx = k;
				}
			}
			if(!CanDrawsCommute(draw, cand))
			{
				break;
			}
		}

		if(bestPipeIdx >= 0 && bestPipeIdx < (count - 1))
		{
			m_pendingDraws.insert(m_pendingDraws.begin() + (bestPipeIdx + 1), draw);
			m_passVertexStart = m_passVertexEnd;
			return;
		}
	}
	if(!m_pendingDraws.empty() && m_statsEnabled)
	{
		const auto& prev = m_pendingDraws.back();
		uint64 xorCaps = static_cast<uint64>(prev.drawPipelineCaps) ^ static_cast<uint64>(draw.drawPipelineCaps);
		if(xorCaps != 0)
		{
			m_stats.dmPipe++;
			uint32 h = static_cast<uint32>((xorCaps * 0x9E3779B97F4A7C15ULL) >> 58);
			for(int step = 0; step < 64; step++)
			{
				uint32 k = (h + step) & 63U;
				if(m_stats.dmXorKeys[k] == xorCaps || m_stats.dmXorCnts[k] == 0)
				{
					m_stats.dmXorKeys[k] = xorCaps;
					m_stats.dmXorCnts[k]++;
					break;
				}
			}
		}
		else if((static_cast<DescriptorSetCapsInt>(prev.descriptorSetCaps) != static_cast<DescriptorSetCapsInt>(draw.descriptorSetCaps)) ||
		        (prev.hasClutOffset != draw.hasClutOffset) || (draw.hasClutOffset && (prev.clutOffset != draw.clutOffset)) ||
		        (prev.hasMipParams != draw.hasMipParams) || (draw.hasMipParams && (prev.mipParamsIndex != draw.mipParamsIndex)))
			m_stats.dmDesc++;
		else if((prev.pushConstants.texBufAddr != draw.pushConstants.texBufAddr) ||
		        (prev.pushConstants.texBufWidth != draw.pushConstants.texBufWidth) ||
		        (prev.pushConstants.texWidth != draw.pushConstants.texWidth) ||
		        (prev.pushConstants.texHeight != draw.pushConstants.texHeight) ||
		        (prev.pushConstants.texMipLevel != draw.pushConstants.texMipLevel) ||
		        (prev.pushConstants.texCsa != draw.pushConstants.texCsa))
			m_stats.dmTex++;
		else
			m_stats.dmOth++;
	}
	m_pendingDraws.push_back(draw);

	m_passVertexStart = m_passVertexEnd;
}

bool CDrawMobile::CanDrawsCommute(const PENDING_DRAW& a, const PENDING_DRAW& b)
{
	if(a.readsColor || a.readsDepth || b.readsColor || b.readsDepth)
	{
		return false;
	}
	if((a.maxX <= b.minX) || (b.maxX <= a.minX) || (a.maxY <= b.minY) || (b.maxY <= a.minY))
	{
		return true;
	}
	auto isPureAdditiveRgb = [](const PENDING_DRAW& d) {
		const auto& c = d.drawPipelineCaps;
		uint8 cmask = c.depthbufferFormat & 0xF;
		return !c.writeDepth && !c.maskColor && (cmask == 0x7) &&
		       c.hasAlphaBlending &&
		       (c.alphaA == CGSHandler::ALPHABLEND_ABD_CS) &&
		       (c.alphaB == CGSHandler::ALPHABLEND_ABD_ZERO) &&
		       (c.alphaD == CGSHandler::ALPHABLEND_ABD_CD);
	};
	return isPureAdditiveRgb(a) && isPureAdditiveRgb(b);
}

void CDrawMobile::PostFlushFrameCommandBuffer()
{
	CDraw::PostFlushFrameCommandBuffer();
	m_indexBufferCursor = 0;
}

void CDrawMobile::FlushRenderPass()
{
	FlushVertices();
	if(!m_pendingDraws.empty())
	{
		//Only the area touched by the draws is loaded, rendered and stored: pixels outside of it
		//keep what memory holds. The vertex shader offsets positions by +0.5, so ceiling(max + 0.5)
		//covers every rasterized pixel including points and lines.
		int32 clippedX0 = std::clamp<int32>(static_cast<int32>(std::floor(m_renderPassMinX)), m_scissorX, m_scissorX + m_scissorWidth);
		int32 clippedX1 = std::clamp<int32>(static_cast<int32>(std::ceil(m_renderPassMaxX + 0.5f)), m_scissorX, m_scissorX + m_scissorWidth);
		int32 clippedY0 = std::clamp<int32>(static_cast<int32>(std::floor(m_renderPassMinY)), m_scissorY, m_scissorY + m_scissorHeight);
		int32 clippedY1 = std::clamp<int32>(static_cast<int32>(std::ceil(m_renderPassMaxY + 0.5f)), m_scissorY, m_scissorY + m_scissorHeight);
		clippedX0 = std::min<int32>(clippedX0, DRAW_AREA_SIZE);
		clippedY0 = std::min<int32>(clippedY0, DRAW_AREA_SIZE);
		clippedX1 = std::min<int32>(clippedX1, DRAW_AREA_SIZE);
		clippedY1 = std::min<int32>(clippedY1, DRAW_AREA_SIZE);

		if((clippedX1 > clippedX0) && (clippedY1 > clippedY0))
		{
			RecordRenderPass(clippedX0, clippedY0, clippedX1 - clippedX0, clippedY1 - clippedY0);
		}
		m_pendingDraws.clear();
		m_pendingSlices.clear();
	}
	m_passWriteDepth = false;
	m_passReadDepth = false;
	m_passHasDepth = false;
	m_passNeedsColorFeedback = false;
	m_passNeedsDepthFeedback = false;
	m_passHasDualSrc = false;
	m_renderPassMinX = FLT_MAX;
	m_renderPassMinY = FLT_MAX;
	m_renderPassMaxX = -FLT_MAX;
	m_renderPassMaxY = -FLT_MAX;
}

bool CDrawMobile::CanLoadClutDuringPass(uint32 clutBufferOffset, uint32 address, uint32 size)
{
	FlushVertices();
	if(m_pendingDraws.empty()) return true;

	for(const auto& draw : m_pendingDraws)
	{
		if(draw.hasClutOffset && (draw.clutOffset == clutBufferOffset)) return false;
	}

	//The CLUT must not come from memory the pass stores to: the load would read it before the store.
	uint32 maxY = std::clamp<int32>(static_cast<int32>(std::ceil(m_renderPassMaxY + 0.5f)), 0, static_cast<int32>(m_scissorY + m_scissorHeight));
	std::pair<uint64, uint64> clutRange = {address, static_cast<uint64>(address) + size};

	auto fbRange = GetBufferRange(m_pushConstants.fbBufAddr, m_pushConstants.fbBufWidth, m_pipelineCaps.framebufferFormat, maxY);
	if(RangesOverlap(clutRange, fbRange)) return false;

	if(m_passWriteDepth)
	{
		auto depthRange = GetBufferRange(m_passDepthBufAddr, m_passDepthBufWidth, m_passDepthFormat, maxY);
		if(RangesOverlap(clutRange, depthRange)) return false;
	}
	return true;
}

bool CDrawMobile::CanHostTransferDuringPass(uint32 address, uint32 size)
{
	FlushVertices();
	if(m_pendingDraws.empty()) return true;

	std::pair<uint64, uint64> xferRange = {address, static_cast<uint64>(address) + size};
	uint32 maxY = std::clamp<int32>(static_cast<int32>(std::ceil(m_renderPassMaxY + 0.5f)), 0, static_cast<int32>(m_scissorY + m_scissorHeight));

	auto fbRange = GetBufferRange(m_pushConstants.fbBufAddr, m_pushConstants.fbBufWidth, m_pipelineCaps.framebufferFormat, maxY);
	if(RangesOverlap(xferRange, fbRange)) return false;

	if(m_passHasDepth)
	{
		auto depthRange = GetBufferRange(m_passDepthBufAddr, m_passDepthBufWidth, m_passDepthFormat, maxY);
		if(RangesOverlap(xferRange, depthRange)) return false;
	}

	for(const auto& draw : m_pendingDraws)
	{
		if(!draw.hasTexture) continue;
		if(RangesOverlap(xferRange, draw.texRange)) return false;
		if(draw.hasMipParams && (draw.mipParamsIndex < MAX_MIPPARAMS_COUNT))
		{
			const auto& mipParams = m_mipParamsCpuCache[draw.mipParamsIndex];
			uint32 mipHeight = draw.pushConstants.texHeight;
			for(uint32 level = 1; level <= mipParams.maxMip && level <= 6; level++)
			{
				mipHeight = std::max<uint32>(mipHeight >> 1, 1);
				auto mipRange = GetBufferRange(mipParams.mipBufs[level - 1].first, mipParams.mipBufs[level - 1].second, draw.textureFormat, mipHeight);
				if(RangesOverlap(xferRange, mipRange)) return false;
			}
		}
	}
	return true;
}

void CDrawMobile::TransitionColorImage(VkCommandBuffer cmdBuf, RESIDENT_COLOR_SLOT& slot, VkImageLayout newLayout)
{
	if(slot.currentLayout == newLayout) return;

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = slot.currentLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = slot.image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

	switch(slot.currentLayout)
	{
	case VK_IMAGE_LAYOUT_UNDEFINED:
		barrier.srcAccessMask = 0;
		srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		break;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_GENERAL:
	default:
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	}

	switch(newLayout)
	{
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		barrier.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
		dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_GENERAL:
	default:
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
		dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	}

	m_context->device.vkCmdPipelineBarrier(cmdBuf, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	slot.currentLayout = newLayout;
}

void CDrawMobile::TransitionDepthImage(VkCommandBuffer cmdBuf, RESIDENT_DEPTH_STATE& slot, VkImageLayout newLayout)
{
	if(slot.currentLayout == newLayout) return;

	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = slot.currentLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = slot.image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

	VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

	switch(slot.currentLayout)
	{
	case VK_IMAGE_LAYOUT_UNDEFINED:
		barrier.srcAccessMask = 0;
		srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
		barrier.srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_GENERAL:
	default:
		barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	}

	switch(newLayout)
	{
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
		barrier.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
		dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_GENERAL:
	default:
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
		dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	}

	m_context->device.vkCmdPipelineBarrier(cmdBuf, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	slot.currentLayout = newLayout;
}

void CDrawMobile::EmitMemoryStoreBarrier(VkCommandBuffer cmdBuf)
{
	auto memoryBarrier = Framework::Vulkan::MemoryBarrier();
	memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	m_context->device.vkCmdPipelineBarrier(cmdBuf,
	                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
	                                       0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
}

void CDrawMobile::LoadColorSlotFromMemory(uint32 slotIdx, int32 x0, int32 y0, int32 x1, int32 y1, bool preserve)
{
	auto& slot = m_residentColors[slotIdx];
	if(m_statsEnabled) m_stats.resColorLoads++;

	auto cmdBuf = m_frameCommandBuffer->GetCommandBuffer();
	m_context->ResetHostXferBatch();

	if(preserve)
	{
		TransitionColorImage(cmdBuf, slot, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	}

	VkRect2D area = {};
	area.offset.x = x0;
	area.offset.y = y0;
	area.extent.width = static_cast<uint32>(x1 - x0);
	area.extent.height = static_cast<uint32>(y1 - y0);

	VkViewport viewport = {};
	viewport.width = DRAW_AREA_SIZE;
	viewport.height = DRAW_AREA_SIZE;
	viewport.maxDepth = 1.0f;
	m_context->device.vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
	m_context->device.vkCmdSetScissor(cmdBuf, 0, 1, &area);

	auto renderPassBeginInfo = Framework::Vulkan::RenderPassBeginInfo();
	renderPassBeginInfo.renderPass = preserve ? m_rpLoadColorPreserve : m_rpLoadColor;
	renderPassBeginInfo.renderArea = area;
	renderPassBeginInfo.framebuffer = slot.loadFb;
	m_context->device.vkCmdBeginRenderPass(cmdBuf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

	PipelineCapsInt key = static_cast<PipelineCapsInt>(slot.psm);
	auto loadPipeline = m_loadColorPipelineCache.TryGetPipeline(key);
	if(!loadPipeline)
	{
		loadPipeline = m_loadColorPipelineCache.RegisterPipeline(key, CreateLoadColorPipeline(slot.psm));
	}

	auto descCaps = make_convertible<DESCRIPTORSET_CAPS>(0);
	descCaps.framebufferFormat = slot.psm;
	descCaps.depthbufferFormat = CGSHandler::PSMZ32;
	descCaps.colorSlot = slotIdx;
	auto descSet = PrepareDescriptorSet(loadPipeline->descriptorSetLayout, descCaps);

	DRAW_PIPELINE_PUSHCONSTANTS pc = {};
	pc.fbBufAddr = slot.addr;
	pc.fbBufWidth = slot.width;

	m_context->device.vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, loadPipeline->pipelineLayout, 0, 1, &descSet, 0, nullptr);
	m_context->device.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, loadPipeline->pipeline);
	m_context->device.vkCmdPushConstants(cmdBuf, loadPipeline->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	m_context->device.vkCmdDraw(cmdBuf, 3, 1, 0, 0);
	m_context->device.vkCmdEndRenderPass(cmdBuf);

	slot.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

void CDrawMobile::FlushColorSlotToMemory(uint32 slotIdx)
{
	auto& slot = m_residentColors[slotIdx];
	if(!slot.valid || !slot.dirty) return;
	if((slot.dirtyX1 <= slot.dirtyX0) || (slot.dirtyY1 <= slot.dirtyY0))
	{
		slot.dirty = false;
		return;
	}

	if(m_statsEnabled) m_stats.resColorStores++;

	auto cmdBuf = m_frameCommandBuffer->GetCommandBuffer();
	m_context->ResetHostXferBatch();

	TransitionColorImage(cmdBuf, slot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	VkRect2D area = {};
	area.offset.x = slot.dirtyX0;
	area.offset.y = slot.dirtyY0;
	area.extent.width = static_cast<uint32>(slot.dirtyX1 - slot.dirtyX0);
	area.extent.height = static_cast<uint32>(slot.dirtyY1 - slot.dirtyY0);

	VkViewport viewport = {};
	viewport.width = DRAW_AREA_SIZE;
	viewport.height = DRAW_AREA_SIZE;
	viewport.maxDepth = 1.0f;
	m_context->device.vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
	m_context->device.vkCmdSetScissor(cmdBuf, 0, 1, &area);

	auto renderPassBeginInfo = Framework::Vulkan::RenderPassBeginInfo();
	renderPassBeginInfo.renderPass = m_rpStoreColor;
	renderPassBeginInfo.renderArea = area;
	renderPassBeginInfo.framebuffer = slot.storeFb;
	m_context->device.vkCmdBeginRenderPass(cmdBuf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

	PipelineCapsInt key = static_cast<PipelineCapsInt>(slot.psm);
	auto storePipeline = m_storeColorPipelineCache.TryGetPipeline(key);
	if(!storePipeline)
	{
		storePipeline = m_storeColorPipelineCache.RegisterPipeline(key, CreateStoreColorPipeline(slot.psm));
	}

	auto descCaps = make_convertible<DESCRIPTORSET_CAPS>(0);
	descCaps.framebufferFormat = slot.psm;
	descCaps.depthbufferFormat = CGSHandler::PSMZ32;
	descCaps.colorSlot = slotIdx;
	descCaps.inputLayoutGeneral = 0;
	auto descSet = PrepareDescriptorSet(storePipeline->descriptorSetLayout, descCaps);

	DRAW_PIPELINE_PUSHCONSTANTS pc = {};
	pc.fbBufAddr = slot.addr;
	pc.fbBufWidth = slot.width;

	m_context->device.vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, storePipeline->pipelineLayout, 0, 1, &descSet, 0, nullptr);
	m_context->device.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, storePipeline->pipeline);
	m_context->device.vkCmdPushConstants(cmdBuf, storePipeline->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	m_context->device.vkCmdDraw(cmdBuf, 3, 1, 0, 0);
	m_context->device.vkCmdEndRenderPass(cmdBuf);

	slot.currentLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	slot.dirty = false;
	slot.dirtyX0 = slot.dirtyY0 = DRAW_AREA_SIZE;
	slot.dirtyX1 = slot.dirtyY1 = 0;
	EmitMemoryStoreBarrier(cmdBuf);
}

void CDrawMobile::LoadDepthFromMemory(uint32 depthSlotIdx, int32 x0, int32 y0, int32 x1, int32 y1, bool preserve)
{
	auto& dslot = m_residentDepths[depthSlotIdx];
	if(m_statsEnabled) m_stats.resDepthLoads++;

	auto cmdBuf = m_frameCommandBuffer->GetCommandBuffer();
	m_context->ResetHostXferBatch();

	if(preserve)
	{
		TransitionDepthImage(cmdBuf, dslot, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}

	VkRect2D area = {};
	area.offset.x = x0;
	area.offset.y = y0;
	area.extent.width = static_cast<uint32>(x1 - x0);
	area.extent.height = static_cast<uint32>(y1 - y0);

	VkViewport viewport = {};
	viewport.width = DRAW_AREA_SIZE;
	viewport.height = DRAW_AREA_SIZE;
	viewport.maxDepth = 1.0f;
	m_context->device.vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
	m_context->device.vkCmdSetScissor(cmdBuf, 0, 1, &area);

	auto renderPassBeginInfo = Framework::Vulkan::RenderPassBeginInfo();
	renderPassBeginInfo.renderPass = preserve ? m_rpLoadDepthPreserve : m_rpLoadDepth;
	renderPassBeginInfo.renderArea = area;
	renderPassBeginInfo.framebuffer = dslot.loadFb;
	m_context->device.vkCmdBeginRenderPass(cmdBuf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

	PipelineCapsInt key = static_cast<PipelineCapsInt>(dslot.psm);
	auto loadPipeline = m_loadDepthPipelineCache.TryGetPipeline(key);
	if(!loadPipeline)
	{
		loadPipeline = m_loadDepthPipelineCache.RegisterPipeline(key, CreateLoadDepthPipeline(dslot.psm));
	}

	auto descCaps = make_convertible<DESCRIPTORSET_CAPS>(0);
	descCaps.framebufferFormat = CGSHandler::PSMCT32;
	descCaps.depthbufferFormat = dslot.psm;
	descCaps.colorSlot = 0;
	descCaps.depthSlot = depthSlotIdx;
	auto descSet = PrepareDescriptorSet(loadPipeline->descriptorSetLayout, descCaps);

	DRAW_PIPELINE_PUSHCONSTANTS pc = {};
	pc.depthBufAddr = dslot.addr;
	pc.depthBufWidth = dslot.width;

	m_context->device.vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, loadPipeline->pipelineLayout, 0, 1, &descSet, 0, nullptr);
	m_context->device.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, loadPipeline->pipeline);
	m_context->device.vkCmdPushConstants(cmdBuf, loadPipeline->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	m_context->device.vkCmdDraw(cmdBuf, 3, 1, 0, 0);
	m_context->device.vkCmdEndRenderPass(cmdBuf);

	dslot.currentLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

void CDrawMobile::FlushDepthToMemory(uint32 depthSlotIdx)
{
	auto& dslot = m_residentDepths[depthSlotIdx];
	if(!dslot.valid || !dslot.dirty) return;
	if((dslot.dirtyX1 <= dslot.dirtyX0) || (dslot.dirtyY1 <= dslot.dirtyY0))
	{
		dslot.dirty = false;
		return;
	}

	if(m_statsEnabled) m_stats.resDepthStores++;

	auto cmdBuf = m_frameCommandBuffer->GetCommandBuffer();
	m_context->ResetHostXferBatch();

	TransitionDepthImage(cmdBuf, dslot, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

	VkRect2D area = {};
	area.offset.x = dslot.dirtyX0;
	area.offset.y = dslot.dirtyY0;
	area.extent.width = static_cast<uint32>(dslot.dirtyX1 - dslot.dirtyX0);
	area.extent.height = static_cast<uint32>(dslot.dirtyY1 - dslot.dirtyY0);

	VkViewport viewport = {};
	viewport.width = DRAW_AREA_SIZE;
	viewport.height = DRAW_AREA_SIZE;
	viewport.maxDepth = 1.0f;
	m_context->device.vkCmdSetViewport(cmdBuf, 0, 1, &viewport);
	m_context->device.vkCmdSetScissor(cmdBuf, 0, 1, &area);

	auto renderPassBeginInfo = Framework::Vulkan::RenderPassBeginInfo();
	renderPassBeginInfo.renderPass = m_rpStoreDepth;
	renderPassBeginInfo.renderArea = area;
	renderPassBeginInfo.framebuffer = dslot.storeFb;
	m_context->device.vkCmdBeginRenderPass(cmdBuf, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

	PipelineCapsInt key = static_cast<PipelineCapsInt>(dslot.psm);
	auto storePipeline = m_storeDepthPipelineCache.TryGetPipeline(key);
	if(!storePipeline)
	{
		storePipeline = m_storeDepthPipelineCache.RegisterPipeline(key, CreateStoreDepthPipeline(dslot.psm));
	}

	auto descCaps = make_convertible<DESCRIPTORSET_CAPS>(0);
	descCaps.framebufferFormat = CGSHandler::PSMCT32;
	descCaps.depthbufferFormat = dslot.psm;
	descCaps.colorSlot = 0;
	descCaps.depthSlot = depthSlotIdx;
	descCaps.inputLayoutGeneral = 0;
	auto descSet = PrepareDescriptorSet(storePipeline->descriptorSetLayout, descCaps);

	DRAW_PIPELINE_PUSHCONSTANTS pc = {};
	pc.depthBufAddr = dslot.addr;
	pc.depthBufWidth = dslot.width;

	m_context->device.vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, storePipeline->pipelineLayout, 0, 1, &descSet, 0, nullptr);
	m_context->device.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, storePipeline->pipeline);
	m_context->device.vkCmdPushConstants(cmdBuf, storePipeline->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	m_context->device.vkCmdDraw(cmdBuf, 3, 1, 0, 0);
	m_context->device.vkCmdEndRenderPass(cmdBuf);

	dslot.currentLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	dslot.dirty = false;
	dslot.dirtyX0 = dslot.dirtyY0 = DRAW_AREA_SIZE;
	dslot.dirtyX1 = dslot.dirtyY1 = 0;
	EmitMemoryStoreBarrier(cmdBuf);
}

uint32 CDrawMobile::AcquireColorSlot(uint32 addr, uint32 width, uint32 psm, int32 x0, int32 y0, int32 x1, int32 y1, bool skipColorLoad)
{
	uint32 canonPsm = CanonicalFbFormat(psm);

	//1. Check if an existing resident slot matches (addr, width, canonPsm)
	for(uint32 s = 0; s < RESIDENT_COLOR_SLOTS; s++)
	{
		auto& slot = m_residentColors[s];
		if(slot.valid && (slot.addr == addr) && (slot.width == width) && (slot.psm == canonPsm))
		{
			slot.lastUsedSeq = ++m_residentSeq;
			if((x0 >= slot.loadedX0) && (y0 >= slot.loadedY0) && (x1 <= slot.loadedX1) && (y1 <= slot.loadedY1))
			{
				return s;
			}
			//Draw area grew beyond what was loaded into this slot: load only the uncovered border strips
			//directly into the resident image with LOAD_OP_LOAD, avoiding any flush of existing pixels.
			int32 oldX0 = slot.loadedX0, oldY0 = slot.loadedY0;
			int32 oldX1 = slot.loadedX1, oldY1 = slot.loadedY1;
			int32 newX0 = std::min(oldX0, x0);
			int32 newY0 = std::min(oldY0, y0);
			int32 newX1 = std::max(oldX1, x1);
			int32 newY1 = std::max(oldY1, y1);
			auto newRange = GetBufferRange(addr, width, canonPsm, static_cast<uint32>(newY1));
			if(newY1 > oldY1)
			{
				for(uint32 k = 0; k < RESIDENT_COLOR_SLOTS; k++)
				{
					if(k != s && m_residentColors[k].valid && RangesOverlap(newRange, m_residentColors[k].gsRange))
					{
						if(m_statsEnabled && m_residentColors[k].dirty) m_stats.resColorStoreOverlap++;
						FlushColorSlotToMemory(k);
						m_residentColors[k].valid = false;
					}
				}
				for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
				{
					if(m_residentDepths[d].valid && RangesOverlap(newRange, m_residentDepths[d].gsRange))
					{
						if(m_statsEnabled && m_residentDepths[d].dirty) m_stats.resDepthStoreOverlap++;
						FlushDepthToMemory(d);
						m_residentDepths[d].valid = false;
					}
				}
			}
			slot.loadedX0 = newX0;
			slot.loadedY0 = newY0;
			slot.loadedX1 = newX1;
			slot.loadedY1 = newY1;
			slot.gsRange = newRange;
			if(newY0 < oldY0) LoadColorSlotFromMemory(s, newX0, newY0, newX1, oldY0, true);
			if(newY1 > oldY1) LoadColorSlotFromMemory(s, newX0, oldY1, newX1, newY1, true);
			if(newX0 < oldX0) LoadColorSlotFromMemory(s, newX0, oldY0, oldX0, oldY1, true);
			if(newX1 > oldX1) LoadColorSlotFromMemory(s, oldX1, oldY0, newX1, oldY1, true);
			return s;
		}
	}

	//2. Pick an unused slot (preferring already-allocated invalid, then unallocated, then LRU eviction)
	uint32 chosen = RESIDENT_COLOR_SLOTS;
	for(uint32 s = 0; s < RESIDENT_COLOR_SLOTS; s++)
	{
		if(m_residentColors[s].allocated && !m_residentColors[s].valid)
		{
			chosen = s;
			break;
		}
	}
	if(chosen == RESIDENT_COLOR_SLOTS)
	{
		for(uint32 s = 0; s < RESIDENT_COLOR_SLOTS; s++)
		{
			if(!m_residentColors[s].allocated)
			{
				chosen = s;
				break;
			}
		}
	}
	if(chosen == RESIDENT_COLOR_SLOTS)
	{
		chosen = 0;
		for(uint32 s = 1; s < RESIDENT_COLOR_SLOTS; s++)
		{
			if(m_residentColors[s].lastUsedSeq < m_residentColors[chosen].lastUsedSeq)
			{
				chosen = s;
			}
		}
	}

	EnsureColorSlotAllocated(chosen);
	auto& slot = m_residentColors[chosen];
	if(slot.valid && slot.dirty)
	{
		if(m_statsEnabled) m_stats.resColorStoreEvict++;
		FlushColorSlotToMemory(chosen);
	}
	slot.valid = false;
	slot.dirty = false;

	//Align cold loadY1 up to the PSM page row boundary (GetBufferRange already rounds to page rows,
	//so this adds zero gsRange bytes while preventing incremental vertical border-strip reloads).
	int32 specX1 = std::min<int32>(m_scissorX + m_scissorWidth, std::max<int32>(x1, static_cast<int32>(width)));
	int32 pageH = static_cast<int32>(CGsPixelFormats::GetPsmPageSize(canonPsm).second);
	int32 alignedY1 = ((y1 + pageH - 1) / pageH) * pageH;
	int32 specY1 = std::min<int32>(m_scissorY + m_scissorHeight, alignedY1);
	int32 loadX0 = skipColorLoad ? x0 : 0;
	int32 loadY0 = skipColorLoad ? y0 : 0;
	int32 loadX1 = skipColorLoad ? x1 : std::min<int32>(DRAW_AREA_SIZE, std::max<int32>(x1, specX1));
	int32 loadY1 = skipColorLoad ? y1 : std::min<int32>(DRAW_AREA_SIZE, std::max<int32>(y1, specY1));
	if(loadX1 <= loadX0) loadX1 = x1;
	if(loadY1 <= loadY0) loadY1 = y1;

	auto newRange = GetBufferRange(addr, width, canonPsm, static_cast<uint32>(loadY1));
	for(uint32 k = 0; k < RESIDENT_COLOR_SLOTS; k++)
	{
		if(k != chosen && m_residentColors[k].valid && RangesOverlap(newRange, m_residentColors[k].gsRange))
		{
			if(m_statsEnabled && m_residentColors[k].dirty) m_stats.resColorStoreOverlap++;
			FlushColorSlotToMemory(k);
			m_residentColors[k].valid = false;
		}
	}
	for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
	{
		if(m_residentDepths[d].valid && RangesOverlap(newRange, m_residentDepths[d].gsRange))
		{
			if(m_statsEnabled && m_residentDepths[d].dirty) m_stats.resDepthStoreOverlap++;
			FlushDepthToMemory(d);
			m_residentDepths[d].valid = false;
		}
	}

	slot.addr = addr;
	slot.width = width;
	slot.psm = canonPsm;
	slot.loadedX0 = loadX0;
	slot.loadedY0 = loadY0;
	slot.loadedX1 = loadX1;
	slot.loadedY1 = loadY1;
	slot.dirtyX0 = DRAW_AREA_SIZE;
	slot.dirtyY0 = DRAW_AREA_SIZE;
	slot.dirtyX1 = 0;
	slot.dirtyY1 = 0;
	slot.gsRange = newRange;
	slot.lastUsedSeq = ++m_residentSeq;
	slot.valid = true;

	if(skipColorLoad)
	{
		auto cmdBuf = m_frameCommandBuffer->GetCommandBuffer();
		TransitionColorImage(cmdBuf, slot, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	}
	else
	{
		LoadColorSlotFromMemory(chosen, loadX0, loadY0, loadX1, loadY1);
	}
	return chosen;
}

uint32 CDrawMobile::AcquireDepthSlot(uint32 addr, uint32 width, uint32 psm, int32 x0, int32 y0, int32 x1, int32 y1, bool skipDepthLoad, uint32 activeColorSlotIdx)
{
	//1. Check if an existing resident depth slot matches (addr, width, psm)
	for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
	{
		auto& dslot = m_residentDepths[d];
		if(dslot.valid && (dslot.addr == addr) && (dslot.width == width) && (dslot.psm == psm))
		{
			dslot.lastUsedSeq = ++m_residentSeq;
			if((x0 >= dslot.loadedX0) && (y0 >= dslot.loadedY0) &&
			   (x1 <= dslot.loadedX1) && (y1 <= dslot.loadedY1))
			{
				return d;
			}
			int32 oldX0 = dslot.loadedX0, oldY0 = dslot.loadedY0;
			int32 oldX1 = dslot.loadedX1, oldY1 = dslot.loadedY1;
			int32 newX0 = std::min(oldX0, x0);
			int32 newY0 = std::min(oldY0, y0);
			int32 newX1 = std::max(oldX1, x1);
			int32 newY1 = std::max(oldY1, y1);
			auto newRange = GetBufferRange(addr, width, psm, static_cast<uint32>(newY1));
			if(newY1 > oldY1)
			{
				for(uint32 k = 0; k < RESIDENT_COLOR_SLOTS; k++)
				{
					if(k != activeColorSlotIdx && m_residentColors[k].valid && RangesOverlap(newRange, m_residentColors[k].gsRange))
					{
						if(m_statsEnabled && m_residentColors[k].dirty) m_stats.resColorStoreOverlap++;
						FlushColorSlotToMemory(k);
						m_residentColors[k].valid = false;
					}
				}
				for(uint32 o = 0; o < RESIDENT_DEPTH_SLOTS; o++)
				{
					if(o != d && m_residentDepths[o].valid && RangesOverlap(newRange, m_residentDepths[o].gsRange))
					{
						if(m_statsEnabled && m_residentDepths[o].dirty) m_stats.resDepthStoreOverlap++;
						FlushDepthToMemory(o);
						m_residentDepths[o].valid = false;
					}
				}
			}
			dslot.loadedX0 = newX0;
			dslot.loadedY0 = newY0;
			dslot.loadedX1 = newX1;
			dslot.loadedY1 = newY1;
			dslot.gsRange = newRange;
			if(newY0 < oldY0) LoadDepthFromMemory(d, newX0, newY0, newX1, oldY0, true);
			if(newY1 > oldY1) LoadDepthFromMemory(d, newX0, oldY1, newX1, newY1, true);
			if(newX0 < oldX0) LoadDepthFromMemory(d, newX0, oldY0, oldX0, oldY1, true);
			if(newX1 > oldX1) LoadDepthFromMemory(d, oldX1, oldY0, newX1, oldY1, true);
			return d;
		}
	}

	//2. Pick an unused depth slot (prefer allocated invalid, then unallocated, then LRU)
	uint32 chosen = RESIDENT_DEPTH_SLOTS;
	for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
	{
		if(m_residentDepths[d].allocated && !m_residentDepths[d].valid)
		{
			chosen = d;
			break;
		}
	}
	if(chosen == RESIDENT_DEPTH_SLOTS)
	{
		for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
		{
			if(!m_residentDepths[d].allocated)
			{
				chosen = d;
				break;
			}
		}
	}
	if(chosen == RESIDENT_DEPTH_SLOTS)
	{
		chosen = 0;
		for(uint32 d = 1; d < RESIDENT_DEPTH_SLOTS; d++)
		{
			if(m_residentDepths[d].lastUsedSeq < m_residentDepths[chosen].lastUsedSeq)
			{
				chosen = d;
			}
		}
	}

	EnsureDepthSlotAllocated(chosen);
	auto& dslot = m_residentDepths[chosen];
	if(dslot.valid && dslot.dirty)
	{
		if(m_statsEnabled) m_stats.resDepthStoreEvict++;
		FlushDepthToMemory(chosen);
	}
	dslot.valid = false;
	dslot.dirty = false;

	int32 specX1 = std::min<int32>(m_scissorX + m_scissorWidth, std::max<int32>(x1, static_cast<int32>(width)));
	int32 pageH = static_cast<int32>(CGsPixelFormats::GetPsmPageSize(psm).second);
	int32 alignedY1 = ((y1 + pageH - 1) / pageH) * pageH;
	int32 specY1 = std::min<int32>(m_scissorY + m_scissorHeight, alignedY1);
	int32 loadX0 = skipDepthLoad ? x0 : 0;
	int32 loadY0 = skipDepthLoad ? y0 : 0;
	int32 loadX1 = skipDepthLoad ? x1 : std::min<int32>(DRAW_AREA_SIZE, std::max<int32>(x1, specX1));
	int32 loadY1 = skipDepthLoad ? y1 : std::min<int32>(DRAW_AREA_SIZE, std::max<int32>(y1, specY1));
	if(loadX1 <= loadX0) loadX1 = x1;
	if(loadY1 <= loadY0) loadY1 = y1;

	auto newRange = GetBufferRange(addr, width, psm, static_cast<uint32>(loadY1));
	for(uint32 k = 0; k < RESIDENT_COLOR_SLOTS; k++)
	{
		if(k != activeColorSlotIdx && m_residentColors[k].valid && RangesOverlap(newRange, m_residentColors[k].gsRange))
		{
			if(m_statsEnabled && m_residentColors[k].dirty) m_stats.resColorStoreOverlap++;
			FlushColorSlotToMemory(k);
			m_residentColors[k].valid = false;
		}
	}
	for(uint32 o = 0; o < RESIDENT_DEPTH_SLOTS; o++)
	{
		if(o != chosen && m_residentDepths[o].valid && RangesOverlap(newRange, m_residentDepths[o].gsRange))
		{
			if(m_statsEnabled && m_residentDepths[o].dirty) m_stats.resDepthStoreOverlap++;
			FlushDepthToMemory(o);
			m_residentDepths[o].valid = false;
		}
	}

	dslot.addr = addr;
	dslot.width = width;
	dslot.psm = psm;
	dslot.loadedX0 = loadX0;
	dslot.loadedY0 = loadY0;
	dslot.loadedX1 = loadX1;
	dslot.loadedY1 = loadY1;
	dslot.dirtyX0 = DRAW_AREA_SIZE;
	dslot.dirtyY0 = DRAW_AREA_SIZE;
	dslot.dirtyX1 = 0;
	dslot.dirtyY1 = 0;
	dslot.gsRange = newRange;
	dslot.lastUsedSeq = ++m_residentSeq;
	dslot.valid = true;

	if(skipDepthLoad)
	{
		auto cmdBuf = m_frameCommandBuffer->GetCommandBuffer();
		TransitionDepthImage(cmdBuf, dslot, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}
	else
	{
		LoadDepthFromMemory(chosen, loadX0, loadY0, loadX1, loadY1);
	}
	return chosen;
}

void CDrawMobile::SyncMemoryForRead(uint32 address, uint32 size)
{
	if(size == 0) return;
	std::pair<uint64, uint64> range = {address, static_cast<uint64>(address) + size};
	for(uint32 s = 0; s < RESIDENT_COLOR_SLOTS; s++)
	{
		if(m_residentColors[s].valid && m_residentColors[s].dirty && RangesOverlap(range, m_residentColors[s].gsRange))
		{
			if(m_statsEnabled) m_stats.resColorStoreSyncR++;
			FlushColorSlotToMemory(s);
		}
	}
	for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
	{
		if(m_residentDepths[d].valid && m_residentDepths[d].dirty && RangesOverlap(range, m_residentDepths[d].gsRange))
		{
			if(m_statsEnabled) m_stats.resDepthStoreSyncR++;
			FlushDepthToMemory(d);
		}
	}
}

void CDrawMobile::SyncMemoryForWrite(uint32 address, uint32 size)
{
	if(size == 0) return;
	std::pair<uint64, uint64> range = {address, static_cast<uint64>(address) + size};
	for(uint32 s = 0; s < RESIDENT_COLOR_SLOTS; s++)
	{
		if(m_residentColors[s].valid && RangesOverlap(range, m_residentColors[s].gsRange))
		{
			if(m_residentColors[s].dirty)
			{
				if(m_statsEnabled) m_stats.resColorStoreSyncW++;
				FlushColorSlotToMemory(s);
			}
			m_residentColors[s].valid = false;
			m_residentColors[s].dirty = false;
		}
	}
	for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
	{
		if(m_residentDepths[d].valid && RangesOverlap(range, m_residentDepths[d].gsRange))
		{
			if(m_residentDepths[d].dirty)
			{
				if(m_statsEnabled) m_stats.resDepthStoreSyncW++;
				FlushDepthToMemory(d);
			}
			m_residentDepths[d].valid = false;
			m_residentDepths[d].dirty = false;
		}
	}
}

void CDrawMobile::FlushDirtyColorSlots()
{
	FlushRenderPass();
	for(uint32 s = 0; s < RESIDENT_COLOR_SLOTS; s++)
	{
		if(m_residentColors[s].valid && m_residentColors[s].dirty)
		{
			FlushColorSlotToMemory(s);
		}
	}
}

void CDrawMobile::FlushAllResidentMemory()
{
	FlushRenderPass();
	for(uint32 s = 0; s < RESIDENT_COLOR_SLOTS; s++)
	{
		if(m_residentColors[s].valid && m_residentColors[s].dirty)
		{
			FlushColorSlotToMemory(s);
		}
		m_residentColors[s].valid = false;
		m_residentColors[s].dirty = false;
	}
	for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
	{
		if(m_residentDepths[d].valid && m_residentDepths[d].dirty)
		{
			FlushDepthToMemory(d);
		}
		m_residentDepths[d].valid = false;
		m_residentDepths[d].dirty = false;
	}
}

void CDrawMobile::InvalidateAllResidentMemory()
{
	FlushRenderPass();
	for(uint32 s = 0; s < RESIDENT_COLOR_SLOTS; s++)
	{
		m_residentColors[s].valid = false;
		m_residentColors[s].dirty = false;
	}
	for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
	{
		m_residentDepths[d].valid = false;
		m_residentDepths[d].dirty = false;
	}
}

void CDrawMobile::RecordRenderPass(int32 x, int32 y, int32 width, int32 height)
{
	if(m_statsEnabled)
	{
		m_stats.passes++;
		if(m_passWriteDepth) m_stats.depthWritePasses++;
		m_stats.pixels += static_cast<uint64>(width) * static_cast<uint64>(height);
	}
	auto& frame = m_frames[m_frameCommandBuffer->GetCurrentFrame()];
	auto commandBuffer = m_frameCommandBuffer->GetCommandBuffer();

	//Ensure any dirty resident FB/depth overlapping textures sampled in this pass is flushed to memoryBuffer first.
	bool anyDirtySlot = false;
	for(uint32 s = 0; s < RESIDENT_COLOR_SLOTS; s++)
	{
		if(m_residentColors[s].valid && m_residentColors[s].dirty)
		{
			anyDirtySlot = true;
			break;
		}
	}
	if(!anyDirtySlot)
	{
		for(uint32 d = 0; d < RESIDENT_DEPTH_SLOTS; d++)
		{
			if(m_residentDepths[d].valid && m_residentDepths[d].dirty)
			{
				anyDirtySlot = true;
				break;
			}
		}
	}
	if(anyDirtySlot)
	{
		std::pair<uint64, uint64> lastTexRange = {0, 0};
		for(const auto& draw : m_pendingDraws)
		{
			if(!draw.hasTexture) continue;
			if(draw.texRange.second > draw.texRange.first && draw.texRange != lastTexRange)
			{
				SyncMemoryForRead(static_cast<uint32>(draw.texRange.first), static_cast<uint32>(draw.texRange.second - draw.texRange.first));
				lastTexRange = draw.texRange;
			}
			if(draw.hasMipParams && (draw.mipParamsIndex < MAX_MIPPARAMS_COUNT))
			{
				const auto& mipParams = m_mipParamsCpuCache[draw.mipParamsIndex];
				uint32 mipHeight = draw.pushConstants.texHeight;
				for(uint32 level = 1; level <= mipParams.maxMip && level <= 6; level++)
				{
					mipHeight = std::max<uint32>(mipHeight >> 1, 1);
					auto mipRange = GetBufferRange(mipParams.mipBufs[level - 1].first, mipParams.mipBufs[level - 1].second, draw.textureFormat, mipHeight);
					if(mipRange.second > mipRange.first)
					{
						SyncMemoryForRead(static_cast<uint32>(mipRange.first), static_cast<uint32>(mipRange.second - mipRange.first));
					}
				}
			}
		}
	}

	//Detect opaque axis-aligned quad at draw 0 covering the full pass area so cold slots can skip Load.
	bool skipColorLoad = false;
	bool skipDepthLoad = false;
	if(!m_pendingDraws.empty())
	{
		const auto& d0 = m_pendingDraws[0];
		uint8 cmask = d0.drawPipelineCaps.depthbufferFormat & 0xF;
		bool fullColor = (cmask == 0xF);
		bool fullDepth = (d0.drawPipelineCaps.writeDepth != 0) && (d0.drawPipelineCaps.depthTestFunction == CGSHandler::DEPTH_TEST_ALWAYS);
		if((d0.sliceCount == 1) && (d0.vertexCount == 6) &&
		   (d0.drawPipelineCaps.primitiveType == PIPELINE_PRIMITIVE_TRIANGLE) &&
		   !d0.drawPipelineCaps.hasAlphaBlending &&
		   (d0.drawPipelineCaps.alphaTestFunction == CGSHandler::ALPHA_TEST_ALWAYS) &&
		   !d0.readsColor && !d0.readsDepth &&
		   (d0.drawPipelineCaps.scanMask == 0) &&
		   !d0.drawPipelineCaps.maskColor &&
		   (fullColor || fullDepth))
		{
			const PRIM_VERTEX* v = frame.vertexBufferPtr + d0.vertexStart;
			float rx0 = v[0].x, rx1 = v[0].x, ry0 = v[0].y, ry1 = v[0].y;
			for(uint32 i = 1; i < 6; i++)
			{
				if(v[i].x < rx0) rx0 = v[i].x;
				if(v[i].x > rx1) rx1 = v[i].x;
				if(v[i].y < ry0) ry0 = v[i].y;
				if(v[i].y > ry1) ry1 = v[i].y;
			}
			if((rx0 <= static_cast<float>(x)) && (ry0 <= static_cast<float>(y)) &&
			   (rx1 >= static_cast<float>(x + width)) && (ry1 >= static_cast<float>(y + height)))
			{
				bool allCorners = true;
				uint32 cornerMask = 0;
				for(uint32 i = 0; i < 6; i++)
				{
					bool isX0 = (v[i].x == rx0), isX1 = (v[i].x == rx1);
					bool isY0 = (v[i].y == ry0), isY1 = (v[i].y == ry1);
					if(!(isX0 || isX1) || !(isY0 || isY1))
					{
						allCorners = false;
						break;
					}
					cornerMask |= 1u << ((isX1 ? 1u : 0u) | (isY1 ? 2u : 0u));
				}
				auto triArea2 = [](const PRIM_VERTEX& a, const PRIM_VERTEX& b, const PRIM_VERTEX& c) {
					return std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
				};
				float rectArea2 = 2.0f * (rx1 - rx0) * (ry1 - ry0);
				float sumArea2 = triArea2(v[0], v[1], v[2]) + triArea2(v[3], v[4], v[5]);
				if(allCorners && (cornerMask == 0xF) && (std::fabs(sumArea2 - rectArea2) <= 0.5f))
				{
					skipColorLoad = fullColor;
					skipDepthLoad = fullDepth;
				}
			}
		}
	}

	int32 x1 = x + width;
	int32 y1 = y + height;

	uint32 colorSlotIdx = AcquireColorSlot(m_passLoadPushConstants.fbBufAddr, m_passLoadPushConstants.fbBufWidth, m_passFbFormat, x, y, x1, y1, skipColorLoad);
	auto& colorSlot = m_residentColors[colorSlotIdx];

	uint32 depthSlotIdx = 0;
	if(m_passHasDepth)
	{
		depthSlotIdx = AcquireDepthSlot(m_passDepthBufAddr, m_passDepthBufWidth, m_passDepthFormat, x, y, x1, y1, skipDepthLoad, colorSlotIdx);
	}

	uint32 passMode = PASS_MODE_NO_DEPTH;
	if(m_passNeedsColorFeedback || m_passNeedsDepthFeedback)
	{
		passMode = m_passHasDepth ? PASS_MODE_FEEDBACK_DEPTH : PASS_MODE_FEEDBACK_NO_DEPTH;
	}
	else if(m_passHasDepth)
	{
		passMode = PASS_MODE_DEPTH;
	}

	bool isFeedback = (passMode == PASS_MODE_FEEDBACK_NO_DEPTH) || (passMode == PASS_MODE_FEEDBACK_DEPTH);
	TransitionColorImage(commandBuffer, colorSlot, isFeedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if(m_passHasDepth)
	{
		TransitionDepthImage(commandBuffer, m_residentDepths[depthSlotIdx], isFeedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	}

	m_context->ResetHostXferBatch();

	VkRect2D area = {};
	area.offset.x = x;
	area.offset.y = y;
	area.extent.width = width;
	area.extent.height = height;

#if GSH_VULKAN_USE_ANNOTATIONS
	m_context->annotations.PushCommandLabel(commandBuffer, string_format("Draw (FBP: 0x%06X, ZBP: 0x%06X)", m_passLoadPushConstants.fbBufAddr, m_passLoadPushConstants.depthBufAddr).c_str());
#endif

	{
		VkViewport viewport = {};
		viewport.width = DRAW_AREA_SIZE;
		viewport.height = DRAW_AREA_SIZE;
		viewport.maxDepth = 1.0f;
		m_context->device.vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
		m_context->device.vkCmdSetScissor(commandBuffer, 0, 1, &area);
	}

	auto renderPassBeginInfo = Framework::Vulkan::RenderPassBeginInfo();
	renderPassBeginInfo.renderPass = m_renderPasses[passMode];
	renderPassBeginInfo.renderArea = area;
	renderPassBeginInfo.framebuffer = GetDrawFramebuffer(colorSlotIdx, m_passHasDepth ? depthSlotIdx : 0, passMode);
	m_context->device.vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

	uint32 frameIdx = m_frameCommandBuffer->GetCurrentFrame();
	bool indexBufferBound = false;
	{
		VkBuffer vertexBuffer = frame.vertexBuffer;
		VkDeviceSize vertexBufferOffset = 0;
		m_context->device.vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer, &vertexBufferOffset);
	}

	//State that did not change since the previous draw is not recorded again: mobile drivers
	//spend more CPU time per command than desktop ones.
	auto& drawCache = m_drawPipelineCaches[passMode];
	const PENDING_DRAW* prevDraw = nullptr;
	const PIPELINE* prevPipeline = nullptr;
	VkDescriptorSet prevDescSet = VK_NULL_HANDLE;
	bool hasBlendConst = false;
	uint32 prevAlphaFix = 0;
	int32 accMinX = 0, accMinY = 0, accMaxX = 0, accMaxY = 0;
	bool hasUnbarriered = false;
	for(const auto& draw : m_pendingDraws)
	{
		if(hasUnbarriered && (draw.readsColor || draw.readsDepth) &&
		   (draw.minX < accMaxX) && (accMinX < draw.maxX) &&
		   (draw.minY < accMaxY) && (accMinY < draw.maxY))
		{
			if(m_statsEnabled)
			{
				m_stats.barriers++;
				if(draw.readReason == 1) m_stats.barAlpha++;
				else if(draw.readReason == 2) m_stats.barDepthOnly++;
				else
				{
					m_stats.barOther++;
					if(draw.readReason & 4) m_stats.barOtherMask++;
					if(draw.readReason & 8) m_stats.barOtherATest++;
				}
			}
			auto memoryBarrier = Framework::Vulkan::MemoryBarrier();
			VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			memoryBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			if(draw.readsDepth)
			{
				srcStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				memoryBarrier.srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			}
			memoryBarrier.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;

			m_context->device.vkCmdPipelineBarrier(commandBuffer, srcStage, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			                                       VK_DEPENDENCY_BY_REGION_BIT, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
			hasUnbarriered = false;
		}
		if(!hasUnbarriered)
		{
			accMinX = draw.minX;
			accMinY = draw.minY;
			accMaxX = draw.maxX;
			accMaxY = draw.maxY;
			hasUnbarriered = true;
		}
		else
		{
			accMinX = std::min(accMinX, draw.minX);
			accMinY = std::min(accMinY, draw.minY);
			accMaxX = std::max(accMaxX, draw.maxX);
			accMaxY = std::max(accMaxY, draw.maxY);
		}

		PipelineCapsInt drawCapsKey = static_cast<PipelineCapsInt>(draw.drawPipelineCaps);
		uint32 pipeSlot = static_cast<uint32>(((drawCapsKey ^ (static_cast<uint64>(passMode) * 0x9E3779B97F4A7C15ULL)) * 0x9E3779B97F4A7C15ULL) >> 58);
		auto& pipeEntry = m_pipelineFastCache[pipeSlot];
		const PIPELINE* drawPipeline = nullptr;
		if((pipeEntry.caps == drawCapsKey) && (pipeEntry.passMode == passMode) && pipeEntry.pipeline)
		{
			drawPipeline = pipeEntry.pipeline;
		}
		else
		{
			drawPipeline = drawCache.TryGetPipeline(draw.drawPipelineCaps);
			if(!drawPipeline)
			{
				drawPipeline = drawCache.RegisterPipeline(draw.drawPipelineCaps, CreateDrawPipeline(draw.drawPipelineCaps, passMode));
			}
			pipeEntry.caps = drawCapsKey;
			pipeEntry.passMode = passMode;
			pipeEntry.pipeline = drawPipeline;
		}

		auto descCaps = draw.descriptorSetCaps;
		descCaps.colorSlot = isFeedback ? colorSlotIdx : 0;
		descCaps.depthSlot = (isFeedback && m_passHasDepth) ? depthSlotIdx : 0;
		descCaps.inputLayoutGeneral = isFeedback ? 1 : 0;
		DescriptorSetCapsInt descKey = static_cast<DescriptorSetCapsInt>(descCaps);
		uint32 descSlot = static_cast<uint32>((descKey * 2654435761U) >> 26);
		auto& descEntry = m_descFastCache[descSlot];
		VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
		if((descEntry.caps == descKey) && (descEntry.descSet != VK_NULL_HANDLE))
		{
			descriptorSet = descEntry.descSet;
		}
		else
		{
			descriptorSet = PrepareDescriptorSet(drawPipeline->descriptorSetLayout, descCaps);
			descEntry.caps = descKey;
			descEntry.descSet = descriptorSet;
		}

		bool layoutChanged = !prevPipeline || (prevPipeline->pipelineLayout != drawPipeline->pipelineLayout);
		uint32 mipParamsOffset = draw.mipParamsIndex * sizeof(DRAW_PIPELINE_MIPPARAMS_UNIFORMS);
		uint32 prevMipParamsOffset = prevDraw ? (prevDraw->mipParamsIndex * sizeof(DRAW_PIPELINE_MIPPARAMS_UNIFORMS)) : 0;

		if(layoutChanged || (prevDescSet != descriptorSet) ||
		   (prevDraw->hasClutOffset != draw.hasClutOffset) || (prevDraw->clutOffset != draw.clutOffset) ||
		   (prevDraw->hasMipParams != draw.hasMipParams) || (prevMipParamsOffset != mipParamsOffset))
		{
			uint32 dynamicOffsets[2];
			uint32 dynamicOffsetCount = 0;
			if(draw.hasClutOffset)
			{
				dynamicOffsets[dynamicOffsetCount++] = draw.clutOffset;
			}
			if(draw.hasMipParams)
			{
				dynamicOffsets[dynamicOffsetCount++] = mipParamsOffset;
			}
			m_context->device.vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipeline->pipelineLayout,
			                                          0, 1, &descriptorSet, dynamicOffsetCount, dynamicOffsetCount ? dynamicOffsets : nullptr);
		}

		if(!prevPipeline || (prevPipeline->pipeline != drawPipeline->pipeline))
		{
			if(m_statsEnabled) m_stats.vkPipes++;
			m_context->device.vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipeline->pipeline);
		}

		if(draw.usesConstColor && (!hasBlendConst || (prevAlphaFix != draw.pushConstants.alphaFix)))
		{
			float f = static_cast<float>(draw.pushConstants.alphaFix) * (1.0f / 128.0f);
			float blendConst[4] = {f, f, f, f};
			m_context->device.vkCmdSetBlendConstants(commandBuffer, blendConst);
			hasBlendConst = true;
			prevAlphaFix = draw.pushConstants.alphaFix;
		}

		if(layoutChanged)
		{
			m_context->device.vkCmdPushConstants(commandBuffer, drawPipeline->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
			                                     0, sizeof(DRAW_PIPELINE_PUSHCONSTANTS), &draw.pushConstants);
		}
		else
		{
			constexpr uint32 kPcWords = sizeof(DRAW_PIPELINE_PUSHCONSTANTS) / sizeof(uint32);
			const auto* curW = reinterpret_cast<const uint32*>(&draw.pushConstants);
			const auto* prevW = reinterpret_cast<const uint32*>(&prevDraw->pushConstants);
			uint32 firstDiff = kPcWords;
			uint32 lastDiff = 0;
			for(uint32 w = 4; w < kPcWords; w++)
			{
				if(curW[w] != prevW[w])
				{
					if(firstDiff == kPcWords) firstDiff = w;
					lastDiff = w;
				}
			}
			if(firstDiff < kPcWords)
			{
				m_context->device.vkCmdPushConstants(commandBuffer, drawPipeline->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
				                                     firstDiff * sizeof(uint32), (lastDiff - firstDiff + 1) * sizeof(uint32), curW + firstDiff);
			}
		}

		if(draw.sliceCount == 1)
		{
			if(m_statsEnabled) m_stats.vkDraws++;
			m_context->device.vkCmdDraw(commandBuffer, draw.vertexCount, 1, draw.vertexStart, 0);
		}
		else if((m_indexBufferCursor + draw.vertexCount) <= MAX_INDEX_COUNT)
		{
			if(!indexBufferBound)
			{
				m_context->device.vkCmdBindIndexBuffer(commandBuffer, m_indexBuffers[frameIdx], 0, VK_INDEX_TYPE_UINT32);
				indexBufferBound = true;
			}
			uint32 firstIndex = m_indexBufferCursor;
			uint32* dstIdx = m_indexBufferPtrs[frameIdx] + firstIndex;
			for(uint32 s = draw.firstSlice; s != ~0U; s = m_pendingSlices[s].next)
			{
				uint32 v = m_pendingSlices[s].start;
				uint32 n = m_pendingSlices[s].count;
				uint32 i = 0;
#if defined(__aarch64__)
				static const uint32 kRamp[4] = {0, 1, 2, 3};
				uint32x4_t vCur = vaddq_u32(vdupq_n_u32(v), vld1q_u32(kRamp));
				uint32x4_t vStep = vdupq_n_u32(4);
				for(; i + 4 <= n; i += 4)
				{
					vst1q_u32(dstIdx + i, vCur);
					vCur = vaddq_u32(vCur, vStep);
				}
#endif
				for(; i < n; i++)
				{
					dstIdx[i] = v + i;
				}
				dstIdx += n;
			}
			m_indexBufferCursor += draw.vertexCount;
			if(m_statsEnabled) m_stats.vkDraws++;
			m_context->device.vkCmdDrawIndexed(commandBuffer, draw.vertexCount, 1, firstIndex, 0, 0);
		}
		else
		{
			for(uint32 s = draw.firstSlice; s != ~0U; s = m_pendingSlices[s].next)
			{
				if(m_statsEnabled) m_stats.vkDraws++;
				m_context->device.vkCmdDraw(commandBuffer, m_pendingSlices[s].count, 1, m_pendingSlices[s].start, 0);
			}
		}
		prevDraw = &draw;
		prevPipeline = drawPipeline;
		prevDescSet = descriptorSet;
	}

	m_context->device.vkCmdEndRenderPass(commandBuffer);
	m_context->annotations.PopCommandLabel(commandBuffer);

	//Update dirty bounds of the resident color slot and depth slot
	colorSlot.dirty = true;
	colorSlot.dirtyX0 = std::min(colorSlot.dirtyX0, x);
	colorSlot.dirtyY0 = std::min(colorSlot.dirtyY0, y);
	colorSlot.dirtyX1 = std::max(colorSlot.dirtyX1, x1);
	colorSlot.dirtyY1 = std::max(colorSlot.dirtyY1, y1);

	if(m_passHasDepth && m_passWriteDepth)
	{
		auto& dslot = m_residentDepths[depthSlotIdx];
		dslot.dirty = true;
		dslot.dirtyX0 = std::min(dslot.dirtyX0, x);
		dslot.dirtyY0 = std::min(dslot.dirtyY0, y);
		dslot.dirtyX1 = std::max(dslot.dirtyX1, x1);
		dslot.dirtyY1 = std::max(dslot.dirtyY1, y1);
	}

	//If FB and depth share the same GS RAM range in this pass, flush both in order and invalidate to keep aliasing exact.
	if(m_passHasDepth && RangesOverlap(colorSlot.gsRange, m_residentDepths[depthSlotIdx].gsRange))
	{
		if(m_statsEnabled && m_residentDepths[depthSlotIdx].dirty) m_stats.resDepthStoreSelf++;
		FlushColorSlotToMemory(colorSlotIdx);
		if(m_residentDepths[depthSlotIdx].dirty) FlushDepthToMemory(depthSlotIdx);
		colorSlot.valid = false;
		m_residentDepths[depthSlotIdx].valid = false;
	}
}

VkDescriptorSet CDrawMobile::PrepareDescriptorSet(VkDescriptorSetLayout descriptorSetLayout, const DESCRIPTORSET_CAPS& caps)
{
	auto descriptorSetIterator = m_descriptorSetCache.find(caps);
	if(descriptorSetIterator != std::end(m_descriptorSetCache))
	{
		return descriptorSetIterator->second;
	}

	auto result = VK_SUCCESS;
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

	//Allocate descriptor set
	{
		auto setAllocateInfo = Framework::Vulkan::DescriptorSetAllocateInfo();
		setAllocateInfo.descriptorPool = m_context->descriptorPool;
		setAllocateInfo.descriptorSetCount = 1;
		setAllocateInfo.pSetLayouts = &descriptorSetLayout;

		result = m_context->device.vkAllocateDescriptorSets(m_context->device, &setAllocateInfo, &descriptorSet);
		CHECKVULKANERROR(result);
	}

	//Update descriptor set
	{
		VkDescriptorBufferInfo descriptorMemoryBufferInfo = {};
		descriptorMemoryBufferInfo.buffer = m_context->memoryBuffer;
		descriptorMemoryBufferInfo.range = VK_WHOLE_SIZE;

		VkDescriptorBufferInfo descriptorClutBufferInfo = {};
		descriptorClutBufferInfo.buffer = m_context->clutBuffer;
		descriptorClutBufferInfo.range = sizeof(uint32) * CGSHandler::CLUTENTRYCOUNT;

		VkDescriptorBufferInfo descriptorMipParamsUniformInfo = {};
		descriptorMipParamsUniformInfo.buffer = m_frames[caps.frameNumber].mipParamsBuffer;
		descriptorMipParamsUniformInfo.range = sizeof(DRAW_PIPELINE_MIPPARAMS_UNIFORMS);

		VkDescriptorImageInfo descriptorTexSwizzleTableImageInfo = {};
		descriptorTexSwizzleTableImageInfo.imageView = m_context->GetSwizzleTable(caps.textureFormat);
		descriptorTexSwizzleTableImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkDescriptorImageInfo descriptorFbSwizzleTableImageInfo = {};
		descriptorFbSwizzleTableImageInfo.imageView = m_context->GetSwizzleTable(caps.framebufferFormat);
		descriptorFbSwizzleTableImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkDescriptorImageInfo descriptorDepthSwizzleTableImageInfo = {};
		descriptorDepthSwizzleTableImageInfo.imageView = m_context->GetSwizzleTable(caps.depthbufferFormat);
		descriptorDepthSwizzleTableImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		VkDescriptorImageInfo descriptorInputColorImageInfo = {};
		descriptorInputColorImageInfo.imageLayout = caps.inputLayoutGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		descriptorInputColorImageInfo.imageView = m_residentColors[caps.colorSlot].imageView;

		VkDescriptorImageInfo descriptorInputDepthImageInfo = {};
		descriptorInputDepthImageInfo.imageLayout = caps.inputLayoutGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		descriptorInputDepthImageInfo.imageView = m_residentDepths[caps.depthSlot].imageView;

		std::vector<VkWriteDescriptorSet> writes;

		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_BUFFER_MEMORY;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writeSet.pBufferInfo = &descriptorMemoryBufferInfo;
			writes.push_back(writeSet);
		}

		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_FB;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writeSet.pImageInfo = &descriptorFbSwizzleTableImageInfo;
			writes.push_back(writeSet);
		}

		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_DEPTH;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writeSet.pImageInfo = &descriptorDepthSwizzleTableImageInfo;
			writes.push_back(writeSet);
		}

		if(caps.hasTexture)
		{
			{
				auto writeSet = Framework::Vulkan::WriteDescriptorSet();
				writeSet.dstSet = descriptorSet;
				writeSet.dstBinding = DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_TEX;
				writeSet.descriptorCount = 1;
				writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				writeSet.pImageInfo = &descriptorTexSwizzleTableImageInfo;
				writes.push_back(writeSet);
			}

			if(CGsPixelFormats::IsPsmIDTEX(caps.textureFormat))
			{
				auto writeSet = Framework::Vulkan::WriteDescriptorSet();
				writeSet.dstSet = descriptorSet;
				writeSet.dstBinding = DESCRIPTOR_LOCATION_IMAGE_CLUT;
				writeSet.descriptorCount = 1;
				writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
				writeSet.pBufferInfo = &descriptorClutBufferInfo;
				writes.push_back(writeSet);
			}

			if(caps.textureUseDynamicMipLOD)
			{
				auto writeSet = Framework::Vulkan::WriteDescriptorSet();
				writeSet.dstSet = descriptorSet;
				writeSet.dstBinding = DESCRIPTOR_LOCATION_UNIFORM_MIPPARAMS;
				writeSet.descriptorCount = 1;
				writeSet.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
				writeSet.pBufferInfo = &descriptorMipParamsUniformInfo;
				writes.push_back(writeSet);
			}
		}

		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_IMAGE_INPUT_COLOR;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
			writeSet.pImageInfo = &descriptorInputColorImageInfo;
			writes.push_back(writeSet);
		}

		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_IMAGE_INPUT_DEPTH;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
			writeSet.pImageInfo = &descriptorInputDepthImageInfo;
			writes.push_back(writeSet);
		}

		if(!caps.hasTexture && m_context->has16BitStorage)
		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_BUFFER_MEMORY_16BIT;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writeSet.pBufferInfo = &descriptorMemoryBufferInfo;
			writes.push_back(writeSet);
		}

		m_context->device.vkUpdateDescriptorSets(m_context->device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
	}

	m_descriptorSetCache.insert(std::make_pair(caps, descriptorSet));

	return descriptorSet;
}

void CDrawMobile::EnsureColorSlotAllocated(uint32 slotIdx)
{
	assert(slotIdx < RESIDENT_COLOR_SLOTS);
	auto& slot = m_residentColors[slotIdx];
	if(slot.allocated) return;

	slot.image = Framework::Vulkan::CImage(m_context->device, m_context->physicalDeviceMemoryProperties,
	                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
	                                       VK_FORMAT_R16G16B16A16_SFLOAT, DRAW_AREA_SIZE, DRAW_AREA_SIZE, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	{
		auto viewInfo = Framework::Vulkan::ImageViewCreateInfo();
		viewInfo.image = slot.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		viewInfo.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
		viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		auto result = m_context->device.vkCreateImageView(m_context->device, &viewInfo, nullptr, &slot.imageView);
		CHECKVULKANERROR(result);
	}

	auto makeFb = [&](VkRenderPass rp, const VkImageView* atts, uint32 attCount, VkFramebuffer& outFb) {
		auto fbInfo = Framework::Vulkan::FramebufferCreateInfo();
		fbInfo.renderPass = rp;
		fbInfo.width = DRAW_AREA_SIZE;
		fbInfo.height = DRAW_AREA_SIZE;
		fbInfo.layers = 1;
		fbInfo.attachmentCount = attCount;
		fbInfo.pAttachments = atts;
		auto result = m_context->device.vkCreateFramebuffer(m_context->device, &fbInfo, nullptr, &outFb);
		CHECKVULKANERROR(result);
	};

	VkImageView colorOnlyAtts[1] = {slot.imageView};

	makeFb(m_renderPasses[PASS_MODE_NO_DEPTH], colorOnlyAtts, 1, slot.drawFbs[0][PASS_MODE_NO_DEPTH]);
	makeFb(m_renderPasses[PASS_MODE_FEEDBACK_NO_DEPTH], colorOnlyAtts, 1, slot.drawFbs[0][PASS_MODE_FEEDBACK_NO_DEPTH]);
	makeFb(m_rpLoadColor, colorOnlyAtts, 1, slot.loadFb);
	makeFb(m_rpStoreColor, colorOnlyAtts, 1, slot.storeFb);

	slot.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	slot.allocated = true;
}

VkFramebuffer CDrawMobile::GetDrawFramebuffer(uint32 colorSlotIdx, uint32 depthSlotIdx, uint32 passMode)
{
	auto& cslot = m_residentColors[colorSlotIdx];
	uint32 dIdx = ((passMode == PASS_MODE_DEPTH) || (passMode == PASS_MODE_FEEDBACK_DEPTH)) ? depthSlotIdx : 0;
	VkFramebuffer& fb = cslot.drawFbs[dIdx][passMode];
	if(fb != VK_NULL_HANDLE)
	{
		return fb;
	}
	VkImageView atts[2] = {cslot.imageView, m_residentDepths[dIdx].imageView};
	auto fbInfo = Framework::Vulkan::FramebufferCreateInfo();
	fbInfo.renderPass = m_renderPasses[passMode];
	fbInfo.width = DRAW_AREA_SIZE;
	fbInfo.height = DRAW_AREA_SIZE;
	fbInfo.layers = 1;
	fbInfo.attachmentCount = 2;
	fbInfo.pAttachments = atts;
	auto result = m_context->device.vkCreateFramebuffer(m_context->device, &fbInfo, nullptr, &fb);
	CHECKVULKANERROR(result);
	return fb;
}

void CDrawMobile::CreateRenderPasses()
{
	//1. Draw render passes (single-subpass, LOAD_OP_LOAD / STORE_OP_STORE on persistent resident VRAM attachments)
	auto createDrawPass = [&](uint32 passMode) {
		assert(m_renderPasses[passMode] == VK_NULL_HANDLE);

		bool hasDepth = (passMode == PASS_MODE_DEPTH) || (passMode == PASS_MODE_FEEDBACK_DEPTH);
		bool isFeedback = (passMode == PASS_MODE_FEEDBACK_NO_DEPTH) || (passMode == PASS_MODE_FEEDBACK_DEPTH);

		VkImageLayout colorLayout = isFeedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkImageLayout depthLayout = isFeedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colorRef = {0, colorLayout};
		VkAttachmentReference depthRef = {1, depthLayout};

		std::vector<VkAttachmentReference> inputRefs;
		if(isFeedback)
		{
			inputRefs.push_back({0, VK_IMAGE_LAYOUT_GENERAL});
			if(hasDepth)
			{
				inputRefs.push_back({1, VK_IMAGE_LAYOUT_GENERAL});
			}
		}

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;
		if(hasDepth)
		{
			subpass.pDepthStencilAttachment = &depthRef;
		}
		if(!inputRefs.empty())
		{
			subpass.inputAttachmentCount = static_cast<uint32>(inputRefs.size());
			subpass.pInputAttachments = inputRefs.data();
		}

		std::vector<VkSubpassDependency> deps;
		{
			VkSubpassDependency dep = {};
			dep.srcSubpass = VK_SUBPASS_EXTERNAL;
			dep.srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
			                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
			                   (hasDepth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) : 0);
			dep.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			                    (hasDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0);
			dep.dstSubpass = 0;
			dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
			                   (hasDepth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) : 0);
			dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
			                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			                    (hasDepth ? (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) : 0) |
			                    (isFeedback ? VK_ACCESS_INPUT_ATTACHMENT_READ_BIT : 0);
			deps.push_back(dep);
		}
		if(isFeedback)
		{
			VkSubpassDependency dep = {};
			dep.srcSubpass = 0;
			dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
			                   (hasDepth ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT) : 0);
			dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			                    (hasDepth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : 0);
			dep.dstSubpass = 0;
			dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			dep.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
			dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
			deps.push_back(dep);
		}

		std::vector<VkAttachmentDescription> attachments;
		{
			VkAttachmentDescription att = {};
			att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
			att.samples = VK_SAMPLE_COUNT_1_BIT;
			att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			att.initialLayout = colorLayout;
			att.finalLayout = colorLayout;
			attachments.push_back(att);
		}
		if(hasDepth)
		{
			VkAttachmentDescription att = {};
			att.format = VK_FORMAT_D32_SFLOAT;
			att.samples = VK_SAMPLE_COUNT_1_BIT;
			att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			att.initialLayout = depthLayout;
			att.finalLayout = depthLayout;
			attachments.push_back(att);
		}

		auto rpInfo = Framework::Vulkan::RenderPassCreateInfo();
		rpInfo.subpassCount = 1;
		rpInfo.pSubpasses = &subpass;
		rpInfo.attachmentCount = static_cast<uint32>(attachments.size());
		rpInfo.pAttachments = attachments.data();
		rpInfo.dependencyCount = static_cast<uint32>(deps.size());
		rpInfo.pDependencies = deps.data();

		auto result = m_context->device.vkCreateRenderPass(m_context->device, &rpInfo, nullptr, &m_renderPasses[passMode]);
		CHECKVULKANERROR(result);
	};

	for(uint32 i = 0; i < PASS_MODE_COUNT; i++)
	{
		createDrawPass(i);
	}

	//2. Standalone Load Color render pass (GS RAM -> resident color slot)
	{
		VkAttachmentDescription att = {};
		att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		att.samples = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		VkSubpassDependency dep = {};
		dep.srcSubpass = VK_SUBPASS_EXTERNAL;
		dep.srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dep.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		dep.dstSubpass = 0;
		dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		auto rpInfo = Framework::Vulkan::RenderPassCreateInfo();
		rpInfo.attachmentCount = 1;
		rpInfo.pAttachments = &att;
		rpInfo.subpassCount = 1;
		rpInfo.pSubpasses = &subpass;
		rpInfo.dependencyCount = 1;
		rpInfo.pDependencies = &dep;
		auto result = m_context->device.vkCreateRenderPass(m_context->device, &rpInfo, nullptr, &m_rpLoadColor);
		CHECKVULKANERROR(result);

		//Compatible preserve-load variant for zero-flush border strip expansion on resident slots.
		att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		dep.srcStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dep.dstStageMask |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dep.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		result = m_context->device.vkCreateRenderPass(m_context->device, &rpInfo, nullptr, &m_rpLoadColorPreserve);
		CHECKVULKANERROR(result);
	}

	//3. Standalone Store Color render pass (resident color slot -> GS RAM SSBO)
	{
		constexpr VkAttachmentStoreOp kStoreOpNone = static_cast<VkAttachmentStoreOp>(1000301000);
		VkAttachmentDescription att = {};
		att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
		att.samples = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att.storeOp = m_context->hasStoreOpNone ? kStoreOpNone : VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference inputRef = {0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.inputAttachmentCount = 1;
		subpass.pInputAttachments = &inputRef;

		auto rpInfo = Framework::Vulkan::RenderPassCreateInfo();
		rpInfo.attachmentCount = 1;
		rpInfo.pAttachments = &att;
		rpInfo.subpassCount = 1;
		rpInfo.pSubpasses = &subpass;
		auto result = m_context->device.vkCreateRenderPass(m_context->device, &rpInfo, nullptr, &m_rpStoreColor);
		CHECKVULKANERROR(result);
	}

	//4. Standalone Load Depth render pass (GS RAM -> persistent D32_SFLOAT depth)
	{
		VkAttachmentDescription att = {};
		att.format = VK_FORMAT_D32_SFLOAT;
		att.samples = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference depthRef = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.pDepthStencilAttachment = &depthRef;

		VkSubpassDependency dep = {};
		dep.srcSubpass = VK_SUBPASS_EXTERNAL;
		dep.srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dep.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		dep.dstSubpass = 0;
		dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

		auto rpInfo = Framework::Vulkan::RenderPassCreateInfo();
		rpInfo.attachmentCount = 1;
		rpInfo.pAttachments = &att;
		rpInfo.subpassCount = 1;
		rpInfo.pSubpasses = &subpass;
		rpInfo.dependencyCount = 1;
		rpInfo.pDependencies = &dep;
		auto result = m_context->device.vkCreateRenderPass(m_context->device, &rpInfo, nullptr, &m_rpLoadDepth);
		CHECKVULKANERROR(result);

		//Compatible preserve-load variant for zero-flush border strip expansion on resident depth slots.
		att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		dep.srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dep.srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dep.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dep.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		result = m_context->device.vkCreateRenderPass(m_context->device, &rpInfo, nullptr, &m_rpLoadDepthPreserve);
		CHECKVULKANERROR(result);
	}

	//5. Standalone Store Depth render pass (persistent D32_SFLOAT depth -> GS RAM SSBO)
	{
		constexpr VkAttachmentStoreOp kStoreOpNone = static_cast<VkAttachmentStoreOp>(1000301000);
		VkAttachmentDescription att = {};
		att.format = VK_FORMAT_D32_SFLOAT;
		att.samples = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att.storeOp = m_context->hasStoreOpNone ? kStoreOpNone : VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference inputRef = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.inputAttachmentCount = 1;
		subpass.pInputAttachments = &inputRef;

		auto rpInfo = Framework::Vulkan::RenderPassCreateInfo();
		rpInfo.attachmentCount = 1;
		rpInfo.pAttachments = &att;
		rpInfo.subpassCount = 1;
		rpInfo.pSubpasses = &subpass;
		auto result = m_context->device.vkCreateRenderPass(m_context->device, &rpInfo, nullptr, &m_rpStoreDepth);
		CHECKVULKANERROR(result);
	}
}

CDrawMobile::HW_BLEND_CONFIG CDrawMobile::GetHardwareBlendConfig(const PIPELINE_CAPS& caps, bool hasDualSrcBlend)
{
	HW_BLEND_CONFIG cfg;
	if(!caps.hasAlphaBlending)
	{
		cfg.supported = true;
		cfg.blendEnable = false;
		return cfg;
	}
	if(!caps.colClamp)
	{
		return cfg;
	}

	uint32 a = caps.alphaA;
	uint32 b = caps.alphaB;
	uint32 c = caps.alphaC;
	uint32 d = caps.alphaD;

	if(a == b)
	{
		cfg.supported = true;
		if(d == CGSHandler::ALPHABLEND_ABD_CS)
		{
			cfg.blendEnable = false;
		}
		else if(d == CGSHandler::ALPHABLEND_ABD_CD)
		{
			cfg.blendEnable = true;
			cfg.zeroSrcColor = true;
			cfg.srcColor = VK_BLEND_FACTOR_ZERO;
			cfg.dstColor = VK_BLEND_FACTOR_ONE;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		else
		{
			cfg.blendEnable = true;
			cfg.zeroSrcColor = true;
			cfg.srcColor = VK_BLEND_FACTOR_ONE;
			cfg.dstColor = VK_BLEND_FACTOR_ZERO;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		return cfg;
	}

	if(c == CGSHandler::ALPHABLEND_C_AD)
	{
		//Framebuffer alpha is stored as Ad/128.0 in R16G16B16A16_SFLOAT (unclamped).
		cfg.blendEnable = true;
		if((a == CGSHandler::ALPHABLEND_ABD_CS) && (b == CGSHandler::ALPHABLEND_ABD_ZERO) && (d == CGSHandler::ALPHABLEND_ABD_CD))
		{
			//Cs * (Ad/128) + Cd
			cfg.supported = true;
			cfg.srcColor = VK_BLEND_FACTOR_DST_ALPHA;
			cfg.dstColor = VK_BLEND_FACTOR_ONE;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_CS) && (b == CGSHandler::ALPHABLEND_ABD_ZERO) && (d == CGSHandler::ALPHABLEND_ABD_ZERO))
		{
			//Cs * (Ad/128)
			cfg.supported = true;
			cfg.srcColor = VK_BLEND_FACTOR_DST_ALPHA;
			cfg.dstColor = VK_BLEND_FACTOR_ZERO;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_ZERO) && (b == CGSHandler::ALPHABLEND_ABD_CS) && (d == CGSHandler::ALPHABLEND_ABD_CD))
		{
			//Cd - Cs * (Ad/128)
			cfg.supported = true;
			cfg.srcColor = VK_BLEND_FACTOR_DST_ALPHA;
			cfg.dstColor = VK_BLEND_FACTOR_ONE;
			cfg.colorOp = VK_BLEND_OP_REVERSE_SUBTRACT;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_CS) && (b == CGSHandler::ALPHABLEND_ABD_CD) && (d == CGSHandler::ALPHABLEND_ABD_CD))
		{
			//Cs * (Ad/128) + Cd * (1 - Ad/128)
			cfg.supported = true;
			cfg.srcColor = VK_BLEND_FACTOR_DST_ALPHA;
			cfg.dstColor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_CD) && (b == CGSHandler::ALPHABLEND_ABD_CS) && (d == CGSHandler::ALPHABLEND_ABD_CS))
		{
			//Cs * (1 - Ad/128) + Cd * (Ad/128)
			cfg.supported = true;
			cfg.srcColor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
			cfg.dstColor = VK_BLEND_FACTOR_DST_ALPHA;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_CD) && (b == CGSHandler::ALPHABLEND_ABD_ZERO) && (d == CGSHandler::ALPHABLEND_ABD_ZERO))
		{
			//Cd * (Ad/128)
			cfg.supported = true;
			cfg.zeroSrcColor = true;
			cfg.srcColor = VK_BLEND_FACTOR_ZERO;
			cfg.dstColor = VK_BLEND_FACTOR_DST_ALPHA;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_CD) && (b == CGSHandler::ALPHABLEND_ABD_ZERO) && (d == CGSHandler::ALPHABLEND_ABD_CS))
		{
			//Cs + Cd * (Ad/128)
			cfg.supported = true;
			cfg.srcColor = VK_BLEND_FACTOR_ONE;
			cfg.dstColor = VK_BLEND_FACTOR_DST_ALPHA;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		return cfg;
	}

	//C is AS or FIX (known in fragment shader as factorC = C/128.0)
	cfg.blendEnable = true;
	if((a == CGSHandler::ALPHABLEND_ABD_CS) && (b == CGSHandler::ALPHABLEND_ABD_ZERO) && (d == CGSHandler::ALPHABLEND_ABD_CD))
	{
		//Cs * f + Cd
		cfg.supported = true;
		cfg.premulSrcByC = true;
		cfg.srcColor = VK_BLEND_FACTOR_ONE;
		cfg.dstColor = VK_BLEND_FACTOR_ONE;
		cfg.colorOp = VK_BLEND_OP_ADD;
		return cfg;
	}
	if((a == CGSHandler::ALPHABLEND_ABD_CS) && (b == CGSHandler::ALPHABLEND_ABD_ZERO) && (d == CGSHandler::ALPHABLEND_ABD_ZERO))
	{
		//Cs * f
		cfg.supported = true;
		cfg.premulSrcByC = true;
		cfg.srcColor = VK_BLEND_FACTOR_ONE;
		cfg.dstColor = VK_BLEND_FACTOR_ZERO;
		cfg.colorOp = VK_BLEND_OP_ADD;
		return cfg;
	}
	if((a == CGSHandler::ALPHABLEND_ABD_ZERO) && (b == CGSHandler::ALPHABLEND_ABD_CS) && (d == CGSHandler::ALPHABLEND_ABD_CD))
	{
		//Cd - Cs * f
		cfg.supported = true;
		cfg.premulSrcByC = true;
		cfg.srcColor = VK_BLEND_FACTOR_ONE;
		cfg.dstColor = VK_BLEND_FACTOR_ONE;
		cfg.colorOp = VK_BLEND_OP_REVERSE_SUBTRACT;
		return cfg;
	}

	//Prefer single-source hardware blending over dual-source blending:
	//- When c == AS and fba == 0, outputColor.a is already As/128.0 (= factorC), so SRC_ALPHA / ONE_MINUS_SRC_ALPHA works natively.
	//- When c == FIX, factorC is uniform per draw, so dynamic blend constants (CONSTANT_COLOR) work natively.
	//Both avoid dual-source blending, keeping full Adreno blend throughput and enabling MRT depth shadow.
	bool useSrcAlpha = (c == CGSHandler::ALPHABLEND_C_AS) && !caps.fba;
	bool useConstColor = (c == CGSHandler::ALPHABLEND_C_FIX);
	if(useSrcAlpha || useConstColor)
	{
		VkBlendFactor fFactor = useConstColor ? VK_BLEND_FACTOR_CONSTANT_COLOR : VK_BLEND_FACTOR_SRC_ALPHA;
		VkBlendFactor invFFactor = useConstColor ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		if((a == CGSHandler::ALPHABLEND_ABD_CS) && (b == CGSHandler::ALPHABLEND_ABD_CD) && (d == CGSHandler::ALPHABLEND_ABD_CD))
		{
			//Cs * f + Cd * (1 - f)
			cfg.supported = true;
			cfg.usesConstColor = useConstColor;
			cfg.premulSrcByC = true;
			cfg.srcColor = VK_BLEND_FACTOR_ONE;
			cfg.dstColor = invFFactor;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_CS) && (b == CGSHandler::ALPHABLEND_ABD_CD) && (d == CGSHandler::ALPHABLEND_ABD_ZERO))
		{
			//Cs * f - Cd * f
			cfg.supported = true;
			cfg.usesConstColor = useConstColor;
			cfg.premulSrcByC = true;
			cfg.srcColor = VK_BLEND_FACTOR_ONE;
			cfg.dstColor = fFactor;
			cfg.colorOp = VK_BLEND_OP_SUBTRACT;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_CD) && (b == CGSHandler::ALPHABLEND_ABD_CS) && (d == CGSHandler::ALPHABLEND_ABD_CS))
		{
			//Cs * (1 - f) + Cd * f
			cfg.supported = true;
			cfg.usesConstColor = useConstColor;
			cfg.premulSrcByOneMinusC = true;
			cfg.srcColor = VK_BLEND_FACTOR_ONE;
			cfg.dstColor = fFactor;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_CD) && (b == CGSHandler::ALPHABLEND_ABD_ZERO) && (d == CGSHandler::ALPHABLEND_ABD_CS))
		{
			//Cs + Cd * f
			cfg.supported = true;
			cfg.usesConstColor = useConstColor;
			cfg.srcColor = VK_BLEND_FACTOR_ONE;
			cfg.dstColor = fFactor;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_CD) && (b == CGSHandler::ALPHABLEND_ABD_ZERO) && (d == CGSHandler::ALPHABLEND_ABD_ZERO))
		{
			//Cd * f
			cfg.supported = true;
			cfg.usesConstColor = useConstColor;
			cfg.zeroSrcColor = true;
			cfg.srcColor = VK_BLEND_FACTOR_ZERO;
			cfg.dstColor = fFactor;
			cfg.colorOp = VK_BLEND_OP_ADD;
		}
		else if((a == CGSHandler::ALPHABLEND_ABD_ZERO) && (b == CGSHandler::ALPHABLEND_ABD_CD) && (d == CGSHandler::ALPHABLEND_ABD_CS))
		{
			//Cs - Cd * f
			cfg.supported = true;
			cfg.usesConstColor = useConstColor;
			cfg.srcColor = VK_BLEND_FACTOR_ONE;
			cfg.dstColor = fFactor;
			cfg.colorOp = VK_BLEND_OP_SUBTRACT;
		}
		return cfg;
	}

	if(!hasDualSrcBlend)
	{
		return cfg;
	}

	if((a == CGSHandler::ALPHABLEND_ABD_CS) && (b == CGSHandler::ALPHABLEND_ABD_CD) && (d == CGSHandler::ALPHABLEND_ABD_CD))
	{
		//Cs * f + Cd * (1 - f)
		cfg.supported = true;
		cfg.dualSrc = true;
		cfg.premulSrcByC = true;
		cfg.srcColor = VK_BLEND_FACTOR_ONE;
		cfg.dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
		cfg.colorOp = VK_BLEND_OP_ADD;
	}
	else if((a == CGSHandler::ALPHABLEND_ABD_CS) && (b == CGSHandler::ALPHABLEND_ABD_CD) && (d == CGSHandler::ALPHABLEND_ABD_ZERO))
	{
		//Cs * f - Cd * f
		cfg.supported = true;
		cfg.dualSrc = true;
		cfg.premulSrcByC = true;
		cfg.srcColor = VK_BLEND_FACTOR_ONE;
		cfg.dstColor = VK_BLEND_FACTOR_SRC1_COLOR;
		cfg.colorOp = VK_BLEND_OP_SUBTRACT;
	}
	else if((a == CGSHandler::ALPHABLEND_ABD_CD) && (b == CGSHandler::ALPHABLEND_ABD_CS) && (d == CGSHandler::ALPHABLEND_ABD_CS))
	{
		//Cs * (1 - f) + Cd * f
		cfg.supported = true;
		cfg.dualSrc = true;
		cfg.premulSrcByOneMinusC = true;
		cfg.srcColor = VK_BLEND_FACTOR_ONE;
		cfg.dstColor = VK_BLEND_FACTOR_SRC1_COLOR;
		cfg.colorOp = VK_BLEND_OP_ADD;
	}
	else if((a == CGSHandler::ALPHABLEND_ABD_CD) && (b == CGSHandler::ALPHABLEND_ABD_ZERO) && (d == CGSHandler::ALPHABLEND_ABD_CS))
	{
		//Cs + Cd * f
		cfg.supported = true;
		cfg.dualSrc = true;
		cfg.srcColor = VK_BLEND_FACTOR_ONE;
		cfg.dstColor = VK_BLEND_FACTOR_SRC1_COLOR;
		cfg.colorOp = VK_BLEND_OP_ADD;
	}
	else if((a == CGSHandler::ALPHABLEND_ABD_CD) && (b == CGSHandler::ALPHABLEND_ABD_ZERO) && (d == CGSHandler::ALPHABLEND_ABD_ZERO))
	{
		//Cd * f
		cfg.supported = true;
		cfg.dualSrc = true;
		cfg.zeroSrcColor = true;
		cfg.srcColor = VK_BLEND_FACTOR_ZERO;
		cfg.dstColor = VK_BLEND_FACTOR_SRC1_COLOR;
		cfg.colorOp = VK_BLEND_OP_ADD;
	}
	else if((a == CGSHandler::ALPHABLEND_ABD_ZERO) && (b == CGSHandler::ALPHABLEND_ABD_CD) && (d == CGSHandler::ALPHABLEND_ABD_CS))
	{
		//Cs - Cd * f
		cfg.supported = true;
		cfg.dualSrc = true;
		cfg.srcColor = VK_BLEND_FACTOR_ONE;
		cfg.dstColor = VK_BLEND_FACTOR_SRC1_COLOR;
		cfg.colorOp = VK_BLEND_OP_SUBTRACT;
	}

	return cfg;
}

uint8 CDrawMobile::GetHardwareColorWriteMask(uint32 framebufferFormat, uint32 fbWriteMask)
{
	switch(framebufferFormat)
	{
	case CGSHandler::PSMCT32:
	case CGSHandler::PSMZ32:
	{
		uint8 mask = 0;
		for(int ch = 0; ch < 4; ch++)
		{
			uint32 byteMask = (fbWriteMask >> (ch * 8)) & 0xFF;
			if(byteMask == 0xFF) mask |= static_cast<uint8>(1u << ch);
			else if(byteMask != 0x00) return 0xFF;
		}
		return mask;
	}
	case CGSHandler::PSMCT24:
	case CGSHandler::PSMZ24:
	{
		uint8 mask = 0;
		for(int ch = 0; ch < 3; ch++)
		{
			uint32 byteMask = (fbWriteMask >> (ch * 8)) & 0xFF;
			if(byteMask == 0xFF) mask |= static_cast<uint8>(1u << ch);
			else if(byteMask != 0x00) return 0xFF;
		}
		return mask;
	}
	case CGSHandler::PSMCT16:
	case CGSHandler::PSMCT16S:
	case CGSHandler::PSMZ16:
	case CGSHandler::PSMZ16S:
	{
		uint8 mask = 0;
		uint32 r = fbWriteMask & 0x001F;
		uint32 g = fbWriteMask & 0x03E0;
		uint32 b = fbWriteMask & 0x7C00;
		uint32 a = fbWriteMask & 0x8000;
		if(r == 0x001F) mask |= 1u; else if(r != 0) return 0xFF;
		if(g == 0x03E0) mask |= 2u; else if(g != 0) return 0xFF;
		if(b == 0x7C00) mask |= 4u; else if(b != 0) return 0xFF;
		if(a == 0x8000) mask |= 8u; else if(a != 0) return 0xFF;
		return mask;
	}
	default:
		return 0xFF;
	}
}

bool CDrawMobile::WritesFragDepth(const PIPELINE_CAPS& caps)
{
	if(!caps.writeDepth) return false;
	if(caps.hasDstAlphaTest) return true;
	if((caps.alphaTestFunction != CGSHandler::ALPHA_TEST_ALWAYS) &&
	   ((caps.alphaTestFailAction == CGSHandler::ALPHA_TEST_FAIL_FBONLY) ||
	    (caps.alphaTestFailAction == CGSHandler::ALPHA_TEST_FAIL_RGBONLY)))
	{
		return true;
	}
	return false;
}

bool CDrawMobile::UsesSoftwareColorRead(const PIPELINE_CAPS& caps, const HW_BLEND_CONFIG& hwBlend)
{
	if(!hwBlend.supported) return true;
	if(caps.hasDstAlphaTest) return true;
	if(caps.maskColor) return true;
	if((caps.alphaTestFunction != CGSHandler::ALPHA_TEST_ALWAYS) &&
	   ((caps.alphaTestFailAction == CGSHandler::ALPHA_TEST_FAIL_ZBONLY) ||
	    (caps.alphaTestFailAction == CGSHandler::ALPHA_TEST_FAIL_RGBONLY)))
	{
		return true;
	}
	return false;
}

VkCompareOp CDrawMobile::MapDepthCompareOp(uint32 depthTestFunction)
{
	switch(depthTestFunction)
	{
	default:
		assert(false);
		[[fallthrough]];
	case CGSHandler::DEPTH_TEST_ALWAYS:
		return VK_COMPARE_OP_ALWAYS;
	case CGSHandler::DEPTH_TEST_NEVER:
		return VK_COMPARE_OP_NEVER;
	case CGSHandler::DEPTH_TEST_GEQUAL:
		return VK_COMPARE_OP_GREATER_OR_EQUAL;
	case CGSHandler::DEPTH_TEST_GREATER:
		return VK_COMPARE_OP_GREATER;
	}
}

uint32 CDrawMobile::GetDrawLayoutIndex(const PIPELINE_CAPS& caps) const
{
	if(!caps.hasTexture) return 0;
	uint32 idx = 1;
	if(CGsPixelFormats::IsPsmIDTEX(caps.textureFormat)) idx |= 2;
	if(caps.textureUseDynamicMipLOD) idx |= 4;
	return idx;
}

void CDrawMobile::CreateSharedLayouts()
{
	for(uint32 idx = 0; idx < DRAW_LAYOUT_COUNT; idx++)
	{
		bool hasTexture = (idx & 1) != 0;
		bool isIdtex = (idx & 2) != 0;
		bool hasDynMip = (idx & 4) != 0;
		if(!hasTexture && (isIdtex || hasDynMip)) continue;

		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;
		auto addBinding = [&](uint32 binding, VkDescriptorType type) {
			VkDescriptorSetLayoutBinding b = {};
			b.binding = binding;
			b.descriptorType = type;
			b.descriptorCount = 1;
			b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			setLayoutBindings.push_back(b);
		};

		addBinding(DESCRIPTOR_LOCATION_BUFFER_MEMORY, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		addBinding(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_FB, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		addBinding(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_DEPTH, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		if(hasTexture)
		{
			addBinding(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_TEX, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
			if(isIdtex) addBinding(DESCRIPTOR_LOCATION_IMAGE_CLUT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
			if(hasDynMip) addBinding(DESCRIPTOR_LOCATION_UNIFORM_MIPPARAMS, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
		}
		addBinding(DESCRIPTOR_LOCATION_IMAGE_INPUT_COLOR, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
		addBinding(DESCRIPTOR_LOCATION_IMAGE_INPUT_DEPTH, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
		if(!hasTexture && m_context->has16BitStorage)
		{
			addBinding(DESCRIPTOR_LOCATION_BUFFER_MEMORY_16BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
		}

		auto setLayoutCreateInfo = Framework::Vulkan::DescriptorSetLayoutCreateInfo();
		setLayoutCreateInfo.bindingCount = static_cast<uint32>(setLayoutBindings.size());
		setLayoutCreateInfo.pBindings = setLayoutBindings.data();
		auto result = m_context->device.vkCreateDescriptorSetLayout(m_context->device, &setLayoutCreateInfo, nullptr, &m_drawLayouts[idx].descriptorSetLayout);
		CHECKVULKANERROR(result);

		VkPushConstantRange pushConstantInfo = {};
		pushConstantInfo.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		pushConstantInfo.offset = 0;
		pushConstantInfo.size = sizeof(DRAW_PIPELINE_PUSHCONSTANTS);

		auto pipelineLayoutCreateInfo = Framework::Vulkan::PipelineLayoutCreateInfo();
		pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
		pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantInfo;
		pipelineLayoutCreateInfo.setLayoutCount = 1;
		pipelineLayoutCreateInfo.pSetLayouts = &m_drawLayouts[idx].descriptorSetLayout;
		result = m_context->device.vkCreatePipelineLayout(m_context->device, &pipelineLayoutCreateInfo, nullptr, &m_drawLayouts[idx].pipelineLayout);
		CHECKVULKANERROR(result);
	}
	m_loadStoreLayout = m_drawLayouts[0];
}

PIPELINE CDrawMobile::CreateDrawPipeline(const PIPELINE_CAPS& caps, uint32 passMode)
{
	PIPELINE drawPipeline;
	drawPipeline.ownsLayouts = false;

	auto vertexShader = CreateVertexShader(caps);
	auto fragmentShader = CreateDrawFragmentShader(caps, passMode);

	auto result = VK_SUCCESS;

	const auto& sharedLayout = m_drawLayouts[GetDrawLayoutIndex(caps)];
	drawPipeline.descriptorSetLayout = sharedLayout.descriptorSetLayout;
	drawPipeline.pipelineLayout = sharedLayout.pipelineLayout;

	auto inputAssemblyInfo = Framework::Vulkan::PipelineInputAssemblyStateCreateInfo();
	switch(caps.primitiveType)
	{
	default:
		assert(false);
	case PIPELINE_PRIMITIVE_TRIANGLE:
		inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		break;
	case PIPELINE_PRIMITIVE_LINE:
		inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		break;
	case PIPELINE_PRIMITIVE_POINT:
		inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
		break;
	}

	std::vector<VkVertexInputAttributeDescription> vertexAttributes;

	{
		VkVertexInputAttributeDescription vertexAttributeDesc = {};
		vertexAttributeDesc.format = VK_FORMAT_R32G32_SFLOAT;
		vertexAttributeDesc.offset = offsetof(PRIM_VERTEX, x);
		vertexAttributeDesc.location = VERTEX_ATTRIB_LOCATION_POSITION;
		vertexAttributes.push_back(vertexAttributeDesc);
	}

	{
		VkVertexInputAttributeDescription vertexAttributeDesc = {};
		vertexAttributeDesc.format = VK_FORMAT_R32_UINT;
		vertexAttributeDesc.offset = offsetof(PRIM_VERTEX, z);
		vertexAttributeDesc.location = VERTEX_ATTRIB_LOCATION_DEPTH;
		vertexAttributes.push_back(vertexAttributeDesc);
	}

	{
		VkVertexInputAttributeDescription vertexAttributeDesc = {};
		vertexAttributeDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
		vertexAttributeDesc.offset = offsetof(PRIM_VERTEX, color);
		vertexAttributeDesc.location = VERTEX_ATTRIB_LOCATION_COLOR;
		vertexAttributes.push_back(vertexAttributeDesc);
	}

	{
		VkVertexInputAttributeDescription vertexAttributeDesc = {};
		vertexAttributeDesc.format = VK_FORMAT_R32G32B32_SFLOAT;
		vertexAttributeDesc.offset = offsetof(PRIM_VERTEX, s);
		vertexAttributeDesc.location = VERTEX_ATTRIB_LOCATION_TEXCOORD;
		vertexAttributes.push_back(vertexAttributeDesc);
	}

	{
		VkVertexInputAttributeDescription vertexAttributeDesc = {};
		vertexAttributeDesc.format = VK_FORMAT_R32_SFLOAT;
		vertexAttributeDesc.offset = offsetof(PRIM_VERTEX, f);
		vertexAttributeDesc.location = VERTEX_ATTRIB_LOCATION_FOG;
		vertexAttributes.push_back(vertexAttributeDesc);
	}

	VkVertexInputBindingDescription binding = {};
	binding.stride = sizeof(PRIM_VERTEX);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	auto vertexInputInfo = Framework::Vulkan::PipelineVertexInputStateCreateInfo();
	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.pVertexBindingDescriptions = &binding;
	vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32>(vertexAttributes.size());
	vertexInputInfo.pVertexAttributeDescriptions = vertexAttributes.data();

	auto rasterStateInfo = Framework::Vulkan::PipelineRasterizationStateCreateInfo();
	rasterStateInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterStateInfo.cullMode = VK_CULL_MODE_NONE;
	rasterStateInfo.lineWidth = 1.0f;

	auto hwBlend = GetHardwareBlendConfig(caps, m_context->hasDualSrcBlend);
	bool writesFragDepth = WritesFragDepth(caps);
	bool useSoftwarePath = UsesSoftwareColorRead(caps, hwBlend) || writesFragDepth;

	VkPipelineColorBlendAttachmentState blendAttachmentState = {};
	blendAttachmentState.colorWriteMask = caps.maskColor ? 0xF : (caps.depthbufferFormat & 0xF);
	if(!useSoftwarePath && hwBlend.blendEnable)
	{
		blendAttachmentState.blendEnable = VK_TRUE;
		blendAttachmentState.srcColorBlendFactor = hwBlend.srcColor;
		blendAttachmentState.dstColorBlendFactor = hwBlend.dstColor;
		blendAttachmentState.colorBlendOp = hwBlend.colorOp;
		blendAttachmentState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttachmentState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;
	}

	auto colorBlendStateInfo = Framework::Vulkan::PipelineColorBlendStateCreateInfo();
	colorBlendStateInfo.attachmentCount = 1;
	colorBlendStateInfo.pAttachments = &blendAttachmentState;

	auto viewportStateInfo = Framework::Vulkan::PipelineViewportStateCreateInfo();
	viewportStateInfo.viewportCount = 1;
	viewportStateInfo.scissorCount = 1;

	bool hasDepthAtt = (passMode == PASS_MODE_DEPTH) || (passMode == PASS_MODE_FEEDBACK_DEPTH);
	auto depthStencilStateInfo = Framework::Vulkan::PipelineDepthStencilStateCreateInfo();
	if(hasDepthAtt)
	{
		if(writesFragDepth)
		{
			depthStencilStateInfo.depthTestEnable = VK_TRUE;
			depthStencilStateInfo.depthCompareOp = VK_COMPARE_OP_ALWAYS;
			depthStencilStateInfo.depthWriteEnable = VK_TRUE;
		}
		else if((caps.depthTestFunction != CGSHandler::DEPTH_TEST_ALWAYS) || (caps.writeDepth != 0))
		{
			depthStencilStateInfo.depthTestEnable = VK_TRUE;
			depthStencilStateInfo.depthCompareOp = MapDepthCompareOp(caps.depthTestFunction);
			depthStencilStateInfo.depthWriteEnable = (caps.writeDepth != 0) ? VK_TRUE : VK_FALSE;
		}
	}

	auto multisampleStateInfo = Framework::Vulkan::PipelineMultisampleStateCreateInfo();
	multisampleStateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	static const VkDynamicState dynamicStates[] =
	    {
	        VK_DYNAMIC_STATE_VIEWPORT,
	        VK_DYNAMIC_STATE_SCISSOR,
	        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
	    };
	auto dynamicStateInfo = Framework::Vulkan::PipelineDynamicStateCreateInfo();
	dynamicStateInfo.pDynamicStates = dynamicStates;
	dynamicStateInfo.dynamicStateCount = sizeof(dynamicStates) / sizeof(dynamicStates[0]);

	VkPipelineShaderStageCreateInfo shaderStages[2] =
	    {
	        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	    };

	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = vertexShader;
	shaderStages[0].pName = "main";
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = fragmentShader;
	shaderStages[1].pName = "main";

	auto pipelineCreateInfo = Framework::Vulkan::GraphicsPipelineCreateInfo();
	pipelineCreateInfo.stageCount = 2;
	pipelineCreateInfo.subpass = 0;
	pipelineCreateInfo.pStages = shaderStages;
	pipelineCreateInfo.pInputAssemblyState = &inputAssemblyInfo;
	pipelineCreateInfo.pVertexInputState = &vertexInputInfo;
	pipelineCreateInfo.pRasterizationState = &rasterStateInfo;
	pipelineCreateInfo.pColorBlendState = &colorBlendStateInfo;
	pipelineCreateInfo.pViewportState = &viewportStateInfo;
	pipelineCreateInfo.pDepthStencilState = &depthStencilStateInfo;
	pipelineCreateInfo.pMultisampleState = &multisampleStateInfo;
	pipelineCreateInfo.pDynamicState = &dynamicStateInfo;
	pipelineCreateInfo.renderPass = m_renderPasses[passMode];
	pipelineCreateInfo.layout = drawPipeline.pipelineLayout;

	result = m_context->device.vkCreateGraphicsPipelines(m_context->device, m_context->pipelineCache, 1, &pipelineCreateInfo, nullptr, &drawPipeline.pipeline);
	CHECKVULKANERROR(result);

	return drawPipeline;
}

Framework::Vulkan::CShaderModule CDrawMobile::CreateDrawFragmentShader(const PIPELINE_CAPS& caps, uint32 /*passMode*/)
{
	using namespace Nuanceur;

	auto hwBlend = GetHardwareBlendConfig(caps, m_context->hasDualSrcBlend);
	bool writesFragDepth = WritesFragDepth(caps);
	bool useSoftwarePath = UsesSoftwareColorRead(caps, hwBlend) || writesFragDepth;
	bool useDualSrc = !useSoftwarePath && hwBlend.dualSrc;
	bool discardScanMask = (caps.scanMask != 0) && !writesFragDepth;
	bool discardAlphaKeep = (caps.alphaTestFunction != CGSHandler::ALPHA_TEST_ALWAYS) &&
	                        (caps.alphaTestFailAction == CGSHandler::ALPHA_TEST_FAIL_KEEP) &&
	                        !writesFragDepth;

	uint32 patchFlags = CContext::SHADER_PATCH_NONE;
	if(writesFragDepth) patchFlags |= CContext::SHADER_PATCH_FRAG_DEPTH;
	if(useDualSrc) patchFlags |= CContext::SHADER_PATCH_DUAL_SRC_BLEND;
	if(discardScanMask || discardAlphaKeep) patchFlags |= CContext::SHADER_PATCH_DISCARD;

	return m_context->GetOrCreateShaderModule(1U, 0U, static_cast<uint64>(caps), patchFlags, [&](Framework::CMemStream& shaderStream) {
		auto b = CShaderBuilder();

	{
		//Inputs
		auto inputPosition = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_SYSTEM_POSITION));
		auto inputDepth = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_TEXCOORD, 1));
		auto inputColor = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_TEXCOORD, 2));
		auto inputTexCoord = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_TEXCOORD, 3));
		auto inputFog = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_TEXCOORD, 4));

		//Outputs
		auto outputColor = CFloat4Lvalue(b.CreateOutput(Nuanceur::SEMANTIC_SYSTEM_COLOR, 0));
		auto outputSecondary = (useDualSrc || writesFragDepth) ? b.CreateOutput(Nuanceur::SEMANTIC_SYSTEM_COLOR, 1) : CShaderBuilder::SYMBOL{};

		//Subpass 1 only reads memoryBuffer; omitting COHERENT allows the GPU L1 cache to cache texel fetches.
		auto memoryBuffer = CArrayUintValue(b.CreateUniformArrayUint("memoryBuffer", DESCRIPTOR_LOCATION_BUFFER_MEMORY));
		auto clutBuffer = CArrayUintValue(b.CreateUniformArrayUint("clutBuffer", DESCRIPTOR_LOCATION_IMAGE_CLUT));
		auto texSwizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_TEX));
		auto fbSwizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_FB));
		auto depthSwizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_DEPTH));
		auto subpassColorInput = useSoftwarePath ? b.CreateSubpassInput(DESCRIPTOR_LOCATION_IMAGE_INPUT_COLOR, 0) : CShaderBuilder::SYMBOL{};
		auto subpassDepthInput = writesFragDepth ? b.CreateSubpassInput(DESCRIPTOR_LOCATION_IMAGE_INPUT_DEPTH, 1) : CShaderBuilder::SYMBOL{};

		//Push constants
		auto fbDepthParams = CInt4Lvalue(b.CreateUniformInt4("fbDepthParams", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
		auto texParams0 = CInt4Lvalue(b.CreateUniformInt4("texParams0", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
		auto texParams1 = CInt4Lvalue(b.CreateUniformInt4("texParams1", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
		auto texParams2 = CInt4Lvalue(b.CreateUniformInt4("texParams2", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
		auto alphaFbParams = CInt4Lvalue(b.CreateUniformInt4("alphaFbParams", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
		auto fogColor = CFloat4Lvalue(b.CreateUniformFloat4("fogColor", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));

		//Uniforms
		auto mipParams0 = CInt4Lvalue(b.CreateUniformInt4("mipParams0", DESCRIPTOR_LOCATION_UNIFORM_MIPPARAMS));
		auto mipParams1 = CInt4Lvalue(b.CreateUniformInt4("mipParams1", DESCRIPTOR_LOCATION_UNIFORM_MIPPARAMS));
		auto mipParams2 = CInt4Lvalue(b.CreateUniformInt4("mipParams2", DESCRIPTOR_LOCATION_UNIFORM_MIPPARAMS));
		auto mipParams3 = CInt4Lvalue(b.CreateUniformInt4("mipParams3", DESCRIPTOR_LOCATION_UNIFORM_MIPPARAMS));
		auto mipParams4 = CFloat4Lvalue(b.CreateUniformFloat4("mipParams4", DESCRIPTOR_LOCATION_UNIFORM_MIPPARAMS));

		auto texBufAddress = CIntLvalue(b.CreateVariableInt("texBufAddress"));
		auto texBufWidth = CIntLvalue(b.CreateVariableInt("texBufWidth"));
		auto texMipLevel = CIntLvalue(b.CreateVariableInt("texMipLevel"));

		texBufAddress = texParams0->x();
		texBufWidth = texParams0->y();
		auto texSize = texParams0->zw();

		texMipLevel = texParams1->x();
		auto texCsa = texParams1->y();
		auto texA0 = ToFloat(texParams1->z()) / NewFloat(b, 255.f);
		auto texA1 = ToFloat(texParams1->w()) / NewFloat(b, 255.f);

		auto clampMin = texParams2->xy();
		auto clampMax = texParams2->zw();

		auto fbWriteMask = ToUint(alphaFbParams->x());
		auto alphaFix = alphaFbParams->y();
		auto alphaRef = alphaFbParams->z();

		auto screenPos = ToInt(inputPosition->xy());

		if(discardScanMask)
		{
			auto scanKeep = ((screenPos->y() & NewInt(b, 1)) == NewInt(b, (caps.scanMask == 2) ? 1 : 0));
			BeginIf(b, !scanKeep);
			{
				Return(b);
			}
			EndIf(b);
		}

		auto emitShadeColor = [&](CInt4Lvalue& srcIColor) {
			auto textureColor = CFloat4Lvalue(b.CreateVariableFloat("textureColor"));
			textureColor = NewFloat4(b, 1, 1, 1, 1);

			if(caps.hasTexture)
			{
				auto clampCoordinates =
				    [&](CInt2Value textureIuv) {
					    auto clampU = CDrawUtils::ClampTexCoord(b, caps.texClampU, textureIuv->x(), texSize->x(), clampMin->x(), clampMax->x());
					    auto clampV = CDrawUtils::ClampTexCoord(b, caps.texClampV, textureIuv->y(), texSize->y(), clampMin->y(), clampMax->y());
					    return NewInt2(clampU, clampV);
				    };

				auto getTextureColor =
				    [&](CInt2Value mipIuv, CFloat4Lvalue& textureColor) {
					    textureColor = CDrawUtils::GetTextureColor(b, caps.textureFormat, caps.clutFormat, mipIuv,
					                                               memoryBuffer, clutBuffer, texSwizzleTable, texBufAddress, texBufWidth, texCsa);
					    if(caps.textureHasAlpha)
					    {
						    CDrawUtils::ExpandAlpha(b, caps.textureFormat, caps.clutFormat, caps.textureBlackIsTransparent, textureColor, texA0, texA1);
					    }
				    };

				auto invQ = NewFloat(b, 1.0f) / inputTexCoord->z();
				auto textureSt = CFloat2Lvalue(b.CreateVariableFloat("textureSt"));
				textureSt = inputTexCoord->xy() * NewFloat2(invQ, invQ);

				if(caps.textureUseDynamicMipLOD)
				{
					auto mipBuf1 = mipParams0->xy();
					auto mipBuf2 = mipParams0->zw();
					auto mipBuf3 = mipParams1->xy();
					auto mipBuf4 = mipParams1->zw();
					auto mipBuf5 = mipParams2->xy();
					auto mipBuf6 = mipParams2->zw();
					auto maxMip = mipParams3->x();
					auto mipL = mipParams3->y();
					auto mipK = mipParams4->x();

					auto lodBias = NewFloat(b, -0.03125f);

					auto expandedMipL = ToFloat(NewInt(b, 1) << mipL);
					auto mipLevelP = Log2(Abs(invQ));
					auto mipLevelR = Trunc((mipLevelP * expandedMipL) + mipK + lodBias);
					auto mipLevel = Min(ToInt(mipLevelR), maxMip);

#define ASSIGN_MIP(a)                     \
	BeginIf(b, mipLevel == NewInt(b, a)); \
	{                                     \
		texMipLevel = NewInt(b, a);       \
		texBufAddress = mipBuf##a->x();   \
		texBufWidth = mipBuf##a->y();     \
	}                                     \
	EndIf(b);
					ASSIGN_MIP(1);
					ASSIGN_MIP(2);
					ASSIGN_MIP(3);
					ASSIGN_MIP(4);
					ASSIGN_MIP(5);
					ASSIGN_MIP(6);
#undef ASSIGN_MIP
				}

				if(caps.textureUseLinearFiltering)
				{
					auto textureLinearPos = CFloat2Lvalue(b.CreateVariableFloat("textureLinearPos"));
					auto textureLinearAb = CFloat2Lvalue(b.CreateVariableFloat("textureLinearAb"));
					auto textureIuv0 = CInt2Lvalue(b.CreateVariableInt("textureIuv0"));
					auto textureIuv1 = CInt2Lvalue(b.CreateVariableInt("textureIuv1"));
					auto textureColorA = CFloat4Lvalue(b.CreateVariableFloat("textureColorA"));
					auto textureColorB = CFloat4Lvalue(b.CreateVariableFloat("textureColorB"));
					auto textureColorC = CFloat4Lvalue(b.CreateVariableFloat("textureColorC"));
					auto textureColorD = CFloat4Lvalue(b.CreateVariableFloat("textureColorD"));

					textureLinearPos = (textureSt * ToFloat(texSize)) + NewFloat2(b, -0.5f, -0.5f);
					textureLinearAb = Mix(Fract(textureLinearPos), NewFloat2(b, 0, 0), IsInf(textureSt));

					textureIuv0 = ToInt(textureLinearPos);
					textureIuv1 = textureIuv0 + NewInt2(b, 1, 1);

					auto textureClampIuv0 = clampCoordinates(textureIuv0);
					auto textureClampIuv1 = clampCoordinates(textureIuv1);
					auto mipIuv0 = NewInt2(textureClampIuv0->x() >> texMipLevel, textureClampIuv0->y() >> texMipLevel);
					auto mipIuv1 = NewInt2(textureClampIuv1->x() >> texMipLevel, textureClampIuv1->y() >> texMipLevel);

					if(CDrawUtils::GetTextureColorQuad(b, caps.textureFormat, caps.clutFormat, mipIuv0, mipIuv1,
					                                   memoryBuffer, clutBuffer, texBufAddress, texBufWidth, texCsa,
					                                   textureColorA, textureColorB, textureColorC, textureColorD))
					{
						if(caps.textureHasAlpha)
						{
							CDrawUtils::ExpandAlpha(b, caps.textureFormat, caps.clutFormat, caps.textureBlackIsTransparent, textureColorA, texA0, texA1);
							CDrawUtils::ExpandAlpha(b, caps.textureFormat, caps.clutFormat, caps.textureBlackIsTransparent, textureColorB, texA0, texA1);
							CDrawUtils::ExpandAlpha(b, caps.textureFormat, caps.clutFormat, caps.textureBlackIsTransparent, textureColorC, texA0, texA1);
							CDrawUtils::ExpandAlpha(b, caps.textureFormat, caps.clutFormat, caps.textureBlackIsTransparent, textureColorD, texA0, texA1);
						}
					}
					else
					{
						getTextureColor(NewInt2(mipIuv0->x(), mipIuv0->y()), textureColorA);
						getTextureColor(NewInt2(mipIuv1->x(), mipIuv0->y()), textureColorB);
						getTextureColor(NewInt2(mipIuv0->x(), mipIuv1->y()), textureColorC);
						getTextureColor(NewInt2(mipIuv1->x(), mipIuv1->y()), textureColorD);
					}

					auto fracX = textureLinearAb->x()->xxxx();
					auto fracY = textureLinearAb->y()->xxxx();
					auto topRow = textureColorA + (textureColorB - textureColorA) * fracX;
					auto botRow = textureColorC + (textureColorD - textureColorC) * fracX;
					textureColor = topRow + (botRow - topRow) * fracY;
				}
				else
				{
					auto texelPos = ToInt(textureSt * ToFloat(texSize));
					auto clampTexPos = clampCoordinates(texelPos);
					auto mipTexPos = NewInt2(clampTexPos->x() >> texMipLevel, clampTexPos->y() >> texMipLevel);
					getTextureColor(mipTexPos, textureColor);
				}

				switch(caps.textureFunction)
				{
				case CGSHandler::TEX0_FUNCTION_MODULATE:
					textureColor = textureColor * inputColor * NewFloat4(b, 2, 2, 2, 2);
					textureColor = Clamp(textureColor, NewFloat4(b, 0, 0, 0, 0), NewFloat4(b, 1, 1, 1, 1));
					if(!caps.textureHasAlpha)
					{
						textureColor = NewFloat4(textureColor->xyz(), inputColor->w());
					}
					break;
				case CGSHandler::TEX0_FUNCTION_DECAL:
					if(!caps.textureHasAlpha)
					{
						textureColor = NewFloat4(textureColor->xyz(), inputColor->w());
					}
					break;
				case CGSHandler::TEX0_FUNCTION_HIGHLIGHT:
				{
					auto tempColor = (textureColor->xyz() * inputColor->xyz() * NewFloat3(b, 2, 2, 2)) + inputColor->www();
					if(caps.textureHasAlpha)
					{
						textureColor = NewFloat4(tempColor, inputColor->w() + textureColor->w());
					}
					else
					{
						textureColor = NewFloat4(tempColor, inputColor->w());
					}
					textureColor = Clamp(textureColor, NewFloat4(b, 0, 0, 0, 0), NewFloat4(b, 1, 1, 1, 1));
				}
				break;
				case CGSHandler::TEX0_FUNCTION_HIGHLIGHT2:
				{
					auto tempColor = (textureColor->xyz() * inputColor->xyz() * NewFloat3(b, 2, 2, 2)) + inputColor->www();
					if(caps.textureHasAlpha)
					{
						textureColor = NewFloat4(tempColor, textureColor->w());
					}
					else
					{
						textureColor = NewFloat4(tempColor, inputColor->w());
					}
					textureColor = Clamp(textureColor, NewFloat4(b, 0, 0, 0, 0), NewFloat4(b, 1, 1, 1, 1));
				}
				break;
				default:
					assert(false);
					break;
				}
			}
			else
			{
				textureColor = inputColor->xyzw();
			}

			if(caps.hasFog)
			{
				auto fogMixColor = Mix(textureColor->xyz(), fogColor->xyz(), inputFog->xxx());
				textureColor = NewFloat4(fogMixColor, textureColor->w());
			}

			srcIColor = ToInt(textureColor->xyzw() * NewFloat4(b, 255.f, 255.f, 255.f, 255.f));
		};

		auto emitAlphaDiscard = [&](CInt4Lvalue& srcIColor) {
			auto srcAlpha = srcIColor->w();
			auto alphaPass = CBoolLvalue(b.CreateVariableBool("alphaPass"));
			switch(caps.alphaTestFunction)
			{
			default:
				assert(false);
				[[fallthrough]];
			case CGSHandler::ALPHA_TEST_NEVER:
				alphaPass = NewBool(b, false);
				break;
			case CGSHandler::ALPHA_TEST_LESS:
				alphaPass = srcAlpha < alphaRef;
				break;
			case CGSHandler::ALPHA_TEST_LEQUAL:
				alphaPass = srcAlpha <= alphaRef;
				break;
			case CGSHandler::ALPHA_TEST_EQUAL:
				alphaPass = srcAlpha == alphaRef;
				break;
			case CGSHandler::ALPHA_TEST_GEQUAL:
				alphaPass = srcAlpha >= alphaRef;
				break;
			case CGSHandler::ALPHA_TEST_GREATER:
				alphaPass = srcAlpha > alphaRef;
				break;
			case CGSHandler::ALPHA_TEST_NOTEQUAL:
				alphaPass = srcAlpha != alphaRef;
				break;
			}
			BeginIf(b, !alphaPass);
			{
				Return(b);
			}
			EndIf(b);
		};

		if(!useSoftwarePath)
		{
			//Hardware fast path: hardware depth test/write + hardware alpha blend, zero subpassLoad, zero barriers.
			auto srcIColor = CInt4Lvalue(b.CreateVariableInt("srcIColor"));
			emitShadeColor(srcIColor);

			if(discardAlphaKeep)
			{
				emitAlphaDiscard(srcIColor);
			}

			auto srcRgbF = ToFloat(srcIColor)->xyz();
			auto factorC = CFloatLvalue(b.CreateVariableFloat("factorC"));
			if(hwBlend.premulSrcByC || hwBlend.premulSrcByOneMinusC || useDualSrc)
			{
				if(caps.alphaC == CGSHandler::ALPHABLEND_C_FIX)
				{
					factorC = ToFloat(alphaFix) * NewFloat(b, 1.0f / 128.0f);
				}
				else
				{
					factorC = ToFloat(srcIColor->w()) * NewFloat(b, 1.0f / 128.0f);
				}
			}

			auto outAlphaI = CIntLvalue(b.CreateVariableInt("outAlphaI"));
			if(caps.fba)
			{
				outAlphaI = srcIColor->w() | NewInt(b, 0x80);
			}
			else
			{
				outAlphaI = srcIColor->w();
			}
			auto outAlphaF = ToFloat(outAlphaI) * NewFloat(b, 1.0f / 128.0f);

			if(hwBlend.zeroSrcColor)
			{
				outputColor = NewFloat4(NewFloat3(b, 0.f, 0.f, 0.f), outAlphaF);
			}
			else if(hwBlend.premulSrcByC)
			{
				outputColor = NewFloat4(srcRgbF * factorC->xxx(), outAlphaF);
			}
			else if(hwBlend.premulSrcByOneMinusC)
			{
				auto oneMinusC = NewFloat(b, 1.0f) - factorC;
				outputColor = NewFloat4(srcRgbF * oneMinusC->xxx(), outAlphaF);
			}
			else
			{
				outputColor = NewFloat4(srcRgbF, outAlphaF);
			}
			if(useDualSrc)
			{
				auto outputColor1 = CFloat4Lvalue(outputSecondary);
				outputColor1 = NewFloat4(factorC, factorC, factorC, factorC);
			}
		}
		else
		{
			//Software fallback path for exotic blends, maskColor, dstAlphaTest, or split color/depth alpha fail actions.
			auto dstColorF = CFloat4Lvalue(b.CreateVariableFloat("dstColorF"));
			dstColorF = Load(CSubpassInputValue(subpassColorInput), NewInt2(b, 0, 0));

			auto finalColorF = CFloat4Lvalue(b.CreateVariableFloat("finalColorF"));
			finalColorF = dstColorF->xyzw();

			auto dstDepthF = CFloatLvalue(b.CreateVariableFloat("dstDepthF"));
			auto finalDepthF = CFloatLvalue(b.CreateVariableFloat("finalDepthF"));
			if(writesFragDepth)
			{
				dstDepthF = Load(CSubpassInputValue(subpassDepthInput), NewInt2(b, 0, 0))->x();
				finalDepthF = dstDepthF->x();

				auto depthTestResult = CBoolLvalue(b.CreateVariableBool("depthTestResult"));
				switch(caps.depthTestFunction)
				{
				case CGSHandler::DEPTH_TEST_ALWAYS:
					depthTestResult = NewBool(b, true);
					break;
				case CGSHandler::DEPTH_TEST_NEVER:
					depthTestResult = NewBool(b, false);
					break;
				case CGSHandler::DEPTH_TEST_GEQUAL:
					depthTestResult = !(inputDepth->x() < dstDepthF);
					break;
				case CGSHandler::DEPTH_TEST_GREATER:
					depthTestResult = dstDepthF < inputDepth->x();
					break;
				}
				if(caps.depthTestFunction != CGSHandler::DEPTH_TEST_ALWAYS)
				{
					BeginIf(b, depthTestResult);
				}
			}

			{
				auto srcIColor = CInt4Lvalue(b.CreateVariableInt("srcIColor"));
				emitShadeColor(srcIColor);

				if(discardAlphaKeep)
				{
					emitAlphaDiscard(srcIColor);
				}

				auto writeColor = CBoolLvalue(b.CreateVariableBool("writeColor"));
				auto writeDepth = CBoolLvalue(b.CreateVariableBool("writeDepth"));
				auto writeAlpha = CBoolLvalue(b.CreateVariableBool("writeAlpha"));
				writeColor = NewBool(b, true);
				writeDepth = NewBool(b, true);
				writeAlpha = NewBool(b, true);

				if(writesFragDepth && (caps.scanMask != 0))
				{
					auto scanKeep = ((screenPos->y() & NewInt(b, 1)) == NewInt(b, (caps.scanMask == 2) ? 1 : 0));
					writeColor = scanKeep;
					writeDepth = scanKeep;
				}

				if(!discardAlphaKeep && (caps.alphaTestFunction != CGSHandler::ALPHA_TEST_ALWAYS))
				{
					CDrawUtils::AlphaTest(b, caps.alphaTestFunction, caps.alphaTestFailAction, srcIColor, alphaRef,
					                      writeColor, writeDepth, writeAlpha);
				}

				auto dstScaled = NewFloat4(dstColorF->xyz(), dstColorF->w() * NewFloat(b, 128.0f) + NewFloat(b, 0.5f));
				auto dstClamped = Clamp(dstScaled, NewFloat4(b, 0.f, 0.f, 0.f, 0.f), NewFloat4(b, 255.f, 255.f, 255.f, 255.f));
				auto dstIColor = CInt4Lvalue(b.CreateVariableInt("dstIColor"));
				dstIColor = ToInt(dstClamped);

				auto dstPixel = CUintLvalue(b.CreateVariableUint("dstPixel"));
				if(caps.hasDstAlphaTest || caps.maskColor)
				{
					switch(caps.framebufferFormat)
					{
					default:
						assert(false);
						[[fallthrough]];
					case CGSHandler::PSMCT32:
					case CGSHandler::PSMCT24:
					case CGSHandler::PSMZ32:
					case CGSHandler::PSMZ24:
						dstPixel = CMemoryUtils::IVec4ToPSM32(b, dstIColor);
						break;
					case CGSHandler::PSMCT16:
					case CGSHandler::PSMCT16S:
					case CGSHandler::PSMZ16:
					case CGSHandler::PSMZ16S:
						dstPixel = CMemoryUtils::IVec4ToPSM16(b, dstIColor);
						break;
					}
				}

				if(caps.hasDstAlphaTest)
				{
					CDrawUtils::DestinationAlphaTest(b, caps.framebufferFormat, caps.dstAlphaTestRef, dstPixel, writeColor, writeDepth);
				}

				auto finalIColor = CInt4Lvalue(b.CreateVariableInt("finalIColor"));
				if(caps.hasAlphaBlending)
				{
					auto alphaA = CDrawUtils::GetAlphaABD(b, caps.alphaA, srcIColor, dstIColor);
					auto alphaB = CDrawUtils::GetAlphaABD(b, caps.alphaB, srcIColor, dstIColor);
					auto alphaC = CDrawUtils::GetAlphaC(b, caps.alphaC, srcIColor, dstIColor, alphaFix);
					auto alphaD = CDrawUtils::GetAlphaABD(b, caps.alphaD, srcIColor, dstIColor);

					auto blendedRGB = ShiftRightArithmetic((alphaA - alphaB) * alphaC, NewInt3(b, 7, 7, 7)) + alphaD;
					auto blendedIColor = NewInt4(blendedRGB, srcIColor->w());
					if(caps.colClamp)
					{
						finalIColor = Clamp(blendedIColor, NewInt4(b, 0, 0, 0, 0), NewInt4(b, 255, 255, 255, 255));
					}
					else
					{
						finalIColor = blendedIColor & NewInt4(b, 255, 255, 255, 255);
					}
				}
				else
				{
					finalIColor = srcIColor->xyzw();
				}

				if(caps.fba)
				{
					finalIColor = NewInt4(finalIColor->xyz(), finalIColor->w() | NewInt(b, 0x80));
				}

				bool canDiscardAlpha =
				    (caps.alphaTestFunction != CGSHandler::ALPHA_TEST_ALWAYS) &&
				    (caps.alphaTestFailAction == CGSHandler::ALPHA_TEST_FAIL_RGBONLY);
				if(canDiscardAlpha)
				{
					BeginIf(b, !writeAlpha);
					{
						finalIColor = NewInt4(finalIColor->xyz(), dstIColor->w());
					}
					EndIf(b);
				}

				if(caps.maskColor)
				{
					auto packedPixel = CUintLvalue(b.CreateVariableUint("packedPixel"));
					switch(caps.framebufferFormat)
					{
					default:
						assert(false);
						[[fallthrough]];
					case CGSHandler::PSMCT32:
					case CGSHandler::PSMCT24:
					case CGSHandler::PSMZ32:
					case CGSHandler::PSMZ24:
						packedPixel = CMemoryUtils::IVec4ToPSM32(b, finalIColor);
						packedPixel = (packedPixel & fbWriteMask) | (dstPixel & ~fbWriteMask);
						finalIColor = CMemoryUtils::PSM32ToIVec4(b, packedPixel);
						break;
					case CGSHandler::PSMCT16:
					case CGSHandler::PSMCT16S:
					case CGSHandler::PSMZ16:
					case CGSHandler::PSMZ16S:
						packedPixel = CMemoryUtils::IVec4ToPSM16(b, finalIColor);
						packedPixel = (packedPixel & fbWriteMask) | (dstPixel & ~fbWriteMask);
						finalIColor = CMemoryUtils::PSM16ToIVec4(b, packedPixel);
						break;
					}
				}

				BeginIf(b, writeColor);
				{
					finalColorF = ToFloat(finalIColor) * NewFloat4(b, 1.0f, 1.0f, 1.0f, 1.0f / 128.0f);
				}
				EndIf(b);

				if(writesFragDepth)
				{
					BeginIf(b, writeDepth);
					{
						finalDepthF = inputDepth->x();
					}
					EndIf(b);
				}
			}

			if(writesFragDepth && (caps.depthTestFunction != CGSHandler::DEPTH_TEST_ALWAYS))
			{
				EndIf(b);
			}

			outputColor = finalColorF->xyzw();
			if(writesFragDepth)
			{
				auto outputDepth = CFloat4Lvalue(outputSecondary);
				outputDepth = NewFloat4(finalDepthF, finalDepthF, finalDepthF, finalDepthF);
			}
		}
	}

		Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_FRAGMENT);
	});
}

CDrawMobile::PIPELINE_CAPS CDrawMobile::MakeLoadStorePipelineCaps(const PIPELINE_CAPS& caps)
{
	auto strippedCaps = make_convertible<PIPELINE_CAPS>(0);
	strippedCaps.framebufferFormat = caps.framebufferFormat;
	strippedCaps.depthbufferFormat = caps.depthbufferFormat;
	strippedCaps.writeDepth = caps.writeDepth;
	return strippedCaps;
}

std::pair<uint64, uint64> CDrawMobile::GetBufferRange(uint32 bufAddr, uint32 bufWidth, uint32 psm, uint32 rows)
{
	auto pageSize = CGsPixelFormats::GetPsmPageSize(psm);
	uint32 effWidth = (bufWidth != 0) ? bufWidth : pageSize.first;
	uint32 pagesPerRow = std::max<uint32>((effWidth + pageSize.first - 1) / pageSize.first, 1);
	uint32 pageRows = std::max<uint32>((rows + pageSize.second - 1) / pageSize.second, 1);
	uint64 start = bufAddr;
	uint64 end = start + static_cast<uint64>(pageRows) * pagesPerRow * CGsPixelFormats::PAGESIZE;
	return std::make_pair(start, end);
}

bool CDrawMobile::RangesOverlap(const std::pair<uint64, uint64>& a, const std::pair<uint64, uint64>& b)
{
	if((a.second > CGSHandler::RAMSIZE) || (b.second > CGSHandler::RAMSIZE))
	{
		//Check wrapped range in 4 MB GS RAM
		auto overlapsWrapped = [](const std::pair<uint64, uint64>& r1, const std::pair<uint64, uint64>& r2) {
			uint64 s1 = r1.first & (CGSHandler::RAMSIZE - 1);
			uint64 len1 = std::min<uint64>(r1.second - r1.first, CGSHandler::RAMSIZE);
			uint64 s2 = r2.first & (CGSHandler::RAMSIZE - 1);
			uint64 len2 = std::min<uint64>(r2.second - r2.first, CGSHandler::RAMSIZE);
			auto check = [](uint64 a0, uint64 a1, uint64 b0, uint64 b1) { return (a0 < b1) && (b0 < a1); };
			uint64 a1 = s1 + len1;
			uint64 b1 = s2 + len2;
			if(check(s1, std::min<uint64>(a1, CGSHandler::RAMSIZE), s2, std::min<uint64>(b1, CGSHandler::RAMSIZE))) return true;
			if(a1 > CGSHandler::RAMSIZE && check(0, a1 - CGSHandler::RAMSIZE, s2, std::min<uint64>(b1, CGSHandler::RAMSIZE))) return true;
			if(b1 > CGSHandler::RAMSIZE && check(s1, std::min<uint64>(a1, CGSHandler::RAMSIZE), 0, b1 - CGSHandler::RAMSIZE)) return true;
			if(a1 > CGSHandler::RAMSIZE && b1 > CGSHandler::RAMSIZE) return true;
			return false;
		};
		return overlapsWrapped(a, b);
	}
	return (a.first < b.second) && (b.first < a.second);
}

bool CDrawMobile::FramebufferOverlapsDepthbuffer() const
{
	uint32 rows = m_scissorY + m_scissorHeight;
	auto fb = GetBufferRange(m_pushConstants.fbBufAddr, m_pushConstants.fbBufWidth, m_pipelineCaps.framebufferFormat, rows);
	auto depth = GetBufferRange(m_pushConstants.depthBufAddr, m_pushConstants.depthBufWidth, m_pipelineCaps.depthbufferFormat, rows);
	return RangesOverlap(fb, depth);
}

bool CDrawMobile::TextureOverlapsDepthbuffer() const
{
	uint32 depthFmt = m_passHasDepth ? m_passDepthFormat : m_pipelineCaps.depthbufferFormat;
	uint32 depthAddr = m_passHasDepth ? m_passDepthBufAddr : m_pushConstants.depthBufAddr;
	uint32 depthWidth = m_passHasDepth ? m_passDepthBufWidth : m_pushConstants.depthBufWidth;
	if(CGsPixelFormats::IsPsmUpperByte(m_pipelineCaps.textureFormat) && CGsPixelFormats::IsPsm24Bits(depthFmt))
	{
		return false;
	}
	uint32 depthRows = std::clamp<int32>(static_cast<int32>(std::ceil(m_renderPassMaxY + 0.5f)), 0, static_cast<int32>(m_scissorY + m_scissorHeight));
	auto depth = GetBufferRange(depthAddr, depthWidth, depthFmt, depthRows);
	auto tex = GetBufferRange(m_pushConstants.texBufAddr, m_pushConstants.texBufWidth, m_pipelineCaps.textureFormat, m_pushConstants.texHeight);
	if(RangesOverlap(tex, depth)) return true;
	if(m_pipelineCaps.textureUseDynamicMipLOD && (m_mipParamsIndex < MAX_MIPPARAMS_COUNT))
	{
		const auto& mipParams = m_mipParamsCpuCache[m_mipParamsIndex];
		uint32 mipHeight = m_pushConstants.texHeight;
		for(uint32 level = 1; level <= mipParams.maxMip && level <= 6; level++)
		{
			mipHeight = std::max<uint32>(mipHeight >> 1, 1);
			auto mip = GetBufferRange(mipParams.mipBufs[level - 1].first, mipParams.mipBufs[level - 1].second, m_pipelineCaps.textureFormat, mipHeight);
			if(RangesOverlap(mip, depth)) return true;
		}
	}
	return false;
}

bool CDrawMobile::TextureOverlapsFramebuffer() const
{
	uint32 fbFmt = !m_pendingDraws.empty() ? m_passFbFormat : m_pipelineCaps.framebufferFormat;
	if(CGsPixelFormats::IsPsmUpperByte(m_pipelineCaps.textureFormat) && CGsPixelFormats::IsPsm24Bits(fbFmt))
	{
		return false;
	}
	uint32 fbRows = std::clamp<int32>(static_cast<int32>(std::ceil(m_renderPassMaxY + 0.5f)), 0, static_cast<int32>(m_scissorY + m_scissorHeight));
	auto fb = GetBufferRange(m_pushConstants.fbBufAddr, m_pushConstants.fbBufWidth, fbFmt, fbRows);
	auto tex = GetBufferRange(m_pushConstants.texBufAddr, m_pushConstants.texBufWidth, m_pipelineCaps.textureFormat, m_pushConstants.texHeight);
	if(RangesOverlap(tex, fb)) return true;
	if(m_pipelineCaps.textureUseDynamicMipLOD && (m_mipParamsIndex < MAX_MIPPARAMS_COUNT))
	{
		const auto& mipParams = m_mipParamsCpuCache[m_mipParamsIndex];
		uint32 mipHeight = m_pushConstants.texHeight;
		for(uint32 level = 1; level <= mipParams.maxMip && level <= 6; level++)
		{
			mipHeight = std::max<uint32>(mipHeight >> 1, 1);
			auto mip = GetBufferRange(mipParams.mipBufs[level - 1].first, mipParams.mipBufs[level - 1].second, m_pipelineCaps.textureFormat, mipHeight);
			if(RangesOverlap(mip, fb)) return true;
		}
	}
	return false;
}

PIPELINE CDrawMobile::CreateLoadColorPipeline(uint32 fbFormat)
{
	PIPELINE loadPipeline;
	loadPipeline.ownsLayouts = false;
	loadPipeline.descriptorSetLayout = m_loadStoreLayout.descriptorSetLayout;
	loadPipeline.pipelineLayout = m_loadStoreLayout.pipelineLayout;

	auto vertexShader = CreateLoadStoreVertexShader();
	auto fragmentShader = CreateLoadColorFragmentShader(fbFormat);

	auto inputAssemblyInfo = Framework::Vulkan::PipelineInputAssemblyStateCreateInfo();
	inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	auto vertexInputInfo = Framework::Vulkan::PipelineVertexInputStateCreateInfo();
	auto rasterStateInfo = Framework::Vulkan::PipelineRasterizationStateCreateInfo();
	rasterStateInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterStateInfo.cullMode = VK_CULL_MODE_NONE;
	rasterStateInfo.lineWidth = 1.0f;

	VkPipelineColorBlendAttachmentState blendAttachmentState = {};
	blendAttachmentState.colorWriteMask = 0xF;
	auto colorBlendStateInfo = Framework::Vulkan::PipelineColorBlendStateCreateInfo();
	colorBlendStateInfo.attachmentCount = 1;
	colorBlendStateInfo.pAttachments = &blendAttachmentState;

	auto viewportStateInfo = Framework::Vulkan::PipelineViewportStateCreateInfo();
	viewportStateInfo.viewportCount = 1;
	viewportStateInfo.scissorCount = 1;

	auto depthStencilStateInfo = Framework::Vulkan::PipelineDepthStencilStateCreateInfo();
	auto multisampleStateInfo = Framework::Vulkan::PipelineMultisampleStateCreateInfo();
	multisampleStateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	static const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	auto dynamicStateInfo = Framework::Vulkan::PipelineDynamicStateCreateInfo();
	dynamicStateInfo.pDynamicStates = dynamicStates;
	dynamicStateInfo.dynamicStateCount = 2;

	VkPipelineShaderStageCreateInfo shaderStages[2] = {
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	};
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = vertexShader;
	shaderStages[0].pName = "main";
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = fragmentShader;
	shaderStages[1].pName = "main";

	auto pipelineCreateInfo = Framework::Vulkan::GraphicsPipelineCreateInfo();
	pipelineCreateInfo.stageCount = 2;
	pipelineCreateInfo.subpass = 0;
	pipelineCreateInfo.pStages = shaderStages;
	pipelineCreateInfo.pInputAssemblyState = &inputAssemblyInfo;
	pipelineCreateInfo.pVertexInputState = &vertexInputInfo;
	pipelineCreateInfo.pRasterizationState = &rasterStateInfo;
	pipelineCreateInfo.pColorBlendState = &colorBlendStateInfo;
	pipelineCreateInfo.pViewportState = &viewportStateInfo;
	pipelineCreateInfo.pDepthStencilState = &depthStencilStateInfo;
	pipelineCreateInfo.pMultisampleState = &multisampleStateInfo;
	pipelineCreateInfo.pDynamicState = &dynamicStateInfo;
	pipelineCreateInfo.renderPass = m_rpLoadColor;
	pipelineCreateInfo.layout = loadPipeline.pipelineLayout;

	auto result = m_context->device.vkCreateGraphicsPipelines(m_context->device, m_context->pipelineCache, 1, &pipelineCreateInfo, nullptr, &loadPipeline.pipeline);
	CHECKVULKANERROR(result);
	return loadPipeline;
}

PIPELINE CDrawMobile::CreateStoreColorPipeline(uint32 fbFormat)
{
	PIPELINE storePipeline;
	storePipeline.ownsLayouts = false;
	storePipeline.descriptorSetLayout = m_loadStoreLayout.descriptorSetLayout;
	storePipeline.pipelineLayout = m_loadStoreLayout.pipelineLayout;

	auto vertexShader = CreateLoadStoreVertexShader();
	auto fragmentShader = CreateStoreColorFragmentShader(fbFormat);

	auto inputAssemblyInfo = Framework::Vulkan::PipelineInputAssemblyStateCreateInfo();
	inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	auto vertexInputInfo = Framework::Vulkan::PipelineVertexInputStateCreateInfo();
	auto rasterStateInfo = Framework::Vulkan::PipelineRasterizationStateCreateInfo();
	rasterStateInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterStateInfo.cullMode = VK_CULL_MODE_NONE;
	rasterStateInfo.lineWidth = 1.0f;

	auto colorBlendStateInfo = Framework::Vulkan::PipelineColorBlendStateCreateInfo();
	colorBlendStateInfo.attachmentCount = 0;
	auto viewportStateInfo = Framework::Vulkan::PipelineViewportStateCreateInfo();
	viewportStateInfo.viewportCount = 1;
	viewportStateInfo.scissorCount = 1;
	auto depthStencilStateInfo = Framework::Vulkan::PipelineDepthStencilStateCreateInfo();
	auto multisampleStateInfo = Framework::Vulkan::PipelineMultisampleStateCreateInfo();
	multisampleStateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	static const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	auto dynamicStateInfo = Framework::Vulkan::PipelineDynamicStateCreateInfo();
	dynamicStateInfo.pDynamicStates = dynamicStates;
	dynamicStateInfo.dynamicStateCount = 2;

	VkPipelineShaderStageCreateInfo shaderStages[2] = {
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	};
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = vertexShader;
	shaderStages[0].pName = "main";
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = fragmentShader;
	shaderStages[1].pName = "main";

	auto pipelineCreateInfo = Framework::Vulkan::GraphicsPipelineCreateInfo();
	pipelineCreateInfo.stageCount = 2;
	pipelineCreateInfo.subpass = 0;
	pipelineCreateInfo.pStages = shaderStages;
	pipelineCreateInfo.pInputAssemblyState = &inputAssemblyInfo;
	pipelineCreateInfo.pVertexInputState = &vertexInputInfo;
	pipelineCreateInfo.pRasterizationState = &rasterStateInfo;
	pipelineCreateInfo.pColorBlendState = &colorBlendStateInfo;
	pipelineCreateInfo.pViewportState = &viewportStateInfo;
	pipelineCreateInfo.pDepthStencilState = &depthStencilStateInfo;
	pipelineCreateInfo.pMultisampleState = &multisampleStateInfo;
	pipelineCreateInfo.pDynamicState = &dynamicStateInfo;
	pipelineCreateInfo.renderPass = m_rpStoreColor;
	pipelineCreateInfo.layout = storePipeline.pipelineLayout;

	auto result = m_context->device.vkCreateGraphicsPipelines(m_context->device, m_context->pipelineCache, 1, &pipelineCreateInfo, nullptr, &storePipeline.pipeline);
	CHECKVULKANERROR(result);
	return storePipeline;
}

PIPELINE CDrawMobile::CreateLoadDepthPipeline(uint32 depthFormat)
{
	PIPELINE loadPipeline;
	loadPipeline.ownsLayouts = false;
	loadPipeline.descriptorSetLayout = m_loadStoreLayout.descriptorSetLayout;
	loadPipeline.pipelineLayout = m_loadStoreLayout.pipelineLayout;

	auto vertexShader = CreateLoadStoreVertexShader();
	auto fragmentShader = CreateLoadDepthFragmentShader(depthFormat);

	auto inputAssemblyInfo = Framework::Vulkan::PipelineInputAssemblyStateCreateInfo();
	inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	auto vertexInputInfo = Framework::Vulkan::PipelineVertexInputStateCreateInfo();
	auto rasterStateInfo = Framework::Vulkan::PipelineRasterizationStateCreateInfo();
	rasterStateInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterStateInfo.cullMode = VK_CULL_MODE_NONE;
	rasterStateInfo.lineWidth = 1.0f;

	auto colorBlendStateInfo = Framework::Vulkan::PipelineColorBlendStateCreateInfo();
	colorBlendStateInfo.attachmentCount = 0;
	auto viewportStateInfo = Framework::Vulkan::PipelineViewportStateCreateInfo();
	viewportStateInfo.viewportCount = 1;
	viewportStateInfo.scissorCount = 1;

	auto depthStencilStateInfo = Framework::Vulkan::PipelineDepthStencilStateCreateInfo();
	depthStencilStateInfo.depthTestEnable = VK_TRUE;
	depthStencilStateInfo.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	depthStencilStateInfo.depthWriteEnable = VK_TRUE;

	auto multisampleStateInfo = Framework::Vulkan::PipelineMultisampleStateCreateInfo();
	multisampleStateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	static const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	auto dynamicStateInfo = Framework::Vulkan::PipelineDynamicStateCreateInfo();
	dynamicStateInfo.pDynamicStates = dynamicStates;
	dynamicStateInfo.dynamicStateCount = 2;

	VkPipelineShaderStageCreateInfo shaderStages[2] = {
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	};
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = vertexShader;
	shaderStages[0].pName = "main";
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = fragmentShader;
	shaderStages[1].pName = "main";

	auto pipelineCreateInfo = Framework::Vulkan::GraphicsPipelineCreateInfo();
	pipelineCreateInfo.stageCount = 2;
	pipelineCreateInfo.subpass = 0;
	pipelineCreateInfo.pStages = shaderStages;
	pipelineCreateInfo.pInputAssemblyState = &inputAssemblyInfo;
	pipelineCreateInfo.pVertexInputState = &vertexInputInfo;
	pipelineCreateInfo.pRasterizationState = &rasterStateInfo;
	pipelineCreateInfo.pColorBlendState = &colorBlendStateInfo;
	pipelineCreateInfo.pViewportState = &viewportStateInfo;
	pipelineCreateInfo.pDepthStencilState = &depthStencilStateInfo;
	pipelineCreateInfo.pMultisampleState = &multisampleStateInfo;
	pipelineCreateInfo.pDynamicState = &dynamicStateInfo;
	pipelineCreateInfo.renderPass = m_rpLoadDepth;
	pipelineCreateInfo.layout = loadPipeline.pipelineLayout;

	auto result = m_context->device.vkCreateGraphicsPipelines(m_context->device, m_context->pipelineCache, 1, &pipelineCreateInfo, nullptr, &loadPipeline.pipeline);
	CHECKVULKANERROR(result);
	return loadPipeline;
}

PIPELINE CDrawMobile::CreateStoreDepthPipeline(uint32 depthFormat)
{
	PIPELINE storePipeline;
	storePipeline.ownsLayouts = false;
	storePipeline.descriptorSetLayout = m_loadStoreLayout.descriptorSetLayout;
	storePipeline.pipelineLayout = m_loadStoreLayout.pipelineLayout;

	auto vertexShader = CreateLoadStoreVertexShader();
	auto fragmentShader = CreateStoreDepthFragmentShader(depthFormat);

	auto inputAssemblyInfo = Framework::Vulkan::PipelineInputAssemblyStateCreateInfo();
	inputAssemblyInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	auto vertexInputInfo = Framework::Vulkan::PipelineVertexInputStateCreateInfo();
	auto rasterStateInfo = Framework::Vulkan::PipelineRasterizationStateCreateInfo();
	rasterStateInfo.polygonMode = VK_POLYGON_MODE_FILL;
	rasterStateInfo.cullMode = VK_CULL_MODE_NONE;
	rasterStateInfo.lineWidth = 1.0f;

	auto colorBlendStateInfo = Framework::Vulkan::PipelineColorBlendStateCreateInfo();
	colorBlendStateInfo.attachmentCount = 0;
	auto viewportStateInfo = Framework::Vulkan::PipelineViewportStateCreateInfo();
	viewportStateInfo.viewportCount = 1;
	viewportStateInfo.scissorCount = 1;
	auto depthStencilStateInfo = Framework::Vulkan::PipelineDepthStencilStateCreateInfo();
	auto multisampleStateInfo = Framework::Vulkan::PipelineMultisampleStateCreateInfo();
	multisampleStateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	static const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	auto dynamicStateInfo = Framework::Vulkan::PipelineDynamicStateCreateInfo();
	dynamicStateInfo.pDynamicStates = dynamicStates;
	dynamicStateInfo.dynamicStateCount = 2;

	VkPipelineShaderStageCreateInfo shaderStages[2] = {
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
	};
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = vertexShader;
	shaderStages[0].pName = "main";
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = fragmentShader;
	shaderStages[1].pName = "main";

	auto pipelineCreateInfo = Framework::Vulkan::GraphicsPipelineCreateInfo();
	pipelineCreateInfo.stageCount = 2;
	pipelineCreateInfo.subpass = 0;
	pipelineCreateInfo.pStages = shaderStages;
	pipelineCreateInfo.pInputAssemblyState = &inputAssemblyInfo;
	pipelineCreateInfo.pVertexInputState = &vertexInputInfo;
	pipelineCreateInfo.pRasterizationState = &rasterStateInfo;
	pipelineCreateInfo.pColorBlendState = &colorBlendStateInfo;
	pipelineCreateInfo.pViewportState = &viewportStateInfo;
	pipelineCreateInfo.pDepthStencilState = &depthStencilStateInfo;
	pipelineCreateInfo.pMultisampleState = &multisampleStateInfo;
	pipelineCreateInfo.pDynamicState = &dynamicStateInfo;
	pipelineCreateInfo.renderPass = m_rpStoreDepth;
	pipelineCreateInfo.layout = storePipeline.pipelineLayout;

	auto result = m_context->device.vkCreateGraphicsPipelines(m_context->device, m_context->pipelineCache, 1, &pipelineCreateInfo, nullptr, &storePipeline.pipeline);
	CHECKVULKANERROR(result);
	return storePipeline;
}

Framework::Vulkan::CShaderModule CDrawMobile::CreateLoadStoreVertexShader()
{
	using namespace Nuanceur;

	auto b = CShaderBuilder();
	{
		auto vertexIndex = CIntLvalue(b.CreateInputInt(Nuanceur::SEMANTIC_SYSTEM_VERTEXINDEX));
		auto outputPosition = CFloat4Lvalue(b.CreateOutput(Nuanceur::SEMANTIC_SYSTEM_POSITION));

		auto position = NewFloat2(
		    ToFloat(vertexIndex << NewInt(b, 1) & NewInt(b, 2)),
		    ToFloat(vertexIndex & NewInt(b, 2)));
		outputPosition = NewFloat4(
		    position->x() * NewFloat(b, 2) + NewFloat(b, -1),
		    position->y() * NewFloat(b, 2) + NewFloat(b, -1),
		    NewFloat(b, 0),
		    NewFloat(b, 1));
	}

	Framework::CMemStream shaderStream;
	Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_VERTEX);
	shaderStream.Seek(0, Framework::STREAM_SEEK_SET);
	return Framework::Vulkan::CShaderModule(m_context->device, shaderStream);
}

Framework::Vulkan::CShaderModule CDrawMobile::CreateLoadColorFragmentShader(uint32 fbFormat)
{
	using namespace Nuanceur;

	return m_context->GetOrCreateShaderModule(2U, 0U, static_cast<uint64>(fbFormat), CContext::SHADER_PATCH_NONE, [&](Framework::CMemStream& shaderStream) {
		auto b = CShaderBuilder();
		{
			auto inputPosition = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_SYSTEM_POSITION));
			auto outputColor = CFloat4Lvalue(b.CreateOutput(Nuanceur::SEMANTIC_SYSTEM_COLOR, 0));

			auto memoryBuffer = CArrayUintValue(b.CreateUniformArrayUint("memoryBuffer", DESCRIPTOR_LOCATION_BUFFER_MEMORY));
			auto fbSwizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_FB));
			auto fbDepthParams = CInt4Lvalue(b.CreateUniformInt4("fbDepthParams", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));

			auto fbBufAddress = fbDepthParams->x();
			auto fbBufWidth = fbDepthParams->y();
			auto dstPixel = CUintLvalue(b.CreateVariableUint("dstPixel"));
			auto fbAddress = CIntLvalue(b.CreateTemporaryInt());
			auto screenPos = ToInt(inputPosition->xy());

			switch(fbFormat)
			{
			default:
				assert(false);
			case CGSHandler::PSMCT32:
			case CGSHandler::PSMCT24:
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT32>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			case CGSHandler::PSMZ32:
			case CGSHandler::PSMZ24:
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ32>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			case CGSHandler::PSMCT16:
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT16>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			case CGSHandler::PSMCT16S:
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT16S>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			case CGSHandler::PSMZ16:
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ16>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			case CGSHandler::PSMZ16S:
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ16S>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			}
			fbAddress = fbAddress & NewInt(b, CGSHandler::RAMSIZE - 1);

			auto dstIColor = CInt4Lvalue(b.CreateVariableInt("dstIColor"));
			switch(fbFormat)
			{
			default:
				assert(false);
			case CGSHandler::PSMCT32:
			case CGSHandler::PSMZ32:
				dstPixel = CMemoryUtils::Memory_Read32(b, memoryBuffer, fbAddress);
				dstIColor = CMemoryUtils::PSM32ToIVec4(b, dstPixel);
				break;
			case CGSHandler::PSMCT24:
			case CGSHandler::PSMZ24:
				dstPixel = CMemoryUtils::Memory_Read24(b, memoryBuffer, fbAddress);
				dstIColor = CMemoryUtils::PSM32ToIVec4(b, dstPixel);
				break;
			case CGSHandler::PSMCT16:
			case CGSHandler::PSMCT16S:
			case CGSHandler::PSMZ16:
			case CGSHandler::PSMZ16S:
				dstPixel = CMemoryUtils::Memory_Read16(b, memoryBuffer, fbAddress);
				dstIColor = CMemoryUtils::PSM16ToIVec4(b, dstPixel);
				break;
			}

			outputColor = ToFloat(dstIColor) * NewFloat4(b, 1.0f, 1.0f, 1.0f, 1.0f / 128.0f);
		}
		Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_FRAGMENT);
	});
}

Framework::Vulkan::CShaderModule CDrawMobile::CreateStoreColorFragmentShader(uint32 fbFormat)
{
	using namespace Nuanceur;

	return m_context->GetOrCreateShaderModule(3U, 0U, static_cast<uint64>(fbFormat), CContext::SHADER_PATCH_NONE, [&](Framework::CMemStream& shaderStream) {
		auto b = CShaderBuilder();
		{
			auto inputPosition = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_SYSTEM_POSITION));
			bool fbIs16 = (fbFormat == CGSHandler::PSMCT16) || (fbFormat == CGSHandler::PSMCT16S) || (fbFormat == CGSHandler::PSMZ16) || (fbFormat == CGSHandler::PSMZ16S);
			bool use16BitStore = m_context->has16BitStorage && fbIs16;
			auto fbSwizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_FB));
			auto subpassColorInput = CSubpassInputValue(b.CreateSubpassInput(DESCRIPTOR_LOCATION_IMAGE_INPUT_COLOR, 0));

			auto fbDepthParams = CInt4Lvalue(b.CreateUniformInt4("fbDepthParams", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
			auto fbBufAddress = fbDepthParams->x();
			auto fbBufWidth = fbDepthParams->y();

			auto colorF = Load(subpassColorInput, NewInt2(b, 0, 0));
			auto scaledColor = NewFloat4(colorF->xyz(), colorF->w() * NewFloat(b, 128.0f) + NewFloat(b, 0.5f));
			auto clampedColor = Clamp(scaledColor, NewFloat4(b, 0.f, 0.f, 0.f, 0.f), NewFloat4(b, 255.f, 255.f, 255.f, 255.f));
			auto dstIColor = ToInt(clampedColor);

			auto dstPixel = CUintLvalue(b.CreateVariableUint("dstPixel"));
			auto fbAddress = CIntLvalue(b.CreateTemporaryInt());
			auto screenPos = ToInt(inputPosition->xy());

			switch(fbFormat)
			{
			default:
				assert(false);
			case CGSHandler::PSMCT32:
			case CGSHandler::PSMCT24:
				dstPixel = CMemoryUtils::IVec4ToPSM32(b, dstIColor);
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT32>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			case CGSHandler::PSMZ32:
			case CGSHandler::PSMZ24:
				dstPixel = CMemoryUtils::IVec4ToPSM32(b, dstIColor);
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ32>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			case CGSHandler::PSMCT16:
				dstPixel = CMemoryUtils::IVec4ToPSM16(b, dstIColor);
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT16>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			case CGSHandler::PSMCT16S:
				dstPixel = CMemoryUtils::IVec4ToPSM16(b, dstIColor);
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT16S>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			case CGSHandler::PSMZ16:
				dstPixel = CMemoryUtils::IVec4ToPSM16(b, dstIColor);
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ16>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			case CGSHandler::PSMZ16S:
				dstPixel = CMemoryUtils::IVec4ToPSM16(b, dstIColor);
				fbAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ16S>(
				    b, fbSwizzleTable, fbBufAddress, fbBufWidth, screenPos);
				break;
			}
			fbAddress = fbAddress & NewInt(b, CGSHandler::RAMSIZE - 1);

			if((fbFormat == CGSHandler::PSMCT24) || (fbFormat == CGSHandler::PSMZ24))
			{
				auto memoryBuffer = CArrayUintValue(b.CreateUniformArrayUint("memoryBuffer", DESCRIPTOR_LOCATION_BUFFER_MEMORY, Nuanceur::SYMBOL_ATTRIBUTE_COHERENT));
				auto wordAddress = fbAddress >> NewInt(b, 2);
				auto word = Load(memoryBuffer, wordAddress) & NewUint(b, 0xFF000000);
				Store(memoryBuffer, wordAddress, word | (dstPixel & NewUint(b, 0xFFFFFF)));
			}
			else if(use16BitStore)
			{
				auto mem16 = CArrayUshortValue(b.CreateUniformArrayUshort("memoryBuffer16", DESCRIPTOR_LOCATION_BUFFER_MEMORY_16BIT, Nuanceur::SYMBOL_ATTRIBUTE_COHERENT));
				CMemoryUtils::Memory_Write16(b, mem16, fbAddress, dstPixel);
			}
			else
			{
				auto memoryBuffer = CArrayUintValue(b.CreateUniformArrayUint("memoryBuffer", DESCRIPTOR_LOCATION_BUFFER_MEMORY, Nuanceur::SYMBOL_ATTRIBUTE_COHERENT));
				CDrawUtils::WriteToFramebuffer(b, fbFormat, memoryBuffer, fbAddress, dstPixel);
			}
		}
		Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_FRAGMENT);
	});
}

Framework::Vulkan::CShaderModule CDrawMobile::CreateLoadDepthFragmentShader(uint32 depthFormat)
{
	using namespace Nuanceur;

	return m_context->GetOrCreateShaderModule(4U, 0U, static_cast<uint64>(depthFormat), CContext::SHADER_PATCH_FRAG_DEPTH, [&](Framework::CMemStream& shaderStream) {
		auto b = CShaderBuilder();
		{
			auto inputPosition = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_SYSTEM_POSITION));
			//Location 1 is rewritten by SHADER_PATCH_FRAG_DEPTH to BuiltIn FragDepth
			auto outputDepth = CFloat4Lvalue(b.CreateOutput(Nuanceur::SEMANTIC_SYSTEM_COLOR, 1));

			auto memoryBuffer = CArrayUintValue(b.CreateUniformArrayUint("memoryBuffer", DESCRIPTOR_LOCATION_BUFFER_MEMORY));
			auto depthSwizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_DEPTH));
			auto fbDepthParams = CInt4Lvalue(b.CreateUniformInt4("fbDepthParams", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));

			auto depthBufAddress = fbDepthParams->z();
			auto depthBufWidth = fbDepthParams->w();
			auto screenPos = ToInt(inputPosition->xy());

			auto depthAddress = CIntLvalue(b.CreateTemporaryInt());
			switch(depthFormat)
			{
			default:
				assert(false);
			case CGSHandler::PSMZ32:
			case CGSHandler::PSMZ24:
				depthAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ32>(
				    b, depthSwizzleTable, depthBufAddress, depthBufWidth, screenPos);
				break;
			case CGSHandler::PSMZ16:
				depthAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ16>(
				    b, depthSwizzleTable, depthBufAddress, depthBufWidth, screenPos);
				break;
			case CGSHandler::PSMZ16S:
				depthAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ16S>(
				    b, depthSwizzleTable, depthBufAddress, depthBufWidth, screenPos);
				break;
			}
			depthAddress = depthAddress & NewInt(b, CGSHandler::RAMSIZE - 1);
			auto dstDepth = CDrawUtils::GetDepth(b, depthFormat, depthAddress, memoryBuffer);
			auto dstDepthF = ToFloat(dstDepth) / NewFloat(b, DEPTH_MAX);
			outputDepth = NewFloat4(dstDepthF, dstDepthF, dstDepthF, dstDepthF);
		}
		Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_FRAGMENT);
	});
}

Framework::Vulkan::CShaderModule CDrawMobile::CreateStoreDepthFragmentShader(uint32 depthFormat)
{
	using namespace Nuanceur;

	return m_context->GetOrCreateShaderModule(5U, 0U, static_cast<uint64>(depthFormat), CContext::SHADER_PATCH_NONE, [&](Framework::CMemStream& shaderStream) {
		auto b = CShaderBuilder();
		{
			auto inputPosition = CFloat4Lvalue(b.CreateInput(Nuanceur::SEMANTIC_SYSTEM_POSITION));
			bool depthIs16 = (depthFormat == CGSHandler::PSMZ16) || (depthFormat == CGSHandler::PSMZ16S);
			bool use16BitStore = m_context->has16BitStorage && depthIs16;
			auto depthSwizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_IMAGE_SWIZZLETABLE_DEPTH));
			auto subpassDepthInput = CSubpassInputValue(b.CreateSubpassInput(DESCRIPTOR_LOCATION_IMAGE_INPUT_DEPTH, 0));

			auto fbDepthParams = CInt4Lvalue(b.CreateUniformInt4("fbDepthParams", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
			auto depthBufAddress = fbDepthParams->z();
			auto depthBufWidth = fbDepthParams->w();
			auto screenPos = ToInt(inputPosition->xy());

			auto depthF = CFloatLvalue(b.CreateVariableFloat("depthF"));
			depthF = Load(subpassDepthInput, NewInt2(b, 0, 0))->x();

			auto dstDepth = CUintLvalue(b.CreateVariableUint("dstDepth"));
			dstDepth = ToUint(depthF * NewFloat(b, DEPTH_MAX));
			if(depthFormat == CGSHandler::PSMZ32)
			{
				BeginIf(b, !(depthF < NewFloat(b, 1.0f)));
				{
					dstDepth = NewUint(b, 0xFFFFFFFF);
				}
				EndIf(b);
			}

			auto depthAddress = CIntLvalue(b.CreateTemporaryInt());
			switch(depthFormat)
			{
			default:
				assert(false);
			case CGSHandler::PSMZ32:
			case CGSHandler::PSMZ24:
				depthAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ32>(
				    b, depthSwizzleTable, depthBufAddress, depthBufWidth, screenPos);
				break;
			case CGSHandler::PSMZ16:
				depthAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ16>(
				    b, depthSwizzleTable, depthBufAddress, depthBufWidth, screenPos);
				break;
			case CGSHandler::PSMZ16S:
				depthAddress = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ16S>(
				    b, depthSwizzleTable, depthBufAddress, depthBufWidth, screenPos);
				break;
			}
			depthAddress = depthAddress & NewInt(b, CGSHandler::RAMSIZE - 1);

			if(depthFormat == CGSHandler::PSMZ24)
			{
				auto memoryBuffer = CArrayUintValue(b.CreateUniformArrayUint("memoryBuffer", DESCRIPTOR_LOCATION_BUFFER_MEMORY, Nuanceur::SYMBOL_ATTRIBUTE_COHERENT));
				auto wordAddress = depthAddress >> NewInt(b, 2);
				auto word = Load(memoryBuffer, wordAddress) & NewUint(b, 0xFF000000);
				Store(memoryBuffer, wordAddress, word | (dstDepth & NewUint(b, 0xFFFFFF)));
			}
			else if(use16BitStore)
			{
				auto mem16 = CArrayUshortValue(b.CreateUniformArrayUshort("memoryBuffer16", DESCRIPTOR_LOCATION_BUFFER_MEMORY_16BIT, Nuanceur::SYMBOL_ATTRIBUTE_COHERENT));
				CMemoryUtils::Memory_Write16(b, mem16, depthAddress, dstDepth);
			}
			else
			{
				auto memoryBuffer = CArrayUintValue(b.CreateUniformArrayUint("memoryBuffer", DESCRIPTOR_LOCATION_BUFFER_MEMORY, Nuanceur::SYMBOL_ATTRIBUTE_COHERENT));
				CDrawUtils::WriteToDepthbuffer(b, depthFormat, memoryBuffer, depthAddress, dstDepth);
			}
		}
		Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_FRAGMENT);
	});
}

void CDrawMobile::EnsureDepthSlotAllocated(uint32 slotIdx)
{
	assert(slotIdx < RESIDENT_DEPTH_SLOTS);
	auto& dslot = m_residentDepths[slotIdx];
	if(dslot.allocated) return;

	dslot.image = Framework::Vulkan::CImage(m_context->device, m_context->physicalDeviceMemoryProperties,
	                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
	                                        VK_FORMAT_D32_SFLOAT, DRAW_AREA_SIZE, DRAW_AREA_SIZE, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

	{
		auto viewInfo = Framework::Vulkan::ImageViewCreateInfo();
		viewInfo.image = dslot.image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_D32_SFLOAT;
		viewInfo.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
		viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
		auto result = m_context->device.vkCreateImageView(m_context->device, &viewInfo, nullptr, &dslot.imageView);
		CHECKVULKANERROR(result);
	}

	auto makeFb = [&](VkRenderPass rp, VkFramebuffer& outFb) {
		auto fbInfo = Framework::Vulkan::FramebufferCreateInfo();
		fbInfo.renderPass = rp;
		fbInfo.width = DRAW_AREA_SIZE;
		fbInfo.height = DRAW_AREA_SIZE;
		fbInfo.layers = 1;
		fbInfo.attachmentCount = 1;
		fbInfo.pAttachments = &dslot.imageView;
		auto result = m_context->device.vkCreateFramebuffer(m_context->device, &fbInfo, nullptr, &outFb);
		CHECKVULKANERROR(result);
	};

	makeFb(m_rpLoadDepth, dslot.loadFb);
	makeFb(m_rpStoreDepth, dslot.storeFb);
	dslot.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	dslot.allocated = true;
}
