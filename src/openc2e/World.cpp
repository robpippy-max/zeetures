/*
 *  World.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Tue May 25 2004.
 *  Copyright (c) 2004-2008 Alyssa Milburn. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 */

#include "Profile.h"
#include "common/devenv.h"
#include "World.h"
#include "SkyTint.h"

#include "Bubble.h"
#include "creatures/Thoughts.h"
#include "creatures/Pheromones.h"
#include "creatures/Disease.h"
#include "creatures/Exposure.h"
#include "creatures/Relationships.h"
#include "creatures/SkeletalCreature.h"
#include "WorldSave.h"

#include <algorithm>
#include "Vehicle.h"

#include <cmath>
#include "AgentHelpers.h"

#include "Camera.h"
#include "Catalogue.h"
#include "CompoundAgent.h" // for setFocus
#include "Engine.h"
#include "Map.h"
#include "MetaRoom.h"
#include "PathResolver.h"
#include "PointerAgent.h"
#include "BirthPortrait.h"
#include "Room.h"
#include "SFCFile.h"
#include "Scriptorium.h"
#include "SpritePart.h"
#include "caosScript.h"
#include "caosVM.h"
#include "common/Random.h"
#include "common/backend/Backend.h"
#include "common/creaturesImage.h"
#include "common/io/FileReader.h"
#include "common/throw_ifnot.h"
#include "creatures/CreatureAgent.h"
#include "creatures/oldCreature.h"
#include "creatures/oldBrain.h"
#include "fileformats/genomeFile.h"
#include "historyManager.h"
#include "imageManager.h"
#include "prayManager.h"

#include <cassert>
#include <filesystem>
#include <fmt/core.h>
#include <limits.h> // for MAXINT
#include <memory>
#include <stdint.h>
namespace fs = std::filesystem;

struct scriptevent {
	unsigned short scriptno;
	AgentRef agent, from;
	caosValue p[2];
};

World world;

World::World() {
	ticktime = 50;
	tickcount = 0;
	worldtickcount = 0;
	race = 50; // sensible default?
	pace = 0.0f; // sensible default?
	quitting = saving = false;
	theHand = 0;
	showrooms = false;
	autokill = false;
	autostop = false;

	gallery.reset(new imageManager());
	history.reset(new historyManager());
	map.reset(new Map());
	praymanager.reset(new prayManager());
	scriptorium.reset(new Scriptorium());
}

World::~World() {
	agents.clear();
	for (auto& i : vmpool)
		delete i;
}

// annoyingly, if we put this in the constructor, the catalogue isn't available yet
void World::init() {
	// First, try initialising the mouse cursor from the catalogue tag.
	if (engine.version > 2 && catalogue.hasTag("Pointer Information")) {
		const std::vector<std::string>& pointerinfo = catalogue.getTag("Pointer Information");
		if (pointerinfo.size() >= 3) {
			std::shared_ptr<creaturesImage> img = gallery->getImage(pointerinfo[2]);
			if (img) {
				theHand = new PointerAgent(pointerinfo[2]);
				int family, genus, species;
				if (sscanf(pointerinfo[0].c_str(), "%d %d %d", &family, &genus, &species) == 3)
					theHand->setClassifier(family, genus, species);
				int hotspotx, hotspoty;
				if (sscanf(pointerinfo[1].c_str(), "%d %d", &hotspotx, &hotspoty) == 2)
					theHand->setHotspot(hotspotx, hotspoty);
				if (engine.gametype == "sm" && pointerinfo.size() >= 5) {
					// TODO: seamonkeys has 'numImages baseImage' too
					int blockwidth, blockheight;
					if (sscanf(pointerinfo[4].c_str(), "%d %d", &blockwidth, &blockheight) == 2) {
						img->setBlockSize(blockwidth, blockheight);
					}
				}
				theHand->finishInit();
			} else {
				fmt::print("There was a seemingly-useful \"Pointer Information\" catalogue tag provided, but sprite file '{}' doesn't exist!\n", pointerinfo[2]);
			}
		}
	}

	// If for some reason we failed to do that (missing/bad catalogue tag? missing file?), try falling back to a sane default.
	if (!theHand) {
		std::shared_ptr<creaturesImage> img;
		if (engine.gametype == "c3")
			img = gallery->getImage("hand"); // as used in C3 and DS
		else
			img = gallery->getImage("syst"); // as used in C1, C2 and CV
		if (img) {
			theHand = new PointerAgent(img->getName());
			if (engine.version > 2) {
				theHand->finishInit(); // SFCFile handles this for c1/c2
				fmt::print("Warning: No valid \"Pointer Information\" catalogue tag, defaulting to '{}'.\n", img->getName());
			}
		} else {
			if (engine.version > 2)
				fmt::print("Couldn't find a valid \"Pointer Information\" catalogue tag, and couldn't find a pointer sprite, so not creating the pointer agent.\n");
			else
				fmt::print("Couldn't find a pointer sprite, so not creating the pointer agent.\n");
		}
	}

	// *** set defaults for non-zero GAME engine variables
	// TODO: this should be doing during world init, rather than global init
	// TODO: not complete
	caosValue v;
	variables.clear();

	// core engine bits
	v.setInt(1);
	variables["engine_debug_keys"] = v;
	v.setInt(1);
	variables["engine_full_screen_toggle"] = v;
	v.setInt(9998);
	variables["engine_plane_for_lines"] = v;
	v.setInt(6);
	variables["engine_zlib_compression"] = v;

	// creature pregnancy
	v.setInt(1);
	variables["engine_multiple_birth_maximum"] = v;
	v.setFloat(0.5f);
	variables["engine_multiple_birth_identical_chance"] = v;

	// port lines
	v.setFloat(600.0f);
	variables["engine_distance_before_port_line_warns"] = v;
	v.setFloat(800.0f);
	variables["engine_distance_before_port_line_snaps"] = v;

	// adjust to default tick rate for C1/C2 if necessary
	if (engine.version < 3)
		ticktime = 100;

	timeofday = dayofseason = season = year = 0;
}

void World::shutdown() {
	agents.clear();
	map->Reset();
}

caosVM* World::getVM(Agent* a) {
	if (vmpool.empty()) {
		return new caosVM(a);
	} else {
		caosVM* x = vmpool.back();
		vmpool.pop_back();
		x->setOwner(a);
		x->resetScriptState();
		return x;
	}
}

void World::freeVM(caosVM* v) {
	// we don't reset the script state here because caosVM might be in our call stack and we don't want to reset the VM from under itself
	v->setOwner(0);
	vmpool.push_back(v);
}

void World::queueScript(unsigned short event, AgentRef agent, AgentRef from, caosValue p0, caosValue p1) {
	scriptevent e;

	assert(agent);

	e.scriptno = event;
	e.agent = agent;
	e.from = from;
	e.p[0] = p0;
	e.p[1] = p1;

	scriptqueue.push_back(e);
}

// TODO: eventually, the part should be referenced via a weak_ptr, maaaaybe?
void World::setFocus(CompoundPart* p) {
	assert(!p || p->canGainFocus());

	// Unfocus the current agent. Not sure if c2e always does this (what if the agent/part is bad?).
	if (focusagent) {
		CompoundAgent* c = dynamic_cast<CompoundAgent*>(focusagent.get());
		assert(c);
		CompoundPart* p = c->part(focuspart);
		if (p && p->canGainFocus())
			p->loseFocus();
	}

	if (!p)
		focusagent.clear();
	else {
		p->gainFocus();
		focusagent = p->getParent();
		focuspart = p->id;
	}
}

void World::tick() {
	if (saving) {
	} // TODO: save
	if (quitting) {
		// the world stays whole: it is saved after this, and shutdown()
		// clears the agents. (They used to be cleared here, before the save:
		// File > Exit saved an empty world.)
		engine.done = true;
		return;
	}

	// remember where vehicles, and creatures, start this tick, for smooth drawing
	for (auto& a : agents) {
		if (auto* v = dynamic_cast<Vehicle*>(a.get())) {
			v->tickstartx = v->x;
			v->tickstarty = v->y;
		} else if (auto* sc = dynamic_cast<SkeletalCreature*>(a.get())) {
			sc->tickstartx = sc->x;
			sc->tickstarty = sc->y;
		}
	}

	// Tick all agents, deleting as necessary.
	std::list<std::shared_ptr<Agent> >::iterator i = agents.begin();
	while (i != agents.end()) {
		std::shared_ptr<Agent> a = *i;
		if (!a) {
			std::list<std::shared_ptr<Agent> >::iterator i2 = i;
			i2++;
			agents.erase(i);
			i = i2;
			continue;
		}
		i++;
		if (Profile::enabled()) {
			PROFILE_SCOPE(a->family == 4 ? "agents: creatures (all)" : a->family == 3 ? "agents: vehicles" : "agents: objects");
			a->tick();
		} else
			a->tick();
	}

	// Process the script queue.
	PROFILE_SCOPE("world: script queue and the rest");
	std::list<scriptevent> newqueue;
	for (auto& i : scriptqueue) {
		std::shared_ptr<Agent> agent = i.agent.lock();
		if (agent) {
			if (engine.version < 3) {
				// only try running a collision script if the agent doesn't have a running script
				// TODO: we don't really understand how script interruption in c1/c2 works
				if (agent->vm && !agent->vm->stopped() && i.scriptno == 6) {
					continue;
				}
			}
			agent->fireScript(i.scriptno, i.from, i.p[0], i.p[1]);
		}
	}
	scriptqueue.clear();
	scriptqueue = newqueue;

	// developer aid: OPENC2E_TEST_BIG_MOVE=1 has the engine itself move the
	// selected creature 400 px up at tick 100 (checks BIG MOVE's stack line)
	static bool testBigMove = devenv("OPENC2E_TEST_BIG_MOVE") != nullptr;
	if (testBigMove && tickcount == 100) {
		if (std::shared_ptr<Agent> c = selectedcreature.lock())
			c->moveTo(c->x, c->y - 400);
	}

	// developer aid: OPENC2E_TEST_PUSH=1 shoves the selected creature east
	// once, at tick 100 (the push hand)
	static bool testPush = devenv("OPENC2E_TEST_PUSH") != nullptr;
	// (=-1: west; =w: west every 20 ticks from tick 100, against a wall)
	if (testPush && selectedcreature && (tickcount == 100 || (std::string(devenv("OPENC2E_TEST_PUSH")) == "w" && tickcount > 100 && tickcount % 20 == 0))) {
		std::string tp = devenv("OPENC2E_TEST_PUSH");
		shoveCreature(selectedcreature.get(), tp == "-1" || tp == "w" ? -1 : 1);
		fmt::print("* TEST PUSH at {:.0f},{:.0f}\n", selectedcreature->x, selectedcreature->y);
	}

	// developer aid: OPENC2E_TEST_HAND_PUSH="x y" turns the toolbar's push
	// hand on and keeps the hand at that place (for snapshots of its pose
	// and arrows)
	if (const char* hp = devenv("OPENC2E_TEST_HAND_PUSH")) {
		float hx, hy;
		if (sscanf(hp, "%f %f", &hx, &hy) == 2 && hand()) {
			handPush = true;
			hand()->moveTo(hx, hy);
			// (and plays the shove west at tick 100, east at 120, printing
			// the hand's pose each tick)
			if (tickcount == 100 || tickcount == 120)
				hand()->playShove(tickcount == 100 ? -1 : 1);
			if (tickcount >= 100 && tickcount < 130)
				if (auto* sp = dynamic_cast<SpritePart*>(hand()->part(0)))
					fmt::print("* TEST SHOVE tick {} pose {}{}\n", tickcount, sp->getPose(), hand()->draw_mirrored ? " mirrored" : "");
		}
	}
	// developer aid: OPENC2E_TEST_HAND_PICKUP="family genus [species]" has the hand
	// pick up the first such object 100 ticks in, hold it beside the egg
	// pile's spot, and print where it holds it (the hand's carry point, the
	// object's place and picture size)
	if (const char* hp = devenv("OPENC2E_TEST_HAND_PICKUP")) {
		static unsigned int s_from = tickcount;
		static AgentRef s_held;
		int f, g, sp = -1;
		// ("carried": the first object a creature is holding, to check that
		// the creature lets go of it)
		bool fromCreature = std::string(hp) == "carried";
		static AgentRef s_holder;
		if ((fromCreature || sscanf(hp, "%d %d %d", &f, &g, &sp) >= 2) && hand()) {
			if (!s_held && tickcount >= s_from + 100 && (fromCreature || tickcount == s_from + 100))
				for (auto& a : agents)
					if (a && (fromCreature ? (a->carriedby && dynamic_cast<CreatureAgent*>(a->carriedby.get())) : (a->family == f && a->genus == g && (sp < 0 || a->species == sp) && !a->carriedby))) {
						if (fromCreature) {
							s_holder = a->carriedby;
							s_from = tickcount - 100;
						}
						hand()->moveTo(a->x, a->y - 40);
						a->fireScript(4, hand(), {}, {});
						s_held = a;
						break;
					}
			if (s_held && tickcount > s_from + 100 && tickcount < s_from + 400) {
				hand()->moveTo(5100, 620);
				if ((tickcount - s_from) % 50 == 0)
					fmt::print("* TEST HAND PICKUP hand at {:.0f},{:.0f} (pointer {},{}) holds {} {} {} at {:.0f},{:.0f} size {}x{}\n", hand()->x, hand()->y,
						hand()->pointerX(), hand()->pointerY(), (int)s_held->family, (int)s_held->genus, s_held->species, s_held->x, s_held->y,
						s_held->getWidth(), s_held->getHeight());
				if (s_holder && (tickcount - s_from) % 50 == 0)
					fmt::print("* TEST HAND PICKUP the creature that held it still holds it: {}; carried by the hand: {}\n",
						s_holder->carrying == s_held ? "yes" : "no", s_held->carriedby.get() == (Agent*)hand() ? "yes" : "no");
			}
		}
	}
	// developer aid: OPENC2E_TEST_HAND_INVISIBLE="x y" turns the toolbar's
	// invisible hand on and keeps the hand at that place (for snapshots)
	if (const char* hi = devenv("OPENC2E_TEST_HAND_INVISIBLE")) {
		float hx, hy;
		if (sscanf(hi, "%f %f", &hx, &hy) == 2 && hand()) {
			handInvisible = true;
			hand()->moveTo(hx, hy);
		}
	}

	// C2's halo (2 1 3, the toolbar's halo button) puts itself over the
	// middle of the selected creature's bounding box, which is off its head
	// when it faces sideways or crouches (owner, 2026-11-09): after the
	// scripts, set it over the middle of the head picture instead
	if (engine.version == 2) {
		if (auto* sc = dynamic_cast<SkeletalCreature*>(selectedcreature.get())) {
			float hx, hy;
			if (sc->headTop(hx, hy))
				for (auto& a : agents)
					if (a && a->family == 2 && a->genus == 1 && a->species == 3 && !a->isDying())
						a->moveTo(hx - a->getWidth() / 2.0f, hy - a->getHeight() - 2);
		}
	}

	tickcount++;
	worldtickcount++;

	BirthPortrait::tick(); // portraits of creatures that have just hatched

	if (engine.version == 2) {
		if (worldtickcount % 3600 == 0) {
			timeofday++;
			if (timeofday == 5) { // 5 parts of the day
				timeofday = 0;
				dayofseason++;
			}
			if (dayofseason == 4) { // 4 days per season
				dayofseason = 0;
				season++;
			}
			if (season == 4) { // 4 seasons per year
				season = 0;
				year++;
			}
		}
	}

	{
		PROFILE_SCOPE("world: map");
		world.map->tick();
	}
	if (engine.version == 2) {
		{
			PROFILE_SCOPE("world: food scent");
			tickFoodScent();
		}
		{
			PROFILE_SCOPE("world: pheromones");
			Pheromones::tick();
		}
		{
			PROFILE_SCOPE("world: disease");
			Disease::tick();
		}
		Exposure::tick();
		tickBreathBubbles();
		{
			PROFILE_SCOPE("world: relationships");
			Relationships::tick();
		}
		{
			PROFILE_SCOPE("world: climate");
			tickClimate();
			tickPressure();
			tickRadiation();
		}
		{
			PROFILE_SCOPE("world: nutrients");
			tickNutrients();
		}
		{
			PROFILE_SCOPE("world: daylight");
			tickDaylight();
		}
	}
	if (engine.version < 3)
		WorldSave::TickLifeStageChanges();

	// developer aid: OPENC2E_DUMP_AGENTS="x0 y0 x1 y1" prints, once after
	// load, every agent overlapping that box: class, plane, size and the
	// sprite file of each part
	static const char* agentBox = devenv("OPENC2E_DUMP_AGENTS");
	static bool agentsDumped = false;
	if (agentBox && !agentsDumped && tickcount > 5) {
		agentsDumped = true;
		int bx0, by0, bx1, by1;
		if (sscanf(agentBox, "%d %d %d %d", &bx0, &by0, &bx1, &by1) == 4) {
			for (auto& a : agents) {
				if (!a || a->x > bx1 || a->x + a->getWidth() < bx0 || a->y > by1 || a->y + a->getHeight() < by0)
					continue;
				std::string sprites;
				{
					auto* c = dynamic_cast<CompoundAgent*>(a.get());
					unsigned int n = c ? c->partCount() : 1;
					for (unsigned int i = 0; i < n; i++) {
						auto* sp = dynamic_cast<SpritePart*>(c ? c->part(i) : a->part(0));
						if (sp && sp->getSprite())
							sprites += fmt::format(" part {} {} img {} z {} at +{},+{}", i, sp->getSprite()->getName(), sp->getFirstImg() + sp->getPose(), sp->zorder, sp->x, sp->y);
					}
				}
				fmt::print("* AGENT {} {} {} plane {} at ({:.0f},{:.0f}) {}x{}{}\n", (int)a->family, (int)a->genus, a->species, a->zorder, a->x, a->y, a->getWidth(), a->getHeight(), sprites);
			}
		}
	}

	// C2: free objects that have left the room system (fallen out of the
	// world: a beach ball and two critters lay below the map's bottom edge at
	// the east-west seam, owner 2026-11-10) are put back on the floor of the
	// nearest room above them, within 600 px. Only objects that collide with
	// the rooms (attr 64; the critters fly by their own scripts, without
	// physics), not carried ones, nor ones in vehicles; every 100 ticks.
	//
	// The same for objects and creatures in a sealed pocket: a room of solid
	// rock in the room map, every doorway shut, when the world started and
	// still. Nothing gets out of one (owner, 2026-11-10: a ball, a bat and a
	// critter's leaving lay under the bamboo-cage lift's floor in the east
	// caves, room 48, below the map's own ground; a bat flies home at dawn
	// through the rock with its collisions off and may wake in such a
	// pocket). The world has about 30 of them; a room whose doorways a script
	// opens and shuts (an airlock) is not one unless both were shut at the
	// start and still are.
	// C2: a metabolism stalled with fuel in store (ATP below what the
	// cheapest working organ needs, so no organ can run, the one that makes
	// ATP included) stays stalled: the creature faints and dies, as in the
	// real game, where a newborn read over DDE froze at ATP 0 with glucose
	// and pyruvate in store (2026-11-18). This used to be restarted with the
	// genome's starting ATP every 100 ticks (642318a3, c09c8452), which kept
	// healthy creatures alive while brain relay cells passed on too little
	// (fixed, b74ca45c) and since then mostly kept newborns with lethal
	// mutations on life support (e.g. the organ that makes PGK switched off
	// by a mutated receptor). Owner, 2026-11-19: drop it.
	// developer aid: OPENC2E_DUMP_ORGANS=MONIKER prints that creature's
	// organs once (energy cost, clock rate, life force, and the reactions
	// that make or use ATP and ADP)
	// (OPENC2E_DUMP_ORGANS=OV97: the creature a script marks with OV97 = 1,
	// once it exists, e.g. one just made from a genome file)
	static bool organsDumped = false;
	if (static const char* dmEnv = devenv("OPENC2E_DUMP_ORGANS"); dmEnv && !organsDumped && tickcount % 100 == 70) {
		bool byMark = std::string(dmEnv) == "OV97";
		for (auto& a : agents) {
			auto* ca = a ? dynamic_cast<CreatureAgent*>(a.get()) : nullptr;
			auto* c2 = ca ? dynamic_cast<c2Creature*>(ca->getCreature()) : nullptr;
			if (!c2)
				continue;
			if (byMark ? !(a->var[97].hasInt() && a->var[97].getInt() == 1) : history->findMoniker(c2->getGenome()) != dmEnv)
				continue;
			organsDumped = true;
			std::string dm = byMark ? history->findMoniker(c2->getGenome()) : std::string(dmEnv);
			int k = 0;
			for (auto& o : c2->getOrgans()) {
				std::string rx;
				for (auto& r : o->getReactions()) {
					auto* g = r->data;
					bool atp = devenv("OPENC2E_DUMP_ORGANS_ALL") != nullptr; // (every reaction, not only those on ATP/ADP)
					for (int i = 0; i < 4; i++)
						if (g->reactant[i] == 99 || g->reactant[i] == 100)
							atp = true;
					if (atp)
						rx += fmt::format(" [{}x{} + {}x{} -> {}x{} + {}x{} rate {}]", (int)g->quantity[0], (int)g->reactant[0], (int)g->quantity[1], (int)g->reactant[1],
							(int)g->quantity[2], (int)g->reactant[2], (int)g->quantity[3], (int)g->reactant[3], (int)g->rate);
				}
				for (gene* og : o->genes)
					if (auto* rc = dynamic_cast<bioReceptorGene*>(og); rc && rc->organ == 0 && rc->tissue == 9)
						rx += fmt::format(" <receptor {} -> 0,9,{}>", (int)rc->chemical, (int)rc->locus);
					else if (rc && rc->organ == 2)
						rx += fmt::format(" <organ receptor: chemical {} -> {} threshold {} nominal {} gain {}{}{} on at stage {}{}>", (int)rc->chemical,
							rc->locus == 0 ? "clock rate" : rc->locus == 1 ? "repair rate" : "injury", (int)rc->threshold, (int)rc->nominal, (int)rc->gain,
							rc->inverted ? " inverted" : "", rc->digital ? " digital" : "", (int)rc->header.switchontime, rc->header.flags.notexpressed ? " not expressed" : "");
					else if (auto* e = dynamic_cast<bioEmitterGene*>(og))
						rx += fmt::format(" {{emits {} from {},{},{} rate {} gain {}}}", (int)e->chemical, (int)e->organ, (int)e->tissue, (int)e->locus, (int)e->rate, (int)e->gain);
				fmt::print("* ORGAN {} #{} cost {} clock {} life {:.0f}/{:.0f} reactions {}{}\n", dm, k++, (int)o->getEnergyCost(), (int)o->getClockRate(),
					o->getLongTermLifeforce(), o->getInitialLifeforce(), o->getReactionCount(), rx);
			}
			if (oldBrain* b = c2->getBrain())
				if (oldLobe* l9 = b->getLobeByTissue(9))
					fmt::print("* LOBE {} tissue 9: neurons {} threshold {} leakage {} gain {} rest {}\n", dm, l9->getNoNeurons(), (int)*l9->getThresholdPointer(),
						(int)*l9->getLeakageRatePointer(), (int)*l9->getInputGainPointer(), (int)l9->getGene()->reststate);
			// and the genes that make or set the energy chain's chemicals
			// (glucose 70, pyruvate 69, energy 98, ATP 99, ADP 100, the
			// glycolysis enzyme phosphoglycerokinase 179)
			auto chainChem = [](int c) { return c == 70 || c == 69 || c == 98 || c == 99 || c == 100 || c == 179; };
			for (auto& gn : c2->getGenome()->genes) {
				if (auto* e = dynamic_cast<bioEmitterGene*>(gn.get()); e && chainChem(e->chemical))
					fmt::print("* GENE {} emitter: chemical {} from organ {} tissue {} locus {} threshold {} rate {} gain {}{}{}{}\n", dm, (int)e->chemical,
						(int)e->organ, (int)e->tissue, (int)e->locus, (int)e->threshold, (int)e->rate, (int)e->gain, e->clear ? " clear" : "", e->digital ? " digital" : "", e->invert ? " invert" : "");
				if (auto* ic = dynamic_cast<bioInitialConcentrationGene*>(gn.get()); ic && chainChem(ic->chemical))
					fmt::print("* GENE {} initial: chemical {} quantity {}\n", dm, (int)ic->chemical, (int)ic->quantity);
				// what feeds the brain's relay lobe 9 (whose cell 13 the
				// phosphoglycerokinase emitter reads)
				if (auto* rc = dynamic_cast<bioReceptorGene*>(gn.get()); rc && rc->organ == 1 && rc->tissue == 0)
					fmt::print("* GENE {} life-stage receptor: chemical {} into locus {} threshold {} nominal {} gain {}{}{}\n", dm, (int)rc->chemical,
						(int)rc->locus, (int)rc->threshold, (int)rc->nominal, (int)rc->gain, rc->inverted ? " inverted" : "", rc->digital ? " digital" : "");
				if (auto* rc = dynamic_cast<bioReceptorGene*>(gn.get()); rc && rc->organ == 0 && rc->tissue == 9)
					fmt::print("* GENE {} receptor: chemical {} into organ 0 tissue 9 locus {} threshold {} nominal {} gain {}{}{}\n", dm, (int)rc->chemical,
						(int)rc->locus, (int)rc->threshold, (int)rc->nominal, (int)rc->gain, rc->inverted ? " inverted" : "", rc->digital ? " digital" : "");
				if (auto* hl = dynamic_cast<bioHalfLivesGene*>(gn.get()))
					fmt::print("* GENE {} half-lives: glucose {} pyruvate {} energy {} ATP {} ADP {} PGK {}\n", dm, (int)hl->halflives[70], (int)hl->halflives[69],
						(int)hl->halflives[98], (int)hl->halflives[99], (int)hl->halflives[100], (int)hl->halflives[179]);
			}
		}
	}
	if (engine.version == 2 && map && tickcount % 100 == 37) {
		static const bool watchRescue = devenv("OPENC2E_WATCH_SUNK") != nullptr;
		if (!sealedRoomsKnown) {
			sealedRoomsKnown = true;
			// (a shut room with a floor line or one creatures can be dropped
			// in is a real, closed place: the terrarium of critters and
			// plants in the west caves, room 85, is one)
			for (auto& r : map->getRooms())
				if (r->allDoorsShut() && r->floorpoints.empty() && r->dropstatus == 0)
					sealedAtStart.insert(r->id);
		}
		auto sealed = [&](const std::shared_ptr<Room>& r) {
			return r && sealedAtStart.count(r->id) && r->allDoorsShut();
		};
		for (auto& a : agents) {
			if (!a || a->isDying() || a->carriedby || a->invehicle)
				continue;
			bool creature = a->family == 4;
			if (!creature && (a->family != 2 || !(a->attr & 64)))
				continue;
			// (a dead creature's body rising out of the world passes through
			// rock on purpose: see oldCreature::tickBody)
			if (creature && !(a->attr & 64))
				continue;
			float cx = a->x + a->getWidth() / 2.0f, bottom = a->y + a->getHeight();
			// across the seam of a wrapping world, the middle is on the other
			// side: a creature against the library's west wall (x ~0) has its
			// top-left wrapped to ~8320, so its middle fell past the east edge,
			// inside the sealed pillar there (room 213), and this lifted it
			// 490 px into room 175 (2026-11-13, three times in a night)
			if (MetaRoom* m = map->metaRoomAt(a->x, a->y); m && m->wraparound()) {
				if (cx >= m->x() + (int)m->width())
					cx -= m->width();
				else if (cx < m->x())
					cx += m->width();
			}
			std::shared_ptr<Room> at = map->roomAt(cx, bottom - 1);
			std::shared_ptr<Room> mid = map->roomAt(cx, a->y + a->getHeight() / 2.0f);
			bool outside = !at && !mid;
			bool pocket = at && sealed(at) && (!mid || sealed(mid));
			if (!(creature ? pocket : (outside || pocket)))
				continue;
			// and a creature whose feet stand in an open room is where it should be
			if (auto* sc = dynamic_cast<SkeletalCreature*>(a.get()))
				if (std::shared_ptr<Room> fr = sc->footRoom(); fr && !sealed(fr))
					continue;
			for (float yy = bottom - 2; yy > bottom - 600 && yy > 0; yy -= 4) {
				std::shared_ptr<Room> r = map->roomAt(cx, yy);
				if (!r || r == at || sealed(r))
					continue;
				float floory = r->floorYatX(cx);
				a->velx = 0;
				a->vely = 0;
				a->moveTo(a->x, floory - a->getHeight());
				if (auto* sc = dynamic_cast<SkeletalCreature*>(a.get()))
					sc->scriptMoved();
				if (watchRescue)
					fmt::print("* RESCUED {} {} {} from {} at ({:.0f}, {:.0f}) onto room {} floor {:.0f}\n", (int)a->family,
						(int)a->genus, a->species, outside ? std::string("outside the rooms") : fmt::format("sealed room {}", at->id),
						cx, bottom, r->id, floory);
				break;
			}
		}
	}

	// developer aid: OPENC2E_WATCH_SUNK=N lists, every N ticks, creatures
	// and objects sunk into a solid floor (bottom more than 20 px below the
	// floor of their room, where that floor is solid) or outside the rooms
	static const int sunkEvery = devenv("OPENC2E_WATCH_SUNK") ? atoi(devenv("OPENC2E_WATCH_SUNK")) : 0;
	if (sunkEvery > 0 && map && tickcount % sunkEvery == 0) {
		for (auto& a : agents) {
			if (!a || a->isDying() || a->carriedby || a->invehicle || a->family == 1)
				continue;
			float cx = a->x + a->getWidth() / 2.0f, bottom = a->y + a->getHeight();
			std::shared_ptr<Room> r = map->roomAt(cx, a->y + a->getHeight() / 2.0f);
			if (!r) {
				fmt::print("* SUNK? {} {} {} at ({:.0f}, {:.0f}): outside every room\n", (int)a->family, (int)a->genus, a->species, cx, bottom);
				continue;
			}
			float floor = r->floorYatX(cx);
			bool solid = r->floorpoints.size() ? r->floorvalue == 0 : true;
			if (solid && bottom > floor + 20.0f)
				fmt::print("* SUNK? {} {} {} attr {} unid {} at ({:.0f}, {:.0f}) size {:.0f}x{:.0f}: room {} floor {:.0f}, {:.0f} px below it\n", (int)a->family, (int)a->genus, a->species, (int)a->attr, a->getUNID(), cx, bottom, (float)a->getWidth(), (float)a->getHeight(), r->id, floor, bottom - floor);
		}
	}

	// TODO: correct behaviour? hrm :/
	world.hand()->velx = world.hand()->velx / 2.0f;
	world.hand()->vely = world.hand()->vely / 2.0f;
}

Agent* World::agentAt(unsigned int x, unsigned int y, bool obey_all_transparency, bool needs_mouseable) {
	CompoundPart* p = partAt(x, y, obey_all_transparency, needs_mouseable);
	if (p)
		return p->getParent();
	else
		return 0;
}

// The topmost creature whose picture's box contains the point (C1/C2
// selection by clicking: see PointerAgent), whatever is drawn over it.
Agent* World::creatureAt(int x, int y) {
	for (auto p : zorder) {
		Agent* parent = p->getParent();
		if (!parent || parent == theHand || parent->family != 4 || !dynamic_cast<CreatureAgent*>(parent))
			continue;
		int ax = x - (int)parent->x, ay = y - (int)parent->y;
		if (ax >= p->x && ax <= p->x + (int)p->getWidth() && ay >= p->y && ay <= p->y + (int)p->getHeight())
			return parent;
	}
	return nullptr;
}

CompoundPart* World::partAt(unsigned int x, unsigned int y, bool obey_all_transparency, bool needs_mouseable, bool needs_clickable) {
	Agent* transagent = 0;
	if (!obey_all_transparency)
		transagent = agentAt(x, y, true, needs_mouseable);

	MetaRoom* m = world.map->metaRoomAt(x, y); // for wraparound checking

	for (auto p : zorder) {
		if (p->getParent() == theHand)
			continue;

		int ax = (int)(x - p->getParent()->x);
		int ay = (int)(y - p->getParent()->y);

		// C1/C2 creatures by their drawn pictures, not their box (owner,
		// 2026-11-15: the hand's pet sign came and went): the box is the
		// creature's 50 px physics width, and a head facing out past it was
		// missed, the hand finding whatever was behind; its see-through
		// corners caught the hand where no creature is drawn
		if (engine.version <= 2)
			if (auto* sk = dynamic_cast<SkeletalCreature*>(p->getParent())) {
				float wx = (float)x;
				if (m && m->wraparound() && std::fabs(wx - sk->x) > m->width() / 2.0f)
					wx += wx < sk->x ? (float)m->width() : -(float)m->width();
				if (sk->partUnder(wx, (float)y) < 0)
					continue;
				if (needs_mouseable && !(p->getParent()->mouseable()))
					continue;
				if (needs_clickable && !(p->canClick()))
					continue;
				if (!obey_all_transparency && p->getParent() != transagent)
					continue;
				return p;
			}

		// we check the wrap too..
		if (!m || !m->wraparound() || p->x > ax + (int)m->width() || p->x + (int)p->getWidth() < ax + (int)m->width()) {
			if (p->x > ax)
				continue;
			if (p->x + (int)p->getWidth() < ax)
				continue;
		} else {
			// wrapped!
			ax += m->width();
		}

		if (p->y > ay)
			continue;
		if (p->y + (int)p->getHeight() < ay)
			continue;

		SpritePart* s = dynamic_cast<SpritePart*>(p);
		if (s && s->isTransparent() && obey_all_transparency) {
			// transparent parts in C1/C2 are scenery
			// TODO: always true? you can't sekritly set parts to be transparent in C2?
			if (engine.version < 3 || s->transparentAt(ax - s->x, ay - s->y))
				continue;
		}

		if (needs_mouseable && !(p->getParent()->mouseable()))
			continue;

		if (needs_clickable && !(p->canClick()))
			continue;

		if (!obey_all_transparency)
			if (p->getParent() != transagent)
				continue;

		return p;
	}

	return 0;
}

void World::setUNID(Agent* whofor, int unid) {
	assert(whofor->shared_from_this() == unidmap[unid].lock() || unidmap[unid].expired());
	whofor->unid = unid;
	unidmap[unid] = whofor->shared_from_this();
}

int World::newUNID(Agent* whofor) {
	do {
		int unid = rand_int32(0, std::numeric_limits<int32_t>::max());
		if (unid && unidmap[unid].expired()) {
			setUNID(whofor, unid);
			return unid;
		}
	} while (1);
}

void World::freeUNID(int unid) {
	unidmap.erase(unid);
}

std::shared_ptr<Agent> World::lookupUNID(int unid) {
	if (unid == 0)
		return std::shared_ptr<Agent>();
	return unidmap[unid].lock();
}

void World::drawWorld(Camera* cam, RenderTarget* surface, bool skip_hand, Agent* only_creature, bool bare, Agent* skip_agent) {
	assert(surface);
	surface->renderClear();

	MetaRoom* m = cam->getMetaRoom();
	if (!m) {
		// Whoops - the room we're in vanished, or maybe we were never in one?
		// Try to get a new one ...
		m = map->getFallbackMetaroom();
		if (!m)
			throw Exception("drawWorld() couldn't find any metarooms");
		cam->goToMetaRoom(m->id);
	}
	int adjustx = cam->getX();
	int adjusty = cam->getY();
	// TODO: current backgrounds is per camera (supposedly?)
	std::shared_ptr<creaturesImage> bkgd = m->getCurrentBackground();

	// TODO: work out what c2e does when it doesn't have a background..
	if (!bkgd)
		return;

	assert(bkgd->numframes() > 0);

	// draw the background
	// TODO: clear the rendertarget background to black
	const int destx = m->x() - adjustx;
	const int desty = m->y() - adjusty;
	surface->renderCreaturesImage(bkgd, 0, destx, desty);
	if (m->wraparound()) {
		// if we're on the second pass, render to the *right* of the normal area
		surface->renderCreaturesImage(bkgd, 0, destx + m->width(), desty);
	}
	// the light through the day (C2): a filter over everything in the world
	// but the hand and speech bubbles, drawn just before the first of those
	// (not in a creature's photo)
	bool lit = only_creature != nullptr;

	// vehicles and their riders glide between ticks: draw them back along
	// this tick's move, by the part of the tick not yet elapsed (not for
	// jumps)
	const float glide = 1.0f - engine.tickFraction();
	const bool smooth = SkeletalCreature::smoothMovement();
	auto glideOffset = [&](renderable* r, int& gx, int& gy) {
		gx = gy = 0;
		auto* p = dynamic_cast<CompoundPart*>(r);
		if (!p)
			return;
		Agent* a = p->getParent();
		// what a creature drawn between poses (SmoothMovement.cpp) carries,
		// its speech, and the selected one's halo (2 1 3), go with it
		if (smooth) {
			float dx, dy;
			auto shifted = [&](Agent* by, int what) {
				auto* sc = dynamic_cast<SkeletalCreature*>(by);
				if (!sc || !(what == 0 ? sc->drawnCarryShift(dx, dy) : what == 1 ? sc->drawnShift(dx, dy) : sc->drawnHeadShift(dx, dy)))
					return;
				gx = (int)std::lround(dx);
				gy = (int)std::lround(dy);
			};
			if (a->carriedby)
				shifted(a->carriedby.get(), 0);
			else if (a->floatingagent)
				shifted(a->floatingagent.get(), 1);
			else if (a->family == 2 && a->genus == 1 && a->species == 3)
				shifted(selectedcreature.get(), 2);
		}
		if (glide <= 0)
			return;
		// a creature carried along by a script (the stream under the cave
		// lift's dock moves it a few px every tick) glides too, rather than
		// jumping ten times a second (owner, 2026-11-14: "really jerky")
		if (auto* sc = dynamic_cast<SkeletalCreature*>(a); sc && !a->invehicle && !a->carriedby && sc->scriptMoveTick() > 0 &&
															 world.tickcount - sc->scriptMoveTick() <= 1) {
			float dx = sc->x - sc->tickstartx, dy = sc->y - sc->tickstarty;
			if (std::fabs(dx) > 64 || std::fabs(dy) > 64)
				return;
			gx = -(int)std::lround(dx * glide);
			gy = -(int)std::lround(dy * glide);
			return;
		}
		Vehicle* v = dynamic_cast<Vehicle*>(a->invehicle ? a->invehicle.get() : a);
		if (!v)
			return;
		float dx = v->x - v->tickstartx, dy = v->y - v->tickstarty;
		if (std::fabs(dx) > 64 || std::fabs(dy) > 64)
			return;
		gx = -(int)std::lround(dx * glide);
		gy = -(int)std::lround(dy * glide);
	};

	// the passes over a world whose ends join: where the objects are, and a
	// world's width to the right and to the left, when the view comes near
	// enough to that end to show any of them (objects may reach past the ends;
	// each pass used to be made for every object, every frame: 2026-11-21)
	const int seamReach = 2000;
	const bool passRight = m->wraparound() && (int)m->width() - (adjustx - (int)m->x()) < surface->getWidth() + seamReach;
	const bool passLeft = m->wraparound() && adjustx - (int)m->x() < seamReach;

	// render all the agents
	for (auto render : renders) {
		if (!lit) {
			auto* p = dynamic_cast<CompoundPart*>(render);
			Agent* owner = p ? p->getParent() : nullptr;
			if (owner && (owner == (Agent*)world.hand() || typeid(*owner) == typeid(Bubble))) {
				SkyTint::draw(surface, m, destx, desty);
				Thoughts::draw(surface, adjustx, adjusty, m->wraparound() ? (int)m->width() : 0, skip_agent);
				lit = true;
			}
		}
		if (skip_hand || only_creature || skip_agent) {
			auto* p = dynamic_cast<CompoundPart*>(render);
			Agent* owner = p ? p->getParent() : nullptr;
			if (skip_hand && owner && owner == (Agent*)world.hand())
				continue;
			// (the Creature's View: the creature does not see itself, nor its
			// own speech bubbles, which float on it; owner, 2026-11-17)
			if (skip_agent && owner == skip_agent)
				continue;
			if (skip_agent && owner && dynamic_cast<Bubble*>(owner) && owner->floatingagent.get() == skip_agent)
				continue;
			if (bare && only_creature && owner != only_creature)
				continue;
			// a photo (only_creature): no speech bubbles either (owner, 2026-11-11)
			if (only_creature && owner && dynamic_cast<Bubble*>(owner))
				continue;
			if (only_creature && owner && owner != only_creature) {
				auto isOther = [&](Agent* x) { return x && x != only_creature && dynamic_cast<CreatureAgent*>(x); };
				if (isOther(owner) || isOther(owner->carriedby.get()))
					continue;
			}
		}
		if (render->showOnRemoteCameras() || cam == engine.camera.get()) {
			// three-pass for wraparound rooms, the third since agents often straddle the boundary
			// TODO: same as above with background rendering
			int gx, gy;
			glideOffset(render, gx, gy);
			const int newx = -adjustx + gx, newy = -adjusty + gy;
			render->render(surface, newx, newy);
			if (passRight)
				render->render(surface, newx + (int)m->width(), newy);
			if (passLeft)
				render->render(surface, newx - (int)m->width(), newy);
		}
	}
	if (!lit) {
		SkyTint::draw(surface, m, destx, desty);
		if (!only_creature) // (not in a photo)
			Thoughts::draw(surface, adjustx, adjusty, m->wraparound() ? (int)m->width() : 0, skip_agent);
	}

	// render port connection lines. TODO: these should be rendered as some kind
	// of renderable, not directly like this.
	for (auto a : world.agents) {
		if (!a)
			continue;
		for (std::map<unsigned int, std::shared_ptr<OutputPort> >::iterator p = a->outports.begin();
			 p != a->outports.end(); p++) {
			for (PortConnectionList::iterator c = p->second->dests.begin(); c != p->second->dests.end(); c++) {
				if (!c->first)
					continue;
				InputPort* target = c->first->inports[c->second].get();
				surface->renderLine(a->x + p->second->x - adjustx, a->y + p->second->y - adjusty,
					c->first->x + target->x - adjustx, c->first->y + target->y - adjusty, Color{0, 0xff, 0, 0xff});
			}
		}
	}

	if (showrooms) {
		std::shared_ptr<Room> room_under_hand = map->roomAt(hand()->x, hand()->y);
		auto draw_room = [&](const auto& r, Color color) {
			// rooms don't wrap over the boundary, so just draw twice
			r->renderBorders(surface, adjustx, adjusty, color);
			if (m->wraparound()) {
				r->renderBorders(surface, adjustx - m->width(), adjusty, color);
			}
		};
		for (const auto& r : cam->getMetaRoom()->rooms) {
			if (!room_under_hand || (r != room_under_hand && !map->hasDoor(room_under_hand, r))) {
				draw_room(r, Color{0xff, 0xff, 0, 0xcc});
			}
		}
		if (room_under_hand) {
			for (const auto& door : room_under_hand->getDoors()) {
				draw_room(door.first, Color{0, 0xff, 0xff, 0xcc});
			}
			draw_room(room_under_hand, Color{0xff, 0, 0xff, 0xcc});
		}
	}

	// View > Scents (openc2e addition, owner's idea in pheromones-design.md,
	// 2026-11-15): each room in view tinted by how much of the chosen scent
	// it holds, faint to strong, so trails can be watched forming. Filled
	// with vertical lines (the renderer draws lines only); a scent runs 0-1.
	// (one past the last channel is food smell, World::tickFoodScent)
	if (showScent >= 0 && showScent <= Pheromones::channelCount()) {
		static const Color palette[6] = {{255, 80, 200, 0}, {80, 200, 255, 0}, {255, 180, 40, 0}, {120, 255, 120, 0}, {200, 120, 255, 0}, {255, 90, 90, 0}};
		const bool food = showScent == Pheromones::channelCount();
		Color base = food ? Color{150, 230, 60, 0} : palette[showScent % 6];
		auto levelOf = [&](const Room& r) { return food ? r.foodscent : (size_t)showScent < r.scent.size() ? r.scent[showScent] : 0.0f; };
		float vx0 = cam->getX(), vx1 = vx0 + cam->getWidth(), vy0 = cam->getY(), vy1 = vy0 + cam->getHeight();
		// scaled to the strongest room in view, so the way toward a source
		// shows wherever the scent has spread (a scent everywhere tinted
		// everything alike); rooms under a tenth of that are left clear
		MetaRoom* mr = cam->getMetaRoom();
		float peak = 0.0f;
		for (const auto& r : mr->rooms)
			if (r->x_right >= vx0 - mr->width() && r->x_left <= vx1 + mr->width() &&
				std::max(r->y_left_floor, r->y_right_floor) >= vy0 && std::min(r->y_left_ceiling, r->y_right_ceiling) <= vy1)
				peak = std::max(peak, levelOf(*r));
		auto fill = [&](const std::shared_ptr<Room>& r, float shift) {
			float level = levelOf(*r);
			if (peak < 0.005f || level < 0.1f * peak)
				return;
			float l = std::clamp(level / peak, 0.0f, 1.0f);
			Color c = base;
			c.a = (uint8_t)(30 + 140 * std::sqrt(l));
			float x0 = std::max((float)r->x_left + shift, vx0), x1 = std::min((float)r->x_right + shift, vx1);
			if (x1 <= x0 || std::min(r->y_left_ceiling, r->y_right_ceiling) > vy1 || std::max(r->y_left_floor, r->y_right_floor) < vy0)
				return;
			float w = std::max(1.0f, (float)r->x_right - (float)r->x_left);
			for (float x = std::floor(x0); x <= x1; x += 1) {
				float t = (x - shift - r->x_left) / w;
				float top = r->y_left_ceiling + (r->y_right_ceiling - (float)r->y_left_ceiling) * t;
				float bottom = r->y_left_floor + (r->y_right_floor - (float)r->y_left_floor) * t;
				surface->renderLine(x - adjustx, top - adjusty, x - adjustx, bottom - adjusty, c);
			}
		};
		MetaRoom* m = cam->getMetaRoom();
		for (const auto& r : m->rooms) {
			fill(r, 0);
			if (m->wraparound()) {
				fill(r, -(float)m->width());
				fill(r, (float)m->width());
			}
		}
	}

	if (hand()->holdingWire) {
		if (!hand()->wireOriginAgent) {
			hand()->holdingWire = 0;
		} else {
			int x, y;
			if (hand()->holdingWire == 1) {
				// holding from outport
				OutputPort* out = hand()->wireOriginAgent->outports[hand()->wireOriginID].get();
				x = out->x;
				y = out->y;
			} else {
				// holding from inport
				InputPort* in = hand()->wireOriginAgent->inports[hand()->wireOriginID].get();
				x = in->x;
				y = in->y;
			}
			surface->renderLine(x + hand()->wireOriginAgent->x - adjustx,
				y + hand()->wireOriginAgent->y - adjusty, hand()->x - adjustx, hand()->y - adjusty, Color{0, 0xff, 0, 0xff});
		}
	}
}

void World::executeInitScript(fs::path x) {
	assert(fs::exists(x));
	assert(!fs::is_directory(x));

	FileReader s(x);
	//fmt::print("executing script {}\n", x);
	//fflush(stdout); fflush(stderr);
	try {
		caosScript script(engine.gametype, x);
		script.parse(s);
		caosVM vm(0);
		script.installScripts();
		vm.runEntirely(script.installer);
	} catch (Exception& e) {
		fmt::print(stderr, "exec of \"{}\" failed due to exception {}\n", fs::path(x).filename().string(), e.prettyPrint());
	}
	fflush(stdout);
	fflush(stderr);
}

// a script held in memory (a built-in world addition; Additions.h)
void World::executeInitScriptText(const std::string& name, const std::string& text) {
	try {
		caosScript script(engine.gametype, name);
		script.parse(text);
		caosVM vm(0);
		script.installScripts();
		vm.runEntirely(script.installer);
	} catch (Exception& e) {
		fmt::print(stderr, "exec of \"{}\" failed due to exception {}\n", name, e.prettyPrint());
	}
	fflush(stdout);
	fflush(stderr);
}

void World::executeBootstrap(fs::path p) {
	if (!fs::is_directory(p)) {
		executeInitScript(p);
		return;
	}

	std::vector<fs::path> scripts;

	fs::directory_iterator fsend;
	for (fs::directory_iterator d(p); d != fsend; ++d) {
		if ((!fs::is_directory(*d)) && (d->path().extension().string() == ".cos"))
			scripts.push_back(*d);
	}

	std::sort(scripts.begin(), scripts.end());
	for (auto s : scripts) {
		executeInitScript(s);
	}
}

void World::executeBootstrap(bool switcher) {
	if (engine.version < 3) {
		// read from Eden.sfc

		if (data_directories.size() == 0)
			throw Exception("C1/2 can't run without data directories!");

		auto edenpath = findMainDirectoryFile("Eden.sfc");
		if (fs::exists(edenpath) && !fs::is_directory(edenpath)) {
			SFCFile sfc;
			FileReader f(edenpath);
			sfc.read(&f);
			sfc.copyToWorld();
			return;
		} else
			throw Exception("couldn't find file Eden.sfc, required for C1/2");
	}

	// TODO: this code is possibly wrong with multiple bootstrap directories
	std::multimap<std::string, fs::path> bootstraps;

	if (switcher) {
		executeBootstrap(getWorldSwitcherBootstrapDirectory());
	} else {
		if (engine.version == 3) {
			if (!catalogue.hasTag("patch_level")) {
				// TODO: This is a hack for DS, basically. Not sure if it works properly. - fuzzie
				engine.eame_variables["engine_no_auxiliary_bootstrap_1"] = caosValue(1);
			}
		}

		for (auto d : getBootstrapDirectories()) {
			fmt::print("* Executing scripts from: {}\n", d.string());
			executeBootstrap(d);
		}
	}
}

void World::initCatalogue() {
	auto dirs = getCatalogueDirectories();
	for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
		if (fs::exists(*it) && fs::is_directory(*it))
			catalogue.initFrom(*it, engine.language);
	}
}

void World::selectCreature(std::shared_ptr<Agent> a) {
	if (a) {
		CreatureAgent* c = dynamic_cast<CreatureAgent*>(a.get());
		THROW_IFNOT(c);
	}

	if (selectedcreature != a) {
		for (auto& agent : world.agents) {
			if (!agent)
				continue;
			agent->queueScript(120, 0, caosValue(a), caosValue(selectedcreature)); // selected creature changed
		}

		selectedcreature = a;
	}
}

std::shared_ptr<genomeFile> World::loadGenome(const std::string& moniker) {
	std::vector<fs::path> possibles = findGeneticsFiles(moniker + ".gen");
	if (possibles.empty())
		return std::shared_ptr<genomeFile>();
	auto filename = rand_choice(possibles);
	// a genome the player installed (in the user data folder) takes the place
	// of the game's own of the same name: the imported Frog Norn pack's
	// frog.gen replaces C2's (owner, 2026-11-10); before, either was chosen
	// at random each time
	const std::string user = getUserDataDir().lexically_normal().string();
	for (auto& f : possibles)
		if (f.lexically_normal().string().rfind(user, 0) == 0) {
			filename = f;
			break;
		}

	std::shared_ptr<genomeFile> p(new genomeFile());
	FileReader gfile(filename);
	gfile >> *(p.get());

	return p;
}

void World::newMoniker(std::shared_ptr<genomeFile> g, std::string genefile, AgentRef agent) {
	std::string d = history->newMoniker(g);
	world.history->getMoniker(d).addEvent(2, "", genefile);
	world.history->getMoniker(d).moveToAgent(agent);
}

std::string World::generateMoniker(std::string basename) {
	if (engine.version < 3) {
		/* old-style monikers are four characters in a format like 9GVC */
		unsigned int n = rand_uint32(1, 9);
		std::string moniker = std::to_string(n);
		for (unsigned int i = 0; i < 3; i++) {
			unsigned int n = rand_uint32(0, 25);
			moniker += fmt::format("{:c}", (char)('A' + n));
		}
		return moniker;
	}

	// TODO: is there a better way to handle this? incoming basename is from catalogue files..
	if (basename.size() != 4) {
		fmt::print("World::generateMoniker got passed '{}' as a basename which isn't 4 characters, so ignoring it\n", basename);
		basename = "xxxx";
	}

	std::string x = basename;
	for (unsigned int i = 0; i < 4; i++) {
		unsigned int n = rand_uint32(0, 0xfffff);
		x = x + "-" + fmt::format("{:05x}", n);
	}

	return x;
}

int World::findCategory(unsigned char family, unsigned char genus, unsigned short species) {
	if (engine.version < 3) {
		if (family == 2 && genus == 1 && species == 1)
			return genus; // 2 1 1 (hand) -> 1
		if (family == 2 && genus > 1 && genus < 26)
			return genus; // 2 2 0 to 2 25 0 -> 2 to 25
		if (family == 3 && genus < 10)
			return genus + 25; // 3 1 0 to 3 9 0 -> 26 to 35
		if (family == 4 && genus < 5)
			return genus + 35; // 4 1 0 to 4 4 0 -> 36 to 39

		return -1;
	}

	if (!catalogue.hasTag("Agent Classifiers"))
		return -1;

	const std::vector<std::string>& t = catalogue.getTag("Agent Classifiers");

	for (unsigned int i = 0; i < t.size(); i++) {
		std::string buffer = fmt::format("{} {} {}", (int)family, (int)genus, (int)species);
		if (t[i] == buffer)
			return i;
		buffer = fmt::format("{} {} 0", (int)family, (int)genus);
		if (t[i] == buffer)
			return i;
		buffer = fmt::format("{} 0 0", (int)family);
		if (t[i] == buffer)
			return i;
		// leave it here: 0 0 0 would be silly to have in Agent Classifiers.
	}

	return -1;
}

/* vim: set noet: */

// Food scent (an addition to C2, owner's idea): a room with edible objects
// smells at least as strong as the most edible of them (AgentHelpers'
// edibleScent, read from eat scripts; 0-1). Every 5 ticks the smell spreads to
// neighbouring rooms through doors, in proportion to how open each door is
// (permeability / 255; walls pass nothing), and slowly fades. Creatures can
// follow it toward food out of sight.
// C2 room temperatures, about once a second (every 10 ticks).
// Heat spreading is decoded from the real game (2026-11-08: a room raised to
// 255 lost 1/8 of its gap to each fully open neighbour per step, about once a
// second, total heat conserved): each door passes perm/255 * 1/8 of the
// temperature difference. Heat sources are our own design (C2's barely act:
// a cooled volcano room did not recover in minutes, and its +10 "desert" rooms
// were cooler than the incubator): a room with a heat source is pulled toward
// a climate target, 105 (the incubator area, comfortable) shifted by 30 *
// sqrt(|source|) in the source's direction, so the volcano (+120) is very hot,
// the desert (+10) hot (~200), the sea (-10, -120) cold; every room relaxes
// slowly toward 105, or the heat sources would slowly heat the whole world.
// Changes made by scripts (rain cools, lightning warms) are taken up and then
// spread and settle. Temperatures stay within 0-255.
void World::tickClimate() {
	static unsigned int counter = 0;
	if (++counter % 10 != 0 || !map)
		return;
	const float spread = 1.0f / 8.0f, sourcePull = 0.5f, ambient = 105.0f, ambientPull = 0.03f;
	const auto& rooms = map->getRooms();
	for (auto& r : rooms)
		if (r->tempf < 0.0f || (int)std::lround(r->tempf) != r->temp)
			r->tempf = (float)std::clamp(r->temp, 0, 255); // first step, or a script changed it
	std::vector<float> next(rooms.size());
	for (size_t i = 0; i < rooms.size(); i++) {
		Room& r = *rooms[i];
		float t = r.tempf;
		for (auto& [n, door] : r.getDoors()) {
			if (!n || door.perm <= 0 || n.get() == &r)
				continue;
			t += spread * (std::min(door.perm, 255) / 255.0f) * (n->tempf - r.tempf);
		}
		if (r.hsrc != 0) {
			float target = ambient + (r.hsrc > 0 ? 30.0f : -30.0f) * std::sqrt((float)std::abs(r.hsrc));
			t += sourcePull * (std::clamp(target, 0.0f, 255.0f) - r.tempf);
		}
		t += ambientPull * (ambient - r.tempf);
		next[i] = std::clamp(t, 0.0f, 255.0f);
	}
	for (size_t i = 0; i < rooms.size(); i++) {
		rooms[i]->tempf = next[i];
		rooms[i]->temp = (int)std::lround(next[i]);
	}
}

// C2 air pressure and wind (owner, 2026-11-21, from the creatures.wiki survey:
// rooms kept the world file's pressure and wind for good, so the storms'
// "addv pres 100", the evaporators' and clouds' pressure sources (PSRC, set
// like thermostats: "doif pres gt 80 setv psrc -60") did nothing, and the
// clouds, which steer by WNDX/WNDY, always met the same wind). Every 10 ticks,
// each doorway passes perm/255 * 1/32 of the pressure difference; a pressure
// source adds PSRC/20 a step (the scripts' -60 is 3 a step); every room
// settles toward the world file's pressure (2% of the gap a step). The wind is
// then made from the differences, as the real game's is: the world file's
// wind (a snapshot of the real game's) blows from high toward low pressure in
// 209 of 216 rooms, and fits best (62%) as the sum over doorways of (this
// room's pressure - the neighbour's) * perm/255 along the line from this
// room's middle to the neighbour's. Scaled by 1.6 so the wind is as strong as
// the snapshot's (rooms' median 17, 90th percentile 49; at 1/8 spread and no
// scaling it was 5 and 17). (Five rooms' wind in the world file was junk,
// 1819178284.) Switch OPENC2E_NO_WIND keeps the old fixed values (and the
// storms' pressure, which went on rising past 500 there).
void World::tickPressure() {
	static unsigned int counter = 0;
	static const bool off = devenv("OPENC2E_NO_WIND") != nullptr;
	if (off || ++counter % 10 != 0 || !map || engine.version != 2)
		return;
	const float spread = 1.0f / 32.0f, sourceRate = 1.0f / 20.0f, basePull = 0.02f, windScale = 1.6f;
	const auto& rooms = map->getRooms();
	static float wrapWidth = -1.0f;
	if (wrapWidth < 0.0f) {
		wrapWidth = 0.0f;
		for (unsigned int i = 0; i < map->getMetaRoomCount(); i++)
			if (MetaRoom* m = map->getMetaRoom(i); m && m->wraparound())
				wrapWidth = std::max(wrapWidth, (float)m->fullwidth());
	}
	for (auto& r : rooms) {
		if (r->presBase < 0)
			r->presBase = std::clamp(r->pres, 0, 255);
		if (r->presf < 0.0f || (int)std::lround(r->presf) != r->pres)
			r->presf = (float)std::clamp(r->pres, 0, 255); // first step, or a script changed it
	}
	auto middle = [](const Room& r) {
		return std::make_pair((r.x_left + r.x_right) / 2.0f,
			(std::min(r.y_left_ceiling, r.y_right_ceiling) + std::max(r.y_left_floor, r.y_right_floor)) / 2.0f);
	};
	std::vector<float> next(rooms.size());
	for (size_t i = 0; i < rooms.size(); i++) {
		Room& r = *rooms[i];
		float p = r.presf;
		float wx = 0, wy = 0;
		auto [rx, ry] = middle(r);
		for (auto& [n, door] : r.getDoors()) {
			if (!n || door.perm <= 0 || n.get() == &r)
				continue;
			float open = std::min(door.perm, 255) / 255.0f;
			p += spread * open * (n->presf - r.presf);
			auto [nx, ny] = middle(*n);
			float dx = nx - rx, dy = ny - ry;
			if (wrapWidth > 0 && std::fabs(dx) > wrapWidth / 2) // across the world's seam
				dx -= dx > 0 ? wrapWidth : -wrapWidth;
			float d = std::hypot(dx, dy);
			if (d > 0) {
				wx += (r.presf - n->presf) * open * dx / d;
				wy += (r.presf - n->presf) * open * dy / d;
			}
		}
		p += sourceRate * (float)r.psrc;
		p += basePull * ((float)r.presBase - r.presf);
		next[i] = std::clamp(p, 0.0f, 255.0f);
		r.windx = (int)std::lround(std::clamp(windScale * wx, -127.0f, 127.0f));
		r.windy = (int)std::lround(std::clamp(windScale * wy, -127.0f, 127.0f));
	}
	for (size_t i = 0; i < rooms.size(); i++) {
		rooms[i]->presf = next[i];
		rooms[i]->pres = (int)std::lround(next[i]);
	}
}

// C2 radiation (owner, 2026-11-21, the last item of the creatures.wiki survey:
// rooms kept the world file's radiation, whatever opened or closed). Every 10
// ticks: each doorway passes perm/255 * 1/8 of the difference (as heat), a
// radiation source adds 0.35 * RSRC a step, and every room loses a tenth a
// step. Fitted to the world file (a snapshot of the real game's: sources in
// the volcano's chamber, 20-100, and a small one in every sky room, 5-30;
// 66 rooms above 5): the rule's steady state explains 85% of it (3/16 spread
// 86%, 1/4 75-81%; less loss fits worse), the volcano's rooms 48-185 against
// 45-179, the sky rooms about half theirs. Creatures sense their room's
// radiation (Creature.cpp, sense 6) and every genome turns that into heavy
// metals. Switch OPENC2E_NO_RADIATION_SPREAD keeps the old fixed values.
void World::tickRadiation() {
	static unsigned int counter = 0;
	static const bool off = devenv("OPENC2E_NO_RADIATION_SPREAD") != nullptr;
	if (off || ++counter % 10 != 0 || !map || engine.version != 2)
		return;
	const float spread = 1.0f / 8.0f, sourceRate = 0.354f, loss = 0.1f;
	const auto& rooms = map->getRooms();
	for (auto& r : rooms)
		if (r->radnf < 0.0f || (int)std::lround(r->radnf) != r->radn)
			r->radnf = (float)std::clamp(r->radn, 0, 255); // first step, or a script changed it
	std::vector<float> next(rooms.size());
	for (size_t i = 0; i < rooms.size(); i++) {
		Room& r = *rooms[i];
		float v = r.radnf;
		for (auto& [n, door] : r.getDoors()) {
			if (!n || door.perm <= 0 || n.get() == &r)
				continue;
			v += spread * (std::min(door.perm, 255) / 255.0f) * (n->radnf - r.radnf);
		}
		v += sourceRate * (float)r.rsrc - loss * r.radnf;
		next[i] = std::clamp(v, 0.0f, 255.0f);
	}
	for (size_t i = 0; i < rooms.size(); i++) {
		rooms[i]->radnf = next[i];
		rooms[i]->radn = (int)std::lround(next[i]);
	}
}

// C2's nutrient cycle (owner's choices, 2026-11-17: a full cycle, the dead
// feeding the soil, nutrients seeping downhill with water, saved with the
// world). The game's own scripts add organic nutrient (ONTR) to a room as
// things rot there and let some plants and fungi feed on it; some plants
// need inorganic nutrient (INTR) within a range to live. Every 50 ticks:
// - organic nutrient above the room's own level (the world file's) breaks
//   down into inorganic, about half in one game season at the comfortable
//   105, twice as fast for each 50 warmer (a quarter to four times);
// - both seep through open doorways: downhill (a room whose floor is lower)
//   a little each step, level rooms even out slowly, water rooms mix;
// - each room settles slowly back toward its own levels (surplus leaches
//   away, a shortfall is made good), so the game's plant habitats hold.
// Scripts' changes are taken up as they come. Kept 0-255.
void World::tickNutrients() {
	static unsigned int counter = 0;
	static const bool off = devenv("OPENC2E_NO_NUTRIENT_CYCLE") != nullptr; // (A/B testing)
	if (off || ++counter % 50 != 0 || !map || engine.version != 2)
		return;
	const float breakdown = 0.0005f, settle = 0.0005f, seepDown = 0.002f, seepLevel = 0.001f, mixWater = 0.003f;
	const auto& rooms = map->getRooms();
	for (auto& r : rooms) {
		if (r->ontrBase < 0)
			r->ontrBase = r->ontr;
		if (r->intrBase < 0)
			r->intrBase = r->intr;
		if (r->ontrf < 0.0f || (int)std::lround(r->ontrf) != r->ontr)
			r->ontrf = (float)std::clamp(r->ontr, 0, 255);
		if (r->intrf < 0.0f || (int)std::lround(r->intrf) != r->intr)
			r->intrf = (float)std::clamp(r->intr, 0, 255);
	}
	std::vector<float> o(rooms.size()), in(rooms.size());
	std::map<Room*, size_t> index;
	for (size_t i = 0; i < rooms.size(); i++) {
		index[rooms[i].get()] = i;
		o[i] = rooms[i]->ontrf;
		in[i] = rooms[i]->intrf;
	}
	auto floorOf = [](const Room& r) { return (float)std::max(r.y_left_floor, r.y_right_floor); };
	for (size_t i = 0; i < rooms.size(); i++) {
		Room& r = *rooms[i];
		// breakdown of the surplus, faster where warm
		float warm = std::clamp(std::pow(2.0f, ((float)r.temp - 105.0f) / 50.0f), 0.25f, 4.0f);
		float surplus = std::max(0.0f, r.ontrf - (float)r.ontrBase);
		float made = breakdown * warm * surplus;
		o[i] -= made;
		in[i] += made;
		// settling back toward the room's own levels
		o[i] += settle * ((float)r.ontrBase - r.ontrf) * (r.ontrf < r.ontrBase ? 1.0f : 0.0f);
		in[i] += settle * ((float)r.intrBase - r.intrf);
		// seeping through open doorways
		for (auto& [n, door] : r.getDoors()) {
			if (!n || door.perm <= 0 || n.get() == &r)
				continue;
			auto it = index.find(n.get());
			if (it == index.end())
				continue;
			size_t j = it->second;
			float open = std::min(door.perm, 255) / 255.0f;
			float rate;
			if (r.type == 2 && n->type == 2)
				rate = mixWater; // water mixes: half each way
			else if (floorOf(*n) > floorOf(r) + 20.0f)
				rate = seepDown; // downhill, one way
			else if (std::fabs(floorOf(*n) - floorOf(r)) <= 20.0f)
				rate = seepLevel;
			else
				continue; // uphill: nothing
			bool oneWay = !(r.type == 2 && n->type == 2) && floorOf(*n) > floorOf(r) + 20.0f;
			float dO = rate * open * (oneWay ? r.ontrf : 0.5f * (r.ontrf - n->ontrf));
			float dI = rate * open * (oneWay ? r.intrf : 0.5f * (r.intrf - n->intrf));
			if (dO > 0) {
				o[i] -= dO;
				o[j] += dO;
			}
			if (dI > 0) {
				in[i] -= dI;
				in[j] += dI;
			}
		}
	}
	for (size_t i = 0; i < rooms.size(); i++) {
		Room& r = *rooms[i];
		r.ontrf = std::clamp(o[i], 0.0f, 255.0f);
		r.intrf = std::clamp(in[i], 0.0f, 255.0f);
		r.ontr = (int)std::lround(r.ontrf);
		r.intr = (int)std::lround(r.intrf);
	}
}

// Day and night in the rooms' light (owner's choices, 2026-11-17, from
// mockups: outdoors as the sky, night at a quarter of daylight). Every
// norn, grendel and ettin has a gene that raises Sleepiness in the dark;
// rooms kept the world file's light day and night, so a cave always made a
// creature sleepy and the open air never did. Now each room's light is its
// own (the world file's, or what a script last set) times a daylight
// factor in proportion to how far outdoors it is (SkyTint's outdoors map,
// traced from the painted sky): full by day, 0.7 at dawn, 0.6 at dusk, a
// quarter at night, fading over about 30 seconds as the sky does. Caves
// and indoor rooms keep their light, and so do rooms with a light source
// (lamps). Every 50 ticks.
void World::tickDaylight() {
	static unsigned int counter = 0;
	static const bool off = devenv("OPENC2E_NO_DAYLIGHT") != nullptr; // (A/B testing)
	if (off || ++counter % 50 != 0 || !map || engine.version != 2)
		return;
	auto factorFor = [](int tod) { return tod == 0 ? 0.7f : tod == 3 ? 0.6f : tod == 4 ? 0.25f : 1.0f; };
	static int from = -2, to = -2;
	static unsigned int changed = 0;
	int want = (int)timeofday;
	if (to == -2)
		from = to = want, changed = worldtickcount;
	else if (want != to)
		from = to, to = want, changed = worldtickcount;
	float t = std::clamp((worldtickcount - changed) / 300.0f, 0.0f, 1.0f);
	float day = factorFor(from) + (factorFor(to) - factorFor(from)) * t;
	for (auto& r : map->getRooms()) {
		if (r->liteBase < 0 || r->lite != r->liteShown)
			r->liteBase = r->lite; // first sight, or a script set it
		if (r->outdoors < 0.0f)
			if (MetaRoom* m = map->metaRoomAt((r->x_left + r->x_right) / 2, (r->y_left_ceiling + r->y_left_floor) / 2))
				r->outdoors = SkyTint::outdoors(m, r->x_left, std::min(r->y_left_ceiling, r->y_right_ceiling), r->x_right, std::max(r->y_left_floor, r->y_right_floor));
		float out = r->lsrc > 0 ? 0.0f : std::max(0.0f, r->outdoors);
		r->lite = (int)std::lround(r->liteBase * (1.0f - out * (1.0f - day)));
		r->liteShown = r->lite;
	}
}

// Breath bubbles (2 1 27 marked OV01 7777 by c2Creature::updateUnderwater)
// rise about 2 px a tick, wobbling a little, and pop when they leave the
// water or after about 10 seconds.
void World::tickBreathBubbles() {
	for (auto& a : agents) {
		if (!a || a->isDying() || a->family != 2 || a->genus != 1 || a->species != 27)
			continue;
		if (!a->var[1].hasInt() || a->var[1].getInt() != 7777)
			continue;
		int age = a->var[2].hasInt() ? a->var[2].getInt() + 1 : 1;
		a->var[2].setInt(age);
		float wobble = (age / 6) % 2 == 0 ? 0.5f : -0.5f;
		a->moveTo(a->x + wobble, a->y - 2.0f);
		std::shared_ptr<Room> r = map ? map->roomAt(a->x + a->getWidth() / 2.0f, a->y + a->getHeight() / 2.0f) : nullptr;
		if (!r || r->type != 2 || age > 100)
			a->kill();
	}
}

void World::tickFoodScent() {
	static unsigned int counter = 0;
	if (++counter % 5 != 0 || !map)
		return;
	const float spread = 0.12f; // share of the difference exchanged through a fully open door per step
	const float fade = 0.03f; // lost per step
	const auto& rooms = map->getRooms();
	for (auto& r : rooms)
		r->foodscentnext = r->foodscent * (1.0f - fade);
	for (auto& r : rooms) {
		for (auto& [n, door] : r->getDoors()) {
			if (door.perm <= 0)
				continue;
			r->foodscentnext += spread * (door.perm / 255.0f) * (n->foodscent - r->foodscent);
		}
	}
	for (auto& a : agents) {
		if (!a || a->family != 2 || a->isDying() || a->carriedby)
			continue;
		float edible = edibleNow(a.get());
		if (edible <= 0.0f)
			continue;
		// a room with food smells at least as strong as its most edible item
		// (not the sum: many fruits must not flood the whole map)
		std::shared_ptr<Room> r = map->roomAt(a->x + a->getWidth() / 2.0f, a->y + a->getHeight() / 2.0f);
		if (!r)
			continue;
		// only food a creature could reach: resting near the floor (fruit
		// still high on a plant smells of nothing a creature can get)
		float gap = r->floorYatX(a->x + a->getWidth() / 2.0f) - (a->y + a->getHeight());
		if (gap > 60.0f)
			continue;
		r->foodscentnext = std::max(r->foodscentnext, edible);
	}
	for (auto& r : rooms)
		r->foodscent = std::clamp(r->foodscentnext, 0.0f, 1.0f);
	static const bool watch = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
	if (watch && counter % 1000 == 0) {
		int some = 0, strong = 0;
		float top = 0;
		for (auto& r : rooms) {
			some += r->foodscent > 0.1f;
			strong += r->foodscent > 1.0f;
			top = std::max(top, r->foodscent);
		}
		fmt::print("* WATCH SCENTMAP rooms {} with smell {} strong {} strongest {:.2f}\n", rooms.size(), some, strong, top);
	}
}
