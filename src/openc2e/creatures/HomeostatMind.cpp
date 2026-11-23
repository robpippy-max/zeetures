/*
 *  HomeostatMind.cpp
 *  openc2e
 *
 *  Mind model v0 ("homeostat"): an original creature mind built from
 *  published research on motivation and learning. See mind-model-v0.md in
 *  the project notes for the design and references.
 *
 *  1. Drive and reward: overall drive D combines the body's 17 drives
 *     nonlinearly (severe needs dominate); an action's reward is the drop
 *     in D it produced (homeostatic reinforcement learning).
 *  2. Expectations: per (object category, action), the expected change in
 *     each drive, learned with the delta rule after every action.
 *  3. Wanting and choice: predicted drop in D given current needs, plus a
 *     curiosity bonus, minus a distance cost; softmax choice whose
 *     temperature falls as arousal rises; a commitment bonus for the
 *     current choice.
 *  4. Habituation: categories whose interactions change nothing lose
 *     attention, recovering over time.
 *  5. Affect: valence (running average of reward) and arousal (D) shape
 *     exploration.
 *  6. Instincts: weak innate expectations, overwritten by experience.
 *  7. Facial expression: chosen from affect (Anger, distress, recent mood,
 *     longer-term valence, overall drive).
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

#include "CreatureMind.h"
#include "common/devenv.h"
#include "Temperament.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fmt/core.h>
#include <deque>
#include <map>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// The mind's running commentary: every creature's choices, outcomes, stimuli
// and what it learns, about six lines a decision (70 MB an hour in a world of
// a hundred). Off for players; OPENC2E_MIND_LOG=1 prints it (developer aid;
// the WATCH_ switches still print their own lines). The arguments are not
// worked out when it is off.
bool mindLogOn() {
	static const bool on = getenv("OPENC2E_MIND_LOG") != nullptr;
	return on;
}
#define MIND_LOG(...) \
	do { \
		if (mindLogOn()) \
			fmt::print(__VA_ARGS__); \
	} while (0)

constexpr int NUM_DRIVES = 17;

// Labels for log lines only (the order of drives in the owner's
// allchemicals.str, chemicals 1-17).
const char* const DRIVE_NAMES[NUM_DRIVES] = {
	"Pain", "Pleasure", "Hunger", "Cold", "Hot", "Tired", "Sleepy", "Lonely",
	"Crowded", "Fear", "Bored", "Anger", "Sex", "Injury", "Suffocate", "Thirst", "Stress"};
enum Drive { PAIN = 0,
	PLEASURE,
	HUNGER,
	COLD,
	HOT,
	TIRED,
	SLEEPY,
	LONELY,
	CROWDED,
	FEAR,
	BORED,
	ANGER,
	SEX,
	INJURY,
	SUFFOCATE,
	THIRST,
	STRESS };

// Action labels for log lines only, as read from the owner's world scripts.
std::string actionName(int decision) {
	switch (decision) {
		case 0: return "idle";
		case 1: return "activate1";
		case 2: return "activate2";
		case 3: return "deactivate";
		case 4: return "approach";
		case 6: return "pickup";
		case 7: return "drop";
		case 8: return "express";
		case 10: return "travel-a";
		case 11: return "travel-b";
		case 12: return "eat";
		case 13: return "hit";
		case 25: return "sleep";
		case 26: return "walk-a";
		case 27: return "walk-b";
		default: return fmt::format("action{}", decision);
	}
}

// Watching others (owner, 2026-11-20): each creature posts what it has just
// done here; every mind reads what was posted since its last tick and takes
// in what came from creatures it can see (watchOthers). Not saved: what was
// learned from it is.
struct Demo {
	enum Kind { OUTCOME, MEAL, HURT, FALL } kind = OUTCOME;
	unsigned long seq = 0;
	std::string who; // the one seen
	int category = -1, decision = -1; // OUTCOME, MEAL: what it acted on, and how
	std::array<float, NUM_DRIVES> delta{}; // OUTCOME: what it got; HURT: the change over `ticks`
	std::array<bool, NUM_DRIVES> felt{};
	float x = 0, y = 0; // where (MEAL: the place; FALL: where it stepped off)
	int room = -1, metaroom = -1; // HURT
	float roomX = 0, roomFloor = 0, ticks = 0;
	bool water = false;
	int dir = 0; // FALL
	float hurt = 0;
};
struct DemoBoard {
	std::deque<Demo> demos;
	unsigned long seq = 0;
	void post(Demo d) {
		d.seq = ++seq;
		demos.push_back(std::move(d));
		while (demos.size() > 2048)
			demos.pop_front();
	}
};
DemoBoard& demoBoard() {
	static DemoBoard b;
	return b;
}
bool watchingOff() {
	static const bool off = devenv("OPENC2E_NO_WATCHING") != nullptr; // (to compare)
	return off;
}

// Every tunable number in one place, so they can become genes later.
struct Params {
	float driveExpN = 3.0f; // D = (sum w_i d_i^n)^(1/m), n > m
	float driveExpM = 2.0f;
	std::array<float, NUM_DRIVES> weights;
	float learningRate = 0.3f; // delta rule
	float individualPrior = 3.0f; // dealings with an individual before its own record counts half
	float retreatFear = 0.3f; // a creature feared this much, close by, is backed away from
	// Temperament's signature behaviours (owner, 2026-11-11: traits must show),
	// each per unit of the trait's z-score: the bold drawn to strangers and the
	// shy put off (scaled by how strange), the shy backing away from strangers
	// close by, the sociable going toward company (more toward the liked), the
	// hot-tempered hitting sooner (more when angry), the energetic pacing about
	// and the placid settling when nothing presses.
	float strangerBias = 0.02f, shyRetreat = 0.015f, socialFollow = 0.012f, temperHit = 0.03f, energyPace = 0.006f;
	int hitDecision = 13;
	// temperament (reactivity): outcomes worse than expected are learned at
	// this share of the rate, better ones at this share
	float worseLearning = 1.0f, betterLearning = 1.0f;
	// readiness to learn about each need (MindGenes.h: danger, food, water,
	// company), times the learning rate for changes in that drive
	std::array<float, 17> driveLearning{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
	// Watching others (owner, 2026-11-20: learning from parents; "family
	// most", "young most", "about a third"): what another creature in sight
	// was seen to get from a thing, where it ate, where it was hurt and what
	// drop hurt it are learned as from its own doing, at this share
	float watchShare = 1.0f / 3.0f;
	float watchStranger = 0.35f; // one not of its family: this share of kin
	std::array<float, 7> watchStage{1.0f, 1.0f, 0.8f, 0.6f, 0.45f, 0.35f, 0.25f}; // by the watcher's life stage
	// pain, injury, fear and suffocation risen this much over watchHurtTicks
	// in one room: others in sight see it hurt there
	float watchHurtMin = 0.04f;
	float watchHurtTicks = 10.0f;
	// naming: a child or older that has just used a thing says its name now
	// and then, and often when a young one of its family (a baby or child)
	// is in sight (CreatureSpeech "name:C"; hearers learn the word)
	float nameChance = 0.08f, nameTeachChance = 0.5f;
	unsigned int nameGap = 150, nameFresh = 25;
	// Felt effects: stimuli tell the body directly what an interaction did
	// (C2 chemicals 18-34 raise drive 1-17, 35-51 lower it), like a taste.
	// A drive an action's stimuli touched is learned from what was felt; the
	// others from the change observed beyond drift, more slowly, since that
	// mixes in whatever else was going on.
	int driveIncreaseChem = 18, driveDecreaseChem = 35;
	float unfeltLearningShare = 0.25f;
	float curiosity = 0.02f; // novelty bonus scale
	// Curiosity by learning progress (owner, 2026-11-16, from the citations
	// pass: Kidd & Hayden 2015; Gottlieb & Oudeyer 2018; Oudeyer, Gottlieb &
	// Lopes 2016). For its first firstTries tries a thing is worth trying
	// for being little known (curiosity / sqrt(1 + tries), as before);
	// after that, for how fast the creature is still learning about it:
	// the gap between its recent and longer-run prediction errors, an error
	// change of lpScale worth full curiosity, at most lpMax times. What is
	// fully known, and what is too random to learn, lose their pull; what
	// starts to surprise again regains it. OPENC2E_COUNT_CURIOSITY: rarity
	// alone, as before (for comparison).
	int firstTries = 3;
	float lpScale = 0.05f, lpMax = 1.5f;
	float boredomCuriosityGain = 2.0f; // Boredom raises curiosity
	float temperature = 0.015f; // softmax base temperature
	float arousalFocus = 2.0f; // high D lowers temperature
	float commitment = 0.01f; // bonus for repeating the current choice
	float distanceCost = 0.00002f; // per pixel to the object
	int maxEpisodeTicks = 12; // looping actions are evaluated this often
	// Sleep: the world's sleep script keeps the creature asleep and resting
	// until it is rested, then wakes it. The mind lets it run, and wakes the
	// creature early only when another need rises sharply while it sleeps.
	int sleepDecision = 25;
	int maxSleepTicks = 3000;
	float wakeRise = 0.25f; // another drive has risen this much since falling asleep...
	float wakeLevel = 0.6f; // ...and is at least this high
	int noSleepAfterWaking = 100; // ticks before sleeping again after an urgent waking
	// The world's sleep script can refuse: C2's lets a creature sleep only
	// while its Tiredness is above 48, and a rested creature in the dark is
	// very sleepy all the same (grendels: walking tires them only once old).
	// A sleep that ends within this many ticks without the creature ever
	// falling asleep was refused; sleep is not offered again for a while.
	int refusedSleepTicks = 5;
	int noSleepAfterRefusal = 300;
	// Woken by cold or heat: the place is the trouble, not sleeping. Do not
	// sleep again near there for a while (so the creature sleeps somewhere
	// else), and do not learn that sleeping itself chills or overheats (a
	// creature woken by the cold at the west edge came to rank sleep so low
	// that it never slept again and ran out of energy)
	float badSleepNear = 300.0f;
	float poisonPain = 1.0f; // felt Pain per unit of poison taken in (see the stimulus loop)
	unsigned int badSleepTicks = 1200;
	// Hunger keeps a creature awake (starved animals sleep far less and
	// forage instead; fruit flies and rats both show it): at hunger
	// starveAwake or more, sleep is not chosen unless sleepiness or
	// tiredness has reached exhausted. (A starving norn slept 300 s at a
	// time: the sleep script's stimuli felt restful, its drives never fell,
	// and it never went looking for food.)
	float starveAwake = 0.85f, exhausted = 0.97f;
	int maxObjectEpisodeTicks = 150; // actions on an object (walk there, then act): at most this long
	int stallTicks = 25; // ...and given up if not getting closer for this long
	float habituationRate = 0.7f; // multiplier after an uneventful interaction
	float habituationRecovery = 0.01f; // per tick, back toward 1
	float eventfulThreshold = 0.02f; // total |drive change| that counts as eventful
	float valenceRate = 0.1f; // running average of reward
	float driftRate = 0.1f; // running average of passive drive change per tick
	size_t choiceSetSize = 5; // softmax over the top few candidates only

	// Genome instincts (gene-translation.md): an instinct is an innate
	// expectation. At full strength it predicts this much change in its drive,
	// and counts as this many remembered experiences (resilience): learning
	// starts slow and speeds up as real experience outweighs it.
	float instinctDriveChange = 0.25f;
	float legacyInstinctResilience = 12.0f; // legacy genes have one number; scaled by strength squared
	float anyObjectInstinctWeight = 1.5f; // an instinct naming no object is a weak hunch about objects in general
	// Instincts that name no drive ("seeing a cliff edge: back away", "in a
	// vehicle: rest", "seeing a bad plant: don't eat it") are inborn nudges:
	// at full strength they add this much to an action's score while their
	// situation holds. Each counts as legacyInstinctResilience x strength^2
	// remembered tries; every try in that situation is judged by the
	// approval felt (the genome's reward chemical rising, its punishment
	// chemical falling; decision 1 of 2026-11-07), so a nudge that brings no
	// approval fades as experience outweighs it, and one that does holds.
	float innateNudge = 0.12f;
	// Spoken to by the hand (owner, 2026-11-22: creatures look at you and
	// answer; known verbs are "suggestions its mind weighs"). Called, it
	// looks round at what it was called to (decision 0, watch it) for about
	// callTicks; a heard verb it knows adds to its action's score through the
	// genome's own obedience instincts (hearing a verb: do it, rewarded),
	// fading over heardTicks, more to the good-tempered and sociable. Praise
	// (a pat) or a slap within praiseTicks of doing as it was told is what
	// that instinct is judged by, as the nudges above: unpraised, obeying
	// fades; slapped for not listening, it listens better next time.
	float callPull = 0.25f;
	int callTicks = 8; // mind ticks, about 3 s
	float obey = 0.15f;
	int heardTicks = 24; // about 10 s
	int praiseTicks = 30;
	float approvalScale = 32.0f; // a change of this much reward minus punishment chemical is full approval
	unsigned char senseOn = 64; // a sense cell at least this lit counts as the situation holding

	// Seeking: when something wanted is out of sight, head for where it was
	// last seen (a first sense of place).
	float seekDiscount = 0.6f; // remembered relief counts for this share of seen relief
	float seekDistanceCost = 0.00015f; // per pixel to walk: something wanted 1000 px away loses 0.15
	float seekMinWant = 0.01f;
	float seekMaxHeight = 200.0f; // walking west/east cannot change floors
	float seekArrived = 40.0f; // closer than this and still not seen: forget the place

	// Changing floors (a first cognitive map). Innate: lifts (C2 category 27,
	// "lifts and up/down movers") and call buttons (2) are for changing floors;
	// which action goes which way is learned from rides, as links.
	int liftCategory = 27, buttonCategory = 2;

	// Edible scent (perceived): smelling nourishing makes eating expected to
	// relieve hunger, up to this much at full scent, weighted as this many
	// remembered experiences; no food smell removes the vague hope of a hunch.
	int eatDecision = 12;
	float scentHungerRelief = 0.4f;
	float scentWeight = 6.0f;

	// Following food smell through the rooms (World::tickFoodScent): when no
	// reachable food is in view, walk or ride toward stronger smell. Valued
	// as this share of the relief eating smelled food is expected to bring.
	float scentFollowShare = 0.5f;
	float scentStronger = 1.1f; // a side must smell this much stronger than here
	float scentFloorStep = 300.0f; // how far up or down a floor change aims
	// Scents with behaviour rules (pheromones.txt): an urge tied to a drive
	// is worth relieving that drive by this much times the rule's weight
	// (then scaled like food smell, scentFollowShare); one with no drive is
	// worth its weight times this.
	float pheromoneRelief = 0.4f;
	// Tending (owner, 2026-11-15; pheromones.txt "... follow for lonely 1
	// tend"): a grown creature drawn by a care scent, with one who gives it
	// off (its own kind: a baby, a child, the senile) in sight, tickles it
	// (activate 1 on it) and brings it food or a toy: picks one up nearby,
	// walks it over and drops it beside them. Weighed as the scent's drive
	// (loneliness, the owner's choice). The same one is not tickled again
	// for tendAgain ticks, nor brought anything for bringAgain.
	float tendSee = 350.0f; // a tendee this near (in sight, reachable) is gone to
	float tendShare = 1.0f; // the tickle: this share of the urge's worth
	float bringShare = 0.9f; // picking up, carrying over and dropping: this share
	float bringSearch = 250.0f; // food or a toy this near is picked up to bring
	float bringDropNear = 60.0f; // dropped when this near (centres, x)
	unsigned int tendAgain = 900, bringAgain = 1800, bringFailWait = 600;
	// sharing with friends (addShare): only when its own hunger is at most
	// shareFed, for a friend (affinity at least shareFriend) whose hunger is
	// at least shareHungry; worth shareWant x (0.5 + affinity) x its hunger
	float shareFed = 0.35f, shareFriend = 0.4f, shareHungry = 0.6f, shareWant = 0.12f;
	// defending a friend (addDefend): worth defendWant, less as its own most
	// pressing need grows
	float defendWant = 0.10f;
	float tendCalmPower = 2.0f; // a tend urge without a drive: times (1 - most pressing need)^this
	std::vector<int> bringCategories{6, 25, 13, MIND_HEALING_FRUIT}; // food, fruit, toys, healing fruit
	float pheromoneWantScale = 0.1f;
	// Following a scent persists (owner chose this, 2026-11-13): mates apart
	// never met, as each follow was one 12-tick step re-weighed from scratch
	// and stopped wherever old scent had pooled ("here" strongest). Once a
	// creature sets off along a scent it keeps that way, its want raised by
	// followBoost, for up to followDecisions decisions while the urge holds;
	// it gives up when the scent falls below followKeepShare of the best it
	// smelled on the way, or after followOvershoot decisions with nothing
	// stronger ahead (it walks on through a pool rather than stopping in it).
	int followDecisions = 20;
	float followBoost = 1.5f;
	float followKeepShare = 0.5f;
	int followOvershoot = 4;
	float floorChangeDiscount = 0.5f; // share of the target's value for each step toward it
	float atPlace = 50.0f; // close enough to a lift's position to use it
	float sameLevel = 120.0f; // heights this close count as the same floor
	float minRide = 60.0f; // a ride shorter than this did not change floors

	// Blocked ways: walking west or east toward something that got the
	// creature nowhere means that way is blocked from here, for a while.
	// Food finding (2026-11-08). A food map: places where food was seen or a
	// meal eased hunger, kept with a confidence that falls when the creature
	// arrives and finds nothing (food regrows, so places are not forgotten
	// at once), and saved with the creature.
	float foodSeeEdible = 0.3f; // a visible object smelling this nourishing marks a food place
	// food less pleasant when full (alliesthesia): below this hunger, eating's
	// expected ease of other drives scales with hunger, down to satedLiking
	float satiationHunger = 0.35f, satedLiking = 0.2f;
	// autumn appetite: the share of the way to full hunger added to how
	// hungry it feels in autumn, times its seasonal strength, at most the max
	float autumnAppetite = 0.2f, autumnAppetiteMax = 0.45f;
	// anticipating hunger: the share of the way to full hunger added per cue
	// (dusk, sleepiness, scarcity), at most anticipateMax, with food at hand
	// within anticipateNear px
	float anticipate = 0.12f, anticipateMax = 0.3f, anticipateNear = 250.0f;
	// seasonal behavior (each times seasonal strength and stage weight): the
	// share of the way to full sex drive added in spring, the share taken
	// off in winter; the share added to loneliness and to sleepiness in
	// winter; the extra worth of novelty in summer
	float springSex = 0.25f, winterSexDamp = 0.4f, winterLonely = 0.25f, winterSleepy = 0.2f, summerPlay = 0.5f;
	float unreachableFoodConfidence = 0.3f; // a new place marked by food seen out of reach
	float foodMerge = 200.0f; // places this close (same floor) are one place
	size_t foodPlaceMax = 8;
	float foodArrived = 60.0f; // this close to a place: look whether food is there
	unsigned int foodRecheckTicks = 200; // a place is judged empty at most this often
	// Travel is valued by time (owner's idea), with walking speed taken as
	// constant: a remembered meal keeps half its value per travelHalf pixels
	// of walking (a floor change counts as floorChangeDistance pixels), and
	// hunger makes the creature more patient: travelHalf * (1 + hungerPatience
	// * hunger^2), so a starving creature will cross the world for food.
	float travelHalf = 700.0f;
	// The place map (a sense of place, owner 2026-11-11): the world in cells
	// of cellWidth x cellHeight, and the ways between them the creature has
	// found by walking, climbing, dropping and riding. Planned over for food
	// further than a straight walk (a route costs its pixels walked, a ride
	// floorChangeDistance); routeMaxCost bounds the search.
	float cellWidth = 120.0f, cellHeight = 100.0f;
	size_t placeEdgeMax = 6000;
	float routeMaxCost = 9000.0f;
	float hungerPatience = 6.0f;
	float floorChangeDistance = 400.0f;
	// Foraging: hungry, nothing in sight, nothing remembered worth the trip,
	// no smell to follow: search, heading for the part of this floor visited
	// least recently, keeping the heading for a while.
	float forageHunger = 0.45f;
	float forageShare = 0.35f; // worth this share of an expected meal
	unsigned int forageCommit = 150; // mind ticks to keep a heading
	float visitBinWidth = 400.0f, visitBinHeight = 300.0f;
	// a boat ride to search across water is not taken to a shore visited
	// this recently (decisions)
	unsigned int forageRevisit = 3000;
	int forageLookBins = 4; // how far ahead (in bins) staleness is judged
	// Noticing lack of progress: a search heading that has not carried the
	// creature foragePass pixels after forageStuckTicks is turned around (both
	// ways stuck: another floor); a food trip that has not come foodTripGain
	// pixels closer in foodTripTicks makes that place "out of reach from
	// here": its confidence halves and it is not tried for foodCooldown ticks.
	// Restlessness (owner's idea): the longer a creature stays hungry without
	// a meal that eases it, the sooner and harder it searches, and the more
	// readily it leaves its own floor. It builds over restlessRamp mind ticks
	// spent at hunger restlessHunger or more, and resets with a good meal.
	float restlessHunger = 0.3f;
	float restlessRamp = 1500.0f; // about 10 game minutes
	float forageHungerRestless = 0.25f; // forage threshold at full restlessness
	float restlessBoost = 2.0f; // forage worth up to (1 + boost) times as much
	float restlessLeaveFloor = 0.6f; // chance per new heading of trying another floor, at full restlessness
	unsigned int floorSearched = 1500; // both ways visited this recently: the floor counts as searched

	// Sense of place (owner, 2026-11-13). Each creature remembers the rooms
	// it has been in: how often and how long, what it saw there, whom it met
	// there, and how its needs went while there, against how they go
	// anywhere (a room's "feel"). Uses: (1) finding things again: a wanted
	// thing out of sight, its last-seen spot already checked, is sought in
	// the room it was most often and most lately seen in; (2) mates: an adult
	// wanting to mate with no mate in view goes where it last met one; (3)
	// favourite places: a strong need sends it to the room that eased that
	// need best (a warm room when cold, its usual bed when sleepy), and
	// (4) avoided places: a room where pain, cold, heat, fear or injury rose
	// is left and not sought; (5) exploring: curiosity about rooms next door
	// it has never been in, more when bored.
	size_t roomMemoryMax = 500;
	float feelMinTicks = 60.0f; // mind ticks in a room before its feel counts
	float feelKeepTicks = 3000.0f; // older feel fades (halved past this)
	unsigned int sightingGap = 100; // a thing seen again after this long is another sighting
	float recallRecency = 30000.0f; // ticks for a sighting's weight to fall by e
	unsigned int recallEmptyTicks = 1500; // a room checked and found without it: not tried for this long
	float mateSex = 0.35f; // Sex drive (0-1) at which a creature goes to where it met a mate
	float mateSeekShare = 0.8f;
	unsigned int mateCheckedTicks = 1500;
	// Longing (owner, 2026-11-13): a male's Sex falls to nothing within a
	// minute of being apart, so wanting a mate is remembered in the mind:
	// time with an adult of its kind and the other sex while wanting to mate
	// builds a longing for that one, which fades over about half an hour
	// apart and, strong enough, sends the creature to where they met even
	// with no Sex drive left; together again, their bodies do the rest.
	float longingSex = 0.3f; // Sex at which time together builds longing
	float longingGain = 0.004f; // per tick together, times Sex (and liking)
	float longingFade = 15000.0f; // ticks to fade by e
	float longingSeek = 0.15f; // enough to go looking
	float longingWant = 0.12f; // what finding them is worth at full longing
	float favouriteNeed = 0.4f; // a need this strong sends it to a room that eased it
	// (pain, lower: a lasting ache, as heavy metals give, is about 0.25-0.35;
	// owner, 2026-11-21: poisoned creatures should find their way back to
	// where the volcano's fruit eased it)
	float favouritePainNeed = 0.25f;
	// how much easing of pain a kind of food must be known to bring, to be
	// carried to one in pain first (a meal's learned Pain change)
	float painCureKnown = 0.01f;
	float favouriteMinEase = 0.0004f; // per tick better than usual, for a room to count as good for a need
	float favouriteHorizon = 400.0f; // ticks of that easing it expects from going
	float favouriteShare = 0.6f;
	// resting and sleeping work anywhere: going to its usual bed is a stroll
	// while only fairly tired or sleepy, and not far; past this it lies down
	float bedNeedMax = 0.75f;
	float bedReach = 1200.0f;
	float avoidFeel = 0.0006f; // per tick worse than usual (pain, cold, heat, fear, injury, suffocation)
	float leaveNeed = 0.2f; // that need at least this high: leave the room
	float exploreWant = 0.05f; // curiosity about an unvisited room next door (doubled with the novelty rules, owner 2026-11-20: 0.025 explored in ~0.5% of decisions)
	float exploreReach = 1500.0f; // a known but unvisited room on this floor this near
	// (owner, 2026-11-20: counts fade, novelty returns, the young explore
	// most) a room's novelty is 1 / sqrt(1 + its visits), the visits fading
	// with time away (by e in noveltyReturn ticks, about halving in a game
	// hour), so rarely visited rooms keep drawing a bored creature and old
	// haunts become worth a look again (count-based exploration: Strehl and
	// Littman 2008, Bellemare et al. 2016; novelty and boredom: Berlyne
	// 1960). Rooms less novel than noveltyMin are not explored.
	float noveltyReturn = 52000.0f;
	float noveltyMin = 0.25f;
	// how much each life stage explores (baby .. senile): the young most
	// (childhood as a time for exploring: Gopnik 2020)
	std::array<float, 7> stageExplore{1.0f, 1.5f, 1.35f, 1.15f, 1.0f, 0.8f, 0.6f};
	// a thought bubble of surprise when an action eases its needs this much
	// more than it expected
	float surpriseReward = 0.15f;
	float foragePass = 80.0f;
	unsigned int forageStuckTicks = 60;
	float foodTripGain = 100.0f;
	unsigned int foodTripTicks = 300, foodCooldown = 600;
	// Futile steps: an action on an object that ends "stalled" (never got any
	// nearer) with the creature no more than futileMove pixels from where it
	// began, futileAfter times in a row, is not offered again within
	// futileNear pixels of that spot for futileTicks. (A norn at the eastern
	// shore chose "row west" on a boat it could not reach thirty times while
	// it starved; steps toward a destination learn nothing themselves, so
	// nothing else ever told it to stop.)
	int futileAfter = 3;
	float futileMove = 30.0f;
	float futileNear = 400.0f;
	unsigned int futileTicks = 1500;

	// Moving attention on: an object used this many times in a row with no
	// benefit (no drop in overall drive, no ride to another floor) is passed
	// over for a while, so the next object of its kind gets noticed.
	int passOverAfter = 2;
	float passOverReward = 0.005f;

	// Escaping water (owner request): in water, getting out is worth the
	// relief of clearing Suffocate plus escapeUrgency (so it leaves before
	// suffocating). With a dry room reachable through open doors it heads
	// that way; without one it tries the devices in view (exits that worked
	// before first). Reaching dry land within exitWindow ticks of using an
	// object makes that object, there, a remembered exit (saved).
	float escapeUrgency = 0.06f;
	float escapeBreathUrgency = 0.3f; // plus this times (1 - breath left)^2
	float escapeDeviceShare = 0.7f; // trying a device: this share of the escape's worth
	float exitBonus = 1.5f; // a remembered exit nearby: this many times the escape's worth
	float exitNear = 400.0f;
	unsigned int exitWindow = 400;
	unsigned int waterSettle = 8; // ticks in (or out of) the water before it counts as a change
	size_t exitMax = 8;
	// Devices that did nothing for an escape (passed over) are not tried again
	// for this long (mind ticks); with none left untried in view, the creature
	// searches the room, walking away from the ones it has tried (in the
	// airlock's west end the land door is out of sight)
	unsigned int triedFor = 400;
	float searchShare = 0.6f; // searching: this share of the escape's worth
	// Devices that flooded the creature (it was dry, used one, and was in water
	// soon after): not used again while dry near where that happened (the
	// airlock's sea door, or its pump once drained, which floods it again)
	float flooderNear = 200.0f;
	// devices worth trying to get out (C2 categories are genus numbers):
	// call buttons (2), machines such as the airlock pump (3), doors such as
	// the airlock's (9), lifts (27)
	std::vector<int> escapeDevices{2, 3, 9, 13, 27, 33}; // (33 teleporters: see starveLeave; 13 toys: the sea jump pads throw onto a shore, sea-pads.cos)

	// Boats (owner request): walking a trip into water is avoided; with a boat
	// in view, rowing that way is worth the trip. C2 boats (category 31): the
	// first activate rows east (back west on a second press), the second rows
	// west, and the boat puts its passengers out on the far shore. Rides are
	// learned (decision and how far east or west it carried the creature), so
	// the innate guess gives way to experience.
	int boatCategory = 31;
	// Travel links (owner's idea: think of boats as the airlock exit is
	// thought of): using a device, then soon being somewhere quite different
	// (a ride ending linkMove px or more away, or on another floor), is
	// remembered as "this object, used here, takes me there" (saved). A
	// creature heading somewhere uses a link that starts within linkNear of
	// it and ends at least linkGain px closer to where it wants to be.
	float linkMove = 300.0f;
	float linkNear = 350.0f;
	// Leaving a place to find food (2026-11-14): starving (Hunger at least
	// starveLeaveHunger) and long without a meal (restlessRamp), a creature
	// takes a known way from here that leads far off (a learned travel link
	// at least starveLeaveFar px away), or tries a teleporter (category 33)
	// in view, worth starveLeaveShare of a meal. The gene lab's lower room
	// has no way out but its teleporter; creatures that had learned it still
	// starved at the room's far end, acting on each other, as nothing offered
	// outweighed that. Doors and machines are left out (airlocks).
	float starveLeaveHunger = 0.85f;
	float starveLeaveFar = 800.0f;
	float starveLeaveShare = 0.7f;
	int teleporterCategory = 33;
	float linkGain = 400.0f;
	unsigned int linkWindow = 600;
	size_t linkMax = 16;
	float linkShare = 0.95f; // a known link: this share of the trip's worth
	float boatShare = 0.9f; // a boat crossing: this share of the trip's worth
	// Drops: walking off a ledge or dock more than cliffDrop px high is
	// avoided when searching or following smell, unless the trip is headed
	// that much lower anyway.
	float cliffDrop = 150.0f; // (no longer used: drops are judged by what falls have taught)
	// Falls (owner, 2026-11-13: creatures learn not to walk off ledges): a
	// drop up to harmlessDrop px is a step; beyond it, what a fall is expected
	// to hurt is learned, per pixel and per place, from falls that hurt.
	float harmlessDrop = 100.0f;
	float fallAvoidHurt = 0.05f; // keep from a drop expected to hurt this much
	float fallAvoidHurtGoingDown = 0.15f; // on a trip that leads lower anyway
	float fallSpotNear = 80.0f;
	float boatCallShare = 0.8f; // calling a boat over with a switch: this share of the crossing's
	// Learning what a switch does: a button pressed to call a boat, then a
	// boat within reach within boatCallWait ticks, is a known boat switch
	// (saved); a button that brought no boat counts against itself, and one
	// with more failures than successes is not pressed for boats again.
	unsigned int boatCallWait = 200;
	// A call button pressed blind (no lift in view, no known link) that brings
	// no lift into view and no ride within this many ticks is a dud there
	// (our boat switches are buttons too: two starving norns pressed one for
	// ever, owner 2026-11-10); no blind presses near it for dudButtonTicks.
	unsigned int blindPressWait = 100;
	unsigned int dudButtonTicks = 2000;
	float dudButtonNear = 150.0f;
	float boatCallKnownBonus = 1.2f; // a known boat switch: worth this much more
	float boatRide = 200.0f; // a ride carrying the creature this far counts
	unsigned int boatNoRide = 60; // pressed a boat, no ride this many ticks later: try the other action
	unsigned int boatFlip = 300; // ...for this long

	float blockedMove = 3.0f; // moved less than this in a walking episode (walking covers about 10)
	// past this Tiredness the genome's tired walk sets in (C2: drive above
	// 174 of 255), a shuffle of a few pixels a step: then only a much
	// smaller move counts as blocked (a starving, worn-out norn by the far
	// east lift shuffled 3-5 px a walk, took both ways for walls and rested
	// 335 times in a row; owner, 2026-11-11)
	float tiredWalk = 174.0f / 255.0f, tiredBlockedShare = 0.2f;
	float blockedNear = 40.0f; // still this close to where it got stuck
	float turnBumpWay = 30.0f; // a walk's bump behind it after less way than this toward it: turning, not a wall
	// a walk cut off sooner than this (another creature's poke or hit, a
	// stimulus) has not shown that the way is blocked
	int nowhereMinTicks = 6;
	unsigned int blockedTicks = 600;

	// 7. Facial expression from affect. Slots refer to the head sprites'
	// expression rows (in the owner's norn heads: 0 neutral, 1 happy,
	// 2 sad, 3 angry); other breeds may differ, so these belong in a breed file.
	int faceNeutral = 0, faceHappy = 1, faceSad = 2, faceAngry = 3;
	float angryAt = 0.5f; // Anger drive level (0-1)
	float distressAt = 0.6f; // max of Pain, Fear, Lonely (0-1)
	float moodRate = 0.5f; // how fast the recent mood follows each reward
	float moodDecay = 0.95f; // per tick, recent mood fades toward neutral
	float happyMood = 0.04f, sadMood = -0.05f; // recent-mood thresholds
	float sadValence = -0.03f; // longer-term valence threshold
	float contentValence = 0.01f, contentDrive = 0.3f; // calm and content

	// Needs are not equal: what keeps the body alive outweighs comfort and
	// company. With D's cube, weight 1.5 against 0.5 makes Hunger at 0.5
	// count as much as Boredom at about 0.72.
	float survivalWeight = 1.5f; // Pain, Hunger, Cold, Hot, Injury, Suffocate, Thirst
	float comfortWeight = 0.5f; // Lonely, Crowded, Bored
	// Pressing survival needs quiet curiosity: at the strongest survival need
	// s, novelty is scaled by 1 - survivalCuriosityDamp * s.
	float survivalCuriosityDamp = 0.8f;

	Params() {
		weights.fill(1.0f);
		weights[SEX] = 0.3f;
		for (int i : {PAIN, HUNGER, COLD, HOT, INJURY, SUFFOCATE, THIRST})
			weights[i] = survivalWeight;
		for (int i : {LONELY, CROWDED, BORED})
			weights[i] = comfortWeight;
	}
};

// Weak innate expectations, keyed by decision number (instincts).
// Values are expected drive changes on the 0-1 scale.
std::array<float, NUM_DRIVES> instinctFor(int decision) {
	std::array<float, NUM_DRIVES> e{};
	switch (decision) {
		case 12: // eat
			e[HUNGER] = -0.05f;
			break;
		case 25: // sleep
			e[TIRED] = -0.05f;
			e[SLEEPY] = -0.05f;
			break;
		default:
			break;
	}
	return e;
}

struct Expectation {
	std::array<float, NUM_DRIVES> delta{}; // expected change in each drive
	int count = 0; // times experienced
	float prior = 0; // innate weight, in remembered experiences (from instincts)
	// how wrong the expectation has been lately (the summed size of its
	// errors over the drives): a fast and a slow running average. Their
	// difference is learning progress, which curiosity follows (see
	// Params::lpScale)
	float errFast = 0, errSlow = 0;
	void noteError(float err) {
		// (new, or loaded from a save made before errors were kept: start
		// both at this error, so no progress is read into the first one)
		if (count == 0 || (errFast == 0.0f && errSlow == 0.0f)) {
			errFast = errSlow = err;
			return;
		}
		errFast += 0.5f * (err - errFast);
		errSlow += 0.1f * (err - errSlow);
	}
	float progress() const { return std::fabs(errSlow - errFast); }
};

struct Place {
	float x = 0, y = 0;
	unsigned int tick = 0;
};

// A ride that changed floors: at this lift position, this action took the
// creature from one height to another.
struct Link {
	float x = 0, fromY = 0, toY = 0;
	int decision = 0;
	int uses = 0;
};

struct FoodPlace {
	float x = 0, y = 0;
	unsigned int tick = 0; // last seen or eaten there
	float confidence = 0.5f;
	int meals = 0;
	unsigned int checked = 0; // last time arriving there was judged
	unsigned int cooldownUntil = 0; // out of reach from here: not tried until then
	int category = -1; // what was seen or eaten there (a thought's picture; the cure trip, MIND_HEALING_FRUIT)
};

struct Candidate {
	int category; // -1 for actions that need no object
	int decision;
	float distance;
	float want; // predicted drop in D
	float novelty;
	float score;
	int seekCategory = -1; // >= 0: travelling toward where this category was last seen
	bool chain = false; // a step toward something else (changing floors): no drive learning
	bool scent = false; // following food smell
	std::string urge; // following or avoiding a scent (pheromones.txt): which, for logging
	AgentRef target; // a particular creature this acts on (individuals), or none
	std::string who; // its moniker
	bool food = false; // heading for a remembered food place
	bool forage = false; // searching for food
	bool escape = false; // getting out of water
	bool boatCall = false; // pressing a button to call a boat
	bool blindPress = false; // pressing a call button with no lift in view or known
	bool route = false; // a step along a route on the place map
	bool looking = false; // looking round at what it was called to
	std::string place; // a trip from the sense of place: "mate:<moniker>", "favourite:<need>", "leave:<need>", "explore"
};

struct CallButton {
	float x = 0, y = 0; // where it was pressed from
	int worked = 0, failed = 0;
};

struct BoatLink {
	int decision = 0;
	float dx = 0; // how far east (positive) or west the ride carried the creature
	int rides = 0;
};

struct TravelLink {
	float fromX = 0, fromY = 0, toX = 0, toY = 0;
	int category = -1, decision = -1;
	int uses = 0;
	int fails = 0; // tries that took it nowhere (not saved)
};

struct ExitLink {
	float x = 0, y = 0; // where the object was used
	int category = -1, decision = -1;
	int uses = 0;
};

class HomeostatMind : public CreatureMind {
  public:
	HomeostatMind() {
		habituation.fill(1.0f);
	}

	std::string name() const override { return "homeostat"; }

	void born(const MindPerception& p) override {
		if (p.brainSettings && *p.brainSettings != brainSettings) {
			brainSettings = *p.brainSettings;
			temperamentApplied = false;
		}
		applyTemperament(p.temperament, p.mindGenes, p.creatureId);
		rng.seed((unsigned int)p.creatureId);
		if (!mindLogOn())
			return;
		std::string acts;
		for (auto& a : *p.actions)
			acts += fmt::format(" {}{}", actionName(a.decision), a.needsObject ? "*" : "");
		MIND_LOG("[homeostat] creature {} born ({} {}); actions (* = on an object):{}\n",
			p.creatureId, p.stage, p.female ? "female" : "male", acts);
	}

	void died(const MindPerception& p) override {
		MIND_LOG("[homeostat] creature {} died at brain tick {}; {}\n", p.creatureId, p.tick, debugState());
	}

	// 7. Speaking (vocalizations, owner 2026-11-10): what the creature says,
	// as a meaning the engine words for its age (CreatureSpeech.h). It
	// speaks of what just happened to it or what it meets, not all the time:
	//   "ow"     Pain or Injury rose sharply
	//   "yum"    Hunger fell while eating
	//   "love"   Sex Drive fell after acting on one of its own kind
	//   "grr"    Anger high and rising
	//   "greet"  a creature of a kind not seen for a while came into view
	//   "need"   now and then, a pressing need (the "express need" action
	//            says it too, through SAYN)
	//   "babble" babies and children, now and then, when content
	// Never asleep. A pause after anything said, and each kind of remark
	// has its own longer pause.
	struct SpeechState {
		std::array<float, NUM_DRIVES> prev{};
		bool havePrev = false;
		unsigned int lastSaid = 0;
		std::map<std::string, unsigned int> lastKind;
		std::map<int, unsigned int> lastSeenKind; // creature category -> mind tick last in view
	} speech;

	std::string chooseSpeech(const MindPerception& p, const std::array<float, NUM_DRIVES>& d) {
		SpeechState& s = speech;
		std::string say;
		unsigned int t = p.tick;
		auto ready = [&](const std::string& kind, unsigned int gap) {
			auto it = s.lastKind.find(kind);
			return it == s.lastKind.end() || t >= it->second + gap;
		};
		// creatures in view now (categories 36-39: norn, grendel, ettin, ...)
		std::map<int, bool> seenNow;
		for (auto& v : p.visible)
			if (v.category >= 36 && v.category <= 39)
				seenNow[v.category] = true;
		if (!p.asleep && s.havePrev && t >= s.lastSaid + 30) {
			auto rose = [&](int i, float by) { return d[i] - s.prev[i] > by; };
			auto fell = [&](int i, float by) { return s.prev[i] - d[i] > by; };
			if ((rose(PAIN, 0.12f) || rose(INJURY, 0.12f)) && ready("ow", 15))
				say = "ow";
			else if (fell(HUNGER, 0.06f) && p.currentDecision == prm.eatDecision && ready("yum", 60))
				say = "yum";
			else if (fell(SEX, 0.15f) && (p.currentDecision == 1 || p.currentDecision == 2) && p.currentAttention >= 36 && ready("love", 60))
				say = "love";
			else if (d[ANGER] > 0.7f && rose(ANGER, 0.03f) && ready("grr", 150))
				say = "grr";
			else if (lastUsedCategory >= 0 && t <= lastUsedTick + prm.nameFresh && (int)p.stage >= 1 && ready("name", prm.nameGap) && !watchingOff() &&
					 std::uniform_real_distribution<float>(0, 1)(rng) <
						 (std::any_of(p.visible.begin(), p.visible.end(), [](const SeenAgent& v) { return v.kin && v.stage >= 0 && v.stage <= 1; }) ? prm.nameTeachChance : prm.nameChance)) {
				// names the thing it has just used: often for a young one of its family
				say = fmt::format("name:{}", lastUsedCategory);
				lastUsedCategory = -1;
			} else {
				for (auto& [cat, yes] : seenNow) {
					auto it = s.lastSeenKind.find(cat);
					bool newcomer = it == s.lastSeenKind.end() || t > it->second + 600;
					if (newcomer && ready("greet", 120)) {
						say = fmt::format("greet:{}", cat - 35);
						break;
					}
				}
			}
			if (say.empty()) {
				int best = -1;
				float level = 0.8f;
				for (int i = 0; i < NUM_DRIVES; i++)
					if (d[i] > level) {
						level = d[i];
						best = i;
					}
				if (best >= 0 && ready("need", 250) && std::uniform_real_distribution<float>(0, 1)(rng) < 0.05f)
					say = fmt::format("need:{}:{}", best, (int)(level * 255));
				else if ((int)p.stage <= 1 && overallDrive(d) < 0.3f && ready("babble", 200) && std::uniform_real_distribution<float>(0, 1)(rng) < 0.02f)
					say = "babble";
			}
		}
		for (auto& [cat, yes] : seenNow)
			s.lastSeenKind[cat] = t;
		s.prev = d;
		s.havePrev = true;
		if (!say.empty()) {
			s.lastSaid = t;
			s.lastKind[say.substr(0, say.find(':'))] = t;
		}
		return say;
	}

	// Inborn temperament (creatures/Temperament.h): each trait scales some
	// settings by exp(k z). Survival needs are left alone; fear keeps at
	// least half its weight however bold the creature.
	std::array<float, 7> appliedTemperament{};
	std::array<float, 8> appliedMindGenes{};
	bool temperamentApplied = false;
	void applyTemperament(const std::array<float, 7>& z, const std::array<float, 8>& mg, int creatureId = 0) {
		if (temperamentApplied && z == appliedTemperament && mg == appliedMindGenes)
			return;
		temperamentApplied = true;
		appliedTemperament = z;
		appliedMindGenes = mg;
		prm = Params();
		// (a test's settings again: they were lost when a young creature's
		// temperament drifted and this ran again)
		if (devenv("OPENC2E_MIND_PARAMS"))
			overrideParams();
		// the mind's own heritable settings (MindGenes.h), each exp(k z)
		{
			auto g = [&](int i, float k) { return std::exp(k * mg[(size_t)i]); };
			prm.learningRate *= g(0, 0.3f); // learning
			prm.temperature *= g(1, -0.3f); // decisiveness
			prm.travelHalf *= g(2, 0.25f); // patience
			prm.exploreWant *= g(3, 0.35f); // wanderlust
			for (int d : {PAIN, INJURY, FEAR})
				prm.driveLearning[d] = g(4, 0.35f); // danger
			for (int d : {HUNGER, THIRST})
				prm.driveLearning[d] = g(5, 0.35f); // food
			prm.driveLearning[SUFFOCATE] = g(6, 0.35f); // water
			for (int d : {LONELY, CROWDED, ANGER})
				prm.driveLearning[d] = g(7, 0.35f); // company
		}
		auto f = [&](int t, float k) { return std::exp(k * z[t]); };
		// boldness (0): less fear, more drawn to the new
		prm.weights[FEAR] *= std::max(0.5f, f(0, -0.3f));
		// curiosity (1): novelty worth more, boredom bites harder
		prm.curiosity *= f(0, 0.15f) * f(1, 0.35f);
		prm.weights[BORED] *= f(1, 0.25f);
		// energy (2): restless sooner, tries more things
		prm.restlessRamp *= f(2, -0.3f);
		prm.temperature *= f(2, 0.2f);
		// sociability (3): loneliness bites harder, crowds less
		prm.weights[LONELY] *= f(3, 0.35f);
		prm.weights[CROWDED] *= f(3, -0.25f);
		// temper (4): anger bites harder
		prm.weights[ANGER] *= f(4, 0.3f);
		// reactivity (5): stress bites harder, moods swing, focus narrows
		// under need, and bad outcomes are learned faster than good ones
		prm.weights[STRESS] *= f(5, 0.3f);
		prm.moodRate = std::min(1.0f, prm.moodRate * f(5, 0.3f));
		prm.arousalFocus *= f(5, 0.25f);
		prm.worseLearning = f(5, 0.25f);
		prm.betterLearning = f(5, -0.1f);
		// steadiness (6): sticks with what it is doing
		prm.commitment *= f(6, 0.5f);
		// brain add-ons of a custom genome (BrainProfiles.h): their roles'
		// settings, as multipliers
		for (auto& [name, factor] : brainSettings) {
			if (name == "learningRate")
				prm.learningRate *= factor;
			else if (name == "betterLearning")
				prm.betterLearning *= factor;
			else if (name == "worseLearning")
				prm.worseLearning *= factor;
			else if (name == "unfeltLearningShare")
				prm.unfeltLearningShare = std::min(1.0f, prm.unfeltLearningShare * factor);
			else if (name == "curiosity")
				prm.curiosity *= factor;
			else if (name == "seekDiscount")
				prm.seekDiscount = std::min(1.0f, prm.seekDiscount * factor);
			else if (name == "forageShare")
				prm.forageShare *= factor;
			else if (name == "foodPlaceMax")
				prm.foodPlaceMax = (size_t)std::lround(prm.foodPlaceMax * factor);
			else if (name == "commitment")
				prm.commitment *= factor;
			else if (name == "temperature")
				prm.temperature *= factor;
			else {
				static std::set<std::string> told;
				if (told.insert(name).second)
					fmt::print("Warning: brain-profiles.txt names a setting the mind does not have: {}\n", name);
			}
		}
		if (devenv("OPENC2E_WATCH_CREATURES"))
			fmt::print("[homeostat] creature {} temperament z {:+.2f} {:+.2f} {:+.2f} {:+.2f} {:+.2f} {:+.2f} {:+.2f}: fear w {:.2f} lonely w {:.2f} bored w {:.2f} anger w {:.2f} "
					   "curiosity {:.3f} commitment {:.3f} worse/better learning {:.2f}/{:.2f}\n",
				creatureId, z[0], z[1], z[2], z[3], z[4], z[5], z[6], prm.weights[FEAR], prm.weights[LONELY], prm.weights[BORED], prm.weights[ANGER],
				prm.curiosity, prm.commitment, prm.worseLearning, prm.betterLearning);
	}

	MindOutput tick(const MindPerception& p) override {
		if (!paramsOverridden) {
			paramsOverridden = true;
			overrideParams();
		}
		wrapW = p.wrapWidth;
		if (p.brainSettings && *p.brainSettings != brainSettings) {
			brainSettings = *p.brainSettings;
			temperamentApplied = false;
		}
		applyTemperament(p.temperament, p.mindGenes, p.creatureId);
		MindOutput out = tickChoose(p);
		out.noveltyOutcome = pendingNovelty;
		pendingNovelty = 0;
		if (gaitWalk && (out.decision == 10 || out.decision == 11))
			out.decision += 16; // walk-a / walk-b: the same way, by gait
		out.say = chooseSpeech(p, normalize(p.drives));
		// for the Mind Kit: how it feels now, and every 5 ticks (2 s) a
		// moment of its mood, the last 450 (15 minutes)
		viewDrives = normalize(p.drives);
		viewD = overallDrive(viewDrives);
		viewTick = p.tick;
		viewX = p.x;
		viewY = p.y;
		if (moodLog.empty() || p.tick >= moodLog.back().tick + 5) {
			int strongest = -1;
			float most = 0;
			for (int i = 0; i < NUM_DRIVES; i++)
				if (viewDrives[i] > most)
					most = viewDrives[i], strongest = i;
			moodLog.push_back({p.tick, contentOf(viewD), strongest, most});
			while (moodLog.size() > 450)
				moodLog.pop_front();
		}
		return out;
	}

	// 0-1: how far the needs are from all at their worst
	float contentOf(float D) const {
		std::array<float, NUM_DRIVES> all; // (its own weights: temperament changes them)
		all.fill(1.0f);
		float worst = overallDrive(all);
		return worst > 0 ? std::clamp(1.0f - D / worst, 0.0f, 1.0f) : 1.0f;
	}

	bool view(MindView& v) const override {
		v.tick = viewTick;
		for (int i = 0; i < NUM_DRIVES && i < 17; i++) {
			v.drives[i] = viewDrives[i];
			v.weight[i] = prm.weights[i];
		}
		v.pullPower = prm.driveExpN;
		v.content = contentOf(viewD);
		v.x = viewX;
		v.y = viewY;
		v.choseAt = viewChose;
		v.busy = inEpisode;
		for (size_t k = 0; k < viewCandidates.size(); k++) {
			const Candidate& c = viewCandidates[k];
			MindView::Option o;
			o.category = c.category;
			o.decision = c.decision;
			o.who = c.who;
			o.want = c.want;
			o.novelty = c.novelty;
			o.score = c.score;
			for (int i = 0; i < NUM_DRIVES && i < 17; i++)
				o.ease[i] = viewEase[k][i];
			if (auto it = expectations.find((c.category + 1) * 64 + c.decision); it != expectations.end())
				o.tried = it->second.count;
			o.chosen = k == 0;
			o.food = c.food;
			o.forage = c.forage;
			o.route = c.route;
			o.escape = c.escape;
			o.scent = c.scent || !c.urge.empty();
			o.travel = c.seekCategory >= 0 || c.chain;
			o.place = c.place;
			v.options.push_back(o);
		}
		auto lesson = [&](int category, int decision, const std::string& who, const Expectation& e) {
			MindView::Lesson l;
			l.category = category;
			l.decision = decision;
			l.who = who;
			l.count = e.count;
			float good = 0;
			for (int i = 0; i < NUM_DRIVES && i < 17; i++) {
				l.delta[i] = e.delta[i];
				good -= prm.weights[i] * e.delta[i];
			}
			l.value = std::clamp(good * 2.0f, -1.0f, 1.0f);
			v.lessons.push_back(l);
		};
		for (auto& [key, e] : expectations)
			if (e.count > 0)
				lesson(key / 64 - 1, key % 64, "", e);
		for (auto& [who, byDecision] : individuals)
			for (auto& [decision, e] : byDecision)
				if (e.count > 0)
					lesson(-2, decision, who, e);
		// where it has been: the place map's cells, with the time it was last
		// in the wider stretch around each
		for (auto& [cell, edges] : placeMap) {
			MindView::Spot sp;
			sp.x = cell.first * prm.cellWidth;
			sp.y = cell.second * prm.cellHeight;
			sp.w = prm.cellWidth;
			sp.h = prm.cellHeight;
			float cx = sp.x + sp.w / 2, cy = sp.y + sp.h / 2;
			if (auto it = visited.find({(int)std::floor(cx / prm.visitBinWidth), (int)std::floor(cy / prm.visitBinHeight)}); it != visited.end())
				sp.last = it->second;
			v.visited.push_back(sp);
		}
		for (auto& f : foodPlaces)
			v.food.push_back({f.x, f.y, f.confidence, f.meals, f.tick});
		for (auto& l : links)
			v.rides.push_back({l.x, l.fromY, l.toY, l.uses});
		for (auto& [id, r] : rooms) {
			MindView::PlaceMemory pm;
			pm.room = id;
			pm.x = r.x;
			pm.floor = r.floor;
			pm.visits = r.visits;
			pm.last = r.last;
			pm.ticks = r.ticks;
			pm.felt = r.feelTicks >= prm.feelMinTicks;
			for (int i = 0; i < NUM_DRIVES && i < 17; i++)
				pm.feel[i] = roomFeel(r, i) * prm.favouriteHorizon; // over a stay of favouriteHorizon ticks
			pm.avoided = roomAvoided(r);
			for (int need : {PAIN, COLD, HOT, TIRED, SLEEPY, LONELY})
				if (-roomFeel(r, need) >= prm.favouriteMinEase && !pm.avoided && !r.water)
					pm.goodFor.push_back(need);
			for (auto& [category, sg] : r.seen)
				pm.things.push_back({category, sg.times});
			for (auto& [who, n] : r.met)
				pm.met.push_back({who, n});
			pm.here = id == roomNow;
			v.places.push_back(pm);
		}
		for (auto& [who, m] : metAt)
			if (float L = longingNow(m, lastTickForView); L >= 0.05f)
				v.longing.push_back({who, L});
		for (auto& m : moodLog)
			v.mood.push_back({m.tick, m.content, m.strongest, m.strength});
		v.outcomes.assign(outcomeLog.begin(), outcomeLog.end());
		return true;
	}

	MindOutput tickChoose(const MindPerception& p) {
		MindOutput out;
		if (pendingPassOver) {
			out.passOver = pendingPassOver;
			pendingPassOver = AgentRef();
		}
		std::array<float, NUM_DRIVES> d = normalize(p.drives);
		// Autumn appetite (owner, 2026-11-16: "should they eat more in autumn
		// to get through winter?"; the strength inherited, Temperament.h,
		// SeasonalRhythm): in autumn hunger is felt as if a share of the way
		// to full, so a creature looks for food sooner and eats past its
		// need; its own chemistry stores what it does not burn (glycogen,
		// fat) for the winter, when its body runs slower (Biochemistry.cpp).
		static const bool noSeasons = devenv("OPENC2E_NO_SEASONAL_BIOLOGY") != nullptr; // (to compare)
		if (!noSeasons && p.season == 2) {
			float a = std::min(prm.autumnAppetiteMax, prm.autumnAppetite * p.seasonalStrength);
			d[HUNGER] += (1.0f - d[HUNGER]) * a;
		}
		// Seasonal behavior (owner, 2026-11-16: "A, B, and E", with "life
		// stage as a mediating factor"; Temperament.h, SeasonalRhythm),
		// each times the creature's seasonal strength and its life stage's
		// weight for that effect:
		// - the breeding season: the sex drive felt stronger in spring, and
		//   weaker in winter (and conceiving likelier in spring, less so in
		//   winter: Breeding.cpp);
		// - winter bonds: loneliness felt stronger in winter (and the
		//   company of creatures it is bonded to warms it in the cold:
		//   Creature.cpp);
		// - winter rest: sleepiness felt stronger in winter;
		// - summer play: the new and untried worth more in summer.
		seasonalCuriosity = 1.0f;
		if (SeasonalRhythm::behaviorOn() && p.season >= 0) {
			float s = std::min(2.0f, p.seasonalStrength);
			auto w = [&](int effect) { return s * SeasonalRhythm::stageWeight(effect, p.stage); };
			if (p.season == 0)
				d[SEX] += (1.0f - d[SEX]) * std::min(0.5f, prm.springSex * w(SeasonalRhythm::SEASON_BREEDING));
			if (p.season == 3) {
				d[SEX] *= std::max(0.2f, 1.0f - prm.winterSexDamp * w(SeasonalRhythm::SEASON_BREEDING));
				d[LONELY] += (1.0f - d[LONELY]) * std::min(0.5f, prm.winterLonely * w(SeasonalRhythm::SEASON_BONDS));
				d[SLEEPY] += (1.0f - d[SLEEPY]) * std::min(0.4f, prm.winterSleepy * w(SeasonalRhythm::SEASON_REST));
			}
			if (p.season == 1)
				seasonalCuriosity = 1.0f + prm.summerPlay * w(SeasonalRhythm::SEASON_PLAY);
		}
		// Anticipating hunger (owner, 2026-11-16, from the citations pass:
		// regulation also acts ahead of need, allostasis; Ramsay & Woods
		// 2014). With food at hand (reachable, within anticipateNear px),
		// and not yet very hungry, hunger is felt a share of the way to full
		// for each cue that a long time without food is coming: dusk (night
		// next; half at night), sleepiness (a sleep is due) and scarcity (it
		// knows at most one good food place). So it eats while it can.
		// OPENC2E_NO_ANTICIPATION turns this off (for comparison).
		{
			static const bool noAnticipation = devenv("OPENC2E_NO_ANTICIPATION") != nullptr;
			if (!noAnticipation && d[HUNGER] < prm.starveAwake) {
				bool foodAtHand = false;
				for (auto& s : p.visible)
					if (s.edible >= prm.foodSeeEdible && s.reachable && s.distance <= prm.anticipateNear) {
						foodAtHand = true;
						break;
					}
				if (foodAtHand) {
					float cues = 0.0f;
					if (p.timeOfDay == 3)
						cues += 1.0f;
					else if (p.timeOfDay == 4)
						cues += 0.5f;
					if (d[SLEEPY] >= 0.5f)
						cues += 1.0f;
					int good = 0;
					for (auto& fp : foodPlaces)
						good += fp.confidence >= 0.5f;
					if (good <= 1)
						cues += 1.0f;
					if (cues > 0.0f)
						d[HUNGER] += (1.0f - d[HUNGER]) * std::min(prm.anticipateMax, prm.anticipate * cues);
				}
			}
		}
		float D = overallDrive(d);
		lastTickForView = p.tick;

		for (auto& h : habituation)
			h += prm.habituationRecovery * (1.0f - h);

		for (auto& inst : p.newInstincts)
			applyInstinct(p, inst);
		visited[{(int)std::floor(p.x / prm.visitBinWidth), (int)std::floor(p.y / prm.visitBinHeight)}] = p.tick;
		updatePlaceMap(p);
		updatePlaces(p, d);
		learnFromFall(p);
		watchOthers(p);
		if (lastTick != 0 && p.tick > lastTick) {
			if (d[HUNGER] >= prm.restlessHunger)
				hungryTicks += (float)(p.tick - lastTick);
			else if (d[HUNGER] < prm.restlessHunger * 0.5f)
				hungryTicks = 0; // fed
		}
		lastTick = p.tick;
		// in or out of the water only once it has lasted waterSettle ticks: a
		// creature against a wall between a water room and a dry one read
		// wet and dry by turns every tick, and learned the device it had just
		// pressed as an exit and a flooder over and over, pressing it on
		// (the gene lab's pumps, owner's screencast 2026-11-13)
		if (p.inRoom && p.inWater != waterRaw) {
			waterRaw = p.inWater;
			waterRawSince = p.tick;
		}
		const bool inWaterSettled = (p.inRoom && waterRaw != wasInWater && p.tick >= waterRawSince + prm.waterSettle) ? waterRaw : wasInWater;
		// (outside every room is not out of the water: a norn that sank
		// through the underground pond's bed learned a button as its exit)
		if (wasInWater && !inWaterSettled && p.inRoom && lastObjectUse.category >= 0 && p.tick < lastObjectUseTick + prm.exitWindow) {
			bool known = false;
			for (auto& e : exits)
				if (e.category == lastObjectUse.category && e.decision == lastObjectUse.decision && std::fabs(e.x - lastObjectUse.x) < 200.0f && std::fabs(e.y - lastObjectUse.y) < prm.sameLevel) {
					e.uses++;
					known = true;
				}
			if (!known) {
				if (exits.size() >= prm.exitMax)
					exits.erase(exits.begin());
				ExitLink e = lastObjectUse;
				e.uses = 1;
				exits.push_back(e);
			}
			MIND_LOG("[homeostat] creature {} t{}: out of the water; remembers {}@{} at {:.0f},{:.0f} as an exit\n", p.creatureId, p.tick,
				actionName(lastObjectUse.decision), lastObjectUse.category, lastObjectUse.x, lastObjectUse.y);
		}
		// (only working a device counts: activate 1 or 2, deactivate; eating
		// or approaching near one taught creatures nonsense flooders)
		if (!wasInWater && inWaterSettled && lastObjectUse.category >= 0 && lastObjectUse.decision >= 1 && lastObjectUse.decision <= 3 && p.tick < lastObjectUseTick + prm.exitWindow) {
			bool known = false;
			for (auto& f : flooders)
				if (f.category == lastObjectUse.category && f.decision == lastObjectUse.decision && std::fabs(f.x - lastObjectUse.x) < prm.flooderNear && std::fabs(f.y - lastObjectUse.y) < prm.sameLevel) {
					f.uses++;
					known = true;
				}
			if (!known) {
				if (flooders.size() >= prm.exitMax)
					flooders.erase(flooders.begin());
				ExitLink f = lastObjectUse;
				f.uses = 1;
				flooders.push_back(f);
			}
			MIND_LOG("[homeostat] creature {} t{}: in the water; remembers {}@{} at {:.0f},{:.0f} as a flooder\n", p.creatureId, p.tick,
				actionName(lastObjectUse.decision), lastObjectUse.category, lastObjectUse.x, lastObjectUse.y);
		}
		wasInWater = inWaterSettled;
		// boat rides
		if (p.riding && !wasRiding) {
			rideStartX = p.x;
			rideDecision = (current.category == prm.boatCategory) ? current.decision : -1;
		} else if (!p.riding && wasRiding && rideDecision >= 0 && std::fabs(dxTo(p, rideStartX)) > prm.boatRide) {
			BoatLink& bl = boatLinks[rideDecision];
			bl.decision = rideDecision;
			bl.dx = (bl.dx * bl.rides + (-dxTo(p, rideStartX))) / (bl.rides + 1);
			bl.rides++;
			MIND_LOG("[homeostat] creature {} t{}: boat ride with {} took it {:.0f} px\n", p.creatureId, p.tick, actionName(rideDecision), -dxTo(p, rideStartX));
		}
		// travel links: a device use followed by a ride (or a floor change)
		// that ends somewhere quite different
		if (pendingLink.category >= 0) {
			if (p.riding)
				pendingRode = true;
			float moved = std::fabs(dxTo(p, pendingLink.fromX)) + std::fabs(p.y - pendingLink.fromY);
			bool landed = pendingRode ? !p.riding : std::fabs(p.y - pendingLink.fromY) > prm.minRide;
			// a ride is judged where it ends: a short one (the cave lift by the
			// waterwheel rises only 170 px) used to leave the link pending, and
			// the walk after it, up to linkWindow ticks, was taken for the
			// ride, so "press the lift" came to lead across the world and
			// hungry creatures crowded in its shaft riding it up and down
			// (owner, 2026-11-13)
			bool rideOver = pendingRode && !p.riding;
			// a lift, or a call button, that gave no ride took it nowhere:
			// walking off after a press is not the device's doing
			bool rideOnly = pendingLink.category == prm.liftCategory || pendingLink.category == prm.buttonCategory;
			if (rideOnly && !pendingRode && std::fabs(dxTo(p, pendingLink.fromX)) > prm.atPlace * 2)
				rideOver = true;
			if (rideOnly && !pendingRode)
				landed = false;
			if (landed && moved >= prm.linkMove && p.tick < pendingLinkTick + prm.linkWindow) {
				bool known = false;
				for (auto& tl : travelLinks)
					if (tl.category == pendingLink.category && tl.decision == pendingLink.decision &&
						std::fabs(tl.fromX - pendingLink.fromX) < 200.0f && std::fabs(tl.fromY - pendingLink.fromY) < prm.sameLevel) {
						tl.toX = (tl.toX * tl.uses + p.x) / (tl.uses + 1);
						tl.toY = (tl.toY * tl.uses + p.y) / (tl.uses + 1);
						tl.uses++;
						known = true;
					}
				if (!known) {
					if (travelLinks.size() >= prm.linkMax)
						travelLinks.erase(travelLinks.begin());
					TravelLink tl = pendingLink;
					tl.toX = p.x;
					tl.toY = p.y;
					tl.uses = 1;
					travelLinks.push_back(tl);
				}
				MIND_LOG("[homeostat] creature {} t{}: learned travel link {}@{} from {:.0f},{:.0f} to {:.0f},{:.0f}\n", p.creatureId, p.tick,
					actionName(pendingLink.decision), pendingLink.category, pendingLink.fromX, pendingLink.fromY, p.x, p.y);
				pendingLink.category = -1;
			} else if (rideOver || p.tick >= pendingLinkTick + prm.linkWindow) {
				// a known link that took it nowhere this time: twice, and it is
				// forgotten (a short ride counts: links wrongly learned from one
				// are forgotten this way)
				for (auto it = travelLinks.begin(); it != travelLinks.end(); ++it)
					if (it->category == pendingLink.category && it->decision == pendingLink.decision &&
						std::fabs(it->fromX - pendingLink.fromX) < 200.0f && std::fabs(it->fromY - pendingLink.fromY) < prm.sameLevel) {
						if (++it->fails >= 2) {
							MIND_LOG("[homeostat] creature {} t{}: forgot travel link {}@{} from {:.0f},{:.0f} (it took it nowhere)\n", p.creatureId, p.tick,
								actionName(it->decision), it->category, it->fromX, it->fromY);
							travelLinks.erase(it);
						}
						break;
					}
				pendingLink.category = -1;
			}
		}
		if (blindPending) {
			bool liftSeen = false;
			for (auto& sa : p.visible)
				if (sa.category == prm.liftCategory)
					liftSeen = true;
			if (liftSeen || p.riding || std::fabs(p.y - blindY) > prm.sameLevel)
				blindPending = false; // it brought something
			else if (p.tick > blindTick + prm.blindPressWait) {
				dudButtons.erase(std::remove_if(dudButtons.begin(), dudButtons.end(), [&](auto& d) { return p.tick >= d.until; }), dudButtons.end());
				if (dudButtons.size() < 16)
					dudButtons.push_back({blindX, blindY, p.tick + prm.dudButtonTicks});
				MIND_LOG("[homeostat] creature {} t{}: a call button at {:.0f},{:.0f} brought no lift; no blind presses there for {} ticks\n",
					p.creatureId, p.tick, blindX, blindY, prm.dudButtonTicks);
				blindPending = false;
			}
		}
		if (callPending) {
			bool boatHere = false;
			for (auto& sa : p.visible)
				if (sa.category == prm.boatCategory && sa.reachable)
					boatHere = true;
			bool done = boatHere || p.tick > callPressTick + prm.boatCallWait;
			if (done) {
				CallButton* cb = nullptr;
				for (auto& c : callButtons)
					if (std::fabs(c.x - callPressX) < 200.0f && std::fabs(c.y - callPressY) < prm.sameLevel)
						cb = &c;
				if (!cb) {
					if (callButtons.size() >= 16)
						callButtons.erase(callButtons.begin());
					callButtons.push_back({callPressX, callPressY, 0, 0});
					cb = &callButtons.back();
				}
				(boatHere ? cb->worked : cb->failed)++;
				MIND_LOG("[homeostat] creature {} t{}: pressed a button at {:.0f},{:.0f} to call a boat: {}\n", p.creatureId, p.tick, callPressX, callPressY,
					boatHere ? "the boat came (a boat switch)" : "no boat came");
				callPending = false;
			}
		}
		if (p.riding)
			boatTryDecision = -1; // the press worked
		else if (boatTryDecision >= 0 && p.tick > boatTryTick + prm.boatNoRide) {
			boatFlipUntil = p.tick + prm.boatFlip; // nothing happened: try the other one
			boatTryDecision = -1;
		}
		wasRiding = p.riding;
		bool foodInSight = false;
		// Food out of reach (fruit hanging over the floor, food behind a
		// wall) marks a place it may later be got, but neither renews a
		// place already known nor counts as finding food there: hungry
		// creatures waited for good at the seam corner (room 180) under the
		// tomato plants of room 118, whose fruit hangs about 100 px up,
		// the place renewed at every look (owner, 2026-11-16: "mind gives
		// up waiting"). Arriving and finding only such food halves the
		// place as finding nothing does, so it is given up and others tried.
		static const bool noReachCheck = devenv("OPENC2E_NO_FOOD_REACH_CHECK") != nullptr; // (to compare)
		for (auto& s : p.visible)
			if (s.edible >= prm.foodSeeEdible) {
				if (s.reachable || noReachCheck) {
					rememberFood(s.x, s.y, p.tick, 0.6f, false, s.category);
					foodInSight = true;
				} else if (!knowsFoodNear(s.x, s.y))
					rememberFood(s.x, s.y, p.tick, prm.unreachableFoodConfidence, false, s.category);
			}
		for (auto& fp : foodPlaces) {
			if (std::fabs(dxTo(p, fp.x)) < prm.foodArrived && std::fabs(fp.y - p.y) < prm.sameLevel && !foodInSight &&
				p.tick >= fp.checked + prm.foodRecheckTicks) {
				fp.confidence *= 0.5f; // came here and found nothing (for now)
				fp.checked = p.tick;
			}
		}
		size_t foodBefore = foodPlaces.size();
		foodPlaces.erase(std::remove_if(foodPlaces.begin(), foodPlaces.end(), [](const FoodPlace& f) { return f.confidence < 0.05f; }), foodPlaces.end());
		if (foodPlaces.size() != foodBefore)
			foodTrip = -1; // indices moved
		for (auto& s : p.visible) {
			places[s.category] = {s.x, s.y, p.tick};
			auto sc = scents.find(s.category);
			if (sc == scents.end() || s.edible > sc->second + 0.05f) {
				scents[s.category] = s.edible;
				applyScent(s.category, s.edible);
			}
		}

		// 7. facial expression from current affect; only sent when it changes
		mood *= prm.moodDecay;
		int face = chooseFace(d, D);
		if (face != lastFace) {
			out.expression = face;
			MIND_LOG("[homeostat] creature {} t{}: face {} -> {} (mood {:+.3f}, valence {:+.3f}, Anger {:.2f})\n",
				p.creatureId, p.tick, lastFace, face, mood, valence, d[ANGER]);
			lastFace = face;
		}

		for (auto& s : p.stimuli) {
			stimuliSeen++;
			for (int k = 0; k < 4; k++) {
				int c = s.feltChem[k];
				float amt = s.feltAmount[k] / 255.0f;
				if (c >= prm.driveIncreaseChem && c < prm.driveIncreaseChem + NUM_DRIVES) {
					felt[c - prm.driveIncreaseChem] += amt;
					feltAny[c - prm.driveIncreaseChem] = true;
				} else if (c >= prm.driveDecreaseChem && c < prm.driveDecreaseChem + NUM_DRIVES) {
					felt[c - prm.driveDecreaseChem] -= amt;
					feltAny[c - prm.driveDecreaseChem] = true;
				} else if (amt > 0 && (c == 140 || c == 141 || c == 143 || c == 144 || c == 145 || (c >= 234 && c <= 239) || c == 170)) {
					// a poison taken in is felt as harm, like a bitter, burning
					// taste (alcohol at half): one bite of the cyanide berries
					// teaches the creature to leave them alone
					felt[PAIN] += amt * (c == 170 ? 0.5f : 1.0f) * prm.poisonPain;
					feltAny[PAIN] = true;
				}
			}
			if (s.numbered)
				MIND_LOG("[homeostat] creature {} stimulus {} #{}\n", p.creatureId, s.command, s.id);
			else
				MIND_LOG("[homeostat] creature {} stimulus {} chems {}:{} {}:{} {}:{} {}:{}\n", p.creatureId, s.command,
					s.chem[0], s.amount[0], s.chem[1], s.amount[1], s.chem[2], s.amount[2], s.chem[3], s.amount[3]);
		}

		if (!p.actions || p.actions->empty())
			return out;

		// The body's drives read all zero until its chemistry has run once
		// (just born or just loaded): nothing to decide or learn from yet.
		if (!drivesSeen) {
			bool any = false;
			for (auto v : p.drives)
				any = any || v != 0;
			if (!any)
				return out;
			drivesSeen = true;
		}

		// hunger, water, or a place or need that has just woken it keep it
		// from dropping off where it stands, as they keep it from choosing
		// sleep (it dropped off and was woken again every second or so)
		out.resistSleep = p.tick < wokenUntil || badSleepAt(p) || starvingAwake(d) || (p.inWater && !p.breathesWater);

		// Dropped off where it stood: a very sleepy body's own reflex (event
		// 69, which interrupts whatever it was doing). It is a sleep like any
		// other: let it sleep. (Not noticed before, the mind's next choice woke
		// it within a second or two, as sleepy as ever, and a tired norn's day
		// was three seconds of sleep, one thing done, three seconds of sleep;
		// reported 2026-11-22.)
		if (p.dozing && !(inEpisode && current.category < 0 && current.decision == prm.sleepDecision)) {
			if (inEpisode)
				endEpisode(p, d, D, "dropped off");
			current = Candidate{};
			current.category = -1;
			current.decision = prm.sleepDecision;
			inEpisode = true;
			fellAsleep = false;
			episodeNudges.clear();
			episodeApprovalStart = approvalNow(p);
			episodeAgent = AgentRef();
			episodeTicks = 0;
			closest = 1e9f;
			lastProgress = 0;
			episodeWalledStart = p.walledCount;
			episodeStartX = startX = p.x;
			episodeStartY = startY = p.y;
			startDrives = sleepStartDrives = d;
			startD = D;
			felt.fill(0.0f);
			feltAny.fill(false);
			MIND_LOG("[homeostat] creature {} t{}: dropped off to sleep\n", p.creatureId, p.tick);
			thinkAbout(p, current, out);
			return out;
		}

		// Asleep: let the sleep script run unless something urgent wakes us.
		if (inEpisode && current.category < 0 && current.decision == prm.sleepDecision) {
			episodeTicks++;
			int urgent = wakeReason(d);
			// asleep in water (it rose round the creature, or it dozed off in
			// the shallows): wake at once, as for suffocation; two batch norns
			// drowned asleep in the seas
			if (p.inWater && urgent < 0)
				urgent = SUFFOCATE;
			if (p.asleep)
				fellAsleep = true;
			bool done = p.actionFinished && episodeTicks > 1;
			if (!done && urgent < 0 && episodeTicks < prm.maxSleepTicks)
				return out;
			if (done && !fellAsleep && episodeTicks <= prm.refusedSleepTicks) {
				noSleepUntil = std::max(noSleepUntil, (unsigned int)(p.tick + prm.noSleepAfterRefusal));
				MIND_LOG("[homeostat] creature {} t{}: sleep refused (not tired enough); not offered for {} ticks\n", p.creatureId, p.tick,
					prm.noSleepAfterRefusal);
				sleepRefused = true; // not a sleep: nothing to learn about sleeping
			}
			// lay down only to rest (tired but not sleepy, sleep.cos): not a
			// sleep either. Learned as one, a few rests taught a norn that
			// sleep does not cure sleepiness, and it stopped choosing it
			// (reported 2026-11-22)
			if (done && !fellAsleep)
				sleepRefused = true;
			endEpisode(p, d, D, done ? "finished" : urgent >= 0 ? DRIVE_NAMES[urgent] : "time");
			sleepRefused = false;
			if (urgent >= 0) {
				MIND_LOG("[homeostat] creature {} t{}: woken by {}\n", p.creatureId, p.tick, DRIVE_NAMES[urgent]);
				noSleepUntil = wokenUntil = p.tick + prm.noSleepAfterWaking;
				if (urgent == COLD || urgent == HOT) {
					badSleepX = p.x;
					badSleepY = p.y;
					badSleepUntil = p.tick + prm.badSleepTicks;
				}
			}
		}

		// Spoken to, or touched by the hand: think again what to do (a choice
		// like any other: a starving creature may well go on eating)
		if (p.heardAge < 0 || p.heardAge < lastHeardAge)
			heardDone = 0; // something new said
		lastHeardAge = p.heardAge;
		if (p.calledAge < 0 || p.calledAge > 2)
			calledIn = false;
		else if (!calledIn) {
			calledIn = true;
			if (inEpisode && !p.riding && !p.inWater && !p.asleep && !(current.category < 0 && current.decision == prm.sleepDecision))
				endEpisode(p, d, D, "called");
		}
		// praise and blame for doing as it was told, or for not listening
		if (p.handPat || p.handSlap) {
			if (!obeyed.empty() && p.tick < obeyedTick + prm.praiseTicks) {
				for (size_t i : obeyed)
					if (i < nudges.size())
						nudges[i].approvalSum += p.handPat ? 1.0f : -1.0f;
				obeyed.clear();
			} else if (p.handSlap && p.heardVerb >= 0 && p.heardAge >= 0 && p.heardAge < prm.praiseTicks)
				for (auto& n : nudges)
					if (std::find(n.verbs.begin(), n.verbs.end(), p.heardVerb) != n.verbs.end())
						n.approvalSum += 0.5f;
		}

		// Is the current episode over?
		if (inEpisode) {
			episodeTicks++;
			if (current.looking && (p.calledAge < 0 || p.calledAge >= prm.callTicks))
				endEpisode(p, d, D, "looked");
		}
		if (inEpisode) {
			bool objectGone = current.category >= 0 && !isVisible(p, current.category);
			// or another of its kind in its place: the engine sees one thing
			// of each kind, and when the one acted on is gone (eaten) or out
			// of sight it takes up another, which the creature then walked to
			// though it might be out of reach (on a ledge above, past a
			// wall); a third of meals ended "stalled", many after the meal
			// itself had been eaten (2026-11-14)
			// (developer aid: OPENC2E_NO_STALLFIX=1 turns this and the
			// "helped" exception below off, for comparison)
			static const bool noStallFix = devenv("OPENC2E_NO_STALLFIX") != nullptr;
			if (current.category >= 0 && !objectGone && episodeAgent && !noStallFix) {
				bool same = false;
				for (auto& s : p.visible)
					if (s.category == current.category && s.agent == episodeAgent)
						same = true;
				objectGone = !same;
			}
			bool finished = p.actionFinished && episodeTicks > 1;
			int limit = current.category >= 0 ? prm.maxObjectEpisodeTicks : prm.maxEpisodeTicks;
			bool stalled = false;
			if (current.category >= 0 && !objectGone) {
				float dist = 0;
				for (auto& s : p.visible)
					if (s.category == current.category)
						dist = s.distance;
				if (dist < closest - 5.0f) {
					closest = dist;
					lastProgress = episodeTicks;
				} else if (episodeTicks - lastProgress >= prm.stallTicks) {
					stalled = true;
					// developer aid: OPENC2E_WATCH_STALLS prints each stalled
					// action on an object: where both were and how far it got
					static const bool watchStalls = devenv("OPENC2E_WATCH_STALLS") != nullptr;
					if (watchStalls)
						for (auto& s : p.visible)
							if (s.category == current.category)
								fmt::print("[homeostat] creature {} t{}: STALL {}@{} creature {:.0f},{:.0f} (from {:.0f},{:.0f}) target {:.0f},{:.0f} dx {:.0f} dy {:.0f} dist {:.0f} closest {:.0f} reachable {} edible {:.2f} walled {}\n",
									p.creatureId, p.tick, actionName(current.decision), current.category, p.x, p.y, episodeStartX, episodeStartY, s.x, s.y,
									s.x - p.x, s.y - p.y, s.distance, closest, s.reachable ? 1 : 0, s.edible, p.walledCount - episodeWalledStart);
				}
			}
			// Walking into a wall, whatever the action (owner, 2026-11-13:
			// creatures paced into walls for long spells). The blocked-way
			// memory knew only its own walks east and west; walking up to
			// something on the far side of a wall (another creature, food, a
			// smell, or a genome action that walks to whatever it last
			// noticed) went on into the wall. Pushed back twice in one
			// action: it stops, keeps away from walking that way here for a
			// while, and an object it was walking to is passed over.
			bool walled = p.walledCount >= episodeWalledStart + 2 && (p.walledDir == 10 || p.walledDir == 11);
			// A walk's bumps behind it, with no way made toward that side, are
			// the creature turning at a wall, not a wall ahead: one standing
			// against a wall swings its foot past it as it turns to walk away,
			// and both ways were marked blocked there, so it never left.
			// Creatures starved pinned to the east wall of the ledge under the
			// east dock (room 14, x 6320), 4-5 of them at all times (owner,
			// 2026-11-16). Turning itself moves a creature's middle 14-20 px.
			// A creature whose "west" walk really goes east (its genes) makes
			// way that side first (200 px seen), and is still caught as before.
			{
				int walkDir = current.decision == 26 ? 10 : current.decision == 27 ? 11 : current.decision;
				static const bool noTurnBump = devenv("OPENC2E_NO_TURN_BUMP") != nullptr; // (to compare)
				if (walled && !noTurnBump && current.category < 0 && (walkDir == 10 || walkDir == 11) && p.walledDir != walkDir) {
					float towardBump = p.walledDir == 11 ? -dxTo(p, episodeStartX) : dxTo(p, episodeStartX);
					if (towardBump < prm.turnBumpWay)
						walled = false;
				}
			}
			if (walled && !finished) {
				noteBlocked(p, p.walledDir);
				// and the walk itself, if it was one: which way a creature's
				// travel actions go depends on its genes (some norns' "west"
				// walks east), so blocking only the way it was going left the
				// same action free; norns in the gene lab's lower room walked
				// into its end walls about 150 times a minute each
				if ((current.decision == 10 || current.decision == 11) && current.category < 0 && current.decision != p.walledDir)
					noteBlocked(p, current.decision);
				if (current.category >= 0 && episodeAgent && !current.chain)
					pendingPassOver = episodeAgent;
				// the place map's ways out of here that go that way count it
				// as a failure (twice over: a wall is plain), so a way wrongly
				// learned through the rock is soon left out of routes planned
				// from elsewhere too; one through a door that was shut comes
				// back as it is walked again
				if (auto it = placeMap.find(cellOf(p.x, p.y)); it != placeMap.end())
					for (auto& e : it->second)
						if (e.dir == p.walledDir)
							e.fails += 2;
				MIND_LOG("[homeostat] creature {} t{}: {}@{} walked into a wall going {} at x {:.0f}; stops\n", p.creatureId, p.tick,
					actionName(current.decision), current.category, p.walledDir == 10 ? "west" : "east", p.x);
				endEpisode(p, d, D, "walled", true);
			} else if (finished || objectGone || stalled || episodeTicks >= limit) {
				endEpisode(p, d, D, finished ? "finished" : objectGone ? "object gone" : stalled ? "stalled" : "time", stalled);
			} else {
				return out; // let the current action run
			}
		}

		// Riding a lift (or another vehicle): stay put until it lets us off,
		// then remember where it took us.
		if (p.riding) {
			if (!riding) {
				riding = true;
				rideFromX = p.x;
				rideFromY = p.y;
			}
			return out;
		}
		if (riding) {
			riding = false;
			learnRide(p);
		}

		// Choose the next action.
		std::vector<Candidate> candidates = evaluate(p, d, D);
		if (p.tick < noSleepUntil || badSleepAt(p) || (p.inWater && !p.breathesWater) || starvingAwake(d)) // (never sleep in water, unless it breathes it)
			candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
								 [&](const Candidate& c) { return c.category < 0 && c.decision == prm.sleepDecision; }),
				candidates.end());
		if (!p.inWater && !flooders.empty())
			candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const Candidate& c) { return isFlooder(p, c.category, c.decision); }),
				candidates.end());
		// a walk found blocked here, whatever it is for: food, smell and
		// route trips chose their walk without asking, and a starving norn in
		// the gene lab's lower room walked east into its end wall for good,
		// food being somewhere east (2026-11-13)
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const Candidate& c) {
			return c.category < 0 && (c.decision == 10 || c.decision == 11) && (isBlocked(p, c.decision) || wallAhead(p, c.decision));
		}), candidates.end());
		if (!futile.empty()) {
			futile.erase(std::remove_if(futile.begin(), futile.end(), [&](const Futile& f) { return p.tick >= f.until; }), futile.end());
			candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const Candidate& c) { return isFutile(p, c.category, c.decision); }),
				candidates.end());
		}
		if (candidates.empty())
			return out;
		Candidate choice = pick(candidates, D);
		noteChoiceForView(candidates, choice, d, D);
		// Asleep but not choosing to sleep (an interrupted sleep script, or
		// a world saved mid-sleep): wake up first, or no other action can run.
		bool chooseSleep = choice.category < 0 && choice.decision == prm.sleepDecision;
		if (p.asleep && !chooseSleep)
			out.sleep = 0;
		logChoice(p, choice, candidates, D);
		thinkAbout(p, choice, out);
		if (choice.urge.rfind("bring:", 0) == 0 && choice.decision == 6)
			bringPickupCategory = choice.category;
		if (choice.urge.rfind("tend:", 0) == 0 && choice.decision == 1 && !choice.who.empty()) {
			tendedUntil[choice.who] = p.tick + prm.tendAgain;
			if (devenv("OPENC2E_WATCH_TEND"))
				fmt::print("[homeostat] creature {} t{}: TEND tickles {}\n", p.creatureId, p.tick, choice.who);
		}

		// (only working the lift can take a creature up or down: activate 1
		// or 2, deactivate; see learnRide)
		if (choice.category == prm.liftCategory && ((choice.decision >= 1 && choice.decision <= 3) || devenv("OPENC2E_KEEP_BADLINKS")))
			lastLiftDecision = choice.decision;
		if (choice.place == "leave:Hunger") {
			starveLeaveTick = p.tick;
			starveLeaveX = p.x;
			starveLeaveY = p.y;
		}
		current = choice;
		inEpisode = true;
		fellAsleep = false;
		episodeNudges.clear();
		for (size_t i = 0; i < nudges.size(); i++)
			if (nudgeHolds(nudges[i], p, choice.category, choice.decision)) {
				episodeNudges.push_back(i);
				if (!nudges[i].verbs.empty()) {
					if (obeyedTick != p.tick)
						obeyed.clear();
					obeyed.push_back(i);
					obeyedTick = p.tick;
					if (devenv("OPENC2E_WATCH_OBEY"))
						fmt::print("[homeostat] creature {} t{}: OBEYS {} (effect {:+.2f}, {} tries)\n", p.creatureId, p.tick, actionName(choice.decision), nudges[i].effect(), nudges[i].tries);
				}
			}
		current.looking = choice.decision == 0 && choice.category >= 0 && choice.category == p.calledCategory && p.calledAge >= 0 && p.calledAge < prm.callTicks;
		if (current.looking && devenv("OPENC2E_WATCH_OBEY"))
			fmt::print("[homeostat] creature {} t{}: LOOKS at category {}\n", p.creatureId, p.tick, choice.category);
		episodeApprovalStart = approvalNow(p);
		if (choice.blindPress && !blindPending) {
			blindPending = true;
			blindX = p.x;
			blindY = p.y;
			blindTick = p.tick;
		}
		episodeAgent = AgentRef();
		if (choice.category >= 0)
			for (auto& s : p.visible)
				if (s.category == choice.category)
					episodeAgent = s.agent;
		// (only working a device can take a creature somewhere: activate 1 or
		// 2, deactivate. A grendel that ate the airlock's land door and was
		// soon let out far away learned "eat the door" as a way to travel,
		// and bit it for ever on its food trips; owner, 2026-11-10)
		if ((choice.category == prm.boatCategory || std::find(prm.escapeDevices.begin(), prm.escapeDevices.end(), choice.category) != prm.escapeDevices.end()) &&
			choice.decision >= 1 && choice.decision <= 3) {
			// working the same device again from the same spot while the last
			// try is still being judged: that try goes on being judged (a
			// creature pressing the west lab's fish-egg machine every few
			// ticks as a route step restarted the judging each time, so its
			// wrong link was never forgotten: 55 presses in a row, 5 eggs
			// each; 2026-11-14)
			bool again = pendingLink.category == choice.category && pendingLink.decision == choice.decision &&
				std::fabs(dxTo(p, pendingLink.fromX)) < 200.0f && std::fabs(p.y - pendingLink.fromY) < prm.sameLevel;
			if (!again) {
				pendingLink = {p.x, p.y, 0, 0, choice.category, choice.decision, 0};
				pendingLinkTick = p.tick;
				pendingRode = false;
			}
		}
		if (choice.boatCall) {
			callPressX = p.x;
			callPressY = p.y;
			callPressTick = p.tick;
			callPending = true;
		}
		if (choice.category == prm.boatCategory && boatTryDecision < 0) {
			boatTryDecision = choice.decision;
			boatTryTick = p.tick;
		}
		if (choice.category >= 0 && std::find(prm.escapeDevices.begin(), prm.escapeDevices.end(), choice.category) != prm.escapeDevices.end()) {
			// (only devices can be exits: leaving the water just after hitting a
			// norn taught one creature that hitting norns is a way out)
			lastObjectUse = {p.x, p.y, choice.category, choice.decision, 0};
			lastObjectUseTick = p.tick;
		}
		episodeTicks = 0;
		closest = 1e9f;
		lastProgress = 0;
		episodeWalledStart = p.walledCount;
		episodeStartX = p.x;
		episodeStartY = p.y;
		startDrives = d;
		startD = D;
		startX = p.x;
		startY = p.y;
		felt.fill(0.0f);
		feltAny.fill(false);
		if (chooseSleep)
			sleepStartDrives = d;

		if (choice.category >= 0)
			out.attentionCategory = choice.category;
		if (choice.target)
			out.attentionAgent = choice.target;
		out.decision = choice.decision;
		return out;
	}

	// World saving: one fact per line, readable and hand-editable.
	// (learned numbers are written to 4 significant figures: at 9, with 145
	// creatures, the save was 28 MB; 2026-11-13)
	std::string saveState() const override {
		std::string s = fmt::format("statever 9\nvalence {}\nmood {}\nepisodes {}\nstimuli {}\n", valence, mood, episodes, stimuliSeen);
		s += "drift";
		for (float f : drift)
			s += fmt::format(" {:.4g}", f);
		s += "\nhabituation";
		for (float f : habituation)
			s += fmt::format(" {:.4g}", f);
		s += "\n";
		for (auto& [key, e] : expectations) {
			// (one never tried and with nothing in it is not written: as
			// good as absent)
			if (e.count <= 0 && e.prior <= 0 && std::all_of(std::begin(e.delta), std::end(e.delta), [](float f) { return f == 0.0f; }))
				continue;
			// key = (category + 1) * 64 + decision
			s += fmt::format("expect {} {} {}", key / 64 - 1, key % 64, e.count);
			for (float f : e.delta)
				s += fmt::format(" {:.4g}", f);
			s += "\n";
			if (e.prior > 0)
				s += fmt::format("prior {} {} {}\n", key / 64 - 1, key % 64, e.prior);
			if (e.errFast > 0 || e.errSlow > 0)
				s += fmt::format("lp {} {} {:.4g} {:.4g}\n", key / 64 - 1, key % 64, e.errFast, e.errSlow);
		}
		for (auto& [who, decisions] : individuals)
			for (auto& [decision, e] : decisions) {
				// (one dealing only is not kept: two thirds of these records,
				// each counting a quarter against the general expectation)
				if (e.count <= 1)
					continue;
				s += fmt::format("individual {} {} {}", who, decision, e.count);
				for (float f : e.delta)
					s += fmt::format(" {:.4g}", f);
				s += "\n";
			}
		for (auto& [decision, e] : innateDefaults) {
			s += fmt::format("innate {} {}", decision, e.prior);
			for (float f : e.delta)
				s += fmt::format(" {:.4g}", f);
			s += "\n";
		}
		for (auto& sig : appliedInstincts)
			s += fmt::format("instinct {}\n", sig);
		for (auto& n : nudges)
			if (n.tries > 0)
				s += fmt::format("nudge {} {} {:.4g}\n", n.signature, n.tries, n.approvalSum);
		for (auto& [sig, st] : savedNudges)
			s += fmt::format("nudge {} {} {:.4g}\n", sig, st.first, st.second);
		for (auto& l : links)
			s += fmt::format("link {} {} {} {} {}\n", l.x, l.fromY, l.toY, l.decision, l.uses);
		for (auto& [category, edible] : scents)
			s += fmt::format("scent {} {}\n", category, edible);
		for (auto& fp : foodPlaces)
			s += fmt::format("food {} {} {} {} {} {}\n", fp.x, fp.y, fp.tick, fp.confidence, fp.meals, fp.category);
		s += fmt::format("restless {}\n", hungryTicks);
		if (gaitWalk)
			s += "gaitwalk 1\n";
		for (auto& e : exits)
			s += fmt::format("exit {} {} {} {} {}\n", e.x, e.y, e.category, e.decision, e.uses);
		for (auto& f : flooders)
			s += fmt::format("flooder {} {} {} {} {}\n", f.x, f.y, f.category, f.decision, f.uses);
		for (auto& [decision, bl] : boatLinks)
			s += fmt::format("boat {} {} {}\n", decision, bl.dx, bl.rides);
		for (auto& tl : travelLinks)
			s += fmt::format("tlink {} {} {} {} {} {} {}\n", tl.fromX, tl.fromY, tl.toX, tl.toY, tl.category, tl.decision, tl.uses);
		for (auto& cb : callButtons)
			s += fmt::format("callbtn {} {} {} {}\n", cb.x, cb.y, cb.worked, cb.failed);
		for (auto& [from, edges] : placeMap)
			for (auto& e : edges)
				s += fmt::format("pedge {} {} {} {} {} {:.0f} {} {}\n", from.first, from.second, e.to.first, e.to.second, e.dir, e.cost, e.uses, e.fails);
		s += fmt::format("fallrate {}\n", fallHurtPerPx);
		for (auto& f : painfulDrops)
			s += fmt::format("pdrop {:.0f} {:.0f} {} {}\n", f.x, f.y, f.dir, f.hurt);
		// sense of place
		s += fmt::format("feelbase {}", feelBaseTicks);
		for (float f : feelBase)
			s += fmt::format(" {:.4g}", f);
		s += "\n";
		for (auto& [id, r] : rooms) {
			s += fmt::format("room {} {:.0f} {:.0f} {} {} {} {} {} {:.0f} {:.0f}", id, r.x, r.floor, r.metaroom, r.water ? 1 : 0, r.visits, r.first, r.last, r.ticks, r.feelTicks);
			for (float f : r.feelSum)
				s += fmt::format(" {:.4g}", f);
			s += "\n";
			for (auto& [category, sg] : r.seen)
				s += fmt::format("rseen {} {} {:.0f} {:.0f} {} {}\n", id, category, sg.x, sg.y, sg.last, sg.times);
			for (auto& [who, n] : r.met)
				s += fmt::format("rmet {} {} {}\n", id, who, n);
		}
		for (auto& [who, m] : metAt)
			s += fmt::format("metat {} {} {:.0f} {:.0f} {} {} {} {} {} {} {}\n", who, m.room, m.x, m.y, m.last, m.genus, m.category, m.stage, m.female ? 1 : 0, m.longing, m.longingAt);
		return s;
	}

	void loadState(const std::string& state) override {
		std::istringstream in(state);
		std::string line;
		int version = 0;
		while (std::getline(in, line)) {
			std::istringstream l(line);
			std::string what;
			l >> what;
			if (what == "statever")
				l >> version;
			else if (what == "prior") {
				int category, decision;
				float prior;
				l >> category >> decision >> prior;
				if (l)
					expectations[(category + 1) * 64 + decision].prior = prior;
			} else if (what == "innate") {
				int decision;
				Expectation e;
				l >> decision >> e.prior;
				for (float& f : e.delta)
					l >> f;
				if (l)
					innateDefaults[decision] = e;
			} else if (what == "scent") {
				int category;
				float edible;
				l >> category >> edible;
				if (l)
					scents[category] = edible;
			} else if (what == "callbtn") {
				CallButton cb;
				l >> cb.x >> cb.y >> cb.worked >> cb.failed;
				if (l && callButtons.size() < 16)
					callButtons.push_back(cb);
			} else if (what == "pedge") {
				Cell from;
				PlaceEdge e;
				l >> from.first >> from.second >> e.to.first >> e.to.second >> e.dir >> e.cost >> e.uses >> e.fails;
				if (l && (e.dir == 10 || e.dir == 11) && placeEdges < prm.placeEdgeMax) {
					placeMap[from].push_back(e);
					placeEdges++;
				}
			} else if (what == "fallrate") {
				l >> fallHurtPerPx;
			} else if (what == "pdrop") {
				PainfulDrop f;
				l >> f.x >> f.y >> f.dir >> f.hurt;
				if (l && painfulDrops.size() < 32)
					painfulDrops.push_back(f);
			} else if (what == "feelbase") {
				l >> feelBaseTicks;
				for (float& f : feelBase)
					l >> f;
			} else if (what == "room") {
				int id, water;
				RoomMemory r;
				l >> id >> r.x >> r.floor >> r.metaroom >> water >> r.visits >> r.first >> r.last >> r.ticks >> r.feelTicks;
				for (float& f : r.feelSum)
					l >> f;
				r.water = water != 0;
				if (l && rooms.size() < prm.roomMemoryMax)
					rooms[id] = r;
			} else if (what == "rseen") {
				int id, category;
				RoomSighting sg;
				l >> id >> category >> sg.x >> sg.y >> sg.last >> sg.times;
				if (l && rooms.count(id))
					rooms[id].seen[category] = sg;
			} else if (what == "rmet") {
				int id, n;
				std::string who;
				l >> id >> who >> n;
				if (l && rooms.count(id))
					rooms[id].met[who] = n;
					trimMet(rooms[id].met, who);
			} else if (what == "metat") {
				std::string who;
				MetCreature m;
				int female;
				l >> who >> m.room >> m.x >> m.y >> m.last >> m.genus >> m.category >> m.stage >> female;
				m.female = female != 0;
				if (l) {
					float longing;
					unsigned int at;
					if (l >> longing >> at) {
						m.longing = longing;
						m.longingAt = at;
					}
					metAt[who] = m;
				}
			} else if (what == "tlink") {
				TravelLink tl;
				l >> tl.fromX >> tl.fromY >> tl.toX >> tl.toY >> tl.category >> tl.decision >> tl.uses;
				// (links by eating, picking up and the like, learned before
				// only device use counted, are dropped)
				if (l && travelLinks.size() < prm.linkMax && tl.decision >= 1 && tl.decision <= 3)
					travelLinks.push_back(tl);
			} else if (what == "boat") {
				BoatLink bl;
				l >> bl.decision >> bl.dx >> bl.rides;
				if (l)
					boatLinks[bl.decision] = bl;
			} else if (what == "exit") {
				ExitLink e;
				l >> e.x >> e.y >> e.category >> e.decision >> e.uses;
				if (l && exits.size() < prm.exitMax)
					exits.push_back(e);
			} else if (what == "flooder") {
				ExitLink f;
				l >> f.x >> f.y >> f.category >> f.decision >> f.uses;
				if (l && f.decision >= 1 && f.decision <= 3 && flooders.size() < prm.exitMax)
					flooders.push_back(f);
			} else if (what == "gaitwalk") {
				int v = 0;
				l >> v;
				gaitWalk = v != 0;
			} else if (what == "restless") {
				l >> hungryTicks;
			} else if (what == "food") {
				FoodPlace fp;
				l >> fp.x >> fp.y >> fp.tick >> fp.confidence >> fp.meals;
				if (l && foodPlaces.size() < prm.foodPlaceMax) {
					int cat;
					if (l >> cat) // (saves from before 2026-11-21 have none)
						fp.category = cat;
					foodPlaces.push_back(fp);
				}
			} else if (what == "link") {
				Link lk;
				l >> lk.x >> lk.fromY >> lk.toY >> lk.decision >> lk.uses;
				// (links by eating, picking up and the like are dropped: see learnRide)
				// (developer aid: OPENC2E_KEEP_BADLINKS keeps them, for comparison)
				if (l && ((lk.decision >= 1 && lk.decision <= 3) || devenv("OPENC2E_KEEP_BADLINKS")))
					links.push_back(lk);
			} else if (what == "instinct") {
				std::string sig;
				l >> sig;
				appliedInstincts.insert(sig);
			} else if (what == "nudge") {
				std::string sig;
				int tries = 0;
				float approval = 0;
				l >> sig >> tries >> approval;
				if (l)
					savedNudges[sig] = {tries, approval};
			} else if (what == "valence")
				l >> valence;
			else if (what == "mood")
				l >> mood;
			else if (what == "episodes")
				l >> episodes;
			else if (what == "stimuli")
				l >> stimuliSeen;
			else if (what == "drift")
				for (float& f : drift)
					l >> f;
			else if (what == "habituation")
				for (float& f : habituation)
					l >> f;
			else if (what == "individual") {
				std::string who;
				int decision;
				Expectation e;
				l >> who >> decision >> e.count;
				for (float& f : e.delta)
					l >> f;
				if (l)
					individuals[who][decision] = e;
			} else if (what == "lp") {
				// learning progress of an expectation (after its "expect" line)
				int category, decision;
				float fast, slow;
				l >> category >> decision >> fast >> slow;
				if (l)
					if (auto it = expectations.find((category + 1) * 64 + decision); it != expectations.end()) {
						it->second.errFast = fast;
						it->second.errSlow = slow;
					}
			} else if (what == "expect") {
				int category, decision;
				Expectation e;
				l >> category >> decision >> e.count;
				for (float& f : e.delta)
					l >> f;
				if (l) {
					float prior = expectations.count((category + 1) * 64 + decision) ? expectations[(category + 1) * 64 + decision].prior : 0.0f;
					e.prior = prior;
					expectations[(category + 1) * 64 + decision] = e;
				}
			}
		}
		if (version == 2 || version == 3) {
			// Learned before felt effects (from drive changes alone, which mix
			// in everything else going on) and, before version 3, while sleep
			// was broken. Mostly noise: start learning again from the instincts
			// (they are applied again) and smells; rides between floors are kept.
			expectations.clear();
			innateDefaults.clear();
			appliedInstincts.clear();
			scents.clear();
			fmt::print("[homeostat] relearning from scratch (expectations saved before felt effects)\n");
		}
		if (version >= 2 && version <= 7) {
			// Saved while a sleep the world's sleep script refused (C2's: only
			// while Tiredness is above 48) was learned as sleeping that did
			// nothing: grendels and ettins, who hardly tire, learned that
			// sleep never helps. Grendels and ettins now sleep when sleepy
			// (sleep.cos, owner 2026-11-10): learn sleep afresh (its prior,
			// from the instincts, is kept). And, until version 8, while lying
			// down to rest was learned as sleeping, and sleep was broken off
			// within seconds.
			if (expectations.erase(prm.sleepDecision)) { // category -1
				// and let the sleep instincts seed it again (signatures end
				// "-action-drive-level")
				for (auto it = appliedInstincts.begin(); it != appliedInstincts.end();) {
					size_t a = it->find('-');
					if (a != std::string::npos && std::atoi(it->c_str() + a + 1) == prm.sleepDecision)
						it = appliedInstincts.erase(it);
					else
						++it;
				}
				fmt::print("[homeostat] relearning sleep (learned while sleep could be refused or cut short)\n");
			}
		}
		if (version < 9) {
			// Saved while instincts naming heard words waited (dormant), marked
			// read all the same. Unmark them, so they are read again.
			for (auto it = appliedInstincts.begin(); it != appliedInstincts.end();) {
				int v[6] = {0, 0, 0, 0, 0, 0};
				bool words = false;
				if (std::sscanf(it->c_str(), "%d.%d.%d.%d.%d.%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
					for (int i = 0; i < 3; i++)
						words |= v[2 * i] == 3 || v[2 * i] == 4;
				if (words)
					it = appliedInstincts.erase(it);
				else
					++it;
			}
		}
		if (version < 7) {
			// Saved while lobe 7 was read as heard nouns (it is attention):
			// its instincts waited. Unmark them, so they are read again.
			for (auto it = appliedInstincts.begin(); it != appliedInstincts.end();) {
				int v[6] = {0, 0, 0, 0, 0, 0};
				bool seven = false;
				if (std::sscanf(it->c_str(), "%d.%d.%d.%d.%d.%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
					for (int i = 0; i < 3; i++)
						seven |= v[2 * i] == 7;
				if (seven)
					it = appliedInstincts.erase(it);
				else
					++it;
			}
		}
		if (version < 6) {
			// Saved while instincts naming a sense, or no drive, waited
			// (dormant); their genes were marked read all the same. Unmark
			// them, so they are read again now that they take effect.
			// Signatures: "lobe.neuron.lobe.neuron.lobe.neuron-action-drive-level".
			for (auto it = appliedInstincts.begin(); it != appliedInstincts.end();) {
				int v[6] = {0, 0, 0, 0, 0, 0};
				bool words = false, sense = false, drive = false;
				if (std::sscanf(it->c_str(), "%d.%d.%d.%d.%d.%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
					for (int i = 0; i < 3; i++) {
						words |= v[2 * i] == 3 || v[2 * i] == 7;
						sense |= v[2 * i] == 5 && v[2 * i + 1] != 19;
						drive |= v[2 * i] == 1;
					}
				if (!words && (sense || !drive))
					it = appliedInstincts.erase(it);
				else
					++it;
			}
		}
		if (version < 2) {
			// Saved before body chemistry ran (frozen bodies: every action seemed
			// to do nothing). That experience misleads, so start learning afresh.
			expectations.clear();
			drift.fill(0.0f);
			habituation.fill(1.0f);
			fmt::print("[homeostat] discarded learning saved before body chemistry worked\n");
		}
	}

	std::string debugState() const override {
		return fmt::format("homeostat: {} episodes, valence {:+.3f}, {} stimuli, {} expectations, instincts {} active {} dormant",
			episodes, valence, stimuliSeen, expectations.size(), activeInstincts, dormantInstincts);
	}

  private:
	Params prm;
	// developer aid: OPENC2E_MIND_PARAMS="name=value,..." sets travel
	// parameters for a test (hungerPatience, travelHalf,
	// floorChangeDistance, routeMaxCost, seekDistanceCost)
	bool paramsOverridden = false;
	void overrideParams() {
		const char* env = devenv("OPENC2E_MIND_PARAMS");
		if (!env)
			return;
		std::map<std::string, float*> known = {{"hungerPatience", &prm.hungerPatience}, {"travelHalf", &prm.travelHalf},
			{"floorChangeDistance", &prm.floorChangeDistance}, {"routeMaxCost", &prm.routeMaxCost}, {"seekDistanceCost", &prm.seekDistanceCost},
			{"exploreWant", &prm.exploreWant}, {"noveltyMin", &prm.noveltyMin}, {"noveltyReturn", &prm.noveltyReturn}};
		std::stringstream ss(env);
		std::string item;
		while (std::getline(ss, item, ',')) {
			auto eq = item.find('=');
			if (eq == std::string::npos)
				continue;
			auto it = known.find(item.substr(0, eq));
			if (it != known.end())
				*it->second = std::strtof(item.c_str() + eq + 1, nullptr);
		}
		static bool told = false;
		if (!told) {
			told = true;
			fmt::print("* MIND PARAMS hungerPatience {} travelHalf {} floorChangeDistance {} routeMaxCost {} seekDistanceCost {}\n",
				prm.hungerPatience, prm.travelHalf, prm.floorChangeDistance, prm.routeMaxCost, prm.seekDistanceCost);
		}
	}
	std::map<int, Expectation> expectations; // key: (category + 1) * 64 + decision
	// Individuals (owner, 2026-11-11): what acting on a particular creature
	// did, by its moniker and the decision, alongside what acting on its kind
	// did. A stranger is judged by its kind; the more a creature has dealt
	// with an individual, the more its own record counts (weight n / (n +
	// individualPrior)), as a prototype gives way to known examples.
	std::map<std::string, std::map<int, Expectation>> individuals;
	std::map<int, Expectation> innateDefaults; // by decision: instincts about any object
	std::set<std::string> appliedInstincts;
	int activeInstincts = 0, dormantInstincts = 0;
	std::vector<std::pair<std::string, float>> brainSettings; // BrainProfiles: applied in applyTemperament
	int pendingNovelty = 0; // MindOutput::noveltyOutcome, for the next tick
	// inborn nudges (Params::innateNudge): from instincts that name no drive
	struct Nudge {
		std::string signature;
		int decision = 0;
		std::vector<int> categories; // empty: whatever it acts on (or nothing)
		std::vector<int> senses; // situation senses that must all be lit
		std::vector<int> verbs, nouns; // words just heard from the hand (verbs, categories)
		float value = 0; // +strength rewarded, -strength punished
		float weight = 1; // in remembered tries
		int tries = 0;
		float approvalSum = 0; // the approval felt over those tries
		float effect() const { return (value * weight + approvalSum) / (weight + tries); }
	};
	std::vector<Nudge> nudges;
	std::map<std::string, std::pair<int, float>> savedNudges; // signature -> tries, approval (until the gene is read again)
	std::vector<size_t> episodeNudges; // the nudges whose situation held when this episode began
	float episodeApprovalStart = 0;
	float approvalNow(const MindPerception& p) const {
		if (!p.chemicals)
			return 0.0f;
		float r = (p.rewardChem > 0 && p.rewardChem < 256) ? p.chemicals[p.rewardChem] : 0.0f;
		float q = (p.punishChem > 0 && p.punishChem < 256) ? p.chemicals[p.punishChem] : 0.0f;
		return r - q;
	}
	std::vector<size_t> obeyed; // the nudges of the last thing done as told, and when
	unsigned int obeyedTick = 0;
	int heardDone = 0, lastHeardAge = -1; // times it has done what was last said
	bool calledIn = false; // this call has been answered (looked round, or not)
	// the action a heard verb names, for this decision (the walks the world
	// lends go for left and right, its sleep for rest)
	static int wordOf(int decision) { return decision == 26 ? 10 : decision == 27 ? 11 : decision == 25 ? 9 : decision; }
	bool nudgeHolds(const Nudge& n, const MindPerception& p, int category, int decision) const {
		if (!n.verbs.empty() || !n.nouns.empty()) {
			if (p.heardAge < 0 || p.heardAge >= prm.heardTicks)
				return false;
			if (!n.verbs.empty() && std::find(n.verbs.begin(), n.verbs.end(), p.heardVerb) == n.verbs.end())
				return false;
			if (!n.nouns.empty() && std::find(n.nouns.begin(), n.nouns.end(), p.heardNoun) == n.nouns.end())
				return false;
			// to the thing named; "come" with none named, to the hand; nothing
			// else is done to the hand unasked
			if (category >= 0) {
				const int meant = p.heardNoun >= 0 ? p.heardNoun : p.heardVerb == 4 ? 1 : -1;
				if (meant >= 0 ? category != meant : category == 1)
					return false;
			}
			if (wordOf(decision) != n.decision)
				return false;
		} else if (n.decision != decision)
			return false;
		if (!n.categories.empty() && std::find(n.categories.begin(), n.categories.end(), category) == n.categories.end())
			return false;
		for (int s : n.senses)
			if (s < 0 || s >= 32 || p.senses[s] < prm.senseOn)
				return false;
		return true;
	}
	std::map<int, Place> places; // where each category was last seen
	std::vector<Link> links; // learned rides between floors
	std::vector<FoodPlace> foodPlaces; // the food map
	std::map<std::pair<int, int>, unsigned int> visited; // (x bin, y bin) -> last mind tick there
	std::vector<ExitLink> exits; // objects that got the creature out of water
	std::vector<ExitLink> flooders; // objects that got the creature into water
	std::vector<std::pair<AgentRef, unsigned int>> triedDevices; // passed over while escaping, and when
	std::map<int, BoatLink> boatLinks; // decision -> learned ride
	std::vector<TravelLink> travelLinks;

	// ---- Sense of place: rooms remembered ----
	struct RoomSighting {
		float x = 0, y = 0; // where it was seen
		unsigned int last = 0; // last seen
		int times = 0; // sightings (one per sightingGap)
		unsigned int emptyAt = 0; // last time looked for here and not found
	};
	struct RoomMemory {
		float x = 0, floor = 0; // its middle and floor
		int metaroom = -1;
		bool water = false;
		int visits = 0; // 0: known only as a room next door
		unsigned int first = 0, last = 0;
		float ticks = 0; // mind ticks spent here
		std::array<float, NUM_DRIVES> feelSum{}; // drive change while here
		float feelTicks = 0;
		std::map<int, RoomSighting> seen; // by category
		std::map<std::string, int> met; // creatures met here: moniker -> meetings (the most met only; trimMet)
	};
	struct MetCreature {
		int room = -1;
		float x = 0, y = 0;
		unsigned int last = 0;
		int genus = -1, category = -1, stage = -1;
		bool female = false;
		unsigned int checked = 0; // gone there and not found
		float longing = 0; // as it was at longingAt (fades with time)
		unsigned int longingAt = 0;
	};
	float longingNow(const MetCreature& m, unsigned int tick) const {
		if (m.longing <= 0.0f)
			return 0.0f;
		return m.longing * std::exp(-(float)(tick > m.longingAt ? tick - m.longingAt : 0) / prm.longingFade);
	}
	std::map<int, RoomMemory> rooms;
	std::map<std::string, MetCreature> metAt; // where each creature was last met
	std::array<float, NUM_DRIVES> feelBase{}; // drive change anywhere (the usual)
	float feelBaseTicks = 0;
	std::array<float, NUM_DRIVES> placePrevD{};
	bool placePrevValid = false;
	int roomNow = -1;
	std::map<int, int> recalledFrom; // category -> room its seek target was recalled from

	// ---- The place map ----
	using Cell = std::pair<int, int>;
	struct PlaceEdge {
		Cell to;
		int dir = 0; // 10 west, 11 east: walking, climbing or dropping that way
		float cost = 0; // pixels covered
		int uses = 0; // times taken (0: only inferred, the way back along a level floor)
		int fails = 0; // tries that met a wall or got nowhere
	};
	std::map<Cell, std::vector<PlaceEdge>> placeMap;
	size_t placeEdges = 0;
	Cell mapCell{0, 0};
	bool mapCellValid = false;
	float mapX = 0, mapY = 0;
	int routeTarget = -1; // food place the current route leads to
	float routeBest = 0; // the least cost left to it so far
	unsigned int routeSince = 0;
	std::vector<CallButton> callButtons; // buttons tried for calling boats
	struct DudButton {
		float x, y;
		unsigned int until;
	};
	std::vector<DudButton> dudButtons; // blind presses that brought nothing
	bool blindPending = false;
	float blindX = 0, blindY = 0;
	unsigned int blindTick = 0;
	float callPressX = 0, callPressY = 0; // a boat-call press waiting for a boat
	unsigned int callPressTick = 0;
	bool callPending = false;
	TravelLink pendingLink; // the latest device use, while it may still turn into a link
	unsigned int pendingLinkTick = 0;
	bool pendingRode = false;
	bool wasRiding = false;
	float rideStartX = 0;
	int rideDecision = -1;
	int boatTryDecision = -1; // last boat action pressed, and when
	unsigned int boatTryTick = 0;
	unsigned int boatFlipUntil = 0; // until then, use the other action than learned/guessed
	bool wasInWater = false;
	bool waterRaw = false;
	unsigned int waterRawSince = 0;
	ExitLink lastObjectUse; // the most recent object action, for exit learning
	unsigned int lastObjectUseTick = 0;
	AgentRef episodeAgent; // the object this episode acts on
	AgentRef lastUselessAgent;
	int uselessInARow = 0;
	AgentRef pendingPassOver;
	int forageHeading = 0; // 10 west, 11 east, 0 none
	unsigned int forageUntil = 0;
	float forageStartX = 0, forageStartY = 0;
	unsigned int forageStartTick = 0;
	int forageStuckFlips = 0; // turnarounds in a row without progress
	int forageVertical = 1; // which way to try another floor: -1 up, 1 down
	unsigned int lastFloorChangeTick = 0; // when a forage last reached another floor
	float hungryTicks = 0; // time hungry without a meal that eased it (restlessness)
	unsigned int lastTick = 0;
	int foodTrip = -1; // index in foodPlaces of the place being headed for
	float foodTripBest = 0;
	unsigned int foodTripSince = 0;
	float foodTripFromX = 0, foodTripFromY = 0; // where the creature was at the last progress
	struct Futile {
		int category = -1, decision = -1;
		float x = 0, y = 0;
		unsigned int until = 0;
	};
	std::vector<Futile> futile; // object actions that got nowhere from here
	int futileInARow = 0;
	int futileCategory = -1, futileDecision = -1;
	// tending: when each creature (by moniker) may be tickled or brought
	// something again, and who the last bring was for
	std::map<std::string, unsigned int> tendedUntil, broughtUntil;
	std::map<int, unsigned int> bringFailUntil; // a category whose pickup to bring left the hands empty: not tried again till then
	int bringPickupCategory = -1; // a pickup to bring was chosen: its category, to check it worked
	std::string bringingFor;
	int lastCarrying = -1;
	std::map<int, float> scents; // strongest edible scent smelled, by category
	// a scent being followed (persistent following): which, which way (10
	// west, 11 east, -1 up, 1 down), the strongest smelled on the way,
	// decisions left, and decisions since anything ahead smelled stronger
	struct Trail {
		std::string scent;
		int kind = 0;
		float best = 0;
		int left = 0;
		int pastPeak = 0;
	};
	// one for each scent: a single shared trail was taken over by any other
	// scent's step that was only offered, not chosen, so the care scent
	// (2026-11-15) cut mates' following by about a fifth
	std::map<std::string, Trail> trails;
	bool riding = false;
	float rideFromX = 0, rideFromY = 0;
	int lastLiftDecision = -1;
	std::array<float, 42> habituation; // (C2's 40 categories, MIND_HEALING_FRUIT and MIND_ROTTEN_FRUIT)
	float valence = 0.0f;
	std::mt19937 rng;

	bool inEpisode = false;
	// for the Mind Kit (view): the options weighed at the last choice (the
	// choice first), how each would ease each need, and a mood log
	std::vector<Candidate> viewCandidates;
	std::vector<std::array<float, NUM_DRIVES>> viewEase;
	std::array<float, NUM_DRIVES> viewDrives{};
	float viewD = 0, viewX = 0, viewY = 0;
	unsigned int viewChose = 0, viewTick = 0;
	struct MoodSample {
		unsigned int tick;
		float content;
		int strongest;
		float strength;
	};
	std::deque<MoodSample> moodLog;
	std::deque<MindView::Outcome> outcomeLog; // the last few episodes (view)
	// Walking by gait (owner, 2026-11-12): travelling west or east runs the
	// world's stepping scripts (26/27: poses 61-64 played as an animation, the
	// planted foot carrying the body along). A grendel whose genes give those
	// poses no stride stepped on the spot for good, starving in the cave
	// under the bridge with food a short walk away, while its gait walk
	// (scripts 42/43, WALK) worked. A stepping walk that gets nowhere with
	// nothing ahead (no wall, water or drop) switches this mind to the gait
	// walk for travel.
	int stepFails = 0;
	bool gaitWalk = false;
	void noteChoiceForView(const std::vector<Candidate>& candidates, const Candidate& choice, const std::array<float, NUM_DRIVES>& d, float D) {
		std::vector<Candidate> sorted = candidates;
		std::sort(sorted.begin(), sorted.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
		viewCandidates.clear();
		viewCandidates.push_back(choice);
		for (auto& c : sorted) {
			if (viewCandidates.size() >= 6)
				break;
			if (c.category == choice.category && c.decision == choice.decision && c.who == choice.who)
				continue;
			viewCandidates.push_back(c);
		}
		// each option's expected ease, split among the needs: what easing
		// each need alone by what it expects would do
		viewEase.clear();
		for (auto& c : viewCandidates) {
			std::array<float, NUM_DRIVES> ease{};
			auto it = expectations.find((c.category + 1) * 64 + c.decision);
			float sum = 0;
			if (it != expectations.end())
				for (int i = 0; i < NUM_DRIVES; i++) {
					std::array<float, NUM_DRIVES> after = d;
					after[i] = std::clamp(d[i] + it->second.delta[i], 0.0f, 1.0f);
					ease[i] = std::max(0.0f, D - overallDrive(after));
					sum += ease[i];
				}
			float want = std::max(0.0f, c.want);
			for (auto& e : ease)
				e = sum > 0 ? e / sum * want : 0.0f;
			viewEase.push_back(ease);
		}
		viewChose = lastTickForView;
	}
	unsigned int lastTickForView = 0;
	float closest = 1e9f; // nearest the creature has been to its target this episode
	unsigned int episodeWalledStart = 0; // MindPerception::walledCount when it began
	int lastProgress = 0;
	float episodeStartX = 0, episodeStartY = 0; // where the creature was when it began
	Candidate current{};
	int episodeTicks = 0;
	std::array<float, NUM_DRIVES> startDrives{};
	float startD = 0.0f;
	unsigned int episodes = 0, stimuliSeen = 0;
	std::array<float, NUM_DRIVES> drift{}; // baseline drive change per tick
	std::array<float, NUM_DRIVES> sleepStartDrives{};
	unsigned int noSleepUntil = 0;
	unsigned int wokenUntil = 0; // (woken by an urgent need: awake until then)
	bool fellAsleep = false; // during the current sleep episode
	bool sleepRefused = false; // the episode ending now was a refused sleep
	float badSleepX = 0, badSleepY = 0; // where a sleep was cut short by cold or heat
	unsigned int badSleepUntil = 0;
	bool drivesSeen = false;
	std::array<float, NUM_DRIVES> felt{}; // drive changes this episode's stimuli delivered
	std::array<bool, NUM_DRIVES> feltAny{};
	float startX = 0;
	float startY = 0; // where the episode began (height)
	struct Blocked {
		float x = 0;
		unsigned int until = 0;
	};
	Blocked blocked[2]; // [0] west, [1] east
	// Ways found blocked, by place (owner's screencast, 2026-11-13: norns in
	// the volcano walked into its west wall again and again, a doorway too
	// narrow for them that looks open): each spot's block lasts longer each
	// time it is met again, up to 16 times blockedTicks.
	struct BlockedSpot {
		float x = 0, y = 0;
		int dir = 0; // 10 west, 11 east
		int times = 0;
		unsigned int until = 0;
	};
	std::vector<BlockedSpot> blockedSpots;
	void noteBlocked(const MindPerception& p, int dir) {
		BlockedSpot* spot = nullptr;
		for (auto& b : blockedSpots)
			if (b.dir == dir && std::fabs(dxTo(p, b.x)) < prm.blockedNear && std::fabs(b.y - p.y) < prm.sameLevel)
				spot = &b;
		if (!spot) {
			if (blockedSpots.size() >= 24)
				blockedSpots.erase(std::min_element(blockedSpots.begin(), blockedSpots.end(), [](const BlockedSpot& a, const BlockedSpot& b) { return a.until < b.until; }));
			blockedSpots.push_back({p.x, p.y, dir, 0, 0});
			spot = &blockedSpots.back();
		}
		spot->times = std::min(spot->times + 1, 5);
		spot->until = p.tick + prm.blockedTicks * (1u << (spot->times - 1));
		blocked[dir == 10 ? 0 : 1] = {p.x, spot->until};
	}
	std::map<int, unsigned int> exploreBlockedUntil; // room -> not explored toward until then
	int exploreTarget = -1; // the room the latest explore trip heads for
	unsigned int starveLeaveTick = 0; // the latest try at leaving for food (see starveLeaveHunger)
	float starveLeaveX = 0, starveLeaveY = 0;

	// East-west distances the short way round a wrapping world (C2's joins
	// its far west edge to its far east): wrapD folds a difference into
	// (-W/2, W/2]; dxTo is how far east x lies from the creature (negative:
	// west).
	float wrapW = 0;
	float wrapD(float d) const {
		if (wrapW <= 0)
			return d;
		d = std::fmod(d, wrapW);
		if (d > wrapW / 2)
			d -= wrapW;
		else if (d <= -wrapW / 2)
			d += wrapW;
		return d;
	}
	float dxTo(const MindPerception& p, float x) const { return wrapD(x - p.x); }

	bool isBlocked(const MindPerception& p, int decision) const {
		int dir = decision == 26 ? 10 : decision == 27 ? 11 : decision;
		const Blocked& b = blocked[dir == 10 ? 0 : 1];
		if (p.tick < b.until && std::fabs(dxTo(p, b.x)) < prm.blockedNear)
			return true;
		for (auto& s : blockedSpots)
			if (s.dir == dir && p.tick < s.until && std::fabs(dxTo(p, s.x)) < prm.blockedNear && std::fabs(s.y - p.y) < prm.sameLevel)
				return true;
		return false;
	}
	float seasonalCuriosity = 1.0f; // summer play: novelty's extra worth this tick (tickChoose)
	float mood = 0.0f; // short-term affect: fast-following, decaying average of reward
	int lastFace = -1;

	// by the place whose cold or heat woke it a while ago
	bool badSleepAt(const MindPerception& p) const {
		return p.tick < badSleepUntil && std::fabs(dxTo(p, badSleepX)) < prm.badSleepNear && std::fabs(p.y - badSleepY) < prm.sameLevel;
	}
	// too hungry to sleep (see prm.starveAwake)
	bool starvingAwake(const std::array<float, NUM_DRIVES>& d) const {
		return d[HUNGER] >= prm.starveAwake && d[SLEEPY] < prm.exhausted && d[TIRED] < prm.exhausted;
	}

	// A bodily need that has risen sharply since falling asleep (hunger,
	// cold, pain from a slap...), or -1. Boredom or loneliness do not wake.
	int wakeReason(const std::array<float, NUM_DRIVES>& d) const {
		int worst = -1;
		for (int i : {PAIN, HUNGER, COLD, HOT, FEAR, INJURY, SUFFOCATE, THIRST}) {
			if (d[i] >= prm.wakeLevel && d[i] - sleepStartDrives[i] >= prm.wakeRise && (worst < 0 || d[i] > d[worst]))
				worst = i;
		}
		// starving and no longer near exhaustion: hunger wakes it (see
		// starveAwake; one that slept because exhausted was woken as soon as
		// it was a hair less so)
		if (worst < 0 && starvingAwake(d) && std::max(d[SLEEPY], d[TIRED]) < prm.exhausted - prm.wakeRise)
			worst = HUNGER;
		return worst;
	}

	int chooseFace(const std::array<float, NUM_DRIVES>& d, float D) const {
		float distress = std::max({d[PAIN], d[FEAR], d[LONELY]});
		if (d[ANGER] >= prm.angryAt)
			return prm.faceAngry;
		if (distress >= prm.distressAt || mood <= prm.sadMood || valence <= prm.sadValence)
			return prm.faceSad;
		if (mood >= prm.happyMood || (valence >= prm.contentValence && D <= prm.contentDrive))
			return prm.faceHappy;
		return prm.faceNeutral;
	}

	static std::array<float, NUM_DRIVES> normalize(const std::array<unsigned char, 17>& raw) {
		std::array<float, NUM_DRIVES> d{};
		for (int i = 0; i < NUM_DRIVES; i++)
			d[i] = raw[i] / 255.0f;
		return d;
	}

	// 1. Overall drive: severe needs dominate several mild ones.
	float overallDrive(const std::array<float, NUM_DRIVES>& d) const {
		float sum = 0.0f;
		for (int i = 0; i < NUM_DRIVES; i++)
			sum += prm.weights[i] * std::pow(std::clamp(d[i], 0.0f, 1.0f), prm.driveExpN);
		return std::pow(sum, 1.0f / prm.driveExpM);
	}

	static bool isVisible(const MindPerception& p, int category) {
		for (auto& s : p.visible)
			if (s.category == category)
				return true;
		return false;
	}

	Expectation& expectation(int category, int decision) {
		int key = (category + 1) * 64 + decision;
		auto it = expectations.find(key);
		if (it == expectations.end()) {
			// A new category starts from what this action has done on other
			// categories so far; the instinct is the fallback before any
			// experience with the action (6. instincts as starting points).
			Expectation e;
			e.delta = instinctFor(decision);
			auto def = innateDefaults.find(decision);
			if (def != innateDefaults.end()) {
				e.delta = def->second.delta;
				e.prior = def->second.prior;
			}
			int total = 0;
			std::array<float, NUM_DRIVES> sum{};
			for (auto& [k, other] : expectations) {
				if (k % 64 == decision && other.count > 0) {
					for (int i = 0; i < NUM_DRIVES; i++)
						sum[i] += other.delta[i] * other.count;
					total += other.count;
				}
			}
			if (total > 0) {
				float w = total + e.prior;
				for (int i = 0; i < NUM_DRIVES; i++)
					e.delta[i] = (sum[i] + e.delta[i] * e.prior) / w;
			}
			it = expectations.emplace(key, e).first;
		}
		return it->second;
	}

	// 3. Wanting: predicted drop in D, given current needs.
	float predictedRelief(const std::array<float, NUM_DRIVES>& d, float D, const Expectation& e) const {
		std::array<float, NUM_DRIVES> after{};
		// A starving body does not recover from tiredness or sleepiness (in
		// C2's chemistry they stay where they are, whatever the rest and
		// sleep scripts report: a sleeping norn at hunger 254 kept Tired 136
		// through 1292 ticks, a resting one Tired 223 through hundreds of
		// rests). So while starving, nothing is expected to relieve them,
		// and food comes first (owner, 2026-11-11).
		bool starving = d[HUNGER] >= prm.starveAwake;
		for (int i = 0; i < NUM_DRIVES; i++) {
			float delta = e.delta[i];
			if (starving && (i == TIRED || i == SLEEPY) && delta < 0.0f)
				delta = 0.0f;
			after[i] = std::clamp(d[i] + delta, 0.0f, 1.0f);
		}
		return D - overallDrive(after);
	}

	std::vector<Candidate> evaluate(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D) {
		std::vector<Candidate> out;
		// 5. Affect: negative valence dampens curiosity; Boredom raises it;
		// a pressing survival need quiets it.
		float survival = std::max({d[PAIN], d[HUNGER], d[COLD], d[HOT], d[INJURY], d[SUFFOCATE], d[THIRST]});
		float curiosity = seasonalCuriosity * prm.curiosity * (1.0f + prm.boredomCuriosityGain * d[BORED]) * std::clamp(1.0f + 10.0f * valence, 0.2f, 1.5f)
			* (1.0f - prm.survivalCuriosityDamp * std::clamp(survival, 0.0f, 1.0f));
		auto add = [&](int category, int decision, float distance, float edible, const SeenAgent* who = nullptr) {
			// what smells of poison is never eaten (a grendel's single bite of
			// the game's cyanide berries was enough to collapse it)
			if (decision == prm.eatDecision && edible < 0.0f)
				return;
			Expectation& kind = expectation(category, decision);
			Expectation blend = kind;
			int known = kind.count;
			const Expectation* record = &kind; // whose learning progress counts
			Candidate c{category, decision, distance, 0, 0, 0};
			if (who && !who->who.empty()) {
				// a particular creature: its own record, as far as it goes
				// (looked up, not made: only dealings make a memory)
				const Expectation* own = nullptr;
				if (auto it = individuals.find(who->who); it != individuals.end())
					if (auto jt = it->second.find(decision); jt != it->second.end())
						own = &jt->second;
				known = own ? own->count : 0;
				record = own;
				float w = known / (known + prm.individualPrior);
				if (own)
					for (int i = 0; i < NUM_DRIVES; i++)
						blend.delta[i] = (1.0f - w) * kind.delta[i] + w * own->delta[i];
				c.target = who->agent;
				c.who = who->who;
			}
			// the healing and the rotten fruit: always that very fruit (the
			// engine knows them as fruit)
			if ((category == MIND_HEALING_FRUIT || category == MIND_ROTTEN_FRUIT) && who)
				c.target = who->agent;
			const Expectation& e = blend;
			c.want = predictedRelief(d, D, e);
			// What does not smell of food is not expected to ease hunger (a
			// seed among fruit; the smell is read from what eating does), though
			// eating it may still be pleasant.
			if (decision == prm.eatDecision && edible < 0.05f && e.delta[HUNGER] < 0.0f) {
				Expectation unscented = e;
				unscented.delta[HUNGER] = 0.0f;
				c.want = predictedRelief(d, D, unscented);
			}
			// Food is less pleasant the fuller one is (negative alliesthesia:
			// Cabanac 1971; sensory-specific satiety, Rolls; Berridge 1991).
			// Eating eases the need for pleasure, boredom and stress (the
			// genomes' own eating stimulus), so full creatures ate for fun:
			// 68% of meals that fed went to creatures with Hunger under 64
			// and 13% to those at 150 or more, in a crowded world short of
			// food (2026-11-16). Below satiationHunger, what eating is
			// expected to ease besides hunger shrinks with hunger, to
			// satedLiking when full. OPENC2E_NO_ALLIESTHESIA turns it off.
			static const bool noAlliesthesia = devenv("OPENC2E_NO_ALLIESTHESIA") != nullptr;
			if (!noAlliesthesia && decision == prm.eatDecision && (category < 36 || category > 39) && d[HUNGER] < prm.satiationHunger) {
				float liking = std::max(prm.satedLiking, d[HUNGER] / prm.satiationHunger);
				Expectation sated = e;
				if (edible < 0.05f && sated.delta[HUNGER] < 0.0f)
					sated.delta[HUNGER] = 0.0f;
				for (int i = 0; i < NUM_DRIVES; i++)
					if (i != HUNGER && sated.delta[i] < 0.0f)
						sated.delta[i] *= liking;
				c.want = predictedRelief(d, D, sated);
			}
			static const bool countCuriosity = devenv("OPENC2E_COUNT_CURIOSITY") != nullptr;
			if (countCuriosity || known < prm.firstTries || !record)
				c.novelty = curiosity / std::sqrt(1.0f + known);
			else
				c.novelty = curiosity * std::min(prm.lpMax, record->progress() / prm.lpScale);
			c.score = c.want + c.novelty - prm.distanceCost * distance;
			if (category >= 0 && category < (int)habituation.size()) {
				// 4. habituation scales the pull of an object
				float pull = c.want + c.novelty;
				c.score = pull * habituation[category] - prm.distanceCost * distance;
			}
			if (inEpisodeJustEnded && category == current.category && decision == current.decision)
				c.score += prm.commitment;
			const auto& z = appliedTemperament;
			for (auto& n : nudges)
				if (nudgeHolds(n, p, category, decision)) {
					if (n.verbs.empty() && n.nouns.empty())
						c.score += prm.innateNudge * n.effect();
					else // told to: fading, and by its temper and sociability
						c.score += prm.obey * n.effect() * (1.0f - (float)p.heardAge / prm.heardTicks) * (p.heardNamed ? 1.25f : 1.0f) * (heardDone > 0 ? 0.25f : 1.0f) * std::clamp(1.0f - 0.25f * z[4] + 0.15f * z[3], 0.3f, 1.6f);
				}
			// called: to look at what it was called to
			if (decision == 0 && category >= 0 && category == p.calledCategory && p.calledAge >= 0 && p.calledAge < prm.callTicks)
				// (told to do something it knows the word for: less)
				c.score += prm.callPull * (p.heardVerb >= 0 && p.heardAge >= 0 && p.heardAge <= p.calledAge + 1 ? 0.35f : 1.0f) * (1.0f - 0.5f * p.calledAge / prm.callTicks) * (p.heardNamed ? 1.25f : 1.0f) * std::clamp(1.0f + 0.2f * z[3], 0.5f, 1.5f);
			if (who && !who->who.empty()) {
				c.score += prm.strangerBias * z[0] * (1.0f - who->familiarity); // boldness
				if (decision == prm.hitDecision)
					c.score += prm.temperHit * z[4] * (0.3f + d[ANGER]); // temper
			}
			out.push_back(c);
		};
		for (auto& a : *p.actions) {
			if (a.needsObject) {
				for (auto& s : p.visible)
					if ((s.reachable && (s.activatable || (a.decision != 1 && a.decision != 2 && a.decision != 3))) ||
						// (called to look at it, or told to come to it: over its
						// head too, as the hand mostly is)
						(a.decision == 0 && s.category == p.calledCategory && p.calledAge >= 0 && p.calledAge < prm.callTicks) ||
						(a.decision == 4 && p.heardVerb == 4 && p.heardAge >= 0 && p.heardAge < prm.heardTicks && s.category == (p.heardNoun >= 0 ? p.heardNoun : 1) && std::fabs(s.y - p.y) < 160.0f))
						add(s.category, a.decision, s.distance, s.edible, (s.who.empty() && s.category != MIND_HEALING_FRUIT && s.category != MIND_ROTTEN_FRUIT) ? nullptr : &s);
			} else {
				// plain wandering never walks off dry land into water (food,
				// forage and seeking trips already check; the pond's new ramp let
				// wandering norns walk in and wade there, regression batch reg3)
				int dir = (a.decision == 26) ? 10 : (a.decision == 27) ? 11 : a.decision;
				if ((dir == 10 || dir == 11) && waterAhead(p, dir))
					continue;
				// nor walks into a wall right ahead (owner, 2026-11-10)
				if ((dir == 10 || dir == 11) && wallAhead(p, dir))
					continue;
				// nor a way found blocked here lately
				if ((dir == 10 || dir == 11) && isBlocked(p, dir))
					continue;				// nor a drop it has learned hurts
				if ((dir == 10 || dir == 11) && cliffAhead(p, dir))
					continue;
				add(-1, a.decision, 0.0f, 0.0f);
			}
		}
		// temperament: the shy back away from strangers close by; the sociable
		// go toward company; the energetic pace about when nothing presses
		{
			const auto& z = appliedTemperament;
			const SeenAgent* company = nullptr;
			float best = -1e9f;
			for (auto& s : p.visible) {
				if (s.who.empty())
					continue;
				if (z[0] < -0.5f && s.familiarity < 0.2f && s.distance < 250.0f) {
					int dir = dxTo(p, s.x) < 0 ? 11 : 10;
					if (!waterAhead(p, dir) && !wallAhead(p, dir)) {
						Candidate c{-1, dir, 0, prm.shyRetreat * -z[0] * (1.0f - s.familiarity), 0, 0};
						c.score = c.want;
						c.chain = true;
						c.who = "shy of " + s.who;
						out.push_back(c);
					}
				}
				float liked = s.affinity + 0.5f * s.familiarity - s.fear;
				if (s.reachable && s.distance > 150.0f && liked > best) {
					best = liked;
					company = &s;
				}
			}
			if (company && z[3] > 0.3f && company->fear < prm.retreatFear) {
				int dir = dxTo(p, company->x) < 0 ? 10 : 11;
				if (!waterAhead(p, dir) && !wallAhead(p, dir) && !isBlocked(p, dir)) {
					Candidate c{-1, dir, 0, prm.socialFollow * z[3] * (0.5f + std::max(0.0f, company->affinity)), 0, 0};
					c.score = c.want;
					c.chain = true;
					c.who = "toward " + company->who;
					out.push_back(c);
				}
			}
			if (D < 0.25f)
				for (auto& c : out)
					if (c.category < 0 && (c.decision == 26 || c.decision == 27))
						c.score += prm.energyPace * z[2];
		}
		// a feared creature close by: back away from it
		for (auto& s : p.visible) {
			if (s.who.empty() || s.fear < prm.retreatFear || s.distance > 400.0f)
				continue;
			int dir = dxTo(p, s.x) < 0 ? 11 : 10;
			if (waterAhead(p, dir) || wallAhead(p, dir))
				continue;
			Expectation away;
			away.delta[FEAR] = -0.3f * s.fear;
			Candidate c{-1, dir, 0, predictedRelief(d, D, away), 0, 0};
			c.score = c.want;
			c.chain = true;
			c.who = "away from " + s.who;
			out.push_back(c);
		}
		addSeekCandidates(p, d, D, out);
		inEpisodeJustEnded = false;
		return out;
	}
	bool inEpisodeJustEnded = false;

	// How much the best action on a category would help, counting only what
	// the creature has experienced or a specific instinct names: a vague hunch
	// ("eat anything when hungry") is reason to try what is at hand, not to
	// travel across the world for it.
	float knownRelief(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, int category) {
		float best = 0.0f;
		for (auto& a : *p.actions) {
			if (!a.needsObject)
				continue;
			Expectation& e = expectation(category, a.decision);
			if (e.count == 0 && e.prior <= 0.0f)
				continue;
			best = std::max(best, predictedRelief(d, D, e));
		}
		return best;
	}

	bool knowsFoodNear(float x, float y) const {
		for (auto& fp : foodPlaces)
			if (std::fabs(wrapD(fp.x - x)) < prm.foodMerge && std::fabs(fp.y - y) < prm.sameLevel)
				return true;
		return false;
	}

	void rememberFood(float x, float y, unsigned int tick, float confidence, bool meal, int category = -1) {
		for (auto& fp : foodPlaces)
			if (std::fabs(wrapD(fp.x - x)) < prm.foodMerge && std::fabs(fp.y - y) < prm.sameLevel) {
				fp.x = fp.x + wrapD(x - fp.x) / 4.0f; // (the short way round)
				fp.tick = tick;
				if (category >= 0)
					fp.category = category;
				fp.confidence = std::max(fp.confidence, confidence);
				fp.meals += meal ? 1 : 0;
				return;
			}
		if (foodPlaces.size() >= prm.foodPlaceMax) {
			// forget the least valuable: low confidence, few meals, long ago
			auto worst = std::min_element(foodPlaces.begin(), foodPlaces.end(), [](const FoodPlace& a, const FoodPlace& b) {
				return a.confidence * (1 + a.meals) < b.confidence * (1 + b.meals);
			});
			foodPlaces.erase(worst);
			foodTrip = -1; // indices moved
		}
		foodPlaces.push_back({x, y, tick, confidence, meal ? 1 : 0, 0});
		foodPlaces.back().category = category;
	}

	// What eating is expected to do for hunger: a typical meal, or the best
	// meal of something food-smelling it has had, if that was more.
	float expectedMealRelief(const std::array<float, NUM_DRIVES>& d, float D) {
		Expectation typical;
		typical.delta[HUNGER] = -prm.scentHungerRelief * 0.6f;
		for (auto& [key, e] : expectations) {
			auto sc = scents.find(key / 64 - 1);
			if (key % 64 == prm.eatDecision && e.count > 0 && sc != scents.end() && sc->second >= 0.2f)
				typical.delta[HUNGER] = std::min(typical.delta[HUNGER], std::max(e.delta[HUNGER], -prm.scentHungerRelief * 1.5f));
		}
		return predictedRelief(d, D, typical);
	}

	// ---- The place map ----
	Cell cellOf(float x, float y) const {
		if (wrapW > 0) {
			x = std::fmod(x, wrapW);
			if (x < 0)
				x += wrapW;
		}
		return {(int)std::floor(x / prm.cellWidth), (int)std::floor(y / prm.cellHeight)};
	}
	void addEdge(Cell from, Cell to, int dir, float cost, bool taken) {
		auto& edges = placeMap[from];
		for (auto& e : edges)
			if (e.to == to && e.dir == dir) {
				if (taken) {
					int n = std::min(e.uses, 9);
					e.cost = (e.cost * n + cost) / (n + 1);
					e.uses++;
					e.fails = std::max(0, e.fails - 1);
				}
				return;
			}
		if (placeEdges >= prm.placeEdgeMax)
			return;
		edges.push_back({to, dir, cost, taken ? 1 : 0, 0});
		placeEdges++;
	}
	// Every tick: a step from one cell into the next under the creature's
	// own power (walking, climbing, dropping; not riding or carried) is a way
	// between them. A level step is assumed to go back the same way too.
	// (Moved by a script is not its own power: the stream under the cave
	// lift carries creatures east along the channel 14 px a tick, which was
	// taken for walking east from the shaft's floor through the rock, and
	// creatures walked into that wall for ever; owner, 2026-11-13.)
	void updatePlaceMap(const MindPerception& p) {
		Cell c = cellOf(p.x, p.y);
		if (!mapCellValid || p.riding || p.carried || p.scriptMoved) {
			mapCell = c;
			mapX = p.x;
			mapY = p.y;
			mapCellValid = !p.riding && !p.carried && !p.scriptMoved;
			return;
		}
		if (c == mapCell)
			return;
		bool walking = inEpisode && (current.category >= 0 || current.decision == 10 || current.decision == 11 || current.decision == 26 || current.decision == 27);
		float mx = -dxTo(p, mapX), my = p.y - mapY; // how far it came
		if (walking && std::fabs(mx) + std::fabs(my) < prm.cellWidth * 3) { // (no more in one go: a teleport)
			int dir = mx < -2.0f ? 10 : mx > 2.0f ? 11 : (current.decision == 10 || current.decision == 11) ? current.decision : 0;
			if (dir) {
				addEdge(mapCell, c, dir, std::fabs(mx) + std::fabs(my), true);
				if (std::fabs(my) < prm.cellHeight * 0.3f)
					addEdge(c, mapCell, dir == 10 ? 11 : 10, std::fabs(mx), false);
			}
		}
		mapCell = c;
		mapX = p.x;
		mapY = p.y;
	}

	// Food further than a straight walk: search the place map (and the
	// remembered rides) for the remembered meal best worth its way, and
	// offer the first step. Returns whether a step was offered.
	bool addRouteToFood(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float meal, std::vector<Candidate>& out) {
		struct Step {
			int dir = 0, category = -1, decision = -1;
			int lift = -1; // a learned lift ride (index in links): walk to it, call it, ride it
		};
		Cell start = cellOf(p.x, p.y);
		std::map<Cell, float> dist;
		std::map<Cell, Step> first;
		using QE = std::pair<float, Cell>;
		std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
		dist[start] = 0;
		q.push({0, start});
		while (!q.empty()) {
			auto [c0, cell] = q.top();
			q.pop();
			if (c0 > dist[cell] || c0 > prm.routeMaxCost)
				continue;
			auto relax = [&](Cell to, float w, Step step) {
				float nd = c0 + w;
				auto it = dist.find(to);
				if (it == dist.end() || nd < it->second) {
					dist[to] = nd;
					first[to] = cell == start ? step : first[cell];
					q.push({nd, to});
				}
			};
			if (auto it = placeMap.find(cell); it != placeMap.end())
				for (auto& e : it->second) {
					if (e.fails > e.uses + 2)
						continue; // tried and failed too often
					if (cell == start && (isBlocked(p, e.dir) || wallAhead(p, e.dir) || waterAhead(p, e.dir)))
						continue;
					relax(e.to, e.cost * (1.0f + 0.5f * e.fails) * (e.uses == 0 ? 1.5f : 1.0f), {e.dir, -1, -1});
				}
			// rides: a device used from this cell (or next to it on the level)
			for (auto& tl : travelLinks) {
				if (tl.fails > tl.uses)
					continue;
				Cell from = cellOf(tl.fromX, tl.fromY);
				if (from.second != cell.second || std::abs(from.first - cell.first) > 1)
					continue;
				Step step{0, tl.category, tl.decision};
				if (cell == start) {
					bool ready = false;
					for (auto& sa : p.visible)
						if (sa.category == tl.category && sa.reachable && sa.activatable)
							ready = true;
					if (isFutile(p, tl.category, tl.decision))
						continue;
					if (!ready) {
						// not at hand from here: walk to where it was used
						if (std::fabs(dxTo(p, tl.fromX)) <= prm.atPlace)
							continue;
						int dir = dxTo(p, tl.fromX) < 0 ? 10 : 11;
						if (isBlocked(p, dir) || wallAhead(p, dir) || waterAhead(p, dir))
							continue;
						step = {dir, -1, -1};
					}
				}
				relax(cellOf(tl.toX, tl.toY), prm.floorChangeDistance + std::fabs(dxTo(p, tl.fromX)) * (cell == start ? 1.0f : 0.0f), step);
			}
			// learned lift rides: from the floor at the lift to the floor it took the creature to
			for (size_t i = 0; i < links.size(); i++) {
				const Link& l = links[i];
				Cell from = cellOf(l.x, l.fromY);
				if (from.second != cell.second || std::abs(from.first - cell.first) > 1)
					continue;
				Step step;
				step.lift = (int)i;
				relax(cellOf(l.x, l.toY), prm.floorChangeDistance, step);
			}
		}
		// the meal best worth its way
		int bestIdx = -1;
		float bestWant = 0, bestCost = 0;
		Step bestStep;
		for (size_t i = 0; i < foodPlaces.size(); i++) {
			const FoodPlace& fp = foodPlaces[i];
			if (p.tick < fp.cooldownUntil)
				continue;
			Cell fc = cellOf(fp.x, fp.y);
			for (int ox = -1; ox <= 1; ox++)
				for (int oy = -1; oy <= 1; oy++) {
					Cell c{fc.first + ox, fc.second + oy};
					if (c == start)
						continue; // there already, or nearly: the walk or the food in sight takes over
					auto it = dist.find(c);
					if (it == dist.end())
						continue;
					float cost = it->second + (std::abs(ox) + std::abs(oy)) * prm.cellWidth;
					float want = meal * fp.confidence * travelDiscount(cost, 0, d[HUNGER]);
					if (want > bestWant) {
						bestWant = want;
						bestIdx = (int)i;
						bestCost = cost;
						bestStep = first[c];
					}
				}
		}
		if (bestIdx < 0 || bestWant < prm.seekMinWant || (bestStep.dir == 0 && bestStep.category < 0 && bestStep.lift < 0))
			return false;
		// a lift ride: get to the lift, ride it if it is here, else call it
		if (bestStep.lift >= 0) {
			const Link& l = links[bestStep.lift];
			const SeenAgent* lift = nullptr;
			const SeenAgent* button = nullptr;
			for (auto& sa : p.visible) {
				if (sa.category == prm.liftCategory)
					lift = &sa;
				if (sa.category == prm.buttonCategory && sa.reachable && sa.activatable && std::fabs(sa.y - p.y) < prm.sameLevel)
					button = &sa;
			}
			bool liftHere = lift && std::fabs(dxTo(p, lift->x)) < prm.atPlace * 1.5f && std::fabs(lift->y - p.y) < prm.sameLevel;
			if (std::fabs(dxTo(p, l.x)) > prm.atPlace) {
				int dir = dxTo(p, l.x) < 0 ? 10 : 11;
				if (isBlocked(p, dir) || wallAhead(p, dir) || waterAhead(p, dir))
					return false;
				bestStep = {dir, -1, -1};
			} else if (liftHere && !isFutile(p, prm.liftCategory, l.decision))
				bestStep = {0, prm.liftCategory, l.decision};
			else if (button && !dudButtonAt(p) && !isFutile(p, prm.buttonCategory, 1))
				bestStep = {0, prm.buttonCategory, 1};
			else
				return false;
		}
		// a route that has not got any shorter for a while: that place is out
		// of reach from here for now
		if (bestIdx != routeTarget) {
			routeTarget = bestIdx;
			routeBest = bestCost;
			routeSince = p.tick;
		} else if (bestCost < routeBest - prm.foodTripGain) {
			routeBest = bestCost;
			routeSince = p.tick;
		} else if (p.tick > routeSince + prm.foodTripTicks * 2) {
			FoodPlace& t = foodPlaces[bestIdx];
			t.confidence *= 0.5f;
			t.cooldownUntil = p.tick + prm.foodCooldown;
			MIND_LOG("[homeostat] creature {} t{}: the way to food at {:.0f},{:.0f} gets no shorter; giving it a rest\n", p.creatureId, p.tick, t.x, t.y);
			routeTarget = -1;
			return false;
		}
		foodTrip = -1; // (the straight-line trip check does not apply)
		// developer aid: OPENC2E_WATCH_ROUTES (1, or a creature id) prints each
		// route step offered: the food it leads to and the step
		static const char* watchRoutes = devenv("OPENC2E_WATCH_ROUTES");
		if (watchRoutes && (std::string(watchRoutes) == "1" || std::to_string(p.creatureId) == watchRoutes)) {
			const FoodPlace& t = foodPlaces[bestIdx];
			fmt::print("[homeostat] creature {} t{}: ROUTE at {:.0f},{:.0f} cell {},{} to food {:.0f},{:.0f} cost {:.0f}: step dir {} cat {} dec {} lift {}\n", p.creatureId, p.tick, p.x,
				p.y, start.first, start.second, t.x, t.y, bestCost, bestStep.dir, bestStep.category, bestStep.decision, bestStep.lift);
		}
		Candidate c{bestStep.category, bestStep.category >= 0 ? bestStep.decision : bestStep.dir, bestCost, bestWant, 0, bestWant};
		c.chain = true;
		c.food = true;
		c.route = true;
		out.push_back(c);
		return true;
	}

	float travelDiscount(float distance, int floors, float hunger) const {
		float half = prm.travelHalf * (1.0f + prm.hungerPatience * hunger * hunger);
		return std::pow(0.5f, (distance + floors * prm.floorChangeDistance) / half);
	}

	// Heading for the best place on the food map (same floor: walk; another
	// floor: steps toward changing floors).
	void addFoodSeek(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, bool canWest, bool canEast, std::vector<Candidate>& out) {
		for (auto& s : p.visible)
			if (s.reachable && s.edible > 0.1f)
				return; // food at hand
		float meal = expectedMealRelief(d, D);
		if (meal < prm.seekMinWant)
			return;
		const FoodPlace* bestSame = nullptr;
		const FoodPlace* bestOther = nullptr;
		float wantSame = 0.0f, wantOther = 0.0f;
		// progress on the current trip
		if (foodTrip >= 0 && foodTrip < (int)foodPlaces.size()) {
			FoodPlace& t = foodPlaces[foodTrip];
			float dist = std::fabs(dxTo(p, t.x)) + std::fabs(t.y - p.y);
			bool moved = std::fabs(dxTo(p, foodTripFromX)) + std::fabs(p.y - foodTripFromY) > prm.foodTripGain * 0.5f;
			if (dist < foodTripBest - prm.foodTripGain && moved) {
				foodTripBest = dist;
				foodTripSince = p.tick;
				foodTripFromX = p.x;
				foodTripFromY = p.y;
			} else if (p.tick > foodTripSince + prm.foodTripTicks) {
				t.confidence *= 0.5f; // out of reach from here, for now
				t.cooldownUntil = p.tick + prm.foodCooldown;
				foodTrip = -1;
			}
		}
		for (auto& fp : foodPlaces) {
			if (p.tick < fp.cooldownUntil)
				continue;
			float dx = std::fabs(dxTo(p, fp.x)), dy = std::fabs(fp.y - p.y);
			bool same = dy < prm.sameLevel;
			if (same && dx < prm.foodArrived)
				continue; // here already
			int floors = same ? 0 : std::max(1, (int)std::lround(dy / prm.scentFloorStep));
			float want = meal * fp.confidence * travelDiscount(dx, floors, d[HUNGER]);
			if (same && want > wantSame) {
				wantSame = want;
				bestSame = &fp;
			} else if (!same && want > wantOther) {
				wantOther = want;
				bestOther = &fp;
			}
		}
		// The place map steps in where a straight walk will not do: the best
		// meal is on another floor, or the walk toward it is barred (water, a
		// wall, a drop). A meal along this floor is walked to directly, even
		// to a spot not yet on the map. (Routes everywhere crowded out
		// searching: batch norns kept going back to remembered, emptied spots.)
		{
			bool walkable = false;
			if (bestSame && wantSame >= prm.seekMinWant && wantSame >= wantOther) {
				int dir = dxTo(p, bestSame->x) < 0 ? 10 : 11;
				walkable = !waterAhead(p, dir) && !((dir == 10 && !canWest) || (dir == 11 && !canEast) || isBlocked(p, dir) || cliffAhead(p, dir, bestSame->y));
			}
			if (!walkable && addRouteToFood(p, d, meal, out))
				return; // a known way, step by step
			if (walkable)
				routeTarget = -1;
		}
		// a known link toward the best place, if it saves a long way
		{
			const FoodPlace* target = (bestSame && wantSame >= wantOther) ? bestSame : bestOther;
			float want = std::max(wantSame, wantOther);
			Candidate t{};
			t.food = true;
			if (target && want >= prm.seekMinWant)
				addLinkToward(p, target->x, target->y, want, t, out);
		}
		auto startTrip = [&](const FoodPlace* fp) {
			int idx = (int)(fp - foodPlaces.data());
			if (idx != foodTrip) {
				foodTrip = idx;
				foodTripBest = std::fabs(dxTo(p, fp->x)) + std::fabs(fp->y - p.y);
				foodTripSince = p.tick;
				foodTripFromX = p.x;
				foodTripFromY = p.y;
			}
		};
		if (bestSame && wantSame >= prm.seekMinWant && wantSame >= wantOther) {
			startTrip(bestSame);
			int decision = dxTo(p, bestSame->x) < 0 ? 10 : 11;
			if (waterAhead(p, decision)) {
				Candidate t{};
				t.food = true;
				addBoatInstead(p, decision, wantSame, t, out);
				return;
			}
			if (!((decision == 10 && !canWest) || (decision == 11 && !canEast) || isBlocked(p, decision) || cliffAhead(p, decision, bestSame->y))) {
				Candidate c{-1, decision, std::fabs(dxTo(p, bestSame->x)), wantSame, 0, wantSame};
				c.chain = true;
				c.food = true;
				out.push_back(c);
				return;
			}
			// The way there is barred from here (a wall, a cliff, a way found
			// blocked) and no route was known: the place is out of reach from
			// here, as a trip that gets no nearer is, at once. It stood as the
			// best place, offering nothing, for foodTripTicks: hungry creatures
			// at the west wall of the seam corner (room 180) stood still,
			// waiting on food the short way round, across the seam, which is
			// shut at that height, or on tomatoes hanging out of reach
			// (owner, 2026-11-16: "mind gives up waiting").
			static const bool noBarredGiveUp = devenv("OPENC2E_NO_BARRED_GIVEUP") != nullptr; // (to compare)
			if (!noBarredGiveUp) {
				FoodPlace& t = foodPlaces[bestSame - foodPlaces.data()];
				t.confidence *= 0.5f;
				t.cooldownUntil = p.tick + prm.foodCooldown;
				foodTrip = -1;
			}
		}
		if (bestOther && wantOther >= prm.seekMinWant) {
			startTrip(bestOther);
			size_t before = out.size();
			addFloorChange(p, wantOther, bestOther->y, -1, canWest, canEast, out);
			for (size_t i = before; i < out.size(); i++)
				out[i].food = true;
		}
	}

	// Leaving a place to find food (see starveLeaveHunger)
	void addStarveLeave(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, bool canWest, bool canEast, std::vector<Candidate>& out) {
		// (developer aid: OPENC2E_NO_STARVELEAVE turns this off, for comparison)
		static const bool off = devenv("OPENC2E_NO_STARVELEAVE") != nullptr;
		if (off || p.inWater || d[HUNGER] < prm.starveLeaveHunger || hungryTicks < prm.restlessRamp)
			return;
		for (auto& s : p.visible)
			if (s.reachable && s.edible > 0.1f)
				return; // food at hand
		float want = expectedMealRelief(d, D) * prm.starveLeaveShare;
		if (want < prm.seekMinWant)
			return;
		// a try from here lately that took it nowhere (the big teleporter pad
		// by the incubator only receives: starving arrivals kept working it)
		if (p.tick < starveLeaveTick + 300 && std::fabs(dxTo(p, starveLeaveX)) < 300.0f && std::fabs(p.y - starveLeaveY) < prm.sameLevel)
			return;
		// a known way from here that leads far off
		const TravelLink* best = nullptr;
		float bestFar = prm.starveLeaveFar;
		for (auto& tl : travelLinks) {
			if (tl.category == prm.liftCategory || tl.category == prm.buttonCategory || tl.category == 3 || tl.category == 9)
				continue; // (lifts and their buttons: the floor-change steps have them; machines and doors: airlocks)
			if (std::fabs(dxTo(p, tl.fromX)) > prm.linkNear || std::fabs(tl.fromY - p.y) > prm.sameLevel)
				continue;
			bool usable = false;
			for (auto& sa : p.visible)
				if (sa.category == tl.category && sa.reachable && sa.activatable)
					usable = true;
			if (!usable || isFutile(p, tl.category, tl.decision))
				continue;
			float far = std::fabs(dxTo(p, tl.toX)) + std::fabs(tl.toY - p.y);
			if (far > bestFar) {
				bestFar = far;
				best = &tl;
			}
		}
		if (best) {
			Candidate c{best->category, best->decision, 0, want, 0, want};
			c.chain = true;
			c.food = true;
			c.place = "leave:Hunger";
			out.push_back(c);
			return;
		}
		// or a teleporter in view, tried
		for (auto& sa : p.visible)
			if (sa.category == prm.teleporterCategory && sa.reachable && sa.activatable && !isFutile(p, sa.category, 1)) {
				Candidate c{sa.category, 1, sa.distance, want * 0.8f, 0, want * 0.8f};
				c.chain = true;
				c.food = true;
				c.place = "leave:Hunger";
				out.push_back(c);
				return;
			}
		// or a lift (owner, 2026-11-20: "smarter lift use", for the west
		// airlock's corridor, rooms 120-118, whose only ways out are two cage
		// lifts): starving creatures there stayed for minutes at Hunger 255,
		// their food memories thousands of px off and so worth too little for
		// the floor-change steps, while this rule left lifts out. As a last
		// resort it rides a lift that is here (a way it has not found
		// futile; activate 1 and 2 go opposite ways), or calls one, or walks
		// to one in sight on its floor. A ride is learned as any other
		// (learnRide), so the way is remembered. OPENC2E_NO_STARVE_LIFT turns
		// this part off.
		static const bool noLift = devenv("OPENC2E_NO_STARVE_LIFT") != nullptr;
		if (!noLift) {
			const SeenAgent* lift = nullptr;
			const SeenAgent* button = nullptr;
			for (auto& sa : p.visible) {
				if (sa.category == prm.liftCategory && std::fabs(sa.y - p.y) < prm.sameLevel && (!lift || sa.distance < lift->distance))
					lift = &sa;
				if (sa.category == prm.buttonCategory && sa.reachable && sa.activatable && std::fabs(sa.y - p.y) < prm.sameLevel && !dudButtonAt(p))
					button = &sa;
			}
			bool liftHere = lift && std::fabs(dxTo(p, lift->x)) < prm.atPlace * 1.5f && lift->reachable;
			if (liftHere) {
				for (int dec : {1, 2})
					if (!isFutile(p, prm.liftCategory, dec)) {
						Candidate c{prm.liftCategory, dec, lift->distance, want * 0.7f, 0, want * 0.7f};
						c.chain = true;
						c.food = true;
						c.place = "leave:Hunger";
						out.push_back(c);
						return;
					}
			} else if (button && !isFutile(p, prm.buttonCategory, 1)) {
				Candidate c{prm.buttonCategory, 1, button->distance, want * 0.7f, 0, want * 0.7f};
				c.chain = true;
				c.food = true;
				c.place = "leave:Hunger";
				out.push_back(c);
				return;
			} else if (lift) {
				int decision = dxTo(p, lift->x) < 0 ? 10 : 11;
				if (!((decision == 10 && !canWest) || (decision == 11 && !canEast) || isBlocked(p, decision))) {
					Candidate c{-1, decision, std::fabs(dxTo(p, lift->x)), want * 0.7f, 0, want * 0.7f};
					c.seekCategory = prm.liftCategory;
					c.food = true;
					c.place = "leave:Hunger";
					out.push_back(c);
					return;
				}
			}
		}
		// or one out of view on this floor that it remembers: walk to it, and
		// it is used when in view (above). The gene lab's lower room has its
		// teleporter at the west end and starving creatures piled up at the
		// east end, 750 px away, out of its sight (owner's screencast,
		// 2026-11-14 night). OPENC2E_NO_STARVEWALK turns this part off.
		static const bool noWalk = devenv("OPENC2E_NO_STARVEWALK") != nullptr;
		if (noWalk)
			return;
		// (one in view but not yet in reach counts too: from the gene lab's
		// east wall the teleporter is in sight 700 px off, too far to be
		// "reachable", so neither part acted on it; 2026-11-14 night)
		float tx = 0, ty = 0;
		bool known = false;
		float nearest = 1e9f;
		for (auto& sa : p.visible)
			if (sa.category == prm.teleporterCategory && sa.distance < nearest) {
				nearest = sa.distance;
				tx = sa.x;
				ty = sa.y;
				known = true;
			}
		if (!known) {
			auto tp = places.find(prm.teleporterCategory);
			if (tp == places.end())
				return;
			tx = tp->second.x;
			ty = tp->second.y;
		}
		float dx = dxTo(p, tx), dy = ty - p.y;
		if (std::fabs(dy) > prm.seekMaxHeight || std::fabs(dx) < prm.seekArrived)
			return;
		int decision = dx < 0 ? 10 : 11;
		if ((decision == 10 && !canWest) || (decision == 11 && !canEast) || isBlocked(p, decision))
			return;
		Candidate c{-1, decision, std::fabs(dx), want * 0.8f, 0, want * 0.8f};
		c.seekCategory = prm.teleporterCategory;
		c.food = true;
		c.place = "leave:Hunger";
		out.push_back(c);
	}

	// Foraging: searching when nothing better is known.
	void addForage(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, bool canWest, bool canEast, std::vector<Candidate>& out) {
		float restless = std::clamp(hungryTicks / prm.restlessRamp, 0.0f, 1.0f);
		float threshold = prm.forageHunger - (prm.forageHunger - prm.forageHungerRestless) * restless;
		if (d[HUNGER] < threshold)
			return;
		for (auto& s : p.visible)
			if (s.edible > 0.1f)
				return;
		float want = expectedMealRelief(d, D) * prm.forageShare * (1.0f + prm.restlessBoost * restless) * (d[HUNGER] - threshold) / (1.0f - threshold);
		if (want < prm.seekMinWant)
			return;
		// something better to go on: a food trip or a smell worth at least
		// as much as searching. (Any lead used to stop the search: a norn
		// starving at the world's bottom-right corner remembered two food
		// places it had all but given up on (confidence 0.06), so it never
		// searched, and rested 135 times in a row; owner, 2026-11-11.)
		for (auto& c : out)
			if ((c.food || c.scent) && c.want >= want)
				return;
		auto open = [&](int dir) { return ((dir == 10 && canWest) || (dir == 11 && canEast)) && !isBlocked(p, dir) && !waterAhead(p, dir) && !cliffAhead(p, dir); };
		// a known link to somewhere not visited lately: a way to search further
		{
			const TravelLink* bestLink = nullptr;
			float bestStale = 3000.0f; // ticks since visited
			for (auto& tl : travelLinks) {
				if (std::fabs(dxTo(p, tl.fromX)) > prm.linkNear || std::fabs(tl.fromY - p.y) > prm.sameLevel)
					continue;
				auto it = visited.find({(int)std::floor(tl.toX / prm.visitBinWidth), (int)std::floor(tl.toY / prm.visitBinHeight)});
				float stale = it == visited.end() ? 100000.0f : (float)(p.tick - it->second);
				if (stale > bestStale) {
					bestStale = stale;
					bestLink = &tl;
				}
			}
			if (bestLink) {
				Candidate t{};
				t.forage = true;
				addLinkToward(p, bestLink->toX, bestLink->toY, want, t, out);
			}
		}
		// water ahead with a boat in view: rowing across is a way to search
		// further, but not back to a shore searched lately (a known ride that
		// way lands where it was not long ago): starving norns took the east
		// sea's boat to the palm side, found nothing, took it straight back,
		// found nothing, and so on, a ride every 40 decisions (owner's
		// screencast, 2026-11-19)
		auto searchedLately = [&](int dir) {
			for (auto& tl : travelLinks) {
				if (tl.category != prm.boatCategory || std::fabs(dxTo(p, tl.fromX)) > prm.linkNear || std::fabs(tl.fromY - p.y) > prm.sameLevel)
					continue;
				float go = wrapD(tl.toX - tl.fromX);
				if ((dir == 10) != (go < 0))
					continue;
				auto it = visited.find({(int)std::floor(tl.toX / prm.visitBinWidth), (int)std::floor(tl.toY / prm.visitBinHeight)});
				if (it != visited.end() && p.tick - it->second < prm.forageRevisit)
					return true;
			}
			return false;
		};
		for (int dir : {10, 11})
			if (waterAhead(p, dir) && !searchedLately(dir)) {
				Candidate t{};
				t.forage = true;
				addBoatInstead(p, dir, want, t, out);
			}
		if (forageHeading != 0 && p.tick > forageStartTick + prm.forageStuckTicks) {
			float progress = forageHeading == 10 ? dxTo(p, forageStartX) : -dxTo(p, forageStartX);
			if (progress < prm.foragePass) {
				forageStuckFlips++;
				int other = forageHeading == 10 ? 11 : 10;
				forageHeading = forageStuckFlips >= 2 || !open(other) ? 0 : other; // both ways stuck: another floor
				forageUntil = p.tick + prm.forageCommit;
			} else
				forageStuckFlips = 0;
			forageStartX = p.x;
			forageStartTick = p.tick;
		}
		if (forageStuckFlips >= 2) {
			// both ways stuck on this floor: look for food on another one
			size_t before = out.size();
			addFloorChange(p, want, p.y + forageVertical * prm.scentFloorStep, -1, canWest, canEast, out);
			for (size_t i = before; i < out.size(); i++)
				out[i].forage = true;
			if (std::fabs(p.y - forageStartY) > prm.minRide) {
				forageStuckFlips = 0; // reached another floor
				forageHeading = 0;
				lastFloorChangeTick = p.tick;
			}
			return;
		}
		if (forageHeading == 0 || p.tick >= forageUntil || !open(forageHeading)) {
			forageStartX = p.x;
			forageStartY = p.y;
			forageStartTick = p.tick;
			// staleness of the next few bins each way on this floor
			int bx = (int)std::floor(p.x / prm.visitBinWidth), by = (int)std::floor(p.y / prm.visitBinHeight);
			auto stale = [&](int dir) {
				float total = 0.0f;
				for (int k = 1; k <= prm.forageLookBins; k++) {
					auto it = visited.find({bx + (dir == 10 ? -k : k), by});
					total += it == visited.end() ? 10000.0f : (float)(p.tick - it->second);
				}
				return total;
			};
			// another floor? the less recently visited of the floors above and
			// below; more likely the longer the creature has gone hungry, and
			// certain once this floor has been searched both ways
			auto staleRow = [&](int dy) {
				float total = 0.0f;
				for (int k = -prm.forageLookBins; k <= prm.forageLookBins; k++) {
					auto it = visited.find({bx + k, by + dy});
					total += it == visited.end() ? 10000.0f : (float)(p.tick - it->second);
				}
				return total;
			};
			forageVertical = staleRow(-1) >= staleRow(1) ? -1 : 1;
			float searched = (float)prm.floorSearched * prm.forageLookBins;
			bool floorDone = stale(10) < searched && stale(11) < searched;
			float roll = (float)((p.tick * 2654435761u) % 1000u) / 1000.0f;
			// just come from another floor, and that one is searched too:
			// changing floors again only rides back and forth (hungry
			// creatures crowded the cave lift's shaft riding it up and down
			// between two searched floors; owner, 2026-11-13). Walk on along
			// this floor instead, toward the less recently visited side,
			// past the nearby bins.
			float otherRow = staleRow(forageVertical);
			bool justChanged = p.tick < lastFloorChangeTick + prm.forageCommit * 2;
			if (justChanged && floorDone && otherRow < (float)prm.floorSearched * (2 * prm.forageLookBins + 1)) {
				int choice = 0;
				if (open(10) && open(11))
					choice = stale(10) >= stale(11) ? 10 : 11;
				else if (open(10))
					choice = 10;
				else if (open(11))
					choice = 11;
				if (choice) {
					forageHeading = choice;
					forageUntil = p.tick + prm.forageCommit * 3;
					forageStuckFlips = 0;
					floorDone = false;
					roll = 1.0f;
				}
			}
			if (floorDone || roll < prm.restlessLeaveFloor * restless) {
				forageStuckFlips = 2; // look on another floor (handled next tick)
				forageStartY = p.y;
				forageHeading = 0;
				forageUntil = p.tick + prm.forageCommit;
				return;
			}
			int choice = 0;
			if (open(10) && open(11))
				choice = stale(10) >= stale(11) ? 10 : 11;
			else if (open(10))
				choice = 10;
			else if (open(11))
				choice = 11;
			forageHeading = choice;
			forageUntil = p.tick + prm.forageCommit;
		}
		if (forageHeading != 0) {
			Candidate c{-1, forageHeading, 0, want, 0, want};
			c.chain = true;
			c.forage = true;
			out.push_back(c);
		} else {
			// both ways blocked: try another floor
			size_t before = out.size();
			addFloorChange(p, want, p.y + forageVertical * prm.scentFloorStep, -1, canWest, canEast, out);
			for (size_t i = before; i < out.size(); i++)
				out[i].forage = true;
		}
	}

	// Which boat action goes east (dir 11) or west (10): learned rides first,
	// then the innate guess (activate 1 east, activate 2 west; C2 decisions 1 and 2).
	int boatDecisionFor(int dir, unsigned int tick) const {
		int d = dir == 11 ? 1 : 2; // decisions: 1 activate 1, 2 activate 2
		for (auto& [decision, bl] : boatLinks)
			if (bl.rides > 0 && ((dir == 11 && bl.dx > 0) || (dir == 10 && bl.dx < 0))) {
				d = decision;
				break;
			}
		// a boat already moored on the far side ignores "that way": after a
		// press that gave no ride, try the other action for a while
		return tick < boatFlipUntil ? 3 - d : d;
	}

	// A trip walking dir would go into water: if a boat is in view, offer
	// rowing that way instead (and do not offer the walk).
	// A drop ahead it expects to hurt: learned from its own falls (none at
	// first: a young creature walks off ledges until a fall teaches it).
	float fallHurtExpected(const MindPerception& p, int dir) const {
		float drop = dir == 10 ? p.dropWest : dir == 11 ? p.dropEast : 0.0f;
		if (drop <= prm.harmlessDrop)
			return 0.0f;
		float expect = fallHurtPerPx * (drop - prm.harmlessDrop);
		for (auto& f : painfulDrops)
			if (f.dir == dir && std::fabs(dxTo(p, f.x)) < prm.fallSpotNear && std::fabs(f.y - p.y) < prm.sameLevel + 60.0f)
				expect = std::max(expect, f.hurt);
		return expect;
	}
	bool cliffAhead(const MindPerception& p, int dir, float targetY = -1e9f) const {
		float expect = fallHurtExpected(p, dir);
		if (expect <= 0.0f)
			return false;
		float drop = dir == 10 ? p.dropWest : dir == 11 ? p.dropEast : 0.0f;
		bool goingDown = targetY > p.y + drop * 0.5f; // a trip going that much lower
		return expect > (goingDown ? prm.fallAvoidHurtGoingDown : prm.fallAvoidHurt);
	}
	// what falls have taught
	float fallHurtPerPx = 0.0f;
	struct PainfulDrop {
		float x = 0, y = 0; // where it stepped off (its middle, its feet's height)
		int dir = 0;
		float hurt = 0;
	};
	std::vector<PainfulDrop> painfulDrops;
	void learnFromFall(const MindPerception& p) {
		if (p.fellHeight <= 0.0f)
			return;
		if (p.fellHurt <= 0.0f || p.fellHeight <= prm.harmlessDrop)
			return; // a step down, or into water: nothing to learn
		int dir = dxTo(p, p.fellFromX) > 0 ? 10 : 11; // it went from there to here
		float perPx = p.fellHurt / (p.fellHeight - prm.harmlessDrop);
		fallHurtPerPx = fallHurtPerPx <= 0.0f ? perPx : fallHurtPerPx + 0.5f * (perPx - fallHurtPerPx);
		bool known = false;
		for (auto& f : painfulDrops)
			if (f.dir == dir && std::fabs(dxTo(p, f.x)) < prm.fallSpotNear && std::fabs(f.y - p.fellFromY) < prm.sameLevel) {
				f.hurt = std::max(f.hurt, p.fellHurt);
				known = true;
			}
		if (!known) {
			if (painfulDrops.size() >= 32)
				painfulDrops.erase(painfulDrops.begin());
			// (its middle's height then, as the perception's y is its middle)
			painfulDrops.push_back({p.fellFromX, p.fellFromY - 40.0f, dir, p.fellHurt});
		}
		MIND_LOG("[homeostat] creature {} t{}: fell {:.0f} px and it hurt ({:.2f}); keeps away from that drop (and from falls like it: {:.4f} a px)\n",
			p.creatureId, p.tick, p.fellHeight, p.fellHurt, fallHurtPerPx);
		if (!p.self.empty() && !watchingOff()) {
			Demo dm;
			dm.kind = Demo::FALL;
			dm.who = p.self;
			dm.x = p.fellFromX;
			dm.y = p.fellFromY - 40.0f;
			dm.dir = dir;
			dm.hurt = p.fellHurt;
			demoBoard().post(std::move(dm));
		}
	}
	// a drop another was seen to fall from and be hurt (watchOthers)
	void learnSeenFall(const Demo& dm, float w) {
		float hurt = dm.hurt * w;
		if (hurt <= 0.0f)
			return;
		for (auto& f : painfulDrops)
			if (f.dir == dm.dir && std::fabs(wrapD(f.x - dm.x)) < prm.fallSpotNear && std::fabs(f.y - dm.y) < prm.sameLevel) {
				f.hurt = std::max(f.hurt, hurt);
				return;
			}
		if (painfulDrops.size() >= 32)
			painfulDrops.erase(painfulDrops.begin());
		painfulDrops.push_back({dm.x, dm.y, dm.dir, hurt});
	}

	// ---- Watching others (owner, 2026-11-20) ----
	std::array<float, NUM_DRIVES> hurtSum{}; // its own hurt in this room lately, to post
	float hurtTicks = 0;
	int hurtRoom = -1;
	int lastUsedCategory = -1; // a thing it has just used (to name it)
	unsigned int lastUsedTick = 0;
	unsigned long demoSeen = 0; // the board's last post read
	bool demoStarted = false;
	// What creatures in sight were seen to do, since the last tick, learned
	// as from its own doing at a share (Params::watchShare): kin count most
	// (watchStranger for the rest), the young most (watchStage). Its own
	// readiness to learn about each need (mind genes) applies as to its own
	// experience. Nothing from before it woke or was loaded.
	void watchOthers(const MindPerception& p) {
		DemoBoard& board = demoBoard();
		if (!demoStarted || p.asleep || p.self.empty() || watchingOff()) {
			demoStarted = true;
			demoSeen = board.seq;
			return;
		}
		if (board.seq == demoSeen)
			return;
		static const bool watch = devenv("OPENC2E_WATCH_WATCHING") != nullptr;
		int stage = std::clamp((int)p.stage, 0, 6);
		for (auto& dm : board.demos) {
			if (dm.seq <= demoSeen || dm.who == p.self)
				continue;
			const SeenAgent* sa = nullptr;
			for (auto& v : p.visible)
				if (v.who == dm.who) {
					sa = &v;
					break;
				}
			if (!sa)
				continue;
			float w = prm.watchShare * (sa->kin ? 1.0f : prm.watchStranger) * prm.watchStage[(size_t)stage];
			switch (dm.kind) {
				case Demo::OUTCOME: {
					Expectation& e = expectation(dm.category, dm.decision);
					for (int i = 0; i < NUM_DRIVES; i++)
						if (dm.felt[i])
							e.delta[i] += w * prm.learningRate * prm.driveLearning[i] * (dm.delta[i] > e.delta[i] ? prm.worseLearning : prm.betterLearning) *
										  (dm.delta[i] - e.delta[i]);
					break;
				}
				case Demo::MEAL: {
					float confidence = std::min(0.6f, 1.8f * w);
					if (confidence >= 0.1f)
						rememberFood(dm.x, dm.y, p.tick, confidence, false, dm.category);
					break;
				}
				case Demo::HURT: {
					if (dm.room < 0 || (!rooms.count(dm.room) && rooms.size() >= prm.roomMemoryMax))
						break;
					bool known = rooms.count(dm.room) > 0;
					RoomMemory& r = rooms[dm.room];
					if (!known) {
						r.x = dm.roomX;
						r.floor = dm.roomFloor;
						r.metaroom = dm.metaroom;
						r.water = dm.water;
					}
					// the hurt seen, over the time seen; its other needs as they go
					// anywhere (so the room reads neither better nor worse for them)
					float seenTicks = w * dm.ticks;
					for (int i = 0; i < NUM_DRIVES; i++) {
						if (dm.felt[i])
							r.feelSum[i] += w * prm.driveLearning[i] * dm.delta[i];
						else if (feelBaseTicks > 0.0f)
							r.feelSum[i] += feelBase[i] / feelBaseTicks * seenTicks;
					}
					r.feelTicks += seenTicks;
					break;
				}
				case Demo::FALL:
					learnSeenFall(dm, w * prm.driveLearning[PAIN]);
					break;
			}
			if (watch)
				fmt::print("[homeostat] creature {} t{}: saw {} ({}) {} (weight {:.2f})\n", p.creatureId, p.tick, dm.who, sa->kin ? "kin" : "not kin",
					dm.kind == Demo::OUTCOME ? fmt::format("use {} on category {}", dm.decision, dm.category) : dm.kind == Demo::MEAL ? std::string("eat") : dm.kind == Demo::HURT ? fmt::format("hurt in room {}", dm.room) : std::string("fall"),
					w);
		}
		demoSeen = board.seq;
	}
	bool waterAhead(const MindPerception& p, int dir) const {
		return !p.inWater && ((dir == 10 && p.waterWest) || (dir == 11 && p.waterEast));
	}
	bool wallAhead(const MindPerception& p, int dir) const {
		return (dir == 10 && p.wallWest) || (dir == 11 && p.wallEast);
	}
	void addBoatInstead(const MindPerception& p, int dir, float want, Candidate tmpl, std::vector<Candidate>& out) {
		bool reachableBoat = false; // seen from below or above, a boat cannot be boarded
		bool reachableButton = false;
		for (auto& sa : p.visible) {
			if (sa.category == prm.boatCategory && sa.reachable)
				reachableBoat = true;
			if (sa.category == prm.buttonCategory && sa.reachable && sa.activatable)
				reachableButton = true;
		}
		if (!reachableBoat) {
			// no boat on this side: a button within reach may call one over
			// (the boat call switches; lift buttons too, harmlessly)
			const CallButton* known = nullptr;
			for (auto& cb : callButtons)
				if (std::fabs(dxTo(p, cb.x)) < 300.0f && std::fabs(cb.y - p.y) < prm.sameLevel)
					known = &cb;
			if (known && known->failed > known->worked)
				reachableButton = false; // this button does not call boats
			if (reachableButton) {
				float w = want * prm.boatShare * prm.boatCallShare * (known && known->worked > 0 ? prm.boatCallKnownBonus : 1.0f);
				Candidate c{prm.buttonCategory, 1, 0, w, 0, w};
				c.boatCall = true;
				c.chain = true;
				c.food = tmpl.food;
				c.forage = tmpl.forage;
				c.scent = tmpl.scent;
				out.push_back(c);
			}
			return;
		}
		Candidate c{prm.boatCategory, boatDecisionFor(dir, p.tick), 0, want * prm.boatShare, 0, want * prm.boatShare};
		c.chain = true;
		c.food = tmpl.food;
		c.forage = tmpl.forage;
		c.scent = tmpl.scent;
		out.push_back(c);
	}

	// A known travel link that starts near here and ends much closer to the
	// goal: go and use its object (seen and reachable). Returns whether one
	// was offered.
	bool addLinkToward(const MindPerception& p, float goalX, float goalY, float want, Candidate tmpl, std::vector<Candidate>& out) {
		const TravelLink* best = nullptr;
		float bestGain = prm.linkGain;
		float here = std::fabs(dxTo(p, goalX)) + std::fabs(goalY - p.y) * 2.0f;
		for (auto& tl : travelLinks) {
			if (std::fabs(dxTo(p, tl.fromX)) > prm.linkNear || std::fabs(tl.fromY - p.y) > prm.sameLevel)
				continue;
			bool reachable = false;
			for (auto& sa : p.visible)
				if (sa.category == tl.category && sa.reachable && (sa.activatable || tl.decision < 1 || tl.decision > 3))
					reachable = true;
			if (!reachable)
				continue;
			float gain = here - (std::fabs(goalX - tl.toX) + std::fabs(goalY - tl.toY) * 2.0f);
			if (gain > bestGain) {
				bestGain = gain;
				best = &tl;
			}
		}
		if (!best)
			return false;
		Candidate c{best->category, best->decision, 0, want * prm.linkShare, 0, want * prm.linkShare};
		c.chain = true;
		c.food = tmpl.food;
		c.forage = tmpl.forage;
		out.push_back(c);
		return true;
	}

	void addEscape(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, bool canWest, bool canEast, std::vector<Candidate>& out) {
		// (a creature that breathes water is at home in it: it leaves for
		// food and the like, not in a hurry; owner, 2026-11-11, frog norns)
		if (!p.inWater || p.breathesWater)
			return;
		Expectation clear;
		clear.delta[SUFFOCATE] = -d[SUFFOCATE];
		float worth = predictedRelief(d, D, clear) + prm.escapeUrgency + prm.escapeBreathUrgency * (1.0f - p.breath) * (1.0f - p.breath);
		auto mark = [&](size_t from) {
			for (size_t i = from; i < out.size(); i++) {
				out[i].escape = true;
				out[i].chain = true; // no drive learning from the steps themselves
			}
		};
		auto tried = [&](const AgentRef& a) {
			for (auto& t : triedDevices)
				if (t.first && t.first == a && p.tick < t.second + prm.triedFor)
					return true;
			return false;
		};
		auto shownTried = [&](int category) {
			for (auto& s : p.visible)
				if (s.category == category)
					return tried(s.agent);
			return false;
		};
		// a remembered exit in view and near
		for (auto& e : exits) {
			if (std::fabs(dxTo(p, e.x)) > prm.exitNear || std::fabs(e.y - p.y) > prm.sameLevel || !isVisible(p, e.category) || shownTried(e.category))
				continue;
			Candidate c{e.category, e.decision, 0, worth * prm.exitBonus, 0, worth * prm.exitBonus};
			c.escape = true;
			out.push_back(c);
			return;
		}
		if (p.dryWay == 10 || p.dryWay == 11) {
			if (!((p.dryWay == 10 && !canWest) || (p.dryWay == 11 && !canEast) || isBlocked(p, p.dryWay))) {
				Candidate c{-1, p.dryWay, 0, worth, 0, worth};
				out.push_back(c);
				mark(out.size() - 1);
				return;
			}
		} else if (p.dryWay == -1 || p.dryWay == 1) {
			size_t before = out.size();
			addFloorChange(p, worth, p.y + p.dryWay * prm.scentFloorStep, -1, canWest, canEast, out);
			mark(before);
			if (out.size() > before)
				return;
		}
		// no open way out: try the devices in view (passing over handles the
		// ones that do nothing)
		bool offered = false;
		float triedX = 0;
		int triedSeen = 0;
		for (auto& s : p.visible) {
			if (s.activatable && std::find(prm.escapeDevices.begin(), prm.escapeDevices.end(), s.category) != prm.escapeDevices.end()) {
				if (tried(s.agent)) {
					triedX += s.x;
					triedSeen++;
					continue;
				}
				for (int decision : {1, 2}) { // activate 1 and 2
					bool can = false;
					for (auto& a : *p.actions)
						if (a.needsObject && a.decision == decision)
							can = true;
					if (!can)
						continue;
					Candidate c{s.category, decision, s.distance, worth * prm.escapeDeviceShare, 0, worth * prm.escapeDeviceShare};
					c.escape = true;
					out.push_back(c);
					offered = true;
				}
			}
		}
		// every device in view already tried: search the room, walking away
		// from them (the other way if that side is closed)
		if (!offered && triedSeen > 0) {
			int away = dxTo(p, triedX / triedSeen) < 0 ? 11 : 10;
			for (int dir : {away, away == 10 ? 11 : 10}) {
				if ((dir == 10 && !canWest) || (dir == 11 && !canEast) || isBlocked(p, dir))
					continue;
				Candidate c{-1, dir, 0, worth * prm.searchShare, 0, worth * prm.searchShare};
				out.push_back(c);
				mark(out.size() - 1);
				break;
			}
		}
	}

	// A device that once flooded the creature, used the same way near the
	// same place
	bool isFutile(const MindPerception& p, int category, int decision) const {
		if (category < 0)
			return false;
		for (auto& f : futile)
			if (f.category == category && f.decision == decision && std::fabs(dxTo(p, f.x)) < prm.futileNear && std::fabs(p.y - f.y) < prm.sameLevel)
				return true;
		return false;
	}
	bool isFlooder(const MindPerception& p, int category, int decision) const {
		if (category < 0)
			return false;
		for (auto& f : flooders)
			if (f.category == category && f.decision == decision && std::fabs(dxTo(p, f.x)) < prm.flooderNear && std::fabs(f.y - p.y) < prm.sameLevel)
				return true;
		return false;
	}



	// ---- Thoughts (owner, 2026-11-13: thought clouds in pictures) ----
	// the symbol for a need
	static const char* needSymbol(int need) {
		switch (need) {
			case PAIN: return "pain";
			case HUNGER: return "hunger";
			case COLD: return "snow";
			case HOT: return "sun";
			case TIRED: return "tired";
			case SLEEPY: return "zzz";
			case LONELY: return "people";
			case FEAR: return "fear";
			case INJURY: return "injury";
			case SUFFOCATE: return "breath";
			default: return "question";
		}
	}
	static int needIndex(const std::string& name) {
		for (int i = 0; i < NUM_DRIVES; i++)
			if (name == DRIVE_NAMES[i])
				return i;
		return -1;
	}
	std::vector<std::string> pendingThought; // from the end of an episode (a surprise)
	std::string pendingThoughtKey;
	// What it is thinking as it starts something new: why it chose it.
	void thinkAbout(const MindPerception& p, const Candidate& c, MindOutput& out) {
		auto way = [&](int decision) -> std::string {
			return decision == 10 || decision == 26 ? "arrow_w" : decision == 11 || decision == 27 ? "arrow_e" : "";
		};
		std::vector<std::string> t;
		std::string key;
		if (c.place.rfind("mate:", 0) == 0) {
			t = {"heart", "who:" + c.place.substr(5)};
			key = c.place;
		} else if (c.place == "explore") {
			t = {"feet", "question"};
			key = "explore";
		} else if (c.place.rfind("favourite:", 0) == 0) {
			t = {needSymbol(needIndex(c.place.substr(10))), "home"};
			key = c.place;
		} else if (c.place.rfind("leave:", 0) == 0) {
			t = {needSymbol(needIndex(c.place.substr(6))), "cross"};
			key = c.place;
		} else if (c.food || c.route) {
			int cat = foodTrip >= 0 && foodTrip < (int)foodPlaces.size() ? foodPlaces[foodTrip].category : -1;
			t = {"hunger"};
			if (cat >= 0)
				t.push_back(fmt::format("cat:{}", cat));
			if (!way(c.decision).empty())
				t.push_back(way(c.decision));
			key = "food";
		} else if (c.scent && c.urge.empty()) {
			t = {"nose", "hunger"};
			key = "smell";
		} else if (c.forage) {
			t = {"hunger", "question"};
			key = "forage";
		} else if (c.escape) {
			t = {"breath"};
			if (!way(c.decision).empty())
				t.push_back(way(c.decision));
			key = "escape";
		} else if (c.seekCategory >= 0) {
			t = {fmt::format("cat:{}", c.seekCategory), "question"};
			key = fmt::format("seek:{}", c.seekCategory);
		} else if (c.who.rfind("away from ", 0) == 0) {
			t = {"fear", "who:" + c.who.substr(10)};
			key = "fear:" + c.who.substr(10);
		} else if (c.category < 0 && c.decision == prm.sleepDecision) {
			t = {"zzz"};
			key = "sleep";
		}
		if (t.empty() && !pendingThought.empty()) {
			t = pendingThought;
			key = pendingThoughtKey;
		}
		pendingThought.clear();
		if (!t.empty()) {
			out.thought = t;
			out.thoughtKey = key;
		}
	}

	// ---- Sense of place ----
	// a room's feel for a need: how much faster (positive) or slower that
	// need rose there than it does anywhere, per tick; 0 until known
	float roomFeel(const RoomMemory& r, int need) const {
		if (r.feelTicks < prm.feelMinTicks || feelBaseTicks < prm.feelMinTicks)
			return 0.0f;
		return r.feelSum[need] / r.feelTicks - feelBase[need] / feelBaseTicks;
	}
	// how much worse than usual the body fared there (pain, cold, heat,
	// fear, injury, suffocation), and which need most
	float roomBadness(const RoomMemory& r, int* worst = nullptr) const {
		float bad = 0, most = 0;
		for (int need : {PAIN, COLD, HOT, FEAR, INJURY, SUFFOCATE}) {
			float f = std::max(0.0f, roomFeel(r, need));
			bad += f;
			if (f > most) {
				most = f;
				if (worst)
					*worst = need;
			}
		}
		return bad;
	}
	bool roomAvoided(const RoomMemory& r) const {
		return roomBadness(r) > prm.avoidFeel;
	}

	// A room's meetings are kept for the creatures met there most (6): with
	// 145 creatures every creature remembered every other one in every room,
	// and the save grew to 28 MB, 287,000 lines of meetings (2026-11-13).
	static void trimMet(std::map<std::string, int>& met, const std::string& keep) {
		while (met.size() > 6) {
			auto least = met.end();
			for (auto it = met.begin(); it != met.end(); ++it)
				if (it->first != keep && (least == met.end() || it->second < least->second))
					least = it;
			if (least == met.end())
				break;
			met.erase(least);
		}
	}
	void updatePlaces(const MindPerception& p, const std::array<float, NUM_DRIVES>& d) {
		if (p.room < 0 || p.riding || p.carried) {
			placePrevValid = false;
			return;
		}
		if (!rooms.count(p.room) && rooms.size() >= prm.roomMemoryMax) {
			// forget the room least lately been in (or only heard of)
			auto oldest = rooms.begin();
			for (auto it = rooms.begin(); it != rooms.end(); ++it)
				if (it->first != roomNow && it->second.last < oldest->second.last)
					oldest = it;
			rooms.erase(oldest);
		}
		RoomMemory& r = rooms[p.room];
		r.x = p.roomX;
		r.floor = p.roomFloor;
		r.metaroom = p.metaroom;
		r.water = p.inWater;
		if (p.room != roomNow) {
			r.visits++;
			if (!r.first)
				r.first = p.tick;
			roomNow = p.room;
		}
		r.last = p.tick;
		r.ticks += 1.0f;
		// how its needs go here, and anywhere
		if (placePrevValid) {
			for (int i = 0; i < NUM_DRIVES; i++) {
				float change = d[i] - placePrevD[i];
				r.feelSum[i] += change;
				feelBase[i] += change;
			}
			r.feelTicks += 1.0f;
			feelBaseTicks += 1.0f;
			// others in sight see it hurt here (pain, injury, fear, suffocation)
			if (!watchingOff() && !p.self.empty()) {
				if (hurtRoom != p.room) {
					hurtSum = {};
					hurtTicks = 0;
					hurtRoom = p.room;
				}
				for (int i : {PAIN, INJURY, FEAR, SUFFOCATE})
					hurtSum[i] += d[i] - placePrevD[i];
				hurtTicks += 1.0f;
				if (hurtTicks >= prm.watchHurtTicks) {
					float rise = 0;
					for (int i : {PAIN, INJURY, FEAR, SUFFOCATE})
						rise += std::max(0.0f, hurtSum[i]);
					if (rise >= prm.watchHurtMin) {
						Demo dm;
						dm.kind = Demo::HURT;
						dm.who = p.self;
						dm.x = p.x;
						dm.y = p.y;
						dm.room = p.room;
						dm.metaroom = p.metaroom;
						dm.roomX = p.roomX;
						dm.roomFloor = p.roomFloor;
						dm.water = p.inWater;
						dm.ticks = hurtTicks;
						for (int i : {PAIN, INJURY, FEAR, SUFFOCATE}) {
							dm.felt[i] = true;
							dm.delta[i] = hurtSum[i];
						}
						demoBoard().post(std::move(dm));
					}
					hurtSum = {};
					hurtTicks = 0;
				}
			}
			if (r.feelTicks > prm.feelKeepTicks) {
				for (float& f : r.feelSum)
					f *= 0.5f;
				r.feelTicks *= 0.5f;
			}
			if (feelBaseTicks > prm.feelKeepTicks * 4) {
				for (float& f : feelBase)
					f *= 0.5f;
				feelBaseTicks *= 0.5f;
			}
		}
		placePrevD = d;
		placePrevValid = true;
		// what it sees from here, and whom it meets
		for (auto& s : p.visible) {
			if (s.who.empty()) {
				if (std::fabs(s.y - p.y) > prm.seekMaxHeight)
					continue; // seen on another level: not "here"
				RoomSighting& sg = r.seen[s.category];
				if (!sg.last || p.tick > sg.last + prm.sightingGap)
					sg.times++;
				sg.x = s.x;
				sg.y = s.y;
				sg.last = p.tick;
			} else {
				MetCreature& m = metAt[s.who];
				if (m.room != p.room || !m.last || p.tick > m.last + prm.sightingGap)
					r.met[s.who]++;
					trimMet(r.met, s.who);
				// found again after a while apart, while longing for it
				if (m.last && p.tick > m.last + 200 && longingNow(m, p.tick) >= prm.longingSeek)
					MIND_LOG("[homeostat] creature {} t{}: found {} again ({} ticks apart, longing {:.2f})\n", p.creatureId, p.tick, s.who,
						p.tick - m.last, longingNow(m, p.tick));
				m.room = p.room;
				m.x = s.x;
				m.y = s.y;
				m.last = p.tick;
				m.genus = s.genus;
				m.category = s.category;
				m.stage = s.stage;
				m.female = s.female;
				m.checked = 0;
				if (d[SEX] >= prm.longingSex && s.genus == p.genus && s.female != p.female && s.stage >= (int)youth && s.stage <= (int)old &&
					p.stage >= youth && p.stage <= old) {
					float now = longingNow(m, p.tick);
					m.longing = std::min(1.0f, now + prm.longingGain * d[SEX] * (1.0f + std::max(0.0f, s.affinity)));
					m.longingAt = p.tick;
				}
			}
		}
		// rooms next door: known of, if not yet been in
		for (auto& n : p.roomsNear) {
			if (rooms.count(n.id) || rooms.size() >= prm.roomMemoryMax)
				continue;
			RoomMemory& k = rooms[n.id];
			k.x = n.x;
			k.floor = n.floor;
			k.water = n.water;
			k.metaroom = p.metaroom;
		}
	}

	// a trip from the sense of place: walk toward (x, y) on this floor, or
	// take steps toward changing floors
	// A long trip is valued as food trips are, by time: worth half as much at
	// travelHalf pixels, and the stronger the need (`need`, 0-1) the more
	// patient the creature (a straight per-pixel cost made any mate across
	// the world worth less than nothing).
	void addGoTo(const MindPerception& p, float want, float x, float y, const std::string& why, bool canWest, bool canEast, std::vector<Candidate>& out, float need = 0.0f) {
		if (want < prm.seekMinWant)
			return;
		size_t before = out.size();
		float dx = dxTo(p, x), dy = y - p.y;
		if (std::fabs(dy) <= prm.seekMaxHeight) {
			if (std::fabs(dx) < prm.seekArrived)
				return;
			int decision = dx < 0 ? 10 : 11;
			if ((decision == 10 && !canWest) || (decision == 11 && !canEast) || isBlocked(p, decision) || waterAhead(p, decision) || wallAhead(p, decision) ||
				cliffAhead(p, decision, y))
				return;
			Candidate c{-1, decision, std::fabs(dx), want, 0, 0};
			c.score = want / (1.0f + std::fabs(dx) / (prm.travelHalf * (1.0f + prm.hungerPatience * need * need)));
			c.chain = true;
			out.push_back(c);
		} else
			addFloorChange(p, want * prm.floorChangeDiscount, y, -1, canWest, canEast, out);
		for (size_t i = before; i < out.size(); i++)
			out[i].place = why;
	}

	// (1) Finding things again: for each wanted kind of thing out of sight
	// with no last-seen spot left to try, the room it was seen in most often
	// and most lately (not one checked lately and found without it, nor one
	// avoided) becomes where to look.
	void recallPlaces(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D) {
		std::map<int, std::pair<float, int>> best; // category -> (weight, room)
		for (auto& [id, r] : rooms) {
			if (r.visits == 0 || roomAvoided(r))
				continue;
			for (auto& [category, sg] : r.seen) {
				if (places.count(category) || isVisible(p, category))
					continue;
				if (sg.emptyAt && p.tick < sg.emptyAt + prm.recallEmptyTicks)
					continue;
				if (id == p.room)
					continue; // here, and not in sight
				float age = (float)(p.tick > sg.last ? p.tick - sg.last : 0);
				float dist = std::fabs(dxTo(p, sg.x)) + std::fabs(sg.y - p.y);
				float w = std::sqrt((float)sg.times) * std::exp(-age / prm.recallRecency) / (1.0f + dist / 1500.0f);
				auto it = best.find(category);
				if (it == best.end() || w > it->second.first)
					best[category] = {w, id};
			}
		}
		for (auto& [category, wr] : best) {
			if (knownRelief(p, d, D, category) * prm.seekDiscount < prm.seekMinWant)
				continue;
			const RoomSighting& sg = rooms[wr.second].seen[category];
			places[category] = {sg.x, sg.y, p.tick};
			recalledFrom[category] = wr.second;
			MIND_LOG("[homeostat] creature {} t{}: remembers seeing {} in room {} (seen there {} time{}); looks there\n", p.creatureId, p.tick,
				category, wr.second, sg.times, sg.times == 1 ? "" : "s");
		}
	}
	// a last-seen or recalled spot reached and the thing not there
	void placeFoundEmpty(const MindPerception& p, int category) {
		if (auto it = recalledFrom.find(category); it != recalledFrom.end()) {
			if (auto r = rooms.find(it->second); r != rooms.end())
				if (auto sg = r->second.seen.find(category); sg != r->second.seen.end())
					sg->second.emptyAt = p.tick;
			recalledFrom.erase(it);
		}
		if (auto r = rooms.find(p.room); r != rooms.end())
			if (auto sg = r->second.seen.find(category); sg != r->second.seen.end())
				sg->second.emptyAt = p.tick;
	}

	// (2) Finding mates: wanting to mate, an adult with no adult of its kind
	// and the other sex in view goes to where it last met one (most lately
	// met first, nearer first).
	void addMateSeek(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, bool canWest, bool canEast, std::vector<Candidate>& out) {
		auto adult = [](int stage) { return stage >= (int)youth && stage <= (int)old; };
		if (!adult((int)p.stage) || p.genus < 0)
			return;
		float strongest = 0;
		for (auto& [who, m] : metAt)
			strongest = std::max(strongest, longingNow(m, p.tick));
		if (d[SEX] < prm.mateSex && strongest < prm.longingSeek)
			return;
		for (auto& s : p.visible)
			if (!s.who.empty() && s.genus == p.genus && s.female != p.female && adult(s.stage))
				return; // one in view: the usual choosing takes over
		const std::string* bestWho = nullptr;
		float bestW = 0;
		for (auto& [who, m] : metAt) {
			if (m.genus != p.genus || m.female == p.female || !adult(m.stage))
				continue;
			// gone from where they last met: only its own scent can lead on
			bool scentOnly = m.checked && p.tick < m.checked + prm.mateCheckedTicks;
			if (scentOnly && !smellsSignature(p, who))
				continue;
			if (!scentOnly)
				if (auto r = rooms.find(m.room); r != rooms.end() && roomAvoided(r->second))
					continue;
			float age = (float)(p.tick > m.last ? p.tick - m.last : 0);
			float dist = std::fabs(dxTo(p, m.x)) + std::fabs(m.y - p.y);
			if (!scentOnly && dist < prm.seekArrived && std::fabs(m.y - p.y) < prm.seekMaxHeight) {
				m.checked = p.tick; // here, and not here
				m.longing = longingNow(m, p.tick) * 0.7f; // a little less hopeful
				m.longingAt = p.tick;
				MIND_LOG("[homeostat] creature {} t{}: came to where it met {} and they are gone\n", p.creatureId, p.tick, who);
				continue;
			}
			// with the drive itself up, any mate will do; on longing alone,
			// the one longed for
			float L = longingNow(m, p.tick);
			if (d[SEX] < prm.mateSex && L < prm.longingSeek)
				continue;
			float w = std::exp(-age / prm.recallRecency) * (0.3f + L) / (1.0f + dist / 1500.0f);
			if (w > bestW) {
				bestW = w;
				bestWho = &who;
			}
		}
		if (!bestWho)
			return;
		const MetCreature& m = metAt[*bestWho];
		bool bestScentOnly = m.checked && p.tick < m.checked + prm.mateCheckedTicks;
		Expectation mating;
		mating.delta[SEX] = -0.3f * d[SEX];
		float L = longingNow(m, p.tick);
		float want = d[SEX] >= prm.mateSex ? std::max(knownRelief(p, d, D, m.category), predictedRelief(d, D, mating)) * prm.mateSeekShare : 0.0f;
		want = std::max(want, prm.longingWant * L);
		// its own scent, if smelled: follow it toward where it is stronger
		// (owner, 2026-11-14: "I want the seeking creature to be able to follow
		// a signature scent ... of that specific creature"); else to where
		// they last met
		// (first to where they last met, by the planned route; its scent
		// only once they were not found there: following the scent all the
		// way found the one sought less often, 60-64% against 66-68%)
		if (bestScentOnly) {
			followSignature(p, *bestWho, want, canWest, canEast, out);
			return;
		}
		addGoTo(p, want, m.x, m.y, "mate:" + *bestWho, canWest, canEast, out, std::max(d[SEX], L));
	}

	bool smellsSignature(const MindPerception& p, const std::string& who) const {
		static const bool off = devenv("OPENC2E_NO_SIGNATURES") != nullptr;
		if (off)
			return false;
		for (auto& ss : p.signatures)
			if (ss.who == who)
				return std::max({ss.here, ss.west, ss.east, ss.up, ss.down}) > 1e-6f;
		return false;
	}

	// A step toward the side where this creature's own scent is stronger
	// than here (Pheromones signature scents). False if it is not smelled, or
	// is strongest here (it is near: the usual looking takes over).
	bool followSignature(const MindPerception& p, const std::string& who, float want, bool canWest, bool canEast, std::vector<Candidate>& out) {
		static const bool off = devenv("OPENC2E_NO_SIGNATURES") != nullptr;
		if (off)
			return false;
		for (auto& ss : p.signatures) {
			if (ss.who != who)
				continue;
			struct Side {
				float scent;
				int kind; // 10 west, 11 east, -1 up, 1 down
			};
			Side sides[4] = {{ss.west, 10}, {ss.east, 11}, {ss.up, -1}, {ss.down, 1}};
			std::sort(std::begin(sides), std::end(sides), [](const Side& a, const Side& b) { return a.scent > b.scent; });
			for (auto& side : sides) {
				if (side.scent <= ss.here * prm.scentStronger + 1e-6f)
					break;
				std::string label = "mate:" + who + ":scent";
				if (side.kind == 10 || side.kind == 11) {
					if (waterAhead(p, side.kind) || (side.kind == 10 && !canWest) || (side.kind == 11 && !canEast) || isBlocked(p, side.kind) || cliffAhead(p, side.kind))
						continue;
					Candidate c{-1, side.kind, 0, want, 0, want};
					c.chain = true;
					c.place = label;
					out.push_back(c);
					return true;
				}
				size_t before = out.size();
				addFloorChange(p, want, p.y + side.kind * prm.scentFloorStep, -1, canWest, canEast, out);
				if (out.size() > before) {
					for (size_t i = before; i < out.size(); i++)
						out[i].place = label;
					return true;
				}
			}
			return false;
		}
		return false;
	}

	// the creatures whose own scent it wants to smell: the ones it longs for
	// most (CreatureMind::wantedSignatures)
	std::vector<std::string> wantedSignatures() const override {
		std::vector<std::pair<float, std::string>> list;
		for (auto& [who, m] : metAt) {
			float L = longingNow(m, lastTick);
			if (L >= 0.05f)
				list.push_back({L, who});
		}
		std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.first > b.first; });
		std::vector<std::string> out;
		for (size_t i = 0; i < list.size() && i < 3; i++)
			out.push_back(list[i].second);
		return out;
	}

	// (3b) The cure (owner, 2026-11-21: "care + learned medicine"): in lasting
	// pain, having learned that the healing fruit (MIND_HEALING_FRUIT) eases
	// it (by eating it or watching it eaten), go to the nearest place it
	// remembers finding some, for what easing the pain is worth. In 12-day
	// Home copies (2026-11-21) it was seldom offered: most of the poisoned
	// live far from the volcano and had neither seen the fruit nor watched it
	// eaten, and those who ate it learned little relief (the soothing comes
	// as the meal ends). Knowledge spreads as creatures meet the fruit and
	// watch each other, over many days. OPENC2E_NO_CURE_SEEK turns it off.
	void addCureTrip(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, bool canWest, bool canEast, std::vector<Candidate>& out) {
		static const bool off = devenv("OPENC2E_NO_CURE_SEEK") != nullptr;
		if (off || d[PAIN] < prm.favouritePainNeed)
			return;
		// (OPENC2E_WATCH_CURE: why a creature in pain is or is not offered the trip)
		static const bool watch = devenv("OPENC2E_WATCH_CURE") != nullptr;
		auto why = [&](const char* w) {
			if (watch && p.tick % 50 == 0)
				fmt::print("* CURE creature {} pain {:.2f}: {}\n", p.creatureId, d[PAIN], w);
		};
		auto known = expectations.find((MIND_HEALING_FRUIT + 1) * 64 + prm.eatDecision);
		// (known from its own meals or from watching others eat it: watching
		// teaches what a thing does without counting as having tried it)
		if (known == expectations.end())
			return why("never ate it");
		if (known->second.delta[PAIN] > -prm.painCureKnown)
			return why(fmt::format("not known to ease pain ({:+.3f})", known->second.delta[PAIN]).c_str());
		for (auto& s : p.visible)
			if (s.category == MIND_HEALING_FRUIT && s.reachable)
				return why("in sight"); // in sight: eating it is offered already
		const FoodPlace* best = nullptr;
		float bestDist = 1e9f;
		for (auto& fp : foodPlaces) {
			if (fp.category != MIND_HEALING_FRUIT || fp.confidence < 0.2f || p.tick < fp.cooldownUntil)
				continue;
			float dist = std::fabs(dxTo(p, fp.x)) + std::fabs(fp.y - p.y);
			if (dist < bestDist) {
				bestDist = dist;
				best = &fp;
			}
		}
		if (!best)
			return why("no place known");
		// worth what easing the pain would be (not the rest of a meal, which
		// a creature not hungry may not want)
		Expectation ease;
		ease.delta[PAIN] = known->second.delta[PAIN];
		float want = predictedRelief(d, D, ease) * prm.favouriteShare;
		why(fmt::format("trip offered, want {:.3f}", want).c_str());
		addGoTo(p, want, best->x, best->y - 40.0f, "cure", canWest, canEast, out, d[PAIN]);
	}

	// (3) Favourite places: a strong need, and a room known to ease it
	// better than anywhere else (not this one, not one avoided): go there.
	void addFavourites(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, bool canWest, bool canEast, std::vector<Candidate>& out) {
		for (int need : {PAIN, COLD, HOT, TIRED, SLEEPY, LONELY}) {
			if (d[need] < (need == PAIN ? prm.favouritePainNeed : prm.favouriteNeed))
				continue;
			if (auto here = rooms.find(p.room); here != rooms.end() && -roomFeel(here->second, need) >= prm.favouriteMinEase)
				continue; // already somewhere good for it
			int bestId = -1;
			float bestScore = -1e9f, bestWant = 0;
			bool bed = need == TIRED || need == SLEEPY;
			if (bed && d[need] > prm.bedNeedMax)
				continue;
			for (auto& [id, r] : rooms) {
				if (id == p.room || r.visits == 0 || r.water || roomAvoided(r))
					continue;
				if (bed && std::fabs(dxTo(p, r.x)) + std::fabs(r.floor - p.y) > prm.bedReach)
					continue;
				float ease = -roomFeel(r, need);
				if (ease < prm.favouriteMinEase)
					continue;
				Expectation e;
				e.delta[need] = -std::min(0.5f, ease * prm.favouriteHorizon);
				float want = predictedRelief(d, D, e) * prm.favouriteShare;
				float score = want / (1.0f + (std::fabs(dxTo(p, r.x)) + std::fabs(r.floor - p.y)) / (prm.travelHalf * (1.0f + prm.hungerPatience * d[need] * d[need])));
				if (score > bestScore) {
					bestScore = score;
					bestWant = want;
					bestId = id;
				}
			}
			if (bestId >= 0)
				addGoTo(p, bestWant, rooms[bestId].x, rooms[bestId].floor - 40.0f, std::string("favourite:") + DRIVE_NAMES[need], canWest, canEast, out, d[need]);
		}
	}

	// (4) Avoided places: in a room where the body fares worse than usual,
	// with that need up, leave it for the room next door that fared best.
	void addLeave(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, bool canWest, bool canEast, std::vector<Candidate>& out) {
		auto here = rooms.find(p.room);
		if (here == rooms.end())
			return;
		int worst = -1;
		if (roomBadness(here->second, &worst) <= prm.avoidFeel || worst < 0 || d[worst] < prm.leaveNeed)
			return;
		// a favourite place for that need already offered: go there instead
		for (auto& c : out)
			if (c.place == std::string("favourite:") + DRIVE_NAMES[worst])
				return;
		const MindPerception::RoomNear* way = nullptr;
		float bestBad = 1e9f;
		for (auto& n : p.roomsNear) {
			if (n.side != 10 && n.side != 11)
				continue;
			if (n.water && !p.inWater)
				continue;
			float bad = 0;
			if (auto r = rooms.find(n.id); r != rooms.end())
				bad = roomBadness(r->second);
			if (bad < bestBad) {
				bestBad = bad;
				way = &n;
			}
		}
		if (!way || bestBad > prm.avoidFeel)
			return;
		Expectation away;
		away.delta[worst] = -0.3f * d[worst];
		addGoTo(p, predictedRelief(d, D, away) * 0.6f, way->x, way->floor - 40.0f, std::string("leave:") + DRIVE_NAMES[worst], canWest, canEast, out);
	}

	// (5) Exploring: curiosity about rooms next door it has never been in
	// (or a known one not far along this floor), more when bored or curious
	// by temperament, quieted by pressing needs.
	void addExplore(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, bool canWest, bool canEast, std::vector<Candidate>& out) {
		float survival = std::max({d[PAIN], d[HUNGER], d[COLD], d[HOT], d[INJURY], d[SUFFOCATE], d[THIRST]});
		float want = prm.exploreWant * (0.5f + d[BORED]) * std::clamp(1.0f + 0.5f * appliedTemperament[1], 0.3f, 2.0f) *
					 (1.0f - prm.survivalCuriosityDamp * std::clamp(survival, 0.0f, 1.0f));
		if (p.stage >= 0 && p.stage < 7)
			want *= prm.stageExplore[(size_t)p.stage];
		if (want < prm.seekMinWant)
			return;
		addExploreByNovelty(p, d, want, canWest, canEast, out);
	}

	// a room's novelty: 1 / sqrt(1 + visits), the visits fading with time away
	float roomNovelty(const RoomMemory& r, unsigned int tick) const {
		float away = tick > r.last ? (float)(tick - r.last) : 0.0f;
		float v = (float)r.visits * std::exp(-away / prm.noveltyReturn);
		return 1.0f / std::sqrt(1.0f + v);
	}

	// the room to explore: of the rooms next door and the known rooms on this
	// floor within reach, the one most novel for its distance (not the room
	// it is in, nor one it failed to reach lately)
	void addExploreByNovelty(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float want, bool canWest, bool canEast, std::vector<Candidate>& out) {
		auto blockedRoom = [&](int id) {
			auto it = exploreBlockedUntil.find(id);
			return it != exploreBlockedUntil.end() && p.tick < it->second;
		};
		const RoomMemory* target = nullptr;
		int targetId = -1;
		float best = 0.0f, bestNovelty = 0.0f;
		auto consider = [&](int id, const RoomMemory& r) {
			if (id == p.room || blockedRoom(id))
				return;
			float nov = roomNovelty(r, p.tick);
			if (nov < prm.noveltyMin)
				return;
			float score = nov / (1.0f + std::fabs(dxTo(p, r.x)) / prm.travelHalf);
			if (score > best) {
				best = score;
				bestNovelty = nov;
				target = &r;
				targetId = id;
			}
		};
		for (auto& n : p.roomsNear) {
			if ((n.side != 10 && n.side != 11) || (n.water && !p.inWater))
				continue;
			auto r = rooms.find(n.id);
			if (r != rooms.end())
				consider(n.id, r->second);
		}
		for (auto& [id, r] : rooms) {
			if (r.water || std::fabs(r.floor - p.roomFloor) > prm.seekMaxHeight || std::fabs(dxTo(p, r.x)) > prm.exploreReach)
				continue;
			consider(id, r);
		}
		if (target) {
			exploreTarget = targetId;
			addGoTo(p, want * bestNovelty, target->x, target->floor - 40.0f, "explore", canWest, canEast, out, d[BORED]);
		}
	}

	// Seeking: for each remembered place whose category is out of sight, how
	// much would the best action on it help? If enough, offer walking toward
	// it (C2 decisions 10 and 11: travel west and east).
	void addSeekCandidates(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, std::vector<Candidate>& out) {
		bool canWest = false, canEast = false;
		for (auto& a : *p.actions) {
			if (!a.needsObject && a.decision == 10)
				canWest = true;
			if (!a.needsObject && a.decision == 11)
				canEast = true;
		}
		if (!canWest && !canEast)
			return;
		// a wall right ahead: no trip goes that way from here
		if (p.wallWest)
			canWest = false;
		if (p.wallEast)
			canEast = false;
		recallPlaces(p, d, D);
		std::vector<int> forget;
		std::vector<int> offLevel;
		for (auto& [category, place] : places) {
			if (isVisible(p, category))
				continue;
			float dx = dxTo(p, place.x), dy = place.y - p.y;
			if (std::fabs(dy) > prm.seekMaxHeight) {
				offLevel.push_back(category);
				continue;
			}
			if (std::fabs(dx) < prm.seekArrived) {
				forget.push_back(category); // here, and it is gone
				continue;
			}
			float want = knownRelief(p, d, D, category) * prm.seekDiscount;
			if (want < prm.seekMinWant)
				continue;
			int decision = dx < 0 ? 10 : 11;
			if ((decision == 10 && !canWest) || (decision == 11 && !canEast) || isBlocked(p, decision))
				continue;
			Candidate c{-1, decision, std::fabs(dx), want, 0, 0};
			c.score = want - prm.seekDistanceCost * std::fabs(dx);
			c.seekCategory = category;
			out.push_back(c);
		}
		for (int category : forget) {
			places.erase(category);
			placeFoundEmpty(p, category);
		}

		// The most wanted thing remembered on another floor: take steps
		// toward changing floors.
		float bestWant = 0.0f;
		int bestCategory = -1;
		for (int category : offLevel) {
			float best = knownRelief(p, d, D, category);
			if (best > bestWant) {
				bestWant = best;
				bestCategory = category;
			}
		}
		if (bestCategory >= 0 && bestWant * prm.floorChangeDiscount >= prm.seekMinWant)
			addFloorChange(p, bestWant * prm.floorChangeDiscount, places[bestCategory].y, bestCategory, canWest, canEast, out);

		addEscape(p, d, D, canWest, canEast, out);
		addFoodSeek(p, d, D, canWest, canEast, out);
		addScentFollow(p, d, D, canWest, canEast, out);
		addScentUrges(p, d, D, canWest, canEast, out);
		addShare(p, d, canWest, canEast, out);
		addDefend(p, d, out);
		addForage(p, d, D, canWest, canEast, out);
		addStarveLeave(p, d, D, canWest, canEast, out);
		addMateSeek(p, d, D, canWest, canEast, out);
		addFavourites(p, d, D, canWest, canEast, out);
		addCureTrip(p, d, D, canWest, canEast, out);
		addLeave(p, d, D, canWest, canEast, out);
		addExplore(p, d, canWest, canEast, out);
	}

	void addScentFollow(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, bool canWest, bool canEast, std::vector<Candidate>& out) {
		// food already within reach: no need to follow a smell
		for (auto& s : p.visible)
			if (s.reachable && s.edible > 0.1f)
				return;
		// what eating smelled food is expected to do: at a typical scent, or
		// what the best food it has eaten so far did, if that was more
		Expectation typical;
		typical.delta[HUNGER] = -prm.scentHungerRelief * 0.6f;
		for (auto& [key, e] : expectations) {
			auto sc = scents.find(key / 64 - 1);
			if (key % 64 == prm.eatDecision && e.count > 0 && sc != scents.end() && sc->second >= 0.2f)
				typical.delta[HUNGER] = std::min(typical.delta[HUNGER], std::max(e.delta[HUNGER], -prm.scentHungerRelief * 1.5f));
		}
		float want = predictedRelief(d, D, typical) * prm.scentFollowShare;
		if (want < prm.seekMinWant)
			return;
		float threshold = p.scentHere * prm.scentStronger + 0.01f;
		struct Side {
			float scent;
			int kind; // 10 west, 11 east, -1 up, 1 down
		};
		Side sides[4] = {{p.scentWest, 10}, {p.scentEast, 11}, {p.scentUp, -1}, {p.scentDown, 1}};
		const Side* best = nullptr;
		for (auto& side : sides)
			if (side.scent > threshold && (!best || side.scent > best->scent))
				best = &side;
		if (!best)
			return;
		if (best->kind == 10 || best->kind == 11) {
			if (waterAhead(p, best->kind)) {
				Candidate t{};
				t.scent = true;
				addBoatInstead(p, best->kind, want, t, out);
				return;
			}
			if ((best->kind == 10 && !canWest) || (best->kind == 11 && !canEast) || isBlocked(p, best->kind) || cliffAhead(p, best->kind))
				return;
			Candidate c{-1, best->kind, 0, want, 0, want};
			c.chain = true;
			c.scent = true;
			out.push_back(c);
		} else {
			size_t before = out.size();
			addFloorChange(p, want, p.y + best->kind * prm.scentFloorStep, -1, canWest, canEast, out);
			for (size_t i = before; i < out.size(); i++)
				out[i].scent = true;
		}
	}

	// Scent urges (pheromones.txt): follow a scent toward its strongest side,
	// or move away from it, as food smell is followed.
	bool bringable(int category) const {
		return std::find(prm.bringCategories.begin(), prm.bringCategories.end(), category) != prm.bringCategories.end();
	}

	// Tending (see Params, tendSee): with one to tend in sight, tickle it and
	// bring it food or a toy. False when there is none to tend here.
	// A bring is over: what was carried for someone is no longer held (and a
	// pickup chosen to bring that left the hands empty is not tried again for
	// a while). Run before tending or sharing offers anything.
	void bringBookkeeping(const MindPerception& p) {
		if (bookkeptTick == p.tick)
			return;
		bookkeptTick = p.tick;
		// a bring finished: what was carried for someone is no longer held
		if (lastCarrying >= 0 && p.carryingCategory < 0 && !bringingFor.empty()) {
			broughtUntil[bringingFor] = p.tick + prm.bringAgain;
			static const bool watch = devenv("OPENC2E_WATCH_TEND") != nullptr;
			if (watch)
				fmt::print("[homeostat] creature {} t{}: TEND brought something to {}\n", p.creatureId, p.tick, bringingFor);
			bringingFor.clear();
		}
		lastCarrying = p.carryingCategory;
		// (a new decision is being made, so a pickup chosen before is over)
		if (bringPickupCategory >= 0) {
			if (p.carryingCategory < 0)
				bringFailUntil[bringPickupCategory] = p.tick + prm.bringFailWait;
			bringPickupCategory = -1;
		}
	}
	unsigned int bookkeptTick = UINT_MAX;

	bool addTend(const MindPerception& p, float want, bool canWest, bool canEast, std::vector<Candidate>& out) {
		bringBookkeeping(p);
		const SeenAgent* tendee = nullptr;
		for (auto& s : p.visible)
			if (s.genus == p.genus && s.genus >= 0 && (s.stage == 0 || s.stage == 1 || s.stage == 6 || s.ill || s.poisoned) && s.reachable && !s.who.empty() &&
				s.distance < prm.tendSee && (!tendee || s.distance < tendee->distance))
				tendee = &s;
		if (!tendee)
			return false;
		bool added = false;
		std::string lab = "tend:" + tendee->who;
		// the tickle
		auto tu = tendedUntil.find(tendee->who);
		if (tu == tendedUntil.end() || p.tick >= tu->second) {
			Candidate c{tendee->category, 1, tendee->distance, want * prm.tendShare, 0, 0};
			c.score = c.want - prm.distanceCost * tendee->distance;
			c.target = tendee->agent;
			c.who = tendee->who;
			c.urge = lab;
			out.push_back(c);
			added = true;
		}
		// the bring
		return addBring(p, *tendee, want * prm.bringShare, "bring:", canWest, canEast, out) || added;
	}

	// Bringing food or a toy to another creature: carry what is held over
	// and drop it beside them, or pick something up nearby to bring. False
	// when nothing was offered.
	bool addBring(const MindPerception& p, const SeenAgent& to, float bw, const std::string& prefix, bool canWest, bool canEast, std::vector<Candidate>& out,
		bool foodOnly = false) {
		auto bu = broughtUntil.find(to.who);
		if (bu != broughtUntil.end() && p.tick < bu->second)
			return false;
		auto ok = [&](int category) { return bringable(category) && (!foodOnly || category != 13); };
		if (ok(p.carryingCategory)) {
			float dx = dxTo(p, to.x);
			Candidate c{-1, 7, 0, bw, 0, bw}; // drop it beside them
			if (std::fabs(dx) > prm.bringDropNear) {
				int kind = dx < 0 ? 10 : 11; // or walk it over
				if ((kind == 10 && !canWest) || (kind == 11 && !canEast) || isBlocked(p, kind) || waterAhead(p, kind) || cliffAhead(p, kind))
					return false;
				c.decision = kind;
				c.chain = true;
			}
			c.urge = prefix + to.who;
			out.push_back(c);
			bringingFor = to.who;
			return true;
		}
		if (p.carryingCategory >= 0)
			return false; // holding something else
		// for one in pain, what this creature has learned eases pain when
		// eaten comes first: the healing fruit, once its use is known (owner,
		// 2026-11-21: carers bring the cure as part of caring)
		auto easesPain = [&](int category) { return expectation(category, prm.eatDecision).delta[PAIN] <= -prm.painCureKnown; };
		bool forPain = to.pain >= prm.favouritePainNeed;
		const SeenAgent* item = nullptr;
		for (auto& s : p.visible)
			if (ok(s.category) && s.carryable && s.reachable && s.distance < prm.bringSearch && s.edible >= 0.0f &&
				p.tick >= bringFailUntil[s.category]) {
				bool better = !item || s.distance < item->distance;
				if (forPain && item && easesPain(s.category) != easesPain(item->category))
					better = easesPain(s.category);
				if (better)
					item = &s;
			}
		if (!item)
			return false;
		Candidate c{item->category, 6, item->distance, bw, 0, 0}; // pick it up to bring
		c.score = bw - prm.distanceCost * item->distance;
		if (item->category == MIND_HEALING_FRUIT)
			c.target = item->agent;
		c.chain = true;
		c.urge = prefix + to.who;
		out.push_back(c);
		return true;
	}

	// Defending (owner's idea, 2026-11-11: defend friends): a friend nearby
	// was just hurt by a creature in sight; unless this one fears that
	// creature, is a baby, or has more pressing needs, it goes for it (hit).
	void addDefend(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, std::vector<Candidate>& out) {
		if (!p.defendFrom || p.stage == 0)
			return;
		const SeenAgent* foe = nullptr;
		for (auto& s : p.visible)
			if (s.agent == p.defendFrom && s.reachable)
				foe = &s;
		if (!foe || foe->fear >= 0.5f || foe->stage == 0)
			return;
		float pressing = std::max({d[PAIN], d[HUNGER], d[TIRED], d[SLEEPY], d[FEAR], d[INJURY], d[SUFFOCATE]});
		float want = prm.defendWant * std::clamp(1.0f - pressing, 0.0f, 1.0f);
		if (want < prm.seekMinWant)
			return;
		Candidate c{foe->category, prm.hitDecision, foe->distance, want, 0, 0};
		c.score = want - prm.distanceCost * foe->distance;
		c.target = foe->agent;
		c.who = foe->who;
		c.urge = "defend:" + p.defendFor;
		out.push_back(c);
		static const bool watch = devenv("OPENC2E_WATCH_TEND") != nullptr;
		if (watch)
			fmt::print("[homeostat] creature {} t{}: DEFEND offered: {} hurt {}\n", p.creatureId, p.tick, foe->who, p.defendFor);
	}

	// Sharing (owner's idea, 2026-11-11: share with friends): not hungry
	// itself, with a friend in sight who looks hungry, a creature brings it
	// food. The friend credits the gift to it when it eats (Relationships).
	void addShare(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, bool canWest, bool canEast, std::vector<Candidate>& out) {
		static const bool noShare = devenv("OPENC2E_NO_SHARE") != nullptr; // (to compare)
		if (noShare || d[HUNGER] > prm.shareFed)
			return;
		bringBookkeeping(p);
		const SeenAgent* friendIn = nullptr;
		for (auto& s : p.visible)
			if (!s.who.empty() && s.affinity >= prm.shareFriend && s.hunger >= prm.shareHungry && s.reachable && s.distance < prm.tendSee &&
				(!friendIn || s.hunger > friendIn->hunger))
				friendIn = &s;
		if (!friendIn)
			return;
		float want = prm.shareWant * (0.5f + friendIn->affinity) * friendIn->hunger;
		if (addBring(p, *friendIn, want, "share:", canWest, canEast, out, true)) {
			static const bool watch = devenv("OPENC2E_WATCH_TEND") != nullptr;
			if (watch)
				fmt::print("[homeostat] creature {} t{}: SHARE offered for {} (affinity {:.2f}, its hunger {:.2f})\n", p.creatureId, p.tick, friendIn->who,
					friendIn->affinity, friendIn->hunger);
		}
	}

	void addScentUrges(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, bool canWest, bool canEast, std::vector<Candidate>& out) {
		// a trail ends when its urge is gone (the drive eased, or the
		// creature no longer smells it as a releaser)
		for (auto& [name, t] : trails) {
			if (t.left <= 0)
				continue;
			bool held = false;
			for (auto& u : p.scentUrges)
				held = held || (!u.avoid && u.scent == name);
			if (!held)
				t.left = 0;
		}
		for (auto& u : p.scentUrges) {
			Trail& trail = trails[u.scent];
			float want;
			if (u.drive >= 0 && u.drive < NUM_DRIVES) {
				Expectation typical;
				typical.delta[u.drive] = -prm.pheromoneRelief * u.weight;
				want = predictedRelief(d, D, typical) * prm.scentFollowShare;
			} else
				want = u.weight * prm.pheromoneWantScale;
			// a tend urge with no drive of its own is for when nothing is
			// pressing: it shrinks as the creature's most pressing need grows
			if (u.tend && u.drive < 0) {
				float pressing = std::max({d[PAIN], d[HUNGER], d[COLD], d[HOT], d[TIRED], d[SLEEPY], d[FEAR], d[INJURY], d[SUFFOCATE], d[THIRST]});
				float calm = std::clamp(1.0f - pressing, 0.0f, 1.0f);
				want *= std::pow(calm, prm.tendCalmPower);
			}
			static const bool watch = devenv("OPENC2E_WATCH_SCENTS") != nullptr;
			if (watch && p.tick % 20 == 0)
				fmt::print("[homeostat] creature {} t{}: scent {} {} want {:.3f} here {:.2f} W {:.2f} E {:.2f} U {:.2f} D {:.2f}\n", p.creatureId, p.tick,
					u.scent, u.avoid ? "avoid" : "follow", want, u.here, u.west, u.east, u.up, u.down);
			if (want < prm.seekMinWant)
				continue;
			std::string label = (u.avoid ? "avoid:" : "follow:") + u.scent;
			if (u.tend && !u.avoid && addTend(p, want, canWest, canEast, out))
				continue; // one to tend is in sight: go to it rather than up the scent
			// a step west or east, or a change of floor, toward the scent
			auto go = [&](int kind, float w, const std::string& lab) {
				if (kind == 10 || kind == 11) {
					if (waterAhead(p, kind) || (kind == 10 && !canWest) || (kind == 11 && !canEast) || isBlocked(p, kind) || cliffAhead(p, kind))
						return false;
					Candidate c{-1, kind, 0, w, 0, w};
					c.chain = true;
					c.scent = true;
					c.urge = lab;
					out.push_back(c);
					return true;
				}
				size_t before = out.size();
				addFloorChange(p, w, p.y + kind * prm.scentFloorStep, -1, canWest, canEast, out);
				for (size_t i = before; i < out.size(); i++) {
					out[i].scent = true;
					out[i].urge = lab;
				}
				return out.size() > before;
			};
			if (!u.avoid) {
				// keep on along the trail already set out on
				if (trail.left > 0 && trail.scent == u.scent) {
					float ahead = trail.kind == 10 ? u.west : trail.kind == 11 ? u.east : trail.kind == -1 ? u.up : u.down;
					trail.best = std::max(trail.best, u.here);
					if (ahead > u.here * prm.scentStronger + 0.01f)
						trail.pastPeak = 0;
					else
						trail.pastPeak++;
					if (std::max(u.here, ahead) >= trail.best * prm.followKeepShare && trail.pastPeak <= prm.followOvershoot &&
						go(trail.kind, want * prm.followBoost, label + ":on")) {
						trail.left--;
						continue;
					}
					trail.left = 0; // lost it: read the scent afresh
				}
				float threshold = u.here * prm.scentStronger + 0.01f;
				struct Side {
					float scent;
					int kind; // 10 west, 11 east, -1 up, 1 down
				};
				Side sides[4] = {{u.west, 10}, {u.east, 11}, {u.up, -1}, {u.down, 1}};
				const Side* best = nullptr;
				for (auto& side : sides)
					if (side.scent > threshold && (!best || side.scent > best->scent))
						best = &side;
				if (!best)
					continue;
				if (go(best->kind, want, label)) {
					trail.scent = u.scent;
					trail.kind = best->kind;
					trail.best = std::max(u.here, best->scent);
					trail.left = prm.followDecisions;
					trail.pastPeak = 0;
				}
			} else {
				// away: if the scent is here or stronger on some side, walk
				// toward the weaker of west and east
				float strongest = std::max({u.here, u.west, u.east, u.up, u.down});
				if (strongest < 0.02f)
					continue;
				if (u.west < u.east)
					go(10, want, label);
				else if (u.east < u.west)
					go(11, want, label);
				else
					go(std::uniform_int_distribution<int>(0, 1)(rng) ? 10 : 11, want, label); // even both ways: either
			}
		}
	}

	void addFloorChange(const MindPerception& p, float want, float targetY, int target, bool canWest, bool canEast, std::vector<Candidate>& out) {
		float direction = targetY > p.y ? 1.0f : -1.0f; // +1 down, -1 up
		auto walkTo = [&](float x) {
			int decision = dxTo(p, x) < 0 ? 10 : 11;
			if ((decision == 10 && !canWest) || (decision == 11 && !canEast) || isBlocked(p, decision))
				return;
			Candidate c{-1, decision, std::fabs(dxTo(p, x)), want, 0, 0};
			c.score = want - prm.seekDistanceCost * std::fabs(dxTo(p, x));
			c.seekCategory = target;
			c.chain = true;
			out.push_back(c);
		};
		auto actOn = [&](int category, int decision, float share) {
			Candidate c{category, decision, 0, want * share, 0, 0};
			c.score = c.want;
			c.chain = true;
			out.push_back(c);
		};
		const SeenAgent* lift = nullptr;
		const SeenAgent* button = nullptr;
		for (auto& s : p.visible) {
			if (s.category == prm.liftCategory)
				lift = &s;
			// only a call button on this floor, within reach: far-east norns
			// on a ledge pressed the call button of the deck below them over
			// a thousand times
			if (s.category == prm.buttonCategory && s.reachable && s.activatable && std::fabs(s.y - p.y) < prm.sameLevel)
				button = &s;
		}
		// a known link from this floor in the right direction, nearest first
		const Link* link = nullptr;
		for (auto& l : links) {
			if (std::fabs(l.fromY - p.y) > prm.sameLevel || (l.toY - l.fromY) * direction <= 0)
				continue;
			if (!link || std::fabs(dxTo(p, l.x)) < std::fabs(dxTo(p, link->x)))
				link = &l;
		}
		bool liftHere = lift && std::fabs(dxTo(p, lift->x)) < prm.atPlace * 1.5f && std::fabs(lift->y - p.y) < prm.sameLevel;
		if (link) {
			if (std::fabs(dxTo(p, link->x)) > prm.atPlace)
				walkTo(link->x);
			else if (liftHere)
				actOn(prm.liftCategory, link->decision, 1.0f);
			else if (button)
				actOn(prm.buttonCategory, 1, 0.8f); // call the lift
			return;
		}
		// no link yet: go to the nearest lift and try what it does
		if (lift) {
			if (std::fabs(dxTo(p, lift->x)) > prm.atPlace)
				walkTo(lift->x);
			else if (liftHere) {
				actOn(prm.liftCategory, 1, 0.6f);
				actOn(prm.liftCategory, 2, 0.6f);
			} else if (button)
				actOn(prm.buttonCategory, 1, 0.8f);
		} else if (button && !dudButtonAt(p)) {
			actOn(prm.buttonCategory, 1, 0.5f);
			out.back().blindPress = true;
		}
	}
	bool dudButtonAt(const MindPerception& p) const {
		for (auto& d : dudButtons)
			if (p.tick < d.until && std::fabs(dxTo(p, d.x)) < prm.dudButtonNear && std::fabs(d.y - p.y) < prm.sameLevel)
				return true;
		return false;
	}

	void applyScent(int category, float edible) {
		Expectation& e = expectation(category, prm.eatDecision);
		if (edible <= 0.0f) {
			if (e.count == 0 && e.delta[HUNGER] < 0.0f)
				e.delta[HUNGER] = 0.0f; // does not smell like food
			return;
		}
		float target = -prm.scentHungerRelief * edible;
		float old = e.count + e.prior;
		e.delta[HUNGER] = (e.delta[HUNGER] * old + target * prm.scentWeight) / (old + prm.scentWeight);
		e.prior += prm.scentWeight;
	}

	void learnRide(const MindPerception& p) {
		float dy = p.y - rideFromY;
		// A ride is credited to the creature's last action on the lift only if
		// that worked it (activate 1 or 2, deactivate). A creature that bit or
		// picked at the cave lift as another's ride moved it learned "eat the
		// lift to go up", and sat in the cabin eating it for good: 43% of the
		// 4,204 lift links saved in Home were by eating, picking up, hitting and
		// the like (2026-11-14). (Travel links had the same rule already.)
		if (std::fabs(dy) < prm.minRide || lastLiftDecision < 0 || ((lastLiftDecision < 1 || lastLiftDecision > 3) && !devenv("OPENC2E_KEEP_BADLINKS")))
			return;
		for (auto& l : links) {
			if (std::fabs(l.x - rideFromX) < prm.atPlace && std::fabs(l.fromY - rideFromY) < prm.sameLevel &&
				std::fabs(l.toY - p.y) < prm.sameLevel && l.decision == lastLiftDecision) {
				l.uses++;
				return;
			}
		}
		links.push_back({rideFromX, rideFromY, p.y, lastLiftDecision, 1});
		MIND_LOG("[homeostat] creature {} learned a ride: {} at x {:.0f} takes it from y {:.0f} to y {:.0f}\n", p.creatureId,
			actionName(lastLiftDecision), rideFromX, rideFromY, p.y);
	}

	// 6. Genome instincts become innate expectations (gene-translation.md).
	void applyInstinct(const MindPerception& p, const InnateInstinct& inst) {
		if (appliedInstincts.count(inst.signature))
			return;
		appliedInstincts.insert(inst.signature);
		std::vector<int> drives, categories, senses, verbs, nouns;
		bool dormant = false;
		for (auto& c : inst.conditions) {
			if (c.kind == InnateCondition::Drive && c.index >= 0 && c.index < NUM_DRIVES)
				drives.push_back(c.index);
			else if (c.kind == InnateCondition::Attention)
				categories.push_back(c.index);
			else if (c.kind == InnateCondition::Sense)
				senses.push_back(c.index);
			else if (c.kind == InnateCondition::HeardVerb)
				verbs.push_back(c.index);
			else if (c.kind == InnateCondition::HeardNoun)
				nouns.push_back(c.index);
			else
				dormant = true;
		}
		// (OPENC2E_NO_GENOME_BRIDGE: senses and no drive wait, as before)
		static const bool oldWay = devenv("OPENC2E_NO_GENOME_BRIDGE") != nullptr;
		if (oldWay && (drives.empty() || !senses.empty()))
			dormant = true;
		// (words with a drive, no such instinct in the game's genomes)
		if (!drives.empty() && (!verbs.empty() || !nouns.empty()))
			dormant = true;
		if (dormant) {
			dormantInstincts++;
			return;
		}
		// (with a drive, the situation senses only narrow when; the drive's
		// expectation below carries it, as before)
		if (drives.empty()) {
			// no drive: an inborn nudge, judged by approval (Params::innateNudge)
			Nudge n;
			n.signature = inst.signature;
			n.decision = inst.decision;
			n.categories = categories;
			n.senses = senses;
			n.verbs = verbs;
			n.nouns = nouns;
			n.value = (inst.reward ? 1.0f : -1.0f) * inst.strength;
			n.weight = 1.0f + (prm.legacyInstinctResilience - 1.0f) * inst.strength * inst.strength;
			if (auto it = savedNudges.find(inst.signature); it != savedNudges.end()) {
				n.tries = it->second.first;
				n.approvalSum = it->second.second;
				savedNudges.erase(it);
			}
			nudges.push_back(n);
			activeInstincts++;
			MIND_LOG("[homeostat] creature {} instinct: {} nudged {}{}{} (strength {:.2f}, weight {:.1f})\n", p.creatureId, actionName(inst.decision),
				inst.reward ? "toward" : "against",
				[&] { std::string w; for (int c : categories) w += fmt::format(" seeing {}", c); return w; }(),
				[&] { std::string w; for (int s : senses) w += fmt::format(" when sense {}", s); return w; }(),
				inst.strength, n.weight);
			return;
		}
		std::array<float, NUM_DRIVES> delta{};
		for (int dr : drives)
			delta[dr] = (inst.reward ? -1.0f : 1.0f) * prm.instinctDriveChange * inst.strength;
		float weight = 1.0f + (prm.legacyInstinctResilience - 1.0f) * inst.strength * inst.strength;

		bool needsObject = false;
		for (auto& a : *p.actions)
			if (a.decision == inst.decision)
				needsObject = a.needsObject;
		auto blend = [&](Expectation& e) {
			float old = e.count + e.prior;
			for (int i = 0; i < NUM_DRIVES; i++)
				e.delta[i] = (e.delta[i] * old + delta[i] * weight) / (old + weight);
			e.prior += weight;
		};
		if (!needsObject) {
			blend(expectation(-1, inst.decision));
		} else if (!categories.empty()) {
			for (int c : categories)
				blend(expectation(c, inst.decision));
		} else {
			// any object: a weak hunch, the starting point for categories not
			// yet met and added to those already known; experience with each
			// kind of object soon outweighs it
			// The hunch sets the starting expectation but adds no weight
			// against experience: one disappointing try should undo it.
			weight = std::min(weight, prm.anyObjectInstinctWeight);
			auto hunch = [&](Expectation& e) {
				float old = e.count + e.prior;
				for (int i = 0; i < NUM_DRIVES; i++)
					e.delta[i] = (e.delta[i] * old + delta[i] * weight) / (old + weight);
			};
			hunch(innateDefaults[inst.decision]);
			for (auto& [key, e] : expectations)
				if (key % 64 == inst.decision && key / 64 - 1 >= 0)
					hunch(e);
		}
		activeInstincts++;
		MIND_LOG("[homeostat] creature {} instinct: {} {} when{} (strength {:.2f}, weight {:.1f})\n", p.creatureId,
			actionName(inst.decision), inst.reward ? "rewarded" : "punished",
			[&] { std::string w; for (int dr : drives) w += std::string(" ") + DRIVE_NAMES[dr]; for (int c : categories) w += fmt::format(" seeing {}", c); return w; }(),
			inst.strength, weight);
	}

	// Softmax choice; temperature falls as arousal (D) rises.
	// Softmax over the top few candidates (an attention bottleneck, so many
	// mediocre options cannot outweigh a clearly better one together).
	Candidate pick(std::vector<Candidate> all, float D) {
		// What to do, then what to do it to: keep only each action's best
		// object, so one action on many objects cannot crowd out the rest.
		std::map<int, Candidate> bestPerAction;
		for (auto& c : all) {
			auto it = bestPerAction.find(c.decision);
			if (it == bestPerAction.end() || c.score > it->second.score)
				bestPerAction[c.decision] = c;
		}
		std::vector<Candidate> cands;
		for (auto& [decision, c] : bestPerAction)
			cands.push_back(c);
		std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
		if (cands.size() > prm.choiceSetSize)
			cands.resize(prm.choiceSetSize);
		float tau = prm.temperature / (1.0f + prm.arousalFocus * D);
		std::vector<double> w;
		for (auto& c : cands)
			w.push_back(std::exp((c.score - cands[0].score) / tau));
		std::discrete_distribution<size_t> dist(w.begin(), w.end());
		return cands[dist(rng)];
	}

	// 1 and 2: reward and learning at the end of an episode.
	void endEpisode(const MindPerception& p, const std::array<float, NUM_DRIVES>& d, float D, const char* why, bool stalled = false) {
		if (current.decision == prm.eatDecision && current.category >= 0 && felt[HUNGER] < -0.05f)
		{
			rememberFood(p.x, p.y, p.tick, 1.0f, true, current.category); // a meal here eased hunger
			hungryTicks = 0;
			if (!p.self.empty() && !watchingOff()) {
				Demo dm;
				dm.kind = Demo::MEAL;
				dm.who = p.self;
				dm.category = current.category;
				dm.x = p.x;
				dm.y = p.y;
				demoBoard().post(std::move(dm));
			}
		}
		if (current.category >= 0 && current.category < 36 && current.seekCategory < 0) {
			lastUsedCategory = current.category; // a thing it may name (not a creature)
			lastUsedTick = p.tick;
		}
		inEpisode = false;
		inEpisodeJustEnded = true;
		episodes++;
		if (!episodeNudges.empty()) {
			float approval = std::clamp((approvalNow(p) - episodeApprovalStart) / prm.approvalScale, -1.0f, 1.0f);
			static const bool watch = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
			for (size_t i : episodeNudges)
				if (i < nudges.size()) {
					if (!nudges[i].verbs.empty())
						heardDone++; // (once is doing as it was told)
					nudges[i].tries++;
					nudges[i].approvalSum += approval;
					if (watch)
						fmt::print("[homeostat] creature {} nudge {} tried ({} tries): approval {:+.2f}, now {:+.2f}\n", p.creatureId, actionName(nudges[i].decision),
							nudges[i].tries, approval, nudges[i].effect());
				}
			episodeNudges.clear();
		}
		std::array<float, NUM_DRIVES> change{};
		float total = 0.0f;
		for (int i = 0; i < NUM_DRIVES; i++) {
			change[i] = d[i] - startDrives[i];
			total += std::fabs(change[i]);
		}
		float reward = startD - D;
		const float expectedReward = current.want; // what it expected (the same scale: overall drive eased)
		// tried mostly for its novelty: how it turned out shapes a young
		// creature's curiosity (Temperament.h, EXPERIENCE_EXPLORE)
		if (current.novelty > current.want && current.novelty > 0.0f && std::fabs(reward) > 0.02f)
			pendingNovelty = reward > 0 ? 1 : -1;

		// an object action that keeps getting nowhere: stop offering it here
		if (current.category >= 0) {
			bool stayed = std::fabs(dxTo(p, startX)) < prm.futileMove && std::fabs(p.y - startY) < prm.futileMove;
			// (an action that eased its needs did something, stalled or not:
			// a norn chewing a many-bite food gets no nearer to it, and three
			// such meals in a row marked eating there futile)
			bool helped = reward > prm.surpriseReward * 0.2f || (current.decision == prm.eatDecision && felt[HUNGER] < -0.05f);
			if (devenv("OPENC2E_NO_STALLFIX"))
				helped = false;
			if (stalled && stayed && !helped) {
				bool same = current.category == futileCategory && current.decision == futileDecision;
				futileInARow = same ? futileInARow + 1 : 1;
				futileCategory = current.category;
				futileDecision = current.decision;
				// the object lies past water it can't breathe in, on the side
				// it was heading: once is enough (owner, 2026-11-12: a norn
				// stood at the pond's edge for about two minutes reaching for
				// food floating in it, three tries for each kind it saw)
				bool pastWater = false;
				if (episodeAgent && !p.inWater && !p.breathesWater)
					for (auto& v : p.visible)
						if (v.agent == episodeAgent) {
							float dx = dxTo(p, v.x);
							pastWater = (dx < 0 && p.waterWest) || (dx > 0 && p.waterEast);
							break;
						}
				// or past a wall the creature stands against on that side
				// (owner, 2026-11-12: creatures pressed against the east wall
				// of the dead-end pocket at the end of the lava corridor,
				// x ~8240, reaching for critters bouncing against it)
				bool pastWall = false;
				if (episodeAgent && !pastWater)
					for (auto& v : p.visible)
						if (v.agent == episodeAgent) {
							float dx = dxTo(p, v.x);
							pastWall = (dx < 0 && p.wallWest) || (dx > 0 && p.wallEast);
							break;
						}
				if (pastWater || pastWall)
					futileInARow = prm.futileAfter;
				if (futileInARow >= prm.futileAfter) {
					futile.push_back({current.category, current.decision, p.x, p.y, p.tick + prm.futileTicks});
					futileInARow = 0;
					MIND_LOG("[homeostat] creature {} t{}: {}@{} gets nowhere from {:.0f},{:.0f}{}; not tried here for a while\n", p.creatureId, p.tick,
						actionName(current.decision), current.category, p.x, p.y, pastWater ? " (it lies past water)" : pastWall ? " (a wall is in the way)" : "");
				}
			} else
				futileInARow = 0;
		}

		// an object that keeps doing nothing: notice another of its kind
		// (not for steps toward a destination, such as pressing a lift's call
		// button: their benefit comes later, from the ride)
		if (current.category >= 0 && episodeAgent && !current.chain && !current.food && !current.forage && !current.scent) {
			bool moved = std::fabs(p.y - startY) > prm.minRide || std::fabs(dxTo(p, startX)) > 300.0f;
			if (reward < prm.passOverReward && !moved) {
				uselessInARow = (episodeAgent == lastUselessAgent) ? uselessInARow + 1 : 1;
				lastUselessAgent = episodeAgent;
				if (uselessInARow >= prm.passOverAfter) {
					pendingPassOver = episodeAgent;
					if (current.escape) {
						triedDevices.erase(std::remove_if(triedDevices.begin(), triedDevices.end(), [&](auto& t) { return !t.first || t.first == episodeAgent || p.tick >= t.second + prm.triedFor; }), triedDevices.end());
						triedDevices.push_back({episodeAgent, p.tick});
					}
					uselessInARow = 0;
				}
			} else
				uselessInARow = 0;
		}

		// Learn what the action did beyond the passage of time: subtract the
		// baseline drift expected over this many ticks, then update the
		// baseline itself.
		int ticks = std::max(episodeTicks, 1);
		std::array<float, NUM_DRIVES> effect{};
		for (int i = 0; i < NUM_DRIVES; i++) {
			// (the passage of time cannot take a drive past its limits)
			float passiveChange = std::clamp(startDrives[i] + drift[i] * ticks, 0.0f, 1.0f) - startDrives[i];
			effect[i] = change[i] - passiveChange;
			drift[i] += prm.driftRate * (change[i] / ticks - drift[i]);
		}

		float blockedMove = prm.blockedMove * (d[TIRED] >= prm.tiredWalk ? prm.tiredBlockedShare : 1.0f);
		bool steppedNowhere = false;
		if ((current.decision == 10 || current.decision == 11) && current.category < 0 && !gaitWalk) {
			bool west = current.decision == 10;
			bool open = !(west ? p.wallWest : p.wallEast) && !(west ? p.waterWest : p.waterEast) && (west ? p.dropWest : p.dropEast) <= 0.0f;
			if (std::fabs(dxTo(p, startX)) < blockedMove && open && !p.inWater && !p.riding) {
				steppedNowhere = true;
				if (++stepFails >= 1) { // (once is enough: every creature's gait walk works)
					gaitWalk = true;
					blocked[0] = blocked[1] = Blocked{};
					MIND_LOG("[homeostat] creature {} t{}: stepping gets it nowhere with the way open; it walks by gait from now on\n", p.creatureId, p.tick);
				}
			} else if (std::fabs(dxTo(p, startX)) >= blockedMove)
				stepFails = 0;
		}
		// (any walk that got nowhere, plain wandering too; not one cut off
		// before it could: in a crowd, pokes and hits ended walks within
		// 2-6 ticks, and each marked its way blocked, so hungry creatures in
		// the crowd at room 14's east wall had both ways marked; 2026-11-16)
		static const bool noShortWalks = devenv("OPENC2E_NO_SHORT_WALK_CHECK") != nullptr; // (to compare)
		if ((current.decision == 10 || current.decision == 11) && current.category < 0 && !p.riding &&
			std::fabs(dxTo(p, startX)) < blockedMove && !(steppedNowhere && gaitWalk) &&
			(noShortWalks || episodeTicks >= prm.nowhereMinTicks)) {
			noteBlocked(p, current.decision);
			if (current.place == "explore" && exploreTarget >= 0)
				exploreBlockedUntil[exploreTarget] = p.tick + prm.blockedTicks * 8;
			if (current.route)
				for (auto& e : placeMap[cellOf(p.x, p.y)])
					if (e.dir == current.decision)
						e.fails++;
			MIND_LOG("[homeostat] creature {} t{}: the way {} is blocked at x {:.0f}\n", p.creatureId, p.tick,
				current.decision == 10 ? "west" : "east", p.x);
		}

		if (current.category < 0 && current.decision == prm.sleepDecision) {
			// the place's warmth is not something sleeping does (see badSleepNear)
			for (int i : {COLD, HOT})
				if (!feltAny[i])
					effect[i] = 0.0f;
		}
		if (current.seekCategory < 0 && !current.chain && !sleepRefused) {
			Expectation& e = expectation(current.category, current.decision);
			// an innate prior counts as remembered experience: learning is slow
			// while it outweighs real experience and speeds up as that grows
			float rate = prm.learningRate * (e.count + 1.0f) / (e.count + 1.0f + e.prior);
			// how wrong it was this time, before learning from it
			auto errorOf = [&](const Expectation& x) {
				float err = 0.0f;
				for (int i = 0; i < NUM_DRIVES; i++) {
					float target = feltAny[i] ? std::clamp(startDrives[i] + felt[i], 0.0f, 1.0f) - startDrives[i] : effect[i];
					err += (feltAny[i] ? 1.0f : prm.unfeltLearningShare) * std::fabs(target - x.delta[i]);
				}
				return err;
			};
			e.noteError(errorOf(e));
			for (int i = 0; i < NUM_DRIVES; i++) {
				if (feltAny[i]) {
					// a drive cannot go below empty or above full
					float target = std::clamp(startDrives[i] + felt[i], 0.0f, 1.0f) - startDrives[i];
					e.delta[i] += rate * prm.driveLearning[i] * (target > e.delta[i] ? prm.worseLearning : prm.betterLearning) * (target - e.delta[i]);
				} else {
					e.delta[i] += rate * prm.driveLearning[i] * (effect[i] > e.delta[i] ? prm.worseLearning : prm.betterLearning) * prm.unfeltLearningShare *
								  (effect[i] - e.delta[i]);
				}
			}
			e.count++;
			// others in sight see what it got from the thing
			if (current.category >= 0 && !p.self.empty() && !watchingOff()) {
				Demo dm;
				dm.kind = Demo::OUTCOME;
				dm.who = p.self;
				dm.category = current.category;
				dm.decision = current.decision;
				dm.x = p.x;
				dm.y = p.y;
				for (int i = 0; i < NUM_DRIVES; i++)
					if (feltAny[i]) {
						dm.felt[i] = true;
						dm.delta[i] = std::clamp(startDrives[i] + felt[i], 0.0f, 1.0f) - startDrives[i];
					}
				demoBoard().post(std::move(dm));
			}
			// and from the particular creature acted on
			if (!current.who.empty() && current.target) {
				Expectation& own = individuals[current.who][current.decision];
				own.noteError(errorOf(own));
				float r = prm.learningRate * 1.5f; // a single individual: learned a little faster than a kind
				for (int i = 0; i < NUM_DRIVES; i++) {
					float target = feltAny[i] ? std::clamp(startDrives[i] + felt[i], 0.0f, 1.0f) - startDrives[i] : effect[i];
					float share = feltAny[i] ? 1.0f : prm.unfeltLearningShare;
					own.delta[i] += r * share * prm.driveLearning[i] * (target > own.delta[i] ? prm.worseLearning : prm.betterLearning) * (target - own.delta[i]);
				}
				own.count++;
			}
		}

		// Mood follows how much better or worse this went than just letting
		// time pass (reward relative to the drift baseline).
		std::array<float, NUM_DRIVES> passive{};
		for (int i = 0; i < NUM_DRIVES; i++)
			passive[i] = std::clamp(startDrives[i] + (change[i] - effect[i]), 0.0f, 1.0f);
		float advantage = overallDrive(passive) - D;
		mood += prm.moodRate * (advantage - mood);

		valence += prm.valenceRate * (reward - valence);
		// a far better (or worse) result than expected: a moment of surprise
		// (a pleasant one only: a disappointment drawn as pain read as hurt)
		if (current.category >= 0 && current.seekCategory < 0 && !current.chain && reward - expectedReward > prm.surpriseReward) {
			pendingThought = {"bang", fmt::format("cat:{}", current.category)};
			pendingThoughtKey = fmt::format("surprise:{}:{}", current.category, p.tick);
		}

		if (current.category >= 0 && current.category < (int)habituation.size()) {
			if (total < prm.eventfulThreshold)
				habituation[current.category] *= prm.habituationRate;
			else
				habituation[current.category] = 1.0f; // dishabituation
		}

		{
			MindView::Outcome oc;
			oc.tick = p.tick;
			oc.category = current.seekCategory >= 0 ? current.seekCategory : current.category;
			oc.decision = current.decision;
			oc.who = current.who;
			oc.travel = current.seekCategory >= 0;
			oc.ticks = (unsigned int)std::max(episodeTicks, 0);
			oc.why = why;
			oc.reward = reward;
			for (int i = 0; i < NUM_DRIVES && i < 17; i++)
				oc.change[i] = change[i];
			outcomeLog.push_back(std::move(oc));
			while (outcomeLog.size() > 8)
				outcomeLog.pop_front();
		}
		if (!mindLogOn())
			return;
		// the three largest drive changes, for the log
		std::array<int, NUM_DRIVES> idx;
		for (int i = 0; i < NUM_DRIVES; i++)
			idx[i] = i;
		std::sort(idx.begin(), idx.end(), [&](int a, int b) { return std::fabs(change[a]) > std::fabs(change[b]); });
		std::string changes;
		for (int k = 0; k < 3; k++) {
			if (std::fabs(change[idx[k]]) < 0.005f)
				break;
			changes += fmt::format(" {} {:+.2f}", DRIVE_NAMES[idx[k]], change[idx[k]]);
		}
		std::sort(idx.begin(), idx.end(), [&](int a, int b) { return std::fabs(effect[a]) > std::fabs(effect[b]); });
		std::string effects;
		for (int k = 0; k < 3; k++) {
			if (std::fabs(effect[idx[k]]) < 0.005f)
				break;
			effects += fmt::format(" {} {:+.2f}", DRIVE_NAMES[idx[k]], effect[idx[k]]);
		}
		std::string feltText;
		for (int i = 0; i < NUM_DRIVES; i++)
			if (feltAny[i])
				feltText += fmt::format(" {} {:+.2f}", DRIVE_NAMES[i], felt[i]);
		MIND_LOG("[homeostat] creature {} t{}: {}@{} ended ({}, {} ticks): reward {:+.3f}{} | beyond drift:{} | felt:{} | D {:.2f} valence {:+.3f}\n",
			p.creatureId, p.tick, current.seekCategory >= 0 ? fmt::format("seek{}:{}", current.seekCategory, actionName(current.decision)) : actionName(current.decision),
			current.category, why, episodeTicks, reward,
			changes.empty() ? " (no drive change)" : changes, effects.empty() ? " nothing" : effects, feltText.empty() ? " nothing" : feltText, D, valence);
	}

	void logChoice(const MindPerception& p, const Candidate& choice, const std::vector<Candidate>& candidates, float D) {
		// developer aid: OPENC2E_WATCH_CANDS (1, or a creature id) prints every
		// candidate of each choice, with the walls and blocked ways sensed
		static const char* watchCands = devenv("OPENC2E_WATCH_CANDS");
		const bool watch = watchCands && (std::string(watchCands) == "1" || std::to_string(p.creatureId) == watchCands);
		if (!mindLogOn() && !watch)
			return;
		std::vector<Candidate> cands = candidates;
		std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
		std::string top;
		for (size_t i = 0; i < cands.size() && i < 3; i++)
			top += fmt::format(" {}@{} {:+.3f}", actionName(cands[i].decision), cands[i].category, cands[i].score);
		std::string needs;
		for (int i = 0; i < NUM_DRIVES; i++)
			if (p.drives[i] >= 64)
				needs += fmt::format(" {} {}", DRIVE_NAMES[i], (int)p.drives[i]);
		MIND_LOG("[homeostat] creature {} t{}: chose {}@{} (want {:+.3f} novelty {:+.3f}) | top:{} | needs:{} | D {:.2f} | smell {:.2f} W {:.2f} E {:.2f} U {:.2f} D {:.2f} | at {:.0f},{:.0f} | restless {:.2f} | food map {}\n",
			p.creatureId, p.tick, !choice.place.empty() ? fmt::format("place:{}:{}", choice.place, actionName(choice.decision)) : !choice.urge.empty() ? fmt::format("{}:{}", choice.urge, actionName(choice.decision)) : choice.route ? fmt::format("route:{}", actionName(choice.decision)) : choice.boatCall ? fmt::format("callboat:{}", actionName(choice.decision)) : choice.escape ? fmt::format("escape:{}", actionName(choice.decision)) : choice.food ? fmt::format("food:{}", actionName(choice.decision)) : choice.forage ? fmt::format("forage:{}", actionName(choice.decision)) : choice.scent ? fmt::format("scent:{}", actionName(choice.decision)) : choice.seekCategory >= 0 ? fmt::format("seek{}:{}", choice.seekCategory, actionName(choice.decision)) : actionName(choice.decision),
			choice.who.empty() ? fmt::format("{}", choice.category) : fmt::format("{}:{}", choice.category, choice.who), choice.want, choice.novelty,
			top, needs.empty() ? " none high" : needs, D, p.scentHere, p.scentWest, p.scentEast, p.scentUp, p.scentDown, p.x, p.y, std::clamp(hungryTicks / prm.restlessRamp, 0.0f, 1.0f),
			[&] { std::string m; for (auto& fp : foodPlaces) m += fmt::format(" {:.0f},{:.0f}x{:.2f}", fp.x, fp.y, fp.confidence); return m.empty() ? std::string(" none") : m; }());
		if (watch) {
			std::string all;
			for (auto& c : cands)
				all += fmt::format(" {}{}{}{}{}@{}={:+.3f}", c.scent ? "scent:" : "", c.forage ? "forage:" : "", c.route ? "route:" : "", c.escape ? "escape:" : "",
					actionName(c.decision), c.category, c.score);
			fmt::print("[homeostat] creature {} t{}: candidates{} | wall{}{} blocked{}{} | drop W {:.0f} E {:.0f}\n", p.creatureId, p.tick, all,
				p.wallWest ? " W" : "", p.wallEast ? " E" : "", isBlocked(p, 10) ? " W" : "", isBlocked(p, 11) ? " E" : "", p.dropWest, p.dropEast);
		}
	}
};

} // namespace

std::unique_ptr<CreatureMind> createHomeostatMind() {
	return std::make_unique<HomeostatMind>();
}
