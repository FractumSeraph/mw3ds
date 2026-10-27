// Bloodmoon's werewolf and the other script calls its scripts need (FractumSeraph/mw3ds): the player's change of form
// (OpenMW's MechanicsManager::setWerewolf and actorutil's werewolf stats), GetWeaponType, SetScale on an actor
#include <algorithm>
#include <cmath>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/magiceffects.hpp"

// BecomeWerewolf / UndoWerewolf on the player. Becoming one: whatever was worn comes off and the beast's robe goes on
// (Bloodmoon's WerewolfRobe, as OpenMW does it), the temporary spells end, the inventory and magic close, the stats
// become the beast's (World::recomputeStats), and anyone who sees it knows: PCKnownWerewolf, and a bounty when one of
// them would report it (iWereWolfBounty). Changing back takes the robe off and restores the stats.
void Session::setPlayerWerewolf(bool on)
{
	if (w.werewolf == on)
		return;
	w.werewolf = on;
	static const char* kRobe = "werewolfrobe";
	if (on)
	{
		for (auto& it : w.inventory)
			if (it.equipped)
			{
				it.equipped = false;
				w.setItemScriptLocal(it.id, "onpcequip", 0.0f);
			}
		if (w.game.object(kRobe))
		{
			w.addItem(kRobe, 1);
			for (size_t i = 0; i < w.inventory.size(); i++)
				if (w.inventory[i].id == kRobe)
				{
					w.inventory[i].equipped = true;
					break;
				}
		}
		w.effects.clear();               // (OpenMW purges the temporary active spells)
		if (screen == SCR_INVENTORY || screen == SCR_MAGIC || screen == SCR_SPELLS)
			closeScreen();
	}
	else
	{
		w.werewolfAcrobatics = false;
		for (auto& it : w.inventory)
			if (it.id == kRobe)
				it.equipped = false;
		w.removeItem(kRobe, 1);
	}
	w.recomputeKeepingBars();
	logf("werewolf: the player %s", on ? "becomes a werewolf" : "is human again");
	if (!on)
		return;
	// Witnesses: a person who can see the player, within the alarm radius (OpenMW's getLOS and awarenessCheck)
	float radius = w.game.gmstf("falarmradius", 2000.0f);
	bool detected = false, reported = false;
	float eye[3] = { w.player.feet[0], w.player.feet[1], w.player.feet[2] + 100.0f };
	for (int i : w.loadedActors)
	{
		const Ref& r = w.refs[i];
		if (r.recType != REC_NPC_ || r.dead || !w.active(i) || w.distanceToPlayer(i) > radius)
			continue;
		float head[3] = { r.pos[0], r.pos[1], r.pos[2] + 110.0f };
		if (!w.lineOfSight(head, eye) || !awarenessCheck(i))
			continue;
		detected = true;
		if ((r.alarm >= 0 ? r.alarm : w.game.actors[r.actor].alarm) > 0)
		{
			reported = true;
			break;
		}
	}
	if (!detected)
		return;
	notify(w.game.gmst("swerewolfalarmmessage", "You have been detected changing into a werewolf."));
	w.globals["pcknownwerewolf"] = 1.0f;
	if (reported)
	{
		w.bounty = std::max(0, w.bounty + (int)w.game.gmstf("iwerewolfbounty", 1000.0f));
		w.globals["pccrimelevel"] = (float)w.bounty;
	}
}

// recomputeStats with the bars kept: health moves by what its maximum moved (OpenMW's DynamicStat::setBase, so the
// beast's fWereWolfHealth adds to the health the player has), magicka and fatigue only held to their maximum
void World::recomputeKeepingBars()
{
	float h = stats.health, hMax = stats.healthMax, m = stats.magicka, f = stats.fatigue;
	recomputeStats();
	stats.health = fminf(stats.healthMax, h + (stats.healthMax - hMax));
	stats.magicka = fminf(m, stats.magickaMax);
	stats.fatigue = fminf(f, stats.fatigueMax);
}

// GetWeaponType: the type of what is in the right hand (WPDT), -1 nothing (or not a weapon), -2 a lockpick, -3 a probe
int World::weaponTypeOf(int ref) const
{
	if (ref < 0)
	{
		for (const auto& it : inventory)
		{
			if (!it.equipped)
				continue;
			const Object* o = game.object(it.id);
			if (!o)
				continue;
			if (o->recType == REC_WEAP && o->subtype < WEAP_ARROW)
				return o->subtype;
			if (o->recType == REC_LOCK)
				return -2;
			if (o->recType == REC_PROB)
				return -3;
		}
		return -1;
	}
	const Ref& r = refs[ref];
	if (r.actor < 0)
		return -1;
	const Object* o = game.object(game.actors[r.actor].weapon);
	return o && o->recType == REC_WEAP ? o->subtype : -1;
}

// An actor's drawn size: its placement's axes scaled by `factor` (the race's height stays in them)
static void rescaleActor(World& w, int ri, float factor)
{
	Actor* a = w.actorOf(ri);
	if (!a || factor <= 0.0f || factor == 1.0f)
		return;
	for (int row = 0; row < 3; row++)
		for (int col = 0; col < 3; col++)
			a->place[row * 4 + col] *= factor;
}

// SetScale / ModScale (OpenMW's CellRef::setScale holds it to 0.5 .. 2): an actor is drawn at it at once, and again
// whenever its cell is read (applyScriptScale); a static's mesh keeps the size it was baked at
void World::setRefScale(int ri, float scale)
{
	Ref& r = refs[ri];
	scale = std::min(2.0f, std::max(0.5f, scale));
	if (scale == r.scale)
		return;
	if (r.scaleOrig <= 0.0f)
		r.scaleOrig = r.scale > 0.0f ? r.scale : 1.0f;
	float factor = r.scale > 0.0f ? scale / r.scale : scale;
	r.scale = scale;
	rescaleActor(*this, ri, factor);
	if (r.actor < 0)
		logf("script: SetScale on %s (not an actor): its mesh keeps its size", r.id.c_str());
}

void World::applyScriptScale(int ri)
{
	const Ref& r = refs[ri];
	if (r.scaleOrig > 0.0f && r.scale != r.scaleOrig)
		rescaleActor(*this, ri, r.scale / r.scaleOrig);
}
