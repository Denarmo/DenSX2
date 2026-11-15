#include "GSH_OpenGL.h"
#include <assert.h>
#include <cstdio>
#include <cstring>
#include <sstream>
#include "Log.h"

#define LOG_NAME "gsh_opengl"

#ifdef GLES_COMPATIBILITY
#define GLSL_VERSION "#version 300 es"
#else
#define GLSL_VERSION "#version 150"
#endif

static const char* s_andFunction =
    "float and(int a, int b)\r\n"
    "{\r\n"
    "	int r = 0;\r\n"
    "	int ha, hb;\r\n"
    "	\r\n"
    "	int m = int(min(float(a), float(b)));\r\n"
    "	\r\n"
    "	for(int k = 1; k <= m; k *= 2)\r\n"
    "	{\r\n"
    "		ha = a / 2;\r\n"
    "		hb = b / 2;\r\n"
    "		if(((a - ha * 2) != 0) && ((b - hb * 2) != 0))\r\n"
    "		{\r\n"
    "			r += k;\r\n"
    "		}\r\n"
    "		a = ha;\r\n"
    "		b = hb;\r\n"
    "	}\r\n"
    "	\r\n"
    "	return float(r);\r\n"
    "}\r\n";

static const char* s_orFunction =
    "float or(int a, int b)\r\n"
    "{\r\n"
    "	int r = 0;\r\n"
    "	int ha, hb;\r\n"
    "	\r\n"
    "	int m = int(max(float(a), float(b)));\r\n"
    "	\r\n"
    "	for(int k = 1; k <= m; k *= 2)\r\n"
    "	{\r\n"
    "		ha = a / 2;\r\n"
    "		hb = b / 2;\r\n"
    "		if(((a - ha * 2) != 0) || ((b - hb * 2) != 0))\r\n"
    "		{\r\n"
    "			r += k;\r\n"
    "		}\r\n"
    "		a = ha;\r\n"
    "		b = hb;\r\n"
    "	}\r\n"
    "	\r\n"
    "	return float(r);\r\n"
    "}\r\n";

namespace
{
	std::string g_programCachePath;
	const char g_programCacheMagic[8] = {'P', 'L', 'A', 'Y', 'G', 'L', 'P', '1'};
	const size_t g_programCacheMaxSize = 64 * 1024 * 1024;

	bool ReadExact(FILE* input, void* data, size_t size)
	{
		return fread(data, 1, size, input) == size;
	}

	bool WriteProgramBinary(FILE* output, uint64 caps, uint32 format, const std::vector<uint8>& data)
	{
		uint32 size = static_cast<uint32>(data.size());
		return (fwrite(&caps, sizeof(caps), 1, output) == 1) &&
		       (fwrite(&format, sizeof(format), 1, output) == 1) &&
		       (fwrite(&size, sizeof(size), 1, output) == 1) &&
		       (fwrite(data.data(), 1, size, output) == size);
	}

	std::string GetGlString(GLenum name)
	{
		auto value = reinterpret_cast<const char*>(glGetString(name));
		return value ? value : "";
	}
}

void CGSH_OpenGL::SetProgramCachePath(const std::string& path)
{
	g_programCachePath = path;
}

void CGSH_OpenGL::LoadProgramCache()
{
	m_programBinaries.clear();
	m_programBinariesSupported = false;
	if(g_programCachePath.empty()) return;
#ifdef GLEW_VERSION
	if(!glGetProgramBinary || !glProgramBinary || !glProgramParameteri) return;
#endif
	GLint formatCount = 0;
	glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formatCount);
	if(formatCount <= 0) return;

	//Binaries are only good for the build and driver that made them.
	std::string key = std::string(PLAY_VERSION) + "|" + GetGlString(GL_VENDOR) + "|" + GetGlString(GL_RENDERER) + "|" +
	                  GetGlString(GL_VERSION) + "|shaders 2|" + std::to_string(m_hasFramebufferFetchExtension) +
	                  std::to_string(m_hasFramebufferFetchDepthExtension);

	if(FILE* input = fopen(g_programCachePath.c_str(), "rb"))
	{
		char magic[sizeof(g_programCacheMagic)] = {};
		uint32 keySize = 0;
		std::string fileKey;
		bool valid = ReadExact(input, magic, sizeof(magic)) && !memcmp(magic, g_programCacheMagic, sizeof(magic)) &&
		             ReadExact(input, &keySize, sizeof(keySize)) && (keySize < 0x1000);
		if(valid)
		{
			fileKey.resize(keySize);
			valid = ReadExact(input, &fileKey[0], keySize) && (fileKey == key);
		}
		size_t totalSize = 0;
		while(valid)
		{
			uint64 caps = 0;
			uint32 format = 0, size = 0;
			if(!ReadExact(input, &caps, sizeof(caps)) || !ReadExact(input, &format, sizeof(format)) ||
			   !ReadExact(input, &size, sizeof(size))) break;
			totalSize += size;
			if(totalSize > g_programCacheMaxSize) break;
			PROGRAM_BINARY binary;
			binary.format = format;
			binary.data.resize(size);
			if(!ReadExact(input, binary.data.data(), size)) break;
			m_programBinaries[caps] = std::move(binary);
		}
		fclose(input);
	}

	//Rewritten whole: drops a record cut short and binaries of another build or driver.
	FILE* output = fopen(g_programCachePath.c_str(), "wb");
	if(!output)
	{
		CLog::GetInstance().Warn(LOG_NAME, "Can't write the program cache '%s'.\r\n", g_programCachePath.c_str());
		return;
	}
	uint32 keySize = static_cast<uint32>(key.size());
	bool written = (fwrite(g_programCacheMagic, sizeof(g_programCacheMagic), 1, output) == 1) &&
	               (fwrite(&keySize, sizeof(keySize), 1, output) == 1) && (fwrite(key.data(), 1, keySize, output) == keySize);
	for(const auto& binary : m_programBinaries)
	{
		written = written && WriteProgramBinary(output, binary.first, binary.second.format, binary.second.data);
	}
	fclose(output);
	m_programBinariesSupported = written;
	CLog::GetInstance().Print(LOG_NAME, "Program cache: %d programs.\r\n", static_cast<int>(m_programBinaries.size()));
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::LoadCachedProgram(ShaderCapsInt caps)
{
	auto binaryIterator = m_programBinaries.find(caps);
	if(binaryIterator == m_programBinaries.end()) return Framework::OpenGl::ProgramPtr();
	const auto& binary = binaryIterator->second;
	auto program = std::make_shared<Framework::OpenGl::CProgram>();
	glProgramBinary(*program, binary.format, binary.data.data(), static_cast<GLsizei>(binary.data.size()));
	GLint status = GL_FALSE;
	glGetProgramiv(*program, GL_LINK_STATUS, &status);
	if(status != GL_TRUE)
	{
		//The driver refused it (an update can do that): compile it again.
		while(glGetError() != GL_NO_ERROR)
		{
		}
		m_programBinaries.erase(binaryIterator);
		return Framework::OpenGl::ProgramPtr();
	}
	return program;
}

void CGSH_OpenGL::SaveCachedProgram(ShaderCapsInt caps, GLuint program)
{
	if(!m_programBinariesSupported) return;
	GLint length = 0;
	glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
	if(length <= 0) return;
	PROGRAM_BINARY binary;
	binary.data.resize(length);
	GLsizei writtenLength = 0;
	GLenum format = 0;
	glGetProgramBinary(program, length, &writtenLength, &format, binary.data.data());
	if(writtenLength <= 0) return;
	binary.data.resize(writtenLength);
	binary.format = format;
	if(FILE* output = fopen(g_programCachePath.c_str(), "ab"))
	{
		WriteProgramBinary(output, caps, binary.format, binary.data);
		fclose(output);
	}
	m_programBinaries[caps] = std::move(binary);
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateShader(const SHADERCAPS& caps)
{
	auto vertexShader = GenerateVertexShader(caps);
	auto fragmentShader = GenerateFragmentShader(caps);

	auto result = std::make_shared<Framework::OpenGl::CProgram>();
	if(m_programBinariesSupported)
	{
		glProgramParameteri(*result, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	}

	result->AttachShader(vertexShader);
	result->AttachShader(fragmentShader);

	glBindAttribLocation(*result, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), "a_position");
	glBindAttribLocation(*result, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::DEPTH), "a_depth");
	glBindAttribLocation(*result, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::COLOR), "a_color");
	glBindAttribLocation(*result, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), "a_texCoord");
	glBindAttribLocation(*result, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::FOG), "a_fog");

#ifdef USE_DUALSOURCE_BLENDING
	glBindFragDataLocationIndexed(*result, 0, 0, "fragColor");
	glBindFragDataLocationIndexed(*result, 0, 1, "blendColor");
#endif

	FRAMEWORK_MAYBE_UNUSED bool linkResult = result->Link();
	assert(linkResult);

	CHECKGLERROR();

	return result;
}

Framework::OpenGl::CShader CGSH_OpenGL::GenerateVertexShader(const SHADERCAPS& caps)
{
	std::stringstream shaderBuilder;
	shaderBuilder << GLSL_VERSION << std::endl;

	shaderBuilder << "layout(std140) uniform VertexParams" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	mat4 g_projMatrix;" << std::endl;
	shaderBuilder << "	mat4 g_texMatrix;" << std::endl;
	shaderBuilder << "};" << std::endl;

	shaderBuilder << "in vec2 a_position;" << std::endl;
	shaderBuilder << "in uint a_depth;" << std::endl;
	shaderBuilder << "in vec4 a_color;" << std::endl;
	shaderBuilder << "in vec3 a_texCoord;" << std::endl;

	shaderBuilder << "out float v_depth;" << std::endl;
	shaderBuilder << "out vec4 v_color;" << std::endl;
	shaderBuilder << "out vec3 v_texCoord;" << std::endl;
	if(caps.hasFog)
	{
		shaderBuilder << "in float a_fog;" << std::endl;
		shaderBuilder << "out float v_fog;" << std::endl;
	}

	shaderBuilder << "void main()" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	vec4 texCoord = g_texMatrix * vec4(a_texCoord, 1);" << std::endl;
	shaderBuilder << "	v_depth = float(a_depth) / 4294967296.0;" << std::endl;
	shaderBuilder << "	v_color = a_color;" << std::endl;
	shaderBuilder << "	v_texCoord = texCoord.xyz;" << std::endl;
	if(caps.hasFog)
	{
		shaderBuilder << "	v_fog = a_fog;" << std::endl;
	}
	shaderBuilder << "	gl_Position = g_projMatrix * vec4(a_position, 0, 1);" << std::endl;
	shaderBuilder << "}" << std::endl;

	auto shaderSource = shaderBuilder.str();

	Framework::OpenGl::CShader result(GL_VERTEX_SHADER);
	result.SetSource(shaderSource.c_str(), shaderSource.size());
	FRAMEWORK_MAYBE_UNUSED bool compilationResult = result.Compile();
	assert(compilationResult);

	CHECKGLERROR();

	return result;
}

Framework::OpenGl::CShader CGSH_OpenGL::GenerateFragmentShader(const SHADERCAPS& caps)
{
	bool alphaTestCanDiscardDepth = (caps.hasAlphaTest) && ((caps.alphaFailMethod == ALPHA_TEST_FAIL_FBONLY) || (caps.alphaFailMethod == ALPHA_TEST_FAIL_RGBONLY));
	bool useFramebufferFetch = (caps.hasAlphaBlend || caps.hasAlphaTest || caps.hasDestAlphaTest) && m_hasFramebufferFetchExtension;
	bool useFramebufferFetchDepth = alphaTestCanDiscardDepth && m_hasFramebufferFetchDepthExtension;

	std::stringstream shaderBuilder;
	shaderBuilder << GLSL_VERSION << std::endl;

	if(useFramebufferFetch)
	{
		shaderBuilder << "#extension GL_EXT_shader_framebuffer_fetch : require" << std::endl;
	}
	if(useFramebufferFetchDepth)
	{
		shaderBuilder << "#extension GL_ARM_shader_framebuffer_fetch_depth_stencil : require" << std::endl;
	}

	shaderBuilder << "precision mediump float;" << std::endl;

	shaderBuilder << "in highp float v_depth;" << std::endl;
	shaderBuilder << "in vec4 v_color;" << std::endl;
	shaderBuilder << "in highp vec3 v_texCoord;" << std::endl;
	if(caps.hasFog)
	{
		shaderBuilder << "in float v_fog;" << std::endl;
	}

	if(useFramebufferFetch)
	{
		shaderBuilder << "inout vec4 fragColor;" << std::endl;
	}
	else
	{
		shaderBuilder << "out vec4 fragColor;" << std::endl;
	}

#ifdef USE_DUALSOURCE_BLENDING
	shaderBuilder << "out vec4 blendColor;" << std::endl;
#endif

	shaderBuilder << "uniform sampler2D g_texture;" << std::endl;
	shaderBuilder << "uniform sampler2D g_palette;" << std::endl;

	shaderBuilder << "layout(std140) uniform FragmentParams" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	vec2 g_textureSize;" << std::endl;
	shaderBuilder << "	vec2 g_texelSize;" << std::endl;
	shaderBuilder << "	vec2 g_clampMin;" << std::endl;
	shaderBuilder << "	vec2 g_clampMax;" << std::endl;
	shaderBuilder << "	float g_texA0;" << std::endl;
	shaderBuilder << "	float g_texA1;" << std::endl;
	shaderBuilder << "	uint g_alphaRef;" << std::endl;
	shaderBuilder << "	float g_alphaFix;" << std::endl;
	shaderBuilder << "	vec3 g_fogColor;" << std::endl;
	shaderBuilder << "	ivec4 g_clutOrigins01;" << std::endl;
	shaderBuilder << "	ivec4 g_clutOrigins23;" << std::endl;
	shaderBuilder << "	ivec4 g_clutScale;" << std::endl;
	shaderBuilder << "};" << std::endl;

	if(caps.texPaletteFromFramebuffer)
	{
		//Entry i of a CSM1 CLUT is pixel (p % 16, p / 16) of a 16x16 square, p being i with bits 3 and 4 swapped;
		//each 8x8 quarter is a block whose place in the framebuffer comes with the draw.
		shaderBuilder << "vec4 paletteColor(float colorIndex)" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	int i = int(colorIndex + 0.5);" << std::endl;
		shaderBuilder << "	int p = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);" << std::endl;
		shaderBuilder << "	int x = p & 15;" << std::endl;
		shaderBuilder << "	int y = p >> 4;" << std::endl;
		shaderBuilder << "	int quarter = (x >> 3) + ((y >> 3) * 2);" << std::endl;
		shaderBuilder << "	ivec2 origin = (quarter == 0) ? g_clutOrigins01.xy : ((quarter == 1) ? g_clutOrigins01.zw : ((quarter == 2) ? g_clutOrigins23.xy : g_clutOrigins23.zw));" << std::endl;
		shaderBuilder << "	return texelFetch(g_palette, (origin + ivec2(x & 7, y & 7)) * g_clutScale.x, 0);" << std::endl;
		shaderBuilder << "}" << std::endl;
	}

	if(caps.texClampS == TEXTURE_CLAMP_MODE_REGION_REPEAT || caps.texClampT == TEXTURE_CLAMP_MODE_REGION_REPEAT)
	{
		shaderBuilder << s_andFunction << std::endl;
		shaderBuilder << s_orFunction << std::endl;
	}

	shaderBuilder << "float combineColors(float a, float b)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint aInt = uint(a * 255.0);" << std::endl;
	shaderBuilder << "	uint bInt = uint(b * 255.0);" << std::endl;
	shaderBuilder << "	uint result = min((aInt * bInt) >> 7, 255u);" << std::endl;
	shaderBuilder << "	return float(result) / 255.0;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "vec4 expandAlpha(vec4 inputColor)" << std::endl;
	shaderBuilder << "{" << std::endl;
	if(caps.texUseAlphaExpansion)
	{
		shaderBuilder << "	float alpha = mix(g_texA0, g_texA1, inputColor.a);" << std::endl;
		if(caps.texBlackIsTransparent)
		{
			shaderBuilder << "	float black = inputColor.r + inputColor.g + inputColor.b;" << std::endl;
			shaderBuilder << "	if(black == 0.0) alpha = 0.0;" << std::endl;
		}
		shaderBuilder << "	return vec4(inputColor.rgb, alpha);" << std::endl;
	}
	else
	{
		shaderBuilder << "	return inputColor;" << std::endl;
	}
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "void main()" << std::endl;
	shaderBuilder << "{" << std::endl;

	if(useFramebufferFetch && caps.hasDestAlphaTest)
	{
		switch(caps.destAlphaTestRef)
		{
		case 0:
			shaderBuilder << "	if(fragColor.a >= 0.5) discard;" << std::endl;
			break;
		case 1:
			shaderBuilder << "	if(fragColor.a < 0.5) discard;" << std::endl;
			break;
		}
	}

	shaderBuilder << "	highp vec3 texCoord = v_texCoord;" << std::endl;
	shaderBuilder << "	texCoord.st /= texCoord.p;" << std::endl;

	if((caps.texClampS != TEXTURE_CLAMP_MODE_STD) || (caps.texClampT != TEXTURE_CLAMP_MODE_STD))
	{
		shaderBuilder << "	texCoord.st *= g_textureSize.st;" << std::endl;
		shaderBuilder << GenerateTexCoordClampingSection(static_cast<TEXTURE_CLAMP_MODE>(caps.texClampS), "s");
		shaderBuilder << GenerateTexCoordClampingSection(static_cast<TEXTURE_CLAMP_MODE>(caps.texClampT), "t");
		shaderBuilder << "	texCoord.st /= g_textureSize.st;" << std::endl;
	}

	shaderBuilder << "	vec4 textureColor = vec4(1, 1, 1, 1);" << std::endl;
	if(caps.isIndexedTextureSource())
	{
		if(!caps.texBilinearFilter)
		{
			shaderBuilder << "	float colorIndex = texture(g_texture, texCoord.st).r * 255.0;" << std::endl;
			if(caps.texSourceMode == TEXTURE_SOURCE_MODE_IDX4)
			{
				shaderBuilder << "	float paletteTexelBias = 0.5 / 16.0;" << std::endl;
				shaderBuilder << "	textureColor = expandAlpha(texture(g_palette, vec2(colorIndex / 16.0 + paletteTexelBias, 0)));" << std::endl;
			}
			else if(caps.texSourceMode == TEXTURE_SOURCE_MODE_IDX8)
			{
				if(caps.texPaletteFromFramebuffer)
				{
					shaderBuilder << "	textureColor = expandAlpha(paletteColor(colorIndex));" << std::endl;
				}
				else
				{
					shaderBuilder << "	float paletteTexelBias = 0.5 / 256.0;" << std::endl;
					shaderBuilder << "	textureColor = expandAlpha(texture(g_palette, vec2(colorIndex / 256.0 + paletteTexelBias, 0)));" << std::endl;
				}
			}
		}
		else
		{
			shaderBuilder << "	float tlIdx = texture(g_texture, texCoord.st                                     ).r * 255.0;" << std::endl;
			shaderBuilder << "	float trIdx = texture(g_texture, texCoord.st + vec2(g_texelSize.x, 0)            ).r * 255.0;" << std::endl;
			shaderBuilder << "	float blIdx = texture(g_texture, texCoord.st + vec2(0, g_texelSize.y)            ).r * 255.0;" << std::endl;
			shaderBuilder << "	float brIdx = texture(g_texture, texCoord.st + vec2(g_texelSize.x, g_texelSize.y)).r * 255.0;" << std::endl;

			if(caps.texSourceMode == TEXTURE_SOURCE_MODE_IDX4)
			{
				shaderBuilder << "	float paletteTexelBias = 0.5 / 16.0;" << std::endl;
				shaderBuilder << "	vec4 tl = expandAlpha(texture(g_palette, vec2(tlIdx / 16.0 + paletteTexelBias, 0)));" << std::endl;
				shaderBuilder << "	vec4 tr = expandAlpha(texture(g_palette, vec2(trIdx / 16.0 + paletteTexelBias, 0)));" << std::endl;
				shaderBuilder << "	vec4 bl = expandAlpha(texture(g_palette, vec2(blIdx / 16.0 + paletteTexelBias, 0)));" << std::endl;
				shaderBuilder << "	vec4 br = expandAlpha(texture(g_palette, vec2(brIdx / 16.0 + paletteTexelBias, 0)));" << std::endl;
			}
			else if(caps.texSourceMode == TEXTURE_SOURCE_MODE_IDX8)
			{
				if(caps.texPaletteFromFramebuffer)
				{
					shaderBuilder << "	vec4 tl = expandAlpha(paletteColor(tlIdx));" << std::endl;
					shaderBuilder << "	vec4 tr = expandAlpha(paletteColor(trIdx));" << std::endl;
					shaderBuilder << "	vec4 bl = expandAlpha(paletteColor(blIdx));" << std::endl;
					shaderBuilder << "	vec4 br = expandAlpha(paletteColor(brIdx));" << std::endl;
				}
				else
				{
					shaderBuilder << "	float paletteTexelBias = 0.5 / 256.0;" << std::endl;
					shaderBuilder << "	vec4 tl = expandAlpha(texture(g_palette, vec2(tlIdx / 256.0 + paletteTexelBias, 0)));" << std::endl;
					shaderBuilder << "	vec4 tr = expandAlpha(texture(g_palette, vec2(trIdx / 256.0 + paletteTexelBias, 0)));" << std::endl;
					shaderBuilder << "	vec4 bl = expandAlpha(texture(g_palette, vec2(blIdx / 256.0 + paletteTexelBias, 0)));" << std::endl;
					shaderBuilder << "	vec4 br = expandAlpha(texture(g_palette, vec2(brIdx / 256.0 + paletteTexelBias, 0)));" << std::endl;
				}
			}

			shaderBuilder << "	highp vec2 f = fract(texCoord.st * g_textureSize);" << std::endl;
			shaderBuilder << "	vec4 tA = mix(tl, tr, f.x);" << std::endl;
			shaderBuilder << "	vec4 tB = mix(bl, br, f.x);" << std::endl;
			shaderBuilder << "	textureColor = mix(tA, tB, f.y);" << std::endl;
		}
	}
	else if(caps.texSourceMode == TEXTURE_SOURCE_MODE_STD)
	{
		shaderBuilder << "	textureColor = expandAlpha(texture(g_texture, texCoord.st));" << std::endl;
	}

	if(caps.texSourceMode != TEXTURE_SOURCE_MODE_NONE)
	{
		if(!caps.texHasAlpha)
		{
			shaderBuilder << "	textureColor.a = 1.0;" << std::endl;
		}

		switch(caps.texFunction)
		{
		case TEX0_FUNCTION_MODULATE:
			shaderBuilder << "	textureColor.rgb = clamp(textureColor.rgb * v_color.rgb * 2.0, 0.0, 1.0);" << std::endl;
			if(!caps.texHasAlpha)
			{
				shaderBuilder << "	textureColor.a = v_color.a;" << std::endl;
			}
			else
			{
				shaderBuilder << "	textureColor.a = combineColors(textureColor.a, v_color.a);" << std::endl;
			}
			break;
		case TEX0_FUNCTION_DECAL:
			if(!caps.texHasAlpha)
			{
				shaderBuilder << "	textureColor.a = v_color.a;" << std::endl;
			}
			break;
		case TEX0_FUNCTION_HIGHLIGHT:
			shaderBuilder << "	textureColor.rgb = clamp(textureColor.rgb * v_color.rgb * 2.0, 0.0, 1.0) + v_color.aaa;" << std::endl;
			if(!caps.texHasAlpha)
			{
				shaderBuilder << "	textureColor.a = v_color.a;" << std::endl;
			}
			else
			{
				shaderBuilder << "	textureColor.a += v_color.a;" << std::endl;
			}
			break;
		case TEX0_FUNCTION_HIGHLIGHT2:
			shaderBuilder << "	textureColor.rgb = clamp(textureColor.rgb * v_color.rgb * 2.0, 0.0, 1.0) + v_color.aaa;" << std::endl;
			if(!caps.texHasAlpha)
			{
				shaderBuilder << "	textureColor.a = v_color.a;" << std::endl;
			}
			break;
		default:
			assert(0);
			break;
		}
	}
	else
	{
		shaderBuilder << "	textureColor = v_color;" << std::endl;
	}

	if(useFramebufferFetch)
	{
		shaderBuilder << "	bool outputColor = true;" << std::endl;
		shaderBuilder << "	bool outputAlpha = true;" << std::endl;
	}
	if(useFramebufferFetchDepth)
	{
		shaderBuilder << "	bool outputDepth = true;" << std::endl;
	}

	if(caps.hasAlphaTest)
	{
		shaderBuilder << GenerateAlphaTestSection(static_cast<ALPHA_TEST_METHOD>(caps.alphaTestMethod), static_cast<ALPHA_TEST_FAIL_METHOD>(caps.alphaFailMethod));
	}

	// ----------------------
	// Prepare final color and alpha values before blending and writing

	if(caps.hasFog)
	{
		shaderBuilder << "	vec3 finalColor = mix(textureColor.rgb, g_fogColor, v_fog);" << std::endl;
	}
	else
	{
		shaderBuilder << "	vec3 finalColor = textureColor.xyz;" << std::endl;
	}

	//For proper alpha blending, alpha has to be multiplied by 2 (0x80 -> 1.0)
#ifdef USE_DUALSOURCE_BLENDING
	shaderBuilder << "	float finalAlpha = textureColor.a;" << std::endl;
	shaderBuilder << "	blendColor.a = clamp(textureColor.a * 2.0, 0.0, 1.0);" << std::endl;
#else
	if(useFramebufferFetch)
	{
		shaderBuilder << "	float finalAlpha = textureColor.a;" << std::endl;
	}
	else
	{
		//This has the side effect of not writing a proper value in the framebuffer (should write alpha "as is")
		shaderBuilder << "	float finalAlpha = clamp(textureColor.a * 2.0, 0.0, 1.0);" << std::endl;
	}
#endif

	if(useFramebufferFetch)
	{
		if(caps.hasAlphaBlend)
		{
			shaderBuilder << GenerateAlphaBlendSection(
			    static_cast<ALPHABLEND_ABD>(caps.alphaBlendA), static_cast<ALPHABLEND_ABD>(caps.alphaBlendB),
			    static_cast<ALPHABLEND_C>(caps.alphaBlendC), static_cast<ALPHABLEND_ABD>(caps.alphaBlendD));
		}

		shaderBuilder << "	if(outputColor)" << std::endl;
		shaderBuilder << "	{" << std::endl;
		shaderBuilder << "		fragColor.xyz = finalColor;" << std::endl;
		shaderBuilder << "	}" << std::endl;

		shaderBuilder << "	if(outputAlpha)" << std::endl;
		shaderBuilder << "	{" << std::endl;
		shaderBuilder << "		fragColor.a = finalAlpha;" << std::endl;
		shaderBuilder << "	}" << std::endl;
	}
	else
	{
		shaderBuilder << "	fragColor.xyz = finalColor;" << std::endl;
		shaderBuilder << "	fragColor.a = finalAlpha;" << std::endl;

		if(caps.colorOutputWhite)
		{
			shaderBuilder << "	fragColor.xyz = vec3(1, 1, 1);" << std::endl;
		}
	}

	// ----------------------

	if(useFramebufferFetchDepth)
	{
		shaderBuilder << "	if(outputDepth)" << std::endl;
		shaderBuilder << "	{" << std::endl;
		shaderBuilder << "		gl_FragDepth = v_depth;" << std::endl;
		shaderBuilder << "	}" << std::endl;
		shaderBuilder << "	else" << std::endl;
		shaderBuilder << "	{" << std::endl;
		shaderBuilder << "		highp float dstDepth = gl_LastFragDepthARM;" << std::endl;
		if(caps.alphaTestDepthTest_DepthFetch)
		{
			if(caps.alphaTestDepthTestEqual_DepthFetch)
			{
				shaderBuilder << "		if(!(v_depth >= dstDepth)) discard;" << std::endl;
			}
			else
			{
				shaderBuilder << "		if(!(v_depth > dstDepth)) discard;" << std::endl;
			}
		}
		shaderBuilder << "		gl_FragDepth = dstDepth;" << std::endl;
		shaderBuilder << "	}" << std::endl;
	}
	else
	{
		shaderBuilder << "	gl_FragDepth = v_depth;" << std::endl;
	}

	// ----------------------

	shaderBuilder << "}" << std::endl;

	auto shaderSource = shaderBuilder.str();

	Framework::OpenGl::CShader result(GL_FRAGMENT_SHADER);
	result.SetSource(shaderSource.c_str(), shaderSource.size());
	FRAMEWORK_MAYBE_UNUSED bool compilationResult = result.Compile();
	assert(compilationResult);

	CHECKGLERROR();

	return result;
}

std::string CGSH_OpenGL::GenerateTexCoordClampingSection(TEXTURE_CLAMP_MODE clampMode, const char* coordinate)
{
	std::stringstream shaderBuilder;

	switch(clampMode)
	{
	case TEXTURE_CLAMP_MODE_STD:
		//Using the GPU's built-in clamping, nothing to do.
		break;
	case TEXTURE_CLAMP_MODE_CLAMP:
		shaderBuilder << "	texCoord." << coordinate << " = clamp(texCoord." << coordinate
		              << ", g_clampMin." << coordinate << ", g_clampMax." << coordinate << ");" << std::endl;
		break;
	case TEXTURE_CLAMP_MODE_REGION_CLAMP:
		shaderBuilder << "	texCoord." << coordinate << " = min(g_clampMax." << coordinate << ", "
		              << "max(g_clampMin." << coordinate << ", texCoord." << coordinate << "));" << std::endl;
		break;
	case TEXTURE_CLAMP_MODE_REGION_REPEAT:
		shaderBuilder << "	texCoord." << coordinate << " = or(int(and(int(texCoord." << coordinate << "), "
		              << "int(g_clampMin." << coordinate << "))), int(g_clampMax." << coordinate << "));";
		break;
	case TEXTURE_CLAMP_MODE_REGION_REPEAT_SIMPLE:
		shaderBuilder << "	texCoord." << coordinate << " = mod(texCoord." << coordinate << ", "
		              << "g_clampMin." << coordinate << ") + g_clampMax." << coordinate << ";" << std::endl;
		break;
	default:
		assert(false);
		break;
	}

	std::string shaderSource = shaderBuilder.str();
	return shaderSource;
}

std::string CGSH_OpenGL::GenerateAlphaTestSection(ALPHA_TEST_METHOD testMethod, ALPHA_TEST_FAIL_METHOD failMethod)
{
	std::stringstream shaderBuilder;

	const char* test = "	if(false)";

	//testMethod is the condition to pass the test
	switch(testMethod)
	{
	case ALPHA_TEST_NEVER:
		test = "	if(true)";
		break;
	case ALPHA_TEST_ALWAYS:
		test = "	if(false)";
		break;
	case ALPHA_TEST_LESS:
		test = "	if(textureColorAlphaInt >= g_alphaRef)";
		break;
	case ALPHA_TEST_LEQUAL:
		test = "	if(textureColorAlphaInt > g_alphaRef)";
		break;
	case ALPHA_TEST_EQUAL:
		test = "	if(textureColorAlphaInt != g_alphaRef)";
		break;
	case ALPHA_TEST_GEQUAL:
		test = "	if(textureColorAlphaInt < g_alphaRef)";
		break;
	case ALPHA_TEST_GREATER:
		test = "	if(textureColorAlphaInt <= g_alphaRef)";
		break;
	case ALPHA_TEST_NOTEQUAL:
		test = "	if(textureColorAlphaInt == g_alphaRef)";
		break;
	default:
		assert(false);
		break;
	}

	shaderBuilder << "	uint textureColorAlphaInt = uint(textureColor.a * 255.0);" << std::endl;
	shaderBuilder << test << std::endl;
	shaderBuilder << "	{" << std::endl;

	switch(failMethod)
	{
	case ALPHA_TEST_FAIL_KEEP:
		// No write at all
		shaderBuilder << "		discard;" << std::endl;
		break;
	case ALPHA_TEST_FAIL_FBONLY:
		// Only write color and alpha
		// TODO: We cannot prevent depth from being written at the moment
		// Failure note: We rather accept writing depth here, than discarding the
		// whole pixel, as most games work better with this hack.
		// Breaks foliage in some games (Grandia X/3, Naruto, etc.)
		// If we have depth buffer fetch, we can properly handle it.
		if(m_hasFramebufferFetchDepthExtension)
		{
			shaderBuilder << "		outputDepth = false;" << std::endl;
		}
		break;
	case ALPHA_TEST_FAIL_ZBONLY:
		// Only write depth
		if(m_hasFramebufferFetchExtension)
		{
			shaderBuilder << "		outputColor = false;" << std::endl;
			shaderBuilder << "		outputAlpha = false;" << std::endl;
		}
		else
		{
			// TODO: Prevent framebuffer writing without extension
			shaderBuilder << "		discard;" << std::endl;
			// Failure note: This also discards depth
		}
		break;
	case ALPHA_TEST_FAIL_RGBONLY:
		// Only write color
		if(m_hasFramebufferFetchExtension)
		{
			shaderBuilder << "		outputAlpha = false;" << std::endl;
		}
		if(m_hasFramebufferFetchDepthExtension)
		{
			shaderBuilder << "		outputDepth = false;" << std::endl;
		}
		// TODO: Prevent alpha and depth writing without extension
		// Failure note: We are somewhat out of luck, and just draw the pixel as is.
		// This is completely wrong, but nothing we can do about it at this point.
		break;
	}

	shaderBuilder << "	}" << std::endl;

	auto shaderSource = shaderBuilder.str();
	return shaderSource;
}

std::string CGSH_OpenGL::GenerateAlphaBlendSection(ALPHABLEND_ABD factorA, ALPHABLEND_ABD factorB,
                                                   ALPHABLEND_C factorC, ALPHABLEND_ABD factorD)
{
	static const auto getABDParam =
	    [](unsigned int abd) {
		    switch(abd)
		    {
		    default:
			    assert(false);
			    [[fallthrough]];
		    case ALPHABLEND_ABD_CS:
			    return "finalColor";
		    case ALPHABLEND_ABD_CD:
			    return "fragColor.xyz";
		    case ALPHABLEND_ABD_ZERO:
			    return "vec3(0, 0, 0)";
		    }
	    };
	static const auto getCParam =
	    [](unsigned int abd) {
		    switch(abd)
		    {
		    default:
			    assert(false);
			    [[fallthrough]];
		    case ALPHABLEND_C_AS:
			    return "finalAlpha";
		    case ALPHABLEND_C_AD:
			    return "fragColor.a";
		    case ALPHABLEND_C_FIX:
			    return "g_alphaFix";
		    }
	    };

	auto valueA = getABDParam(factorA);
	auto valueB = getABDParam(factorB);
	auto valueC = getCParam(factorC);
	auto valueD = getABDParam(factorD);

	std::stringstream shaderBuilder;
	shaderBuilder << "	finalColor = (" << valueA << " - " << valueB << ") * (" << valueC << " * 2.0) + " << valueD << ";" << std::endl;

	auto shaderSource = shaderBuilder.str();
	return shaderSource;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GeneratePresentProgram()
{
	Framework::OpenGl::CShader vertexShader(GL_VERTEX_SHADER);
	Framework::OpenGl::CShader pixelShader(GL_FRAGMENT_SHADER);

	{
		std::stringstream shaderBuilder;
		shaderBuilder << GLSL_VERSION << std::endl;
		shaderBuilder << "in vec2 a_position;" << std::endl;
		shaderBuilder << "in vec2 a_texCoord;" << std::endl;
		shaderBuilder << "out vec2 v_texCoord;" << std::endl;
		shaderBuilder << "uniform vec2 g_texCoordScale;" << std::endl;
		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	v_texCoord = a_texCoord * g_texCoordScale;" << std::endl;
		shaderBuilder << "	gl_Position = vec4(a_position, 0, 1);" << std::endl;
		shaderBuilder << "}" << std::endl;

		vertexShader.SetSource(shaderBuilder.str().c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = vertexShader.Compile();
		assert(result);
	}

	{
		std::stringstream shaderBuilder;
		shaderBuilder << GLSL_VERSION << std::endl;
		shaderBuilder << "precision mediump float;" << std::endl;
		shaderBuilder << "in vec2 v_texCoord;" << std::endl;
		shaderBuilder << "out vec4 fragColor;" << std::endl;
		shaderBuilder << "uniform sampler2D g_texture;" << std::endl;
		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	fragColor = texture(g_texture, v_texCoord);" << std::endl;
		shaderBuilder << "}" << std::endl;

		pixelShader.SetSource(shaderBuilder.str().c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = pixelShader.Compile();
		assert(result);
	}

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		program->AttachShader(vertexShader);
		program->AttachShader(pixelShader);

		glBindAttribLocation(*program, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), "a_position");
		glBindAttribLocation(*program, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), "a_texCoord");

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateCopyToFbProgram()
{
	Framework::OpenGl::CShader vertexShader(GL_VERTEX_SHADER);
	Framework::OpenGl::CShader pixelShader(GL_FRAGMENT_SHADER);

	{
		std::stringstream shaderBuilder;
		shaderBuilder << GLSL_VERSION << std::endl;
		shaderBuilder << "in vec2 a_position;" << std::endl;
		shaderBuilder << "in vec2 a_texCoord;" << std::endl;
		shaderBuilder << "out vec2 v_texCoord;" << std::endl;
		shaderBuilder << "uniform vec2 g_srcPosition;" << std::endl;
		shaderBuilder << "uniform vec2 g_srcSize;" << std::endl;
		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	v_texCoord = (a_texCoord * g_srcSize) + g_srcPosition;" << std::endl;
		shaderBuilder << "	gl_Position = vec4(a_position, 0, 1);" << std::endl;
		shaderBuilder << "}" << std::endl;

		vertexShader.SetSource(shaderBuilder.str().c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = vertexShader.Compile();
		assert(result);
	}

	{
		std::stringstream shaderBuilder;
		shaderBuilder << GLSL_VERSION << std::endl;
		shaderBuilder << "precision mediump float;" << std::endl;
		shaderBuilder << "in vec2 v_texCoord;" << std::endl;
		shaderBuilder << "out vec4 fragColor;" << std::endl;
		shaderBuilder << "uniform sampler2D g_texture;" << std::endl;
		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	fragColor = texture(g_texture, v_texCoord);" << std::endl;
		shaderBuilder << "}" << std::endl;

		pixelShader.SetSource(shaderBuilder.str().c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = pixelShader.Compile();
		assert(result);
	}

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		program->AttachShader(vertexShader);
		program->AttachShader(pixelShader);

		glBindAttribLocation(*program, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), "a_position");
		glBindAttribLocation(*program, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), "a_texCoord");

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateGpuClutProgram()
{
	Framework::OpenGl::CShader vertexShader(GL_VERTEX_SHADER);
	Framework::OpenGl::CShader pixelShader(GL_FRAGMENT_SHADER);

	{
		std::stringstream shaderBuilder;
		shaderBuilder << GLSL_VERSION << std::endl;
		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	vec2 position = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));" << std::endl;
		shaderBuilder << "	gl_Position = vec4((position * 2.0) - 1.0, 0, 1);" << std::endl;
		shaderBuilder << "}" << std::endl;

		vertexShader.SetSource(shaderBuilder.str().c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = vertexShader.Compile();
		assert(result);
	}

	{
		//Entry i of a CSM1 CLUT is pixel (p % 16, p / 16) of a 64 pixel wide buffer at the CLUT pointer, p being i with
		//bits 3 and 4 swapped: in block (p % 16) / 8 + 2 * ((p / 16) / 8) after the pointer's, placed by the PSMCT32
		//block layout of the framebuffer's pages.
		std::stringstream shaderBuilder;
		shaderBuilder << GLSL_VERSION << std::endl;
		shaderBuilder << "precision mediump float;" << std::endl;
		shaderBuilder << "precision highp int;" << std::endl;
		shaderBuilder << "out vec4 fragColor;" << std::endl;
		shaderBuilder << "uniform sampler2D g_framebuffer;" << std::endl;
		shaderBuilder << "uniform ivec4 g_position;" << std::endl; //page, block, pages per row, scale
		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	int i = int(gl_FragCoord.x);" << std::endl;
		shaderBuilder << "	int p = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);" << std::endl;
		shaderBuilder << "	int x = p & 15;" << std::endl;
		shaderBuilder << "	int y = p >> 4;" << std::endl;
		shaderBuilder << "	int block = g_position.y + ((x >> 3) | ((y >> 3) << 1));" << std::endl;
		shaderBuilder << "	int page = g_position.x + (block >> 5);" << std::endl;
		shaderBuilder << "	block &= 31;" << std::endl;
		shaderBuilder << "	int blockX = (block & 1) | ((block >> 1) & 2) | ((block >> 2) & 4);" << std::endl;
		shaderBuilder << "	int blockY = ((block >> 1) & 1) | ((block >> 2) & 2);" << std::endl;
		shaderBuilder << "	ivec2 position = ivec2(((page % g_position.z) * 64) + (blockX * 8) + (x & 7), ((page / g_position.z) * 32) + (blockY * 8) + (y & 7));" << std::endl;
		shaderBuilder << "	position *= g_position.w;" << std::endl;
		shaderBuilder << "	fragColor = texelFetch(g_framebuffer, position, 0);" << std::endl;
		shaderBuilder << "}" << std::endl;

		pixelShader.SetSource(shaderBuilder.str().c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = pixelShader.Compile();
		assert(result);
	}

	auto program = std::make_shared<Framework::OpenGl::CProgram>();
	program->AttachShader(vertexShader);
	program->AttachShader(pixelShader);
	FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
	assert(result);
	return program;
}
