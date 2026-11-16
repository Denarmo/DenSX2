#include "GSH_Vulkan_Libretro.h"
#include "gs/GSH_Vulkan/GSH_VulkanPlatformDefs.h"
#include "vulkan/StructDefs.h"
#include "Log.h"
#include <algorithm>
#include <cstring>
#include <vector>

#define LOG_NAME "LIBRETRO"

extern retro_video_refresh_t g_video_cb;

namespace
{
	const retro_hw_render_interface_vulkan* g_frontend = nullptr;

	//The negotiation runs before the GS exists: the device made then is adopted by the GS later.
	retro_vulkan_create_device_wrapper_t g_createDeviceWrapper = nullptr;
	void* g_createDeviceOpaque = nullptr;
	VkDevice g_device = VK_NULL_HANDLE;

	PFN_vkQueueSubmit g_queueSubmit = nullptr;
	PFN_vkQueueWaitIdle g_queueWaitIdle = nullptr;

	VKAPI_ATTR VkResult VKAPI_CALL CreateDeviceThroughFrontend(VkPhysicalDevice gpu, const VkDeviceCreateInfo* createInfo,
	                                                           const VkAllocationCallbacks*, VkDevice* device)
	{
		*device = g_createDeviceWrapper(gpu, g_createDeviceOpaque, createInfo);
		return (*device != VK_NULL_HANDLE) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
	}

	VKAPI_ATTR VkResult VKAPI_CALL AdoptFrontendDevice(VkPhysicalDevice, const VkDeviceCreateInfo*,
	                                                   const VkAllocationCallbacks*, VkDevice* device)
	{
		*device = g_device;
		return (g_device != VK_NULL_HANDLE) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
	}

	//The frontend owns the device.
	VKAPI_ATTR void VKAPI_CALL KeepDevice(VkDevice, const VkAllocationCallbacks*)
	{
	}

	//The queue is the frontend's too: every use holds its lock.
	VKAPI_ATTR VkResult VKAPI_CALL LockedQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
	{
		g_frontend->lock_queue(g_frontend->handle);
		auto result = g_queueSubmit(queue, count, submits, fence);
		g_frontend->unlock_queue(g_frontend->handle);
		return result;
	}

	VKAPI_ATTR VkResult VKAPI_CALL LockedQueueWaitIdle(VkQueue queue)
	{
		g_frontend->lock_queue(g_frontend->handle);
		auto result = g_queueWaitIdle(queue);
		g_frontend->unlock_queue(g_frontend->handle);
		return result;
	}

	const VkApplicationInfo* GetApplicationInfo()
	{
		static VkApplicationInfo appInfo = {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "DenSX2";
		appInfo.pEngineName = "DenSX2";
#if GSH_VULKAN_IS_DESKTOP
		appInfo.apiVersion = VK_API_VERSION_1_2;
#else
		appInfo.apiVersion = VK_API_VERSION_1_1;
#endif
		return &appInfo;
	}

	bool HasDeviceExtension(const Framework::Vulkan::CInstance& instance, VkPhysicalDevice gpu, const char* name)
	{
		uint32_t count = 0;
		instance.vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> extensions(count);
		instance.vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, extensions.data());
		return std::any_of(extensions.begin(), extensions.end(),
		                   [&](const VkExtensionProperties& e) { return !strcmp(e.extensionName, name); });
	}

	bool CreateDevice2(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu, VkSurfaceKHR,
	                   PFN_vkGetInstanceProcAddr, retro_vulkan_create_device_wrapper_t wrapper, void* opaque)
	{
		Framework::Vulkan::CInstance wrapped(instance);
		if(gpu == VK_NULL_HANDLE)
		{
			uint32_t count = 0;
			wrapped.vkEnumeratePhysicalDevices(instance, &count, nullptr);
			std::vector<VkPhysicalDevice> gpus(count);
			wrapped.vkEnumeratePhysicalDevices(instance, &count, gpus.data());
			if(gpus.empty()) return false;
			gpu = gpus[0];
		}
#if GSH_VULKAN_IS_DESKTOP
		if(!HasDeviceExtension(wrapped, gpu, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME))
		{
			CLog::GetInstance().Warn(LOG_NAME, "This GPU has no fragment shader interlock: no Vulkan GS.\n");
			return false;
		}
#endif
		g_createDeviceWrapper = wrapper;
		g_createDeviceOpaque = opaque;
		wrapped.vkCreateDevice = CreateDeviceThroughFrontend;
		wrapped.vkDestroyDevice = KeepDevice;
		VkDevice device = VK_NULL_HANDLE;
		{
			auto created = CGSH_Vulkan::CreateDeviceObject(wrapped, gpu, false);
			device = created;
		}
		if(device == VK_NULL_HANDLE) return false;
		g_device = device;

		auto getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(wrapped.vkGetDeviceProcAddr(device, "vkGetDeviceQueue"));
		VkQueue queue = VK_NULL_HANDLE;
		getDeviceQueue(device, 0, 0, &queue);
		context->gpu = gpu;
		context->device = device;
		context->queue = queue;
		context->queue_family_index = 0;
		context->presentation_queue = queue;
		context->presentation_queue_family_index = 0;
		return true;
	}

	void DestroyDevice()
	{
		g_device = VK_NULL_HANDLE;
	}
}

CGSHandler::FactoryFunction CGSH_Vulkan_Libretro::GetFactoryFunction()
{
	return []() { return new CGSH_Vulkan_Libretro(); };
}

bool CGSH_Vulkan_Libretro::IsSupported()
{
	//The same test as the standalone frontend's device list: the desktop GS needs fragment shader
	//interlock, the mobile one runs on any device.
	try
	{
		auto instance = CGSH_Vulkan::CreateInstance(false);

		uint32_t deviceCount = 0;
		if(instance.vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr) != VK_SUCCESS) return false;
		std::vector<VkPhysicalDevice> devices(deviceCount);
		if(instance.vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()) != VK_SUCCESS) return false;

		for(const auto& device : devices)
		{
#if GSH_VULKAN_IS_DESKTOP
			uint32_t extensionCount = 0;
			instance.vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
			std::vector<VkExtensionProperties> extensions(extensionCount);
			instance.vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
			bool hasInterlock = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
				return strcmp(extension.extensionName, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME) == 0;
			});
			if(hasInterlock) return true;
#else
			(void)device;
			return true;
#endif
		}
	}
	catch(const std::exception& exception)
	{
		CLog::GetInstance().Warn(LOG_NAME, "Vulkan is not usable: %s\n", exception.what());
	}
	return false;
}

const retro_hw_render_context_negotiation_interface_vulkan* CGSH_Vulkan_Libretro::GetNegotiationInterface()
{
	static retro_hw_render_context_negotiation_interface_vulkan negotiation = {};
	negotiation.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
	negotiation.interface_version = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
	negotiation.get_application_info = GetApplicationInfo;
	negotiation.destroy_device = DestroyDevice;
	negotiation.create_device2 = CreateDevice2;
	return &negotiation;
}

void CGSH_Vulkan_Libretro::SetFrontendInterface(const retro_hw_render_interface_vulkan* frontend)
{
	g_frontend = frontend;
}

void CGSH_Vulkan_Libretro::InitializeImpl()
{
	assert(g_frontend);
	m_instance = Framework::Vulkan::CInstance(g_frontend->instance);
	m_instance.vkCreateDevice = AdoptFrontendDevice;
	m_instance.vkDestroyDevice = KeepDevice;
	g_device = g_frontend->device;
	m_physicalDeviceOverride = g_frontend->gpu;
	m_context->surfaceFormat = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};

	CGSH_Vulkan::InitializeImpl();
	SetPresentationWindow();
}

void CGSH_Vulkan_Libretro::ReleaseImpl()
{
	CGSH_Vulkan::ReleaseImpl();
	std::lock_guard<std::mutex> lock(m_frameMutex);
	for(auto& frame : m_frames)
	{
		frame = FRAME();
	}
	m_readyFrame = m_readingFrame = m_takenFrame = -1;
}

void CGSH_Vulkan_Libretro::OnDeviceCreated()
{
	g_queueSubmit = m_context->device.vkQueueSubmit;
	g_queueWaitIdle = m_context->device.vkQueueWaitIdle;
	m_context->device.vkQueueSubmit = LockedQueueSubmit;
	m_context->device.vkQueueWaitIdle = LockedQueueWaitIdle;
}

void CGSH_Vulkan_Libretro::UpdatePresentation()
{
	SendGSCall([this]() { SetPresentationWindow(); });
}

void CGSH_Vulkan_Libretro::SetPresentationWindow()
{
	//The Vulkan GS draws at the PS2's own resolution; the frontend scales the picture.
	PRESENTATION_PARAMS presentationParams;
	presentationParams.mode = PRESENTATION_MODE_FILL;
	presentationParams.windowWidth = GetCrtWidth();
	presentationParams.windowHeight = GetCrtHeight();
	m_presentationWidth = presentationParams.windowWidth;
	m_presentationHeight = presentationParams.windowHeight;
	SetPresentationParams(presentationParams);
}

void CGSH_Vulkan_Libretro::FlipImpl(const DISPLAY_INFO& dispInfo)
{
	if((GetCrtWidth() != m_presentationWidth) || (GetCrtHeight() != m_presentationHeight))
	{
		SetPresentationWindow();
	}

	//At most one finished frame waits for retro_run; the image retro_run reads or last took stays.
	int target = -1;
	{
		std::unique_lock<std::mutex> lock(m_frameMutex);
		m_frameCondition.wait(lock, [this]() { return m_freeRun || (m_readyFrame < 0); });
		for(int i = 0; i < IMAGE_COUNT; i++)
		{
			if((i != m_readyFrame) && (i != m_readingFrame) && (i != m_takenFrame))
			{
				target = i;
				break;
			}
		}
		if(target < 0)
		{
			target = m_readyFrame; //free run: replace the frame nobody took
			m_readyFrame = -1;
		}
	}

	m_present->SetPresentationViewport(GetPresentationViewport());
	m_present->DoPresentOffscreen(dispInfo, target, m_presentationWidth, m_presentationHeight);

	{
		std::lock_guard<std::mutex> lock(m_frameMutex);
		auto& frame = m_frames[target];
		frame.image = m_present->GetOffscreenImage(target);
		frame.view = m_present->GetOffscreenImageView(target);
		frame.width = m_presentationWidth;
		frame.height = m_presentationHeight;
		m_readyFrame = target;
	}
	m_frameCondition.notify_all();

	CGSH_Vulkan::FlipImpl(dispInfo);
}

void CGSH_Vulkan_Libretro::PresentBackbuffer()
{
}

void CGSH_Vulkan_Libretro::SetFreeRun(bool freeRun)
{
	{
		std::lock_guard<std::mutex> lock(m_frameMutex);
		m_freeRun = freeRun;
	}
	m_frameCondition.notify_all();
}

void CGSH_Vulkan_Libretro::PresentFrame()
{
	FRAME frame;
	int index = -1;
	{
		std::unique_lock<std::mutex> lock(m_frameMutex);
		//A frame late is shown again rather than holding up the frontend's own frame.
		if(!m_frameCondition.wait_for(lock, std::chrono::milliseconds(50), [this]() { return m_readyFrame >= 0; }))
		{
			lock.unlock();
			if(g_video_cb) g_video_cb(nullptr, m_lastWidth, m_lastHeight, 0);
			return;
		}
		index = m_readyFrame;
		m_readyFrame = -1;
		m_readingFrame = index;
		frame = m_frames[index];
	}
	m_frameCondition.notify_all();

	m_retroImage = {};
	m_retroImage.image_view = frame.view;
	m_retroImage.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	m_retroImage.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	m_retroImage.create_info.image = frame.image;
	m_retroImage.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	m_retroImage.create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	m_retroImage.create_info.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
	m_retroImage.create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	g_frontend->set_image(g_frontend->handle, &m_retroImage, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
	m_lastWidth = frame.width;
	m_lastHeight = frame.height;
	if(g_video_cb) g_video_cb(RETRO_HW_FRAME_BUFFER_VALID, frame.width, frame.height, 0);

	{
		std::lock_guard<std::mutex> lock(m_frameMutex);
		m_readingFrame = -1;
		m_takenFrame = index;
	}
	m_frameCondition.notify_all();
}
