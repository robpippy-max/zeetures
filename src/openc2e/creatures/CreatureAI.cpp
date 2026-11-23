/*
 *  CreatureAI.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Sat May 15 2010.
 *  Copyright (c) 2004-2010 Alyssa Milburn. All rights reserved.
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
#include "creatures/Pheromones.h"
#include "creatures/Breeding.h"
#include "creatures/Disease.h"
#include "caosValue.h"
#include "CreatureAgent.h"
#include "SkeletalCreature.h"
#include "caosScript.h"
#include "Engine.h"
#include "World.h"
#include "Lift.h"
#include "historyManager.h"
#include "Map.h"
#include "MetaRoom.h"
#include "Room.h"
#include "c2eBrain.h"
#include "c2eCreature.h"
#include "common/Random.h"
#include "oldBrain.h"
#include "CreatureSpeech.h"
#include "creatures/Thoughts.h"
#include "oldCreature.h"
#include "Temperament.h"
#include "BrainProfiles.h"
#include <map>
#include <sstream>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <stdint.h>

// C2, under a mind: two kinds of the replaced brain's sensory cells feed body
// chemistry through the genome's emitters, so they are set here from what the
// creature perceives (the brain itself does not run). The attention lobe
// (tissue 2): the cell of the category attended to. The sensory-situation
// lobe (tissue 5), cell 19: a creature of its own genus and the other sex in
// sight (the norn's instincts pair it with Sex Drive and the kiss). A norn's
// sex drive chemical (13) is made from 115 and the fertility hormone 114, and
// 115 from what those two cells emit (116 while attending to a norn, 120 with
// the other sex in sight); without them no creature ever felt sex drive, and
// the world's kiss script, which asks both partners for more than 64 of it,
// never let any mate (found 2026-11-10, starting on breeding).
void oldCreature::feedSensoryCells() {
	if (engine.version != 2 || !brain)
		return;
	// the source lobe (2) and the attention lobe (7, from 2026-11-17: some
	// genomes' emitters read it): the category attended to
	for (unsigned int tissue : {2u, 7u})
		if (oldLobe* att = brain->getLobeByTissue(tissue))
			for (unsigned int n = 0; n < att->getNoNeurons(); n++) {
				oldNeuron* cell = att->getNeuron(n);
				cell->state = cell->output = ((int)n == attn && attention) ? 255 : 0;
			}
	if (oldLobe* sit = brain->getLobeByTissue(5); sit && sit->getNoNeurons() > 19) {
		// looked for on the mind's cadence (every 4 ticks), not every tick:
		// a scan of every agent, run by every creature (performance,
		// 2026-11-13); creatures only, and the cheap tests first
		if (ticks % 4 == 0) {
			mateInSight = false;
			for (auto& a : world.agents) {
				if (!a || a->family != 4 || a.get() == parentagent || a->genus != parentagent->genus)
					continue;
				auto* ca = dynamic_cast<CreatureAgent*>(a.get());
				if (!ca || !ca->getCreature() || !ca->getCreature()->isAlive() || ca->getCreature()->isFemale() == female)
					continue;
				if (!agentInSight(AgentRef(a)))
					continue;
				if (kinBarred(parent, ca))
					continue; // close kin, with someone else to choose (Breeding.h)
				mateInSight = true;
				break;
			}
		}
		oldNeuron* cell = sit->getNeuron(19);
		cell->state = cell->output = mateInSight ? 255 : 0;
	}
}

// C1/C2 under a mind: the involuntary actions (reflexes) the genes trigger
// through the involuntary-action loci (a female's egg laying, sneezing,
// coughing, shivering, falling asleep, the throes of dying) run the world's
// scripts 64-71 as in the original brain path, which a mind replaces; before,
// none ran, so a pregnant female never laid her egg (found 2026-11-10,
// breeding). As reflexes they interrupt what the creature is doing; each
// script starts by setting its own latency (LTCY), so it does not repeat at
// once.
void oldCreature::tickInvoluntaryActions() {
	if (zombie)
		return;
	for (unsigned int i = 0; i < 8; i++) {
		if (involactionlatency[i] > 0) {
			involactionlatency[i]--;
			continue;
		}
		if (involaction[i] > 0)
			parentagent->queueScript(i + 64);
	}
}

// SAYN (the world's "express need" action ends with it): the creature says
// its strongest need, worded for its age (CreatureSpeech).
void oldCreature::sayNeed() {
	unsigned int ndrives = (engine.version == 1) ? 16 : 17;
	int best = -1, level = 0;
	for (unsigned int i = 0; i < ndrives; i++)
		if ((int)getDrive(i) > level) {
			level = getDrive(i);
			best = (int)i;
		}
	// the same need again within about 30 s: the gesture only (an adult
	// asking for sleep eleven times running in a test, 2026-11-10)
	static const bool every = devenv("OPENC2E_TEST_SPEECH") != nullptr; // developer aid: no pause
	if (!every && best == lastNeedSaid && ticks < lastNeedTick + 300)
		return;
	lastNeedSaid = best;
	lastNeedTick = ticks;
	creatureSays(parentagent, (int)stage, best < 0 ? phraseFor("babble", parentagent, this) : phraseFor(fmt::format("need:{}:{}", best, level), parentagent, this));
}

void oldCreature::tickBrain() {
	if (mind) {
		tickMind();
		// the brain is replaced, but its chemical relay lobes are body chemistry
		static const bool norelay = devenv("OPENC2E_NO_RELAY") != nullptr; // for A/B testing
		{
			PROFILE_SCOPE("creature: brain relays");
			if (brain && !norelay)
				brain->tickChemicalRelays();
		}
		{
			PROFILE_SCOPE("creature: senses and involuntary");
			feedSensoryCells();
			tickInvoluntaryActions();
		}
		return;
	}

	if (asleep) {
		attn = -1;
		decn = -1;
		attention.clear(); // TODO: doesn't belong here
		if (!dreaming)
			return; // TODO
	}

	// TODO: correct timing?
	if ((ticks % 4) != 0)
		return;

	if (dreaming) {
		// TODO
	}

	// TODO

	chooseAgents();

	// TODO

	brain->tick();

	// TODO

	AgentRef oldattn = attention;
	int olddecn = decn;

	// TODO: doesn't belong here
	if (attn >= 0 && attn < (int)chosenagents.size())
		attention = chosenagents[attn];

	if (zombie)
		return; // TODO: docs say zombies "don't process decision scripts", correct?

	// involuntary actions
	for (unsigned int i = 0; i < 8; i++) {
		if (involactionlatency[i] > 0) {
			involactionlatency[i]--;
			continue;
		}

		// TODO
		if (parentagent->vmStopped() && involaction[i] > 0) {
			parentagent->queueScript(i + 64);
		}
	}

	// TODO
	if (parentagent->vmStopped() || oldattn != attention || olddecn != decn) {
		// TODO
		// a new action stops any walking the last one left going (a walk
		// action starts walking again itself): an eat whose target had gone
		// left a norn walking east into the volcano's wall while it "ate"
		// the fruit behind it (owner, 2026-11-09)
		if (auto* ca = dynamic_cast<CreatureAgent*>(parentagent))
			ca->stopWalking();
		parentagent->queueScript(decn + 16); // Extra scripts
	}
}

void c2eCreature::tickBrain() {
	if (asleep) {
		attn = -1;
		decn = -1;
		attention.clear(); // TODO: doesn't belong here
		if (!dreaming)
			return; // TODO
	}

	// TODO: correct timing?
	if ((ticks % 4) != 0)
		return;

	if (dreaming) {
		// TODO: this returns a bool (whether it did an instinct or not), shouldn't we do non-instinct dreaming or something if it's false?
		// .. if not, make it a void ;p
		processInstinct();
		return;
	}

	c2eLobe* drivlobe = brain->getLobeById("driv");
	if (drivlobe) {
		for (unsigned int i = 0; i < 20 && i < drivlobe->getNoNeurons(); i++) {
			drivlobe->setNeuronInput(i, drives[i]);
		}
	}

	/*c2eLobe *verblobe = brain->getLobeById("verb");
	if (verblobe) {
		for (unsigned int i = 0; i < verblobe->getNoNeurons(); i++) {
			verblobe->setNeuronInput(i, 0.0f); // TODO
		}
	}
	
	c2eLobe *nounlobe = brain->getLobeById("noun");
	if (nounlobe) {
		for (unsigned int i = 0; i < nounlobe->getNoNeurons(); i++) {
			nounlobe->setNeuronInput(i, 0.0f); // TODO
		}	
	}*/

#ifndef _CREATURE_STANDALONE
	// TODO: situ, detl

	chooseAgents();

	c2eLobe* visnlobe = brain->getLobeById("visn");
	if (visnlobe) {
		for (unsigned int i = 0; i < visnlobe->getNoNeurons() && i < chosenagents.size(); i++) {
			AgentRef a = chosenagents[i];
			if (!a)
				continue;

			// TODO: use eye position? see Creature::agentInSight
			float ourxpos = parentagent->x + (parentagent->getWidth() / 2.0f);
			float theirxpos = a->x + (a->getWidth() / 2.0f);
			float distance = theirxpos - ourxpos;

			// TODO: squash result into appropriate range?
			visnlobe->setNeuronInput(i, distance / parentagent->range);
		}
	}

	c2eLobe* smellobe = brain->getLobeById("smel");
	if (smellobe) {
		// TODO
	}
#endif

	brain->tick();

#ifndef _CREATURE_STANDALONE
	AgentRef oldattn = attention;
	int olddecn = decn;

	c2eLobe* attnlobe = brain->getLobeById("attn");
	if (attnlobe) {
		attn = attnlobe->getSpareNeuron();
	}

	c2eLobe* decnlobe = brain->getLobeById("decn");
	if (decnlobe) {
		// TODO: check bounds of mappinginfo
		decn = mappinginfo[decnlobe->getSpareNeuron()];
	}

	// TODO: doesn't belong here
	if (attn >= 0 && attn < (int)chosenagents.size())
		attention = chosenagents[attn];

	if (zombie)
		return; // TODO: docs say zombies "don't process decision scripts", correct?

	// fire scripts as needed
	// TODO: doesn't belong here
	// TODO: deal with decisions which don't have agents attached
	// TODO: deal with moving between ATTNs which don't have a choseagent right now (eg, nothing in sight)
	if (parentagent->vmStopped() || oldattn != attention || olddecn != decn) {
		if (attention && dynamic_cast<CreatureAgent*>(attention.get())) {
			parentagent->queueScript(decn + 32); // 'on creatures'
		} else {
			parentagent->queueScript(decn + 16); // 'on agents'
		}
	}

	// involuntary actions
	for (unsigned int i = 0; i < 8; i++) {
		if (involactionlatency[i] > 0) {
			involactionlatency[i]--;
			continue;
		}

		if (involaction[i] > 0.0f) {
			parentagent->queueScript(i + 64);
		}
	}
#endif
}

bool c2eCreature::processInstinct() {
	if (unprocessedinstincts.empty())
		return false;

	creatureInstinctGene* g = unprocessedinstincts.front();
	unprocessedinstincts.pop_front();

	// *** work out which verb neuron to fire by reverse-mapping from the mapping table
	int actualverb = reverseMapVerbToNeuron(g->action);
	// we have no idea which verb neuron to use, so no instinct processing
	if (actualverb == -1)
		return false;

	// *** debug output

	/*fmt::print("*** processing instinct for verb #{}\n", actualverb);
	fmt::print("reinforce using drive #{} at level {}\n", (int)g->drive, ((int)g->level - 128) / 128.0f);
	for (unsigned int i = 0; i < 3; i++) {
		if (g->lobes[i] != 255) {
			fmt::print("input: lobe tissue #{}, neuron #{}\n", (int)(g->lobes[i] - 1), (int)g->neurons[i]);
		}
	}*/

	/*
	 * instinct processing! a production by fuzzie in conjunction with coffee
	 *
	 * this is mostly guesswork because instincts seem to take place in a single tick in the engine,
	 * making them pretty difficult to observe
	 *
	 * we reset the brain by setting pre-REM chemical to full and ticking it once, then we set REM to full
	 * and perform two ticks: one with just the inputs set, and once with a response in the 'resp' lobe
	 */

	// *** sanity checks/setup

	c2eLobe* resplobe = brain->getLobeById("resp");
	c2eLobe* verblobe = brain->getLobeById("verb");
	// no response/verb lobe? no instincts for you, then..
	if (!resplobe || !verblobe)
		return false;

	// if action/drive are beyond the size of the relevant lobe, can't process instinct
	if ((unsigned int)actualverb >= verblobe->getNoNeurons())
		return false;
	if (g->drive >= resplobe->getNoNeurons())
		return false;

	c2eLobe* inputlobe[3] = {0, 0, 0};

	for (unsigned int i = 0; i < 3; i++) {
		// TODO: what about unused?
		uint8_t lobetissueid = g->lobes[i];
		if (lobetissueid == 255)
			continue;
		/* fuzzie would like to take this opportunity to quote from the pygenes source:
		 * Apparently, someone decided that because the rows are 1 above the lobe IDs, they should write the ROW NUMBER into the file, instead. Someone, somewhere, needs SHOOTING. */
		lobetissueid -= 1;
		inputlobe[i] = brain->getLobeByTissue(lobetissueid);
		// TODO: should we really barf out if this happens?
		if (!inputlobe[i])
			return false;
		if (g->neurons[i] >= inputlobe[i]->getNoNeurons())
			return false;
	}

	// *** reset brain

	// TODO: is this a sensible place to wipe the lobes?
	for (auto& lobe : brain->lobes)
		lobe.second->wipe();

	// TODO: non-hardcode 212/213? they seem to be in "Brain Parameters" catalogue tag
	// TODO: won't learning be sort of ruined by the repeated application of pre-REM?
	chemicals[212] = 1.0f; // pre-REM to full
	chemicals[213] = 0.0f; // REM to null
	brain->tick();
	chemicals[212] = 0.0f; // pre-REM to null
	chemicals[213] = 1.0f; // REM to full

	// *** set inputs and tick

	for (unsigned int i = 0; i < 3; i++) {
		// TODO: eeeevil hack - it looks like this is required, but is there no better way?
		if (g->lobes[i] == 3) {
			/*
			 * the visn lobe subtracts input from 1.0 to get distance of object, so 1.0 is no good
			 * we use 0.1, like c2e seems to feed it (the joys of hacked genetics and brain-in-a-vat!)
			 */
			// TODO: shouldn't we check lobe size?
			c2eLobe* visnlobe = brain->getLobeById("visn");
			if (visnlobe)
				visnlobe->setNeuronInput(g->neurons[i], 0.1f);
		}

		if (inputlobe[i])
			inputlobe[i]->setNeuronInput(g->neurons[i], 1.0f);
	}
	verblobe->setNeuronInput(actualverb, 1.0f);
	brain->tick();

	// *** set response and tick

	// TODO: shouldn't we make sure that decn/attn achieved the desired result?
	// TODO: should we set the input neurons again here? (it seems to work without - fuzzie)

	// TODO: TODO: TODO: check division of g->level!!
	// g->drive seems to be a direct mapping
	resplobe->setNeuronInput(g->drive, ((int)g->level - 128) / 128.0f);
	brain->tick();

	// *** finish off and return

	// TODO: shouldn't REM be present throughout sleep?
	chemicals[213] = 0.0f; // REM to null

	// wipe the lobes again, to stop any issues with neurons being set which shouldn't be at the end of an instinct run
	// TODO: is wiping the lobes here truly what we should do?
	for (auto& lobe : brain->lobes)
		lobe.second->wipe();

	//fmt::print("*** instinct done\n");
	//fmt::print("\n");

	return true;
}

#include "AgentHelpers.h"

// where a creature looks from (see agentInSight)
static void eyePoint(Agent* parentagent, float& ex, float& ey) {
	ex = parentagent->x + parentagent->getWidth() / 2.0f;
	ey = parentagent->y + parentagent->getHeight() / 2.0f;
	if (engine.version == 2)
		if (auto* sc = dynamic_cast<SkeletalCreature*>(parentagent)) {
			float raw = sc->x + sc->attachmentX(sc->isLeftFootDown() ? 11 : 12, 1);
			float fx = sc->footX();
			if (fx != raw)
				ex = fx;
		}
}

bool Creature::agentInSight(AgentRef a) {
#ifndef _CREATURE_STANDALONE
	if (a->invisible())
		return false;

	// TODO: specify x/y location for eyes
	// TODO: check open cabin?
	// At Albia's seam a creature's body can be drawn across the wall at the
	// edge of the map while its foot stands on the near side (the library's
	// west wall): it looks from above its foot, not from the middle of the
	// body in the rock beyond, from where it saw the volcano's critters
	// through the wall (owner, 2026-11-10).
	if (engine.version == 2)
		if (auto* sc = dynamic_cast<SkeletalCreature*>(parentagent)) {
			float raw = sc->x + sc->attachmentX(sc->isLeftFootDown() ? 11 : 12, 1);
			float fx = sc->footX();
			if (fx != raw)
				return agentPartlyVisible(parentagent, a.get(), fx, parentagent->y + parentagent->getHeight() / 2.0f);
		}
	return agentPartlyVisible(parentagent, a.get());
#else
	return false;
#endif
}

void Creature::chooseAgents() {
#ifndef _CREATURE_STANDALONE
	// zot any chosen agents which went out of range, went invisible or changed category
	for (unsigned int i = 0; i < chosenagents.size(); i++) {
		AgentRef a = chosenagents[i];
		if (a) {
			if (a->category != (int)i || !agentInSight(a))
				chosenagents[i].clear();
			// C1/C2: re-pick the nearest each time, except what the creature is
			// acting on now (so it is not switched mid-action)
			else if (engine.version < 3 && a != attention)
				chosenagents[i].clear();
		}
	}

	std::vector<std::vector<AgentRef> > possibles(chosenagents.size());

	// the eye point once, not for every object (performance, 2026-11-13)
	float ex, ey;
	eyePoint(parentagent, ex, ey);
	for (auto& a : world.agents) {
		if (!a)
			continue;

		// if agent category is -1 or outside of our #categories, continue
		if (a->category < 0)
			continue;
		if (a->category >= (int)chosenagents.size())
			continue;

		// if we already chose an agent from this category, continue
		if (chosenagents[a->category])
			continue;

		if (a->invisible() || !agentPartlyVisible(parentagent, a.get(), ex, ey))
			continue;
		// the Pointer toolbar's invisible hand: creatures do not notice it
		if (world.handInvisible && a.get() == (Agent*)world.hand())
			continue;

		possibles[a->category].push_back(a);
	}

	for (unsigned int i = 0; i < chosenagents.size(); i++) {
		if (!chosenagents[i])
			chosenagents[i] = selectRepresentativeAgent(i, possibles[i]);
	}
#endif
}


// Can a creature walking sideways from room "from" get to touch an object
// spanning x0-x1? It crosses into the next room on that side through a
// doorway it fits through (permeability at least its size), stepping up no
// more than a little; a wall (a closed doorway, like the one between the
// incubator bank and the pond, behind which the pond's water pump stands)
// stops it. A floor line it fits through (floor value at least its size, as
// in the desert lift's shaft) is walked under, on the room's bottom. Before
// this, an object at the creature's height behind a wall counted as
// reachable, and a norn walked into that wall for as long as it wanted the
// pump (owner, 2026-11-09).
static float walkFloorAt(Room& r, float x, int fit) {
	if (fit <= r.floorvalue) { // passable floor line: the room's bottom edge
		float t = r.x_right > r.x_left ? (x - r.x_left) / (float)(r.x_right - r.x_left) : 0;
		return r.y_left_floor + (r.y_right_floor - (float)r.y_left_floor) * std::clamp(t, 0.0f, 1.0f);
	}
	return r.floorYatX(x);
}

// Albia wraps: its east and west edges join through doorways (the rooms at
// x 8240-8352 open onto those at x 0). The width of a wrapping world's map,
// or 0, and how far to shift room n (by that width) to put it on the side of
// x nearest to it, so that rooms across the seam lie beside each other.
static float wrapWidth() {
	if (engine.version != 2 || !world.map)
		return 0.0f;
	for (unsigned int i = 0; i < world.map->getMetaRoomCount(); i++)
		if (MetaRoom* m = world.map->getMetaRoom(i); m && m->wraparound())
			return (float)m->width();
	return 0.0f;
}
static float seamShift(const Room& n, float x, float w) {
	if (w <= 0.0f)
		return 0.0f;
	float c = (n.x_left + n.x_right) / 2.0f;
	if (c - x > w / 2.0f)
		return -w;
	if (c - x < -w / 2.0f)
		return w;
	return 0.0f;
}

// The next room walking off room r on one side (east or west), at boundary
// x bx (in r's own terms), or null where that side is a wall. The room's
// shift across the seam is returned in nshift (bx - nshift is the boundary in
// the next room's own terms). With dry, water does not count; with
// fallsCount, a doorway down into a room wholly below (a drop) counts.
static std::shared_ptr<Room> walkOnFrom(const Room& r, bool east, int fit, bool dry, bool fallsCount, float w, float& nshift) {
	float bx = east ? (float)r.x_right : (float)r.x_left;
	float fy = walkFloorAt(const_cast<Room&>(r), bx, fit);
	float rc = (r.x_left + r.x_right) / 2.0f;
	std::shared_ptr<Room> next;
	float bestd = 1e9f;
	for (auto& [n, door] : const_cast<Room&>(r).getDoors()) {
		if (door.perm < fit || (dry && n->deepWater()))
			continue;
		float s = seamShift(*n, rc, w);
		if (east ? n->x_left + s + 2 < r.x_right : n->x_right + s > r.x_left + 2)
			continue; // not on that side
		float nbx = bx - s;
		float nf = walkFloorAt(*n, nbx, fit);
		if (nf < fy - 40) {
			// a floor line far above: walked under, along the room's
			// bottom edge, when that is level with here (the desert
			// lift's lower stop, under its shaft's floor line); else a climb
			float t = n->x_right > n->x_left ? (nbx - n->x_left) / (float)(n->x_right - n->x_left) : 0;
			float bottom = n->y_left_floor + (n->y_right_floor - (float)n->y_left_floor) * std::clamp(t, 0.0f, 1.0f);
			if (std::fabs(bottom - fy) > 40)
				continue;
			nf = bottom;
		}
		if (std::min(n->y_left_ceiling, n->y_right_ceiling) > fy) { // wholly below this floor: a fall, not a walk
			if (!fallsCount)
				continue;
			nf = fy; // open, whatever the depth
		}
		float dd = std::fabs(nf - fy);
		if (dd < bestd) {
			bestd = dd;
			next = n;
			nshift = s;
		}
	}
	return next;
}

// Does stepping off room r on one side end in water (deep water through the
// doorway, or a floor that opens down into it, a few rooms down)? Used to
// stop a creature walking up to an object over the end of a dock (it walked
// off the east dock's end reaching for an object there and sank; 2026-11-12).
bool walkOffEndsInWater(const Room& r, bool east, int fit) {
	const float w = wrapWidth();
	float ns = 0.0f;
	std::shared_ptr<Room> n = walkOnFrom(r, east, fit, false, true, w, ns);
	for (int depth = 0; n && depth < 4; depth++) {
		if (n->deepWater())
			return true;
		std::shared_ptr<Room> below;
		float nf = std::max(n->y_left_floor, n->y_right_floor);
		for (auto& [m, d2] : n->getDoors())
			if (d2.perm >= fit && std::min(m->y_left_ceiling, m->y_right_ceiling) + 2 >= nf)
				below = m;
		n = below;
	}
	return false;
}

static bool walkableTo(std::shared_ptr<Room> from, float x0, float x1, int fit) {
	// from dry land, a way through water does not count: the pond's ramp
	// made the fish in it "reachable" from the bank, and norns walked in after
	// them (regression batch reg2, owner's pond, 2026-11-09)
	const bool dry = from->type != 2;
	// across the seam the short way round: the object on the side nearest
	// the creature, and each room kept beside the last (off: its shift)
	const float w = wrapWidth();
	{
		float oc = (x0 + x1) / 2.0f, fc = (from->x_left + from->x_right) / 2.0f;
		if (w > 0 && oc - fc > w / 2.0f)
			x0 -= w, x1 -= w;
		else if (w > 0 && oc - fc < -w / 2.0f)
			x0 += w, x1 += w;
	}
	std::shared_ptr<Room> r = from;
	float off = 0.0f;
	for (int step = 0; r && step < 30; step++) {
		if (r->x_right + off > x0 && r->x_left + off < x1)
			return true; // standing in this room it can touch the object
		bool east = x0 >= r->x_right + off;
		float nshift = 0.0f;
		r = walkOnFrom(*r, east, fit, dry, false, w, nshift);
		off += nshift;
	}
	return false;
}

// Which room the creature's senses start from, and its x in that room's
// terms. openc2e wraps an agent by its top-left corner, so at Albia's seam a
// creature can stand with its foot just past x 0 (in the library) while its
// corner, and so the middle of its body, is at x ~8300, in the sealed rock
// beyond the library's west wall: judged from there, the wall's doorways and
// the things beyond it were all wrong, and a grendel walked into that wall for
// good (owner, 2026-11-10). Such a creature is judged by its foot.
static std::shared_ptr<Room> senseRoom(Agent* a, float cx, float cy, float& sx) {
	sx = cx;
	if (!world.map)
		return nullptr;
	if (engine.version == 2)
		if (auto* sc = dynamic_cast<SkeletalCreature*>(a)) {
			float raw = sc->x + sc->attachmentX(sc->isLeftFootDown() ? 11 : 12, 1);
			float fx = sc->footX();
			if (fx != raw) { // the foot is past the edge of the map: across the seam
				sx = fx;
				if (auto r = sc->footRoom())
					return r;
				return world.map->roomAt(fx, cy);
			}
			// wading: the middle is in the air above shallow water, but the
			// creature walks the water's bed; its senses of direction (smell,
			// walls, water ahead) are the bed's (the pond's bed is the way to
			// the cave behind its waterfall, owner 2026-11-10)
			if (auto r = sc->footRoom(); r && r->shallowWater())
				return r;
		}
	return world.map->roomAt(cx, cy);
}

AgentRef oldCreature::selectRepresentativeAgent(int type, std::vector<AgentRef> possibles) {
	// The nearest one, with height counting extra: an object on another floor
	// is much harder to reach than one the same distance away on this floor;
	// food-smelling ones count as nearer.
	// (openc2e picked one at random, often unreachable.)
	(void)type;
	if (possibles.empty())
		return AgentRef();
	float cx = parentagent->x + parentagent->getWidth() / 2.0f;
	float cy = parentagent->y + parentagent->getHeight() / 2.0f;
	float sx = cx;
	std::shared_ptr<Room> here = engine.version == 2 ? senseRoom(parentagent, cx, cy, sx) : nullptr;
	const int fit = std::max(1, parentagent->size);
	AgentRef best;
	float bestcost = 0;
	for (auto& a : possibles) {
		float ax = a->x + a->getWidth() / 2.0f, ay = a->y + a->getHeight() / 2.0f;
		float cost = std::fabs(ax - cx) + 4.0f * std::fabs(ay - cy);
		// one the creature cannot walk to (behind a wall) only if nothing else
		if (here && !walkableTo(here, (float)a->x, a->x + a->getWidth(), fit))
			cost += 5000.0f;
		// nor scenery it can do nothing with (not activatable, carryable or
		// food), like the airlock's drain machine, if anything else is there
		if (engine.version <= 2 && !(a->attr & (1 | 4)) && edibleNow(a.get()) <= 0)
			cost += 3000.0f;
		// passed over by the mind (its use kept doing nothing): notice others first
		for (auto& po : passedover)
			if (po.first == a && ticks < po.second)
				cost = cost * 6.0f + 500.0f;
		// The smell of food draws attention: in a category that mixes food
		// with things that are not (a fruit and its seed), the food is
		// noticed first unless it is much farther away.
		cost *= 1.0f - 0.6f * std::max(0.0f, edibleNow(a.get()));
		if (!best || cost < bestcost) {
			best = a;
			bestcost = cost;
		}
	}
	return best;
}

AgentRef c2eCreature::selectRepresentativeAgent(int type, std::vector<AgentRef> possibles) {
	// TODO: proper selection method
	(void)type;
	if (possibles.size() > 0)
		return rand_choice(possibles);
	else
		return AgentRef();
}

int c2eCreature::reverseMapVerbToNeuron(unsigned int verb) {
	// TODO: reverse-mapping like this seems utterly horrible, is it correct?
	int actualverb = -1;
	for (unsigned int i = 0; i < mappinginfo.size(); i++) {
		if (mappinginfo[i] == verb)
			actualverb = (int)i;
	}
	return actualverb;
}

void oldCreature::handleStimulus(unsigned int id) {
	StimulusEvent s;
	s.numbered = true;
	s.id = id;
	handleStimulus(s);
}

void oldCreature::handleStimulus(const StimulusEvent& s) {
	// Only active with a pluggable mind; the original brain path still
	// ignores stimuli, as before.
	if (!mind)
		return;
	if (!alive)
		return;
	StimulusEvent felt = s;
	applyStimulusChemistry(felt);
	// a creature harmed or helped may mark whoever did it with a scent
	// (pheromones.txt mark rules)
	if (engine.version == 2 && felt.source) {
		Pheromones::felt(parentagent, this, felt.source.get(), felt.feltChem, felt.feltAmount);
		// and its feeling for whoever did it moves (Relationships.h)
		Relationships::felt(parentagent, this, felt.source.get(), felt.feltChem, felt.feltAmount);
	}
	// what it does to a young creature's temperament (Temperament.h): hurt
	// (pain, injury, punishment), comfort (reward, needs for pleasure and
	// company and play eased), and a fight when another creature hurt it
	// (both of them)
	if (engine.version == 2) {
		float harm = 0, help = 0;
		for (size_t k = 0; k < 4; k++) {
			int ch = felt.feltChem[k], am = felt.feltAmount[k];
			if (am <= 0)
				continue;
			float f = am / 128.0f;
			if (ch == 18 || ch == 31) // pain, injury
				harm += f;
			else if (ch == 53) // punishment
				harm += 0.7f * f;
			else if (ch == 52) // reward
				help += f;
			else if (ch == 36 || ch == 42 || ch == 45) // need for pleasure, loneliness, boredom eased
				help += 0.7f * f;
		}
		if (harm > 0)
			temperamentExperience(EXPERIENCE_HURT, harm);
		// comfort and play come from the hand, other creatures and toys, not
		// from a meal (eating rewards too)
		Agent* from = felt.source.get();
		bool caring = from && from != parentagent && (from == (Agent*)world.hand() || dynamic_cast<CreatureAgent*>(from) || from->category == 13);
		if (help > 0 && caring)
			temperamentExperience(EXPERIENCE_COMFORT, help);
		if (harm > 0 && felt.source && felt.source.get() != parentagent)
			if (auto* other = dynamic_cast<CreatureAgent*>(felt.source.get()))
				if (auto* oc = dynamic_cast<oldCreature*>(other->getCreature())) {
					temperamentExperience(EXPERIENCE_FIGHT, harm);
					oc->temperamentExperience(EXPERIENCE_FIGHT, 0.5f * harm);
				}
	}
	// the sense cell it names (a pat lights "patted", a slap "slapped" ...)
	if (felt.feltSense >= 0 && felt.feltSense < 32 && felt.feltSenseIntensity > 0)
		senseFromStimuli[felt.feltSense] = std::max(senseFromStimuli[felt.feltSense], (float)std::min(felt.feltSenseIntensity, 255));
	if (pendingstimuli.size() < 64)
		pendingstimuli.push_back(felt);
}

// The body side of a stimulus: add the chemicals the world (full form) or
// the creature's own genome (numbered form, via stimulus genes) specifies.
// Only the chemical part is used; brain-related gene fields are ignored.
// Not implemented: the 'modulate' and 'addoffset' gene flags.
void oldCreature::applyStimulusChemistry(StimulusEvent& s) {
	if (s.numbered) {
		creatureStimulusGene* g = nullptr;
		for (auto& gene : genome->genes) {
			if (auto* x = dynamic_cast<creatureStimulusGene*>(gene.get())) {
				// genes that have switched on by now (shouldProcessGene only
				// passes those switching on at this very stage, so adults
				// felt nothing); the last one for this stimulus wins
				if (x->stim == s.id && isGeneExpressed(x))
					g = x;
			}
		}
		if (!g)
			return;
		if (asleep && !g->whenasleep)
			return;
		s.feltSense = g->sensoryneuron;
		s.feltSenseIntensity = g->intensity;
		for (unsigned int i = 0; i < 4; i++) {
			if (g->drives[i] != 0 && g->drives[i] != 255 && g->amounts[i] > 0) {
				addChemical(g->drives[i], g->amounts[i]);
				s.feltChem[i] = g->drives[i];
				s.feltAmount[i] = g->amounts[i];
			}
		}
	} else {
		s.feltSense = s.input;
		s.feltSenseIntensity = s.intensity;
		for (unsigned int i = 0; i < 4; i++) {
			int chem = s.chem[i], amount = s.amount[i];
			if (chem > 0 && chem < 255 && amount > 0) {
				addChemical((unsigned char)chem, (unsigned char)std::min(amount, 255));
				s.feltChem[i] = chem;
				s.feltAmount[i] = std::min(amount, 255);
			}
		}
	}
}

// Lists the decision scripts (events 16-47) the world defines for this
// creature, and whether each acts on the attended object.
void oldCreature::buildMindActions() {
	mindactions.clear();
	for (int n = 0; n < 32; n++) {
		std::shared_ptr<script> s = parentagent->findScript(16 + n);
		if (!s)
			continue;
		std::string code = s->code;
		std::transform(code.begin(), code.end(), code.begin(), [](unsigned char ch) { return std::tolower(ch); });
		bool needsObject = code.find("_it_") != std::string::npos || code.find("appr") != std::string::npos || code.find("aim:") != std::string::npos;
		mindactions.push_back({n, needsObject});
	}
}

MindPerception oldCreature::buildPerception() {
	MindPerception p;
	p.creatureId = parentagent->getUNID();
	if (engine.version == 2)
		p.self = world.history->findMoniker(getGenome());
	for (size_t w = 0; w < vocab.size(); w++)
		if (knowsWord(this, (int)w))
			p.knownWords.push_back((int)w);
	p.temperament = temperament.z;
	static const bool noMindGenes = devenv("OPENC2E_NO_MIND_GENES") != nullptr; // (to compare: every mind gene average)
	if (!noMindGenes)
		p.mindGenes = mindGenes.z;
	p.season = (int)world.season;
	p.timeOfDay = (int)world.timeofday;
	p.seasonalStrength = SeasonalRhythm::strength(seasonalZ);
	p.tick = ticks / 4;
	p.stage = stage;
	p.asleep = asleep;
	p.dreaming = dreaming;
	p.female = female;
	p.genus = (int)genus;
	unsigned int ndrives = (engine.version == 1) ? 16 : 17;
	for (unsigned int i = 0; i < ndrives; i++)
		p.drives[i] = getDrive(i);
	p.chemicals = chemicals;

	float cx = parentagent->x + parentagent->getWidth() / 2.0f;
	float cy = parentagent->y + parentagent->getHeight() / 2.0f;
	auto see = [&](AgentRef a, unsigned int i) {
		float ax = a->x + a->getWidth() / 2.0f;
		float ay = a->y + a->getHeight() / 2.0f;
		// walking only moves the creature sideways, and it must touch the object
		// (APPR): reachable only if their heights overlap
		bool reachable = !(a->y + a->getHeight() < parentagent->y || a->y > parentagent->y + parentagent->getHeight());
		// and nothing but open doorways between (not a wall)
		if (reachable && engine.version == 2 && world.map)
			if (float sx; std::shared_ptr<Room> here = senseRoom(parentagent, cx, cy, sx))
				if (!(reachable = walkableTo(here, (float)a->x, a->x + a->getWidth(), std::max(1, parentagent->size))) && devenv("OPENC2E_TEST_REACH"))
					fmt::print("* REACH {} {} {} at {:.0f},{:.0f} behind a wall from {:.0f},{:.0f}\n", (int)a->family, (int)a->genus, a->species, ax, ay, cx, cy);
		// C2 marks what may be activated (attr 4): the airlock's drain machine
		// (2 3 4, attr 0) is run only by its two doors, which close the sea
		// door first; a norn activating it directly drained the chamber with
		// the doors shut (owner, 2026-11-09)
		bool activatable = engine.version > 2 || (a->attr & 4);
		// another creature can always be acted on (kiss, tickle, push: the
		// world's creature-on-creature scripts), whatever its attributes
		if (dynamic_cast<CreatureAgent*>(a.get()))
			activatable = true;
		SeenAgent sa{(int)i, a, std::hypot(ax - cx, ay - cy), ax, ay, edibleNow(a.get()), reachable, activatable};
		sa.carryable = engine.version > 2 || (a->attr & 1); // C2 attr 1: creatures may pick it up
		// another creature: who it is and what this one feels about it
		if (auto* ca = dynamic_cast<CreatureAgent*>(a.get()); ca && engine.version == 2)
			if (auto* oc = dynamic_cast<oldCreature*>(ca->getCreature())) {
				sa.who = world.history->findMoniker(oc->getGenome());
				sa.genus = oc->getGenus();
				sa.female = oc->isFemale();
				sa.stage = (int)oc->getStage();
				// its Hunger drive (C2 drive 2, as the census reads it). C2 keeps
				// drives apart from the chemicals: chemical 150 is not hunger
				// there and does not move (2026-11-20: about 60% of Home's
				// creatures stuck at 248, so sharing went to the wrong friends)
				sa.hunger = oc->getDrive(2) / 255.0f;
				sa.kin = closeKin(p.self, sa.who);
				sa.ill = Disease::isIll(sa.who);
				sa.poisoned = oc->getChemical(140) > 50;
				sa.pain = oc->getDrive(0) / 255.0f;
				if (const Bond* b = Relationships::bondOf(this, sa.who)) {
					sa.affinity = b->affinity;
					sa.fear = b->fear;
					sa.familiarity = b->familiarity;
				}
			}
		p.visible.push_back(sa);
	};
	// the volcanic bushes' fruit is seen apart (MIND_HEALING_FRUIT): the one
	// noticed as fruit, if it is one, and else the nearest in sight
	// (OPENC2E_NO_HEALING_KIND: the fruit stays plain fruit to the mind, to compare)
	static const bool noHealingKind = devenv("OPENC2E_NO_HEALING_KIND") != nullptr;
	auto healing = [](Agent* a) {
		return !noHealingKind && engine.version == 2 && a && a->family == 2 && a->genus == 25 && a->var[2].hasInt() && a->var[2].getInt() == 2;
	};
	// rotten fruit too (MIND_ROTTEN_FRUIT), when one is what is noticed as
	// fruit; the nearest good fruit in sight is then seen as well, so a rotten
	// one lying nearer never hides it
	auto rotten = [](Agent* a) {
		return engine.version == 2 && a && a->family == 2 && a->genus == 25 && a->species == 149;
	};
	bool healingSeen = false, rottenSeen = false;
	for (unsigned int i = 0; i < chosenagents.size(); i++)
		if (chosenagents[i]) {
			bool h = mind && healing(chosenagents[i].get());
			bool r = mind && rotten(chosenagents[i].get());
			see(chosenagents[i], r ? (unsigned int)MIND_ROTTEN_FRUIT : h ? (unsigned int)MIND_HEALING_FRUIT : i);
			healingSeen = healingSeen || h;
			rottenSeen = rottenSeen || r;
		}
	if (rottenSeen) {
		AgentRef best;
		float bestd = 1e18f;
		for (auto& a : world.agents) {
			if (!a || a->family != 2 || a->genus != 25 || a->isDying() || a->carriedby || rotten(a.get()) || healing(a.get()) || edibleNow(a.get()) <= 0.0f)
				continue;
			float d = std::hypot(a->x - cx, a->y - cy);
			if (d < bestd && agentInSight(AgentRef(a))) {
				bestd = d;
				best = AgentRef(a);
			}
		}
		if (best)
			see(best, 25u);
	}
	if (mind && engine.version == 2 && !healingSeen) {
		AgentRef best;
		float bestd = 1e18f;
		for (auto& a : world.agents) {
			if (!a || a->genus != 25 || a->isDying() || !healing(a.get()) || a->carriedby)
				continue;
			float d = std::hypot(a->x - cx, a->y - cy);
			if (d < bestd && agentInSight(AgentRef(a))) {
				bestd = d;
				best = AgentRef(a);
			}
		}
		if (best)
			see(best, (unsigned int)MIND_HEALING_FRUIT);
	}
	// C2: every other creature in sight too, not only the one noticed for
	// its kind, so the mind can tell individuals apart
	if (engine.version == 2 && mind)
		for (auto& a : world.agents) {
			if (!a || a->family != 4 || a.get() == parentagent || a->isDying() || a->category < 0 || a->category >= (int)chosenagents.size())
				continue;
			auto* ca = dynamic_cast<CreatureAgent*>(a.get());
			if (!ca || !ca->getCreature() || !ca->getCreature()->isAlive() || chosenagents[a->category] == AgentRef(a))
				continue;
			if (agentInSight(AgentRef(a)))
				see(AgentRef(a), (unsigned int)a->category);
		}

	p.stimuli = pendingstimuli;
	p.actions = &mindactions;
	p.actionFinished = parentagent->vmStopped();
	p.currentAttention = attn;
	p.currentDecision = decn;
	p.x = cx;
	p.y = cy;
	if (world.map)
		if (MetaRoom* m = world.map->wrappingMetaRoomNear(cx, cy))
			p.wrapWidth = (float)m->width();
	p.riding = (bool)parentagent->invehicle;
	if (defendFrom && world.worldtickcount < defendUntil) {
		p.defendFrom = defendFrom;
		p.defendFor = defendFor;
	}
	if (auto* sc = dynamic_cast<SkeletalCreature*>(parentagent); sc && sc->lastFall.tick && sc->lastFall.tick != fallSeenTick) {
		fallSeenTick = sc->lastFall.tick;
		p.fellHeight = sc->lastFall.height;
		p.fellHurt = sc->lastFall.hurt;
		p.fellFromX = sc->lastFall.fromX;
		p.fellFromY = sc->lastFall.fromY;
	}
	if (auto* sc = dynamic_cast<SkeletalCreature*>(parentagent)) {
		p.walledCount = sc->wallBumps();
		p.walledDir = sc->wallBumpDir();
		p.scriptMoved = sc->scriptMoveTick() > 0 && world.tickcount <= sc->scriptMoveTick() + 5;
	}
	p.carried = (bool)parentagent->carriedby;
	if (parentagent->carrying)
		p.carryingCategory = parentagent->carrying->category;
	if (world.map)
		p.inRoom = (bool)world.map->roomAt(cx, cy);
	if (engine.version == 2 && world.map) {
		float sx = cx;
		std::shared_ptr<Room> here = senseRoom(parentagent, cx, cy, sx);
		if (here) {
			// which side a neighbouring room is on (across the seam the short
			// way round: Albia's east and west edges join)
			const float w = wrapWidth();
			const float hc = (here->x_left + here->x_right) / 2.0f;
			auto westOf = [&](const Room& n) { return n.x_right + seamShift(n, hc, w) <= here->x_left + 2; };
			auto eastOf = [&](const Room& n) { return n.x_left + seamShift(n, hc, w) + 2 >= here->x_right; };
			p.scentHere = here->foodscent;
			// sense of place: this room, its metaroom and its neighbours
			p.room = (int)here->id;
			if (MetaRoom* mr = world.map->metaRoomAt((unsigned int)std::max(0.0f, cx), (unsigned int)std::max(0.0f, cy)))
				p.metaroom = (int)mr->id;
			p.roomX = (here->x_left + here->x_right) / 2.0f;
			p.roomFloor = (float)std::max(here->y_left_floor, here->y_right_floor);
			{
				const int fitNear = std::max(1, parentagent->size);
				unsigned int hFloor = std::max(here->y_left_floor, here->y_right_floor);
				unsigned int hCeiling = std::min(here->y_left_ceiling, here->y_right_ceiling);
				for (auto& [n, door] : here->getDoors()) {
					if (door.perm < fitNear)
						continue;
					MindPerception::RoomNear rn;
					rn.id = (int)n->id;
					rn.x = (n->x_left + n->x_right) / 2.0f + seamShift(*n, hc, w);
					rn.floor = (float)std::max(n->y_left_floor, n->y_right_floor);
					rn.water = n->type == 2;
					if (westOf(*n))
						rn.side = 10;
					else if (eastOf(*n))
						rn.side = 11;
					else if (std::max(n->y_left_floor, n->y_right_floor) <= hCeiling + 2)
						rn.side = -1;
					else if (std::min(n->y_left_ceiling, n->y_right_ceiling) + 2 >= hFloor)
						rn.side = 1;
					else
						continue;
					p.roomsNear.push_back(rn);
				}
			}
			// only doorways the creature fits through count for its senses of
			// direction (smell spreads through narrower gaps, but following it
			// there walked creatures into walls: the middle lift's lower stop
			// smells strongly of food to the west through a gap no norn passes)
			const int fit = std::max(1, parentagent->size);
			unsigned int floor = std::max(here->y_left_floor, here->y_right_floor);
			unsigned int ceiling = std::min(here->y_left_ceiling, here->y_right_ceiling);
			for (auto& [n, door] : here->getDoors()) {
				if (door.perm < fit)
					continue;
				float* side;
				if (westOf(*n))
					side = &p.scentWest;
				else if (eastOf(*n))
					side = &p.scentEast;
				else if (std::max(n->y_left_floor, n->y_right_floor) <= ceiling + 2)
					side = &p.scentUp;
				else if (std::min(n->y_left_ceiling, n->y_right_ceiling) + 2 >= floor)
					side = &p.scentDown;
				else
					continue;
				*side = std::max(*side, n->foodscent);
			}
			// urges from smelled scents, with the same senses of direction
			for (auto& u : Pheromones::urgesFor(parentagent, this)) {
				MindPerception::ScentUrge su;
				su.scent = Pheromones::channelName(u.channel);
				su.avoid = u.avoid;
				su.drive = u.drive;
				su.weight = u.weight;
				su.tend = u.tend;
				auto level = [&](const Room& r) { return (size_t)u.channel < r.scent.size() ? r.scent[u.channel] : 0.0f; };
				su.here = level(*here);
				for (auto& [n, door] : here->getDoors()) {
					if (door.perm < fit)
						continue;
					float* side;
					if (westOf(*n))
						side = &su.west;
					else if (eastOf(*n))
						side = &su.east;
					else if (std::max(n->y_left_floor, n->y_right_floor) <= ceiling + 2)
						side = &su.up;
					else if (std::min(n->y_left_ceiling, n->y_right_ceiling) + 2 >= floor)
						side = &su.down;
					else
						continue;
					*side = std::max(*side, level(*n));
				}
				p.scentUrges.push_back(su);
			}
			// the own scents of the creatures the mind longs for
			if (CreatureMind* m = getMind())
				for (auto& who : m->wantedSignatures()) {
					MindPerception::SignatureSense ss;
					ss.who = who;
					ss.here = Pheromones::signatureIn(*here, who);
					for (auto& [n, door] : here->getDoors()) {
						if (door.perm < fit)
							continue;
						float* side;
						if (westOf(*n))
							side = &ss.west;
						else if (eastOf(*n))
							side = &ss.east;
						else if (std::max(n->y_left_floor, n->y_right_floor) <= ceiling + 2)
							side = &ss.up;
						else if (std::min(n->y_left_ceiling, n->y_right_ceiling) + 2 >= floor)
							side = &ss.down;
						else
							continue;
						*side = std::max(*side, Pheromones::signatureIn(*n, who));
					}
					p.signatures.push_back(ss);
				}
			// water ahead on either side (to stay out of seas when walking)
			for (auto& [n, door] : here->getDoors()) {
				if (door.perm < fit || !n->deepWater())
					continue;
				if (westOf(*n))
					p.waterWest = true;
				else if (eastOf(*n))
					p.waterEast = true;
			}
			// drops ahead: stepping through a side where the floor beyond is
			// much lower is a fall (off a dock or a ledge); a step, small or
			// large, that ends in water counts as water ahead (the east dock
			// ends 24 px above a "floor" that is the sea's surface, open to the
			// water below: a norn following a smell walked off it and sank;
			// owner, 2026-11-12)
			for (auto& [n, door] : here->getDoors()) {
				if (door.perm < fit || n->type == 2)
					continue;
				bool west = westOf(*n), east = eastOf(*n);
				if (!west && !east)
					continue;
				float bx = west ? (float)here->x_left : (float)here->x_right;
				// the room a step that way goes into, as the foot takes it
				// (SkeletalCreature::snapDownFoot): of the rooms on that side,
				// the one whose floor is nearest the floor here, none far above.
				// The desert lift's shaft (room 12, its walk-over floor at y
				// 573-598) borders rooms below that line too (15 to the west,
				// 14 to the east), reached only by falling down the shaft; their
				// floors read as ~300 px drops both ways, so starving norns there
				// were offered no walk to food (2026-11-19). A real drop beyond
				// the side is still followed down from the room stepped into.
				{
					float herefloor = here->floorYatX(bx);
					float mine = std::fabs(n->floorYatX(bx - seamShift(*n, hc, w) + (west ? -2.0f : 2.0f)) - herefloor);
					bool nearer = false;
					for (auto& [o, d3] : here->getDoors()) {
						if (o == n || d3.perm < fit || o->type == 2 || (west ? !westOf(*o) : !eastOf(*o)))
							continue;
						float ox = bx - seamShift(*o, hc, w) + (west ? -2.0f : 2.0f);
						if (ox < o->x_left || ox > o->x_right)
							continue;
						float of = o->floorYatX(ox);
						if (of < herefloor - 40.0f)
							continue; // (a floor far above is a wall, not a step)
						if (std::fabs(of - herefloor) < mine)
							nearer = true;
					}
					if (nearer)
						continue;
				}
				// where would the step end? follow open floors down a few
				// rooms, a little way past the side (where a creature
				// stepping off comes down): the drop is to the floor it would
				// land on, not the next room's floor line when that is open
				// air (the upper-east ledge's end: 63 px to room 179's open
				// floor, but 400 px to where norns landed; 2026-11-13)
				float nx = bx - seamShift(*n, hc, w) + (west ? -25.0f : 25.0f);
				float land = n->floorYatX(nx);
				std::shared_ptr<Room> r = n;
				for (int depth = 0; r && depth < 4; depth++) {
					if (r->deepWater()) {
						(west ? p.waterWest : p.waterEast) = true;
						break;
					}
					std::shared_ptr<Room> below;
					float rfloor = r->floorYatX(nx);
					for (auto& [m, d2] : r->getDoors())
						if (d2.perm >= fit && std::min(m->y_left_ceiling, m->y_right_ceiling) + 2 >= std::max(r->y_left_floor, r->y_right_floor) &&
							m->x_left <= nx && m->x_right >= nx && m->type != 2 && std::fabs(m->y_left_ceiling + (m->y_right_ceiling - m->y_left_ceiling) * (nx - m->x_left) / std::max(1.0f, (float)(m->x_right - m->x_left)) - rfloor) < 6.0f)
							below = m;
					if (!below) {
						// (water below: the loop above's first test, next time)
						for (auto& [m, d2] : r->getDoors())
							if (d2.perm >= fit && m->deepWater() && std::min(m->y_left_ceiling, m->y_right_ceiling) + 2 >= std::max(r->y_left_floor, r->y_right_floor))
								below = m;
						if (!below)
							break;
					} else
						land = below->floorYatX(nx);
					r = below;
				}
				float drop = land - here->floorYatX(bx);
				if (drop > 60.0f)
					(west ? p.dropWest : p.dropEast) = std::max(west ? p.dropWest : p.dropEast, drop);
			}
			{
				// developer aid: OPENC2E_WATCH_DROPS prints each change of the
				// drops a creature sees ahead
				static const bool watchdrops = devenv("OPENC2E_WATCH_DROPS") != nullptr;
				static std::map<int, std::pair<int, int>> lastdrops;
				std::pair<int, int> nowd{(int)p.dropWest, (int)p.dropEast};
				if (watchdrops && lastdrops[parentagent->getUNID()] != nowd) {
					fmt::print("* WATCH DROPS unid {} room {} at {:.0f},{:.0f}: west {} east {}\n", parentagent->getUNID(), here->id, p.x, p.y, nowd.first, nowd.second);
					lastdrops[parentagent->getUNID()] = nowd;
				}
			}
			{
				static const bool watchwater = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
				static std::map<int, int> lastwater;
				int now = (p.waterWest ? 1 : 0) | (p.waterEast ? 2 : 0);
				if (watchwater && lastwater[parentagent->getUNID()] != now) {
					fmt::print("* WATCH WATER unid {} room {} ({}-{}) water ahead{}{}\n", parentagent->getUNID(), here->id,
						here->x_left, here->x_right, p.waterWest ? " west" : "", p.waterEast ? " east" : "", now ? "" : ": none");
					lastwater[parentagent->getUNID()] = now;
				}
			}
			// a wall right ahead: walking that way from where the foot stands
			// goes nowhere (no doorway the creature fits through goes on at
			// walking height, nor down). Wandering walks met no such sense and
			// a grendel walked into the library's west wall for good (owner,
			// 2026-11-10). Only standing: the foot's room is known.
			if (auto* sc = dynamic_cast<SkeletalCreature*>(parentagent))
				if (std::shared_ptr<Room> fr = sc->footRoom()) {
					float fx = sc->footX();
					fx -= seamShift(*fr, fx, w); // in the foot room's own terms
					for (bool east : {false, true}) {
						float edge = east ? (float)fr->x_right : (float)fr->x_left;
						if (std::fabs(fx - edge) > 30.0f)
							continue;
						float ns = 0.0f;
						if (!walkOnFrom(*fr, east, fit, false, true, w, ns))
							(east ? p.wallEast : p.wallWest) = true;
					}
					static const bool watchwall = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
					static std::map<int, int> lastwall;
					int wallnow = (p.wallWest ? 1 : 0) | (p.wallEast ? 2 : 0);
					if (watchwall && lastwall[parentagent->getUNID()] != wallnow) {
						fmt::print("* WATCH WALL unid {} foot {:.0f} room {} ({}-{}) wall{}{}\n", parentagent->getUNID(), fx, fr->id,
							fr->x_left, fr->x_right, p.wallWest ? " west" : "", p.wallEast ? " east" : "");
						lastwall[parentagent->getUNID()] = wallnow;
					}
				}
			// water: the way to the nearest dry room through open doors
			// in water when the middle is, or the feet are (wading in a shallow
			// pond, the middle stays in the air room above it)
			// (wading on the bed of shallow water with the head above it is
			// walking, not being in water: the pond's bed is the way to the
			// cave behind its waterfall)
			p.breath = breathLeft();
			p.breathesWater = breathesWater();
			p.inWater = here->type == 2;
			if (!p.inWater)
				if (std::shared_ptr<Room> feet = world.map->roomAt(sx, parentagent->y + parentagent->getHeight() - 2))
					p.inWater = feet->type == 2;
			if (p.inWater && !here->deepWater())
				if (std::shared_ptr<Room> feet = world.map->roomAt(sx, parentagent->y + parentagent->getHeight() - 2); feet && feet->shallowWater() && p.breath >= 1.0f)
					p.inWater = false;
			if (p.inWater) {
				struct Step {
					std::shared_ptr<Room> room;
					int way, depth;
				};
				std::vector<Step> queue;
				std::vector<Room*> seen{here.get()};
				// sideways only to a room whose floor is within a step of the
				// floor walked from (40 px, as steps up are walked): the
				// submarine's berth (room 149, floor 906) lies east of the
				// east sea's middle (151) and west of its east end (150), 270
				// px above the seabed, so a norn there was sent east from one
				// and west from the other, back and forth along the seabed for
				// good, and never tried the jump pad beside it (2026-11-18)
				// (floors as walked: a water room's bed may slope up to its
				// exit, as the pond's ramp does; at the edge the two share)
				auto withinStep = [&](Room& n, Room& from, float fromFloor) {
					bool west = n.x_right <= from.x_left + 2;
					float edge = west ? (float)n.x_right - 1.0f : (float)n.x_left + 1.0f;
					return n.floorYatX(edge) + 40.0f >= fromFloor;
				};
				float feet = parentagent->y + parentagent->getHeight();
				for (auto& [n, door] : here->getDoors()) {
					if (door.perm < fit)
						continue;
					int way = 0;
					if (westOf(*n))
						way = withinStep(*n, *here, feet) ? 10 : 0;
					else if (eastOf(*n))
						way = withinStep(*n, *here, feet) ? 11 : 0;
					else if (std::max(n->y_left_floor, n->y_right_floor) <= ceiling + 2)
						way = 0; // up: not on foot (lifts are handled by the mind)
					else if (std::min(n->y_left_ceiling, n->y_right_ceiling) + 2 >= floor)
						way = 1;
					if (way)
						queue.push_back({n, way, 1});
				}
				for (size_t i = 0; i < queue.size() && i < 200; i++) {
					Step st = queue[i];
					if (std::find(seen.begin(), seen.end(), st.room.get()) != seen.end())
						continue;
					seen.push_back(st.room.get());
					if (st.room->type != 2) {
						p.dryWay = st.way;
						p.dryRooms = st.depth;
						break;
					}
					if (st.depth >= 8)
						continue;
					for (auto& [n, door] : st.room->getDoors()) {
						if (door.perm < fit)
							continue;
						// on foot only sideways or down: a room above (such as
						// the surface over the sea) cannot be climbed up to
						unsigned int nfloor = std::max(n->y_left_floor, n->y_right_floor);
						unsigned int rceiling = std::min(st.room->y_left_ceiling, st.room->y_right_ceiling);
						if (nfloor <= rceiling + 2)
							continue;
						// (sideways only: a room below is reached by going down)
						bool side = n->x_right <= st.room->x_left + 2 || n->x_left + 2 >= st.room->x_right;
						if (side) {
							bool west = n->x_right <= st.room->x_left + 2;
							float edge = west ? (float)st.room->x_left + 1.0f : (float)st.room->x_right - 1.0f;
							if (!withinStep(*n, *st.room, st.room->floorYatX(edge)))
								continue;
						}
						queue.push_back({n, st.way, st.depth + 1});
					}
				}
			}
		}
	}
	return p;
}

// Temperament drift (Temperament.h): an experience of a kind and size, at
// this creature's life stage. Its temperament is its inborn one plus the
// drift; the mind takes it up on its next tick. A shift of another 0.1 in
// any trait is logged.
void oldCreature::refreshTemperament() {
	temperament = Temperaments::withDrift(temperamentBorn, temperamentDrift);
	for (int i = 0; i < NUM_TRAITS; i++)
		temperament.z[i] = std::clamp(temperament.z[i] + geneShift[i], -2.0f, 2.0f);
}

Temperament oldCreature::inheritableTemperament() const {
	return Temperaments::withDrift(temperamentBorn, temperamentDrift);
}

void oldCreature::temperamentExperience(int kind, float size) {
	static const bool off = devenv("OPENC2E_NO_TEMPERAMENT_DRIFT") != nullptr; // (A/B testing)
	if (off || engine.version != 2 || size == 0.0f)
		return;
	if (!Temperaments::experience(temperamentDrift, kind, size, (int)stage))
		return;
	refreshTemperament();
	Temperament drifted = Temperaments::withDrift(temperamentBorn, temperamentDrift);
	for (int i = 0; i < NUM_TRAITS; i++) {
		float moved = drifted.z[i] - temperamentBorn.z[i];
		if (std::fabs(moved - driftLogged[i]) >= 0.1f) {
			driftLogged[i] = moved;
			std::string who = world.history ? world.history->findMoniker(genome) : std::string();
			fmt::print("* {}'s temperament: {} {:+.2f} since it hatched ({})\n", who, Temperaments::traitName(i), moved,
				moved > 0 ? Temperaments::highWord(i) : Temperaments::lowWord(i));
		}
	}
}

// C2 under a mind: the genome's emitters that read the replaced brain's
// cells keep working, as their authors wired them, because those cells are
// set here from what the creature perceives and does (the brain itself does
// not run; feedSensoryCells does the attention lobe and the mate cell).
// Meanings from the C2 CAOS guide: the sense lobe's 32 situations (tissue
// 5), the drive lobe (1: the 17 drives) and the decision lobe (6: the
// action under way). The senses also go to the mind, for the instincts that
// name them. Stimuli light the sense cell their gene or script names (a pat
// "patted"), fading over a second or two.
void oldCreature::updateBrainCells(MindPerception& p) {
	// (OPENC2E_NO_GENOME_BRIDGE: as before, for A/B testing)
	static const bool off = devenv("OPENC2E_NO_GENOME_BRIDGE") != nullptr;
	if (engine.version != 2 || !brain || off)
		return;
	// which chemicals its decision lobe learns from: the receptors on that
	// lobe's first two chemical slots (loci 13 and 14)
	if (rewardChem < 0) {
		rewardChem = 52;
		punishChem = 53;
		for (auto& gene : genome->genes)
			if (auto* r = dynamic_cast<bioReceptorGene*>(gene.get()); r && r->organ == 0 && r->tissue == 6) {
				if (r->locus == 13)
					rewardChem = r->chemical;
				else if (r->locus == 14)
					punishChem = r->chemical;
			}
	}
	p.rewardChem = rewardChem;
	p.punishChem = punishChem;
	// brain add-ons of a custom genome: roles for the mind (BrainProfiles.h)
	if (!brainProfileRead) {
		brainProfileRead = true;
		BrainProfiles::Result r = BrainProfiles::read(*genome);
		// (OPENC2E_NO_BRAIN_PROFILES: read and logged, not applied; A/B testing)
		static const bool off = devenv("OPENC2E_NO_BRAIN_PROFILES") != nullptr;
		if (!off)
			brainSettings = r.settings;
		if (r.any()) {
			std::string who = world.history ? world.history->findMoniker(genome) : std::string();
			for (auto& line : BrainProfiles::describe(r))
				fmt::print("* {} brain: {}\n", who, line);
		}
	}
	p.brainSettings = &brainSettings;
	// a bad fall, or nearly drowning, shapes a young temperament too
	if (p.fellHurt > 0)
		temperamentExperience(EXPERIENCE_HURT, 2.0f * p.fellHurt);
	if (p.inWater && !p.breathesWater && p.breath < 0.3f)
		temperamentExperience(EXPERIENCE_HURT, 0.05f);
	if (temperamentDrift.fightRecent != 0.0f) {
		Temperaments::fadeFights(temperamentDrift);
		if (ticks % 200 == 0)
			refreshTemperament();
	}

	std::array<float, 32> s{};
	for (int i = 0; i < 32; i++) {
		s[i] = senseFromStimuli[i];
		senseFromStimuli[i] = senseFromStimuli[i] < 2.0f ? 0.0f : senseFromStimuli[i] * 0.9f;
	}
	auto set = [&](int i, float v) { s[i] = std::max(s[i], std::clamp(v, 0.0f, 255.0f)); };
	if (!brainCellsStarted) {
		brainCellsStarted = true;
		lastWallBumps = p.walledCount;
	}
	if (p.walledCount != lastWallBumps)
		set(2, 255); // bumped into a wall
	lastWallBumps = p.walledCount;
	if (p.wallWest || p.wallEast)
		set(3, 255); // near a wall
	if (p.riding)
		set(4, 255); // in a vehicle
	// IT: what it is attending to
	Agent* it = attention.get();
	if (it) {
		float dx = (it->x + it->getWidth() / 2.0f) - p.x, dy = (it->y + it->getHeight() / 2.0f) - p.y;
		if (p.wrapWidth > 0 && std::fabs(dx) > p.wrapWidth / 2.0f)
			dx -= (dx > 0 ? p.wrapWidth : -p.wrapWidth);
		float dist = std::sqrt(dx * dx + dy * dy);
		if (lastIt.get() == it && lastItDistance >= 0) {
			if (dist < lastItDistance - 2.0f)
				set(10, 255); // approaching
			else if (dist > lastItDistance + 2.0f)
				set(11, 255); // moving away
		}
		lastIt = attention;
		lastItDistance = dist;
		set(12, 255.0f * (1.0f - dist / 400.0f)); // how near
		auto* ca = dynamic_cast<CreatureAgent*>(it);
		set(ca ? 15 : 14, 255); // a creature, or an object
		if (ca && ca->getCreature() && world.history) {
			std::string me = world.history->findMoniker(genome), them = world.history->findMoniker(ca->getCreature()->getGenome());
			if (!me.empty() && !them.empty() && world.history->hasMoniker(me) && world.history->hasMoniker(them)) {
				monikerData& m = world.history->getMoniker(me);
				monikerData& t = world.history->getMoniker(them);
				if (m.mother == them || m.father == them)
					set(17, 255); // my parent
				else if (t.mother == me || t.father == me)
					set(18, 255); // my child
				else if ((!m.mother.empty() && (m.mother == t.mother || m.mother == t.father)) || (!m.father.empty() && (m.father == t.father || m.father == t.mother)))
					set(16, 255); // my brother or sister
			}
		}
	} else {
		lastIt = AgentRef();
		lastItDistance = -1;
	}
	if (mateInSight)
		set(19, 255);
	{
		int dir = (p.currentDecision == 26) ? 10 : (p.currentDecision == 27) ? 11 : p.currentDecision;
		if ((dir == 10 && p.dropWest > 40.0f) || (dir == 11 && p.dropEast > 40.0f))
			set(28, 255); // walking toward an edge
	}
	if (parentagent->falling && !p.carried && !p.riding && !p.inWater)
		set(30, 255); // falling
	if (p.fellHeight > 0)
		set(31, 255); // landed

	if (oldLobe* sense = brain->getLobeByTissue(5))
		for (unsigned int n = 0; n < 32 && n < sense->getNoNeurons(); n++) {
			if (n == 19)
				continue; // feedSensoryCells
			oldNeuron* cell = sense->getNeuron(n);
			cell->state = cell->output = (unsigned char)std::lround(s[n]);
		}
	if (oldLobe* drive = brain->getLobeByTissue(1))
		for (unsigned int n = 0; n < 17 && n < drive->getNoNeurons(); n++) {
			oldNeuron* cell = drive->getNeuron(n);
			cell->state = cell->output = p.drives[n];
		}
	if (oldLobe* decision = brain->getLobeByTissue(6))
		for (unsigned int n = 0; n < decision->getNoNeurons(); n++) {
			oldNeuron* cell = decision->getNeuron(n);
			cell->state = cell->output = ((int)n == decn) ? 255 : 0;
		}
	for (int i = 0; i < 32; i++)
		p.senses[i] = (unsigned char)std::lround(s[i]);
	// developer aid: OPENC2E_WATCH_CREATURES lists the lit senses now and then
	static const bool watch = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
	if (watch && ticks % 200 == 0) {
		std::string lit;
		for (int i = 0; i < 32; i++)
			if (p.senses[i] >= 64)
				lit += fmt::format(" {}", i);
		if (!lit.empty())
			fmt::print("* WATCH SENSES unid {} lit:{} (reward chem {}, punishment {})\n", parentagent->getUNID(), lit, rewardChem, punishChem);
	}
}

// Genome instincts, translated for a mind (gene-translation.md, section 3).
// C2 instinct inputs name a lobe and a neuron; in the C2 genomes' standard
// lobe layout (the C2 CAOS guide's order: perception, drive, source, verb,
// noun, sense, decision, attention, concept) lobe 1 holds the drives, 2 the
// source (the category of what stimulated it), 3 heard verbs, 4 heard
// nouns, 5 senses and 7 attention. Source and attention both name the
// object. (Lobe 7 was read as heard nouns until 2026-11-17: the game's
// norns' "hungry, attending to a flower: push or pull it" waited; the
// genomes' wiring shows 7 reading 2 and 4, as attention does.) Lobe 0 neuron 0 marks an unused input. The reinforcer is chemical 52
// (reward) or 53 (punishment).
// Sense 19 (the other sex of the creature's own genus in sight, set by
// feedSensoryCells) names the object of the act: a creature of its own genus
// (category 35 + genus), as the sex instincts mean it (Sex Drive with the
// other sex in sight: kiss).
static bool translateInstinct(creatureInstinctGene* g, InnateInstinct& out, int genus) {
	if (g->drive != 52 && g->drive != 53)
		return false;
	out.reward = (g->drive == 52);
	out.decision = g->action;
	out.strength = g->level / 255.0f;
	out.signature = fmt::format("{}.{}.{}.{}.{}.{}-{}-{}-{}", g->lobes[0], g->neurons[0], g->lobes[1], g->neurons[1],
		g->lobes[2], g->neurons[2], g->action, g->drive, g->level);
	for (int i = 0; i < 3; i++) {
		int lobe = g->lobes[i], neuron = g->neurons[i];
		if (lobe == 0 && neuron == 0)
			continue;
		switch (lobe) {
			case 1: out.conditions.push_back({InnateCondition::Drive, neuron}); break;
			case 2: out.conditions.push_back({InnateCondition::Attention, neuron}); break;
			case 3: out.conditions.push_back({InnateCondition::HeardVerb, neuron}); break;
			case 5:
				if (neuron == 19 && genus >= 1 && genus <= 4)
					out.conditions.push_back({InnateCondition::Attention, 35 + genus});
				else if (neuron == 15) {
					// "it is a creature": any of the creature categories (36-39)
					for (int c = 36; c <= 39; c++)
						out.conditions.push_back({InnateCondition::Attention, c});
				} else if (neuron == 14)
					; // "it is an object": any object, no condition
				else
					out.conditions.push_back({InnateCondition::Sense, neuron});
				break;
			case 4: out.conditions.push_back({InnateCondition::HeardNoun, neuron}); break;
			case 7: out.conditions.push_back({InnateCondition::Attention, neuron}); break;
			default: return false; // not a lobe these genomes use for instincts
		}
	}
	return true;
}

void oldCreature::tickMind() {
	// same cadence as the original brain path
	if ((ticks % 4) != 0)
		return;
	// unconscious: no thought, no choices (C2; see updateConsciousness)
	if (unconscious)
		return;

	chooseAgents();
	if (!mindborn)
		buildMindActions();
	MindPerception p = [&] { PROFILE_SCOPE("creature: perception"); return buildPerception(); }();
	PROFILE_SCOPE("creature: tickMind after perception");
	updateBrainCells(p);
	pendingstimuli.clear();
	if (engine.version == 2) {
		while (!unprocessedinstincts.empty()) {
			InnateInstinct inst;
			if (translateInstinct(unprocessedinstincts.front(), inst, parentagent->genus))
				p.newInstincts.push_back(inst);
			unprocessedinstincts.pop_front();
		}
	}

	if (!mindborn) {
		mindborn = true;
		mind->born(p);
	}
	static const bool watchrides = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
	if (watchrides && p.riding != wasriding) {
		Agent* v = parentagent->invehicle.get();
		fmt::print("* WATCH RIDE unid {} {} at ({:.0f}, {:.0f}){}\n", parentagent->getUNID(), p.riding ? "boards" : "leaves", p.x, p.y,
			v ? fmt::format(" vehicle {} {} {}", (int)v->family, (int)v->genus, v->species) : std::string());
	}
	wasriding = p.riding;
	MindOutput out = [&] { PROFILE_SCOPE("creature: mind"); return mind->tick(p); }();
	if (out.noveltyOutcome != 0)
		temperamentExperience(EXPERIENCE_EXPLORE, (float)out.noveltyOutcome);
	if (out.passOver) {
		passedover.erase(std::remove_if(passedover.begin(), passedover.end(), [&](auto& po) { return !po.first || ticks >= po.second || po.first == out.passOver; }), passedover.end());
		passedover.push_back({out.passOver, ticks + 2000}); // about 3 minutes
		if (devenv("OPENC2E_WATCH_CREATURES"))
			fmt::print("[homeostat] creature {} passes over {} {} {} at ({:.0f}, {:.0f})\n", p.creatureId, (int)out.passOver->family,
				(int)out.passOver->genus, out.passOver->species, out.passOver->x, out.passOver->y);
	}

	if (!out.say.empty())
		creatureSays(parentagent, (int)stage, phraseFor(out.say, parentagent, this));
	if (!out.thought.empty())
		Thoughts::think(parentagent, out.thought, out.thoughtKey);

	if (out.expression >= 0 && out.expression < 6) {
		if (SkeletalCreature* sc = dynamic_cast<SkeletalCreature*>(parentagent))
			sc->setFacialExpression((unsigned int)out.expression);
	}

	if (out.sleep == 1 && !asleep)
		setAsleep(true);
	else if (out.sleep == 0 && asleep)
		setAsleep(false);

	AgentRef oldattn = attention;
	int olddecn = decn;
	if (out.attentionCategory >= 0 && out.attentionCategory < (int)chosenagents.size())
		attn = out.attentionCategory;
	// a particular creature chosen: attend to that one
	if (out.attentionAgent && out.attentionAgent->category >= 0 && out.attentionAgent->category < (int)chosenagents.size() &&
		agentInSight(out.attentionAgent)) {
		chosenagents[out.attentionAgent->category] = out.attentionAgent;
		attn = out.attentionAgent->category;
	}
	if (out.decision >= 0)
		decn = out.decision;
	if (attn >= 0 && attn < (int)chosenagents.size())
		attention = chosenagents[attn];

	if (zombie)
		return;

	// Run the world's script for the chosen action when the choice changes,
	// or when the mind asks again after the previous action finished.
	if (decn >= 0 && (oldattn != attention || olddecn != decn || (out.decision >= 0 && p.actionFinished))) {
		static const bool watch = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
		if (watch && attention) {
			float dx = (attention->x + attention->getWidth() / 2.0f) - (parentagent->x + parentagent->getWidth() / 2.0f);
			float dy = (attention->y + attention->getHeight() / 2.0f) - (parentagent->y + parentagent->getHeight() / 2.0f);
			fmt::print("* WATCH ACT unid {} decision {} on {} {} {} (category {}) dx {:.0f} dy {:.0f}\n", parentagent->getUNID(), decn,
				(int)attention->family, (int)attention->genus, attention->species, attn, dx, dy);
		}
		// developer aid: OPENC2E_WATCH_LIFT_USE prints each action a creature
		// starts on a lift: the lift's place, OBV0 (1 while it is moving: its
		// activate scripts then do nothing) and YVEC
		static const bool watchLift = devenv("OPENC2E_WATCH_LIFT_USE") != nullptr;
		if (watchLift && attention)
			if (Lift* lift = dynamic_cast<Lift*>(attention.get()))
				fmt::print("* WATCH LIFT USE t{} unid {} decision {} on lift {} {} {} at {:.0f},{:.0f} obv0 {} yvec {} creature at {:.0f},{:.0f}\n", world.tickcount,
					parentagent->getUNID(), decn, (int)lift->family, (int)lift->genus, lift->species, lift->x, lift->y, lift->var[0].hasInt() ? lift->var[0].getInt() : -1,
					lift->yvec, parentagent->x, parentagent->y);
		parentagent->queueScript(decn + 16);
	}
}

void oldCreature::notifyMindOfDeath() {
	if (!mind || !mindborn || minddied)
		return;
	minddied = true;
	mind->died(buildPerception());
}

void c2eCreature::handleStimulus(c2eStim& stim) {
	// TODO: handle out-of-range verb_amount/noun_amount

	if (stim.verb_id >= 0) {
		c2eLobe* verblobe = brain->getLobeById("verb");
		if (verblobe) {
			if ((unsigned int)stim.verb_id < verblobe->getNoNeurons())
				verblobe->setNeuronInput(stim.verb_id, stim.verb_amount);
		}
	}

	if (stim.noun_id >= 0) {
		c2eLobe* nounlobe = brain->getLobeById("noun");
		if (nounlobe) {
			if ((unsigned int)stim.noun_id < nounlobe->getNoNeurons())
				nounlobe->setNeuronInput(stim.noun_id, stim.noun_amount);
		}
	}

	for (unsigned int i = 0; i < 4; i++) {
		if (stim.drive_id[i] >= 0) {
			unsigned char chemno = stim.drive_id[i] + 148;
			adjustChemical(chemno, stim.drive_amount[i]);
			if (!stim.drive_silent[i]) {
				c2eLobe* resplobe = brain->getLobeById("resp");
				if (resplobe) {
					if ((unsigned int)stim.drive_id[i] < resplobe->getNoNeurons())
						resplobe->setNeuronInput(stim.drive_id[i], stim.drive_amount[i]);
				}
			}
		}
	}
}

// TODO: this needs to be passed noun details, it seems, judging by documentation
void c2eCreature::handleStimulus(unsigned int id, float strength) {
	// note that g->addoffset does not seem to exist in c2e

	c2eStim stim;
	creatureStimulusGene* g = 0;

	// TODO: generate the damn c2eStims in addGene, thus zapping a whole bunch of bugs
	for (auto& gene : genome->genes) {
		if (creatureStimulusGene* x = dynamic_cast<creatureStimulusGene*>(gene.get())) {
			if (x->stim == id) {
				g = x;
				break;
			}
		}
	}
	if (!g)
		return;

	// if we're asleep and the stimulus isn't to be processed when asleep, return
	if (!g->whenasleep && isAsleep())
		return;

	// TODO: g->modulate

	// TODO: is multipler usage below okay?
	float multiplier = (strength == 0.0f ? 1.0f : strength);

	/*
	 * TODO: what the heck does g->intensity do? it seems to almost entirely be 0
	 * in the standard genomes (apart from a 255?) and it doesn't seem to change
	 * anything - fuzzie
	 */
	// TODO: grmph, g->sensoryneuron and g->significance make no sense to me right now
	// either, so commenting the verb_id setting out until someone works it out - fuzzie
	/*if (stim.verb_id != 0) // TODO: this is a guess to stop stuff from resting seemingly forever
		stim.verb_id = reverseMapVerbToNeuron(g->sensoryneuron); */
	// TODO: huh? gene kit has it in 255-ish steps
	stim.verb_amount = g->significance * (1.0f / 124.0f); /* multiply by 0.5? */
	for (unsigned int i = 0; i < 4; i++) {
		// TODO: ack, amount should be bound to range
		// TODO: fuzzie is suspicious about multiply/dividing on g->amounts here
		if (g->drives[i] != 255)
			stim.setupDriveStim(i, g->drives[i], ((g->amounts[i] * (1.0f / 124.0f)) - 1.0f) * multiplier, g->silent[i]);

		if (strength == 0.0f)
			stim.drive_silent[i] = true;
	}

	handleStimulus(stim);
}

/* vim: set noet: */
