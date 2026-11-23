/*
 *  CreatureMind.h
 *  openc2e
 *
 *  A narrow, pluggable interface for C1/C2 creature decision-making.
 *
 *  Each brain tick the engine fills a MindPerception (what the creature
 *  sees, its drives and chemistry, stimuli received since the last tick)
 *  and asks the mind for a MindOutput (what to attend to, which action to
 *  take). The world's own creature scripts (event 16 + decision) carry out
 *  the action. Minds never touch engine internals directly.
 *
 *  Select a mind with the --mind command-line option. Without it, the
 *  original oldBrain code path is used unchanged.
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

#include "AgentRef.h"
#include "lifestage.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

// A category of the mind's own, past C2's 40 (owner, 2026-11-21: "care +
// learned medicine"): the volcanic bushes' fruit (2 25 x with OV02 2), which
// gives EDTA and soothes heavy metals' pain. To the engine it is fruit (25);
// the mind sees it apart so what eating it does is learned apart (by the eater
// and by watchers), and carers can bring it on purpose. A choice of it always
// names the fruit itself (MindOutput::attentionAgent).
const int MIND_HEALING_FRUIT = 40;
// Our bushes' rotten fruit (2 25 149; owner, 2026-11-21), seen apart from good
// fruit in the same way, so a creature that eats one learns to leave them.
const int MIND_ROTTEN_FRUIT = 41;

struct SeenAgent {
	int category; // world category index (0-39 for C1/C2)
	AgentRef agent;
	float distance; // between centres, in world pixels
	float x = 0, y = 0; // the agent's centre, in world pixels
	float edible = 0; // scent: how nourishing its eat script is, 0-1 (see edibleScent)
	bool reachable = true; // overlaps the creature's height, so walking can reach it
	bool activatable = true; // its attributes let it be activated (C1/C2 attr 4)
	bool carryable = true; // creatures may pick it up (C1/C2 attr 1)
	// another creature: which one (its moniker), and what this creature
	// feels about it (creatures/Relationships.h); empty for objects
	std::string who;
	float affinity = 0, fear = 0, familiarity = 0;
	// another creature's kind, sex and life stage (genus 1 norn, 2 grendel,
	// 3 ettin; -1 for objects): for remembering where mates were met
	int genus = -1;
	bool female = false;
	int stage = -1;
	// how hungry it looks (its hunger, 0-1: it says so and shows it)
	float hunger = 0;
	// close kin of this creature (a parent, grandparent or other forebear, a
	// child or other descendant, a brother, sister or half-sibling:
	// Breeding.h closeKin): who it learns most from by watching
	bool kin = false;
	bool ill = false; // ill (Disease.h): tended like the young (owner, 2026-11-21)
	bool poisoned = false; // heavy metals over 50 (Exposure.h): tended too
	float pain = 0; // its Pain drive, 0-1, as a carer can see it hurting
};

// An instinct from the genome, translated into neutral terms (see
// gene-translation.md). For C2: "when these conditions hold, doing this action
// is rewarded (or punished), this strongly".
struct InnateCondition {
	enum Kind { Drive, Attention, HeardVerb, HeardNoun, Sense } kind;
	int index; // drive number, object category, word number or sense number
};
struct InnateInstinct {
	std::vector<InnateCondition> conditions;
	int decision = 0;
	bool reward = true; // false: punishment
	float strength = 0; // 0-1
	std::string signature; // identifies the gene, so it is applied once
};

// A stimulus sent to the creature by a world script. Two forms exist in
// C1/C2 scripts: a numbered stimulus (STM# WRIT) and a full description
// (STIM WRIT / STIM FROM). The engine passes them to the mind unchanged
// and applies nothing itself; what a stimulus does is the mind's business.
struct StimulusEvent {
	bool numbered = false; // true: only 'id' is meaningful
	unsigned int id = 0; // numbered form: stimulus number
	int significance = 0, input = 0, intensity = 0, features = 0; // full form
	std::array<int, 4> chem{}; // full form: chemical numbers (0 = none)
	std::array<int, 4> amount{}; // full form: amounts for each chemical
	AgentRef source; // the agent whose script sent it, if known
	std::string command; // e.g. "STM# WRIT", for logging
	// What the body actually received (either form, after the creature's
	// stimulus genes and sleep): chemical numbers and amounts, 0 = none.
	std::array<int, 4> feltChem{};
	std::array<int, 4> feltAmount{};
	// the sense cell it lights (C2: its stimulus gene's sensory neuron, or
	// the full form's input) and how strongly; -1 none
	int feltSense = -1, feltSenseIntensity = 0;
};

// An action the world's creature scripts make available (event 16 + decision).
struct MindAction {
	int decision;
	bool needsObject; // the script acts on the attended object (_it_, appr, aim:)
};

struct MindPerception {
	int creatureId = 0; // the creature agent's UNID, for logging
	std::string self; // its moniker (C2), or empty
	// the words of the game's word list it has learned (indices), for the
	// llm mind to speak with (robpippy-max fork, 2026-11-22)
	std::vector<int> knownWords;
	unsigned int tick = 0; // creature age in brain ticks
	lifestage stage = baby;
	bool asleep = false;
	bool dozing = false; // the body's own falling asleep (the reflex, event 69) is running
	bool dreaming = false;
	bool female = false;
	int genus = -1; // 1 norn, 2 grendel, 3 ettin
	// Sense of place (owner, 2026-11-13): the room the creature is in (by its
	// feet, as its senses use; -1 outside every room), that room's middle
	// and floor, its metaroom, and the rooms next to it through doorways the
	// creature fits through (side: 10 west, 11 east, -1 up, 1 down)
	int room = -1, metaroom = -1;
	float roomX = 0, roomFloor = 0;
	struct RoomNear {
		int id = -1;
		int side = 0;
		float x = 0, floor = 0; // its middle and floor
		bool water = false;
	};
	std::vector<RoomNear> roomsNear;
	// a fall of its own just ended (owner, 2026-11-13): how far, how much it
	// hurt (0-1), and where it stepped off (its middle and feet then)
	float fellHeight = 0, fellHurt = 0, fellFromX = 0, fellFromY = 0;
	// how many times its walking has been pushed back by a wall so far, and
	// the way it was going the last time (10 west, 11 east)
	unsigned int walledCount = 0;
	int walledDir = 0;
	std::array<unsigned char, 17> drives{}; // C2 has 17, C1 uses the first 16
	const unsigned char* chemicals = nullptr; // 256 levels, read-only
	std::vector<SeenAgent> visible; // at most one agent per category
	std::vector<StimulusEvent> stimuli; // received since the last mind tick
	bool actionFinished = true; // the creature's current action script has stopped
	const std::vector<MindAction>* actions = nullptr; // read from the world's scripts
	int currentAttention = -1; // category index, or -1
	int currentDecision = -1; // decision index, or -1
	float x = 0, y = 0; // the creature's centre, in world pixels
	// the world wraps east-west this many pixels around (C1/C2: the far west
	// edge joins the far east); 0 for none. Distances across the seam are
	// the short way round.
	float wrapWidth = 0;
	bool riding = false; // carried by a vehicle (a lift, a boat)
	// a friend nearby was just hurt by this creature (or none): who hurt it,
	// and the friend's moniker
	AgentRef defendFrom;
	std::string defendFor;
	bool carried = false; // held by the hand (or another agent): not moving by itself
	int carryingCategory = -1; // the category of what this creature holds, or -1
	bool scriptMoved = false; // moved by a script in the last few ticks (a stream, a launcher): not by its own power
	bool inRoom = true; // inside the room map (false: inside the ground, about to be lifted out)
	bool breathesWater = false; // amphibious (its genome raises no suffocation under water)
	// food smell (C2, World::tickFoodScent): in this room, and the strongest
	// through the doors on each side
	float scentHere = 0, scentWest = 0, scentEast = 0, scentUp = 0, scentDown = 0;
	// C2: urges from smelled scents (pheromones.txt; creatures/Pheromones.h):
	// follow a scent up its gradient or move away from it. drive >= 0: worth
	// relieving that drive by `weight`; -1: a fixed want of `weight`.
	struct ScentUrge {
		std::string scent;
		bool avoid = false;
		int drive = -1;
		float weight = 1.0f;
		bool tend = false; // its source (a baby, a child, the senile) is tickled and brought food or a toy
		float here = 0, west = 0, east = 0, up = 0, down = 0;
	};
	std::vector<ScentUrge> scentUrges;
	// signature scents (Pheromones::signatureIn) of the creatures the mind
	// asked for (CreatureMind::wantedSignatures), by moniker: here and through
	// the doorways each way
	struct SignatureSense {
		std::string who;
		float here = 0, west = 0, east = 0, up = 0, down = 0;
	};
	std::vector<SignatureSense> signatures;
	// inborn temperament (creatures/Temperament.h): a z-score for each of
	// boldness, curiosity, energy, sociability, temper, reactivity,
	// steadiness; 0 is average
	std::array<float, 7> temperament{};
	// the mind's own heritable settings (creatures/MindGenes.h): learning,
	// decisiveness, patience, wanderlust, then readiness to learn about
	// danger, food, water and company; z-scores, 0 average
	std::array<float, 8> mindGenes{};
	// the season (0 spring, 1 summer, 2 autumn, 3 winter) and how strongly
	// this creature follows it (SeasonalRhythm::strength: 1 average)
	int season = -1;
	// the part of the day (0 dawn, 1 morning, 2 midday, 3 dusk, 4 night)
	int timeOfDay = -1;
	float seasonalStrength = 1.0f;
	// water (C2 room type 2): whether the creature is in it, and which way the
	// nearest dry room lies through open doors (10 west, 11 east, -1 up,
	// 1 down, 0 none found) and how many rooms away
	bool inWater = false;
	float breath = 1.0f; // breath left while under water (1 full, 0 none: suffocating)
	bool waterWest = false, waterEast = false; // the room through the door on that side is water (or a fall lands in water)
	float dropWest = 0, dropEast = 0; // how far the floor drops stepping through that side (0: no drop)
	// a wall right ahead on that side: the foot is at the room's edge and no
	// doorway it fits through goes on at walking height (nor a drop)
	bool wallWest = false, wallEast = false;
	int dryWay = 0, dryRooms = 0;
	std::vector<InnateInstinct> newInstincts; // genome instincts expressed since the last tick
	// C2: the 32 situation senses of the genomes' sense lobe (C2 CAOS guide,
	// "Offsets into SENSE_LOBE": 0 patted, 1 slapped, 2 bumped, 3 near a wall,
	// 4 in a vehicle ... 10 it is approaching, 15 it is a creature, 16-18 it
	// is my sibling, parent, child, 19 a mate in sight ... 30 falling, 31
	// landed), 0-255, as set for the genome's emitters (oldCreature::
	// updateBrainCells). Instincts that name a sense wait for it.
	std::array<unsigned char, 32> senses{};
	// The hand's speech (owner, 2026-11-22: creatures "look at you and
	// answer"; known verbs are weighed): the verb and the thing it knows the
	// words for, -1 none, and mind ticks since (-1: nothing lately)
	int heardVerb = -1, heardNoun = -1, heardAge = -1;
	bool heardNamed = false;
	// called to look at a thing (the hand that spoke or touched it, or what
	// it named): its category, and mind ticks since
	int calledCategory = -1, calledAge = -1;
	// the hand has just patted or slapped it
	bool handPat = false, handSlap = false;
	// the chemicals this genome rewards and punishes learning with (those
	// its decision lobe's first two chemical slots take: 52 and 53 in the
	// game's genomes)
	int rewardChem = 52, punishChem = 53;
	// a custom genome's brain add-ons, as the mind's settings to multiply
	// (creatures/BrainProfiles.h); null or empty: none
	const std::vector<std::pair<std::string, float>>* brainSettings = nullptr;
};

struct MindOutput {
	int attentionCategory = -1; // -1 = no change
	int decision = -1; // -1 = no change; otherwise run creature event 16 + decision
	int sleep = -1; // -1 = no change, 0 = wake, 1 = fall asleep
	bool resistSleep = false; // fights off the body's own falling asleep (event 69) for now
	int expression = -1; // -1 = no change; else facial expression slot 0-5 in the head sprites
	// An object whose use keeps doing nothing for the creature: for a while,
	// notice others of its kind first (the engine notices one object per
	// category; without this a creature in the airlock only ever saw the sea
	// door, never the land door that lets it out).
	AgentRef passOver;
	// A particular agent to attend to (one of the creatures in view), when
	// the mind chose an individual rather than a kind; else the engine picks
	// one of attentionCategory.
	AgentRef attentionAgent;
	// Something to say (a meaning, worded by the engine for the creature's
	// age: CreatureSpeech.h), or empty.
	std::string say;
	// What it is thinking, as pictures for a thought cloud (owner,
	// 2026-11-13; creatures/Thoughts.h): each a symbol's name ("hunger",
	// "heart", "arrow_w"...), "cat:N" for a picture of a thing of category
	// N, or "who:MONIKER" for a creature's face; empty: no new thought.
	// thoughtKey names the thought, so one long trip is not shown again
	// and again.
	std::vector<std::string> thought;
	std::string thoughtKey;
	// an action tried mostly for its novelty (curiosity) has just turned out
	// well (+1) or badly (-1), or 0: what shapes a young creature's
	// curiosity (Temperament.h, EXPERIENCE_EXPLORE)
	int noveltyOutcome = 0;
};

// What a mind holds, for the Mind Kit (openc2e addition, owner 2026-11-11):
// a snapshot drawn on request, in the mind's own terms but plain enough to
// show. Ages are in mind ticks (about 2.5 a second in C1/C2).
struct MindView {
	unsigned int tick = 0; // now
	std::array<float, 17> drives{}; // 0-1
	// how much each need matters to this mind (its temperament changes
	// some), and how a need's pull grows with its level: pull = weight x
	// level ^ pullPower
	std::array<float, 17> weight{};
	float pullPower = 1;
	float content = 0; // 0-1: how far from its worst its needs are
	float x = 0, y = 0; // where it is

	// the options it weighed at its last choice, best first
	struct Option {
		int category = -1; // -1: needs no object
		int decision = 0;
		std::string who; // a particular creature (moniker), or empty
		float want = 0; // how much it expects its needs to ease
		float novelty = 0; // the pull of the untried
		float score = 0;
		std::array<float, 17> ease{}; // want, split among the needs it eases
		int tried = 0; // times it has done this to this kind of thing
		bool chosen = false;
		bool food = false, forage = false, route = false, escape = false, scent = false, travel = false;
		std::string place; // a trip from the sense of place ("mate:<moniker>", "favourite:<need>", "leave:<need>", "explore")
	};
	std::vector<Option> options;
	unsigned int choseAt = 0; // when it last chose
	bool busy = false; // still doing what it chose

	// what it has learned: the expected change in each need from doing
	// something to a kind of thing (or to one creature, `who`)
	struct Lesson {
		int category = -1, decision = 0;
		std::string who;
		int count = 0;
		std::array<float, 17> delta{};
		float value = 0; // how much it eased its needs on the whole, -1..1
	};
	std::vector<Lesson> lessons;

	// what it remembers of the world
	struct Spot {
		float x = 0, y = 0, w = 0, h = 0;
		unsigned int last = 0; // last there (0: not known)
	};
	std::vector<Spot> visited;
	struct Food {
		float x = 0, y = 0, confidence = 0;
		int meals = 0;
		unsigned int last = 0;
	};
	std::vector<Food> food;
	struct Ride {
		float x = 0, fromY = 0, toY = 0;
		int uses = 0;
	};
	std::vector<Ride> rides;
	// sense of place: the rooms it remembers (visits 0: only seen next door)
	struct PlaceMemory {
		int room = -1;
		float x = 0, floor = 0; // the room's middle and floor
		int visits = 0;
		unsigned int last = 0;
		float ticks = 0; // time spent there
		bool felt = false; // long enough there to know how it feels
		std::array<float, 17> feel{}; // each need's change over a stay, against the usual (+ worse)
		bool avoided = false;
		std::vector<int> goodFor; // needs it goes there for
		std::vector<std::pair<int, int>> things; // category, sightings
		std::vector<std::pair<std::string, int>> met; // moniker, meetings
		bool here = false;
	};
	std::vector<PlaceMemory> places;
	std::vector<std::pair<std::string, float>> longing; // creatures it longs to find again, 0-1

	// how its last few choices turned out, oldest first: what it did, how
	// long it took, why it stopped and what happened to its needs
	struct Outcome {
		unsigned int tick = 0; // when it ended
		int category = -1, decision = 0;
		std::string who; // a particular creature (moniker), or empty
		bool travel = false; // going to find a kind of thing (category)
		unsigned int ticks = 0; // how long it lasted
		// "finished", "time", "stalled", "object gone", or the name of a
		// need that grew urgent enough to cut it short
		std::string why;
		float reward = 0; // how much easier its needs became on the whole
		std::array<float, 17> change{}; // each need's change, 0-1 scale
	};
	std::vector<Outcome> outcomes;

	// how content it has been lately, and its strongest need then
	struct Moment {
		unsigned int tick = 0;
		float content = 0;
		int strongest = -1;
		float strength = 0;
	};
	std::vector<Moment> mood;
};

class CreatureMind {
  public:
	virtual ~CreatureMind() = default;
	virtual std::string name() const = 0;
	virtual void born(const MindPerception&) {}
	virtual MindOutput tick(const MindPerception&) = 0;
	virtual void died(const MindPerception&) {}
	virtual std::string debugState() const { return {}; }
	// a snapshot for the Mind Kit; false if this mind has none
	virtual bool view(MindView&) const { return false; }
	// World saving: the mind's learned state as text, and restoring it.
	virtual std::string saveState() const { return {}; }
	virtual void loadState(const std::string&) {}
	// creatures (monikers) whose own scent it wants to smell next tick
	virtual std::vector<std::string> wantedSignatures() const { return {}; }
};

// Returns nullptr for an empty or unknown name.
std::unique_ptr<CreatureMind> createCreatureMind(const std::string& name);

// The names createCreatureMind() accepts, for --help and error messages.
std::string availableCreatureMinds();
