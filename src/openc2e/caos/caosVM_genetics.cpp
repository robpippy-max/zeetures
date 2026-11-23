/*
 *  caosVM_genetics.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Fri Dec 9 2005.
 *  Copyright (c) 2005 Alyssa Milburn. All rights reserved.
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

#include "Engine.h"
#include "World.h"
#include "caosVM.h"
#include "common/throw_ifnot.h"
#include "creatures/Breeding.h"
#include "creatures/CreatureAgent.h"
#include "fileformats/genomeFile.h"
#include "historyManager.h"

#include <cassert>
#include <fmt/core.h>
#include <memory>

/**
 GENE CLON (command) dest_agent (agent) dest_slot (integer) src_agent (agent) src_slot (integer)
 %status stub
 
 Clone a genome. A new moniker is created.
*/
void c_GENE_CLON(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(src_slot)
	VM_PARAM_VALIDAGENT(src_agent)
	VM_PARAM_INTEGER_UNUSED(dest_slot)
	VM_PARAM_VALIDAGENT(dest_agent)

	// TODO
}

/**
 GENE CROS (command) dest_agent (agent) dest_slot (integer) mum_agent (agent) mum_slot (integer) dad_agent (agent) dad_slot (integer) mum_mutation_chance (integer) mum_mutation_degree (integer) dad_mutation_chance (integer) dad_mutation_degree (integer)
 %status stub

 Cross two genomes, creating a new one.
*/
void c_GENE_CROS(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(dad_mutation_degree)
	VM_PARAM_INTEGER_UNUSED(dad_mutation_chance)
	VM_PARAM_INTEGER_UNUSED(mum_mutation_degree)
	VM_PARAM_INTEGER_UNUSED(mum_mutation_chance)
	VM_PARAM_INTEGER_UNUSED(dad_slot)
	VM_PARAM_VALIDAGENT(dad_agent)
	VM_PARAM_INTEGER_UNUSED(mum_slot)
	VM_PARAM_VALIDAGENT(mum_agent)
	VM_PARAM_INTEGER_UNUSED(dest_slot)
	VM_PARAM_VALIDAGENT(dest_agent)

	// TODO
}

/**
 GENE KILL (command) agent (agent) slot (integer)
 %status stub

 Delete a genome from a slot.
*/
void c_GENE_KILL(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(slot)
	VM_PARAM_VALIDAGENT(agent)

	// TODO
}

/**
 GENE LOAD (command) agent (agent) slot (integer) genefile (string)
 %status maybe

 Load a genome file into a slot. You can use * and ? wildcards in the filename.
*/
void c_GENE_LOAD(caosVM* vm) {
	VM_PARAM_STRING(genefile)
	VM_PARAM_INTEGER(slot)
	VM_PARAM_VALIDAGENT(agent)

	std::shared_ptr<genomeFile> p = world.loadGenome(genefile);
	if (!p)
		throw Exception("failed to find genome file '" + genefile + "'");

	THROW_IFNOT(p->getVersion() == 3);

	agent->genome_slots[slot] = p;
	world.newMoniker(p, genefile, agent);
}

/**
 GENE MOVE (command) dest_agent (agent) dest_slot (integer) src_agent (agent) src_slot (integer)
 %status maybe

 Move a genome to another slot.
*/
void c_GENE_MOVE(caosVM* vm) {
	VM_PARAM_INTEGER(src_slot)
	VM_PARAM_VALIDAGENT(src_agent)
	VM_PARAM_INTEGER(dest_slot)
	VM_PARAM_VALIDAGENT(dest_agent)

	std::map<unsigned int, std::shared_ptr<class genomeFile> >::iterator i = src_agent->genome_slots.find(src_slot);
	THROW_IFNOT(i != src_agent->genome_slots.end());

	std::string moniker = world.history->findMoniker(i->second);
	assert(moniker != std::string("")); // internal consistency, i think..

	dest_agent->genome_slots[dest_slot] = src_agent->genome_slots[src_slot];
	src_agent->genome_slots.erase(i);
	world.history->getMoniker(moniker).moveToAgent(dest_agent);
}

/**
 GTOS (string) slot (integer)
 %status maybe
 
 Return the moniker stored in the given gene slot of the target agent.
*/
void v_GTOS(caosVM* vm) {
	VM_PARAM_INTEGER(slot)

	valid_agent(vm->targ);
	if (vm->targ->genome_slots.find(slot) == vm->targ->genome_slots.end()) {
		vm->result.setString(""); // CV needs this, at least
	} else {
		std::shared_ptr<class genomeFile> g = vm->targ->genome_slots[slot];
		vm->result.setString(world.history->findMoniker(g));
	}
}

/**
 MTOA (agent) moniker (string)
 %status maybe

 Return the agent which has the given moniker stored in a gene slot, or NULL if none.
*/
void v_MTOA(caosVM* vm) {
	VM_PARAM_STRING(moniker)

	THROW_IFNOT(world.history->hasMoniker(moniker));
	vm->result.setAgent(world.history->getMoniker(moniker).owner);
}

/**
 MTOC (agent) moniker (string)
 %status maybe

 Return the live creature with the given moniker, or NULL if none.
*/
void v_MTOC(caosVM* vm) {
	VM_PARAM_STRING(moniker)

	vm->result.setAgent(0);
	if (!world.history->hasMoniker(moniker))
		return;
	Agent* a = world.history->getMoniker(moniker).owner;
	if (!a)
		return;
	CreatureAgent* c = dynamic_cast<CreatureAgent*>(a);
	THROW_IFNOT(c); // TODO: is this assert valid? can history events have non-creature owners?
	vm->result.setAgent(a);
}

// a genome by moniker: a living creature's own, one just taken out of the
// world, or a file in Genetics
static std::shared_ptr<genomeFile> genomeByName(const std::string& name) {
	if (world.history->hasMoniker(name))
		if (auto g = world.history->getMoniker(name).genome.lock())
			return g;
	if (auto g = world.history->goneGenome(name))
		return g;
	return world.loadGenome(name);
}

/**
 NEW: GENE (command) mum (integer) dad (integer) destination (variable)
 %status maybe
 %variants c1 c2

 Makes a new genome from two others, named by moniker as integers (MONK, an
 egg's genome), the way a conception does (crossover and mutation), stores it
 under a new name and sets destination to that name. mum and dad may be the
 same: a copy of one genome, with the usual small mutations (the
 Metamorphosis device clones a creature this way); either may be 0, also a
 copy of the other (the gene splicer with one pod filled). A creature's
 genome is found by its moniker for a while after the creature is gone.
*/
void c_NEW_GENE(caosVM* vm) {
	VM_PARAM_VARIABLE(destination)
	VM_PARAM_INTEGER(dad)
	VM_PARAM_INTEGER(mum)

	// one of them 0: a copy of the other (C2's gene splicer with a creature
	// in one pod only asks for this)
	THROW_IFNOT(mum != 0 || dad != 0);
	if (mum == 0)
		mum = dad;
	if (dad == 0)
		dad = mum;
	std::string mumName = genomeNameFromInt(mum), dadName = genomeNameFromInt(dad);
	// the Boney Grendels pack's genome, when the player does not have it
	// (its download failed, or packs are off): the game's own grendel. The
	// grendel eggs ask for one or the other at random (grendels.cos), and
	// those that asked for "bone" stopped mid-hatch (a player, 2026-11-22).
	if ((mumName == "bone" || dadName == "bone") && !genomeByName("bone")) {
		fmt::print("* NEW: GENE: no Boney Grendels pack here, a standard grendel instead\n");
		for (auto* name : {&mumName, &dadName})
			if (*name == "bone")
				*name = "gren";
	}
	std::shared_ptr<genomeFile> m = genomeByName(mumName), d = genomeByName(dadName);
	if (!m)
		throw Exception("NEW: GENE: failed to find genome '" + mumName + "'");
	if (!d)
		throw Exception("NEW: GENE: failed to find genome '" + dadName + "'");
	THROW_IFNOT(m->getVersion() == engine.version && d->getVersion() == engine.version);

	CrossReport rep;
	std::shared_ptr<genomeFile> child = crossGenomes(*m, *d, mumName, dadName, &rep);
	std::string name = storeGenome(*child);
	if (name.empty())
		throw Exception("NEW: GENE: couldn't store the new genome");
	fmt::print("* NEW: GENE {} and {}: genome {} ({} genes, {} mutations)\n", mumName, dadName, name, rep.genes, rep.mutations);
	if (mumName != dadName && world.history->hasMoniker(mumName) && world.history->hasMoniker(dadName))
		world.history->splicedGenomes.insert(name);
	destination->setInt(genomeNameToInt(name));
}

/* vim: set noet: */
