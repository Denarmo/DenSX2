#include "GSH_OpenGL_Libretro.h"
#include "ext/libretro.h"
#include "Log.h"
#include <algorithm>
#include <cstring>
#include <thread>

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <pthread.h>
#endif

#define LOG_NAME "LIBRETRO"

extern int g_res_factor;
extern CGSHandler::PRESENTATION_MODE g_presentation_mode;
extern retro_video_refresh_t g_video_cb;
extern struct retro_hw_render_callback g_hw_render;

CGSH_OpenGL_Libretro::CGSH_OpenGL_Libretro()
    : CGSH_OpenGL(false)
{
}

CGSH_OpenGL_Libretro::~CGSH_OpenGL_Libretro()
{
	if(m_useWorker)
	{
		StopWorker();
	}
}

CGSH_OpenGL::FactoryFunction CGSH_OpenGL_Libretro::GetFactoryFunction()
{
	return []() { return new CGSH_OpenGL_Libretro(); };
}

void CGSH_OpenGL_Libretro::InitializeImpl()
{
	fprintf(stderr, "%s\n", __FUNCTION__);

#if defined(USE_GLEW)
	glewExperimental = GL_TRUE;
	auto result = glewInit();
	CLog::GetInstance().Warn(LOG_NAME, "glewInit %d\n", result == GLEW_OK);
	assert(result == GLEW_OK);
	if(result != GLEW_OK)
	{
		fprintf(stderr, "Error: %s\n", glewGetErrorString(result));
		CLog::GetInstance().Warn(LOG_NAME, "Error: %s\n", glewGetErrorString(result));
		return;
	}

#endif

	if(!m_useWorker && g_hw_render.get_current_framebuffer)
		m_presentFramebuffer = g_hw_render.get_current_framebuffer();

	UpdatePresentationImpl();

	CGSH_OpenGL::InitializeImpl();
}

void CGSH_OpenGL_Libretro::UpdatePresentation()
{
	SendGSCall([this]() { UpdatePresentationImpl(); });
}

void CGSH_OpenGL_Libretro::UpdatePresentationImpl()
{
	SetPresentationWindow();
	NotifyPreferencesChanged();
}

void CGSH_OpenGL_Libretro::SetPresentationWindow()
{
	PRESENTATION_PARAMS presentationParams;
	presentationParams.mode = g_presentation_mode;
	presentationParams.windowWidth = GetCrtWidth() * g_res_factor;
	presentationParams.windowHeight = GetCrtHeight() * g_res_factor;
	m_presentationWidth = presentationParams.windowWidth;
	m_presentationHeight = presentationParams.windowHeight;

	SetPresentationParams(presentationParams);
}

void CGSH_OpenGL_Libretro::FlushMailBox()
{
	bool isFlushed = false;
	SendGSCall([&]() {
		isFlushed = true;
		m_flipped = true;
	},
	           true);
	while(!isFlushed)
	{
		// Wait for flush to complete
		ProcessSingleFrame();
	}
}

void CGSH_OpenGL_Libretro::Reset()
{
	if(m_useWorker)
	{
		SetFreeRun(true);
		SendGSCall([this]() {
			ResetBase();
			CGSH_OpenGL::ReleaseImpl();
			InitializeImpl();
		},
		           true, true);
		SetFreeRun(false);
		return;
	}
	FlushMailBox();
	ResetBase();
	CGSH_OpenGL::ReleaseImpl();
	InitializeImpl();
}

void CGSH_OpenGL_Libretro::Release()
{
	if(m_useWorker)
	{
		SetFreeRun(true);
		SendGSCall([this]() {
			ResetBase();
			CGSH_OpenGL::ReleaseImpl();
		},
		           true, true);
		StopWorker();
		return;
	}
	FlushMailBox();
	ResetBase();
	CGSH_OpenGL::ReleaseImpl();
}

void CGSH_OpenGL_Libretro::FlipImpl(const DISPLAY_INFO& dispInfo)
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_useWorker)
	{
		if((GetCrtWidth() * g_res_factor != m_presentationWidth) || (GetCrtHeight() * g_res_factor != m_presentationHeight))
			SetPresentationWindow();

		//Draw into the buffer retro_run has not got: at most one finished frame waits for it.
		GLsync readFence = 0;
		{
			std::unique_lock<std::mutex> lock(m_presentMutex);
			m_presentCondition.wait(lock, [this]() { return m_freeRun || (m_readyBuffer < 0); });
			m_targetBuffer = 1 - ((m_readyBuffer >= 0) ? m_readyBuffer : std::max(m_takenBuffer, 0));
			m_presentCondition.wait(lock, [this]() { return m_readingBuffer != m_targetBuffer; });
			std::swap(readFence, m_presentBuffers[m_targetBuffer].readFence);
		}
		if(readFence)
		{
			glWaitSync(readFence, 0, GL_TIMEOUT_IGNORED);
			glDeleteSync(readFence);
		}

		auto& buffer = m_presentBuffers[m_targetBuffer];
		if((buffer.width != m_presentationWidth) || (buffer.height != m_presentationHeight))
		{
			if(buffer.texture) glDeleteTextures(1, &buffer.texture);
			glGenTextures(1, &buffer.texture);
			glBindTexture(GL_TEXTURE_2D, buffer.texture);
			glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, m_presentationWidth, m_presentationHeight);
			glBindTexture(GL_TEXTURE_2D, 0);
			if(!buffer.framebuffer) glGenFramebuffers(1, &buffer.framebuffer);
			glBindFramebuffer(GL_FRAMEBUFFER, buffer.framebuffer);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer.texture, 0);
			buffer.width = m_presentationWidth;
			buffer.height = m_presentationHeight;
		}
		m_presentFramebuffer = buffer.framebuffer;
		CGSH_OpenGL::FlipImpl(dispInfo);
		m_targetBuffer = -1;
		return;
	}

	if(g_hw_render.get_current_framebuffer)
		m_presentFramebuffer = g_hw_render.get_current_framebuffer();
	else
		return;

	//Frames are reported at the current CRT size (PAL games switch to 512 lines after boot),
	//so the presentation window has to follow it.
	if((GetCrtWidth() * g_res_factor != m_presentationWidth) || (GetCrtHeight() * g_res_factor != m_presentationHeight))
		SetPresentationWindow();

	CGSH_OpenGL::FlipImpl(dispInfo);
}

void CGSH_OpenGL_Libretro::ProcessCallsUntil(const std::function<bool()>& done)
{
	if(m_useWorker)
	{
		SetFreeRun(true);
		while(!done())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		SetFreeRun(false);
		return;
	}
	m_holdPresent = true;
	while(!done())
	{
		if(!ProcessPendingCall())
		{
			std::this_thread::yield();
		}
	}
	m_holdPresent = false;
}

bool CGSH_OpenGL_Libretro::PresentHeldFrame()
{
	if(!m_flipped) return false;
	m_flipped = false;
	if(m_presentHeld)
	{
		m_presentHeld = false;
		PresentBackbuffer();
	}
	else if(g_video_cb)
	{
		g_video_cb(nullptr, GetCrtWidth() * g_res_factor, GetCrtHeight() * g_res_factor, 0);
	}
	return true;
}

void CGSH_OpenGL_Libretro::PresentBackbuffer()
{
	CLog::GetInstance().Print(LOG_NAME, "%s\n", __FUNCTION__);

	if(m_useWorker)
	{
		if(m_targetBuffer < 0) return;
		GLsync writeFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		glFlush();
		{
			std::lock_guard<std::mutex> lock(m_presentMutex);
			if(m_readyBuffer >= 0)
			{
				auto& dropped = m_presentBuffers[m_readyBuffer];
				if(dropped.writeFence) glDeleteSync(dropped.writeFence);
				dropped.writeFence = 0;
			}
			m_presentBuffers[m_targetBuffer].writeFence = writeFence;
			m_readyBuffer = m_targetBuffer;
		}
		m_presentCondition.notify_all();
		return;
	}

	if(m_holdPresent)
	{
		m_presentHeld = true;
		return;
	}
	if(g_video_cb)
		g_video_cb(RETRO_HW_FRAME_BUFFER_VALID, GetCrtWidth() * g_res_factor, GetCrtHeight() * g_res_factor, 0);
}

bool CGSH_OpenGL_Libretro::StartWorker()
{
#if defined(__ANDROID__)
	if(m_useWorker) return true;

	EGLDisplay display = eglGetCurrentDisplay();
	EGLContext sharedContext = eglGetCurrentContext();
	if((display == EGL_NO_DISPLAY) || (sharedContext == EGL_NO_CONTEXT)) return false;

	EGLint configId = 0;
	eglQueryContext(display, sharedContext, EGL_CONFIG_ID, &configId);
	EGLint configAttribs[] = {EGL_CONFIG_ID, configId, EGL_NONE};
	EGLConfig config = nullptr;
	EGLint configCount = 0;
	if(!eglChooseConfig(display, configAttribs, &config, 1, &configCount) || (configCount == 0)) return false;

	EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 2, EGL_NONE};
	EGLContext context = eglCreateContext(display, config, sharedContext, contextAttribs);
	if(context == EGL_NO_CONTEXT)
	{
		EGLint es3Attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
		context = eglCreateContext(display, config, sharedContext, es3Attribs);
	}
	if(context == EGL_NO_CONTEXT)
	{
		CLog::GetInstance().Warn(LOG_NAME, "No shared EGL context for the GS thread (0x%x).\n", eglGetError());
		return false;
	}

	EGLSurface surface = EGL_NO_SURFACE;
	const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
	if(!extensions || !strstr(extensions, "EGL_KHR_surfaceless_context"))
	{
		EGLint pbufferAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
		surface = eglCreatePbufferSurface(display, config, pbufferAttribs);
		if(surface == EGL_NO_SURFACE)
		{
			eglDestroyContext(display, context);
			return false;
		}
	}

	m_eglDisplay = display;
	m_eglContext = context;
	m_eglSurface = surface;
	m_workerDone = false;
	m_useWorker = true;
	m_worker = std::thread([this]() { WorkerProc(); });
	return true;
#else
	return false;
#endif
}

void CGSH_OpenGL_Libretro::WorkerProc()
{
#if defined(__ANDROID__)
	pthread_setname_np(pthread_self(), "GS Thread");
	auto display = static_cast<EGLDisplay>(m_eglDisplay);
	auto surface = static_cast<EGLSurface>(m_eglSurface);
	eglMakeCurrent(display, surface, surface, static_cast<EGLContext>(m_eglContext));

	while(!m_workerDone)
	{
		ProcessSingleFrame();
	}

	for(auto& buffer : m_presentBuffers)
	{
		if(buffer.writeFence) glDeleteSync(buffer.writeFence);
		if(buffer.readFence) glDeleteSync(buffer.readFence);
		if(buffer.framebuffer) glDeleteFramebuffers(1, &buffer.framebuffer);
		if(buffer.texture) glDeleteTextures(1, &buffer.texture);
		buffer.writeFence = buffer.readFence = 0;
		buffer.framebuffer = buffer.texture = 0;
		buffer.width = buffer.height = 0;
	}
	glFinish();
	eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
#endif
}

void CGSH_OpenGL_Libretro::StopWorker()
{
#if defined(__ANDROID__)
	SetFreeRun(true);
	SendGSCall([this]() {
		m_workerDone = true;
		m_flipped = true;
	});
	m_worker.join();
	for(auto& buffer : m_presentBuffers)
	{
		if(buffer.readFramebuffer && eglGetCurrentContext() != EGL_NO_CONTEXT) glDeleteFramebuffers(1, &buffer.readFramebuffer);
		buffer.readFramebuffer = 0;
	}
	auto display = static_cast<EGLDisplay>(m_eglDisplay);
	if(m_eglSurface) eglDestroySurface(display, static_cast<EGLSurface>(m_eglSurface));
	eglDestroyContext(display, static_cast<EGLContext>(m_eglContext));
	m_eglDisplay = m_eglContext = m_eglSurface = nullptr;
	m_readyBuffer = m_takenBuffer = m_readingBuffer = -1;
	m_useWorker = false;
#endif
}

void CGSH_OpenGL_Libretro::SetFreeRun(bool freeRun)
{
	{
		std::lock_guard<std::mutex> lock(m_presentMutex);
		m_freeRun = freeRun;
	}
	m_presentCondition.notify_all();
}

void CGSH_OpenGL_Libretro::PresentWorkerFrame()
{
	int index = -1;
	GLsync writeFence = 0;
	{
		std::unique_lock<std::mutex> lock(m_presentMutex);
		//A frame late is shown again rather than holding up the frontend's own frame.
		if(!m_presentCondition.wait_for(lock, std::chrono::milliseconds(50), [this]() { return m_readyBuffer >= 0; }))
		{
			lock.unlock();
			if(g_video_cb)
				g_video_cb(nullptr, m_presentationWidth, m_presentationHeight, 0);
			return;
		}
		index = m_readyBuffer;
		m_readyBuffer = -1;
		m_takenBuffer = index;
		m_readingBuffer = index;
		std::swap(writeFence, m_presentBuffers[index].writeFence);
	}
	m_presentCondition.notify_all();

	auto& buffer = m_presentBuffers[index];
	if(writeFence)
	{
		glWaitSync(writeFence, 0, GL_TIMEOUT_IGNORED);
		glDeleteSync(writeFence);
	}
	if(!buffer.readFramebuffer) glGenFramebuffers(1, &buffer.readFramebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, buffer.readFramebuffer);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer.texture, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_hw_render.get_current_framebuffer ? g_hw_render.get_current_framebuffer() : 0);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glBlitFramebuffer(0, 0, buffer.width, buffer.height, 0, 0, buffer.width, buffer.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	GLsync readFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();
	uint32 width = buffer.width;
	uint32 height = buffer.height;
	{
		std::lock_guard<std::mutex> lock(m_presentMutex);
		if(buffer.readFence) glDeleteSync(buffer.readFence);
		buffer.readFence = readFence;
		m_readingBuffer = -1;
	}
	m_presentCondition.notify_all();

	if(g_video_cb)
		g_video_cb(RETRO_HW_FRAME_BUFFER_VALID, width, height, 0);
}
