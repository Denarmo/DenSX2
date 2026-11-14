#pragma once

#include <functional>

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

private:
	void UpdatePresentationImpl();

	bool m_holdPresent = false;
	bool m_presentHeld = false;
};
