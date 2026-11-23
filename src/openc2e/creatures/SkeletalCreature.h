/*
 *  SkeletalCreature.h
 *  openc2e
 *
 *  Created by Alyssa Milburn on Thu 10 Mar 2005.
 *  Copyright (c) 2005-2006 Alyssa Milburn. All rights reserved.
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

#pragma once

#include "AnimatablePart.h"
#include "CreatureAgent.h"
#include "fileformats/attFile.h"
#include "fileformats/genomeFile.h"

#include <cassert>
#include <memory>

class creaturesImage;

class SkeletalCreature : public Agent, public CreatureAgent {
  protected:
	class SkeletonPart* skeleton;

	unsigned int ticks; // TODO: unnecessary?

	unsigned int posedirection;
	unsigned int pose[17];
	unsigned int facialexpression;
	unsigned int pregnancy;
	bool eyesclosed;
	// blinking: drawn with the eyes shut for a moment every few seconds
	// (only the head picture; the eyes-closed state for sleep is apart)
	int blinkwait = 40, blinkticks = 0;

	bool calculated;
	int oldfootx, oldfooty;
	int lastgoodfootx, lastgoodfooty; // TODO: sucky code
	// walks pushed back by a wall (C2): how many so far, and the way it was
	// walking the last time (10 west, 11 east), for the mind
	unsigned int walledCount = 0;
	int walledDir = 0;
	int fallStuck = 0; // ticks falling without moving (part of its box in rock): eased sideways
	// Falls of its own (owner, 2026-11-13: creatures should learn not to walk
	// off ledges): a fall it walked into, not one from the hand, a vehicle or
	// a script, hurts on landing by the height fallen (dry landings only)
	bool fallTracking = false, fallOwn = false;
	float fallTopY = 0;
	unsigned int lastHeldTick = 0, lastScriptMoveTick = 0;
	void landed(float height);
	bool downfoot_left;
	std::shared_ptr<class Room> downfootroom;

	creatureAppearanceGene* appearancegenes[6];
	std::map<unsigned int, creaturePoseGene*> posegenes;
	std::map<unsigned int, creatureGaitGene*> gaitgenes;

	std::shared_ptr<creaturesImage> images[17];
	float partk[17] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}; // each part's growth scale (stage growth)
	bool oldWithoutArt = false; // old, and its breed has no old-age pictures (a lighter grey; skeletonInit)
	attFile att[17];

	int width, height, adjustx, adjusty;
	int drawnwidth = 50, drawnheight = 100;
	int partx[17], party[17];

	// Smooth movement (SmoothMovement.cpp): drawn between poses. C2 only, and
	// drawing only: pose[], partx[] and the rest are the creature's as before.
	struct Glide {
		bool have = false; // `to` and `dir` are its latest pose
		bool pending = false; // between begin and end of a pose change
		float from[14] = {}; // each part's pose as drawn when the change began
		unsigned int to[14] = {}, dir = 0;
		float start = 0, length = 0; // in world ticks (length 0: not gliding)
		unsigned int changed = 0; // the tick of the latest change
		int foot = 11; // the foot that stood on the ground
		float footx = 0, footy = 0; // and where it was drawn
		bool settled = true; // off is known
		float offx = 0, offy = 0; // how far that foot was drawn from where it is now
		float evenFrom = 0, evenTo = 0; // how far across it is drawn to even out its walk
		float originx = 0, originy = 0; // the creature's x and y as drawn when the change began
	} glide;
	// its walk's cycle: how far to draw the body, at each pose, from where
	// the pose puts it, for an even speed
	struct WalkCycle {
		creatureGaitGene* gene = nullptr;
		unsigned int dir = 0;
		int size = 0;
		float even[8] = {};
	} walkCycle;
	float evenWalk();
	float evenNow() const;
	void glideBegin(); // the pose has changed, the skeleton has not yet
	void glideEnd(); // the skeleton is the new pose's
	void glideSettle(); // the change's other effects (the foot snapped to its floor) are done
	float glideShare() const; // how far through it is, 0-1 (1: not gliding)
	void attachmentBetween(int part, int id, float p, float& ox, float& oy) const;
	float turnBetween(int part, float p, int& frame) const;
	bool drawnSkeleton(float poses[14], float jx[14], float jy[14], float& bx, float& by) const;
	bool renderGliding(class RenderTarget* renderer, int xoffset, int yoffset);

	std::string dataString(unsigned int _stage, bool sprite, unsigned int dataspecies, unsigned int databreed);

	unsigned int gaiti = 0; // (was left unset: a fresh or loaded creature read its gait gene past its 8 poses once)
	float paceCredit = 0; // C2: walking pace by life stage (stagePace)
	int approachUnid = -1, approachTurns = 0; // approaching: what, and how often it has turned back for it lately
	unsigned int approachTurnedAt = 0;
	static float stagePace(int stage);
	float nextStepIn = 0; // while a walk's step is taken: ticks until its next (0: not a step)
	bool walkSteps(float share);
	void stepTiredness();
	creatureGaitGene* gaitgene;

	std::pair<int, int> getCarryPoint();
	bool carriedFront = false; // as last sorted for drawing

	void physicsTick();
	void snapDownFoot();

	void gaitTick();

	void processGenes();

	creatureAppearanceGene* appearanceGeneForPart(char p);
	std::shared_ptr<creaturesImage> tintBodySprite(std::shared_ptr<creaturesImage>);

	Agent* getAgent() { return this; }

  public:
	SkeletalCreature(unsigned char _family);
	virtual ~SkeletalCreature();

	void render(RenderTarget* renderer, int xoffset, int yoffset);
	// smooth movement: on or off (View menu; off in C1 and C3)
	static bool smoothMovement();
	static void setSmoothMovement(bool on);
	// how far it is drawn now from where it is: its corner (what floats on
	// it), its head, the thing it carries; false: where it is
	bool drawnShift(float& dx, float& dy) const;
	bool drawnHeadShift(float& dx, float& dy) const;
	bool drawnCarryShift(float& dx, float& dy) const;
	// its neck as drawn, in the world, for a camera that follows it
	bool drawnNeck(float& nx, float& ny) const;
	virtual int handleClick(float, float);
	int partUnder(float wx, float wy); // the body part drawn at a world point, or -1
	static bool bodyOrLeg(int part); // the body, a thigh, shin or foot (spank, push)

	void skeletonInit();
	void recalculateSkeleton();

	unsigned int getPose(unsigned int i) { return pose[i]; }
	// everything posing changes, to put a creature back exactly as it was
	// after posing it for a picture (portraits)
	struct PoseMemo {
		unsigned int pose[17], posedirection, facialexpression;
		bool eyesclosed, calculated, downfoot_left;
		int blinkticks, oldfootx, oldfooty, lastgoodfootx, lastgoodfooty;
		int width, height, adjustx, adjusty, partx[17], party[17];
		float x, y;
		Glide glide;
	};
	PoseMemo memoPose() const;
	void restorePose(const PoseMemo& m);
	// poses for a picture without moving: the feet are not kept in place
	void poseInPlace(const std::string& s) {
		calculated = false;
		setPose(s);
	}
	void setPose(unsigned int p);
	void setPose(std::string s);
	void setPoseGene(unsigned int p);
	void setGaitGene(unsigned int g);

	void tick();

	unsigned int getPregnancy() { return pregnancy; }
	void setPregnancy(unsigned int p) {
		assert(p < 4);
		pregnancy = p;
	}
	bool getEyesClosed() { return eyesclosed; }
	void setEyesClosed(bool e) { eyesclosed = e; }
	unsigned int getFacialExpression() { return facialexpression; }
	void setFacialExpression(unsigned int f) {
		assert(f < 6);
		facialexpression = f;
	}
	unsigned int getSkelWidth() { return width; }
	// how wide its picture is in this pose, from its x (its box is a fixed
	// 50 wide; a big grendel is drawn much wider)
	int drawnWidth() const { return drawnwidth; }
	// developer aid: its parts' pictures, frames and growth scales
	std::string describeParts();
	// walks pushed back by a wall so far, and the way of the last (10 west, 11 east)
	unsigned int wallBumps() const { return walledCount; }
	int wallBumpDir() const { return walledDir; }
	unsigned int scriptMoveTick() const { return lastScriptMoveTick; }
	float tickstartx = 0, tickstarty = 0; // where it was when this tick began (smooth drawing of scripted moves)
	unsigned int getSkelHeight() { return height; }

	int attachmentX(unsigned int part, unsigned int id);
	// the middle of the top of the head picture, in world coordinates (for
	// the halo); false when there is no head picture
	bool headTop(float& hx, float& hy);
	// where a breath bubble's picture (bubs, 32 x 48) goes, in the world
	// (owner, 2026-11-11): 3 px outside the head's visible edge on the side
	// it faces (right facing right or toward the viewer, left facing left
	// or away), level with its snout tip; looking up, as high as the mouth
	// point or a share of the head's height from its top if higher
	bool bubbleSpot(float& bx, float& by);
	// its head's pictures (part a, slot 1; frame 8 faces the viewer), for a thought
	// bubble's picture of it
	std::shared_ptr<creaturesImage> headSprite() { return images[1]; }
	// the push hand: slide along the ground for a few ticks (dir 1 east,
	// -1 west), fast at first, easing off; walls and floors as for walking
	void shove(int dir) {
		shovedir = dir;
		shoveticks = 10;
	}
	int shovedir = 0, shoveticks = 0;
	int attachmentY(unsigned int part, unsigned int id);
	bool isLeftFootDown() { return downfoot_left; }
	bool carriesInFront() const override;
	// for the creature's senses: the room its down foot stands in (null while
	// falling, carried or riding) and the foot's x, wrapped into the map of a
	// wrapping world
	std::shared_ptr<class Room> footRoom() const { return downfootroom; }
	// a script moved it (MVTO, MVBY): its foot room is found afresh where it
	// now stands, not judged as a step from the room it stood in
	void scriptMoved();
	// its latest fall of its own: how far, how much it hurt (0-1), where it
	// stepped off (its middle and feet then) and when (0: none yet)
	struct Fall {
		float height = 0, hurt = 0, fromX = 0, fromY = 0;
		unsigned int tick = 0;
	} lastFall;
	// the way its body faces now, in DIRN's terms (0 away, 1 toward the
	// viewer, 2 right, 3 left), from its current pose
	unsigned int facing() const { static const unsigned int dirn[4] = {2, 3, 1, 0}; return posedirection < 4 ? dirn[posedirection] : 2; }
	float footX();
	// the foot's height (the foot on the ground), in world terms
	float footY() { return y + attachmentY(downfoot_left ? 11 : 12, 1); }

	CompoundPart* part(unsigned int id);
	void setZOrder(unsigned int plane);

	void finishInit();
	void creatureAged();

	std::string getFaceSpriteName();
	unsigned int getFaceSpriteFrame();
};

class SkeletonPart : public AnimatablePart {
	friend class SkeletalCreature;

  protected:
	SkeletonPart(SkeletalCreature* p);

  public:
	void tick();
	void partRender(class RenderTarget* renderer, int xoffset, int yoffset);
	unsigned int getWidth() { return ((SkeletalCreature*)parent)->getSkelWidth(); }
	unsigned int getHeight() { return ((SkeletalCreature*)parent)->getSkelHeight(); }
	void setPose(unsigned int p);
	void setFrameNo(unsigned int f);
};

// C2: whether a breed is drawn at one age only and openc2e grows its one
// set of pictures to fit each life stage (OPENC2E_STAGE_SCALE; grendels and
// ettins, norns with =norns). The Hatchery credits that work.
bool BreedGrownForEveryStage(int species, int breed);
bool StageGrowthForEveryBreed();
int BreedOneSetLooksAge(int species, int breed);

/* vim: set noet: */
