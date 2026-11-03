// The Android build's sound: the ndsp stand-ins (tools/test/native/stub/3ds.h nativeDsp*) played through SDL. The
// channels are the native test build's mixer's (tools/test/native/native_stubs.cpp, NATIVE_AUDIO), mixed here as
// SDL asks for more instead of a frame at a time: PCM16 and DSP-ADPCM, resampled linearly, at the DSP's 32728 Hz
#include <3ds.h>
#include <SDL3/SDL.h>
#include <cstring>

#include "android.hpp"
#include "components/debug/debuglog.hpp"

namespace
{
const int kChannels = 24, kQueue = 16, kOutRate = 32728;
struct Channel
{
	float rate = 32728.0f, mixL = 1.0f, mixR = 1.0f;
	ndspWaveBuf* queue[kQueue];
	int count = 0;
	double pos = 0;
	// DSP-ADPCM: the frames are decoded as play reaches them, the last few kept
	int format = NDSP_FORMAT_MONO_PCM16;
	s16 coefs[16] = {};
	int h1 = 0, h2 = 0;
	u32 decoded = 0;               // samples of the playing buffer decoded so far
	s16 recent[4] = {};
};
Channel s_ch[kChannels];
SDL_Mutex* s_lock = nullptr;
SDL_AudioStream* s_stream = nullptr;

struct Locked
{
	Locked() { if (s_lock) SDL_LockMutex(s_lock); }
	~Locked() { if (s_lock) SDL_UnlockMutex(s_lock); }
};

// A buffer starting (or looping): the DSP-ADPCM decoder takes the buffer's start state, or carries on without one
void adpcmStart(Channel& c, const ndspWaveBuf* b)
{
	c.decoded = 0;
	if (b->adpcm_data)
	{
		c.h1 = b->adpcm_data->history0;
		c.h2 = b->adpcm_data->history1;
	}
}

// Sample p of the playing buffer (decoded up to p: play only moves forward)
s16 adpcmAt(Channel& c, const ndspWaveBuf* b, u32 p)
{
	const u8* d = (const u8*)b->data_vaddr;
	while (c.decoded <= p && c.decoded < b->nsamples)
	{
		u32 k = c.decoded, frame = k / 14, at = k % 14;
		u8 head = d[frame * 8], byte = d[frame * 8 + 1 + at / 2];
		int r = at % 2 ? byte & 15 : byte >> 4;
		r = r >= 8 ? r - 16 : r;
		int pi = (head >> 4) & 7;
		int v = (((r * (1 << (head & 15))) * 2048) + 1024 + c.coefs[pi * 2] * c.h1 + c.coefs[pi * 2 + 1] * c.h2) >> 11;
		v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
		c.h2 = c.h1;
		c.h1 = v;
		c.recent[k & 3] = (s16)v;
		c.decoded++;
	}
	return c.recent[(p < c.decoded ? p : c.decoded - 1) & 3];
}

// n stereo samples of every playing channel, mixed
void mix(s16* out, int n)
{
	static float acc[2048 * 2];
	memset(acc, 0, sizeof(float) * n * 2);
	for (auto& c : s_ch)
	{
		double step = c.rate / kOutRate;
		for (int i = 0; i < n && c.count; i++)
		{
			ndspWaveBuf* b = c.queue[0];
			u32 len = b->nsamples, p = (u32)c.pos;
			if (len == 0)
			{
				c.pos = len;
				p = 0;
			}
			float f = (float)(c.pos - p);
			float s0 = 0.0f, s1 = 0.0f;
			if (len)
			{
				if (c.format == NDSP_FORMAT_ADPCM)
				{
					if (c.decoded == 0)
						adpcmStart(c, b);
					s1 = adpcmAt(c, b, p + 1 < len ? p + 1 : p);
					s0 = adpcmAt(c, b, p);
					if (p + 1 >= len)
						s1 = s0;
				}
				else
				{
					const s16* d = (const s16*)b->data_vaddr;
					s0 = d[p], s1 = p + 1 < len ? d[p + 1] : s0;
				}
			}
			float v = s0 + (s1 - s0) * f;
			acc[i * 2] += v * c.mixL;
			acc[i * 2 + 1] += v * c.mixR;
			c.pos += step;
			if (c.pos >= len)
			{
				c.decoded = 0;
				if (b->looping && len)
					c.pos -= len;
				else
				{
					b->status = NDSP_WBUF_DONE;
					memmove(c.queue, c.queue + 1, sizeof(c.queue[0]) * (--c.count));
					c.pos = 0;
					if (c.count)
						c.queue[0]->status = NDSP_WBUF_PLAYING;
				}
			}
		}
	}
	for (int i = 0; i < n * 2; i++)
		out[i] = (s16)(acc[i] > 32767.0f ? 32767 : acc[i] < -32768.0f ? -32768 : acc[i]);
}

void SDLCALL feed(void*, SDL_AudioStream* stream, int additional, int)
{
	static s16 out[2048 * 2];
	while (additional > 0)
	{
		int n = additional / 4;
		n = n < 1 ? 1 : n > 2048 ? 2048 : n;
		{
			Locked locked;
			mix(out, n);
		}
		SDL_PutAudioStreamData(stream, out, n * 4);
		additional -= n * 4;
	}
}
}

void androidAudioStart()
{
	s_lock = SDL_CreateMutex();
	SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, kOutRate };
	s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, nullptr);
	if (!s_stream)
	{
		logf("audio: no output (%s)", SDL_GetError());
		return;
	}
	SDL_ResumeAudioStreamDevice(s_stream);
	logf("audio: playing at %d Hz", kOutRate);
}

void nativeDspReset(int ch)
{
	if (ch < 0 || ch >= kChannels)
		return;
	Locked locked;
	for (int i = 0; i < s_ch[ch].count; i++)
		s_ch[ch].queue[i]->status = NDSP_WBUF_DONE;
	s_ch[ch] = Channel();
}
void nativeDspFormat(int ch, int format)
{
	Locked locked;
	if (ch >= 0 && ch < kChannels)
		s_ch[ch].format = format;
}
void nativeDspCoefs(int ch, const u16* coefs)
{
	Locked locked;
	if (ch >= 0 && ch < kChannels)
		memcpy(s_ch[ch].coefs, coefs, sizeof(s_ch[ch].coefs));
}
void nativeDspRate(int ch, float rate)
{
	Locked locked;
	if (ch >= 0 && ch < kChannels)
		s_ch[ch].rate = rate;
}
void nativeDspMix(int ch, const float* m)
{
	Locked locked;
	if (ch >= 0 && ch < kChannels)
	{
		s_ch[ch].mixL = m[0];
		s_ch[ch].mixR = m[1];
	}
}
// False: no room (the stand-in then marks the buffer done, as if it had played)
bool nativeDspAdd(int ch, ndspWaveBuf* b)
{
	Locked locked;
	if (!s_stream || ch < 0 || ch >= kChannels || s_ch[ch].count >= kQueue)
		return false;
	b->status = s_ch[ch].count == 0 ? NDSP_WBUF_PLAYING : NDSP_WBUF_QUEUED;
	s_ch[ch].queue[s_ch[ch].count++] = b;
	return true;
}
u32 nativeDspPos(int ch)
{
	Locked locked;
	return ch >= 0 && ch < kChannels && s_ch[ch].count ? (u32)s_ch[ch].pos : 0;
}
