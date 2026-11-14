#include "SH_LibreAudio.h"
#include "ext/libretro.h"
#include <cstring>
#include <mutex>

extern retro_audio_sample_batch_t g_set_audio_sample_batch_cb;

CSoundHandler* CSH_LibreAudio::HandlerFactory()
{
	return new CSH_LibreAudio();
}

void CSH_LibreAudio::Write(int16* buffer, unsigned int sampleCount, unsigned int sampleRate)
{
	//Blocks written before the frontend takes them are kept, not replaced.
	std::lock_guard<std::mutex> lock(m_buffer_lock);
	m_buffer.insert(m_buffer.end(), buffer, buffer + sampleCount);
}

void CSH_LibreAudio::ProcessBuffer()
{
	std::lock_guard<std::mutex> lock(m_buffer_lock);
	if(m_buffer.empty()) return;
	if(g_set_audio_sample_batch_cb)
		g_set_audio_sample_batch_cb(m_buffer.data(), m_buffer.size() / 2);
	m_buffer.clear();
}

bool CSH_LibreAudio::HasFreeBuffers()
{
	return false;
}

void CSH_LibreAudio::Reset()
{
}

void CSH_LibreAudio::RecycleBuffers()
{
}
