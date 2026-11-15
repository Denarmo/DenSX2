#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "gs/GSH_Vulkan/GSH_Vulkan.h"
#include "ext/libretro_vulkan.h"

//The Vulkan GS for libretro frontends that give a Vulkan device (RETRO_HW_CONTEXT_VULKAN).
//The device is made through the frontend's negotiation interface, so the frontend can add the
//extensions it needs; GS calls run on the GS's own thread, which draws each flipped frame into
//an image of its own. retro_run hands the latest such image to the frontend (set_image), and
//the GS thread waits for retro_run to take a frame before drawing the next one.
class CGSH_Vulkan_Libretro : public CGSH_Vulkan
{
public:
	CGSH_Vulkan_Libretro() = default;
	virtual ~CGSH_Vulkan_Libretro() = default;

	static FactoryFunction GetFactoryFunction();
	//Whether this machine has a GPU the Vulkan GS can run on.
	static bool IsSupported();
	static const retro_hw_render_context_negotiation_interface_vulkan* GetNegotiationInterface();
	//context_reset: the frontend's device. Must be set before the handler is made.
	static void SetFrontendInterface(const retro_hw_render_interface_vulkan*);

	void InitializeImpl() override;
	void ReleaseImpl() override;
	void FlipImpl(const DISPLAY_INFO&) override;
	void PresentBackbuffer() override;

	void UpdatePresentation();
	//The GS thread stops waiting for retro_run to take its frames (newer frames replace older ones).
	void SetFreeRun(bool);
	//retro_run: hands the latest frame to the frontend, or shows the last one again after 50 ms.
	void PresentFrame();

protected:
	void OnDeviceCreated() override;

private:
	enum
	{
		IMAGE_COUNT = 3,
	};

	struct FRAME
	{
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		uint32 width = 0;
		uint32 height = 0;
	};

	void SetPresentationWindow();

	uint32 m_presentationWidth = 0;
	uint32 m_presentationHeight = 0;

	FRAME m_frames[IMAGE_COUNT];
	retro_vulkan_image m_retroImage = {};
	int m_readyFrame = -1;
	int m_readingFrame = -1;
	int m_takenFrame = -1;
	uint32 m_lastWidth = 640;
	uint32 m_lastHeight = 448;
	bool m_freeRun = false;
	std::mutex m_frameMutex;
	std::condition_variable m_frameCondition;
};
