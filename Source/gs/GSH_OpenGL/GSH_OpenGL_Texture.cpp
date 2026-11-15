#include <assert.h>
#include <cstring>
#include <sys/stat.h>
#include <limits.h>
#include <algorithm>
#ifdef _WIN32
#include <intrin.h>
#endif
#include "SimdDefs.h"
#include "GSH_OpenGL.h"
#include "StdStream.h"
#include "bitmap/BMP.h"
#include "../GsPixelFormats.h"

/////////////////////////////////////////////////////////////
// Texture Loading
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::SetupTextureUpdaters()
{
	bool canUseSimd48Updaters = true;

#ifdef _WIN32
	{
		std::array<int, 4> cpuInfo;
		__cpuid(cpuInfo.data(), 1);

		static const uint32 CPUID_FLAG_SSSE3 = 0x000200;
		bool hasSsse3 = (cpuInfo[2] & CPUID_FLAG_SSSE3) != 0;

		//x86 SIMD updaters use PSHUFB that is available on SSSE3
		canUseSimd48Updaters = hasSsse3;
	}
#endif

	for(unsigned int i = 0; i < PSM_MAX; i++)
	{
		m_textureUpdater[i] = &CGSH_OpenGL::TexUpdater_Invalid;
	}

	m_textureUpdater[PSMCT32] = &CGSH_OpenGL::TexUpdater_Psm32;
	m_textureUpdater[PSMCT24] = &CGSH_OpenGL::TexUpdater_Psm32;
	m_textureUpdater[PSMCT16] = &CGSH_OpenGL::TexUpdater_Psm16<CGsPixelFormats::CPixelIndexorPSMCT16>;
	m_textureUpdater[PSMCT32_UNK] = &CGSH_OpenGL::TexUpdater_Psm32;
	m_textureUpdater[PSMCT24_UNK] = &CGSH_OpenGL::TexUpdater_Psm32;
	m_textureUpdater[PSMCT16S] = &CGSH_OpenGL::TexUpdater_Psm16<CGsPixelFormats::CPixelIndexorPSMCT16S>;

	if(canUseSimd48Updaters)
	{
		m_textureUpdater[PSMT8] = &CGSH_OpenGL::TexUpdater_Psm8;
		m_textureUpdater[PSMT4] = &CGSH_OpenGL::TexUpdater_Psm4;
	}
	else
	{
		m_textureUpdater[PSMT8] = &CGSH_OpenGL::TexUpdater_Psm48<CGsPixelFormats::CPixelIndexorPSMT8>;
		m_textureUpdater[PSMT4] = &CGSH_OpenGL::TexUpdater_Psm48<CGsPixelFormats::CPixelIndexorPSMT4>;
	}

	m_textureUpdater[PSMT8H] = &CGSH_OpenGL::TexUpdater_Psm48H<24, 0xFF>;
	m_textureUpdater[PSMT4HL] = &CGSH_OpenGL::TexUpdater_Psm48H<24, 0x0F>;
	m_textureUpdater[PSMT4HH] = &CGSH_OpenGL::TexUpdater_Psm48H<28, 0x0F>;
}

uint32 CGSH_OpenGL::GetFramebufferBitDepth(uint32 psm)
{
	if((psm == PSMCT32) || (psm == PSMCT24))
	{
		return 32;
	}
	else if((psm == PSMCT16) || (psm == PSMCT16S))
	{
		return 16;
	}
	else
	{
		assert(false);
		return 32;
	}
}

CGSH_OpenGL::TEXTUREFORMAT_INFO CGSH_OpenGL::GetTextureFormatInfo(uint32 psm)
{
	switch(psm)
	{
	case PSMCT32:
	case PSMCT24:
	case PSMCT32_UNK:
	case PSMCT24_UNK:
		return TEXTUREFORMAT_INFO{GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
	case PSMCT16:
	case PSMCT16S:
		return TEXTUREFORMAT_INFO{GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1};
	case PSMT8:
	case PSMT4:
	case PSMT8H:
	case PSMT4HL:
	case PSMT4HH:
		return TEXTUREFORMAT_INFO{GL_R8, GL_RED, GL_UNSIGNED_BYTE};
	default:
		assert(false);
		return TEXTUREFORMAT_INFO{GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
	}
}

CGSH_OpenGL::TEXTURE_INFO CGSH_OpenGL::SearchTextureFramebuffer(const TEX0& tex0)
{
	TEXTURE_INFO texInfo;
	FramebufferPtr framebuffer;

	//The scans below depend on the texture's address, width and format only: remember their result until
	//the framebuffer list changes (Gran Turismo 4 keeps 30+ framebuffers and sets up thousands of textures).
	uint64 lookupKey = static_cast<uint64>(tex0.GetBufPtr()) | (static_cast<uint64>(tex0.GetBufWidth()) << 32) | (static_cast<uint64>(tex0.nPsm) << 56);
	auto& lookup = m_textureFramebufferLookups[(lookupKey ^ (lookupKey >> 13) ^ (lookupKey >> 32)) % TEXTURE_FRAMEBUFFER_LOOKUPS];
	if((lookup.key == lookupKey) && (lookup.version == m_framebuffersVersion))
	{
		if(lookup.framebufferIndex < 0) return TEXTURE_INFO();
		framebuffer = m_framebuffers[lookup.framebufferIndex];
		texInfo.offsetX = lookup.offsetX;
		texInfo.alphaAsIndex = lookup.alphaAsIndex;
	}
	else
	{
		lookup.key = lookupKey;
		lookup.version = m_framebuffersVersion;

		//First pass, look for an exact match
		for(const auto& candidateFramebuffer : m_framebuffers)
		{
			//Case: TEX0 points at the start of a frame buffer with the same width
			if(candidateFramebuffer->m_basePtr == tex0.GetBufPtr() &&
			   candidateFramebuffer->m_width == tex0.GetBufWidth() &&
			   IsCompatibleFramebufferPSM(candidateFramebuffer->m_psm, tex0.nPsm))
			{
				framebuffer = candidateFramebuffer;
				break;
			}

			//Case: TEX0 point at the start of a frame buffer with the same width
			//but uses upper 8-bits (alpha) as an indexed texture (used in Yakuza)
			else if(candidateFramebuffer->m_basePtr == tex0.GetBufPtr() &&
			        candidateFramebuffer->m_width == tex0.GetBufWidth() &&
			        candidateFramebuffer->m_psm == CGSHandler::PSMCT32 &&
			        tex0.nPsm == CGSHandler::PSMT8H)
			{
				framebuffer = candidateFramebuffer;
				texInfo.alphaAsIndex = true;
				break;
			}
		}

		if(!framebuffer)
		{
			//Second pass, be a bit more flexible
			for(const auto& candidateFramebuffer : m_framebuffers)
			{
				//Another case: TEX0 is pointing to the start of a page within our framebuffer (BGDA does this)
				if(candidateFramebuffer->m_basePtr <= tex0.GetBufPtr() &&
				   candidateFramebuffer->m_width == tex0.GetBufWidth() &&
				   candidateFramebuffer->m_psm == tex0.nPsm)
				{
					uint32 framebufferOffset = tex0.GetBufPtr() - candidateFramebuffer->m_basePtr;

					//Bail if offset is not aligned on a page boundary
					if((framebufferOffset & (CGsPixelFormats::PAGESIZE - 1)) != 0) continue;

					auto framebufferPageSize = CGsPixelFormats::GetPsmPageSize(candidateFramebuffer->m_psm);
					uint32 framebufferPageCountX = candidateFramebuffer->m_width / framebufferPageSize.first;
					uint32 framebufferPageIndex = framebufferOffset / CGsPixelFormats::PAGESIZE;

					//Bail if pointed page isn't on the first line
					if(framebufferPageIndex >= framebufferPageCountX) continue;

					framebuffer = candidateFramebuffer;
					texInfo.offsetX = static_cast<float>(framebufferPageIndex * framebufferPageSize.first) / static_cast<float>(candidateFramebuffer->m_width);
					break;
				}
			}
		}

		lookup.framebufferIndex = -1;
		if(framebuffer)
		{
			lookup.framebufferIndex = static_cast<int32>(std::find(m_framebuffers.begin(), m_framebuffers.end(), framebuffer) - m_framebuffers.begin());
		}
		lookup.offsetX = texInfo.offsetX;
		lookup.alphaAsIndex = texInfo.alphaAsIndex;
	}

	if(framebuffer)
	{
		CommitFramebufferDirtyPages(framebuffer, 0, tex0.GetHeight());
		if(m_multisampleEnabled)
		{
			ResolveFramebufferMultisample(framebuffer, m_fbScale);
		}

		float scaleRatioX = static_cast<float>(tex0.GetWidth()) / static_cast<float>(framebuffer->m_width);
		float scaleRatioY = static_cast<float>(tex0.GetHeight()) / static_cast<float>(framebuffer->m_height);

		texInfo.textureHandle = framebuffer->m_texture;
		texInfo.scaleRatioX = scaleRatioX;
		texInfo.scaleRatioY = scaleRatioY;
		return texInfo;
	}
	else
	{
		return TEXTURE_INFO();
	}
}

CGSH_OpenGL::TEXTURE_INFO CGSH_OpenGL::PrepareTexture(const TEX0& tex0)
{
	auto texInfo = SearchTextureFramebuffer(tex0);
	if(texInfo.textureHandle != 0)
	{
		return texInfo;
	}

	if(PrepareFramebufferByteTexture(tex0, texInfo))
	{
		return texInfo;
	}

	//Formats not read from RAM (depth formats) don't need what the GPU drew there in RAM.
	if(m_textureUpdater[tex0.nPsm] != &CGSH_OpenGL::TexUpdater_Invalid)
	{
		SyncTextureArea(tex0);
	}
	auto texture = m_textureCache.Search(tex0);
	if(!texture)
	{
		//Validate texture dimensions to prevent problems
		auto texWidth = tex0.GetWidth();
		auto texHeight = tex0.GetHeight();
		assert(texWidth <= TEX0_MAX_TEXTURE_SIZE);
		assert(texHeight <= TEX0_MAX_TEXTURE_SIZE);
		texWidth = std::min<uint32>(texWidth, TEX0_MAX_TEXTURE_SIZE);
		texHeight = std::min<uint32>(texHeight, TEX0_MAX_TEXTURE_SIZE);
		auto texFormat = GetTextureFormatInfo(tex0.nPsm);

		{
			auto textureHandle = Framework::OpenGl::CTexture::Create();
			glBindTexture(GL_TEXTURE_2D, textureHandle);
			glTexStorage2D(GL_TEXTURE_2D, 1, texFormat.internalFormat, texWidth, texHeight);
			CHECKGLERROR();
			m_textureCache.Insert(tex0, std::move(textureHandle));
		}

		texture = m_textureCache.Search(tex0);
		texture->m_cachedArea.Invalidate(0, RAMSIZE);
	}

	texInfo.textureHandle = texture->m_textureHandle;

	glBindTexture(GL_TEXTURE_2D, texture->m_textureHandle);
	auto& cachedArea = texture->m_cachedArea;
	auto texturePageSize = CGsPixelFormats::GetPsmPageSize(tex0.nPsm);
	auto areaRect = cachedArea.GetAreaPageRect();

	while(cachedArea.HasDirtyPages())
	{
		auto dirtyRect = cachedArea.GetDirtyPageRect();
		assert((dirtyRect.width != 0) && (dirtyRect.height != 0));
		cachedArea.ClearDirtyPages(dirtyRect);

		uint32 texX = dirtyRect.x * texturePageSize.first;
		uint32 texY = dirtyRect.y * texturePageSize.second;
		uint32 texWidth = dirtyRect.width * texturePageSize.first;
		uint32 texHeight = dirtyRect.height * texturePageSize.second;
		if(texX >= tex0.GetWidth()) continue;
		if(texY >= tex0.GetHeight()) continue;
		//assert(texX < tex0.GetWidth());
		//assert(texY < tex0.GetHeight());
		if((texX + texWidth) > tex0.GetWidth())
		{
			texWidth = tex0.GetWidth() - texX;
		}
		if((texY + texHeight) > tex0.GetHeight())
		{
			texHeight = tex0.GetHeight() - texY;
		}
		((this)->*(m_textureUpdater[tex0.nPsm]))(tex0.GetBufPtr(), tex0.nBufWidth, texX, texY, texWidth, texHeight);
	}

	cachedArea.ClearDirtyPages();

	return texInfo;
}

GLuint CGSH_OpenGL::PreparePalette(const TEX0& tex0)
{
	if(m_gpuClutActive)
	{
		if(CGsPixelFormats::IsPsmIDTEX8(tex0.nPsm))
		{
			//The texture is the palette's framebuffer read through its alpha: the palette needs its own copy.
			if(m_gpuClutSource && m_renderState.texture0AlphaAsIndex && (m_renderState.texture0Handle == m_gpuClutSource->m_texture))
			{
				MaterializeGpuClut();
			}
			if(!m_gpuClutSource) return m_gpuClutTexture;
			if(m_multisampleEnabled) ResolveFramebufferMultisample(m_gpuClutSource, m_fbScale);
			for(int quarter = 0; quarter < 4; quarter++)
			{
				int block = m_gpuClutPosition[1] + quarter;
				int page = m_gpuClutPosition[0] + (block >> 5);
				block &= 31;
				int blockX = (block & 1) | ((block >> 1) & 2) | ((block >> 2) & 4);
				int blockY = ((block >> 1) & 1) | ((block >> 2) & 2);
				m_fragmentParams.clutOrigins[(quarter * 2) + 0] = ((page % m_gpuClutPosition[2]) * 64) + (blockX * 8);
				m_fragmentParams.clutOrigins[(quarter * 2) + 1] = ((page / m_gpuClutPosition[2]) * 32) + (blockY * 8);
			}
			m_fragmentParams.clutScale[0] = m_gpuClutPosition[3];
			m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;
			return m_gpuClutSource->m_texture;
		}
		ReadGpuClut();
	}
	GLuint textureHandle = PalCache_Search(tex0);
	if(textureHandle != 0)
	{
		return textureHandle;
	}

	std::array<uint32, 256> convertedClut;
	MakeLinearCLUT(tex0, convertedClut);

	unsigned int entryCount = CGsPixelFormats::IsPsmIDTEX4(tex0.nPsm) ? 16 : 256;
	textureHandle = PalCache_Search(entryCount, convertedClut.data());
	if(textureHandle != 0)
	{
		return textureHandle;
	}

	glGenTextures(1, &textureHandle);
	glBindTexture(GL_TEXTURE_2D, textureHandle);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, entryCount, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, convertedClut.data());
	//Palettes are always sampled this way: setting it once spares the driver a texture state change per draw.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	PalCache_Insert(tex0, convertedClut.data(), textureHandle);

	return textureHandle;
}

void CGSH_OpenGL::DumpTexture(unsigned int nWidth, unsigned int nHeight, uint32 checksum)
{
#ifdef _WIN32
	char sFilename[256];

	for(unsigned int i = 0; i < UINT_MAX; i++)
	{
		struct _stat Stat;
		sprintf(sFilename, "./textures/tex_%08X_%08X.bmp", i, checksum);
		if(_stat(sFilename, &Stat) == -1) break;
	}

	Framework::CBitmap bitmap(nWidth, nHeight, 32);

	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, bitmap.GetPixels());
	Framework::CStdStream outputStream(fopen(sFilename, "wb"));
	Framework::CBMP::WriteBitmap(bitmap, outputStream);
#endif
}

void CGSH_OpenGL::TexUpdater_Invalid(uint32 bufPtr, uint32 bufWidth, unsigned int texX, unsigned int texY, unsigned int texWidth, unsigned int texHeight)
{
	assert(0);
}

void CGSH_OpenGL::TexUpdater_Psm32(uint32 bufPtr, uint32 bufWidth, unsigned int texX, unsigned int texY, unsigned int texWidth, unsigned int texHeight)
{
	CGsPixelFormats::CPixelIndexorPSMCT32 indexor(m_pRAM, bufPtr, bufWidth);

	uint32* dst = reinterpret_cast<uint32*>(m_pCvtBuffer);
	for(unsigned int y = 0; y < texHeight; y++)
	{
		for(unsigned int x = 0; x < texWidth; x++)
		{
			dst[x] = indexor.GetPixel(texX + x, texY + y);
		}

		dst += texWidth;
	}

	UploadCvtBuffer(texX, texY, texWidth, texHeight, GL_RGBA, GL_UNSIGNED_BYTE, 4);
	CHECKGLERROR();
}

template <typename IndexorType>
void CGSH_OpenGL::TexUpdater_Psm16(uint32 bufPtr, uint32 bufWidth, unsigned int texX, unsigned int texY, unsigned int texWidth, unsigned int texHeight)
{
	IndexorType indexor(m_pRAM, bufPtr, bufWidth);

	auto dst = reinterpret_cast<uint16*>(m_pCvtBuffer);
	for(unsigned int y = 0; y < texHeight; y++)
	{
		for(unsigned int x = 0; x < texWidth; x++)
		{
			auto pixel = indexor.GetPixel(texX + x, texY + y);
			auto cvtPixel =
			    (((pixel & 0x001F) >> 0) << 11) | //R
			    (((pixel & 0x03E0) >> 5) << 6) |  //G
			    (((pixel & 0x7C00) >> 10) << 1) | //B
			    (pixel >> 15);                    //A
			dst[x] = cvtPixel;
		}

		dst += texWidth;
	}

	UploadCvtBuffer(texX, texY, texWidth, texHeight, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, 2);
	CHECKGLERROR();
}

#if defined(FRAMEWORK_SIMD_USE_SSE)
#include <xmmintrin.h>
#include <emmintrin.h>
#include <tmmintrin.h>

void convertColumn8(uint8* dest, const int destStride, int colNum, __m128i a, __m128i b, __m128i c, __m128i d)
{
	__m128i* mdest = (__m128i*)dest;

	__m128i temp_a = a;
	__m128i temp_c = c;

	a = _mm_unpacklo_epi8(temp_a, b);
	c = _mm_unpackhi_epi8(temp_a, b);
	b = _mm_unpacklo_epi8(temp_c, d);
	d = _mm_unpackhi_epi8(temp_c, d);

	temp_a = a;
	temp_c = c;

	a = _mm_unpacklo_epi16(temp_a, b);
	c = _mm_unpackhi_epi16(temp_a, b);
	b = _mm_unpacklo_epi16(temp_c, d);
	d = _mm_unpackhi_epi16(temp_c, d);

	temp_a = a;
	__m128i temp_b = b;

	a = _mm_unpacklo_epi8(temp_a, c);
	b = _mm_unpackhi_epi8(temp_a, c);
	c = _mm_unpacklo_epi8(temp_b, d);
	d = _mm_unpackhi_epi8(temp_b, d);

	temp_a = a;
	temp_c = c;

	a = _mm_unpacklo_epi64(temp_a, b);
	c = _mm_unpackhi_epi64(temp_a, b);
	b = _mm_unpacklo_epi64(temp_c, d);
	d = _mm_unpackhi_epi64(temp_c, d);

	if((colNum & 1) == 0)
	{
		c = _mm_shuffle_epi32(c, _MM_SHUFFLE(2, 3, 0, 1));
		d = _mm_shuffle_epi32(d, _MM_SHUFFLE(2, 3, 0, 1));
	}
	else
	{
		a = _mm_shuffle_epi32(a, _MM_SHUFFLE(2, 3, 0, 1));
		b = _mm_shuffle_epi32(b, _MM_SHUFFLE(2, 3, 0, 1));
	}

	int mStride = destStride / 16;

	mdest[0] = a;
	mdest[mStride] = b;
	mdest[mStride * 2] = c;
	mdest[mStride * 3] = d;
}

inline void convertColumn8(uint8* dest, const int destStride, uint8* src, int colNum)
{
	__m128i* mSrc = (__m128i*)src;

	__m128i a = mSrc[0];
	__m128i b = mSrc[1];
	__m128i c = mSrc[2];
	__m128i d = mSrc[3];
	convertColumn8(dest, destStride, colNum, a, b, c, d);
}

inline void convertColumn4(uint8* dest, const int destStride, uint8* src, int colNum)
{
	__m128i* mSrc = (__m128i*)src;

	__m128i a = mSrc[0];
	__m128i b = mSrc[1];
	__m128i c = mSrc[2];
	__m128i d = mSrc[3];

	// 4 bpp looks like 2 8bpp columns side by side.
	// The 4pp are expanded to 8bpp.
	// so 01 23 45 67 89 ab cd ef gh ij kl mn op qr st uv expands to
	// 00 01 02 03 08 09 0a 0b 0g 0h 0i 0j 0o 0p 0q 0r as the first row on the left hand block.

	__m128i perm = _mm_setr_epi8(0, 1, 4, 5, 8, 9, 0x0c, 0x0d, 2, 3, 6, 7, 0x0a, 0x0b, 0x0e, 0x0f);
	a = _mm_shuffle_epi8(a, perm);
	b = _mm_shuffle_epi8(b, perm);
	c = _mm_shuffle_epi8(c, perm);
	d = _mm_shuffle_epi8(d, perm);

	__m128i a_orig = a;

	const __m128i mask = _mm_set1_epi32(0x0f0f0f0f);
	const __m128i shiftCount = _mm_set_epi32(0, 0, 0, 4);
	__m128i lowNybbles = _mm_and_si128(a, mask);
	__m128i highNybbles = _mm_and_si128(_mm_srl_epi32(a, shiftCount), mask);
	a = _mm_unpacklo_epi8(lowNybbles, highNybbles);
	__m128i a2 = _mm_unpackhi_epi8(lowNybbles, highNybbles);

	lowNybbles = _mm_and_si128(b, mask);
	highNybbles = _mm_and_si128(_mm_srl_epi32(b, shiftCount), mask);
	b = _mm_unpacklo_epi8(lowNybbles, highNybbles);
	__m128i b2 = _mm_unpackhi_epi8(lowNybbles, highNybbles);

	lowNybbles = _mm_and_si128(c, mask);
	highNybbles = _mm_and_si128(_mm_srl_epi32(c, shiftCount), mask);
	c = _mm_unpacklo_epi8(lowNybbles, highNybbles);
	__m128i c2 = _mm_unpackhi_epi8(lowNybbles, highNybbles);

	lowNybbles = _mm_and_si128(d, mask);
	highNybbles = _mm_and_si128(_mm_srl_epi32(d, shiftCount), mask);
	d = _mm_unpacklo_epi8(lowNybbles, highNybbles);
	__m128i d2 = _mm_unpackhi_epi8(lowNybbles, highNybbles);

	convertColumn8(dest, destStride, colNum, a, b, c, d);
	if(destStride > 16)
	{
		convertColumn8(dest + 16, destStride, colNum, a2, b2, c2, d2);
	}
}

#elif defined(FRAMEWORK_SIMD_USE_NEON)
#include <arm_neon.h>

inline void convertColumn8(uint8x16x4_t data, uint8* dest, const int destStride, int colNum)
{
	uint16x8_t row0 = vcombine_u16(vmovn_u32(vreinterpretq_u32_u8(data.val[0])), vmovn_u32(vreinterpretq_u32_u8(data.val[2])));
	uint16x8_t revr0 = vrev32q_u16(vreinterpretq_u16_u8(data.val[0]));
	uint16x8_t revr2 = vrev32q_u16(vreinterpretq_u16_u8(data.val[2]));
	uint16x8_t row1 = vcombine_u16(vmovn_u32(vreinterpretq_u32_u16(revr0)), vmovn_u32(vreinterpretq_u32_u16(revr2)));

	uint16x8_t row2 = vcombine_u16(vmovn_u32(vreinterpretq_u32_u8(data.val[1])), vmovn_u32(vreinterpretq_u32_u8(data.val[3])));
	uint16x8_t revr1 = vrev32q_u16(vreinterpretq_u16_u8(data.val[1]));
	uint16x8_t revr3 = vrev32q_u16(vreinterpretq_u16_u8(data.val[3]));
	uint16x8_t row3 = vcombine_u16(vmovn_u32(vreinterpretq_u32_u16(revr1)), vmovn_u32(vreinterpretq_u32_u16(revr3)));

	if((colNum & 1) == 0)
	{
		row2 = vreinterpretq_u16_u32(vrev64q_u32(vreinterpretq_u32_u16(row2)));
		row3 = vreinterpretq_u16_u32(vrev64q_u32(vreinterpretq_u32_u16(row3)));
	}
	else
	{
		row0 = vreinterpretq_u16_u32(vrev64q_u32(vreinterpretq_u32_u16(row0)));
		row1 = vreinterpretq_u16_u32(vrev64q_u32(vreinterpretq_u32_u16(row1)));
	}

	vst1q_u8(dest, vreinterpretq_u8_u16(row0));
	vst1q_u8(dest + destStride, vreinterpretq_u8_u16(row1));
	vst1q_u8(dest + 2 * destStride, vreinterpretq_u8_u16(row2));
	vst1q_u8(dest + 3 * destStride, vreinterpretq_u8_u16(row3));
}

inline void convertColumn8(uint8* dest, const int destStride, uint8* src, int colNum)
{
	// This sucks in the entire column and de-interleaves it
	uint8x16x4_t data = vld4q_u8(src);
	convertColumn8(data, dest, destStride, colNum);
}

inline void convertColumn4(uint8* dest, const int destStride, uint8* src, int colNum)
{
	// https://developer.arm.com/architectures/instruction-sets/simd-isas/neon/intrinsics

	uint8x16x4_t data = vld4q_u8(src);

	const auto mask = vdupq_n_u8(0x0F);

	auto high_nybbles = vshrq_n_u8(data.val[0], 4);
	auto lo_nybbles = vandq_u8(data.val[0], mask);

	uint8x16x4_t col8Data;
	col8Data.val[0] = lo_nybbles;
	col8Data.val[1] = high_nybbles;

	high_nybbles = vshrq_n_u8(data.val[1], 4);
	lo_nybbles = vandq_u8(data.val[1], mask);
	col8Data.val[2] = lo_nybbles;
	col8Data.val[3] = high_nybbles;
	convertColumn8(col8Data, dest, destStride, colNum);

	if(destStride > 16)
	{

		high_nybbles = vshrq_n_u8(data.val[2], 4);
		lo_nybbles = vandq_u8(data.val[2], mask);
		col8Data.val[0] = lo_nybbles;
		col8Data.val[1] = high_nybbles;
		high_nybbles = vshrq_n_u8(data.val[3], 4);
		lo_nybbles = vandq_u8(data.val[3], mask);
		col8Data.val[2] = lo_nybbles;
		col8Data.val[3] = high_nybbles;
		convertColumn8(col8Data, dest + 16, destStride, colNum);
	}
}

#else
/*
// If we have a platform that does not have SIMD then implement the basic case here.
void convertColumn8(uint8* dest, const int destStride, uint8* src, int colNum)
{

}
*/
#endif

void CGSH_OpenGL::TexUpdater_Psm8(uint32 bufPtr, uint32 bufWidth, unsigned int texX, unsigned int texY, unsigned int texWidth, unsigned int texHeight)
{
	if(texWidth < 16)
	{
		// Widths are powers of 2, so anything over 16 will be an integral number of columns wide.
		// Note: for small textures it still may be a win to do the SIMD swizzle and then cut out the sub-region to
		// correct the row stride.
		return CGSH_OpenGL::TexUpdater_Psm48<CGsPixelFormats::CPixelIndexorPSMT8>(bufPtr, bufWidth, texX, texY, texWidth, texHeight);
	}

	CGsPixelFormats::CPixelIndexorPSMT8 indexor(m_pRAM, bufPtr, bufWidth);
	uint8* dst = m_pCvtBuffer;
	for(unsigned int y = 0; y < texHeight; y += 16)
	{
		for(unsigned int x = 0; x < texWidth; x += 16)
		{
			uint8* colDst = dst;
			uint8* src = indexor.GetPixelAddress(texX + x, texY + y);

			// process an entire 16x16 block.
			// A column (64 bytes) is 16x4 pixels and they stack vertically in a block

			int colNum = 0;
			for(unsigned int coly = 0; coly < 16; coly += 4)
			{
				convertColumn8(colDst + x, texWidth, src, colNum++);
				src += 64;
				colDst += texWidth * 4;
			}
		}

		dst += texWidth * 16;
	}

	UploadCvtBuffer(texX, texY, texWidth, texHeight, GL_RED, GL_UNSIGNED_BYTE, 1);
	CHECKGLERROR();
}

void CGSH_OpenGL::TexUpdater_Psm4(unsigned int bufPtr, unsigned int bufWidth, unsigned int texX, unsigned int texY, unsigned int texWidth, unsigned int texHeight)
{
	if(texWidth < 16)
	{
		// Widths are powers of 2, so anything over 32 will be an integral number of columns wide.
		// 16 wide textures are dealt with as a special case in the SIMD code.
		// Note: for small textures it still may be a win to do the SIMD swizzle and then cut out the sub-region to
		// correct the row stride.
		return CGSH_OpenGL::TexUpdater_Psm48<CGsPixelFormats::CPixelIndexorPSMT4>(bufPtr, bufWidth, texX, texY, texWidth, texHeight);
	}

	CGsPixelFormats::CPixelIndexorPSMT4 indexor(m_pRAM, bufPtr, bufWidth);

	uint8* dst = m_pCvtBuffer;
	for(unsigned int y = 0; y < texHeight; y += 16)
	{
		for(unsigned int x = 0; x < texWidth; x += 32)
		{
			uint8* colDst = dst + x;
			unsigned int nx = texX + x;
			unsigned int ny = texY + y;
			uint32 colAddr = indexor.GetColumnAddress(nx, ny);
			uint8* src = m_pRAM + colAddr;

			// process an entire 32x16 block.
			// A column (64 bytes) is 32x4 pixels and they stack vertically in a block

			for(unsigned int colNum = 0; colNum < 4; ++colNum)
			{
				convertColumn4(colDst, texWidth, src, colNum);
				src += 64;
				colDst += texWidth * 4;
			}
		}

		dst += texWidth * 16;
	}
	UploadCvtBuffer(texX, texY, texWidth, texHeight, GL_RED, GL_UNSIGNED_BYTE, 1);
	CHECKGLERROR();
}

template <typename IndexorType>
void CGSH_OpenGL::TexUpdater_Psm48(uint32 bufPtr, uint32 bufWidth, unsigned int texX, unsigned int texY, unsigned int texWidth, unsigned int texHeight)
{
	IndexorType indexor(m_pRAM, bufPtr, bufWidth);

	uint8* dst = m_pCvtBuffer;
	for(unsigned int y = 0; y < texHeight; y++)
	{
		for(unsigned int x = 0; x < texWidth; x++)
		{
			uint8 pixel = indexor.GetPixel(texX + x, texY + y);
			dst[x] = pixel;
		}

		dst += texWidth;
	}

	UploadCvtBuffer(texX, texY, texWidth, texHeight, GL_RED, GL_UNSIGNED_BYTE, 1);
	CHECKGLERROR();
}

template <uint32 shiftAmount, uint32 mask>
void CGSH_OpenGL::TexUpdater_Psm48H(uint32 bufPtr, uint32 bufWidth, unsigned int texX, unsigned int texY, unsigned int texWidth, unsigned int texHeight)
{
	CGsPixelFormats::CPixelIndexorPSMCT32 indexor(m_pRAM, bufPtr, bufWidth);

	uint8* dst = m_pCvtBuffer;
	for(unsigned int y = 0; y < texHeight; y++)
	{
		for(unsigned int x = 0; x < texWidth; x++)
		{
			uint32 pixel = indexor.GetPixel(texX + x, texY + y);
			pixel = (pixel >> shiftAmount) & mask;
			dst[x] = static_cast<uint8>(pixel);
		}

		dst += texWidth;
	}

	UploadCvtBuffer(texX, texY, texWidth, texHeight, GL_RED, GL_UNSIGNED_BYTE, 1);
	CHECKGLERROR();
}

/////////////////////////////////////////////////////////////
// Palette
/////////////////////////////////////////////////////////////

CGSH_OpenGL::CPalette::CPalette()
    : m_live(false)
    , m_isIDTEX4(false)
    , m_cpsm(0)
    , m_csa(0)
    , m_texture(0)
{
}

CGSH_OpenGL::CPalette::~CPalette()
{
	Free();
}

void CGSH_OpenGL::CPalette::Free()
{
	if(m_texture != 0)
	{
		glDeleteTextures(1, &m_texture);
		m_texture = 0;
		m_live = false;
	}
}

/////////////////////////////////////////////////////////////
// Palette Caching
/////////////////////////////////////////////////////////////

GLuint CGSH_OpenGL::PalCache_Search(const TEX0& tex0)
{
	for(auto paletteIterator(m_paletteCache.begin());
	    paletteIterator != m_paletteCache.end(); paletteIterator++)
	{
		const auto& palette = *paletteIterator;
		//Palettes used since the last invalidation are kept at the front.
		if(palette->m_generation != m_paletteGeneration) break;
		if(!palette->m_live) continue;
		if(CGsPixelFormats::IsPsmIDTEX4(tex0.nPsm) != palette->m_isIDTEX4) continue;
		if(tex0.nCPSM != palette->m_cpsm) continue;
		if(tex0.nCSA != palette->m_csa) continue;
		auto texture = palette->m_texture;
		m_paletteCache.splice(m_paletteCache.begin(), m_paletteCache, paletteIterator);
		return texture;
	}

	return 0;
}

GLuint CGSH_OpenGL::PalCache_Search(unsigned int entryCount, const uint32* contents)
{
	for(auto paletteIterator(m_paletteCache.begin());
	    paletteIterator != m_paletteCache.end(); paletteIterator++)
	{
		const auto& palette = *paletteIterator;

		if(palette->m_texture == 0) continue;

		unsigned int palEntryCount = palette->m_isIDTEX4 ? 16 : 256;
		if(palEntryCount != entryCount) continue;

		if(memcmp(contents, palette->m_contents, sizeof(uint32) * entryCount) != 0) continue;

		palette->m_live = true;
		palette->m_generation = m_paletteGeneration;

		auto texture = palette->m_texture;
		m_paletteCache.splice(m_paletteCache.begin(), m_paletteCache, paletteIterator);
		return texture;
	}

	return 0;
}

void CGSH_OpenGL::PalCache_Insert(const TEX0& tex0, const uint32* contents, GLuint textureHandle)
{
	const auto& texture = *m_paletteCache.rbegin();
	texture->Free();

	unsigned int entryCount = CGsPixelFormats::IsPsmIDTEX4(tex0.nPsm) ? 16 : 256;

	texture->m_isIDTEX4 = CGsPixelFormats::IsPsmIDTEX4(tex0.nPsm);
	texture->m_cpsm = tex0.nCPSM;
	texture->m_csa = tex0.nCSA;
	texture->m_texture = textureHandle;
	texture->m_live = true;
	texture->m_generation = m_paletteGeneration;
	memcpy(texture->m_contents, contents, entryCount * sizeof(uint32));

	m_paletteCache.splice(m_paletteCache.begin(), m_paletteCache, std::prev(m_paletteCache.end()));
}

void CGSH_OpenGL::PalCache_Invalidate(uint32 csa)
{
	m_paletteGeneration++;
}

void CGSH_OpenGL::PalCache_Flush()
{
	std::for_each(std::begin(m_paletteCache), std::end(m_paletteCache),
	              [](PalettePtr& palette) { palette->Free(); });
}
