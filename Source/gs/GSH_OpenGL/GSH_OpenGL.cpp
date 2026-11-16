#include <stdio.h>
#include <assert.h>
#include <cstring>
#include <math.h>

#include "AppConfig.h"
#include "../GsPixelFormats.h"
#include "../GsTransferRange.h"
#include "GSH_OpenGL.h"
#ifdef __ANDROID__
#include <GLES2/gl2ext.h>
#endif

#ifdef USE_DUALSOURCE_BLENDING
//Dual source blending constants
#define BLEND_SRC_ALPHA GL_SRC1_ALPHA
#define BLEND_ONE_MINUS_SRC_ALPHA GL_ONE_MINUS_SRC1_ALPHA
#else
//Standard blending constants
#define BLEND_SRC_ALPHA GL_SRC_ALPHA
#define BLEND_ONE_MINUS_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
#endif

#define NUM_SAMPLES 8
#define FRAMEBUFFER_HEIGHT 1024

// clang-format off
const GLenum CGSH_OpenGL::g_nativeClampModes[CGSHandler::CLAMP_MODE_MAX] =
{
	GL_REPEAT,
	GL_CLAMP_TO_EDGE,
	GL_REPEAT,
	GL_REPEAT
};

const unsigned int CGSH_OpenGL::g_shaderClampModes[CGSHandler::CLAMP_MODE_MAX] =
{
	TEXTURE_CLAMP_MODE_STD,
	TEXTURE_CLAMP_MODE_CLAMP,
	TEXTURE_CLAMP_MODE_REGION_CLAMP,
	TEXTURE_CLAMP_MODE_REGION_REPEAT
};

const unsigned int CGSH_OpenGL::g_alphaTestInverse[CGSHandler::ALPHA_TEST_MAX] =
{
	ALPHA_TEST_ALWAYS,
	ALPHA_TEST_NEVER,
	ALPHA_TEST_GEQUAL,
	ALPHA_TEST_GREATER,
	ALPHA_TEST_NOTEQUAL,
	ALPHA_TEST_LESS,
	ALPHA_TEST_LEQUAL,
	ALPHA_TEST_EQUAL
};
// clang-format on

static uint32 MakeColor(uint8 r, uint8 g, uint8 b, uint8 a)
{
	return (a << 24) | (b << 16) | (g << 8) | (r);
}

CGSH_OpenGL::CGSH_OpenGL(bool gsThreaded)
    : CGSHandler(gsThreaded)
    , m_pCvtBuffer(nullptr)
{
	RegisterPreferences();
	LoadPreferences();

	m_pCvtBuffer = new uint8[CVTBUFFERSIZE];

	memset(&m_renderState, 0, sizeof(m_renderState));
	m_vertexBuffer.reserve(VERTEX_BUFFER_SIZE);
}

CGSH_OpenGL::~CGSH_OpenGL()
{
	delete[] m_pCvtBuffer;
}

void CGSH_OpenGL::InitializeImpl()
{
	InitializeRC();

	m_nVtxCount = 0;

	for(unsigned int i = 0; i < MAX_PALETTE_CACHE; i++)
	{
		m_paletteCache.push_back(PalettePtr(new CPalette()));
	}

	m_renderState.isValid = false;
	m_validGlState = 0;
}

void CGSH_OpenGL::ReleaseImpl()
{
	ResetImpl();

	m_paletteCache.clear();
	m_shaders.clear();
	m_presentProgram.reset();
	m_presentVertexBuffer.Reset();
	m_presentVertexArray.Reset();
	m_copyToFbProgram.reset();
	m_gpuClutProgram.reset();
	m_gpuClutTexture.Reset();
	m_gpuClutFramebuffer.Reset();
	m_gpuClutActive = false;
	m_copyToFbTexture.Reset();
	m_copyToFbVertexBuffer.Reset();
	m_copyToFbVertexArray.Reset();
	ReleaseStreamBuffer(m_primStream);
	ReleaseStreamBuffer(m_vertexParamsStream);
	ReleaseStreamBuffer(m_fragmentParamsStream);
	ReleaseStreamBuffer(m_unpackStream);
	m_unpackBuffer.Reset();
	m_primBuffer.Reset();
	m_primVertexArray.Reset();
	m_vertexParamsBuffer.Reset();
	m_fragmentParamsBuffer.Reset();
}

void CGSH_OpenGL::ResetImpl()
{
	LoadPreferences();
	m_textureCache.Flush();
	PalCache_Flush();
	m_framebuffers.clear();
	m_framebuffersVersion++;
	ClearAllGpuPages();
	m_depthbuffers.clear();
	m_vertexBuffer.clear();
	m_renderState.isValid = false;
	m_validGlState = 0;
	m_nVtxCount = 0;
	m_drawingToDepth = false;
	m_primitiveType = PRIM_INVALID;
	m_pendingPrim = false;
	m_pendingPrimValue = 0;
}

void CGSH_OpenGL::MarkNewFrame()
{
	FlushVertexBuffer();
	m_renderState.isValid = false;
	m_validGlState = 0;
	CGSHandler::MarkNewFrame();
}

void CGSH_OpenGL::FlipImpl(const DISPLAY_INFO& dispInfo)
{
	FlushVertexBuffer();
	m_renderState.isValid = false;
	m_validGlState = 0;

	const auto& dispLayer = dispInfo.layers[0];

	FramebufferPtr framebuffer;

	if(dispLayer.enabled)
	{
		for(const auto& candidateFramebuffer : m_framebuffers)
		{
			if(
			    (candidateFramebuffer->m_basePtr == dispLayer.bufPtr) &&
			    (GetFramebufferBitDepth(candidateFramebuffer->m_psm) == GetFramebufferBitDepth(dispLayer.psm)) &&
			    (candidateFramebuffer->m_width == dispLayer.bufWidth))
			{
				//We have a winner
				framebuffer = candidateFramebuffer;
				break;
			}
		}

		if(!framebuffer && (dispLayer.bufWidth != 0))
		{
			framebuffer = FramebufferPtr(new CFramebuffer(dispLayer.bufPtr, dispLayer.bufWidth, FRAMEBUFFER_HEIGHT, dispLayer.psm, m_fbScale, m_multisampleEnabled));
			m_framebuffers.push_back(framebuffer);
			m_framebuffersVersion++;
			PopulateFramebuffer(framebuffer);
		}
	}

	if(framebuffer)
	{
		CommitFramebufferDirtyPages(framebuffer, 0, dispLayer.height);
		if(m_multisampleEnabled)
		{
			ResolveFramebufferMultisample(framebuffer, m_fbScale);
		}
	}

	//Clear all of our output framebuffer
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_presentFramebuffer);

		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0, 0, 0, 0);
		glViewport(0, 0, m_presentationParams.windowWidth, m_presentationParams.windowHeight);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	auto presentationViewport = GetPresentationViewport();
	if((dispInfo.oddFieldOffset != 0) && (dispLayer.height != 0))
	{
		//Without this, the picture moves up and down every field.
		presentationViewport.offsetY += static_cast<int32>(dispInfo.oddFieldOffset * presentationViewport.height / static_cast<float>(dispLayer.height));
	}
	glViewport(presentationViewport.offsetX, presentationViewport.offsetY,
	           presentationViewport.width, presentationViewport.height);

	if(framebuffer)
	{
		float u1 = static_cast<float>(dispLayer.width) / static_cast<float>(framebuffer->m_width);
		float v1 = static_cast<float>(dispLayer.height) / static_cast<float>(framebuffer->m_height);

		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, 0);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, framebuffer->m_texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);

		glUseProgram(*m_presentProgram);

		assert(m_presentTextureUniform != -1);
		glUniform1i(m_presentTextureUniform, 0);

		assert(m_presentTexCoordScaleUniform != -1);
		glUniform2f(m_presentTexCoordScaleUniform, u1, v1);

		glBindBuffer(GL_ARRAY_BUFFER, m_presentVertexBuffer);
		glBindVertexArray(m_presentVertexArray);

#ifdef _DEBUG
		m_presentProgram->Validate();
#endif

		glDrawArrays(GL_TRIANGLES, 0, 3);
	}

	CHECKGLERROR();

	static bool g_dumpFramebuffers = false;
	if(g_dumpFramebuffers)
	{
		for(const auto& framebuffer : m_framebuffers)
		{
			glBindTexture(GL_TEXTURE_2D, framebuffer->m_texture);
			DumpTexture(framebuffer->m_width * m_fbScale, framebuffer->m_height * m_fbScale, framebuffer->m_basePtr);
		}
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	PresentBackbuffer();
	CGSHandler::FlipImpl(dispInfo);
}

void CGSH_OpenGL::LoadState(Framework::CZipArchiveReader& archive)
{
	CGSHandler::LoadState(archive);
	SendGSCall(
	    [this]() {
		    m_textureCache.InvalidateRange(0, RAMSIZE);
	    });
}

void CGSH_OpenGL::RegisterPreferences()
{
	CGSHandler::RegisterPreferences();
	CAppConfig::GetInstance().RegisterPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR, 1);
	CAppConfig::GetInstance().RegisterPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES, false);
}

void CGSH_OpenGL::NotifyPreferencesChangedImpl()
{
	LoadPreferences();
	m_textureCache.Flush();
	PalCache_Flush();
	m_framebuffers.clear();
	m_framebuffersVersion++;
	ClearAllGpuPages();
	m_depthbuffers.clear();
	CGSHandler::NotifyPreferencesChangedImpl();
}

void CGSH_OpenGL::LoadPreferences()
{
	m_fbScale = CAppConfig::GetInstance().GetPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR);
	m_forceBilinearTextures = CAppConfig::GetInstance().GetPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES);
}

void CGSH_OpenGL::InitializeRC()
{
	//Initialize basic stuff
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepthf(0.0f);

	CheckExtensions();
	SetupTextureUpdaters();
	LoadProgramCache();

	m_presentProgram = GeneratePresentProgram();
	m_presentVertexBuffer = GeneratePresentVertexBuffer();
	m_presentVertexArray = GeneratePresentVertexArray();
	m_presentTextureUniform = glGetUniformLocation(*m_presentProgram, "g_texture");
	m_presentTexCoordScaleUniform = glGetUniformLocation(*m_presentProgram, "g_texCoordScale");

	m_copyToFbProgram = GenerateCopyToFbProgram();
	m_copyToFbTexture = Framework::OpenGl::CTexture::Create();
	m_copyToFbVertexBuffer = GenerateCopyToFbVertexBuffer();
	m_copyToFbVertexArray = GenerateCopyToFbVertexArray();
	m_copyToFbSrcPositionUniform = glGetUniformLocation(*m_copyToFbProgram, "g_srcPosition");
	m_copyToFbSrcSizeUniform = glGetUniformLocation(*m_copyToFbProgram, "g_srcSize");

	m_primBuffer = Framework::OpenGl::CBuffer::Create();
	m_primVertexArray = GeneratePrimVertexArray();

	m_vertexParamsBuffer = GenerateUniformBlockBuffer(sizeof(VERTEXPARAMS));
	m_fragmentParamsBuffer = GenerateUniformBlockBuffer(sizeof(FRAGMENTPARAMS));

	glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &m_uniformBufferAlignment);
	if(m_uniformBufferAlignment <= 0) m_uniformBufferAlignment = 256;
	//Each segment switch costs a fence, which the driver submits: segments hold a few frames' worth.
	CreateStreamBuffer(m_primBuffer, m_primStream, GL_ARRAY_BUFFER, 0x2000000);
	CreateStreamBuffer(m_vertexParamsBuffer, m_vertexParamsStream, GL_UNIFORM_BUFFER, 0x1000000);
	CreateStreamBuffer(m_fragmentParamsBuffer, m_fragmentParamsStream, GL_UNIFORM_BUFFER, 0x1000000);
	m_unpackBuffer = Framework::OpenGl::CBuffer::Create();
	CreateStreamBuffer(m_unpackBuffer, m_unpackStream, GL_PIXEL_UNPACK_BUFFER, 0x2000000);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	m_validGlState &= ~(GLSTATE_VERTEX_PARAMS | GLSTATE_FRAGMENT_PARAMS);

	PresentBackbuffer();

	CHECKGLERROR();
}

void CGSH_OpenGL::CheckExtensions()
{
	GLint numExtensions = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
	for(GLint i = 0; i < numExtensions; i++)
	{
		const char* extensionName = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i));
		if(!strcmp(extensionName, "GL_EXT_shader_framebuffer_fetch"))
		{
			m_hasFramebufferFetchExtension = true;
		}
		else if(!strcmp(extensionName, "GL_ARM_shader_framebuffer_fetch_depth_stencil"))
		{
			m_hasFramebufferFetchDepthExtension = true;
		}
		else if(!strcmp(extensionName, "GL_EXT_buffer_storage"))
		{
			m_hasBufferStorageExtension = true;
		}
	}

	//Blending in the shader through framebuffer fetch makes every draw much more expensive in Adreno's
	//driver (a Colin McRae Rally 3 stage ran 13% faster without it), so blend with the fixed function there.
	auto renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
	if(renderer && strstr(renderer, "Adreno"))
	{
		m_hasFramebufferFetchExtension = false;
	}
}

Framework::OpenGl::CBuffer CGSH_OpenGL::GeneratePresentVertexBuffer()
{
	auto buffer = Framework::OpenGl::CBuffer::Create();

	// clang-format off
	static const float bufferContents[] =
	{
		//Pos         UV
		-1.0f, -1.0f, 0.0f,  1.0f,
		-1.0f,  3.0f, 0.0f, -1.0f,
		 3.0f, -1.0f, 2.0f,  1.0f,
	};
	// clang-format on

	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(bufferContents), bufferContents, GL_STATIC_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	CHECKGLERROR();

	return buffer;
}

Framework::OpenGl::CVertexArray CGSH_OpenGL::GeneratePresentVertexArray()
{
	auto vertexArray = Framework::OpenGl::CVertexArray::Create();

	glBindVertexArray(vertexArray);

	glBindBuffer(GL_ARRAY_BUFFER, m_presentVertexBuffer);

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), 2, GL_FLOAT,
	                      GL_FALSE, sizeof(float) * 4, reinterpret_cast<const GLvoid*>(0));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), 2, GL_FLOAT,
	                      GL_FALSE, sizeof(float) * 4, reinterpret_cast<const GLvoid*>(8));

	glBindVertexArray(0);

	CHECKGLERROR();

	return vertexArray;
}

Framework::OpenGl::CBuffer CGSH_OpenGL::GenerateCopyToFbVertexBuffer()
{
	auto buffer = Framework::OpenGl::CBuffer::Create();

	// clang-format off
	static const float bufferContents[] =
	{
		//Pos       UV
		-1.0f, -1.0f, 0.0f, 0.0f,
		 1.0f, -1.0f, 1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f, 1.0f,
		 1.0f,  1.0f, 1.0f, 1.0f,
	};
	// clang-format on

	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(bufferContents), bufferContents, GL_STATIC_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	CHECKGLERROR();

	return buffer;
}

Framework::OpenGl::CVertexArray CGSH_OpenGL::GenerateCopyToFbVertexArray()
{
	auto vertexArray = Framework::OpenGl::CVertexArray::Create();

	glBindVertexArray(vertexArray);

	glBindBuffer(GL_ARRAY_BUFFER, m_copyToFbVertexBuffer);

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), 2, GL_FLOAT,
	                      GL_FALSE, sizeof(float) * 4, reinterpret_cast<const GLvoid*>(0));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), 2, GL_FLOAT,
	                      GL_FALSE, sizeof(float) * 4, reinterpret_cast<const GLvoid*>(8));

	glBindVertexArray(0);

	CHECKGLERROR();

	return vertexArray;
}

Framework::OpenGl::CVertexArray CGSH_OpenGL::GeneratePrimVertexArray()
{
	auto vertexArray = Framework::OpenGl::CVertexArray::Create();

	glBindVertexArray(vertexArray);

	glBindBuffer(GL_ARRAY_BUFFER, m_primBuffer);

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), 2, GL_FLOAT,
	                      GL_FALSE, sizeof(PRIM_VERTEX), reinterpret_cast<const GLvoid*>(offsetof(PRIM_VERTEX, x)));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::DEPTH));
	glVertexAttribIPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::DEPTH), 1, GL_UNSIGNED_INT,
	                       sizeof(PRIM_VERTEX), reinterpret_cast<const GLvoid*>(offsetof(PRIM_VERTEX, z)));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::COLOR));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::COLOR), 4, GL_UNSIGNED_BYTE,
	                      GL_TRUE, sizeof(PRIM_VERTEX), reinterpret_cast<const GLvoid*>(offsetof(PRIM_VERTEX, color)));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), 3, GL_FLOAT,
	                      GL_FALSE, sizeof(PRIM_VERTEX), reinterpret_cast<const GLvoid*>(offsetof(PRIM_VERTEX, s)));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::FOG));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::FOG), 1, GL_FLOAT,
	                      GL_FALSE, sizeof(PRIM_VERTEX), reinterpret_cast<const GLvoid*>(offsetof(PRIM_VERTEX, f)));

	glBindVertexArray(0);

	CHECKGLERROR();

	return vertexArray;
}

Framework::OpenGl::CBuffer CGSH_OpenGL::GenerateUniformBlockBuffer(size_t blockSize)
{
	auto uniformBlockBuffer = Framework::OpenGl::CBuffer::Create();

	glBindBuffer(GL_UNIFORM_BUFFER, uniformBlockBuffer);
	glBufferData(GL_UNIFORM_BUFFER, blockSize, nullptr, GL_STREAM_DRAW);
	CHECKGLERROR();

	return uniformBlockBuffer;
}

void CGSH_OpenGL::MakeLinearZOrtho(float* matrix, float left, float right, float bottom, float top)
{
	matrix[0] = 2.0f / (right - left);
	matrix[1] = 0;
	matrix[2] = 0;
	matrix[3] = 0;

	matrix[4] = 0;
	matrix[5] = 2.0f / (top - bottom);
	matrix[6] = 0;
	matrix[7] = 0;

	matrix[8] = 0;
	matrix[9] = 0;
	matrix[10] = 1;
	matrix[11] = 0;

	matrix[12] = -(right + left) / (right - left);
	matrix[13] = -(top + bottom) / (top - bottom);
	matrix[14] = 0;
	matrix[15] = 1;
}

/////////////////////////////////////////////////////////////
// Context Unpacking
/////////////////////////////////////////////////////////////

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GetShaderFromCaps(const SHADERCAPS& shaderCaps)
{
	auto shaderIterator = m_shaders.find(shaderCaps);
	if(shaderIterator == m_shaders.end())
	{
		auto shader = LoadCachedProgram(shaderCaps);
		if(!shader)
		{
			shader = GenerateShader(shaderCaps);
			SaveCachedProgram(shaderCaps, *shader);
		}

		glUseProgram(*shader);
		m_validGlState &= ~GLSTATE_PROGRAM;

		auto textureUniform = glGetUniformLocation(*shader, "g_texture");
		if(textureUniform != -1)
		{
			glUniform1i(textureUniform, 0);
		}

		auto paletteUniform = glGetUniformLocation(*shader, "g_palette");
		if(paletteUniform != -1)
		{
			glUniform1i(paletteUniform, 1);
		}

		auto vertexParamsUniformBlock = glGetUniformBlockIndex(*shader, "VertexParams");
		if(vertexParamsUniformBlock != GL_INVALID_INDEX)
		{
			glUniformBlockBinding(*shader, vertexParamsUniformBlock, 0);
		}

		auto fragmentParamsUniformBlock = glGetUniformBlockIndex(*shader, "FragmentParams");
		if(fragmentParamsUniformBlock != GL_INVALID_INDEX)
		{
			glUniformBlockBinding(*shader, fragmentParamsUniformBlock, 1);
		}

		CHECKGLERROR();

		m_shaders.insert(std::make_pair(shaderCaps, shader));
		shaderIterator = m_shaders.find(shaderCaps);
	}
	return shaderIterator->second;
}

void CGSH_OpenGL::SetRenderingContext(uint64 primReg)
{
	auto prim = make_convertible<PRMODE>(primReg);

	unsigned int context = prim.nContext;

	uint64 testReg = m_nReg[GS_REG_TEST_1 + context];
	uint64 frameReg = m_nReg[GS_REG_FRAME_1 + context];
	uint64 alphaReg = m_nReg[GS_REG_ALPHA_1 + context];
	uint64 zbufReg = m_nReg[GS_REG_ZBUF_1 + context];
	uint64 tex0Reg = m_nReg[GS_REG_TEX0_1 + context];
	uint64 tex1Reg = m_nReg[GS_REG_TEX1_1 + context];
	uint64 texAReg = m_nReg[GS_REG_TEXA];
	uint64 clampReg = m_nReg[GS_REG_CLAMP_1 + context];
	uint64 fogColReg = m_nReg[GS_REG_FOGCOL];
	uint64 scissorReg = m_nReg[GS_REG_SCISSOR_1 + context];

	//--------------------------------------------------------
	//Get shader caps
	//--------------------------------------------------------

	auto shaderCaps = make_convertible<SHADERCAPS>(0);
	FillShaderCapsFromTexture(shaderCaps, tex0Reg, tex1Reg, texAReg, clampReg);
	FillShaderCapsFromTest(shaderCaps, testReg);
	FillShaderCapsFromAlpha(shaderCaps, prim.nAlpha != 0, alphaReg);

	if(prim.nFog)
	{
		shaderCaps.hasFog = 1;
	}

	if(!prim.nTexture)
	{
		shaderCaps.texSourceMode = TEXTURE_SOURCE_MODE_NONE;
		shaderCaps.texPaletteFromFramebuffer = 0;
	}

	if(shaderCaps.texPaletteFromFramebuffer)
	{
		//A draw into the palette's framebuffer would read its own target: it gets a copy.
		auto frame = make_convertible<FRAME>(frameReg);
		if((m_gpuClutSource->m_basePtr == frame.GetBasePtr()) && (m_gpuClutSource->m_width == frame.GetWidth()))
		{
			MaterializeGpuClut();
			shaderCaps.texPaletteFromFramebuffer = 0;
		}
	}

	//--------------------------------------------------------
	// Fast path: if everything (shader caps + relevant regs) matches, skip.
	//--------------------------------------------------------
	if(m_renderState.isValid)
	{
		if(m_renderState.isTextureStateValid && m_renderState.isFramebufferStateValid &&
		   (static_cast<ShaderCapsInt>(m_renderState.shaderCaps) == static_cast<ShaderCapsInt>(shaderCaps)) &&
		   (m_renderState.primReg == primReg) &&
		   (m_renderState.alphaReg == alphaReg) &&
		   (m_renderState.testReg == testReg) &&
		   (m_renderState.zbufReg == zbufReg) &&
		   (m_renderState.frameReg == frameReg) &&
		   (m_renderState.scissorReg == scissorReg) &&
		   (m_renderState.tex0Reg == tex0Reg) &&
		   (m_renderState.tex1Reg == tex1Reg) &&
		   (m_renderState.texAReg == texAReg) &&
		   (m_renderState.clampReg == clampReg) &&
		   (m_renderState.fogColReg == fogColReg))
		{
			// Nothing changed.
			auto offsetFast = make_convertible<XYOFFSET>(m_nReg[GS_REG_XYOFFSET_1 + context]);
			m_nPrimOfsX = offsetFast.GetX();
			m_nPrimOfsY = offsetFast.GetY();
			return;
		}
	}

	//--------------------------------------------------------
	// Determine which states changed; perform a single flush before modifying any.
	//--------------------------------------------------------
	uint64 prevPrimValue = m_renderState.isValid ? m_renderState.primReg : 0;
	auto prevPrim = make_convertible<PRMODE>(prevPrimValue);
	bool shaderChanged = !m_renderState.isValid || (static_cast<ShaderCapsInt>(m_renderState.shaderCaps) != static_cast<ShaderCapsInt>(shaderCaps));
	bool primChanged = !m_renderState.isValid || (m_renderState.primReg != primReg);
	bool blendEnableChanged = primChanged && !m_hasFramebufferFetchExtension && ((!m_renderState.isValid) || (prevPrim.nAlpha != prim.nAlpha));
	bool alphaChanged = !m_renderState.isValid || (m_renderState.alphaReg != alphaReg);
	bool testChanged = !m_renderState.isValid || (m_renderState.testReg != testReg);
	bool zbufChanged = !m_renderState.isValid || (m_renderState.zbufReg != zbufReg);
	bool framebufferRelatedChanged = !m_renderState.isValid || !m_renderState.isFramebufferStateValid ||
	                                 (m_renderState.frameReg != frameReg) || zbufChanged || (m_renderState.scissorReg != scissorReg) || testChanged;
	bool textureEnableChanged = !m_renderState.isValid || (prevPrim.nTexture != prim.nTexture);
	bool textureChanged = !m_renderState.isValid || !m_renderState.isTextureStateValid ||
	                      (m_renderState.tex0Reg != tex0Reg) || (m_renderState.tex1Reg != tex1Reg) || (m_renderState.texAReg != texAReg) ||
	                      (m_renderState.clampReg != clampReg) || textureEnableChanged;
	bool fogColorChanged = !m_renderState.isValid || (m_renderState.fogColReg != fogColReg);

	bool needFlush = shaderChanged || primChanged || alphaChanged || testChanged || zbufChanged || framebufferRelatedChanged || textureChanged || fogColorChanged;
	if(needFlush)
	{
		FlushVertexBuffer();
	}

	//--------------------------------------------------------
	// Apply state changes (order kept similar to original when relevant)
	//--------------------------------------------------------
	if(shaderChanged)
	{
		m_renderState.shaderCaps = shaderCaps;
	}

	if(blendEnableChanged)
	{
		if(!m_hasFramebufferFetchExtension)
		{
			m_renderState.blendEnabled = ((prim.nAlpha != 0) && m_alphaBlendingEnabled) ? GL_TRUE : GL_FALSE;
			m_validGlState &= ~GLSTATE_BLEND;
		}
	}

	if(alphaChanged)
	{
		SetupBlendingFunction(alphaReg);
		CHECKGLERROR();
	}

	if(testChanged)
	{
		SetupTestFunctions(testReg);
		CHECKGLERROR();
	}

	if(zbufChanged || testChanged)
	{
		SetupDepthBuffer(zbufReg, testReg);
		CHECKGLERROR();
	}

	if(framebufferRelatedChanged)
	{
		SetupFramebuffer(frameReg, zbufReg, scissorReg, testReg);
		CHECKGLERROR();
	}

	if(textureChanged)
	{
		SetupTexture(primReg, tex0Reg, tex1Reg, texAReg, clampReg);
		CHECKGLERROR();
	}

	if(m_renderState.shaderCaps.texPaletteFromFramebuffer && !m_gpuClutSource)
	{
		//The palette was copied while the texture was set up: the copy is bound.
		auto caps = m_renderState.shaderCaps;
		caps.texPaletteFromFramebuffer = 0;
		m_renderState.shaderCaps = caps;
	}

	if(fogColorChanged)
	{
		SetupFogColor(fogColReg);
		CHECKGLERROR();
	}

	auto offset = make_convertible<XYOFFSET>(m_nReg[GS_REG_XYOFFSET_1 + context]);
	m_nPrimOfsX = offset.GetX();
	m_nPrimOfsY = offset.GetY();

	CHECKGLERROR();

	m_renderState.isValid = true;
	m_renderState.isTextureStateValid = true;
	m_renderState.isFramebufferStateValid = true;
	m_renderState.primReg = primReg;
	m_renderState.alphaReg = alphaReg;
	m_renderState.testReg = testReg;
	m_renderState.zbufReg = zbufReg;
	m_renderState.scissorReg = scissorReg;
	m_renderState.frameReg = frameReg;
	m_renderState.tex0Reg = tex0Reg;
	m_renderState.tex1Reg = tex1Reg;
	m_renderState.texAReg = texAReg;
	m_renderState.clampReg = clampReg;
	m_renderState.fogColReg = fogColReg;
}

void CGSH_OpenGL::SetupBlendingFunction(uint64 alphaReg)
{
	auto alpha = make_convertible<ALPHA>(alphaReg);

	if(m_hasFramebufferFetchExtension)
	{
		m_fragmentParams.alphaFix = (static_cast<float>(alpha.nFix) / 255.f);
		m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;
		return;
	}

	int nFunction = GL_FUNC_ADD;
	if((alpha.nA == alpha.nB) && (alpha.nD == ALPHABLEND_ABD_CS))
	{
		//ab*0 (when a == b) - Cs
		glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == alpha.nB) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//ab*1 (when a == b) - Cd
		glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == alpha.nB) && (alpha.nD == ALPHABLEND_ABD_ZERO))
	{
		//ab*2 (when a == b) - Zero
		glBlendFuncSeparate(GL_ZERO, GL_ZERO, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CS) && (alpha.nB == ALPHABLEND_ABD_CD) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//0101 - Cs * As + Cd * (1 - As)
		glBlendFuncSeparate(BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 1) && (alpha.nD == 1))
	{
		//Cs * Ad + Cd * (1 - Ad)
		glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 2) && (alpha.nD == 1))
	{
		if(alpha.nFix == 0x80)
		{
			glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
		}
		else
		{
			//Source alpha value is implied in the formula
			//As = FIX / 0x80
			glBlendColor(0.0f, 0.0f, 0.0f, (float)alpha.nFix / 128.0f);
			glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
		}
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 1))
	{
		glBlendFuncSeparate(BLEND_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 2))
	{
		//Cs * As
		glBlendFuncSeparate(BLEND_SRC_ALPHA, GL_ZERO, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 1) && (alpha.nD == 1))
	{
		//Cs * Ad + Cd
		glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 2) && (alpha.nD == 1))
	{
		if(alpha.nFix == 0x80)
		{
			glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
		}
		else
		{
			//Cs * FIX + Cd
			glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
			glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
		}
	}
	else if((alpha.nA == ALPHABLEND_ABD_CS) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_ZERO))
	{
		//0222 - Cs * FIX
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ZERO, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 1) && (alpha.nB == 0) && (alpha.nC == 0) && (alpha.nD == 0))
	{
		//(Cd - Cs) * As + Cs
		glBlendFuncSeparate(BLEND_ONE_MINUS_SRC_ALPHA, BLEND_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//1001 -> (Cd - Cs) * As + Cd (Inaccurate, needs +1 to As)
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendFuncSeparate(BLEND_SRC_ALPHA, BLEND_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_ZERO))
	{
		//1002 -> (Cd - Cs) * As
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendFuncSeparate(BLEND_SRC_ALPHA, BLEND_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_AD) && (alpha.nD == ALPHABLEND_ABD_CS))
	{
		//1010 -> Cs * (1 - Ad) + Cd * Ad
		glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_CS))
	{
		//1020 -> Cs * (1 - FIX) + Cd * FIX
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_ONE_MINUS_CONSTANT_ALPHA, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//1021 -> (Cd - Cs) * FIX + Cd
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendColor(0.0f, 0.0f, 0.0f, (float)alpha.nFix / 128.0f);
		glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 1) && (alpha.nB == 0) && (alpha.nC == 2) && (alpha.nD == 2))
	{
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendColor(0.0f, 0.0f, 0.0f, (float)alpha.nFix / 128.0f);
		glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 1) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 0))
	{
		//Cd * As + Cs
		glBlendFuncSeparate(GL_ONE, BLEND_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//1201 -> Cd * (As + 1)
		//Relies on colorOutputWhite shader cap
		glBlendFuncSeparate(GL_DST_COLOR, BLEND_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_ZERO))
	{
		//1202 - Cd * As
		glBlendFuncSeparate(GL_ZERO, BLEND_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_AD) && (alpha.nD == ALPHABLEND_ABD_CS))
	{
		//1210 - Cs + (Cd * Ad)
		glBlendFuncSeparate(GL_ONE, GL_DST_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_AD) && (alpha.nD == ALPHABLEND_ABD_ZERO))
	{
		//1212 - Cd * Ad
		glBlendFuncSeparate(GL_ZERO, GL_DST_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_CS))
	{
		//1220 -> Cd * FIX + Cs
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_ONE, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//1221 -> Cd * (1 + FIX)
		//Relies on colorOutputWhite shader cap
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_DST_COLOR, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 1) && (alpha.nB == 2) && (alpha.nC == 2) && (alpha.nD == 2))
	{
		//1222 -> Cd * FIX
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_ZERO, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_ZERO) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CS))
	{
		//2000 -> Cs * (1 - As)
		glBlendFuncSeparate(BLEND_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_ZERO) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//2001 -> Cd - Cs * As
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendFuncSeparate(BLEND_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_ZERO) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_AD) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//2011 -> Cd - Cs * Ad
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_ZERO) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//2021 -> Cd - Cs * FIX
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_ZERO) && (alpha.nB == ALPHABLEND_ABD_CD) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//2101 -> Cd * (1 - As)
		glBlendFuncSeparate(GL_ZERO, BLEND_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_ZERO) && (alpha.nB == ALPHABLEND_ABD_CD) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//2121 -> Cd * (1 - FIX)
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
	}
	else
	{
		assert(0);
		//Default blending
		glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
	}

	glBlendEquationSeparate(nFunction, GL_FUNC_ADD);
}

void CGSH_OpenGL::SetupTestFunctions(uint64 testReg)
{
	auto test = make_convertible<TEST>(testReg);

	m_fragmentParams.alphaRef = test.nAlphaRef;
	m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;

	m_renderState.depthTest = (test.nDepthEnabled != 0) && m_depthTestingEnabled;
	m_validGlState &= ~GLSTATE_DEPTHTEST;

	if(test.nDepthEnabled)
	{
		unsigned int nFunc = GL_NEVER;

		switch(test.nDepthMethod)
		{
		case DEPTH_TEST_NEVER:
			nFunc = GL_NEVER;
			break;
		case DEPTH_TEST_ALWAYS:
			nFunc = GL_ALWAYS;
			break;
		case DEPTH_TEST_GEQUAL:
			nFunc = GL_GEQUAL;
			break;
		case DEPTH_TEST_GREATER:
			nFunc = GL_GREATER;
			break;
		}

		glDepthFunc(nFunc);
	}
}

void CGSH_OpenGL::SetupDepthBuffer(uint64 zbufReg, uint64 testReg)
{
	auto zbuf = make_convertible<ZBUF>(zbufReg);
	auto test = make_convertible<TEST>(testReg);

	bool depthWriteEnabled = (zbuf.nMask ? false : true);
	//If alpha test is enabled for always failing and update only colors, depth writes are disabled
	if(
	    (test.nAlphaEnabled == 1) &&
	    (test.nAlphaMethod == ALPHA_TEST_NEVER) &&
	    ((test.nAlphaFail == ALPHA_TEST_FAIL_FBONLY) || (test.nAlphaFail == ALPHA_TEST_FAIL_RGBONLY)))
	{
		depthWriteEnabled = false;
	}
	m_renderState.depthMask = depthWriteEnabled;
	m_validGlState &= ~GLSTATE_DEPTHMASK;
}

void CGSH_OpenGL::SetupFramebuffer(uint64 frameReg, uint64 zbufReg, uint64 scissorReg, uint64 testReg)
{
	if(frameReg == 0) return;

	auto frame = make_convertible<FRAME>(frameReg);
	auto zbuf = make_convertible<ZBUF>(zbufReg);
	auto scissor = make_convertible<SCISSOR>(scissorReg);
	auto test = make_convertible<TEST>(testReg);

	bool r = (frame.nMask & 0x000000FF) == 0;
	bool g = (frame.nMask & 0x0000FF00) == 0;
	bool b = (frame.nMask & 0x00FF0000) == 0;
	bool a = (frame.nMask & 0xFF000000) == 0;

	//Don't write to alpha in PSMCT24
	if(frame.nPsm == PSMCT24)
	{
		a = false;
	}

	if((test.nAlphaEnabled == 1) && (test.nAlphaMethod == ALPHA_TEST_NEVER))
	{
		if(test.nAlphaFail == ALPHA_TEST_FAIL_RGBONLY)
		{
			a = false;
		}
		else if(test.nAlphaFail == ALPHA_TEST_FAIL_ZBONLY)
		{
			r = g = b = a = false;
		}
	}

	m_renderState.colorMaskR = r;
	m_renderState.colorMaskG = g;
	m_renderState.colorMaskB = b;
	m_renderState.colorMaskA = a;
	m_validGlState &= ~GLSTATE_COLORMASK;

	//Check if we're drawing into a buffer that's been used for depth before
	{
		auto zbufWrite = make_convertible<ZBUF>(frameReg);
		auto depthbuffer = FindDepthbuffer(zbufWrite, frame);
		m_drawingToDepth = (depthbuffer != nullptr);
	}

	//Look for a framebuffer that matches the specified information
	auto framebuffer = FindFramebuffer(frame);
	if(!framebuffer)
	{
		framebuffer = FramebufferPtr(new CFramebuffer(frame.GetBasePtr(), frame.GetWidth(), FRAMEBUFFER_HEIGHT, frame.nPsm, m_fbScale, m_multisampleEnabled));
		m_framebuffers.push_back(framebuffer);
		m_framebuffersVersion++;
		PopulateFramebuffer(framebuffer);
	}

	CommitFramebufferDirtyPages(framebuffer, scissor.scay0, scissor.scay1);

	auto depthbuffer = FindDepthbuffer(zbuf, frame);
	if(!depthbuffer)
	{
		depthbuffer = DepthbufferPtr(new CDepthbuffer(zbuf.GetBasePtr(), frame.GetWidth(), FRAMEBUFFER_HEIGHT, zbuf.nPsm, m_fbScale, m_multisampleEnabled));
		m_depthbuffers.push_back(depthbuffer);
	}

	assert(framebuffer->m_width == depthbuffer->m_width);

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);

	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthbuffer->m_depthBuffer);
	CHECKGLERROR();

	assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	m_renderState.framebufferHandle = framebuffer->m_framebuffer;
	//Draws whose tests always fail write nothing (Colin McRae Rally 3 has a 1024x1024 one over all of GS memory).
	bool writesNothing = ((test.nDepthEnabled != 0) && (test.nDepthMethod == DEPTH_TEST_NEVER)) ||
	                     ((test.nAlphaEnabled != 0) && (test.nAlphaMethod == ALPHA_TEST_NEVER) && (test.nAlphaFail == ALPHA_TEST_FAIL_KEEP));
	m_drawFramebuffer = ((r || g || b || a) && !writesNothing) ? framebuffer.get() : nullptr;
	m_drawFramebufferPtr = framebuffer;
	m_drawMask = (r ? 0x000000FF : 0) | (g ? 0x0000FF00 : 0) | (b ? 0x00FF0000 : 0) | (a ? 0xFF000000 : 0);
	m_validGlState |= GLSTATE_FRAMEBUFFER; //glBindFramebuffer used to set just above

	//We assume that we will be drawing to this framebuffer and that we'll need
	//to resolve samples at some point if multisampling is enabled
	framebuffer->m_resolveNeeded = true;

	{
		GLenum drawBufferId = GL_COLOR_ATTACHMENT0;
		glDrawBuffers(1, &drawBufferId);
		CHECKGLERROR();
	}

	m_renderState.viewportWidth = framebuffer->m_width;
	m_renderState.viewportHeight = framebuffer->m_height;
	m_validGlState &= ~GLSTATE_VIEWPORT;

	float projWidth = static_cast<float>(framebuffer->m_width);
	float projHeight = static_cast<float>(framebuffer->m_height);

	//The GS samples pixel n at n, OpenGL at its centre: move the picture half a (GS) pixel so both sample the
	//same places. Otherwise 1:1 copies sample texel edges and leave lines of wrong texels (Colin McRae Rally 3).
	const float pixelOffset = 0.5f;
	MakeLinearZOrtho(m_vertexParams.projMatrix, -pixelOffset, projWidth - pixelOffset, -pixelOffset, projHeight - pixelOffset);
	m_validGlState &= ~GLSTATE_VERTEX_PARAMS;

	m_renderState.scissorX = scissor.scax0;
	m_renderState.scissorY = scissor.scay0;
	m_renderState.scissorWidth = scissor.scax1 - scissor.scax0 + 1;
	m_renderState.scissorHeight = scissor.scay1 - scissor.scay0 + 1;
	m_validGlState &= ~GLSTATE_SCISSOR;
}

void CGSH_OpenGL::SetupFogColor(uint64 fogColReg)
{
	auto fogCol = make_convertible<FOGCOL>(fogColReg);
	m_fragmentParams.fogColor[0] = static_cast<float>(fogCol.nFCR) / 255.0f;
	m_fragmentParams.fogColor[1] = static_cast<float>(fogCol.nFCG) / 255.0f;
	m_fragmentParams.fogColor[2] = static_cast<float>(fogCol.nFCB) / 255.0f;
	m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;
}

bool CGSH_OpenGL::CanRegionRepeatClampModeSimplified(uint32 clampMin, uint32 clampMax)
{
	for(unsigned int j = 1; j < 0x3FF; j = ((j << 1) | 1))
	{
		if(clampMin < j) break;
		if(clampMin != j) continue;

		if((clampMin & clampMax) != 0) break;

		return true;
	}

	return false;
}

bool CGSH_OpenGL::RequiresAlphaTestDepthTest_DepthFetch(const TEST& test) const
{
	if(!m_hasFramebufferFetchDepthExtension) return false;
	//If alpha test is not enabled, bail
	if(!test.nAlphaEnabled) return false;
	//If depth test not enabled, bail
	if(!test.nDepthEnabled) return false;
	//If depth test doesn't need depth value to compare, bail
	if(!((test.nDepthMethod == DEPTH_TEST_GEQUAL) || (test.nDepthMethod == DEPTH_TEST_GREATER))) return false;
	//If alpha test doesn't discard depth (independently of other channels), bail
	if(!((test.nAlphaFail == ALPHA_TEST_FAIL_FBONLY) || (test.nAlphaFail == ALPHA_TEST_FAIL_RGBONLY))) return false;
	return true;
}

void CGSH_OpenGL::FillShaderCapsFromTexture(SHADERCAPS& shaderCaps, const uint64& tex0Reg, const uint64& tex1Reg, const uint64& texAReg, const uint64& clampReg)
{
	auto tex0 = make_convertible<TEX0>(tex0Reg);
	auto tex1 = make_convertible<TEX1>(tex1Reg);
	auto texA = make_convertible<TEXA>(texAReg);
	auto clamp = make_convertible<CLAMP>(clampReg);

	shaderCaps.texSourceMode = TEXTURE_SOURCE_MODE_STD;

	if((clamp.nWMS >= CLAMP_MODE_CLAMP) || (clamp.nWMT >= CLAMP_MODE_CLAMP))
	{
		unsigned int clampMode[2];

		clampMode[0] = g_shaderClampModes[clamp.nWMS];
		clampMode[1] = g_shaderClampModes[clamp.nWMT];

		if(clampMode[0] == TEXTURE_CLAMP_MODE_REGION_REPEAT && CanRegionRepeatClampModeSimplified(clamp.GetMinU(), clamp.GetMaxU())) clampMode[0] = TEXTURE_CLAMP_MODE_REGION_REPEAT_SIMPLE;
		if(clampMode[1] == TEXTURE_CLAMP_MODE_REGION_REPEAT && CanRegionRepeatClampModeSimplified(clamp.GetMinV(), clamp.GetMaxV())) clampMode[1] = TEXTURE_CLAMP_MODE_REGION_REPEAT_SIMPLE;

		shaderCaps.texClampS = clampMode[0];
		shaderCaps.texClampT = clampMode[1];
	}

	if(CGsPixelFormats::IsPsmIDTEX(tex0.nPsm))
	{
		if((tex1.nMinFilter != MIN_FILTER_NEAREST) || (tex1.nMagFilter != MIN_FILTER_NEAREST))
		{
			//We'll need to filter the texture manually
			shaderCaps.texBilinearFilter = 1;
		}

		if(m_forceBilinearTextures)
		{
			shaderCaps.texBilinearFilter = 1;
		}
	}

	if(tex0.nColorComp == 1)
	{
		shaderCaps.texHasAlpha = 1;
	}

	if((tex0.nPsm == PSMCT16) || (tex0.nPsm == PSMCT16S) || (tex0.nPsm == PSMCT24))
	{
		shaderCaps.texUseAlphaExpansion = 1;
	}

	if(CGsPixelFormats::IsPsmIDTEX(tex0.nPsm))
	{
		if((tex0.nCPSM == PSMCT16) || (tex0.nCPSM == PSMCT16S))
		{
			shaderCaps.texUseAlphaExpansion = 1;
		}

		shaderCaps.texSourceMode = CGsPixelFormats::IsPsmIDTEX4(tex0.nPsm) ? TEXTURE_SOURCE_MODE_IDX4 : TEXTURE_SOURCE_MODE_IDX8;
		shaderCaps.texPaletteFromFramebuffer = (m_gpuClutSource && CGsPixelFormats::IsPsmIDTEX8(tex0.nPsm)) ? 1 : 0;
	}

	if(texA.nAEM)
	{
		shaderCaps.texBlackIsTransparent = 1;
	}

	shaderCaps.texFunction = tex0.nFunction;
}

void CGSH_OpenGL::FillShaderCapsFromTest(SHADERCAPS& shaderCaps, const uint64& testReg)
{
	auto test = make_convertible<TEST>(testReg);

	if(test.nAlphaEnabled)
	{
		//Handle special way of turning off color or depth writes
		//Proper write masks will be set at other places
		if((test.nAlphaMethod == ALPHA_TEST_NEVER) && (test.nAlphaFail != ALPHA_TEST_FAIL_KEEP))
		{
			shaderCaps.hasAlphaTest = 0;
		}
		else
		{
			shaderCaps.hasAlphaTest = m_alphaTestingEnabled ? 1 : 0;
			shaderCaps.alphaTestMethod = test.nAlphaMethod;
			shaderCaps.alphaFailMethod = test.nAlphaFail;
		}

		if(RequiresAlphaTestDepthTest_DepthFetch(test))
		{
			//If we use depthbuffer fetch to reject depth writes on alpha test fail,
			//we need to handle depth test ourselves since we still need to potentially
			//discard the fragment based on its depth value. Since our strategy for rejecting
			//depth writes relies on providing the previous depth value to the current
			//fragment, the built-in depth test won't be able to give proper results.
			shaderCaps.alphaTestDepthTest_DepthFetch = 1;
			shaderCaps.alphaTestDepthTestEqual_DepthFetch = (test.nDepthMethod == DEPTH_TEST_GEQUAL);
		}
	}
	else
	{
		shaderCaps.hasAlphaTest = 0;
	}

	shaderCaps.hasDestAlphaTest = test.nDestAlphaEnabled;
	shaderCaps.destAlphaTestRef = test.nDestAlphaMode;
}

void CGSH_OpenGL::FillShaderCapsFromAlpha(SHADERCAPS& shaderCaps, bool alphaEnabled, const uint64& alphaReg)
{
	if(!alphaEnabled) return;

	auto alpha = make_convertible<ALPHA>(alphaReg);

	if(m_hasFramebufferFetchExtension)
	{
		shaderCaps.hasAlphaBlend = alphaEnabled;
		shaderCaps.alphaBlendA = alpha.nA;
		shaderCaps.alphaBlendB = alpha.nB;
		shaderCaps.alphaBlendC = alpha.nC;
		shaderCaps.alphaBlendD = alpha.nD;
	}
	else
	{
		//If we don't use the source color at all, output white to support some blending modes (ex: ones that doubles dest color).
		shaderCaps.colorOutputWhite = (alpha.nA != ALPHABLEND_ABD_CS) && (alpha.nB != ALPHABLEND_ABD_CS) && (alpha.nD != ALPHABLEND_ABD_CS);
	}
}

void CGSH_OpenGL::SetupTexture(uint64 primReg, uint64 tex0Reg, uint64 tex1Reg, uint64 texAReg, uint64 clampReg)
{
	m_renderState.texture0Handle = 0;
	m_renderState.texture1Handle = 0;
	m_renderState.texture0MinFilter = GL_NEAREST;
	m_renderState.texture0MagFilter = GL_NEAREST;
	m_renderState.texture0WrapS = GL_CLAMP_TO_EDGE;
	m_renderState.texture0WrapT = GL_CLAMP_TO_EDGE;
	m_validGlState &= ~GLSTATE_TEXTURE;

	auto prim = make_convertible<PRMODE>(primReg);

	if(tex0Reg == 0 || prim.nTexture == 0)
	{
		return;
	}

	auto tex0 = make_convertible<TEX0>(tex0Reg);
	auto tex1 = make_convertible<TEX1>(tex1Reg);
	auto texA = make_convertible<TEXA>(texAReg);
	auto clamp = make_convertible<CLAMP>(clampReg);

	m_nTexWidth = tex0.GetWidth();
	m_nTexHeight = tex0.GetHeight();

	auto texInfo = PrepareTexture(tex0);
	m_renderState.texture0Handle = texInfo.textureHandle;
	m_renderState.texture0AlphaAsIndex = texInfo.alphaAsIndex;

	//Setup sampling modes
	switch(tex1.nMagFilter)
	{
	case MAG_FILTER_NEAREST:
		m_renderState.texture0MagFilter = GL_NEAREST;
		break;
	case MAG_FILTER_LINEAR:
		m_renderState.texture0MagFilter = GL_LINEAR;
		break;
	}

	switch(tex1.nMinFilter)
	{
	case MIN_FILTER_NEAREST:
	case MIN_FILTER_NEAREST_MIP_NEAREST:
	case MIN_FILTER_NEAREST_MIP_LINEAR:
		m_renderState.texture0MinFilter = GL_NEAREST;
		break;
	case MIN_FILTER_LINEAR:
	case MIN_FILTER_LINEAR_MIP_NEAREST:
	case MIN_FILTER_LINEAR_MIP_LINEAR:
		m_renderState.texture0MinFilter = GL_LINEAR;
		break;
	default:
		assert(0);
		break;
	}

	if(m_forceBilinearTextures)
	{
		m_renderState.texture0MagFilter = GL_LINEAR;
		m_renderState.texture0MinFilter = GL_LINEAR;
	}

	unsigned int clampMin[2] = {0, 0};
	unsigned int clampMax[2] = {0, 0};
	float textureScaleRatio[2] = {texInfo.scaleRatioX, texInfo.scaleRatioY};
	m_renderState.texture0WrapS = g_nativeClampModes[clamp.nWMS];
	m_renderState.texture0WrapT = g_nativeClampModes[clamp.nWMT];

	if((clamp.nWMS >= CLAMP_MODE_CLAMP) || (clamp.nWMT >= CLAMP_MODE_CLAMP))
	{
		unsigned int clampMode[2];

		clampMode[0] = g_shaderClampModes[clamp.nWMS];
		clampMode[1] = g_shaderClampModes[clamp.nWMT];

		clampMin[0] = clamp.GetMinU();
		clampMin[1] = clamp.GetMinV();

		clampMax[0] = clamp.GetMaxU();
		clampMax[1] = clamp.GetMaxV();

		for(unsigned int i = 0; i < 2; i++)
		{
			if(clampMode[i] == TEXTURE_CLAMP_MODE_CLAMP)
			{
				uint32 size = (i == 0) ? tex0.GetWidth() : tex0.GetHeight();
				clampMin[i] = 0;
				clampMax[i] = static_cast<unsigned int>(static_cast<float>(size - 1) * textureScaleRatio[i]);
			}
			else if(clampMode[i] == TEXTURE_CLAMP_MODE_REGION_REPEAT)
			{
				if(CanRegionRepeatClampModeSimplified(clampMin[i], clampMax[i]))
				{
					clampMin[i]++;
				}
			}
			else if(clampMode[i] == TEXTURE_CLAMP_MODE_REGION_CLAMP)
			{
				clampMin[i] = static_cast<unsigned int>(static_cast<float>(clampMin[i]) * textureScaleRatio[i]);
				clampMax[i] = static_cast<unsigned int>(static_cast<float>(clampMax[i]) * textureScaleRatio[i]);
			}
		}
	}

	if(CGsPixelFormats::IsPsmIDTEX(tex0.nPsm) &&
	   (m_renderState.texture0MinFilter != GL_NEAREST || m_renderState.texture0MagFilter != GL_NEAREST))
	{
		//We'll need to filter the texture manually
		m_renderState.texture0MinFilter = GL_NEAREST;
		m_renderState.texture0MagFilter = GL_NEAREST;
	}

	if(CGsPixelFormats::IsPsmIDTEX(tex0.nPsm))
	{
		m_renderState.texture1Handle = PreparePalette(tex0);
	}

	memset(m_vertexParams.texMatrix, 0, sizeof(m_vertexParams.texMatrix));
	m_vertexParams.texMatrix[0 + (0 * 4)] = texInfo.scaleRatioX;
	m_vertexParams.texMatrix[1 + (1 * 4)] = texInfo.scaleRatioY;
	m_vertexParams.texMatrix[2 + (2 * 4)] = 1;
	m_vertexParams.texMatrix[0 + (3 * 4)] = texInfo.offsetX;
	m_vertexParams.texMatrix[3 + (3 * 4)] = 1;
	m_validGlState &= ~GLSTATE_VERTEX_PARAMS;

	m_fragmentParams.textureSize[0] = static_cast<float>(tex0.GetWidth());
	m_fragmentParams.textureSize[1] = static_cast<float>(tex0.GetHeight());
	m_fragmentParams.texelSize[0] = 1.0f / static_cast<float>(tex0.GetWidth());
	m_fragmentParams.texelSize[1] = 1.0f / static_cast<float>(tex0.GetHeight());
	m_fragmentParams.clampMin[0] = static_cast<float>(clampMin[0]);
	m_fragmentParams.clampMin[1] = static_cast<float>(clampMin[1]);
	m_fragmentParams.clampMax[0] = static_cast<float>(clampMax[0]);
	m_fragmentParams.clampMax[1] = static_cast<float>(clampMax[1]);
	m_fragmentParams.texA0 = static_cast<float>(texA.nTA0) / 255.f;
	m_fragmentParams.texA1 = static_cast<float>(texA.nTA1) / 255.f;
	m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;
}

CGSH_OpenGL::FramebufferPtr CGSH_OpenGL::FindFramebuffer(const FRAME& frame) const
{
	auto framebufferIterator = std::find_if(std::begin(m_framebuffers), std::end(m_framebuffers),
	                                        [&](const FramebufferPtr& framebuffer) {
		                                        return (framebuffer->m_basePtr == frame.GetBasePtr()) &&
		                                               IsCompatibleFramebufferPSM(framebuffer->m_psm, frame.nPsm) &&
		                                               (framebuffer->m_width == frame.GetWidth());
	                                        });

	return (framebufferIterator != std::end(m_framebuffers)) ? *(framebufferIterator) : FramebufferPtr();
}

CGSH_OpenGL::DepthbufferPtr CGSH_OpenGL::FindDepthbuffer(const ZBUF& zbuf, const FRAME& frame) const
{
	auto depthbufferIterator = std::find_if(std::begin(m_depthbuffers), std::end(m_depthbuffers),
	                                        [&](const DepthbufferPtr& depthbuffer) {
		                                        return (depthbuffer->m_basePtr == zbuf.GetBasePtr()) && (depthbuffer->m_width == frame.GetWidth());
	                                        });

	return (depthbufferIterator != std::end(m_depthbuffers)) ? *(depthbufferIterator) : DepthbufferPtr();
}

/////////////////////////////////////////////////////////////
// Individual Primitives Implementations
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::Prim_Point()
{
	auto xyz = make_convertible<XYZ>(m_VtxBuffer[0].position);
	auto rgbaq = make_convertible<RGBAQ>(m_VtxBuffer[0].rgbaq);

	float x = xyz.GetX();
	float y = xyz.GetY();
	uint32 z = xyz.nZ;

	x -= m_nPrimOfsX;
	y -= m_nPrimOfsY;

	auto color = MakeColor(
	    rgbaq.nR, rgbaq.nG,
	    rgbaq.nB, rgbaq.nA);

	// clang-format off
	PRIM_VERTEX vertex =
	{
		//x, y, z, color, s, t, q, f
		  x, y, z, color, 0, 0, 1, 0,
	};
	// clang-format on

	m_vertexBuffer.push_back(vertex);
}

void CGSH_OpenGL::Prim_Line()
{
	XYZ xyz[2];
	xyz[0] <<= m_VtxBuffer[1].position;
	xyz[1] <<= m_VtxBuffer[0].position;

	float nX1 = xyz[0].GetX();
	float nY1 = xyz[0].GetY();
	uint32 nZ1 = xyz[0].nZ;
	float nX2 = xyz[1].GetX();
	float nY2 = xyz[1].GetY();
	uint32 nZ2 = xyz[1].nZ;

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;

	RGBAQ rgbaq[2];
	rgbaq[0] <<= m_VtxBuffer[1].rgbaq;
	rgbaq[1] <<= m_VtxBuffer[0].rgbaq;

	float nS[2] = {0, 0};
	float nT[2] = {0, 0};
	float nQ[2] = {1, 1};

	if(m_PrimitiveMode.nTexture)
	{
		if(m_PrimitiveMode.nUseUV)
		{
			UV uv[2];
			uv[0] <<= m_VtxBuffer[1].uv;
			uv[1] <<= m_VtxBuffer[0].uv;

			nS[0] = uv[0].GetU() / static_cast<float>(m_nTexWidth);
			nS[1] = uv[1].GetU() / static_cast<float>(m_nTexWidth);

			nT[0] = uv[0].GetV() / static_cast<float>(m_nTexHeight);
			nT[1] = uv[1].GetV() / static_cast<float>(m_nTexHeight);
		}
		else
		{
			ST st[2];
			st[0] <<= m_VtxBuffer[1].st;
			st[1] <<= m_VtxBuffer[0].st;

			nS[0] = st[0].nS;
			nS[1] = st[1].nS;
			nT[0] = st[0].nT;
			nT[1] = st[1].nT;

			bool isQ0Neg = (rgbaq[0].nQ < 0);
			bool isQ1Neg = (rgbaq[1].nQ < 0);

			assert(isQ0Neg == isQ1Neg);

			nQ[0] = rgbaq[0].nQ;
			nQ[1] = rgbaq[1].nQ;
		}
	}

	auto color1 = MakeColor(
	    rgbaq[0].nR, rgbaq[0].nG,
	    rgbaq[0].nB, rgbaq[0].nA);

	auto color2 = MakeColor(
	    rgbaq[1].nR, rgbaq[1].nG,
	    rgbaq[1].nB, rgbaq[1].nA);

	// clang-format off
	PRIM_VERTEX vertices[] =
	{
		{	nX1,	nY1,	nZ1,	color1,	nS[0],	nT[0],	nQ[0],	0	},
		{	nX2,	nY2,	nZ2,	color2,	nS[1],	nT[1],	nQ[1],	0	},
	};
	// clang-format on

	m_vertexBuffer.insert(m_vertexBuffer.end(), std::begin(vertices), std::end(vertices));
}

void CGSH_OpenGL::Prim_Triangle()
{
	float nF1, nF2, nF3;

	XYZ vertex[3];
	vertex[0] <<= m_VtxBuffer[2].position;
	vertex[1] <<= m_VtxBuffer[1].position;
	vertex[2] <<= m_VtxBuffer[0].position;

	float nX1 = vertex[0].GetX(), nX2 = vertex[1].GetX(), nX3 = vertex[2].GetX();
	float nY1 = vertex[0].GetY(), nY2 = vertex[1].GetY(), nY3 = vertex[2].GetY();
	uint32 nZ1 = vertex[0].nZ, nZ2 = vertex[1].nZ, nZ3 = vertex[2].nZ;

	RGBAQ rgbaq[3];
	rgbaq[0] <<= m_VtxBuffer[2].rgbaq;
	rgbaq[1] <<= m_VtxBuffer[1].rgbaq;
	rgbaq[2] <<= m_VtxBuffer[0].rgbaq;

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;
	nX3 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;
	nY3 -= m_nPrimOfsY;

	if(m_PrimitiveMode.nFog)
	{
		nF1 = (float)(0xFF - m_VtxBuffer[2].fog) / 255.0f;
		nF2 = (float)(0xFF - m_VtxBuffer[1].fog) / 255.0f;
		nF3 = (float)(0xFF - m_VtxBuffer[0].fog) / 255.0f;
	}
	else
	{
		nF1 = nF2 = nF3 = 0.0;
	}

	float nS[3] = {0, 0, 0};
	float nT[3] = {0, 0, 0};
	float nQ[3] = {1, 1, 1};

	if(m_PrimitiveMode.nTexture)
	{
		if(m_PrimitiveMode.nUseUV)
		{
			UV uv[3];
			uv[0] <<= m_VtxBuffer[2].uv;
			uv[1] <<= m_VtxBuffer[1].uv;
			uv[2] <<= m_VtxBuffer[0].uv;

			nS[0] = uv[0].GetU() / static_cast<float>(m_nTexWidth);
			nS[1] = uv[1].GetU() / static_cast<float>(m_nTexWidth);
			nS[2] = uv[2].GetU() / static_cast<float>(m_nTexWidth);

			nT[0] = uv[0].GetV() / static_cast<float>(m_nTexHeight);
			nT[1] = uv[1].GetV() / static_cast<float>(m_nTexHeight);
			nT[2] = uv[2].GetV() / static_cast<float>(m_nTexHeight);
		}
		else
		{
			ST st[3];
			st[0] <<= m_VtxBuffer[2].st;
			st[1] <<= m_VtxBuffer[1].st;
			st[2] <<= m_VtxBuffer[0].st;

			nS[0] = st[0].nS;
			nS[1] = st[1].nS;
			nS[2] = st[2].nS;
			nT[0] = st[0].nT;
			nT[1] = st[1].nT;
			nT[2] = st[2].nT;

			bool isQ0Neg = (rgbaq[0].nQ < 0);
			bool isQ1Neg = (rgbaq[1].nQ < 0);
			bool isQ2Neg = (rgbaq[2].nQ < 0);

			assert(isQ0Neg == isQ1Neg);
			assert(isQ1Neg == isQ2Neg);

			nQ[0] = rgbaq[0].nQ;
			nQ[1] = rgbaq[1].nQ;
			nQ[2] = rgbaq[2].nQ;
		}
	}

	auto color1 = MakeColor(
	    rgbaq[0].nR, rgbaq[0].nG,
	    rgbaq[0].nB, rgbaq[0].nA);

	auto color2 = MakeColor(
	    rgbaq[1].nR, rgbaq[1].nG,
	    rgbaq[1].nB, rgbaq[1].nA);

	auto color3 = MakeColor(
	    rgbaq[2].nR, rgbaq[2].nG,
	    rgbaq[2].nB, rgbaq[2].nA);

	if(m_PrimitiveMode.nShading == 0)
	{
		//Flat shaded triangles use the last color set
		color1 = color2 = color3;
	}

	// clang-format off
	PRIM_VERTEX vertices[] =
	{
		{	nX1,	nY1,	nZ1,	color1,	nS[0],	nT[0],	nQ[0],	nF1	},
		{	nX2,	nY2,	nZ2,	color2,	nS[1],	nT[1],	nQ[1],	nF2	},
		{	nX3,	nY3,	nZ3,	color3,	nS[2],	nT[2],	nQ[2],	nF3	},
	};
	// clang-format on

	m_vertexBuffer.insert(m_vertexBuffer.end(), std::begin(vertices), std::end(vertices));
}

void CGSH_OpenGL::Prim_Sprite()
{
	XYZ xyz[2];
	xyz[0] <<= m_VtxBuffer[1].position;
	xyz[1] <<= m_VtxBuffer[0].position;

	float nX1 = xyz[0].GetX();
	float nY1 = xyz[0].GetY();
	float nX2 = xyz[1].GetX();
	float nY2 = xyz[1].GetY();
	uint32 nZ = xyz[1].nZ;

	RGBAQ rgbaq[2];
	rgbaq[0] <<= m_VtxBuffer[1].rgbaq;
	rgbaq[1] <<= m_VtxBuffer[0].rgbaq;

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;

	float nS[2] = {0, 0};
	float nT[2] = {0, 0};

	float nF = 0;

	if(m_PrimitiveMode.nFog)
	{
		nF = (float)(0xFF - m_VtxBuffer[0].fog) / 255.0f;
	}

	if(m_PrimitiveMode.nTexture)
	{
		if(m_PrimitiveMode.nUseUV)
		{
			UV uv[2];
			uv[0] <<= m_VtxBuffer[1].uv;
			uv[1] <<= m_VtxBuffer[0].uv;

			nS[0] = uv[0].GetU() / static_cast<float>(m_nTexWidth);
			nS[1] = uv[1].GetU() / static_cast<float>(m_nTexWidth);

			nT[0] = uv[0].GetV() / static_cast<float>(m_nTexHeight);
			nT[1] = uv[1].GetV() / static_cast<float>(m_nTexHeight);
		}
		else
		{
			ST st[2];

			st[0] <<= m_VtxBuffer[1].st;
			st[1] <<= m_VtxBuffer[0].st;

			float nQ1 = rgbaq[1].nQ;
			float nQ2 = rgbaq[0].nQ;
			if(nQ1 == 0) nQ1 = 1;
			if(nQ2 == 0) nQ2 = 1;

			nS[0] = st[0].nS / nQ1;
			nS[1] = st[1].nS / nQ2;

			nT[0] = st[0].nT / nQ1;
			nT[1] = st[1].nT / nQ2;
		}
	}

	auto color = MakeColor(
	    rgbaq[1].nR, rgbaq[1].nG,
	    rgbaq[1].nB, rgbaq[1].nA);

	// clang-format off
	PRIM_VERTEX vertices[] =
	{
		{nX1, nY1, nZ, color, nS[0], nT[0], 1, nF},
		{nX2, nY1, nZ, color, nS[1], nT[0], 1, nF},
		{nX1, nY2, nZ, color, nS[0], nT[1], 1, nF},

		{nX1, nY2, nZ, color, nS[0], nT[1], 1, nF},
		{nX2, nY1, nZ, color, nS[1], nT[0], 1, nF},
		{nX2, nY2, nZ, color, nS[1], nT[1], 1, nF},
	};
	// clang-format on

	m_vertexBuffer.insert(m_vertexBuffer.end(), std::begin(vertices), std::end(vertices));
}

void CGSH_OpenGL::FlushVertexBuffer()
{
	if(m_vertexBuffer.empty()) return;

	assert(m_renderState.isValid == true);

	auto shader = GetShaderFromCaps(m_renderState.shaderCaps);
	if(*shader != m_renderState.shaderHandle)
	{
		m_renderState.shaderHandle = *shader;
		m_validGlState &= ~GLSTATE_PROGRAM;
	}
	MarkGpuPages();
	DoRenderPass();
	m_vertexBuffer.clear();
}

void CGSH_OpenGL::DoRenderPass()
{
	if((m_validGlState & GLSTATE_VERTEX_PARAMS) == 0)
	{
		m_vertexParamsOffset = StreamData(m_vertexParamsBuffer, m_vertexParamsStream, GL_UNIFORM_BUFFER,
		                                  &m_vertexParams, sizeof(VERTEXPARAMS), m_uniformBufferAlignment);
		glBindBufferRange(GL_UNIFORM_BUFFER, 0, m_vertexParamsBuffer, m_vertexParamsOffset, sizeof(VERTEXPARAMS));
		CHECKGLERROR();
		m_validGlState |= GLSTATE_VERTEX_PARAMS;
	}

	if((m_validGlState & GLSTATE_FRAGMENT_PARAMS) == 0)
	{
		m_fragmentParamsOffset = StreamData(m_fragmentParamsBuffer, m_fragmentParamsStream, GL_UNIFORM_BUFFER,
		                                    &m_fragmentParams, sizeof(FRAGMENTPARAMS), m_uniformBufferAlignment);
		glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_fragmentParamsBuffer, m_fragmentParamsOffset, sizeof(FRAGMENTPARAMS));
		CHECKGLERROR();
		m_validGlState |= GLSTATE_FRAGMENT_PARAMS;
	}

	if((m_validGlState & GLSTATE_PROGRAM) == 0)
	{
		glUseProgram(m_renderState.shaderHandle);
		m_validGlState |= GLSTATE_PROGRAM;
	}

	if((m_validGlState & GLSTATE_VIEWPORT) == 0)
	{
		glViewport(0, 0, m_renderState.viewportWidth * m_fbScale, m_renderState.viewportHeight * m_fbScale);
		m_validGlState |= GLSTATE_VIEWPORT;
	}

	if((m_validGlState & GLSTATE_SCISSOR) == 0)
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(m_renderState.scissorX * m_fbScale, m_renderState.scissorY * m_fbScale,
		          m_renderState.scissorWidth * m_fbScale, m_renderState.scissorHeight * m_fbScale);
		m_validGlState |= GLSTATE_SCISSOR;
	}

	if((m_validGlState & GLSTATE_BLEND) == 0)
	{
		m_renderState.blendEnabled ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
		m_validGlState |= GLSTATE_BLEND;
	}

	if((m_validGlState & GLSTATE_DEPTHTEST) == 0)
	{
		m_renderState.depthTest ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
		m_validGlState |= GLSTATE_DEPTHTEST;
	}

	if((m_validGlState & GLSTATE_COLORMASK) == 0)
	{
		glColorMask(
		    m_renderState.colorMaskR, m_renderState.colorMaskG,
		    m_renderState.colorMaskB, m_renderState.colorMaskA);
		m_validGlState |= GLSTATE_COLORMASK;
	}

	if((m_validGlState & GLSTATE_DEPTHMASK) == 0)
	{
		glDepthMask(m_renderState.depthMask ? GL_TRUE : GL_FALSE);
		m_validGlState |= GLSTATE_DEPTHMASK;
	}

	if((m_validGlState & GLSTATE_TEXTURE) == 0)
	{
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_renderState.texture0Handle);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_renderState.texture0MinFilter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_renderState.texture0MagFilter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, m_renderState.texture0WrapS);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, m_renderState.texture0WrapT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, m_renderState.texture0AlphaAsIndex ? GL_ALPHA : GL_RED);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m_renderState.texture1Handle);
		if(m_renderState.shaderCaps.texPaletteFromFramebuffer)
		{
			//A framebuffer's texture keeps the swizzle of its last use as an alpha-indexed texture.
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
		}

		m_validGlState |= GLSTATE_TEXTURE;
	}

	if((m_validGlState & GLSTATE_FRAMEBUFFER) == 0)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_renderState.framebufferHandle);
		m_validGlState |= GLSTATE_FRAMEBUFFER;
	}

	auto vertexOffset = StreamData(m_primBuffer, m_primStream, GL_ARRAY_BUFFER,
	                               m_vertexBuffer.data(), sizeof(PRIM_VERTEX) * m_vertexBuffer.size(), sizeof(PRIM_VERTEX));

	glBindVertexArray(m_primVertexArray);

	GLenum primitiveMode = GL_NONE;
	switch(m_primitiveType)
	{
	case PRIM_POINT:
		primitiveMode = GL_POINTS;
		break;
	case PRIM_LINE:
	case PRIM_LINESTRIP:
		primitiveMode = GL_LINES;
		break;
	case PRIM_TRIANGLE:
	case PRIM_TRIANGLESTRIP:
	case PRIM_TRIANGLEFAN:
	case PRIM_SPRITE:
		primitiveMode = GL_TRIANGLES;
		break;
	default:
		assert(false);
		break;
	}

	glDrawArrays(primitiveMode, vertexOffset / sizeof(PRIM_VERTEX), m_vertexBuffer.size());

	m_drawCallCount++;
}

void CGSH_OpenGL::CreateStreamBuffer(GLuint buffer, STREAM_BUFFER& stream, GLenum target, GLsizeiptr size)
{
	ReleaseStreamBuffer(stream);
	stream = STREAM_BUFFER();
#ifdef __ANDROID__
	static auto bufferStorage = reinterpret_cast<PFNGLBUFFERSTORAGEEXTPROC>(eglGetProcAddress("glBufferStorageEXT"));
	if(!m_hasBufferStorageExtension || !bufferStorage) return;
	const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
	glBindBuffer(target, buffer);
	bufferStorage(target, size, nullptr, flags);
	stream.mapped = reinterpret_cast<uint8*>(glMapBufferRange(target, 0, size, flags));
	if(stream.mapped) stream.size = size;
#endif
}

void CGSH_OpenGL::UploadCvtBuffer(unsigned int texX, unsigned int texY, unsigned int texWidth, unsigned int texHeight,
                                  GLenum format, GLenum type, unsigned int bytesPerPixel)
{
	//Uploading from client memory makes some drivers (Adreno) allocate and map new memory for every update.
	GLsizeiptr size = ((texWidth * bytesPerPixel + 3) & ~3) * texHeight;
	if(!m_unpackStream.mapped || (size > (m_unpackStream.size / STREAM_SEGMENTS)))
	{
		glTexSubImage2D(GL_TEXTURE_2D, 0, texX, texY, texWidth, texHeight, format, type, m_pCvtBuffer);
		return;
	}
	auto offset = StreamData(m_unpackBuffer, m_unpackStream, GL_PIXEL_UNPACK_BUFFER, m_pCvtBuffer, size, 16);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, m_unpackBuffer);
	glTexSubImage2D(GL_TEXTURE_2D, 0, texX, texY, texWidth, texHeight, format, type, reinterpret_cast<const void*>(offset));
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void CGSH_OpenGL::ReleaseStreamBuffer(STREAM_BUFFER& stream)
{
	for(auto& fence : stream.fences)
	{
		if(fence) glDeleteSync(fence);
		fence = nullptr;
	}
}

GLintptr CGSH_OpenGL::StreamData(GLuint buffer, STREAM_BUFFER& stream, GLenum target, const void* data, GLsizeiptr size, GLsizeiptr alignment)
{
	if(!stream.mapped || (size > (stream.size / STREAM_SEGMENTS)))
	{
		glBindBuffer(target, buffer);
		glBufferData(target, size, data, GL_STREAM_DRAW);
		return 0;
	}
	GLintptr offset = ((stream.offset + alignment - 1) / alignment) * alignment;
	if((offset + size) > stream.size) offset = 0;
	//Before writing into a segment, wait until the GPU is done with what was written there on the previous lap.
	auto segmentSize = stream.size / STREAM_SEGMENTS;
	unsigned int lastSegment = (offset + size - 1) / segmentSize;
	while(stream.segment != lastSegment)
	{
		stream.fences[stream.segment] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		stream.segment = (stream.segment + 1) % STREAM_SEGMENTS;
		if(auto& fence = stream.fences[stream.segment])
		{
			while(glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000) == GL_TIMEOUT_EXPIRED)
			{
			}
			glDeleteSync(fence);
			fence = nullptr;
		}
	}
	memcpy(stream.mapped + offset, data, size);
	stream.offset = offset + size;
	return offset;
}

void CGSH_OpenGL::DrawToDepth(unsigned int primitiveType, uint64 primReg)
{
	//A game might be attempting to clear depth by using the zbuffer
	//as a frame buffer and drawing a black sprite into it
	//Space Harrier does this

	//Must be flat, no texture map, no fog, no blend and no aa
	if((primReg & 0x1F8) != 0) return;

	//Must be a sprite
	if(primitiveType != PRIM_SPRITE) return;

	//Invalidate state
	FlushVertexBuffer();
	m_renderState.isValid = false;

	auto prim = make_convertible<PRMODE>(primReg);

	uint64 frameReg = m_nReg[GS_REG_FRAME_1 + prim.nContext];

	auto frame = make_convertible<FRAME>(frameReg);
	auto zbufWrite = make_convertible<ZBUF>(frameReg);

	auto depthbuffer = FindDepthbuffer(zbufWrite, frame);
	assert(depthbuffer);

	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthbuffer->m_depthBuffer);
	CHECKGLERROR();

	assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	glDepthMask(GL_TRUE);
	glClearDepthf(0);
	glClear(GL_DEPTH_BUFFER_BIT);

	m_validGlState &= ~GLSTATE_DEPTHMASK;
}

void CGSH_OpenGL::CopyToFb(
    int32 srcX0, int32 srcY0, int32 srcX1, int32 srcY1,
    int32 srcWidth, int32 srcHeight,
    int32 dstX0, int32 dstY0, int32 dstX1, int32 dstY1, bool copyAlpha)
{
	m_validGlState &= ~(GLSTATE_BLEND | GLSTATE_COLORMASK | GLSTATE_SCISSOR | GLSTATE_PROGRAM);
	m_validGlState &= ~(GLSTATE_VIEWPORT | GLSTATE_DEPTHTEST | GLSTATE_DEPTHMASK);

	assert(srcX1 >= srcX0);
	assert(srcY1 >= srcY0);
	assert(dstX1 >= dstX0);
	assert(dstY1 >= dstY0);

	float positionX = static_cast<float>(srcX0) / static_cast<float>(srcWidth);
	float positionY = static_cast<float>(srcY0) / static_cast<float>(srcHeight);
	float sizeX = static_cast<float>(srcX1 - srcX0) / static_cast<float>(srcWidth);
	float sizeY = static_cast<float>(srcY1 - srcY0) / static_cast<float>(srcHeight);

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, copyAlpha ? GL_TRUE : GL_FALSE);
	glDepthMask(GL_FALSE);

	glUseProgram(*m_copyToFbProgram);

	glUniform2f(m_copyToFbSrcPositionUniform, positionX, positionY);
	glUniform2f(m_copyToFbSrcSizeUniform, sizeX, sizeY);
	glViewport(dstX0, dstY0, dstX1 - dstX0, dstY1 - dstY0);

	glBindBuffer(GL_ARRAY_BUFFER, m_copyToFbVertexBuffer);
	glBindVertexArray(m_copyToFbVertexArray);

#ifdef _DEBUG
	m_copyToFbProgram->Validate();
#endif

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	CHECKGLERROR();
}

/////////////////////////////////////////////////////////////
// Other Functions
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::WriteRegisterImpl(uint8 nRegister, uint64 nData)
{
	CGSHandler::WriteRegisterImpl(nRegister, nData);

	switch(nRegister)
	{
	case GS_REG_PRIM:
		m_pendingPrim = true;
		m_pendingPrimValue = nData;
		break;

	case GS_REG_XYZ2:
	case GS_REG_XYZ3:
	case GS_REG_XYZF2:
	case GS_REG_XYZF3:
		VertexKick(nRegister, nData);
		break;
	}
}

void CGSH_OpenGL::ProcessPrim(uint64 value)
{
	unsigned int newPrimitiveType = static_cast<unsigned int>(value & 0x07);
	if(newPrimitiveType != m_primitiveType)
	{
		FlushVertexBuffer();
	}
	m_primitiveType = newPrimitiveType;
	switch(m_primitiveType)
	{
	case PRIM_POINT:
		m_nVtxCount = 1;
		break;
	case PRIM_LINE:
	case PRIM_LINESTRIP:
		m_nVtxCount = 2;
		break;
	case PRIM_TRIANGLE:
	case PRIM_TRIANGLESTRIP:
	case PRIM_TRIANGLEFAN:
		m_nVtxCount = 3;
		break;
	case PRIM_SPRITE:
		m_nVtxCount = 2;
		break;
	}
}

void CGSH_OpenGL::VertexKick(uint8 nRegister, uint64 nValue)
{
	if(m_pendingPrim)
	{
		m_pendingPrim = false;
		ProcessPrim(m_pendingPrimValue);
	}

	if(m_nVtxCount == 0) return;

	bool nDrawingKick = (nRegister == GS_REG_XYZ2) || (nRegister == GS_REG_XYZF2);
	bool nFog = (nRegister == GS_REG_XYZF2) || (nRegister == GS_REG_XYZF3);

	if(!m_drawEnabled) nDrawingKick = false;

	if(nFog)
	{
		m_VtxBuffer[m_nVtxCount - 1].position = nValue & 0x00FFFFFFFFFFFFFFULL;
		m_VtxBuffer[m_nVtxCount - 1].rgbaq = m_nReg[GS_REG_RGBAQ];
		m_VtxBuffer[m_nVtxCount - 1].uv = m_nReg[GS_REG_UV];
		m_VtxBuffer[m_nVtxCount - 1].st = m_nReg[GS_REG_ST];
		m_VtxBuffer[m_nVtxCount - 1].fog = (uint8)(nValue >> 56);
	}
	else
	{
		m_VtxBuffer[m_nVtxCount - 1].position = nValue;
		m_VtxBuffer[m_nVtxCount - 1].rgbaq = m_nReg[GS_REG_RGBAQ];
		m_VtxBuffer[m_nVtxCount - 1].uv = m_nReg[GS_REG_UV];
		m_VtxBuffer[m_nVtxCount - 1].st = m_nReg[GS_REG_ST];
		m_VtxBuffer[m_nVtxCount - 1].fog = (uint8)(m_nReg[GS_REG_FOG] >> 56);
	}

	m_nVtxCount--;

	if(m_nVtxCount == 0)
	{
		if((m_nReg[GS_REG_PRMODECONT] & 1) != 0)
		{
			m_PrimitiveMode <<= m_nReg[GS_REG_PRIM];
		}
		else
		{
			m_PrimitiveMode <<= m_nReg[GS_REG_PRMODE];
		}

		if(nDrawingKick)
		{
			// Fast skip if nothing relevant changed since last draw
			bool needCtx = true;
			if(m_renderState.isValid && m_renderState.isTextureStateValid && m_renderState.isFramebufferStateValid)
			{
				uint64 primReg = m_PrimitiveMode;
				auto primLocal = make_convertible<PRMODE>(primReg);
				unsigned ctx = primLocal.nContext;
				uint64 testReg = m_nReg[GS_REG_TEST_1 + ctx];
				uint64 frameReg = m_nReg[GS_REG_FRAME_1 + ctx];
				uint64 alphaReg = m_nReg[GS_REG_ALPHA_1 + ctx];
				uint64 zbufReg = m_nReg[GS_REG_ZBUF_1 + ctx];
				uint64 tex0Reg = m_nReg[GS_REG_TEX0_1 + ctx];
				uint64 tex1Reg = m_nReg[GS_REG_TEX1_1 + ctx];
				uint64 texAReg = m_nReg[GS_REG_TEXA];
				uint64 clampReg = m_nReg[GS_REG_CLAMP_1 + ctx];
				uint64 fogColReg = m_nReg[GS_REG_FOGCOL];
				uint64 scissorReg = m_nReg[GS_REG_SCISSOR_1 + ctx];
				if(m_renderState.primReg == primReg &&
				   m_renderState.testReg == testReg &&
				   m_renderState.frameReg == frameReg &&
				   m_renderState.alphaReg == alphaReg &&
				   m_renderState.zbufReg == zbufReg &&
				   m_renderState.tex0Reg == tex0Reg &&
				   m_renderState.tex1Reg == tex1Reg &&
				   m_renderState.texAReg == texAReg &&
				   m_renderState.clampReg == clampReg &&
				   m_renderState.fogColReg == fogColReg &&
				   m_renderState.scissorReg == scissorReg)
				{
					needCtx = false; // fully same state; skip call
				}
			}
			if(needCtx) SetRenderingContext(m_PrimitiveMode);
		}

		switch(m_primitiveType)
		{
		case PRIM_POINT:
			if(nDrawingKick) Prim_Point();
			m_nVtxCount = 1;
			break;
		case PRIM_LINE:
			if(nDrawingKick) Prim_Line();
			m_nVtxCount = 2;
			break;
		case PRIM_LINESTRIP:
			if(nDrawingKick) Prim_Line();
			memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
			m_nVtxCount = 1;
			break;
		case PRIM_TRIANGLE:
			if(nDrawingKick) Prim_Triangle();
			m_nVtxCount = 3;
			break;
		case PRIM_TRIANGLESTRIP:
			if(nDrawingKick) Prim_Triangle();
			memcpy(&m_VtxBuffer[2], &m_VtxBuffer[1], sizeof(VERTEX));
			memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
			m_nVtxCount = 1;
			break;
		case PRIM_TRIANGLEFAN:
			if(nDrawingKick) Prim_Triangle();
			memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
			m_nVtxCount = 1;
			break;
		case PRIM_SPRITE:
			if(nDrawingKick) Prim_Sprite();
			m_nVtxCount = 2;
			break;
		}

		if(nDrawingKick && m_drawingToDepth)
		{
			DrawToDepth(m_primitiveType, m_PrimitiveMode);
		}
	}
}

void CGSH_OpenGL::ProcessHostToLocalTransfer()
{
	//Pages to merge need it even when the transfer changed nothing: their framebuffer doesn't have what RAM has.
	if(m_trxCtx.nDirty || !m_mergePages.empty())
	{
		FlushVertexBuffer();
		m_renderState.isTextureStateValid = false;
		m_renderState.isFramebufferStateValid = false;

		auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
		auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
		auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);

		auto [transferAddress, transferSize] = GsTransfer::GetDstRange(bltBuf, trxReg, trxPos);

		m_textureCache.InvalidateRange(transferAddress, transferSize);

		//Pages merged on the GPU stay with their framebuffer, which doesn't reload them from RAM.
		std::vector<std::pair<uint32, GPU_PAGE>> mergedPages;
		if(!m_mergePages.empty())
		{
			MergeTransfer(bltBuf, trxReg, trxPos);
			for(const auto& [page, owner] : m_mergePages)
			{
				if(m_gpuPages[page].owner == owner) mergedPages.emplace_back(page, m_gpuPages[page]);
			}
		}
		ClearGpuPages(transferAddress, transferSize);
		for(const auto& [page, state] : mergedPages)
		{
			if(!m_gpuPages[page].owner) m_gpuPageCount++;
			m_gpuPages[page] = state;
		}

		bool isUpperByteTransfer = (bltBuf.nDstPsm == PSMT8H) || (bltBuf.nDstPsm == PSMT4HL) || (bltBuf.nDstPsm == PSMT4HH);
		for(const auto& framebuffer : m_framebuffers)
		{
			if((framebuffer->m_psm == PSMCT24) && isUpperByteTransfer) continue;
			framebuffer->m_cachedArea.Invalidate(transferAddress, transferSize);
			for(const auto& [page, state] : mergedPages)
			{
				if(state.owner != framebuffer.get()) continue;
				framebuffer->m_cachedArea.ClearPageDirty(page - (framebuffer->m_basePtr / CGsPixelFormats::PAGESIZE));
			}
		}
	}
	m_mergePages.clear();
}

void CGSH_OpenGL::ProcessLocalToHostTransfer()
{
	//This is constrained to work only with ps2autotest, will be unconstrained later

	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);

	if(bltBuf.nSrcPsm != PSMCT32) return;

	uint32 transferAddress = bltBuf.GetSrcPtr();
	if(transferAddress != 0) return;

	if((trxReg.nRRW != 32) || (trxReg.nRRH != 32)) return;
	if((trxPos.nSSAX != 0) || (trxPos.nSSAY != 0)) return;

	auto framebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(),
	                                        [](const FramebufferPtr& framebuffer) {
		                                        return (framebuffer->m_psm == PSMCT32) && (framebuffer->m_basePtr == 0);
	                                        });
	if(framebufferIterator == std::end(m_framebuffers)) return;
	const auto& framebuffer = (*framebufferIterator);

	FlushVertexBuffer();
	m_renderState.isValid = false;

	auto pixels = new uint32[trxReg.nRRW * trxReg.nRRH];

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);
	glReadPixels(trxPos.nSSAX, trxPos.nSSAY, trxReg.nRRW, trxReg.nRRH, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	CHECKGLERROR();

	//Write back to RAM
	{
		CGsPixelFormats::CPixelIndexorPSMCT32 indexor(m_pRAM, bltBuf.GetSrcPtr(), bltBuf.nSrcWidth);
		for(uint32 y = trxPos.nSSAY; y < (trxPos.nSSAY + trxReg.nRRH); y++)
		{
			for(uint32 x = trxPos.nSSAX; x < (trxPos.nSSAX + trxReg.nRRH); x++)
			{
				uint32 pixel = pixels[x + (y * trxReg.nRRW)];
				indexor.SetPixel(x, y, pixel);
			}
		}
	}

	delete[] pixels;
}

void CGSH_OpenGL::ProcessLocalToLocalTransfer()
{
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto srcFramebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(),
	                                           [&](const FramebufferPtr& framebuffer) {
		                                           return (framebuffer->m_basePtr == bltBuf.GetSrcPtr()) &&
		                                                  (framebuffer->m_width == bltBuf.GetSrcWidth());
	                                           });
	auto dstFramebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(),
	                                           [&](const FramebufferPtr& framebuffer) {
		                                           return (framebuffer->m_basePtr == bltBuf.GetDstPtr()) &&
		                                                  (framebuffer->m_width == bltBuf.GetDstWidth());
	                                           });

	bool foundSrc = srcFramebufferIterator != std::end(m_framebuffers);
	bool foundDest = dstFramebufferIterator != std::end(m_framebuffers);

	if(foundSrc && foundDest)
	{
		FlushVertexBuffer();
		m_renderState.isValid = false;

		const auto& srcFramebuffer = (*srcFramebufferIterator);
		const auto& dstFramebuffer = (*dstFramebufferIterator);
		ReleaseGpuClutSource(dstFramebuffer.get());

		glBindFramebuffer(GL_FRAMEBUFFER, dstFramebuffer->m_framebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFramebuffer->m_framebuffer);

		//Copy buffers
		glBlitFramebuffer(
		    0, 0, srcFramebuffer->m_width * m_fbScale, srcFramebuffer->m_height * m_fbScale,
		    0, 0, srcFramebuffer->m_width * m_fbScale, srcFramebuffer->m_height * m_fbScale,
		    GL_COLOR_BUFFER_BIT, GL_NEAREST);
		CHECKGLERROR();

		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	}
	else if(foundSrc && !foundDest)
	{
		FlushVertexBuffer();
		m_renderState.isValid = false;

		auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
		auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
		auto imgbuffer = Framework::CBitmap(trxReg.nRRW * m_fbScale, trxReg.nRRH * m_fbScale, 32);

		glBindFramebuffer(GL_FRAMEBUFFER, (*srcFramebufferIterator)->m_framebuffer);
		glReadPixels(trxPos.nSSAX * m_fbScale, trxPos.nSSAY * m_fbScale, trxReg.nRRW * m_fbScale, trxReg.nRRH * m_fbScale, GL_RGBA, GL_UNSIGNED_BYTE, imgbuffer.GetPixels());
		CHECKGLERROR();

		// Sample down to requested size
		imgbuffer = imgbuffer.Resize(trxReg.nRRW, trxReg.nRRH);

		auto [transferAddress, transferSize] = GsTransfer::GetDstRange(bltBuf, trxReg, trxPos);
		m_textureCache.InvalidateRange(transferAddress, transferSize);
		ClearGpuPages(transferAddress, transferSize);

		//Write back to RAM
		{
			CGsPixelFormats::CPixelIndexorPSMCT32 indexor(m_pRAM, bltBuf.GetDstPtr(), bltBuf.nDstWidth);
			for(uint32 y = 0; y < trxReg.nRRH; y++)
			{
				for(uint32 x = 0; x < trxReg.nRRW; x++)
				{
					auto pixel = imgbuffer.GetPixel(x, y);
					indexor.SetPixel(trxPos.nDSAX + x, trxPos.nDSAY + y, MakeColor(pixel.r, pixel.g, pixel.b, pixel.a));
				}
			}
		}
	}
	else
	{
		assert(0);
	}
}

void CGSH_OpenGL::ProcessClutTransfer(uint32 csa, uint32)
{
	FlushVertexBuffer();
	m_renderState.isTextureStateValid = false;
	PalCache_Invalidate(csa);
}

Framework::CBitmap CGSH_OpenGL::GetScreenshot()
{
#if 0
	auto dispInfo = GetCurrentDisplayInfo();
	const auto& dispLayer = dispInfo.layers[0];

	FramebufferPtr framebuffer;
	for(const auto& candidateFramebuffer : m_framebuffers)
	{
		if(
		    (candidateFramebuffer->m_basePtr == dispLayer.bufPtr) &&
		    (GetFramebufferBitDepth(candidateFramebuffer->m_psm) == GetFramebufferBitDepth(dispLayer.psm)) &&
		    (candidateFramebuffer->m_width == dispLayer.bufWidth))
		{
			//We have a winner
			framebuffer = candidateFramebuffer;
			break;
		}
	}

	auto imgbuffer = Framework::CBitmap(dispWidth * m_fbScale, dispHeight * m_fbScale, 32);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);
	glReadPixels(0, 0, dispWidth * m_fbScale, dispHeight * m_fbScale, GL_RGBA, GL_UNSIGNED_BYTE, imgbuffer.GetPixels());
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if(halfHeight)
	{
		return imgbuffer.Resize(dispWidth * m_fbScale, dispHeight * 2 * m_fbScale);
	}
	return imgbuffer;
#endif
	return Framework::CBitmap();
}

Framework::CBitmap CGSH_OpenGL::GetFramebuffer(uint64 frameReg)
{
	Framework::CBitmap result;
	SendGSCall([&]() { result = GetFramebufferImpl(frameReg); }, true);
	return result;
}

Framework::CBitmap CGSH_OpenGL::GetFramebufferImpl(uint64 frameReg)
{
#ifndef GLES_COMPATIBILITY
	auto frame = make_convertible<FRAME>(frameReg);
	auto framebuffer = FindFramebuffer(frame);
	if(!framebuffer)
	{
		return Framework::CBitmap();
	}
	else
	{
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);
		auto imgbuffer = Framework::CBitmap(framebuffer->m_width * m_fbScale, framebuffer->m_height * m_fbScale, 32);
		glReadPixels(0, 0, framebuffer->m_width * m_fbScale, framebuffer->m_height * m_fbScale, GL_BGRA, GL_UNSIGNED_BYTE, imgbuffer.GetPixels());
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return imgbuffer;
	}
#else
	throw std::runtime_error("Feature is not implemented in current backend.");
#endif
}

Framework::CBitmap CGSH_OpenGL::GetDepthbuffer(uint64 frameReg, uint64 zbufReg)
{
	return Framework::CBitmap();
}

Framework::CBitmap CGSH_OpenGL::GetTexture(uint64 tex0Reg, uint32 maxMip, uint64 miptbp1Reg, uint64 miptbp2Reg, uint32 mipLevel)
{
	Framework::CBitmap result;
	SendGSCall([&]() { result = GetTextureImpl(tex0Reg, maxMip, miptbp1Reg, miptbp2Reg, mipLevel); }, true);
	return result;
}

Framework::CBitmap CGSH_OpenGL::GetTextureImpl(uint64 tex0Reg, uint32 maxMip, uint64 miptbp1Reg, uint64 miptbp2Reg, uint32 mipLevel)
{
#ifndef GLES_COMPATIBILITY
	if(mipLevel != 0)
	{
		//Not supported
		return Framework::CBitmap();
	}

	auto tex0 = make_convertible<TEX0>(tex0Reg);
	auto width = std::max<uint32>(tex0.GetWidth() >> mipLevel, 1);
	auto height = std::max<uint32>(tex0.GetHeight() >> mipLevel, 1);

	FRAME frame = {};
	frame.nPtr = tex0.GetBufPtr() / 8192;
	frame.nWidth = tex0.GetWidth() / 64;
	frame.nPsm = tex0.nPsm;
	auto framebuffer = FindFramebuffer(frame);
	if(framebuffer)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);
		auto imgbuffer = Framework::CBitmap(width, height, 32);
		glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, imgbuffer.GetPixels());
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return imgbuffer;
	}

	auto texInfo = PrepareTexture(tex0);
	if(texInfo.textureHandle == 0)
	{
		return Framework::CBitmap();
	}

	auto texFormat = GetTextureFormatInfo(tex0.nPsm);
	auto bitsPerPixel =
	    [format = tex0.nPsm]() {
		    switch(format)
		    {
		    case PSMCT16:
			    return 16;
		    case PSMT4:
		    case PSMT8:
		    case PSMT8H:
		    case PSMT4HL:
		    case PSMT4HH:
			    return 8;
		    default:
			    assert(false);
		    case PSMCT32:
		    case PSMCT24:
			    return 32;
		    }
	    }();

	auto imgbuffer = Framework::CBitmap(width, height, bitsPerPixel);
	glGetTexImage(GL_TEXTURE_2D, 0, texFormat.format, texFormat.type, imgbuffer.GetPixels());
	glBindTexture(GL_TEXTURE_2D, 0);
	return imgbuffer;
#else
	throw std::runtime_error("Feature is not implemented in current backend.");
#endif
}

int CGSH_OpenGL::GetFramebufferScale()
{
	return m_fbScale;
}

const CGSHandler::VERTEX* CGSH_OpenGL::GetInputVertices() const
{
	return m_VtxBuffer;
}

bool CGSH_OpenGL::GetDepthTestingEnabled() const
{
	return m_depthTestingEnabled;
}

void CGSH_OpenGL::SetDepthTestingEnabled(bool depthTestingEnabled)
{
	m_depthTestingEnabled = depthTestingEnabled;
	m_renderState.isValid = false;
}

bool CGSH_OpenGL::GetAlphaBlendingEnabled() const
{
	return m_alphaBlendingEnabled;
}

void CGSH_OpenGL::SetAlphaBlendingEnabled(bool alphaBlendingEnabled)
{
	m_alphaBlendingEnabled = alphaBlendingEnabled;
	m_renderState.isValid = false;
}

bool CGSH_OpenGL::GetAlphaTestingEnabled() const
{
	return m_alphaTestingEnabled;
}

void CGSH_OpenGL::SetAlphaTestingEnabled(bool alphaTestingEnabled)
{
	m_alphaTestingEnabled = alphaTestingEnabled;
	m_renderState.isValid = false;
}

/////////////////////////////////////////////////////////////
// Framebuffer
/////////////////////////////////////////////////////////////

CGSH_OpenGL::CFramebuffer::CFramebuffer(uint32 basePtr, uint32 width, uint32 height, uint32 psm, uint32 scale, bool multisampled)
    : m_basePtr(basePtr)
    , m_width(width)
    , m_height(height)
    , m_psm(psm)
{
	m_cachedArea.SetArea(psm, basePtr, width, height);

	//Build color attachment
	m_texture = Framework::OpenGl::CTexture::Create();
	glBindTexture(GL_TEXTURE_2D, m_texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, m_width * scale, m_height * scale);
	CHECKGLERROR();

	if(multisampled)
	{
		//We also need an attachment for multisampled color
		m_colorBufferMs = Framework::OpenGl::CRenderbuffer::Create();
		glBindRenderbuffer(GL_RENDERBUFFER, m_colorBufferMs);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, NUM_SAMPLES, GL_RGBA8, m_width * scale, m_height * scale);
		CHECKGLERROR();
	}

	//Build framebuffer
	m_framebuffer = Framework::OpenGl::CFramebuffer::Create();
	glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
	if(multisampled)
	{
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_colorBufferMs);
	}
	else
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
	}
	CHECKGLERROR();

	if(multisampled)
	{
		m_resolveFramebuffer = Framework::OpenGl::CFramebuffer::Create();
		glBindFramebuffer(GL_FRAMEBUFFER, m_resolveFramebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
		CHECKGLERROR();

		assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void CGSH_OpenGL::PopulateFramebuffer(const FramebufferPtr& framebuffer)
{
	auto texFormat = GetTextureFormatInfo(framebuffer->m_psm);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_copyToFbTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, texFormat.internalFormat, framebuffer->m_width, framebuffer->m_height,
	             0, texFormat.format, texFormat.type, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	((this)->*(m_textureUpdater[framebuffer->m_psm]))(framebuffer->m_basePtr,
	                                                  framebuffer->m_width / 64, 0, 0, framebuffer->m_width, framebuffer->m_height);
	CHECKGLERROR();

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);

	CopyToFb(
	    0, 0, framebuffer->m_width, framebuffer->m_height,
	    framebuffer->m_width, framebuffer->m_height,
	    0, 0, framebuffer->m_width * m_fbScale, framebuffer->m_height * m_fbScale);
	framebuffer->m_resolveNeeded = true;

	CHECKGLERROR();
}

void CGSH_OpenGL::CommitFramebufferDirtyPages(const FramebufferPtr& framebuffer, unsigned int minY, unsigned int maxY)
{
	class CCopyToFbEnabler
	{
	public:
		void EnableCopyToFb(const FramebufferPtr& framebuffer, GLuint copyToFbTexture)
		{
			if(m_copyToFbEnabled) return;

			glDisable(GL_SCISSOR_TEST);

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, copyToFbTexture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, framebuffer->m_width, framebuffer->m_height,
			             0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);

			m_copyToFbEnabled = true;
		}

	private:
		bool m_copyToFbEnabled = false;
	};

	auto& cachedArea = framebuffer->m_cachedArea;
	if(cachedArea.HasDirtyPages()) ReleaseGpuClutSource(framebuffer.get());

	auto texturePageSize = CGsPixelFormats::GetPsmPageSize(framebuffer->m_psm);

	CCopyToFbEnabler copyToFbEnabler;
	while(cachedArea.HasDirtyPages())
	{
		auto dirtyRect = cachedArea.GetDirtyPageRect();
		assert((dirtyRect.width != 0) && (dirtyRect.height != 0));
		cachedArea.ClearDirtyPages(dirtyRect);

		uint32 texX = dirtyRect.x * texturePageSize.first;
		uint32 texY = dirtyRect.y * texturePageSize.second;
		uint32 texWidth = dirtyRect.width * texturePageSize.first;
		uint32 texHeight = dirtyRect.height * texturePageSize.second;

		if(texY >= maxY)
		{
			//Don't bother
			continue;
		}

		assert(texX < framebuffer->m_width);
		assert(texY < framebuffer->m_height);
		//assert(texY >= minY);
		//assert(texY < maxY);
		if((texX + texWidth) > framebuffer->m_width)
		{
			texWidth = framebuffer->m_width - texX;
		}
		if((texY + texHeight) > framebuffer->m_height)
		{
			texHeight = framebuffer->m_height - texY;
		}

		m_validGlState &= ~(GLSTATE_SCISSOR | GLSTATE_FRAMEBUFFER | GLSTATE_TEXTURE);
		copyToFbEnabler.EnableCopyToFb(framebuffer, m_copyToFbTexture);

		((this)->*(m_textureUpdater[framebuffer->m_psm]))(framebuffer->m_basePtr, framebuffer->m_width / 64,
		                                                  texX, texY, texWidth, texHeight);

		CopyToFb(
		    texX, texY, (texX + texWidth), (texY + texHeight),
		    framebuffer->m_width, framebuffer->m_height,
		    texX * m_fbScale, texY * m_fbScale, (texX + texWidth) * m_fbScale, (texY + texHeight) * m_fbScale);
		framebuffer->m_resolveNeeded = true;

		CHECKGLERROR();
	}

	//Mark all pages as clean, but might be wrong due to range not
	//covering an area that might be used later on
	cachedArea.ClearDirtyPages();
}

void CGSH_OpenGL::ResolveFramebufferMultisample(const FramebufferPtr& framebuffer, uint32 scale)
{
	if(!framebuffer->m_resolveNeeded) return;

	m_validGlState &= ~(GLSTATE_SCISSOR | GLSTATE_FRAMEBUFFER);

	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_resolveFramebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer->m_framebuffer);
	glBlitFramebuffer(
	    0, 0, framebuffer->m_width * scale, framebuffer->m_height * scale,
	    0, 0, framebuffer->m_width * scale, framebuffer->m_height * scale,
	    GL_COLOR_BUFFER_BIT, GL_NEAREST);
	CHECKGLERROR();

	framebuffer->m_resolveNeeded = false;
}

/////////////////////////////////////////////////////////////
// Depthbuffer
/////////////////////////////////////////////////////////////

CGSH_OpenGL::CDepthbuffer::CDepthbuffer(uint32 basePtr, uint32 width, uint32 height, uint32 psm, uint32 scale, bool multisampled)
    : m_basePtr(basePtr)
    , m_width(width)
    , m_height(height)
    , m_psm(psm)
{
	//Build depth attachment
	m_depthBuffer = Framework::OpenGl::CRenderbuffer::Create();
	glBindRenderbuffer(GL_RENDERBUFFER, m_depthBuffer);
	if(multisampled)
	{
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, NUM_SAMPLES, GL_DEPTH_COMPONENT32F, m_width * scale, m_height * scale);
	}
	else
	{
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, m_width * scale, m_height * scale);
	}
	CHECKGLERROR();
}

/////////////////////////////////////////////////////////////
// GPU to RAM synchronisation
/////////////////////////////////////////////////////////////

//Every format splits a page into 32 blocks of 256 bytes: which pages, and which of their blocks, a rectangle of a
//buffer covers. Bases are in blocks, so a buffer may start inside a page.
static void AddPageBlocks(std::vector<std::pair<uint32, uint32>>& result, uint32 page, uint32 blocks)
{
	if(blocks == 0) return;
	page %= CGSHandler::RAMSIZE / CGsPixelFormats::PAGESIZE;
	auto entry = std::find_if(result.rbegin(), result.rend(), [page](const std::pair<uint32, uint32>& item) { return item.first == page; });
	if(entry != result.rend())
		entry->second |= blocks;
	else
		result.push_back(std::make_pair(page, blocks));
}

template <typename Storage>
static void GetRectBlocks(uint32 basePtr, uint32 bufWidth, uint32 x0, uint32 y0, uint32 x1, uint32 y1, std::vector<std::pair<uint32, uint32>>& result, bool wholeBlocks)
{
	result.clear();
	if(wholeBlocks)
	{
		//Only blocks the rectangle covers entirely.
		x0 = ((x0 + Storage::BLOCKWIDTH - 1) / Storage::BLOCKWIDTH) * Storage::BLOCKWIDTH;
		y0 = ((y0 + Storage::BLOCKHEIGHT - 1) / Storage::BLOCKHEIGHT) * Storage::BLOCKHEIGHT;
		x1 = (((x1 + 1) / Storage::BLOCKWIDTH) * Storage::BLOCKWIDTH);
		y1 = (((y1 + 1) / Storage::BLOCKHEIGHT) * Storage::BLOCKHEIGHT);
		if((x1 <= x0) || (y1 <= y0)) return;
		x1--;
		y1--;
	}
	uint32 baseBlock = basePtr / 0x100;
	for(uint32 pageY = y0 / Storage::PAGEHEIGHT; pageY <= (y1 / Storage::PAGEHEIGHT); pageY++)
	{
		for(uint32 pageX = x0 / Storage::PAGEWIDTH; pageX <= (x1 / Storage::PAGEWIDTH); pageX++)
		{
			uint32 pageLeft = pageX * Storage::PAGEWIDTH;
			uint32 pageTop = pageY * Storage::PAGEHEIGHT;
			uint32 blockX0 = (std::max<uint32>(x0, pageLeft) - pageLeft) / Storage::BLOCKWIDTH;
			uint32 blockX1 = (std::min<uint32>(x1, pageLeft + Storage::PAGEWIDTH - 1) - pageLeft) / Storage::BLOCKWIDTH;
			uint32 blockY0 = (std::max<uint32>(y0, pageTop) - pageTop) / Storage::BLOCKHEIGHT;
			uint32 blockY1 = (std::min<uint32>(y1, pageTop + Storage::PAGEHEIGHT - 1) - pageTop) / Storage::BLOCKHEIGHT;
			//As CPixelIndexor places pages: a buffer narrower than a page has rows that overlap.
			uint32 pageBlock = baseBlock + ((pageX + ((pageY * bufWidth) / Storage::PAGEWIDTH)) * 32);
			uint32 mask = 0;
			if((blockX0 == 0) && (blockY0 == 0) && (blockX1 == (Storage::PAGEWIDTH / Storage::BLOCKWIDTH) - 1) &&
			   (blockY1 == (Storage::PAGEHEIGHT / Storage::BLOCKHEIGHT) - 1))
			{
				mask = ~0U;
			}
			else
			{
				for(uint32 blockY = blockY0; blockY <= blockY1; blockY++)
				{
					for(uint32 blockX = blockX0; blockX <= blockX1; blockX++)
					{
						mask |= 1U << Storage::m_nBlockSwizzleTable[blockY][blockX];
					}
				}
			}
			uint32 offset = pageBlock % 32;
			AddPageBlocks(result, pageBlock / 32, mask << offset);
			if(offset != 0) AddPageBlocks(result, (pageBlock / 32) + 1, mask >> (32 - offset));
		}
	}
}

static bool GetBufferBlocks(uint32 psm, uint32 basePtr, uint32 bufWidth, uint32 x0, uint32 y0, uint32 x1, uint32 y1, std::vector<std::pair<uint32, uint32>>& result, bool wholeBlocks = false)
{
	switch(psm)
	{
	case CGSHandler::PSMT8H:
	case CGSHandler::PSMT4HL:
	case CGSHandler::PSMT4HH:
	case CGSHandler::PSMCT24:
	case CGSHandler::PSMZ24:
		//These leave some of each pixel's bytes alone.
		if(wholeBlocks)
		{
			result.clear();
			return true;
		}
		GetRectBlocks<CGsPixelFormats::STORAGEPSMCT32>(basePtr, bufWidth, x0, y0, x1, y1, result, wholeBlocks);
		return true;
	case CGSHandler::PSMCT32:
		GetRectBlocks<CGsPixelFormats::STORAGEPSMCT32>(basePtr, bufWidth, x0, y0, x1, y1, result, wholeBlocks);
		return true;
	case CGSHandler::PSMCT16:
		GetRectBlocks<CGsPixelFormats::STORAGEPSMCT16>(basePtr, bufWidth, x0, y0, x1, y1, result, wholeBlocks);
		return true;
	case CGSHandler::PSMCT16S:
		GetRectBlocks<CGsPixelFormats::STORAGEPSMCT16S>(basePtr, bufWidth, x0, y0, x1, y1, result, wholeBlocks);
		return true;
	case CGSHandler::PSMT8:
		GetRectBlocks<CGsPixelFormats::STORAGEPSMT8>(basePtr, bufWidth, x0, y0, x1, y1, result, wholeBlocks);
		return true;
	case CGSHandler::PSMT4:
		GetRectBlocks<CGsPixelFormats::STORAGEPSMT4>(basePtr, bufWidth, x0, y0, x1, y1, result, wholeBlocks);
		return true;
	case CGSHandler::PSMZ32:
		GetRectBlocks<CGsPixelFormats::STORAGEPSMZ32>(basePtr, bufWidth, x0, y0, x1, y1, result, wholeBlocks);
		return true;
	case CGSHandler::PSMZ16:
		GetRectBlocks<CGsPixelFormats::STORAGEPSMZ16>(basePtr, bufWidth, x0, y0, x1, y1, result, wholeBlocks);
		return true;
	case CGSHandler::PSMZ16S:
		GetRectBlocks<CGsPixelFormats::STORAGEPSMZ16S>(basePtr, bufWidth, x0, y0, x1, y1, result, wholeBlocks);
		return true;
	default:
		return false;
	}
}

static bool IsSyncedFramebufferPsm(uint32 psm)
{
	return (psm == CGSHandler::PSMCT32) || (psm == CGSHandler::PSMCT24) || (psm == CGSHandler::PSMCT16) || (psm == CGSHandler::PSMCT16S);
}

//Where a page of RAM is in a framebuffer, in pixels; false when the framebuffer doesn't hold it.
static bool GetPagePosition(uint32 basePtr, uint32 width, uint32 height, uint32 psm, uint32 page, uint32& x, uint32& y)
{
	auto pageSize = CGsPixelFormats::GetPsmPageSize(psm);
	uint32 basePage = basePtr / CGsPixelFormats::PAGESIZE;
	if(page < basePage) page += CGSHandler::RAMSIZE / CGsPixelFormats::PAGESIZE;
	uint32 pagesPerRow = std::max<uint32>(1, width / pageSize.first);
	uint32 relativePage = page - basePage;
	x = (relativePage % pagesPerRow) * pageSize.first;
	y = (relativePage / pagesPerRow) * pageSize.second;
	return y < height;
}

void CGSH_OpenGL::MarkGpuPages()
{
	auto framebuffer = m_drawFramebuffer;
	if(m_gpuClutSource && (m_gpuClutSource.get() == framebuffer)) MaterializeGpuClut();
	if(!framebuffer || !IsSyncedFramebufferPsm(framebuffer->m_psm)) return;

	float minX = m_vertexBuffer[0].x, maxX = minX;
	float minY = m_vertexBuffer[0].y, maxY = minY;
	for(const auto& vertex : m_vertexBuffer)
	{
		minX = (std::min)(minX, vertex.x);
		maxX = (std::max)(maxX, vertex.x);
		minY = (std::min)(minY, vertex.y);
		maxY = (std::max)(maxY, vertex.y);
	}
	//Pixels from the left/top edge up to, not including, the right/bottom one.
	int32 x0 = std::max<int32>(static_cast<int32>(floorf(minX)), m_renderState.scissorX);
	int32 y0 = std::max<int32>(static_cast<int32>(floorf(minY)), m_renderState.scissorY);
	int32 x1 = std::min<int32>(static_cast<int32>(ceilf(maxX)) - 1, m_renderState.scissorX + m_renderState.scissorWidth - 1);
	int32 y1 = std::min<int32>(static_cast<int32>(ceilf(maxY)) - 1, m_renderState.scissorY + m_renderState.scissorHeight - 1);
	x0 = std::max<int32>(x0, 0);
	y0 = std::max<int32>(y0, 0);
	x1 = std::min<int32>(x1, framebuffer->m_width - 1);
	y1 = std::min<int32>(y1, framebuffer->m_height - 1);
	if((x1 < x0) || (y1 < y0)) return;

	GetBufferBlocks(framebuffer->m_psm, framebuffer->m_basePtr, framebuffer->m_width, x0, y0, x1, y1, m_drawBlockList);
	std::vector<uint32> uploads;
	for(const auto& [page, blocks] : m_drawBlockList)
	{
		InvalidateByteViews(page);
		ClearRamCurrent(page, blocks);
		auto& state = m_gpuPages[page];
		if(state.owner != framebuffer)
		{
			if(state.owner && !MoveGpuPage(state.owner, framebuffer, page))
			{
				SyncFramebufferToRam(state.owner, page, page);
			}
			if(!state.owner)
			{
				//The page is copied back later: what this draw leaves of it must be what RAM has.
				if(state.syncedFrom != framebuffer) uploads.push_back(page);
				state.owner = framebuffer;
				state.drawnBlocks = 0;
				state.channels = 0;
				m_gpuPageCount++;
			}
			state.syncedFrom = nullptr;
		}
		state.drawnBlocks |= blocks;
		state.channels |= m_drawMask;
	}
	if(!uploads.empty()) UploadFramebufferPages(m_drawFramebufferPtr, uploads);
}

//Another framebuffer over the same memory takes a page over: a copy on the GPU.
bool CGSH_OpenGL::MoveGpuPage(CFramebuffer* source, CFramebuffer* destination, uint32 page)
{
	InvalidateByteViews(page);
	auto& state = m_gpuPages[page];
	//Each framebuffer places the page by its own width: they need whole pages per row.
	if(((source->m_width % 64) != 0) || ((destination->m_width % 64) != 0)) return false;
	bool source32 = (source->m_psm == PSMCT32) || (source->m_psm == PSMCT24);
	bool destination32 = (destination->m_psm == PSMCT32) || (destination->m_psm == PSMCT24);
	if(!source32 || !destination32) return false;
	uint32 sourceX = 0, sourceY = 0, destinationX = 0, destinationY = 0;
	if(!GetPagePosition(source->m_basePtr, source->m_width, source->m_height, source->m_psm, page, sourceX, sourceY)) return false;
	if(!GetPagePosition(destination->m_basePtr, destination->m_width, destination->m_height, destination->m_psm, page, destinationX, destinationY)) return false;
	//A PSMCT24 framebuffer would drop the alpha the other one drew.
	if((destination->m_psm == PSMCT24) && (state.channels & 0xFF000000)) return false;

	auto destinationIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(),
	                                        [destination](const FramebufferPtr& candidate) { return candidate.get() == destination; });
	if(destinationIterator == m_framebuffers.end()) return false;
	ReleaseGpuClutSource(destination);
	if(state.syncedFrom != destination)
	{
		//Its own copy of the page first: what the source didn't draw (and its alpha, from a PSMCT24 one).
		UploadFramebufferPages(*destinationIterator, {page});
	}

	m_validGlState &= ~(GLSTATE_SCISSOR | GLSTATE_FRAMEBUFFER | GLSTATE_TEXTURE);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, destination->m_framebuffer);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source->m_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	//A PSMCT24 framebuffer's alpha is not the memory's: the destination keeps its own.
	bool copyAlpha = (source->m_psm == PSMCT32) && (destination->m_psm == PSMCT32);
	CopyToFb(sourceX, sourceY, sourceX + 64, sourceY + 32, source->m_width, source->m_height,
	         destinationX * m_fbScale, destinationY * m_fbScale, (destinationX + 64) * m_fbScale, (destinationY + 32) * m_fbScale, copyAlpha);
	destination->m_resolveNeeded = true;
	CHECKGLERROR();

	state.owner = destination;
	if(!copyAlpha) state.channels &= 0x00FFFFFF;
	return true;
}

void CGSH_OpenGL::UploadFramebufferPages(const FramebufferPtr& framebuffer, const std::vector<uint32>& pages)
{
	if(m_gpuClutSource && (m_gpuClutSource.get() == framebuffer.get())) MaterializeGpuClut();
	for(auto page : pages) InvalidateByteViews(page);
	m_validGlState &= ~(GLSTATE_SCISSOR | GLSTATE_FRAMEBUFFER | GLSTATE_TEXTURE);
	glDisable(GL_SCISSOR_TEST);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_copyToFbTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, framebuffer->m_width, framebuffer->m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);

	auto pageSize = CGsPixelFormats::GetPsmPageSize(framebuffer->m_psm);
	for(uint32 page : pages)
	{
		uint32 x = 0, y = 0;
		if(!GetPagePosition(framebuffer->m_basePtr, framebuffer->m_width, framebuffer->m_height, framebuffer->m_psm, page, x, y)) continue;
		uint32 width = std::min<uint32>(pageSize.first, framebuffer->m_width - x);
		uint32 height = std::min<uint32>(pageSize.second, framebuffer->m_height - y);
		((this)->*(m_textureUpdater[framebuffer->m_psm]))(framebuffer->m_basePtr, framebuffer->m_width / 64, x, y, width, height);
		CopyToFb(x, y, x + width, y + height, framebuffer->m_width, framebuffer->m_height,
		         x * m_fbScale, y * m_fbScale, (x + width) * m_fbScale, (y + height) * m_fbScale);
	}
	framebuffer->m_resolveNeeded = true;
	CHECKGLERROR();
}

//Brings what the GPU drew over [address, address + size) to RAM, for a CLUT read.
void CGSH_OpenGL::SyncMemoryForRead(uint32 address, uint32 size)
{
	if(!m_vertexBuffer.empty() && m_renderState.isValid) FlushVertexBuffer();
	if(m_gpuPageCount == 0) return;
	uint32 firstPage = address / CGsPixelFormats::PAGESIZE;
	uint32 lastPage = (address + std::max<uint32>(size, 1) - 1) / CGsPixelFormats::PAGESIZE;
	uint32 firstBlock = address / 0x100;
	uint32 lastBlock = (address + std::max<uint32>(size, 1) - 1) / 0x100;
	for(uint32 page = firstPage; page <= lastPage; page++)
	{
		auto& state = m_gpuPages[page % GPU_PAGE_COUNT];
		if(!state.owner) continue;
		uint32 blocks = 0;
		for(uint32 block = (std::max)(firstBlock, page * 32); block <= (std::min)(lastBlock, (page * 32) + 31); block++)
		{
			blocks |= 1U << (block % 32);
		}
		if((state.drawnBlocks & blocks) && !IsRamCurrentRange(std::max<uint32>(address, page * CGsPixelFormats::PAGESIZE),
		                                                       std::min<uint32>(address + std::max<uint32>(size, 1), (page + 1) * CGsPixelFormats::PAGESIZE)))
		{
			SyncFramebufferToRam(state.owner, page % GPU_PAGE_COUNT, page % GPU_PAGE_COUNT);
		}
	}
}

void CGSH_OpenGL::SyncTextureArea(const TEX0& tex0)
{
	if(m_gpuPageCount == 0) return;
	if(!GetBufferBlocks(tex0.nPsm, tex0.GetBufPtr(), tex0.GetBufWidth(), 0, 0, tex0.GetWidth() - 1, tex0.GetHeight() - 1, m_blockList)) return;
	for(const auto& [page, blocks] : m_blockList)
	{
		auto& state = m_gpuPages[page];
		if(state.owner && (state.drawnBlocks & blocks) && !IsRamCurrent(page, state.drawnBlocks & blocks))
		{
			//All of the framebuffer: games read one page by page (Gran Turismo 4), one copy serves them all.
			if(!m_vertexBuffer.empty() && m_renderState.isValid) FlushVertexBuffer();
			SyncFramebufferToRam(state.owner);
			if(m_gpuPageCount == 0) return;
		}
	}
}

//Copies what the GPU drew in the framebuffer's pages (those from firstPage to lastPage) to RAM.
void CGSH_OpenGL::SyncFramebufferToRam(CFramebuffer* framebuffer, uint32 firstPage, uint32 lastPage)
{
	auto pageSize = CGsPixelFormats::GetPsmPageSize(framebuffer->m_psm);
	uint32 pagesPerRow = std::max<uint32>(1, framebuffer->m_width / pageSize.first);
	uint32 pageRows = (framebuffer->m_height + pageSize.second - 1) / pageSize.second;
	uint32 basePage = framebuffer->m_basePtr / CGsPixelFormats::PAGESIZE;

	struct DRAWN_PAGE
	{
		uint32 x, y, channels;
		uint32 page;
		uint32 drawnBlocks;
	};
	std::vector<DRAWN_PAGE> pages;
	uint32 minX = ~0U, minY = ~0U, maxX = 0, maxY = 0;
	for(uint32 pageY = 0; pageY < pageRows; pageY++)
	{
		for(uint32 pageX = 0; pageX < pagesPerRow; pageX++)
		{
			uint32 page = (basePage + (pageY * pagesPerRow) + pageX) % GPU_PAGE_COUNT;
			if((page < firstPage) || (page > lastPage)) continue;
			auto& state = m_gpuPages[page];
			if(state.owner != framebuffer) continue;
			pages.push_back({pageX, pageY, state.channels, page, state.drawnBlocks});
			minX = (std::min)(minX, pageX);
			maxX = (std::max)(maxX, pageX);
			minY = (std::min)(minY, pageY);
			maxY = (std::max)(maxY, pageY);
		}
	}
	if(pages.empty()) return;

	uint32 rectX = minX * pageSize.first;
	uint32 rectY = minY * pageSize.second;
	uint32 rectWidth = std::min<uint32>((maxX + 1) * pageSize.first, framebuffer->m_width) - rectX;
	uint32 rectHeight = std::min<uint32>((maxY + 1) * pageSize.second, framebuffer->m_height) - rectY;
	uint32 scale = m_fbScale;

	GLuint readFramebuffer = framebuffer->m_framebuffer;
	if(m_multisampleEnabled)
	{
		auto framebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(),
		                                        [framebuffer](const FramebufferPtr& candidate) { return candidate.get() == framebuffer; });
		if(framebufferIterator != m_framebuffers.end())
		{
			ResolveFramebufferMultisample(*framebufferIterator, scale);
			readFramebuffer = framebuffer->m_resolveFramebuffer;
		}
	}

	m_readbackBuffer.resize(rectWidth * scale * rectHeight * scale);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
	glReadPixels(rectX * scale, rectY * scale, rectWidth * scale, rectHeight * scale, GL_RGBA, GL_UNSIGNED_BYTE, m_readbackBuffer.data());
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_renderState.framebufferHandle);
	CHECKGLERROR();

	uint32 rowPitch = rectWidth * scale;
	auto to16 = [](uint32 pixel) -> uint16 {
		return static_cast<uint16>(((pixel >> 3) & 0x1F) | (((pixel >> 11) & 0x1F) << 5) | (((pixel >> 19) & 0x1F) << 10) |
		                           ((pixel & 0x80000000) ? 0x8000 : 0));
	};
	bool is16 = (framebuffer->m_psm == PSMCT16) || (framebuffer->m_psm == PSMCT16S);
	uint32 blockWidth = is16 ? 16 : 8;
	uint32 blockHeight = 8;
	uint32 bufWidth = framebuffer->m_width / 64;
	CGsPixelFormats::CPixelIndexorPSMCT32 indexor32(m_pRAM, framebuffer->m_basePtr, bufWidth);
	CGsPixelFormats::CPixelIndexorPSMCT16 indexor16(m_pRAM, framebuffer->m_basePtr, bufWidth);
	CGsPixelFormats::CPixelIndexorPSMCT16S indexor16S(m_pRAM, framebuffer->m_basePtr, bufWidth);
	for(const auto& page : pages)
	{
		//Only the channels the draws wrote.
		uint32 mask = page.channels;
		if(framebuffer->m_psm == PSMCT24) mask &= 0x00FFFFFF;
		for(uint32 blockY = 0; blockY < (pageSize.second / blockHeight); blockY++)
		{
			for(uint32 blockX = 0; blockX < (pageSize.first / blockWidth); blockX++)
			{
				uint32 block = 0;
				switch(framebuffer->m_psm)
				{
				case PSMCT16:
					block = CGsPixelFormats::STORAGEPSMCT16::m_nBlockSwizzleTable[blockY][blockX];
					break;
				case PSMCT16S:
					block = CGsPixelFormats::STORAGEPSMCT16S::m_nBlockSwizzleTable[blockY][blockX];
					break;
				default:
					block = CGsPixelFormats::STORAGEPSMCT32::m_nBlockSwizzleTable[blockY][blockX];
					break;
				}
				//RAM is newer in the blocks no draw wrote (a transfer may have replaced them).
				if(!(page.drawnBlocks & (1U << block))) continue;
				uint32 x0 = (page.x * pageSize.first) + (blockX * blockWidth);
				uint32 y0 = (page.y * pageSize.second) + (blockY * blockHeight);
				uint32 x1 = std::min<uint32>(x0 + blockWidth, framebuffer->m_width);
				uint32 y1 = std::min<uint32>(y0 + blockHeight, framebuffer->m_height);
				for(uint32 y = y0; y < y1; y++)
				{
					const uint32* row = m_readbackBuffer.data() + ((y - rectY) * scale * rowPitch);
					for(uint32 x = x0; x < x1; x++)
					{
						uint32 pixel = row[(x - rectX) * scale];
						switch(framebuffer->m_psm)
						{
						case PSMCT32:
						case PSMCT24:
							indexor32.SetPixel(x, y, (mask == ~0U) ? pixel : ((indexor32.GetPixel(x, y) & ~mask) | (pixel & mask)));
							break;
						case PSMCT16:
							indexor16.SetPixel(x, y, to16(pixel));
							break;
						case PSMCT16S:
							indexor16S.SetPixel(x, y, to16(pixel));
							break;
						}
					}
				}
			}
		}
		auto& state = m_gpuPages[page.page];
		state.owner = nullptr;
		state.syncedFrom = framebuffer;
		state.drawnBlocks = state.channels = 0;
		m_gpuPageCount--;
		m_textureCache.InvalidateRange(page.page * CGsPixelFormats::PAGESIZE, CGsPixelFormats::PAGESIZE);
	}
}

//The framebuffers reload the pages a transfer writes from RAM: what the GPU drew there is brought to RAM first,
//unless the transfer overwrites all of it.
void CGSH_OpenGL::BeginTransferWrite()
{
	CGSHandler::BeginTransferWrite();
	m_mergePages.clear();
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
	if((trxReg.nRRW == 0) || (trxReg.nRRH == 0)) return;
	//Draws queued before the transfer mark their pages first.
	if(!m_vertexBuffer.empty() && m_renderState.isValid) FlushVertexBuffer();
	if(!GetBufferBlocks(bltBuf.nDstPsm, bltBuf.GetDstPtr(), bltBuf.GetDstWidth(), trxPos.nDSAX, trxPos.nDSAY,
	                    trxPos.nDSAX + trxReg.nRRW - 1, trxPos.nDSAY + trxReg.nRRH - 1, m_blockList))
	{
		auto [address, size] = GsTransfer::GetDstRange(bltBuf, trxReg, trxPos);
		SyncMemoryForRead(address, size);
		ClearGpuPages(address, size);
		return;
	}
	if(m_gpuPageCount == 0) return;
	GetBufferBlocks(bltBuf.nDstPsm, bltBuf.GetDstPtr(), bltBuf.GetDstWidth(), trxPos.nDSAX, trxPos.nDSAY,
	                trxPos.nDSAX + trxReg.nRRW - 1, trxPos.nDSAY + trxReg.nRRH - 1, m_wholeBlockList, true);
	for(const auto& [page, blocks] : m_blockList)
	{
		auto& state = m_gpuPages[page];
		if(!state.owner) continue;
		uint32 overwritten = 0;
		for(const auto& [wholePage, wholeBlocks] : m_wholeBlockList)
		{
			if(wholePage == page) overwritten |= wholeBlocks;
		}
		if(state.drawnBlocks & ~overwritten)
		{
			if(CanMergeTransfer(state.owner, bltBuf, page))
			{
				m_mergePages.emplace_back(page, state.owner);
			}
			else
			{
				SyncFramebufferToRam(state.owner, page, page);
			}
		}
		else
		{
			//The transfer replaces all the GPU drew there: a later sync must not bring it back over the transfer.
			state = GPU_PAGE();
			m_gpuPageCount--;
		}
	}
}

void CGSH_OpenGL::ClearGpuPages(uint32 address, uint32 size)
{
	if(size == 0) return;
	uint32 firstPage = address / CGsPixelFormats::PAGESIZE;
	uint32 lastPage = (address + size - 1) / CGsPixelFormats::PAGESIZE;
	for(uint32 page = firstPage; page <= lastPage; page++)
	{
		auto& state = m_gpuPages[page % GPU_PAGE_COUNT];
		state.syncedFrom = nullptr;
		if(state.owner)
		{
			state = GPU_PAGE();
			m_gpuPageCount--;
		}
	}
}

//GS memory was replaced (a state was loaded): RAM has everything, the GPU's copies are reloaded from it.
void CGSH_OpenGL::WriteBackMemoryCache()
{
	if(!m_vertexBuffer.empty() && m_renderState.isValid) FlushVertexBuffer();
	ClearAllGpuPages();
	m_gpuClutActive = false;
	m_textureCache.InvalidateRange(0, RAMSIZE);
	for(const auto& framebuffer : m_framebuffers)
	{
		framebuffer->m_cachedArea.Invalidate(0, RAMSIZE);
	}
}

void CGSH_OpenGL::ClearAllGpuPages()
{
	InvalidateByteViews();
	for(auto& view : m_byteViews) view.source.reset();
	std::fill(m_ramCurrent.begin(), m_ramCurrent.end(), 0);
	m_mergePages.clear();
	m_gpuClutSource.reset();
	m_gpuClutActive = false;
	std::fill(std::begin(m_gpuPages), std::end(m_gpuPages), GPU_PAGE());
	m_gpuPageCount = 0;
	m_drawFramebuffer = nullptr;
	m_drawFramebufferPtr.reset();
}

void CGSH_OpenGL::SyncCLUT(const TEX0& tex0)
{
	if(!ProcessCLD(tex0)) return;
	if(SyncClutOnGpu(tex0)) return;

	if(m_gpuClutActive)
	{
		m_gpuClutActive = false;
		m_gpuClutSource.reset();
		ProcessClutTransfer(tex0.nCSA, 0);
	}
	SyncMemoryForRead(tex0.GetCLUTPtr(), (tex0.nCSM != 0) ? CGsPixelFormats::PAGESIZE : (CGsPixelFormats::IsPsmIDTEX4(tex0.nPsm) ? 0x40 : 0x400));
	switch(tex0.nPsm)
	{
	case PSMT8:
	case PSMT8H:
		ReadCLUT8(tex0);
		break;
	case PSMT4:
	case PSMT4HH:
	case PSMT4HL:
		ReadCLUT4(tex0);
		break;
	}
}

//A 256-color CLUT the GPU drew (Gran Turismo 4 draws one per pass of its colour filter) is copied into the
//palette texture on the GPU: reading it back would stall on every one.
bool CGSH_OpenGL::SyncClutOnGpu(const TEX0& tex0)
{
	if(!m_vertexBuffer.empty() && m_renderState.isValid) FlushVertexBuffer();
	if(m_gpuPageCount == 0) return false;
	if(!CGsPixelFormats::IsPsmIDTEX8(tex0.nPsm) || (tex0.nCSM != 0) || (tex0.nCPSM != PSMCT32)) return false;
	uint32 clutPtr = tex0.GetCLUTPtr();
	//The framebuffer must have all of the CLUT's blocks and have drawn some (otherwise RAM has it as it is).
	GetRectBlocks<CGsPixelFormats::STORAGEPSMCT32>(clutPtr, 64, 0, 0, 15, 15, m_blockList, false);
	CFramebuffer* framebuffer = nullptr;
	bool drawn = false;
	for(const auto& [clutPage, blocks] : m_blockList)
	{
		const auto& state = m_gpuPages[clutPage];
		if(!state.owner || (framebuffer && (state.owner != framebuffer))) return false;
		framebuffer = state.owner;
		drawn |= (state.drawnBlocks & blocks) != 0;
	}
	if(!drawn || (framebuffer->m_psm != PSMCT32) || (clutPtr < framebuffer->m_basePtr)) return false;

	//The CLUT is 4 blocks of 8x8 pixels from its pointer's block on, which the shader places in the framebuffer.
	uint32 offset = clutPtr - framebuffer->m_basePtr;
	uint32 block = (offset % CGsPixelFormats::PAGESIZE) / 0x100;
	uint32 page = offset / CGsPixelFormats::PAGESIZE;
	uint32 pagesPerRow = std::max<uint32>(1, framebuffer->m_width / 64);
	if((((page + 1) / pagesPerRow) * 32) >= framebuffer->m_height) return false;

	auto framebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(),
	                                        [framebuffer](const FramebufferPtr& candidate) { return candidate.get() == framebuffer; });
	if(framebufferIterator == m_framebuffers.end()) return false;

	//The draws read the palette from the framebuffer until something changes it (MaterializeGpuClut).
	m_gpuClutSource = *framebufferIterator;
	m_gpuClutPosition[0] = page;
	m_gpuClutPosition[1] = block;
	m_gpuClutPosition[2] = pagesPerRow;
	m_gpuClutPosition[3] = m_fbScale;
	m_gpuClutActive = true;
	m_renderState.isTextureStateValid = false;
	return true;
}

//Copies the palette the draws read from a framebuffer into the palette texture, before the framebuffer changes.
void CGSH_OpenGL::MaterializeGpuClut()
{
	if(!m_gpuClutSource) return;
	auto source = std::move(m_gpuClutSource);
	m_gpuClutSource.reset();
	if(m_multisampleEnabled) ResolveFramebufferMultisample(source, m_fbScale);

	if(!m_gpuClutProgram)
	{
		m_gpuClutProgram = GenerateGpuClutProgram();
		m_gpuClutPositionUniform = glGetUniformLocation(*m_gpuClutProgram, "g_position");
		glUseProgram(*m_gpuClutProgram);
		glUniform1i(glGetUniformLocation(*m_gpuClutProgram, "g_framebuffer"), 0);
		m_gpuClutTexture = Framework::OpenGl::CTexture::Create();
		glBindTexture(GL_TEXTURE_2D, m_gpuClutTexture);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 256, 1);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		m_gpuClutFramebuffer = Framework::OpenGl::CFramebuffer::Create();
		glBindFramebuffer(GL_FRAMEBUFFER, m_gpuClutFramebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gpuClutTexture, 0);
		CHECKGLERROR();
	}

	m_validGlState &= ~(GLSTATE_BLEND | GLSTATE_COLORMASK | GLSTATE_SCISSOR | GLSTATE_PROGRAM | GLSTATE_TEXTURE);
	m_validGlState &= ~(GLSTATE_VIEWPORT | GLSTATE_DEPTHTEST | GLSTATE_DEPTHMASK | GLSTATE_FRAMEBUFFER);

	glBindFramebuffer(GL_FRAMEBUFFER, m_gpuClutFramebuffer);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_FALSE);
	glViewport(0, 0, 256, 1);
	glUseProgram(*m_gpuClutProgram);
	glUniform4i(m_gpuClutPositionUniform, m_gpuClutPosition[0], m_gpuClutPosition[1], m_gpuClutPosition[2], m_gpuClutPosition[3]);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source->m_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
	glBindVertexArray(m_copyToFbVertexArray);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	CHECKGLERROR();

	m_renderState.isTextureStateValid = false;
}

void CGSH_OpenGL::ReleaseGpuClutSource(const CFramebuffer* framebuffer)
{
	if(m_gpuClutSource && (m_gpuClutSource.get() == framebuffer)) MaterializeGpuClut();
	for(auto& view : m_byteViews)
	{
		if(view.source && (view.source.get() == framebuffer)) view.valid.reset();
	}
}

void CGSH_OpenGL::InvalidateByteViews(uint32 page)
{
	for(auto& view : m_byteViews) view.valid.reset(page);
}

void CGSH_OpenGL::InvalidateByteViews()
{
	for(auto& view : m_byteViews) view.valid.reset();
}

void CGSH_OpenGL::ClearRamCurrent(uint32 page, uint32 blocks)
{
	uint64* words = m_ramCurrent.data() + (page * 256);
	for(uint32 block = 0; block < 32; block++)
	{
		if(blocks & (1U << block)) std::fill(words + (block * 8), words + (block * 8) + 8, 0);
	}
}

//Whether RAM has all of these drawn blocks as the GPU does.
bool CGSH_OpenGL::IsRamCurrent(uint32 page, uint32 blocks) const
{
	const uint64* words = m_ramCurrent.data() + (page * 256);
	for(uint32 block = 0; block < 32; block++)
	{
		if(!(blocks & (1U << block))) continue;
		for(uint32 i = 0; i < 8; i++)
		{
			if(words[(block * 8) + i] != ~0ULL) return false;
		}
	}
	return true;
}

//Whether RAM has the bytes [start, end) of one page as the GPU does.
bool CGSH_OpenGL::IsRamCurrentRange(uint32 start, uint32 end) const
{
	if(start >= end) return true;
	uint32 page = (start / CGsPixelFormats::PAGESIZE) % GPU_PAGE_COUNT;
	const auto& state = m_gpuPages[page];
	if(!state.owner) return true;
	const uint64* words = m_ramCurrent.data() + (page * 256);
	for(uint32 address = start; address < end; address++)
	{
		uint32 offset = address % CGsPixelFormats::PAGESIZE;
		if(!(state.drawnBlocks & (1U << (offset / 0x100)))) continue;
		uint32 nibble = offset * 2;
		if(((words[nibble / 64] >> (nibble % 64)) & 3) != 3) return false;
	}
	return true;
}

//Nibble of a PSMT4 page (128x128) each texel is.
static const uint16* GetPsmt4NibbleTable()
{
	static std::vector<uint16> table;
	if(!table.empty()) return table.data();
	table.assign(128 * 128, 0);
	std::vector<uint8> memory(CGsPixelFormats::PAGESIZE);
	CGsPixelFormats::CPixelIndexorPSMT4 indexor(memory.data(), 0, 2);
	//Four passes, each nibble holding 4 bits of its own index.
	for(uint32 pass = 0; pass < 4; pass++)
	{
		for(uint32 byte = 0; byte < CGsPixelFormats::PAGESIZE; byte++)
		{
			memory[byte] = static_cast<uint8>((((byte * 2) >> (pass * 4)) & 0x0F) | ((((byte * 2) + 1) >> (pass * 4)) & 0x0F) << 4);
		}
		for(uint32 y = 0; y < 128; y++)
		{
			for(uint32 x = 0; x < 128; x++)
			{
				table[(y * 128) + x] |= static_cast<uint16>(indexor.GetPixel(x, y) << (pass * 4));
			}
		}
	}
	return table.data();
}

namespace
{
	struct TRANSFER_LAYOUT
	{
		uint32 pageWidth = 0;
		uint32 pageHeight = 0;
		uint32 unitNibbles = 0;
		const uint32* byteOffsets = nullptr;
		const uint16* nibbleOffsets = nullptr;
	};

	bool GetTransferLayout(uint32 psm, TRANSFER_LAYOUT& layout)
	{
		switch(psm)
		{
		case CGSHandler::PSMCT32:
			layout = {64, 32, 8, CGsPixelFormats::CPixelIndexorPSMCT32::GetPageOffsets(), nullptr};
			return true;
		case CGSHandler::PSMCT24:
			layout = {64, 32, 6, CGsPixelFormats::CPixelIndexorPSMCT32::GetPageOffsets(), nullptr};
			return true;
		case CGSHandler::PSMCT16:
			layout = {64, 64, 4, CGsPixelFormats::CPixelIndexorPSMCT16::GetPageOffsets(), nullptr};
			return true;
		case CGSHandler::PSMCT16S:
			layout = {64, 64, 4, CGsPixelFormats::CPixelIndexorPSMCT16S::GetPageOffsets(), nullptr};
			return true;
		case CGSHandler::PSMT8:
			layout = {128, 64, 2, CGsPixelFormats::CPixelIndexorPSMT8::GetPageOffsets(), nullptr};
			return true;
		case CGSHandler::PSMT4:
			layout = {128, 128, 1, nullptr, GetPsmt4NibbleTable()};
			return true;
		default:
			return false;
		}
	}
}

//A transfer into a page a PSMCT32/PSMCT24 framebuffer drew can be merged into the framebuffer instead of bringing
//the page back to RAM (a GPU stall): Gran Turismo 4 uploads its HUD textures over memory it rendered to.
bool CGSH_OpenGL::CanMergeTransfer(CFramebuffer* owner, const BITBLTBUF& bltBuf, uint32 page)
{
	if(m_multisampleEnabled) return false;
	if((owner->m_psm != PSMCT32) && (owner->m_psm != PSMCT24)) return false;
	if(((owner->m_width % 64) != 0) || ((owner->m_basePtr % CGsPixelFormats::PAGESIZE) != 0)) return false;
	TRANSFER_LAYOUT layout;
	if(!GetTransferLayout(bltBuf.nDstPsm, layout)) return false;
	if(bltBuf.GetDstWidth() < layout.pageWidth) return false;
	uint32 pageX = 0, pageY = 0;
	if(!GetPagePosition(owner->m_basePtr, owner->m_width, owner->m_height, owner->m_psm, page, pageX, pageY)) return false;
	return std::any_of(m_framebuffers.begin(), m_framebuffers.end(), [owner](const FramebufferPtr& candidate) { return candidate.get() == owner; });
}

//After the transfer wrote RAM: the nibbles it wrote in each page of m_mergePages go into the framebuffer that has
//the page (a copy of the page, then a pass taking the written bits from RAM), and RAM is current for them.
void CGSH_OpenGL::MergeTransfer(const BITBLTBUF& bltBuf, const TRXREG& trxReg, const TRXPOS& trxPos)
{
	TRANSFER_LAYOUT layout;
	if(!GetTransferLayout(bltBuf.nDstPsm, layout)) return;
	uint32 pagesPerRow = bltBuf.GetDstWidth() / layout.pageWidth;
	uint32 baseNibble = bltBuf.GetDstPtr() * 2;

	std::vector<int32> maskIndex(GPU_PAGE_COUNT, -1);
	std::vector<uint64> masks(m_mergePages.size() * 256);
	for(uint32 i = 0; i < m_mergePages.size(); i++)
	{
		maskIndex[m_mergePages[i].first] = i;
	}
	for(uint32 y = 0; y < trxReg.nRRH; y++)
	{
		uint32 pixelY = (trxPos.nDSAY + y) % 2048;
		uint32 rowPage = (pixelY / layout.pageHeight) * pagesPerRow;
		uint32 rowOffset = (pixelY % layout.pageHeight) * layout.pageWidth;
		for(uint32 x = 0; x < trxReg.nRRW; x++)
		{
			uint32 pixelX = (trxPos.nDSAX + x) % 2048;
			uint32 offset = rowOffset + (pixelX % layout.pageWidth);
			uint32 inPage = layout.nibbleOffsets ? layout.nibbleOffsets[offset] : (layout.byteOffsets[offset] * 2);
			uint32 nibble = (baseNibble + ((rowPage + (pixelX / layout.pageWidth)) * CGsPixelFormats::PAGESIZE * 2) + inPage) % (RAMSIZE * 2);
			int32 index = maskIndex[nibble / (CGsPixelFormats::PAGESIZE * 2)];
			if(index < 0) continue;
			uint64* mask = masks.data() + (index * 256);
			uint32 pageNibble = nibble % (CGsPixelFormats::PAGESIZE * 2);
			for(uint32 i = 0; i < layout.unitNibbles; i++)
			{
				uint32 bit = pageNibble + i;
				mask[bit / 64] |= 1ULL << (bit % 64);
			}
		}
	}

	if(!m_mergeProgram)
	{
		m_mergeProgram = GenerateMergeProgram();
		glUseProgram(*m_mergeProgram);
		glUniform1i(glGetUniformLocation(*m_mergeProgram, "g_copy"), 0);
		glUniform1i(glGetUniformLocation(*m_mergeProgram, "g_data"), 1);
		m_mergeData = Framework::OpenGl::CTexture::Create();
		glBindTexture(GL_TEXTURE_2D, m_mergeData);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 128, 32);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		CHECKGLERROR();
	}
	uint32 scale = m_fbScale;
	if(m_mergeCopyScale != scale)
	{
		m_mergeCopy = Framework::OpenGl::CTexture::Create();
		glBindTexture(GL_TEXTURE_2D, m_mergeCopy);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64 * scale, 32 * scale);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		m_mergeCopyScale = scale;
		CHECKGLERROR();
	}

	auto offsets32 = CGsPixelFormats::CPixelIndexorPSMCT32::GetPageOffsets();
	std::vector<uint32> data(128 * 32);
	for(uint32 i = 0; i < m_mergePages.size(); i++)
	{
		auto [page, owner] = m_mergePages[i];
		const uint64* mask = masks.data() + (i * 256);
		uint64* ramCurrent = m_ramCurrent.data() + (page * 256);
		bool written = false;
		for(uint32 word = 0; word < 256; word++)
		{
			ramCurrent[word] |= mask[word];
			written |= (mask[word] != 0);
		}
		if(!written) continue;

		uint32 pageX = 0, pageY = 0;
		GetPagePosition(owner->m_basePtr, owner->m_width, owner->m_height, owner->m_psm, page, pageX, pageY);
		const uint8* pageMemory = m_pRAM + (page * CGsPixelFormats::PAGESIZE);
		for(uint32 y = 0; y < 32; y++)
		{
			for(uint32 x = 0; x < 64; x++)
			{
				uint32 offset = offsets32[(y * 64) + x];
				uint32 bits = static_cast<uint32>(mask[(offset * 2) / 64] >> ((offset * 2) % 64)) & 0xFF;
				uint32 bitMask = 0;
				for(uint32 nibble = 0; nibble < 8; nibble++)
				{
					if(bits & (1 << nibble)) bitMask |= 0xFU << (nibble * 4);
				}
				data[(y * 128) + x] = *reinterpret_cast<const uint32*>(pageMemory + offset);
				data[(y * 128) + 64 + x] = bitMask;
			}
		}

		m_validGlState &= ~(GLSTATE_BLEND | GLSTATE_COLORMASK | GLSTATE_SCISSOR | GLSTATE_PROGRAM | GLSTATE_TEXTURE);
		m_validGlState &= ~(GLSTATE_VIEWPORT | GLSTATE_DEPTHTEST | GLSTATE_DEPTHMASK | GLSTATE_FRAMEBUFFER);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m_mergeData);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 32, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_mergeCopy);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, owner->m_framebuffer);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pageX * scale, pageY * scale, 64 * scale, 32 * scale);

		glBindFramebuffer(GL_FRAMEBUFFER, owner->m_framebuffer);
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_SCISSOR_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDepthMask(GL_FALSE);
		glViewport(pageX * scale, pageY * scale, 64 * scale, 32 * scale);
		glUseProgram(*m_mergeProgram);
		glUniform4i(glGetUniformLocation(*m_mergeProgram, "g_params"), pageX * scale, pageY * scale, scale, 0);
		glBindVertexArray(m_copyToFbVertexArray);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		CHECKGLERROR();

		InvalidateByteViews(page);
	}
}

//The framebuffer that drew all the pages of a PSMT8/PSMT4 texture, if it is one the byte views can read.
CGSH_OpenGL::CFramebuffer* CGSH_OpenGL::GetByteViewSource(const TEX0& tex0, uint32 pageWidth, uint32 pageHeight)
{
	uint32 pagesPerRow = tex0.GetBufWidth() / pageWidth;
	uint32 pagesX = (std::min<uint32>(tex0.GetWidth(), TEX0_MAX_TEXTURE_SIZE) + pageWidth - 1) / pageWidth;
	uint32 pagesY = (std::min<uint32>(tex0.GetHeight(), TEX0_MAX_TEXTURE_SIZE) + pageHeight - 1) / pageHeight;
	uint32 firstPage = tex0.GetBufPtr() / CGsPixelFormats::PAGESIZE;
	CFramebuffer* source = nullptr;
	for(uint32 y = 0; y < pagesY; y++)
	{
		for(uint32 x = 0; x < pagesX; x++)
		{
			auto owner = m_gpuPages[(firstPage + (y * pagesPerRow) + x) % GPU_PAGE_COUNT].owner;
			if(!owner || (source && (owner != source))) return nullptr;
			source = owner;
		}
	}
	if((source->m_psm != PSMCT32) && (source->m_psm != PSMCT24)) return nullptr;
	if(((source->m_width % 64) != 0) || ((source->m_basePtr % CGsPixelFormats::PAGESIZE) != 0)) return nullptr;
	return source;
}

//A PSMT8/PSMT4 texture over pages a PSMCT32/PSMCT24 framebuffer drew (Gran Turismo 4 reads its frame so, a page at a
//time, and its HUD textures uploaded over its render targets): its texels are taken from the framebuffer on the GPU
//instead of bringing the framebuffer back to RAM.
bool CGSH_OpenGL::PrepareFramebufferByteTexture(const TEX0& tex0, TEXTURE_INFO& texInfo)
{
	uint32 viewIndex = 0;
	uint32 tileHeight = 0;
	if(tex0.nPsm == PSMT8)
	{
		viewIndex = 0;
		tileHeight = 64;
	}
	else if(tex0.nPsm == PSMT4)
	{
		viewIndex = 1;
		tileHeight = 128;
	}
	else
	{
		return false;
	}
	if(m_gpuPageCount == 0) return false;
	if(((tex0.GetBufPtr() % CGsPixelFormats::PAGESIZE) != 0) || (tex0.GetBufWidth() == 0) || ((tex0.GetBufWidth() % 128) != 0)) return false;
	if(!GetByteViewSource(tex0, 128, tileHeight)) return false;
	if(!m_vertexBuffer.empty() && m_renderState.isValid) FlushVertexBuffer();
	auto source = GetByteViewSource(tex0, 128, tileHeight);
	if(!source) return false;
	auto sourceIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(),
	                                   [source](const FramebufferPtr& candidate) { return candidate.get() == source; });
	if(sourceIterator == m_framebuffers.end()) return false;

	auto& view = m_byteViews[viewIndex];
	uint32 width = std::min<uint32>(tex0.GetWidth(), TEX0_MAX_TEXTURE_SIZE);
	uint32 height = std::min<uint32>(tex0.GetHeight(), TEX0_MAX_TEXTURE_SIZE);
	uint32 pagesPerRow = tex0.GetBufWidth() / 128;
	uint32 pagesX = (width + 127) / 128;
	uint32 pagesY = (height + tileHeight - 1) / tileHeight;
	uint32 firstPage = tex0.GetBufPtr() / CGsPixelFormats::PAGESIZE;
	bool valid = (view.source.get() == source);
	for(uint32 y = 0; valid && (y < pagesY); y++)
	{
		for(uint32 x = 0; x < pagesX; x++)
		{
			valid = valid && view.valid.test((firstPage + (y * pagesPerRow) + x) % GPU_PAGE_COUNT);
		}
	}
	if(!valid) BuildByteView(viewIndex, *sourceIterator);

	//A few textures of each size in turn, so that one isn't rewritten while a queued draw still reads it.
	auto& [textures, next] = m_byteViewTextures[(width << 16) | height];
	if(textures.size() < 32)
	{
		auto texture = Framework::OpenGl::CTexture::Create();
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width, height);
		textures.push_back(std::move(texture));
		next = static_cast<uint32>(textures.size() - 1);
	}
	else
	{
		next = (next + 1) % textures.size();
	}
	GLuint texture = textures[next];

	glBindFramebuffer(GL_READ_FRAMEBUFFER, view.framebuffer);
	glBindTexture(GL_TEXTURE_2D, texture);
	for(uint32 y = 0; y < pagesY; y++)
	{
		for(uint32 x = 0; x < pagesX; x++)
		{
			uint32 page = (firstPage + (y * pagesPerRow) + x) % GPU_PAGE_COUNT;
			uint32 pageX = 0, pageY = 0;
			if(!GetPagePosition(source->m_basePtr, source->m_width, source->m_height, source->m_psm, page, pageX, pageY)) continue;
			glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x * 128, y * tileHeight, (pageX / 64) * 128, (pageY / 32) * tileHeight,
			                    std::min<uint32>(128, width - (x * 128)), std::min<uint32>(tileHeight, height - (y * tileHeight)));
		}
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_renderState.framebufferHandle);
	CHECKGLERROR();

	texInfo.textureHandle = texture;
	return true;
}

//Lays out all of the framebuffer's bytes as PSMT8 pages (view 0) or its nibbles as PSMT4 pages (view 1).
void CGSH_OpenGL::BuildByteView(uint32 viewIndex, const FramebufferPtr& source)
{
	auto& view = m_byteViews[viewIndex];
	uint32 tileHeight = (viewIndex == 0) ? 64 : 128;
	if(m_multisampleEnabled) ResolveFramebufferMultisample(source, m_fbScale);
	uint32 pagesPerRow = source->m_width / 64;
	uint32 pageRows = (source->m_height + 31) / 32;
	uint32 width = pagesPerRow * 128;
	uint32 height = pageRows * tileHeight;

	if(!m_byteViewProgram)
	{
		m_byteViewProgram = GenerateByteViewProgram();
		glUseProgram(*m_byteViewProgram);
		glUniform1i(glGetUniformLocation(*m_byteViewProgram, "g_framebuffer"), 0);
		glUniform1i(glGetUniformLocation(*m_byteViewProgram, "g_layout"), 1);
	}

	if(static_cast<GLuint>(view.layout) == 0)
	{
		//Where each texel of a PSMT8/PSMT4 page is in a PSMCT32 page: pixel x, y, byte (and nibble, bit 2).
		auto offsets32 = CGsPixelFormats::CPixelIndexorPSMCT32::GetPageOffsets();
		std::vector<uint16> pixels(2048);
		for(uint32 i = 0; i < 64 * 32; i++)
		{
			pixels[offsets32[i] / 4] = static_cast<uint16>(((i / 64) << 8) | (i % 64));
		}
		std::vector<uint8> layout(128 * tileHeight * 4);
		auto offsets8 = CGsPixelFormats::CPixelIndexorPSMT8::GetPageOffsets();
		auto nibbles4 = GetPsmt4NibbleTable();
		for(uint32 i = 0; i < 128 * tileHeight; i++)
		{
			uint32 nibble = (viewIndex == 0) ? (offsets8[i] * 2) : nibbles4[i];
			uint32 byte = nibble / 2;
			uint16 pixel = pixels[byte / 4];
			layout[(i * 4) + 0] = static_cast<uint8>(pixel & 0xFF);
			layout[(i * 4) + 1] = static_cast<uint8>(pixel >> 8);
			layout[(i * 4) + 2] = static_cast<uint8>((byte % 4) | ((nibble & 1) << 2));
			layout[(i * 4) + 3] = 0;
		}
		view.layout = Framework::OpenGl::CTexture::Create();
		glBindTexture(GL_TEXTURE_2D, view.layout);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8UI, 128, tileHeight);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, tileHeight, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, layout.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		CHECKGLERROR();
	}

	if((width != view.width) || (height != view.height))
	{
		view.texture = Framework::OpenGl::CTexture::Create();
		glBindTexture(GL_TEXTURE_2D, view.texture);
		glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, width, height);
		view.framebuffer = Framework::OpenGl::CFramebuffer::Create();
		glBindFramebuffer(GL_FRAMEBUFFER, view.framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, view.texture, 0);
		CHECKGLERROR();
		view.width = width;
		view.height = height;
	}

	m_validGlState &= ~(GLSTATE_BLEND | GLSTATE_COLORMASK | GLSTATE_SCISSOR | GLSTATE_PROGRAM | GLSTATE_TEXTURE);
	m_validGlState &= ~(GLSTATE_VIEWPORT | GLSTATE_DEPTHTEST | GLSTATE_DEPTHMASK | GLSTATE_FRAMEBUFFER);

	glBindFramebuffer(GL_FRAMEBUFFER, view.framebuffer);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_FALSE);
	glViewport(0, 0, width, height);
	glUseProgram(*m_byteViewProgram);
	glUniform1i(glGetUniformLocation(*m_byteViewProgram, "g_scale"), m_fbScale);
	glUniform1i(glGetUniformLocation(*m_byteViewProgram, "g_nibbles"), viewIndex);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, view.layout);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source->m_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
	glBindVertexArray(m_copyToFbVertexArray);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	CHECKGLERROR();

	view.source = source;
	view.valid.reset();
	uint32 basePage = source->m_basePtr / CGsPixelFormats::PAGESIZE;
	for(uint32 page = 0; page < (pagesPerRow * pageRows); page++)
	{
		view.valid.set((basePage + page) % GPU_PAGE_COUNT);
	}
}

//A 16-color CLUT taken from a CLUT the GPU drew: its entries are read back (rare, it stalls).
void CGSH_OpenGL::ReadGpuClut()
{
	MaterializeGpuClut();
	m_gpuClutActive = false;
	uint32 colors[256];
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_gpuClutFramebuffer);
	glReadPixels(0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, colors);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, m_renderState.framebufferHandle);
	for(unsigned int i = 0; i < 256; i++)
	{
		m_pCLUT[i + 0x000] = static_cast<uint16>(colors[i] & 0xFFFF);
		m_pCLUT[i + 0x100] = static_cast<uint16>(colors[i] >> 16);
	}
	ProcessClutTransfer(0, 0);
}
