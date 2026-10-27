// The online mode in the game (FractumSeraph/mw3ds): the stored character put on the player, the player's state told
// to the server as openmw-web's own client tells it (scripts/mp/identity.lua: the same events, cadences and shapes),
// the other players drawn with the player's body builder, chat and the online screen.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <cstdlib>
#include <3ds.h>

#include "components/debug/debuglog.hpp"
#include "components/interpreter/scriptfunctions.hpp"
#include "extern/cjson/cJSON.h"
#include "mwbase/session.hpp"
#include "mwgui/widgets.hpp"
#include "mwmechanics/combathelpers.hpp"
#include "mwmp/online.hpp"
#include "mwrender/npcanimation.hpp"
#include "mwworld/worldhelpers.hpp"
#include "components/interpreter/interpreterhelpers.hpp"

// What the online mode watches of a placed object (PROTOCOL.md M3): taken, a swing door open, its lock (0 open), shown
struct ObjSeen
{
	bool taken = false, open = false, enabled = true;
	int lock = 0;
};

struct OnlineGame
{
	std::map<uint16_t, PlayerBody> bodies;
	std::map<uint16_t, int> bodyRevision;
	bool readySent = false, chargenSaid = false;
	int lastCell = -2;
	std::string cellKey;
	NetPose lastPose;
	float moveAt = 0, lastMoveSent = -1;
	bool wasMoving = false, wasAir = false;
	float appearanceAt = 0, equipmentAt = 0, dynamicAt = 0, progressAt = 0, inventoryAt = 0, spellsAt = 0;
	std::string appearanceSaid, equipmentSaid, dynamicSaid, attributesSaid, skillsSaid, levelSaid, inventorySaid;
	std::vector<std::string> spellsSaid;
	bool spellsSeeded = false;
	int chatRevisionShown = 0;
	int tab = 0;                         // the online screen: 0 players, 1 chat, 2 friends
	UiList list;
	LValue friends;                      // the last FriendList
	bool party = false;
	// the world's people the server simulates: each cell's key (by World::cells index), the wire's content file index
	// for each file of the load order, and per puppet the pose flags last acted on
	std::vector<std::string> cellKeys;
	std::vector<int> contentFiles;
	std::map<uint64_t, uint8_t> puppetFlags;   // by wire key
	float inputAt = 0;                   // the next PlayerInput
	float lastInputAt = -1;
	float selfHp = -1;                   // the health SelfStats last set
	// Quests and the rest of the shared story (PROTOCOL.md M6), as last told or heard: what goes out is what changed
	// since (openmw-web quests.lua's diff caches, which are its echo guard too)
	bool questSeeded = false;
	float questAt = 0;
	std::map<std::string, int> journalSaid;
	std::set<std::string> topicsSaid;
	std::map<std::string, float> globalsSaid;
	std::map<std::string, std::string> factionsSaid;
	int bountySaid = -1;
	// The clock (M7): adopted from the server, and our rests and waits asked of it
	// The world's objects: each as last seen or told (by wire key), the cells' states waiting for their objects
	std::map<uint64_t, ObjSeen> objSeen;
	std::map<std::string, LValue> cellStates;
	bool objectsHidden = false;          // something was hidden or shown: World::refreshHidden
	float objectsAt = 0;
	int opId = 0;
	// The container open here (a level reference; -1 none), its contents as last told, and our operations on each
	// container the server has not yet echoed (ContainerUpdate comes back to us too): wire key -> item -> count
	int contRef = -1;
	float contAt = 0;
	std::map<std::string, int> contSaid;
	std::map<uint64_t, std::map<std::string, int>> contPending;
	struct ContOp { uint64_t key; std::string id; int dn; int ref; };
	std::map<int, ContOp> contOps;       // by opId, until the server answers
	// The player's spells on the server's people this frame (PROTOCOL.md M5 CombatSpellHit), by target: sent whole
	struct SpellHit { std::string spellId; LValue effects = LValue::newTable(); LValue indexes = LValue::newTable(); };
	std::map<int, SpellHit> spellHits;
	// Things dropped: the server's net id -> the slot here (what it is, how many, its cell); our drops waiting for their
	// id; takes waiting for an answer (what leaves the pack again if the server says no)
	struct NetObj
	{
		int slot = -1;
		std::string cellKey, id;
		int count = 1;
		NetObj() {}
		NetObj(int s, const std::string& c) : slot(s), cellKey(c) {}
	};
	std::map<uint32_t, NetObj> netObjs;
	std::map<uint32_t, int> netActors;   // the people the server's world spawned: net id -> their slot here
	std::map<int, NetObj> pendingDrops;
	int tempSeq = 0;
	struct TakeOp { std::string id; int count; };
	std::map<int, TakeOp> takeOps;
	// The person we are talking to (DialogueLock asked for), and the sky: the region we said we are in, each region's
	// weather holder, what we last said of ours
	int talkRef = -1;
	float skyAt = 0, weatherSaidAt = -100;
	std::string regionSaid, weatherSaid;
	std::map<std::string, uint16_t> weatherHolder;
	// The server's people following us (claimed), and the merchant whose purse is open (its gold as last told)
	float followAt = 0;
	std::set<int> followers, travelSaid;
	int purseRef = -1, purseSaid = 0;
	bool purseOpened = false;
	// Our own body: death told when, the pack's counts and the Mark as last told, the magic we said is on us (key ->
	// spell), and what shows on the other players (id -> cast key -> effects)
	float deathSaidAt = -100, selfAt = 0, markAt = 0, activeAt = 0, diedAt = -1;
	std::map<std::string, int> countsSaid;
	bool countsSeeded = false;
	std::string markSaid;
	std::map<std::string, std::string> activeSaid;
	std::map<uint16_t, std::map<std::string, std::vector<int>>> playerMagic;
	// Scripts: the objects whose variables we watch (until when) and their values as told; the global scripts running
	std::map<int, float> memberWatch;
	std::map<int, std::vector<float>> memberSaid;
	float memberAt = 0, globalScriptsAt = 0;
	std::set<std::string> globalScriptsSaid;
	bool globalScriptsSeeded = false;
	// Records we made, told to the server (tempId -> our id) or refused by it
	float recordsAt = 0;
	std::map<int, std::string> recordPending;
	std::set<std::string> recordRefused;
	// The outdoor cells we said we explored; the server's last correction of where we stand, as seen
	std::set<std::string> exploredSaid;
	bool exploredSeeded = false;
	float correctionSeen = 0;
	std::string reportName;              // the player a report is being written about
	bool isOwner = false;                // this world is ours (WorldMode isOwner): we may send guests home
	int presence = 0;                    // who sees us: 0 everyone, 1 friends, 2 nobody (private), 3 appearing offline
	bool wasInWorld = false;             // played in a world this session: a lost connection is tried again
	bool worldCleared = false;           // between worlds: the last one's state already forgotten
	float pruneAt = 60;
	std::set<std::string> builtRecords;  // records built here from RecordsSync (gone with their world)
	float reconnectAt = 0, reconnectDelay = 5;
	// A friend's world's journal while we visit it (JournalSync borrowed): our own, set aside until we are home
	bool journalBorrowed = false;
	std::unordered_map<std::string, int> ownJournalIndex;
	std::vector<JournalLine> ownJournal;
	std::unordered_set<std::string> ownTopics;
	float dynamicHp = -1, dynamicFt = -1;   // the bars as last told (the local gain, d, is counted from them)
	bool clockAdopted = false;
	float clockHour = -1;                // gameHour after the last frame (a jump past it is a rest or a wait)
};

namespace
{
const float kTau = 6.28318530718f;

std::string lowerCase(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z')
			c += 32;
	return s;
}

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

// The equipment slots of OpenMW's Lua (types.Actor.EQUIPMENT_SLOT, PROTOCOL.md PlayerEquipment): helmet 0, cuirass 1,
// greaves 2, left pauldron 3, right pauldron 4, left gauntlet 5, right gauntlet 6, boots 7, shirt 8, pants 9, skirt 10,
// robe 11, left ring 12, right ring 13, amulet 14, belt 15, carried right (weapon) 16, carried left (shield, torch) 17,
// ammunition 18
int slotOf(const Object* o, int& ringsSeen, bool& gloveLeftUsed)
{
	if (!o)
		return -1;
	switch (o->recType)
	{
	case REC_WEAP:
		return o->subtype >= WEAP_ARROW ? 18 : 16;
	case REC_LIGH:
		return 17;
	case REC_LOCK: case REC_PROB:
		return 16;
	case REC_ARMO:
	{
		static const int kArmor[11] = { 0, 1, 3, 4, 2, 7, 5, 6, 17, 5, 6 };   // ARMO type -> slot (gloves by hand)
		return o->subtype >= 0 && o->subtype < 11 ? kArmor[o->subtype] : -1;
	}
	case REC_CLOT:
	{
		switch (o->subtype)
		{
		case 0: return 9;           // pants
		case 1: return 7;           // shoes
		case 2: return 8;           // shirt
		case 3: return 15;          // belt
		case 4: return 11;          // robe
		case 5: return 6;           // right glove
		case 6: return 5;           // left glove
		case 7: return 10;          // skirt
		case 8: return ringsSeen++ == 0 ? 12 : 13;    // rings: left, then right
		case 9: return 14;          // amulet
		default: return -1;
		}
	}
	default:
		return -1;
	}
	(void)gloveLeftUsed;
}

// A reference the server's world spawned (a summons, a leveled creature): its content file on the wire is -2 and its
// index the server's net id (openmw-web mwmp/luabindings.cpp); a Ref here keeps the same pair in rnFile / rnIndex
const int kNetActorFile = -2;

// A record's id on the wire and back (our own made ones travel by the server's id: onlineRecords)
std::string netIdOf(const std::string& local);
std::string localIdOf(const std::string& net);

std::string cellKeyAt(const World& w, int cell)
{
	if (cell < 0 || cell >= (int)w.cells.size())
		return "";
	const LevelCell& c = w.cells[cell];
	if (!c.interior)
	{
		char k[32];
		snprintf(k, sizeof(k), "%d,%d", c.gx, c.gy);
		return k;
	}
	return lowerCase(c.name.str());
}

std::string cellKeyOf(const World& w)
{
	return cellKeyAt(w, w.current);
}

// A cellKey back to one of the level's cells (-1: not in this data)
int cellOfKey(const World& w, const std::string& key)
{
	int gx, gy;
	if (sscanf(key.c_str(), "%d,%d", &gx, &gy) == 2 && key.find_first_not_of("-0123456789,") == std::string::npos)
		return w.gridCell(gx, gy);
	return w.cellIndex(key);
}

std::string describe(const LValue& v)
{
	return lserEncode(v);                 // (the encoded bytes: a cheap, exact "did it change?")
}
}

// The level cell's file for a cellKey, for Session::start before anything is loaded: exteriors are ext_<x>_<y> (m for
// minus), interiors their name
std::string onlineStartCell(const std::string& key)
{
	int gx, gy;
	if (sscanf(key.c_str(), "%d,%d", &gx, &gy) == 2 && key.find_first_not_of("-0123456789,") == std::string::npos)
	{
		auto part = [](int v) { return v < 0 ? "m" + std::to_string(-v) : std::to_string(v); };
		return "ext_" + part(gx) + "_" + part(gy);
	}
	return key;
}

// ---- the stored character put on the player (openmw-web identity.lua applyRecord, both phases at once here)

void Session::onlineApplyRecord()
{
	if (!g_online || !g_online->playerRecord)
		return;
	if (!og)
		og = new OnlineGame();
	const cJSON* rec = g_online->playerRecord;
	const cJSON* app = cJSON_GetObjectItem(rec, "appearance");
	if (app)
	{
		if (*str(app, "race"))
			w.stats.race = lowerCase(str(app, "race"));
		w.stats.female = !cJSON_IsTrue(cJSON_GetObjectItem(app, "isMale"));
		w.stats.head = lowerCase(str(app, "head"));
		w.stats.hair = lowerCase(str(app, "hair"));
		if (*str(app, "name"))
			w.stats.name = str(app, "name");
		if (*str(app, "class"))
			w.stats.cls = str(app, "class");                  // (record ids as the record has them: "Warrior")
		if (*str(app, "birthsign"))
			w.stats.birthsign = str(app, "birthsign");
		w.werewolf = cJSON_IsTrue(cJSON_GetObjectItem(app, "isWerewolf"));
	}
	const cJSON* spells = cJSON_GetObjectItem(rec, "spells");
	if (cJSON_GetArraySize(spells) > 0)
	{
		// the saved set replaces what a fresh character has (it holds the race's and sign's own too)
		w.stats.spells.clear();
		const cJSON* s;
		cJSON_ArrayForEach(s, spells)
			if (cJSON_IsString(s))
			{
				std::string id = lowerCase(s->valuestring);
				const RaceDef* rd = w.race();
				const BirthDef* bd = w.birthsign();
				bool implied = (rd && std::find(rd->spells.begin(), rd->spells.end(), id) != rd->spells.end())
					|| (bd && std::find(bd->spells.begin(), bd->spells.end(), id) != bd->spells.end());
				if (!implied)
					w.stats.spells.push_back(id);
			}
	}
	w.recomputeStats();
	// Stats: the bases the server keeps (identity.lua snapProgression: .base), as the level-ups and training on top of
	// what race and class gave (recomputeStats' attrBonus / skillBonus)
	const cJSON* st = cJSON_GetObjectItem(rec, "stats");
	if (st)
	{
		if (cJSON_IsNumber(cJSON_GetObjectItem(st, "level")))
			w.stats.level = std::max(1, (int)num(st, "level"));
		if (cJSON_IsNumber(cJSON_GetObjectItem(st, "reputation")))
			w.pcReputation = (int)num(st, "reputation");
		const cJSON* at = cJSON_GetObjectItem(st, "attributes");
		for (int i = 0; i < 8; i++)
		{
			const cJSON* v = cJSON_GetObjectItem(at, kAttrFuncs[i]);
			if (cJSON_IsNumber(v))
				w.stats.attrBonus[i] = (int)v->valuedouble - w.stats.attrCreation[i];
			const cJSON* d = cJSON_GetObjectItem(at, (std::string(kAttrFuncs[i]) + "_damage").c_str());
			w.stats.attrDamage[i] = cJSON_IsNumber(d) ? (float)d->valuedouble : 0.0f;
		}
		const cJSON* sk = cJSON_GetObjectItem(st, "skills");
		for (int k = 0; k < 27; k++)
		{
			const cJSON* v = cJSON_GetObjectItem(sk, kSkillFuncs[k]);
			if (cJSON_IsNumber(v))
				w.stats.skillBonus[k] = (int)v->valuedouble - w.stats.skillCreation[k];
		}
		w.recomputeStats();
		const cJSON* dyn = cJSON_GetObjectItem(st, "dynamic");
		if (const cJSON* hp = cJSON_GetObjectItem(dyn, "hp"))
		{
			float base = (float)num(hp, "b", w.stats.healthMax);
			w.stats.healthBonus += base - w.stats.healthMax;
			w.stats.healthMax = base;
			float c = (float)num(hp, "c", base);
			// never restored dead: back at a sliver of health where they fell (identity.lua)
			w.stats.health = c > 0.0f ? c : fmaxf(1.0f, floorf(base * 0.1f));
		}
		if (const cJSON* mp = cJSON_GetObjectItem(dyn, "mp"))
			w.stats.magicka = fminf((float)num(mp, "c", w.stats.magicka), w.stats.magickaMax);
		if (const cJSON* ft = cJSON_GetObjectItem(dyn, "ft"))
			w.stats.fatigue = fminf((float)num(ft, "c", w.stats.fatigue), w.stats.fatigueMax);
	}
	// The pack: the stored items replace the new game's, then the stored equipment is put on
	const cJSON* inv = cJSON_GetObjectItem(rec, "inventory");
	if (cJSON_IsArray(inv))
	{
		w.inventory.clear();
		const cJSON* e;
		cJSON_ArrayForEach(e, inv)
		{
			std::string id = lowerCase(str(e, "id"));
			int n = (int)num(e, "n", 1);
			if (!id.empty() && n > 0 && w.game.object(id))
				w.addItem(id, n);
			else if (!id.empty())
				logf("online: the record's item %s isn't in this data", id.c_str());
		}
	}
	const cJSON* eq = cJSON_GetObjectItem(rec, "equipment");
	if (cJSON_IsObject(eq))
	{
		const cJSON* e;
		cJSON_ArrayForEach(e, eq)
			if (cJSON_IsString(e))
			{
				std::string id = lowerCase(e->valuestring);
				for (size_t i = 0; i < w.inventory.size(); i++)
					if (w.inventory[i].id == id && !w.inventory[i].equipped)
					{
						equipItem((int)i, true);
						break;
					}
			}
	}
	if (w.werewolf)
		w.recomputeKeepingBars();
	// Quests, topics, globals, factions, bounty
	const cJSON* j = cJSON_GetObjectItem(rec, "journal");
	if (cJSON_IsObject(j))
	{
		const cJSON* e;
		cJSON_ArrayForEach(e, j)
			if (cJSON_IsNumber(e))
				w.journalAdd(lowerCase(e->string), (int)e->valuedouble);
	}
	const cJSON* topics = cJSON_GetObjectItem(rec, "topics");
	{
		const cJSON* e;
		cJSON_ArrayForEach(e, topics)
			if (cJSON_IsString(e))
				w.knownTopics.insert(lowerCase(e->valuestring));
	}
	const cJSON* globals = cJSON_GetObjectItem(rec, "globals");
	{
		const cJSON* e;
		cJSON_ArrayForEach(e, globals)
			if (cJSON_IsNumber(e))
				w.globals[lowerCase(e->string)] = (float)e->valuedouble;
	}
	const cJSON* factions = cJSON_GetObjectItem(rec, "factions");
	{
		const cJSON* e;
		cJSON_ArrayForEach(e, factions)
		{
			std::string f = lowerCase(e->string);
			w.pcRank[f] = (int)num(e, "rank", 0);
			if (cJSON_IsNumber(cJSON_GetObjectItem(e, "reputation")))
				w.pcFacRep[f] = (int)num(e, "reputation");
			if (cJSON_IsTrue(cJSON_GetObjectItem(e, "expelled")))
				w.pcExpelled.insert(f);
		}
	}
	if (cJSON_IsNumber(cJSON_GetObjectItem(rec, "bounty")))
	{
		w.bounty = (int)num(rec, "bounty");
		w.globals["pccrimelevel"] = (float)w.bounty;
	}
	// Where they were in this world
	const cJSON* pos = cJSON_GetObjectItem(rec, "position");
	if (pos && *str(pos, "cellKey"))
	{
		int cell = cellOfKey(w, lowerCase(str(pos, "cellKey")));
		float p[3] = { (float)num(pos, "x"), (float)num(pos, "y"), (float)num(pos, "z") };
		if (cell >= 0)
			teleportPlayer(cell, p, w.player.yaw);
		else
			logf("online: the record's cell %s isn't in this data: starting where the game does", str(pos, "cellKey"));
	}
	logf("online: the character %s restored (%s %s, level %d, %d items)", w.stats.name.c_str(), w.stats.race.c_str(),
		w.stats.female ? "female" : "male", w.stats.level, (int)w.inventory.size());
}

// ---- what the server is told, as identity.lua tells it

namespace
{
LValue appearanceOf(const World& w)
{
	LValue a = LValue::newTable();
	a.set("race", w.stats.race);
	// ("" is the race's first head / hair here: the server wants the body part's id, as OpenMW's record has it)
	auto part = [&](bool hair) {
		const std::string& id = hair ? w.stats.hair : w.stats.head;
		if (!id.empty())
			return id;
		const auto* list = playerHeadChoices(w, hair);
		return list ? list->front().first : std::string();
	};
	a.set("head", part(false));
	a.set("hair", part(true));
	a.set("isMale", LValue::boolean(!w.stats.female));
	a.set("class", w.stats.cls);
	a.set("name", w.stats.name);
	if (!w.stats.birthsign.empty())
		a.set("birthsign", w.stats.birthsign);
	if (w.werewolf)
		a.set("isWerewolf", LValue::boolean(true));
	return a;
}

LValue equipmentOf(const World& w)
{
	LValue slots = LValue::newTable();
	int rings = 0;
	bool glove = false;
	for (auto& it : w.inventory)
		if (it.equipped)
		{
			int slot = slotOf(w.game.object(it), rings, glove);
			if (slot >= 0)
			{
				LValue key((double)slot);
				bool taken = false;
				for (auto& kv : slots.table)
					taken = taken || kv.first.num == (double)slot;
				if (!taken)
					slots.table.emplace_back(key, LValue(netIdOf(it.id)));
			}
		}
	LValue b = LValue::newTable();
	b.set("slots", slots);
	return b;
}
}

void Session::onlineTick(float dt)
{
	if (!g_online)
		return;
	if (!og)
		og = new OnlineGame();
	Online& on = *g_online;
	// The PC test build steps its frames as fast as it can: MW3DS_ONLINE_REALTIME=1 holds them to 30 a second, so a
	// scripted run meets the other players in real time (the console is real time already, and has no such variable)
	static const bool realtime = getenv("MW3DS_ONLINE_REALTIME") != nullptr;
	if (realtime)
	{
		static u64 last = 0;
		u64 ms = osGetTime();
		if (last && ms - last < 33)
			svcSleepThread((s64)(33 - (ms - last)) * 1000000);
		last = osGetTime();
	}
	on.update(dt);
	// (out of the world: nothing of ours waits for it, and placing people is ours again)
	if (on.phase != Online::IN_WORLD)
	{
		w.actorsElsewhere = false;
		w.playerDrops.clear();
		w.playerTakes.clear();
		w.spawnAsks.clear();
	}
	if (on.phase == Online::FAILED)
	{
		if (!og->appearanceSaid.empty() || og->readySent)
		{
			notify(on.error() + " (reconnecting)");
			og->readySent = false;
			og->appearanceSaid.clear();
			for (auto& b : og->bodies)
				b.second.free(w);
			og->bodies.clear();
			og->wasInWorld = true;
			og->reconnectAt = on.now + 5.0f;
			og->reconnectDelay = 5.0f;
		}
		// back in by itself: after 5 s, then less and less often (to a minute), while the game plays on here; the
		// world's people are ours to run meanwhile (no holder is known)
		on.holderOf.clear();
		if (og->wasInWorld && on.now >= og->reconnectAt && on.reconnect())
		{
			og->reconnectDelay = fminf(60.0f, og->reconnectDelay * 2.0f);
			og->reconnectAt = on.now + og->reconnectDelay;
		}
		return;
	}
	// between worlds (a friend's, or home): ready again in the next one, with nothing of the last one's state
	if (on.switching && on.phase < Online::WELCOMED && !og->worldCleared)
	{
		og->worldCleared = true;
		onlineForgetWorld();
	}
	if (on.phase >= Online::WELCOMED)
		og->worldCleared = false;
	if (on.switching && on.phase < Online::WELCOMED)
	{
		og->readySent = false;
		og->appearanceSaid.clear();
		og->equipmentSaid.clear();
		og->dynamicSaid.clear();
		og->attributesSaid.clear();
		og->skillsSaid.clear();
		og->levelSaid.clear();
		og->inventorySaid.clear();
	}
	if (on.phase == Online::WELCOMED && !og->readySent && w.current >= 0)
	{
		og->cellKey = cellKeyOf(w);
		og->lastCell = w.current;
		on.ready(og->cellKey, w.player.feet);
		og->readySent = true;
		og->chargenSaid = !on.newCharacter;
	}
	if (on.phase != Online::IN_WORLD)
		return;
	float now = on.now;
	// a scripted run's line to say (online_test.json "say"), once the world has us
	if (!on.testSay.empty() && now > on.testSayAt)
	{
		on.sendChat(on.testSay);
		on.testSay.clear();
	}
	// The cell: a door, a spell, a step into the next exterior cell
	if (w.current != og->lastCell && w.current >= 0)
	{
		og->lastCell = w.current;
		og->cellKey = cellKeyOf(w);
		on.sendCellChange(og->cellKey, w.player.feet);
	}
	// Character creation done (CharGenState -1): the slot is adopted and saving starts (PROTOCOL.md ChargenComplete)
	if (!og->chargenSaid)
	{
		auto cg = w.globals.find("chargenstate");
		if (cg != w.globals.end() && cg->second == -1.0f)
		{
			og->appearanceSaid.clear();
			og->appearanceAt = 0;
			og->chargenSaid = true;
		}
	}
	// The pose: 15 a second while moving, and on each change of stride (a jump, stopping)
	NetPose p;
	p.x = w.player.feet[0];
	p.y = w.player.feet[1];
	p.z = w.player.feet[2];
	p.yaw = w.player.yaw;
	p.pitch = w.player.pitch;
	float dx = p.x - og->lastPose.x, dy = p.y - og->lastPose.y, dz = p.z - og->lastPose.z;
	float moved = sqrtf(dx * dx + dy * dy + dz * dz);
	bool air = !w.player.onGround && !w.player.swimming;
	float stepSpeed = og->lastMoveSent >= 0 && now > og->lastMoveSent ? sqrtf(dx * dx + dy * dy) / (now - og->lastMoveSent) : 0.0f;
	p.animVel = fminf(2.0f, stepSpeed / fmaxf(1.0f, w.player.runSpeed * 0.5f));
	p.flags = (stepSpeed > w.player.runSpeed * 0.6f ? 1 : 0) | (w.player.sneaking ? 2 : 0) | (air && !og->wasAir ? 4 : 0)
		| (air ? 8 : 0) | (weaponDrawn ? 16 : 0);
	bool turned = fabsf(p.yaw - og->lastPose.yaw) > 0.02f || fabsf(p.pitch - og->lastPose.pitch) > 0.05f;
	bool moving = moved > 0.5f || turned;
	bool edge = moving != og->wasMoving || air != og->wasAir || (p.flags & 16) != (og->lastPose.flags & 16);
	if ((moving && now - og->lastMoveSent >= 1.0f / 15.0f) || edge || now - og->lastMoveSent > 2.0f)
	{
		if (!moving)
			p.animVel = 0;
		on.sendMove(p);
		og->lastPose = p;
		og->lastMoveSent = now;
		og->wasMoving = moving;
		og->wasAir = air;
	}
	// The input tier, 30 a second while a holder (the sim peer) runs the world: aims the server's copy of us, holds and
	// lets go its attack as ours (so its blows land there, PROTOCOL.md Phase 4C), readies its weapon. Our own PlayerMove
	// still places it. Movement as the axes of how we went, against where we look
	if (!on.holderOf.empty() && now >= og->inputAt)
	{
		og->inputAt = now + 1.0f / 30.0f;
		float fwd = 0, side = 0;
		if (stepSpeed > 1.0f && moved > 0.0f)
		{
			float sy = sinf(p.yaw), cy = cosf(p.yaw), d = sqrtf(dx * dx + dy * dy);
			fwd = (dx * sy + dy * cy) / fmaxf(d, 1e-3f);
			side = (dx * cy - dy * sy) / fmaxf(d, 1e-3f);
		}
		uint8_t f = (p.flags & 1) | (w.player.sneaking ? 2 : 0) | (air && !og->wasAir ? 4 : 0)
			| (weaponDrawn && attackCharge >= 0.0f ? 8 : 0) | (weaponDrawn ? 16 : 0);
		int ms = og->lastInputAt < 0 ? 33 : (int)((now - og->lastInputAt) * 1000.0f);
		on.sendInput(fwd, side, p.yaw, p.pitch, f, ms);
		og->lastInputAt = now;
	}
	// Appearance (1 s), equipment (0.5 s), the bars (0.25 s), attributes / skills / level (1 s), the pack (2 s),
	// spells (1 s): each sent when it changed
	if (now >= og->appearanceAt && og->chargenSaid)
	{
		og->appearanceAt = now + 1.0f;
		LValue a = appearanceOf(w);
		std::string s = describe(a);
		if (s != og->appearanceSaid)
		{
			bool first = og->appearanceSaid.empty();
			og->appearanceSaid = s;
			on.sendEvent("PlayerAppearance", a);
			if (first && on.newCharacter)
			{
				on.sendEvent("ChargenComplete", LValue::newTable());
				on.newCharacter = false;
				logf("online: character creation done: ChargenComplete");
			}
		}
	}
	if (now >= og->equipmentAt)
	{
		og->equipmentAt = now + 0.5f;
		LValue e = equipmentOf(w);
		std::string s = describe(e);
		if (s != og->equipmentSaid)
		{
			og->equipmentSaid = s;
			on.sendEvent("PlayerEquipment", e);
		}
	}
	if (now >= og->dynamicAt)
	{
		og->dynamicAt = now + 0.25f;
		auto bar = [](float c, float b) {
			LValue v = LValue::newTable();
			v.set("c", (double)floorf(c + 0.5f));
			v.set("b", (double)floorf(b + 0.5f));
			return v;
		};
		LValue d = LValue::newTable();
		LValue hpBar = bar(w.stats.health, w.stats.healthMax), ftBar = bar(w.stats.fatigue, w.stats.fatigueMax);
		// (a gain here since the last claim: the server adds it to the bar it holds, which the world may have lowered)
		if (og->dynamicHp >= 0 && w.stats.health > og->dynamicHp + 0.5f)
			hpBar.set("d", (double)floorf(w.stats.health - og->dynamicHp + 0.5f));
		if (og->dynamicFt >= 0 && w.stats.fatigue > og->dynamicFt + 0.5f)
			ftBar.set("d", (double)floorf(w.stats.fatigue - og->dynamicFt + 0.5f));
		d.set("hp", hpBar);
		d.set("mp", bar(fmaxf(0.0f, w.stats.magicka), w.stats.magickaMax));
		d.set("ft", ftBar);
		std::string s = describe(d);
		if (s != og->dynamicSaid)
		{
			og->dynamicSaid = s;
			on.sendEvent("PlayerStatsDynamic", d);
			og->dynamicHp = w.stats.health;
			og->dynamicFt = w.stats.fatigue;
		}
	}
	if (now >= og->progressAt && og->chargenSaid)
	{
		og->progressAt = now + 1.0f;
		LValue at = LValue::newTable(), sk = LValue::newTable(), lv = LValue::newTable();
		for (int i = 0; i < 8; i++)
		{
			at.set(kAttrFuncs[i], (double)std::min(100, w.stats.attrCreation[i] + w.stats.attrBonus[i]));
			at.set(std::string(kAttrFuncs[i]) + "_damage", (double)w.stats.attrDamage[i]);
		}
		for (int k = 0; k < 27; k++)
			sk.set(kSkillFuncs[k], (double)std::min(100, w.stats.skillCreation[k] + w.stats.skillBonus[k]));
		lv.set("level", (double)w.stats.level);
		lv.set("reputation", (double)std::max(0, std::min(255, w.pcReputation)));
		std::string sa = describe(at), ss = describe(sk), sl = describe(lv);
		if (sa != og->attributesSaid)
		{
			og->attributesSaid = sa;
			on.sendEvent("PlayerAttributes", at);
		}
		if (ss != og->skillsSaid)
		{
			og->skillsSaid = ss;
			on.sendEvent("PlayerSkills", sk);
		}
		if (sl != og->levelSaid)
		{
			og->levelSaid = sl;
			on.sendEvent("PlayerLevel", lv);
		}
	}
	if (now >= og->inventoryAt && og->chargenSaid)
	{
		og->inventoryAt = now + 2.0f;
		LValue items = LValue::newTable();
		std::map<std::string, int> counts;
		for (auto& it : w.inventory)
			counts[netIdOf(it.id)] += it.count;
		for (auto& c : counts)
		{
			LValue e = LValue::newTable();
			e.set("id", c.first);
			e.set("n", (double)c.second);
			items.push(e);
		}
		LValue b = LValue::newTable();
		b.set("items", items);
		std::string s = describe(b);
		if (s != og->inventorySaid)
		{
			og->inventorySaid = s;
			on.sendEvent("PlayerInventory", b);
		}
	}
	if (now >= og->spellsAt && og->chargenSaid)
	{
		og->spellsAt = now + 1.0f;
		std::vector<std::string> have;
		for (auto& id : w.stats.spells)
			have.push_back(netIdOf(id));
		std::sort(have.begin(), have.end());
		if (!og->spellsSeeded)
		{
			// the restored set is what the server already holds: only changes from here go out
			og->spellsSaid = on.playerRecord ? have : std::vector<std::string>();
			og->spellsSeeded = true;
		}
		if (have != og->spellsSaid)
		{
			LValue add = LValue::newTable(), remove = LValue::newTable();
			for (auto& id : have)
				if (!std::binary_search(og->spellsSaid.begin(), og->spellsSaid.end(), id))
					add.push(LValue(id));
			for (auto& id : og->spellsSaid)
				if (!std::binary_search(have.begin(), have.end(), id))
					remove.push(LValue(id));
			LValue b = LValue::newTable();
			b.set("add", add);
			b.set("remove", remove);
			on.sendEvent("PlayerSpellbook", b);
			og->spellsSaid = have;
		}
	}
	// What came in: chat on the top screen's notes, the social answers the online screen shows
	int fresh = std::min((int)on.chat.size(), on.chatRevision - og->chatRevisionShown);
	for (int i = (int)on.chat.size() - fresh; i < (int)on.chat.size(); i++)
		notify(on.chat[i].from.empty() ? on.chat[i].text : on.chat[i].from + ": " + on.chat[i].text);
	og->chatRevisionShown = on.chatRevision;
	while (!on.events.empty())
	{
		Online::Event e = std::move(on.events.front());
		on.events.pop_front();
		if (e.name == "FriendList")
		{
			og->friends = e.body;
			// a scripted run's friend: their request accepted, then their world joined (once)
			if (!on.testJoin.empty())
			{
				auto lowerName = [](std::string s) { for (char& ch : s) ch = (char)tolower((unsigned char)ch); return s; };
				std::string want = lowerName(on.testJoin);
				if (const LValue* req = e.body.get("requests"))
					for (int i = 1; i <= req->arraySize(); i++)
						if (lowerName(req->at(i)->stringAt("name")) == want)
						{
							LValue b = LValue::newTable();
							b.set("name", req->at(i)->stringAt("name"));
							on.sendEvent("FriendAccept", b);
						}
				if (const LValue* fr = e.body.get("friends"))
					for (int i = 1; i <= fr->arraySize(); i++)
						if (lowerName(fr->at(i)->stringAt("name")) == want && fr->at(i)->boolAt("online"))
						{
							LValue b = LValue::newTable();
							b.set("acct", fr->at(i)->stringAt("acct"));
							on.sendEvent("JoinFriend", b);
							on.testJoin.clear();
							logf("online test: joining %s", want.c_str());
							break;
						}
			}
		}
		else if (e.name == "WorldMode")
		{
			og->party = e.body.stringAt("mode") == "party";
			og->isOwner = e.body.boolAt("isOwner");
		}
		else if (e.name == "InviteAccepted")
		{
			int cell = cellOfKey(w, lowerCase(e.body.stringAt("cellKey")));
			float pos[3] = { (float)e.body.numberAt("x"), (float)e.body.numberAt("y"), (float)e.body.numberAt("z") };
			if (cell >= 0)
				teleportPlayer(cell, pos, w.player.yaw);
		}
		// JoinFriend's answer: their world's address (or why not); WorldClosed: the host went solo, home again
		else if (e.name == "JoinFriend")
		{
			if (e.body.boolAt("ok"))
			{
				notify("Joining " + e.body.stringAt("friendName") + "...");
				on.switchWorld(e.body.stringAt("wsPath"));
			}
			else
				notify("Can't join: " + e.body.stringAt("error"));
		}
		else if (e.name == "WorldClosed")
		{
			notify("The host closed their world: going home");
			on.goHome();
		}
		else if (e.name == "FriendRequestReceived")
			notify(e.body.stringAt("fromName") + " asks to be your friend (Online screen)");
		else if (e.name == "InviteReceived")
			notify(e.body.stringAt("fromName") + " invites you (Online screen)");
		else if (e.name == "StateRefused")
			logf("online: the server refused our %s", e.body.stringAt("kind").c_str());
		// The bars the server's copy of us has: it takes the world's blows (Phase 4A), we show them
		else if (e.name == "SelfStats")
		{
			auto bar = [&](const char* k, float& cur, float& base) {
				if (const LValue* v = e.body.get(k))
				{
					cur = (float)v->numberAt("c", cur);
					base = (float)v->numberAt("b", base);
				}
			};
			float hp = w.stats.health;
			bar("hp", w.stats.health, w.stats.healthMax);
			bar("mp", w.stats.magicka, w.stats.magickaMax);
			bar("ft", w.stats.fatigue, w.stats.fatigueMax);
			if (w.stats.health < hp - 0.5f && w.stats.health > 0.0f)
				playSound(-1, "Health Damage");
			og->dynamicSaid.clear();
			og->dynamicHp = w.stats.health;   // (the server's own figures: no gain of ours to add to them)
			og->dynamicFt = w.stats.fatigue;
			// dead here, alive there (a death the world didn't confirm): standing again
			if (playerDead && w.stats.health > 0.0f && on.now - og->diedAt > 2.0f)
			{
				playerDead = false;
				if (screen == SCR_DEATH)
					screen = SCR_NONE;
				logf("online: the world says we live (health %.0f)", w.stats.health);
			}
		}
		// A crime the server's copy of us did there (an assault on the world's people): its bounty here
		else if (e.name == "PlayerCrime")
		{
			int add = (int)e.body.numberAt("bounty");
			w.bounty = std::max(0, w.bounty + add);
			logf("online: a crime in the world (%s): bounty %+d, now %d", e.body.stringAt("kind").c_str(), add, w.bounty);
		}
		else if (onlineStoryEvent(e.name, e.body) || onlineDropEvent(e.name, e.body) || onlineObjectEvent(e.name, e.body)
			|| onlineContainerEvent(e.name, e.body) || onlineTalkAndSkyEvent(e.name, e.body) || onlineSelfEvent(e.name, e.body)
			|| onlineScriptEvent(e.name, e.body) || onlineActorEvent(e.name, e.body)
			|| onlineRecordEvent(e.name, e.body) || onlineCombatEvent(e.name, e.body))
			;
		else if (e.name == "WorldKillCount")
			w.deadCounts[lowerCase(e.body.stringAt("refId"))] = (int)e.body.numberAt("count");
	}
	onlinePuppets(now);
	onlineSpellHits();
	onlineStory(now);
	onlineObjects(now);
	onlineDrops();
	onlineContainers(now);
	onlineTalkAndSky(now);
	onlineFollowersAndPurses(now);
	onlineSelf(now);
	onlineScripts(now);
	onlineRecords(now);
	onlineWorldMisc(now);
	// The other players' bodies: dressed as they are, where they stand (a tenth of a second behind, between the last
	// two poses), walking or running as fast as they go
	for (auto it = og->bodies.begin(); it != og->bodies.end();)
	{
		auto rp = on.players.find(it->first);
		if (rp == on.players.end() || !rp->second.inView)
		{
			it->second.free(w);
			it = og->bodies.erase(it);
		}
		else
			++it;
	}
	for (auto& kv : on.players)
	{
		RemotePlayer& r = kv.second;
		if (!r.inView || r.lastAt < 0 || !r.appearanceKnown)
			continue;
		PlayerBody& b = og->bodies[r.id];
		BodyLook look;
		look.race = r.race;
		look.female = !r.male;
		look.head = r.head;
		look.hair = r.hair;
		for (auto& id : r.equipment)
			if (const Object* o = w.game.object(id))
			{
				if (o->recType == REC_CLOT || o->recType == REC_ARMO)
					look.worn.push_back(id);
				else if (o->recType == REC_WEAP && o->subtype < WEAP_ARROW)
					look.weapon = id;
			}
		if (r.werewolf)
			werewolfLook(look);
		bool weaponOut = r.last.flags & 16;
		if (og->bodyRevision[r.id] != r.lookRevision * 2 + (weaponOut ? 1 : 0) || !b.ready)
		{
			b.rebuildLook(w, look, weaponOut);
			og->bodyRevision[r.id] = r.lookRevision * 2 + (weaponOut ? 1 : 0);
		}
		float t = r.lastAt > r.prevAt ? (now - 0.1f - r.prevAt) / (r.lastAt - r.prevAt) : 1.0f;
		t = fmaxf(0.0f, fminf(1.0f, t));
		float yd = r.last.yaw - r.prev.yaw;
		if (yd > kTau / 2)
			yd -= kTau;
		if (yd < -kTau / 2)
			yd += kTau;
		b.placed = true;
		b.placeFeet[0] = r.prev.x + (r.last.x - r.prev.x) * t;
		b.placeFeet[1] = r.prev.y + (r.last.y - r.prev.y) * t;
		b.placeFeet[2] = r.prev.z + (r.last.z - r.prev.z) * t;
		b.placeYaw = r.prev.yaw + yd * t;
		{
			float invis = 0.0f, cham = 0.0f;
			for (auto& c : og->playerMagic[r.id])
				for (int fx : c.second)
					invis += fx == 39 ? 1.0f : 0.0f, cham += fx == 40 ? 50.0f : 0.0f;
			if (!b.set.actors.empty())
				b.set.actors[0].alpha = actorSeeThrough(false, invis, cham);
		}
		BodyAct act;
		act.moveY = r.last.animVel > 0.05f ? 1.0f : 0.0f;
		act.airborne = r.last.flags & 8;
		float speed = r.last.animVel * w.player.runSpeed * 0.5f;
		b.update(w, dt, speed, false, r.last.flags & 2, weaponOut, act);
	}
}

// ---- the shared story (PROTOCOL.md M6) and the clock (M7)

namespace
{
// The globals the clock owns (the server's, WorldTime), the bounty's (CrimeUpdate) and the ones that never stop moving
bool sharedGlobal(const std::string& g)
{
	static const char* kSkip[] = { "gamehour", "day", "month", "year", "dayspassed", "timescale", "pccrimelevel",
		"random100", "chargenstate" };
	for (const char* k : kSkip)
		if (g == k)
			return false;
	return true;
}

std::string factionSaid(const World& w, const std::string& f)
{
	auto r = w.pcRank.find(f);
	auto rep = w.pcFacRep.find(f);
	char s[64];
	snprintf(s, sizeof(s), "%d/%d/%d", r != w.pcRank.end() ? r->second : -1, rep != w.pcFacRep.end() ? rep->second : 0,
		w.pcExpelled.count(f) ? 1 : 0);
	return s;
}

// Day of the year (0-based) of a day of the month and a month (0-based)
int dayOfYear(int day, int month)
{
	int d = day - 1;
	for (int m = 0; m < month && m < 12; m++)
		d += kMonthDays[m];
	return d;
}
}

void Session::onlineStory(float now)
{
	Online& on = *g_online;
	// what the server already holds (the stored character just put on): only changes from here go out
	if (!og->questSeeded)
	{
		og->questSeeded = true;
		for (auto& j : w.journalIndex)
			og->journalSaid[j.first] = j.second;
		og->topicsSaid.insert(w.knownTopics.begin(), w.knownTopics.end());
		for (auto& g : w.globals)
			og->globalsSaid[g.first] = g.second;
		for (auto& f : w.pcRank)
			og->factionsSaid[f.first] = factionSaid(w, f.first);
		og->bountySaid = w.bounty;
	}
	if (now >= og->questAt && og->chargenSaid)
	{
		og->questAt = now + 1.0f;
		for (auto& j : w.journalIndex)
		{
			auto s = og->journalSaid.find(j.first);
			if (s != og->journalSaid.end() && s->second == j.second)
				continue;
			og->journalSaid[j.first] = j.second;
			LValue b = LValue::newTable();
			b.set("questId", j.first);
			b.set("index", (double)j.second);
			on.sendEvent("JournalEntry", b);
		}
		LValue topics = LValue::newTable();
		for (auto& t : w.knownTopics)
			if (topics.arraySize() < 64 && og->topicsSaid.insert(t).second)
				topics.push(LValue(t));
		if (topics.arraySize() > 0)
		{
			LValue b = LValue::newTable();
			b.set("topics", topics);
			on.sendEvent("TopicsLearned", b);
		}
		int sent = 0;
		for (auto& g : w.globals)
		{
			if (!sharedGlobal(g.first))
				continue;
			auto s = og->globalsSaid.find(g.first);
			if (s != og->globalsSaid.end() && s->second == g.second)
				continue;
			if (sent++ >= 24)
				break;                    // (the rest next second: the server's 60 a second budget)
			og->globalsSaid[g.first] = g.second;
			LValue b = LValue::newTable();
			b.set("name", g.first);
			b.set("value", (double)g.second);
			on.sendEvent("GlobalVarUpdate", b);
		}
		for (auto& f : w.pcRank)
		{
			std::string s = factionSaid(w, f.first);
			if (og->factionsSaid[f.first] == s)
				continue;
			og->factionsSaid[f.first] = s;
			LValue b = LValue::newTable();
			b.set("factionId", f.first);
			b.set("rank", (double)std::max(-1, std::min(20, f.second)));
			auto rep = w.pcFacRep.find(f.first);
			if (rep != w.pcFacRep.end())
				b.set("reputation", (double)rep->second);
			b.set("expelled", LValue::boolean(w.pcExpelled.count(f.first) > 0));
			on.sendEvent("FactionUpdate", b);
		}
		if (w.bounty != og->bountySaid)
		{
			og->bountySaid = w.bounty;
			LValue b = LValue::newTable();
			b.set("bounty", (double)std::max(0, w.bounty));
			on.sendEvent("CrimeUpdate", b);
		}
	}
	// A rest or a wait here moved the clock on: asked of the server, which moves everyone's (or says no, and the
	// next WorldTime puts ours back)
	if (og->clockHour >= 0 && og->clockAdopted && w.gameHour > og->clockHour + 0.5f)
	{
		LValue b = LValue::newTable();
		b.set("advanceHours", (double)fminf(720.0f, w.gameHour - og->clockHour));
		b.set("reason", w.pcSleeping ? "rest" : "wait");
		on.sendEvent("WorldTimeRequest", b);
		logf("online: asked the world for %.1f hours on", w.gameHour - og->clockHour);
	}
	og->clockHour = w.gameHour;
}

// One of the story's events from the server; false when it isn't one
bool Session::onlineStoryEvent(const std::string& name, const LValue& b)
{
	if (name == "JournalSync")
	{
		bool borrowed = b.boolAt("borrowed");
		if (borrowed && !og->journalBorrowed)
		{
			og->ownJournalIndex = w.journalIndex;
			og->ownJournal = w.journal;
			og->ownTopics = w.knownTopics;
			w.journalIndex.clear();
			w.journal.clear();
			og->journalSaid.clear();
			og->journalBorrowed = true;
			logf("online: a friend's journal for this visit; ours set aside");
		}
		else if (!borrowed && og->journalBorrowed)
		{
			w.journalIndex = og->ownJournalIndex;
			w.journal = og->ownJournal;
			w.knownTopics = og->ownTopics;
			og->journalBorrowed = false;
			logf("online: our own journal back");
		}
		// the shared journal (or ours): each quest at its stage, the dated log in the order earned
		if (const LValue* log = b.get("journalLog"))
			for (int i = 1; i <= log->arraySize(); i++)
			{
				const LValue* e = log->at(i);
				w.journalAdd(lowerCase(e->stringAt("q")), (int)e->numberAt("i"));
			}
		if (const LValue* q = b.get("quests"))
			for (auto& kv : q->table)
				if (kv.first.type == LValue::STRING && kv.second.type == LValue::NUMBER)
					w.journalAdd(lowerCase(kv.first.str), (int)kv.second.num);
		for (auto& j : w.journalIndex)
			og->journalSaid[j.first] = j.second;
		if (const LValue* t = b.get("topics"))
			for (int i = 1; i <= t->arraySize(); i++)
			{
				std::string id = lowerCase(t->at(i)->str);
				w.knownTopics.insert(id);
				og->topicsSaid.insert(id);
			}
		logf("online: the journal from the world: %d quests", (int)w.journalIndex.size());
	}
	else if (name == "JournalEntry")
	{
		std::string q = lowerCase(b.stringAt("questId"));
		if (w.journalAdd(q, (int)b.numberAt("index")))
			notify(w.game.gmst("sjournalentry", "Your journal has been updated."));
		og->journalSaid[q] = w.journalIndex[q];
	}
	else if (name == "TopicsLearned")
	{
		if (const LValue* t = b.get("topics"))
			for (int i = 1; i <= t->arraySize(); i++)
			{
				std::string id = lowerCase(t->at(i)->str);
				w.knownTopics.insert(id);
				og->topicsSaid.insert(id);
			}
	}
	else if (name == "GlobalVarSync" || name == "GlobalVarUpdate")
	{
		auto put = [&](const std::string& n, double v) {
			std::string g = lowerCase(n);
			if (!sharedGlobal(g))
				return;
			w.setGlobal(g, (float)v);
			og->globalsSaid[g] = w.globals[g];
		};
		if (name == "GlobalVarUpdate")
			put(b.stringAt("name"), b.numberAt("value"));
		else if (const LValue* g = b.get("globals"))
			for (auto& kv : g->table)
				if (kv.first.type == LValue::STRING && kv.second.type == LValue::NUMBER)
					put(kv.first.str, kv.second.num);
	}
	else if (name == "FactionUpdate")
	{
		std::string f = lowerCase(b.stringAt("factionId"));
		if (f.empty())
			return true;
		w.pcRank[f] = (int)b.numberAt("rank", -1);
		if (b.get("reputation"))
			w.pcFacRep[f] = (int)b.numberAt("reputation");
		if (b.boolAt("expelled"))
			w.pcExpelled.insert(f);
		else
			w.pcExpelled.erase(f);
		og->factionsSaid[f] = factionSaid(w, f);
	}
	else if (name == "CrimeUpdate")
	{
		w.bounty = std::max(0, (int)b.numberAt("bounty", w.bounty));
		w.globals["pccrimelevel"] = (float)w.bounty;
		og->bountySaid = w.bounty;
	}
	else if (name == "WorldTime")
	{
		// The server's clock: taken outright on arrival (the world ran on while we were away), then whenever ours
		// drifted a quarter of an hour from it. The days move by the calendar globals; gameHour keeps its own day count
		// (respawns, corpses and corprus count by it), only its hour moves
		int day = (int)b.numberAt("day", 1), month = (int)b.numberAt("month", 0), year = (int)b.numberAt("year", 427);
		float hour = (float)b.numberAt("gameHour", 9.0);
		GameDate d = w.date();
		int days = (year - d.year) * 365 + dayOfYear(day, month) - dayOfYear(d.day, d.month);
		float diff = days * 24.0f + hour - d.hour;
		if (!og->clockAdopted || fabsf(diff) > 0.25f)
		{
			if (diff >= 0.0f)
				w.gameHour += diff;          // (the calendar moves on with it: World::syncTime)
			else
			{
				w.globals["day"] = (float)day;
				w.globals["month"] = (float)month;
				w.globals["year"] = (float)year;
				w.gameHour = floorf(w.gameHour / 24.0f) * 24.0f + hour;
			}
			w.syncTime();
			og->clockHour = w.gameHour;
			if (!og->clockAdopted)
				logf("online: the world's clock: day %d of month %d, %d, %.2f h", day, month + 1, year, hour);
			og->clockAdopted = true;
		}
		if (b.get("timeScale"))
			w.globals["timescale"] = (float)b.numberAt("timeScale");
	}
	else if (name == "WorldTimeRefused")
		notify("The world's time can't move on: " + b.stringAt("reason"));
	else
		return false;
	return true;
}

// ---- the world's objects (PROTOCOL.md M3): what was taken, opened, unlocked, switched on or off, and who lies dead

namespace
{
ObjSeen objNow(const Ref& r)
{
	ObjSeen s;
	s.taken = r.pickedUp;
	s.open = r.doorTarget > 0.0f;
	s.lock = r.lockLevel > 0 ? r.lockLevel : 0;
	s.enabled = r.enabled;
	return s;
}

// "c:<index>:<content file>" (the server's doc keys) as a wire key; 0 for a spawned object's "n:<id>"
uint64_t keyOfRefKey(const std::string& k)
{
	long long index, file;
	if (sscanf(k.c_str(), "n:%lld", &index) == 1)
		return netRefKey(kNetActorFile, (uint32_t)index);
	if (sscanf(k.c_str(), "c:%lld:%lld", &index, &file) != 2)
		return 0;
	return netRefKey((int32_t)file, (uint32_t)index);
}
}

// The reference of this level a wire key names, in the cell given (or any loaded one); -1 when it isn't here
int Session::onlineRefOf(unsigned long long key, const std::string& cellKey)
{
	auto match = [&](int i) {
		const Ref& r = w.refs[i];
		if (r.rnFile == kNetActorFile)
			return netRefKey(kNetActorFile, r.rnIndex) == key;
		return r.rnFile >= 0 && r.rnFile < (int)og->contentFiles.size()
			&& netRefKey(og->contentFiles[r.rnFile], r.rnIndex) == key;
	};
	int c = cellKey.empty() ? -1 : cellOfKey(w, cellKey);
	if (c >= 0 && w.cells[c].refsLoaded)
	{
		const LevelCell& lc = w.cells[c];
		for (int i = lc.refBase; i < lc.refBase + lc.refCount; i++)
			if (match(i))
				return i;
	}
	int found = -1;
	w.forLoadedRefs([&](int i) {
		if (found < 0 && match(i))
			found = i;
	});
	return found;
}

// What the world says of one object, put on it here (and noted as seen: it is not told back)
void Session::onlineObjectApply(int ri, const char* what, double value)
{
	Ref& r = w.refs[ri];
	std::string k = what;
	if (k == "taken" || k == "deleted")
	{
		if (isItemType(r.type.str()))
			r.pickedUp = true;
		else if (r.actor < 0)
			r.enabled = false;
		else if (!r.dead)
		{
			r.dead = true;            // (a corpse cleared away there)
			r.enabled = false;
		}
		og->objectsHidden = true;
	}
	else if (k == "enabled")
	{
		r.enabled = value != 0.0;
		og->objectsHidden = true;
	}
	else if (k == "open")
	{
		bool open = value != 0.0;
		if ((r.doorTarget > 0.0f) != open)
		{
			r.doorTarget = open ? 1.5708f : 0.0f;
			if (r.obj)
				playSound(ri, open ? r.obj->openSound : r.obj->closeSound);
		}
	}
	else if (k == "lock")
		r.lockLevel = value > 0 ? (int)value : r.lockLevel > 0 ? -r.lockLevel : r.lockLevel;
	else if (k == "dead")
	{
		// dead there: a body here, already lying down (no fall, no sound, no tally: WorldKillCount keeps that)
		if (r.actor >= 0 && !r.dead)
		{
			r.dead = true;
			r.health = 0.0f;
			r.ai = AI_IDLE;
			w.syncActor(ri);
		}
	}
	if (r.rnFile >= 0 && r.rnFile < (int)og->contentFiles.size())
		og->objSeen[netRefKey(og->contentFiles[r.rnFile], r.rnIndex)] = objNow(r);
}

// A cell's state as the server keeps it (WorldCellState), once its objects are here
void Session::onlineCellState(const std::string& cellKey, const LValue& b)
{
	int c = cellOfKey(w, cellKey);
	if (c < 0 || !w.cells[c].refsLoaded)
		return;
	int n = 0;
	auto each = [&](const char* field, const char* what, double value) {
		if (const LValue* list = b.get(field))
			for (int i = 1; i <= list->arraySize(); i++)
			{
				uint64_t key = keyOfRefKey(list->at(i)->str);
				int ri = key ? onlineRefOf(key, cellKey) : -1;
				if (ri >= 0)
					onlineObjectApply(ri, what, value), n++;
			}
	};
	each("deleted", "deleted", 1);
	each("disabled", "enabled", 0);
	each("enabled", "enabled", 1);
	each("deaths", "dead", 1);
	if (const LValue* moved = b.get("moved"))
		for (auto& kv : moved->table)
		{
			int ri = onlineRefOf(keyOfRefKey(kv.first.str), cellKey);
			if (ri < 0 || w.refs[ri].actor >= 0)
				continue;
			Ref& r = w.refs[ri];
			r.pos[0] = (float)kv.second.numberAt("x", r.pos[0]);
			r.pos[1] = (float)kv.second.numberAt("y", r.pos[1]);
			r.pos[2] = (float)kv.second.numberAt("z", r.pos[2]);
			r.rot[2] = (float)kv.second.numberAt("rotZ", r.rot[2]);
			r.moved = true;
			r.fitBox();
			n++;
		}
	if (const LValue* vars = b.get("memberVars"))
		for (auto& kv : vars->table)
		{
			int ri = onlineRefOf(keyOfRefKey(kv.first.str), cellKey);
			if (ri < 0)
				continue;
			for (auto& v : kv.second.table)
				if (v.first.type == LValue::STRING && v.second.type == LValue::NUMBER)
					onlineMemberVar(ri, v.first.str, v.second.num);
			n++;
		}
	if (const LValue* placed = b.get("placed"))
		for (int i = 1; i <= placed->arraySize(); i++)
			onlinePlace(*placed->at(i));
	if (const LValue* doors = b.get("doors"))
		for (auto& kv : doors->table)
		{
			int ri = onlineRefOf(keyOfRefKey(kv.first.str), cellKey);
			if (ri >= 0)
				onlineObjectApply(ri, "open", kv.second.truthy() ? 1 : 0), n++;
		}
	if (const LValue* locks = b.get("locks"))
		for (auto& kv : locks->table)
		{
			int ri = onlineRefOf(keyOfRefKey(kv.first.str), cellKey);
			if (ri >= 0)
				onlineObjectApply(ri, "lock", kv.second.numberAt("lockLevel", 0)), n++;
		}
	if (n)
		logf("online: %s as the world has it: %d objects changed", cellKey.c_str(), n);
}

void Session::onlineObjects(float now)
{
	Online& on = *g_online;
	// cells whose state came before their objects did: put on once they are here
	for (auto it = og->cellStates.begin(); it != og->cellStates.end();)
	{
		int c = cellOfKey(w, it->first);
		if (c >= 0 && w.cells[c].refsLoaded)
		{
			onlineCellState(it->first, it->second);
			it = og->cellStates.erase(it);
		}
		else
			++it;
	}
	if (og->objectsHidden)
	{
		og->objectsHidden = false;
		w.refreshHidden();
	}
	if (now < og->objectsAt)
		return;
	og->objectsAt = now + 0.5f;
	// What the player did to the world since: told (a take is the server's to grant; the others are relayed)
	w.forLoadedRefs([&](int i) {
		const Ref& r = w.refs[i];
		if (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0)
			return;
		if (r.actor >= 0)
			return;                   // (people and creatures are the M4 stream's)
		uint64_t key = netRefKey(og->contentFiles[r.rnFile], r.rnIndex);
		ObjSeen s = objNow(r);
		auto seen = og->objSeen.find(key);
		if (seen == og->objSeen.end())
		{
			og->objSeen[key] = s;     // (first look: as the level and the world's state have it)
			return;
		}
		ObjSeen was = seen->second;
		seen->second = s;
		std::string cellKey = og->cellKeys.size() > (size_t)r.cell ? og->cellKeys[r.cell] : "";
		auto body = [&]() {
			LValue b = LValue::newTable();
			b.set("ref", LValue::refNum(r.rnIndex, og->contentFiles[r.rnFile]));
			b.set("cellKey", cellKey);
			return b;
		};
		if (s.taken && !was.taken)
		{
			LValue b = body();
			b.set("opId", (double)++og->opId);
			og->takeOps[og->opId] = { r.idLower.str(), std::max(1, r.count) };
			on.sendEvent("ObjectTakeRequest", b);
		}
		if (s.open != was.open && r.recType == REC_DOOR)
		{
			LValue b = body();
			b.set("open", LValue::boolean(s.open));
			on.sendEvent("DoorState", b);
		}
		if (s.lock != was.lock)
		{
			LValue b = body();
			if (s.lock > 0)
				b.set("lockLevel", (double)s.lock);
			on.sendEvent("ObjectLock", b);
		}
		if (s.enabled != was.enabled)
		{
			LValue b = body();
			b.set("enabled", LValue::boolean(s.enabled));
			on.sendEvent("ObjectEnabled", b);
		}
	});
}

// One of the objects' events from the server; false when it isn't one
bool Session::onlineObjectEvent(const std::string& name, const LValue& b)
{
	if (name == "WorldCellState")
	{
		std::string key = b.stringAt("cellKey");
		int c = cellOfKey(w, key);
		if (c >= 0 && w.cells[c].refsLoaded)
			onlineCellState(key, b);
		else if (c >= 0)
			og->cellStates[key] = b;
		return true;
	}
	if (name != "ObjectDelete" && name != "DoorState" && name != "ObjectLock" && name != "ObjectEnabled"
		&& name != "ObjectTakeResult")
		return false;
	if (name == "ObjectTakeResult")
	{
		auto op = og->takeOps.find((int)b.numberAt("opId", -1));
		if (op == og->takeOps.end())
			return true;
		OnlineGame::TakeOp t = op->second;
		og->takeOps.erase(op);
		if (!b.boolAt("ok", true))
		{
			// someone else had it first: it leaves the pack again
			w.removeItem(t.id, t.count);
			notify("Someone else took it first.");
			logf("online: the world refused taking %d %s (%s)", t.count, t.id.c_str(), b.stringAt("reason").c_str());
		}
		return true;
	}
	const LValue* ref = b.get("ref");
	if (!ref || ref->type != LValue::REFNUM || b.numberAt("byId", -1) == g_online->playerId)
		return true;
	int ri = onlineRefOf(netRefKey(ref->refFile, ref->refIndex), b.stringAt("cellKey"));
	if (ri < 0)
		return true;
	logf("online: %s on %s from player %d", name.c_str(), w.refs[ri].id.c_str(), (int)b.numberAt("byId", -1));
	if (name == "ObjectDelete")
		onlineObjectApply(ri, "deleted", 1);
	else if (name == "DoorState")
		onlineObjectApply(ri, "open", b.boolAt("open") ? 1 : 0);
	else if (name == "ObjectLock")
		onlineObjectApply(ri, "lock", b.numberAt("lockLevel", 0));
	else
		onlineObjectApply(ri, "enabled", b.boolAt("enabled", true) ? 1 : 0);
	return true;
}

// ---- containers (PROTOCOL.md M3): one chest, one corpse, one plant for everyone. The first to open it tells the server
// what it holds (our roll of its leveled lists); from then on the server's copy is the truth and every take or put is
// an operation on it

namespace
{
std::map<std::string, int> contentsOf(const Ref& r)
{
	std::map<std::string, int> m;
	for (auto& c : r.contents)
		m[lowerCase(c.second)] += stockCount(c.first);
	for (auto it = m.begin(); it != m.end();)
		it = it->second > 0 ? std::next(it) : m.erase(it);
	return m;
}

// The server's items put in place of ours: what we had of an id keeps its wear, charge and soul as far as it goes
void putContents(Ref& r, const LValue* items)
{
	std::map<std::string, int> want;
	if (items)
		for (int i = 1; i <= items->arraySize(); i++)
		{
			const LValue* e = items->at(i);
			int n = (int)e->numberAt("n");
			if (n > 0)
				want[localIdOf(e->stringAt("id"))] += n;
		}
	std::vector<ContentItem> kept;
	for (auto& c : r.contents)
	{
		auto it = want.find(lowerCase(c.second));
		if (it == want.end() || it->second <= 0)
			continue;
		int n = std::min(stockCount(c.first), it->second);
		ContentItem k = c;
		k.first = n;
		kept.push_back(k);
		it->second -= n;
	}
	for (auto& wv : want)
		if (wv.second > 0)
			kept.emplace_back(wv.second, wv.first);
	r.contents.swap(kept);
}
}

// What the player took out of an open container or put in since we last looked: one operation each on the server's
// copy, and kept as ours until the server echoes it
void Session::onlineContainerFlush(int ri)
{
	if (ri < 0 || ri >= (int)w.refs.size())
		return;
	const Ref& r = w.refs[ri];
	if (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0)
		return;
	std::map<std::string, int> have = contentsOf(r);
	std::set<std::string> ids;
	for (auto& c : have)
		ids.insert(c.first);
	for (auto& c : og->contSaid)
		ids.insert(c.first);
	uint64_t key = netRefKey(og->contentFiles[r.rnFile], r.rnIndex);
	for (auto& id : ids)
	{
		int dn = (have.count(id) ? have[id] : 0) - (og->contSaid.count(id) ? og->contSaid[id] : 0);
		if (!dn)
			continue;
		LValue b = LValue::newTable();
		b.set("ref", LValue::refNum(r.rnIndex, og->contentFiles[r.rnFile]));
		b.set("cellKey", og->cellKeys.size() > (size_t)r.cell ? og->cellKeys[r.cell] : "");
		b.set("opId", (double)++og->opId);
		b.set("op", dn < 0 ? "take" : "put");
		b.set("itemId", netIdOf(id));
		b.set("n", (double)abs(dn));
		g_online->sendEvent("ContainerOpRequest", b);
		og->contPending[key][id] += dn;
		og->contOps[og->opId] = { key, id, dn, ri };
	}
	og->contSaid = have;
}

void Session::onlineContainers(float now)
{
	Online& on = *g_online;
	int cr = screen == SCR_CONTAINER && containerRef >= 0 && containerRef < (int)w.refs.size() ? containerRef : -1;
	if (cr >= 0)
	{
		const Ref& r = w.refs[cr];
		if (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0
			|| (r.actor >= 0 && !r.dead))
			cr = -1;                  // (a spawned one, or someone's pockets: ours alone)
	}
	if (cr != og->contRef)
	{
		onlineContainerFlush(og->contRef);     // (what was taken just as it closed)
		og->contRef = cr;
		if (cr < 0)
			return;
		const Ref& r = w.refs[cr];
		og->contSaid = contentsOf(r);
		og->contPending.erase(netRefKey(og->contentFiles[r.rnFile], r.rnIndex));
		LValue items = LValue::newTable();
		for (auto& c : og->contSaid)
		{
			LValue e = LValue::newTable();
			e.set("id", netIdOf(c.first));
			e.set("n", (double)c.second);
			items.push(e);
		}
		LValue b = LValue::newTable();
		b.set("ref", LValue::refNum(r.rnIndex, og->contentFiles[r.rnFile]));
		b.set("cellKey", og->cellKeys.size() > (size_t)r.cell ? og->cellKeys[r.cell] : "");
		b.set("contents", items);
		on.sendEvent("ContainerOpen", b);
		og->contAt = now + 0.25f;
		return;
	}
	if (cr < 0 || now < og->contAt)
		return;
	og->contAt = now + 0.25f;
	onlineContainerFlush(cr);
}

bool Session::onlineContainerEvent(const std::string& name, const LValue& b)
{
	if (name != "ContainerState" && name != "ContainerUpdate" && name != "ContainerOpResult")
		return false;
	if (name == "ContainerOpResult")
	{
		auto op = og->contOps.find((int)b.numberAt("opId", -1));
		if (op == og->contOps.end())
			return true;
		OnlineGame::ContOp o = op->second;
		og->contOps.erase(op);
		if (b.boolAt("ok", true))
			return true;
		// refused (someone took it first): undone here. What was taken leaves the pack, what was put comes back, and
		// the container is read again from the server
		og->contPending[o.key][o.id] -= o.dn;
		if (o.dn < 0)
			w.removeItem(o.id, -o.dn);
		else
			w.addItem(o.id, o.dn);
		logf("online: the world refused %s %d %s (%s)", o.dn < 0 ? "taking" : "putting", abs(o.dn), o.id.c_str(),
			b.stringAt("reason").c_str());
		if (o.ref >= 0 && o.ref < (int)w.refs.size() && w.refs[o.ref].rnFile >= 0
			&& w.refs[o.ref].rnFile < (int)og->contentFiles.size() && og->contentFiles[w.refs[o.ref].rnFile] >= 0)
		{
			const Ref& r = w.refs[o.ref];
			LValue q = LValue::newTable();
			q.set("ref", LValue::refNum(r.rnIndex, og->contentFiles[r.rnFile]));
			q.set("cellKey", og->cellKeys.size() > (size_t)r.cell ? og->cellKeys[r.cell] : "");
			g_online->sendEvent("ContainerOpen", q);
			if (o.ref == og->contRef)
				og->contSaid = contentsOf(r);     // (nothing more to tell: the answer puts the server's copy in)
		}
		return true;
	}
	const LValue* ref = b.get("ref");
	if (!ref || ref->type != LValue::REFNUM)
		return true;
	int ri = onlineRefOf(netRefKey(ref->refFile, ref->refIndex), b.stringAt("cellKey"));
	if (ri < 0)
		return true;
	Ref& r = w.refs[ri];
	if (const LValue* g = b.get("gold"))
		if (g->type == LValue::NUMBER && r.actor >= 0 && !r.dead)
		{
			r.gold = std::max(0, (int)g->num);
			if (ri == og->purseRef)
				og->purseSaid = r.gold;
		}
	if (r.actor >= 0 && !r.dead)
		return true;
	if (name == "ContainerState")
	{
		// the server's copy, with our operations it hasn't answered yet on top (taken before its answer came)
		if (ri == og->contRef)
			onlineContainerFlush(ri);
		putContents(r, b.get("items"));
		auto mine = og->contPending.find(netRefKey(ref->refFile, ref->refIndex));
		if (mine != og->contPending.end())
		{
			std::map<std::string, int> m = contentsOf(r);
			for (auto& p : mine->second)
				m[p.first] = std::max(0, m[p.first] + p.second);
			LValue items = LValue::newTable();
			for (auto& c : m)
			{
				LValue e = LValue::newTable();
				e.set("id", c.first);
				e.set("n", (double)c.second);
				items.push(e);
			}
			putContents(r, &items);
		}
		logf("online: %s holds what the world says (%d stacks): %s", r.id.c_str(), (int)r.contents.size(),
			lserDescribe(b, 200).c_str());
	}
	else if (const LValue* d = b.get("delta"))
	{
		// a take or a put; ours come back too (the server's one way of applying them), already done here
		std::string id = localIdOf(d->stringAt("itemId"));
		int dn = (int)d->numberAt("dn");
		int& mine = og->contPending[netRefKey(ref->refFile, ref->refIndex)][id];
		if (mine != 0 && (mine > 0) == (dn > 0) && abs(mine) >= abs(dn))
		{
			mine -= dn;
			return true;
		}
		std::map<std::string, int> m = contentsOf(r);
		m[id] = std::max(0, m[id] + dn);
		LValue items = LValue::newTable();
		for (auto& c : m)
		{
			LValue e = LValue::newTable();
			e.set("id", c.first);
			e.set("n", (double)c.second);
			items.push(e);
		}
		putContents(r, &items);
		logf("online: %s changed by another player (%s %+d)", r.id.c_str(), id.c_str(), dn);
	}
	if (ri == og->contRef)
		og->contSaid = contentsOf(r);
	return true;
}

// ---- the player's spells on the server's people (PROTOCOL.md M5): the cast goes to the target's owner, which applies
// the spell record by its id and the effects that hit, with its own reflect, absorption and resistances

bool Session::onlineSpellHit(int ri, const SpellEffect& e)
{
	if (!netSpell || netSpellId.empty() || !onlinePuppet(ri))
		return false;
	OnlineGame::SpellHit& h = og->spellHits[ri];
	h.spellId = netIdOf(netSpellId);
	const SpellEffect* first = netSpell->effects.data();
	int index = &e >= first && &e < first + netSpell->effects.size() ? (int)(&e - first) : -1;
	std::string id = magicEffectId(e.effect);
	if (!id.empty())
	{
		LValue fx = LValue::newTable();
		fx.set("id", id);
		fx.set("magnitude", (double)((e.min + e.max) / 2));
		fx.set("duration", (double)e.duration);
		if (index >= 0)
			fx.set("index", (double)index);
		h.effects.push(fx);
	}
	if (index >= 0)
		h.indexes.push(LValue(index));
	return true;
}

void Session::onlineSpellHits()
{
	for (auto& kv : og->spellHits)
	{
		int ri = kv.first;
		OnlineGame::SpellHit& h = kv.second;
		if (h.effects.arraySize() == 0 || ri < 0 || ri >= (int)w.refs.size())
			continue;
		const Ref& r = w.refs[ri];
		bool net = r.rnFile == kNetActorFile;
		if (!net && (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0))
			continue;
		std::string cellKey = onlineActorCell(ri);
		LValue target = LValue::newTable();
		if (net)
			target.set("net", (double)r.rnIndex);
		else
			target.set("ref", LValue::refNum(r.rnIndex, og->contentFiles[r.rnFile]));
		target.set("cellKey", cellKey);
		auto ep = g_online->epochOf.find(cellKey);
		if (ep != g_online->epochOf.end())
			target.set("epoch", (double)ep->second);
		LValue b = LValue::newTable();
		b.set("target", target);
		b.set("spellId", h.spellId);
		b.set("effects", h.effects);
		b.set("casterId", (double)g_online->playerId);
		if (h.indexes.arraySize() == h.effects.arraySize())
			b.set("indexes", h.indexes);
		g_online->sendEvent("CombatSpellHit", b);
		logf("online: %s at %s goes to the world (%d effects)", h.spellId.c_str(), r.id.c_str(), h.effects.arraySize());
	}
	og->spellHits.clear();
}

// ---- things dropped (PROTOCOL.md M3): the player's drops go to the server, which names each (a net id) and shows it
// to everyone; others' come here as ObjectPlace (and in a cell's state as `placed`), and a pick-up of one is asked of
// the server as a take

namespace
{
LValue dropState(const Ref& r)
{
	LValue st = LValue::newTable();
	if (r.dropState.condition >= 0)
		st.set("condition", (double)r.dropState.condition);
	if (r.dropState.charge >= 0.0f)
		st.set("charge", (double)r.dropState.charge);
	if (!r.dropState.soul.empty())
		st.set("soul", r.dropState.soul);
	return st;
}
}

// Someone's drop (an ObjectPlace-shaped body) put here, or one of ours already lying where it says adopted
void Session::onlinePlace(const LValue& b)
{
	uint32_t net = (uint32_t)b.numberAt("netId", 0);
	if (!net || og->netObjs.count(net))
		return;
	if (b.boolAt("actor"))
	{
		// someone the server's world spawned (a summons, a leveled creature): here as one of its people
		if (og->netActors.count(net))
			return;
		std::string rid = localIdOf(b.stringAt("recordId"));
		float at[3] = { (float)b.numberAt("x"), (float)b.numberAt("y"), (float)b.numberAt("z") };
		int c = cellOfKey(w, b.stringAt("cellKey"));
		int slot = c >= 0 ? w.spawnActor(rid, at, (float)b.numberAt("rotZ"), c) : -1;
		if (slot >= 0)
		{
			w.refs[slot].rnFile = kNetActorFile;
			w.refs[slot].rnIndex = net;
			og->netActors[net] = slot;
			logf("online: %s spawned by the world (net %u)", rid.c_str(), net);
		}
		return;
	}
	std::string id = localIdOf(b.stringAt("recordId"));
	float pos[3] = { (float)b.numberAt("x"), (float)b.numberAt("y"), (float)b.numberAt("z") };
	int cell = cellOfKey(w, b.stringAt("cellKey"));
	if (cell < 0 || !w.game.object(id))
		return;
	// (a drop of ours from before, kept by the save as well as by the server: the same thing, not a second one)
	for (int k : w.spawned)
	{
		const Ref& r = w.refs[k];
		if (r.cell >= 0 && r.dropped && r.idLower == id && fabsf(r.pos[0] - pos[0]) < 2.0f && fabsf(r.pos[1] - pos[1]) < 2.0f
			&& fabsf(r.pos[2] - pos[2]) < 2.0f)
		{
			bool taken = false;
			for (auto& n : og->netObjs)
				taken |= n.second.slot == k;
			if (!taken)
			{
				og->netObjs[net] = OnlineGame::NetObj(k, b.stringAt("cellKey"));
				w.refs[k].netId = net;
				return;
			}
		}
	}
	InventoryItem it = { id, std::max(1, (int)b.numberAt("count", 1)), false };
	if (const LValue* st = b.get("state"))
	{
		it.condition = (int)st->numberAt("condition", -1);
		it.charge = (float)st->numberAt("charge", -1);
		it.soul = st->stringAt("soul");
	}
	int slot = w.dropItem(it, pos, (float)b.numberAt("rotZ"), cell);
	if (slot >= 0)
	{
		og->netObjs[net] = OnlineGame::NetObj(slot, b.stringAt("cellKey"));
		w.refs[slot].netId = net;
		logf("online: %d x %s placed by player %d", it.count, id.c_str(), (int)b.numberAt("byId", -1));
	}
}

void Session::onlineDrops()
{
	Online& on = *g_online;
	// (what each mapping is, from when it was made: a slot used again later is not the same thing)
	for (auto& n : og->netObjs)
		if (n.second.id.empty() && n.second.slot >= 0 && n.second.slot < (int)w.refs.size())
		{
			n.second.id = w.refs[n.second.slot].idLower.str();
			n.second.count = std::max(1, w.refs[n.second.slot].count);
		}
	// the player's own drops since the last look: each asked of the server, put back if it says no
	for (int slot : w.playerDrops)
	{
		if (slot < 0 || slot >= (int)w.refs.size() || w.refs[slot].cell < 0)
			continue;
		const Ref& r = w.refs[slot];
		int tempId = ++og->tempSeq;
		std::string cellKey = og->cellKeys.size() > (size_t)r.cell ? og->cellKeys[r.cell] : cellKeyAt(w, r.cell);
		og->pendingDrops[tempId] = OnlineGame::NetObj(slot, cellKey);
		LValue b = LValue::newTable();
		b.set("tempId", (double)tempId);
		b.set("recordId", netIdOf(r.idLower.str()));
		b.set("cellKey", cellKey);
		b.set("x", (double)r.pos[0]);
		b.set("y", (double)r.pos[1]);
		b.set("z", (double)r.pos[2]);
		b.set("rotZ", (double)r.rot[2]);
		b.set("count", (double)std::max(1, r.count));
		b.set("fromInventory", LValue::boolean(true));
		LValue st = dropState(r);
		if (!st.table.empty())
			b.set("state", st);
		on.sendEvent("ObjectSpawnRequest", b);
	}
	w.playerDrops.clear();
	// the shared ones picked up here (World::pickUp): a take, the server's to grant
	for (auto& t : w.playerTakes)
	{
		auto it = og->netObjs.find(t.net);
		LValue b = LValue::newTable();
		b.set("net", (double)t.net);
		b.set("cellKey", it != og->netObjs.end() ? it->second.cellKey : cellKeyAt(w, w.current));
		b.set("opId", (double)++og->opId);
		og->takeOps[og->opId] = { t.id, t.count };
		on.sendEvent("ObjectTakeRequest", b);
		if (it != og->netObjs.end())
			og->netObjs.erase(it);
	}
	w.playerTakes.clear();
	// the server's things and people whose cell left memory: forgotten here (its state puts them back when we return)
	for (u32 net : w.evictedNet)
	{
		og->netObjs.erase(net);
		og->netActors.erase(net);
	}
	w.evictedNet.clear();
	// (a mapping whose slot went another way, a despawn: dropped without a word)
	for (auto it = og->netObjs.begin(); it != og->netObjs.end();)
	{
		int slot = it->second.slot;
		bool gone = slot < 0 || slot >= (int)w.refs.size() || w.refs[slot].cell < 0 || w.refs[slot].netId != it->first;
		it = gone ? og->netObjs.erase(it) : std::next(it);
	}
}

bool Session::onlineDropEvent(const std::string& name, const LValue& b)
{
	if (name == "ObjectPlace")
	{
		if (b.numberAt("byId", -1) != g_online->playerId)
			onlinePlace(b);
		return true;
	}
	if (name == "ObjectSpawnAck" || name == "ObjectSpawnRefused")
	{
		auto p = og->pendingDrops.find((int)b.numberAt("tempId", -1));
		if (p == og->pendingDrops.end())
			return true;
		int slot = p->second.slot;
		std::string cellKey = p->second.cellKey;
		og->pendingDrops.erase(p);
		if (name == "ObjectSpawnAck")
		{
			uint32_t net = (uint32_t)b.numberAt("netId");
			og->netObjs[net] = OnlineGame::NetObj(slot, cellKey);
			if (slot < (int)w.refs.size() && w.refs[slot].cell >= 0 && w.refs[slot].dropped)
				w.refs[slot].netId = net;
		}
		else if (slot < (int)w.refs.size() && w.refs[slot].cell >= 0 && w.refs[slot].dropped)
		{
			// refused (nothing to drop, too far, a full cell): back in the pack
			logf("online: the world refused a drop of %s (%s)", w.refs[slot].id.c_str(), b.stringAt("reason").c_str());
			w.pickUp(slot);
		}
		return true;
	}
	if (name == "ObjectDelete" && b.get("net"))
	{
		auto na = og->netActors.find((uint32_t)b.numberAt("net"));
		if (na != og->netActors.end())
		{
			int slot = na->second;
			uint32_t net = na->first;
			og->netActors.erase(na);
			if (slot < (int)w.refs.size() && w.refs[slot].rnFile == kNetActorFile && w.refs[slot].rnIndex == net)
				w.despawn(slot);
			return true;
		}
		auto n = og->netObjs.find((uint32_t)b.numberAt("net"));
		if (n != og->netObjs.end())
		{
			int slot = n->second.slot;
			og->netObjs.erase(n);
			if (slot < (int)w.refs.size() && w.refs[slot].cell >= 0 && w.refs[slot].dropped
				&& w.refs[slot].netId == (uint32_t)b.numberAt("net"))
				w.despawn(slot);
		}
		return true;
	}
	return false;
}

// ---- one conversation with a person at a time (PROTOCOL.md M6 DialogueLock), and the sky (M7): each region's weather
// is simulated by one of the players there (WorldWeatherAuthority) and shown to the rest

void Session::onlineTalkAndSky(float now)
{
	Online& on = *g_online;
	// The person we talk to: the server lends them to one player at a time
	int talk = dlg.open && dlg.ref >= 0 && dlg.ref < (int)w.refs.size() ? dlg.ref : -1;
	if (talk >= 0)
	{
		const Ref& r = w.refs[talk];
		if (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0)
			talk = -1;
	}
	if (talk != og->talkRef)
	{
		auto lockBody = [&](int ri, bool want) {
			const Ref& r = w.refs[ri];
			if (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0)
				return;
			LValue b = LValue::newTable();
			b.set("ref", LValue::refNum(r.rnIndex, og->contentFiles[r.rnFile]));
			b.set("cellKey", onlineActorCell(ri));
			b.set("want", LValue::boolean(want));
			on.sendEvent("DialogueLock", b);
		};
		if (og->talkRef >= 0 && og->talkRef < (int)w.refs.size())
			lockBody(og->talkRef, false);
		og->talkRef = talk;
		if (talk >= 0)
			lockBody(talk, true);
	}
	if (now < og->skyAt)
		return;
	og->skyAt = now + 2.0f;
	// The region we are in (outdoors; indoors the last one stands), told on each change: the server gives each region's
	// weather to one of the players in it
	if (w.current >= 0 && w.cells[w.current].underSky() && !w.weatherRegion.empty() && w.weatherRegion != og->regionSaid)
	{
		og->regionSaid = w.weatherRegion;
		og->weatherSaid.clear();
		LValue b = LValue::newTable();
		b.set("region", og->regionSaid);
		on.sendEvent("WorldRegionChange", b);
	}
	auto h = og->weatherHolder.find(og->regionSaid);
	if (og->regionSaid.empty() || h == og->weatherHolder.end() || h->second != (uint16_t)on.playerId)
		return;
	// ours to simulate: our sky as it is, on each change and once a minute
	LValue b = LValue::newTable();
	b.set("region", og->regionSaid);
	b.set("current", (double)w.weatherNow);
	if (w.weatherNext != w.weatherNow)
	{
		b.set("next", (double)w.weatherNext);
		b.set("transition", (double)fmaxf(0.0f, fminf(1.0f, w.weatherBlend)));
	}
	char fp[96];
	snprintf(fp, sizeof(fp), "%s|%d|%d", og->regionSaid.c_str(), w.weatherNow, w.weatherNext);
	if (og->weatherSaid == fp && now - og->weatherSaidAt < 60.0f)
		return;
	og->weatherSaid = fp;
	og->weatherSaidAt = now;
	on.sendEvent("WorldWeather", b);
}

bool Session::onlineTalkAndSkyEvent(const std::string& name, const LValue& b)
{
	if (name == "DialogueLockResult")
	{
		const LValue* ref = b.get("ref");
		if (b.boolAt("granted") || !b.get("holderId") || !ref || ref->type != LValue::REFNUM || og->talkRef < 0
			|| og->talkRef >= (int)w.refs.size())
			return true;
		const Ref& r = w.refs[og->talkRef];
		if (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0)
			return true;
		if (netRefKey(og->contentFiles[r.rnFile], r.rnIndex) != netRefKey(ref->refFile, ref->refIndex))
			return true;
		// someone else is talking to them
		std::string who = "Someone else";
		auto p = g_online->players.find((uint16_t)b.numberAt("holderId"));
		if (p != g_online->players.end() && !p->second.name.empty())
			who = p->second.name;
		if (screen != SCR_NONE)
			closeScreen();
		og->talkRef = -1;
		notify(who + " is talking with " + (r.obj ? r.obj->name.str() : r.id.str()) + ".");
		return true;
	}
	if (name == "WorldWeatherAuthority")
	{
		std::string region = lowerCase(b.stringAt("region"));
		int holder = (int)b.numberAt("holderId", 0);
		if (holder)
			og->weatherHolder[region] = (uint16_t)holder;
		else
			og->weatherHolder.erase(region);
		if (region == og->regionSaid)
			og->weatherSaid.clear();
		return true;
	}
	if (name == "WorldWeather")
	{
		std::string region = lowerCase(b.stringAt("region"));
		auto h = og->weatherHolder.find(region);
		if (h != og->weatherHolder.end() && h->second == (uint16_t)g_online->playerId && !b.boolAt("restore"))
			return true;              // (our own, echoed)
		int current = (int)b.numberAt("current", -1), next = (int)b.numberAt("next", current);
		int target = next >= 0 && next != current ? next : current;
		if (target < 0 || target > 9)
			return true;
		w.forcedWeather[region] = target;
		// the region we are in: the same change of sky, begun the moment the holder's begins
		if (region == lowerCase(w.weatherRegion) && target != w.weatherNext)
			w.addWeatherTransition(target);
		return true;
	}
	return false;
}

// ---- followers and merchants' purses: a person the conversation sent after us (AIFollow, AIEscort) follows the server's
// copy of us there (PROTOCOL.md M4's follow claim); a merchant's gold is one purse for everyone (M3's op "gold")

void Session::onlineFollowersAndPurses(float now)
{
	Online& on = *g_online;
	if (now >= og->followAt)
	{
		og->followAt = now + 0.5f;
		// Whom the people the server simulates follow here: our own puppets' packages, set by a result script
		std::set<int> following;
		for (int i : w.loadedActors)
		{
			const Ref& r = w.refs[i];
			if (r.dead || !onlinePuppet(i))
				continue;
			if (r.aiPackage == AIPKG_TRAVEL && og->talkRef == i && !og->travelSaid.count(i))
			{
				// (a conversation sent them somewhere: the holder walks them there)
				og->travelSaid.insert(i);
				if (r.rnFile >= 0 && r.rnFile < (int)og->contentFiles.size() && og->contentFiles[r.rnFile] >= 0)
				{
					std::string ck = onlineActorCell(i);
					LValue b = LValue::newTable();
					b.set("cellKey", ck);
					auto ep = on.epochOf.find(ck);
					b.set("epoch", (double)(ep != on.epochOf.end() ? ep->second : 0));
					b.set("ref", LValue::refNum(r.rnIndex, og->contentFiles[r.rnFile]));
					LValue t = LValue::newTable();
					t.set("x", (double)r.aiDest[0]);
					t.set("y", (double)r.aiDest[1]);
					t.set("z", (double)r.aiDest[2]);
					b.set("travel", t);
					on.sendEvent("ActorAI", b);
				}
			}
			if ((r.aiPackage != AIPKG_FOLLOW && r.aiPackage != AIPKG_ESCORT) || r.aiTarget != "player")
				continue;
			following.insert(i);
		}
		auto claim = [&](int ri, bool follow) {
			const Ref& r = w.refs[ri];
			if (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0)
				return;
			std::string cellKey = onlineActorCell(ri);
			LValue b = LValue::newTable();
			b.set("cellKey", cellKey);
			auto ep = on.epochOf.find(cellKey);
			b.set("epoch", (double)(ep != on.epochOf.end() ? ep->second : 0));
			b.set("ref", LValue::refNum(r.rnIndex, og->contentFiles[r.rnFile]));
			if (follow)
				b.set("follow", (double)on.playerId);
			if (follow && r.aiPackage == AIPKG_ESCORT)
			{
				LValue esc = LValue::newTable();
				esc.set("x", (double)r.aiDest[0]);
				esc.set("y", (double)r.aiDest[1]);
				esc.set("z", (double)r.aiDest[2]);
				esc.set("duration", (double)r.aiDuration);
				b.set("escort", esc);
			}
			on.sendEvent("ActorAI", b);
			logf("online: %s %s us there", r.id.c_str(), follow ? "follows" : "no longer follows");
		};
		for (int i : following)
			if (!og->followers.count(i))
				claim(i, true);
		for (int i : og->followers)
			if (!following.count(i) && i < (int)w.refs.size() && w.refs[i].actor >= 0)
				claim(i, false);
		og->followers.swap(following);
	}
	// The merchant we trade with: their purse, as the server keeps it for everyone
	int m = (screen == SCR_BARTER && barterRef >= 0 && barterRef < (int)w.refs.size()) ? barterRef : -1;
	if (m >= 0)
	{
		const Ref& r = w.refs[m];
		if (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0)
			m = -1;
	}
	auto body = [&](const Ref& r) {
		LValue b = LValue::newTable();
		b.set("ref", LValue::refNum(r.rnIndex, og->contentFiles[r.rnFile]));
		b.set("cellKey", og->cellKeys.size() > (size_t)r.cell ? og->cellKeys[r.cell] : "");
		return b;
	};
	if (m != og->purseRef)
	{
		og->purseRef = m;
		if (m < 0)
			return;
		const Ref& r = w.refs[m];
		og->purseSaid = r.gold;
		// (opened the way a container is: their stock as rolled here, and the purse, for a server that has neither yet)
		LValue items = LValue::newTable();
		for (auto& c : r.contents)
		{
			if (stockCount(c.first) <= 0)
				continue;
			LValue e = LValue::newTable();
			e.set("id", lowerCase(c.second));
			e.set("n", (double)std::min(100, stockCount(c.first)));
			items.push(e);
		}
		LValue b = body(r);
		b.set("contents", items);
		b.set("gold", (double)std::max(0, r.gold));
		og->purseOpened = true;
		on.sendEvent("ContainerOpen", b);
		return;
	}
	if (m < 0)
		return;
	const Ref& r = w.refs[m];
	if (r.gold != og->purseSaid)
	{
		LValue b = body(r);
		b.set("opId", (double)++og->opId);
		b.set("op", "gold");
		b.set("goldDelta", (double)(r.gold - og->purseSaid));
		on.sendEvent("ContainerOpRequest", b);
		og->purseSaid = r.gold;
	}
}

// ---- the player's own body in the world (PROTOCOL.md M2 and Phase 4): death and coming back, what the player gains
// (drop conservation), the Mark, the magic on them (the server's copy wears it too), and what the server's copy of
// them went through there: skill uses, spells, magic and item wear

void Session::onlineSelf(float now)
{
	Online& on = *g_online;
	// Dead: told, and told again every five seconds until the world brings us back (PlayerResurrect)
	if (playerDead)
	{
		if (og->diedAt < 0)
			og->diedAt = now;
		if (now - og->diedAt > 45.0f)
		{
			// (the world never answered: up again where we fell, a little hurt, rather than stuck on this screen)
			playerDead = false;
			w.stats.health = fmaxf(1.0f, w.stats.healthMax * 0.25f);
			if (screen == SCR_DEATH)
				screen = SCR_NONE;
			og->diedAt = -1;
			notify("The world did not answer: you stand up again.");
			logf("online: no resurrection in 45 s; standing up here");
			return;
		}
		if (now - og->deathSaidAt >= 5.0f)
		{
			og->deathSaidAt = now;
			on.sendEvent("PlayerDeath", LValue::newTable());
			logf("online: dead; the world is told");
		}
	}
	else
	{
		og->deathSaidAt = -100.0f;
		og->diedAt = -1;
	}
	if (now < og->selfAt)
		return;
	og->selfAt = now + 0.25f;
	// Every count that went up since: credited on the server against what we may drop (PlayerItemAcquired)
	std::map<std::string, int> counts;
	for (auto& it : w.inventory)
		counts[lowerCase(it.id)] += it.count;
	if (og->countsSeeded)
		for (auto& c : counts)
		{
			auto was = og->countsSaid.find(c.first);
			int before = was == og->countsSaid.end() ? 0 : was->second;
			if (c.second > before)
			{
				LValue b = LValue::newTable();
				b.set("id", netIdOf(c.first));
				b.set("n", (double)(c.second - before));
				on.sendEvent("PlayerItemAcquired", b);
			}
		}
	og->countsSaid.swap(counts);
	og->countsSeeded = true;
	// The Mark (once a second is plenty: it is set by a spell)
	if (w.markCell >= 0 && now >= og->markAt)
	{
		og->markAt = now + 1.0f;
		char fp[96];
		snprintf(fp, sizeof(fp), "%d|%.0f|%.0f|%.0f", w.markCell, w.markPos[0], w.markPos[1], w.markPos[2]);
		if (og->markSaid != fp)
		{
			bool first = og->markSaid.empty();
			og->markSaid = fp;
			if (!first || !on.playerRecord)
			{
				LValue b = LValue::newTable();
				b.set("cell", cellKeyAt(w, w.markCell));
				b.set("x", (double)w.markPos[0]);
				b.set("y", (double)w.markPos[1]);
				b.set("z", (double)w.markPos[2]);
				on.sendEvent("PlayerMark", b);
			}
		}
	}
	// The magic on us that we cast ourselves, by cast (its spell and which of its effects): the server's copy wears
	// the same, so the world's blows meet our Shield, and the others see our Invisibility (PlayerActiveSpells)
	if (now >= og->activeAt)
	{
		og->activeAt = now + 0.5f;
		std::map<std::string, std::pair<std::string, std::set<int>>> active;   // key -> spell, effect indexes
		for (auto& e : w.effects)
			if (!e.spellId.empty() && e.index >= 0 && e.remaining > 0.0f)
			{
				auto& a = active[std::to_string(e.cast)];
				a.first = e.spellId;
				a.second.insert(e.index);
			}
		LValue add = LValue::newTable(), remove = LValue::newTable();
		for (auto& a : active)
			if (!og->activeSaid.count(a.first))
			{
				LValue e = LValue::newTable();
				e.set("key", a.first);
				e.set("id", netIdOf(a.second.first));
				LValue idx = LValue::newTable();
				for (int i : a.second.second)
					idx.push(LValue(i));
				e.set("effects", idx);
				add.push(e);
			}
		for (auto& s : og->activeSaid)
			if (!active.count(s.first))
			{
				LValue e = LValue::newTable();
				e.set("key", s.first);
				e.set("id", netIdOf(s.second));
				remove.push(e);
			}
		if (add.arraySize() || remove.arraySize())
		{
			LValue b = LValue::newTable();
			b.set("add", add);
			b.set("remove", remove);
			on.sendEvent("PlayerActiveSpells", b);
		}
		og->activeSaid.clear();
		for (auto& a : active)
			og->activeSaid[a.first] = lowerCase(a.second.first);
	}
}

// A spell record's effects (by 0-based index) put on the player as the world laid them on its copy of us
void Session::onlineSpellOnSelf(const std::string& spellId, const LValue* indexes, const std::string& source)
{
	auto sp = w.game.spells.find(localIdOf(spellId));
	if (sp == w.game.spells.end())
		return;
	const std::vector<SpellEffect>& fx = sp->second.effects;
	std::vector<int> which;
	if (indexes && indexes->arraySize() > 0)
		for (int i = 1; i <= indexes->arraySize(); i++)
			which.push_back((int)indexes->at(i)->num);
	else
		for (size_t i = 0; i < fx.size(); i++)
			which.push_back((int)i);
	for (int i : which)
		if (i >= 0 && i < (int)fx.size())
			applyEffectToPlayer(fx[i], source.empty() ? sp->second.name : source);
}

bool Session::onlineSelfEvent(const std::string& name, const LValue& b)
{
	if (name == "PlayerResurrect")
	{
		// back from death (the respawn plugin: where we fell, or the server's spot), or stood up where we lie
		playerDead = false;
		if (b.boolAt("restoreHp"))
		{
			w.stats.health = w.stats.healthMax;
			w.stats.magicka = w.stats.magickaMax;
			w.stats.fatigue = w.stats.fatigueMax;
		}
		else
			w.stats.health = fmaxf(1.0f, (float)b.numberAt("hp", w.stats.healthMax * 0.1f));
		if (screen == SCR_DEATH)
			screen = SCR_NONE;
		std::string key = lowerCase(b.stringAt("cellKey"));
		int cell = key.empty() ? -1 : cellOfKey(w, key);
		if (cell >= 0)
		{
			float pos[3] = { (float)b.numberAt("x"), (float)b.numberAt("y"), (float)b.numberAt("z") };
			teleportPlayer(cell, pos, w.player.yaw);
		}
		og->dynamicSaid.clear();
		notify("You are back among the living.");
		logf("online: resurrected (%s), health %.0f of %.0f", key.empty() ? "where we lie" : key.c_str(), w.stats.health,
			w.stats.healthMax);
		return true;
	}
	if (name == "SelfSkillUse")
	{
		// a block or an armor hit on our copy: our skill learns from it
		std::string s = lowerCase(b.stringAt("skill"));
		logf("online: our copy used %s there", s.c_str());
		for (int k = 0; k < 27; k++)
			if (s == kSkillFuncs[k])
				useSkill(k, (int)b.numberAt("useType", 0));
		return true;
	}
	if (name == "SelfSpells")
	{
		// spells the world gave our copy (a disease from a bite): ours now
		if (const LValue* add = b.get("add"))
			for (int i = 1; i <= add->arraySize(); i++)
			{
				std::string id = localIdOf(add->at(i)->str);
				if (id.empty() || !w.game.spells.count(id))
					continue;
				bool have = false;
				for (auto& s : w.stats.spells)
					have |= lowerCase(s) == id;
				if (!have)
				{
					w.stats.spells.push_back(id);
					notify("You have contracted " + w.game.spells.at(id).name + ".");
				}
			}
		w.refreshStats();
		return true;
	}
	if (name == "SelfActiveSpells")
	{
		// magic the world laid on our copy (a hostile Paralyze): on us too
		if (const LValue* add = b.get("add"))
			for (int i = 1; i <= add->arraySize(); i++)
			{
				const LValue* e = add->at(i);
				onlineSpellOnSelf(e->stringAt("id"), e->get("effects"), "");
			}
		return true;
	}
	if (name == "SelfItemStates")
	{
		// our items' wear, charge and souls as the world's swings and casts left them, in order per item
		if (const LValue* st = b.get("itemStates"))
			for (auto& kv : st->table)
			{
				std::string id = lowerCase(kv.first.str);
				int k = 1;
				for (auto& it : w.inventory)
				{
					if (lowerCase(it.id) != id)
						continue;
					const LValue* s = kv.second.at(k++);
					if (!s)
						break;
					if (s->get("condition"))
						it.condition = (int)s->numberAt("condition");
					if (s->get("charge"))
						it.charge = (float)s->numberAt("charge");
					if (s->get("soul"))
						it.soul = s->stringAt("soul");
				}
			}
		return true;
	}
	if (name == "AvatarActiveSpells")
	{
		// another player's magic: what shows on their body (Invisibility, Chameleon)
		auto p = g_online->players.find((uint16_t)b.numberAt("id"));
		if (p == g_online->players.end())
			return true;
		auto effectsOf = [&](const LValue* e) {
			std::vector<int> out;
			auto sp = w.game.spells.find(lowerCase(e->stringAt("id")));
			if (sp == w.game.spells.end())
				return out;
			const LValue* idx = e->get("effects");
			for (int i = 1; idx && i <= idx->arraySize(); i++)
			{
				int k = (int)idx->at(i)->num;
				if (k >= 0 && k < (int)sp->second.effects.size())
					out.push_back(sp->second.effects[k].effect);
			}
			return out;
		};
		auto& shown = og->playerMagic[p->first];
		if (const LValue* add = b.get("add"))
			for (int i = 1; i <= add->arraySize(); i++)
				shown[add->at(i)->stringAt("key")] = effectsOf(add->at(i));
		if (const LValue* rem = b.get("remove"))
			for (int i = 1; i <= rem->arraySize(); i++)
				shown.erase(rem->at(i)->stringAt("key"));
		return true;
	}
	return false;
}

// ---- scripts' state (PROTOCOL.md M6): an object's own variables (MemberVarUpdate), watched on what we talk to or open,
// and the global scripts running (GlobalScriptsUpdate / Sync)

namespace
{
ScriptInstance* scriptOf(World& w, int ri)
{
	if (ri < 0 || ri >= (int)w.refs.size())
		return nullptr;
	int s = w.refs[ri].script;
	return s >= 0 && s < (int)w.scripts.size() && w.scripts[s].script ? &w.scripts[s] : nullptr;
}
}

void Session::onlineScripts(float now)
{
	Online& on = *g_online;
	// what we talk to and what we have open: watched while it lasts and two seconds after (a result script's last word)
	for (int ri : { og->talkRef, og->contRef })
		if (ri >= 0)
		{
			auto it = og->memberWatch.find(ri);
			if (it == og->memberWatch.end())
			{
				// first look: as it is (what the server already has, or our own start)
				if (ScriptInstance* si = scriptOf(w, ri))
					og->memberSaid[ri] = si->locals;
			}
			og->memberWatch[ri] = now + 2.0f;
		}
	if (now >= og->memberAt)
	{
		og->memberAt = now + 0.25f;
		for (auto it = og->memberWatch.begin(); it != og->memberWatch.end();)
		{
			int ri = it->first;
			ScriptInstance* si = scriptOf(w, ri);
			const Ref* r = ri < (int)w.refs.size() ? &w.refs[ri] : nullptr;
			if (si && r && r->rnFile >= 0 && r->rnFile < (int)og->contentFiles.size() && og->contentFiles[r->rnFile] >= 0)
			{
				std::vector<float>& said = og->memberSaid[ri];
				said.resize(si->locals.size(), 0.0f);
				for (size_t k = 0; k < si->locals.size() && k < si->script->localNames.size(); k++)
				{
					if (si->locals[k] == said[k])
						continue;
					said[k] = si->locals[k];
					LValue b = LValue::newTable();
					b.set("ref", LValue::refNum(r->rnIndex, og->contentFiles[r->rnFile]));
					b.set("name", lowerCase(si->script->localNames[k]));
					b.set("value", (double)si->locals[k]);
					on.sendEvent("MemberVarUpdate", b);
				}
			}
			if (now > it->second)
			{
				og->memberSaid.erase(ri);
				it = og->memberWatch.erase(it);
			}
			else
				++it;
		}
	}
	// The global scripts running (Sleepers, VampireCheck ...), every few seconds: the campaign keeps them going
	if (now >= og->globalScriptsAt)
	{
		og->globalScriptsAt = now + 3.0f;
		std::set<std::string> running;
		for (auto& s : w.scripts)
			if (s.ref < 0 && s.running && s.script && s.item.empty())
				running.insert(lowerCase(s.script->name));
		if (og->globalScriptsSeeded)
		{
			LValue started = LValue::newTable(), stopped = LValue::newTable();
			for (auto& n : running)
				if (!og->globalScriptsSaid.count(n) && started.arraySize() < 256)
					started.push(LValue(n));
			for (auto& n : og->globalScriptsSaid)
				if (!running.count(n) && stopped.arraySize() < 256)
					stopped.push(LValue(n));
			if (started.arraySize() || stopped.arraySize())
			{
				LValue b = LValue::newTable();
				if (started.arraySize())
					b.set("started", started);
				if (stopped.arraySize())
					b.set("stopped", stopped);
				on.sendEvent("GlobalScriptsUpdate", b);
			}
		}
		og->globalScriptsSaid.swap(running);
		og->globalScriptsSeeded = true;
	}
}

// An object's variable set as the world has it
void Session::onlineMemberVar(int ri, const std::string& name, double value)
{
	ScriptInstance* si = scriptOf(w, ri);
	if (!si)
		return;
	std::string n = lowerCase(name);
	if (float* v = si->local(n))
	{
		*v = (float)value;
		auto said = og->memberSaid.find(ri);
		int k = si->script->localIndex(n);
		if (said != og->memberSaid.end() && k >= 0 && k < (int)said->second.size())
			said->second[k] = (float)value;
	}
}

bool Session::onlineScriptEvent(const std::string& name, const LValue& b)
{
	if (name == "MemberVarUpdate")
	{
		const LValue* ref = b.get("ref");
		if (!ref || ref->type != LValue::REFNUM)
			return true;
		int ri = onlineRefOf(netRefKey(ref->refFile, ref->refIndex), b.stringAt("cellKey"));
		if (ri >= 0)
			onlineMemberVar(ri, b.stringAt("name"), b.numberAt("value"));
		return true;
	}
	if (name == "GlobalScriptsSync")
	{
		// the campaign's running global scripts: any we lack start (and are not told back)
		int started = 0;
		if (const LValue* run = b.get("running"))
			for (int i = 1; i <= run->arraySize(); i++)
			{
				std::string n = lowerCase(run->at(i)->str);
				bool have = false;
				for (auto& s : w.scripts)
					have |= s.ref < 0 && s.running && s.script && lowerCase(s.script->name) == n;
				if (!have && w.startGlobalScript(n) >= 0)
					started++;
				og->globalScriptsSaid.insert(n);
			}
		logf("online: the world's global scripts: %d started here", started);
		return true;
	}
	return false;
}

// ---- the world's people, the rest of what their holder says (PROTOCOL.md M4): the magic that shows on them, their
// disposition and Fight / Flee / Alarm, a door they went through, a unique corpse stripped, a scripted line, a package
// done; and a guard of the world come to arrest us (PlayerArrest)

bool Session::onlineActorEvent(const std::string& name, const LValue& b)
{
	if (name == "PlayerArrest")
	{
		const LValue* g = b.get("guard");
		int ri = g && g->type == LValue::REFNUM ? onlineRefOf(netRefKey(g->refFile, g->refIndex), "") : -1;
		if (ri < 0 || w.refs[ri].dead)
			return true;
		arrestingGuard = ri;
		w.refs[ri].alarmed = true;
		if (screen != SCR_NONE)
			closeScreen();
		forceGreeting(ri);
		if (screen != SCR_DIALOGUE)
			openScreen(SCR_ARREST);
		logf("online: %s arrests us", w.refs[ri].id.c_str());
		return true;
	}
	if (name != "ActorEffects" && name != "ActorDisposition" && name != "ActorCellChange" && name != "ActorStripLoot"
		&& name != "ActorSay" && name != "ActorAI")
		return false;
	const LValue* ref = b.get("ref");
	LValue netRef = LValue::refNum((uint32_t)b.numberAt("net", 0), kNetActorFile);
	if (!ref && b.get("net"))
		ref = &netRef;
	if (!ref || ref->type != LValue::REFNUM)
		return true;
	int ri = onlineRefOf(netRefKey(ref->refFile, ref->refIndex), b.stringAt("cellKey"));
	if (ri < 0)
		return true;
	Ref& r = w.refs[ri];
	if (name != "ActorAI")
		logf("online: %s on %s", name.c_str(), r.id.c_str());
	if (name == "ActorEffects")
	{
		// what shows: added as timed effects on them, taken off by the spell's effects when it ends there
		if (const LValue* add = b.get("add"))
			for (int i = 1; i <= add->arraySize(); i++)
			{
				const LValue* e = add->at(i);
				auto sp = w.game.spells.find(lowerCase(e->stringAt("id")));
				if (sp == w.game.spells.end())
					continue;
				const LValue* idx = e->get("effects");
				for (int k = 1; idx && k <= idx->arraySize(); k++)
				{
					int n = (int)idx->at(k)->num;
					if (n < 0 || n >= (int)sp->second.effects.size())
						continue;
					const SpellEffect& fx = sp->second.effects[n];
					r.effects.push_back({ fx.effect, (fx.min + fx.max) * 0.5f, (float)std::max(1, fx.duration) });
				}
			}
		if (const LValue* rem = b.get("remove"))
			for (int i = 1; i <= rem->arraySize(); i++)
			{
				auto sp = w.game.spells.find(lowerCase(rem->at(i)->stringAt("id")));
				if (sp == w.game.spells.end())
					continue;
				for (auto& fx : sp->second.effects)
					for (auto it = r.effects.begin(); it != r.effects.end();)
						it = it->effect == fx.effect ? r.effects.erase(it) : std::next(it);
			}
	}
	else if (name == "ActorDisposition")
	{
		r.disposition = std::max(0, std::min(100, (int)b.numberAt("disposition", r.disposition)));
		if (const LValue* ai = b.get("ai"))
		{
			if (ai->get("fight"))
				r.fight = std::max(0, std::min(100, (int)ai->numberAt("fight")));
			if (ai->get("flee"))
				r.flee = std::max(0, std::min(100, (int)ai->numberAt("flee")));
			if (ai->get("alarm"))
				r.alarm = std::max(0, std::min(100, (int)ai->numberAt("alarm")));
		}
	}
	else if (name == "ActorCellChange")
	{
		// through a door there: here too (out of sight of this cell, into the other)
		int cell = cellOfKey(w, lowerCase(b.stringAt("toCellKey")));
		float pos[3] = { (float)b.numberAt("x"), (float)b.numberAt("y"), (float)b.numberAt("z") };
		if (cell >= 0)
			w.relocate(ri, cell, pos, r.rot[2]);
		auto a = g_online->actors.find(netRefKey(ref->refFile, ref->refIndex));
		if (a != g_online->actors.end())
			a->second.lastAt = a->second.prevAt = -1;
	}
	else if (name == "ActorStripLoot")
	{
		r.contents.clear();
		r.gold = 0;
	}
	else if (name == "ActorSay")
	{
		if (!b.stringAt("file").empty())
			say(ri, b.stringAt("file"), b.stringAt("text"));
	}
	else if (name == "ActorAI" && b.boolAt("done"))
		r.aiDone = true;              // (GetAIPackageDone: the holder's package finished)
	return true;
}

// A crime's witness the server simulates turned on us here (makeHostile on a puppet): the fight is the holder's to
// start, so it is claimed there (ActorAI combat = us, crime)
void Session::onlineCrimeCombat(int ri, bool pursue)
{
	if (!g_online || !og || !onlinePuppet(ri))
		return;
	const Ref& r = w.refs[ri];
	if (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0)
		return;
	std::string cellKey = onlineActorCell(ri);
	LValue b = LValue::newTable();
	b.set("cellKey", cellKey);
	auto ep = g_online->epochOf.find(cellKey);
	b.set("epoch", (double)(ep != g_online->epochOf.end() ? ep->second : 0));
	b.set("ref", LValue::refNum(r.rnIndex, og->contentFiles[r.rnFile]));
	b.set("combat", (double)g_online->playerId);
	b.set("crime", LValue::boolean(true));
	if (pursue)
		b.set("pursue", LValue::boolean(true));
	g_online->sendEvent("ActorAI", b);
}

// ---- records the players make (PROTOCOL.md M7): a spell from the spellmaker, a brewed potion, an enchantment and the
// item it is on. Each gets the server's id (RecordCreate -> Ack), and that id is what travels in its place; the others'
// arrive in RecordsSync and are built here under their server id

namespace
{
std::map<std::string, std::string> g_recNet;          // our record id -> the server's
std::map<std::string, std::string> g_recLocal;        // and back

LValue effectsData(const std::vector<SpellEffect>& fx)
{
	LValue list = LValue::newTable();
	for (auto& e : fx)
	{
		LValue d = LValue::newTable();
		d.set("id", magicEffectId(e.effect));
		if (e.skill >= 0 && e.skill < 27)
			d.set("affectedSkill", kSkillFuncs[e.skill]);
		if (e.attribute >= 0 && e.attribute < 8)
			d.set("affectedAttribute", kAttrFuncs[e.attribute]);
		d.set("range", (double)e.range);
		d.set("area", (double)e.area);
		d.set("magnitudeMin", (double)std::min(100, e.min));
		d.set("magnitudeMax", (double)std::min(100, e.max));
		d.set("duration", (double)std::min(1440, e.duration));
		list.push(d);
	}
	return list;
}

std::vector<SpellEffect> effectsFrom(const LValue* list)
{
	std::vector<SpellEffect> out;
	for (int i = 1; list && i <= list->arraySize(); i++)
	{
		const LValue* d = list->at(i);
		std::string id = lowerCase(d->stringAt("id"));
		int effect = -1;
		for (int k = 0; k < 143 && effect < 0; k++)
			if (magicEffectId(k) == id)
				effect = k;
		if (effect < 0)
			continue;
		SpellEffect e = { effect, -1, -1, (int)d->numberAt("magnitudeMin"), (int)d->numberAt("magnitudeMax"),
			(int)d->numberAt("duration"), (int)d->numberAt("range") };
		e.area = (int)d->numberAt("area");
		std::string sk = lowerCase(d->stringAt("affectedSkill")), at = lowerCase(d->stringAt("affectedAttribute"));
		for (int k = 0; k < 27; k++)
			if (sk == kSkillFuncs[k])
				e.skill = k;
		for (int k = 0; k < 8; k++)
			if (at == kAttrFuncs[k])
				e.attribute = k;
		out.push_back(e);
	}
	return out;
}

const char* itemKind(const Object& o)
{
	switch (o.recType)
	{
	case REC_WEAP: return "weapon";
	case REC_ARMO: return "armor";
	case REC_CLOT: return "clothing";
	case REC_BOOK: return "book";
	case REC_ALCH: return "potion";
	default: return "misc";
	}
}
}

namespace
{
// The id a record travels by: the server's for one of ours it named, else the id itself
std::string netIdOf(const std::string& local)
{
	auto it = g_recNet.find(lowerCase(local));
	return it != g_recNet.end() ? it->second : lowerCase(local);
}

// And the id here of one that came in: ours by its server id, else the id itself (others' are built under it)
std::string localIdOf(const std::string& net)
{
	auto it = g_recLocal.find(lowerCase(net));
	return it != g_recLocal.end() ? it->second : lowerCase(net);
}
}

void Session::onlineRecords(float now)
{
	if (now < og->recordsAt)
		return;
	og->recordsAt = now + 2.0f;
	Online& on = *g_online;
	auto pending = [&](const std::string& id) {
		for (auto& p : og->recordPending)
			if (p.second == id)
				return true;
		return false;
	};
	auto send = [&](const std::string& id, const char* kind, const LValue& data) {
		int temp = ++og->tempSeq;
		og->recordPending[temp] = id;
		LValue b = LValue::newTable();
		b.set("tempId", (double)temp);
		b.set("kind", kind);
		b.set("data", data);
		on.sendEvent("RecordCreate", b);
		logf("online: %s %s told to the world", kind, id.c_str());
	};
	auto known = [&](const std::string& id) { return g_recNet.count(id) || pending(id) || og->recordRefused.count(id); };
	for (auto& sid : w.madeSpells)
	{
		std::string id = lowerCase(sid);
		auto sp = w.game.spells.find(id);
		if (known(id) || sp == w.game.spells.end())
			continue;
		LValue d = LValue::newTable();
		d.set("name", sp->second.name);
		d.set("type", 0.0);
		d.set("cost", (double)std::max(1, sp->second.cost));
		d.set("isAutocalc", LValue::boolean(false));
		d.set("effects", effectsData(sp->second.effects));
		send(id, "spell", d);
	}
	for (auto& pid : w.brewed)
	{
		std::string id = lowerCase(pid);
		const Object* o = w.game.object(id);
		if (known(id) || !o)
			continue;
		LValue d = LValue::newTable();
		d.set("name", o->name.str());
		d.set("model", "m\\misc_potion_standard_01.nif");
		d.set("icon", o->icon.str());
		d.set("weight", (double)o->weight);
		d.set("value", (double)o->value);
		d.set("effects", effectsData(o->effects));
		send(id, "potion", d);
	}
	for (auto& iid : w.madeItems)
	{
		std::string id = lowerCase(iid);
		const Object* o = w.game.object(id);
		if (known(id) || !o)
			continue;
		// the enchantment first: the item names it by the server's id
		std::string ench = lowerCase(o->ench.str());
		auto en = w.game.spells.find(ench);
		if (!ench.empty() && en != w.game.spells.end() && !g_recNet.count(ench))
		{
			if (!known(ench))
			{
				LValue d = LValue::newTable();
				d.set("type", (double)std::max(0, en->second.type - ENCH_ONCE));
				d.set("cost", (double)std::max(1, en->second.cost));
				d.set("charge", (double)std::min(400, std::max(0, en->second.charge)));
				d.set("isAutocalc", LValue::boolean(false));
				d.set("effects", effectsData(en->second.effects));
				send(ench, "enchantment", d);
			}
			continue;
		}
		LValue d = LValue::newTable();
		d.set("name", o->name.str());
		d.set("icon", o->icon.str());
		d.set("weight", (double)o->weight);
		d.set("value", (double)o->value);
		if (!ench.empty())
			d.set("enchant", netIdOf(ench));
		if (o->subtype >= 0)
			d.set("type", (double)o->subtype);
		if (o->health > 0)
			d.set("health", (double)o->health);
		if (o->enchant > 0)
			d.set("enchantCapacity", (double)o->enchant);
		if (o->recType == REC_WEAP)
		{
			d.set("chopMinDamage", (double)o->chop[0]);
			d.set("chopMaxDamage", (double)o->chop[1]);
			d.set("slashMinDamage", (double)o->slash[0]);
			d.set("slashMaxDamage", (double)o->slash[1]);
			d.set("thrustMinDamage", (double)o->thrust[0]);
			d.set("thrustMaxDamage", (double)o->thrust[1]);
			d.set("speed", (double)std::min(2.0f, o->speed));
			d.set("reach", (double)std::min(2.0f, o->reach));
		}
		if (o->recType == REC_ARMO)
			d.set("baseArmor", (double)std::min(200, o->armor));
		send(id, itemKind(*o), d);
	}
}

// Someone's record built here under its server id: a spell or enchantment as a spell, a potion or an item as a copy of
// one of the game's of its kind (its look: the one with the same icon when there is one) with the record's own values
void Session::onlineRecord(const LValue& r)
{
	std::string net = lowerCase(r.stringAt("recordNetId")), kind = r.stringAt("kind");
	const LValue* d = r.get("data");
	if (net.empty() || !d || g_recLocal.count(net) || w.game.spells.count(net) || w.game.object(net))
		return;
	if (kind == "spell" || kind == "enchantment")
	{
		SpellDef sp;
		sp.id = net;
		sp.name = d->stringAt("name", net);
		sp.cost = (int)d->numberAt("cost");
		sp.charge = (int)d->numberAt("charge");
		sp.type = kind == "spell" ? (int)d->numberAt("type") : ENCH_ONCE + (int)d->numberAt("type");
		sp.effects = effectsFrom(d->get("effects"));
		w.game.spells[net] = sp;
		og->builtRecords.insert(net);
		return;
	}
	RecName want = kind == "potion" ? REC_ALCH : kind == "weapon" ? REC_WEAP : kind == "armor" ? REC_ARMO
		: kind == "clothing" ? REC_CLOT : kind == "book" ? REC_BOOK : REC_MISC;
	std::string icon = lowerCase(d->stringAt("icon"));
	const Object* base = nullptr;
	for (auto& kv : w.game.objects)
		if (kv.second.recType == want && kv.second.iconIx >= 0)
		{
			std::string oi = lowerCase(kv.second.icon.str());
			bool same = !icon.empty() && !oi.empty() && (icon.size() >= oi.size() ? icon.compare(icon.size() - oi.size(),
				oi.size(), oi) == 0 : oi.compare(oi.size() - icon.size(), icon.size(), icon) == 0);
			if (!base || same)
				base = &kv.second;
			if (same)
				break;
		}
	if (!base)
		return;
	Object o = *base;
	o.id = net;
	o.name = d->stringAt("name", net);
	o.value = (int)d->numberAt("value", o.value);
	o.weight = (float)d->numberAt("weight", o.weight);
	if (want == REC_ALCH)
		o.effects = effectsFrom(d->get("effects"));
	std::string ench = d->stringAt("enchant");
	if (!ench.empty())
	{
		o.ench = localIdOf(ench);
		o.magic = true;
		if (o.iconBase.empty())
			o.iconBase = base->id;
	}
	w.game.objects[net] = o;
	og->builtRecords.insert(net);
}

bool Session::onlineRecordEvent(const std::string& name, const LValue& b)
{
	if (name == "RecordCreateAck" || name == "RecordCreateRefused")
	{
		auto p = og->recordPending.find((int)b.numberAt("tempId", -1));
		if (p == og->recordPending.end())
			return true;
		std::string local = p->second;
		og->recordPending.erase(p);
		if (name == "RecordCreateAck")
		{
			std::string net = lowerCase(b.stringAt("recordNetId"));
			g_recNet[local] = net;
			g_recLocal[net] = local;
			logf("online: %s is %s to the world", local.c_str(), net.c_str());
		}
		else
		{
			og->recordRefused.insert(local);
			logf("online: the world refused our record %s (%s)", local.c_str(), b.stringAt("reason").c_str());
		}
		return true;
	}
	if (name == "RecordsSync")
	{
		// (enchantments first: the items name them)
		if (const LValue* recs = b.get("records"))
			for (int pass = 0; pass < 2; pass++)
				for (int i = 1; i <= recs->arraySize(); i++)
				{
					const LValue* r = recs->at(i);
					bool spellish = r->stringAt("kind") == "spell" || r->stringAt("kind") == "enchantment";
					if (spellish == (pass == 0))
						onlineRecord(*r);
				}
		return true;
	}
	return false;
}

// ---- hits and spells on us, our spells on other players and their casts (PROTOCOL.md M5); the map's explored places
// and a cell made new again (M7); the server's correction of where we stand (0x0103)

// Our cast just released: shown on others' screens (CombatCast), and its touch / target effects, when the crosshair
// is on another player rather than on one of the world's people, sent to them (CombatSpellHit: a heal on a friend,
// or harm where the server allows player fights)
void Session::onlineCastReleased(const SpellDef& sp, const std::string& spellId)
{
	if (!g_online || !og || g_online->phase != Online::IN_WORLD)
		return;
	Online& on = *g_online;
	LValue cast = LValue::newTable();
	cast.set("spellId", netIdOf(spellId));
	cast.set("casterId", (double)on.playerId);
	cast.set("kind", castSource.empty() ? "spell" : "enchant");
	on.sendEvent("CombatCast", cast);
	bool touch = false, target = false;
	for (auto& e : sp.effects)
		touch |= e.range == 1, target |= e.range == 2;
	if (!touch && !target)
		return;
	float eye[3] = { w.player.feet[0], w.player.feet[1], w.player.feet[2] + PLAYER_EYE_HEIGHT };
	float dir[3] = { sinf(w.player.yaw) * cosf(w.player.pitch), cosf(w.player.yaw) * cosf(w.player.pitch), sinf(w.player.pitch) };
	int best = -1;
	float bestD = 1e9f;
	for (auto& kv : on.players)
	{
		const RemotePlayer& p = kv.second;
		if (!p.inView || p.lastAt < 0)
			continue;
		float to[3] = { p.last.x - eye[0], p.last.y - eye[1], p.last.z + 80.0f - eye[2] };
		float d = sqrtf(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
		float reach = target ? 4000.0f : 200.0f;
		if (d < 1.0f || d > reach)
			continue;
		float along = (to[0] * dir[0] + to[1] * dir[1] + to[2] * dir[2]) / d;
		if (along > cosf(0.12f) && d < bestD)
		{
			best = kv.first;
			bestD = d;
		}
	}
	if (best < 0)
		return;
	LValue effects = LValue::newTable(), indexes = LValue::newTable();
	for (size_t i = 0; i < sp.effects.size(); i++)
	{
		const SpellEffect& e = sp.effects[i];
		if (e.range != 1 && e.range != 2)
			continue;
		LValue fx = LValue::newTable();
		fx.set("id", magicEffectId(e.effect));
		fx.set("magnitude", (double)((e.min + e.max) / 2));
		fx.set("duration", (double)e.duration);
		fx.set("index", (double)i);
		effects.push(fx);
		indexes.push(LValue((int)i));
	}
	LValue t = LValue::newTable();
	t.set("playerId", (double)best);
	LValue b = LValue::newTable();
	b.set("target", t);
	b.set("spellId", netIdOf(spellId));
	b.set("effects", effects);
	b.set("indexes", indexes);
	b.set("casterId", (double)on.playerId);
	on.sendEvent("CombatSpellHit", b);
	logf("online: %s at player %d", spellId.c_str(), best);
}

void Session::onlineWorldMisc(float now)
{
	Online& on = *g_online;
	// People a script placed: the server's world places them (PROTOCOL.md M3 ObjectSpawnRequest actor=true -> QuestSpawn)
	{
		// (only where someone holds the cell we are in: elsewhere there is nobody to place them)
		auto h = on.holderOf.find(cellKeyAt(w, w.current));
		w.actorsElsewhere = h != on.holderOf.end() && h->second != (uint16_t)on.playerId;
	}
	for (auto& a : w.spawnAsks)
	{
		LValue b = LValue::newTable();
		b.set("tempId", 0.0);
		b.set("recordId", netIdOf(a.id));
		b.set("cellKey", cellKeyAt(w, w.current));
		b.set("x", (double)a.pos[0]);
		b.set("y", (double)a.pos[1]);
		b.set("z", (double)a.pos[2]);
		b.set("rotZ", (double)a.yaw);
		b.set("count", (double)std::min(10, a.count));
		b.set("actor", LValue::boolean(true));
		on.sendEvent("ObjectSpawnRequest", b);
		logf("online: asked the world to place %d %s", a.count, a.id.c_str());
	}
	w.spawnAsks.clear();
	// The places we have been outdoors (the map), told once each
	if (w.current >= 0 && !w.cells[w.current].interior)
	{
		std::string key = cellKeyAt(w, w.current);
		if (og->exploredSaid.insert(key).second && og->exploredSeeded)
		{
			LValue keys = LValue::newTable();
			keys.push(LValue(key));
			LValue b = LValue::newTable();
			b.set("cellKeys", keys);
			on.sendEvent("WorldMapExplored", b);
		}
		og->exploredSeeded = true;
	}
	// Where the server says we stand (it refused a move, or it runs our movement): put there when it is far from here
	if (on.correctionAt > og->correctionSeen)
	{
		og->correctionSeen = on.correctionAt;
		float dx = on.correction.x - w.player.feet[0], dy = on.correction.y - w.player.feet[1];
		float dz = on.correction.z - w.player.feet[2];
		if (dx * dx + dy * dy + dz * dz > 96.0f * 96.0f && w.current >= 0)
		{
			float pos[3] = { on.correction.x, on.correction.y, on.correction.z };
			teleportPlayer(w.current, pos, w.player.yaw);
			logf("online: the world put us back at %.0f %.0f %.0f", pos[0], pos[1], pos[2]);
		}
	}
	(void)now;
}

bool Session::onlineCombatEvent(const std::string& name, const LValue& b)
{
	if (name == "CombatHit")
	{
		// a blow on us the server sent here to land (no copy of us there): our armor takes its share
		const LValue* d = b.get("damage");
		if (!d)
			return true;
		float hp = (float)d->numberAt("health"), ft = (float)d->numberAt("fatigue"), mp = (float)d->numberAt("magicka");
		float armor = playerArmor();
		if (hp > 0.0f)
			hp = hp * hp / (hp + fmaxf(0.0f, armor));     // (OpenMW's armor: damage x damage / (damage + rating))
		w.stats.health -= hp;
		w.stats.fatigue -= ft;
		w.stats.magicka = fmaxf(0.0f, w.stats.magicka - mp);
		if (hp > 0.0f)
			playSound(-1, "Health Damage");
		logf("online: hit by player %d for %.1f", (int)b.numberAt("attackerId", -1), hp);
		return true;
	}
	if (name == "CombatSpellHit")
	{
		// a spell on us (a friend's heal, a foe's fire): the record's effects that hit, with our own reflect and
		// resistances
		onlineSpellOnSelf(b.stringAt("spellId"), b.get("indexes"), "");
		return true;
	}
	if (name == "CombatCast")
	{
		// someone cast: the cast's light on their body
		auto p = g_online->players.find((uint16_t)b.numberAt("fromId", b.numberAt("casterId", -1)));
		auto sp = w.game.spells.find(localIdOf(b.stringAt("spellId")));
		if (p != g_online->players.end() && p->second.lastAt >= 0 && sp != w.game.spells.end())
		{
			float pos[3] = { p->second.last.x, p->second.last.y, p->second.last.z };
			spellVfx(sp->second, 0, pos, -2, p->second.last.yaw);
		}
		return true;
	}
	if (name == "WorldMapExplored")
	{
		// places the others found: on our map (the named ones, as the game marks only those)
		if (const LValue* keys = b.get("cellKeys"))
			for (int i = 1; i <= keys->arraySize(); i++)
			{
				std::string key = keys->at(i)->str;
				og->exploredSaid.insert(key);
				int c = cellOfKey(w, key);
				if (c >= 0 && !w.cells[c].interior && w.cells[c].name.str() != w.cells[c].region.str()
					&& lowerCase(w.cells[c].name.str()) != "wilderness")
					w.mapKnown.insert(lowerCase(w.cells[c].name.str()));
			}
		return true;
	}
	if (name == "WorldCellReset")
	{
		// the server made a cell new again: its objects as the level has them, then its state asked for afresh
		int c = cellOfKey(w, b.stringAt("cellKey"));
		if (c >= 0)
		{
			w.respawnCell(c);
			LValue q = LValue::newTable();
			q.set("cellKey", b.stringAt("cellKey"));
			g_online->sendEvent("ResyncRequest", q);
			logf("online: %s made new again", b.stringAt("cellKey").c_str());
		}
		return true;
	}
	return false;
}

// ---- the world's people and creatures the server simulates (PROTOCOL.md M4): whoever holds their cell (the sim peer)
// streams them; here they stand, walk, swing and die as it says (openmw-web's puppet.lua)

// The cell an actor stands in now, as the server keys it: outdoors the grid square under it (the stream walks people
// across squares; their reference still names the one the level placed them in), indoors its cell
std::string Session::onlineActorCell(int ri)
{
	const Ref& r = w.refs[ri];
	int c = r.at >= 0 ? r.at : r.cell;
	if (c < 0 || c >= (int)w.cells.size())
		return "";
	if (!w.cells[c].interior)
	{
		char k[32];
		snprintf(k, sizeof(k), "%d,%d", (int)floorf(r.pos[0] / 8192.0f), (int)floorf(r.pos[1] / 8192.0f));
		return k;
	}
	return c < (int)og->cellKeys.size() ? og->cellKeys[c] : cellKeyAt(w, c);
}

bool Session::onlinePuppet(int ri)
{
	if (!g_online || !og || g_online->phase != Online::IN_WORLD || ri < 0 || ri >= (int)w.refs.size())
		return false;
	const Ref& r = w.refs[ri];
	if (r.actor < 0)
		return false;
	if (r.rnFile == kNetActorFile)
		return true;                  // (spawned by the server's world: always its)
	if (r.rnFile < 0 || r.rnFile >= (int)og->contentFiles.size() || og->contentFiles[r.rnFile] < 0)
		return false;
	Online& on = *g_online;
	auto a = on.actors.find(netRefKey(og->contentFiles[r.rnFile], r.rnIndex));
	if (a != on.actors.end() && a->second.lastAt >= 0)
		return true;
	if (on.holderOf.empty())
		return false;
	auto h = on.holderOf.find(onlineActorCell(ri));
	return h != on.holderOf.end() && h->second != (uint16_t)on.playerId;
}

namespace
{
// The animation for a streamed pose: a blow when its attack starts, else standing, walking or running, in the ready
// stance with its weapon out
void puppetAnimate(World& w, Ref& r, Actor& a, ActorSet& set, uint8_t flags, uint8_t was, bool moving)
{
	const ActorDef& def = w.game.actors[r.actor];
	const Object* wpn = def.creature ? nullptr : w.game.object(def.weapon);
	bool drawn = flags & 16;
	a.showWeapon = drawn;
	const Skeleton& sk = actorSkeleton(set, a.skeleton);
	if ((flags & 8) && !(was & 8))
	{
		actorPlay(set, a, blowGroup(set, a, r, wpn).c_str(), ANIM_ONCE);
		return;
	}
	if (a.mode == ANIM_ONCE && !moving)
		return;                       // (a blow, a hit, a block: played through)
	const char* base = moving ? (flags & 1 ? "RunForward" : "WalkForward") : "Idle";
	std::string g = drawn || def.creature ? stanceGroup(set, a, base, wpn) : std::string(base);
	int gi = actorFindGroup(sk, g.c_str());
	if (gi < 0)
		g = base, gi = actorFindGroup(sk, base);
	if (!moving && !drawn && a.mode == ANIM_IDLE)
		return;                       // (its idles play on)
	if (gi >= 0 && a.group != gi)
		actorPlay(set, a, g.c_str(), moving ? ANIM_LOOP : ANIM_IDLE);
}
}

void Session::onlinePuppets(float now)
{
	Online& on = *g_online;
	// (once a minute: actors not heard from in two minutes and not dead go, and what was seen of objects when it
	// grows past what a few towns hold: seen again quietly)
	if (now >= og->pruneAt)
	{
		og->pruneAt = now + 60.0f;
		for (auto it = on.actors.begin(); it != on.actors.end();)
		{
			bool stale = !it->second.dead && it->second.lastAt >= 0 && now - it->second.lastAt > 120.0f;
			if (stale)
				og->puppetFlags.erase(it->first);
			it = stale ? on.actors.erase(it) : std::next(it);
		}
		if (og->objSeen.size() > 20000)
			og->objSeen.clear();
		for (auto it = og->playerMagic.begin(); it != og->playerMagic.end();)
			it = on.players.count(it->first) ? std::next(it) : og->playerMagic.erase(it);
	}
	if (og->cellKeys.size() != w.cells.size())
	{
		og->cellKeys.resize(w.cells.size());
		for (size_t c = 0; c < w.cells.size(); c++)
			og->cellKeys[c] = cellKeyAt(w, (int)c);
	}
	og->contentFiles.clear();
	for (int k = 0; k < 8; k++)
		og->contentFiles.push_back(on.contentFileOf(k));
	for (int i : w.loadedActors)
	{
		Ref& r = w.refs[i];
		if (r.actor < 0 || !r.visible() || !onlinePuppet(i))
			continue;
		auto it = on.actors.find(r.rnFile == kNetActorFile ? netRefKey(kNetActorFile, r.rnIndex)
			: netRefKey(og->contentFiles[r.rnFile], r.rnIndex));
		if (it == on.actors.end())
			continue;
		NetActor& n = it->second;
		if (n.hp >= 0)
		{
			if (n.hpMax > 0)
				r.healthMax = n.hpMax;
			if (!r.dead)
				r.health = fmaxf(n.dead ? 0.0f : 1.0f, n.hp);
		}
		if (n.dead && !r.dead)
		{
			killNpc(i, false);            // (the world's death: no crime, no witness here; the server keeps the tally)
			continue;
		}
		if (!n.dead && r.dead && n.hp > 0)
		{
			r.dead = r.died = false;      // ActorRevive (a script's Resurrect there)
			r.health = n.hp;
			logf("online: %s revived by the world", r.id.c_str());
			r.ai = AI_IDLE;
			if (Actor* a = w.actorOf(i))
				actorPlay(*w.actorsOf(i), *a, "Idle", ANIM_IDLE);
		}
		if (r.dead || n.lastAt < 0)
			continue;
		// between the last two poses, a tenth of a second behind (they come 10 to 20 a second)
		float t = n.lastAt > n.prevAt ? (now - 0.1f - n.prevAt) / (n.lastAt - n.prevAt) : 1.0f;
		t = fmaxf(0.0f, fminf(1.0f, t));
		float yd = n.last.yaw - n.prev.yaw;
		if (yd > kTau / 2)
			yd -= kTau;
		if (yd < -kTau / 2)
			yd += kTau;
		float x = n.prev.x + (n.last.x - n.prev.x) * t, y = n.prev.y + (n.last.y - n.prev.y) * t;
		float z = n.prev.z + (n.last.z - n.prev.z) * t;
		bool moving = n.last.animVel > 0.05f || fabsf(x - r.pos[0]) + fabsf(y - r.pos[1]) > 0.5f;
		r.pos[0] = x;
		r.pos[1] = y;
		r.pos[2] = z;
		r.rot[2] = n.prev.yaw + yd * t;
		r.ai = n.combat ? AI_COMBAT : AI_IDLE;
		r.moved = true;
		if (!og->puppetFlags.count(it->first))
			logf("online: %s follows the world's stream (%.0f %.0f %.0f)", r.id.c_str(), x, y, z);
		uint8_t& was = og->puppetFlags[it->first];
		if (Actor* a = w.actorOf(i))
			puppetAnimate(w, r, *a, *w.actorsOf(i), n.last.flags, was, moving && n.last.animVel > 0.02f);
		was = n.last.flags;
		w.syncActor(i);
	}
}

void Session::onlineDrawPlayers(bool deform)
{
	if (!og)
		return;
	for (auto& kv : og->bodies)
	{
		if (kv.second.ready)
			kv.second.draw(w, deform);
	}
}

// A report written about a player (the online screen's Report): to the server's moderators
void Session::onlineReport(const std::string& reason)
{
	if (!g_online || !og || og->reportName.empty())
		return;
	LValue b = LValue::newTable();
	b.set("name", og->reportName);
	b.set("reason", reason.substr(0, 400));
	g_online->sendEvent("ReportPlayer", b);
	notify("Reported " + og->reportName + ". Thank you.");
	og->reportName.clear();
}

// Leaving a world (another one's, home, or the same one again after a drop): what was its alone goes. Its people
// and things the server spawned, the ids it gave our records and the ones it built here, who held which cell, the
// operations waiting for its answers; the next world's welcome brings its own
void Session::onlineForgetWorld()
{
	Online& on = *g_online;
	for (auto& n : og->netActors)
		if (n.second < (int)w.refs.size() && w.refs[n.second].rnFile == kNetActorFile && w.refs[n.second].rnIndex == n.first)
			w.despawn(n.second);
	for (auto& n : og->netObjs)
		if (n.second.slot >= 0 && n.second.slot < (int)w.refs.size() && w.refs[n.second.slot].netId == n.first)
			w.despawn(n.second.slot);
	og->netActors.clear();
	og->netObjs.clear();
	og->pendingDrops.clear();
	og->takeOps.clear();
	og->contPending.clear();
	og->contOps.clear();
	og->cellStates.clear();
	og->recordPending.clear();
	og->recordRefused.clear();
	og->weatherHolder.clear();
	og->followers.clear();
	og->travelSaid.clear();
	og->memberWatch.clear();
	og->memberSaid.clear();
	og->puppetFlags.clear();
	og->playerMagic.clear();
	og->objSeen.clear();
	og->spellHits.clear();
	og->talkRef = og->contRef = og->purseRef = -1;
	for (auto& id : og->builtRecords)
	{
		w.game.spells.erase(id);
		w.game.objects.erase(id);
	}
	og->builtRecords.clear();
	g_recNet.clear();
	g_recLocal.clear();
	on.holderOf.clear();
	on.epochOf.clear();
	on.actors.clear();
	w.actorsElsewhere = false;
	logf("online: the last world's state forgotten");
}

void Session::onlineFree()
{
	w.actorsElsewhere = false;
	if (!og)
		return;
	for (auto& kv : og->bodies)
		kv.second.free(w);
	delete og;
	og = nullptr;
}

// ---- the online screen (the HUD's Online button): players here, chat, friends

void Session::drawOnline()
{
	if (!g_online)
	{
		closeScreen();
		return;
	}
	if (!og)
		og = new OnlineGame();
	Online& on = *g_online;
	uiCaption("Online", on.worldName.empty() ? on.config.server : on.worldName);
	// (the connection lost: what is going on, and a way to try at once)
	if (on.phase != Online::IN_WORLD)
	{
		uiRect(4, 50, 312, 150, 0xB0000000);
		uiTextCentered(160, 100, 0.5f, col::text, on.phase == Online::FAILED ? "Disconnected: " + on.error()
			: on.status.empty() ? std::string("Connecting...") : on.status);
		if (on.phase == Online::FAILED && og->wasInWorld && uiButton(84, 150, 152, 28, "Reconnect now"))
		{
			on.reconnect();
			og->reconnectDelay = 5.0f;
		}
		if (uiButton(216, 208, 100, 28, "Back") || (uiIn().down & KEY_B))
			closeScreen();
		return;
	}
	static const char* kTabs[3] = { "Players", "Chat", "Friends" };
	for (int i = 0; i < 3; i++)
		if (uiButton(4 + i * 80, 24, 76, 22, kTabs[i], og->tab == i))
		{
			og->tab = i;
			og->list = UiList();
		}
	if (uiButton(244, 24, 72, 22, og->party ? "Party: on" : "Party: off"))
	{
		LValue b = LValue::newTable();
		b.set("mode", og->party ? "private" : "party");
		on.sendEvent("SetWorldMode", b);
	}
	std::vector<std::string> rows, right;
	if (og->tab == 0)
	{
		rows.push_back(w.stats.name + " (you)");
		right.push_back(on.rttMs >= 0 ? std::to_string((int)on.rttMs) + " ms" : "");
		for (auto& kv : on.players)
		{
			rows.push_back(kv.second.name.empty() ? "Player " + std::to_string(kv.first) : kv.second.name);
			right.push_back(kv.second.inView ? "near" : kv.second.cellKey);
		}
		uiList(og->list, 4, 50, 312, 150, rows, 0.5f, true, &right);
		// (a player to report: ReportPlayer files it for the server's moderators with what they said lately)
		int sel = og->list.selected;
		if (sel >= 1 && sel < (int)rows.size())
		{
			auto it = on.players.begin();
			std::advance(it, sel - 1);
			const std::string& who = it->second.name;
			if (uiButton(110, 208, 100, 28, "Report"))
			{
				og->reportName = who;
				wantText = TEXT_REPORT;
			}
			// (block: no messages, requests or invites from them; mute: their chat hidden; the owner sends a guest home)
			bool muted = false;
			if (const LValue* m = og->friends.get("muted"))
				for (int i = 1; i <= m->arraySize(); i++)
					muted |= lowerCase(m->at(i)->stringAt("name")) == lowerCase(who);
			if (uiButton(4, 174, 100, 26, "Block"))
			{
				LValue b = LValue::newTable();
				b.set("name", who);
				on.sendEvent("BlockAdd", b);
				notify("Blocked " + who);
			}
			if (!muted && uiButton(108, 174, 100, 26, "Mute"))
			{
				LValue b = LValue::newTable();
				b.set("name", who);
				on.sendEvent("MuteAdd", b);
				notify("Muted " + who);
			}
			if (og->isOwner && uiButton(212, 174, 100, 26, "Send home"))
			{
				LValue b = LValue::newTable();
				b.set("name", who);
				on.sendEvent("WorldKick", b);
			}
		}
	}
	else if (og->tab == 1)
	{
		std::string text;
		for (auto& l : on.chat)
			text += (l.from.empty() ? "" : l.from + ": ") + l.text + "\n";
		static UiScroll scroll;
		uiTextBox(scroll, 4, 50, 312, 150, text.empty() ? "No messages yet." : text, 0.45f);
	}
	else
	{
		// FriendList: friends (online first), then requests and invites waiting for us
		struct Row { std::string acct, name, kind; bool online; };
		std::vector<Row> fr;
		auto take = [&](const char* key, const char* kind) {
			if (const LValue* l = og->friends.get(key))
				for (int i = 1; i <= l->arraySize(); i++)
				{
					const LValue* e = l->at(i);
					fr.push_back({ e->stringAt("acct"), e->stringAt("name"), kind, e->boolAt("online") });
				}
		};
		take("requests", "request");
		take("invites", "invite");
		take("friends", "friend");
		take("blocked", "blocked");
		take("muted", "muted");
		for (auto& f : fr)
		{
			rows.push_back(f.name);
			right.push_back(f.kind == "request" ? "wants to be friends" : f.kind == "invite" ? "invites you"
				: f.kind == "blocked" ? "blocked" : f.kind == "muted" ? "muted" : f.online ? "online" : "offline");
		}
		int pick = uiList(og->list, 4, 50, 312, 120, rows, 0.5f, true, &right);
		(void)pick;
		int sel = og->list.selected;
		bool has = sel >= 0 && sel < (int)fr.size();
		if (has && fr[sel].kind == "request" && uiButton(4, 174, 100, 26, "Accept"))
		{
			LValue b = LValue::newTable();
			b.set("name", fr[sel].name);
			on.sendEvent("FriendAccept", b);
		}
		if (has && fr[sel].kind == "invite" && uiButton(4, 174, 100, 26, "Go there"))
		{
			LValue b = LValue::newTable();
			b.set("acct", fr[sel].acct);
			on.sendEvent("InviteAccept", b);
		}
		if (has && fr[sel].kind == "friend" && fr[sel].online && uiButton(212, 174, 100, 26, "Join"))
		{
			LValue b = LValue::newTable();
			b.set("acct", fr[sel].acct);
			on.sendEvent("JoinFriend", b);
		}
		if (has && fr[sel].kind == "friend" && uiButton(4, 174, 100, 26, "Invite here"))
		{
			LValue b = LValue::newTable();
			b.set("acct", fr[sel].acct);
			on.sendEvent("InviteSend", b);
			notify("Invited " + fr[sel].name);
		}
		if (uiButton(108, 174, 100, 26, "Add friend"))
			wantText = TEXT_FRIEND;
		static const char* kRemove[][2] = { { "friend", "FriendRemove" }, { "blocked", "BlockRemove" }, { "muted", "MuteRemove" } };
		static const char* kRemoveLabel[] = { "Remove", "Unblock", "Unmute" };
		bool removing = false;
		for (int k = 0; k < 3 && has; k++)
			if (fr[sel].kind == kRemove[k][0])
			{
				removing = true;
				if (uiButton(110, 208, 100, 28, kRemoveLabel[k]))
				{
					LValue b = LValue::newTable();
					b.set("acct", fr[sel].acct);
					on.sendEvent(kRemove[k][1], b);
				}
			}
		// who sees us: everyone, friends, nobody, or we appear offline (PresenceMode, SetAvailability)
		static const char* kPresence[] = { "Seen: all", "Seen: friends", "Seen: none", "Offline" };
		if (!removing && uiButton(110, 208, 100, 28, kPresence[og->presence]))
		{
			og->presence = (og->presence + 1) % 4;
			static const char* kMode[] = { "public", "friends", "private" };
			LValue a = LValue::newTable();
			a.set("state", og->presence == 3 ? "offline" : "online");
			on.sendEvent("SetAvailability", a);
			if (og->presence < 3)
			{
				LValue m = LValue::newTable();
				m.set("mode", kMode[og->presence]);
				on.sendEvent("PresenceMode", m);
			}
		}
	}
	if (uiButton(4, 208, 100, 28, "Say"))
		wantText = TEXT_CHAT;
	if (uiButton(216, 208, 100, 28, "Back") || (uiIn().down & KEY_B))
		closeScreen();
}
