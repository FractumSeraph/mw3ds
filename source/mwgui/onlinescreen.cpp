// The online mode's screens before the game loads (FractumSeraph/mw3ds): the server and account, signing in, the
// profile a server may ask for, the character list. Driven like the title screen: the bottom screen takes touch and the
// D-pad, the system keyboard asks for text. Returns "play" once the world has welcomed the player (g_online holds the
// connection and what to load), "" to go back to the title.
#include <3ds.h>
#include <citro2d.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "components/debug/debuglog.hpp"
#include "components/files/zfile.hpp"
#include "extern/cjson/cJSON.h"
#include "mwgui/loadingscreen.hpp"
#include "mwgui/widgets.hpp"
#include "mwinput/inputmanagerimp.hpp"
#include "mwsound/soundmanagerimp.hpp"
#include "mwmp/online.hpp"
#include "mwmp/netsocket.hpp"
#include "platform/linear.hpp"

namespace
{
// The system keyboard: false when cancelled
bool askText(const char* hint, std::string& text, bool password, int maxLen = 64)
{
	SwkbdState kb;
	swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, maxLen);
	swkbdSetHintText(&kb, hint);
	if (password)
		swkbdSetPasswordMode(&kb, SWKBD_PASSWORD_HIDE_DELAY);
	else
		swkbdSetInitialText(&kb, text.c_str());
	swkbdSetValidation(&kb, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	char buf[256] = {};
	SwkbdButton b = swkbdInputText(&kb, buf, sizeof(buf));
	if (b != SWKBD_BUTTON_CONFIRM)
		return false;
	text = buf;
	return true;
}

// The data's content files as the server names them (game.json "addons": the converter's --tribunal / --bloodmoon)
void readContent(const char* dataDir)
{
	std::vector<std::string> esm = { "morrowind.esm" };
	std::string path = std::string(dataDir) + "/game.json";
	if (char* text = zreadAll(path.c_str()))
	{
		if (cJSON* j = cJSON_Parse(text))
		{
			const cJSON* a;
			cJSON_ArrayForEach(a, cJSON_GetObjectItem(j, "addons"))
				if (cJSON_IsString(a))
					esm.push_back(std::string(a->valuestring) + ".esm");
			cJSON_Delete(j);
		}
		free(text);
	}
	onlineSetContent(esm);
	std::string list;
	for (auto& e : esm)
		list += (list.empty() ? "" : ", ") + e;
	logf("online: this data's content: %s", list.c_str());
}
}

// Not online after all: the connection and the console's socket service go, so the game is the offline one again
void onlineLeave()
{
	delete g_online;
	g_online = nullptr;
	netExit();
}

std::string onlineTitleScreen(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* dataDir)
{
	if (!g_online)
		g_online = new Online();
	Online& on = *g_online;
	on.config.load();
	readContent(dataDir);
	std::string username, email;
	UiList list;
	bool wasTouching = false, dragged = false;
	int lastX = 0, lastY = 0, pressX = 0, pressY = 0;
	u64 lastTick = svcGetSystemTick();
	while (aptMainLoop())
	{
		audioUpdate();
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		UiInput ui = {};
		touchPosition tp;
		hidTouchRead(&tp);
		if (held & KEY_TOUCH)
		{
			ui.touching = true;
			ui.touchX = tp.px;
			ui.touchY = tp.py;
			if (!wasTouching)
			{
				dragged = false;
				pressX = tp.px;
				pressY = tp.py;
			}
			else
			{
				ui.dragDX = tp.px - lastX;
				ui.dragDY = tp.py - lastY;
				if (abs(tp.px - pressX) + abs(tp.py - pressY) > kTapSlop)
					dragged = true;
			}
			lastX = tp.px;
			lastY = tp.py;
		}
		else if (wasTouching && !dragged)
		{
			ui.tapped = true;
			ui.tapX = lastX;
			ui.tapY = lastY;
		}
		wasTouching = held & KEY_TOUCH;
		ui.down = down;
		ui.held = held;
		u64 tick = svcGetSystemTick();
		ui.dt = fminf((float)(tick - lastTick) / SYSCLOCK_ARM11, 0.1f);
		lastTick = tick;
		uiBeginFrame(ui);
		on.update(ui.dt);
		if (on.phase == Online::WELCOMED)
			return "play";

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		linearRetire();
		C2D_Prepare();
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		clearTop(top, 0x000000FF);
		C2D_SceneBegin(top);
		bool art = uiArt("art/menu_morrowind.t3x", 400, 240, 0, 0, 400, 240);
		uiRect(0, art ? 150 : 60, 400, 90, 0xB0000000);
		uiTextCentered(200, art ? 156 : 70, 0.6f, col::header, "Online");
		uiTextCentered(200, art ? 180 : 94, 0.45f, col::text, on.config.server);
		std::string line = on.phase == Online::FAILED ? on.error() : on.status;
		if (line.empty())
			line = "Sign in with your account on the server's website";
		uiTextCentered(200, art ? 204 : 118, 0.45f, on.phase == Online::FAILED ? col::health : col::textDim, line);

		C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
		C2D_SceneBegin(bottom);
		uiRect(0, 0, 320, 20, col::panelLight);
		uiRect(0, 20, 320, 1, col::border);
		std::string result = "\x01";
		switch (on.phase)
		{
		case Online::IDLE: case Online::FAILED:
		{
			uiText(6, 2, 0.55f, col::header, "Sign in");
			uiText(8, 34, 0.45f, col::textDim, "Server");
			if (uiButton(8, 50, 304, 28, on.config.server))
			{
				std::string s = on.config.server;
				if (askText("Server, e.g. https://morrowind.fractumseraph.net", s, false, 200))
				{
					on.config.server = s;
					on.config.save();
				}
			}
			uiText(8, 86, 0.45f, col::textDim, "Account");
			if (uiButton(8, 102, 304, 28, on.config.account.empty() ? "(tap to enter)" : on.config.account))
			{
				std::string a = on.config.account;
				if (askText("Your account name", a, false))
				{
					on.config.account = a;
					on.config.save();
				}
			}
			uiText(8, 140, 0.4f, col::textDim, "The password is asked each time and never kept.");
			if (uiButton(4, 208, 154, 28, "Back") || (down & KEY_B))
				result = "";
			bool can = !on.config.account.empty() && !on.config.server.empty();
			if (uiButton(162, 208, 154, 28, "Sign in", true, can) || (can && (down & KEY_A)))
			{
				std::string pw;
				if (askText("Password", pw, true))
					on.signIn(on.config.server, on.config.account, pw);
				lastTick = svcGetSystemTick();
			}
			break;
		}
		case Online::PROFILE_NEEDED:
		{
			uiText(6, 2, 0.55f, col::header, "Your profile");
			uiText(8, 30, 0.42f, col::text, on.status);
			uiText(8, 52, 0.45f, col::textDim, "Username (3-20 letters and digits)");
			if (uiButton(8, 68, 304, 28, username.empty() ? "(tap to enter)" : username))
				askText("Username", username, false, 20);
			uiText(8, 104, 0.45f, col::textDim, "Email (when the server asks for one)");
			if (uiButton(8, 120, 304, 28, email.empty() ? "(tap to enter)" : email))
				askText("Email", email, false, 254);
			if (uiButton(4, 208, 154, 28, "Back") || (down & KEY_B))
			{
				on.cancel();
				result = "";
			}
			if (uiButton(162, 208, 154, 28, "Save", true, !username.empty()))
				on.setProfile(username, email);
			lastTick = svcGetSystemTick();
			break;
		}
		case Online::CHARACTERS:
		{
			uiText(6, 2, 0.55f, col::header, "Characters");
			std::vector<std::string> rows, right;
			for (auto& c : on.characters)
			{
				rows.push_back(c.name.empty() ? "(unnamed)" : c.name);
				right.push_back(c.needsChargen ? "new" : "level " + std::to_string(c.level));
			}
			int pick = uiList(list, 4, 24, 312, 176, rows, 0.5f, true, &right);
			bool has = list.selected >= 0 && list.selected < (int)on.characters.size();
			if (uiButton(4, 208, 100, 28, "Sign out") || (down & KEY_B))
			{
				on.cancel();
				result = "";
			}
			if (uiButton(108, 208, 100, 28, "New"))
				on.createCharacter();
			if (uiButton(212, 208, 104, 28, "Play", true, has) || (has && pick >= 0 && (down & KEY_A)))
				on.chooseCharacter(on.characters[list.selected].id);
			break;
		}
		default:
			// signing in, opening the world, connecting: what is happening, and a way out
			uiText(6, 2, 0.55f, col::header, "Online");
			uiTextCentered(160, 100, 0.5f, col::text, on.status);
			if (uiButton(84, 208, 152, 28, "Cancel") || (down & KEY_B))
			{
				on.cancel();
				result = "";
			}
			break;
		}
		C3D_FrameEnd(0);
		if (result != "\x01")
		{
			if (on.phase != Online::IDLE)
				on.cancel();
			onlineLeave();
			return result;
		}
	}
	onlineLeave();
	return "";
}

// Scripted runs (tools/test, the PC test build): sdmc:/3ds/mw3ds/online_test.json {"server", "account", "password",
// "character": "new" | "first" | an id} signs in without the screens, for a local test server's test accounts. False
// when there is no such file or the sign-in failed (logged)
bool onlineAutoSignIn(const char* dataDir)
{
	char* text = zreadAll("sdmc:/3ds/mw3ds/online_test.json");
	if (!text)
		return false;
	cJSON* j = cJSON_Parse(text);
	free(text);
	if (!j)
		return false;
	auto get = [&](const char* k) {
		const cJSON* v = cJSON_GetObjectItem(j, k);
		return std::string(cJSON_IsString(v) ? v->valuestring : "");
	};
	std::string server = get("server"), account = get("account"), password = get("password"), character = get("character");
	std::string say = get("say"), probe = get("probe"), join = get("join");
	cJSON_Delete(j);
	if (!g_online)
		g_online = new Online();
	readContent(dataDir);
	// "probe": one HTTPS (or HTTP) GET of a public address first, its answer logged: the console's TLS checked against
	// a real server without signing in anywhere
	if (!probe.empty())
	{
		HttpRequest h;
		h.start("GET", probe, {}, "");
		u64 t0 = osGetTime();
		while (h.update() == HttpRequest::RUNNING && osGetTime() - t0 < 20000)
			svcSleepThread(10 * 1000 * 1000);
		logf("online test: probe %s -> %s %d %s", probe.c_str(), h.state() == HttpRequest::DONE ? "answer" : "FAILED",
			h.status, h.state() == HttpRequest::DONE ? h.body.substr(0, 160).c_str() : h.error().c_str());
	}
	if (server.empty())
		return false;
	Online& on = *g_online;
	on.signIn(server, account, password);
	on.testSay = say;
	on.testJoin = join;
	on.testSayAt = 5.0f;
	logf("online test: signing in to %s as %s", server.c_str(), account.c_str());
	u64 start = osGetTime(), last = svcGetSystemTick();
	bool chosen = false;
	while (osGetTime() - start < 60000)
	{
		u64 tick = svcGetSystemTick();
		on.update(fminf((float)(tick - last) / SYSCLOCK_ARM11, 0.1f));
		last = tick;
		if (on.phase == Online::WELCOMED)
			return true;
		if (on.phase == Online::FAILED)
			return false;
		if (on.phase == Online::PROFILE_NEEDED)
			on.setProfile(account.substr(0, 20), account + "@example.test");
		if (on.phase == Online::CHARACTERS && !chosen)
		{
			chosen = true;
			if (character == "new" || on.characters.empty())
				on.createCharacter();
			else if (character == "first" || character.empty())
				on.chooseCharacter(on.characters[0].id);
			else
				on.chooseCharacter(character);
		}
		svcSleepThread(10 * 1000 * 1000);
	}
	logf("online test: no welcome in a minute");
	return false;
}
