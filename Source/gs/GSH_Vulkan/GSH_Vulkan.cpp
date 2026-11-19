#include "GSH_Vulkan.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include "std_experimental_map.h"
#include "../GsPixelFormats.h"
#include "../GsTransferRange.h"
#include "Log.h"
#include "AppConfig.h"
#include "GSH_VulkanPlatformDefs.h"
#include "GSH_VulkanDrawDesktop.h"
#include "GSH_VulkanDrawMobile.h"
#include "GSH_VulkanDeviceInfo.h"
#include "vulkan/Loader.h"
#include "vulkan/StructDefs.h"
#include "vulkan/StructChain.h"
#include "vulkan/Utils.h"

#define LOG_NAME ("gsh_vulkan")

using namespace GSH_Vulkan;

static uint32 MakeColor(uint8 r, uint8 g, uint8 b, uint8 a)
{
	return (a << 24) | (b << 16) | (g << 8) | (r);
}

static uint16 RGBA32ToRGBA16(uint32 inputColor)
{
	uint32 result = 0;
	result |= ((inputColor & 0x000000F8) >> (0 + 3)) << 0;
	result |= ((inputColor & 0x0000F800) >> (8 + 3)) << 5;
	result |= ((inputColor & 0x00F80000) >> (16 + 3)) << 10;
	result |= ((inputColor & 0x80000000) >> 31) << 15;
	return result;
}

template <typename StorageFormat>
static Framework::Vulkan::CImage CreateSwizzleTable(Framework::Vulkan::CDevice& device,
                                                    const VkPhysicalDeviceMemoryProperties& memoryProperties, VkQueue queue, Framework::Vulkan::CCommandBufferPool& commandBufferPool)
{
	auto result = Framework::Vulkan::CImage(device, memoryProperties,
	                                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                                        VK_FORMAT_R32_UINT, StorageFormat::PAGEWIDTH, StorageFormat::PAGEHEIGHT);
	result.Fill(queue, commandBufferPool, memoryProperties,
	            CGsPixelFormats::CPixelIndexor<StorageFormat>::GetPageOffsets());
	result.SetLayout(queue, commandBufferPool, VK_IMAGE_LAYOUT_GENERAL);
	return result;
}

std::string CGSH_Vulkan::m_statsPath;
std::string CGSH_Vulkan::m_pipelineCachePath;

void CGSH_Vulkan::SetStatsPath(std::string path)
{
	m_statsPath = std::move(path);
}

void CGSH_Vulkan::SetPipelineCachePath(std::string path)
{
	m_pipelineCachePath = std::move(path);
}

CGSH_Vulkan::CGSH_Vulkan()
{
	m_context = std::make_shared<CContext>();
}

Framework::Vulkan::CInstance CGSH_Vulkan::CreateInstance(bool useValidationLayers)
{
	auto instanceCreateInfo = Framework::Vulkan::InstanceCreateInfo();

	std::vector<const char*> layers;
#if defined(_DEBUG)
	static const char* validationLayerName = "VK_LAYER_KHRONOS_validation";
	if(useValidationLayers)
	{
		useValidationLayers = Framework::Vulkan::CLoader::GetInstance().IsInstanceLayerPresent(validationLayerName);
	}
	if(useValidationLayers)
	{
		layers.push_back(validationLayerName);
	}
#endif

	std::vector<const char*> extensions;
	extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
	extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#if GSH_VULKAN_USE_ANNOTATIONS
	if(
	    Framework::Vulkan::CLoader::GetInstance().IsInstanceExtensionPresent(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
#if defined(__ANDROID__)
	    || useValidationLayers //On Android, the validation layer implements the debug utils extension, but it doesn't seem to be reported properly
#endif
	)
	{
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}
#endif
#ifdef _WIN32
	extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
#ifdef __APPLE__
#if TARGET_OS_IPHONE
	extensions.push_back(VK_MVK_IOS_SURFACE_EXTENSION_NAME);
#else
	extensions.push_back(VK_MVK_MACOS_SURFACE_EXTENSION_NAME);
#endif
#endif
#ifdef __linux__
#ifdef __ANDROID__
	extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#else
	extensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#endif
#endif

	auto appInfo = Framework::Vulkan::ApplicationInfo();
	appInfo.pApplicationName = "Play!";
	appInfo.pEngineName = "Play!";
#if GSH_VULKAN_IS_DESKTOP
	appInfo.apiVersion = VK_API_VERSION_1_2;
#elif GSH_VULKAN_IS_MOBILE
	appInfo.apiVersion = VK_API_VERSION_1_0;
#else
#error Unsupported Vulkan flavor
#endif

	instanceCreateInfo.pApplicationInfo = &appInfo;
	instanceCreateInfo.enabledExtensionCount = static_cast<uint32>(extensions.size());
	instanceCreateInfo.ppEnabledExtensionNames = extensions.data();
	instanceCreateInfo.enabledLayerCount = static_cast<uint32>(layers.size());
	instanceCreateInfo.ppEnabledLayerNames = layers.data();
	return Framework::Vulkan::CInstance(instanceCreateInfo);
}

void CGSH_Vulkan::InitializeImpl()
{
	assert(!m_instance.IsEmpty());
	m_context->instance = &m_instance;

	if(m_physicalDeviceOverride != VK_NULL_HANDLE)
	{
		m_context->physicalDevice = m_physicalDeviceOverride;
	}
	else
	{
		auto physicalDevices = GetPhysicalDevices();
		assert(!physicalDevices.empty());
		auto physicalDeviceIndex = GetPhysicalDeviceIndex(physicalDevices);
		m_context->physicalDevice = physicalDevices[physicalDeviceIndex];
	}

	auto renderQueueFamilies = GetRenderQueueFamilies(m_context->physicalDevice);
	assert(!renderQueueFamilies.empty());
	auto renderQueueFamily = renderQueueFamilies[0];

	m_instance.vkGetPhysicalDeviceMemoryProperties(m_context->physicalDevice, &m_context->physicalDeviceMemoryProperties);

	if(m_context->surface)
	{
		auto surfaceFormats = GetDeviceSurfaceFormats(m_context->physicalDevice);
		assert(surfaceFormats.size() > 0);
		int surfaceFormatIndex = 0;
		//Look for a suitable surface format
		for(int i = 0; i < surfaceFormats.size(); i++)
		{
			const auto& surfaceFormat = surfaceFormats[i];
			if(surfaceFormat.colorSpace != VK_COLORSPACE_SRGB_NONLINEAR_KHR)
			{
				continue;
			}
			if(
			    (surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM) ||
			    (surfaceFormat.format == VK_FORMAT_R8G8B8A8_UNORM))
			{
				surfaceFormatIndex = i;
				break;
			}
		}
		m_context->surfaceFormat = surfaceFormats[surfaceFormatIndex];
	}

	CreateDevice(m_context->physicalDevice);
	OnDeviceCreated();
	m_context->device.vkGetDeviceQueue(m_context->device, renderQueueFamily, 0, &m_context->queue);
	m_context->commandBufferPool = Framework::Vulkan::CCommandBufferPool(m_context->device, renderQueueFamily);

	//Create device-specialized pipeline cache, seeded from disk if available
	{
		m_devicePipelineCachePath.clear();
		if(!m_pipelineCachePath.empty())
		{
			VkPhysicalDeviceProperties props = {};
			m_instance.vkGetPhysicalDeviceProperties(m_context->physicalDevice, &props);
			std::string base = m_pipelineCachePath;
			if((base.size() >= 4) && (base.compare(base.size() - 4, 4, ".bin") == 0))
			{
				base.resize(base.size() - 4);
			}
			char suffix[64];
			snprintf(suffix, sizeof(suffix), "-v2-%04x-%04x-%08x.bin",
			         props.vendorID, props.deviceID, props.driverVersion);
			m_devicePipelineCachePath = base + suffix;
			char spirvSuffix[64];
			snprintf(spirvSuffix, sizeof(spirvSuffix), "-spirv-v%u-%02x.bin",
			         CContext::SPIRV_CACHE_VERSION, m_context->GetSpirvCapsMask());
			m_context->LoadSpirvCache(base + spirvSuffix);
		}
		std::vector<uint8> cacheData;
		if(!m_devicePipelineCachePath.empty())
		{
			std::ifstream in(m_devicePipelineCachePath, std::ios::binary | std::ios::ate);
			if(in)
			{
				auto sz = in.tellg();
				if(sz > 0 && sz < (32 * 1024 * 1024))
				{
					cacheData.resize(static_cast<size_t>(sz));
					in.seekg(0);
					in.read(reinterpret_cast<char*>(cacheData.data()), sz);
				}
			}
		}
		m_pipelineCacheSavedSize = cacheData.size();
		auto cacheInfo = Framework::Vulkan::PipelineCacheCreateInfo();
		cacheInfo.initialDataSize = cacheData.size();
		cacheInfo.pInitialData = cacheData.empty() ? nullptr : cacheData.data();
		if(m_context->device.vkCreatePipelineCache(m_context->device, &cacheInfo, nullptr, &m_context->pipelineCache) != VK_SUCCESS)
		{
			m_context->pipelineCache = VK_NULL_HANDLE;
		}
	}

	CreateDescriptorPool();
	CreateMemoryBuffer();
	CreateClutBuffer();

	m_swizzleTablePSMCT32 = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMCT32>(m_context->device, m_context->physicalDeviceMemoryProperties, m_context->queue, m_context->commandBufferPool);
	m_swizzleTablePSMCT16 = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMCT16>(m_context->device, m_context->physicalDeviceMemoryProperties, m_context->queue, m_context->commandBufferPool);
	m_swizzleTablePSMCT16S = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMCT16S>(m_context->device, m_context->physicalDeviceMemoryProperties, m_context->queue, m_context->commandBufferPool);
	m_swizzleTablePSMT8 = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMT8>(m_context->device, m_context->physicalDeviceMemoryProperties, m_context->queue, m_context->commandBufferPool);
	m_swizzleTablePSMT4 = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMT4>(m_context->device, m_context->physicalDeviceMemoryProperties, m_context->queue, m_context->commandBufferPool);
	m_swizzleTablePSMZ32 = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMZ32>(m_context->device, m_context->physicalDeviceMemoryProperties, m_context->queue, m_context->commandBufferPool);
	m_swizzleTablePSMZ16 = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMZ16>(m_context->device, m_context->physicalDeviceMemoryProperties, m_context->queue, m_context->commandBufferPool);
	m_swizzleTablePSMZ16S = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMZ16S>(m_context->device, m_context->physicalDeviceMemoryProperties, m_context->queue, m_context->commandBufferPool);

	m_context->swizzleTablePSMCT32View = m_swizzleTablePSMCT32.CreateImageView();
	m_context->swizzleTablePSMCT16View = m_swizzleTablePSMCT16.CreateImageView();
	m_context->swizzleTablePSMCT16SView = m_swizzleTablePSMCT16S.CreateImageView();
	m_context->swizzleTablePSMT8View = m_swizzleTablePSMT8.CreateImageView();
	m_context->swizzleTablePSMT4View = m_swizzleTablePSMT4.CreateImageView();
	m_context->swizzleTablePSMZ32View = m_swizzleTablePSMZ32.CreateImageView();
	m_context->swizzleTablePSMZ16View = m_swizzleTablePSMZ16.CreateImageView();
	m_context->swizzleTablePSMZ16SView = m_swizzleTablePSMZ16S.CreateImageView();

	m_context->annotations.SetImageName(m_swizzleTablePSMCT32, "Swizzle Table PSMCT32");
	m_context->annotations.SetImageName(m_swizzleTablePSMCT16, "Swizzle Table PSMCT16");
	m_context->annotations.SetImageName(m_swizzleTablePSMCT16S, "Swizzle Table PSMCT16S");
	m_context->annotations.SetImageName(m_swizzleTablePSMT8, "Swizzle Table PSMT8");
	m_context->annotations.SetImageName(m_swizzleTablePSMT4, "Swizzle Table PSMT4");
	m_context->annotations.SetImageName(m_swizzleTablePSMZ32, "Swizzle Table PSMZ32");
	m_context->annotations.SetImageName(m_swizzleTablePSMZ16, "Swizzle Table PSMZ16");
	m_context->annotations.SetImageName(m_swizzleTablePSMZ16S, "Swizzle Table PSMZ16S");

	m_context->annotations.SetImageViewName(m_context->swizzleTablePSMCT32View, "Swizzle Table View PSMCT32");
	m_context->annotations.SetImageViewName(m_context->swizzleTablePSMCT16View, "Swizzle Table View PSMCT16");
	m_context->annotations.SetImageViewName(m_context->swizzleTablePSMCT16SView, "Swizzle Table View PSMCT16S");
	m_context->annotations.SetImageViewName(m_context->swizzleTablePSMT8View, "Swizzle Table View PSMT8");
	m_context->annotations.SetImageViewName(m_context->swizzleTablePSMT4View, "Swizzle Table View PSMT4");
	m_context->annotations.SetImageViewName(m_context->swizzleTablePSMZ32View, "Swizzle Table View PSMZ32");
	m_context->annotations.SetImageViewName(m_context->swizzleTablePSMZ16View, "Swizzle Table View PSMZ16");
	m_context->annotations.SetImageViewName(m_context->swizzleTablePSMZ16SView, "Swizzle Table View PSMZ16S");

	m_frameCommandBuffer = std::make_shared<CFrameCommandBuffer>(m_context);
	m_clutLoad = std::make_shared<CClutLoad>(m_context, m_frameCommandBuffer);
#if GSH_VULKAN_IS_DESKTOP
	m_draw = std::make_shared<CDrawDesktop>(m_context, m_frameCommandBuffer);
#elif GSH_VULKAN_IS_MOBILE
	m_draw = std::make_shared<CDrawMobile>(m_context, m_frameCommandBuffer);
#else
#error Unsupported Vulkan flavor
#endif
	m_present = std::make_shared<CPresent>(m_context);
	m_transferHost = std::make_shared<CTransferHost>(m_context, m_frameCommandBuffer);
	m_transferLocal = std::make_shared<CTransferLocal>(m_context, m_frameCommandBuffer);

	m_frameCommandBuffer->RegisterWriter(m_draw.get());
	m_frameCommandBuffer->RegisterWriter(m_transferHost.get());
	m_statsEnabled = !m_statsPath.empty();
	m_draw->SetStatsEnabled(m_statsEnabled);
	m_frameCommandBuffer->BeginFrame();
}

void CGSH_Vulkan::SavePipelineCache()
{
	m_context->SaveSpirvCache();
	if((m_context->pipelineCache == VK_NULL_HANDLE) || m_devicePipelineCachePath.empty())
	{
		return;
	}
	auto getCacheData = reinterpret_cast<PFN_vkGetPipelineCacheData>(
	    m_instance.vkGetDeviceProcAddr(m_context->device, "vkGetPipelineCacheData"));
	if(!getCacheData) return;
	size_t dataSize = 0;
	if((getCacheData(m_context->device, m_context->pipelineCache, &dataSize, nullptr) == VK_SUCCESS) &&
	   (dataSize > 0) && (dataSize != m_pipelineCacheSavedSize))
	{
		std::vector<uint8> cacheData(dataSize);
		if(getCacheData(m_context->device, m_context->pipelineCache, &dataSize, cacheData.data()) == VK_SUCCESS)
		{
			std::string tmpPath = m_devicePipelineCachePath + ".tmp";
			{
				std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
				if(!out) return;
				out.write(reinterpret_cast<const char*>(cacheData.data()), static_cast<std::streamsize>(dataSize));
				if(!out) return;
			}
			std::remove(m_devicePipelineCachePath.c_str());
			if(std::rename(tmpPath.c_str(), m_devicePipelineCachePath.c_str()) == 0)
			{
				m_pipelineCacheSavedSize = dataSize;
			}
		}
	}
}

void CGSH_Vulkan::ReleaseImpl()
{
	ResetImpl();

	//Flush any pending rendering commands
	m_context->device.vkQueueWaitIdle(m_context->queue);

	m_clutLoad.reset();
	m_draw.reset();
	m_present.reset();
	m_transferHost.reset();
	m_transferLocal.reset();
	m_frameCommandBuffer.reset();

	m_context->device.vkDestroyImageView(m_context->device, m_context->swizzleTablePSMCT32View, nullptr);
	m_context->device.vkDestroyImageView(m_context->device, m_context->swizzleTablePSMCT16View, nullptr);
	m_context->device.vkDestroyImageView(m_context->device, m_context->swizzleTablePSMCT16SView, nullptr);
	m_context->device.vkDestroyImageView(m_context->device, m_context->swizzleTablePSMT8View, nullptr);
	m_context->device.vkDestroyImageView(m_context->device, m_context->swizzleTablePSMT4View, nullptr);
	m_context->device.vkDestroyImageView(m_context->device, m_context->swizzleTablePSMZ32View, nullptr);
	m_context->device.vkDestroyImageView(m_context->device, m_context->swizzleTablePSMZ16View, nullptr);
	m_context->device.vkDestroyImageView(m_context->device, m_context->swizzleTablePSMZ16SView, nullptr);

	m_swizzleTablePSMCT32.Reset();
	m_swizzleTablePSMCT16.Reset();
	m_swizzleTablePSMCT16S.Reset();
	m_swizzleTablePSMT8.Reset();
	m_swizzleTablePSMT4.Reset();
	m_swizzleTablePSMZ32.Reset();
	m_swizzleTablePSMZ16.Reset();
	m_swizzleTablePSMZ16S.Reset();

	m_context->device.vkDestroyDescriptorPool(m_context->device, m_context->descriptorPool, nullptr);
	if(m_context->pipelineCache != VK_NULL_HANDLE)
	{
		SavePipelineCache();
		m_context->device.vkDestroyPipelineCache(m_context->device, m_context->pipelineCache, nullptr);
		m_context->pipelineCache = VK_NULL_HANDLE;
	}
	m_context->clutBuffer.Reset();
	m_context->memoryBuffer.Reset();
	m_context->memoryBufferCopy.Reset();
	m_context->memoryBufferTransfer.Reset();
	m_context->commandBufferPool.Reset();
	m_context->device.Reset();

	delete[] m_memoryCache;
	m_memoryCache = nullptr;
}

void CGSH_Vulkan::ResetImpl()
{
	m_vtxCount = 0;
	m_stripHaveVerts = 0;
	m_stripFastActive = false;
	m_primitiveType = PRIM_INVALID;
	m_pendingPrim = false;
	m_pendingPrimValue = 0;
	m_regState.isValid = false;
	m_renderContextDirty = true;
	m_lastPrimitiveMode = ~0ULL;
	ClearClutCache();
	memset(m_memoryCache, 0, RAMSIZE);
	WriteBackMemoryCache();
}

void CGSH_Vulkan::SetPresentationParams(const CGSHandler::PRESENTATION_PARAMS& presentationParams)
{
	CGSHandler::SetPresentationParams(presentationParams);
	m_present->ValidateSwapChain(presentationParams);
}

void CGSH_Vulkan::MarkNewFrame()
{
	m_draw->FlushDirtyColorSlots();
	m_drawCallCount = m_frameCommandBuffer->GetFlushCount();
	m_frameCommandBuffer->ResetFlushCount();
	m_frameCommandBuffer->EndFrame();
	m_frameCommandBuffer->BeginFrame();
	m_regState.isValid = false;
	m_renderContextDirty = true;
	++m_pipelineCacheSaveFrames;
	if((m_pipelineCacheSaveFrames == 180) || ((m_pipelineCacheSaveFrames % 1800) == 0))
	{
		SavePipelineCache();
	}
	if(m_statsEnabled)
	{
		auto nowNs = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                                     std::chrono::steady_clock::now().time_since_epoch())
		                                     .count());
		if(m_statsWindowStartNs == 0) m_statsWindowStartNs = nowNs;
		m_statsFenceWaitNs += m_frameCommandBuffer->TakeFenceWaitNs();
		m_statsCmdFlushes += m_drawCallCount;
		auto s = m_draw->TakeStats();
		m_statsDrawAcc.passes += s.passes;
		m_statsDrawAcc.depthWritePasses += s.depthWritePasses;
		m_statsDrawAcc.draws += s.draws;
		m_statsDrawAcc.vkDraws += s.vkDraws;
		m_statsDrawAcc.vkPipes += s.vkPipes;
		m_statsDrawAcc.dmPipe += s.dmPipe;
		m_statsDrawAcc.dmDesc += s.dmDesc;
		m_statsDrawAcc.dmTex += s.dmTex;
		m_statsDrawAcc.dmOth += s.dmOth;
		for(int k = 0; k < 64; k++)
		{
			if(s.dmXorCnts[k] == 0) continue;
			uint32 h = static_cast<uint32>((s.dmXorKeys[k] * 0x9E3779B97F4A7C15ULL) >> 58);
			for(int step = 0; step < 64; step++)
			{
				uint32 j = (h + step) & 63U;
				if(m_statsDrawAcc.dmXorKeys[j] == s.dmXorKeys[k] || m_statsDrawAcc.dmXorCnts[j] == 0)
				{
					m_statsDrawAcc.dmXorKeys[j] = s.dmXorKeys[k];
					m_statsDrawAcc.dmXorCnts[j] += s.dmXorCnts[k];
					break;
				}
			}
		}
		m_statsDrawAcc.verts += s.verts;
		m_statsDrawAcc.pixels += s.pixels;
		m_statsDrawAcc.barriers += s.barriers;
		m_statsDrawAcc.barAlpha += s.barAlpha;
		m_statsDrawAcc.barDepthOnly += s.barDepthOnly;
		m_statsDrawAcc.barOther += s.barOther;
		m_statsDrawAcc.barOtherMask += s.barOtherMask;
		m_statsDrawAcc.barOtherATest += s.barOtherATest;
		m_statsDrawAcc.drawHwFast += s.drawHwFast;
		m_statsDrawAcc.drawHwDual += s.drawHwDual;
		m_statsDrawAcc.drawHwMask += s.drawHwMask;
		m_statsDrawAcc.drawSwFb += s.drawSwFb;
		m_statsDrawAcc.drawSwDepth += s.drawSwDepth;
		m_statsDrawAcc.drawSkipped += s.drawSkipped;
		m_statsDrawAcc.abStd += s.abStd;
		m_statsDrawAcc.abAdd += s.abAdd;
		m_statsDrawAcc.abFix += s.abFix;
		m_statsDrawAcc.abOther += s.abOther;
		for(uint32 i = 0; i < 256; i++)
		{
			m_statsDrawAcc.abModes[i] += s.abModes[i];
		}
		m_statsDrawAcc.flushCapsFmt += s.flushCapsFmt;
		m_statsDrawAcc.flushCapsDepthOverlap += s.flushCapsDepthOverlap;
		m_statsDrawAcc.flushCapsMemCopy += s.flushCapsMemCopy;
		m_statsDrawAcc.flushFbAddr += s.flushFbAddr;
		m_statsDrawAcc.flushZbAddr += s.flushZbAddr;
		m_statsDrawAcc.flushScissor += s.flushScissor;
		m_statsDrawAcc.flushTexDepthOverlap += s.flushTexDepthOverlap;
		m_statsDrawAcc.drawDynMip += s.drawDynMip;
		m_statsDrawAcc.drawStatMip += s.drawStatMip;
		m_statsDrawAcc.drawLinear += s.drawLinear;
		m_statsDrawAcc.drawMemCopy += s.drawMemCopy;
		m_statsDrawAcc.drawTexOverlapsFb += s.drawTexOverlapsFb;
		m_statsDrawAcc.drawPoints += s.drawPoints;
		m_statsDrawAcc.drawLines += s.drawLines;
		m_statsDrawAcc.drawAlphaBlend += s.drawAlphaBlend;
		m_statsDrawAcc.drawDstAlphaTest += s.drawDstAlphaTest;
		m_statsDrawAcc.fbPsmMask |= s.fbPsmMask;
		m_statsDrawAcc.zbPsmMask |= s.zbPsmMask;
		m_statsDrawAcc.texPsmMask |= s.texPsmMask;
		m_statsDrawAcc.resColorLoads += s.resColorLoads;
		m_statsDrawAcc.resColorStores += s.resColorStores;
		m_statsDrawAcc.resDepthLoads += s.resDepthLoads;
		m_statsDrawAcc.resDepthStores += s.resDepthStores;
		m_statsDrawAcc.resDepthStoreEvict += s.resDepthStoreEvict;
		m_statsDrawAcc.resDepthStoreOverlap += s.resDepthStoreOverlap;
		m_statsDrawAcc.resDepthStoreSyncW += s.resDepthStoreSyncW;
		m_statsDrawAcc.resDepthStoreSyncR += s.resDepthStoreSyncR;
		m_statsDrawAcc.resDepthStoreSelf += s.resDepthStoreSelf;
		m_statsDrawAcc.resColorStoreEvict += s.resColorStoreEvict;
		m_statsDrawAcc.resColorStoreOverlap += s.resColorStoreOverlap;
		m_statsDrawAcc.resColorStoreSyncW += s.resColorStoreSyncW;
		m_statsDrawAcc.resColorStoreSyncR += s.resColorStoreSyncR;
		if(++m_statsFrames >= 60)
		{
			double f = static_cast<double>(m_statsFrames);
			double frameMs = static_cast<double>(nowNs - m_statsWindowStartNs) / (1e6 * f);
			double fenceMs = static_cast<double>(m_statsFenceWaitNs) / (1e6 * f);
			if(FILE* out = fopen(m_statsPath.c_str(), "a"))
			{
				uint32 topCode[4] = {0, 0, 0, 0};
				uint32 topCnt[4] = {0, 0, 0, 0};
				for(uint32 c = 0; c < 256; c++)
				{
					uint32 cnt = m_statsDrawAcc.abModes[c];
					if(cnt == 0) continue;
					for(int k = 0; k < 4; k++)
					{
						if(cnt > topCnt[k])
						{
							for(int j = 3; j > k; j--)
							{
								topCnt[j] = topCnt[j - 1];
								topCode[j] = topCode[j - 1];
							}
							topCnt[k] = cnt;
							topCode[k] = c;
							break;
						}
					}
				}
				uint64 topXor0 = 0, topXor1 = 0;
				uint32 topXorCnt0 = 0, topXorCnt1 = 0;
				for(int k = 0; k < 64; k++)
				{
					uint32 c = m_statsDrawAcc.dmXorCnts[k];
					if(c > topXorCnt0)
					{
						topXorCnt1 = topXorCnt0;
						topXor1 = topXor0;
						topXorCnt0 = c;
						topXor0 = m_statsDrawAcc.dmXorKeys[k];
					}
					else if(c > topXorCnt1)
					{
						topXorCnt1 = c;
						topXor1 = m_statsDrawAcc.dmXorKeys[k];
					}
				}
				fprintf(out,
				        "gs: %.2f fps (%.2f ms/f, fence %.2f ms/f) passes=%.1f (zW=%.1f) draws=%.1f(vk=%.0f,vp=%.0f,hw=%.0f,ds=%.0f,mk=%.0f,sw=%.0f,swZ=%.0f,sk=%.0f) bar=%.1f(ab=%.0f,z=%.0f,o=%.0f[m=%.0f,a=%.0f]) verts=%.0f Mpx=%.2f "
				        "flushes[fmt=%.1f zOv=%.1f mCp=%.1f fb=%.1f zb=%.1f sc=%.1f tZOv=%.1f clut=%.1f hX=%.1f(%.0fKB) lX=%.1f rb=%.1f cmd=%.1f] "
				        "clut[ld=%.1f clr=%.1f] feat[dMip=%.1f sMip=%.1f lin=%.1f mCp=%.1f tFbOv=%.1f pt=%.1f ln=%.1f ab=%.1f(std=%.0f,add=%.0f,fix=%.0f,oth=%.0f) abTop[%u%u%u%u=%.0f,%u%u%u%u=%.0f,%u%u%u%u=%.0f] abId=%.1f dat=%.1f] "
				        "psm[fb=%llx zb=%llx tx=%llx] res[cL=%.1f cS=%.1f(ev=%.1f,ov=%.1f,w=%.1f,r=%.1f) zL=%.1f zS=%.1f(ev=%.1f,ov=%.1f,w=%.1f,r=%.1f,s=%.1f)] dm[p=%.0f(%llx:%.0f,%llx:%.0f),d=%.0f,t=%.0f,o=%.0f] s8=%d s16=%d dsb=%d son=%d qm=%x\n",
				        1000.0 / frameMs, frameMs, fenceMs,
				        m_statsDrawAcc.passes / f, m_statsDrawAcc.depthWritePasses / f,
				        m_statsDrawAcc.draws / f, m_statsDrawAcc.vkDraws / f, m_statsDrawAcc.vkPipes / f,
				        m_statsDrawAcc.drawHwFast / f, m_statsDrawAcc.drawHwDual / f, m_statsDrawAcc.drawHwMask / f,
				        m_statsDrawAcc.drawSwFb / f, m_statsDrawAcc.drawSwDepth / f, m_statsDrawAcc.drawSkipped / f,
				        m_statsDrawAcc.barriers / f,
				        m_statsDrawAcc.barAlpha / f, m_statsDrawAcc.barDepthOnly / f, m_statsDrawAcc.barOther / f,
				        m_statsDrawAcc.barOtherMask / f, m_statsDrawAcc.barOtherATest / f,
				        m_statsDrawAcc.verts / f,
				        static_cast<double>(m_statsDrawAcc.pixels) / (1e6 * f),
				        m_statsDrawAcc.flushCapsFmt / f, m_statsDrawAcc.flushCapsDepthOverlap / f,
				        m_statsDrawAcc.flushCapsMemCopy / f, m_statsDrawAcc.flushFbAddr / f,
				        m_statsDrawAcc.flushZbAddr / f, m_statsDrawAcc.flushScissor / f,
				        m_statsDrawAcc.flushTexDepthOverlap / f, m_statsClutFlushes / f,
				        m_statsHostXfers / f, static_cast<double>(m_statsHostXferBytes) / (1024.0 * f),
				        m_statsLocalXfers / f, m_statsReadbacks / f, m_statsCmdFlushes / f,
				        m_statsClutLoads / f, m_statsClutClears / f,
				        m_statsDrawAcc.drawDynMip / f, m_statsDrawAcc.drawStatMip / f,
				        m_statsDrawAcc.drawLinear / f, m_statsDrawAcc.drawMemCopy / f,
				        m_statsDrawAcc.drawTexOverlapsFb / f, m_statsDrawAcc.drawPoints / f,
				        m_statsDrawAcc.drawLines / f, m_statsDrawAcc.drawAlphaBlend / f,
				        m_statsDrawAcc.abStd / f, m_statsDrawAcc.abAdd / f, m_statsDrawAcc.abFix / f, m_statsDrawAcc.abOther / f,
				        (topCode[0] >> 6) & 3, (topCode[0] >> 4) & 3, (topCode[0] >> 2) & 3, topCode[0] & 3, topCnt[0] / f,
				        (topCode[1] >> 6) & 3, (topCode[1] >> 4) & 3, (topCode[1] >> 2) & 3, topCode[1] & 3, topCnt[1] / f,
				        (topCode[2] >> 6) & 3, (topCode[2] >> 4) & 3, (topCode[2] >> 2) & 3, topCode[2] & 3, topCnt[2] / f,
				        m_statsAlphaIdentity / f, m_statsDrawAcc.drawDstAlphaTest / f,
				        static_cast<unsigned long long>(m_statsDrawAcc.fbPsmMask),
				        static_cast<unsigned long long>(m_statsDrawAcc.zbPsmMask),
				        static_cast<unsigned long long>(m_statsDrawAcc.texPsmMask),
				        m_statsDrawAcc.resColorLoads / f, m_statsDrawAcc.resColorStores / f,
				        m_statsDrawAcc.resColorStoreEvict / f, m_statsDrawAcc.resColorStoreOverlap / f,
				        m_statsDrawAcc.resColorStoreSyncW / f, m_statsDrawAcc.resColorStoreSyncR / f,
				        m_statsDrawAcc.resDepthLoads / f, m_statsDrawAcc.resDepthStores / f,
				        m_statsDrawAcc.resDepthStoreEvict / f, m_statsDrawAcc.resDepthStoreOverlap / f,
				        m_statsDrawAcc.resDepthStoreSyncW / f, m_statsDrawAcc.resDepthStoreSyncR / f,
				        m_statsDrawAcc.resDepthStoreSelf / f,
				        m_statsDrawAcc.dmPipe / f,
				        static_cast<unsigned long long>(topXor0), topXorCnt0 / f,
				        static_cast<unsigned long long>(topXor1), topXorCnt1 / f,
				        m_statsDrawAcc.dmDesc / f, m_statsDrawAcc.dmTex / f, m_statsDrawAcc.dmOth / f,
				        m_context->has8BitStorage ? 1 : 0, m_context->has16BitStorage ? 1 : 0,
				        m_context->hasDualSrcBlend ? 1 : 0, m_context->hasStoreOpNone ? 1 : 0, m_context->storageQueryMask);
				fclose(out);
			}
			m_statsFrames = 0;
			m_statsWindowStartNs = nowNs;
			m_statsFenceWaitNs = 0;
			m_statsHostXfers = 0;
			m_statsHostXferBytes = 0;
			m_statsLocalXfers = 0;
			m_statsReadbacks = 0;
			m_statsClutLoads = 0;
			m_statsClutFlushes = 0;
			m_statsClutClears = 0;
			m_statsCmdFlushes = 0;
			m_statsAlphaIdentity = 0;
			m_statsDrawAcc = {};
		}
	}
	CGSHandler::MarkNewFrame();
}

void CGSH_Vulkan::FlipImpl(const DISPLAY_INFO& dispInfo)
{
	if(m_present && m_context->surface)
	{
		m_present->SetPresentationViewport(GetPresentationViewport());
		m_present->DoPresent(dispInfo);
	}

	PresentBackbuffer();
	for(auto& xferHistoryPair : m_xferHistory)
	{
		xferHistoryPair.second.Advance();
	}
	std::experimental::erase_if(m_xferHistory,
	                            [](const auto& xferTrackerPair) { return xferTrackerPair.second.IsEmpty(); });
	CGSHandler::FlipImpl(dispInfo);
}

std::vector<VkPhysicalDevice> CGSH_Vulkan::GetPhysicalDevices()
{
	auto result = VK_SUCCESS;

	uint32_t physicalDeviceCount = 0;
	result = m_instance.vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, nullptr);
	CHECKVULKANERROR(result);

	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	result = m_instance.vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, physicalDevices.data());
	CHECKVULKANERROR(result);

	return physicalDevices;
}

uint32 CGSH_Vulkan::GetPhysicalDeviceIndex(const std::vector<VkPhysicalDevice>& physicalDevices) const
{
	auto selectedDevice = CDeviceInfo::GetInstance().GetSelectedDevice();
	for(uint32 i = 0; i < physicalDevices.size(); i++)
	{
		const auto& physicalDevice = physicalDevices[i];
		VkPhysicalDeviceProperties physicalDeviceProperties = {};
		m_instance.vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);
		if(
		    (physicalDeviceProperties.deviceID == selectedDevice.deviceId) &&
		    (physicalDeviceProperties.vendorID == selectedDevice.vendorId))
		{
			return i;
		}
	}
	assert(false);
	return 0;
}

std::vector<uint32_t> CGSH_Vulkan::GetRenderQueueFamilies(VkPhysicalDevice physicalDevice)
{
	auto result = VK_SUCCESS;
	std::vector<uint32_t> renderQueueFamilies;

	uint32_t queueFamilyCount = 0;
	m_instance.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

	CLog::GetInstance().Print(LOG_NAME, "Found %d queue families.\r\n", queueFamilyCount);

	std::vector<VkQueueFamilyProperties> queueFamilyPropertiesArray(queueFamilyCount);
	m_instance.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilyPropertiesArray.data());

	for(uint32_t queueFamilyIndex = 0; queueFamilyIndex < queueFamilyCount; queueFamilyIndex++)
	{
		bool graphicsSupported = false;

		CLog::GetInstance().Print(LOG_NAME, "Queue Family Info:\r\n");

		const auto& queueFamilyProperties = queueFamilyPropertiesArray[queueFamilyIndex];
		CLog::GetInstance().Print(LOG_NAME, "Queue Count:    %d\r\n", queueFamilyProperties.queueCount);
		CLog::GetInstance().Print(LOG_NAME, "Operating modes:\r\n");
		if(queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			graphicsSupported = true;
			CLog::GetInstance().Print(LOG_NAME, "  Graphics\r\n");
		}
		if(queueFamilyProperties.queueFlags & VK_QUEUE_COMPUTE_BIT)
		{
			CLog::GetInstance().Print(LOG_NAME, "  Compute\r\n");
		}
		if(queueFamilyProperties.queueFlags & VK_QUEUE_TRANSFER_BIT)
		{
			CLog::GetInstance().Print(LOG_NAME, "  Transfer\r\n");
		}
		if(queueFamilyProperties.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)
		{
			CLog::GetInstance().Print(LOG_NAME, "  Sparse Binding\r\n");
		}

		VkBool32 surfaceSupported = VK_FALSE;
		if(m_context->surface)
		{
			result = m_instance.vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, m_context->surface, &surfaceSupported);
			CHECKVULKANERROR(result);

			CLog::GetInstance().Print(LOG_NAME, "Supports surface: %d\r\n", surfaceSupported);
		}

		if(graphicsSupported && (!m_context->surface || surfaceSupported))
		{
			renderQueueFamilies.push_back(queueFamilyIndex);
		}
	}

	return renderQueueFamilies;
}

std::vector<VkSurfaceFormatKHR> CGSH_Vulkan::GetDeviceSurfaceFormats(VkPhysicalDevice physicalDevice)
{
	assert(m_context->surface != VK_NULL_HANDLE);

	auto result = VK_SUCCESS;

	uint32_t surfaceFormatCount = 0;
	result = m_instance.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_context->surface, &surfaceFormatCount, nullptr);
	CHECKVULKANERROR(result);

	CLog::GetInstance().Print(LOG_NAME, "Found %d surface formats.\r\n", surfaceFormatCount);

	std::vector<VkSurfaceFormatKHR> surfaceFormats(surfaceFormatCount);
	result = m_instance.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_context->surface, &surfaceFormatCount, surfaceFormats.data());
	CHECKVULKANERROR(result);

	for(const auto& surfaceFormat : surfaceFormats)
	{
		CLog::GetInstance().Print(LOG_NAME, "Surface Format Info:\r\n");

		CLog::GetInstance().Print(LOG_NAME, "Format:      %d\r\n", surfaceFormat.format);
		CLog::GetInstance().Print(LOG_NAME, "Color Space: %d\r\n", surfaceFormat.colorSpace);
	}

	return surfaceFormats;
}

void CGSH_Vulkan::Query8And16BitStorage(const Framework::Vulkan::CInstance& instance, VkPhysicalDevice physicalDevice, bool& has8BitStorage, bool& has16BitStorage, bool* hasUniform8, bool* hasUniform16, uint32* diagMask)
{
#if GSH_VULKAN_IS_DESKTOP
	(void)instance;
	(void)physicalDevice;
	has8BitStorage = true;
	has16BitStorage = true;
	if(hasUniform8) *hasUniform8 = true;
	if(hasUniform16) *hasUniform16 = true;
	if(diagMask) *diagMask = 0xFFFF;
#else
	has8BitStorage = false;
	has16BitStorage = false;
	if(hasUniform8) *hasUniform8 = false;
	if(hasUniform16) *hasUniform16 = false;
	uint32 mask = 0;

	VkPhysicalDeviceProperties props = {};
	instance.vkGetPhysicalDeviceProperties(physicalDevice, &props);
	if(props.apiVersion >= VK_API_VERSION_1_1) mask |= 0x01;
	if(props.apiVersion < VK_API_VERSION_1_1)
	{
		if(diagMask) *diagMask = mask;
		return;
	}

	uint32_t extCount = 0;
	if(instance.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr) != VK_SUCCESS)
	{
		if(diagMask) *diagMask = mask;
		return;
	}
	std::vector<VkExtensionProperties> exts(extCount);
	instance.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, exts.data());
	auto hasExt = [&](const char* name) {
		return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& e) { return strcmp(e.extensionName, name) == 0; });
	};
	bool hasExt8 = hasExt(VK_KHR_8BIT_STORAGE_EXTENSION_NAME) && hasExt(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
	if(hasExt8) mask |= 0x02;

	auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
	    Framework::Vulkan::CLoader::GetInstance().vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
	if(!getFeatures2)
	{
		getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2KHR>(
		    Framework::Vulkan::CLoader::GetInstance().vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
	}
	if(!getFeatures2)
	{
		if(diagMask) *diagMask = mask;
		return;
	}
	mask |= 0x04;

	VkPhysicalDeviceFeatures2 features2 = {};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

	VkPhysicalDevice16BitStorageFeatures features16 = {};
	features16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
	features2.pNext = &features16;

	VkPhysicalDevice8BitStorageFeaturesKHR features8 = {};
	features8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_KHR;
	VkPhysicalDeviceShaderFloat16Int8FeaturesKHR featuresF16I8 = {};
	featuresF16I8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR;
	if(hasExt8)
	{
		features16.pNext = &features8;
		features8.pNext = &featuresF16I8;
	}

	getFeatures2(physicalDevice, &features2);

	if(features2.features.shaderInt16) mask |= 0x10;
	if(features16.storageBuffer16BitAccess) mask |= 0x20;
	if(features16.uniformAndStorageBuffer16BitAccess) mask |= 0x40;
	if(featuresF16I8.shaderInt8) mask |= 0x100;
	if(features8.storageBuffer8BitAccess) mask |= 0x200;
	if(features8.uniformAndStorageBuffer8BitAccess) mask |= 0x400;
	if(diagMask) *diagMask = mask;

	has8BitStorage =
	    hasExt8 &&
	    (featuresF16I8.shaderInt8 == VK_TRUE) &&
	    (features8.storageBuffer8BitAccess == VK_TRUE);
	//Nuanceur emits 8-bit storage capabilities whenever 16-bit storage is used.
	has16BitStorage =
	    has8BitStorage &&
	    (features2.features.shaderInt16 == VK_TRUE) &&
	    (features16.storageBuffer16BitAccess == VK_TRUE);
	if(hasUniform16) *hasUniform16 = has16BitStorage && (features16.uniformAndStorageBuffer16BitAccess == VK_TRUE);
	if(hasUniform8) *hasUniform8 = has8BitStorage && (features8.uniformAndStorageBuffer8BitAccess == VK_TRUE);
#endif
}

void CGSH_Vulkan::CreateDevice(VkPhysicalDevice physicalDevice)
{
	assert(m_context->device.IsEmpty());

	Query8And16BitStorage(m_instance, physicalDevice,
	                      m_context->has8BitStorage, m_context->has16BitStorage,
	                      &m_context->hasUniform8BitStorage, &m_context->hasUniform16BitStorage,
	                      &m_context->storageQueryMask);
	{
		VkPhysicalDeviceFeatures deviceFeatures = {};
		auto getFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
		    Framework::Vulkan::CLoader::GetInstance().vkGetInstanceProcAddr(m_instance, "vkGetPhysicalDeviceFeatures"));
		if(getFeatures)
		{
			getFeatures(physicalDevice, &deviceFeatures);
		}
		m_context->hasDualSrcBlend = (deviceFeatures.dualSrcBlend == VK_TRUE);
	}
	m_context->device = CreateDeviceObject(m_instance, physicalDevice, m_context->surface != VK_NULL_HANDLE);

	{
		VkPhysicalDeviceProperties deviceProperties = {};
		m_context->instance->vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
		m_context->storageBufferAlignment = deviceProperties.limits.minStorageBufferOffsetAlignment;
		m_context->computeWorkgroupInvocations = deviceProperties.limits.maxComputeWorkGroupInvocations;
		if(deviceProperties.apiVersion >= VK_API_VERSION_1_3)
		{
			m_context->hasStoreOpNone = true;
		}
		else
		{
			uint32_t extCount = 0;
			if(m_instance.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr) == VK_SUCCESS)
			{
				std::vector<VkExtensionProperties> exts(extCount);
				m_instance.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, exts.data());
				for(const auto& e : exts)
				{
					if((strcmp(e.extensionName, "VK_EXT_load_store_op_none") == 0) ||
					   (strcmp(e.extensionName, "VK_KHR_load_store_op_none") == 0))
					{
						m_context->hasStoreOpNone = true;
						break;
					}
				}
			}
		}
	}

	m_context->annotations = decltype(m_context->annotations)(m_context->instance, &m_context->device);
}

Framework::Vulkan::CDevice CGSH_Vulkan::CreateDeviceObject(const Framework::Vulkan::CInstance& instance, VkPhysicalDevice physicalDevice, bool withSwapchain)
{
	bool has8BitStorage = false;
	bool has16BitStorage = false;
	bool hasUniform8 = false;
	bool hasUniform16 = false;
	Query8And16BitStorage(instance, physicalDevice, has8BitStorage, has16BitStorage, &hasUniform8, &hasUniform16);

	float queuePriorities[] = {1.0f};

	auto deviceQueueCreateInfo = Framework::Vulkan::DeviceQueueCreateInfo();
	deviceQueueCreateInfo.flags = 0;
	deviceQueueCreateInfo.queueFamilyIndex = 0;
	deviceQueueCreateInfo.queueCount = 1;
	deviceQueueCreateInfo.pQueuePriorities = queuePriorities;

	std::vector<const char*> enabledExtensions;
	if(withSwapchain)
	{
		enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	}
#if GSH_VULKAN_IS_DESKTOP
	enabledExtensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
#else
	if(has8BitStorage)
	{
		enabledExtensions.push_back(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
		enabledExtensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
	}
	{
		VkPhysicalDeviceProperties props = {};
		instance.vkGetPhysicalDeviceProperties(physicalDevice, &props);
		if(props.apiVersion < VK_API_VERSION_1_3)
		{
			uint32_t extCount = 0;
			if(instance.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr) == VK_SUCCESS)
			{
				std::vector<VkExtensionProperties> exts(extCount);
				instance.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, exts.data());
				auto hasExt = [&](const char* name) {
					return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& e) { return strcmp(e.extensionName, name) == 0; });
				};
				if(hasExt("VK_KHR_load_store_op_none"))
				{
					enabledExtensions.push_back("VK_KHR_load_store_op_none");
				}
				else if(hasExt("VK_EXT_load_store_op_none"))
				{
					enabledExtensions.push_back("VK_EXT_load_store_op_none");
				}
			}
		}
	}
#endif

	std::vector<const char*> enabledLayers;

	Framework::Vulkan::CStructChain createDeviceStructs;

#if GSH_VULKAN_IS_DESKTOP
	{
		auto physicalDeviceFeaturesInvocationInterlock = Framework::Vulkan::PhysicalDeviceFragmentShaderInterlockFeaturesEXT();
		physicalDeviceFeaturesInvocationInterlock.fragmentShaderPixelInterlock = VK_TRUE;
		createDeviceStructs.AddStruct(physicalDeviceFeaturesInvocationInterlock);
	}
#endif

	{
		VkPhysicalDeviceFeatures supportedFeatures = {};
		auto getFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
		    Framework::Vulkan::CLoader::GetInstance().vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures"));
		if(getFeatures)
		{
			getFeatures(physicalDevice, &supportedFeatures);
		}

		auto physicalDeviceFeatures2 = Framework::Vulkan::PhysicalDeviceFeatures2KHR();
#ifndef __APPLE__
		//MoltenVK doesn't report this properly (probably due to mobile devices supporting buffer stores and not image stores)
		physicalDeviceFeatures2.features.fragmentStoresAndAtomics = VK_TRUE;
#endif
		if(supportedFeatures.dualSrcBlend == VK_TRUE)
		{
			physicalDeviceFeatures2.features.dualSrcBlend = VK_TRUE;
		}
		if(has16BitStorage)
		{
			physicalDeviceFeatures2.features.shaderInt16 = VK_TRUE;
		}
		createDeviceStructs.AddStruct(physicalDeviceFeatures2);
	}

#if GSH_VULKAN_IS_DESKTOP
	{
		auto physicalDeviceVulkan12Features = Framework::Vulkan::PhysicalDeviceVulkan12Features();
		physicalDeviceVulkan12Features.shaderInt8 = VK_TRUE;
		physicalDeviceVulkan12Features.storageBuffer8BitAccess = VK_TRUE;
		physicalDeviceVulkan12Features.uniformAndStorageBuffer8BitAccess = VK_TRUE;
		createDeviceStructs.AddStruct(physicalDeviceVulkan12Features);
	}
#else
	if(has8BitStorage)
	{
		VkPhysicalDevice8BitStorageFeaturesKHR features8 = {};
		features8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES_KHR;
		features8.storageBuffer8BitAccess = VK_TRUE;
		features8.uniformAndStorageBuffer8BitAccess = hasUniform8 ? VK_TRUE : VK_FALSE;
		createDeviceStructs.AddStruct(features8);

		VkPhysicalDeviceShaderFloat16Int8FeaturesKHR featuresF16I8 = {};
		featuresF16I8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR;
		featuresF16I8.shaderInt8 = VK_TRUE;
		createDeviceStructs.AddStruct(featuresF16I8);
	}
#endif

	if(has16BitStorage)
	{
		auto physicalDevice16BitStorageFeatures = Framework::Vulkan::PhysicalDevice16BitStorageFeatures();
		physicalDevice16BitStorageFeatures.storageBuffer16BitAccess = VK_TRUE;
		physicalDevice16BitStorageFeatures.uniformAndStorageBuffer16BitAccess = hasUniform16 ? VK_TRUE : VK_FALSE;
		createDeviceStructs.AddStruct(physicalDevice16BitStorageFeatures);
	}

	auto deviceCreateInfo = Framework::Vulkan::DeviceCreateInfo();
	deviceCreateInfo.pNext = createDeviceStructs.GetNext();
	deviceCreateInfo.flags = 0;
	deviceCreateInfo.enabledLayerCount = static_cast<uint32>(enabledLayers.size());
	deviceCreateInfo.ppEnabledLayerNames = enabledLayers.data();
	deviceCreateInfo.enabledExtensionCount = static_cast<uint32>(enabledExtensions.size());
	deviceCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();
	deviceCreateInfo.queueCreateInfoCount = 1;
	deviceCreateInfo.pQueueCreateInfos = &deviceQueueCreateInfo;

	return Framework::Vulkan::CDevice(instance, physicalDevice, deviceCreateInfo);
}

void CGSH_Vulkan::CreateDescriptorPool()
{
	std::vector<VkDescriptorPoolSize> poolSizes;

	{
		VkDescriptorPoolSize poolSize = {};
		poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		poolSize.descriptorCount = 0x800;
		poolSizes.push_back(poolSize);
	}

	{
		VkDescriptorPoolSize poolSize = {};
		poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		poolSize.descriptorCount = 0x800;
		poolSizes.push_back(poolSize);
	}

	{
		VkDescriptorPoolSize poolSize = {};
		poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
		poolSize.descriptorCount = 0x800;
		poolSizes.push_back(poolSize);
	}

	{
		VkDescriptorPoolSize poolSize = {};
		poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		poolSize.descriptorCount = 0x800;
		poolSizes.push_back(poolSize);
	}

	{
		VkDescriptorPoolSize poolSize = {};
		poolSize.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
		poolSize.descriptorCount = 0x800;
		poolSizes.push_back(poolSize);
	}

	auto descriptorPoolCreateInfo = Framework::Vulkan::DescriptorPoolCreateInfo();
	descriptorPoolCreateInfo.poolSizeCount = static_cast<uint32>(poolSizes.size());
	descriptorPoolCreateInfo.pPoolSizes = poolSizes.data();
	descriptorPoolCreateInfo.maxSets = 0x1000;

	auto result = m_context->device.vkCreateDescriptorPool(m_context->device, &descriptorPoolCreateInfo, nullptr, &m_context->descriptorPool);
	CHECKVULKANERROR(result);
}

void CGSH_Vulkan::CreateMemoryBuffer()
{
	assert(m_context->memoryBuffer.IsEmpty());
	assert(!m_memoryCache);

	m_context->memoryBuffer = Framework::Vulkan::CBuffer(m_context->device,
	                                                     m_context->physicalDeviceMemoryProperties,
	                                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	                                                     RAMSIZE);
	m_context->annotations.SetBufferName(m_context->memoryBuffer, "GS Memory");

	m_memoryCache = new uint8[RAMSIZE];
	memset(m_memoryCache, 0, RAMSIZE);

	m_context->memoryBuffer.Write(m_context->queue, m_context->commandBufferPool,
	                              m_context->physicalDeviceMemoryProperties, m_memoryCache);

	m_context->memoryBufferCopy = Framework::Vulkan::CBuffer(m_context->device,
	                                                         m_context->physicalDeviceMemoryProperties,
	                                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	                                                         RAMSIZE);
	m_context->annotations.SetBufferName(m_context->memoryBufferCopy, "GS Memory Copy");

	m_context->memoryBufferTransfer = Framework::Vulkan::CBuffer(m_context->device,
	                                                             m_context->physicalDeviceMemoryProperties,
	                                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
	                                                             RAMSIZE);
	m_context->annotations.SetBufferName(m_context->memoryBufferTransfer, "GS Memory Transfer");
}

void CGSH_Vulkan::CreateClutBuffer()
{
	assert(m_context->clutBuffer.IsEmpty());

	static const uint32 clutBufferSize = CLUTENTRYCOUNT * sizeof(uint32) * CLUT_CACHE_SIZE;
	m_context->clutBuffer = Framework::Vulkan::CBuffer(m_context->device,
	                                                   m_context->physicalDeviceMemoryProperties,
	                                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
	                                                   clutBufferSize);
	m_context->annotations.SetBufferName(m_context->clutBuffer, "CLUT Buffer");
}

void CGSH_Vulkan::ProcessPrim(uint64 data)
{
	unsigned int newPrimitiveType = static_cast<unsigned int>(data & 0x07);
	if(newPrimitiveType != m_primitiveType)
	{
		auto toPipelinePrim = [](unsigned int primType) {
			switch(primType)
			{
			case PRIM_POINT:
				return CDraw::PIPELINE_PRIMITIVE_POINT;
			case PRIM_LINE:
			case PRIM_LINESTRIP:
				return CDraw::PIPELINE_PRIMITIVE_LINE;
			default:
				return CDraw::PIPELINE_PRIMITIVE_TRIANGLE;
			}
		};
		bool topoChanged = toPipelinePrim(newPrimitiveType) != toPipelinePrim(m_primitiveType);
		bool spriteChanged = (newPrimitiveType == PRIM_SPRITE) != (m_primitiveType == PRIM_SPRITE);
		if(topoChanged)
		{
			m_draw->FlushVertices();
		}
		if(topoChanged || spriteChanged)
		{
			m_renderContextDirty = true;
		}
	}
	m_primitiveType = newPrimitiveType;
	m_stripHaveVerts = 0;
	if((newPrimitiveType == PRIM_TRIANGLESTRIP) && !m_renderContextDirty &&
	   ((((m_nReg[GS_REG_PRMODECONT] & 1) != 0) ? data : m_nReg[GS_REG_PRMODE]) == m_lastPrimitiveMode))
	{
		m_stripFastActive = true;
	}
	else
	{
		m_stripFastActive = false;
	}
	switch(m_primitiveType)
	{
	case PRIM_POINT:
		m_vtxCount = 1;
		break;
	case PRIM_LINE:
	case PRIM_LINESTRIP:
		m_vtxCount = 2;
		break;
	case PRIM_TRIANGLE:
	case PRIM_TRIANGLESTRIP:
	case PRIM_TRIANGLEFAN:
		m_vtxCount = 3;
		break;
	case PRIM_SPRITE:
		m_vtxCount = 2;
		break;
	}
}

void CGSH_Vulkan::VertexKick(uint8 registerId, uint64 data)
{
	if(m_pendingPrim)
	{
		m_pendingPrim = false;
		ProcessPrim(m_pendingPrimValue);
	}

	if(m_vtxCount == 0) return;

	bool drawingKick = (registerId == GS_REG_XYZ2) || (registerId == GS_REG_XYZF2);
	bool fog = (registerId == GS_REG_XYZF2) || (registerId == GS_REG_XYZF3);

	if(!m_drawEnabled) drawingKick = false;

	//Fast path for triangle strips: unpack each incoming vertex once instead of 3x per triangle.
	if(m_primitiveType == PRIM_TRIANGLESTRIP)
	{
		if(m_stripFastActive && !m_renderContextDirty)
		{
			//m_primitiveMode is already current; skip PRMODECONT/PRIM checks on every strip vertex.
		}
		else
		{
			uint64 curPrimMode = ((m_nReg[GS_REG_PRMODECONT] & 1) != 0) ? m_nReg[GS_REG_PRIM] : m_nReg[GS_REG_PRMODE];
			auto nextMode = make_convertible<PRMODE>(curPrimMode);
			//Enter the fast strip path on vertex 0 even when render context is dirty (e.g. texture switch before strip)
			//as long as UV mode isn't active (UV needs m_invTexWidth/Height from SetRenderingContext).
			if(!m_stripFastActive && ((m_vtxCount != 3) || (nextMode.nTexture && nextMode.nUseUV && (m_renderContextDirty || (curPrimMode != m_lastPrimitiveMode)))))
			{
				goto slowVertexKick;
			}
			m_primitiveMode = nextMode;
			if(drawingKick && (m_renderContextDirty || (curPrimMode != m_lastPrimitiveMode)))
			{
				SetRenderingContext(m_primitiveMode);
			}
			else if(m_renderContextDirty || (curPrimMode != m_lastPrimitiveMode))
			{
				auto offset = make_convertible<XYOFFSET>(m_nReg[GS_REG_XYOFFSET_1 + m_primitiveMode.nContext]);
				m_primOfsX = offset.GetX();
				m_primOfsY = offset.GetY();
			}
			m_stripFastActive = true;
		}
		{
			static constexpr float inv16 = 1.0f / 16.0f;
			static constexpr float inv255 = 1.0f / 255.0f;
			uint64 pos = fog ? (data & 0x00FFFFFFFFFFFFFFULL) : data;
			uint64 rgbaq = m_nReg[GS_REG_RGBAQ];
			float vx = static_cast<float>(static_cast<uint32>(pos & 0xFFFF)) * inv16 - m_primOfsX;
			float vy = static_cast<float>(static_cast<uint32>((pos >> 16) & 0xFFFF)) * inv16 - m_primOfsY;
			uint32 vz = static_cast<uint32>(pos >> 32);
			uint32 vcolor = static_cast<uint32>(rgbaq);
			float vf = 0.0f;
			if(m_primitiveMode.nFog)
			{
				uint8 fogByte = fog ? static_cast<uint8>(data >> 56) : static_cast<uint8>(m_nReg[GS_REG_FOG] >> 56);
				vf = static_cast<float>(0xFF - fogByte) * inv255;
			}
			float vs = 0.0f, vt = 0.0f, vq = 1.0f;
			if(m_primitiveMode.nTexture)
			{
				if(m_primitiveMode.nUseUV)
				{
					uint64 u = m_nReg[GS_REG_UV];
					vs = static_cast<float>(static_cast<uint32>(u & 0x3FFF)) * (inv16 * m_invTexWidth);
					vt = static_cast<float>(static_cast<uint32>((u >> 16) & 0x3FFF)) * (inv16 * m_invTexHeight);
				}
				else
				{
					uint64 st = m_nReg[GS_REG_ST];
					uint32 sb = static_cast<uint32>(st);
					uint32 tb = static_cast<uint32>(st >> 32);
					uint32 qb = static_cast<uint32>(rgbaq >> 32);
					memcpy(&vs, &sb, sizeof(float));
					memcpy(&vt, &tb, sizeof(float));
					memcpy(&vq, &qb, sizeof(float));
				}
			}

			CDraw::PRIM_VERTEX v = {vx, vy, vz, vcolor, vs, vt, vq, vf};
			if(m_stripHaveVerts < 2)
			{
				m_stripVerts[m_stripHaveVerts++] = v;
				m_vtxCount--;
				return;
			}
			if(drawingKick)
			{
				const auto& v0 = m_stripVerts[0];
				const auto& v1 = m_stripVerts[1];
				float minX = v0.x < v1.x ? (v0.x < vx ? v0.x : vx) : (v1.x < vx ? v1.x : vx);
				float minY = v0.y < v1.y ? (v0.y < vy ? v0.y : vy) : (v1.y < vy ? v1.y : vy);
				float maxX = v0.x > v1.x ? (v0.x > vx ? v0.x : vx) : (v1.x > vx ? v1.x : vx);
				float maxY = v0.y > v1.y ? (v0.y > vy ? v0.y : vy) : (v1.y > vy ? v1.y : vy);
				m_draw->UpdateDrawBounds(minX, minY);
				m_draw->UpdateDrawBounds(maxX, maxY);
				auto* dst = m_draw->ReserveVertices(3);
				dst[0] = v0;
				dst[1] = v1;
				dst[2] = v;
				if(m_primitiveMode.nShading == 0)
				{
					dst[0].color = vcolor;
					dst[1].color = vcolor;
				}
			}
			m_stripVerts[0] = m_stripVerts[1];
			m_stripVerts[1] = v;
			m_vtxCount = 1;
			return;
		}
	}

slowVertexKick:

	if(fog)
	{
		m_vtxBuffer[m_vtxCount - 1].position = data & 0x00FFFFFFFFFFFFFFULL;
		m_vtxBuffer[m_vtxCount - 1].rgbaq = m_nReg[GS_REG_RGBAQ];
		m_vtxBuffer[m_vtxCount - 1].uv = m_nReg[GS_REG_UV];
		m_vtxBuffer[m_vtxCount - 1].st = m_nReg[GS_REG_ST];
		m_vtxBuffer[m_vtxCount - 1].fog = static_cast<uint8>(data >> 56);
	}
	else
	{
		m_vtxBuffer[m_vtxCount - 1].position = data;
		m_vtxBuffer[m_vtxCount - 1].rgbaq = m_nReg[GS_REG_RGBAQ];
		m_vtxBuffer[m_vtxCount - 1].uv = m_nReg[GS_REG_UV];
		m_vtxBuffer[m_vtxCount - 1].st = m_nReg[GS_REG_ST];
		m_vtxBuffer[m_vtxCount - 1].fog = static_cast<uint8>(m_nReg[GS_REG_FOG] >> 56);
	}

	m_vtxCount--;

	if(m_vtxCount == 0)
	{
		if((m_nReg[GS_REG_PRMODECONT] & 1) != 0)
		{
			m_primitiveMode <<= m_nReg[GS_REG_PRIM];
		}
		else
		{
			m_primitiveMode <<= m_nReg[GS_REG_PRMODE];
		}

		if(drawingKick && (m_renderContextDirty || (static_cast<uint64>(m_primitiveMode) != m_lastPrimitiveMode)))
		{
			SetRenderingContext(m_primitiveMode);
		}

		switch(m_primitiveType)
		{
		case PRIM_POINT:
			if(drawingKick) Prim_Point();
			m_vtxCount = 1;
			break;
		case PRIM_LINE:
			if(drawingKick) Prim_Line();
			m_vtxCount = 2;
			break;
		case PRIM_LINESTRIP:
			if(drawingKick) Prim_Line();
			m_vtxBuffer[1] = m_vtxBuffer[0];
			m_vtxCount = 1;
			break;
		case PRIM_TRIANGLE:
			if(drawingKick) Prim_Triangle();
			m_vtxCount = 3;
			break;
		case PRIM_TRIANGLESTRIP:
			if(drawingKick) Prim_Triangle();
			m_vtxBuffer[2] = m_vtxBuffer[1];
			m_vtxBuffer[1] = m_vtxBuffer[0];
			m_vtxCount = 1;
			if(drawingKick && !m_renderContextDirty)
			{
				static constexpr float inv16 = 1.0f / 16.0f;
				static constexpr float inv255 = 1.0f / 255.0f;
				const float scaleU = inv16 * m_invTexWidth;
				const float scaleV = inv16 * m_invTexHeight;
				auto unpackBuf = [&](const VERTEX& src) -> CDraw::PRIM_VERTEX {
					float vx = static_cast<float>(static_cast<uint32>(src.position & 0xFFFFU)) * inv16 - m_primOfsX;
					float vy = static_cast<float>(static_cast<uint32>((src.position >> 16) & 0xFFFFU)) * inv16 - m_primOfsY;
					uint32 vz = static_cast<uint32>(src.position >> 32);
					uint32 vcolor = static_cast<uint32>(src.rgbaq);
					float vf = m_primitiveMode.nFog ? (static_cast<float>(0xFF - src.fog) * inv255) : 0.0f;
					float vs = 0.0f, vt = 0.0f, vq = 1.0f;
					if(m_primitiveMode.nTexture)
					{
						if(m_primitiveMode.nUseUV)
						{
							vs = static_cast<float>(static_cast<uint32>(src.uv & 0x3FFFU)) * scaleU;
							vt = static_cast<float>(static_cast<uint32>((src.uv >> 16) & 0x3FFFU)) * scaleV;
						}
						else
						{
							uint32 sb = static_cast<uint32>(src.st);
							uint32 tb = static_cast<uint32>(src.st >> 32);
							uint32 qb = static_cast<uint32>(src.rgbaq >> 32);
							memcpy(&vs, &sb, sizeof(float));
							memcpy(&vt, &tb, sizeof(float));
							memcpy(&vq, &qb, sizeof(float));
						}
					}
					return {vx, vy, vz, vcolor, vs, vt, vq, vf};
				};
				m_stripVerts[0] = unpackBuf(m_vtxBuffer[2]);
				m_stripVerts[1] = unpackBuf(m_vtxBuffer[1]);
				m_stripHaveVerts = 2;
				m_stripFastActive = true;
			}
			break;
		case PRIM_TRIANGLEFAN:
			if(drawingKick) Prim_Triangle();
			m_vtxBuffer[1] = m_vtxBuffer[0];
			m_vtxCount = 1;
			break;
		case PRIM_SPRITE:
			if(drawingKick) Prim_Sprite();
			m_vtxCount = 2;
			break;
		}
	}
}

static std::pair<uint32, uint32> GetMipLevelInfo(uint32 level, const CGSHandler::MIPTBP1& miptbp1, const CGSHandler::MIPTBP2& miptbp2)
{
	switch(level)
	{
	default:
		assert(false);
		return std::pair<uint32, uint32>(0, 0);
	case 1:
		return std::pair<uint32, uint32>(miptbp1.GetTbp1(), miptbp1.GetTbw1());
	case 2:
		return std::pair<uint32, uint32>(miptbp1.GetTbp2(), miptbp1.GetTbw2());
	case 3:
		return std::pair<uint32, uint32>(miptbp1.GetTbp3(), miptbp1.GetTbw3());
	case 4:
		return std::pair<uint32, uint32>(miptbp2.GetTbp4(), miptbp2.GetTbw4());
	case 5:
		return std::pair<uint32, uint32>(miptbp2.GetTbp5(), miptbp2.GetTbw5());
	case 6:
		return std::pair<uint32, uint32>(miptbp2.GetTbp6(), miptbp2.GetTbw6());
	}
}

void CGSH_Vulkan::SetRenderingContext(uint64 primReg)
{
	auto prim = make_convertible<PRMODE>(primReg);

	unsigned int context = prim.nContext;

	auto offset = make_convertible<XYOFFSET>(m_nReg[GS_REG_XYOFFSET_1 + context]);
	auto frame = make_convertible<FRAME>(m_nReg[GS_REG_FRAME_1 + context]);
	auto zbuf = make_convertible<ZBUF>(m_nReg[GS_REG_ZBUF_1 + context]);
	auto tex0 = make_convertible<TEX0>(m_nReg[GS_REG_TEX0_1 + context]);
	auto tex1 = make_convertible<TEX1>(m_nReg[GS_REG_TEX1_1 + context]);
	auto miptbp1 = make_convertible<MIPTBP1>(m_nReg[GS_REG_MIPTBP1_1 + context]);
	auto miptbp2 = make_convertible<MIPTBP2>(m_nReg[GS_REG_MIPTBP2_1 + context]);
	auto clamp = make_convertible<CLAMP>(m_nReg[GS_REG_CLAMP_1 + context]);
	auto alpha = make_convertible<ALPHA>(m_nReg[GS_REG_ALPHA_1 + context]);
	auto scissor = make_convertible<SCISSOR>(m_nReg[GS_REG_SCISSOR_1 + context]);
	auto test = make_convertible<TEST>(m_nReg[GS_REG_TEST_1 + context]);
	auto texA = make_convertible<TEXA>(m_nReg[GS_REG_TEXA]);
	auto fogCol = make_convertible<FOGCOL>(m_nReg[GS_REG_FOGCOL]);
	auto scanMask = m_nReg[GS_REG_SCANMSK] & 3;
	auto colClamp = m_nReg[GS_REG_COLCLAMP] & 1;
	auto fba = m_nReg[GS_REG_FBA_1 + context] & 1;

	auto pipelineCaps = make_convertible<CDraw::PIPELINE_CAPS>(0);
	if(prim.nTexture)
	{
		pipelineCaps.hasTexture = 1;
		pipelineCaps.textureHasAlpha = tex0.nColorComp;
		pipelineCaps.textureFunction = tex0.nFunction;
		pipelineCaps.texClampU = clamp.nWMS;
		pipelineCaps.texClampV = clamp.nWMT;
		pipelineCaps.textureFormat = tex0.nPsm;
		bool isIdtex = CGsPixelFormats::IsPsmIDTEX(tex0.nPsm);
		pipelineCaps.clutFormat = isIdtex ? tex0.nCPSM : 0;
		bool usesTexAlpha = false;
		if(tex0.nColorComp)
		{
			if(isIdtex)
			{
				usesTexAlpha = (tex0.nCPSM == CGSHandler::PSMCT16) || (tex0.nCPSM == CGSHandler::PSMCT16S);
			}
			else
			{
				usesTexAlpha = (tex0.nPsm == CGSHandler::PSMCT24) || (tex0.nPsm == CGSHandler::PSMCT16) ||
				               (tex0.nPsm == CGSHandler::PSMCT16S) || (tex0.nPsm == CGSHandler::PSMZ16) ||
				               (tex0.nPsm == CGSHandler::PSMZ16S);
			}
		}
		pipelineCaps.textureBlackIsTransparent = usesTexAlpha ? texA.nAEM : 0;
	}
	pipelineCaps.hasFog = prim.nFog;
	pipelineCaps.scanMask = scanMask;
	pipelineCaps.hasAlphaBlending = prim.nAlpha && m_alphaBlendingEnabled;
	pipelineCaps.colClamp = 0;
	pipelineCaps.fba = fba;
	pipelineCaps.hasDstAlphaTest = test.nDestAlphaEnabled;
	pipelineCaps.dstAlphaTestRef = test.nDestAlphaEnabled ? test.nDestAlphaMode : 0;
	pipelineCaps.writeDepth = (zbuf.nMask == 0) && (test.nDepthEnabled != 0); //Depth test disabled -> no writes to depth buffer
	pipelineCaps.framebufferFormat = frame.nPsm;
	pipelineCaps.depthbufferFormat = zbuf.nPsm | 0x30;

	switch(m_primitiveType)
	{
	default:
		assert(false);
		[[fallthrough]];
	case PRIM_TRIANGLE:
	case PRIM_TRIANGLEFAN:
	case PRIM_TRIANGLESTRIP:
	case PRIM_SPRITE:
		pipelineCaps.primitiveType = CDraw::PIPELINE_PRIMITIVE_TRIANGLE;
		break;
	case PRIM_LINE:
	case PRIM_LINESTRIP:
		pipelineCaps.primitiveType = CDraw::PIPELINE_PRIMITIVE_LINE;
		break;
	case PRIM_POINT:
		pipelineCaps.primitiveType = CDraw::PIPELINE_PRIMITIVE_POINT;
		break;
	}

	uint32 fbWriteMask = ~frame.nMask;

	uint32 texBufPtr = tex0.GetBufPtr();
	uint32 texBufWidth = tex0.GetBufWidth();
	uint32 texMipLevel = 0;

	if(prim.nTexture)
	{
		bool minLinear = false;
		bool magLinear = false;

		switch(tex1.nMinFilter)
		{
		case MIN_FILTER_NEAREST:
		case MIN_FILTER_NEAREST_MIP_NEAREST:
		case MIN_FILTER_NEAREST_MIP_LINEAR:
			minLinear = false;
			break;
		case MIN_FILTER_LINEAR:
		case MIN_FILTER_LINEAR_MIP_NEAREST:
		case MIN_FILTER_LINEAR_MIP_LINEAR:
			minLinear = true;
			break;
		}

		switch(tex1.nMagFilter)
		{
		case MAG_FILTER_NEAREST:
			magLinear = false;
			break;
		case MAG_FILTER_LINEAR:
			magLinear = true;
			break;
		}

		//Ignore min filter if we don't have mipmap levels.
		if(tex1.nMaxMip == 0)
		{
			minLinear = magLinear;
		}
		else
		{
			bool hasMip = (tex1.nMinFilter >= MIN_FILTER_NEAREST_MIP_NEAREST);
			if(hasMip)
			{
				if(tex1.nLODMethod == LOD_CALC_STATIC)
				{
					int k = trunc(tex1.GetK());
					texMipLevel = std::clamp<int>(k, 0, tex1.nMaxMip);
					if(texMipLevel != 0)
					{
						auto mipLevelInfo = GetMipLevelInfo(texMipLevel, miptbp1, miptbp2);
						texBufPtr = mipLevelInfo.first;
						texBufWidth = mipLevelInfo.second;
					}
				}
				else
				{
					pipelineCaps.textureUseDynamicMipLOD = true;
				}
			}
		}

		pipelineCaps.textureUseLinearFiltering = (minLinear && magLinear);
	}

	if(pipelineCaps.hasAlphaBlending)
	{
		bool isIdentityBlend =
		    (((alpha.nA == alpha.nB) || ((alpha.nC == ALPHABLEND_C_FIX) && (alpha.nFix == 0))) && (alpha.nD == ALPHABLEND_ABD_CS)) ||
		    ((alpha.nA == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nFix == 0x80) &&
		     (((alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nD == ALPHABLEND_ABD_ZERO)) ||
		      ((alpha.nB == ALPHABLEND_ABD_CD) && (alpha.nD == ALPHABLEND_ABD_CD))));
		if(isIdentityBlend)
		{
			pipelineCaps.hasAlphaBlending = 0;
			if(m_statsEnabled) m_statsAlphaIdentity++;
		}
		else
		{
			pipelineCaps.colClamp = colClamp;
			pipelineCaps.alphaA = alpha.nA;
			pipelineCaps.alphaB = alpha.nB;
			pipelineCaps.alphaC = alpha.nC;
			pipelineCaps.alphaD = alpha.nD;
		}
	}

	pipelineCaps.depthTestFunction = test.nDepthMethod;
	if(!test.nDepthEnabled || !m_depthTestingEnabled)
	{
		pipelineCaps.depthTestFunction = CGSHandler::DEPTH_TEST_ALWAYS;
	}

	//Disable depth writes if both frame buffer and z buffer share the same address
	//TODO: This could be refined to check if there's actually no frame buffer writes
	//(ie.: fbmsk, alpha testing)
	if(frame.GetBasePtr() == zbuf.GetBasePtr())
	{
		pipelineCaps.writeDepth = false;
	}

	pipelineCaps.alphaTestFunction = test.nAlphaMethod;
	pipelineCaps.alphaTestFailAction = test.nAlphaFail;
	if(!test.nAlphaEnabled || !m_alphaTestingEnabled)
	{
		pipelineCaps.alphaTestFunction = CGSHandler::ALPHA_TEST_ALWAYS;
	}

	//Convert alpha testing to write masking if possible
	if(
	    (pipelineCaps.alphaTestFunction == CGSHandler::ALPHA_TEST_NEVER) &&
	    (pipelineCaps.alphaTestFailAction == CGSHandler::ALPHA_TEST_FAIL_FBONLY))
	{
		pipelineCaps.alphaTestFunction = CGSHandler::ALPHA_TEST_ALWAYS;
		pipelineCaps.writeDepth = 0;
	}

	if(
	    (pipelineCaps.alphaTestFunction == CGSHandler::ALPHA_TEST_NEVER) &&
	    (pipelineCaps.alphaTestFailAction == CGSHandler::ALPHA_TEST_FAIL_RGBONLY))
	{
		pipelineCaps.alphaTestFunction = CGSHandler::ALPHA_TEST_ALWAYS;
		pipelineCaps.writeDepth = 0;
		fbWriteMask &= 0x00FFFFFF;
	}

	if(pipelineCaps.alphaTestFunction == CGSHandler::ALPHA_TEST_ALWAYS)
	{
		pipelineCaps.alphaTestFailAction = 0;
	}

	switch(frame.nPsm)
	{
	case CGSHandler::PSMCT32:
	case CGSHandler::PSMZ32:
		pipelineCaps.maskColor = (fbWriteMask != 0xFFFFFFFF);
		break;
	case CGSHandler::PSMCT24:
	case CGSHandler::PSMZ24:
		fbWriteMask = fbWriteMask & 0x00FFFFFF;
		pipelineCaps.maskColor = (fbWriteMask != 0x00FFFFFF);
		break;
	case CGSHandler::PSMCT16:
	case CGSHandler::PSMCT16S:
	case CGSHandler::PSMZ16S:
		fbWriteMask = RGBA32ToRGBA16(fbWriteMask) & 0xFFFF;
		pipelineCaps.maskColor = (fbWriteMask != 0xFFFF);
		break;
	default:
		assert(false);
		break;
	}

	//Check if we need to save a copy of RAM because primitive writes to texture area
	bool needsTextureCopy = false;
	uint32 memoryCopyAddress = 0;
	uint32 memoryCopySize = 0;
	if((m_primitiveType == PRIM_SPRITE) && pipelineCaps.hasTexture)
	{
		uint32 texBufPtr = tex0.GetBufPtr();
		uint32 frameBufPtr = frame.GetBasePtr();
		uint32 depthBufPtr = zbuf.GetBasePtr();
		bool isTexUpperBytePsm = CGsPixelFormats::IsPsmUpperByte(tex0.nPsm);
		{
			bool isFrame24Bits = CGsPixelFormats::IsPsm24Bits(frame.nPsm);
			needsTextureCopy |= (texBufPtr == frameBufPtr) && !(isTexUpperBytePsm && isFrame24Bits);
		}
		if(pipelineCaps.writeDepth)
		{
			bool isDepth24Bits = CGsPixelFormats::IsPsm24Bits(zbuf.nPsm);
			needsTextureCopy |= (texBufPtr == depthBufPtr) && !(isTexUpperBytePsm && isDepth24Bits);
		}
		if(needsTextureCopy)
		{
			CGsCachedArea textureArea;
			textureArea.SetArea(tex0.nPsm, tex0.nBufPtr, tex0.GetBufWidth(), tex0.GetHeight());
			memoryCopyAddress = texBufPtr;
			memoryCopySize = textureArea.GetSize();
			if((memoryCopyAddress + memoryCopySize) > RAMSIZE)
			{
				//Some games (Castlevania: Curse of Darkness) have a texture that goes beyond
				//RAMSIZE, make sure we stay inside bounds.
				memoryCopySize = RAMSIZE - memoryCopyAddress;
			}
			m_draw->SetMemoryCopyParams(memoryCopyAddress, memoryCopySize);
		}
	}
	pipelineCaps.textureUseMemoryCopy = needsTextureCopy;

	m_draw->SetPipelineCaps(pipelineCaps);
	m_draw->SetFramebufferParams(frame.GetBasePtr(), frame.GetWidth(), fbWriteMask);
	if((pipelineCaps.depthTestFunction != CGSHandler::DEPTH_TEST_ALWAYS) || pipelineCaps.writeDepth)
	{
		m_draw->SetDepthbufferParams(zbuf.GetBasePtr(), frame.GetWidth());
	}
	if(pipelineCaps.hasTexture)
	{
		uint32 texCsa = CGsPixelFormats::IsPsmIDTEX(tex0.nPsm) ? (tex0.nCSA * 0x10) : 0;
		m_draw->SetTextureParams(texBufPtr, texBufWidth, tex0.GetWidth(), tex0.GetHeight(), texMipLevel, texCsa);
		if(
		    pipelineCaps.textureUseDynamicMipLOD &&
		    (!m_regState.isValid ||
		     (m_regState.tex1 != tex1) ||
		     (m_regState.miptbp1 != miptbp1) ||
		     (m_regState.miptbp2 != miptbp2)))
		{
			CDraw::MipBufs mipBufs;
			for(int i = 1; i <= tex1.nMaxMip; i++)
			{
				mipBufs[i - 1] = GetMipLevelInfo(i, miptbp1, miptbp2);
			}
			for(int i = tex1.nMaxMip + 1; i <= 6; i++)
			{
				mipBufs[i - 1] = std::pair<uint32, uint32>(0, 0);
			}
			m_draw->SetMipParams(mipBufs, tex1.nMaxMip, tex1.GetK(), tex1.nLODL);
			m_regState.isValid = true;
			m_regState.tex1 = tex1;
			m_regState.miptbp1 = miptbp1;
			m_regState.miptbp2 = miptbp2;
		}
		if(pipelineCaps.textureHasAlpha)
		{
			m_draw->SetTextureAlphaParams(texA.nTA0, texA.nTA1);
		}
		if((clamp.nWMS >= CLAMP_MODE_REGION_CLAMP) || (clamp.nWMT >= CLAMP_MODE_REGION_CLAMP))
		{
			m_draw->SetTextureClampParams(
			    clamp.GetMinU(), clamp.GetMinV(),
			    clamp.GetMaxU(), clamp.GetMaxV());
		}
	}
	if(pipelineCaps.hasFog)
	{
		m_draw->SetFogParams(
		    static_cast<float>(fogCol.nFCR) / 255.f,
		    static_cast<float>(fogCol.nFCG) / 255.f,
		    static_cast<float>(fogCol.nFCB) / 255.f);
	}
	if(pipelineCaps.hasAlphaBlending && (pipelineCaps.alphaC == ALPHABLEND_C_FIX))
	{
		m_draw->SetAlphaBlendingParams(alpha.nFix);
	}
	if((pipelineCaps.alphaTestFunction != CGSHandler::ALPHA_TEST_ALWAYS) &&
	   (pipelineCaps.alphaTestFunction != CGSHandler::ALPHA_TEST_NEVER))
	{
		m_draw->SetAlphaTestParams(test.nAlphaRef);
	}
	m_draw->SetScissor(scissor.scax0, scissor.scay0,
	                   scissor.scax1 - scissor.scax0 + 1,
	                   scissor.scay1 - scissor.scay0 + 1);

	m_fbBasePtr = frame.GetBasePtr();

	m_primOfsX = offset.GetX();
	m_primOfsY = offset.GetY();

	m_texWidth = tex0.GetWidth();
	m_texHeight = tex0.GetHeight();
	m_invTexWidth = 1.0f / static_cast<float>(m_texWidth);
	m_invTexHeight = 1.0f / static_cast<float>(m_texHeight);

	m_renderContextDirty = false;
	m_lastPrimitiveMode = static_cast<uint64>(prim);
}

void CGSH_Vulkan::Prim_Point()
{
	auto xyz = make_convertible<XYZ>(m_vtxBuffer[0].position);
	auto rgbaq = make_convertible<RGBAQ>(m_vtxBuffer[0].rgbaq);

	float x = xyz.GetX();
	float y = xyz.GetY();
	uint32 z = xyz.nZ;

	x -= m_primOfsX;
	y -= m_primOfsY;

	auto color = MakeColor(
	    rgbaq.nR, rgbaq.nG,
	    rgbaq.nB, rgbaq.nA);

	// clang-format off
	CDraw::PRIM_VERTEX vertex =
	{
		//x, y, z, color, s, t, q, f
		  x, y, z, color, 0, 0, 1, 0,
	};
	// clang-format on

	m_draw->AddVertices(&vertex, &vertex + 1);
}

void CGSH_Vulkan::Prim_Line()
{
	XYZ pos[2];
	pos[0] <<= m_vtxBuffer[1].position;
	pos[1] <<= m_vtxBuffer[0].position;

	float x1 = pos[0].GetX(), x2 = pos[1].GetX();
	float y1 = pos[0].GetY(), y2 = pos[1].GetY();
	uint32 z1 = pos[0].nZ, z2 = pos[1].nZ;

	RGBAQ rgbaq[2];
	rgbaq[0] <<= m_vtxBuffer[1].rgbaq;
	rgbaq[1] <<= m_vtxBuffer[0].rgbaq;

	x1 -= m_primOfsX;
	x2 -= m_primOfsX;

	y1 -= m_primOfsY;
	y2 -= m_primOfsY;

	float s[2] = {0, 0};
	float t[2] = {0, 0};
	float q[2] = {1, 1};

	if(m_primitiveMode.nTexture)
	{
		if(m_primitiveMode.nUseUV)
		{
			UV uv[3];
			uv[0] <<= m_vtxBuffer[1].uv;
			uv[1] <<= m_vtxBuffer[0].uv;

			s[0] = uv[0].GetU() / static_cast<float>(m_texWidth);
			s[1] = uv[1].GetU() / static_cast<float>(m_texWidth);

			t[0] = uv[0].GetV() / static_cast<float>(m_texHeight);
			t[1] = uv[1].GetV() / static_cast<float>(m_texHeight);

			m_lastLineU = s[0];
			m_lastLineV = t[0];
		}
		else
		{
			ST st[2];
			st[0] <<= m_vtxBuffer[1].st;
			st[1] <<= m_vtxBuffer[0].st;

			s[0] = st[0].nS;
			s[1] = st[1].nS;
			t[0] = st[0].nT;
			t[1] = st[1].nT;

			q[0] = rgbaq[0].nQ;
			q[1] = rgbaq[1].nQ;
		}
	}

	auto color1 = MakeColor(
	    rgbaq[0].nR, rgbaq[0].nG,
	    rgbaq[0].nB, rgbaq[0].nA);

	auto color2 = MakeColor(
	    rgbaq[1].nR, rgbaq[1].nG,
	    rgbaq[1].nB, rgbaq[1].nA);

	// clang-format off
	CDraw::PRIM_VERTEX vertices[] =
	{
		{	x1, y1, z1, color1, s[0], t[0], q[0], 0 },
		{	x2, y2, z2, color2, s[1], t[1], q[1], 0 },
	};
	// clang-format on

	m_draw->AddVertices(std::begin(vertices), std::end(vertices));
}

void CGSH_Vulkan::Prim_Triangle()
{
	const uint64 p0 = m_vtxBuffer[2].position;
	const uint64 p1 = m_vtxBuffer[1].position;
	const uint64 p2 = m_vtxBuffer[0].position;

	static constexpr float inv16 = 1.0f / 16.0f;
	const float x1 = static_cast<float>(static_cast<uint32>(p0 & 0xFFFF)) * inv16 - m_primOfsX;
	const float y1 = static_cast<float>(static_cast<uint32>((p0 >> 16) & 0xFFFF)) * inv16 - m_primOfsY;
	const uint32 z1 = static_cast<uint32>(p0 >> 32);

	const float x2 = static_cast<float>(static_cast<uint32>(p1 & 0xFFFF)) * inv16 - m_primOfsX;
	const float y2 = static_cast<float>(static_cast<uint32>((p1 >> 16) & 0xFFFF)) * inv16 - m_primOfsY;
	const uint32 z2 = static_cast<uint32>(p1 >> 32);

	const float x3 = static_cast<float>(static_cast<uint32>(p2 & 0xFFFF)) * inv16 - m_primOfsX;
	const float y3 = static_cast<float>(static_cast<uint32>((p2 >> 16) & 0xFFFF)) * inv16 - m_primOfsY;
	const uint32 z3 = static_cast<uint32>(p2 >> 32);

	float s0 = 0, s1 = 0, s2 = 0;
	float t0 = 0, t1 = 0, t2 = 0;
	float q0 = 1, q1 = 1, q2 = 1;
	float f0 = 0, f1 = 0, f2 = 0;

	if(m_primitiveMode.nFog)
	{
		static constexpr float inv255 = 1.0f / 255.0f;
		f0 = static_cast<float>(0xFF - m_vtxBuffer[2].fog) * inv255;
		f1 = static_cast<float>(0xFF - m_vtxBuffer[1].fog) * inv255;
		f2 = static_cast<float>(0xFF - m_vtxBuffer[0].fog) * inv255;
	}

	if(m_primitiveMode.nTexture)
	{
		if(m_primitiveMode.nUseUV)
		{
			const float scaleU = inv16 * m_invTexWidth;
			const float scaleV = inv16 * m_invTexHeight;
			const uint64 u0 = m_vtxBuffer[2].uv;
			const uint64 u1 = m_vtxBuffer[1].uv;
			const uint64 u2 = m_vtxBuffer[0].uv;
			s0 = static_cast<float>(static_cast<uint32>(u0 & 0x3FFF)) * scaleU;
			t0 = static_cast<float>(static_cast<uint32>((u0 >> 16) & 0x3FFF)) * scaleV;
			s1 = static_cast<float>(static_cast<uint32>(u1 & 0x3FFF)) * scaleU;
			t1 = static_cast<float>(static_cast<uint32>((u1 >> 16) & 0x3FFF)) * scaleV;
			s2 = static_cast<float>(static_cast<uint32>(u2 & 0x3FFF)) * scaleU;
			t2 = static_cast<float>(static_cast<uint32>((u2 >> 16) & 0x3FFF)) * scaleV;
		}
		else
		{
			auto st0 = make_convertible<ST>(m_vtxBuffer[2].st);
			auto st1 = make_convertible<ST>(m_vtxBuffer[1].st);
			auto st2 = make_convertible<ST>(m_vtxBuffer[0].st);
			s0 = st0.nS;
			t0 = st0.nT;
			s1 = st1.nS;
			t1 = st1.nT;
			s2 = st2.nS;
			t2 = st2.nT;
			uint32 qb0 = static_cast<uint32>(m_vtxBuffer[2].rgbaq >> 32);
			uint32 qb1 = static_cast<uint32>(m_vtxBuffer[1].rgbaq >> 32);
			uint32 qb2 = static_cast<uint32>(m_vtxBuffer[0].rgbaq >> 32);
			memcpy(&q0, &qb0, sizeof(float));
			memcpy(&q1, &qb1, sizeof(float));
			memcpy(&q2, &qb2, sizeof(float));
		}
	}

	const uint32 color3 = static_cast<uint32>(m_vtxBuffer[0].rgbaq);
	const uint32 color1 = (m_primitiveMode.nShading == 0) ? color3 : static_cast<uint32>(m_vtxBuffer[2].rgbaq);
	const uint32 color2 = (m_primitiveMode.nShading == 0) ? color3 : static_cast<uint32>(m_vtxBuffer[1].rgbaq);

	float minX = x1 < x2 ? (x1 < x3 ? x1 : x3) : (x2 < x3 ? x2 : x3);
	float minY = y1 < y2 ? (y1 < y3 ? y1 : y3) : (y2 < y3 ? y2 : y3);
	float maxX = x1 > x2 ? (x1 > x3 ? x1 : x3) : (x2 > x3 ? x2 : x3);
	float maxY = y1 > y2 ? (y1 > y3 ? y1 : y3) : (y2 > y3 ? y2 : y3);
	m_draw->UpdateDrawBounds(minX, minY);
	m_draw->UpdateDrawBounds(maxX, maxY);
	auto* dst = m_draw->ReserveVertices(3);
	dst[0] = {x1, y1, z1, color1, s0, t0, q0, f0};
	dst[1] = {x2, y2, z2, color2, s1, t1, q1, f1};
	dst[2] = {x3, y3, z3, color3, s2, t2, q2, f2};
	if(m_primitiveType == PRIM_TRIANGLESTRIP)
	{
		m_stripVerts[0] = dst[1];
		if(m_primitiveMode.nShading == 0)
		{
			m_stripVerts[0].color = static_cast<uint32>(m_vtxBuffer[1].rgbaq);
		}
		m_stripVerts[1] = dst[2];
		m_stripHaveVerts = 2;
		m_stripFastActive = true;
	}
}

void CGSH_Vulkan::Prim_Sprite()
{
	XYZ pos[2];
	pos[0] <<= m_vtxBuffer[1].position;
	pos[1] <<= m_vtxBuffer[0].position;

	float x1 = pos[0].GetX(), y1 = pos[0].GetY();
	float x2 = pos[1].GetX(), y2 = pos[1].GetY();
	uint32 z = pos[1].nZ;

	RGBAQ rgbaq[2];
	rgbaq[0] <<= m_vtxBuffer[1].rgbaq;
	rgbaq[1] <<= m_vtxBuffer[0].rgbaq;

	x1 -= m_primOfsX;
	x2 -= m_primOfsX;

	y1 -= m_primOfsY;
	y2 -= m_primOfsY;

	float s[2] = {0, 0};
	float t[2] = {0, 0};

	float f = 0;

	if(m_primitiveMode.nFog)
	{
		f = static_cast<float>(0xFF - m_vtxBuffer[0].fog) / 255.0f;
	}

	if(m_primitiveMode.nTexture)
	{
		if(m_primitiveMode.nUseUV)
		{
			UV uv[2];
			uv[0] <<= m_vtxBuffer[1].uv;
			uv[1] <<= m_vtxBuffer[0].uv;

			s[0] = uv[0].GetU() / static_cast<float>(m_texWidth);
			s[1] = uv[1].GetU() / static_cast<float>(m_texWidth);

			t[0] = uv[0].GetV() / static_cast<float>(m_texHeight);
			t[1] = uv[1].GetV() / static_cast<float>(m_texHeight);
		}
		else
		{
			ST st[2];

			st[0] <<= m_vtxBuffer[1].st;
			st[1] <<= m_vtxBuffer[0].st;

			float q1 = rgbaq[1].nQ;
			float q2 = rgbaq[0].nQ;
			if(q1 == 0) q1 = 1;
			if(q2 == 0) q2 = 1;

			s[0] = st[0].nS / q1;
			s[1] = st[1].nS / q2;

			t[0] = st[0].nT / q1;
			t[1] = st[1].nT / q2;
		}
	}

	auto color = MakeColor(
	    rgbaq[1].nR, rgbaq[1].nG,
	    rgbaq[1].nB, rgbaq[1].nA);

	{
		CGsSpriteRect rect(x1, y1, x2, y2);
		CheckSpriteCachedClutInvalidation(rect);
	}

	if(m_draw->IsMemoryCopyActive())
	{
		// clang-format off
		CDraw::PRIM_VERTEX vertices[] =
		{
			{x1, y1, z, color, s[0], t[0], 1, f},
			{x2, y1, z, color, s[1], t[0], 1, f},
			{x1, y2, z, color, s[0], t[1], 1, f},

			{x1, y2, z, color, s[0], t[1], 1, f},
			{x2, y1, z, color, s[1], t[0], 1, f},
			{x2, y2, z, color, s[1], t[1], 1, f},
		};
		// clang-format on
		m_draw->AddVertices(std::begin(vertices), std::end(vertices));
	}
	else
	{
		m_draw->UpdateDrawBounds(x1, y1);
		m_draw->UpdateDrawBounds(x2, y2);
		auto* dst = m_draw->ReserveVertices(6);
		dst[0] = {x1, y1, z, color, s[0], t[0], 1, f};
		dst[1] = {x2, y1, z, color, s[1], t[0], 1, f};
		dst[2] = {x1, y2, z, color, s[0], t[1], 1, f};
		dst[3] = {x1, y2, z, color, s[0], t[1], 1, f};
		dst[4] = {x2, y1, z, color, s[1], t[0], 1, f};
		dst[5] = {x2, y2, z, color, s[1], t[1], 1, f};
	}
}

void CGSH_Vulkan::ClearClutCache()
{
	if(m_statsEnabled) m_statsClutClears++;
	memset(&m_clutStates, 0, sizeof(m_clutStates));
	m_lastClutFillCacheIndex = -1;
	m_lastClutFillPointer = -1;
	m_renderContextDirty = true;
}

void CGSH_Vulkan::InvalidateClutCacheRange(uint32 address, uint32 size)
{
	uint64 xferStart = address;
	uint64 xferEnd = xferStart + size;
	bool clearedAny = false;
	for(uint32 i = 0; i < CLUT_CACHE_SIZE; i++)
	{
		if(static_cast<uint64>(m_clutStates[i]) == 0) continue;
		uint64 clutStart = 0;
		uint64 clutEnd = RAMSIZE;
		if(m_clutStates[i].csm == 0)
		{
			clutStart = (m_clutStates[i].cbp * 256) & ~(CGsPixelFormats::PAGESIZE - 1);
			clutEnd = clutStart + CGsPixelFormats::PAGESIZE * 2;
		}
		if((xferStart < clutEnd) && (clutStart < xferEnd))
		{
			m_clutStates[i] <<= 0;
			if(static_cast<int32>(i) == m_lastClutFillCacheIndex)
			{
				m_lastClutFillCacheIndex = -1;
				m_lastClutFillPointer = -1;
			}
			clearedAny = true;
		}
	}
	if(clearedAny)
	{
		m_renderContextDirty = true;
		if(m_statsEnabled) m_statsClutClears++;
	}
}

int32 CGSH_Vulkan::FindCachedClut(const CLUTKEY& key) const
{
	for(uint32 i = 0; i < CLUT_CACHE_SIZE; i++)
	{
		if(m_clutStates[i] == key)
		{
			return i;
		}
	}
	return -1;
}

void CGSH_Vulkan::CheckSpriteCachedClutInvalidation(const CGsSpriteRect& rect)
{
	//This is specific to Gran Turismo 4, but could be expanded to work with other games
	//GT4 writes to an area in GS RAM then uses this as a CLUT in one of its post-processing effects.
	//We flush the whole CLUT cache if we find a sprite draw that writes to any previous location of cached cluts
	if((rect.left == 0) && (rect.top == 0) && (rect.GetWidth() == 8) && (rect.GetHeight() == 16))
	{
		bool foundClut = false;
		for(uint32 i = 0; i < CLUT_CACHE_SIZE; i++)
		{
			uint32 cbpBasePtr = m_clutStates[i].cbp * 0x100;
			if(cbpBasePtr == m_fbBasePtr)
			{
				foundClut = true;
				break;
			}
		}
		if(foundClut)
		{
			memset(&m_clutStates, 0, sizeof(m_clutStates));
			m_lastClutFillCacheIndex = -1;
			m_lastClutFillPointer = -1;
			m_draw->FlushVertices();
			m_renderContextDirty = true;
		}
	}
}

CGSH_Vulkan::CLUTKEY CGSH_Vulkan::MakeCachedClutKey(const TEX0& tex0, const TEXCLUT& texClut)
{
	auto clutKey = make_convertible<CLUTKEY>(0);
	clutKey.idx4 = CGsPixelFormats::IsPsmIDTEX4(tex0.nPsm);
	clutKey.cbp = tex0.nCBP;
	clutKey.cpsm = tex0.nCPSM;
	clutKey.csm = tex0.nCSM;
	clutKey.csa = tex0.nCSA;
	clutKey.cbw = texClut.nCBW;
	clutKey.cou = texClut.nCOU;
	clutKey.cov = texClut.nCOV;
	return clutKey;
}

/////////////////////////////////////////////////////////////
// Other Functions
/////////////////////////////////////////////////////////////

void CGSH_Vulkan::SubmitWriteBufferImpl(const RegisterWrite* writeStart, const RegisterWrite* writeEnd)
{
	if(writeStart != writeEnd) m_regsDirty = true;
	for(auto write = writeStart; write != writeEnd;)
	{
		if(m_xferBuffer.empty() && m_primitiveType == PRIM_TRIANGLESTRIP && m_stripFastActive && !m_renderContextDirty && m_drawEnabled && !m_pendingPrim)
		{
			const bool hasFog = (m_primitiveMode.nFog != 0);
			const bool hasTex = (m_primitiveMode.nTexture != 0);
			const bool useUV = (m_primitiveMode.nUseUV != 0);
			const bool flatShading = (m_primitiveMode.nShading == 0);

			auto runStripLoop = [&](auto hasTexC, auto useUVC, auto hasFogC, auto flatC) {
				constexpr bool kHasTex = decltype(hasTexC)::value;
				constexpr bool kUseUV = decltype(useUVC)::value;
				constexpr bool kHasFog = decltype(hasFogC)::value;
				constexpr bool kFlat = decltype(flatC)::value;
				static constexpr float inv16 = 1.0f / 16.0f;
				static constexpr float inv255 = 1.0f / 255.0f;
				const float ofsX = m_primOfsX;
				const float ofsY = m_primOfsY;
				const float scaleU = inv16 * m_invTexWidth;
				const float scaleV = inv16 * m_invTexHeight;

				uint64 curST = m_nReg[GS_REG_ST];
				uint64 curRGBAQ = m_nReg[GS_REG_RGBAQ];
				uint64 curUV = m_nReg[GS_REG_UV];
				uint64 curFog = m_nReg[GS_REG_FOG];
				GSH_Vulkan::CDraw::PRIM_VERTEX v0 = m_stripVerts[0];
				GSH_Vulkan::CDraw::PRIM_VERTEX v1 = m_stripVerts[1];
				uint32 haveVerts = m_stripHaveVerts;
				uint32 vtxCount = m_vtxCount;
				float minX = 3.402823466e+38f;
				float minY = 3.402823466e+38f;
				float maxX = -3.402823466e+38f;
				float maxY = -3.402823466e+38f;
				bool emittedAny = false;
				bool prevKicked = false;

				GSH_Vulkan::CDraw::PRIM_VERTEX* dstPtr = m_draw->GetCurrentVertexPtr();
				uint32 availVerts = m_draw->GetRemainingVertices();
				uint32 emittedVerts = 0;

				while(write != writeEnd)
				{
					//Fast-track packed (ST, RGBAQ, XYZ*) 3-register vertex triples in the dominant strip path,
					//including ADC=1 (XYZF3/XYZ3) restarts and initial strip vertices (haveVerts < 2).
					if constexpr(kHasTex && !kUseUV && !kFlat)
					{
						while((write + 2) < writeEnd && vtxCount != 0 && availVerts >= 3 &&
						      write[0].first == GS_REG_ST && write[1].first == GS_REG_RGBAQ &&
						      (write[2].first == GS_REG_XYZF2 || write[2].first == GS_REG_XYZ2 ||
						       write[2].first == GS_REG_XYZF3 || write[2].first == GS_REG_XYZ3))
						{
							curST = write[0].second;
							curRGBAQ = write[1].second;
							uint8 r2 = write[2].first;
							uint64 data = write[2].second;
							bool drawingKick = (r2 == GS_REG_XYZF2 || r2 == GS_REG_XYZ2);
							bool fog = (r2 == GS_REG_XYZF2 || r2 == GS_REG_XYZF3);
							uint64 pos = fog ? (data & 0x00FFFFFFFFFFFFFFULL) : data;
							float vx = static_cast<float>(static_cast<uint32>(pos & 0xFFFFU)) * inv16 - ofsX;
							float vy = static_cast<float>(static_cast<uint32>((pos >> 16) & 0xFFFFU)) * inv16 - ofsY;
							uint32 vz = static_cast<uint32>(pos >> 32);
							uint32 vcolor = static_cast<uint32>(curRGBAQ);
							float vf = 0.0f;
							if constexpr(kHasFog)
							{
								uint8 fogByte = fog ? static_cast<uint8>(data >> 56) : static_cast<uint8>(curFog >> 56);
								vf = static_cast<float>(0xFF - fogByte) * inv255;
							}
							uint32 sb = static_cast<uint32>(curST);
							uint32 tb = static_cast<uint32>(curST >> 32);
							uint32 qb = static_cast<uint32>(curRGBAQ >> 32);
							float vs, vt, vq;
							memcpy(&vs, &sb, sizeof(float));
							memcpy(&vt, &tb, sizeof(float));
							memcpy(&vq, &qb, sizeof(float));
							GSH_Vulkan::CDraw::PRIM_VERTEX v = {vx, vy, vz, vcolor, vs, vt, vq, vf};
							if(haveVerts < 2)
							{
								if(haveVerts == 0) v0 = v;
								else v1 = v;
								haveVerts++;
								vtxCount--;
								prevKicked = false;
								write += 3;
								continue;
							}
							if(drawingKick)
							{
								if(!prevKicked)
								{
									if(v0.x < minX) minX = v0.x;
									if(v0.y < minY) minY = v0.y;
									if(v0.x > maxX) maxX = v0.x;
									if(v0.y > maxY) maxY = v0.y;
									if(v1.x < minX) minX = v1.x;
									if(v1.y < minY) minY = v1.y;
									if(v1.x > maxX) maxX = v1.x;
									if(v1.y > maxY) maxY = v1.y;
								}
								if(vx < minX) minX = vx;
								if(vy < minY) minY = vy;
								if(vx > maxX) maxX = vx;
								if(vy > maxY) maxY = vy;
								emittedAny = true;
								prevKicked = true;
								dstPtr[0] = v0;
								dstPtr[1] = v1;
								dstPtr[2] = v;
								dstPtr += 3;
								availVerts -= 3;
								emittedVerts += 3;
							}
							else
							{
								prevKicked = false;
							}
							v0 = v1;
							v1 = v;
							vtxCount = 1;
							write += 3;
						}
						if(write == writeEnd) break;
					}
					uint8 reg = write->first;
					uint64 data = write->second;
					if(reg == GS_REG_ST)
					{
						curST = data;
						write++;
						continue;
					}
					if(reg == GS_REG_RGBAQ)
					{
						curRGBAQ = data;
						write++;
						continue;
					}
					if(reg == GS_REG_XYZ2 || reg == GS_REG_XYZF2 || reg == GS_REG_XYZ3 || reg == GS_REG_XYZF3)
					{
						if(vtxCount == 0)
						{
							write++;
							continue;
						}
						bool drawingKick = (reg == GS_REG_XYZ2) || (reg == GS_REG_XYZF2);
						bool fog = (reg == GS_REG_XYZF2) || (reg == GS_REG_XYZF3);
						uint64 pos = fog ? (data & 0x00FFFFFFFFFFFFFFULL) : data;
						float vx = static_cast<float>(static_cast<uint32>(pos & 0xFFFFU)) * inv16 - ofsX;
						float vy = static_cast<float>(static_cast<uint32>((pos >> 16) & 0xFFFFU)) * inv16 - ofsY;
						uint32 vz = static_cast<uint32>(pos >> 32);
						uint32 vcolor = static_cast<uint32>(curRGBAQ);
						float vf = 0.0f;
						if constexpr(kHasFog)
						{
							uint8 fogByte = fog ? static_cast<uint8>(data >> 56) : static_cast<uint8>(curFog >> 56);
							vf = static_cast<float>(0xFF - fogByte) * inv255;
						}
						float vs = 0.0f, vt = 0.0f, vq = 1.0f;
						if constexpr(kHasTex)
						{
							if constexpr(kUseUV)
							{
								vs = static_cast<float>(static_cast<uint32>(curUV & 0x3FFFU)) * scaleU;
								vt = static_cast<float>(static_cast<uint32>((curUV >> 16) & 0x3FFFU)) * scaleV;
							}
							else
							{
								uint32 sb = static_cast<uint32>(curST);
								uint32 tb = static_cast<uint32>(curST >> 32);
								uint32 qb = static_cast<uint32>(curRGBAQ >> 32);
								memcpy(&vs, &sb, sizeof(float));
								memcpy(&vt, &tb, sizeof(float));
								memcpy(&vq, &qb, sizeof(float));
							}
						}

						GSH_Vulkan::CDraw::PRIM_VERTEX v = {vx, vy, vz, vcolor, vs, vt, vq, vf};
						if(haveVerts < 2)
						{
							if(haveVerts == 0) v0 = v;
							else v1 = v;
							haveVerts++;
							vtxCount--;
							prevKicked = false;
							write++;
							continue;
						}
						if(drawingKick)
						{
							GSH_Vulkan::CDraw::PRIM_VERTEX* dst = nullptr;
							if(availVerts >= 3)
							{
								dst = dstPtr;
								dstPtr += 3;
								availVerts -= 3;
								emittedVerts += 3;
							}
							else
							{
								if(emittedVerts != 0)
								{
									m_draw->AdvanceVertices(emittedVerts);
									emittedVerts = 0;
								}
								if(emittedAny)
								{
									m_draw->UpdateDrawBounds(minX, minY);
									m_draw->UpdateDrawBounds(maxX, maxY);
									minX = minY = 3.402823466e+38f;
									maxX = maxY = -3.402823466e+38f;
									emittedAny = false;
									prevKicked = false;
								}
								dst = m_draw->ReserveVertices(3);
								dstPtr = m_draw->GetCurrentVertexPtr();
								availVerts = m_draw->GetRemainingVertices();
							}
							if(!prevKicked)
							{
								if(v0.x < minX) minX = v0.x;
								if(v0.y < minY) minY = v0.y;
								if(v0.x > maxX) maxX = v0.x;
								if(v0.y > maxY) maxY = v0.y;
								if(v1.x < minX) minX = v1.x;
								if(v1.y < minY) minY = v1.y;
								if(v1.x > maxX) maxX = v1.x;
								if(v1.y > maxY) maxY = v1.y;
							}
							if(vx < minX) minX = vx;
							if(vy < minY) minY = vy;
							if(vx > maxX) maxX = vx;
							if(vy > maxY) maxY = vy;
							emittedAny = true;
							prevKicked = true;

							if constexpr(kFlat)
							{
								GSH_Vulkan::CDraw::PRIM_VERTEX f0 = v0;
								GSH_Vulkan::CDraw::PRIM_VERTEX f1 = v1;
								f0.color = vcolor;
								f1.color = vcolor;
								dst[0] = f0;
								dst[1] = f1;
								dst[2] = v;
							}
							else
							{
								dst[0] = v0;
								dst[1] = v1;
								dst[2] = v;
							}
						}
						else
						{
							prevKicked = false;
						}
						v0 = v1;
						v1 = v;
						vtxCount = 1;
						write++;
						continue;
					}
					if(reg == GS_REG_PRIM)
					{
						uint64 nextMode = ((m_nReg[GS_REG_PRMODECONT] & 1) != 0) ? data : m_nReg[GS_REG_PRMODE];
						if(((data & 0x07) == PRIM_TRIANGLESTRIP) && (nextMode == m_lastPrimitiveMode))
						{
							m_nReg[GS_REG_PRIM] = data;
							haveVerts = 0;
							vtxCount = 3;
							prevKicked = false;
							write++;
							continue;
						}
					}
					if(reg == GS_REG_UV)
					{
						curUV = data;
						write++;
						continue;
					}
					if(reg == GS_REG_FOG)
					{
						curFog = data;
						write++;
						continue;
					}
					break;
				}

				if(emittedVerts != 0)
				{
					m_draw->AdvanceVertices(emittedVerts);
				}
				m_nReg[GS_REG_ST] = curST;
				m_nReg[GS_REG_RGBAQ] = curRGBAQ;
				m_nReg[GS_REG_UV] = curUV;
				m_nReg[GS_REG_FOG] = curFog;
				m_stripVerts[0] = v0;
				m_stripVerts[1] = v1;
				m_stripHaveVerts = haveVerts;
				m_vtxCount = vtxCount;
				if(emittedAny)
				{
					m_draw->UpdateDrawBounds(minX, minY);
					m_draw->UpdateDrawBounds(maxX, maxY);
				}
			};

			if(hasTex && !useUV && !flatShading)
			{
				if(hasFog)
					runStripLoop(std::true_type{}, std::false_type{}, std::true_type{}, std::false_type{});
				else
					runStripLoop(std::true_type{}, std::false_type{}, std::false_type{}, std::false_type{});
			}
			else if(!hasTex && !flatShading)
			{
				if(hasFog)
					runStripLoop(std::false_type{}, std::false_type{}, std::true_type{}, std::false_type{});
				else
					runStripLoop(std::false_type{}, std::false_type{}, std::false_type{}, std::false_type{});
			}
			else
			{
				if(hasTex)
				{
					if(useUV)
					{
						if(hasFog)
							flatShading ? runStripLoop(std::true_type{}, std::true_type{}, std::true_type{}, std::true_type{})
							            : runStripLoop(std::true_type{}, std::true_type{}, std::true_type{}, std::false_type{});
						else
							flatShading ? runStripLoop(std::true_type{}, std::true_type{}, std::false_type{}, std::true_type{})
							            : runStripLoop(std::true_type{}, std::true_type{}, std::false_type{}, std::false_type{});
					}
					else
					{
						if(hasFog)
							runStripLoop(std::true_type{}, std::false_type{}, std::true_type{}, std::true_type{});
						else
							runStripLoop(std::true_type{}, std::false_type{}, std::false_type{}, std::true_type{});
					}
				}
				else
				{
					if(hasFog)
						runStripLoop(std::false_type{}, std::false_type{}, std::true_type{}, std::true_type{});
					else
						runStripLoop(std::false_type{}, std::false_type{}, std::false_type{}, std::true_type{});
				}
			}
			if(write == writeEnd) break;
		}

		uint8 reg = write->first;
		uint64 data = write->second;
		write++;
		if(m_xferBuffer.empty())
		{
			switch(reg)
			{
			case GS_REG_ST:
			case GS_REG_RGBAQ:
			case GS_REG_UV:
			case GS_REG_FOG:
				m_nReg[reg] = data;
				continue;
			case GS_REG_XYZ2:
			case GS_REG_XYZ3:
			case GS_REG_XYZF2:
			case GS_REG_XYZF3:
				m_nReg[reg] = data;
				VertexKick(reg, data);
				continue;
			default:
				break;
			}
		}
		WriteRegisterImpl(reg, data);
	}
#ifdef _DEBUG
	assert(m_transferCount != 0);
	m_transferCount--;
#endif
}

void CGSH_Vulkan::WriteRegisterImpl(uint8 registerId, uint64 data)
{
	//Some games such as Silent Hill 2 don't finish their transfers
	//completely: make sure we push the data to the GS's RAM nevertheless.
	if(!m_xferBuffer.empty() && (registerId != GS_REG_HWREG))
	{
		ProcessHostToLocalTransfer();
	}

	CGSHandler::WriteRegisterImpl(registerId, data);

	switch(registerId)
	{
	case GS_REG_PRIM:
		//Check if previous primitive set was a line
		if(m_primitiveType == PRIM_LINE)
		{
			//Virtua Fighter 2, Sega Rally 95 (and others):
			//Draws lines and uses result as CLUT for next draw calls.

			//Optimization: We check if the last drawn line UV matches what might currently be in the CLUT
			//If it's different, we force reset all saved CLUTs, otherwise, we assume we can keep
			//what we had previously. This helps reducing the number of compute dispatch needed to update the CLUT

			//Optimization lead: The game draws lines to update the CLUT area very often, but a lot of times
			//the result doesn't seem to be used. We could probably save a lot by not drawing these lines.

			if(
			    (m_clutLineU != m_lastLineU) ||
			    (m_clutLineV != m_lastLineV))
			{
				ClearClutCache();
			}
			m_clutLineU = m_lastLineU;
			m_clutLineV = m_lastLineV;
		}
		m_pendingPrim = true;
		m_pendingPrimValue = data;
		break;

	case GS_REG_ST:
	case GS_REG_RGBAQ:
	case GS_REG_UV:
	case GS_REG_FOG:
		break;

	case GS_REG_XYZ2:
	case GS_REG_XYZ3:
	case GS_REG_XYZF2:
	case GS_REG_XYZF3:
		VertexKick(registerId, data);
		break;

	default:
		m_renderContextDirty = true;
		break;
	}
}

void CGSH_Vulkan::ProcessHostToLocalTransfer()
{
	const uint8* xferData = m_xferDirectData ? m_xferDirectData : m_xferBuffer.data();
	const uint32 xferSize = m_xferDirectData ? m_xferDirectSize : static_cast<uint32>(m_xferBuffer.size());

	if(m_statsEnabled)
	{
		m_statsHostXfers++;
		m_statsHostXferBytes += xferSize;
	}

	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);

	uint32 xferDstAddr = 0;
	uint32 xferDstSize = RAMSIZE;
	if((trxReg.nRRW != 0) && (trxReg.nRRH != 0))
	{
		auto [dstAddress, dstSize] = GsTransfer::GetDstRange(bltBuf, trxReg, trxPos);
		xferDstAddr = dstAddress;
		xferDstSize = dstSize;
		InvalidateClutCacheRange(dstAddress, dstSize);
		if(!m_draw->CanHostTransferDuringPass(dstAddress, dstSize))
		{
			m_draw->FlushRenderPass();
		}
		m_draw->SyncMemoryForWrite(dstAddress, dstSize);
	}
	else
	{
		ClearClutCache();
		m_draw->FlushAllResidentMemory();
	}

	m_transferHost->Params.bufAddress = bltBuf.GetDstPtr();
	m_transferHost->Params.bufWidth = bltBuf.GetDstWidth();
	m_transferHost->Params.rrw = trxReg.nRRW;
	m_transferHost->Params.dsax = trxPos.nDSAX;
	m_transferHost->Params.dsay = trxPos.nDSAY;

	auto pipelineCaps = make_convertible<CTransferHost::PIPELINE_CAPS>(0);
	pipelineCaps.dstFormat = bltBuf.nDstPsm;

	m_transferHost->SetPipelineCaps(pipelineCaps);
	m_transferHost->DoTransfer(xferData, xferSize, xferDstAddr, xferDstSize);

	m_xferDirectData = nullptr;
	m_xferDirectSize = 0;
	m_xferBuffer.clear();
}

void CGSH_Vulkan::ProcessLocalToHostTransfer()
{
	bool readsEnabled = CAppConfig::GetInstance().GetPreferenceBoolean(PREF_CGSHANDLER_GS_RAM_READS_ENABLED);
	if(readsEnabled)
	{
		if(m_statsEnabled) m_statsReadbacks++;
		m_draw->FlushRenderPass();
		m_context->ResetHostXferBatch();

		auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
		auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
		auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);

		m_xferHistory.insert(std::make_pair(bltBuf, LOCAL_TO_HOST_XFER_HISTORY{}));
		auto transfer = m_xferHistory.find(bltBuf);
		transfer->second.MarkUsed();

		auto [copyBase, copySize] = GsTransfer::GetSrcRange(bltBuf, trxReg, trxPos);

		//Some games do that, example: Star Ocean 3
		assert((copyBase + copySize) <= RAMSIZE);
		if(copyBase >= RAMSIZE)
		{
			//Huh, something really wierd happened
			return;
		}
		if((copyBase + copySize) > RAMSIZE)
		{
			copySize = RAMSIZE - copyBase;
		}
		m_draw->SyncMemoryForRead(copyBase, copySize);

		auto& srcBuffer = m_context->memoryBuffer;
		auto& dstBuffer = m_context->memoryBufferTransfer;

		auto commandBuffer = m_frameCommandBuffer->GetCommandBuffer();

		{
			auto memoryBarrier = Framework::Vulkan::MemoryBarrier();
			memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			memoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

			m_context->device.vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
			                                       0, 1, &memoryBarrier, 0, nullptr, 0, nullptr);
		}

		{
			VkBufferCopy bufferCopy = {};
			bufferCopy.size = copySize;
			bufferCopy.dstOffset = copyBase;
			bufferCopy.srcOffset = copyBase;
			m_context->device.vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &bufferCopy);
		}

		if(!transfer->second.IsRecurring())
		{
			m_frameCommandBuffer->Flush();
			m_context->device.vkQueueWaitIdle(m_context->queue);
		}

		void* bufferPtr = nullptr;
		auto result = m_context->device.vkMapMemory(m_context->device, dstBuffer.GetMemory(), copyBase, copySize, 0, &bufferPtr);
		CHECKVULKANERROR(result);

		memcpy(m_memoryCache + copyBase, bufferPtr, copySize);

		m_context->device.vkUnmapMemory(m_context->device, dstBuffer.GetMemory());
	}
}

void CGSH_Vulkan::ProcessLocalToLocalTransfer()
{
	if(m_statsEnabled) m_statsLocalXfers++;
	//Flush previous cached info
	ClearClutCache();
	m_draw->FlushAllResidentMemory();

	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);

	assert(trxPos.nDIR == 0);

	m_transferLocal->Params.srcBufAddress = bltBuf.GetSrcPtr();
	m_transferLocal->Params.srcBufWidth = bltBuf.GetSrcWidth();
	m_transferLocal->Params.dstBufAddress = bltBuf.GetDstPtr();
	m_transferLocal->Params.dstBufWidth = bltBuf.GetDstWidth();
	m_transferLocal->Params.ssax = trxPos.nSSAX;
	m_transferLocal->Params.ssay = trxPos.nSSAY;
	m_transferLocal->Params.dsax = trxPos.nDSAX;
	m_transferLocal->Params.dsay = trxPos.nDSAY;
	m_transferLocal->Params.rrw = trxReg.nRRW;
	m_transferLocal->Params.rrh = trxReg.nRRH;

	auto pipelineCaps = make_convertible<CTransferLocal::PIPELINE_CAPS>(0);
	pipelineCaps.srcFormat = bltBuf.nSrcPsm;
	pipelineCaps.dstFormat = bltBuf.nDstPsm;

	if(bltBuf.GetSrcPtr() == bltBuf.GetDstPtr())
	{
		//If src and dst pointers are the same, we need to copy the memory area in our memoryBufferCopy.
		//This is needed for Tairyou Jigoku for a scrolling effect in the sewer level.
		//TODO: Handle overlapping regions (even if memory addresses don't match)

		auto [transferAddress, transferSize] = GsTransfer::GetSrcRange(bltBuf, trxReg, trxPos);

		//Since GetSrcRange returns values in page granularity, it's possible that we obtain an
		//[address, size] pair that goes beyond RAM's range. Happens in FF12's intro level.
		if((transferAddress + transferSize) > RAMSIZE)
		{
			transferSize = RAMSIZE - transferAddress;
		}

		auto commandBuffer = m_frameCommandBuffer->GetCommandBuffer();

		VkBufferCopy bufferCopy = {};
		bufferCopy.srcOffset = transferAddress;
		bufferCopy.dstOffset = transferAddress;
		bufferCopy.size = transferSize;

		m_context->device.vkCmdCopyBuffer(commandBuffer, m_context->memoryBuffer, m_context->memoryBufferCopy, 1, &bufferCopy);

		pipelineCaps.srcUseMemoryCopy = true;
	}

	m_transferLocal->SetPipelineCaps(pipelineCaps);
	m_transferLocal->DoTransfer();
}

void CGSH_Vulkan::ProcessClutTransfer(uint32 csa, uint32)
{
}

void CGSH_Vulkan::BeginTransferWrite()
{
	assert(m_xferBuffer.empty());
	m_xferBuffer.clear();
	m_xferDirectData = nullptr;
	m_xferDirectSize = 0;
}

void CGSH_Vulkan::TransferWrite(const uint8* imageData, uint32 length)
{
	if(!m_xferDirectData && m_xferBuffer.empty() && (m_trxCtx.nSize == length))
	{
		m_xferDirectData = imageData;
		m_xferDirectSize = length;
		return;
	}
	if(m_xferDirectData)
	{
		m_xferBuffer.insert(m_xferBuffer.end(), m_xferDirectData, m_xferDirectData + m_xferDirectSize);
		m_xferDirectData = nullptr;
		m_xferDirectSize = 0;
	}
	m_xferBuffer.insert(m_xferBuffer.end(), imageData, imageData + length);
}

void CGSH_Vulkan::WriteBackMemoryCache()
{
	m_draw->InvalidateAllResidentMemory();
	m_frameCommandBuffer->Flush();
	m_context->device.vkQueueWaitIdle(m_context->queue);

	m_context->memoryBuffer.Write(m_context->queue, m_context->commandBufferPool,
	                              m_context->physicalDeviceMemoryProperties, m_memoryCache);
}

void CGSH_Vulkan::SyncMemoryCache()
{
	m_draw->FlushAllResidentMemory();
	m_frameCommandBuffer->Flush();
	m_context->device.vkQueueWaitIdle(m_context->queue);

	m_context->memoryBuffer.Read(m_context->queue, m_context->commandBufferPool,
	                             m_context->physicalDeviceMemoryProperties, m_memoryCache);
}

void CGSH_Vulkan::SyncCLUT(const TEX0& tex0)
{
	if(!CGsPixelFormats::IsPsmIDTEX(tex0.nPsm)) return;
	if(!ProcessCLD(tex0))
	{
		//We haven't loaded anything, but it's possible that this draw call will reference data from a previous load.
		//Tales of Destiny games will do that, loading 256 CLUT entries and then not loading but referencing the entries.
		if((tex0.nCSM == 0) && CGsPixelFormats::IsPsmIDTEX4(tex0.nPsm) &&
		   (m_lastClutFillCacheIndex != -1) && (m_lastClutFillPointer == tex0.nCBP))
		{
			uint32 clutBufferOffset = sizeof(uint32) * CLUTENTRYCOUNT * m_lastClutFillCacheIndex;
			m_draw->SetClutBufferOffset(clutBufferOffset);
		}
		return;
	}

	auto texClut = make_convertible<TEXCLUT>(m_nReg[GS_REG_TEXCLUT]);

	auto clutKey = MakeCachedClutKey(tex0, texClut);
	int32 clutCacheIndex = FindCachedClut(clutKey);
	if(clutCacheIndex == -1)
	{
		clutCacheIndex = m_nextClutCacheIndex++;
		m_nextClutCacheIndex %= CLUT_CACHE_SIZE;
		m_clutStates[clutCacheIndex] = clutKey;

		uint32 clutBufferOffset = sizeof(uint32) * CLUTENTRYCOUNT * clutCacheIndex;
		//A CSM1 CLUT lies within two pages from its pointer; a CSM2 one may be anywhere.
		uint32 clutAddress = tex0.GetCLUTPtr() & ~(CGsPixelFormats::PAGESIZE - 1);
		uint32 clutSize = (tex0.nCSM == 0) ? (CGsPixelFormats::PAGESIZE * 2) : RAMSIZE;
		if(!m_draw->CanLoadClutDuringPass(clutBufferOffset, clutAddress, clutSize))
		{
			if(m_statsEnabled) m_statsClutFlushes++;
			m_draw->FlushRenderPass();
		}
		m_draw->SyncMemoryForRead(clutAddress, clutSize);
		if(m_statsEnabled) m_statsClutLoads++;
		m_clutLoad->DoClutLoad(clutBufferOffset, tex0, texClut);
	}

	uint32 clutBufferOffset = sizeof(uint32) * CLUTENTRYCOUNT * clutCacheIndex;
	m_draw->SetClutBufferOffset(clutBufferOffset);

	if(tex0.nCSM == 0)
	{
		if(CGsPixelFormats::IsPsmIDTEX8(tex0.nPsm))
		{
			m_lastClutFillCacheIndex = clutCacheIndex;
			m_lastClutFillPointer = tex0.nCBP;
		}
		else if(m_lastClutFillPointer != tex0.nCBP)
		{
			//If we have a IDTEX4 texture, but it's from a different place, invalidate
			m_lastClutFillCacheIndex = -1;
		}
	}
	else
	{
		m_lastClutFillCacheIndex = -1;
	}
}

uint8* CGSH_Vulkan::GetRam() const
{
	return m_memoryCache;
}

Framework::CBitmap CGSH_Vulkan::GetScreenshot()
{
	return Framework::CBitmap();
}

bool CGSH_Vulkan::GetDepthTestingEnabled() const
{
	return m_depthTestingEnabled;
}

void CGSH_Vulkan::SetDepthTestingEnabled(bool depthTestingEnabled)
{
	m_depthTestingEnabled = depthTestingEnabled;
	m_renderContextDirty = true;
}

bool CGSH_Vulkan::GetAlphaBlendingEnabled() const
{
	return m_alphaBlendingEnabled;
}

void CGSH_Vulkan::SetAlphaBlendingEnabled(bool alphaBlendingEnabled)
{
	m_alphaBlendingEnabled = alphaBlendingEnabled;
	m_renderContextDirty = true;
}

bool CGSH_Vulkan::GetAlphaTestingEnabled() const
{
	return m_alphaTestingEnabled;
}

void CGSH_Vulkan::SetAlphaTestingEnabled(bool alphaTestingEnabled)
{
	m_alphaTestingEnabled = alphaTestingEnabled;
	m_renderContextDirty = true;
}

Framework::CBitmap CGSH_Vulkan::GetFramebuffer(uint64 frameReg)
{
	Framework::CBitmap result;
	SendGSCall([&]() { result = GetFramebufferImpl(frameReg); }, true);
	return result;
}

Framework::CBitmap CGSH_Vulkan::GetDepthbuffer(uint64 frameReg, uint64 zbufReg)
{
	Framework::CBitmap result;
	SendGSCall([&]() { result = GetDepthbufferImpl(frameReg, zbufReg); }, true);
	return result;
}

Framework::CBitmap CGSH_Vulkan::GetTexture(uint64 tex0Reg, uint32 maxMip, uint64 miptbp1Reg, uint64 miptbp2Reg, uint32 mipLevel)
{
	Framework::CBitmap result;
	SendGSCall([&]() { result = GetTextureImpl(tex0Reg, maxMip, miptbp1Reg, miptbp2Reg, mipLevel); }, true);
	return result;
}

int CGSH_Vulkan::GetFramebufferScale()
{
	return 1;
}

const CGSHandler::VERTEX* CGSH_Vulkan::GetInputVertices() const
{
	return m_vtxBuffer;
}

Framework::CBitmap CGSH_Vulkan::GetFramebufferImpl(uint64 frameReg)
{
	auto frame = make_convertible<FRAME>(frameReg);

	SyncMemoryCache();

	uint32 frameWidth = frame.GetWidth();
	uint32 frameHeight = 1024;
	Framework::CBitmap bitmap;

	switch(frame.nPsm)
	{
	case PSMCT32:
	{
		bitmap = ReadImage32<CGsPixelFormats::CPixelIndexorPSMCT32>(GetRam(), frame.GetBasePtr(),
		                                                            frame.nWidth, frameWidth, frameHeight);
	}
	break;
	case PSMCT24:
	{
		bitmap = ReadImage32<CGsPixelFormats::CPixelIndexorPSMCT32, 0x00FFFFFF>(GetRam(), frame.GetBasePtr(),
		                                                                        frame.nWidth, frameWidth, frameHeight);
	}
	break;
	case PSMCT16:
	{
		bitmap = ReadImage16<CGsPixelFormats::CPixelIndexorPSMCT16>(GetRam(), frame.GetBasePtr(),
		                                                            frame.nWidth, frameWidth, frameHeight);
	}
	break;
	case PSMCT16S:
	{
		bitmap = ReadImage16<CGsPixelFormats::CPixelIndexorPSMCT16S>(GetRam(), frame.GetBasePtr(),
		                                                             frame.nWidth, frameWidth, frameHeight);
	}
	break;
	default:
		assert(false);
		break;
	}
	return bitmap;
}

Framework::CBitmap CGSH_Vulkan::GetDepthbufferImpl(uint64 frameReg, uint64 zbufReg)
{
	auto frame = make_convertible<FRAME>(frameReg);
	auto zbuf = make_convertible<ZBUF>(zbufReg);

	SyncMemoryCache();

	uint32 frameWidth = frame.GetWidth();
	uint32 frameHeight = 1024;
	Framework::CBitmap bitmap;

	switch(zbuf.nPsm | 0x30)
	{
	case PSMZ32:
	{
		bitmap = ReadImage32<CGsPixelFormats::CPixelIndexorPSMZ32>(GetRam(), zbuf.GetBasePtr(),
		                                                           frame.nWidth, frameWidth, frameHeight);
	}
	break;
	case PSMZ24:
	{
		bitmap = ReadImage32<CGsPixelFormats::CPixelIndexorPSMZ32, 0x00FFFFFF>(GetRam(), zbuf.GetBasePtr(),
		                                                                       frame.nWidth, frameWidth, frameHeight);
	}
	break;
	case PSMZ16S:
	{
		bitmap = ReadImage16<CGsPixelFormats::CPixelIndexorPSMZ16S>(GetRam(), zbuf.GetBasePtr(),
		                                                            frame.nWidth, frameWidth, frameHeight);
	}
	break;
	default:
		assert(false);
		break;
	}

	return bitmap;
}

Framework::CBitmap CGSH_Vulkan::GetTextureImpl(uint64 tex0Reg, uint32 maxMip, uint64 miptbp1Reg, uint64 miptbp2Reg, uint32 mipLevel)
{
	auto tex0 = make_convertible<TEX0>(tex0Reg);
	auto miptbp1 = make_convertible<MIPTBP1>(miptbp1Reg);
	auto miptbp2 = make_convertible<MIPTBP2>(miptbp2Reg);

	auto width = std::max<uint32>(tex0.GetWidth() >> mipLevel, 1);
	auto height = std::max<uint32>(tex0.GetHeight() >> mipLevel, 1);
	auto [tbp, tbw] =
	    [&]() {
		    switch(mipLevel)
		    {
		    default:
			    assert(false);
			    [[fallthrough]];
		    case 0:
			    return std::make_pair(tex0.GetBufPtr(), tex0.GetBufWidth());
		    case 1:
		    case 2:
		    case 3:
		    case 4:
		    case 5:
		    case 6:
			    return GetMipLevelInfo(mipLevel, miptbp1, miptbp2);
		    }
	    }();
	assert((tbw & 0x3F) == 0);
	tbw /= 64;

	SyncMemoryCache();

	Framework::CBitmap bitmap;

	switch(tex0.nPsm)
	{
	case PSMCT32:
		bitmap = ReadImage32<CGsPixelFormats::CPixelIndexorPSMCT32>(GetRam(), tbp, tbw, width, height);
		break;
	case PSMCT24:
		bitmap = ReadImage32<CGsPixelFormats::CPixelIndexorPSMCT32, 0x00FFFFFF>(GetRam(), tbp, tbw, width, height);
		break;
	case PSMCT16:
		bitmap = ReadImage16<CGsPixelFormats::CPixelIndexorPSMCT16>(GetRam(), tbp, tbw, width, height);
		break;
	case PSMCT16S:
		bitmap = ReadImage16<CGsPixelFormats::CPixelIndexorPSMCT16S>(GetRam(), tbp, tbw, width, height);
		break;
	case PSMT8:
		bitmap = ReadImage8<CGsPixelFormats::CPixelIndexorPSMT8>(GetRam(), tbp, tbw, width, height);
		break;
	case PSMT4:
		bitmap = ReadImage8<CGsPixelFormats::CPixelIndexorPSMT4>(GetRam(), tbp, tbw, width, height);
		break;
	}

	return bitmap;
}
