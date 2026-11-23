/*
 *  historyManager.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Wed 26 Apr 2006.
 *  Copyright (c) 2006 Alyssa Milburn. All rights reserved.
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

#include "historyManager.h"

#include <cstdint>
#include <cstdlib>

#include "Catalogue.h"
#include "Engine.h" // version
#include "World.h"
#include "Map.h"
#include "Room.h"
#include "Agent.h"
#include "caosValue.h"
#include "common/Exception.h"
#include "common/Random.h"
#include "common/throw_ifnot.h"
#include "creatures/Creature.h"
#include "creatures/CreatureAgent.h"
#include "fileformats/genomeFile.h"

#include <cassert>
#include <fmt/core.h>
#include <memory>

historyevent::historyevent(unsigned int eno, CreatureAgent* c) {
	timestamp = time(NULL);
	eventno = eno;
	worldtick = world.tickcount;
	// TODO: worldname = world.name;
	// TODO: worldmoniker = world.moniker;
	// TODO: networkid = world.username;

	if (c) {
		tage = c->getCreature()->getAge();
		stage = c->getCreature()->getStage();
	} else {
		tage = -1;
		stage = baby; // TODO: correct?
	}
}

void monikerData::init(std::string m, std::shared_ptr<genomeFile> f) {
	moniker = m;
	status = unreferenced;
	warpveteran = false;
	variant = -1;
	gender = -1;
	no_crossover_points = 0;
	no_point_mutations = 0;

	assert(f);
	genome = f;

	for (auto& gene : f->genes) {
		if (creatureGenusGene* g = dynamic_cast<creatureGenusGene*>(gene.get())) {
			// initialize genus
			genus = g->genus + 1;
			break;
		}
	}
}

historyevent& monikerData::addEvent(unsigned int event, std::string moniker1, std::string moniker2) {
	CreatureAgent* c = 0;
	if (owner)
		c = dynamic_cast<CreatureAgent*>(owner.get());

	events.push_back(historyevent(event, c));
	events.back().monikers[0] = moniker1;
	events.back().monikers[1] = moniker2;
	if (event == 0 || event == 1) { // conceived, spliced: its parents
		historyManager::lineageGeneration++;
		mother = moniker1;
		father = moniker2;
	}

	for (auto& agent : world.agents) {
		if (!agent)
			continue;

		agent->queueScript(127, 0, moniker, (int)(events.size() - 1)); // new life event
	}

	return events.back();
}

void monikerData::moveToAgent(AgentRef a) {
	assert(status == referenced || status == unreferenced);

	owner = a;
	if (!owner) {
		status = unreferenced;
	} else {
		status = referenced;
	}
}

void monikerData::moveToCreature(AgentRef a) {
	moveToAgent(a);
	if (borntick == 0) { // a new creature: born now (a restored one gets its saved records after)
		borntick = world.worldtickcount;
		bornknown = true;
	}

	CreatureAgent* c = dynamic_cast<CreatureAgent*>(owner.get());
	THROW_IFNOT(c);
	status = creature;
}

void monikerData::transferCreature(AgentRef a) {
	THROW_IFNOT(status == creature || status == borncreature);
	THROW_IFNOT(dynamic_cast<CreatureAgent*>(a.get()));
	owner = a; // status (and history) unchanged
}

void monikerData::wasBorn() {
	THROW_IFNOT(status == creature);
	CreatureAgent* c = dynamic_cast<CreatureAgent*>(owner.get());
	THROW_IFNOT(c);

	status = borncreature;
	gender = (c->getCreature()->isFemale() ? 2 : 1);
	variant = c->getCreature()->getVariant();
}

void monikerData::cameBack() {
	status = borncreature;
	died = false;
	diedtick = 0;
	diedstage = -1;
}

void monikerData::hasDied() {
	CreatureAgent* c = dynamic_cast<CreatureAgent*>(owner.get());
	THROW_IFNOT(c);

	// TODO

	status = dead;
}

monikerstatus monikerData::getStatus() {
	switch (status) {
		case referenced:
		case exported:
		case unreferenced:
			return status;

		case creature:
		case borncreature:
			if (owner)
				return status;
			else
				return deadandkilled; // we missed it?! TODO: what's correct behaviour here?

		case dead:
			if (owner)
				return dead;
			else
				return deadandkilled;

		default:
			throw Exception("monikerData::getStatus should not be here");
	}
}

std::string historyManager::newMoniker(std::shared_ptr<genomeFile> genome) {
	unsigned int genus = 0;

	for (auto& gene : genome->genes) {
		if (creatureGenusGene* g = dynamic_cast<creatureGenusGene*>(gene.get())) {
			// initialize genus
			genus = g->genus + 1;
			break;
		}
	}

	std::string basename = "xxxx";

	if (engine.version > 2) {
		const std::vector<std::string>* extensions = 0;
		std::string tagname = fmt::format("Moniker Friendly Names {}", (int)genus);

		if (catalogue.hasTag(tagname)) {
			extensions = &catalogue.getTag(tagname);
		} else if (catalogue.hasTag("Moniker Friendly Names")) {
			extensions = &catalogue.getTag("Moniker Friendly Names");
		} else {
			fmt::print(
				"Warning: No \"Moniker Friendly Names\" in catalogue for genus {}, defaulting to 'xxxx' for a moniker friendly name.\n",
				genus);
		}

		if (extensions) {
			basename = rand_choice(*extensions);
		}
	}

	std::string newmoniker = world.generateMoniker(basename);
	unsigned int i = 0;
	while (hasMoniker(newmoniker)) { // last-moment sanity check..
		newmoniker = world.generateMoniker(basename);
		i++;
		if (i > 100) // emergency "don't go into an infinite loop handling"
			throw Exception("Couldn't generate a moniker we don't already have!");
	}

	if (engine.version > 2)
		newmoniker = "001-" + newmoniker; // TODO: bad hack we should use the generation number here!

	genomeMonikers.clear();
	lineageGeneration++;
	monikers[newmoniker].init(newmoniker, genome);
	return newmoniker;
}

unsigned int historyManager::lineageGeneration = 0;

bool historyManager::hasMoniker(const std::string& s) {
	std::map<std::string, monikerData>::iterator i = monikers.find(s);
	if (i == monikers.end())
		return false;
	else
		return true;
}

monikerData& historyManager::getMoniker(const std::string& s) {
	std::map<std::string, monikerData>::iterator i = monikers.find(s);
	if (i == monikers.end())
		throw Exception("getMoniker was called with a non-existant moniker");
	return i->second;
}

std::string historyManager::findMoniker(std::shared_ptr<genomeFile> g) {
	if (g) {
		auto c = genomeMonikers.find(g.get());
		if (c != genomeMonikers.end()) {
			auto m = monikers.find(c->second);
			if (m != monikers.end() && m->second.genome.lock() == g)
				return c->second;
			genomeMonikers.erase(c);
		}
	}
	for (auto& moniker : monikers) {
		if (moniker.second.genome.lock() == g) {
			if (g)
				genomeMonikers[g.get()] = moniker.first;
			return moniker.first;
		}
	}

	return "";
}

std::string historyManager::findMoniker(AgentRef a) {
	for (auto& moniker : monikers) {
		if (moniker.second.owner == a)
			return moniker.first;
	}

	return "";
}

void historyManager::delMoniker(std::string s) {
	std::map<std::string, monikerData>::iterator i = monikers.find(s);
	if (i == monikers.end())
		throw Exception("getMoniker was called with a non-existant moniker");
	genomeMonikers.clear();
	lineageGeneration++;
	monikers.erase(i);
}

/* vim: set noet: */

void historyManager::tell(const std::string& mon, int kind, const std::string& other, const std::string& detail) {
	if (quiet || mon.empty() || !hasMoniker(mon))
		return;
	storyEntry e;
	e.tick = world.worldtickcount;
	e.kind = kind;
	e.other = other;
	e.detail = detail;
	getMoniker(mon).story.push_back(e);
}

void historyManager::logLife(int kind, const std::string& mon, int stage, bool oldage) {
	if (quiet || !hasMoniker(mon))
		return;
	monikerData& m = getMoniker(mon);
	lifeLogEntry e;
	e.kind = kind;
	e.tick = world.worldtickcount;
	e.moniker = mon;
	e.name = m.name;
	e.genus = m.genus ? m.genus : 1;
	e.female = m.gender == 2;
	if (CreatureAgent* c = dynamic_cast<CreatureAgent*>(m.owner.get()))
		e.female = c->getCreature()->isFemale();
	e.stage = stage;
	e.age = world.worldtickcount >= m.borntick ? world.worldtickcount - m.borntick : 0;
	e.oldage = oldage;
	e.ageknown = m.bornknown;
	if (kind == lifeLogEntry::death)
		if (Agent* a = m.owner.get()) {
			float cx = a->x + a->getWidth() / 2.0f, cy = a->y + a->getHeight() / 2.0f;
			e.x = (int)cx;
			e.y = (int)cy;
			auto r = world.map ? world.map->roomAt(cx, cy) : nullptr;
			e.room = r ? (int)r->id : -1;
		}
	lifelog.push_back(e);
	notices.push_back(e);
}

// The four who grew old together (owner, 2026-11-15): four norns that hatched
// within seconds of each other died of old age at the same moment, their
// genomes' lifespan run out together. The memorial stone (Additions,
// memorial.cos) keeps that story; when it happens again in a world, the
// group is announced and its names are added to the stone's words.
void historyManager::checkTogether(const lifeLogEntry& d) {
	// within 30 s of each other, at C2's ten ticks a second: "hatched within
	// a few seconds of each other", and so ending as close together
	const int64_t span = 300;
	if (d.kind != lifeLogEntry::death || !d.oldage || !d.ageknown || d.age > d.tick)
		return;
	int64_t born = (int64_t)d.tick - d.age;
	// a group already announced (a fifth one dying with it adds nothing)
	for (auto& e : lifelog)
		if (e.kind == lifeLogEntry::together && std::llabs((int64_t)d.tick - (int64_t)e.tick) <= 2 * span)
			return;
	std::vector<const lifeLogEntry*> group;
	for (auto& e : lifelog) {
		if (e.kind != lifeLogEntry::death || !e.oldage || !e.ageknown || e.age > e.tick)
			continue;
		if (std::llabs((int64_t)d.tick - (int64_t)e.tick) > span)
			continue;
		if (std::llabs(born - ((int64_t)e.tick - e.age)) > span)
			continue;
		group.push_back(&e);
	}
	if (group.size() < 4)
		return;
	std::string names;
	for (size_t i = 0; i < group.size(); i++) {
		const lifeLogEntry& e = *group[i];
		std::string n = hasMoniker(e.moniker) && !getMoniker(e.moniker).name.empty() ? getMoniker(e.moniker).name : !e.name.empty() ? e.name : e.moniker;
		names += (i == 0 ? "" : i + 1 == group.size() ? " and " : ", ") + n;
	}
	lifeLogEntry t;
	t.kind = lifeLogEntry::together;
	t.tick = d.tick;
	t.moniker = d.moniker;
	t.genus = d.genus;
	t.female = d.female;
	t.stage = (int)group.size();
	t.age = d.age;
	t.oldage = true;
	t.cause = names;
	lifelog.push_back(t);
	notices.push_back(t);
}

std::string historyManager::latestTogether(unsigned int* tick) const {
	for (auto it = lifelog.rbegin(); it != lifelog.rend(); ++it)
		if (it->kind == lifeLogEntry::together) {
			if (tick)
				*tick = it->tick;
			return it->cause;
		}
	return {};
}

void monikerData::initRecord(std::string m, unsigned char g, int gen) {
	moniker = m;
	status = dead;
	warpveteran = false;
	variant = -1;
	gender = gen;
	genus = g;
	no_crossover_points = 0;
	no_point_mutations = 0;
}

static const char* defaultNameWord(int genus) {
	switch (genus) {
		case 2: return "Grendel";
		case 3: return "Ettin";
		default: return "Norn";
	}
}

std::string historyManager::nextDefaultName(int genus) {
	unsigned int n = ++namecount[genus];
	return fmt::format("{} {:04d}", defaultNameWord(genus), n);
}

bool historyManager::renameMoniker(const std::string& from, const std::string& to) {
	if (from == to || to.empty() || hasMoniker(to) || !hasMoniker(from))
		return false;
	genomeMonikers.clear();
	lineageGeneration++;
	auto node = monikers.extract(from); // the records themselves stay where they are
	node.key() = to;
	node.mapped().moniker = to;
	monikers.insert(std::move(node));
	return true;
}

monikerData& historyManager::addRecord(const std::string& mon, unsigned char genus, int gender) {
	genomeMonikers.clear();
	lineageGeneration++;
	monikerData& m = monikers[mon];
	m.initRecord(mon, genus, gender);
	return m;
}
