/*
 *  Creature.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Tue May 25 2004.
 *  Copyright (c) 2004-2006 Alyssa Milburn. All rights reserved.
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
#include "Creature.h"
#include "Disease.h"
#include "GenomeTraits.h"

#include "Catalogue.h"
#include "Engine.h"
#include "CreatureAgent.h"
#include "World.h"
#include "common/Random.h"
#include "Room.h"
#include "SkeletalCreature.h"
#include "MetaRoom.h"
#include "Lift.h"
#include "Map.h"
#include "c2eBrain.h"
#include "c2eCreature.h"
#include "common/throw_ifnot.h"
#include "historyManager.h"
#include "oldBrain.h"
#include "oldCreature.h"
#include "Temperament.h"
#include "Relationships.h"
#include "Breeding.h"
#include "caosVM.h"
#include "caosScript.h"
#include "Scriptorium.h"

#include <algorithm>
#include <functional>
#include <cassert>
#include <memory>
#include <sstream>
#include <cctype>

std::map<std::string, int>& censusDeaths() {
	static std::map<std::string, int> tally;
	return tally;
}

Creature::Creature(std::shared_ptr<genomeFile> g, bool is_female, unsigned char _variant, CreatureAgent* a) {
	assert(g);
	genome = g;

	female = is_female;
	genus = 0; // TODO: really, we shouldn't do this, and should instead later assert that a genus was set
	variant = _variant;
	stage = baby;

	parent = a;
	assert(parent);
	parentagent = dynamic_cast<Agent*>(parent);
	assert(parentagent);

	alive = true; // ?
	asleep = false; // ?
	dreaming = false; // ?
	tickage = false;
	zombie = false;

	age = ticks = 0;

	attn = decn = -1;

	for (unsigned short& i : tintinfo)
		i = 128;
}

Creature::~Creature() {
}

void Creature::finishInit() {
	processGenes();
}

bool Creature::shouldProcessGene(gene* g) {
	geneFlags& flags = g->header.flags;

	// non-expressed genes are to be ignored
	if (flags.notexpressed)
		return false;

	// gender-specific genes are only to be processed if they are of this
	if (flags.femaleonly && !female)
		return false;
	if (flags.maleonly && female)
		return false;

	// obviously we only switch on at the stage in question
	if (g->header.switchontime != stage)
		return false;

	// TODO: header.variant?

	return true;
}

bool Creature::isGeneExpressed(gene* g) {
	geneFlags& flags = g->header.flags;
	if (flags.notexpressed)
		return false;
	if (flags.femaleonly && !female)
		return false;
	if (flags.maleonly && female)
		return false;
	return g->header.switchontime <= stage;
}

void Creature::processGenes() {
	for (auto& gene : genome->genes) {
		if (shouldProcessGene(gene.get()))
			addGene(gene.get());
	}
}

void oldCreature::processGenes() {
	brain->processGenes();
	Creature::processGenes();
}

void c2Creature::processGenes() {
	// Biochemistry genes belong to the organ they follow in the genome. Walk
	// the whole genome in order (genes of earlier stages included) so that a
	// gene switching on at a later life stage joins that organ, not whichever
	// organ happens to be last. Genes after the brain organ (not modelled)
	// keep going to the organ before it, as before.
	brain->processGenes();
	std::shared_ptr<c2Organ> current;
	for (auto& gp : genome->genes) {
		gene* g = gp.get();
		if (organGene* o = dynamic_cast<organGene*>(g)) {
			if (!o->isBrain()) {
				current = nullptr;
				for (auto& org : organs)
					if (org->getGene() == o)
						current = org;
			}
		}
		if (!shouldProcessGene(g))
			continue;
		addingorgan = current;
		addGene(g);
		if (organGene* o = dynamic_cast<organGene*>(g))
			if (!o->isBrain() && !organs.empty() && organs.back()->getGene() == o)
				current = organs.back();
	}
	addingorgan = nullptr;

	// each organ turns only its new genes into reactions, emitters and
	// receptors (it used to redo all of them at every life stage, so an adult
	// ran every reaction five times over)
	for (auto& organ : organs) {
		organ->processGenes();
	}
}

void c2eCreature::processGenes() {
	// brain must be processed first (to create loci etc)
	// organs should be processed last, because new ones will be created by normal processGenes()

	brain->processGenes();
	Creature::processGenes();
	for (auto& organ : organs) {
		organ->processGenes();
	}
}

void Creature::addGene(gene* g) {
	if (typeid(*g) == typeid(creatureInstinctGene)) {
		unprocessedinstincts.push_back((creatureInstinctGene*)g);
	} else if (typeid(*g) == typeid(creatureGenusGene)) {
		// TODO: mmh, genus changes after setup shouldn't be valid
		genus = ((creatureGenusGene*)g)->genus;
		parentagent->genus = genus + 1;
	} else if (typeid(*g) == typeid(creaturePigmentGene)) {
		creaturePigmentGene& p = *((creaturePigmentGene*)g);
		// red, green or blue: a mutated gene can name another colour, and
		// writing past tintinfo overwrote the creature's next members (a
		// mutated ettin egg's genome, zxyi, crashed Home, 2026-11-12)
		if (p.color < 3)
			tintinfo[p.color] = p.amount;
	} else if (typeid(*g) == typeid(creaturePigmentBleedGene)) {
		creaturePigmentBleedGene& p = *((creaturePigmentBleedGene*)g);
		tintinfo[3] = p.rotation;
		tintinfo[4] = p.swap;
	}
}

void Creature::ageCreature() {
	if (stage >= senile) {
		diedofoldage = true;
		die();
		return;
	} //previously we just returned

	stage = (lifestage)((int)stage + 1);
	processGenes();

	assert(parent);
	parent->creatureAged();
#ifndef _CREATURE_STANDALONE
	world.history->getMoniker(world.history->findMoniker(genome)).addEvent(4, "", ""); // aged event
	world.history->tell(world.history->findMoniker(genome), storyEntry::stage, "", std::to_string((int)stage));
#endif
}

void Creature::setAsleep(bool a) {
	// TODO: skeletalcreature might need to close eyes? or should that just be done during the skeletal update?
	if (!a && dreaming)
		setDreaming(false);
	asleep = a;
}

void Creature::setDreaming(bool d) {
	if (d && !asleep)
		setAsleep(true);
	dreaming = d;
}

void Creature::born() {
	// C1/C2: a newborn becomes the selected creature if none is selected.
	if (engine.version < 3 && !world.selectedcreature)
		world.selectCreature(AgentRef(parentagent).lock());

	parent->creatureBorn();

	// TODO: life event?
#ifndef _CREATURE_STANDALONE
	{
		std::string mon = world.history->findMoniker(genome);
		monikerData& m = world.history->getMoniker(mon);
		m.wasBorn();
		m.addEvent(3, "", ""); // born event, parents..
		if (!world.history->quiet) { // a real birth, not a creature restored from a save
			m.borntick = world.worldtickcount;
			m.bornknown = true;
			if (m.name.empty())
				m.name = world.history->nextDefaultName(m.genus);
			world.history->logLife(lifeLogEntry::birth, mon, (int)stage);
			world.history->tell(mon, storyEntry::born);
			// and in each parent's story: a child of theirs hatched
			for (const std::string* p : {&m.mother, &m.father})
				if (!p->empty())
					world.history->tell(*p, storyEntry::childHatched, mon);
		}
	}
#endif

	tickage = true;
}

void Creature::die() {
	parent->creatureDied();

	// TODO: life event?
#ifndef _CREATURE_STANDALONE
	{
		std::string mon = world.history->findMoniker(genome);
		monikerData& m = world.history->getMoniker(mon);
		m.hasDied();
		m.addEvent(7, "", ""); // died event
		if (!m.died) {
			m.died = true;
			m.diedtick = world.worldtickcount;
			m.diedstage = (int)stage;
			// its genes, kept for the Breeder's Kit's Insemination (Breeding.h)
			if (engine.version == 2 && genome)
				if (auto* oc = dynamic_cast<oldCreature*>(this))
					keepDepartedGenes(mon, *genome, oc->inheritableTemperament(), oc->seasonalZ, oc->isFemale());
			// C2's nutrient cycle (World::tickNutrients; owner, 2026-11-17: the
			// dead feed the soil): organic nutrient in the room where it died,
			// more from a grown body (a baby 15, a child 25, an adolescent 35,
			// older 50)
			if (engine.version == 2 && world.map && parentagent && !devenv("OPENC2E_NO_NUTRIENT_CYCLE")) {
				static const int amount[7] = {15, 25, 35, 50, 50, 50, 50};
				float cx = parentagent->x + parentagent->getWidth() / 2.0f, cy = parentagent->y + parentagent->getHeight() / 2.0f;
				if (auto r = world.map->roomAt(cx, cy)) {
					int add = amount[std::clamp((int)stage, 0, 6)];
					r->ontr = std::min(255, r->ontr + add);
					if (r->ontrf >= 0.0f)
						r->ontrf = std::min(255.0f, r->ontrf + add);
				}
			}
			std::string cause = diedofoldage ? "old age" : !diedof.empty() ? diedof : "unknown causes";
			censusDeaths()[cause]++;
			size_t before = world.history->notices.size();
			world.history->logLife(lifeLogEntry::death, mon, (int)stage, diedofoldage);
			if (world.history->notices.size() > before) {
				world.history->notices.back().cause = cause;
				world.history->lifelog.back().cause = cause;
				if (diedofoldage) {
					lifeLogEntry d = world.history->lifelog.back(); // (a copy: the log may grow)
					world.history->checkTogether(d);
				}
			}
			world.history->tell(mon, storyEntry::died, "", cause);
		}
	}
#endif
	// TODO: disable brain/biochemistry updates

	// force die script
	parentagent->stopScript();
	parentagent->queueScript(72); // Death script in c1, c2 and c2e

	// skeletalcreature eyes, also? see setAsleep comment
	alive = false;
}

void Creature::tick() {
	ticks++;

	if (!alive)
		return;

	if (tickage)
		age++;
}

/*
 * oldCreature contains the shared elements of C1 of C2 (creatures are mostly identical in both games)
 */
oldCreature::oldCreature(std::shared_ptr<genomeFile> g, bool is_female, unsigned char _variant, CreatureAgent* a)
	: Creature(g, is_female, _variant, a) {
	// each creature its own temperament (a saved world puts back its own)
	temperamentBorn = Temperaments::roll();
	// and the part its C2 genes give (GenomeTraits.h)
	if (engine.version == 2 && g)
		geneShift = GenomeTraits::shift(*g);
	refreshTemperament();
	mindGenes = MindGeneSet::roll();
	// its own pace of ageing, 0.9-1.1 and most often near 1 (the sum of two
	// draws); OPENC2E_NO_LIFE_SPREAD keeps every creature at its genome's
	if (engine.version == 2 && !devenv("OPENC2E_NO_LIFE_SPREAD"))
		lifespanScale = 1.0f + rand_float(-0.05f, 0.05f) + rand_float(-0.05f, 0.05f);
	if (!engine.mindname.empty()) {
		mind = createCreatureMind(engine.mindname);
		// C1/C2 agents fall into categories 0-39 (see World::findCategory)
		chosenagents.resize(40);
	}

	biochemticks = 0;
	halflives = 0;

	for (unsigned char& i : floatingloci)
		i = 0;
	for (unsigned char& i : lifestageloci)
		i = 0;
	for (unsigned char& i : involaction)
		i = 0;
	for (unsigned char& chemical : chemicals)
		chemical = 0;

	for (unsigned int& i : involactionlatency)
		i = 0;

	muscleenergy = 0;
	fertile = pregnant = receptive = 0;
	dead = 0;

	brain = 0; // just in case
}

c1Creature::c1Creature(std::shared_ptr<genomeFile> g, bool is_female, unsigned char _variant, CreatureAgent* a)
	: oldCreature(g, is_female, _variant, a) {
	assert(g->getVersion() == 1);

	for (unsigned char& sense : senses)
		sense = 0;
	for (unsigned char& i : gaitloci)
		i = 0;
	for (unsigned char& drive : drives)
		drive = 0;

	// TODO: chosenagents size

	brain = new oldBrain(this);
	finishInit();
	brain->init();
}

c2Creature::c2Creature(std::shared_ptr<genomeFile> g, bool is_female, unsigned char _variant, CreatureAgent* a)
	: oldCreature(g, is_female, _variant, a) {
	assert(g->getVersion() == 2);

	for (unsigned char& sense : senses)
		sense = 0;
	for (unsigned char& i : gaitloci)
		i = 0;
	for (unsigned char& drive : drives)
		drive = 0;

	mutationchance = 0;
	mutationdegree = 0;

	// TODO: chosenagents size

	brain = new oldBrain(this);
	finishInit();
	brain->init();
}

c2eCreature::c2eCreature(std::shared_ptr<genomeFile> g, bool is_female, unsigned char _variant, CreatureAgent* a)
	: Creature(g, is_female, _variant, a) {
	assert(g->getVersion() == 3);

	for (float& chemical : chemicals)
		chemical = 0.0f;

	// initialise loci
	for (float& i : lifestageloci)
		i = 0.0f;
	muscleenergy = 0.0f;
	for (float& i : floatingloci)
		i = 0.0f;
	fertile = pregnant = ovulate = receptive = chanceofmutation = degreeofmutation = 0.0f;
	dead = 0.0f;
	for (float& i : involaction)
		i = 0.0f;
	for (float& i : gaitloci)
		i = 0.0f;
	for (float& sense : senses)
		sense = 0.0f;
	for (float& drive : drives)
		drive = 0.0f;

	for (unsigned int& i : involactionlatency)
		i = 0;

	halflives = 0;

	if (!catalogue.hasTag("Action Script To Neuron Mappings"))
		throw Exception("c2eCreature was unable to read the 'Action Script To Neuron Mappings' catalogue tag");
	const std::vector<std::string>& mappinginfotag = catalogue.getTag("Action Script To Neuron Mappings");
	for (const auto& i : mappinginfotag)
		mappinginfo.push_back(std::stoi(i));

	// TODO: should we really hard-code this?
	chosenagents.resize(40);

	brain = new c2eBrain(this);
	finishInit();
	brain->init();
}

unsigned int c1Creature::getGait() {
	unsigned int gait = 0;

	for (unsigned int i = 1; i < 8; i++)
		if (gaitloci[i] > gaitloci[gait])
			gait = i;

	return gait;
}

unsigned int c2Creature::getGait() {
	unsigned int gait = 0;

	// the courting walk (gait 13, which a norn's genes set from its sex drive
	// chemical) comes readily as a youth or adult, less as an adolescent or
	// when old, never as a baby, child or when senile (owner, 2026-11-10:
	// vigor with age)
	static const float courting[7] = {0.0f, 0.0f, 0.6f, 1.0f, 1.0f, 0.4f, 0.0f};
	auto level = [&](unsigned int i) {
		float v = gaitloci[i];
		if (i == 13 && (int)stage >= 0 && (int)stage < 7)
			v *= courting[(int)stage];
		return v;
	};
	for (unsigned int i = 1; i < 16; i++)
		if (level(i) > level(gait))
			gait = i;

	return gait;
}

unsigned int c2eCreature::getGait() {
	unsigned int gait = 0;

	for (unsigned int i = 1; i < 16; i++)
		if (gaitloci[i] > gaitloci[gait])
			gait = i;

	return gait;
}

void c1Creature::tick() {
	// TODO: should we tick some things even if dead?
	if (!alive) {
		PROFILE_SCOPE("creature: dead bodies");
		notifyMindOfDeath();
		return;
	}

	senses[0] = 255; // always-on
	senses[1] = (asleep ? 255 : 0); // asleep
	senses[2] = 0; // air coldness (TODO)
	senses[3] = 0; // air hotness (TODO)
	senses[4] = 0; // light level (TODO)
	senses[5] = 0; // crowdedness (TODO)

	tickBrain();
	tickBiochemistry();

	// lifestage checks
	for (unsigned int i = 0; i < 7; i++) {
		if ((lifestageloci[i] != 0) && (stage == (lifestage)i))
			ageCreature();
	}

	// C1/C2: the world's egg-laying script takes the baby's genome name from
	// BABY and sets it to 0 as the egg is laid; that ends the pregnancy
	if (pregnant != 0 && parentagent && parentagent->babymoniker == 0)
		pregnant = 0;

	{
		PROFILE_SCOPE("creature: pregnancy watch");
		watchPregnancy();
	}
	{
		PROFILE_SCOPE("creature: keep in room system");
		keepInRoomSystem();
	}

	if (dead != 0) {
		// the genome's own death signal (C2's "dead" locus): its chemistry
		// says it is dead
		if (diedof.empty())
			diedof = "illness";
		die();
	}

	Creature::tick();
}

// C2 creatures breathe through the genome: an emitter on the air-quality locus
// (sensorimotor 9) makes Air, which the genome turns into oxygen. The engine's
// part is the air quality. With the head in a sea room (room type 2, "in the
// sea") there is no air: the genome then runs short of oxygen and the creature
// suffocates. Bubbles rise from the head while it is under water, using the
// world's own bubble object (classifier 2 1 27, sprite "bubs").
void c2Creature::updateUnderwater() {
	if (!parentagent || !world.map)
		return;
	// the mouth (point 1 of the head part's attachments), or near the top
	// of the body if the creature has no skeleton
	float headx = parentagent->x + parentagent->getWidth() / 2.0f;
	float heady = parentagent->y + 10.0f;
	if (SkeletalCreature* sc = dynamic_cast<SkeletalCreature*>(parentagent)) {
		headx = parentagent->x + sc->attachmentX(1, 1);
		heady = parentagent->y + sc->attachmentY(1, 1);
	}
	std::shared_ptr<Room> room = world.map->roomAt(headx, heady);
	bool underwater = room && room->type == 2;
	// Standing on dry floor, the head is not under water, even where the
	// box of a water room beside the floor's edge reaches higher than the
	// head: at the end of the green walkway east of the incubator (room 14,
	// floor 892) a creature facing the sea's room (31, up to y 669) blew
	// bubbles (owner, 2026-11-10; 22 such edges on the map). Only with the
	// feet in water too (or falling, or swimming: no foot room) is it under.
	if (underwater)
		if (SkeletalCreature* sc = dynamic_cast<SkeletalCreature*>(parentagent))
			if (std::shared_ptr<Room> feet = sc->footRoom(); feet && feet->type != 2)
				underwater = false;
	// Inside a vehicle (the submarine) it breathes the vehicle's air: a norn
	// carried under the sea blew bubbles (owner, 2026-11-10)
	if (underwater && parentagent->invehicle)
		underwater = false;
	// a creature that breathes water (owner, 2026-11-11: frog norns are
	// amphibious) keeps its air: no breath held, no bubbles
	if (underwater && breathesWater())
		underwater = false;
	if (underwater) {
		if (breathHeld < breathTicks)
			breathHeld++; // holding its breath: the air sense stays as it was
		else
			senses[9] = 0; // out of breath
	}
	if (!underwater) {
		breathHeld = breathHeld > 100 ? breathHeld - 100 : 0; // breathing again
		bubbletimer = 0;
		return;
	}
	if (bubbletimer > 0) {
		bubbletimer--;
		return;
	}
	bubbletimer = rand_int32(12, 40);
	static const bool fastBubbles = devenv("OPENC2E_TEST_BUBBLES") != nullptr; // developer aid: a bubble every few ticks
	if (fastBubbles)
		bubbletimer = 3;
	if (devenv("OPENC2E_DEBUG_BUBBLES"))
		if (SkeletalCreature* sc = dynamic_cast<SkeletalCreature*>(parentagent))
			fmt::print("* BUBBLE agent {:.0f},{:.0f} size {}x{} mouth {},{} neck {},{} -> {:.0f},{:.0f}\n", parentagent->x, parentagent->y, parentagent->getWidth(),
				parentagent->getHeight(), sc->attachmentX(1, 1), sc->attachmentY(1, 1), sc->attachmentX(1, 0), sc->attachmentY(1, 0), headx, heady);
	// marked as a breath bubble (OV01 7777): the world makes those rise
	// (World::tickBreathBubbles; the game's bubble object has no movement
	// of its own, so bubbles hung where they were blown while the creature
	// sank or walked on: owner, 2026-11-11)
	// where the bubble starts (owner, 2026-11-11): beside the head on the
	// side it faces, at its snout (SkeletalCreature::bubbleSpot); a creature
	// without a skeleton, near its middle top
	float bx = headx - 5, by = heady - 5;
	if (SkeletalCreature* sc = dynamic_cast<SkeletalCreature*>(parentagent))
		sc->bubbleSpot(bx, by);
	std::string caos = fmt::format("inst new: simp bubs 10 0 8000 0 setv cls2 2 1 27 setv ov01 7777 mvto {} {} mesg writ targ 8",
		(int)bx, (int)by);
	std::string result = engine.executeNetwork(caos);
	if (!result.empty())
		fmt::print("* Couldn't make a bubble: {}\n", result);
}

// C2: a dead creature's body. The death script (event 72) lets it lie for
// 30 s, then makes it pass through walls and floors (attr 0, size 0) and
// float up out of the top of the world, where it is removed (death.cos).
// - A lying body is kept in the room system: a dead ettin came to rest in the
//   rock under the upper-east deck's end (7743,968), out of every room, and
//   raised an error 737 times through the night until it was cleared
//   (2026-11-13). A rising one is not: it passes through rock on purpose,
//   and being set down on a floor with its speed taken away left it there
//   for good, its script waiting for it to reach the top (34 bodies in Home,
//   2026-11-14).
// - Should the death script not be running, it is started again; should a
//   rising body have stopped, it is set rising again. Five bodies lay at the
//   gene lab's lower room's east wall for hours without ever rising, and
//   hungry creatures crowded round them (owner, 2026-11-14).
void oldCreature::tickBody() {
#ifndef _CREATURE_STANDALONE
	Agent* a = parentagent;
	if (!a || engine.version != 2)
		return;
	bool lying = (a->attr & 64) != 0;
	if (lying)
		keepInRoomSystem();
	if (a->carriedby || a->invehicle)
		return;
	deadTicks++;
	if (deadTicks < 400 || deadTicks % 50 != 0)
		return;
	bool running = a->vm && !a->vm->stopped() && a->vm->currentscript && a->vm->currentscript->scrp == 72;
	if (!running) {
		fmt::print("* The body of {} at {:.0f},{:.0f} was not running its death script; started it again\n",
			world.history->findMoniker(genome), a->x, a->y);
		a->stopScript();
		a->queueScript(72);
	} else if (!lying && a->size == 0 && a->vely == 0.0f) {
		fmt::print("* The body of {} at {:.0f},{:.0f} had stopped rising; set rising again\n",
			world.history->findMoniker(genome), a->x, a->y);
		a->vely = -2;
	}
#endif
}

int oldCreature::companyNear(float radius, float minAffinity, int most) {
	if (companyAt != 0 && world.tickcount < companyAt + 20 && world.tickcount >= companyAt)
		return companyCount;
	companyAt = world.tickcount ? world.tickcount : 1;
	companyCount = 0;
	if (!parentagent || bonds.empty())
		return 0;
	float cx = parentagent->x + parentagent->getWidth() / 2.0f, cy = parentagent->y + parentagent->getHeight() / 2.0f;
	for (auto& a : world.agents) {
		auto* ca = dynamic_cast<CreatureAgent*>(a.get());
		if (!ca || a.get() == parentagent || a->isDying() || !ca->getCreature() || !ca->getCreature()->isAlive())
			continue;
		float ox = a->x + a->getWidth() / 2.0f, oy = a->y + a->getHeight() / 2.0f;
		if (std::fabs(ox - cx) > radius || std::fabs(oy - cy) > radius)
			continue;
		const Bond* b = Relationships::bondOf(this, world.history->findMoniker(ca->getCreature()->getGenome()));
		if (b && b->affinity >= minAffinity && ++companyCount >= most)
			break;
	}
	return companyCount;
}

void c2Creature::tick() {
	// TODO: should we tick some things even if dead?
	if (!alive) {
		notifyMindOfDeath();
		tickBody();
		return;
	}

	senses[0] = 255; // always-on
	senses[1] = (asleep ? 255 : 0); // asleep
	senses[2] = 0; // air coldness: below the comfort band, see below
	senses[3] = 0; // air hotness: above it
	senses[4] = 0; // light level: the room's (rooms keep the light level the world gives them)
	if (parentagent && world.map) {
		if (std::shared_ptr<Room> r = world.map->roomAt(parentagent->x + parentagent->getWidth() / 2.0f, parentagent->y + parentagent->getHeight() / 2.0f)) {
			senses[4] = (unsigned char)std::clamp(r->lite, 0, 255);
			// Room temperature (0-255) to the genome's air coldness/hotness
			// senses (its emitters fire above 8). Comfortable from 50 to 140,
			// three per degree beyond. The hot side agrees with real C2 (a
			// newborn heated up in a ~170 room, not at 89-114); the cold side
			// is our choice: real C2 creatures felt no cold even at 30.
			// Company warms (owner, 2026-11-16: winter bonds; animals huddle
			// in the cold, small ones gaining most): in a cold room each
			// creature it is bonded to within 120 px (affinity 0.3 or more,
			// at most three) counts as 8 degrees warmer, times its life
			// stage's weight for bonds (babies 1.5, adults 0.8).
			int temp = r->temp;
			if (temp < 50 && SeasonalRhythm::behaviorOn())
				temp += (int)std::lround(8.0f * companyNear(120.0f, 0.3f, 3) * SeasonalRhythm::stageWeight(SeasonalRhythm::SEASON_BONDS, (int)stage));
			senses[2] = (unsigned char)std::clamp((50 - temp) * 3, 0, 255);
			senses[3] = (unsigned char)std::clamp((r->temp - 140) * 3, 0, 255);
		}
	}
	// Crowdedness, radiation and slopes (2026-11-16): every C2 genome (norn,
	// grendel, ettin, Frog, Boney) reads them, and they were 0. The genome's
	// inverted crowdedness emitter (Loneliness, chemical 8) then fed every
	// creature loneliness even in a crowd, and its Crowded emitter (9) never
	// fired. Our own definitions (C2's exact ones are not documented):
	// crowdedness 40 for each other living creature within 200 px across and
	// 120 up or down, to 255; radiation the room's; slope up and slope down
	// the floor's rise or fall under it, the way it faces, 512 per pixel of
	// rise per pixel (the genes' digital emitters fire above 64, about 7
	// degrees). Time of day, season and wind are read by no genome: left 0.
	senses[5] = 0;
	senses[6] = 0;
	senses[10] = 0;
	senses[11] = 0;
	static const bool oldSenses = devenv("OPENC2E_NO_NEW_SENSES") != nullptr; // (developer aid: as before, for comparing)
	if (parentagent && world.map && !oldSenses) {
		float cx = parentagent->x + parentagent->getWidth() / 2.0f;
		float cy = parentagent->y + parentagent->getHeight() / 2.0f;
		// (the living creatures' middles, gathered once a world tick)
		static unsigned int gatheredAt = ~0u;
		static std::vector<std::pair<Agent*, std::pair<float, float>>> living;
		if (gatheredAt != world.tickcount) {
			gatheredAt = world.tickcount;
			living.clear();
			for (auto& a : world.agents)
				if (auto* ca = dynamic_cast<CreatureAgent*>(a.get()); ca && !a->isDying() && ca->getCreature() && ca->getCreature()->isAlive())
					living.push_back({a.get(), {a->x + a->getWidth() / 2.0f, a->y + a->getHeight() / 2.0f}});
		}
		int near = 0;
		for (auto& [a, pos] : living)
			if (a != parentagent && std::fabs(pos.first - cx) < 200.0f && std::fabs(pos.second - cy) < 120.0f)
				near++;
		senses[5] = (unsigned char)std::min(255, near * 40);
		if (std::shared_ptr<Room> r = world.map->roomAt(cx, cy)) {
			senses[6] = (unsigned char)std::clamp(r->radn, 0, 255);
			if (r->floorpoints.size()) {
				float rise = (r->floorYatX(cx - 8) - r->floorYatX(cx + 8)) / 16.0f; // up to the right (y grows downward)
				int facing = 2;
				if (auto* sc = dynamic_cast<SkeletalCreature*>(parentagent))
					facing = (int)sc->facing();
				float ahead = facing == 2 ? rise : facing == 3 ? -rise : 0.0f;
				senses[10] = (unsigned char)std::clamp((int)(ahead * 512.0f), 0, 255);
				senses[11] = (unsigned char)std::clamp((int)(-ahead * 512.0f), 0, 255);
			}
		}
	}
	senses[7] = 0; // time of day (no genome reads it)
	senses[8] = 0; // season (no genome reads it)
	senses[9] = 255; // air quality: full, unless the head is under water (below)
	updateUnderwater();
	// developer aid: OPENC2E_TEST_NO_AIR cuts the air of a creature a script
	// marks with OV96 = 1, as if it were out of breath under water (to time
	// how long a creature can go without air before it cannot recover)
	static const bool testNoAir = devenv("OPENC2E_TEST_NO_AIR") != nullptr;
	if (testNoAir && parentagent && parentagent->var[96].hasInt() && parentagent->var[96].getInt() == 1)
		senses[9] = 0;
	senses[12] = 0; // wind towards (no genome reads it)
	senses[13] = 0; // wind behind (no genome reads it)

	{
		PROFILE_SCOPE("creature: tickBrain (whole)");
		tickBrain();
	}
	// TODO: update brain organ every 0.4ms (ie: when brain is processed)!
	{
		PROFILE_SCOPE("creature: biochemistry");
		tickBiochemistry();
	}
	{
		PROFILE_SCOPE("creature: consciousness");
		updateConsciousness();
	}

	// developer aid: with OPENC2E_WATCH_CREATURES, organ health every 400 ticks
	static const bool watchorgans = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
	if (watchorgans && ticks % 400 == 0 && !organs.empty()) {
		float minpct = 100.0f, sumpct = 0.0f;
		int deadorgans = 0;
		for (auto& o : organs) {
			float pct = o->getInitialLifeforce() > 0 ? 100.0f * o->getLongTermLifeforce() / o->getInitialLifeforce() : 0.0f;
			minpct = std::min(minpct, pct);
			sumpct += pct;
			if (o->getLongTermLifeforce() <= 0.5f)
				deadorgans++;
		}
		fmt::print("* WATCH ORGANS t{} unid {} atp {} min {:.1f}% mean {:.1f}% dead {}/{}\n", ticks, parentagent ? parentagent->getUNID() : -1,
			(int)chemicals[99], minpct, sumpct / organs.size(), deadorgans, organs.size());
	}

	// lifestage checks
	for (unsigned int i = 0; i < 7; i++) {
		if ((lifestageloci[i] != 0) && (stage == (lifestage)i))
			ageCreature();
	}

	static const bool watchChem = devenv("OPENC2E_WATCH_CHEM") != nullptr;
	if (watchChem)
		watchChemistry();
	// developer aid: OPENC2E_WATCH_FUEL=N prints each creature's hunger and
	// fuel chemicals (and the breathing ones: pyruvate, lactate, oxygen,
	// carbon dioxide, air) every N ticks (the CAOS CHEM reading has no way to tell
	// creatures apart in C2: UNID is not readable there)
	static const int watchFuel = devenv("OPENC2E_WATCH_FUEL") ? atoi(devenv("OPENC2E_WATCH_FUEL")) : 0;
	if (watchFuel > 0 && world.tickcount % watchFuel == 0 && world.history)
		fmt::print("* FUEL {} t{} hunger {} glucose {} glycogen {} starch {} fat {} adipose {} energy {} ATP {} ADP {} pyruvate {} lactate {} oxygen {} CO2 {} air {} PGK {} breath held {}{}\n",
			world.history->findMoniker(genome), world.tickcount, (int)chemicals[3], (int)chemicals[70], (int)chemicals[72],
			(int)chemicals[73], (int)chemicals[74], (int)chemicals[75], (int)chemicals[98], (int)chemicals[99], (int)chemicals[100],
			(int)chemicals[69], (int)chemicals[68], (int)chemicals[95], (int)chemicals[90], (int)chemicals[96], (int)chemicals[179], breathHeld, unconscious ? " (unconscious)" : "");

	// C1/C2: the world's egg-laying script takes the baby's genome name from
	// BABY and sets it to 0 as the egg is laid; that ends the pregnancy
	if (pregnant != 0 && parentagent && parentagent->babymoniker == 0)
		pregnant = 0;

	watchPregnancy();
	keepInRoomSystem();

	if (dead != 0) {
		// the genome's own death signal (C2's "dead" locus): its chemistry
		// says it is dead
		if (diedof.empty())
			diedof = "illness";
		die();
	}

	Creature::tick();
}

void c2Creature::watchChemistry() {
	const int ATP = 99;
	chemHistory.emplace_back(chemicals, chemicals + 256);
	organHistory.push_back(getOrganState());
	if (chemHistory.size() > 400) {
		chemHistory.pop_front();
		organHistory.pop_front();
	}
	if (chemDumped || chemicals[ATP] >= 30)
		return;
	unsigned char peak = 0;
	for (auto& h : chemHistory)
		peak = std::max(peak, h[ATP]);
	if (peak < 180)
		return;
	chemDumped = true;
	int unid = parentagent ? parentagent->getUNID() : -1;
	size_t n = chemHistory.size();
	fmt::print("* CHEM COLLAPSE unid {} tick {} asleep {} dead-locus {} ATP {} (peak {}) over the last {} ticks; every 20th tick, oldest first:\n",
		unid, world.tickcount, (bool)asleep, (int)dead, (int)chemicals[ATP], (int)peak, n);
	for (int c = 1; c < 256; c++) {
		int lo = 255, hi = 0;
		for (auto& h : chemHistory) {
			lo = std::min(lo, (int)h[c]);
			hi = std::max(hi, (int)h[c]);
		}
		if (hi - lo < 30 && c != ATP)
			continue;
		std::string row;
		for (size_t i = 0; i < n; i += 20)
			row += fmt::format(" {}", (int)chemHistory[i][c]);
		row += fmt::format(" | now {}", (int)chemHistory[n - 1][c]);
		fmt::print("*   chem {:3}:{}\n", c, row);
	}
	for (size_t o = 0; 2 * o + 1 < organHistory.back().size(); o++) {
		float first = organHistory.front()[2 * o + 1], last = organHistory.back()[2 * o + 1];
		float sfirst = organHistory.front()[2 * o], slast = organHistory.back()[2 * o];
		if (std::fabs(first - last) > 1.0f || std::fabs(sfirst - slast) > first * 0.05f || last <= 0.5f)
			fmt::print("*   organ {}: short-term life {:.0f} -> {:.0f}, long-term {:.0f} -> {:.0f}\n", o, sfirst, slast, first, last);
	}
}

void c2eCreature::tick() {
	// TODO: should we tick some things even if dead?
	if (!alive)
		return;

	// TODO: update muscleenergy

	senses[0] = 1.0f; // always-on
	senses[1] = (asleep ? 1.0f : 0.0f); // asleep
	// space for old C2 senses: hotness, coldness, light level
	senses[5] = 0.0f; // crowedness (TODO)
	// space for old C2 senses: radiation, time of day, season
	senses[9] = 1.0f; // air quality (TODO)
	senses[10] = 0.0f; // steepness of upcoming slope (up) (TODO)
	senses[11] = 0.0f; // steepness of upcoming slope (down) (TODO)
	// space for old C2 senses: oncoming wind, wind from behind

	tickBrain();
	tickBiochemistry();

	// lifestage checks
	for (unsigned int i = 0; i < 7; i++) {
		if ((lifestageloci[i] != 0.0f) && (stage == (lifestage)i))
			ageCreature();
	}

	if (dead != 0.0f)
		die();

	Creature::tick();
}

// A creature found outside every room (inside the ground, under a floor),
// not held and not riding, is lifted to the nearest room straight above it,
// a dry one if there is one within reach, and stood on its floor. An old norn
// swimming in the underground pond sank through its bed into the rock
// (owner, 2026-11-11); the floor check in Agent::physicsTick leaves water
// rooms alone, so nothing brought her back. Checked about twice a second.
// whether a room's floor at x is an open doorway into water below
static bool floorOpensOntoWater(Room* r, float x) {
	float span = std::max(1.0f, (float)r->x_right - (float)r->x_left);
	float floor = r->y_left_floor + ((float)r->y_right_floor - (float)r->y_left_floor) * (x - r->x_left) / span;
	for (auto& d : r->getDoors()) {
		Room* o = d.first.get();
		if (!o || d.second.perm <= 0 || o->type != 2 || x < o->x_left || x > o->x_right)
			continue;
		float ospan = std::max(1.0f, (float)o->x_right - (float)o->x_left);
		float ceiling = o->y_left_ceiling + ((float)o->y_right_ceiling - (float)o->y_left_ceiling) * (x - o->x_left) / ospan;
		if (std::fabs(ceiling - floor) <= 4)
			return true;
	}
	return false;
}

void oldCreature::keepInRoomSystem() {
#ifndef _CREATURE_STANDALONE
	if (!parentagent || !world.map || parentagent->carriedby || parentagent->invehicle)
		return;
	if (keepcheck > 0) {
		keepcheck--;
		return;
	}
	keepcheck = 5;
	Agent* a = parentagent;
	float cx = a->x + a->getWidth() / 2.0f, cy = a->y + a->getHeight() / 2.0f;
	// across the world's seam: a creature whose middle is past the map's
	// east edge (walking down the volcano passage into the potion room,
	// x 8352 = 0) is judged in the map's own terms (shift), and moved back
	// in its own (Map::roomAt finds no room past the edge, so it was taken
	// for a creature in rock and "rescued" 760 px into the library;
	// owner, 2026-11-13)
	float shift = 0;
	if (MetaRoom* wm = world.map->wrappingMetaRoomNear(cx, cy); wm && wm->wraparound()) {
		if (cx >= (float)wm->x() + wm->width())
			shift = -(float)wm->width();
		else if (cx < (float)wm->x())
			shift = (float)wm->width();
	}
	cx += shift;
	float ax = a->x + shift; // its x in those terms

	// A sealed pocket: a room no open door (permeability above 0) joins to
	// the rest of the world, like room 208 under the incubator lift's shaft,
	// where a norn lay trapped (owner, 2026-11-11), or the pockets a grendel
	// and an ettin were found in before. Whatever put the creature there, it
	// can never walk out: lift it to the floor of the first room above it
	// that belongs to the world. Rooms are grouped through open doors; the
	// largest group is the world (refreshed every minute, as additions edit
	// doors).
	static std::map<Room*, int> group;
	static std::map<int, int> groupSize;
	static int worldGroup = -1;
	static unsigned int grouped = UINT_MAX;
	if (grouped == UINT_MAX || world.tickcount - grouped > 600) {
		grouped = world.tickcount;
		group.clear();
		// a door a script can open joins its rooms while shut: the glass
		// terrarium by the incubator (room 85, pear plants inside) is shut
		// off from room 81 by a door its door object (2 9 21) opens and
		// shuts, and until it was first opened in a session every creature
		// inside was "rescued" out through the glass, and hungry ones
		// outside, reaching for the pears behind it, again and again (up to
		// 21 times a 5-minute copy; 2026-11-19). Any script line "setv door
		// <dir> <room> <room> <perm>" with a perm other than a plain 0 counts
		// (scanned again whenever the number of scripts changes).
		static size_t scannedScripts = SIZE_MAX;
		if (world.scriptorium && world.scriptorium->scriptCount() != scannedScripts) {
			scannedScripts = world.scriptorium->scriptCount();
			int marked = 0;
			world.scriptorium->forEachCode([&](const std::string& code) {
				std::string lc = code;
				std::transform(lc.begin(), lc.end(), lc.begin(), [](unsigned char c) { return (char)std::tolower(c); });
				for (size_t at = lc.find("setv door "); at != std::string::npos; at = lc.find("setv door ", at + 10)) {
					std::istringstream in(lc.substr(at + 10, 40));
					std::string dir, perm;
					int r1 = -1, r2 = -1;
					if (!(in >> dir >> r1 >> r2 >> perm) || perm == "0")
						continue;
					std::shared_ptr<Room> a1 = world.map->getRoom(r1), a2 = world.map->getRoom(r2);
					if (a1 && a2 && world.map->hasDoor(a1, a2) && world.map->getDoorPerm(a1, a2) <= 0) {
						world.map->markDoorOpenable(a1, a2);
						marked++;
					}
				}
			});
			if (devenv("OPENC2E_LIST_POCKETS"))
				fmt::print("* Doors scripts open, shut now: {}\n", marked);
		}
		std::map<int, int> sizes;
		int g = 0;
		for (auto& r0 : world.map->getRooms()) {
			if (group.count(r0.get()))
				continue;
			std::vector<std::shared_ptr<Room>> todo = {r0};
			group[r0.get()] = g;
			while (!todo.empty()) {
				auto r = todo.back();
				todo.pop_back();
				sizes[g]++;
				// (a door open at any time this session counts: the
				// airlock's chamber, both its doors shut mid-cycle, was
				// taken for a sealed pocket and a norn in it was lifted
				// 800 px onto the dock; owner's screencast, 2026-11-13)
				for (auto& d : r->getDoors())
					if ((d.second.perm > 0 || d.second.everOpen) && d.first && !group.count(d.first.get())) {
						group[d.first.get()] = g;
						todo.push_back(d.first);
					}
			}
			g++;
		}
		// groups a lift joins are one: its cabin carries creatures between
		// the rooms at its stops whatever the doors (the cage lift is the way
		// into the airlock's underground west side, ten rooms closed off by
		// doors)
		std::vector<int> parent(g);
		for (int k = 0; k < g; k++)
			parent[k] = k;
		std::function<int(int)> find = [&](int k) { return parent[k] == k ? k : parent[k] = find(parent[k]); };
		for (auto& ag : world.agents)
			if (Lift* lift = dynamic_cast<Lift*>(ag.get())) {
				float lx = lift->x + lift->getWidth() / 2.0f;
				int first = -1;
				for (int y : lift->callbuttony)
					if (std::shared_ptr<Room> r = world.map->roomAt(lx, y - 4); r && group.count(r.get())) {
						int k = find(group[r.get()]);
						if (first < 0)
							first = k;
						else
							parent[k] = find(first);
					}
			}
		sizes.clear();
		for (auto& [r, k] : group) {
			k = find(k);
			sizes[k]++;
		}
		groupSize = sizes;
		worldGroup = -1;
		int most = 0;
		for (auto& [k, n] : sizes)
			if (n > most)
				most = n, worldGroup = k;
		// developer aid: OPENC2E_LIST_POCKETS lists the rooms outside the world
		if (devenv("OPENC2E_LIST_POCKETS")) {
			std::string list;
			for (auto& [r, k] : group)
				if (k != worldGroup)
					list += fmt::format(" {}(group {})", r->id, k);
			fmt::print("* Room groups: {}, the world {} rooms; outside it:{}\n", g, most, list);
			// and every lift's stops: the room its cabin stands in there and
			// that room's group (a group cut off by doors may still be reached
			// by lift)
			for (auto& ag : world.agents) {
				Lift* lift = dynamic_cast<Lift*>(ag.get());
				if (!lift)
					continue;
				std::string stops;
				float lx = lift->x + lift->getWidth() / 2.0f;
				for (int y : lift->callbuttony) {
					std::shared_ptr<Room> r = world.map->roomAt(lx, y - 4);
					stops += r ? fmt::format(" {}(room {} x {}-{} group {}{})", y, r->id, r->x_left, r->x_right, group.count(r.get()) ? group[r.get()] : -1,
									 group.count(r.get()) && group[r.get()] == worldGroup ? " world" : "")
							   : fmt::format(" {}(no room)", y);
				}
				stops += fmt::format("; cabin x {:.0f}-{:.0f}", lift->x + lift->cabinleft, lift->x + lift->cabinright);
				fmt::print("* Lift {} {} {} at x {:.0f}-{:.0f}: stops{}\n", (int)lift->family, (int)lift->genus, lift->species, lift->x,
					lift->x + lift->getWidth(), stops);
			}
		}
	}
	if (std::shared_ptr<Room> foot = world.map->roomAt(cx, a->y + a->getHeight() - 2)) {
		// (not when its feet stand in a room of the world: a creature pressed
		// against the far east wall (x 8240) had its middle 2 px into the
		// sealed pocket beyond and was lifted 900 px to the surface; owner,
		// 2026-11-12)
		// (only the world: a sealed group of four rooms, the tunnel under the
		// bridge, was taken as somewhere creatures are brought on purpose,
		// and a grendel starved there; 2026-11-12. Groups a lift joins to the
		// world are part of it.)
		auto livable = [&](Room* r) { return group.count(r) && group[r] == worldGroup; };
		bool feetInWorld = false;
		// (for the messages below: where its foot was judged to stand)
		std::string footNote;
		if (auto* sc = dynamic_cast<SkeletalCreature*>(a)) {
			std::shared_ptr<Room> fr = sc->footRoom();
			footNote = fmt::format(" (foot {:.0f},{:.0f}, foot room {})", sc->footX(), sc->footY(), fr ? fmt::format("{}", fr->id) : std::string("none yet"));
			// (no foot room yet, after a load or a script's move: the room
			// under the foot itself. A creature at the foot of the tree-trunk
			// lift's shaft, its box over the rock beside it (room 103) and
			// its foot in the shaft, was moved 40 px each time; 2026-11-15)
			if (!fr) {
				fr = world.map->roomAt(sc->footX(), sc->footY() - 2);
				// (a foot a few pixels under a floor stands on that floor, as
				// placing a foot takes it: a norn just moved by a script, its foot
				// 4 px under the library's floor, was judged in the sealed rock
				// below (room 107) and moved; 2026-11-18)
				if (!fr || !livable(fr.get())) {
					std::shared_ptr<Room> above = world.map->roomAt(sc->footX(), sc->footY() - 8);
					if (above && above->type != 2 && livable(above.get())) {
						float fl = above->floorYatX(sc->footX());
						if (fl >= sc->footY() - 8 && fl <= sc->footY() + 1)
							fr = above;
					}
				}
			}
			if (fr && livable(fr.get()))
				feetInWorld = true;
		}
		// nor when its middle stands on a floor of the world, the middle on a
		// wall line judged from either side: a norn facing the library's east
		// wall (x 1473, the sealed shaft 102 beyond) with its middle on the
		// line and its foot over the shaft, its foot room cleared while it
		// hopped or was knocked, was moved 27 px each time (most of the room
		// 102 rescues; 2026-11-18)
		if (!feetInWorld) {
			float feet = a->y + a->getHeight();
			for (float off : {-4.0f, 4.0f}) {
				std::shared_ptr<Room> r = world.map->roomAt(cx + off, feet - 2);
				if (r && r->type != 2 && livable(r.get()) && std::fabs(r->floorYatX(cx + off) - feet) <= 8.0f)
					feetInWorld = true;
			}
		}
		if (feetInWorld || !group.count(foot.get()) || group[foot.get()] == worldGroup) {
			lastWorldMidX = cx;
			lastWorldTick = world.tickcount;
			haveLastWorld = true;
		}
		if (!feetInWorld && group.count(foot.get()) && group[foot.get()] != worldGroup) {
			// up onto the floor it sank through: a floor of the world just
			// above its feet (owner, 2026-11-14: creatures sinking at the gene
			// lab's east end were moved beside the pocket, into the space
			// behind the room's east wall, 130 px lower in the rock)
			auto liftUp = [&](float maxUp) -> bool {
				for (float yy = a->y + a->getHeight() - 12; yy > a->y + a->getHeight() - 12 - maxUp; yy -= 10) {
					std::shared_ptr<Room> r = world.map->roomAt(cx, yy);
					if (!r || r->type == 2 || !group.count(r.get()) || group[r.get()] != worldGroup)
						continue;
					float span = std::max(1.0f, (float)r->x_right - (float)r->x_left);
					float bottom = r->y_left_floor + ((float)r->y_right_floor - (float)r->y_left_floor) * (cx - r->x_left) / span;
					if (r->floorpoints.size())
						bottom = std::min(bottom, r->floorYatX(cx));
					float oldx = a->x, oldy = a->y;
					// wholly on that floor: lifted at the same x, a creature at the
					// east end of the dock (room 13, which ends at x 6565) still
					// overhung the open sea beyond, fell through again by its corner
					// and was lifted again, 1,228 times in 20 minutes (3RPA,
					// starving; 2026-11-12)
					float nx = ax, w = a->getWidth();
					if ((float)r->x_right - (float)r->x_left > w + 4)
						nx = std::clamp(nx, (float)r->x_left + 2, (float)r->x_right - w - 2);
					if (nx != ax) {
						cx = nx + w / 2.0f;
						bottom = r->y_left_floor + ((float)r->y_right_floor - (float)r->y_left_floor) * (cx - r->x_left) / span;
						if (r->floorpoints.size())
							bottom = std::min(bottom, r->floorYatX(cx));
					}
					a->moveTo(nx - shift, bottom - a->getHeight() - 1);
					// and its foot on that floor too: the foot is not in the box's
					// middle (a creature facing east has it about 60 px from the
					// box's left), so one set back at the east end of the dock
					// (room 13, to x 6565) had its foot past the end, over the
					// sea (room 140), fell again and was lifted again, over and
					// over (2026-11-15: 30 to 87 rescues a test run)
					if (auto* sc = dynamic_cast<SkeletalCreature*>(a)) {
						float fx = sc->footX(); // (already in the map's terms)
						float dx = 0;
						if (fx > (float)r->x_right - 4)
							dx = (float)r->x_right - 4 - fx;
						else if (fx < (float)r->x_left + 4)
							dx = (float)r->x_left + 4 - fx;
						if (dx != 0) {
							cx += dx;
							float fl = r->y_left_floor + ((float)r->y_right_floor - (float)r->y_left_floor) * (cx - r->x_left) / span;
							if (r->floorpoints.size())
								fl = std::min(fl, r->floorYatX(cx));
							a->moveTo(a->x + dx, fl - a->getHeight() - 1);
						}
					}
					a->velx = 0;
					a->vely = 0;
					if (auto* sc = dynamic_cast<SkeletalCreature*>(a))
						sc->scriptMoved(); // its feet find their room afresh, not the pocket's floor
					fmt::print("* {} (unid {}) was in a sealed pocket (room {}) at {:.0f},{:.0f}; lifted to the floor of room {} at {:.0f},{:.0f}{}\n",
						world.history->findMoniker(genome), a->getUNID(), foot->id, oldx, oldy, r->id, a->x, a->y, footNote);
					return true;
				}
				return false;
			};
			// the nearest room of the world beside it, at its feet's height, up
			// to maxdx away
			auto moveBeside = [&](float maxdx, float onlySide = 0) -> bool {
			float fy = a->y + a->getHeight() - 2;
			// (across the seam of a wrapping world too: the pocket at the far
			// east edge, room 183, lies beside the rooms at x 0)
			MetaRoom* mr = world.map->metaRoomAt(cx, fy);
			if (!mr)
				mr = world.map->wrappingMetaRoomNear(cx, fy);
			for (float dx = 4; dx <= maxdx; dx += 4)
				for (float side : {-1.0f, 1.0f}) {
					if (onlySide != 0 && side != onlySide)
						continue;
					std::shared_ptr<Room> r = mr ? mr->roomAt(cx + side * dx, fy) : world.map->roomAt(cx + side * dx, fy);
					if (!r || r->type == 2 || !livable(r.get()))
						continue;
					// (not air over open water: room 140, east of the dock's end,
					// has the sea through its floor, and a creature moved there
					// dropped to the seabed; 2026-11-12)
					if (floorOpensOntoWater(r.get(), cx + side * dx))
						continue;
					float oldx = a->x, oldy = a->y;
					a->moveTo(a->x + side * (dx + a->getWidth() / 2.0f), a->y);
					a->velx = 0;
					a->vely = 0;
					if (auto* sc = dynamic_cast<SkeletalCreature*>(a))
						sc->scriptMoved();
					fmt::print("* {} (unid {}) was in a sealed pocket (room {}) at {:.0f},{:.0f}; moved beside it into room {} at {:.0f},{:.0f}{}\n",
						world.history->findMoniker(genome), a->getUNID(), foot->id, oldx, oldy, r->id, a->x, a->y, footNote);
					return true;
				}
			return false;
			};
			// first a short step onto a floor beside, when the creature is only
			// a little past a wall (at the gene lab's west wall, x 6113, its
			// middle 8 px into the rock beyond and its feet 30 px under the
			// lab's floor, a creature was lifted 165 px onto the walkway above;
			// 2026-11-15), then up onto the floor it sank through, then further
			// beside
			// a floor of the world just beside, at or a little above its feet
			// (up to maxrise): it steps onto it, its box clear of the wall
			auto stepOnto = [&](float maxdx, float maxrise, float onlySide = 0) -> bool {
				float fy = a->y + a->getHeight() - 2, w = a->getWidth();
				for (float dx = 2; dx <= maxdx; dx += 2)
					for (float side : {-1.0f, 1.0f}) {
						if (onlySide != 0 && side != onlySide)
							continue;
						float x2 = cx + side * dx;
						for (float k = 0; k <= maxrise; k += 4) {
							std::shared_ptr<Room> r = world.map->roomAt(x2, fy - k);
							if (!r || r->type == 2 || !livable(r.get()) || floorOpensOntoWater(r.get(), x2))
								continue;
							float floor = r->floorYatX(x2);
							if (floor > fy + 4 || floor < fy - maxrise - 4)
								continue;
							float nx = x2 + side * (w / 2.0f) - w / 2.0f; // middle half a width past the wall
							std::shared_ptr<Room> at = world.map->roomAt(nx + w / 2.0f, floor - 2);
							if (at != r)
								break;
							float oldx = a->x, oldy = a->y;
							a->moveTo(nx - shift, r->floorYatX(nx + w / 2.0f) - a->getHeight() - 1);
							a->velx = 0;
							a->vely = 0;
							if (auto* sc = dynamic_cast<SkeletalCreature*>(a))
								sc->scriptMoved();
							fmt::print("* {} (unid {}) was in a sealed pocket (room {}) at {:.0f},{:.0f}; stepped onto the floor of room {} beside it at {:.0f},{:.0f}{}\n",
								world.history->findMoniker(genome), a->getUNID(), foot->id, oldx, oldy, r->id, a->x, a->y, footNote);
							return true;
						}
					}
				return false;
			};
			// first back to the side it came from, when it slid sideways into
			// the pocket (its middle last stood in the world at least 8 px to
			// that side within the last 200 ticks): a norn that walked from the
			// tree-trunk lift's shaft (room 80) into the rock between it and the
			// library (102) was stepped into the library through the wall,
			// whichever floor was nearest (2026-11-19). One that sank straight
			// down keeps the usual order.
			if (haveLastWorld && world.tickcount - lastWorldTick <= 200) {
				float back = lastWorldMidX - cx;
				if (std::fabs(back) >= 8.0f && std::fabs(back) <= 300.0f) {
					float side = back < 0 ? -1.0f : 1.0f;
					if (stepOnto(32.0f, 40.0f, side) || stepOnto(32.0f, 80.0f, side) || moveBeside(80.0f, side))
						return;
				}
			}
			if (stepOnto(32.0f, 40.0f))
				return;
			// then a floor beside a little higher: a norn turning at the
			// gene lab's west wall (x 6113) had its 50 px box swing over the
			// rock beyond (room 160, floor 100 px down), fell in it, and by
			// the next check its feet were 57 px under the lab's floor; it
			// was lifted 220 px onto the ledge above (room 158), a floor it
			// could not walk back from (2026-11-19)
			if (stepOnto(32.0f, 80.0f))
				return;
			if (liftUp(150.0f))
				return;
			if (moveBeside(80.0f))
				return;
			if (liftUp(800.0f))
				return;
		}
	}

	if (world.map->roomAt(cx, cy) || world.map->roomAt(cx, a->y + a->getHeight() - 2))
		return;
	// Stepping off a ledge: a foot reaching out over the lower floor beside
	// pulls the body down before the body is past the edge, into the rock
	// under the ledge's end (the lower room within its width at its feet's
	// height). That is a fall under way,
	// not a creature stuck in rock: it is moved the few pixels out over the
	// drop and falls the rest of the way (owner, 2026-11-13: creatures that
	// walk off a ledge should drop straight down; at the upper-east deck's
	// end, room 145 over 179, they were lifted back onto the ledge instead).
	{
		float fy = a->y + a->getHeight() - 2, w = a->getWidth();
		float bestdx = 0;
		std::shared_ptr<Room> over;
		for (float dx = 2; dx <= w + 10 && !over; dx += 2)
			for (float side : {-1.0f, 1.0f}) {
				std::shared_ptr<Room> r = world.map->roomAt(cx + side * dx, fy);
				// a dry room of the world (not a sealed pocket, like room 213
				// at the far east wall) whose floor there is not above its feet
				if (!r || r->type == 2 || !group.count(r.get()) || group[r.get()] != worldGroup)
					continue;
				if (r->floorYatX(cx + side * dx) < fy - 4)
					continue; // (its floor at or below its feet: they may be there already)
				// and open beside it (its ceiling above the creature's head):
				// not the cave under the ledge (room 157 under 145's floor)
				float span = std::max(1.0f, (float)r->x_right - (float)r->x_left);
				float ceiling = r->y_left_ceiling + ((float)r->y_right_ceiling - (float)r->y_left_ceiling) * (cx + side * dx - r->x_left) / span;
				if (ceiling > a->y + 4)
					continue;
				over = r;
				bestdx = side * dx;
				break;
			}
		if (over) {
			float oldx = a->x, oldy = a->y;
			// all of it just inside that room's edge, hanging over the drop
			float edge = bestdx > 0 ? (float)over->x_left + 2 : (float)over->x_right - 2;
			float nx = bestdx > 0 ? std::max(ax, edge) : std::min(ax, edge - w);
			a->moveTo(nx - shift, a->y);
			a->velx = 0;
			if (auto* sc = dynamic_cast<SkeletalCreature*>(a))
				sc->scriptMoved();
			fmt::print("* {} stepped off a ledge at {:.0f},{:.0f}; moved out over room {} at {:.0f} to fall\n",
				world.history->findMoniker(genome), oldx, oldy, over->id, a->x);
			return;
		}
	}
	std::shared_ptr<Room> dry, wet;
	float found = 0;
	for (float yy = cy - 10; yy > cy - 800 && !dry; yy -= 10) {
		std::shared_ptr<Room> r = world.map->roomAt(cx, yy);
		if (!r)
			continue;
		if (r->type == 2) {
			if (!wet)
				wet = r;
		} else {
			dry = r;
			found = yy;
		}
	}
	std::shared_ptr<Room> r = dry ? dry : wet;
	if (!r)
		return; // nothing above: leave it (the hand can still move it)
	(void)found;
	float span = std::max(1.0f, (float)r->x_right - (float)r->x_left);
	float bottom = r->y_left_floor + ((float)r->y_right_floor - (float)r->y_left_floor) * (cx - r->x_left) / span;
	if (r->floorpoints.size() && r->type != 2)
		bottom = std::min(bottom, r->floorYatX(cx));
	float oldx = a->x, oldy = a->y;
	// wholly on that floor, as for a sealed pocket: a body lifted onto the
	// upper-east deck (room 145, which ends at x 7745) at the same x
	// overhung its end and dropped back into the rock below, again and again
	// (2026-11-13)
	float nx = ax, w = a->getWidth();
	if ((float)r->x_right - (float)r->x_left > w + 4 && r->type != 2) {
		nx = std::clamp(nx, (float)r->x_left + 2, (float)r->x_right - w - 2);
		if (nx != ax) {
			cx = nx + w / 2.0f;
			bottom = r->y_left_floor + ((float)r->y_right_floor - (float)r->y_left_floor) * (cx - r->x_left) / span;
			if (r->floorpoints.size())
				bottom = std::min(bottom, r->floorYatX(cx));
		}
	}
	a->moveTo(nx - shift, bottom - a->getHeight() - 1);
	a->velx = 0;
	a->vely = 0;
	if (auto* sc = dynamic_cast<SkeletalCreature*>(a))
		sc->scriptMoved();
	fmt::print("* {} was inside the ground at {:.0f},{:.0f}; lifted to the floor of room {} ({}) at {:.0f},{:.0f}\n",
		world.history->findMoniker(genome), oldx, oldy, r->id, r->type == 2 ? "water" : "dry", a->x, a->y);
#endif
}

void oldCreature::watchPregnancy() {
	bool now = pregnant != 0;
	if (pregnancywatched && now && !waspregnant) {
#ifndef _CREATURE_STANDALONE
		std::string mon = world.history->findMoniker(genome);
		if (world.history->hasMoniker(mon)) {
			world.history->getMoniker(mon).addEvent(8, "", ""); // became pregnant
			world.history->logLife(lifeLogEntry::pregnancy, mon, (int)stage);
		}
#endif
	}
	if (now && parentagent && parentagent->babymoniker != 0)
		lastbaby = parentagent->babymoniker;
	// the pregnancy over: she has laid her egg. The status bar announces it
	// and goes to the egg (owner, 2026-11-11: a laid egg was lost). The egg
	// carries the baby's genome name (OBV0, from BABY, which the world's
	// laying script clears just before it makes the egg)
	if (pregnancywatched && !now && waspregnant && isAlive()) {
		std::string mon = world.history->findMoniker(genome);
		world.history->tell(mon, storyEntry::laidEgg);
#ifndef _CREATURE_STANDALONE
		std::string child = lastbaby ? std::string((const char*)&lastbaby, 4) : "";
		if (world.history->hasMoniker(mon)) {
			size_t before = world.history->notices.size();
			world.history->logLife(lifeLogEntry::eggLaid, mon, (int)stage);
			if (world.history->notices.size() > before) {
				world.history->notices.back().child = child;
				world.history->lifelog.back().child = child;
			}
		}
#endif
		lastbaby = 0;
	}
	waspregnant = now;
	pregnancywatched = true;
}

// Death and the rescue window (owner, 2026-11-11). A body out of energy
// (ATP, chemical 99, at 4 or less) for 300 ticks falls unconscious: it lies
// down (C2's own faint, creature event 70, run again as it ends) and its
// mind stops. While it is unconscious its organs go on failing at the rate
// its genome gives (the organs' damage at zero energy), so it can be saved
// only by mending the cause: out of the water, fed (food the hand drops on
// it), a Health Kit injection. Once ATP stays at 40 or more for 100 ticks
// it comes round. If an organ's lasting health reaches nothing first, it
// dies of it: "drowning" if it was under water and out of breath,
// "starvation" if hungry, else "organ failure".
// UNCS 1/0 from a script: faint now (as when out of energy: lying down, the
// mind still; it comes round once its energy has recovered), or come round
void c2Creature::setUnconscious(bool u) {
#ifndef _CREATURE_STANDALONE
	if (!alive || !parentagent || u == unconscious)
		return;
	std::string mon = world.history->findMoniker(genome);
	unconscious = u;
	if (u) {
		recoveringTicks = 0;
		asleep = false;
		parentagent->queueScript(70);
		world.history->logLife(lifeLogEntry::unconscious, mon, (int)stage);
		world.history->tell(mon, storyEntry::unconscious);
		fmt::print("* {} fell unconscious (a script, UNCS 1)\n", mon);
	} else {
		noEnergyTicks = 0;
		world.history->logLife(lifeLogEntry::cameRound, mon, (int)stage);
		world.history->tell(mon, storyEntry::cameRound);
		fmt::print("* {} came round (a script, UNCS 0)\n", mon);
	}
#else
	unconscious = u;
#endif
}

void c2Creature::updateConsciousness() {
#ifndef _CREATURE_STANDALONE
	if (!alive || !parentagent)
		return;
	// death: an organ has failed
	for (auto& o : organs)
		if (o->getInitialLifeforce() > 0.0f && o->getLongTermLifeforce() <= 0.5f) {
			if (breathHeld >= breathTicks)
				diedof = "drowning";
			else if (drives[2] >= 216) // hunger (C2 drive 2) at 0.85 or more
				diedof = "starvation";
			// ill (Disease.h) and not starving: what it was ill with
			else if (Disease::isIll(world.history->findMoniker(genome)) && !(chemicals[72] < 10 && chemicals[74] < 10 && chemicals[75] < 10))
				diedof = "illness: " + Disease::illnessWords(world.history->findMoniker(genome));
			// or its fuel stores spent (glycogen 72, fat 74, adipose tissue
			// 75): once ATP is gone its chemistry stands still, Hunger with
			// it, so a creature that starved can lie unconscious with the
			// Hunger it had when it fainted, and a little glucose it could
			// no longer burn (2026-11-21: three Home hatchlings recorded as
			// "organ failure" at Hunger ~100, glycogen and fat at 0)
			else if (chemicals[72] < 10 && chemicals[74] < 10 && chemicals[75] < 10)
				diedof = "starvation";
			else
				diedof = "organ failure";
			// where it died (its middle and room), to map where drownings
			// and starvation happen (owner, 2026-11-14)
			{
				float cx = parentagent->x + parentagent->getWidth() / 2.0f;
				float cy = parentagent->y + parentagent->getHeight() / 2.0f;
				auto room = world.map ? world.map->roomAt(cx, cy) : nullptr;
				fmt::print("* {} died of {} (an organ failed) at {:.0f},{:.0f} (room {})\n", world.history->findMoniker(genome), diedof, cx, cy, room ? (int)room->id : -1);
			}
			unconscious = false;
			die();
			return;
		}
	unsigned char atp = chemicals[99];
	std::string mon = world.history->findMoniker(genome);
	if (!unconscious) {
		noEnergyTicks = atp <= 4 ? noEnergyTicks + 1 : 0;
		if (noEnergyTicks >= 300) {
			unconscious = true;
			recoveringTicks = 0;
			asleep = false;
			parentagent->queueScript(70); // C2's faint: lying down, groaning
			world.history->logLife(lifeLogEntry::unconscious, mon, (int)stage);
			world.history->tell(mon, storyEntry::unconscious);
			fmt::print("* {} fell unconscious (out of energy)\n", mon);
		}
		return;
	}
	recoveringTicks = atp >= 40 ? recoveringTicks + 1 : 0;
	if (devenv("OPENC2E_WATCH_CREATURES") && ticks % 20 == 0)
		fmt::print("* WATCH UNCONSCIOUS {} atp {} recovering {} vm {}\n", mon, (int)atp, recoveringTicks, parentagent->vmStopped() ? "idle" : "running");
	if (recoveringTicks >= 100) {
		unconscious = false;
		noEnergyTicks = 0;
		world.history->logLife(lifeLogEntry::cameRound, mon, (int)stage);
		world.history->tell(mon, storyEntry::cameRound);
		fmt::print("* {} came round\n", mon);
		return;
	}
	// keep lying down: the faint again whenever it ends
	if (parentagent->vmStopped() && !parentagent->carriedby)
		parentagent->queueScript(70);
#endif
}

// The Science Kit's Syringe has two mixtures for the dead, as the game's own
// text has it: the Metabolism Transplant, "first step in kickstarting a dead
// creature", and the Defibrillant, "second step ... repeat every 10 secs". A
// dead body's chemistry stands still, so their chemicals alone did nothing
// (owner, 2026-11-22). Our own rule: for as long as the body lies where it
// died (30 s, until C2's death script lifts it away), a mixture rich in Energy
// fills its stores, and after that one rich in ATP brings it back: out cold,
// with the organs that failed at a quarter of their strength, to come round
// once its ATP holds (updateConsciousness), or to die again if what killed
// it has not been put right. Old age cannot be undone.
c2Creature::Kickstart c2Creature::kickstartState() {
	if (alive)
		return notDead;
#ifndef _CREATURE_STANDALONE
	if (!parentagent || parentagent->isDying() || diedofoldage || engine.version != 2 || !world.history)
		return tooLate;
	std::string mon = world.history->findMoniker(genome);
	if (mon.empty() || !world.history->hasMoniker(mon))
		return tooLate;
	const monikerData& m = world.history->getMoniker(mon);
	if (!m.died || world.worldtickcount < m.diedtick || world.worldtickcount - m.diedtick > 290)
		return tooLate;
	return kickstartPrimed ? primed : stillDead;
#else
	return tooLate;
#endif
}

c2Creature::Kickstart c2Creature::kickstart(int energy, int atp) {
	Kickstart s = kickstartState();
	if (s != stillDead && s != primed)
		return s;
#ifndef _CREATURE_STANDALONE
	const int plenty = 200;
	if (energy >= plenty)
		kickstartPrimed = true;
	if (!kickstartPrimed)
		return stillDead;
	if (atp < plenty)
		return primed;
	// back, out cold
	kickstartPrimed = false;
	alive = true;
	minddied = false;
	dead = 0;
	deadTicks = 0;
	diedof.clear();
	breathHeld = 0;
	noEnergyTicks = 0;
	recoveringTicks = 0;
	asleep = false;
	unconscious = true;
	for (auto& o : organs) {
		const float quarter = o->getInitialLifeforce() * 0.25f;
		if (o->getLongTermLifeforce() < quarter)
			o->restoreLifeforce(quarter, quarter);
	}
	parentagent->stopScript(); // (the death script, waiting to lift it away)
	parentagent->queueScript(70); // lying, groaning
	std::string mon = world.history->findMoniker(genome);
	world.history->getMoniker(mon).cameBack();
	// (its death stays in the life log; its notice on the status bar goes)
	for (auto* list : {&world.history->notices, &world.history->shownNotices})
		list->erase(std::remove_if(list->begin(), list->end(), [&](const lifeLogEntry& e) { return e.kind == lifeLogEntry::death && e.moniker == mon; }), list->end());
	world.history->logLife(lifeLogEntry::cameRound, mon, (int)stage);
	world.history->lifelog.back().cause = "revived";
	world.history->notices.back().cause = "revived";
	world.history->tell(mon, storyEntry::cameRound, "", "revived");
	fmt::print("* {} brought back to life (the Syringe)\n", mon);
#endif
	return revived;
}

bool c2Creature::breathesWater() {
	if (waterbreather < 0) {
		waterbreather = 0;
		// C2's bodies get their oxygen (chemical 95) from air (96), which an
		// emitter makes from the air sense (creature, sensorimotor, locus 9,
		// which the engine empties under water). A body whose air or oxygen
		// does not depend on that sense breathes water: some emitter makes
		// air or oxygen from anything else (the Frog Norn pack moved its air
		// emitter to the always-on sense, locus 0), or nothing reads the air
		// sense at all. Read from the genome as it is, so old genomes need no
		// changes (owner, 2026-11-11).
		if (engine.version == 2 && genome) {
			bool fromAirSense = false, fromElsewhere = false, airSenseRead = false;
			for (auto& g : genome->genes) {
				auto* e = dynamic_cast<bioEmitterGene*>(g.get());
				if (!e)
					continue;
				bool airSense = e->organ == 1 && e->tissue == 4 && e->locus == 9;
				airSenseRead = airSenseRead || airSense;
				if (e->chemical == 95 || e->chemical == 96)
					(airSense ? fromAirSense : fromElsewhere) = true;
			}
			waterbreather = (fromElsewhere || !airSenseRead) ? 1 : 0;
			(void)fromAirSense;
			if (waterbreather)
				fmt::print("* {} breathes water as well as air (its genome's air does not come from the air sense)\n", world.history->findMoniker(genome));
		}
	}
	return waterbreather == 1;
}

void oldCreature::addGene(gene* g) {
	Creature::addGene(g);
	if (typeid(*g) == typeid(bioInitialConcentrationGene)) {
		// initialise chemical levels
		bioInitialConcentrationGene* b = (bioInitialConcentrationGene*)(g);
		chemicals[b->chemical] = b->quantity;
	} else if (typeid(*g) == typeid(bioHalfLivesGene)) {
		bioHalfLivesGene* d = dynamic_cast<bioHalfLivesGene*>(g);
		assert(d);
		halflives = d;
	}
}

void c1Creature::addGene(gene* g) {
	oldCreature::addGene(g);

	if (typeid(*g) == typeid(bioReactionGene)) {
		reactions.push_back(std::shared_ptr<c1Reaction>(new c1Reaction((bioReactionGene*)g)));
	} else if (typeid(*g) == typeid(bioEmitterGene)) {
		emitters.push_back(c1Emitter((bioEmitterGene*)g, this));
	} else if (typeid(*g) == typeid(bioReceptorGene)) {
		receptors.push_back(c1Receptor((bioReceptorGene*)g, this));
	}
}

void c2Creature::addGene(gene* g) {
	oldCreature::addGene(g);

	if (typeid(*g) == typeid(organGene)) {
		// create organ
		organGene* o = dynamic_cast<organGene*>(g);
		assert(o);
		if (!o->isBrain()) { // TODO: handle brain organ
			organs.push_back(std::shared_ptr<c2Organ>(new c2Organ(this, o)));
		}
	} else if (typeid(*g) == typeid(bioReactionGene) || typeid(*g) == typeid(bioEmitterGene) || typeid(*g) == typeid(bioReceptorGene)) {
		std::shared_ptr<c2Organ> organ = addingorgan ? addingorgan : (organs.empty() ? nullptr : organs.back());
		THROW_IFNOT(organ);
		organ->genes.push_back(g);
	}
}

void c2eCreature::addGene(gene* g) {
	Creature::addGene(g);

	if (typeid(*g) == typeid(bioInitialConcentrationGene)) {
		// initialise chemical levels
		bioInitialConcentrationGene* b = (bioInitialConcentrationGene*)(g);
		chemicals[b->chemical] = b->quantity / 255.0f; // TODO: correctness unchecked
	} else if (typeid(*g) == typeid(organGene)) {
		// create organ
		organGene* o = dynamic_cast<organGene*>(g);
		assert(o);
		if (!o->isBrain()) { // TODO: handle brain organ
			organs.push_back(std::shared_ptr<c2eOrgan>(new c2eOrgan(this, o)));
		}
	} else if (typeid(*g) == typeid(bioReactionGene)) {
		THROW_IFNOT(organs.size() > 0);
		organs.back()->genes.push_back(g);
	} else if (typeid(*g) == typeid(bioEmitterGene)) {
		THROW_IFNOT(organs.size() > 0);
		organs.back()->genes.push_back(g);
	} else if (typeid(*g) == typeid(bioReceptorGene)) {
		THROW_IFNOT(organs.size() > 0);
		organs.back()->genes.push_back(g);
	} else if (typeid(*g) == typeid(bioHalfLivesGene)) {
		bioHalfLivesGene* d = dynamic_cast<bioHalfLivesGene*>(g);
		assert(d);
		halflives = d;
	}
}

void c2eCreature::adjustDrive(unsigned int id, float value) {
	assert(id < 20);
	drives[id] += value;

	if (drives[id] < 0.0f)
		drives[id] = 0.0f;
	else if (drives[id] > 1.0f)
		drives[id] = 1.0f;
}

/* vim: set noet: */
