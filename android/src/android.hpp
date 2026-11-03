// The Android build's own parts (android/src), between the game's 3DS stand-ins (tools/test/native/stub) and SDL3
#pragma once
#include <3ds.h>
#include <SDL3/SDL.h>

// SDL, the display's window and its OpenGL ES 3.0 context, the sound, and the paths the game's sdmc:/ maps to (the
// app's own folder on the watch's storage): before appMain. False: no window (the app closes)
bool androidStart();
SDL_Window* androidWindow();

// Which of the console's screens the display shows, and where in it (pixels, from its top left): android_input.cpp's
// choice, put there by C3D_FrameEnd (gles_gpu.cpp)
struct ScreenView
{
	int screen;           // GFX_TOP or GFX_BOTTOM
	float x, y, w, h;
};
ScreenView androidScreenView(int windowW, int windowH);
// The watch's controls over the screen (android_input.cpp), drawn with glesDisc and glesText
void androidDrawOverlay(int windowW, int windowH);
// The view's turn since the last call, in radians (platform/platform.hpp platformLook)
void androidLookTake(float& yaw, float& pitch);

// The sound: SDL's output, mixing the ndsp stand-ins' channels as they play (android_audio.cpp)
void androidAudioStart();

// Drawing on the display itself, in its pixels (gles_gpu.cpp): a disc (with inner > 0, a ring from inner to r) and a
// line of 5 x 7 letters (capitals, '=', '^', 'v') px pixels to a dot, centred on (cx, cy). Colours 0xAABBGGRR
void glesDisc(float cx, float cy, float r, u32 color, float inner);
void glesText(const char* s, float cx, float cy, float px, u32 color);
// A line of words in the watch's own typeface (the system font's, MorrowindActivity.renderGlyphs), h pixels tall,
// centred on (cx, cy); glesText's letters when there is none
void glesLabel(const char* s, float cx, float cy, float h, u32 color);
