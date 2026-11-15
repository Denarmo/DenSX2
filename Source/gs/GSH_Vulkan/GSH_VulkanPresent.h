#pragma once

#include <memory>
#include "GSH_VulkanContext.h"
#include "GSH_VulkanPipelineCache.h"
#include "vulkan/ShaderModule.h"
#include "vulkan/Buffer.h"
#include "vulkan/Image.h"

namespace GSH_Vulkan
{
	class CPresent
	{
	public:
		CPresent(const ContextPtr&);
		virtual ~CPresent();

		void ValidateSwapChain(const CGSHandler::PRESENTATION_PARAMS&);
		void SetPresentationViewport(const CGSHandler::PRESENTATION_VIEWPORT&);
		void DoPresent(const CGSHandler::DISPLAY_INFO&);

		//Without a surface (libretro): draws into image 'index' of its own, of this size, which then
		//is in SHADER_READ_ONLY_OPTIMAL layout. The image is remade when the size changes.
		void DoPresentOffscreen(const CGSHandler::DISPLAY_INFO&, uint32 index, uint32 width, uint32 height);
		VkImage GetOffscreenImage(uint32 index) const;
		VkImageView GetOffscreenImageView(uint32 index) const;

	private:
		enum BLEND_MODE
		{
			BLEND_MODE_NONE,
			BLEND_MODE_SRC_ALPHA,
			BLEND_MODE_CST_ALPHA,
		};

		typedef uint32 PipelineCapsInt;

		struct PIPELINE_CAPS : public convertible<PipelineCapsInt>
		{
			uint32 bufPsm : 6;
			uint32 blendMode : 2;
		};

		typedef CPipelineCache<PipelineCapsInt> PipelineCache;
		typedef std::unordered_map<uint32, VkDescriptorSet> DescriptorSetCache;

		struct PRESENT_COMMANDBUFFER
		{
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			VkFence execCompleteFence = VK_NULL_HANDLE;
		};
		typedef std::vector<PRESENT_COMMANDBUFFER> PresentCommandBufferArray;

		struct PRESENT_VERTEX
		{
			float x, y;
			float u, v;
		};

		struct PRESENT_PARAMS
		{
			uint32 bufAddress;
			uint32 bufWidth;
			uint32 dispWidth;
			uint32 dispHeight;
			uint32 layerX;
			uint32 layerY;
			uint32 layerWidth;
			uint32 layerHeight;
		};

		struct SWAPCHAIN_SEMAPHORES
		{
			VkSemaphore imageAcquireSemaphore = VK_NULL_HANDLE;
			VkSemaphore renderCompleteSemaphore = VK_NULL_HANDLE;
		};

		struct OFFSCREEN_IMAGE
		{
			Framework::Vulkan::CImage image;
			VkImageView view = VK_NULL_HANDLE;
			VkFramebuffer framebuffer = VK_NULL_HANDLE;
			uint32 width = 0;
			uint32 height = 0;
		};

		void UpdateBackbuffer(VkImage, VkFramebuffer, VkExtent2D, const SWAPCHAIN_SEMAPHORES*, const CGSHandler::DISPLAY_INFO&);
		void DestroyOffscreenImage(OFFSCREEN_IMAGE&);
		PRESENT_COMMANDBUFFER PrepareCommandBuffer();
		VkDescriptorSet PrepareDescriptorSet(VkDescriptorSetLayout, uint32);

		void CreateSwapChain();
		void CreateSwapChainImageViews();
		void CreateSwapChainFramebuffers();
		void DestroySwapChain();

		void CreateRenderPass();
		void CreateVertexBuffer();

		PIPELINE CreateDrawPipeline(const PIPELINE_CAPS&);
		Framework::Vulkan::CShaderModule CreateVertexShader();
		Framework::Vulkan::CShaderModule CreateFragmentShader(const PIPELINE_CAPS&);

		static const PRESENT_VERTEX g_vertexBufferContents[4];

		ContextPtr m_context;

		VkExtent2D m_surfaceExtents;
		VkSwapchainKHR m_swapChain = VK_NULL_HANDLE;
		std::vector<VkImage> m_swapChainImages;
		std::vector<VkImageView> m_swapChainImageViews;
		std::vector<VkFramebuffer> m_swapChainFramebuffers;
		std::vector<SWAPCHAIN_SEMAPHORES> m_swapChainSemaphores;
		std::vector<OFFSCREEN_IMAGE> m_offscreenImages;
		uint32 m_currentSwapChainSemaphoreIndex = 0;
		bool m_swapChainValid = false;
		CGSHandler::PRESENTATION_VIEWPORT m_presentationViewport;
		VkRenderPass m_renderPass = VK_NULL_HANDLE;
		Framework::Vulkan::CBuffer m_vertexBuffer;
		PresentCommandBufferArray m_presentCommandBuffers;
		PipelineCache m_pipelineCache;
		DescriptorSetCache m_descriptorSetCache;
	};

	typedef std::shared_ptr<CPresent> PresentPtr;
}
