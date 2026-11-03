// The PICA's textures as the .t3x files hold them, decoded on the CPU: their formats' sizes, ETC1 / ETC1A4 blocks, the
// tiled formats into RGBA rows (0xAABBGGRR, the top row first) and the GBA-style compressions in front of the data. Shared
// by the native test build's software GPU (native_gpu.cpp) and the Android build's OpenGL ES one (android/src/gles_gpu.cpp)
#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include "components/debug/debuglog.hpp"

static int texBits(int fmt)
{
	switch (fmt)
	{
	case GPU_RGBA8: return 32;
	case GPU_RGB8: return 24;
	case GPU_RGBA5551: case GPU_RGB565: case GPU_RGBA4: case GPU_LA8: case GPU_HILO8: return 16;
	case GPU_L8: case GPU_A8: case GPU_LA4: case GPU_ETC1A4: return 8;
	default: return 4;      // L4, A4, ETC1
	}
}

static inline u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }

static void etc1Block(u64 v, u64 alpha, bool hasAlpha, u32 out[4][4])
{
	static const int kMod[8][4] = { { 2, 8, -2, -8 }, { 5, 17, -5, -17 }, { 9, 29, -9, -29 }, { 13, 42, -13, -42 },
		{ 18, 60, -18, -60 }, { 24, 80, -24, -80 }, { 33, 106, -33, -106 }, { 47, 183, -47, -183 } };
	bool diff = (v >> 33) & 1, flip = (v >> 32) & 1;
	int t1 = (v >> 37) & 7, t2 = (v >> 34) & 7;
	int c1[3], c2[3];
	for (int k = 0; k < 3; k++)
	{
		int shift = 59 - k * 8;
		if (diff)
		{
			int a = (v >> shift) & 31;
			int d = (v >> (shift - 3)) & 7;
			if (d >= 4)
				d -= 8;
			int b = std::clamp(a + d, 0, 31);
			c1[k] = (a << 3) | (a >> 2);
			c2[k] = (b << 3) | (b >> 2);
		}
		else
		{
			int a = (v >> (shift + 1)) & 15, b = (v >> (shift - 3)) & 15;
			c1[k] = a * 17;
			c2[k] = b * 17;
		}
	}
	for (int x = 0; x < 4; x++)
		for (int y = 0; y < 4; y++)
		{
			int i = x * 4 + y;
			bool second = flip ? y >= 2 : x >= 2;
			int idx = (((v >> (16 + i)) & 1) << 1) | ((v >> i) & 1);
			int m = kMod[second ? t2 : t1][idx];
			const int* c = second ? c2 : c1;
			u32 a = hasAlpha ? ((alpha >> (i * 4)) & 15) * 17 : 255;
			out[y][x] = rgba(std::clamp(c[0] + m, 0, 255), std::clamp(c[1] + m, 0, 255), std::clamp(c[2] + m, 0, 255), a);
		}
}

// One level of a PICA texture: 8 x 8 tiles in rows, texels in Morton order inside a tile (ETC: four 4 x 4 blocks).
// The first row in memory is the top of the picture (v = 1).
// px: w x h texels, already sized by the caller (magenta where the data is missing)
static void decodeLevelInto(const u8* d, int w, int h, int fmt, std::vector<u32>& px)
{
	if (!d || w < 8 || h < 8)
		return;
	auto put = [&](int x, int row, u32 c) { px[row * w + x] = c; };
	if (fmt == GPU_ETC1 || fmt == GPU_ETC1A4)
	{
		bool hasAlpha = fmt == GPU_ETC1A4;
		for (int ty = 0; ty < h / 8; ty++)
			for (int tx = 0; tx < w / 8; tx++)
				for (int b = 0; b < 4; b++)
				{
					u64 alpha = 0, v;
					if (hasAlpha)
					{
						memcpy(&alpha, d, 8);
						d += 8;
					}
					memcpy(&v, d, 8);
					d += 8;
					u32 block[4][4];
					etc1Block(v, alpha, hasAlpha, block);
					int bx = tx * 8 + (b & 1) * 4, by = ty * 8 + (b >> 1) * 4;
					for (int y = 0; y < 4; y++)
						for (int x = 0; x < 4; x++)
							put(bx + x, by + y, block[y][x]);
				}
		return;
	}
	int bits = texBits(fmt);
	for (int ty = 0; ty < h / 8; ty++)
		for (int tx = 0; tx < w / 8; tx++)
		{
			const u8* tile = d + (size_t)(ty * (w / 8) + tx) * 64 * bits / 8;
			for (int i = 0; i < 64; i++)
			{
				int x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
				int y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
				const u8* p = tile + i * bits / 8;
				u32 c = 0xFFFFFFFF;
				u16 s = bits == 16 ? (u16)(p[0] | (p[1] << 8)) : 0;
				switch (fmt)
				{
				case GPU_RGBA8: c = rgba(p[3], p[2], p[1], p[0]); break;
				case GPU_RGB8: c = rgba(p[2], p[1], p[0], 255); break;
				case GPU_RGBA5551: c = rgba(((s >> 11) & 31) * 255 / 31, ((s >> 6) & 31) * 255 / 31, ((s >> 1) & 31) * 255 / 31, (s & 1) * 255); break;
				case GPU_RGB565: c = rgba(((s >> 11) & 31) * 255 / 31, ((s >> 5) & 63) * 255 / 63, (s & 31) * 255 / 31, 255); break;
				case GPU_RGBA4: c = rgba(((s >> 12) & 15) * 17, ((s >> 8) & 15) * 17, ((s >> 4) & 15) * 17, (s & 15) * 17); break;
				case GPU_LA8: c = rgba(p[1], p[1], p[1], p[0]); break;
				case GPU_HILO8: c = rgba(p[1], p[0], 0, 255); break;
				case GPU_L8: c = rgba(p[0], p[0], p[0], 255); break;
				case GPU_A8: c = rgba(0, 0, 0, p[0]); break;
				case GPU_LA4: c = rgba((p[0] >> 4) * 17, (p[0] >> 4) * 17, (p[0] >> 4) * 17, (p[0] & 15) * 17); break;
				case GPU_L4: case GPU_A4:
				{
					u8 n = (tile[i / 2] >> ((i & 1) * 4)) & 15;
					c = fmt == GPU_L4 ? rgba(n * 17, n * 17, n * 17, 255) : rgba(0, 0, 0, n * 17);
					break;
				}
				}
				put(tx * 8 + x, ty * 8 + y, c);
			}
		}
}

// .t3x: the texture data behind a GBA-style compression header (none, LZ10, LZ11, Huffman or RLE)
static std::vector<u8> decompress(const u8* d, size_t n)
{
	std::vector<u8> out;
	if (n < 4)
		return out;
	u8 type = d[0];
	size_t size = d[1] | (d[2] << 8) | (d[3] << 16), p = 4;
	if (size == 0 && n >= 8)
	{
		size = d[4] | (d[5] << 8) | (d[6] << 16) | ((size_t)d[7] << 24);
		p = 8;
	}
	out.reserve(size);
	auto back = [&](size_t disp, size_t len) {
		for (size_t i = 0; i < len && out.size() < size; i++)
			out.push_back(disp <= out.size() ? out[out.size() - disp] : 0);
	};
	if (type == 0x00)
		out.assign(d + p, d + std::min(n, p + size));
	else if (type == 0x10 || type == 0x11)
	{
		while (out.size() < size && p < n)
		{
			u8 flags = d[p++];
			for (int bit = 7; bit >= 0 && out.size() < size && p < n; bit--)
			{
				if (!(flags & (1 << bit)))
				{
					out.push_back(d[p++]);
					continue;
				}
				size_t len, disp;
				u8 b0 = d[p];
				if (type == 0x10)
				{
					len = (b0 >> 4) + 3;
					disp = (((b0 & 15) << 8) | d[p + 1]) + 1;
					p += 2;
				}
				else if ((b0 >> 4) == 0)
				{
					len = (((b0 & 15) << 4) | (d[p + 1] >> 4)) + 0x11;
					disp = (((d[p + 1] & 15) << 8) | d[p + 2]) + 1;
					p += 3;
				}
				else if ((b0 >> 4) == 1)
				{
					len = (((b0 & 15) << 12) | (d[p + 1] << 4) | (d[p + 2] >> 4)) + 0x111;
					disp = (((d[p + 2] & 15) << 8) | d[p + 3]) + 1;
					p += 4;
				}
				else
				{
					len = (b0 >> 4) + 1;
					disp = (((b0 & 15) << 8) | d[p + 1]) + 1;
					p += 2;
				}
				back(disp, len);
			}
		}
	}
	else if ((type & 0xF0) == 0x20)
	{
		// Huffman: a tree of 8-bit nodes (offset to the children in bits 0-5, "child is data" in bits 7 / 6),
		// then 32-bit words read from the top bit
		int symBits = type & 15;
		size_t treeAt = p, root = p + 1, stream = p + (d[p] + 1) * 2;
		size_t node = root;
		bool low = true;
		u8 pending = 0;
		while (out.size() < size && stream + 4 <= n)
		{
			u32 word = d[stream] | (d[stream + 1] << 8) | (d[stream + 2] << 16) | ((u32)d[stream + 3] << 24);
			stream += 4;
			for (int bit = 31; bit >= 0 && out.size() < size; bit--)
			{
				int b = (word >> bit) & 1;
				u8 nd = d[node];
				size_t child = (node & ~(size_t)1) + (nd & 0x3F) * 2 + 2 + b;
				bool leaf = b ? (nd & 0x40) : (nd & 0x80);
				if (!leaf)
				{
					node = child;
					continue;
				}
				u8 sym = d[child];
				if (symBits == 8)
					out.push_back(sym);
				else if (low)
				{
					pending = sym & 15;
					low = false;
				}
				else
				{
					out.push_back(pending | ((sym & 15) << 4));
					low = true;
				}
				node = root;
			}
		}
		(void)treeAt;
	}
	else if (type == 0x30)
	{
		while (out.size() < size && p < n)
		{
			u8 f = d[p++];
			if (f & 0x80)
			{
				size_t len = (f & 0x7F) + 3;
				u8 v = d[p++];
				for (size_t i = 0; i < len && out.size() < size; i++)
					out.push_back(v);
			}
			else
				for (size_t i = 0, len = (f & 0x7F) + 1; i < len && out.size() < size && p < n; i++)
					out.push_back(d[p++]);
		}
	}
	else
		logf("native gpu: unknown texture compression 0x%02x", type);
	out.resize(size);
	return out;
}
