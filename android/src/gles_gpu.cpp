// The Android build's GPU: what citro3d and citro2d draw on the 3DS, drawn with OpenGL ES 3.0. The game's own drawing
// code (mwrender/renderingmanager.cpp, the UI) is unchanged: it calls the citro3d and citro2d of the stand-in headers
// (tools/test/native/stub), and this file is that API on the watch's GPU, as tools/test/native/native_gpu.cpp is in
// software (the rules here are that file's: where they differ from the PICA, it says).
//
// - Render targets are framebuffers: the two screens (400 x 240 and 320 x 240, upright) and the local map's texture.
//   The screens are put on the watch's display at the frame's end (android_input.cpp chooses which, and where).
// - Textures are uploaded from the PICA's data on first use (pica_textures.hpp decodes it): ETC1 as it is, the rest as
//   RGBA. Their rows are uploaded top first, so a texture's v is read the other way up; a render target's is not.
// - shaders/cell.v.pica and shaders/skin.v.pica are GLSL (vertexSource), without the PICA's 24-bit floats.
// - The texture combiners are a fragment shader made for each set of stages the game uses (fragmentSource).
// - citro2d's rectangles, triangles and images are batched (Batch2D) until the target, the texture or the state changes.
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <SDL3/SDL.h>
#include <jni.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "android.hpp"
#include "components/debug/debuglog.hpp"
#include "mwbase/sessiontypes.hpp"
#include "mwrender/screenshotmanager.hpp"
#include "pica_textures.hpp"

// ---- render targets and textures

struct C3D_RenderTarget
{
	int w = 0, h = 0;
	GLuint fbo = 0, color = 0, depth = 0;
	int screen = -1, side = -1;
	C3D_Tex* tex = nullptr;
	bool rgb565 = false;            // an Old 3DS's 3D screen: its clear colour comes packed as RGB565
};

// A C3D_Tex's GL side (C3D_Tex::native)
struct GlTex
{
	GLuint id = 0;
	bool dirty = true;              // the data changed (C3D_TexFlush) or was never uploaded
	C3D_RenderTarget* target = nullptr;     // a render target's texture: drawn into, never uploaded
};

static std::vector<C3D_RenderTarget*> s_targets;
static C3D_RenderTarget* s_target = nullptr;
static bool s_etc1 = false;         // GL_OES_compressed_ETC1_RGB8_texture (else ETC2's RGB, which reads ETC1 the same)
static GLuint s_white = 0;          // a texture unit with nothing bound reads white, as native_gpu.cpp's does

static GlTex* glTex(C3D_Tex* t)
{
	if (!t->native)
		t->native = new GlTex();
	return (GlTex*)t->native;
}

// ---- GPU state (as native_gpu.cpp keeps it)

static bool s_depthOn = false, s_depthWrite = true;
static int s_depthFunc = GPU_GREATER;
static float s_zScale = -1.0f, s_zOffset = 0.0f;
static int s_cull = GPU_CULL_NONE;
static bool s_alphaTestOn = false;
static int s_alphaFunc = GPU_ALWAYS, s_alphaRef = 0;
static int s_blend[4] = { GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA };
static bool s_scissorOn = false;
static u32 s_scissor[4];
static C3D_Tex* s_units[3];
static C3D_TexEnv s_env[6];
static float s_unif[96][4];
static unsigned s_unifChanges = 1;      // counts changes to s_unif (a program re-uploads them when it differs)
static bool s_envChanged = true;        // a combiner stage changed: the program is looked for again
static C3D_AttrInfo s_attr;
static C3D_BufInfo s_buf;
static float s_view2d[2];
static float s_frameRate = 60.0f;

// shaders/cell.v.pica's and shaders/skin.v.pica's uniform slots (native_gpu.cpp's: the shaders' declarations)
enum { U_PROJECTION = 0, U_MODELVIEW = 4, U_UVOFFSET = 8, U_FOGVEC, U_POSOFFSET, U_POSSCALE, U_UVSCALE, U_TINT, U_LIGHTPOS,
	U_LIGHTCOLOR, U_AMBIENT, U_TORCHVEC, U_STEREOCURVE, U_BONES };
enum { U_LAMP = U_BONES, U_SIDE, B_GAMMA = 0, B_LIGHTON, B_STEREOON };
static bool s_lightOn = false, s_gammaOn = false, s_stereoOn = false;

// ---- shaders

static GLuint compileShader(GLenum type, const std::string& src)
{
	GLuint s = glCreateShader(type);
	const char* p = src.c_str();
	glShaderSource(s, 1, &p, nullptr);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		char info[2048];
		glGetShaderInfoLog(s, sizeof(info), nullptr, info);
		logf("gles: shader failed: %s", info);
	}
	return s;
}

static GLuint linkProgram(const std::string& vs, const std::string& fs, const char* const* attribs, int nAttribs)
{
	GLuint p = glCreateProgram();
	GLuint v = compileShader(GL_VERTEX_SHADER, vs), f = compileShader(GL_FRAGMENT_SHADER, fs);
	glAttachShader(p, v);
	glAttachShader(p, f);
	for (int i = 0; i < nAttribs; i++)
		glBindAttribLocation(p, i, attribs[i]);
	glLinkProgram(p);
	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		char info[2048];
		glGetProgramInfoLog(p, sizeof(info), nullptr, info);
		logf("gles: program failed: %s", info);
	}
	glDeleteShader(v);
	glDeleteShader(f);
	return p;
}

// shaders/cell.v.pica (skinned: shaders/skin.v.pica) as native_gpu.cpp's vertexShader runs them: position from the
// packed input, model-view, projection; texture coordinate; fog; the baked colour times the tint, the lamps' share put
// back (w x lamp) and the low sun's side light, plus the carried light; the torch's reach. Depth as the PICA maps it
// (C3D_DepthMap: near -1 -> 1, far 0 -> 0 for GPU_GREATER) into GL's -w..w
static std::string vertexSource(bool skinned)
{
	char head[512];
	snprintf(head, sizeof(head),
		"#version 300 es\nprecision highp float;\n#define SKINNED %d\n#define U_PROJECTION %d\n#define U_MODELVIEW %d\n"
		"#define U_UVOFFSET %d\n#define U_FOGVEC %d\n#define U_POSOFFSET %d\n#define U_POSSCALE %d\n#define U_UVSCALE %d\n"
		"#define U_TINT %d\n#define U_LIGHTPOS %d\n#define U_LIGHTCOLOR %d\n#define U_AMBIENT %d\n#define U_TORCHVEC %d\n"
		"#define U_BONES %d\n#define U_LAMP %d\n#define U_SIDE %d\n",
		skinned ? 1 : 0, U_PROJECTION, U_MODELVIEW, U_UVOFFSET, U_FOGVEC, U_POSOFFSET, U_POSSCALE, U_UVSCALE, U_TINT,
		U_LIGHTPOS, U_LIGHTCOLOR, U_AMBIENT, U_TORCHVEC, U_BONES, U_LAMP, U_SIDE);
	return std::string(head) + R"(
uniform vec4 u[96];
uniform bool lightOn, gammaOn;
uniform vec2 zMap;
in vec4 a0, a1, a2, a3, a4;
out vec4 vColor;
out vec2 vUv0, vUv1;
float dp4(int r, vec4 v) { return dot(u[r], v); }
void main()
{
	vec4 r0 = vec4(u[U_POSOFFSET].xyz + a0.xyz * u[U_POSSCALE].xyz, 1.0);
#if SKINNED
	vec4 p = vec4(a0.xyz * u[U_POSSCALE].xyz, 1.0);
	vec3 acc = vec3(0.0);
	for (int k = 0; k < 4; k++)
	{
		int b = U_BONES + int(a3[k]) * 3;
		float wt = a4[k] * (1.0 / 255.0);
		if (b + 2 < 96)
			acc += wt * vec3(dp4(b, p), dp4(b + 1, p), dp4(b + 2, p));
	}
	r0.xyz = acc;
#endif
	vec4 r1 = vec4(dp4(U_MODELVIEW, r0), dp4(U_MODELVIEW + 1, r0), dp4(U_MODELVIEW + 2, r0), dp4(U_MODELVIEW + 3, r0));
	vec4 o = vec4(dp4(U_PROJECTION, r1), dp4(U_PROJECTION + 1, r1), dp4(U_PROJECTION + 2, r1), dp4(U_PROJECTION + 3, r1));
	vUv0 = u[U_UVOFFSET].xy + a1.xy * u[U_UVSCALE].xy;
	float fog = dot(u[U_FOGVEC], r1);
	vec3 d = r1.xyz - u[U_LIGHTPOS].xyz;
	float f1 = lightOn ? max(0.0, 1.0 - dot(d, d) * u[U_LIGHTPOS].w) : 0.0;
	vec4 c = a2 / 255.0 * u[U_TINT];
#if !SKINNED
	float side = max(0.0, a2.w * u[U_SIDE].w + u[U_LAMP].w);
	c.rgb += a0.w * u[U_LAMP].rgb + side * u[U_SIDE].rgb;
#endif
	c.rgb += u[U_AMBIENT].rgb + u[U_LIGHTCOLOR].rgb * f1;
	if (gammaOn)
		c.rgb = exp2(u[U_AMBIENT].w * log2(max(c.rgb, vec3(0.001))));
	vColor = clamp(c, 0.0, 1.0);
	vUv1 = vec2(fog, dot(u[U_TORCHVEC], r1));
	float zw = zMap.x * o.z + zMap.y * o.w;
	gl_Position = vec4(o.x, o.y, 2.0 * zw - o.w, o.w);
}
)";
}

// The texture combiners (C3D_TexEnv x 6) as native_gpu.cpp runs them: sources, the RGB operand (colour or alpha), the
// functions; the first stage's "previous" is 0. Then the alpha test
static std::string tevSource(int s, bool alpha)
{
	const char* v = s == GPU_TEXTURE0 ? "t0" : s == GPU_TEXTURE1 ? "t1" : s == GPU_PRIMARY_COLOR ? "vColor" : s == GPU_CONSTANT ?
		"k" : "prev";
	return std::string(v) + (alpha ? ".a" : "");
}

static std::string tevFunc(int f, const std::string& a, const std::string& b, const std::string& c, bool rgb)
{
	std::string one = rgb ? "vec3(1.0)" : "1.0";
	switch (f)
	{
	case GPU_MODULATE: return a + " * " + b;
	case GPU_ADD: return "min(" + one + ", " + a + " + " + b + ")";
	case GPU_INTERPOLATE: return a + " * " + c + " + " + b + " * (" + one + " - " + c + ")";
	case GPU_MULTIPLY_ADD: return "min(" + one + ", " + a + " * " + b + " + " + c + ")";
	default: return a;
	}
}

static std::string fragmentSource()
{
	std::string s = R"(#version 300 es
precision mediump float;
in vec4 vColor;
in vec2 vUv0, vUv1;
uniform sampler2D tex0, tex1;
uniform vec2 flipV, lodBias;
uniform vec4 consts[6];
uniform int alphaFunc;
uniform float alphaRef;
out vec4 fragColor;
void main()
{
	vec4 t0 = texture(tex0, vec2(vUv0.x, flipV.x > 0.5 ? 1.0 - vUv0.y : vUv0.y), lodBias.x);
	vec4 t1 = texture(tex1, vec2(vUv1.x, flipV.y > 0.5 ? 1.0 - vUv1.y : vUv1.y), lodBias.y);
	vec4 prev = vec4(0.0);
	vec4 k;
)";
	for (int st = 0; st < 6; st++)
	{
		const C3D_TexEnv& e = s_env[st];
		std::string rgb[3], al[3];
		for (int i = 0; i < 3; i++)
		{
			rgb[i] = e.opRgb[i] == GPU_TEVOP_RGB_SRC_ALPHA ? "vec3(" + tevSource(e.srcRgb[i], true) + ")" :
				tevSource(e.srcRgb[i], false) + ".rgb";
			al[i] = tevSource(e.srcAlpha[i], true);
		}
		char line[64];
		snprintf(line, sizeof(line), "\tk = consts[%d];\n", st);
		s += line;
		s += "\tprev = vec4(" + tevFunc(e.funcRgb, rgb[0], rgb[1], rgb[2], true) + ", " +
			tevFunc(e.funcAlpha, al[0], al[1], al[2], false) + ");\n";
	}
	s += R"(	float a = floor(prev.a * 255.0 + 0.5);
	bool pass = alphaFunc == 0 ? false : alphaFunc == 1 ? a == alphaRef : alphaFunc == 2 ? a != alphaRef :
		alphaFunc == 3 ? a < alphaRef : alphaFunc == 4 ? a <= alphaRef : alphaFunc == 5 ? a > alphaRef :
		alphaFunc == 6 ? a >= alphaRef : true;
	if (!pass)
		discard;
	fragColor = prev;
}
)";
	return s;
}

struct Program3D
{
	GLuint id = 0;
	unsigned unifSeen = 0;          // the uniforms' change count it was last given (s_unifChanges)
	GLint u, lightOn, gammaOn, zMap, tex0, tex1, flipV, lodBias, consts, alphaFunc, alphaRef;
};

// One program for each set of combiner stages and each vertex shader
static std::map<std::string, Program3D> s_programs;

static std::string tevKey(bool skinned)
{
	std::string k(1, skinned ? 'S' : 'C');
	for (auto& e : s_env)
	{
		int v[14] = { e.srcRgb[0], e.srcRgb[1], e.srcRgb[2], e.srcAlpha[0], e.srcAlpha[1], e.srcAlpha[2], e.opRgb[0],
			e.opRgb[1], e.opRgb[2], e.funcRgb, e.funcAlpha, 0, 0, 0 };
		for (int i = 0; i < 11; i++)
			k += (char)(v[i] & 0x7F);
	}
	return k;
}

static Program3D& program3D(bool skinned)
{
	static Program3D* last[2] = {};
	if (!s_envChanged && last[skinned])
		return *last[skinned];
	if (s_envChanged)
		last[0] = last[1] = nullptr;
	s_envChanged = false;
	std::string key = tevKey(skinned);
	auto it = s_programs.find(key);
	if (it != s_programs.end())
		return *(last[skinned] = &it->second);
	static const char* const kAttribs[] = { "a0", "a1", "a2", "a3", "a4" };
	Program3D p;
	p.id = linkProgram(vertexSource(skinned), fragmentSource(), kAttribs, 5);
	p.u = glGetUniformLocation(p.id, "u");
	p.lightOn = glGetUniformLocation(p.id, "lightOn");
	p.gammaOn = glGetUniformLocation(p.id, "gammaOn");
	p.zMap = glGetUniformLocation(p.id, "zMap");
	p.tex0 = glGetUniformLocation(p.id, "tex0");
	p.tex1 = glGetUniformLocation(p.id, "tex1");
	p.flipV = glGetUniformLocation(p.id, "flipV");
	p.lodBias = glGetUniformLocation(p.id, "lodBias");
	p.consts = glGetUniformLocation(p.id, "consts");
	p.alphaFunc = glGetUniformLocation(p.id, "alphaFunc");
	p.alphaRef = glGetUniformLocation(p.id, "alphaRef");
	glUseProgram(p.id);
	glUniform1i(p.tex0, 0);
	glUniform1i(p.tex1, 1);
	logf("gles: program %d for %s combiners (%d made)", (int)p.id, skinned ? "skinned" : "cell", (int)s_programs.size() + 1);
	return *(last[skinned] = &(s_programs[key] = p));
}

// citro2d's: screen pixels, the colour, the texture's (u, v) and how far the colour replaces the texture's
static const char* const k2DVertex = R"(#version 300 es
precision highp float;
uniform vec2 size;
in vec2 pos;
in vec4 color;
in vec3 uvk;
out vec4 vColor;
out vec3 vUvk;
void main()
{
	vColor = color;
	vUvk = uvk;
	gl_Position = vec4(pos.x / size.x * 2.0 - 1.0, 1.0 - pos.y / size.y * 2.0, 0.0, 1.0);
}
)";
static const char* const k2DFragment = R"(#version 300 es
precision mediump float;
uniform sampler2D tex;
uniform float useTex, flipV;
in vec4 vColor;
in vec3 vUvk;
out vec4 fragColor;
void main()
{
	vec4 c = vColor;
	if (useTex > 0.5)
	{
		vec4 s = texture(tex, vec2(vUvk.x, flipV > 0.5 ? 1.0 - vUvk.y : vUvk.y));
		c = vec4(mix(s.rgb, c.rgb, vUvk.z), s.a * c.a);
	}
	fragColor = c;
}
)";
struct Program2D { GLuint id = 0; GLint size, tex, useTex, flipV; };
static Program2D s_p2d;
static GLuint s_vao3d = 0, s_vao2d = 0, s_vbo3d[2] = {}, s_ibo3d = 0, s_vbo2d = 0;

// ---- textures

static GLint glWrap(int w) { return w == GPU_REPEAT ? GL_REPEAT : GL_CLAMP_TO_EDGE; }

// ETC1 / ETC1A4's colour blocks in the order GL reads them: the PICA keeps four 4 x 4 blocks to each 8 x 8 tile (each
// with its alpha first in ETC1A4) and each block's 64 bits little-endian; GL wants the blocks in rows, big-endian
static std::vector<u8> etc1Linear(const u8* d, int w, int h, bool hasAlpha)
{
	std::vector<u8> out((size_t)w * h / 2);
	int bw = w / 4;
	for (int ty = 0; ty < h / 8; ty++)
		for (int tx = 0; tx < w / 8; tx++)
			for (int b = 0; b < 4; b++)
			{
				if (hasAlpha)
					d += 8;
				int bx = tx * 2 + (b & 1), by = ty * 2 + (b >> 1);
				u8* o = &out[((size_t)by * bw + bx) * 8];
				for (int i = 0; i < 8; i++)
					o[i] = d[7 - i];
				d += 8;
			}
	return out;
}

static void uploadTex(C3D_Tex* t, GlTex* g)
{
	if (!g->id)
		glGenTextures(1, &g->id);
	glBindTexture(GL_TEXTURE_2D, g->id);
	g->dirty = false;
	const u8* d = (const u8*)t->data;
	if (!d)
		return;
	size_t at = 0;
	int levels = 0;
	std::vector<u32> px;
	for (int k = 0; k <= t->maxLevel; k++)
	{
		int w = t->width >> k, h = t->height >> k;
		if (w < 8 || h < 8)
			break;
		if (t->fmt == GPU_ETC1)
		{
			std::vector<u8> blocks = etc1Linear(d + at, w, h, false);
			glCompressedTexImage2D(GL_TEXTURE_2D, k, s_etc1 ? GL_ETC1_RGB8_OES : GL_COMPRESSED_RGB8_ETC2, w, h, 0,
				(GLsizei)blocks.size(), blocks.data());
		}
		else
		{
			px.assign((size_t)w * h, 0xFFFF00FF);
			decodeLevelInto(d + at, w, h, t->fmt, px);
			glTexImage2D(GL_TEXTURE_2D, k, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
		}
		at += (size_t)w * h * texBits(t->fmt) / 8;
		levels++;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, std::max(0, levels - 1));
}

// A texture on a unit, uploaded if it changed, with its filter and wrap; its v the other way up (1) unless it is a
// render target's
static float bindTex(int unit, C3D_Tex* t)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	if (!t)
	{
		glBindTexture(GL_TEXTURE_2D, s_white);
		return 0.0f;
	}
	GlTex* g = glTex(t);
	if (g->target)
		glBindTexture(GL_TEXTURE_2D, g->id);
	else if (g->dirty || !g->id)
		uploadTex(t, g);
	else
		glBindTexture(GL_TEXTURE_2D, g->id);
	bool mips = !g->target && t->maxLevel > 0;
	GLint mag = t->filter == GPU_LINEAR ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? (mag == GL_LINEAR ? GL_LINEAR_MIPMAP_NEAREST :
		GL_NEAREST_MIPMAP_NEAREST) : mag);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, glWrap(t->wrapS));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, glWrap(t->wrapT));
	return g->target ? 0.0f : 1.0f;
}

static float lodBias(const C3D_Tex* t)
{
	if (!t)
		return 0.0f;
	int b = t->lodBias & 0x1FFF;
	if (b & 0x1000)
		b -= 0x2000;
	return b / 256.0f;
}

// ---- .t3x (native_gpu.cpp's importT3x: the header, the subtextures, the data with its smaller copies)

struct T3x { std::vector<Tex3DS_SubTexture> subs; };

static Tex3DS_Texture importT3x(const u8* d, size_t n, C3D_Tex* tex)
{
	if (n < 5)
		return nullptr;
	int count = d[0] | (d[1] << 8);
	int wl = d[2] & 7, hl = (d[2] >> 3) & 7, fmt = d[3], mips = d[4];
	size_t p = 5;
	auto* t = new T3x();
	for (int i = 0; i < count && p + 12 <= n; i++, p += 12)
	{
		u16 v[6];
		for (int k = 0; k < 6; k++)
			v[k] = d[p + k * 2] | (d[p + k * 2 + 1] << 8);
		t->subs.push_back({ v[0], v[1], v[2] / 1024.0f, v[3] / 1024.0f, v[4] / 1024.0f, v[5] / 1024.0f });
	}
	std::vector<u8> data = decompress(d + p, n - p);
	if (!C3D_TexInit(tex, 1 << (wl + 3), 1 << (hl + 3), fmt))
	{
		delete t;
		return nullptr;
	}
	tex->maxLevel = mips;
	size_t all = 0;
	for (int k = 0; k <= mips; k++)
		all += (size_t)(tex->width >> k) * (tex->height >> k) * texBits(fmt) / 8;
	if (all > tex->size)
	{
		void* more = realloc(tex->data, all);
		if (more)
			tex->data = more;
		else
			tex->maxLevel = 0;
	}
	memcpy(tex->data, data.data(), std::min<size_t>(data.size(), tex->maxLevel ? all : tex->size));
	glTex(tex)->dirty = true;
	return (Tex3DS_Texture)t;
}

Tex3DS_Texture Tex3DS_TextureImport(const void* data, size_t size, C3D_Tex* tex, void*, bool)
{
	return importT3x((const u8*)data, size, tex);
}

Tex3DS_Texture Tex3DS_TextureImportStdio(FILE* f, C3D_Tex* tex, void*, bool)
{
	std::vector<u8> buf;
	u8 chunk[65536];
	size_t got;
	while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0)
		buf.insert(buf.end(), chunk, chunk + got);
	return importT3x(buf.data(), buf.size(), tex);
}

void Tex3DS_TextureFree(Tex3DS_Texture t)
{
	delete (T3x*)t;
}

bool C3D_TexInit(C3D_Tex* t, u16 w, u16 h, int fmt)
{
	t->width = w;
	t->height = h;
	t->fmt = fmt;
	t->size = (u32)w * h * texBits(fmt) / 8;
	t->maxLevel = 0;
	t->wrapS = t->wrapT = GPU_CLAMP_TO_EDGE;
	t->filter = GPU_NEAREST;
	t->native = nullptr;
	// (a texture is linear memory on the console: none made when there isn't room, as there)
	if (t->size > linearSpaceFree())
	{
		t->data = nullptr;
		return false;
	}
	t->data = calloc(1, t->size ? t->size : 1);
	return t->data != nullptr;
}

bool C3D_TexInitVRAM(C3D_Tex* t, u16 w, u16 h, int fmt) { return C3D_TexInit(t, w, h, fmt); }

void C3D_TexDelete(C3D_Tex* t)
{
	free(t->data);
	t->data = nullptr;
	if (GlTex* g = (GlTex*)t->native)
	{
		if (g->id && !g->target)
			glDeleteTextures(1, &g->id);
		if (g->target)
			g->target->tex = nullptr;
		delete g;
	}
	t->native = nullptr;
}

void C3D_TexFlush(C3D_Tex* t)
{
	glTex(t)->dirty = true;
}

void C3D_TexSetFilter(C3D_Tex* t, int mag, int) { t->filter = mag; }
void C3D_TexSetWrap(C3D_Tex* t, int s, int tt) { t->wrapS = s; t->wrapT = tt; }

// ---- citro2d's batch: quads and triangles in one target with one texture, drawn when either changes

struct Vertex2D { float x, y, r, g, b, a, u, v, k; };
static std::vector<Vertex2D> s_batch;
static C3D_RenderTarget* s_batchTarget = nullptr;
static C3D_Tex* s_batchTex = nullptr;
static bool s_batchWindow = false;          // the overlay: the watch's own framebuffer (glesWindowBegin)
static int s_windowW = 0, s_windowH = 0;

static void bindTarget(C3D_RenderTarget* t)
{
	glBindFramebuffer(GL_FRAMEBUFFER, t ? t->fbo : 0);
	if (t)
		glViewport(0, 0, t->w, t->h);
	else
		glViewport(0, 0, s_windowW, s_windowH);
}

// The game passes the scissor in the framebuffer's sideways coordinates (native_gpu.cpp scissorRect); GL counts rows
// from the bottom
static void applyScissor(C3D_RenderTarget* t)
{
	if (!s_scissorOn || !t)
	{
		glDisable(GL_SCISSOR_TEST);
		return;
	}
	int x0 = std::max(0, t->w - (int)s_scissor[3]), x1 = std::min(t->w, t->w - (int)s_scissor[1]);
	int y0 = std::max(0, (int)s_scissor[0]), y1 = std::min(t->h, (int)s_scissor[2]);
	glEnable(GL_SCISSOR_TEST);
	glScissor(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

static void flush2D()
{
	if (s_batch.empty())
		return;
	bindTarget(s_batchWindow ? nullptr : s_batchTarget);
	glUseProgram(s_p2d.id);
	float w = s_batchWindow ? (float)s_windowW : (float)s_batchTarget->w;
	float h = s_batchWindow ? (float)s_windowH : (float)s_batchTarget->h;
	glUniform2f(s_p2d.size, w, h);
	float flip = bindTex(0, s_batchTex);
	glUniform1f(s_p2d.useTex, s_batchTex ? 1.0f : 0.0f);
	glUniform1f(s_p2d.flipV, flip);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDepthMask(GL_FALSE);
	if (s_batchWindow)
		glDisable(GL_SCISSOR_TEST);
	else
		applyScissor(s_batchTarget);
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindVertexArray(s_vao2d);
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo2d);
	glBufferData(GL_ARRAY_BUFFER, s_batch.size() * sizeof(Vertex2D), s_batch.data(), GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, (GLsizei)s_batch.size());
	glBindVertexArray(0);
	s_batch.clear();
}

static void batchFor(C3D_RenderTarget* t, C3D_Tex* tex, bool window)
{
	if (!s_batch.empty() && (t != s_batchTarget || tex != s_batchTex || window != s_batchWindow))
		flush2D();
	s_batchTarget = t;
	s_batchTex = tex;
	s_batchWindow = window;
}

// ---- citro3d

static void initGl()
{
	const char* ext = (const char*)glGetString(GL_EXTENSIONS);
	s_etc1 = ext && strstr(ext, "GL_OES_compressed_ETC1_RGB8_texture");
	logf("gles: %s, %s, %s; ETC1 %s", (const char*)glGetString(GL_VENDOR), (const char*)glGetString(GL_RENDERER),
		(const char*)glGetString(GL_VERSION), s_etc1 ? "as ETC1" : "as ETC2");
	glGenTextures(1, &s_white);
	glBindTexture(GL_TEXTURE_2D, s_white);
	u32 white = 0xFFFFFFFF;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	static const char* const k2DAttribs[] = { "pos", "color", "uvk" };
	s_p2d.id = linkProgram(k2DVertex, k2DFragment, k2DAttribs, 3);
	s_p2d.size = glGetUniformLocation(s_p2d.id, "size");
	s_p2d.tex = glGetUniformLocation(s_p2d.id, "tex");
	s_p2d.useTex = glGetUniformLocation(s_p2d.id, "useTex");
	s_p2d.flipV = glGetUniformLocation(s_p2d.id, "flipV");
	glUseProgram(s_p2d.id);
	glUniform1i(s_p2d.tex, 0);
	glGenVertexArrays(1, &s_vao2d);
	glGenBuffers(1, &s_vbo2d);
	glBindVertexArray(s_vao2d);
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo2d);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), (void*)offsetof(Vertex2D, x));
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), (void*)offsetof(Vertex2D, r));
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex2D), (void*)offsetof(Vertex2D, u));
	glBindVertexArray(0);
	glGenVertexArrays(1, &s_vao3d);
	glGenBuffers(2, s_vbo3d);
	glGenBuffers(1, &s_ibo3d);
}

bool C3D_Init(size_t)
{
	for (auto& e : s_env)
		C3D_TexEnvInit(&e);
	initGl();
	return true;
}
void C3D_Fini() {}

float C3D_FrameRate(float fps)
{
	if (fps > 0.0f)
		s_frameRate = fps;
	return s_frameRate;
}

bool C3D_FrameBegin(u8) { return true; }

static C3D_RenderTarget* screenTarget(int screen)
{
	C3D_RenderTarget* found = nullptr;
	for (auto* t : s_targets)
		if (t->screen == screen && (screen == GFX_BOTTOM || t->side != GFX_RIGHT))
			found = t;
	return found;
}

// The frame onto the watch's display: the screen android_input.cpp shows, where it says, then its controls; at the
// rate the game asks for (C3D_FrameRate: 30 a second, of the display's 60)
void C3D_FrameEnd(u8)
{
	flush2D();
	SDL_Window* win = androidWindow();
	SDL_GetWindowSizeInPixels(win, &s_windowW, &s_windowH);
	bindTarget(nullptr);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	ScreenView view = androidScreenView(s_windowW, s_windowH);
	if (C3D_RenderTarget* t = screenTarget(view.screen))
	{
		// (the screen's texture, drawn as citro2d's image of it: upright, a render target's way up)
		static C3D_Tex screenTex;
		static GlTex screenGl;
		screenGl.id = t->color;
		screenGl.target = t;
		screenTex.native = &screenGl;
		screenTex.filter = GPU_LINEAR;
		screenTex.wrapS = screenTex.wrapT = GPU_CLAMP_TO_EDGE;
		batchFor(nullptr, &screenTex, true);
		float x0 = view.x, y0 = view.y, x1 = view.x + view.w, y1 = view.y + view.h;
		Vertex2D q[6] = { { x0, y0, 1, 1, 1, 1, 0, 1, 0 }, { x1, y0, 1, 1, 1, 1, 1, 1, 0 }, { x0, y1, 1, 1, 1, 1, 0, 0, 0 },
			{ x1, y0, 1, 1, 1, 1, 1, 1, 0 }, { x1, y1, 1, 1, 1, 1, 1, 0, 0 }, { x0, y1, 1, 1, 1, 1, 0, 0, 0 } };
		s_batch.insert(s_batch.end(), q, q + 6);
		flush2D();
	}
	androidDrawOverlay(s_windowW, s_windowH);
	flush2D();
	SDL_GL_SwapWindow(win);
	// (the swap waits for the display; a rate below its own sleeps the rest of the frame first)
	static Uint64 last = 0;
	Uint64 now = SDL_GetTicksNS();
	float rate = std::min(s_frameRate, (float)g_fpsCap);     // (Options > Video > Frame rate)
	Uint64 period = (Uint64)(1e9 / std::max(10.0f, rate));
	if (rate < 59.0f && last && now - last < period - 4000000)
		SDL_DelayPrecise(period - 4000000 - (now - last));
	last = SDL_GetTicksNS();
}

void C3D_FrameDrawOn(C3D_RenderTarget* t) { s_target = t; }

static C3D_RenderTarget* newTarget(int w, int h, GLuint colorTex, bool depth)
{
	auto* t = new C3D_RenderTarget();
	t->w = w;
	t->h = h;
	if (colorTex)
		t->color = colorTex;
	else
	{
		glGenTextures(1, &t->color);
		glBindTexture(GL_TEXTURE_2D, t->color);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glGenFramebuffers(1, &t->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
	if (depth)
	{
		glGenRenderbuffers(1, &t->depth);
		glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
	}
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE)
		logf("gles: render target %d x %d incomplete (0x%x)", w, h, status);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	s_targets.push_back(t);
	return t;
}

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, int colorFmt, int depthFmt)
{
	C3D_RenderTarget* t = newTarget(height, width, 0, depthFmt != -1);    // made sideways, as the screens are
	t->rgb565 = colorFmt == GPU_RB_RGB565;
	return t;
}

C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, int, int, int depthFmt)
{
	GlTex* g = glTex(tex);
	if (g->id)
		glDeleteTextures(1, &g->id);
	glGenTextures(1, &g->id);
	glBindTexture(GL_TEXTURE_2D, g->id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex->width, tex->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	C3D_RenderTarget* t = newTarget(tex->width, tex->height, g->id, depthFmt != -1);
	t->tex = tex;
	g->target = t;
	g->dirty = false;
	return t;
}

void C3D_RenderTargetSetOutput(C3D_RenderTarget* t, int screen, int side, u32)
{
	t->screen = screen;
	t->side = side;
}

C3D_RenderTarget* C2D_CreateScreenTarget(int screen, int side)
{
	C3D_RenderTarget* t = newTarget(screen == GFX_TOP ? 400 : 320, 240, 0, true);
	C3D_RenderTargetSetOutput(t, screen, side, 0);
	return t;
}

void C3D_RenderTargetDelete(C3D_RenderTarget* t)
{
	flush2D();
	s_targets.erase(std::remove(s_targets.begin(), s_targets.end(), t), s_targets.end());
	if (t->tex && t->tex->native)
		((GlTex*)t->tex->native)->target = nullptr;     // (its texture keeps the picture: the texture's own now)
	else if (t->color)
		glDeleteTextures(1, &t->color);
	if (t->depth)
		glDeleteRenderbuffers(1, &t->depth);
	glDeleteFramebuffers(1, &t->fbo);
	if (s_target == t)
		s_target = nullptr;
	if (s_batchTarget == t)
		s_batchTarget = nullptr;
	delete t;
}

// A clear fills the whole target, as the PICA's memory fill does: no scissor
static void clearTarget(C3D_RenderTarget* t, bool color, bool depth, u32 c, float z)
{
	if (!t)
		return;
	flush2D();
	bindTarget(t);
	glDisable(GL_SCISSOR_TEST);
	GLbitfield bits = 0;
	if (color)
	{
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor((c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f, (c >> 24) / 255.0f);
		bits |= GL_COLOR_BUFFER_BIT;
	}
	if (depth && t->depth)
	{
		glDepthMask(GL_TRUE);
		glClearDepthf(z);
		bits |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
	}
	if (bits)
		glClear(bits);
}

void C3D_RenderTargetClear(C3D_RenderTarget* t, int bits, u32 c, u32 depth)
{
	// citro3d's clear colour is 0xRRGGBBAA; a 16-bit target takes it as RGB565 (the GPU's fill writes the low bits)
	if (t->rgb565)
		c = ((c >> 11 & 0x1F) * 255 / 31) << 24 | ((c >> 5 & 0x3F) * 255 / 63) << 16 | ((c & 0x1F) * 255 / 31) << 8 | 0xFF;
	clearTarget(t, bits != C3D_CLEAR_DEPTH, bits != C3D_CLEAR_COLOR, rgba(c >> 24, (c >> 16) & 255, (c >> 8) & 255, c & 255),
		(depth & 0xFFFFFF) / 16777215.0f);
}

void C2D_TargetClear(C3D_RenderTarget* t, u32 color) { clearTarget(t, true, true, color, 0.0f); }
void C2D_SceneBegin(C3D_RenderTarget* t) { s_target = t; }
void C2D_Prepare() {}
void C2D_ViewReset() { s_view2d[0] = s_view2d[1] = 0.0f; }
void C2D_ViewTranslate(float x, float y) { s_view2d[0] += x; s_view2d[1] += y; }

void C3D_DepthTest(bool enable, int function, int writemask)
{
	s_depthOn = enable;
	s_depthFunc = function;
	s_depthWrite = writemask == GPU_WRITE_ALL;
}
void C3D_DepthMap(bool, float zScale, float zOffset) { s_zScale = zScale; s_zOffset = zOffset; }
void C3D_CullFace(int mode) { s_cull = mode; }
void C3D_AlphaTest(bool enable, int function, int ref) { s_alphaTestOn = enable; s_alphaFunc = function; s_alphaRef = ref; }
void C3D_AlphaBlend(int, int, int srcClr, int dstClr, int srcAlpha, int dstAlpha)
{
	s_blend[0] = srcClr;
	s_blend[1] = dstClr;
	s_blend[2] = srcAlpha;
	s_blend[3] = dstAlpha;
}
void C3D_SetScissor(int mode, u32 left, u32 top, u32 right, u32 bottom)
{
	flush2D();
	s_scissorOn = mode == GPU_SCISSOR_NORMAL;
	s_scissor[0] = left;
	s_scissor[1] = top;
	s_scissor[2] = right;
	s_scissor[3] = bottom;
}
void C3D_TexBind(int unit, C3D_Tex* t)
{
	if (unit >= 0 && unit < 3)
		s_units[unit] = t;
}

C3D_TexEnv* C3D_GetTexEnv(int id) { s_envChanged = true; return &s_env[std::clamp(id, 0, 5)]; }
void C3D_TexEnvInit(C3D_TexEnv* env)
{
	s_envChanged = true;
	for (int i = 0; i < 3; i++)
	{
		env->srcRgb[i] = env->srcAlpha[i] = i == 0 ? GPU_PREVIOUS : GPU_PRIMARY_COLOR;
		env->opRgb[i] = GPU_TEVOP_RGB_SRC_COLOR;
		env->opAlpha[i] = GPU_TEVOP_A_SRC_ALPHA;
	}
	env->funcRgb = env->funcAlpha = GPU_REPLACE;
	env->color = 0xFFFFFFFF;
}
void C3D_TexEnvSrc(C3D_TexEnv* env, int mode, int s1, int s2, int s3)
{
	s_envChanged = true;
	int s[3] = { s1, s2, s3 };
	for (int i = 0; i < 3; i++)
	{
		if (mode & C3D_RGB)
			env->srcRgb[i] = s[i];
		if (mode & C3D_Alpha)
			env->srcAlpha[i] = s[i];
	}
}
void C3D_TexEnvOpRgb(C3D_TexEnv* env, int o1, int o2, int o3)
{
	s_envChanged = true;
	env->opRgb[0] = o1;
	env->opRgb[1] = o2;
	env->opRgb[2] = o3;
}
void C3D_TexEnvFunc(C3D_TexEnv* env, int mode, int func)
{
	s_envChanged = true;
	if (mode & C3D_RGB)
		env->funcRgb = func;
	if (mode & C3D_Alpha)
		env->funcAlpha = func;
}
void C3D_TexEnvColor(C3D_TexEnv* env, u32 color) { env->color = color; }

DVLB_s* DVLB_ParseFile(u32*, u32) { return new DVLB_s(); }
void DVLB_Free(DVLB_s* d) { delete d; }
int shaderProgramInit(shaderProgram_s* p) { static shaderInstance_s si; p->vertexShader = &si; return 0; }
int shaderProgramFree(shaderProgram_s*) { return 0; }
int shaderProgramSetVsh(shaderProgram_s*, DVLE_s*) { return 0; }
s8 shaderInstanceGetUniformLocation(shaderInstance_s*, const char* name)
{
	static const struct { const char* name; int slot; } kSlots[] = { { "projection", U_PROJECTION },
		{ "modelView", U_MODELVIEW }, { "uvOffset", U_UVOFFSET }, { "fogVec", U_FOGVEC }, { "posOffset", U_POSOFFSET },
		{ "posScale", U_POSSCALE }, { "uvScale", U_UVSCALE }, { "tint", U_TINT }, { "lightPos", U_LIGHTPOS },
		{ "lightColor", U_LIGHTCOLOR }, { "ambientAdd", U_AMBIENT }, { "torchVec", U_TORCHVEC }, { "bones", U_BONES },
		{ "lamp", U_LAMP }, { "sideLight", U_SIDE }, { "stereoCurve", U_STEREOCURVE },
		{ "gammaOn", B_GAMMA }, { "lightOn", B_LIGHTON }, { "stereoOn", B_STEREOON } };
	for (auto& s : kSlots)
		if (strcmp(s.name, name) == 0)
			return s.slot;
	return -1;
}
void C3D_BindProgram(shaderProgram_s*) {}

// C3D_FVUnifWritePtr's rows: taken into the uniforms before the next draw or single set (native_gpu.cpp's, at full
// precision here)
static C3D_FVec s_unifStaged[96];
static int s_stagedFrom = 96, s_stagedTo = 0;
static void unifStagedTake()
{
	if (s_stagedFrom < s_stagedTo)
		s_unifChanges++;
	for (int id = s_stagedFrom; id < s_stagedTo; id++)
	{
		s_unif[id][0] = s_unifStaged[id].x;
		s_unif[id][1] = s_unifStaged[id].y;
		s_unif[id][2] = s_unifStaged[id].z;
		s_unif[id][3] = s_unifStaged[id].w;
	}
	s_stagedFrom = 96, s_stagedTo = 0;
}
C3D_FVec* C3D_FVUnifWritePtr(int, int id, int size)
{
	static C3D_FVec spare[96];
	if (id < 0 || size < 0 || id + size > 96)
		return spare;
	unifStagedTake();
	s_stagedFrom = id, s_stagedTo = id + size;
	for (int k = id; k < id + size; k++)
		s_unifStaged[k] = FVec4_New(s_unif[k][0], s_unif[k][1], s_unif[k][2], s_unif[k][3]);
	return &s_unifStaged[id];
}
void C3D_FVUnifSet(int, int id, float x, float y, float z, float w)
{
	unifStagedTake();
	if (id < 0 || id >= 96)
		return;
	s_unifChanges++;
	s_unif[id][0] = x;
	s_unif[id][1] = y;
	s_unif[id][2] = z;
	s_unif[id][3] = w;
}
void C3D_BoolUnifSet(int, int id, bool value)
{
	if (id == B_LIGHTON)
		s_lightOn = value;
	else if (id == B_GAMMA)
		s_gammaOn = value;
	else if (id == B_STEREOON)
		s_stereoOn = value;
}
void C3D_FVUnifMtx4x4(int, int id, const C3D_Mtx* m)
{
	for (int r = 0; r < 4; r++)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, id + r, m->r[r].x, m->r[r].y, m->r[r].z, m->r[r].w);
}

C3D_AttrInfo* C3D_GetAttrInfo() { return &s_attr; }
void AttrInfo_Init(C3D_AttrInfo* info) { info->count = 0; }
int AttrInfo_AddLoader(C3D_AttrInfo* info, int, int format, int count)
{
	if (info->count >= 8)
		return -1;
	info->format[info->count] = format;
	info->elements[info->count] = count;
	return info->count++;
}
C3D_BufInfo* C3D_GetBufInfo() { return &s_buf; }
void BufInfo_Init(C3D_BufInfo* info) { info->data = nullptr; info->stride = 0; info->data2 = nullptr; info->stride2 = 0; info->split = 8; }
// The game's layouts: one buffer of every attribute in order, or (a shared model's batch) a second one holding the
// attributes after the first one's
int BufInfo_Add(C3D_BufInfo* info, const void* data, ptrdiff_t stride, int attribCount, u64)
{
	if (info->data)
	{
		info->data2 = data;
		info->stride2 = (int)stride;
		return 1;
	}
	info->data = data;
	info->stride = (int)stride;
	info->split = attribCount;
	return 0;
}

float C3D_GetDrawingTime() { return 0.0f; }

static GLenum glAttrType(int fmt)
{
	return fmt == GPU_FLOAT ? GL_FLOAT : fmt == GPU_SHORT ? GL_SHORT : fmt == GPU_BYTE ? GL_BYTE : GL_UNSIGNED_BYTE;
}
static int attrSize(int fmt) { return fmt == GPU_FLOAT ? 4 : fmt == GPU_SHORT ? 2 : 1; }

static GLenum glCompare(int f)
{
	switch (f)
	{
	case GPU_NEVER: return GL_NEVER;
	case GPU_EQUAL: return GL_EQUAL;
	case GPU_NOTEQUAL: return GL_NOTEQUAL;
	case GPU_LESS: return GL_LESS;
	case GPU_LEQUAL: return GL_LEQUAL;
	case GPU_GREATER: return GL_GREATER;
	case GPU_GEQUAL: return GL_GEQUAL;
	default: return GL_ALWAYS;
	}
}

static int alphaCode(int f)
{
	switch (f)
	{
	case GPU_NEVER: return 0;
	case GPU_EQUAL: return 1;
	case GPU_NOTEQUAL: return 2;
	case GPU_LESS: return 3;
	case GPU_LEQUAL: return 4;
	case GPU_GREATER: return 5;
	case GPU_GEQUAL: return 6;
	default: return 7;
	}
}

static GLenum glFactor(int f)
{
	switch (f)
	{
	case GPU_ZERO: return GL_ZERO;
	case GPU_SRC_ALPHA: return GL_SRC_ALPHA;
	case GPU_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
	default: return GL_ONE;
	}
}

void C3D_DrawElements(int, int count, int type, const void* indices)
{
	unifStagedTake();
	C3D_RenderTarget* t = s_target;
	if (!t || !s_buf.data || count < 3)
		return;
	flush2D();
	bool shortIndex = type != C3D_UNSIGNED_BYTE;
	int maxIndex = 0;
	if (shortIndex)
		for (int i = 0; i < count; i++)
			maxIndex = std::max(maxIndex, (int)((const u16*)indices)[i]);
	else
		for (int i = 0; i < count; i++)
			maxIndex = std::max(maxIndex, (int)((const u8*)indices)[i]);
	bool skinned = s_attr.count >= 5;
	Program3D& p = program3D(skinned);
	bindTarget(t);
	glUseProgram(p.id);
	if (p.unifSeen != s_unifChanges)
	{
		glUniform4fv(p.u, 96, &s_unif[0][0]);
		p.unifSeen = s_unifChanges;
	}
	glUniform1i(p.lightOn, s_lightOn);
	glUniform1i(p.gammaOn, s_gammaOn);
	glUniform2f(p.zMap, s_zScale, s_zOffset);
	float consts[6][4];
	for (int i = 0; i < 6; i++)
	{
		u32 c = s_env[i].color;
		consts[i][0] = (c & 255) / 255.0f;
		consts[i][1] = ((c >> 8) & 255) / 255.0f;
		consts[i][2] = ((c >> 16) & 255) / 255.0f;
		consts[i][3] = (c >> 24) / 255.0f;
	}
	glUniform4fv(p.consts, 6, &consts[0][0]);
	glUniform1i(p.alphaFunc, s_alphaTestOn ? alphaCode(s_alphaFunc) : 7);
	glUniform1f(p.alphaRef, (float)s_alphaRef);
	float f0 = bindTex(0, s_units[0]), f1 = bindTex(1, s_units[1]);
	glUniform2f(p.flipV, f0, f1);
	glUniform2f(p.lodBias, lodBias(s_units[0]), lodBias(s_units[1]));
	glActiveTexture(GL_TEXTURE0);
	// Depth, culling (the PICA's front faces are counter-clockwise as seen, as GL's are), blending, scissor
	if (s_depthOn)
	{
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(glCompare(s_depthFunc));
	}
	else
		glDisable(GL_DEPTH_TEST);
	glDepthMask(s_depthOn && s_depthWrite ? GL_TRUE : GL_FALSE);
	if (s_cull == GPU_CULL_BACK_CCW)
	{
		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		glFrontFace(GL_CCW);
	}
	else
		glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFuncSeparate(glFactor(s_blend[0]), glFactor(s_blend[1]), glFactor(s_blend[2]), glFactor(s_blend[3]));
	applyScissor(t);
	// The vertices the indices reach, from the game's buffers (one, or two from the attribute `split` on)
	glBindVertexArray(s_vao3d);
	int n = maxIndex + 1;
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo3d[0]);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * s_buf.stride, s_buf.data, GL_STREAM_DRAW);
	if (s_buf.data2)
	{
		glBindBuffer(GL_ARRAY_BUFFER, s_vbo3d[1]);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * s_buf.stride2, s_buf.data2, GL_STREAM_DRAW);
	}
	int off = 0;
	for (int i = 0; i < 5; i++)
	{
		if (i >= s_attr.count)
		{
			glDisableVertexAttribArray(i);
			glVertexAttrib4f(i, 0, 0, 0, 1);
			continue;
		}
		bool second = s_buf.data2 && i >= s_buf.split;
		if (second && i == s_buf.split)
			off = 0;
		glBindBuffer(GL_ARRAY_BUFFER, s_vbo3d[second ? 1 : 0]);
		glEnableVertexAttribArray(i);
		glVertexAttribPointer(i, s_attr.elements[i], glAttrType(s_attr.format[i]), GL_FALSE,
			second ? s_buf.stride2 : s_buf.stride, (void*)(intptr_t)off);
		off += attrSize(s_attr.format[i]) * s_attr.elements[i];
	}
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo3d);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)count * (shortIndex ? 2 : 1), indices, GL_STREAM_DRAW);
	glDrawElements(GL_TRIANGLES, count, shortIndex ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, nullptr);
	glBindVertexArray(0);
}

// ---- citro2d: screen pixels, alpha blended, no depth; images tinted as citro2d does (rgb towards the tint by its
// blend, alpha multiplied)

static Vertex2D v2(float x, float y, u32 c, float u = 0, float v = 0, float k = 0)
{
	return { x + s_view2d[0], y + s_view2d[1], (c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f,
		(c >> 24) / 255.0f, u, v, k };
}

bool C2D_DrawRectSolid(float x, float y, float, float w, float h, u32 clr)
{
	if (!s_target)
		return true;
	batchFor(s_target, nullptr, false);
	Vertex2D q[6] = { v2(x, y, clr), v2(x + w, y, clr), v2(x, y + h, clr), v2(x + w, y, clr), v2(x + w, y + h, clr),
		v2(x, y + h, clr) };
	s_batch.insert(s_batch.end(), q, q + 6);
	return true;
}

bool C2D_DrawTriangle(float x0, float y0, u32 c0, float x1, float y1, u32 c1, float x2, float y2, u32 c2, float)
{
	if (!s_target)
		return true;
	batchFor(s_target, nullptr, false);
	Vertex2D q[3] = { v2(x0, y0, c0), v2(x1, y1, c1), v2(x2, y2, c2) };
	s_batch.insert(s_batch.end(), q, q + 3);
	return true;
}

// Corners top left, top right, bottom left, bottom right, around (cx, cy) by angle
static bool drawImage(C2D_Image img, float cx, float cy, float hw, float hh, float angle, const C2D_ImageTint* tint)
{
	if (!s_target || !img.tex || !img.subtex)
		return true;
	batchFor(s_target, img.tex, false);
	const Tex3DS_SubTexture* s = img.subtex;
	float c = cosf(angle), sn = sinf(angle);
	static const float kCorner[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
	float uv[4][2] = { { s->left, s->top }, { s->right, s->top }, { s->left, s->bottom }, { s->right, s->bottom } };
	Vertex2D v[4];
	for (int i = 0; i < 4; i++)
	{
		float dx = kCorner[i][0] * hw, dy = kCorner[i][1] * hh;
		u32 color = tint ? tint->corners[i].color : 0xFFFFFFFF;
		float blend = tint ? tint->corners[i].blend : 0.0f;
		v[i] = v2(cx + dx * c - dy * sn, cy + dx * sn + dy * c, color, uv[i][0], uv[i][1], blend);
	}
	Vertex2D q[6] = { v[0], v[1], v[2], v[2], v[1], v[3] };
	s_batch.insert(s_batch.end(), q, q + 6);
	return true;
}

bool C2D_DrawImageAt(C2D_Image img, float x, float y, float, const C2D_ImageTint* tint, float sx, float sy)
{
	if (!img.subtex)
		return true;
	float w = img.subtex->width * sx, h = img.subtex->height * sy;
	return drawImage(img, x + w / 2, y + h / 2, w / 2, h / 2, 0.0f, tint);
}

bool C2D_DrawImageAtRotated(C2D_Image img, float x, float y, float, float angle, const C2D_ImageTint* tint, float sx, float sy)
{
	if (!img.subtex)
		return true;
	return drawImage(img, x, y, img.subtex->width * sx / 2, img.subtex->height * sy / 2, angle, tint);
}

// The 3DS's system font (before the theme's Morrowind font is loaded: the title, the data download): the watch's own,
// drawn by MorrowindActivity.renderGlyphs into a texture of characters 32 to 255. The game lays its text out with the
// stand-in's metrics (native_compat.h: every character 13 wide, lines 30 apart), so each is drawn in a cell that size
static const int kGlyphW = 13, kGlyphH = 30, kGlyphScale = 2, kGlyphColumns = 16, kGlyphRows = 14;

static C3D_Tex* systemFont()
{
	static C3D_Tex tex;
	static GlTex gl;
	static bool tried = false;
	if (tried)
		return gl.id ? &tex : nullptr;
	tried = true;
	JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
	jobject activity = (jobject)SDL_GetAndroidActivity();
	if (!env || !activity)
		return nullptr;
	jclass cls = env->GetObjectClass(activity);
	jmethodID render = env->GetStaticMethodID(cls, "renderGlyphs", "(II)[I");
	int cw = kGlyphW * kGlyphScale, ch = kGlyphH * kGlyphScale, w = cw * kGlyphColumns, h = ch * kGlyphRows;
	jintArray got = render ? (jintArray)env->CallStaticObjectMethod(cls, render, cw, ch) : nullptr;
	if (env->ExceptionCheck())
		env->ExceptionClear();
	if (got && env->GetArrayLength(got) == w * h)
	{
		std::vector<u32> px(w * h);
		env->GetIntArrayRegion(got, 0, w * h, (jint*)px.data());
		for (auto& p : px)          // (ARGB to RGBA bytes)
			p = (p & 0xFF00FF00) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
		glGenTextures(1, &gl.id);
		glBindTexture(GL_TEXTURE_2D, gl.id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		gl.dirty = false;
		tex.native = &gl;
		tex.width = (u16)w;
		tex.height = (u16)h;
		tex.filter = GPU_LINEAR;
		tex.wrapS = tex.wrapT = GPU_CLAMP_TO_EDGE;
		logf("gles: the system font from the watch's (%d x %d)", w, h);
	}
	else
		logf("gles: no system font (the text shows as blocks)");
	if (got)
		env->DeleteLocalRef(got);
	env->DeleteLocalRef(cls);
	env->DeleteLocalRef(activity);
	return gl.id ? &tex : nullptr;
}

void C2D_DrawText(const C2D_Text* t, u32, float x, float y, float, float sx, float sy, ...)
{
	if (!s_target || !t || !t->s)
		return;
	va_list args;
	va_start(args, sy);
	u32 color = va_arg(args, u32);
	va_end(args);
	C3D_Tex* font = systemFont();
	float pen = x;
	for (const u8* p = (const u8*)t->s; *p;)
	{
		// (UTF-8: characters past 255 as '?')
		u32 c = *p++;
		if (c >= 0x80)
		{
			int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
			c &= more == 3 ? 0x07 : more == 2 ? 0x0F : 0x1F;
			for (; more > 0 && (*p & 0xC0) == 0x80; more--)
				c = (c << 6) | (*p++ & 0x3F);
		}
		if (c == 10)                // (a line break: the game draws each line by itself)
			continue;
		if (c > 255 || (c >= 127 && c < 160))
			c = '?';
		if (c > ' ')
		{
			if (!font)
				C2D_DrawRectSolid(pen + 2 * sx, y + 8 * sy, 0.5f, 9 * sx, 16 * sy, (color & 0x00FFFFFF) | 0x90000000);
			else
			{
				batchFor(s_target, font, false);
				int at = (int)c - 32;
				float u0 = (float)(at % kGlyphColumns) / kGlyphColumns, u1 = u0 + 1.0f / kGlyphColumns;
				float v0 = 1.0f - (float)(at / kGlyphColumns) / kGlyphRows, v1 = v0 - 1.0f / kGlyphRows;
				float x0 = pen, y0 = y, x1 = pen + kGlyphW * sx, y1 = y + kGlyphH * sy;
				Vertex2D q[6] = { v2(x0, y0, color, u0, v0, 1), v2(x1, y0, color, u1, v0, 1), v2(x0, y1, color, u0, v1, 1),
					v2(x1, y0, color, u1, v0, 1), v2(x1, y1, color, u1, v1, 1), v2(x0, y1, color, u0, v1, 1) };
				s_batch.insert(s_batch.end(), q, q + 6);
			}
		}
		pen += kGlyphW * sx;
	}
}

// ---- the overlay on the watch's own framebuffer (android_input.cpp's controls): discs and 5 x 7 letters, in its
// pixels (no citro2d view)

static Vertex2D w2(float x, float y, u32 c)
{
	return { x, y, (c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f, (c >> 24) / 255.0f, 0, 0, 0 };
}

void glesDisc(float cx, float cy, float r, u32 color, float inner)
{
	batchFor(nullptr, nullptr, true);
	const int kSides = 32;
	for (int i = 0; i < kSides; i++)
	{
		float a0 = i * 6.2831853f / kSides, a1 = (i + 1) * 6.2831853f / kSides;
		float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
		Vertex2D o0 = w2(cx + c0 * r, cy + s0 * r, color), o1 = w2(cx + c1 * r, cy + s1 * r, color);
		if (inner <= 0.0f)
		{
			Vertex2D q[3] = { w2(cx, cy, color), o0, o1 };
			s_batch.insert(s_batch.end(), q, q + 3);
		}
		else
		{
			Vertex2D i0 = w2(cx + c0 * inner, cy + s0 * inner, color), i1 = w2(cx + c1 * inner, cy + s1 * inner, color);
			Vertex2D q[6] = { i0, o0, o1, i0, o1, i1 };
			s_batch.insert(s_batch.end(), q, q + 6);
		}
	}
}

void glesArc(float cx, float cy, float r, float inner, float a0, float a1, u32 color)
{
	batchFor(nullptr, nullptr, true);
	int sides = std::max(2, (int)(fabsf(a1 - a0) / 4.0f));
	for (int i = 0; i < sides; i++)
	{
		// (degrees anticlockwise from the right; the display's y runs down)
		float b0 = (a0 + (a1 - a0) * i / sides) * 0.01745329f, b1 = (a0 + (a1 - a0) * (i + 1) / sides) * 0.01745329f;
		Vertex2D o0 = w2(cx + cosf(b0) * r, cy - sinf(b0) * r, color), o1 = w2(cx + cosf(b1) * r, cy - sinf(b1) * r, color);
		Vertex2D i0 = w2(cx + cosf(b0) * inner, cy - sinf(b0) * inner, color);
		Vertex2D i1 = w2(cx + cosf(b1) * inner, cy - sinf(b1) * inner, color);
		Vertex2D q[6] = { i0, o0, o1, i0, o1, i1 };
		s_batch.insert(s_batch.end(), q, q + 6);
	}
}

// (each letter 5 columns of 7 rows, the top row in the lowest bit)
static const struct { char c; u8 cols[5]; } kGlyphs[] = {
	{ 'A', { 0x7E, 0x09, 0x09, 0x09, 0x7E } }, { 'B', { 0x7F, 0x49, 0x49, 0x49, 0x36 } },
	{ 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } }, { 'D', { 0x7F, 0x41, 0x41, 0x22, 0x1C } },
	{ 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } }, { 'G', { 0x3E, 0x41, 0x49, 0x49, 0x7A } },
	{ 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } }, { 'J', { 0x20, 0x40, 0x41, 0x3F, 0x01 } },
	{ 'K', { 0x7F, 0x08, 0x14, 0x22, 0x41 } }, { 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
	{ 'M', { 0x7F, 0x02, 0x0C, 0x02, 0x7F } }, { 'N', { 0x7F, 0x04, 0x08, 0x10, 0x7F } },
	{ 'O', { 0x3E, 0x41, 0x41, 0x41, 0x3E } }, { 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } },
	{ 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } }, { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
	{ 'T', { 0x01, 0x01, 0x7F, 0x01, 0x01 } }, { 'U', { 0x3F, 0x40, 0x40, 0x40, 0x3F } },
	{ 'V', { 0x1F, 0x20, 0x40, 0x20, 0x1F } }, { 'W', { 0x3F, 0x40, 0x38, 0x40, 0x3F } },
	{ 'X', { 0x63, 0x14, 0x08, 0x14, 0x63 } }, { 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } },
	{ '=', { 0x14, 0x14, 0x14, 0x14, 0x14 } }, { '^', { 0x04, 0x02, 0x01, 0x02, 0x04 } },
	{ 'v', { 0x10, 0x20, 0x40, 0x20, 0x10 } },
};

void glesText(const char* s, float cx, float cy, float px, u32 color)
{
	batchFor(nullptr, nullptr, true);
	int n = (int)strlen(s);
	float x = cx - (n * 6 - 1) * px / 2, y = cy - 7 * px / 2;
	for (const char* c = s; *c; c++, x += 6 * px)
		for (auto& g : kGlyphs)
			if (g.c == *c)
				for (int col = 0; col < 5; col++)
					for (int row = 0; row < 7; row++)
						if (g.cols[col] >> row & 1)
						{
							float x0 = x + col * px, y0 = y + row * px, x1 = x0 + px, y1 = y0 + px;
							Vertex2D q[6] = { w2(x0, y0, color), w2(x1, y0, color), w2(x0, y1, color), w2(x1, y0, color),
								w2(x1, y1, color), w2(x0, y1, color) };
							s_batch.insert(s_batch.end(), q, q + 6);
						}
}

void glesLabel(const char* s, float cx, float cy, float h, u32 color)
{
	C3D_Tex* font = systemFont();
	if (!font)
	{
		glesText(s, cx, cy, h / 7.0f, color);
		return;
	}
	batchFor(nullptr, font, true);
	int n = (int)strlen(s);
	// (each character in its cell of the system font's, kGlyphW x kGlyphH, the line's height h)
	float cw = h * kGlyphW / kGlyphH, x = cx - n * cw / 2.0f, y0 = cy - h / 2.0f, y1 = cy + h / 2.0f;
	for (const u8* p = (const u8*)s; *p; p++, x += cw)
	{
		int c = *p;
		if (c <= ' ' || c > 126)
			continue;
		int at = c - 32;
		float u0 = (float)(at % kGlyphColumns) / kGlyphColumns, u1 = u0 + 1.0f / kGlyphColumns;
		float v0 = 1.0f - (float)(at / kGlyphColumns) / kGlyphRows, v1 = v0 - 1.0f / kGlyphRows;
		Vertex2D q[6] = { { x, y0, 0, 0, 0, 0, u0, v0, 1 }, { x + cw, y0, 0, 0, 0, 0, u1, v0, 1 },
			{ x, y1, 0, 0, 0, 0, u0, v1, 1 }, { x + cw, y0, 0, 0, 0, 0, u1, v0, 1 }, { x + cw, y1, 0, 0, 0, 0, u1, v1, 1 },
			{ x, y1, 0, 0, 0, 0, u0, v1, 1 } };
		for (auto& v : q)
		{
			v.r = (color & 255) / 255.0f;
			v.g = ((color >> 8) & 255) / 255.0f;
			v.b = ((color >> 16) & 255) / 255.0f;
			v.a = (color >> 24) / 255.0f;
		}
		s_batch.insert(s_batch.end(), q, q + 6);
	}
}

// ---- screenshots: not on the watch (the console's SHOT; the PC build's world map bake)

bool screenshotSave(const char*) { return false; }
void screenshotDrawThisFrame() {}
bool screenshotSaveTexture(C3D_Tex*, const char*) { return false; }
