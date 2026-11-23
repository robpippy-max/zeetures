/*
 *  caosVM_creatures.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Tue Jun 01 2004.
 *  Copyright (c) 2004 Alyssa Milburn. All rights reserved.
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
#include "common/devenv.h"
#include "MetaRoom.h"
#include "Map.h"
#include "AgentHelpers.h"
#include "World.h"
#include "BirthPortrait.h"
#include "WorldSave.h"
#include "caosVM.h"
#include "common/Random.h"
#include "common/throw_ifnot.h"
#include "creatures/Disease.h"
#include "creatures/Exposure.h"
#include "creatures/Breeding.h"
#include "creatures/CompoundCreature.h"
#include "creatures/SkeletalCreature.h"
#include "creatures/c2eCreature.h"
#include "creatures/CreatureSpeech.h"
#include "creatures/oldCreature.h"
#include "creatures/Temperament.h"
#include "historyManager.h"

#include <cassert>
#include <cmath>
#include <memory>

CreatureAgent* caosVM::getTargCreatureAgent() {
	valid_agent(targ);
	CreatureAgent* c = dynamic_cast<CreatureAgent*>(targ.get());
	THROW_IFNOT(c);
	return c;
}

Creature* caosVM::getTargCreature() {
	return getTargCreatureAgent()->getCreature();
}

oldCreature* getoldCreature(Agent* a) {
	if (!a)
		return 0;
	CreatureAgent* b = dynamic_cast<CreatureAgent*>(a);
	if (!b)
		return 0;
	oldCreature* c = dynamic_cast<oldCreature*>(b->getCreature());
	return c;
}

// Delivers a C1/C2 stimulus from OWNR to every creature in range.
// SIGN: creatures that can see OWNR. TACT: creatures touching OWNR.
// SHOU: creatures in the same metaroom within 500 pixels, an engine
// approximation of "can hear" (openc2e has no hearing model yet).
enum class OldStimRange { Shout, Sign, Tact };
static void broadcastOldStimulus(caosVM* vm, StimulusEvent s, OldStimRange range, const char* command) {
	std::shared_ptr<Agent> src = vm->owner.lock();
	if (!src)
		return;
	s.source = vm->owner;
	s.command = command;
	float sx = src->x + src->getWidth() / 2.0f;
	float sy = src->y + src->getHeight() / 2.0f;
	MetaRoom* smeta = world.map->metaRoomAt(sx, sy);
	for (auto& a : world.agents) {
		if (!a || a == src)
			continue;
		oldCreature* c = getoldCreature(a.get());
		if (!c)
			continue;
		bool inrange = false;
		switch (range) {
			case OldStimRange::Sign:
				inrange = agentIsVisible(a.get(), src.get());
				break;
			case OldStimRange::Tact:
				inrange = agentsTouching(a.get(), src.get());
				break;
			case OldStimRange::Shout: {
				float cx = a->x + a->getWidth() / 2.0f;
				float cy = a->y + a->getHeight() / 2.0f;
				inrange = smeta && world.map->metaRoomAt(cx, cy) == smeta && std::hypot(cx - sx, cy - sy) <= 500.0f;
				break;
			}
		}
		if (inrange)
			c->handleStimulus(s);
	}
}

c2eCreature* getc2eCreature(Agent* a) {
	if (!a)
		return 0;
	CreatureAgent* b = dynamic_cast<CreatureAgent*>(a);
	if (!b)
		return 0;
	c2eCreature* c = dynamic_cast<c2eCreature*>(b->getCreature());
	return c;
}

/**
 STM# SHOU (command) stimulusno (integer)
 %status maybe
 %variants c1 c2
*/
void c_STM_SHOU(caosVM* vm) {
	VM_PARAM_INTEGER(stimulusno)

	StimulusEvent s;
	s.numbered = true;
	s.id = stimulusno;
	broadcastOldStimulus(vm, s, OldStimRange::Shout, "STM# SHOU");
}

/**
 STM# SIGN (command) stimulusno (integer)
 %status maybe
 %variants c1 c2
*/
void c_STM_SIGN(caosVM* vm) {
	VM_PARAM_INTEGER(stimulusno)

	StimulusEvent s;
	s.numbered = true;
	s.id = stimulusno;
	broadcastOldStimulus(vm, s, OldStimRange::Sign, "STM# SIGN");
}

/**
 STM# TACT (command) stimulusno (integer)
 %status maybe
 %variants c1 c2
*/
void c_STM_TACT(caosVM* vm) {
	VM_PARAM_INTEGER(stimulusno)

	StimulusEvent s;
	s.numbered = true;
	s.id = stimulusno;
	broadcastOldStimulus(vm, s, OldStimRange::Tact, "STM# TACT");
}

/**
 STM# WRIT (command) object (agent) stimulusno (integer)
 %status maybe
 %variants c1 c2
*/
void c_STM_WRIT(caosVM* vm) {
	VM_PARAM_INTEGER(stimulusno)
	VM_PARAM_VALIDAGENT(object)

	oldCreature* c = getoldCreature(object.get());
	if (!c)
		return; // ignored on non-creatures

	StimulusEvent s;
	s.numbered = true;
	s.id = stimulusno;
	s.source = vm->owner;
	s.command = "STM# WRIT";
	c->handleStimulus(s);
}

/**
 STIM SHOU (command) stimulus (integer) strength (float)
 %status stub

 Send a stimulus of the given type to all Creatures who can hear OWNR.
*/
void c_STIM_SHOU(caosVM* vm) {
	VM_VERIFY_SIZE(3)
	VM_PARAM_FLOAT_UNUSED(strength)
	VM_PARAM_INTEGER_UNUSED(stimulus)

	// TODO
}

/**
 STIM SIGN (command) stimulus (integer) strength (float)
 %status stub

 Sends a stimulus of the given type to all Creatures who can see OWNR.
*/
void c_STIM_SIGN(caosVM* vm) {
	VM_VERIFY_SIZE(3)
	VM_PARAM_FLOAT_UNUSED(strength)
	VM_PARAM_INTEGER_UNUSED(stimulus)

	// TODO
}

/**
 STIM TACT (command) stimulus (integer) strength (float)
 %status stub

 Sends a stimulus of the given type to all Creatures who are touching OWNR.
*/
void c_STIM_TACT(caosVM* vm) {
	VM_VERIFY_SIZE(3)
	VM_PARAM_FLOAT_UNUSED(strength)
	VM_PARAM_INTEGER_UNUSED(stimulus)

	// TODO
}

/**
 STIM WRIT (command) creature (agent) stimulus (integer) strength (float)
 %status maybe

 Sends a stimulus of the given type to specific Creature.
*/
void c_STIM_WRIT(caosVM* vm) {
	VM_VERIFY_SIZE(3)
	VM_PARAM_FLOAT(strength)
	VM_PARAM_INTEGER(stimulus)
	VM_PARAM_VALIDAGENT(creature)

	c2eCreature* c = getc2eCreature(creature.get());
	if (!c)
		return; // ignored on non-creatures

	// TODO: ownr?
	c->handleStimulus(stimulus, strength);
}

/**
 STIM SHOU (command) significance (integer) input (integer) intensity (integer) features (integer) chem0 (integer) amount0 (integer) chem1 (integer) amount1 (integer) chem2 (integer) amount2 (integer) chem3 (integer) amount3 (integer)
 %status maybe
 %variants c1 c2
*/
void c_STIM_SHOU_c2(caosVM* vm) {
	VM_PARAM_INTEGER(amount3)
	VM_PARAM_INTEGER(chem3)
	VM_PARAM_INTEGER(amount2)
	VM_PARAM_INTEGER(chem2)
	VM_PARAM_INTEGER(amount1)
	VM_PARAM_INTEGER(chem1)
	VM_PARAM_INTEGER(amount0)
	VM_PARAM_INTEGER(chem0)
	VM_PARAM_INTEGER(features)
	VM_PARAM_INTEGER(intensity)
	VM_PARAM_INTEGER(input)
	VM_PARAM_INTEGER(significance)

	StimulusEvent s;
	s.significance = significance;
	s.input = input;
	s.intensity = intensity;
	s.features = features;
	s.chem = {chem0, chem1, chem2, chem3};
	s.amount = {amount0, amount1, amount2, amount3};
	broadcastOldStimulus(vm, s, OldStimRange::Shout, "STIM SHOU");
}

/**
 STIM SIGN (command) significance (integer) input (integer) intensity (integer) features (integer) chem0 (integer) amount0 (integer) chem1 (integer) amount1 (integer) chem2 (integer) amount2 (integer) chem3 (integer) amount3 (integer)
 %status maybe
 %variants c1 c2
*/
void c_STIM_SIGN_c2(caosVM* vm) {
	VM_PARAM_INTEGER(amount3)
	VM_PARAM_INTEGER(chem3)
	VM_PARAM_INTEGER(amount2)
	VM_PARAM_INTEGER(chem2)
	VM_PARAM_INTEGER(amount1)
	VM_PARAM_INTEGER(chem1)
	VM_PARAM_INTEGER(amount0)
	VM_PARAM_INTEGER(chem0)
	VM_PARAM_INTEGER(features)
	VM_PARAM_INTEGER(intensity)
	VM_PARAM_INTEGER(input)
	VM_PARAM_INTEGER(significance)

	StimulusEvent s;
	s.significance = significance;
	s.input = input;
	s.intensity = intensity;
	s.features = features;
	s.chem = {chem0, chem1, chem2, chem3};
	s.amount = {amount0, amount1, amount2, amount3};
	broadcastOldStimulus(vm, s, OldStimRange::Sign, "STIM SIGN");
}

/**
 STIM TACT (command) significance (integer) input (integer) intensity (integer) features (integer) chem0 (integer) amount0 (integer) chem1 (integer) amount1 (integer) chem2 (integer) amount2 (integer) chem3 (integer) amount3 (integer)
 %status maybe
 %variants c1 c2
*/
void c_STIM_TACT_c2(caosVM* vm) {
	VM_PARAM_INTEGER(amount3)
	VM_PARAM_INTEGER(chem3)
	VM_PARAM_INTEGER(amount2)
	VM_PARAM_INTEGER(chem2)
	VM_PARAM_INTEGER(amount1)
	VM_PARAM_INTEGER(chem1)
	VM_PARAM_INTEGER(amount0)
	VM_PARAM_INTEGER(chem0)
	VM_PARAM_INTEGER(features)
	VM_PARAM_INTEGER(intensity)
	VM_PARAM_INTEGER(input)
	VM_PARAM_INTEGER(significance)

	StimulusEvent s;
	s.significance = significance;
	s.input = input;
	s.intensity = intensity;
	s.features = features;
	s.chem = {chem0, chem1, chem2, chem3};
	s.amount = {amount0, amount1, amount2, amount3};
	broadcastOldStimulus(vm, s, OldStimRange::Tact, "STIM TACT");
}

/**
 STIM WRIT (command) creature (agent) significance (integer) input (integer) intensity (integer) features (integer) chem0 (integer) amount0 (integer) chem1 (integer) amount1 (integer) chem2 (integer) amount2 (integer) chem3 (integer) amount3 (integer)
 %status maybe
 %variants c1 c2
*/
void c_STIM_WRIT_c2(caosVM* vm) {
	VM_PARAM_INTEGER(amount3)
	VM_PARAM_INTEGER(chem3)
	VM_PARAM_INTEGER(amount2)
	VM_PARAM_INTEGER(chem2)
	VM_PARAM_INTEGER(amount1)
	VM_PARAM_INTEGER(chem1)
	VM_PARAM_INTEGER(amount0)
	VM_PARAM_INTEGER(chem0)
	VM_PARAM_INTEGER(features)
	VM_PARAM_INTEGER(intensity)
	VM_PARAM_INTEGER(input)
	VM_PARAM_INTEGER(significance)
	VM_PARAM_AGENT(creature)

	oldCreature* c = getoldCreature(creature.get());
	if (!c)
		return; // ignored on non-creatures

	StimulusEvent s;
	s.significance = significance;
	s.input = input;
	s.intensity = intensity;
	s.features = features;
	s.chem = {chem0, chem1, chem2, chem3};
	s.amount = {amount0, amount1, amount2, amount3};
	s.source = vm->owner;
	s.command = "STIM WRIT";
	c->handleStimulus(s);
}

/**
 STIM FROM (command) significance (integer) input (integer) intensity (integer) features (integer) chem0 (integer) amount0 (integer) chem1 (integer) amount1 (integer) chem2 (integer) amount2 (integer) chem3 (integer) amount3 (integer)
 %status maybe
 %variants c1 c2
*/
void c_STIM_FROM_c1(caosVM* vm) {
	VM_PARAM_INTEGER(amount3)
	VM_PARAM_INTEGER(chem3)
	VM_PARAM_INTEGER(amount2)
	VM_PARAM_INTEGER(chem2)
	VM_PARAM_INTEGER(amount1)
	VM_PARAM_INTEGER(chem1)
	VM_PARAM_INTEGER(amount0)
	VM_PARAM_INTEGER(chem0)
	VM_PARAM_INTEGER(features)
	VM_PARAM_INTEGER(intensity)
	VM_PARAM_INTEGER(input)
	VM_PARAM_INTEGER(significance)

	valid_agent(vm->from.getAgent());
	oldCreature* c = getoldCreature(vm->from.getAgent().get());
	if (!c)
		return; // ignored when FROM is not a creature

	StimulusEvent s;
	s.significance = significance;
	s.input = input;
	s.intensity = intensity;
	s.features = features;
	s.chem = {chem0, chem1, chem2, chem3};
	s.amount = {amount0, amount1, amount2, amount3};
	s.source = vm->owner;
	s.command = "STIM FROM";
	c->handleStimulus(s);
}

/**
 SWAY SHOU (command) drive (integer) adjust (float) drive (integer) adjust (float) drive (integer) adjust (float) drive (integer) adjust (float) 
 %status stub

 Adjusts these four drives in all Creatures who can hear OWNR.
*/
void c_SWAY_SHOU(caosVM* vm) {
	VM_VERIFY_SIZE(8)
	VM_PARAM_FLOAT_UNUSED(adjust4)
	VM_PARAM_INTEGER_UNUSED(drive4)
	VM_PARAM_FLOAT_UNUSED(adjust3)
	VM_PARAM_INTEGER_UNUSED(drive3)
	VM_PARAM_FLOAT_UNUSED(adjust2)
	VM_PARAM_INTEGER_UNUSED(drive2)
	VM_PARAM_FLOAT_UNUSED(adjust1)
	VM_PARAM_INTEGER_UNUSED(drive1)

	valid_agent(vm->owner);
	//TODO
}

/**
 SWAY SIGN (command) drive (integer) adjust (float) drive (integer) adjust (float) drive (integer) adjust (float) drive (integer) adjust (float) 
 %status stub

 Adjusts these four drives in all Creatures who can see OWNR.
*/
void c_SWAY_SIGN(caosVM* vm) {
	VM_VERIFY_SIZE(8)
	VM_PARAM_FLOAT_UNUSED(adjust4)
	VM_PARAM_INTEGER_UNUSED(drive4)
	VM_PARAM_FLOAT_UNUSED(adjust3)
	VM_PARAM_INTEGER_UNUSED(drive3)
	VM_PARAM_FLOAT_UNUSED(adjust2)
	VM_PARAM_INTEGER_UNUSED(drive2)
	VM_PARAM_FLOAT_UNUSED(adjust1)
	VM_PARAM_INTEGER_UNUSED(drive1)

	valid_agent(vm->owner);
	//TODO
}

/**
 SWAY TACT (command) drive (integer) adjust (float) drive (integer) adjust (float) drive (integer) adjust (float) drive (integer) adjust (float) 
 %status stub

 Adjusts these four drives in all Creatures who are touching OWNR.
*/
void c_SWAY_TACT(caosVM* vm) {
	VM_VERIFY_SIZE(8)
	VM_PARAM_FLOAT_UNUSED(adjust4)
	VM_PARAM_INTEGER_UNUSED(drive4)
	VM_PARAM_FLOAT_UNUSED(adjust3)
	VM_PARAM_INTEGER_UNUSED(drive3)
	VM_PARAM_FLOAT_UNUSED(adjust2)
	VM_PARAM_INTEGER_UNUSED(drive2)
	VM_PARAM_FLOAT_UNUSED(adjust1)
	VM_PARAM_INTEGER_UNUSED(drive1)

	valid_agent(vm->owner);
	//TODO
}

/**
 SWAY WRIT (command) creature (agent) drive (integer) adjust (float) drive (integer) adjust (float) drive (integer) adjust (float) drive (integer) adjust (float) 
 %status stub

 Adjusts these four drives in the specified creature.
*/
void c_SWAY_WRIT(caosVM* vm) {
	VM_VERIFY_SIZE(8)
	VM_PARAM_FLOAT_UNUSED(adjust4)
	VM_PARAM_INTEGER_UNUSED(drive4)
	VM_PARAM_FLOAT_UNUSED(adjust3)
	VM_PARAM_INTEGER_UNUSED(drive3)
	VM_PARAM_FLOAT_UNUSED(adjust2)
	VM_PARAM_INTEGER_UNUSED(drive2)
	VM_PARAM_FLOAT_UNUSED(adjust1)
	VM_PARAM_INTEGER_UNUSED(drive1)
	VM_PARAM_VALIDAGENT(creature)

	valid_agent(vm->owner);
	//TODO
}

/**
 NOHH (command)
 %status stub

 Tells the target Creature to stop holding hands with the pointer.
*/
void c_NOHH(caosVM* vm) {
	VM_VERIFY_SIZE(0)

	if (!vm->targ)
		return; // DS agent help, at least, does 'targ hhld nohh'

	Creature* c = vm->getTargCreature();
	// TODO
	(void)c;
}

/**
 ZOMB (command) zombie (integer)
 %status maybe

 Turns zombification of the target Creature on and off.  Set to 1 to disconnect the brain and 
 motor of the target Creature, and 0 to undo.
*/
void c_ZOMB(caosVM* vm) {
	VM_VERIFY_SIZE(1)
	VM_PARAM_INTEGER(zombie)

	Creature* c = vm->getTargCreature();

	c->setZombie(zombie);
}

/**
 ZOMB (integer)
 %status maybe

 Returns 1 if target Creature is zombified, or 0 if otherwise.
*/
void v_ZOMB(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	vm->result.setInt(c->isZombie());
}

/**
 DIRN (command) direction (integer)
 %status maybe
 %variants c2 cv c3

 Changes the target Creature to face a different direction.
*/
void c_DIRN(caosVM* vm) {
	VM_VERIFY_SIZE(1)
	VM_PARAM_INTEGER(direction)

	THROW_IFNOT(direction >= 0 && direction <= 3);

	CreatureAgent* c = vm->getTargCreatureAgent();
	c->setDirection(direction);
}

/**
 DIRN (integer)
 %status maybe
 %variants c2 cv c3

 Returns the direction the target Creatures is facing.
*/
void v_DIRN(caosVM* vm) {
	// C1/C2: no creature, no direction (0). Object 2 13 2's timer turns it
	// the way its carrier faces with TARG CARR then DIRN, also while nothing
	// carries it; that threw "Invalid agent handle" about once a second.
	if (engine.version < 3 && !dynamic_cast<CreatureAgent*>(vm->targ.get())) {
		vm->result.setInt(0);
		return;
	}
	CreatureAgent* c = vm->getTargCreatureAgent();

	// C2 creatures turn by their poses, which don't update the direction
	// DIRN sets: report the way the body actually faces
	if (engine.version == 2)
		if (auto* sc = dynamic_cast<SkeletalCreature*>(vm->targ.get())) {
			vm->result.setInt(sc->facing());
			return;
		}
	vm->result.setInt(c->getDirection());
}

/**
 HHLD (agent)
 %status stub

 Returns Creatures that are holding hands with pointer, or NULL if none.
*/
void v_HHLD(caosVM* vm) {
	VM_VERIFY_SIZE(0)

	vm->result.setAgent(0); // TODO
}

/**
 MVFT (command) x (float) y (float)
 %status maybe

 Move the target Creature's foot (along with the rest of the Creature, obviously) to the given 
 coordinates.  You should use this rather than MVTO for Creatures.
*/
void c_MVFT(caosVM* vm) {
	VM_VERIFY_SIZE(2)
	VM_PARAM_FLOAT(y)
	VM_PARAM_FLOAT(x)

	THROW_IFNOT(vm->targ);
	SkeletalCreature* c = dynamic_cast<SkeletalCreature*>(vm->targ.get());
	THROW_IFNOT(c);

	// TODO: this should be nicer
	float downfootxoffset = c->attachmentX(c->isLeftFootDown() ? 11 : 12, 0);
	float downfootyoffset = c->attachmentY(c->isLeftFootDown() ? 11 : 12, 0);

	vm->targ->moveTo(x - downfootxoffset, y - downfootyoffset);
}

/**
 CREA (integer) agent (agent)
 %status done

 Determines whether the given agent is a creature or not (0 or 1).
*/
void v_CREA(caosVM* vm) {
	VM_VERIFY_SIZE(1)
	VM_PARAM_AGENT(agent)

	CreatureAgent* c = dynamic_cast<CreatureAgent*>(agent.get());
	if (c)
		vm->result.setInt(1);
	else
		vm->result.setInt(0);
}

/**
 VOCB (command)
 %status maybe

 Makes the target Creature learn all vocabulary words immediately.
*/
void c_VOCB(caosVM* vm) {
	VM_VERIFY_SIZE(0)

	Creature* c = vm->getTargCreature();
	learnAllWords(dynamic_cast<oldCreature*>(c));
}

/**
 DEAD (command)
 %status maybe

 Kill the target Creature biologically.
*/
void c_DEAD(caosVM* vm) {
	Creature* c = vm->getTargCreature();

	c->die();
}

/**
 DEAD (integer)
 %status maybe
 %variants c1 c2 cv c3

 Determines whether the target Creature is alive (0) or dead (255 in c1/c2, 1 otherwise).
*/
void v_DEAD(caosVM* vm) {
	Creature* c = vm->getTargCreature();

	if (engine.version < 3 && !c->isAlive())
		vm->result.setInt(255);
	else
		vm->result.setInt(!c->isAlive());
}

/**
 NORN (command) creature (agent)
 %status maybe

 Sets the target Creature to the given one.
*/
void c_NORN(caosVM* vm) {
	VM_PARAM_AGENT(creature)

	world.selectCreature(creature);
}

/**
 NORN (agent)
 %status maybe
 %variants cv c3

 Returns the target Creature.
*/
/**
 NORN (variable)
 %status maybe
 %variants c1 c2

 Returns the target Creature.
*/
CAOS_LVALUE(NORN, (void)0,
	caosValue(world.selectedcreature),
	world.selectCreature(newvalue.getAgent()))

#include "AgentHelpers.h"

/**
 URGE SHOU (command) noun_stim (float) verb_id (integer) verb_stim (float)
 %status stub

 Urges all Creatures who can hear OWNR to perform the given action on OWNR.
 The two stimuli parameters can range from -1.0 (discourage) to 1.0 (encourage).
*/
void c_URGE_SHOU(caosVM* vm) {
	VM_PARAM_FLOAT_UNUSED(verb_stim)
	VM_PARAM_INTEGER_UNUSED(verb_id)
	VM_PARAM_FLOAT_UNUSED(noun_stim)

	valid_agent(vm->owner);

	// TODO
}

/**
 URGE SIGN (command) noun_stim (float) verb_id (integer) verb_stim (float)
 %status maybe

 Urges all Creatures who can see OWNR to perform the given action on OWNR.
 The two stimuli parameters can range from -1.0 (discourage) to 1.0 (encourage).
*/
void c_URGE_SIGN(caosVM* vm) {
	VM_PARAM_FLOAT(verb_stim)
	VM_PARAM_INTEGER(verb_id)
	VM_PARAM_FLOAT(noun_stim)

	valid_agent(vm->owner);

	// TODO: sanitise stim params (bind to -1.0/1.0)
	c2eStim stim;
	stim.noun_id = vm->owner->category;
	if (stim.noun_id == -1)
		return; // TODO: correct?
	stim.noun_amount = noun_stim;
	stim.verb_id = verb_id;
	stim.verb_amount = verb_stim;

	std::vector<std::shared_ptr<Agent> > agents = getVisibleList(vm->owner, 0, 0, 0);
	for (auto a : agents) {
		// guaranteed to be valid from getVisibleList

		CreatureAgent* ca = dynamic_cast<CreatureAgent*>(a.get());
		if (!ca)
			continue;
		c2eCreature* c = dynamic_cast<c2eCreature*>(ca->getCreature());
		if (!c)
			continue;

		c->handleStimulus(stim);
	}
}

/**
 URGE TACT (command) noun_stim (float) verb_id (integer) verb_stim (float)
 %status stub

 Urges all Creatures who are touching OWNR to perform the given action on OWNR.
 The two stimuli parameters can range from -1.0 (discourage) to 1.0 (encourage).
*/
void c_URGE_TACT(caosVM* vm) {
	VM_PARAM_FLOAT_UNUSED(verb_stim)
	VM_PARAM_INTEGER_UNUSED(verb_id)
	VM_PARAM_FLOAT_UNUSED(noun_stim)

	valid_agent(vm->owner);

	// TODO
}

/**
 URGE WRIT (command) creature (agent) noun_id (integer) noun_stim (float) verb_id (integer) verb_stim (float)
 %status stub

 Urges the specified Creature to perform the specified action (verb) on the specified object type 
 (noun).  Provide a stim greater than 1 to force, and an id of -1 and a stim greater than 1 to unforce.
*/
void c_URGE_WRIT(caosVM* vm) {
	VM_PARAM_FLOAT_UNUSED(verb_stim)
	VM_PARAM_INTEGER_UNUSED(verb_id)
	VM_PARAM_FLOAT_UNUSED(noun_stim)
	VM_PARAM_INTEGER_UNUSED(noun_id)
	VM_PARAM_AGENT(creature)

	if (!creature)
		return;
	c2eCreature* c = getc2eCreature(creature.get());
	if (!c)
		return; // ignored on non-creatures

	// TODO
}

/**
 DRIV (command) drive_id (integer) adjust (float)
 %status maybe

 Modifies the level of a drive in target Creature by the given level, which can range from -1.0 (decrease) to 1.0 (increase).
*/
void c_DRIV(caosVM* vm) {
	VM_PARAM_FLOAT(adjust)
	VM_PARAM_INTEGER(drive_id)
	THROW_IFNOT(drive_id >= 0 && drive_id < 20);

	valid_agent(vm->targ);
	c2eCreature* c = getc2eCreature(vm->targ.get());
	if (!c)
		return; // ignored on non-creatures

	c->adjustDrive(drive_id, adjust);
}

/**
 DRIV (float) drive_id (integer)
 %status maybe

 Returns the level of a drive (0.0 to 1.0) in target Creature.
*/
void v_DRIV(caosVM* vm) {
	VM_PARAM_INTEGER(drive_id)
	THROW_IFNOT(drive_id >= 0 && drive_id < 20);

	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);

	vm->result.setFloat(c->getDrive(drive_id));
}

/**
 CHEM (command) chemical_id (integer) adjust (float)
 %status maybe

 Modifies the level of a chemical in target Creature's bloodstream by adjust, which can range from -1.0 (decrease) to 1.0 (increase).
*/
void c_CHEM(caosVM* vm) {
	VM_PARAM_FLOAT(adjust)
	VM_PARAM_INTEGER(chemical_id)
	THROW_IFNOT(chemical_id >= 0 && chemical_id < 256);

	valid_agent(vm->targ);
	c2eCreature* c = getc2eCreature(vm->targ.get());
	if (!c)
		return; // ignored on non-creatures

	c->adjustChemical(chemical_id, adjust);
}

/**
 CHEM (command) chemical_id (integer) adjust (integer)
 %status maybe
 %variants c1 c2

 Set the level of a chemical (0 to 255) in target creature's bloodstream.
*/
void c_CHEM_c1(caosVM* vm) {
	VM_PARAM_INTEGER(adjust)
	VM_PARAM_INTEGER(chemical_id)
	THROW_IFNOT(chemical_id >= 0 && chemical_id < 256);

	valid_agent(vm->targ);
	oldCreature* c = getoldCreature(vm->targ.get());
	if (!c)
		return; // ignored on non-creatures

	// openc2e: a negative amount takes chemical away (it used to be cast to
	// a byte and added: -255 added 1). Real C2 ignores negative amounts.
	if (adjust >= 0)
		c->addChemical(chemical_id, (unsigned char)std::min(adjust, 255));
	else
		c->subChemical(chemical_id, (unsigned char)std::min(-adjust, 255));
}

/**
 CHEM (float) chemical_id (integer)
 %status maybe

 Returns the level of a chemical (0.0 to 1.0) in target creature's bloodstream.
*/
void v_CHEM(caosVM* vm) {
	VM_PARAM_INTEGER(chemical_id)
	THROW_IFNOT(chemical_id >= 0 && chemical_id < 256);

	valid_agent(vm->targ);
	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);

	vm->result.setFloat(c->getChemical(chemical_id));
}

/**
 CHEM (integer) chemical_id (integer)
 %status maybe
 %variants c1 c2
 
 Returns the level of a chemical (0 to 255) in target creature's bloodstream.
*/
void v_CHEM_c1(caosVM* vm) {
	VM_PARAM_INTEGER(chemical_id)
	THROW_IFNOT(chemical_id >= 0 && chemical_id < 256);

	valid_agent(vm->targ);
	oldCreature* c = getoldCreature(vm->targ.get());
	THROW_IFNOT(c);

	vm->result.setInt(c->getChemical(chemical_id));
}

/**
 ASLP (command) asleep (integer)
 %status maybe
 %variants c1 c2 cv c3

 If asleep is 1, makes the target creature sleep. If asleep is 0, makes the target creature wake.
*/
void c_ASLP(caosVM* vm) {
	VM_PARAM_INTEGER(asleep)

	Creature* c = vm->getTargCreature();

	c->setAsleep(asleep);
}

/**
 ASLP (integer)
 %status maybe
 %variants c1 c2 cv c3

 Determines whether the target Creature is asleep.
*/
void v_ASLP(caosVM* vm) {
	Creature* c = vm->getTargCreature();

	vm->result.setInt(c->isAsleep());
}

#include "AgentHelpers.h"

class blockUntilApproached : public blockCond {
  protected:
	AgentRef parent, it;
	CreatureAgent* parentcreature;

  public:
	blockUntilApproached(AgentRef p, AgentRef i)
		: parent(p), it(i) {
		assert(parent);
		parentcreature = dynamic_cast<CreatureAgent*>(parent.get());
		assert(parentcreature);
	}
	virtual bool operator()() {
		if (!parent)
			return false;
		if (!it) {
			// nothing to approach (the target has gone): stand, rather than
			// carry on with whatever walking was going on
			parentcreature->stopWalking();
			return false; // TODO: CAs
		}

		if (agentsTouching(parent, it)) {
			static const bool watch = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
			if (watch)
				fmt::print("* WATCH REACHED unid {} {} {} {}\n", parent->getUNID(), (int)it->family, (int)it->genus, it->species);
			return false;
		}

		// parentcreature is guaranteed to be valid if parent is

		// TODO: cope with this problem (eg: another APPR, creature paused, non-skeletal creature, etc)
		//if (parentcreature->isApproaching()) return false;

		// note that this merely sets up the approach to be done on the next tick of the creature
		parentcreature->approach(it);

		return true;
	}
};

/**
 APPR (command)
 %status maybe
 %variants c1 c2 cv c3

 Makes the target Creature approach the IT agent (or if none, an agent of that category using CAs), 
 blocking until it makes it there or gives up.
*/
void c_APPR(caosVM* vm) {
	CreatureAgent* a = vm->getTargCreatureAgent();
	THROW_IFNOT(a);

	vm->startBlocking(new blockUntilApproached(vm->targ, vm->_it_));
}

/**
 UNCS (command) unconscious (integer)
 %status maybe
 %variants c2 cv c3

 Makes the target Creature conscious if 0, or unconscious if 1.
*/
void c_UNCS(caosVM* vm) {
	VM_PARAM_INTEGER(unconscious)

	Creature* c = vm->getTargCreature();
	// C2: the engine's own unconsciousness (c2Creature::updateConsciousness):
	// 1 faints it now (it comes round once its energy has recovered), 0
	// brings it round
	if (auto* oc = dynamic_cast<oldCreature*>(c))
		oc->setUnconscious(unconscious != 0);
}

/**
 UNCS (integer)
 %status maybe
 %variants c2 cv c3

 Returns 1 if the target Creature is unconscious, or 0 otherwise.
*/
void v_UNCS(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	// the creature scripts ask before reacting or walking ("doif uncs ne 0
	// stop"); answering 0 always let an unconscious creature walk and react
	auto* oc = dynamic_cast<oldCreature*>(c);
	vm->result.setInt(oc && oc->isUnconscious() ? 1 : 0);
}

/**
 FACE (command) number (integer)
 %status stub
*/
void c_FACE(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(number)

	THROW_IFNOT(vm->targ);
	SkeletalCreature* c = dynamic_cast<SkeletalCreature*>(vm->targ.get());
	THROW_IFNOT(c);

	// TODO
}

/**
 FACE (facevalue)
 %status maybe

 Returns the front-facing pose for the current facial expression or the current
 sprite filename of the target creature, depending on the desired return type.
*/
void v_FACE(caosVM* vm) {
	THROW_IFNOT(vm->targ);
	SkeletalCreature* c = dynamic_cast<SkeletalCreature*>(vm->targ.get());
	THROW_IFNOT(c);

	vm->result = FaceValue{
		(int)c->getFaceSpriteFrame(),
		c->getFaceSpriteName()};
}

/**
 LIKE (command) creature (agent)
 %status stub
 
 Causes the target Creature to state an opinion about the specified Creature.
*/
void c_LIKE(caosVM* vm) {
	VM_PARAM_VALIDAGENT(creature)

	vm->getTargCreature();
	// TODO
}

/**
 LIMB (string) bodypart (integer) genus (integer) gender (integer) age (integer) variant (integer)
 %status stub
 
 Returns the filename for the specified part of a Creature, substituting as necessary.
*/
void v_LIMB(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(variant)
	VM_PARAM_INTEGER_UNUSED(age)
	VM_PARAM_INTEGER_UNUSED(gender)
	VM_PARAM_INTEGER_UNUSED(genus)
	VM_PARAM_INTEGER_UNUSED(bodypart)

	vm->getTargCreature();
	vm->result.setString(""); // TODO
}

/**
 ORDR SHOU (command) speech (string)
 %status stub

 Makes the target Creature speak the specified speech to all creatures in hearing range.
*/
void c_ORDR_SHOU(caosVM* vm) {
	VM_PARAM_STRING_UNUSED(speech)

	valid_agent(vm->targ);
	// TODO
}

/**
 ORDR SIGN (command) speech (string)
 %status stub

 Makes the target Creature speak the specified speech to all creatures that can see it.
*/
void c_ORDR_SIGN(caosVM* vm) {
	VM_PARAM_STRING_UNUSED(speech)

	valid_agent(vm->targ);
	// TODO
}

/**
 ORDR TACT (command) speech (string)
 %status stub

 Makes the target Creature speak the specified speech to all creatures that are touching it.
*/
void c_ORDR_TACT(caosVM* vm) {
	VM_PARAM_STRING_UNUSED(speech)

	valid_agent(vm->targ);
	// TODO
}

/**
 ORDR WRIT (command) creature (agent) speech (string)
 %status stub

 Makes the target Creature speak the specified speech to the specified creature.
*/
void c_ORDR_WRIT(caosVM* vm) {
	VM_PARAM_STRING_UNUSED(speech)
	VM_PARAM_VALIDAGENT(creature)

	valid_agent(vm->targ);
	// TODO
}

/**
 DREA (command) dream (integer)
 %status maybe

 Turns a Creature's dreaming on and off.  A Creature's instincts are 
 processed while it is dreaming.  If it is not asleep already, then it 
 will be made to sleep before dreaming begins.
*/
void c_DREA(caosVM* vm) {
	VM_PARAM_INTEGER(dream)

	Creature* c = vm->getTargCreature();
	c->setDreaming(dream);
}

/**
 DREA (integer)
 %status maybe

 Returns whether or not the target Creature is dreaming (0 or 1).
*/
void v_DREA(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	vm->result.setInt(c->isDreaming());
}

/**
 BORN (command)
 %status maybe

 Registers the birth of the target Creature, and sends a birth event to 
 the game.
*/
void c_BORN(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	c->born();
}

/**
 CAGE (integer)
 %status maybe
 %variants c1 c2 cv c3

 Returns the integer value of the target Creature's current life stage.
*/
void v_CAGE(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	vm->result.setInt((int)c->getStage());
}

/**
 BYIT (integer)
 %status maybe

 Determines whether or not the target Creature can reach the IT agent (0 
 or 1).
*/
void v_BYIT(caosVM* vm) {
	vm->getTargCreature();

	if (vm->_it_ && agentsTouching(vm->targ, vm->_it_)) // TODO
		vm->result.setInt(1);
	else
		vm->result.setInt(0);
}

/**
 _IT_ (agent)
 %status maybe
 %variants c1 c2 cv c3

 Returns the agent that the OWNR creature was focused on when the 
 current script began running.
*/
void v_IT(caosVM* vm) {
	valid_agent(vm->owner);
	THROW_IFNOT(dynamic_cast<CreatureAgent*>(vm->owner.get())); // TODO: return null instead?
	vm->result.setAgent(vm->_it_);
}

/**
 NEWC (command) family (integer) gene_agent (agent) gene_slot (integer) sex (integer) variant (integer)
 %status maybe
 
 Creates a new creature over the space of a few ticks, using the specified agent/slot for genetic data. sex is 0 for random, 1 for male or 2 for female.
*/
void c_NEWC(caosVM* vm) {
	VM_PARAM_INTEGER(variant)
	VM_PARAM_INTEGER(sex)
	VM_PARAM_INTEGER(gene_slot)
	VM_PARAM_VALIDAGENT(gene_agent)
	VM_PARAM_INTEGER(family)

	// TODO: creation should be blocking and multiple-tick!

	std::map<unsigned int, std::shared_ptr<class genomeFile> >::iterator i = gene_agent->genome_slots.find(gene_slot);
	THROW_IFNOT(i != gene_agent->genome_slots.end());

	// randomise sex if necessary
	if (sex == 0)
		sex = rand_int32(1, 2);
	THROW_IFNOT(sex == 1 || sex == 2); // TODO: correct?

	// TODO: if variant is 0, randomise between 1 and 8
	SkeletalCreature* a = new SkeletalCreature(family);
	try {
		c2eCreature* c = new c2eCreature(i->second, (sex == 2), variant, a);
		a->setCreature(c);
	} catch (...) {
		delete a;
		throw;
	}

	a->finishInit();

	world.history->getMoniker(world.history->findMoniker(i->second)).moveToCreature(a);
	gene_agent->genome_slots.erase(i);

	vm->setTarg(a);
}

/**
 NEW: CREA (command) family (integer) gene_agent (agent) gene_slot (integer) sex (integer) variant (integer)
 %status stub
 
 Creates a new creature using the specified agent/slot for genetic data. sex is 0 for random, 1 for male or 2 for female.
*/
void c_NEW_CREA(caosVM* vm) {
	/*VM_PARAM_INTEGER(variant)
	VM_PARAM_INTEGER(sex)
	VM_PARAM_INTEGER(gene_slot)
	VM_PARAM_VALIDAGENT(gene_agent)
	VM_PARAM_INTEGER(family)*/

	c_NEWC(vm); // TODO
	//targ = NULL; // TODO
}

/**
 NEW: CREA (command) moniker (integer) sex (integer)
 %status maybe
 %variants c1 c2

 Creates a new creature using the specified moniker for genetic data. sex is 0 for random, 1 for male or 2 for female.
*/
void c_NEW_CREA_c1(caosVM* vm) {
	VM_PARAM_INTEGER(sex)
	VM_PARAM_INTEGER(moniker)

	THROW_IFNOT(moniker != 0);

	std::string realmoniker = std::string((char*)&moniker, 4);
	std::shared_ptr<genomeFile> genome = world.loadGenome(realmoniker);
	if (!genome)
		throw Exception("failed to find genome file '" + realmoniker + "'");
	// radiation before birth, in its mother and its egg (Exposure.h)
	if (engine.version == 2)
		genome = Exposure::atHatching(realmoniker, genome);

	THROW_IFNOT(genome->getVersion() == engine.version);

	// randomise sex if necessary
	if (sex == 0)
		sex = rand_int32(1, 2);
	THROW_IFNOT(sex == 1 || sex == 2); // TODO: correct?

	SkeletalCreature* a = new SkeletalCreature(4);

	// TODO: why do we even need to pass a variant here?
	oldCreature* c;

	try {
		if (engine.version == 1)
			c = new c1Creature(genome, (sex == 2), 0, a);
		else
			c = new c2Creature(genome, (sex == 2), 0, a);
	} catch (...) {
		delete a;
		throw;
	}

	a->setCreature(c);
	try {
		a->finishInit();
	} catch (...) {
		// (no body could be made for it: never left half made in the world,
		// where its next step crashed the game)
		a->kill();
		throw;
	}
	// its seasonal rhythm, if one was passed on with this genome (MATE)
	{
		float z;
		if (SeasonalRhythm::load(realmoniker, z))
			c->seasonalZ = z;
		// and its temperament (MATE), in place of the one drawn at random
		// and its mind's own settings (MindGenes.h)
		MindGenes mg;
		if (MindGeneSet::load(realmoniker, mg))
			c->mindGenes = mg;
		// what its own genes' chemistry does to its temperament (GenomeTraits.h)
		std::string geneWords;
		for (int i = 0; i < NUM_TRAITS; i++)
			if (std::fabs(c->geneShift[i]) >= 0.05f)
				geneWords += fmt::format("{}{} {:+.2f}", geneWords.empty() ? "" : ", ", Temperaments::traitName(i), c->geneShift[i]);
		if (!geneWords.empty())
			fmt::print("* {}'s genes shift its temperament: {}\n", realmoniker, geneWords);
		Temperament t;
		if (Temperaments::load(realmoniker, t)) {
			c->temperamentBorn = t;
			c->refreshTemperament();
			fmt::print("* {} hatched with a temperament from its parents: {} ({})\n", realmoniker, Temperaments::describe(t), Temperaments::toLine(t));
		}
	}

	a->genome_slots[0] = genome;
	world.newMoniker(genome, realmoniker, a);
	{
		std::string mon = world.history->findMoniker(genome);
		monikerData& m = world.history->getMoniker(mon);
		m.moveToCreature(a);
		// a bred creature's genus gene names its parents (MATE): conceived
		for (auto& gn : genome->genes)
			if (auto* genus = dynamic_cast<creatureGenusGene*>(gn.get())) {
				std::string mum = genus->mum.c_str(), dad = genus->dad.c_str();
				if (mum.size() == 4 && dad.size() == 4 && world.history->hasMoniker(mum) && world.history->hasMoniker(dad)) {
					// (the gene splicer's: spliced; bred: conceived)
					bool spliced = world.history->splicedGenomes.count(realmoniker) > 0;
					m.addEvent(spliced ? 1 : 0, mum, dad);
					m.spliced = spliced;
				}
				break;
			}
		// C1/C2 hatching scripts don't call BORN, so the birth is recorded
		// here: its date, a default name ("Ettin 0002") and the life log
		if (!world.history->quiet) {
			m.borntick = world.worldtickcount;
			m.bornknown = true;
			if (m.name.empty())
				m.name = world.history->nextDefaultName(m.genus);
			world.history->logLife(lifeLogEntry::birth, mon, (int)c->getStage());
			BirthPortrait::schedule(AgentRef(a)); // its portrait, once placed
		}
	}

	// C1/C2 hatching scripts don't call BORN, so select a new creature here
	// if nothing is selected (see also Creature::born)
	if (!world.selectedcreature)
		world.selectCreature(AgentRef(a).lock());

	vm->setTarg(a);
}

/**
 NEW: CRAG (command) family (integer) gene_agent (agent) gene_slot (integer) sex (integer) variant (integer) sprite_file (string) image_count (integer) first_image (integer) plane (integer)
 %status maybe
 %variants c3 sm
*/
void c_NEW_CRAG(caosVM* vm) {
	VM_PARAM_INTEGER(plane)
	VM_PARAM_INTEGER(first_image)
	VM_PARAM_INTEGER(image_count)
	VM_PARAM_STRING(sprite_file)
	VM_PARAM_INTEGER(variant)
	VM_PARAM_INTEGER(sex)
	VM_PARAM_INTEGER(gene_slot)
	VM_PARAM_VALIDAGENT(gene_agent)
	VM_PARAM_INTEGER(family)

	std::map<unsigned int, std::shared_ptr<class genomeFile> >::iterator i = gene_agent->genome_slots.find(gene_slot);
	THROW_IFNOT(i != gene_agent->genome_slots.end());

	// randomise sex if necessary
	if (sex == 0)
		sex = rand_int32(1, 2);
	THROW_IFNOT(sex == 1 || sex == 2); // TODO: correct?

	// TODO: if variant is 0, randomise between 1 and 8
	CompoundCreature* a = new CompoundCreature(family, plane, sprite_file, first_image, image_count);
	try {
		c2eCreature* c = new c2eCreature(i->second, (sex == 2), variant, a);
		a->setCreature(c);
	} catch (...) {
		delete a;
		throw;
	}

	a->finishInit();

	world.history->getMoniker(world.history->findMoniker(i->second)).moveToCreature(a);
	i->second.reset(); // TODO: remove the slot from the gene_agent entirely

	vm->setTarg(a);
}

/**
 LTCY (command) action (integer) min (integer) max (integer)
 %status maybe
 %variants c1 c2 cv c3
*/
void c_LTCY(caosVM* vm) {
	VM_PARAM_INTEGER(max);
	THROW_IFNOT(max >= 0 && max <= 255);
	VM_PARAM_INTEGER(min);
	THROW_IFNOT(min >= 0 && min <= 255);
	VM_PARAM_INTEGER(action);

	int n = rand_int32(min, max);

	Creature* c = vm->getTargCreature();

	c2eCreature* c2ec = dynamic_cast<c2eCreature*>(c);
	if (c2ec) {
		c2ec->setInvolActionLatency(action, n);
	}
	oldCreature* oldc = dynamic_cast<oldCreature*>(c);
	if (oldc) {
		oldc->setInvolActionLatency(action, n);
	}
}

/**
 F**K (command)
 %status stub
 %variants c1
*/

/**
 MATE (command)
 %variants c2 cv c3
 %status maybe

 The TARG creature and _IT_ mate. In C2 the world's kiss script (activate 1
 or 2 on a creature, between a willing grown pair) runs it. If one is female
 and one male, she is fertile and receptive (her reproductive loci, set by
 her chemistry) and not already pregnant, and he is fertile, she conceives:
 the baby's genome is made from both (crossGenomes: crossover and gentle
 mutation, our own recipe) and kept in the user Genetics folder under a new
 four-letter name, which she carries as BABY until the world's egg-laying
 script lays it in an egg. Her pregnant locus is set, so her genes' pregnancy
 chemistry runs. OPENC2E_TEST_CONCEIVE=1 (developer aid) skips the liking, fertility
 checks. C2 only for now.
*/
void c_MATE(caosVM* vm) {
	CreatureAgent* a = dynamic_cast<CreatureAgent*>(vm->targ.get());
	THROW_IFNOT(a);
	// with no creature in attention, nothing (BunchaStuff's "Sex it up"
	// sends every norn the kiss message, whatever it is looking at)
	CreatureAgent* b = dynamic_cast<CreatureAgent*>(vm->_it_.get());
	if (!b)
		return;
	tryConceive(a, b);
}

/**
 DRV! (integer)
 %status stub
 %variants c1 c2 cv c3
*/
void v_DRV(caosVM* vm) {
	vm->getTargCreature();
	vm->result.setInt(0); // TODO
}

/**
 IITT (agent)
 %status maybe

 Return the agent which the target creature is currently focused on. Note that you should probably use _IT_ in creature scripts.
*/
void v_IITT(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	vm->result.setAgent(c->getAttentionFocus());
}

/**
 AGES (command) times (integer)
 %status maybe
 %variants c1 c2 cv c3

 Age (ie, increase the life stage of) the target creature the specified number of times.
 openc2e extension for C1/C2: a negative number makes the creature that many
 stages younger (it keeps its chemistry, organs and mind; TARG becomes the new
 body), and the creature's life chemical is set to where the new stage begins.
*/
void c_AGES(caosVM* vm) {
	VM_PARAM_INTEGER(times)

	if (engine.version < 3) {
		Creature* c = vm->getTargCreature();
		std::shared_ptr<Agent> a = vm->targ.lock();
		std::string err;
		if (!WorldSave::SetCreatureLifeStage(a, (int)c->getStage() + times, err))
			throw caosException(err);
		vm->setTarg(a);
		return;
	}
	THROW_IFNOT(times >= 0);

	Creature* c = vm->getTargCreature();
	for (int i = 0; i < times; i++) {
		c->ageCreature();
	}
}

/**
 DBG: STAG (command) stage (integer)
 %status maybe
 %variants c1 c2

 openc2e: move the target creature to life stage 0-6 (baby to senile) the
 way the Creatures menu does: one stage at a time with a short pause, ageing
 through the genome's own ageing receptors (younger: rebuilt stage by stage).
*/
void c_DBG_STAG(caosVM* vm) {
	VM_PARAM_INTEGER(stage)
	vm->getTargCreature();
	WorldSave::RequestLifeStage(vm->targ.lock(), stage);
}

/**
 LOCI (command) type (integer) organ (integer) tissue (integer) id (integer) value (float)
 %status maybe

 Set the value of the specified loci of the target creature. 'type' is 0 for receptor loci and 1 for emitter loci.
 See genetics documentation for details of the parameters.
*/
void c_LOCI(caosVM* vm) {
	VM_PARAM_FLOAT(value)
	VM_PARAM_INTEGER(id)
	VM_PARAM_INTEGER(tissue)
	VM_PARAM_INTEGER(organ)
	VM_PARAM_INTEGER(type)

	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);

	float* f = c->getLocusPointer(!type, organ, tissue, id);
	THROW_IFNOT(f);
	*f = value;
}

/**
 LOCI (float) type (integer) organ (integer) tissue (integer) id (integer)
 %status maybe
 
 Return the current value of the specified loci of the target creature. 'type' is 0 for receptor loci and 1 for emitter loci.
 See genetics documentation for details of thei parameters.
*/
void v_LOCI(caosVM* vm) {
	VM_PARAM_INTEGER(id)
	VM_PARAM_INTEGER(tissue)
	VM_PARAM_INTEGER(organ)
	VM_PARAM_INTEGER(type)

	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);

	float* f = c->getLocusPointer(!type, organ, tissue, id);
	THROW_IFNOT(f);
	vm->result.setFloat(*f);
}

/**
 TAGE (integer)
 %status maybe

 Returns age of target creature, in ticks. Only counts ticks since it was BORN.
*/
void v_TAGE(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	vm->result.setInt(c->getAge());
}

/**
 ORGN (integer)
 %status maybe

 Return the number of organs the target creature has.
*/
void v_ORGN(caosVM* vm) {
	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);
	vm->result.setInt(c->noOrgans());
}

/**
 ORGF (float) organ (integer) value (integer)
 %status maybe

 Return some data about the specified organ (numbered starting at zero) of the target creature.

 value should be one of the following types of data:
 0: clock rate
 1: short term life force as a proportion of the initial life force
 2: repair rate
 3: injury to apply
 4: initial life force
 5: short term life force
 6: long term life force
 7: damage rate
 8: energy cost
 9: atp damage coefficient

 Returns -1 if the specified organ or value is invalid.
*/
void v_ORGF(caosVM* vm) {
	VM_PARAM_INTEGER(value)
	VM_PARAM_INTEGER(organ)

	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);
	if (organ < 0 || (unsigned int)organ >= c->noOrgans()) {
		vm->result.setFloat(-1.0f);
		return;
	}

	std::shared_ptr<c2eOrgan> o = c->getOrgan(organ);

	switch (value) {
		case 0: vm->result.setFloat(o->getClockRate()); break;
		case 1: vm->result.setFloat(o->getShortTermLifeforce() / o->getInitialLifeforce()); break;
		case 2: vm->result.setFloat(o->getRepairRate()); break;
		case 3: vm->result.setFloat(o->getInjuryToApply()); break;
		case 4: vm->result.setFloat(o->getInitialLifeforce()); break;
		case 5: vm->result.setFloat(o->getShortTermLifeforce()); break;
		case 6: vm->result.setFloat(o->getLongTermLifeforce()); break;
		case 7: vm->result.setFloat(o->getDamageRate()); break;
		case 8: vm->result.setFloat(o->getEnergyCost()); break;
		case 9: vm->result.setFloat(o->getATPDamageCoefficient()); break;
		default: vm->result.setFloat(-1.0f); break;
	}
}

/**
 ORGI (integer) organ (integer) value (integer)
 %status maybe

 Returns a count of receptors (value 0), emitters (value 1) or reactions (value 2) in the specified organ (numbered starting at zero) of the target creature.
 
 Returns -1 if the specified organ or value is invalid.
*/
void v_ORGI(caosVM* vm) {
	VM_PARAM_INTEGER(value)
	VM_PARAM_INTEGER(organ)

	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);
	if (organ < 0 || (unsigned int)organ >= c->noOrgans()) {
		vm->result.setFloat(-1.0f);
		return;
	}

	std::shared_ptr<c2eOrgan> o = c->getOrgan(organ);

	switch (value) {
		case 0: vm->result.setInt(o->getReceptorCount()); break;
		case 1: vm->result.setInt(o->getEmitterCount()); break;
		case 2: vm->result.setInt(o->getReactionCount()); break;
		default: vm->result.setFloat(-1.0f); break;
	}
}

/**
 SOUL (command) part (integer) on (integer)
 %status stub
*/
void c_SOUL(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(on)
	VM_PARAM_INTEGER(part)
	THROW_IFNOT(part >= 0 && part <= 8);

	vm->getTargCreature();
	// TODO
}

/**
 SOUL (integer) part (integer)
 %status stub
*/
void v_SOUL(caosVM* vm) {
	VM_PARAM_INTEGER(part)
	THROW_IFNOT(part >= 0 && part <= 8);

	vm->getTargCreature();
	vm->result.setInt(1); // TODO
}

/**
 DECN (integer)
 %status maybe
*/
void v_DECN(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	vm->result.setInt(c->getDecisionId());
}

/**
 ATTN (integer)
 %status maybe
 
 Returns the current focus of attention id.
*/
void v_ATTN(caosVM* vm) {
	vm->result.setInt(vm->getTargCreature()->getAttentionId());
	// TODO: should this be targ? or ownr like C1?
}

static Creature* getOwnerCreature(caosVM* vm) {
	valid_agent(vm->owner);
	CreatureAgent* c = dynamic_cast<CreatureAgent*>(vm->owner.get());
	THROW_IFNOT(c);
	return c->getCreature();
}

/**
 ATTN (integer)
 %status maybe
 %variants c1
 
 IT - obj that OWNR creature is attending to (may be NULL)
 NOTE: only OWNR's IT can be determined, not TARG's
*/
void v_ATTN_c1(caosVM* vm) {
	vm->result.setInt(getOwnerCreature(vm)->getAttentionId());
}

/**
 TOUC (command)
 %status stub
 %variants c1 c2 cv c3
*/
void c_TOUC(caosVM* vm) {
	vm->getTargCreature();
	// TODO
}

/**
 FORF (command) creature (agent)
 %status stub
*/
void c_FORF(caosVM* vm) {
	VM_PARAM_VALIDAGENT(creature)

	vm->getTargCreature();
	// TODO: do we handle pointer as well?
	Creature* learn = dynamic_cast<Creature*>(creature.get());
	if (!learn)
		return;

	// TODO
}

/**
 WALK (command)
 %status maybe
 %variants c1 c2 cv c3
*/
void c_WALK(caosVM* vm) {
	CreatureAgent* c = vm->getTargCreatureAgent();
	c->startWalking();
}

/**
 DONE (command)
 %status stub
 %variants c1 c2 cv c3
*/
void c_DONE(caosVM* vm) {
	vm->getTargCreature();
	// TODO
}

/**
 INS# (integer)
 %status maybe
 %variants c2

 The number of instincts the target creature has still to take in (C2:
 they are taken in the egg and during sleep). Used by the Bedtime COB to
 tell when a sleeper has finished.
*/
void v_INSN(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	vm->result.setInt(c ? (int)c->getNoUnprocessedInstincts() : 0);
}

/**
 LCUS (integer) type (integer) organ (integer) tissue (integer) locus (integer)
 %status maybe
 %variants c2

 The value of one of the target creature's loci, numbered as C2's DDE: LCUS
 (type 0 receptor, 1 emitter; organ 0 brain, 1 creature; tissue a lobe or
 body system; the locus within it). 0 for a locus it does not have. Used as
 a value by C2 COBs (BunchaStuff's Grendel Scarey Mask).
*/
void v_LCUS(caosVM* vm) {
	VM_PARAM_INTEGER(locus)
	VM_PARAM_INTEGER(tissue)
	VM_PARAM_INTEGER(organ)
	VM_PARAM_INTEGER(type)

	vm->result.setInt(0);
	auto* oc = dynamic_cast<oldCreature*>(vm->getTargCreature());
	if (!oc || organ < 0 || organ > 255 || tissue < 0 || tissue > 255 || locus < 0 || locus > 255)
		return;
	if (unsigned char* p = oc->getLocusPointer(type == 0, (unsigned char)organ, (unsigned char)tissue, (unsigned char)locus))
		vm->result.setInt(*p);
}

/**
 SAYN (command)
 %status maybe
 %variants c1 c2 cv c3

 The target creature says its strongest need. In C1/C2 (openc2e's own,
 under a mind): worded for its age, in a speech bubble and its voice
 (CreatureSpeech); the world's "express need" action ends with SAYN.
*/
void c_SAYN(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	if (auto* oc = dynamic_cast<oldCreature*>(c))
		oc->sayNeed();
}

/**
 IMPT (command) nudge (integer)
 %status stub
 %variants c1 c2
*/
void c_IMPT(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(nudge)

	// TODO: check for creature targ?
	// TODO
}

/**
 AIM: (command) actionno (integer)
 %status stub
 %variants c1 c2
*/
void c_AIM(caosVM* vm){
	VM_PARAM_INTEGER_UNUSED(actionno)

	// TODO: check for creature targ? who knows?
	// TODO
}

/**
 BABY (variable)
 %status maybe
 %variants c1 c2
*/
CAOS_LVALUE_TARG_SIMPLE(BABY, vm->targ->babymoniker) // TODO

	/**
 SNEZ (command)
 %status maybe
 %variants c1 c2

 The target creature sneezes: creatures in front of it may catch its
 illnesses (C2: Disease.h; owner, 2026-11-21).
*/
void c_SNEZ(caosVM* vm) {
	vm->getTargCreature();
	Disease::sneeze(vm->targ.get());
}

/**
 DRIV (integer) drive (integer)
 %status maybe
 %variants c1 c2

 Returns the value for the specified drive of the target creature.
*/
void v_DRIV_c1(caosVM* vm) {
	VM_PARAM_INTEGER(drive)
	THROW_IFNOT(drive < 16);

	oldCreature* c = getoldCreature(vm->targ.get());
	// TODO: c2 support
	THROW_IFNOT(c);

	vm->result.setInt(c->getDrive(drive));
}

/**
 DREA (command) max (integer)
 %status stub
 %variants c1 c2

 Start dreaming and process (at most?) max instincts.
*/
void c_DREA_c1(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(max)

	oldCreature* c = getoldCreature(vm->targ.get());
	THROW_IFNOT(c);

	// TODO
}

/**
 BRED (integer) part (integer)
 %status stub
*/
void v_BRED(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(part)

	vm->result.setInt(0); // TODO
}

/**
 BVAR (integer)
 %status stub
*/
void v_BVAR(caosVM* vm) {
	vm->result.setInt(0); // TODO
}

/**
 EXPR (command) index (integer) ticks (integer)
 %status stub
*/
void c_EXPR(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(ticks)
	VM_PARAM_INTEGER_UNUSED(index)

	// TODO
}

/**
 EXPR (integer)
 %status stub
*/
void v_EXPR(caosVM* vm) {
	vm->result.setInt(0); // TODO
}

/**
 TNTC (command) tintindex (integer) part (integer) commit (integer)
 %status stub
*/
void c_TNTC(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(commit)
	VM_PARAM_INTEGER_UNUSED(part)
	VM_PARAM_INTEGER_UNUSED(tintindex)

	// TODO
}

/**
 INJR (command) organ (integer) amount (integer)
 %status maybe
 %variants c2
*/
void c_INJR(caosVM* vm) {
	VM_PARAM_INTEGER(amount)
	VM_PARAM_INTEGER(organ)

	valid_agent(vm->targ);
	// C2: injures one organ (-1: one at random) by amount (0-255, a share of
	// its full life force). The game's scripts use it for a slap from the
	// hand (10), hard landings (up to 150) and an object that strikes
	// creatures; it did nothing before (2026-11-12)
	auto* ca = dynamic_cast<CreatureAgent*>(vm->targ.get());
	auto* c = ca ? dynamic_cast<c2Creature*>(ca->getCreature()) : nullptr;
	if (!c || !c->isAlive() || amount <= 0)
		return;
	auto& organs = c->getOrgans();
	if (organs.empty())
		return;
	int i = organ < 0 ? rand_int32(0, (int)organs.size() - 1) : organ;
	if (i >= (int)organs.size())
		return;
	organs[i]->applyInjury((float)std::min(amount, 255));
}

/**
 SAY$ (command) string (string)
 %status maybe
 %variants c1 c2

 The target creature says the string, in a speech bubble and its voice; no
 word is sent to the creatures in earshot (C2 CAOS guide). Used by COBs
 (the Holo Doc's "Elf needs food!", the Flower Pack's "pretty flower").
*/
void c_SAY(caosVM* vm) {
	VM_PARAM_STRING(string)

	valid_agent(vm->targ);
	Creature* c = vm->getTargCreature();
	if (!c || string.empty())
		return;
	Utterance u;
	u.shown = string;
	u.spoken = string;
	creatureSays(vm->targ.get(), (int)c->getStage(), u);
}

/**
 SAY# (command) word (integer)
 %status maybe
 %variants c1 c2

 The target creature says word number `word` of the game's word list, in a
 speech bubble and its voice (as it knows the word, or in its own tongue),
 and the creatures in earshot hear it (C2 CAOS guide: "send that word as a
 signal message to creatures in earshot").
*/
void c_SAYNUM(caosVM* vm) {
	VM_PARAM_INTEGER(word)

	valid_agent(vm->targ);
	Creature* c = vm->getTargCreature();
	if (!c)
		return;
	Utterance u = wordUtterance(vm->targ.get(), dynamic_cast<oldCreature*>(c), word);
	if (u.shown.empty())
		return;
	creatureSays(vm->targ.get(), (int)c->getStage(), u);
}

/**
 TRIG (command) lobe (integer) cell (integer) amount (integer)
 %status stub
 %variants c2
*/
void c_TRIG(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(amount)
	VM_PARAM_INTEGER_UNUSED(cell)
	VM_PARAM_INTEGER_UNUSED(lobe)

	valid_agent(vm->targ);
	// TODO
}

/**
 MONK (integer)
 %status maybe
 %variants c2

 Returns the target creature's moniker as an integer (its four letters, as
 NEW: GENE and NEW: CREA take it).
*/
void v_MONK(caosVM* vm) {
	oldCreature* c = getoldCreature(vm->targ.get());
	THROW_IFNOT(c);

	vm->result.setInt(genomeNameToInt(world.history->findMoniker(c->getGenome())));
}

/**
 MOTR (command) enable (integer)
 %status stub
*/
void c_MOTR(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(enable)

	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);

	// TODO
}

/**
 MOTR (integer)
 %status stub
*/
void v_MOTR(caosVM* vm) {
	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);

	vm->result.setInt(1); // TODO
}

/**
 MIND (command) enable (integer)
 %status stub
*/
void c_MIND(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(enable)

	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);

	// TODO
}

/**
 MIND (integer)
 %status stub
*/
void v_MIND(caosVM* vm) {
	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);

	vm->result.setInt(1); // TODO
}

/**
 STEP (command) faculty (integer)
 %status stub
*/
void c_STEP(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(faculty)

	c2eCreature* c = getc2eCreature(vm->targ.get());
	THROW_IFNOT(c);

	// TODO
}

/**
 SEEN (agent) category (integer)
 %status maybe
*/
void v_SEEN(caosVM* vm) {
	VM_PARAM_INTEGER(category)

	Creature* c = vm->getTargCreature();
	THROW_IFNOT(c);

	THROW_IFNOT(category >= 0);
	THROW_IFNOT((unsigned int)category < c->getNoCategories());

	vm->result.setAgent(c->getChosenAgentForCategory(category));
}

/**
 DOIN (command) noinstincts (integer)
 %status stub

 Make the target creature process the specified number of instincts.
*/
void c_DOIN(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(noinstincts)

	Creature* c = vm->getTargCreature();
	THROW_IFNOT(c);

	// TODO
}

/**
 INS# (integer)
 %status maybe

 Return the number of unprocessed instincts left in the instinct queue for the target creature.
*/
void v_INS(caosVM* vm) {
	Creature* c = vm->getTargCreature();
	THROW_IFNOT(c);

	vm->result.setInt(c->getNoUnprocessedInstincts());
}

// clothes

/**
 BODY (command) set (integer) layer (integer)
 %status stub
*/
void c_BODY(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(layer)
	VM_PARAM_INTEGER_UNUSED(set)

	// TODO
}

/**
 BODY (integer) part (integer)
 %status stub
*/
void v_BODY(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(part)

	vm->result.setInt(-1); // TODO
}

/**
 DYED (command) part (integer) overlay (integer) set (integer) layer (integer)
 %status stub
*/
void c_DYED(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(layer)
	VM_PARAM_INTEGER_UNUSED(set)
	VM_PARAM_INTEGER_UNUSED(overlay)
	VM_PARAM_INTEGER_UNUSED(part)

	// TODO
}

/**
 HAIR (command) ruffleness (integer)
 %status stub
*/
void c_HAIR(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(ruffleness)

	// TODO
}

/**
 NUDE (command)
 %status stub
*/
void c_NUDE(caosVM*) {
	// TODO
}

/**
 RSET (command)
 %status stub
*/
void c_RSET(caosVM*) {
	// TODO
}

/**
 STRE (command)
 %status stub
*/
void c_STRE(caosVM*) {
	// TODO
}

/**
 SWAP (command) variant (integer) part (integer) commit (integer)
 %status stub
*/
void c_SWAP(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(commit)
	VM_PARAM_INTEGER_UNUSED(part)
	VM_PARAM_INTEGER_UNUSED(variant)

	// TODO
}

/**
 WEAR (command) part (integer) set (integer) layer (integer)
 %status stub
*/
void c_WEAR(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(layer)
	VM_PARAM_INTEGER_UNUSED(set)
	VM_PARAM_INTEGER_UNUSED(part)

	// TODO
}

/**
 WEAR (integer) part (integer) layer (integer)
 %status stub
*/
void v_WEAR(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(layer)
	VM_PARAM_INTEGER_UNUSED(part)

	vm->result.setInt(-1); // TODO
}

/**
 TNTO (command) tintindex (integer) part (integer) set (integer) layer (integer)
 %status stub
*/
void c_TNTO(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(layer)
	VM_PARAM_INTEGER_UNUSED(set)
	VM_PARAM_INTEGER_UNUSED(part)
	VM_PARAM_INTEGER_UNUSED(tintindex)

	// TODO
}

// attachment locations

/**
 DFTX (float)
 %status stub
*/
void v_DFTX(caosVM* vm) {
	vm->result.setFloat(0); // TODO
}

/**
 DFTY (float)
 %status stub
*/
void v_DFTY(caosVM* vm) {
	vm->result.setFloat(0); // TODO
}

/**
 UFTX (float)
 %status stub
*/
void v_UFTX(caosVM* vm) {
	vm->result.setFloat(0); // TODO
}

/**
 UFTY (float)
 %status stub
*/
void v_UFTY(caosVM* vm) {
	vm->result.setFloat(0); // TODO
}

/**
 HEDX (float) index (integer)
 %status stub
*/
void v_HEDX(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(index)

	vm->result.setFloat(0); // TODO
}

/**
 HEDY (float) index (integer)
 %status stub
*/
void v_HEDY(caosVM* vm) {
	VM_PARAM_INTEGER_UNUSED(index)

	vm->result.setFloat(0); // TODO
}

/**
 MTHX (float)
 %status stub
*/
void v_MTHX(caosVM* vm) {
	vm->result.setFloat(0); // TODO
}

/**
 MTHY (float)
 %status stub
*/
void v_MTHY(caosVM* vm) {
	vm->result.setFloat(0); // TODO
}
