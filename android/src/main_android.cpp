// The Android build's entry (a Wear OS watch's): the game is source/engine.cpp, the same code as on the console
// (platform/platform.hpp). SDL3 calls this main from its activity (SDL_main.h)
#include <SDL3/SDL_main.h>

#include "android.hpp"
#include "platform/platform.hpp"

// A scripted run's end changes nothing here: the game goes on, as on the console
void platformRunOver()
{
}

bool platformFailFast()
{
	return false;
}

void platformClearVram()
{
}

// The watch's slide and bezel (android_input.cpp)
void platformLook(float& yaw, float& pitch)
{
	androidLookTake(yaw, pitch);
}

// A buzz (android_input.cpp)
void platformFeedback(int what)
{
	androidFeedback(what);
}

int main(int, char**)
{
	if (!androidStart())
		return 1;
	return appMain();
}
