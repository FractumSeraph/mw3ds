#pragma once

// What the inventory and magic screens share
#include <cmath>
#include <cstdio>
#include <cstring>

extern const char* kQuickHint;
// The quick keys' line in Inventory and Magic at y: the hint (ZL + D-pad), or on Android, which has neither, four slot
// buttons that set the highlighted item or spell (quickkeysmenu.cpp)
void quickKeyLine(float y);

// The button that casts, and how a quick key is pressed, in the active layout
#ifdef MW3DS_ANDROID
static inline std::string castHint() { return "CAST, under MORE, casts it"; }   // (a watch's ring)
#else
static inline std::string castHint() { return std::string(g_controlLayout != CONTROLS_CLASSIC ? "L" : "Y") + " to cast"; }
#endif
