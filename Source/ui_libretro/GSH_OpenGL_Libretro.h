#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "gs/GSH_OpenGL/GSH_OpenGL.h"

class CGSH_OpenGL_Libretro : public CGSH_OpenGL
{
public:
	CGSH_OpenGL_Libretro();
	virtual ~CGSH_OpenGL_Libretro();

	static FactoryFunction GetFactoryFunction();

	void InitializeImpl() override;
	void FlipImpl(const DISPLAY_INFO&) override;
	void Reset();
	void Release();
	void PresentBackbuffer() override;
	void UpdatePresentation();
	void FlushMailBox();

	//Runs GS calls on this thread until done() is true. Frames flipped meanwhile are held for
	//the next PresentHeldFrame, since the frontend takes frames only in retro_run.
	void ProcessCallsUntil(const std::function<bool()>& done);
	bool PresentHeldFrame();

	//Android: GS calls run on a thread of their own, in an EGL context shared with the frontend's,
	//so the GS keeps working while the frontend draws its own frame and waits for the display.
	//Called with the frontend's context current; false when no such context can be made.
	bool StartWorker();
	bool HasWorker() const
	{
		return m_useWorker;
	}
	//The worker stops waiting for retro_run to take its frames (newer frames replace older ones).
	void SetFreeRun(bool);
	//retro_run: blits the worker's next frame to the frontend's framebuffer.
	void PresentWorkerFrame();

private:
	struct PRESENT_BUFFER
	{
		GLuint texture = 0;
		GLuint framebuffer = 0;
		GLuint readFramebuffer = 0;
		uint32 width = 0;
		uint32 height = 0;
		GLsync writeFence = 0;
		GLsync readFence = 0;
	};

	void UpdatePresentationImpl();
	void SetPresentationWindow();
	void WorkerProc();
	void StopWorker();

	bool m_holdPresent = false;
	uint32 m_presentationWidth = 0;
	uint32 m_presentationHeight = 0;
	bool m_presentHeld = false;

	bool m_useWorker = false;
	bool m_workerDone = false;
	std::thread m_worker;
	void* m_eglDisplay = nullptr;
	void* m_eglContext = nullptr;
	void* m_eglSurface = nullptr;

	PRESENT_BUFFER m_presentBuffers[2];
	int m_targetBuffer = -1;
	int m_readyBuffer = -1;
	int m_takenBuffer = -1;
	int m_readingBuffer = -1;
	bool m_freeRun = false;
	std::mutex m_presentMutex;
	std::condition_variable m_presentCondition;
};
