// The online mode in the game (FractumSeraph/mw3ds): the stored character put on the player, the player's state told
// to the server as openmw-web's own client tells it (scripts/mp/identity.lua: the same events, cadences and shapes),
// the other players drawn with the player's body builder, chat and the online screen.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <cstdlib>
#include <3ds.h>

#include "components/debug/debuglog.hpp"
#include "components/interpreter/scriptfunctions.hpp"
#include "extern/cjson/cJSON.h"
#include "mwbase/session.hpp"
#include "mwgui/widgets.hpp"
#include "mwmp/online.hpp"
#include "mwrender/npcanimation.hpp"

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

std::string cellKeyOf(const World& w)
{
	if (w.current < 0)
		return "";
	const LevelCell& c = w.cells[w.current];
	if (!c.interior)
	{
		char k[32];
		snprintf(k, sizeof(k), "%d,%d", c.gx, c.gy);
		return k;
	}
	return lowerCase(c.name.str());
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
					slots.table.emplace_back(key, LValue(lowerCase(it.id)));
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
	if (on.phase == Online::FAILED)
	{
		if (!og->appearanceSaid.empty() || og->readySent)
		{
			notify(on.error());
			og->readySent = false;
			og->appearanceSaid.clear();
			og->bodies.clear();
		}
		return;
	}
	// between worlds (a friend's, or home): ready again in the next one
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
		d.set("hp", bar(w.stats.health, w.stats.healthMax));
		d.set("mp", bar(fmaxf(0.0f, w.stats.magicka), w.stats.magickaMax));
		d.set("ft", bar(w.stats.fatigue, w.stats.fatigueMax));
		std::string s = describe(d);
		if (s != og->dynamicSaid)
		{
			og->dynamicSaid = s;
			on.sendEvent("PlayerStatsDynamic", d);
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
			counts[lowerCase(it.id)] += it.count;
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
			have.push_back(lowerCase(id));
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
			og->party = e.body.stringAt("mode") == "party";
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
	}
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
		BodyAct act;
		act.moveY = r.last.animVel > 0.05f ? 1.0f : 0.0f;
		act.airborne = r.last.flags & 8;
		float speed = r.last.animVel * w.player.runSpeed * 0.5f;
		b.update(w, dt, speed, false, r.last.flags & 2, weaponOut, act);
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

void Session::onlineFree()
{
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
		for (auto& f : fr)
		{
			rows.push_back(f.name);
			right.push_back(f.kind == "request" ? "wants to be friends" : f.kind == "invite" ? "invites you"
				: f.online ? "online" : "offline");
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
	}
	if (uiButton(4, 208, 100, 28, "Say"))
		wantText = TEXT_CHAT;
	if (uiButton(216, 208, 100, 28, "Back") || (uiIn().down & KEY_B))
		closeScreen();
}
