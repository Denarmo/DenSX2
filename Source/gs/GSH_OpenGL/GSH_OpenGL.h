#pragma once

#include <list>
#include <bitset>
#include <map>
#include <unordered_map>
#include "../GSHandler.h"
#include "../GsDebuggerInterface.h"
#include "../GsCachedArea.h"
#include "../GsTextureCache.h"
#include "opengl/OpenGlDef.h"
#include "opengl/Program.h"
#include "opengl/Shader.h"
#include "opengl/Resource.h"

#define PREF_CGSH_OPENGL_RESOLUTION_FACTOR "renderer.opengl.resfactor"
#define PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES "renderer.opengl.forcebilineartextures"

#if !defined(GLES_COMPATIBILITY) && !defined(__APPLE__)
//- Dual source blending is disabled on macOS because it seems to be problematic on
//  certain Intel GPUs drivers.
#define USE_DUALSOURCE_BLENDING
#endif

class CGSH_OpenGL : public CGSHandler, public CGsDebuggerInterface
{
public:
	CGSH_OpenGL(bool = true);
	virtual ~CGSH_OpenGL();

	static void RegisterPreferences();
	//Compiled programs are kept in this file between runs (empty: not kept). Set before the GS initializes.
	static void SetProgramCachePath(const std::string&);

	void LoadState(Framework::CZipArchiveReader&) override;

	void ProcessHostToLocalTransfer() override;
	void ProcessLocalToHostTransfer() override;
	void ProcessLocalToLocalTransfer() override;
	void ProcessClutTransfer(uint32, uint32) override;

	Framework::CBitmap GetScreenshot() override;

	//Debugger Interface
	bool GetDepthTestingEnabled() const override;
	void SetDepthTestingEnabled(bool) override;

	bool GetAlphaBlendingEnabled() const override;
	void SetAlphaBlendingEnabled(bool) override;

	bool GetAlphaTestingEnabled() const override;
	void SetAlphaTestingEnabled(bool) override;

	Framework::CBitmap GetFramebuffer(uint64) override;
	Framework::CBitmap GetDepthbuffer(uint64, uint64) override;
	Framework::CBitmap GetTexture(uint64, uint32, uint64, uint64, uint32) override;
	int GetFramebufferScale() override;

	const VERTEX* GetInputVertices() const override;

protected:
	void PalCache_Flush();
	void LoadPreferences();
	void InitializeImpl() override;
	void ReleaseImpl() override;
	void ResetImpl() override;
	void NotifyPreferencesChangedImpl() override;
	void MarkNewFrame() override;
	void FlipImpl(const DISPLAY_INFO&) override;

	GLuint m_presentFramebuffer = 0;

private:
	typedef CGsTextureCache<Framework::OpenGl::CTexture> TextureCache;
	typedef uint64 ShaderCapsInt;

	struct SHADERCAPS : public convertible<ShaderCapsInt>
	{
		unsigned int texFunction : 2; //0 - Modulate, 1 - Decal, 2 - Highlight, 3 - Hightlight2
		unsigned int texClampS : 3;
		unsigned int texClampT : 3;
		unsigned int texSourceMode : 2;
		unsigned int texHasAlpha : 1;
		unsigned int texBilinearFilter : 1;
		unsigned int texUseAlphaExpansion : 1;
		unsigned int texBlackIsTransparent : 1;
		unsigned int hasFog : 1;

		//Alpha blending caps
		unsigned int hasAlphaBlend : 1;
		unsigned int alphaBlendA : 2;
		unsigned int alphaBlendB : 2;
		unsigned int alphaBlendC : 2;
		unsigned int alphaBlendD : 2;
		unsigned int colorOutputWhite : 1;

		//Alpha testing caps
		unsigned int hasAlphaTest : 1;
		unsigned int alphaTestMethod : 3;
		unsigned int hasDestAlphaTest : 1;
		unsigned int destAlphaTestRef : 1;
		unsigned int alphaFailMethod : 2;
		unsigned int alphaTestDepthTest_DepthFetch : 1;
		unsigned int alphaTestDepthTestEqual_DepthFetch : 1;

		//The 256-colour palette is a CLUT in a framebuffer, read by the shader where it is
		unsigned int texPaletteFromFramebuffer : 1;

		bool isIndexedTextureSource() const
		{
			return texSourceMode == TEXTURE_SOURCE_MODE_IDX4 || texSourceMode == TEXTURE_SOURCE_MODE_IDX8;
		}
	};
	static_assert(sizeof(SHADERCAPS) == sizeof(ShaderCapsInt), "SHADERCAPS too big for ShaderCapsInt.");

	struct RENDERSTATE
	{
		bool isValid;
		bool isTextureStateValid;
		bool isFramebufferStateValid;

		//Register State
		uint64 primReg;
		uint64 frameReg;
		uint64 testReg;
		uint64 alphaReg;
		uint64 zbufReg;
		uint64 scissorReg;
		uint64 tex0Reg;
		uint64 tex1Reg;
		uint64 texAReg;
		uint64 clampReg;
		uint64 fogColReg;

		//Intermediate State
		SHADERCAPS shaderCaps;

		//OpenGL state
		GLuint shaderHandle;
		GLuint framebufferHandle;
		GLuint texture0Handle;
		GLint texture0MinFilter;
		GLint texture0MagFilter;
		GLint texture0WrapS;
		GLint texture0WrapT;
		bool texture0AlphaAsIndex;
		GLuint texture1Handle;
		GLsizei viewportWidth;
		GLsizei viewportHeight;
		GLint scissorX;
		GLint scissorY;
		GLsizei scissorWidth;
		GLsizei scissorHeight;
		bool blendEnabled;
		bool colorMaskR;
		bool colorMaskG;
		bool colorMaskB;
		bool colorMaskA;
		bool depthMask;
		bool depthTest;
	};

	//These need to match the layout of the shader's uniform block
	struct VERTEXPARAMS
	{
		float projMatrix[16];
		float texMatrix[16];
	};
	static_assert(sizeof(VERTEXPARAMS) == 0x80, "Size of VERTEXPARAMS must be 128 bytes.");

	struct FRAGMENTPARAMS
	{
		float textureSize[2];
		float texelSize[2];
		float clampMin[2];
		float clampMax[2];
		float texA0;
		float texA1;
		uint32 alphaRef;
		float alphaFix;
		float fogColor[3];
		float padding2;
		//Where the four blocks of a palette read from a framebuffer are (x, y pairs), and the framebuffer's scale
		int32 clutOrigins[8];
		int32 clutScale[4];
	};
	static_assert(sizeof(FRAGMENTPARAMS) == 0x70, "Size of FRAGMENTPARAMS must be 112 bytes.");

	enum
	{
		MAX_TEXTURE_CACHE = 256,
		MAX_PALETTE_CACHE = 256,
	};

	enum CVTBUFFERSIZE
	{
		CVTBUFFERSIZE = 0x800000,
	};

	typedef void (CGSH_OpenGL::*TEXTUREUPDATER)(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);

	enum
	{
		TEXTURE_SOURCE_MODE_NONE = 0,
		TEXTURE_SOURCE_MODE_STD = 1,
		TEXTURE_SOURCE_MODE_IDX4 = 2,
		TEXTURE_SOURCE_MODE_IDX8 = 3
	};

	enum TEXTURE_CLAMP_MODE
	{
		TEXTURE_CLAMP_MODE_STD,
		TEXTURE_CLAMP_MODE_CLAMP,
		TEXTURE_CLAMP_MODE_REGION_CLAMP,
		TEXTURE_CLAMP_MODE_REGION_REPEAT,
		TEXTURE_CLAMP_MODE_REGION_REPEAT_SIMPLE
	};

	typedef std::unordered_map<ShaderCapsInt, Framework::OpenGl::ProgramPtr> ShaderMap;

	class CPalette
	{
	public:
		CPalette();
		~CPalette();

		void Free();

		bool m_live;
		uint32 m_generation = 0;

		bool m_isIDTEX4;
		uint32 m_cpsm;
		uint32 m_csa;
		GLuint m_texture;
		uint32 m_contents[256];
	};
	typedef std::shared_ptr<CPalette> PalettePtr;
	typedef std::list<PalettePtr> PaletteList;

	class CFramebuffer
	{
	public:
		CFramebuffer(uint32, uint32, uint32, uint32, uint32, bool);
		~CFramebuffer() = default;

		uint32 m_basePtr;
		uint32 m_width;
		uint32 m_height;
		uint32 m_psm;

		Framework::OpenGl::CFramebuffer m_framebuffer;
		Framework::OpenGl::CTexture m_texture;

		Framework::OpenGl::CFramebuffer m_resolveFramebuffer;
		bool m_resolveNeeded = false;
		Framework::OpenGl::CRenderbuffer m_colorBufferMs;

		CGsCachedArea m_cachedArea;
	};
	typedef std::shared_ptr<CFramebuffer> FramebufferPtr;
	typedef std::vector<FramebufferPtr> FramebufferList;

	class CDepthbuffer
	{
	public:
		CDepthbuffer(uint32, uint32, uint32, uint32, uint32, bool);
		~CDepthbuffer() = default;

		uint32 m_basePtr;
		uint32 m_width;
		uint32 m_height;
		uint32 m_psm;
		Framework::OpenGl::CRenderbuffer m_depthBuffer;
	};
	typedef std::shared_ptr<CDepthbuffer> DepthbufferPtr;
	typedef std::vector<DepthbufferPtr> DepthbufferList;

	struct TEXTURE_INFO
	{
		GLuint textureHandle = 0;
		float offsetX = 0;
		float scaleRatioX = 1;
		float scaleRatioY = 1;
		bool alphaAsIndex = false;
	};

	struct TEXTUREFORMAT_INFO
	{
		GLenum internalFormat;
		GLenum format;
		GLenum type;
	};

	enum class PRIM_VERTEX_ATTRIB
	{
		POSITION = 1,
		DEPTH,
		COLOR,
		TEXCOORD,
		FOG,
	};

	struct PRIM_VERTEX
	{
		float x, y;
		uint32 z;
		uint32 color;
		float s, t, q;
		float f;
	};

	enum VERTEX_BUFFER_SIZE
	{
		VERTEX_BUFFER_SIZE = 0x1000,
	};

	typedef std::vector<PRIM_VERTEX> VertexBuffer;

	void WriteRegisterImpl(uint8, uint64) override;

	void InitializeRC();
	void CheckExtensions();
	void SetupTextureUpdaters();
	virtual void PresentBackbuffer() = 0;
	void MakeLinearZOrtho(float*, float, float, float, float);
	TEXTURE_INFO PrepareTexture(const TEX0&);
	TEXTURE_INFO SearchTextureFramebuffer(const TEX0&);
	GLuint PreparePalette(const TEX0&);

	void ProcessPrim(uint64);
	void VertexKick(uint8, uint64);

	Framework::OpenGl::ProgramPtr GetShaderFromCaps(const SHADERCAPS&);
	void SyncMemoryForRead(uint32, uint32) override;
	void WriteBackMemoryCache() override;
	void BeginTransferWrite() override;
	void SyncCLUT(const TEX0&) override;
	bool SyncClutOnGpu(const TEX0&);
	void ReadGpuClut();
	void MaterializeGpuClut();
	void ReleaseGpuClutSource(const CFramebuffer*);
	Framework::OpenGl::ProgramPtr GenerateGpuClutProgram();
	Framework::OpenGl::ProgramPtr GenerateByteViewProgram();
	bool PrepareFramebufferByteTexture(const TEX0&, TEXTURE_INFO&);
	CFramebuffer* GetByteViewSource(const TEX0&);
	void BuildByteView(const FramebufferPtr&);
	void MarkGpuPages();
	void SyncTextureArea(const TEX0&);
	void SyncFramebufferToRam(CFramebuffer*, uint32 = 0, uint32 = ~0U);
	void ClearGpuPages(uint32, uint32);
	void ClearAllGpuPages();
	void UploadFramebufferPages(const FramebufferPtr&, const std::vector<uint32>&);
	bool MoveGpuPage(CFramebuffer*, CFramebuffer*, uint32);

	Framework::OpenGl::ProgramPtr GenerateShader(const SHADERCAPS&);
	void LoadProgramCache();
	Framework::OpenGl::ProgramPtr LoadCachedProgram(ShaderCapsInt);
	void SaveCachedProgram(ShaderCapsInt, GLuint);
	Framework::OpenGl::CShader GenerateVertexShader(const SHADERCAPS&);
	Framework::OpenGl::CShader GenerateFragmentShader(const SHADERCAPS&);
	std::string GenerateTexCoordClampingSection(TEXTURE_CLAMP_MODE, const char*);
	std::string GenerateAlphaTestSection(ALPHA_TEST_METHOD, ALPHA_TEST_FAIL_METHOD);
	std::string GenerateAlphaBlendSection(ALPHABLEND_ABD, ALPHABLEND_ABD, ALPHABLEND_C, ALPHABLEND_ABD);

	Framework::OpenGl::ProgramPtr GeneratePresentProgram();
	Framework::OpenGl::CBuffer GeneratePresentVertexBuffer();
	Framework::OpenGl::CVertexArray GeneratePresentVertexArray();

	Framework::OpenGl::ProgramPtr GenerateCopyToFbProgram();
	Framework::OpenGl::CBuffer GenerateCopyToFbVertexBuffer();
	Framework::OpenGl::CVertexArray GenerateCopyToFbVertexArray();

	Framework::OpenGl::CVertexArray GeneratePrimVertexArray();
	Framework::OpenGl::CBuffer GenerateUniformBlockBuffer(size_t);

	void Prim_Point();
	void Prim_Line();
	void Prim_Triangle();
	void Prim_Sprite();

	void FlushVertexBuffer();
	void DoRenderPass();

	void CopyToFb(int32, int32, int32, int32, int32, int32, int32, int32, int32, int32, bool = true);
	void DrawToDepth(unsigned int, uint64);

	void SetRenderingContext(uint64);
	void SetupTestFunctions(uint64);
	void SetupDepthBuffer(uint64, uint64);
	void SetupFramebuffer(uint64, uint64, uint64, uint64);
	void SetupBlendingFunction(uint64);
	void SetupFogColor(uint64);

	static bool CanRegionRepeatClampModeSimplified(uint32, uint32);
	bool RequiresAlphaTestDepthTest_DepthFetch(const TEST&) const;
	void FillShaderCapsFromTexture(SHADERCAPS&, const uint64&, const uint64&, const uint64&, const uint64&);
	void FillShaderCapsFromTest(SHADERCAPS&, const uint64&);
	void FillShaderCapsFromAlpha(SHADERCAPS&, bool, const uint64&);

	void SetupTexture(uint64, uint64, uint64, uint64, uint64);
	static uint32 GetFramebufferBitDepth(uint32);
	static TEXTUREFORMAT_INFO GetTextureFormatInfo(uint32);

	FramebufferPtr FindFramebuffer(const FRAME&) const;
	DepthbufferPtr FindDepthbuffer(const ZBUF&, const FRAME&) const;

	void DumpTexture(unsigned int, unsigned int, uint32);

	Framework::CBitmap GetFramebufferImpl(uint64);
	Framework::CBitmap GetTextureImpl(uint64, uint32, uint64, uint64, uint32);

	//Texture updaters
	void TexUpdater_Invalid(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);

	void TexUpdater_Psm32(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);
	template <typename>
	void TexUpdater_Psm16(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);

	void TexUpdater_Psm8(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);
	void TexUpdater_Psm4(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);

	template <typename>
	void TexUpdater_Psm48(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);
	template <uint32, uint32>
	void TexUpdater_Psm48H(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);

	//Context variables (put this in a struct or something?)
	float m_nPrimOfsX;
	float m_nPrimOfsY;
	uint32 m_nTexWidth;
	uint32 m_nTexHeight;

	bool m_forceBilinearTextures = false;
	unsigned int m_fbScale = 1;
	bool m_multisampleEnabled = false;
	bool m_depthTestingEnabled = true;
	bool m_alphaBlendingEnabled = true;
	bool m_alphaTestingEnabled = true;

	uint8* m_pCvtBuffer;

	GLuint PalCache_Search(const TEX0&);
	GLuint PalCache_Search(unsigned int, const uint32*);
	void PalCache_Insert(const TEX0&, const uint32*, GLuint);
	void PalCache_Invalidate(uint32);

	void PopulateFramebuffer(const FramebufferPtr&);
	void CommitFramebufferDirtyPages(const FramebufferPtr&, unsigned int, unsigned int);
	void ResolveFramebufferMultisample(const FramebufferPtr&, uint32);

	Framework::OpenGl::ProgramPtr m_presentProgram;
	Framework::OpenGl::CBuffer m_presentVertexBuffer;
	Framework::OpenGl::CVertexArray m_presentVertexArray;
	GLint m_presentTextureUniform = -1;
	GLint m_presentTexCoordScaleUniform = -1;

	Framework::OpenGl::ProgramPtr m_copyToFbProgram;
	Framework::OpenGl::CTexture m_copyToFbTexture;
	Framework::OpenGl::CBuffer m_copyToFbVertexBuffer;
	Framework::OpenGl::CVertexArray m_copyToFbVertexArray;
	GLint m_copyToFbSrcPositionUniform = -1;
	GLint m_copyToFbSrcSizeUniform = -1;

	TextureCache m_textureCache;
	PaletteList m_paletteCache;
	//A CLUT transfer makes every palette stale: they stay live only while their generation is current.
	uint32 m_paletteGeneration = 0;
	FramebufferList m_framebuffers;
	//Bumped whenever m_framebuffers changes (framebuffers are only added or all cleared).
	uint32 m_framebuffersVersion = 0;

	//Which framebuffer a texture's address, width and format resolve to, for the current framebuffer list.
	struct TEXTURE_FRAMEBUFFER_LOOKUP
	{
		uint64 key = ~0ULL;
		uint32 version = ~0U;
		int32 framebufferIndex = -1;
		float offsetX = 0;
		bool alphaAsIndex = false;
	};
	enum
	{
		TEXTURE_FRAMEBUFFER_LOOKUPS = 64,
	};
	TEXTURE_FRAMEBUFFER_LOOKUP m_textureFramebufferLookups[TEXTURE_FRAMEBUFFER_LOOKUPS];
	DepthbufferList m_depthbuffers;

	//GS memory pages drawn on the GPU that RAM doesn't have yet, with the framebuffer holding them. A texture or
	//CLUT read from RAM over such a page copies the framebuffer back first (Gran Turismo 4 reads its frame as
	//PSMT8 through CLUTs it renders).
	enum
	{
		GPU_PAGE_COUNT = RAMSIZE / 0x2000, //CGsPixelFormats::PAGESIZE
	};
	struct GPU_PAGE
	{
		CFramebuffer* owner = nullptr;      //The framebuffer that drew in it
		CFramebuffer* syncedFrom = nullptr; //RAM has the page as this framebuffer has it
		uint32 drawnBlocks = 0;
		uint32 channels = 0;                //The bits of each pixel the draws wrote
	};
	GPU_PAGE m_gpuPages[GPU_PAGE_COUNT];
	uint32 m_gpuPageCount = 0;
	std::vector<std::pair<uint32, uint32>> m_blockList;
	std::vector<std::pair<uint32, uint32>> m_wholeBlockList;
	std::vector<std::pair<uint32, uint32>> m_drawBlockList;
	std::vector<uint32> m_readbackBuffer;
	CFramebuffer* m_drawFramebuffer = nullptr;
	FramebufferPtr m_drawFramebufferPtr;
	uint32 m_drawMask = 0;
	Framework::OpenGl::ProgramPtr m_gpuClutProgram;
	GLint m_gpuClutPositionUniform = -1;
	Framework::OpenGl::CTexture m_gpuClutTexture;
	Framework::OpenGl::CFramebuffer m_gpuClutFramebuffer;
	bool m_gpuClutActive = false;
	//While set, the palette is the CLUT at m_gpuClutPosition in this framebuffer, not yet copied.
	FramebufferPtr m_gpuClutSource;
	int32 m_gpuClutPosition[4] = {};
	//PSMT8 reads of a framebuffer the GPU drew: its bytes laid out as PSMT8 pages, a 128x64 tile for each of its
	//64x32 pages, valid for the pages no draw changed since.
	FramebufferPtr m_byteViewSource;
	Framework::OpenGl::CTexture m_byteViewTexture;
	Framework::OpenGl::CFramebuffer m_byteViewFramebuffer;
	uint32 m_byteViewWidth = 0;
	uint32 m_byteViewHeight = 0;
	std::bitset<GPU_PAGE_COUNT> m_byteViewValid;
	Framework::OpenGl::ProgramPtr m_byteViewProgram;
	Framework::OpenGl::CTexture m_byteViewLayout;
	std::map<uint32, std::pair<std::vector<Framework::OpenGl::CTexture>, uint32>> m_byteViewTextures;

	Framework::OpenGl::CBuffer m_primBuffer;
	Framework::OpenGl::CVertexArray m_primVertexArray;

	VERTEX m_VtxBuffer[3];
	int m_nVtxCount;

	bool m_pendingPrim = false;
	uint64 m_pendingPrimValue = 0;
	PRMODE m_PrimitiveMode;
	unsigned int m_primitiveType;
	bool m_drawingToDepth = false;

	static const GLenum g_nativeClampModes[CGSHandler::CLAMP_MODE_MAX];
	static const unsigned int g_shaderClampModes[CGSHandler::CLAMP_MODE_MAX];
	static const unsigned int g_alphaTestInverse[CGSHandler::ALPHA_TEST_MAX];

	TEXTUREUPDATER m_textureUpdater[CGSHandler::PSM_MAX];

	enum GLSTATE_BITS : uint32
	{
		GLSTATE_VERTEX_PARAMS = 0x0001,
		GLSTATE_FRAGMENT_PARAMS = 0x0002,
		GLSTATE_PROGRAM = 0x0004,
		GLSTATE_SCISSOR = 0x0008,
		GLSTATE_BLEND = 0x0010,
		GLSTATE_COLORMASK = 0x0020,
		GLSTATE_DEPTHMASK = 0x0040,
		GLSTATE_TEXTURE = 0x0080,
		GLSTATE_FRAMEBUFFER = 0x0100,
		GLSTATE_VIEWPORT = 0x0200,
		GLSTATE_DEPTHTEST = 0x0400,
	};

	ShaderMap m_shaders;

	struct PROGRAM_BINARY
	{
		uint32 format = 0;
		std::vector<uint8> data;
	};
	std::unordered_map<ShaderCapsInt, PROGRAM_BINARY> m_programBinaries;
	bool m_programBinariesSupported = false;
	RENDERSTATE m_renderState;
	uint32 m_validGlState = 0;
	VERTEXPARAMS m_vertexParams;
	FRAGMENTPARAMS m_fragmentParams;
	Framework::OpenGl::CBuffer m_vertexParamsBuffer;
	Framework::OpenGl::CBuffer m_fragmentParamsBuffer;
	VertexBuffer m_vertexBuffer;

	//Draw data is appended to buffers allocated once instead of giving each draw new storage,
	//which drivers such as Adreno's turn into a GPU memory allocation per draw.
	enum
	{
		STREAM_SEGMENTS = 8,
	};
	struct STREAM_BUFFER
	{
		GLsizeiptr size = 0;
		GLintptr offset = 0;
		uint8* mapped = nullptr;
		unsigned int segment = 0;
		GLsync fences[STREAM_SEGMENTS] = {};
	};
	void CreateStreamBuffer(GLuint, STREAM_BUFFER&, GLenum, GLsizeiptr);
	void ReleaseStreamBuffer(STREAM_BUFFER&);
	GLintptr StreamData(GLuint, STREAM_BUFFER&, GLenum, const void*, GLsizeiptr, GLsizeiptr);
	void UploadCvtBuffer(unsigned int, unsigned int, unsigned int, unsigned int, GLenum, GLenum, unsigned int);
	bool m_hasBufferStorageExtension = false;

	STREAM_BUFFER m_primStream;
	STREAM_BUFFER m_vertexParamsStream;
	STREAM_BUFFER m_fragmentParamsStream;
	Framework::OpenGl::CBuffer m_unpackBuffer;
	STREAM_BUFFER m_unpackStream;
	GLintptr m_vertexParamsOffset = 0;
	GLintptr m_fragmentParamsOffset = 0;
	GLint m_uniformBufferAlignment = 256;

	//If GPU has framebuffer fetch extension, some things will be done
	//within the shader, such alpha blending
	bool m_hasFramebufferFetchExtension = false;
	bool m_hasFramebufferFetchDepthExtension = false;
};
