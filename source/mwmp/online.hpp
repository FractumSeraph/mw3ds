#pragma once

// The online mode (FractumSeraph/mw3ds): the 3DS as a player on an openmw-web multiplayer server, speaking its wire
// protocol (openmw-web server/PROTOCOL.md, omw-mp.2, proto 3).
//
// Two kinds of server address:
//   https://host           an openmw-web platform (its gateway): sign in with the account's password (/auth/password),
//                          pick or make a character (/auth/characters), its world (POST /worlds) is dialled at
//                          wss://host/w/<world> and joined with a login ticket (/auth/ticket)
//   ws[s]://host[:port]/ws one world server alone: the password goes straight in SessionLoginRequest
// The client never simulates the world's NPCs (SessionHello without simulatesActors: the server's sim peer does), so a
// 3DS can't freeze a cell for everyone.
//
// Driven once a frame by update(), from the title screens (signing in) and the game (playing): nothing waits on the
// network.
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "mwmp/lser.hpp"
#include "mwmp/websocket.hpp"

struct cJSON;

// sdmc:/3ds/mw3ds/online.json: the server and account last signed in with (never the password)
struct OnlineConfig
{
	std::string server = "https://morrowind.fractumseraph.net";
	std::string account;
	void load();
	void save() const;
};

struct OnlineCharacter
{
	std::string id, name;
	int level = 1;
	bool needsChargen = false;
};

// A pose as the wire carries it (PlayerMove, 20 bytes)
struct NetPose
{
	float x = 0, y = 0, z = 0;
	float yaw = 0, pitch = 0;            // radians
	uint8_t flags = 0;                   // bit0 run, 1 sneak, 2 jump edge, 3 in air, 4 weapon drawn, 5 spell ready
	float animVel = 0;                   // 0..2 x base walk speed
};

// Another player in this world (the roster, PlayerList / PlayerJoinWorld), and what we know of them
struct RemotePlayer
{
	uint16_t id = 0;
	std::string name;
	bool appearanceKnown = false;
	std::string race, head, hair, cls;
	bool male = true;
	bool werewolf = false;               // PlayerAppearance isWerewolf: drawn in the beast's form
	std::vector<std::string> equipment;  // worn ids (lowercase), from PlayerEquipment
	std::string cellKey;
	bool inView = false;                 // poses arriving (PlayerMoveBatch); false after PlayerLeaveView
	// The last two poses and when they came (World time, seconds): drawn between them a little behind
	NetPose prev, last;
	float prevAt = -1, lastAt = -1;
	int lookRevision = 0;                // bumped when appearance or equipment changed (the body is redressed)
	float hp = -1, hpMax = -1;
};

// One of the world's people or creatures as the server streams it (PROTOCOL.md M4): the cell's holder (on a platform,
// always its sim peer) simulates it, everyone else draws it where the stream says
struct NetActor
{
	NetPose prev, last;                  // the last two poses (ActorMoveBatch) and when they came (Online::now)
	float prevAt = -1, lastAt = -1;
	float hp = -1, hpMax = -1;           // ActorStatsDynamic (-1: not heard)
	bool dead = false;                   // ActorDeath (true) / ActorRevive (false), as the world has it
	int combat = 0;                      // ActorAI: the player id it fights, 0 none
};

// A reference number as the wire carries it (RefNum: content file index, index): one 64-bit key
inline uint64_t netRefKey(int32_t file, uint32_t index) { return ((uint64_t)(uint32_t)file << 32) | index; }

struct ChatLine
{
	std::string channel, from, text;
};

class Online
{
public:
	enum Phase
	{
		IDLE,
		SIGNING_IN,          // POST /auth/password
		PROFILE_NEEDED,      // the server wants a username (and an email): the screen asks, then setProfile
		CHARACTERS,          // the list is in: chooseCharacter / createCharacter
		OPENING_WORLD,       // POST /worlds, /auth/ticket
		CONNECTING,          // the WebSocket and its TLS
		SESSION,             // Hello, login: waiting for the Welcome
		WELCOMED,            // in: the game may load (playerRecord says what), then ready()
		IN_WORLD,            // SessionReady sent: playing
		FAILED,              // error() says why; signIn again
	};

	OnlineConfig config;
	Phase phase = IDLE;
	std::string status;                                   // a line for the screen: what is going on now
	const std::string& error() const { return err; }

	// ---- signing in (the title's online screens)
	void signIn(const std::string& server, const std::string& account, const std::string& password);
	void setProfile(const std::string& username, const std::string& email);
	std::vector<OnlineCharacter> characters;
	void chooseCharacter(const std::string& id);
	void createCharacter();
	void cancel();                                        // back to IDLE, everything closed
	void update(float dt);                                // every frame, title or game

	// ---- what the server said at the welcome
	int playerId = -1;
	std::string displayName, characterId, motd, worldName;
	bool newCharacter = false;                            // playerRecord null: Morrowind's character creation first
	cJSON* playerRecord = nullptr;                        // the stored character (owned here), else null
	bool pvp = false, clientAuthority = true, simulated = false;

	// ---- playing
	void ready(const std::string& cellKey, const float pos[3]);   // SessionReady and the first PlayerCellChange
	void sendEvent(const std::string& name, const LValue& body);
	void sendMove(const NetPose& p);
	void sendCellChange(const std::string& cellKey, const float pos[3]);
	void sendChat(const std::string& text);
	// Another world of this platform (a friend's party: the JoinFriend answer's wsPath; home: this character's own):
	// PlayerLeaving here, a fresh ticket, and the new world's welcome. The loaded game carries on; ready() again after
	void switchWorld(const std::string& wsPath);
	void goHome();
	bool switching = false;                               // between worlds: the game keeps going, nobody sees us
	bool rejoined = false;                                // welcomed by a second world (the record is not put on again)
	std::map<uint16_t, RemotePlayer> players;
	// ---- the world's people and creatures (M4): each cell's holder (ActorAuthorityInfo: a player id, the sim peer's on
	// a platform) and what the stream says of each actor, by netRefKey. A cell nobody holds is simulated here, as
	// offline (the server's degraded mode)
	std::map<std::string, uint16_t> holderOf;
	std::map<std::string, uint32_t> epochOf;
	std::map<uint64_t, NetActor> actors;
	// The content file index the wire uses for a file of this data's load order (0 Morrowind.esm, 1 Tribunal.esm ...):
	// its place in the session's content list, -1 when the list hasn't it
	int contentFileOf(int loadOrderIndex) const;
	int loadOrderOf(int contentFile) const;        // and back (-1: not one of this data's files)
	// ---- the input tier (0x0102): what the player does, for the server's copy of them to swing, block and ready
	void sendInput(float moveAxis, float sideAxis, float yaw, float pitch, uint8_t flags, int simMs);
	float selfStatsAt = -100;                     // when the server last reported our bars (SelfStats: its copy of us takes the blows)
	std::deque<ChatLine> chat;                            // the newest last, a few dozen kept
	int chatRevision = 0;
	// Events the game itself acts on (InviteAccepted, StateRefused ...): name and body, oldest first
	struct Event { std::string name; LValue body; };
	std::deque<Event> events;
	float now = 0;                                        // seconds since the first update (real time: pose timing)
	unsigned long long clockStart = 0;
	float rttMs = -1;
	std::string testSay;                                  // scripted runs: a line said once in the world
	std::string testJoin;                                 // ... a friend (username) whose request is accepted and world joined
	float testSayAt = 0;

	~Online();

private:
	enum Kind { GATEWAY, WORLD } kind = GATEWAY;
	std::string base;                                     // https://host[:port] (gateway) or the ws url (world)
	std::string account, password, locker, ticket, accountKey;
	std::string wsUrl, err;
	HttpRequest http;
	enum Step { NONE, LOGIN, PROFILE_GET, PROFILE_SET, CHARS, CHAR_NEW, WORLDS, TICKET } step = NONE;
	float retryAt = -1;
	int worldTries = 0;
	WebSocket ws;
	uint32_t seq = 0, inputSeq = 0;
	bool helloSent = false, manifestRetried = false;
	std::vector<std::string> manifest;
	float pingAt = 0;
	void fail(const std::string& why);
	void request(Step s, const std::string& method, const std::string& path, const std::string& body);
	void onHttp();
	void openWorld();
	void sendJson(cJSON* obj);                            // (deleted after)
	void onText(const std::string& text);
	void onBinary(const std::string& data);
	void onEvent(const std::string& name, const LValue& body);
	void defaultManifest();
	void addChat(const std::string& channel, const std::string& from, const std::string& text);
};

extern Online* g_online;                                  // while the online mode is on (else null)

// The data's content files as the server names them, in load order (morrowind.esm, tribunal.esm, bloodmoon.esm)
void onlineSetContent(const std::vector<std::string>& esmLowerNames);
// A cellKey ("x,y" outdoors, the lowercase name inside) as the level cell Session::start takes (mwmp/mpgame.cpp)
std::string onlineStartCell(const std::string& cellKey);
