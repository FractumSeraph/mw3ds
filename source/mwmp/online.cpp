// The online mode's connection to an openmw-web server (mwmp/online.hpp)
#include "mwmp/online.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "extern/cjson/cJSON.h"
#include <3ds.h>

Online* g_online = nullptr;

namespace
{
const char* kConfigPath = "sdmc:/3ds/mw3ds/online.json";
const char* kSubprotocol = "omw-mp.2";
std::vector<std::string> g_content = { "morrowind.esm" };

// Binary frame types (PROTOCOL.md "Binary type registry")
const uint16_t T_EVENT = 0x0002, T_PLAYER_MOVE = 0x0100, T_MOVE_BATCH = 0x0101, T_STATE_BATCH = 0x0103,
	T_ACTOR_BATCH = 0x0200;

const char* str(const cJSON* o, const char* key, const char* fallback = "")
{
	const cJSON* v = cJSON_GetObjectItem(o, key);
	return cJSON_IsString(v) ? v->valuestring : fallback;
}

double num(const cJSON* o, const char* key, double fallback = 0)
{
	const cJSON* v = cJSON_GetObjectItem(o, key);
	return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

std::string lowerCase(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z')
			c += 32;
	return s;
}

void putU16(std::string& s, uint16_t v)
{
	s.push_back((char)(v & 0xFF));
	s.push_back((char)(v >> 8));
}

void putU32(std::string& s, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		s.push_back((char)(v >> (8 * i)));
}

void putF32(std::string& s, float f)
{
	uint32_t v;
	memcpy(&v, &f, 4);
	putU32(s, v);
}

uint16_t getU16(const uint8_t* p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

uint32_t getU32(const uint8_t* p)
{
	return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
}

float getF32(const uint8_t* p)
{
	uint32_t v = getU32(p);
	float f;
	memcpy(&f, &v, 4);
	return f;
}

const float kTau = 6.28318530718f;

// PlayerMove's 20 bytes (PROTOCOL.md 0x0100): x y z f32, yaw u16 (0..2pi), pitch u8 (-pi/2..pi/2), flags, animVel u8
// (0..2 x walk), a sample-time counter (left 0: the player's own timing rides 0x0102's simMs, which we don't send)
void putPose(std::string& s, const NetPose& p)
{
	putF32(s, p.x);
	putF32(s, p.y);
	putF32(s, p.z);
	float yaw = fmodf(p.yaw, kTau);
	if (yaw < 0)
		yaw += kTau;
	putU16(s, (uint16_t)((int)lroundf(yaw / kTau * 65536.0f) & 0xFFFF));
	float pitch = fmaxf(-1.5707963f, fminf(1.5707963f, p.pitch));
	s.push_back((char)(uint8_t)lroundf((pitch + 1.5707963f) / 3.14159265f * 255.0f));
	s.push_back((char)p.flags);
	s.push_back((char)(uint8_t)fminf(255.0f, fmaxf(0.0f, p.animVel / 2.0f * 255.0f)));
	s.push_back(0);
	s.push_back(0);
	s.push_back(0);
}

NetPose getPose(const uint8_t* p)
{
	NetPose n;
	n.x = getF32(p);
	n.y = getF32(p + 4);
	n.z = getF32(p + 8);
	n.yaw = getU16(p + 12) / 65536.0f * kTau;
	n.pitch = p[14] / 255.0f * 3.14159265f - 1.5707963f;
	n.flags = p[15];
	n.animVel = p[16] / 255.0f * 2.0f;
	return n;
}

std::string jsonString(const std::string& s)
{
	cJSON* j = cJSON_CreateString(s.c_str());
	char* t = cJSON_PrintUnformatted(j);
	std::string out = t ? t : "\"\"";
	free(t);
	cJSON_Delete(j);
	return out;
}
}

void onlineSetContent(const std::vector<std::string>& esm)
{
	g_content = esm;
}

// ---- the saved server and account

void OnlineConfig::load()
{
	FILE* f = fopen(kConfigPath, "rb");
	if (!f)
		return;
	std::string text;
	char buf[1024];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	fclose(f);
	cJSON* j = cJSON_Parse(text.c_str());
	if (!j)
		return;
	if (*str(j, "server"))
		server = str(j, "server");
	account = str(j, "account");
	cJSON_Delete(j);
}

void OnlineConfig::save() const
{
	cJSON* j = cJSON_CreateObject();
	cJSON_AddStringToObject(j, "server", server.c_str());
	cJSON_AddStringToObject(j, "account", account.c_str());
	char* t = cJSON_Print(j);
	if (FILE* f = fopen(kConfigPath, "wb"))
	{
		fputs(t, f);
		fclose(f);
	}
	free(t);
	cJSON_Delete(j);
}

// ---- signing in

Online::~Online()
{
	cancel();
}

void Online::fail(const std::string& why)
{
	err = why;
	status = why;
	phase = FAILED;
	step = NONE;
	logf("online: %s", why.c_str());
	ws.close();
}

void Online::cancel()
{
	if (phase == IN_WORLD || phase == WELCOMED)
	{
		sendEvent("PlayerLeaving", LValue::newTable());
		ws.update();
	}
	ws.close();
	step = NONE;
	phase = IDLE;
	status.clear();
	players.clear();
	if (playerRecord)
		cJSON_Delete(playerRecord);
	playerRecord = nullptr;
	password.clear();
}

void Online::defaultManifest()
{
	// The world's own list (its sim peer's): the engine's builtin scripts first, the masters, the multiplayer scripts last
	manifest.clear();
	manifest.push_back("builtin.omwscripts");
	for (auto& m : g_content)
		manifest.push_back(m);
	manifest.push_back("mp.omwscripts");
}

void Online::signIn(const std::string& server, const std::string& acct, const std::string& pw)
{
	cancel();
	err.clear();
	account = acct;
	password = pw;
	config.server = server;
	config.account = acct;
	config.save();
	defaultManifest();
	manifestRetried = false;
	worldTries = 0;
	characters.clear();
	std::string s = server;
	while (!s.empty() && s.back() == '/')
		s.pop_back();
	if (s.compare(0, 5, "ws://") == 0 || s.compare(0, 6, "wss://") == 0)
	{
		// One world: straight to its socket (/ws when no path is given)
		kind = WORLD;
		NetUrl u;
		if (!u.parse(s))
		{
			fail("That isn't a server address");
			return;
		}
		wsUrl = u.path == "/" ? s + "/ws" : s;
		characterId.clear();
		openWorld();
		return;
	}
	if (s.find("://") == std::string::npos)
		s = "https://" + s;
	kind = GATEWAY;
	base = s;
	phase = SIGNING_IN;
	status = "Signing in...";
	cJSON* body = cJSON_CreateObject();
	cJSON_AddStringToObject(body, "name", acct.c_str());
	cJSON_AddStringToObject(body, "password", pw.c_str());
	char* t = cJSON_PrintUnformatted(body);
	request(LOGIN, "POST", "/auth/password", t);
	free(t);
	cJSON_Delete(body);
}

void Online::request(Step s, const std::string& method, const std::string& path, const std::string& body)
{
	step = s;
	std::vector<std::string> headers;
	if (!locker.empty())
		headers.push_back("Authorization: Bearer " + locker);
	if (!http.start(method, base + path, headers, body))
		fail("Can't reach the server: " + http.error());
}

void Online::setProfile(const std::string& username, const std::string& email)
{
	cJSON* body = cJSON_CreateObject();
	cJSON_AddStringToObject(body, "username", username.c_str());
	if (!email.empty())
		cJSON_AddStringToObject(body, "email", email.c_str());
	char* t = cJSON_PrintUnformatted(body);
	phase = SIGNING_IN;
	status = "Saving your profile...";
	request(PROFILE_SET, "POST", "/auth/profile", t);
	free(t);
	cJSON_Delete(body);
}

void Online::chooseCharacter(const std::string& id)
{
	characterId = id;
	newCharacter = false;
	for (auto& c : characters)
		if (c.id == id)
			newCharacter = c.needsChargen;
	phase = OPENING_WORLD;
	status = "Opening your world...";
	worldTries = 0;
	cJSON* body = cJSON_CreateObject();
	cJSON_AddStringToObject(body, "mode", "private");
	cJSON_AddStringToObject(body, "characterId", id.c_str());
	char* t = cJSON_PrintUnformatted(body);
	request(WORLDS, "POST", "/worlds", t);
	free(t);
	cJSON_Delete(body);
}

void Online::createCharacter()
{
	phase = SIGNING_IN;
	status = "Making a new character...";
	request(CHAR_NEW, "POST", "/auth/characters", "{}");
}

// One HTTP answer in: the next step
void Online::onHttp()
{
	int code = http.status;
	cJSON* j = cJSON_Parse(http.body.c_str());
	Step was = step;
	step = NONE;
	auto errorText = [&](const char* fallback) {
		std::string e = j ? str(j, "message", str(j, "error", "")) : "";
		char codeText[16];
		snprintf(codeText, sizeof(codeText), " (%d)", code);
		return (e.empty() ? std::string(fallback) : e) + codeText;
	};
	switch (was)
	{
	case LOGIN:
		if (code == 200 && j)
		{
			ticket = str(j, "ticket");
			locker = str(j, "locker");
			accountKey = str(j, "account");
			displayName = str(j, "name", account.c_str());
			password.clear();                // (the ticket and the session token are all we need from here)
			status = "Signed in as " + displayName;
			request(PROFILE_GET, "GET", "/auth/profile", "");
		}
		else if (code == 401)
			fail("Wrong account name or password");
		else if (code == 403)
			fail(errorText("This server doesn't take passwords, or the account is banned"));
		else if (code == 429)
			fail("Too many sign-ins: wait a minute and try again");
		else
			fail(errorText("Sign-in failed"));
		break;
	case PROFILE_GET:
		if (code == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "needsProfile")))
		{
			phase = PROFILE_NEEDED;
			status = cJSON_IsTrue(cJSON_GetObjectItem(j, "hasEmail")) ? "Choose a username" : "Choose a username, and give an email";
		}
		else
			request(CHARS, "GET", "/auth/characters", "");
		break;
	case PROFILE_SET:
		if (code == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "ok")))
			request(CHARS, "GET", "/auth/characters", "");
		else
		{
			phase = PROFILE_NEEDED;
			status = std::string("Not accepted: ") + (j ? str(j, "error", "try another") : "try another")
				+ (j && *str(j, "field") ? std::string(" (") + str(j, "field") + ")" : "");
		}
		break;
	case CHARS:
		if (code == 200 && j)
		{
			characters.clear();
			const cJSON* c;
			cJSON_ArrayForEach(c, cJSON_GetObjectItem(j, "characters"))
			{
				OnlineCharacter oc;
				oc.id = str(c, "id");
				oc.name = str(c, "name");
				oc.level = (int)num(c, "level", 1);
				oc.needsChargen = cJSON_IsTrue(cJSON_GetObjectItem(c, "needsChargen"));
				characters.push_back(oc);
			}
			phase = CHARACTERS;
			status = characters.empty() ? "No characters yet: make one" : "Choose a character";
		}
		else
			fail(errorText("Couldn't read your characters"));
		break;
	case CHAR_NEW:
		if (code == 200 && j)
		{
			const cJSON* c = cJSON_GetObjectItem(j, "character");
			OnlineCharacter oc;
			oc.id = str(c, "id");
			oc.name = str(c, "name", "New character");
			oc.needsChargen = true;
			characters.push_back(oc);
			chooseCharacter(oc.id);
		}
		else
			fail(errorText("Couldn't make a character"));
		break;
	case WORLDS:
		if (code == 200 && j && *str(j, "wsPath"))
		{
			worldName = str(j, "name", str(j, "id"));
			NetUrl u;
			u.parse(base);
			wsUrl = std::string(u.tls ? "wss://" : "ws://") + u.host + (u.port != (u.tls ? 443 : 80)
				? ":" + std::to_string(u.port) : "") + str(j, "wsPath");
			// The ticket from signing in is good for 15 minutes; a later world asks for a fresh one
			if (ticket.empty())
				request(TICKET, "POST", "/auth/ticket", "");
			else
				openWorld();
		}
		else if (code == 503 && ++worldTries < 20)
		{
			status = "The server is starting your world...";
			retryAt = now + 3.0f;
			step = WORLDS;
		}
		else
			fail(errorText(code == 429 ? "Too many worlds open for this account" : "Couldn't open your world"));
		break;
	case TICKET:
		if (code == 200 && j && *str(j, "ticket"))
		{
			ticket = str(j, "ticket");
			openWorld();
		}
		else
			fail(errorText("Signed out: sign in again"));
		break;
	default:
		break;
	}
	if (j)
		cJSON_Delete(j);
}

void Online::openWorld()
{
	phase = CONNECTING;
	status = "Connecting to the world...";
	helloSent = false;
	seq = 0;
	players.clear();
	if (!ws.open(wsUrl, kSubprotocol))
		fail("Can't reach the world: " + ws.error());
}

void Online::sendJson(cJSON* obj)
{
	char* t = cJSON_PrintUnformatted(obj);
	if (t)
		ws.sendText(t);
	free(t);
	cJSON_Delete(obj);
}

void Online::update(float dt)
{
	// the network's clock is the real one (a scripted run on the PC steps its frames faster than real time)
	(void)dt;
	u64 ms = osGetTime();
	if (!clockStart)
		clockStart = ms;
	now = (float)(ms - clockStart) / 1000.0f;
	if (step != NONE)
	{
		if (retryAt >= 0)
		{
			if (now < retryAt)
				return;
			retryAt = -1;
			Step s = step;
			if (s == WORLDS)
			{
				chooseCharacter(characterId);
				return;
			}
		}
		HttpRequest::State hs = http.update();
		if (hs == HttpRequest::DONE)
			onHttp();
		else if (hs == HttpRequest::FAILED)
			fail("Can't reach the server: " + http.error());
		return;
	}
	if (phase < CONNECTING || phase == FAILED)
		return;
	WebSocket::State s = ws.update();
	WebSocket::Message m;
	while (ws.next(m))
	{
		if (m.text)
			onText(m.data);
		else
			onBinary(m.data);
	}
	if (phase == FAILED)
		return;
	if (s == WebSocket::FAILED)
	{
		fail(phase >= WELCOMED ? "Lost the connection: " + ws.error() : "Can't reach the world: " + ws.error());
		return;
	}
	if (s == WebSocket::OPEN && !helloSent)
	{
		// SessionHello (PROTOCOL.md session tier): no engine hash (the server's default "warn" takes that), our content
		// files, and no simulatesActors: the server's sim peer runs the NPCs
		helloSent = true;
		phase = SESSION;
		status = "Joining...";
		cJSON* h = cJSON_CreateObject();
		cJSON_AddStringToObject(h, "t", "SessionHello");
		cJSON_AddNumberToObject(h, "proto", 3);
		cJSON_AddStringToObject(h, "engineHash", "");
		cJSON_AddNumberToObject(h, "lserVersion", 0);
		cJSON* list = cJSON_AddArrayToObject(h, "manifest");
		for (size_t i = 0; i < manifest.size(); i++)
		{
			cJSON* e = cJSON_CreateObject();
			cJSON_AddStringToObject(e, "name", manifest[i].c_str());
			cJSON_AddNumberToObject(e, "size", 0);
			cJSON_AddNumberToObject(e, "idx", (double)i);
			cJSON_AddItemToArray(list, e);
		}
		sendJson(h);
	}
	if (phase == IN_WORLD && now >= pingAt)
	{
		pingAt = now + 10.0f;
		cJSON* p = cJSON_CreateObject();
		cJSON_AddStringToObject(p, "t", "SessionPing");
		cJSON_AddNumberToObject(p, "clientTime", now * 1000.0);
		sendJson(p);
	}
}

// The session tier: JSON text frames
void Online::onText(const std::string& text)
{
	cJSON* j = cJSON_Parse(text.c_str());
	if (!j)
		return;
	std::string t = str(j, "t");
	if (t == "SessionHelloOk")
	{
		logf("online: hello ok from %s (content %s)", str(j, "serverName"), str(j, "contentPolicy"));
		cJSON* l = cJSON_CreateObject();
		if (kind == GATEWAY)
		{
			cJSON_AddStringToObject(l, "t", "SessionLoginTicket");
			cJSON_AddStringToObject(l, "ticket", ticket.c_str());
			ticket.clear();                  // (spent once accepted; the next world asks for a fresh one)
		}
		else
		{
			cJSON_AddStringToObject(l, "t", "SessionLoginRequest");
			cJSON_AddStringToObject(l, "account", account.c_str());
			cJSON_AddStringToObject(l, "password", password.c_str());
		}
		if (!characterId.empty())
			cJSON_AddStringToObject(l, "characterId", characterId.c_str());
		sendJson(l);
	}
	else if (t == "SessionWelcome")
	{
		worldName = wsUrl.substr(wsUrl.rfind('/') + 1);
		playerId = (int)num(j, "playerId", -1);
		motd = str(j, "motd");
		if (*str(j, "characterId"))
			characterId = str(j, "characterId");
		const cJSON* flags = cJSON_GetObjectItem(j, "flags");
		pvp = cJSON_IsTrue(cJSON_GetObjectItem(flags, "pvp"));
		simulated = cJSON_IsTrue(cJSON_GetObjectItem(flags, "simulated"));
		const cJSON* ca = cJSON_GetObjectItem(flags, "clientAuthority");
		clientAuthority = !ca || cJSON_IsTrue(ca);
		const cJSON* rec = cJSON_GetObjectItem(j, "playerRecord");
		if (!rejoined)
		{
			if (playerRecord)
				cJSON_Delete(playerRecord);
			playerRecord = rec && cJSON_IsObject(rec) ? cJSON_Duplicate(rec, true) : nullptr;
			newCharacter = playerRecord == nullptr;
		}
		switching = false;
		const cJSON* prof = cJSON_GetObjectItem(j, "profile");
		if (*str(prof, "username"))
			displayName = str(prof, "username");
		phase = WELCOMED;
		status = "In the world";
		password.clear();
		logf("online: welcome, player %d, %s character", playerId, newCharacter ? "a new" : "a returning");
		// (the message of the day comes again as a server ChatMessage: PROTOCOL.md, shown once from there)
	}
	else if (t == "SessionDisconnect")
	{
		std::string code = str(j, "code"), detail = str(j, "detail");
		// A world that runs other content files says which (BAD_CONTENT "... runs A + B + C"): when they are all ones this
		// data has (or the multiplayer scripts the 3DS stands in for), try once more with its list and order
		if (code == "BAD_CONTENT" && !manifestRetried)
		{
			size_t at = detail.find("runs ");
			std::vector<std::string> theirs;
			bool usable = at != std::string::npos;
			if (usable)
			{
				std::string rest = detail.substr(at + 5);
				size_t p = 0;
				while (p < rest.size())
				{
					size_t plus = rest.find(" + ", p);
					std::string name = lowerCase(rest.substr(p, plus == std::string::npos ? std::string::npos : plus - p));
					while (!name.empty() && (name.back() == '.' || name.back() == ' ' || name.back() == ')'))
						name.pop_back();
					theirs.push_back(name);
					if (plus == std::string::npos)
						break;
					p = plus + 3;
				}
				for (auto& n : theirs)
				{
					bool ours = n.size() > 11 && n.compare(n.size() - 11, 11, ".omwscripts") == 0;
					for (auto& c : g_content)
						ours = ours || c == n;
					usable = usable && ours;
				}
			}
			if (usable && !theirs.empty())
			{
				logf("online: the world runs %s; joining with its list", detail.c_str());
				manifestRetried = true;
				manifest = theirs;
				cJSON_Delete(j);
				if (kind == GATEWAY)
					request(TICKET, "POST", "/auth/ticket", "");
				else
					openWorld();
				return;
			}
		}
		std::string why = code == "BAD_CONTENT" ? "This world runs other game files: " + detail
			: code == "AUTH_FAILED" ? "The world refused the sign-in" + (detail.empty() ? "" : ": " + detail)
			: code == "SUPERSEDED" ? "You signed in somewhere else"
			: code == "BANNED" ? "This account is banned" + (detail.empty() ? "" : ": " + detail)
			: code == "SERVER_FULL" ? "The world is full"
			: code == "SHUTDOWN" ? "The server is shutting down"
			: "Disconnected (" + code + (detail.empty() ? "" : ": " + detail) + ")";
		cJSON_Delete(j);
		fail(why);
		return;
	}
	else if (t == "SessionPong")
		rttMs = (float)(now * 1000.0 - num(j, "clientTime", now * 1000.0));
	else if (t == "ProfileResult")
	{
		if (!cJSON_IsTrue(cJSON_GetObjectItem(j, "ok")))
			addChat("server", "", std::string("Profile: ") + str(j, "error"));
	}
	cJSON_Delete(j);
}

void Online::ready(const std::string& cellKey, const float pos[3])
{
	if (phase != WELCOMED)
		return;
	cJSON* r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "t", "SessionReady");
	sendJson(r);
	phase = IN_WORLD;
	sendCellChange(cellKey, pos);           // (until this one nobody sees us: PROTOCOL.md M1)
	if (newCharacter)
		logf("online: a new character: Morrowind's creation first, ChargenComplete after");
}

// ---- binary frames: [u16 type][u32 seq] payload

void Online::sendEvent(const std::string& name, const LValue& body)
{
	if (phase != IN_WORLD && phase != WELCOMED)
		return;
	std::string f;
	putU16(f, T_EVENT);
	putU32(f, ++seq);
	f.push_back((char)name.size());
	f += name;
	f += lserEncode(body);
	ws.sendBinary(f);
}

void Online::sendMove(const NetPose& p)
{
	if (phase != IN_WORLD)
		return;
	std::string f;
	putU16(f, T_PLAYER_MOVE);
	putU32(f, ++seq);
	putPose(f, p);
	ws.sendBinary(f);
}

void Online::sendCellChange(const std::string& cellKey, const float pos[3])
{
	LValue b = LValue::newTable();
	b.set("cellKey", cellKey);
	b.set("x", (double)pos[0]);
	b.set("y", (double)pos[1]);
	b.set("z", (double)pos[2]);
	sendEvent("PlayerCellChange", b);
}

void Online::switchWorld(const std::string& wsPath)
{
	if (kind != GATEWAY || wsPath.empty())
		return;
	if (phase == IN_WORLD || phase == WELCOMED)
	{
		sendEvent("PlayerLeaving", LValue::newTable());
		ws.update();
	}
	ws.close();
	NetUrl u;
	u.parse(base);
	wsUrl = std::string(u.tls ? "wss://" : "ws://") + u.host + (u.port != (u.tls ? 443 : 80)
		? ":" + std::to_string(u.port) : "") + wsPath;
	switching = rejoined = true;
	manifestRetried = false;
	phase = OPENING_WORLD;
	status = "Travelling to another world...";
	logf("online: to the world at %s", wsPath.c_str());
	request(TICKET, "POST", "/auth/ticket", "");
}

void Online::goHome()
{
	if (kind != GATEWAY || characterId.empty())
		return;
	if (phase == IN_WORLD || phase == WELCOMED)
	{
		sendEvent("PlayerLeaving", LValue::newTable());
		ws.update();
	}
	ws.close();
	switching = rejoined = true;
	chooseCharacter(characterId);
	status = "Going home...";
}

void Online::sendChat(const std::string& text)
{
	LValue b = LValue::newTable();
	b.set("text", text.substr(0, 400));
	sendEvent("ChatSend", b);
}

void Online::addChat(const std::string& channel, const std::string& from, const std::string& text)
{
	chat.push_back({ channel, from, text });
	while (chat.size() > 40)
		chat.pop_front();
	chatRevision++;
	logf("chat [%s] %s: %s", channel.c_str(), from.c_str(), text.c_str());
}

void Online::onBinary(const std::string& data)
{
	if (data.size() < 6)
		return;
	const uint8_t* p = (const uint8_t*)data.data();
	uint16_t type = getU16(p);
	const uint8_t* body = p + 6;
	size_t n = data.size() - 6;
	if (type == T_EVENT)
	{
		if (n < 1 || n < 1 + (size_t)body[0])
			return;
		std::string name((const char*)body + 1, body[0]);
		LValue v;
		if (!lserDecode(body + 1 + body[0], n - 1 - body[0], v))
		{
			logf("online: event %s did not decode (%u bytes)", name.c_str(), (unsigned)(n - 1 - body[0]));
			return;
		}
		onEvent(name, v);
	}
	else if (type == T_MOVE_BATCH)
	{
		// [u8 count] count x (u16 id + 20-byte pose): the players we see, newest pose each
		if (n < 1)
			return;
		int count = body[0];
		for (int i = 0; i < count && 1 + (size_t)(i + 1) * 22 <= n; i++)
		{
			const uint8_t* e = body + 1 + i * 22;
			uint16_t id = getU16(e);
			if (id == (uint16_t)playerId)
				continue;
			RemotePlayer& r = players[id];
			r.id = id;
			r.prev = r.lastAt >= 0 ? r.last : getPose(e + 2);
			r.prevAt = r.lastAt >= 0 ? r.lastAt : now;
			r.last = getPose(e + 2);
			r.lastAt = now;
			r.inView = true;
		}
	}
	// 0x0103 (our own state: only under the server's input model) and 0x0200 (the sim peer's NPCs) are for later
	(void)T_STATE_BATCH;
	(void)T_ACTOR_BATCH;
}

// The events this client acts on (PROTOCOL.md event tier); the rest wait in `events` for the game, or are dropped
void Online::onEvent(const std::string& name, const LValue& b)
{
	auto idOf = [&]() { return (uint16_t)b.numberAt("id", 0); };
	if (name == "PlayerList")
	{
		// the roster: merged with what already came (the others' looks and gear arrive around it), the absent dropped
		std::map<uint16_t, RemotePlayer> kept;
		if (const LValue* list = b.get("players"))
			for (int i = 1; i <= list->arraySize(); i++)
			{
				const LValue* e = list->at(i);
				uint16_t id = (uint16_t)e->numberAt("id");
				if (id == (uint16_t)playerId)
					continue;
				RemotePlayer r = players.count(id) ? players[id] : RemotePlayer();
				r.id = id;
				r.name = e->stringAt("name");
				kept[id] = r;
			}
		players.swap(kept);
	}
	else if (name == "PlayerJoinWorld")
	{
		uint16_t id = idOf();
		if (id != (uint16_t)playerId)
		{
			players[id].id = id;
			players[id].name = b.stringAt("name");
			addChat("server", "", b.stringAt("name") + " joined");
		}
	}
	else if (name == "PlayerLeaveWorld")
	{
		auto it = players.find(idOf());
		if (it != players.end())
		{
			addChat("server", "", it->second.name + " left");
			players.erase(it);
		}
	}
	else if (name == "PlayerLeaveView")
	{
		auto it = players.find(idOf());
		if (it != players.end())
		{
			it->second.inView = false;
			it->second.lastAt = it->second.prevAt = -1;
		}
	}
	else if (name == "PlayerAppearance")
	{
		uint16_t id = idOf();
		if (id == (uint16_t)playerId)
			return;
		RemotePlayer& r = players[id];
		r.id = id;
		r.race = lowerCase(b.stringAt("race"));
		r.head = lowerCase(b.stringAt("head"));
		r.hair = lowerCase(b.stringAt("hair"));
		r.cls = b.stringAt("class");
		r.male = b.boolAt("isMale", true);
		r.werewolf = b.boolAt("isWerewolf", false);
		if (!b.stringAt("name").empty())
			r.name = b.stringAt("name");
		r.appearanceKnown = true;
		r.lookRevision++;
	}
	else if (name == "PlayerEquipment")
	{
		uint16_t id = idOf();
		if (id == (uint16_t)playerId)
			return;
		RemotePlayer& r = players[id];
		r.id = id;
		r.equipment.clear();
		if (const LValue* slots = b.get("slots"))
			for (auto& kv : slots->table)
				if (kv.second.type == LValue::STRING)
					r.equipment.push_back(lowerCase(kv.second.str));
		r.lookRevision++;
	}
	else if (name == "PlayerCellChange")
	{
		uint16_t id = idOf();
		if (id != (uint16_t)playerId && players.count(id))
		{
			RemotePlayer& r = players[id];
			r.cellKey = b.stringAt("cellKey");
			r.lastAt = r.prevAt = -1;          // (a door or a spell: no sliding from where they were)
		}
	}
	else if (name == "PlayerStatsDynamic")
	{
		uint16_t id = idOf();
		auto it = players.find(id);
		if (it != players.end())
			if (const LValue* hp = b.get("hp"))
			{
				it->second.hp = (float)hp->numberAt("c", it->second.hp);
				it->second.hpMax = (float)hp->numberAt("b", it->second.hpMax);
			}
	}
	else if (name == "ChatMessage")
		addChat(b.stringAt("channel", "say"), b.stringAt("from"), b.stringAt("text"));
	else if (name == "SocialResult")
	{
		if (!b.boolAt("ok", true))
			addChat("server", "", b.stringAt("op") + ": " + b.stringAt("detail"));
		events.push_back({ name, b });
	}
	else
	{
		// for the game layer (InviteAccepted, StateRefused, FriendList, JoinFriend, WorldMode ...); a few dozen at most
		events.push_back({ name, b });
		while (events.size() > 64)
			events.pop_front();
	}
	(void)jsonString;
}
