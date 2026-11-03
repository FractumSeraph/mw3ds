// The Android build's window, paths and controls: a Wear OS watch (Samsung's Galaxy Watch 8 Classic: a round 438 x 438
// display, a turning bezel, a back button) or a phone, with a gamepad if one is paired. The game reads the console's
// buttons, pads and touch screen (tools/test/native/stub/3ds.h's hid stand-ins, defined here), so the watch is made to
// look like a 3DS to it:
//
// - The display shows one of the console's screens at a time, as large as fits in its circle: the top screen (the
//   world) while the game plays, the bottom one (the touch screen: the title, the HUD's buttons, the menus) otherwise.
//   The ring's SCREEN and the bottom screen's WORLD swap them; a menu opening or closing picks again.
// - On the top screen one finger plays (the watch is on the other hand): a slide walks, a tap is A (talk, open,
//   take), held still it attacks; the bezel turns. WALK walks on by itself (the bezel steers). MORE, or the back
//   button, opens a ring of actions in words: MENU, JUMP, READY, CAST, SCREEN, SNEAK, VIEW (first / third person) and
//   LOOK (a slide looks instead of walking). They press what the button layout chosen in Options has them on
//   (layoutKeys). A press buzzes.
// - On the bottom screen a finger is the stylus; BACK (B), MENU (START) and OK (A) are above it. The bezel moves the
//   menus' highlight and scrolls their text; the back button is B.
#include <3ds.h>
#include <SDL3/SDL.h>
#include <jni.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "android.hpp"
#include "components/debug/debuglog.hpp"
#include "mwbase/sessiontypes.hpp"
#include "mwinput/inputmanagerimp.hpp"
#include "mwmechanics/mechanicsstate.hpp"
#include "platform/platform.hpp"

static SDL_Window* s_window = nullptr;
static SDL_GLContext s_gl = nullptr;
static bool s_round = false;
static bool s_quit = false;

SDL_Window* androidWindow()
{
	return s_window;
}

bool aptMainLoop()
{
	return !s_quit;
}

bool androidStart()
{
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
	{
		SDL_Log("mw3ds: SDL_Init: %s", SDL_GetError());
		return false;
	}
	// sdmc:/3ds/mw3ds is the app's own folder (Android/data/net.fractumseraph.morrowind/files/3ds/mw3ds), the converted
	// game data its data folder, as on the console's SD card (tools/test/native/native_stubs.cpp nativePath)
	const char* ext = SDL_GetAndroidExternalStoragePath();
	std::string root = ext ? ext : SDL_GetAndroidInternalStoragePath();
	setenv("NATIVE_SD", root.c_str(), 1);
	setenv("MW3DS_DATA", (root + "/3ds/mw3ds/data").c_str(), 1);
	mkdir((root + "/3ds").c_str(), 0777);
	mkdir((root + "/3ds/mw3ds").c_str(), 0777);
	const char* round = getenv("MW3DS_ROUND");          // (MorrowindActivity: Configuration.isScreenRound)
	s_round = round && round[0] == '1';

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	s_window = SDL_CreateWindow("FractumMorrowind", 0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
	if (!s_window)
	{
		SDL_Log("mw3ds: SDL_CreateWindow: %s", SDL_GetError());
		return false;
	}
	s_gl = SDL_GL_CreateContext(s_window);
	if (!s_gl)
	{
		SDL_Log("mw3ds: SDL_GL_CreateContext: %s", SDL_GetError());
		return false;
	}
	SDL_GL_MakeCurrent(s_window, s_gl);
	SDL_GL_SetSwapInterval(1);
	androidAudioStart();
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(s_window, &w, &h);
	SDL_Log("mw3ds: %d x %d display%s, data in %s", w, h, s_round ? " (round)" : "", root.c_str());
	return true;
}

// ---- what the console's buttons and pads read

enum { VIEW_TOP, VIEW_BOTTOM };
static int s_view = VIEW_BOTTOM;
static int s_autoView = -1;
static unsigned s_lastFrames = 0;

static u32 s_held = 0, s_down = 0, s_up = 0;
static u32 s_pulse = 0;                 // pressed for one scan, let go the next (a tap's A, a ring item)
static bool s_sneak = false;            // sneaking (the ring's SNEAK), held as the layout's sneak button
static bool s_walk = false;             // walking on by itself (the WALK button): the bezel steers
static bool s_lookMode = false;         // a slide looks instead of walking (the ring's LOOK)
static bool s_ring = false;             // the ring of actions is open
static touchPosition s_touch = {};
static float s_padX = 0, s_padY = 0;    // the Circle Pad, -1 to 1 (a finger, or a gamepad's left stick)
static float s_stickX = 0, s_stickY = 0, s_stick2X = 0, s_stick2Y = 0;   // a gamepad's sticks
static int s_scrollFrames = 0;          // the bezel scrolling a text pane: frames of the C-stick held, and which way
static float s_scrollY = 0;
static u32 s_padKeys = 0;               // a gamepad's buttons, as the console's
static float s_lookYaw = 0, s_lookPitch = 0;
static std::atomic<int> s_rotary{ 0 };  // bezel clicks, hundredths (MorrowindActivity, on the UI thread)
static SDL_Gamepad* s_gamepad = nullptr;

extern "C" JNIEXPORT void JNICALL Java_net_fractumseraph_morrowind_MorrowindActivity_nativeRotary(JNIEnv*, jclass, jfloat clicks)
{
	s_rotary += (int)lroundf(clicks * 100.0f);
}

// A short buzz under the finger, so a press is felt without looking (MorrowindActivity.buzz): times of them, a little
// apart
static void buzz(int ms, int times = 1)
{
	static jclass cls = nullptr;
	static jmethodID method = nullptr;
	JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
	if (!env)
		return;
	if (!cls)
	{
		jobject activity = (jobject)SDL_GetAndroidActivity();
		if (!activity)
			return;
		jclass local = env->GetObjectClass(activity);
		cls = (jclass)env->NewGlobalRef(local);
		method = env->GetStaticMethodID(cls, "buzz", "(II)V");
		env->DeleteLocalRef(local);
		env->DeleteLocalRef(activity);
	}
	if (method)
		env->CallStaticVoidMethod(cls, method, (jint)ms, (jint)times);
	if (env->ExceptionCheck())
		env->ExceptionClear();
}

// The display's circle (or rectangle, on a phone), and the screens and buttons in it
struct Geometry
{
	float cx, cy, r;
	float w, h;
};
static Geometry geometry(int ww, int wh)
{
	return { ww * 0.5f, wh * 0.5f, std::min(ww, wh) * 0.5f, (float)ww, (float)wh };
}

ScreenView androidScreenView(int ww, int wh)
{
	Geometry g = geometry(ww, wh);
	ScreenView v;
	v.screen = s_view == VIEW_TOP ? GFX_TOP : GFX_BOTTOM;
	float sw = s_view == VIEW_TOP ? 400.0f : 320.0f, sh = 240.0f;
	if (s_round)
	{
		// the largest rectangle of the screen's shape inside the circle: its diagonal the diameter
		float diag = sqrtf(sw * sw + sh * sh);
		v.w = 2.0f * g.r * sw / diag;
		v.h = 2.0f * g.r * sh / diag;
	}
	else
	{
		float k = std::min(ww / sw, wh / sh);
		v.w = sw * k;
		v.h = sh * k;
	}
	v.x = g.cx - v.w * 0.5f;
	v.y = g.cy - v.h * 0.5f;
	return v;
}

// What a button or a ring item does: a console button (as the layout Options chose has it: layoutKeys), or one of
// the watch's own (the screens' swap, walking on, the look mode, the ring)
enum Action { ACT_START, ACT_READY, ACT_JUMP, ACT_CAST, ACT_ATTACK, ACT_SNEAK, ACT_POV, ACT_SCREEN, ACT_LOOK, ACT_WALK,
	ACT_RING, ACT_B, ACT_A };
// Where (degrees round the centre from the right, anticlockwise, and how far out as a share of the radius) and its word
struct Button { Action act; float deg, out; const char* label; };
// On the top screen only two, in the bands above and below it, clear of the picture: the ring and walking on
static const Button kTopButtons[] = { { ACT_RING, 90, 0.76f, "MORE" }, { ACT_WALK, 270, 0.76f, "WALK" } };
// Above the bottom screen (high: lower, they would cover its top row) and below it
static const Button kBottomButtons[] = {
	{ ACT_B, 112, 0.88f, "BACK" }, { ACT_START, 90, 0.80f, "MENU" }, { ACT_A, 68, 0.88f, "OK" },
	{ ACT_SCREEN, 270, 0.80f, "WORLD" },
};
// The ring: eight actions round the display, the world dimmed behind them
static const Button kRing[] = {
	{ ACT_START, 90, 0.60f, "MENU" }, { ACT_JUMP, 45, 0.60f, "JUMP" }, { ACT_READY, 0, 0.60f, "READY" },
	{ ACT_CAST, 315, 0.60f, "CAST" }, { ACT_SCREEN, 270, 0.60f, "SCREEN" }, { ACT_SNEAK, 225, 0.60f, "SNEAK" },
	{ ACT_POV, 180, 0.60f, "VIEW" }, { ACT_LOOK, 135, 0.60f, "LOOK" },
};
static const float kButtonSize = 0.13f;     // an edge button's radius, as a share of the display's
static const float kRingSize = 0.19f;       // a ring item's

static void buttonsFor(const Button*& list, int& n, float& size)
{
	if (s_ring && s_view == VIEW_TOP)
	{
		list = kRing;
		n = (int)(sizeof(kRing) / sizeof(kRing[0]));
		size = kRingSize;
		return;
	}
	list = s_view == VIEW_TOP ? kTopButtons : kBottomButtons;
	n = s_view == VIEW_TOP ? (int)(sizeof(kTopButtons) / sizeof(kTopButtons[0])) :
		(int)(sizeof(kBottomButtons) / sizeof(kBottomButtons[0]));
	size = kButtonSize;
}

static void buttonAt(const Geometry& g, const Button& b, float& x, float& y)
{
	float a = b.deg * 3.14159265f / 180.0f;
	x = g.cx + cosf(a) * g.r * b.out;
	y = g.cy - sinf(a) * g.r * b.out;
}

// The console's buttons an action presses in the button layout Options chose (mwinput/inputmanagerimp.cpp readInput)
static u32 layoutKeys(Action a)
{
	bool classic = g_controlLayout == CONTROLS_CLASSIC, goldenEye = g_controlLayout == CONTROLS_GOLDENEYE;
	switch (a)
	{
	case ACT_START: return KEY_START;
	case ACT_POV: return KEY_SELECT;
	case ACT_A: return KEY_A;
	case ACT_B: return KEY_B;
	case ACT_READY: return KEY_X;
	case ACT_JUMP: return classic ? KEY_B : KEY_Y;
	case ACT_CAST: return classic ? KEY_Y : KEY_L;
	case ACT_ATTACK: return classic ? KEY_X : KEY_R;
	case ACT_SNEAK: return classic ? KEY_L : goldenEye ? KEY_B : stylusLayout() ? KEY_B : KEY_ZL;
	default: return 0;
	}
}

// A button or ring item let go over itself: what it does. The console's buttons are pressed for one scan; the rest
// change the watch's own state
static void act(Action a)
{
	logf("watch: action %d", (int)a);
	switch (a)
	{
	case ACT_SCREEN:
		s_view = s_view == VIEW_TOP ? VIEW_BOTTOM : VIEW_TOP;
		s_walk = false;
		break;
	case ACT_LOOK:
		s_lookMode = !s_lookMode;
		break;
	case ACT_WALK:
		s_walk = !s_walk;
		break;
	case ACT_RING:
		s_ring = true;
		break;
	case ACT_SNEAK:
		if (g_controlLayout == CONTROLS_GOLDENEYE)
			s_pulse |= KEY_B;           // (GoldenEye's B is a toggle of its own: goldenEyeInput)
		else
			s_sneak = !s_sneak;
		break;
	default:
		s_pulse |= layoutKeys(a);
		break;
	}
	if (a != ACT_RING)
		s_ring = false;
}

// ---- fingers
//
// On the top screen one finger does it all (a watch is worn on the other hand): a slide walks (a stick where it
// landed: pushed far, it runs). The weapon (or the fists) decides the rest, so that no blow is struck by accident:
// put away, a tap is A (talk, open, take) and holding still does nothing; out, a tap is a quick blow and holding
// still winds one up (struck when let go, a buzz once it's wound up fully). The bezel turns. With the ring's LOOK
// on, a slide looks instead; a second finger's slide looks while the first walks (two thumbs, on a phone). The
// game's Combat aids bring the weapon out when someone fights (a buzz) and hold back a first blow at someone
// peaceful (two buzzes)

enum Role { ROLE_NONE, ROLE_GESTURE, ROLE_BUTTON, ROLE_SCREEN, ROLE_OUTSIDE };
struct Finger
{
	SDL_FingerID id = 0;
	Role role = ROLE_NONE;
	int button = -1;
	float x0 = 0, y0 = 0, x = 0, y = 0, lastX = 0, lastY = 0;
	Uint64 t0 = 0;
	bool slid = false;          // gone farther than a tap may
	bool attack = false;        // held still long enough: the attack
	bool looks = false;         // its slide looks (the look mode, or a second finger)
	bool unseen = true;         // down since the last scan: a press that ends before one still counts (fingerUp)
};
static Finger s_fingers[10];
static const float kFingerSlop = 14.0f;     // display pixels a tap may wander
static const Uint64 kTapMs = 300;           // a tap is quicker
static const Uint64 kHoldMs = 350;          // held still this long: the attack

static Finger* finger(SDL_FingerID id, bool make)
{
	for (auto& f : s_fingers)
		if (f.role != ROLE_NONE && f.id == id)
			return &f;
	if (!make)
		return nullptr;
	for (auto& f : s_fingers)
		if (f.role == ROLE_NONE)
		{
			f = Finger();
			f.id = id;
			return &f;
		}
	return nullptr;
}

static int buttonUnder(const Geometry& g, float x, float y)
{
	const Button* list;
	int n;
	float size;
	buttonsFor(list, n, size);
	for (int i = 0; i < n; i++)
	{
		float bx, by;
		buttonAt(g, list[i], bx, by);
		float d = size * g.r * 1.15f;
		if ((x - bx) * (x - bx) + (y - by) * (y - by) <= d * d)
			return i;
	}
	return -1;
}

static void fingerDown(const Geometry& g, Finger& f, float x, float y, int ww, int wh)
{
	f.x0 = f.x = f.lastX = x;
	f.y0 = f.y = f.lastY = y;
	f.t0 = SDL_GetTicks();
	int b = buttonUnder(g, x, y);
	if (b >= 0)
	{
		f.role = ROLE_BUTTON;
		f.button = b;
		buzz(12);
		return;
	}
	if (s_ring && s_view == VIEW_TOP)
	{
		f.role = ROLE_OUTSIDE;          // (let go there, it closes the ring)
		return;
	}
	if (s_view == VIEW_BOTTOM)
	{
		ScreenView v = androidScreenView(ww, wh);
		f.role = x >= v.x && x < v.x + v.w && y >= v.y && y < v.y + v.h ? ROLE_SCREEN : ROLE_NONE;
		return;
	}
	f.role = ROLE_GESTURE;
	bool walking = false;
	for (auto& o : s_fingers)
		walking |= &o != &f && o.role == ROLE_GESTURE && o.slid && !o.looks;
	f.looks = s_lookMode || walking;
}

static bool s_touchPulse = false;       // a touch that came and went between two scans: held for one

static void fingerUp(const Geometry& g, Finger& f, int ww, int wh)
{
	if (f.role == ROLE_GESTURE && !f.slid && !f.attack && SDL_GetTicks() - f.t0 < kTapMs)
	{
		// (the weapon out: a quick blow, let go at once; away: A)
		s_pulse |= g_weaponOut ? layoutKeys(ACT_ATTACK) : KEY_A;
		buzz(10);
	}
	if (f.role == ROLE_BUTTON)
	{
		// (let go over its button: it acts; slid off it first, nothing)
		const Button* list;
		int n;
		float size;
		buttonsFor(list, n, size);
		if (f.button < n && buttonUnder(g, f.x, f.y) == f.button)
			act(list[f.button].act);
	}
	if (f.role == ROLE_OUTSIDE && !f.slid)
		s_ring = false;
	// (a touch quicker than a frame, as adb's input tap is: the game sees it down for one scan)
	if (f.unseen && f.role == ROLE_SCREEN && !s_touchPulse)
	{
		ScreenView v = androidScreenView(ww, wh);
		s_touch.px = (u16)std::clamp((f.x - v.x) / v.w * 320.0f, 0.0f, 319.0f);
		s_touch.py = (u16)std::clamp((f.y - v.y) / v.h * 240.0f, 0.0f, 239.0f);
		s_touchPulse = true;
	}
	f.role = ROLE_NONE;
}

// ---- each frame

void hidScanInput()
{
	int ww = 0, wh = 0;
	SDL_GetWindowSizeInPixels(s_window, &ww, &wh);
	Geometry g = geometry(ww, wh);
	// The view the game's state wants: the top screen while it plays with no menu up (its frame loop counts frames),
	// the bottom one otherwise; a change of state picks it, the swap overrides it until the next. A menu opening stops
	// walking on and closes the ring
	int want = g_mainFrames != s_lastFrames && !g_menuForInput ? VIEW_TOP : VIEW_BOTTOM;
	s_lastFrames = g_mainFrames;
	bool anyFinger = false;
	for (auto& f : s_fingers)
		anyFinger |= f.role != ROLE_NONE;
	if (want != s_autoView && !anyFinger)
	{
		logf("watch: the %s screen (menu %d)", want == VIEW_TOP ? "top" : "bottom", (int)g_menuForInput);
		s_autoView = want;
		s_view = want;
		if (want == VIEW_BOTTOM)
			s_walk = s_ring = false;
	}

	u32 pulse = 0;
	SDL_Event e;
	while (SDL_PollEvent(&e))
	{
		switch (e.type)
		{
		case SDL_EVENT_QUIT:
		case SDL_EVENT_TERMINATING:
			s_quit = true;
			break;
		case SDL_EVENT_FINGER_DOWN:
			if (Finger* f = finger(e.tfinger.fingerID, true))
				fingerDown(g, *f, e.tfinger.x * ww, e.tfinger.y * wh, ww, wh);
			break;
		case SDL_EVENT_FINGER_MOTION:
			if (Finger* f = finger(e.tfinger.fingerID, false))
			{
				f->x = e.tfinger.x * ww;
				f->y = e.tfinger.y * wh;
				if (fabsf(f->x - f->x0) + fabsf(f->y - f->y0) > kFingerSlop)
					f->slid = true;
			}
			break;
		case SDL_EVENT_FINGER_UP:
		case SDL_EVENT_FINGER_CANCELED:
			if (Finger* f = finger(e.tfinger.fingerID, false))
				fingerUp(g, *f, ww, wh);
			break;
		case SDL_EVENT_KEY_DOWN:
			// The back button: the ring on the top screen (shut again by another press), B on the bottom one
			if (e.key.scancode == SDL_SCANCODE_AC_BACK && !e.key.repeat)
			{
				if (s_view == VIEW_TOP)
					s_ring = !s_ring;
				else
					pulse |= KEY_B;
				buzz(12);
			}
			break;
		case SDL_EVENT_GAMEPAD_ADDED:
			if (!s_gamepad)
				s_gamepad = SDL_OpenGamepad(e.gdevice.which);
			break;
		case SDL_EVENT_GAMEPAD_REMOVED:
			if (s_gamepad && SDL_GetGamepadID(s_gamepad) == e.gdevice.which)
			{
				SDL_CloseGamepad(s_gamepad);
				s_gamepad = nullptr;
			}
			break;
		default:
			break;
		}
	}

	// A gamepad, as the console: its buttons where Nintendo's are (A on the right), the sticks the Circle Pad and the
	// C-stick, the triggers ZL / ZR
	s_padKeys = 0;
	s_stickX = s_stickY = s_stick2X = s_stick2Y = 0;
	if (s_gamepad)
	{
		static const struct { SDL_GamepadButton b; u32 key; } kMap[] = {
			{ SDL_GAMEPAD_BUTTON_EAST, KEY_A }, { SDL_GAMEPAD_BUTTON_SOUTH, KEY_B }, { SDL_GAMEPAD_BUTTON_NORTH, KEY_X },
			{ SDL_GAMEPAD_BUTTON_WEST, KEY_Y }, { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, KEY_L },
			{ SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, KEY_R }, { SDL_GAMEPAD_BUTTON_START, KEY_START },
			{ SDL_GAMEPAD_BUTTON_BACK, KEY_SELECT }, { SDL_GAMEPAD_BUTTON_DPAD_UP, KEY_DUP },
			{ SDL_GAMEPAD_BUTTON_DPAD_DOWN, KEY_DDOWN }, { SDL_GAMEPAD_BUTTON_DPAD_LEFT, KEY_DLEFT },
			{ SDL_GAMEPAD_BUTTON_DPAD_RIGHT, KEY_DRIGHT } };
		for (auto& m : kMap)
			if (SDL_GetGamepadButton(s_gamepad, m.b))
				s_padKeys |= m.key;
		if (SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000)
			s_padKeys |= KEY_ZL;
		if (SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000)
			s_padKeys |= KEY_ZR;
		s_stickX = SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
		s_stickY = -SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
		s_stick2X = SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
		s_stick2Y = -SDL_GetGamepadAxis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
	}

	// The bezel: on the top screen the view's turn (15 degrees a click, clockwise to the right); on the bottom one the
	// menus' highlight (the D-pad's up / down: a list's row, an item grid's row) and a text pane's scroll (the C-stick,
	// a few frames a click)
	int rot = s_rotary.exchange(0);
	if (rot)
	{
		if (s_view == VIEW_TOP)
			s_lookYaw += rot / 100.0f * 15.0f * 3.14159265f / 180.0f;
		else
		{
			pulse |= rot > 0 ? KEY_DDOWN : KEY_DUP;
			s_scrollY = rot > 0 ? -1.0f : 1.0f;
			s_scrollFrames = 4;
		}
	}

	// The fingers: walking, looking, the attack, the buttons, the stylus
	u32 held = s_padKeys;
	s_padX = s_stickX;
	s_padY = s_stickY;
	bool touching = false, fingerWalks = false;
	// (a slide across the display: the stylus layout's 0.01 radians a bottom-screen pixel, at Options' stylus speed)
	float perPixel = 0.01f * 320.0f / (1.6f * g.r) * g_stylusSpeed / 100.0f;
	Uint64 now = SDL_GetTicks();
	for (auto& f : s_fingers)
	{
		switch (f.role)
		{
		case ROLE_GESTURE:
			// (held still with the weapon out: the wind-up; with it away, nothing)
			if (!f.slid && !f.attack && now - f.t0 >= kHoldMs && g_weaponOut)
			{
				f.attack = true;
				buzz(15);
			}
			if (f.attack)
				held |= layoutKeys(ACT_ATTACK);
			if (f.slid && f.looks)
			{
				s_lookYaw += (f.x - f.lastX) * perPixel;
				s_lookPitch += (f.lastY - f.y) * perPixel;
			}
			else if (f.slid)
			{
				float reach = 0.22f * g.r;
				float dx = (f.x - f.x0) / reach, dy = (f.y0 - f.y) / reach;
				float len = sqrtf(dx * dx + dy * dy);
				if (len > 1.0f)
					dx /= len, dy /= len;
				s_padX = dx;
				s_padY = dy;
				fingerWalks = true;
			}
			break;
		case ROLE_SCREEN:
			if (!touching)
			{
				ScreenView v = androidScreenView(ww, wh);
				float tx = (f.x - v.x) / v.w * 320.0f, ty = (f.y - v.y) / v.h * 240.0f;
				s_touch.px = (u16)std::clamp(tx, 0.0f, 319.0f);
				s_touch.py = (u16)std::clamp(ty, 0.0f, 239.0f);
				touching = true;
			}
			break;
		default:
			break;
		}
		f.lastX = f.x;
		f.lastY = f.y;
		f.unseen = false;
	}
	// walking on: straight ahead at a run until a finger walks itself, or WALK is pressed again
	if (s_walk && s_view == VIEW_TOP && !fingerWalks)
		s_padY = 1.0f;
	if (s_scrollFrames > 0)
	{
		s_stick2Y = s_scrollY;
		s_scrollFrames--;
	}
	if (!touching && s_touchPulse)
		touching = true;            // (s_touch holds where it was)
	s_touchPulse = false;
	if (touching)
		held |= KEY_TOUCH;
	if (s_sneak && s_view == VIEW_TOP && g_controlLayout != CONTROLS_GOLDENEYE)
		held |= layoutKeys(ACT_SNEAK);
	// (a pulse is held for one scan and let go the next, so the game sees it go down and come up)
	held |= pulse | s_pulse;
	s_pulse = 0;
	s_down = held & ~s_held;
	s_up = s_held & ~held;
	s_held = held;
}

u32 hidKeysDown() { return s_down; }
u32 hidKeysHeld() { return s_held; }
u32 hidKeysUp() { return s_up; }

void hidTouchRead(touchPosition* t)
{
	*t = s_touch;
}

// The 3DS's pads count about 156 at their edge (stickAxes' scale)
void hidCircleRead(circlePosition* c)
{
	c->dx = (s16)lroundf(std::clamp(s_padX, -1.0f, 1.0f) * 156.0f);
	c->dy = (s16)lroundf(std::clamp(s_padY, -1.0f, 1.0f) * 156.0f);
}

void hidCstickRead(circlePosition* c)
{
	c->dx = (s16)lroundf(std::clamp(s_stick2X, -1.0f, 1.0f) * 156.0f);
	c->dy = (s16)lroundf(std::clamp(s_stick2Y, -1.0f, 1.0f) * 156.0f);
}

void androidFeedback(int what)
{
	switch (what)
	{
	case FEEDBACK_READIED: buzz(30); break;
	case FEEDBACK_CHARGED: buzz(20); break;
	case FEEDBACK_HELD_BACK: buzz(25, 2); break;
	case FEEDBACK_HIT: buzz(70); break;
	default: break;
	}
}

void androidLookTake(float& yaw, float& pitch)
{
	yaw = s_lookYaw;
	pitch = s_lookPitch;
	s_lookYaw = s_lookPitch = 0.0f;
}

// ---- the controls drawn over the screen

static const u32 kInk = 0xF0E8F0F8, kEdge = 0xC0B0D8E8, kLit = 0x90B0D8E8, kShade = 0x70000000;

void androidDrawOverlay(int ww, int wh)
{
	Geometry g = geometry(ww, wh);
	if (s_ring && s_view == VIEW_TOP)
		glesDisc(g.cx, g.cy, g.r, 0xA0000000, 0.0f);         // (the world dimmed behind the ring)
	const Button* list;
	int n;
	float size;
	buttonsFor(list, n, size);
	float br = size * g.r;
	for (int i = 0; i < n; i++)
	{
		float x, y;
		buttonAt(g, list[i], x, y);
		bool pressed = false;
		for (auto& f : s_fingers)
			pressed |= f.role == ROLE_BUTTON && f.button == i;
		Action a = list[i].act;
		bool on = (a == ACT_SNEAK && s_sneak) || (a == ACT_WALK && s_walk) || (a == ACT_LOOK && s_lookMode);
		glesDisc(x, y, br, pressed || on ? kLit : kShade, 0.0f);
		glesDisc(x, y, br, kEdge, br - 2.0f);
		// (the label's line as tall as fits: its letters about half that, the word inside the disc)
		float chars = (float)strlen(list[i].label);
		glesLabel(list[i].label, x, y, std::min(br * 0.85f, 1.7f * br / (chars * 13.0f / 30.0f)), kInk);
	}
	if (s_view != VIEW_TOP)
		return;
	// what is on, in words beside the ring's button: sneaking, looking
	if (!s_ring)
	{
		float y = g.cy - g.r * 0.76f;
		if (s_sneak)
			glesLabel("SNEAK", g.cx - g.r * 0.42f, y, g.r * 0.13f, kInk);
		if (s_lookMode)
			glesLabel("LOOK", g.cx + g.r * 0.42f, y, g.r * 0.13f, kInk);
	}
	// the stick under a walking finger, a ring round one held for the attack
	for (auto& f : s_fingers)
	{
		if (f.role != ROLE_GESTURE)
			continue;
		if (f.slid && !f.looks)
		{
			float reach = 0.22f * g.r;
			glesDisc(f.x0, f.y0, reach, 0x80B0D8E8, reach - 2.0f);
			glesDisc(f.x0 + s_padX * reach, f.y0 - s_padY * reach, reach * 0.4f, 0x70B0D8E8, 0.0f);
		}
		if (f.attack)
			glesDisc(f.x, f.y, g.r * 0.16f, 0xC04060E0, g.r * 0.16f - 4.0f);
	}
}

// ---- the on-screen keyboard: the system's, in a dialog (MorrowindActivity.askText), while the game waits

void swkbdInit(SwkbdState* s, int type, int, int)
{
	memset(s, 0, sizeof(*s));
	s->type = type;
}

void swkbdSetHintText(SwkbdState* s, const char* hint)
{
	snprintf(s->hint, sizeof(s->hint), "%s", hint ? hint : "");
}

void swkbdSetInitialText(SwkbdState* s, const char* text)
{
	snprintf(s->initial, sizeof(s->initial), "%s", text ? text : "");
}

SwkbdButton swkbdInputText(SwkbdState* s, char* buf, size_t size)
{
	JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
	jobject activity = (jobject)SDL_GetAndroidActivity();
	if (!env || !activity)
		return SWKBD_BUTTON_NONE;
	jclass cls = env->GetObjectClass(activity);
	jmethodID ask = env->GetStaticMethodID(cls, "askText", "(Ljava/lang/String;Ljava/lang/String;ZZ)Ljava/lang/String;");
	SwkbdButton result = SWKBD_BUTTON_NONE;
	if (ask)
	{
		jstring hint = env->NewStringUTF(s->hint), initial = env->NewStringUTF(s->initial);
		jstring got = (jstring)env->CallStaticObjectMethod(cls, ask, hint, initial, (jboolean)(s->passwordMode != 0),
			(jboolean)(s->type == SWKBD_TYPE_NUMPAD));
		if (got)
		{
			const char* text = env->GetStringUTFChars(got, nullptr);
			snprintf(buf, size, "%s", text);
			env->ReleaseStringUTFChars(got, text);
			env->DeleteLocalRef(got);
			result = SWKBD_BUTTON_CONFIRM;
		}
		env->DeleteLocalRef(hint);
		env->DeleteLocalRef(initial);
	}
	if (env->ExceptionCheck())
		env->ExceptionClear();
	env->DeleteLocalRef(cls);
	env->DeleteLocalRef(activity);
	return result;
}
