#pragma once

// The game is the same code in the console build, the PC test build (tools/test/native) and the Android build (android/):
// source/engine.cpp. Each build's entry file defines these few things and calls appMain, nothing else
// (tools/test/native/build.py refuses an entry file that grows), so what they run can't drift apart:
//   source/main.cpp                    the console
//   tools/test/native/main_native.cpp  the PC test build
//   android/src/main_android.cpp       the Android build (a Wear OS watch's)

// Start-up, the title screen and the frame loop until the game is left (engine.cpp)
int appMain();

// A scripted run is over: its last step taken and the game saved, or (platformFailFast) its first failure. The PC test
// build ends the process there; the console carries on
void platformRunOver();

// MW3DS_FAILFAST=1 on the PC test build: a run stops at its first failure. Never on the console
bool platformFailFast();

// The view turned this frame by what the console doesn't have, in radians (yaw: to the right; pitch: up): the Android
// build's slide on the watch's screen and its bezel. Nothing on the console and the PC test build
void platformLook(float& yaw, float& pitch);

// Before gfxInit: the console's VRAM cleared to black (gfxInit puts the screens' buffers there and lights the screens on
// them at once, before the game's first frame). Nothing on the PC test build
void platformClearVram();
