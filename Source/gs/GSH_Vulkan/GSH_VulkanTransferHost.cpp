#include "GSH_VulkanTransferHost.h"
#include "GSH_VulkanMemoryUtils.h"
#include "GSH_VulkanPlatformDefs.h"
#include "MemStream.h"
#include "vulkan/StructDefs.h"
#include "vulkan/Utils.h"
#include "nuanceur/generators/SpirvShaderGenerator.h"
#include "../GSHandler.h"

using namespace GSH_Vulkan;

#define XFER_BUFFER_SIZE 0x600000

#define DESCRIPTOR_LOCATION_MEMORY 0
#define DESCRIPTOR_LOCATION_XFERBUFFER 1
#define DESCRIPTOR_LOCATION_SWIZZLETABLE_DST 2
#define DESCRIPTOR_LOCATION_MEMORY_8BIT 3
#define DESCRIPTOR_LOCATION_MEMORY_16BIT 4

CTransferHost::CTransferHost(const ContextPtr& context, const FrameCommandBufferPtr& frameCommandBuffer)
    : m_context(context)
    , m_frameCommandBuffer(frameCommandBuffer)
    , m_pipelineCache(context->device)
{
	for(auto& frame : m_frames)
	{
		frame.xferBuffer = Framework::Vulkan::CBuffer(
		    m_context->device, m_context->physicalDeviceMemoryProperties,
		    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		    XFER_BUFFER_SIZE);

		auto result = m_context->device.vkMapMemory(m_context->device, frame.xferBuffer.GetMemory(),
		                                            0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&frame.xferBufferPtr));
		CHECKVULKANERROR(result);
	}

	m_localSize = 128; //16x8 2D workgroups (matches Adreno wave size and PS2 swizzle tile locality)
	m_pipelineCaps <<= 0;
}

CTransferHost::~CTransferHost()
{
	for(auto& frame : m_frames)
	{
		m_context->device.vkUnmapMemory(m_context->device, frame.xferBuffer.GetMemory());
	}
}

void CTransferHost::SetPipelineCaps(const PIPELINE_CAPS& pipelineCaps)
{
	m_pipelineCaps = pipelineCaps;
}

void CTransferHost::DoTransfer(const XferBuffer& inputData)
{
	DoTransfer(inputData.data(), static_cast<uint32>(inputData.size()));
}

void CTransferHost::DoTransfer(const uint8* inputData, uint32 inputSize, uint32 dstAddress, uint32 dstSize)
{
	auto result = VK_SUCCESS;

	uint32 xferBufferRemainSize = XFER_BUFFER_SIZE - m_xferBufferOffset;
	if(xferBufferRemainSize < inputSize)
	{
		m_frameCommandBuffer->Flush();
		assert((XFER_BUFFER_SIZE - m_xferBufferOffset) >= inputSize);
	}

	auto& frame = m_frames[m_frameCommandBuffer->GetCurrentFrame()];

	assert(inputSize <= XFER_BUFFER_SIZE);
	memcpy(frame.xferBufferPtr + m_xferBufferOffset, inputData, inputSize);
	assert((m_xferBufferOffset & 0x03) == 0);
	Params.xferBufferOffset = m_xferBufferOffset / 4;

	uint32 pixelCount = 0;
	switch(m_pipelineCaps.dstFormat)
	{
	default:
		assert(false);
	case CGSHandler::PSMCT32:
	case CGSHandler::PSMZ32:
		pixelCount = inputSize / 4;
		break;
	case CGSHandler::PSMCT24:
	case CGSHandler::PSMZ24:
		pixelCount = inputSize / 3;
		break;
	case CGSHandler::PSMCT16S:
	case CGSHandler::PSMCT16:
	case CGSHandler::PSMZ16:
	case CGSHandler::PSMZ16S:
		pixelCount = inputSize / 2;
		break;
	case CGSHandler::PSMT8:
	case CGSHandler::PSMT8H:
		pixelCount = inputSize;
		break;
	case CGSHandler::PSMT4:
	case CGSHandler::PSMT4HL:
	case CGSHandler::PSMT4HH:
		pixelCount = inputSize * 2;
		break;
	}

	Params.pixelCount = pixelCount;
	if((pixelCount == 0) || (Params.rrw == 0)) return;
	uint32 rows = (pixelCount + Params.rrw - 1) / Params.rrw;

	//Use zero-atomic byte-pair packing for aligned PSMT4 uploads when 8-bit SSBO storage is available:
	//(x, y) [bit1(y)==0] and (x^4, y+2) always occupy the low and high nibbles of the exact same GS RAM byte.
	auto effCaps = m_pipelineCaps;
	bool usePacked4Bit = m_context->has8BitStorage &&
	                     (effCaps.dstFormat == CGSHandler::PSMT4) &&
	                     (Params.rrw >= 8) && ((Params.rrw & 7U) == 0) &&
	                     ((Params.dsax & 7U) == 0) && ((Params.dsay & 3U) == 0) &&
	                     ((rows & 3U) == 0) && (pixelCount == (Params.rrw * rows));
	effCaps.packed4Bit = usePacked4Bit ? 1 : 0;

	//Find pipeline and create it if we've never encountered it before (O(1) flat cache)
	uint32 fmtKey = effCaps.dstFormat & 0x3FU;
	uint32 pipeKey = fmtKey | (effCaps.packed4Bit << 6);
	const PIPELINE* xferPipeline = m_pipelineFast[pipeKey];
	if(!xferPipeline)
	{
		xferPipeline = m_pipelineCache.TryGetPipeline(effCaps);
		if(!xferPipeline)
		{
			xferPipeline = m_pipelineCache.RegisterPipeline(effCaps, CreateXferPipeline(effCaps));
		}
		m_pipelineFast[pipeKey] = xferPipeline;
	}

	uint32 dispatchRows = usePacked4Bit ? (rows >> 1) : rows;
	uint32 workGroupsX = (Params.rrw + 15) / 16;
	uint32 workGroupsY = (dispatchRows + 7) / 8;

	uint32 frameIdx = m_frameCommandBuffer->GetCurrentFrame();
	VkDescriptorSet descriptorSet = m_descSetFast[frameIdx][fmtKey];
	if(descriptorSet == VK_NULL_HANDLE)
	{
		auto descriptorSetCaps = make_convertible<DESCRIPTORSET_CAPS>(0);
		descriptorSetCaps.dstPsm = m_pipelineCaps.dstFormat;
		descriptorSetCaps.frameIdx = frameIdx;
		descriptorSet = PrepareDescriptorSet(xferPipeline->descriptorSetLayout, descriptorSetCaps);
		m_descSetFast[frameIdx][fmtKey] = descriptorSet;
	}
	auto commandBuffer = m_frameCommandBuffer->GetCommandBuffer();

	//Only emit a compute barrier on the first transfer of a batch or if this transfer overlaps
	//a prior un-barriered transfer in GS memory.
	uint32 rangeStart = dstAddress;
	uint32 rangeEnd = dstAddress + dstSize;
	bool needBarrier = m_context->hostXferNeedsBarrier || (rangeEnd > CGSHandler::RAMSIZE) || (m_context->pendingHostXferRanges.size() >= 64);
	if(!needBarrier)
	{
		for(const auto& prev : m_context->pendingHostXferRanges)
		{
			if((rangeStart < prev.second) && (prev.first < rangeEnd))
			{
				needBarrier = true;
				break;
			}
		}
	}
	if(needBarrier)
	{
		auto memoryBarrier = Framework::Vulkan::MemoryBarrier();
		memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

		m_context->device.vkCmdPipelineBarrier(commandBuffer,
		                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                                       0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
		m_context->pendingHostXferRanges.clear();
		m_context->hostXferNeedsBarrier = false;
	}
	if(rangeEnd <= CGSHandler::RAMSIZE)
	{
		m_context->pendingHostXferRanges.emplace_back(rangeStart, rangeEnd);
	}
	else
	{
		m_context->hostXferNeedsBarrier = true;
	}

	m_context->clutNeedsHostBarrier = true;

	bool stateStale = (m_lastComputeBindGen != m_context->computeBindGen);
	if(stateStale || (m_lastBoundDescSet != descriptorSet))
	{
		m_context->device.vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, xferPipeline->pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
		m_lastBoundDescSet = descriptorSet;
	}
	if(stateStale || (m_lastBoundPipeline != xferPipeline->pipeline))
	{
		m_context->device.vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, xferPipeline->pipeline);
		m_lastBoundPipeline = xferPipeline->pipeline;
	}
	m_lastComputeBindGen = m_context->computeBindGen;
	m_context->device.vkCmdPushConstants(commandBuffer, xferPipeline->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(XFERPARAMS), &Params);
	m_context->device.vkCmdDispatch(commandBuffer, workGroupsX, workGroupsY, 1);

	m_xferBufferOffset += inputSize;
	m_xferBufferOffset = (m_xferBufferOffset + (m_context->storageBufferAlignment - 1)) & -m_context->storageBufferAlignment;
}

VkDescriptorSet CTransferHost::PrepareDescriptorSet(VkDescriptorSetLayout descriptorSetLayout, const DESCRIPTORSET_CAPS& caps)
{
	auto descriptorSetIterator = m_descriptorSetCache.find(caps);
	if(descriptorSetIterator != std::end(m_descriptorSetCache))
	{
		return descriptorSetIterator->second;
	}

	VkResult result = VK_SUCCESS;
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
		std::vector<VkWriteDescriptorSet> writes;

		VkDescriptorBufferInfo descriptorMemoryBufferInfo = {};
		descriptorMemoryBufferInfo.buffer = m_context->memoryBuffer;
		descriptorMemoryBufferInfo.range = VK_WHOLE_SIZE;

		VkDescriptorBufferInfo descriptorBufferInfo = {};
		descriptorBufferInfo.buffer = m_frames[caps.frameIdx].xferBuffer;
		descriptorBufferInfo.range = VK_WHOLE_SIZE;

		VkDescriptorImageInfo descriptorDstSwizzleTableInfo = {};
		descriptorDstSwizzleTableInfo.imageView = m_context->GetSwizzleTable(caps.dstPsm);
		descriptorDstSwizzleTableInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

		//Memory Image Descriptor
		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_MEMORY;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writeSet.pBufferInfo = &descriptorMemoryBufferInfo;
			writes.push_back(writeSet);
		}

		//Memory Image Descriptor 8 bit
		if(m_context->has8BitStorage)
		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_MEMORY_8BIT;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writeSet.pBufferInfo = &descriptorMemoryBufferInfo;
			writes.push_back(writeSet);
		}

		//Memory Image Descriptor 16 bit
		if(m_context->has16BitStorage)
		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_MEMORY_16BIT;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writeSet.pBufferInfo = &descriptorMemoryBufferInfo;
			writes.push_back(writeSet);
		}

		//Xfer Buffer Descriptor
		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_XFERBUFFER;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			writeSet.pBufferInfo = &descriptorBufferInfo;
			writes.push_back(writeSet);
		}

		//Dst Swizzle Table
		{
			auto writeSet = Framework::Vulkan::WriteDescriptorSet();
			writeSet.dstSet = descriptorSet;
			writeSet.dstBinding = DESCRIPTOR_LOCATION_SWIZZLETABLE_DST;
			writeSet.descriptorCount = 1;
			writeSet.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writeSet.pImageInfo = &descriptorDstSwizzleTableInfo;
			writes.push_back(writeSet);
		}

		m_context->device.vkUpdateDescriptorSets(m_context->device, writes.size(), writes.data(), 0, nullptr);
	}

	m_descriptorSetCache.insert(std::make_pair(caps, descriptorSet));

	return descriptorSet;
}

void CTransferHost::PreFlushFrameCommandBuffer()
{
}

void CTransferHost::PostFlushFrameCommandBuffer()
{
	m_xferBufferOffset = 0;
	m_context->ResetHostXferBatch();
}

Framework::Vulkan::CShaderModule CTransferHost::CreateXferShader(const PIPELINE_CAPS& caps)
{
	using namespace Nuanceur;

	auto b = CShaderBuilder();

	b.SetMetadata(CShaderBuilder::METADATA_LOCALSIZE_X, 16);
	b.SetMetadata(CShaderBuilder::METADATA_LOCALSIZE_Y, 8);

	{
		auto inputInvocationId = CInt4Lvalue(b.CreateInputInt(Nuanceur::SEMANTIC_SYSTEM_GIID));
		auto memoryBuffer = CArrayUintValue(b.CreateUniformArrayUint("memoryBuffer", DESCRIPTOR_LOCATION_MEMORY));
		auto xferBuffer = CArrayUintValue(b.CreateUniformArrayUint("xferBuffer", DESCRIPTOR_LOCATION_XFERBUFFER));
		auto dstSwizzleTable = CImageUint2DValue(b.CreateImage2DUint(DESCRIPTOR_LOCATION_SWIZZLETABLE_DST));

		auto xferParams0 = CInt4Lvalue(b.CreateUniformInt4("xferParams0", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));
		auto xferParams1 = CInt4Lvalue(b.CreateUniformInt4("xferParams1", Nuanceur::UNIFORM_UNIT_PUSHCONSTANT));

		auto bufAddress = xferParams0->x();
		auto bufWidth = xferParams0->y();
		auto rrw = xferParams0->z();
		auto dsax = xferParams0->w();
		auto dsay = xferParams1->x();
		auto xferBufferOffset = xferParams1->y();
		auto pixelCount = xferParams1->z();

		auto rrx = inputInvocationId->x();
		if(caps.packed4Bit)
		{
			auto yIdx = inputInvocationId->y();
			auto rry = ((yIdx & NewInt(b, ~1)) << NewInt(b, 1)) | (yIdx & NewInt(b, 1));
			auto pixelIndex0 = (rry * rrw) + rrx;
			auto pixelIndex1 = ((rry + NewInt(b, 2)) * rrw) + ToInt(ToUint(rrx) ^ NewUint(b, 4));
			BeginIf(b, (rrx >= rrw) || (pixelIndex1 >= pixelCount));
			{
				Return(b);
			}
			EndIf(b);

			auto trxX = (rrx + dsax) & NewInt(b, 2047);
			auto trxY = (rry + dsay) & NewInt(b, 2047);
			auto loNib = XferStream_Read4(b, xferBuffer, xferBufferOffset, pixelIndex0);
			auto hiNib = XferStream_Read4(b, xferBuffer, xferBufferOffset, pixelIndex1);
			auto byteVal = loNib | (hiNib << NewUint(b, 4));
			auto nibAddress = CMemoryUtils::GetPixelAddress_PSMT4(
			    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
			auto memoryBuffer8 = CArrayUcharValue(b.CreateUniformArrayUchar("memoryBuffer8", DESCRIPTOR_LOCATION_MEMORY_8BIT));
			CMemoryUtils::Memory_Write8(b, memoryBuffer8, nibAddress >> NewInt(b, 1), byteVal);
		}
		else
		{
			auto rry = inputInvocationId->y();
			auto pixelIndex = (rry * rrw) + rrx;

			BeginIf(b, (rrx >= rrw) || (pixelIndex >= pixelCount));
			{
				Return(b);
			}
			EndIf(b);

			auto trxX = (rrx + dsax) & NewInt(b, 2047);
			auto trxY = (rry + dsay) & NewInt(b, 2047);

			switch(caps.dstFormat)
			{
			case CGSHandler::PSMCT32:
			{
				auto input = XferStream_Read32(b, xferBuffer, xferBufferOffset, pixelIndex);
				auto address = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT32>(
				    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				CMemoryUtils::Memory_Write32(b, memoryBuffer, address, input);
			}
			break;
			case CGSHandler::PSMZ32:
			{
				auto input = XferStream_Read32(b, xferBuffer, xferBufferOffset, pixelIndex);
				auto address = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ32>(
				    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				CMemoryUtils::Memory_Write32(b, memoryBuffer, address, input);
			}
			break;
			case CGSHandler::PSMCT24:
			case CGSHandler::PSMZ24:
			{
				auto input = XferStream_Read24(b, xferBuffer, xferBufferOffset, pixelIndex);
				auto address = [&]() {
					if(caps.dstFormat == CGSHandler::PSMZ24)
					{
						return CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ32>(
						    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
					}
					return CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT32>(
					    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				}();
				if(m_context->has8BitStorage && m_context->has16BitStorage)
				{
					auto memoryBuffer8 = CArrayUcharValue(b.CreateUniformArrayUchar("memoryBuffer8", DESCRIPTOR_LOCATION_MEMORY_8BIT));
					auto memoryBuffer16 = CArrayUshortValue(b.CreateUniformArrayUshort("memoryBuffer16", DESCRIPTOR_LOCATION_MEMORY_16BIT));
					CMemoryUtils::Memory_Write24(b, memoryBuffer8, memoryBuffer16, address, input);
				}
				else
				{
					CMemoryUtils::Memory_Write24(b, memoryBuffer, address, input);
				}
			}
			break;
			case CGSHandler::PSMCT16:
			case CGSHandler::PSMCT16S:
			case CGSHandler::PSMZ16:
			case CGSHandler::PSMZ16S:
			{
				auto input = XferStream_Read16(b, xferBuffer, xferBufferOffset, pixelIndex);
				auto address = CIntLvalue(b.CreateTemporaryInt());
				if(caps.dstFormat == CGSHandler::PSMCT16)
				{
					address = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT16>(
					    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				}
				else if(caps.dstFormat == CGSHandler::PSMCT16S)
				{
					address = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT16S>(
					    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				}
				else if(caps.dstFormat == CGSHandler::PSMZ16)
				{
					address = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ16>(
					    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				}
				else
				{
					address = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMZ16S>(
					    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				}
				if(m_context->has16BitStorage)
				{
					auto memoryBuffer16 = CArrayUshortValue(b.CreateUniformArrayUshort("memoryBuffer16", DESCRIPTOR_LOCATION_MEMORY_16BIT));
					CMemoryUtils::Memory_Write16(b, memoryBuffer16, address, input);
				}
				else
				{
					CMemoryUtils::Memory_Write16(b, memoryBuffer, address, input);
				}
			}
			break;
			case CGSHandler::PSMT8:
			{
				auto input = XferStream_Read8(b, xferBuffer, xferBufferOffset, pixelIndex);
				auto address = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMT8>(
				    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				if(m_context->has8BitStorage)
				{
					auto memoryBuffer8 = CArrayUcharValue(b.CreateUniformArrayUchar("memoryBuffer8", DESCRIPTOR_LOCATION_MEMORY_8BIT));
					CMemoryUtils::Memory_Write8(b, memoryBuffer8, address, input);
				}
				else
				{
					CMemoryUtils::Memory_Write8(b, memoryBuffer, address, input);
				}
			}
			break;
			case CGSHandler::PSMT4:
			{
				auto input = XferStream_Read4(b, xferBuffer, xferBufferOffset, pixelIndex);
				auto address = CMemoryUtils::GetPixelAddress_PSMT4(
				    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				CMemoryUtils::Memory_Write4(b, memoryBuffer, address, input);
			}
			break;
			case CGSHandler::PSMT8H:
			{
				auto input = XferStream_Read8(b, xferBuffer, xferBufferOffset, pixelIndex);
				auto address = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT32>(
				    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				if(m_context->has8BitStorage)
				{
					auto memoryBuffer8 = CArrayUcharValue(b.CreateUniformArrayUchar("memoryBuffer8", DESCRIPTOR_LOCATION_MEMORY_8BIT));
					CMemoryUtils::Memory_Write8(b, memoryBuffer8, address + NewInt(b, 3), input);
				}
				else
				{
					CMemoryUtils::Memory_Write8(b, memoryBuffer, address + NewInt(b, 3), input);
				}
			}
			break;
			case CGSHandler::PSMT4HL:
			{
				auto input = XferStream_Read4(b, xferBuffer, xferBufferOffset, pixelIndex);
				auto address = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT32>(
				    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				auto nibAddress = (address + NewInt(b, 3)) * NewInt(b, 2);
				CMemoryUtils::Memory_Write4(b, memoryBuffer, nibAddress, input);
			}
			break;
			case CGSHandler::PSMT4HH:
			{
				auto input = XferStream_Read4(b, xferBuffer, xferBufferOffset, pixelIndex);
				auto address = CMemoryUtils::GetPixelAddress<CGsPixelFormats::STORAGEPSMCT32>(
				    b, dstSwizzleTable, bufAddress, bufWidth, NewInt2(trxX, trxY));
				auto nibAddress = ((address + NewInt(b, 3)) * NewInt(b, 2)) | NewInt(b, 1);
				CMemoryUtils::Memory_Write4(b, memoryBuffer, nibAddress, input);
			}
			break;
			default:
				assert(false);
				break;
			}
		}
	}

	Framework::CMemStream shaderStream;
	Nuanceur::CSpirvShaderGenerator::Generate(shaderStream, b, Nuanceur::CSpirvShaderGenerator::SHADER_TYPE_COMPUTE);
	return m_context->CreateShaderModule(shaderStream);
}

Nuanceur::CUintRvalue CTransferHost::XferStream_Read32(Nuanceur::CShaderBuilder& b, Nuanceur::CArrayUintValue xferBuffer, Nuanceur::CIntValue xferBufferOffset, Nuanceur::CIntValue pixelIndex)
{
	return Load(xferBuffer, xferBufferOffset + pixelIndex);
}

Nuanceur::CUintRvalue CTransferHost::XferStream_Read24(Nuanceur::CShaderBuilder& b, Nuanceur::CArrayUintValue xferBuffer, Nuanceur::CIntValue xferBufferOffset, Nuanceur::CIntValue pixelIndex)
{
	auto byteOffset = pixelIndex * NewInt(b, 3);
	auto byte0 = XferStream_Read8(b, xferBuffer, xferBufferOffset, byteOffset + NewInt(b, 0));
	auto byte1 = XferStream_Read8(b, xferBuffer, xferBufferOffset, byteOffset + NewInt(b, 1));
	auto byte2 = XferStream_Read8(b, xferBuffer, xferBufferOffset, byteOffset + NewInt(b, 2));
	return (byte0) | (byte1 << NewUint(b, 8)) | (byte2 << NewUint(b, 16));
}

Nuanceur::CUintRvalue CTransferHost::XferStream_Read16(Nuanceur::CShaderBuilder& b, Nuanceur::CArrayUintValue xferBuffer, Nuanceur::CIntValue xferBufferOffset, Nuanceur::CIntValue pixelIndex)
{
	auto srcOffset = pixelIndex / NewInt(b, 2);
	auto srcShift = (ToUint(pixelIndex) & NewUint(b, 1)) * NewUint(b, 16);
	return (Load(xferBuffer, xferBufferOffset + srcOffset) >> srcShift) & NewUint(b, 0xFFFF);
}

Nuanceur::CUintRvalue CTransferHost::XferStream_Read8(Nuanceur::CShaderBuilder& b, Nuanceur::CArrayUintValue xferBuffer, Nuanceur::CIntValue xferBufferOffset, Nuanceur::CIntValue pixelIndex)
{
	auto srcOffset = pixelIndex / NewInt(b, 4);
	auto srcShift = (ToUint(pixelIndex) & NewUint(b, 3)) * NewUint(b, 8);
	return (Load(xferBuffer, xferBufferOffset + srcOffset) >> srcShift) & NewUint(b, 0xFF);
}

Nuanceur::CUintRvalue CTransferHost::XferStream_Read4(Nuanceur::CShaderBuilder& b, Nuanceur::CArrayUintValue xferBuffer, Nuanceur::CIntValue xferBufferOffset, Nuanceur::CIntValue pixelIndex)
{
	auto srcOffset = pixelIndex / NewInt(b, 8);
	auto srcShift = (ToUint(pixelIndex) & NewUint(b, 7)) * NewUint(b, 4);
	return (Load(xferBuffer, xferBufferOffset + srcOffset) >> srcShift) & NewUint(b, 0xF);
}

PIPELINE CTransferHost::CreateXferPipeline(const PIPELINE_CAPS& caps)
{
	PIPELINE xferPipeline;

	auto xferShader = CreateXferShader(caps);

	VkResult result = VK_SUCCESS;

	{
		std::vector<VkDescriptorSetLayoutBinding> bindings;

		//GS memory
		{
			VkDescriptorSetLayoutBinding binding = {};
			binding.binding = DESCRIPTOR_LOCATION_MEMORY;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings.push_back(binding);
		}

		//GS memory - 8bit buffer access
		if(m_context->has8BitStorage)
		{
			VkDescriptorSetLayoutBinding binding = {};
			binding.binding = DESCRIPTOR_LOCATION_MEMORY_8BIT;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings.push_back(binding);
		}

		//GS memory - 16bit buffer access
		if(m_context->has16BitStorage)
		{
			VkDescriptorSetLayoutBinding binding = {};
			binding.binding = DESCRIPTOR_LOCATION_MEMORY_16BIT;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings.push_back(binding);
		}

		//Xfer buffer
		{
			VkDescriptorSetLayoutBinding binding = {};
			binding.binding = DESCRIPTOR_LOCATION_XFERBUFFER;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings.push_back(binding);
		}

		//Dst Swizzle Table
		{
			VkDescriptorSetLayoutBinding binding = {};
			binding.binding = DESCRIPTOR_LOCATION_SWIZZLETABLE_DST;
			binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			binding.descriptorCount = 1;
			binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings.push_back(binding);
		}

		auto createInfo = Framework::Vulkan::DescriptorSetLayoutCreateInfo();
		createInfo.bindingCount = bindings.size();
		createInfo.pBindings = bindings.data();
		result = m_context->device.vkCreateDescriptorSetLayout(m_context->device, &createInfo, nullptr, &xferPipeline.descriptorSetLayout);
		CHECKVULKANERROR(result);
	}

	{
		VkPushConstantRange pushConstantInfo = {};
		pushConstantInfo.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		pushConstantInfo.offset = 0;
		pushConstantInfo.size = sizeof(XFERPARAMS);

		auto pipelineLayoutCreateInfo = Framework::Vulkan::PipelineLayoutCreateInfo();
		pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
		pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantInfo;
		pipelineLayoutCreateInfo.setLayoutCount = 1;
		pipelineLayoutCreateInfo.pSetLayouts = &xferPipeline.descriptorSetLayout;

		result = m_context->device.vkCreatePipelineLayout(m_context->device, &pipelineLayoutCreateInfo, nullptr, &xferPipeline.pipelineLayout);
		CHECKVULKANERROR(result);
	}

	{
		auto createInfo = Framework::Vulkan::ComputePipelineCreateInfo();
		createInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		createInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		createInfo.stage.pName = "main";
		createInfo.stage.module = xferShader;
		createInfo.layout = xferPipeline.pipelineLayout;

		result = m_context->device.vkCreateComputePipelines(m_context->device, m_context->pipelineCache, 1, &createInfo, nullptr, &xferPipeline.pipeline);
		CHECKVULKANERROR(result);
	}

	return xferPipeline;
}
