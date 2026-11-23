/*
 *  PointerAgent.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Tue Aug 30 2005.
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

#include "PointerAgent.h"
#include "common/devenv.h"

#include "Camera.h"
#include "DullPart.h"
#include "SpritePart.h"
#include "Engine.h"

#include <cmath>
#include "Map.h"
#include "MetaRoom.h"
#include "Room.h"
#include "World.h"
#include "caosVM.h"
#include "creatures/CreatureAgent.h"
#include "creatures/oldCreature.h"
#include "Scriptorium.h"
#include "creatures/SkeletalCreature.h"

#include <cassert>
#include <climits>
#include <fmt/core.h>
#include <memory>

// TODO: change imagecount?
PointerAgent::PointerAgent(std::string spritefile)
	: SimpleAgent(2, 1, 1, INT_MAX, spritefile, 0, 0) {
	name = "hand";
	handle_events = true;
	holdingWire = 0;
	wireOriginID = 0;

	overlay = NULL;
	overlayTimer = 0;

	hotspotx = 0;
	hotspoty = 0;
}

// the push hand: a short hop sideways (dir 1 east, -1 west)
void shoveCreature(Agent* a, int dir) {
	if (auto* sc = dynamic_cast<SkeletalCreature*>(a))
		sc->shove(dir);
}

// The hand's shove, as real C2 plays it (watched 2026-11-11, poses read
// over DDE about six times a second while the owner pushed a grendel): a
// shove west, the hand pressing on the creature's right, is frames 8, 9, 8
// of the hand sprite, about half a second, then the usual pointer. A shove
// east has its own drawing: frames 10, 11, 12, the hand turning up to press
// on the creature's left (owner, 2026-11-13: C2's push hand has different
// animations for pushing right and left). (The DDE readings showed frames
// 1-2 for a shove east, the finger's click, and for a while a shove east
// here was the west shove mirrored; frames 10-12 are the hand sprite's
// pressing-east drawings, used nowhere else.)
void PointerAgent::playShove(int dir) {
	auto* sp = dynamic_cast<SpritePart*>(part(0));
	if (engine.version != 2 || !sp)
		return;
	if (pushAnimAt >= pushAnim.size())
		prePushPose = sp->getPose();
	if (dir > 0)
		pushAnim = {10, 10, 11, 11, 12, 12, 11, 11};
	else
		pushAnim = {8, 8, 9, 9, 8, 8};
	draw_mirrored = false;
	pushAnimAt = 0;
	sp->setPose(pushAnim[0]);
}

void PointerAgent::finishInit() {
	Agent::finishInit();

	// float relative to main camera
	// TODO: this seems hard-coded in DS (pointer attr is 0)
	attr = attr & 32;
	floatSetup();
}

void PointerAgent::carry(AgentRef a) {
	Agent::carry(a);

	int eve;
	if (engine.version < 3)
		eve = 53;
	else
		eve = 104;
	firePointerScript(eve, a); // Pointer Pickup
}

void PointerAgent::drop(AgentRef a) {
	Agent::drop(a);

	// Hand-feeding (C2; owner, 2026-11-11): food dropped on an unconscious
	// creature is eaten for it: the food's own eat script (event 12) runs
	// as if the creature had eaten it, so a starving creature can be brought
	// round
	if (engine.version == 2 && a) {
		Agent* food = a.get();
		bool edible = world.scriptorium->getScript(food->family, food->genus, food->species, 12) != nullptr;
		if (edible)
			for (auto& other : world.agents) {
				auto* ca = other ? dynamic_cast<CreatureAgent*>(other.get()) : nullptr;
				auto* oc = ca ? dynamic_cast<oldCreature*>(ca->getCreature()) : nullptr;
				if (!oc || !oc->isUnconscious() || !oc->isAlive())
					continue;
				bool overlap = food->x < other->x + other->getWidth() && food->x + food->getWidth() > other->x &&
							   food->y < other->y + other->getHeight() && food->y + food->getHeight() > other->y;
				if (overlap) {
					food->queueScript(12, AgentRef(other));
					fmt::print("* Hand-fed {} to an unconscious creature\n", food->identify());
					break;
				}
			}
	}

	int eve;
	if (engine.version < 3)
		eve = 54;
	else
		eve = 105;
	firePointerScript(eve, a); // Pointer Drop
}

// TODO: this should have a queueScript equiv too
void PointerAgent::firePointerScript(unsigned short event, Agent* src) {
	assert(src); // TODO: I /think/ this should only be called by the engine..
	std::shared_ptr<script> s = src->findScript(event);
	if (!s && engine.version < 3) { // TODO: are we sure this doesn't apply to c2e?
		s = findScript(event); // TODO: we should make sure this actually belongs to the pointer agent and isn't a fallback, maybe
	}
	if (!s)
		return;
	if (!vm)
		vm = world.getVM(this);
	if (vm->fireScript(s, false, src)) { // TODO: should FROM be src?
		vm->setTarg(this);
		zotstack();
	}
}

void PointerAgent::physicsTick() {
	// TODO: this is a hack, which does nothing, because we set a velocity in main() but also move the cursor manually
}

void PointerAgent::tick() {
	// the Pointer toolbar's invisible hand (C1/C2): drawn see-through, as C2
	// draws it, while creatures can't see it
	if (engine.version <= 2)
		if (auto* sp = dynamic_cast<SpritePart*>(part(0)))
			sp->setStippled(world.handInvisible);
	if (!paused) {
		int x = pointerX(), y = pointerY();
		CompoundPart* a = world.partAt(x, y);
		Agent* parent = 0;

		// work out what the old part was, if it hasn't disappeared from under us
		// TODO: what if the old part was replaced by a new part with identical id? :(
		CompoundPart* oldpart = 0;
		if (agent_under_pointer)
			oldpart = agent_under_pointer->part(part_under_pointer);

		// handle mouse in/out events
		if (oldpart != a) {
			if (oldpart)
				oldpart->mouseOut();
			if (a)
				a->mouseIn();
		}

		// update our memory of what part we're over
		if (a) {
			part_under_pointer = a->id;
			parent = a->getParent();
		}
		agent_under_pointer = parent;

		// The push hand (C2; owner, 2026-11-11). As watched in real C2:
		// while the Pointer toolbar's push is on, the hand stays the usual
		// pointer, and over a creature it shows which way a click would
		// shove it, with the sprite's arrows (frame 14, east, to the hand's
		// right; frame 13, west, to its left), as C2 shows the angel and
		// devil. Overlay codes -4 (east) and -3 (west). A click plays the
		// shove (playShove).
		bool pushing = engine.version == 2 && world.handPush && !carrying;
		if (pushAnimAt < pushAnim.size()) {
			if (auto* sp = dynamic_cast<SpritePart*>(part(0)))
				sp->setPose(++pushAnimAt < pushAnim.size() ? pushAnim[pushAnimAt] : prePushPose);
			if (pushAnimAt >= pushAnim.size())
				draw_mirrored = false;
		}
		if (pushing) {
			int want = 0;
			// (on the body and legs only: owner, 2026-11-15)
			auto* sk = dynamic_cast<SkeletalCreature*>(parent);
			if (a && parent && dynamic_cast<CreatureAgent*>(parent) && !parent->carriedby && !parent->invehicle && (!sk || SkeletalCreature::bodyOrLeg(sk->partUnder(x, y))))
				want = x < parent->x + parent->getWidth() / 2.0f ? -4 : -3;
			// (the arrows are drawn mirrored with the hand during a mirrored
			// shove, so they take the other arrow then)
			if (want != overlayTimer || (want && !overlay) || overlayMirrored != draw_mirrored) {
				if (overlay)
					delete overlay;
				overlay = NULL;
				overlayTimer = 0;
				overlayMirrored = draw_mirrored;
				if (want) {
					overlay = new DullPart(this, 0, "syst", (want == -4) != draw_mirrored ? 14 : 13, want == -4 ? 50 : -28, 14, 1);
					overlayTimer = want;
				}
			}
			SimpleAgent::tick();
			return;
		}

		// overlay magic for c1/c2 - angel, devil, generic overlay
		if (a && parent && dynamic_cast<SkeletalCreature*>(parent)) {
			// the part under the cursor belongs to a SkeletalCreature
			int scriptid = a->handleClick(x - a->x - parent->x, y - a->y - parent->y);
			if (devenv("OPENC2E_WATCH_HAND_SIGN"))
				fmt::print("* HAND SIGN t{} at {},{}: script {} timer {}\n", world.tickcount, x, y, scriptid, overlayTimer);
			if (scriptid == 0) {
				// devil
				if (engine.version == 2) {
					if (overlayTimer != -1) { // dodgy hax. -1 is devil, -2 is angel.
						overlayTimer = -1;
						if (overlay)
							delete overlay;
						overlay = new DullPart(this, 0, "syst", 16, 32, 32, 1);
					}
				} else if (engine.version == 3) {
					if (overlayTimer != 1) {
						queueScript(117, 0, 1);
						overlayTimer = 1;
					}
				}
			} else if (scriptid == 1) {
				// angel
				if (engine.version == 2) {
					if (overlayTimer != -2) {
						overlayTimer = -2;
						if (overlay)
							delete overlay;
						overlay = new DullPart(this, 0, "syst", 15, 32, 32, 1);
					}
				} else if (engine.version == 3) {
					if (overlayTimer != 2) {
						queueScript(117, 0, 2);
						overlayTimer = 2;
					}
				}
			} else {
				// if we're over a creature and not angel/devil, and we had one before,
				// destroy the overlay. Not sure if this case ever gets hit.
				if (engine.version == 2) {
					if (overlayTimer < 0 && overlay) {
						delete overlay;
						overlay = NULL; // (it was left pointing at the deleted part)
						overlayTimer = 0;
					}
				} else if (engine.version == 3) {
					if (overlayTimer != 0) {
						queueScript(117, 0, 0);
						overlayTimer = 0;
					}
				}
			}
		} else {
			if (devenv("OPENC2E_WATCH_HAND_SIGN"))
				fmt::print("* HAND SIGN t{} at {},{}: not over a creature (part {})\n", world.tickcount, x, y, a ? a->getParent()->identify() : std::string("none"));
			if (engine.version == 2) {
				if (overlayTimer > 0) {
					overlayTimer--;
				} else if (overlay) {
					delete overlay;
					overlay = NULL;
					overlayTimer = 0;
				}
			} else if (engine.version == 3) {
				if (overlayTimer != 0) {
					queueScript(117, 0, 0);
					overlayTimer = 0;
				}
			}
		}
	}
	SimpleAgent::tick();
}

void PointerAgent::kill() {
	// pointer agent isn't killable
}

void PointerAgent::setHotspot(int x, int y) {
	hotspotx = x;
	hotspoty = y;
}

void PointerAgent::handleEvent(BackendEvent& event) {
	int x = pointerX(), y = pointerY();

	if (event.type == eventmousemove) {
		// the window shows the world at engine.zoom
		moveTo((int)std::floor(event.x / engine.zoom) + engine.camera->getX() - hotspotx, (int)std::floor(event.y / engine.zoom) + engine.camera->getY() - hotspoty);
		velx = (int)event.xrel * 4;
		vely = (int)event.yrel * 4;

		if (editAgent)
			editAgent->moveTo(pointerX(), pointerY());

		// middle mouse button scrolling
		if (event.button & buttonmiddle) {
			// (C1/C2: which stops following, as the wheel does)
			if (engine.version <= 2 && (event.xrel != 0 || event.yrel != 0))
				engine.camera->trackAgent(AgentRef(), 0, 0, brittle, none);
			engine.camera->moveTo(engine.camera->getX() - (int)std::lround(event.xrel / engine.zoom), engine.camera->getY() - (int)std::lround(event.yrel / engine.zoom), jump);
		}
	} else if (!handle_events) {
		/* mouse move events are (apparently - see eg C3 agent help) still handled with handle_events disabled, but nothing else */
		return;
	} else if (event.type == eventmousebuttondown) {
		// do our custom handling
		if (event.button == buttonleft) {
			CompoundPart* a = world.partAt(x, y, true, false, true);
			// C1/C2: clicking a creature selects it, also where something is
			// drawn over it (its speech bubble, what it carries, the hand's
			// typing bubble when teaching it a word; owner, 2026-11-10: a
			// click with the teach hand should select)
			if (engine.version < 3 && !(a && dynamic_cast<CreatureAgent*>(a->getParent())))
				if (Agent* c = world.creatureAt(x, y))
					world.selectCreature(AgentRef(c).lock());
			if (a) {
				Agent* parent = a->getParent();

				// C1/C2: clicking a creature selects it (the click still goes
				// on to tickle/slap as usual). C3/DS selection is script-driven.
				if (engine.version < 3 && dynamic_cast<CreatureAgent*>(parent))
					world.selectCreature(AgentRef(parent).lock());

				int eve = -1;

				bool foundport = false;
				if (engine.version > 2) {
					for (std::map<unsigned int, std::shared_ptr<OutputPort> >::iterator i = parent->outports.begin();
						 i != parent->outports.end(); i++) {
						// TODO: 4 is a magic number i pulled out of nooooowhere
						if (abs(i->second->x + parent->x - x) < 4 && abs(i->second->y + parent->y - y) < 4) {
							foundport = true;
							if (holdingWire == 2) {
								parent->join(i->first, wireOriginAgent, wireOriginID);
								holdingWire = 0;
								eve = 111;
							} else if (holdingWire == 0) {
								eve = 110;
								holdingWire = 1;
								wireOriginAgent = parent;
								wireOriginID = i->first;
							}
							break;
						}
					}
					if (!foundport) {
						for (std::map<unsigned int, std::shared_ptr<InputPort> >::iterator i = parent->inports.begin();
							 i != parent->inports.end(); i++) {
							// TODO: 4 is a magic number i pulled out of nooooowhere
							if (abs(i->second->x + parent->x - x) < 4 && abs(i->second->y + parent->y - y) < 4) {
								foundport = true;
								if (holdingWire == 1) {
									if (i->second->source) {
										eve = 114;
									} else {
										wireOriginAgent->join(wireOriginID, parent, i->first);
										holdingWire = 0;
										eve = 111;
									}
								} else if (holdingWire == 0) {
									if (i->second->source) {
										eve = 112;
										holdingWire = 1;
										wireOriginAgent = i->second->source;
										wireOriginID = i->second->sourceid;
										i->second->source->outports[i->second->sourceid]->dests.remove(std::pair<AgentRef, unsigned int>(parent, i->first));
										i->second->source.clear();
									} else {
										eve = 110;
										holdingWire = 2;
										wireOriginAgent = parent;
										wireOriginID = i->first;
									}
								}
								break;
							}
						}
					}
				}

				if (!foundport) {
					if (holdingWire) {
						holdingWire = 0;
						eve = 113;
					} else {
						// the Pointer toolbar's push hand (C1/C2): a click on a
						// creature shoves it away from where the hand pressed, a
						// short hop sideways, instead of tickling or slapping it
						if (engine.version < 3 && world.handPush && dynamic_cast<CreatureAgent*>(parent) && !parent->carriedby && !parent->invehicle) {
							// on the body and legs only (owner, 2026-11-15)
							if (auto* sk = dynamic_cast<SkeletalCreature*>(parent); sk && !SkeletalCreature::bodyOrLeg(sk->partUnder(x, y)))
								return;
							int dir = x < parent->x + parent->getWidth() / 2.0f ? 1 : -1;
							shoveCreature(parent, dir);
							playShove(dir);
							return;
						}
						// if the agent isn't paused, tell it to handle a click
						int scriptid = a->handleClick(x - a->x - parent->x, y - a->y - parent->y);
						if (scriptid != -1) {
							// fire the script
							// _p1_ is id of part for button clicks, according to Edynn code
							// TODO: should _p1_ always be set? :)
							if (!parent->paused)
								parent->queueScript(scriptid, world.hand(), (int)a->id);

							// annoyingly queueScript doesn't reliably tell us if it did anything useful.
							// TODO: work out the mess which is queueScript's return values etc
							if (!parent->findScript(scriptid))
								return;

							// fire the associated pointer script too, if necessary
							// TODO: fuzzie has no idea if this code is remotely correct
							if (engine.version < 3) {
								eve = 50;
							} else {
								eve = 101; // Pointer Activate 1
							}
							switch (scriptid) {
								case 0: eve += 2; break; // deactivate
								case 1: break; // activate 1
								case 2: eve += 1; break; // activate 2
								default: return;
							}
						}
					}
				}

				if (eve != -1)
					firePointerScript(eve, a->getParent());
			} else if (engine.version > 2) {
				if (holdingWire) {
					holdingWire = 0;
					queueScript(113, 0);
				} else {
					queueScript(116, 0); // Pointer Clicked Background
				}
			}
		} else if (event.button == buttonright) {
			if (editAgent) {
				editAgent.clear();
				return;
			}

			if (world.paused)
				return; // TODO: wrong?

			// picking up and dropping are implictly handled by the scripts (well,
			// messages) 4 and 5	TODO: check if this is correct behaviour, one issue
			// is that this isn't instant, another is the messages might only be
			// fired in c2e when you use MESG WRIT, in which case we'll need to
			// manually set carrying to NULL and a here, respectively - fuzzie
			if (carrying) {
				if (engine.version == 1) {
					// ensure there's a room to drop into, in C1
					MetaRoom* m = world.map->metaRoomAt(x, y);
					if (!m)
						return;
					std::shared_ptr<Room> r = m->nextFloorFromPoint(x, y);
					if (!r)
						return;

					carrying->queueScript(5, this); // drop

					return;
				}

				bool allowdrop = true;

				if (engine.version == 2) {
					/* check dropstatus for the room we're dropping into */
					MetaRoom* m = world.map->metaRoomAt(carrying->x, carrying->y);
					if (m) {
						std::shared_ptr<Room> r = m->roomAt(carrying->x, carrying->y);
						if (r) {
							if (r->dropstatus == 0)
								allowdrop = false; // Never
							else if (r->dropstatus == 1) { // Above-Floor
								float floory = r->floorYatX(carrying->x);
								if (floory < (carrying->y + carrying->getHeight())) {
									// snap to above the floor
									// TODO: snapping to above the floor isn't very well thought-out (eg, this will drop into vehicles)
									carrying->y = floory - carrying->getHeight();
									// try pushing upwards for a while if we're not validly in the room system
									while (!carrying->validInRoomSystem() && carrying->y - floory + carrying->getHeight() < 50) {
										carrying->y--;
									}
								}
							}
						} else
							allowdrop = false;
					} else
						allowdrop = false;
				}

				/* deny drops which result in agents lying outside the room system */
				if (allowdrop && carrying->suffercollisions()) {
					allowdrop = carrying->validInRoomSystem();
				}

				if (allowdrop) {
					carrying->queueScript(5, this); // drop

					// TODO: is this the correct check?
					if (carrying->sufferphysics() && carrying->suffercollisions()) {
						// TODO: do this in the pointer agent?
						carrying->velx = velx;
						carrying->vely = vely;
					}
				} else {
					if (engine.version == 2) {
						if (overlay)
							delete overlay;
						overlay = new DullPart(this, 0, "syst", 17, 0, 0, 1);
						overlayTimer = 8;

						// play 'deny drop' sound
						playAudio("excl", true, false); // TODO: should this be controlled?
					} else {
						// TODO: drop deny animation/etc for c2e
					}
				}
			} else {
				Agent* a = world.agentAt(x, y, false, true);
				if (a) {
					a->queueScript(4, this); // pickup
				}
			}
		} else if (event.button == buttonmiddle) {
			std::vector<std::shared_ptr<Room> > rooms = world.map->roomsAt(x, y);
			if (rooms.size() > 0)
				fmt::print("Room at cursor is {}\n", rooms[0]->id);
			Agent* a = world.agentAt(x, y, true);
			if (a)
				fmt::print("Agent under mouse is {}\n", a->identify());
			else
				fmt::print("No agent under cursor\n");
		}
	}
}

/* vim: set noet: */
