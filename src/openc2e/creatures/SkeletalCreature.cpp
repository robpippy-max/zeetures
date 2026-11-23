/*
 *  SkeletalCreature.cpp
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
/* 20th feb 2005, by fuzzie
   with thanks to vadim for helping out

   TODO:
	* hair tidiness/untidiness (offset by 0 to 2 multiplied by 16)
	* render hairs/ears .. at the moment we avoid them, we lack zorder sanity too
	* support clothes parts
	* interpolate between differing poses, since c2e seems to
	* sanity checks: eg check that sprites have enough frames at load time
*/

#include "Profile.h"
#include "common/devenv.h"
#include "SkeletalCreature.h"

bool walkOffEndsInWater(const Room& r, bool east, int fit); // CreatureAI.cpp

#include "BreedSlots.h"
#include "common/Random.h"
#include "creatures/oldCreature.h"
#include "historyManager.h"

#include "Creature.h"
#include "Engine.h"
#include "Map.h"
#include "MetaRoom.h"
#include "PathResolver.h"
#include "Room.h"
#include "caosScript.h"
#include "caosVM.h"
#include "World.h"
#include "caosValue.h"
#include "common/backend/Backend.h"
#include "common/creaturesImage.h"
#include "common/io/FileReader.h"
#include "common/io/IOException.h"
#include "common/throw_ifnot.h"
#include "imageManager.h"

#include <tuple>
#include <array>
#include <cmath>
#include <cassert>
#include <stdint.h>
#include <fmt/core.h>
#include <map>
#include <memory>
#include <typeinfo> // TODO: remove when genome system is fixed

struct bodypartinfo {
	char letter;
	int parent;
	int attorder;
};

bodypartinfo cee_bodyparts[17] = {
	{'b', -1, -1}, // body
	{'a', 0, 0}, // head - attached to body
	{'c', 0, 1}, // left thigh - attached to body
	{'f', 0, 2}, // right thigh - attached to body
	{'i', 0, 3}, // left humerus - attached to body
	{'k', 0, 4}, // right humerus - attached to body
	{'m', 0, 5}, // tail root - attached to body
	{'d', 2, 1}, // left shin - attached to left thigh
	{'g', 3, 1}, // right shin - attached to right thigh
	{'j', 4, 1}, // left radius - attached to left humerus
	{'l', 5, 1}, // right radius - attached to right humerus
	{'e', 7, 1}, // left foot - attached to left shin
	{'h', 8, 1}, // right foot - attached to right shin
	{'n', 6, 1}, // tail tip - attached to tail root
	// Creatures Village only:
	{'o', 1, 2}, // left ear - attached to head
	{'p', 1, 3}, // right ear - attached to head
	{'q', 1, 4} // hair - attached to head
};

unsigned int cee_zorder[4][17] = {
	{6, 13, 2, 7, 11, 4, 9, 0, 14, 1, 16, 15, 5, 3, 8, 10, 12},
	{6, 13, 3, 8, 12, 5, 10, 0, 15, 1, 16, 14, 2, 7, 11, 4, 9},
	{6, 13, 2, 4, 9, 5, 3, 7, 8, 10, 0, 11, 12, 16, 14, 15, 1},
	{2, 4, 9, 1, 14, 15, 16, 5, 3, 7, 8, 10, 11, 12, 0, 6, 13}};

// needed for setPose(string) at least .. maybe cee_bodyparts should be indexed by letter
unsigned int cee_lookup[17] = {1, 0, 2, 7, 11, 3, 8, 12, 4, 9, 5, 10, 6, 13, 14, 15, 16};

int SkeletalPartCount() {
	if (engine.gametype == "cv")
		return 17;
	else
		return 14;
}

SkeletalCreature::SkeletalCreature(unsigned char _family)
	: Agent(_family, 0, 0, 0) {
	facialexpression = 0;
	pregnancy = 0;
	eyesclosed = false;

	// TODO: set zorder randomly :) should be somewhere between 1000-2700, at a /guess/
	zorder = 1500;

	ticks = 0;
	gaitgene = 0;

	calculated = false;

	if (engine.version == 1) {
		setAttributes(64 + 4 + 2); // mouseable, activateable, groundbound(?!)
	} else if (engine.version == 2) {
		setAttributes(128 + 64 + 4 + 2); // mouseable, activateable, suffersphysics, sufferscollisions
		// default values from original engine
		size = 224;
		rest = 70;
		accg = 15;
		aero = 10;
	}

	for (auto& appearancegene : appearancegenes)
		appearancegene = 0;

	skeleton = new SkeletonPart(this);
}

SkeletalCreature::~SkeletalCreature() {
	delete skeleton;
}

std::string SkeletalCreature::dataString(unsigned int _stage, bool tryfemale, unsigned int dataspecies, unsigned int databreed) {
	std::string s;
	s += (char)('0' + dataspecies + (tryfemale ? 4 : 0));
	s += (char)('0' + _stage);
	if (engine.version == 1)
		s += (char)('0' + databreed);
	else
		s += breedSlotName((int)databreed); // a-z, then aa, ab ... (BreedSlots.h)
	return s;
}

void SkeletalCreature::processGenes() {
	std::shared_ptr<genomeFile> genome = creature->getGenome();

	for (auto& gene : genome->genes) {
		if (!creature->shouldProcessGene(gene.get()))
			continue;

		if (creatureAppearanceGene* x = dynamic_cast<creatureAppearanceGene*>(gene.get())) {
			if (x->part > 5)
				continue;
			appearancegenes[x->part] = x;
		} else if (creaturePoseGene* x = dynamic_cast<creaturePoseGene*>(gene.get())) {
			posegenes[x->poseno] = x;
		} else if (creatureGaitGene* x = dynamic_cast<creatureGaitGene*>(gene.get())) {
			gaitgenes[x->drive] = x;
		}
	}
}

creatureAppearanceGene* SkeletalCreature::appearanceGeneForPart(char x) {
	// TODO: tail madness?

	if (x == 'a' || x >= 'o') {
		// head
		return appearancegenes[0];
	} else if (x == 'b') {
		// body
		return appearancegenes[1];
	} else if (x >= 'c' && x <= 'h') {
		// legs
		return appearancegenes[2];
	} else if (x >= 'i' && x <= 'l') {
		// arms
		return appearancegenes[3];
	} else if (x == 'm' || x == 'n') {
		// tail
		return appearancegenes[4];
	}

	return 0;
}


// Life-stage scaling for breeds drawn at one age only (owner, 2026-11-10).
// The engine picks a creature's sprites by life stage, falling back to the
// nearest younger age it has, so a breed with only one set (Bony grendels,
// C2's own ettins) looks the same from hatching to old age. With
// OPENC2E_STAGE_SCALE set, such a breed's pictures (and with them every
// joint point in its body data, so the parts still meet) are scaled to the
// growth of a reference breed: the same species' breed with the most ages,
// or else the standard grendels (the owner's guideline). The age a one-set
// breed was drawn at is the reference age nearest its size; with no
// reference of its own species, it is taken as drawn adult.
// (Zeetures: on unless OPENC2E_STAGE_SCALE=0; owner, 2026-11-15: Home's
// settings are the defaults)
static bool stageScaleOn() {
	static const bool on = !getenv("OPENC2E_STAGE_SCALE") || std::string(getenv("OPENC2E_STAGE_SCALE")) != "0";
	return on;
}

// the average frame size (mean of width and height) of a breed's body
// sprite at an age, or 0 if there is none
static float partSetSize(char part, int species, bool female, int age, int breed) {
	static std::map<std::tuple<char, int, bool, int, int>, float> cache;
	auto key = std::make_tuple(part, species, female, age, breed);
	if (auto it = cache.find(key); it != cache.end())
		return it->second;
	float size = 0;
	for (bool f : {female, false}) {
		std::string name(1, part);
		name += (char)('0' + species + (f ? 4 : 0));
		name += (char)('0' + age);
		name += breedSlotName(breed);
		if (auto img = world.gallery->getImage(name, true)) {
			float sum = 0;
			unsigned int n = img->numframes();
			for (unsigned int i = 0; i < n; i++)
				sum += (img->width(i) + img->height(i)) / 2.0f;
			size = n ? sum / n : 0;
			break;
		}
		if (!f)
			break;
	}
	cache[key] = size;
	return size;
}

static float bodySetSize(int species, bool female, int age, int breed) {
	return partSetSize('b', species, female, age, breed);
}

// The reference growth for a species: its breed with the most ages drawn
// (at least two), else the grendels'. ages[a] is that breed's body size at
// age a, 0 where it has none.
struct StageGrowth {
	int species = -1, breed = -1;
	std::array<float, 7> ages{};
	float at(int stage) const { // the size the engine would show at a stage
		for (int a = std::min(stage, 6); a >= 0; a--)
			if (ages[a] > 0)
				return ages[a];
		return 0;
	}
};
static const StageGrowth& stageGrowth(int species) {
	static std::map<int, StageGrowth> cache;
	if (auto it = cache.find(species); it != cache.end())
		return it->second;
	StageGrowth best;
	int bestcount = 1;
	// the species' standard breed, when it has several ages: C2's own norn
	// is breed h (norn.gen), its grendel breed a
	const int standard = species == 0 ? 7 : 0;
	{
		StageGrowth g;
		int count = 0;
		for (int a = 0; a <= 6; a++)
			if ((g.ages[a] = bodySetSize(species, false, a, standard)) > 0)
				count++;
		if (count >= 2) {
			g.species = species;
			g.breed = standard;
			return cache[species] = g;
		}
	}
	for (int breed = 0; breed < 26; breed++) {
		StageGrowth g;
		int count = 0;
		for (int a = 0; a <= 6; a++)
			if ((g.ages[a] = bodySetSize(species, false, a, breed)) > 0)
				count++;
		if (count > bestcount || (breed == 0 && count >= 2)) {
			bestcount = count;
			best = g;
			best.species = species;
			best.breed = breed;
			if (breed == 0)
				break; // the standard breed (a) is the guideline when it has ages
		}
	}
	if (best.breed < 0 && species != 1)
		best = stageGrowth(1); // the standard grendels as the guideline
	return cache[species] = best;
}

static float stageScale(int species, bool female, int breed, int stage);

bool BreedGrownForEveryStage(int species, int breed) {
	for (bool female : {false, true}) {
		float f = stageScale(species, female, breed, 0);
		for (int st = 1; st <= 6; st++)
			if (stageScale(species, female, breed, st) != f)
				return true;
	}
	return false;
}

// the factor for a part from a breed with only one set of pictures, at a
// life stage (1 for breeds drawn at more than one age)
static float stageScale(int species, bool female, int breed, int stage) {
	if (!stageScaleOn() || engine.version != 2)
		return 1.0f;
	// grendels and ettins (owner); norns (C2's own frog norn, breed l) only
	// with OPENC2E_STAGE_SCALE=norns, until the owner has seen previews
	static const bool norns = getenv("OPENC2E_STAGE_SCALE") && std::string(getenv("OPENC2E_STAGE_SCALE")) == "norns";
	if (species != 1 && species != 2 && !(species == 0 && norns))
		return 1.0f;
	float own = bodySetSize(species, female, 0, breed);
	if (own <= 0)
		return 1.0f;
	for (int a = 1; a <= 6; a++)
		if (bodySetSize(species, female, a, breed) > 0)
			return 1.0f; // drawn at more than one age: its own pictures
	const StageGrowth& g = stageGrowth(species);
	if (g.breed < 0)
		return 1.0f;
	// the age it was drawn at: the reference age nearest its size, or adult
	int drawn = -1;
	if (g.species == species) {
		float bestd = 1e9f;
		for (int a = 0; a <= 6; a++)
			if (g.ages[a] > 0 && std::fabs(std::log(own / g.ages[a])) < bestd) {
				bestd = std::fabs(std::log(own / g.ages[a]));
				drawn = a;
			}
	} else {
		for (int a = 6; a >= 0 && drawn < 0; a--)
			if (g.ages[a] > 0)
				drawn = a;
	}
	if (drawn < 0 || g.at(drawn) <= 0)
		return 1.0f;
	static std::map<std::tuple<int, bool, int>, bool> logged;
	if (!logged[std::make_tuple(species, female, breed)]) {
		logged[std::make_tuple(species, female, breed)] = true;
		std::string f;
		for (int st = 0; st <= 6; st++)
			f += fmt::format(" {:.2f}", g.at(st) / g.at(drawn));
		fmt::print("* STAGE SCALE species {} {} breed {}: one set (body size {:.0f}), drawn as age {} of reference species {} breed {}; by stage 0-6:{}\n",
			species, female ? "female" : "male", breedSlotName(breed), own, drawn, g.species, breedSlotName(g.breed), f);
	}
	return g.at(stage) / g.at(drawn);
}

// ---- Per-part growth (prototype, owner 2026-11-13: option 3) ----------------
// With OPENC2E_STAGE_GROWTH=parts, every breed (norns too) gets a look for
// each of the seven life stages: a stage it has no pictures for is drawn
// from the set the engine falls back to, each body part scaled by how much
// that part grows between the two ages in its species' reference breed
// (heads grow less than legs). Reference ages the reference breed lacks
// (C2 draws no adolescent or adult) are interpolated between its drawn ages.
// (Zeetures: on unless OPENC2E_STAGE_GROWTH=off)
static bool partGrowthOn() {
	static const bool on = !getenv("OPENC2E_STAGE_GROWTH") || std::string(getenv("OPENC2E_STAGE_GROWTH")) == "parts";
	return on;
}

// a part's size in the reference breed at an age, interpolated (on a log
// scale) between the ages it was drawn at, held level beyond them
static float referencePartSize(char part, const StageGrowth& g, int age) {
	std::array<float, 7> s{};
	for (int a = 0; a <= 6; a++)
		s[a] = partSetSize(part, g.species, false, a, g.breed);
	if (s[age] > 0)
		return s[age];
	int lo = -1, hi = -1;
	for (int a = age - 1; a >= 0 && lo < 0; a--)
		if (s[a] > 0)
			lo = a;
	for (int a = age + 1; a <= 6 && hi < 0; a++)
		if (s[a] > 0)
			hi = a;
	if (lo >= 0 && hi >= 0)
		return s[lo] * std::pow(s[hi] / s[lo], (age - lo) / (float)(hi - lo));
	return lo >= 0 ? s[lo] : hi >= 0 ? s[hi] : 0;
}

// the age a breed's only set of pictures shows (by its body's size against
// the reference), or -1 if it has pictures at more than one age
static int oneSetDrawnAge(int species, bool female, int breed) {
	float own = bodySetSize(species, female, 0, breed);
	if (own <= 0)
		return -1;
	for (int a = 1; a <= 6; a++)
		if (bodySetSize(species, female, a, breed) > 0)
			return -1;
	const StageGrowth& g = stageGrowth(species);
	if (g.breed < 0)
		return -1;
	int drawn = -1;
	if (g.species == species) {
		float bestd = 1e9f;
		for (int a = 0; a <= 6; a++)
			if (g.ages[a] > 0 && std::fabs(std::log(own / g.ages[a])) < bestd) {
				bestd = std::fabs(std::log(own / g.ages[a]));
				drawn = a;
			}
	} else {
		for (int a = 6; a >= 0 && drawn < 0; a--)
			if (g.ages[a] > 0)
				drawn = a;
	}
	return drawn;
}

// For the Hatchery card: whether the engine fits every breed's pictures to
// the life stages it has none for (per-part growth), and the age a breed
// drawn at one age only looks (its size against its species' reference;
// a species with no reference of its own, the ettins, is taken as adult),
// or -1 for a breed with pictures at more than one age.
bool StageGrowthForEveryBreed() {
	return partGrowthOn();
}

int BreedOneSetLooksAge(int species, int breed) {
	for (bool female : {false, true}) {
		int a = oneSetDrawnAge(species, female, breed);
		if (a < 0)
			continue;
		return stageGrowth(species).species == species ? a : 4;
	}
	return -1;
}

// the factor for part `part` shown at `stage` from pictures found at
// `foundstage`
static float partGrowth(char part, int species, bool female, int breed, int foundstage, int stage) {
	int drawn = oneSetDrawnAge(species, female, breed);
	if (drawn < 0)
		drawn = foundstage;
	if (drawn == stage)
		return 1.0f;
	const StageGrowth& g = stageGrowth(species);
	if (g.breed < 0)
		return 1.0f;
	float now = referencePartSize(part, g, stage), then = referencePartSize(part, g, drawn);
	if (now <= 0 || then <= 0)
		return 1.0f;
	return now / then;
}

// OPENC2E_SENILE_TINT=1 (prototype): a senile creature's pictures muted toward
// grey and lightened a little (owner, 2026-11-13); OPENC2E_SENILE_TINT=g,l
// sets the grey share and the lift
// (Zeetures: on unless OPENC2E_SENILE_TINT=0)
static bool senileTint(float& grey, float& lift) {
	static const char* v = getenv("OPENC2E_SENILE_TINT") ? getenv("OPENC2E_SENILE_TINT") : "1";
	if (std::string(v) == "0")
		return false;
	grey = 0.55f, lift = 0.08f;
	sscanf(v, "%f,%f", &grey, &lift);
	if (std::string(v) == "1")
		grey = 0.55f, lift = 0.08f;
	return true;
}

void SkeletalCreature::skeletonInit() {
	glide = Glide(); // (its pictures and joints may change)
	walkCycle = WalkCycle();
	// TODO: the exception throwing in here needs some more thought
	// TODO: if we throw an exception when we need to kill the creature off, else segfault :/

	for (int i = 0; i < SkeletalPartCount(); i++) { // CV hackery
		if (engine.version == 1) // TODO: this is hackery to skip tails for C1
			if (i == 6 || i == 13)
				continue;

		// reset the existing image reference
		images[i].reset();

		// find the relevant gene
		char x = cee_bodyparts[i].letter;
		creatureAppearanceGene* partapp = appearanceGeneForPart(x);

		int partspecies, partvariant;
		partspecies = creature->getGenus();
		if (partapp) {
			if (engine.version > 1) {
				partspecies = geneBreedSpecies(partapp->species);
				partvariant = geneBreedSlot(partapp->species, partapp->variant);
			} else
				partvariant = partapp->variant;
		} else {
			partvariant = creature->getVariant();
		}

		int spe = partspecies;

		/*
		 * In order to find a sprite, we try our current stage first, then the stages below us,
		 * and then we try again with all lower variants, and if that fails for female sprites
		 * then we try the whole thing again but trying to find a male sprite.
		 */

		// find relevant sprite
		bool tryfemale = creature->isFemale();
		// (the file found, so its body data can be the matching one)
		int foundstage = -1, foundvar = -1;
		bool foundfemale = false;
		int fallback = 0;
		while (!images[i]) {
			int var = partvariant;
			while (var > -1 && !images[i]) {
				int stage_to_try = creature->getStage();
				while (stage_to_try > -1 && !images[i]) {
					images[i] = world.gallery->getImage(x + dataString(stage_to_try, tryfemale, spe, var), true); // (a search: misses expected)
					if (images[i]) {
						foundstage = stage_to_try;
						foundvar = var;
						foundfemale = tryfemale;
					}
					stage_to_try--;
				}
				// past the game's own 26 slots, a missing picture falls back
				// to them, not slot by slot through thousands
				var = var > 26 ? 25 : var - 1;
			}
			if (!tryfemale) {
				// no pictures of that species at all (a mutation in the gene
				// for this part's breed can name a species only an added pack
				// has): the creature's own species' then. Without this the
				// birth failed half done, and the game crashed (2026-11-22).
				if (fallback >= 2)
					break;
				spe = fallback++ == 0 ? (int)creature->getGenus() : 0; // (then a norn's)
				tryfemale = creature->isFemale();
				continue;
			}
			tryfemale = false;
		}
		if (!images[i])
			throw Exception(fmt::format("SkeletalCreature couldn't find an image for part {:c} of species {}, variant {}, stage {}", x, (int)partspecies, (int)partvariant, (int)creature->getStage()));

		// find relevant ATT data: the body data that goes with the sprite
		// found (same sex, age and breed). Looking it up as male only gave
		// female golden norns (breed k, whose male and female heads differ
		// in size and neck point) the male neck points: heads rode high.
		decltype(findBodyDataFile("")) attpath;
		if (foundstage >= 0)
			attpath = findBodyDataFile(x + dataString(foundstage, foundfemale, spe, foundvar) + ".att");
		int var = partvariant;
		while (var > -1 && attpath.empty()) {
			int stage_to_try = creature->getStage();
			while (stage_to_try > -1 && attpath.empty()) {
				attpath = findBodyDataFile(x + dataString(stage_to_try, false, spe, var) + ".att");
				stage_to_try--;
			}
			var = var > 26 ? 25 : var - 1;
		}
		if (attpath.empty())
			throw Exception(fmt::format("SkeletalCreature couldn't find body data for part {:c} of species {}, variant {}, stage {}", x, (int)partspecies, (int)partvariant, creature->getStage()));

		// load ATT file
		try {
			FileReader attfile(attpath);
			att[i] = ReadAttFile(attfile);
		} catch (const IOException&) {
			throw Exception(fmt::format("SkeletalCreature couldn't load body data for part {:c} of species {}, variant {}, stage {} (tried file {})", x, (int)partspecies, (int)partvariant, creature->getStage(), attpath.string()));
		}

		images[i] = tintBodySprite(images[i]);

		// a breed drawn at one age only, scaled to the life stage: the
		// pictures and every joint point together, so the parts still meet
		float k = partGrowthOn() ? partGrowth(x, spe, foundfemale, foundvar, foundstage, creature->getStage())
								 : stageScale(spe, foundfemale, foundvar, creature->getStage());
		partk[i] = k;
		if (std::fabs(k - 1.0f) > 0.01f) {
			images[i] = world.gallery->scaled(images[i], k);
			for (auto& pose : att[i].attachments)
				for (auto& v : pose)
					v = (unsigned int)std::lround(v * k);
		}
		float grey, lift;
		if (creature->getStage() == 6 && senileTint(grey, lift))
			images[i] = world.gallery->muted(images[i], grey, lift);
		// An old creature of a breed with no old-age pictures (the C1 norns
		// Brown Mouse, White Haired Pixie, Horse; Kai) shows its younger
		// pictures, scaled: they get a lighter version of the senile grey,
		// so age still shows (owner, 2026-11-15: scaling and colouring for
		// the C1 norns, as long as sprites added later do not clash). Decided
		// by the body's pictures (part 0), for the whole creature: once a
		// breed has its own old-age set, the body is found at that age and
		// this is off.
		// OPENC2E_OLD_TINT=g,l sets the grey share and lift; 0 turns it off.
		if (i == 0)
			oldWithoutArt = creature->getStage() == 5 && foundstage >= 0 && foundstage < 5;
		static const std::array<float, 2> oldTint = [] {
			std::array<float, 2> t{0.25f, 0.04f};
			if (const char* e = devenv("OPENC2E_OLD_TINT"))
				if (sscanf(e, "%f,%f", &t[0], &t[1]) < 1)
					t = {0.0f, 0.0f};
			return t;
		}();
		if (oldWithoutArt && oldTint[0] > 0.0f)
			images[i] = world.gallery->muted(images[i], oldTint[0], oldTint[1]);
		// Babies are drawn more vivid (owner, 2026-11-14: babies "more
		// vibrant"; chose +100% of the options shown, "subtlety is not
		// needed"): their pictures' colours are pushed away from grey by
		// 1.0 (twice as saturated), the reverse of the old's grey above.
		// OPENC2E_BABY_VIVID=s sets another strength (0 turns it off).
		static const float babyVivid = [] {
			const char* e = devenv("OPENC2E_BABY_VIVID");
			return e ? (float)atof(e) : 1.0f;
		}();
		if (babyVivid > 0.0f && creature->getStage() == 0)
			images[i] = world.gallery->muted(images[i], -babyVivid, 0.0f);
	}
}

std::shared_ptr<creaturesImage> SkeletalCreature::tintBodySprite(std::shared_ptr<creaturesImage> s) {
	// TODO: don't bother tinting if we don't need to

	// TODO: work out tinting for other engine versions
	if (engine.version > 2) {
		std::shared_ptr<creaturesImage> newimage = world.gallery->tint(s, creature->getTint(0), creature->getTint(1), creature->getTint(2), creature->getTint(3), creature->getTint(4));
		return newimage;
	}

	return s;
}

void SkeletalCreature::render(RenderTarget* renderer, int xoffset, int yoffset) {
	// nowhere near the picture (the world is drawn three times for its
	// wraparound): none of its parts would be drawn (performance,
	// 2026-11-16); the margin covers poses whose pictures differ in size
	// from the ones its bounds were worked out with
	if (!displaycore) {
		const int margin = 32;
		if (xoffset + drawnwidth + margin < 0 || xoffset - margin > renderer->getWidth() || yoffset + drawnheight + margin < 0 || yoffset - margin > renderer->getHeight())
			return;
	}
	if (!displaycore && renderGliding(renderer, xoffset, yoffset))
		return;
	bool mirror_body_parts = (world.variables["engine_mirror_creature_body_parts"] == 1);

	for (int j = 0; j < 17; j++) {
		int i = cee_zorder[posedirection][j];
		if (i >= SkeletalPartCount())
			continue; // CV hackery

		if (engine.version == 1) // TODO: this is hackery to skip tails for C1
			if (i == 6 || i == 13)
				continue;

		bodypartinfo* part = &cee_bodyparts[i];

		unsigned int ourpose = pose[i];

		bool mirror = false;
		if (i != 14 && i != 15 && mirror_body_parts && ourpose >= 4 && ourpose <= 7) {
			ourpose -= 4;
			mirror = true;
		}

		// adjust for pregnancy/facial expressions/etc as necessary
		if (part->parent == -1) { // body
			ourpose += (pregnancy * (engine.version < 3 ? 10 : 16));
		} else if (i == 1) { // head
			ourpose += ((eyesclosed || blinkticks > 0) ? (engine.version < 3 ? 10 : 16) : 0) + (facialexpression * (engine.version < 3 ? 20 : 32));
		} else if (i == 16) { // hair
			ourpose += 0; // TODO: 16 * hair
		}

		assert(images[i]);

		RenderOptions render_opts;
		render_opts.mirror = mirror;
		renderer->renderCreaturesImage(images[i], ourpose, partx[i] + adjustx + xoffset, party[i] + adjusty + yoffset, render_opts);

		if (displaycore) {
			// TODO: we draw a lot of points twice here :)
			Color color{0xff, 0x1e, 0xff, 0xcc};
			int atx = attachmentX(i, 0) + xoffset, aty = attachmentY(i, 0) + yoffset;
			renderer->renderLine(atx - 1, aty, atx + 1, aty, color);
			renderer->renderLine(atx, aty - 1, atx, aty + 1, color);
			atx = attachmentX(i, 1) + xoffset;
			aty = attachmentY(i, 1) + yoffset;
			renderer->renderLine(atx - 1, aty, atx + 1, aty, color);
			renderer->renderLine(atx, aty - 1, atx, aty + 1, color);
		}
	}
}

float SkeletalCreature::footX() {
	float fx = x + attachmentX(downfoot_left ? 11 : 12, 1);
	if (world.map) {
		MetaRoom* m = world.map->metaRoomAt(x, y);
		if (!m)
			m = world.map->wrappingMetaRoomNear(fx, y);
		if (m && m->wraparound()) {
			if (fx >= (int)m->x() + (int)m->width())
				fx -= m->width();
			else if (fx < (int)m->x())
				fx += m->width();
		}
	}
	return fx;
}

int SkeletalCreature::attachmentX(unsigned int part, unsigned int id) {
	return partx[part] + att[part].attachments[pose[part]][0 + (id * 2)] + adjustx;
}

bool SkeletalCreature::headTop(float& hx, float& hy) {
	if (!images[1])
		return false;
	hx = x + partx[1] + adjustx + images[1]->width(pose[1]) / 2.0f;
	hy = y + party[1] + adjusty;
	return true;
}

namespace {
// a head picture's visible box and, on each side, the middle height of its
// outermost column (the snout tip, facing that way): worked out once per
// picture and frame
struct HeadShape {
	int l = 0, t = 0, r = 0, b = 0, tipRight = 0, tipLeft = 0;
	bool ok = false;
};
HeadShape headShape(const creaturesImage* img, unsigned int frame) {
	static std::map<std::pair<const creaturesImage*, unsigned int>, HeadShape> cache;
	auto key = std::make_pair(img, frame);
	if (auto it = cache.find(key); it != cache.end())
		return it->second;
	HeadShape s;
	int w = (int)img->width(frame), h = (int)img->height(frame);
	s.l = w;
	s.t = h;
	s.r = -1;
	s.b = -1;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (!img->transparentAt(frame, x, y)) {
				s.l = std::min(s.l, x);
				s.r = std::max(s.r, x);
				s.t = std::min(s.t, y);
				s.b = std::max(s.b, y);
			}
	if (s.r >= s.l && s.b >= s.t) {
		auto middle = [&](int x) {
			int sum = 0, n = 0;
			for (int y = 0; y < h; y++)
				if (!img->transparentAt(frame, x, y)) {
					sum += y;
					n++;
				}
			return n ? sum / n : s.b;
		};
		s.tipRight = middle(s.r);
		s.tipLeft = middle(s.l);
		s.r += 1;
		s.b += 1;
		s.ok = true;
	}
	cache[key] = s;
	return s;
}
} // namespace

bool SkeletalCreature::bubbleSpot(float& bx, float& by) {
	if (!images[1] || pose[1] >= images[1]->numframes())
		return false;
	unsigned int view = pose[1];
	HeadShape s = headShape(images[1].get(), view);
	if (!s.ok)
		return false;
	bool right = view <= 3 || view == 8;
	float y = right ? s.tipRight : s.tipLeft;
	// looking up: higher (slightly up: 45% of the head's height from its
	// top; fully up: 25%), and at least as high as the head's mouth point
	if (view == 2 || view == 6 || view == 3 || view == 7) {
		float share = (view == 2 || view == 6) ? 0.45f : 0.25f;
		y = std::min(y, s.t + share * (s.b - s.t));
		if (att[1].nolines > view && att[1].noattachments[view] >= 4)
			y = std::min(y, (float)att[1].attachments[view][3]);
	}
	float headx = x + partx[1] + adjustx, heady = this->y + party[1] + adjusty;
	// the bubble picture has its bubble in columns 8-21, its bottom at row 42
	bx = headx + (right ? s.r + 3 - 8 : s.l - 3 - 21);
	by = heady + y - 42 + 4;
	return true;
}

int SkeletalCreature::attachmentY(unsigned int part, unsigned int id) {
	return party[part] + att[part].attachments[pose[part]][1 + (id * 2)] + adjusty;
}

void SkeletalCreature::recalculateSkeleton() {
	glideBegin();
	int lowestx = 0, lowesty = 0, highestx = 0, highesty = 0;

	for (int i = 0; i < SkeletalPartCount(); i++) {
		if (engine.version == 1) // TODO: this is hackery to skip tails for C1
			if (i == 6 || i == 13)
				continue;

		bodypartinfo* part = &cee_bodyparts[i];

		if (part->parent == -1) {
			partx[i] = 0;
			party[i] = 0;
		} else {
			attFile& bodyattinfo = att[0];
			// attFile &attinfo = att[i];

			int attachx = att[i].attachments[pose[i]][0];
			int attachy = att[i].attachments[pose[i]][1];
			int x, y;

			if (part->parent == 0) { // linking to body
				x = bodyattinfo.attachments[pose[0]][part->attorder * 2];
				y = bodyattinfo.attachments[pose[0]][(part->attorder * 2) + 1];
			} else { // extra limb
				attFile& parentattinfo = att[part->parent];

				x = partx[part->parent] + parentattinfo.attachments[pose[part->parent]][part->attorder * 2];
				y = party[part->parent] + parentattinfo.attachments[pose[part->parent]][(part->attorder * 2) + 1];
			}

			x = x - attachx;
			y = y - attachy;

			partx[i] = x;
			party[i] = y;

			if (x < lowestx) {
				lowestx = x;
			}
			if (y < lowesty) {
				lowesty = y;
			}
			if (x + (int)images[i]->width(pose[i]) > highestx) {
				highestx = x + images[i]->width(pose[i]);
			}
			if (y + (int)images[i]->height(pose[i]) > highesty) {
				highesty = y + images[i]->height(pose[i]);
			}
		}
	}

	adjustx = -lowestx;
	adjusty = -lowesty;
	drawnwidth = std::max(1, highestx - lowestx);
	drawnheight = std::max(1, highesty - lowesty);

	// TODO: muh, we should cooperate with physics system etc
	if (!carriedby && !invehicle && calculated) {
		int orig_footpart = (downfoot_left ? 11 : 12);
		float oldbottom = y + height; // height is the lower foot's y, from the last calculation
		// adjust location to match foot
		float dx = attachmentX(orig_footpart, 1) - oldfootx;
		// C2: a creature in the air does not walk. Its walk carried on while
		// it fell (the mind's walk goes on, and each step moved it on by the
		// planted foot's stride), and nothing checks walls in the air, so a
		// norn that went off the ledge by the west lift (x 494, y 1540) came
		// down 184 px further east, through the rock face into the room
		// below (owner, 2026-11-13). Moving up or down, or well clear of the
		// floor, a pose change keeps it where it is across (the floor test
		// alone missed a fall through an open floor between two rooms).
		if (engine.version == 2 && falling && world.map) {
			float cx = x + getWidth() / 2.0f, feet = y + height;
			std::shared_ptr<Room> r = world.map->roomAt(cx, feet - 1);
			if (std::fabs(vely) > 3.0f || !r || r->floorYatX(cx) - feet > 8.0f)
				dx = 0;
			// settling on the floor after a hop (still "falling", so no foot
			// snapping checks walls): a pose change does not carry it into a
			// room it could not walk into. Knocked back by a hit at the end of
			// the floor under the lab dome, norns were stepped through the
			// wall (a doorway of 50) onto the open floor beyond, and fell.
			if (dx != 0 && r) {
				std::shared_ptr<Room> to = world.map->roomAt(cx - dx, feet - 1);
				if (to != r && (!to || !world.map->hasDoor(r, to) || world.map->getDoorPerm(r, to) < size))
					dx = 0;
			}
		}
		moveTo(x - dx, y - (attachmentY(orig_footpart, 1) - oldfooty));
		if (engine.version == 2 && falling) {
			// While physics is in charge (falling, landing, bouncing), nothing snaps the
			// feet back to a floor, and keeping the previous down foot fixed lets the
			// other foot reach lower in the new pose. That pushed the body's bottom edge
			// through floors into the room below (e.g. through the pond bottom), where
			// physics then let the creature keep falling. Never let a pose change move
			// the lowest foot down while falling.
			float newlowest = std::max(attachmentY(11, 1), attachmentY(12, 1));
			if (y + newlowest > oldbottom)
				moveTo(x, oldbottom - newlowest);
		}
	}

	// work out which foot is down
	int leftfoot = attachmentY(11, 1);
	int rightfoot = attachmentY(12, 1);
	downfoot_left = (rightfoot < leftfoot);

	calculated = true;
	int orig_footpart = (downfoot_left ? 11 : 12);
	oldfootx = attachmentX(orig_footpart, 1);
	oldfooty = attachmentY(orig_footpart, 1);

	// recalculate width/height
	height = downfoot_left ? leftfoot : rightfoot;
	width = 50; // TODO: arbitary values bad

	glideEnd();

	// what it carries goes in front of or behind it with the hand (re-sorted
	// for drawing when that changes)
	if (bool front = carriesInFront(); front != carriedFront) {
		carriedFront = front;
		if (carrying)
			carrying->setZOrder(carrying->zorder);
	}

	// TODO: muh, we should cooperate with physics system etc
	/*if (carriedby || invehicle)
		downfootroom.reset();
	else
		snapDownFoot();*/
}

void SkeletalCreature::snapDownFoot() {
	// TODO: this isn't very well thought-out.

	int orig_footpart = (downfoot_left ? 11 : 12);
	float footx = x + attachmentX(orig_footpart, 1);
	float footy = y + attachmentY(orig_footpart, 1);

	MetaRoom* m = world.map->metaRoomAt(x, y);
	if (!m) {
		// past the edge of a wrapping world, which has no metaroom of its own
		// out there: find the wrapping metaroom it belongs to
		m = world.map->wrappingMetaRoomNear(footx, y);
	}
	if (!m)
		return; // TODO: exceptiony death

	// In a wrapping world (C2's Albia joins its east and west edges) openc2e
	// wraps an agent by its top-left corner, so a creature near the east edge
	// can have its foot (about 50 px right of that corner) past the map. Judge
	// rooms and floors with the foot's x wrapped into the map. Without this,
	// room lookups (which wrap on their own) gave such a creature the rooms at
	// the west end while its floors were measured at the out-of-range x, and
	// one fell through a solid floor and out of the room system.
	float wrapshift = 0;
	auto wrapFoot = [&](float fx) {
		if (m->wraparound()) {
			if (fx >= (int)m->x() + (int)m->width())
				return -(float)m->width();
			if (fx < (int)m->x())
				return (float)m->width();
		}
		return 0.0f;
	};
	wrapshift = wrapFoot(footx);
	footx += wrapshift;

	std::shared_ptr<Room> newroom;

	// C2: whether the floor from where the foot last stood to where it is now
	// is one slope, however steep, rather than a step: the two rooms' floor
	// lines, sampled every 4 px, never change by more than 7 (about 60
	// degrees). A long stride on a steep slope rises over 40 px, which was
	// taken for a wall, or goes down over 20, which was taken for a fall: at
	// the hidden tunnel west of the incubator's pond (47 degrees) 9 of 27
	// creatures walking west and 9 of 26 walking east were stuck on it (a
	// player, 2026-11-22).
	auto oneSlope = [&](const std::shared_ptr<Room>& from, const std::shared_ptr<Room>& to) {
		static const bool off = devenv("OPENC2E_NO_SLOPES") != nullptr; // (to compare)
		if (engine.version != 2 || off || !from || !to || carriedby || invehicle)
			return false;
		const float x0 = (float)lastgoodfootx + wrapshift, x1 = footx;
		if (std::fabs(x1 - x0) < 1.0f || std::fabs(x1 - x0) > 80.0f)
			return false;
		auto floorAt = [&](float fx, float& fy) {
			Room* r = fx >= from->x_left && fx <= from->x_right ? from.get() : fx >= to->x_left && fx <= to->x_right ? to.get() : nullptr;
			if (r)
				fy = r->floorYatX(fx);
			return r != nullptr;
		};
		const int n = std::max(1, (int)std::ceil(std::fabs(x1 - x0) / 4.0f));
		float last = 0, fy = 0;
		for (int k = 0; k <= n; k++) {
			if (!floorAt(x0 + (x1 - x0) * k / n, fy))
				return false;
			if (k > 0 && std::fabs(fy - last) > 7.0f)
				return false;
			last = fy;
		}
		return true;
	};

	if (downfootroom) {
		if (downfootroom->containsPoint(footx, footy)) {
			newroom = downfootroom;
		} else {
			if (downfootroom->x_left <= footx && downfootroom->x_right >= footx) {
				newroom = downfootroom; // TODO, we're just forcing for now
			} else {
				float ydiff = 10000.0f; // TODO: big number
				const float unwrappedfootx = footx;
				for (auto& d : downfootroom->getDoors()) {
					std::shared_ptr<Room> thisroom = d.first;
					// A wrapping world (C2's Albia joins its east and west edges
					// through doors) has rooms just past the seam at the other end:
					// judge them with the foot's x wrapped to that end, or a creature
					// walking off the edge finds no room and is pushed back.
					footx = unwrappedfootx;
					if (m->wraparound()) {
						if (footx >= m->x() + (int)m->width() && thisroom->x_left < m->x() + (int)m->width() / 2)
							footx -= m->width();
						else if (footx < m->x() && thisroom->x_right > m->x() + (int)m->width() / 2)
							footx += m->width();
					}
					if (engine.version == 2 && size > d.second.perm)
						continue;
					// From dry ground a water room is a wall: creatures fall or are
					// dropped into water but never walk in (they walk out of it, up
					// the pond's ramp). Which way a walk action goes depends on the
					// creature's poses, so the mind's own check missed some: a norn
					// walked off the incubator bank into the pond and was trapped at
					// its far end (regression batch reg5).
					if (engine.version == 2 && thisroom->deepWater() && downfootroom->type != 2)
						continue;
					// A neighbouring floor far above the foot is a wall, not a step. Without
					// this, a creature at an edge whose level exit is closed to it (e.g. the
					// garden's left edge, where the door down to the watermill ramp is too
					// small for creatures) was moved into the open-air room above and left
					// to fall, dropping it into the pit below.
					// A room whose floor line can be walked beneath (floor value above
					// 0: a lift shaft whose bottom is the lift's lower stop) is judged
					// by its bottom edge, as walking in it is (below): its line at the
					// top of the shaft made the shaft's doorway a wall, so creatures
					// stepped in place at the desert lift's foot (owner, 2026-11-09).
					float thisfloor = thisroom->floorYatX(footx);
					if (engine.version == 2 && thisroom->floorvalue > 0 && thisfloor < footy - 40)
						thisfloor = thisroom->bottom.pointAtX(footx).y;
					if (engine.version == 2 && thisfloor < footy - 40 && !oneSlope(downfootroom, thisroom))
						continue;
					if (thisroom->x_left <= footx && thisroom->x_right >= footx) {
						float thisydiff = fabs(footy - thisfloor);
						if (thisydiff < ydiff) {
							newroom = thisroom;
							ydiff = thisydiff;
						}
					}
				}
				footx = unwrappedfootx;
				// (crossing the seam needs no move here: the foot is judged
				// wrapped above, and moveTo wraps the creature itself)
			}
		}
	} else {
		newroom = bestRoomAt(footx, footy, 3, m, std::shared_ptr<Room>());
		// C2: a foot placed afresh (after a load, a lift's let-out or a
		// script's move) beyond a wall from the creature's middle stays on
		// the middle's side. Facing west, a creature's foot is near its box's
		// left, so one let out of the tree-trunk lift at its shaft's west
		// wall (rooms 79/80, from x 1546) had its foot placed in the sealed
		// rock beyond (rooms 102/103) and was moved out again and again
		// (2026-11-15). It is moved over so the foot is inside the middle's
		// room.
		// (only from a sealed room, one no doorway into is ever open, to one
		// that is not: the other way round, a foot in the shaft with the
		// middle over the rock, it stays where it is)
		auto sealedRoom = [](const std::shared_ptr<Room>& r) {
			for (auto& d : r->getDoors())
				if (d.second.perm > 0 || d.second.everOpen)
					return false;
			return true;
		};
		static const bool noFootMiddle = devenv("OPENC2E_NO_FOOT_MIDDLE") != nullptr; // (to compare)
		if (engine.version == 2 && !noFootMiddle && newroom && sealedRoom(newroom)) {
			float midx = x + getWidth() / 2.0f;
			std::shared_ptr<Room> mid = bestRoomAt(midx, footy, 3, m, std::shared_ptr<Room>());
			// (a middle on the wall line itself belongs to the side away from
			// the foot: a norn facing the library's east wall, its middle at
			// x 1473 where the library (114) meets the sealed shaft (102), had
			// that line judged part of the shaft, so its foot stayed in the
			// shaft and it was "rescued" from it; 2026-11-18)
			if (mid == newroom) {
				std::shared_ptr<Room> side = bestRoomAt(midx + (footx > midx ? -3.0f : 3.0f), footy, 3, m, std::shared_ptr<Room>());
				if (side)
					mid = side;
			}
			if (mid && mid != newroom && !sealedRoom(mid) && (!world.map->hasDoor(mid, newroom) || world.map->getDoorPerm(mid, newroom) < size)) {
				float dx = 0;
				if (footx < (float)mid->x_left + 2)
					dx = (float)mid->x_left + 2 - footx;
				else if (footx > (float)mid->x_right - 2)
					dx = (float)mid->x_right - 2 - footx;
				if (dx != 0 && std::fabs(dx) <= getWidth()) {
					x += dx;
					footx += dx;
					newroom = mid;
					if (devenv("OPENC2E_WATCH_ROOMS"))
						fmt::print("* WATCH ROOM t{} unid {} foot kept with its middle in room {} (moved {:.0f})\n", world.tickcount, unid, mid->id, dx);
				}
			}
		}
		// a foot placed a few pixels under a floor (a world's restore can put
		// it 1-2 px low) stands on that floor, not on the floor of the room
		// below: a grendel saved on room 34's floor (y 1858) was set on room
		// 50's, 366 px down (2026-11-12, "creatures fall through floors")
		if (engine.version == 2) {
			std::shared_ptr<Room> above = bestRoomAt(footx, footy - 8, 3, m, std::shared_ptr<Room>());
			if (above && above != newroom && above->type != 2) {
				float fl = above->floorYatX(footx);
				if (fl >= footy - 8 && fl <= footy + 1)
					newroom = above;
			}
		}

		// insane emergency handling
		float newfooty = footy;
		while (!newroom && newfooty > (footy - 500.0f)) {
			newroom = m->roomAt(footx, newfooty);
			newfooty--;
		}

		// TODO: give up here

		footy = newfooty;
	}

	bool newroomchosen = (newroom != downfootroom) && downfootroom; // (set again after a push back)
	bool hadroom = downfootroom != nullptr;
	// developer aid: OPENC2E_WATCH_ROOMS prints each change of a creature's
	// foot room and which way it was found
	static const bool watchrooms = devenv("OPENC2E_WATCH_ROOMS") != nullptr;
	auto watchRoom = [&](const char* how, std::shared_ptr<Room> from, std::shared_ptr<Room> to) {
		if (watchrooms && from != to)
			fmt::print("* WATCH ROOM t{} unid {} {} foot ({:.0f}, {:.0f}) x {:.0f} room {} -> {}{}\n", world.tickcount, unid, how,
				footx, footy, x, from ? (int)from->id : -1, to ? (int)to->id : -1,
				from && to && world.map->hasDoor(from, to) ? fmt::format(" (door perm {})", world.map->getDoorPerm(from, to)) : std::string());
	};
	watchRoom(hadroom ? "walk" : "place", downfootroom, newroom);
	std::shared_ptr<Room> prevroom = downfootroom;
	downfootroom = newroom;

	if (!downfootroom /*|| !falling */) {
		// TODO: hackery to cope with scripts moving us, this needs handling correctly somewhere
		// how far the foot is from where it last stood (across the seam of a
		// wrapping world, the short way round)
		float movedx = lastgoodfootx - attachmentX(orig_footpart, 1) - x;
		if (m->wraparound())
			movedx -= std::round(movedx / (float)m->width()) * (float)m->width();
		float movedy = lastgoodfooty - attachmentY(orig_footpart, 1) - y;
		// A short jump of the foot into a room that the room it stood in has
		// no doorway to (open at the creature's size) is a wall, not a script
		// moving it: at the world's seam a grendel turning at the library's
		// west wall had its foot shift ~58 px west across the seam, and was
		// taken into the sealed pocket beyond (room 213), where it fell to the
		// map's bottom and was trapped (owner, 2026-11-10).
		// A free creature (not carried, riding or just moved by a script)
		// meets a shut door between two rooms as a wall however far its
		// foot went: a shove (the hand's push, another creature's hit)
		// threw a norn more than 50 px against the airlock's shut land door,
		// which the retry below took for a script's move, accepting the
		// chamber beyond it (owner, 2026-11-08: "walked through the
		// airlock's closed land door").
		bool walled = false;
		if (engine.version == 2 && prevroom) {
			bool near = fabs(movedx) <= 150.0f && fabs(movedy) <= 150.0f;
			bool freebody = !carriedby && !invehicle && world.tickcount > lastHeldTick + 5 && world.tickcount > lastScriptMoveTick + 5;
			std::shared_ptr<Room> here = (near || freebody) ? bestRoomAt(footx, footy, 3, m, std::shared_ptr<Room>()) : std::shared_ptr<Room>();
			if (here && here != prevroom) {
				bool door = world.map->hasDoor(prevroom, here);
				bool shut = !door || world.map->getDoorPerm(prevroom, here) < size;
				if (shut && (near || (door && freebody && !devenv("OPENC2E_NO_SHUT_DOOR_WALL")))) {
					walled = true;
					watchRoom(near ? "walled" : "walled (shut door)", here, prevroom);
				}
			}
		}
		if (walled) {
			walledCount++;
			walledDir = movedx < 0 ? 11 : 10; // (the foot went east of where it last stood: east)
		} // pushed back below
		else if (fabs(movedx) > 50.0f || fabs(movedy) > 50.0f) {
			// Retry once from the room the foot is in now. Only once: if the
			// retry also finds no room it may enter (e.g. every way on is a
			// wall), retrying again recursed until the stack overflowed and the
			// game vanished without a trace. After one retry, accept the room
			// the foot is in (a script moved us; pushing back would undo it).
			static bool retrying = false;
			std::shared_ptr<Room> here = bestRoomAt(footx, footy, 3, m, std::shared_ptr<Room>());
			if (here && !retrying) {
				watchRoom("retry", downfootroom, here);
				downfootroom = here;
				retrying = true;
				snapDownFoot();
				retrying = false;
				return;
			} else if (!here) {
				// (at most a few times: a stuck creature would say it every tick)
				static int said = 0;
				if (said < 20 && ++said)
					fmt::print(stderr, "Creature out of room system at ({}, {})!{}\n", footx, footy, said == 20 ? " (no more of these this run)" : "");
				// TODO: exceptiony death?
				return;
			}
			watchRoom("accept", downfootroom, here);
			downfootroom = here;
		}
	}

	if (!downfootroom) {
		// We fell out of the room system! How did that happen? Push ourselves back in, run collision script.
		// fmt::print("Creature out of room system at ({}, {}), pushing it back in.\n", footx, footy);

		// TODO: sucky code
		x = lastgoodfootx - attachmentX(orig_footpart, 1);
		wrapshift = wrapFoot(lastgoodfootx);
		footx = lastgoodfootx + wrapshift;
		footy = lastgoodfooty;
		// C2: back on the floor it stood on. A walking foot's point lies a
		// few pixels under the floor line, so looking the room up there found
		// the room below whenever that room's top is the floor: walking into
		// the wall at the east end of the floor under the lab dome (rooms
		// 210/211, the room below 38) dropped norns 300 px onto the hut
		// platform (owner, 2026-11-13; four of six norns in a test).
		if (engine.version == 2 && prevroom && prevroom->x_left <= footx && prevroom->x_right >= footx)
			downfootroom = prevroom;
		else
			downfootroom = m->roomAt(footx, footy);
		newroomchosen = downfootroom != prevroom && prevroom;
		queueScript(6);

		if (!downfootroom) {
			static int said = 0;
			if (said < 20 && ++said)
				fmt::print(stderr, "no down foot room! ({}, {}){}\n", footx, footy, said == 20 ? " (no more of these this run)" : "");
			// TODO: exceptiony death
			return;
		}
	}

	bool belowfloor = false;
	float newy = downfootroom->floorYatX(footx);
	// (also for a foot placed afresh: a norn let out or moved by a script
	// into the desert lift's shaft, room 12, below its floor line, was set
	// up on that line, 243-301 px above; 2026-11-18, found from BIG MOVE)
	if (engine.version == 2 && y > newy && downfootroom->floorvalue > 0) {
		// TODO: hilar hack: cope with walking below floors.
		// C2 rooms with floor points have a ground line inside the room. Where that
		// ground is passable (a floor value above 0, e.g. a lift shaft whose bottom
		// is the stop below the ground), creatures may walk along the room's real
		// bottom beneath it. Where the floor value is 0 the ground is solid, nothing
		// is meant to be beneath it, and pinning a creature to the bottom buried it
		// in the scenery; there the creature stays on (or returns to) the ground line.
		belowfloor = true;
		if (devenv("OPENC2E_WATCH_CREATURES"))
			fmt::print("* WATCH BELOWFLOOR t{} unid {} foot ({:.0f}, {:.0f}) room {} floor {:.0f} -> room bottom {:.0f}\n",
				world.tickcount, unid, footx, footy, downfootroom->id, newy, downfootroom->bottom.pointAtX(footx).y);
		newy = downfootroom->bottom.pointAtX(footx).y;
	}

	// C2: about to fall (the foot stepped onto a floor far below) with the
	// body's bottom a few px under the floor that its middle stands on and
	// that holds it (solid at its size): the body is first set back on that
	// floor, so the fall starts on it and it holds. A walking foot's point
	// lies a few px under the floor line, and creatures walking east off the
	// cage lift's middle stop (room 21, floor 840, its way down 120 wide)
	// into the tunnel beside (room 20, its floor 25 px lower) began their
	// fall below 840 and dropped 320-345 px down the empty shaft, badly hurt
	// (0-6 a 5-minute copy; 2026-11-19).
	auto onMiddleFloor = [&]() {
		if (engine.version != 2 || carriedby || invehicle || !downfootroom)
			return;
		float midx = x + getWidth() / 2.0f, bottom = y + getHeight();
		std::shared_ptr<Room> mid = m->roomAt(midx, bottom - 10);
		if (!mid || mid == downfootroom || mid->type == 2)
			return;
		float mf = mid->floorYatX(midx);
		if (mf > bottom - 0.5f || mf < bottom - 12.0f)
			return; // (on the floor or above it: the fall starts on it anyway)
		bool solid;
		if (mid->floorpoints.size())
			solid = size > mid->floorvalue;
		else {
			std::shared_ptr<Room> under = m->roomAt(midx, std::max(mid->y_left_floor, mid->y_right_floor) + 1);
			solid = !under || !world.map->hasDoor(mid, under) || world.map->getDoorPerm(mid, under) < size;
		}
		if (!solid)
			return;
		if (devenv("OPENC2E_WATCH_ROOMS"))
			fmt::print("* WATCH ROOM t{} unid {} falling from under room {}'s floor ({:.0f}, bottom {:.0f}): set on it\n", world.tickcount, unid, mid->id, mf, bottom);
		moveTo(x, mf - getHeight() - 1);
	};

	if (engine.version > 1) {
		// TODO: hilar hack: enable gravity if we're snapping by much
		// (down only: a step up into the next room, up to the 40 px the door
		// rule above allows, is taken; as a fall, a creature at the top of the
		// boardwalk pool's bank fell back each time from the 33 px step onto
		// the planks, owner 2026-11-10)
		const bool slope = (newy - (footy - y)) - y > 20 && hadroom && !belowfloor && oneSlope(prevroom, downfootroom);
		if (newroomchosen && (newy - (footy - y)) - y > 20 && !slope) {
			onMiddleFloor();
			falling = true;
			return;
		}
		// a creature just placed (put down by the hand, let out of a vehicle,
		// moved by a script) with its floor far below falls to it, rather
		// than being set on it at once: a grendel let go above the open
		// floor at the far east was moved 313 px down in one step
		// (2026-11-12). Not at a world's start (the save's places stand).
		if (engine.version == 2 && !hadroom && engine.worldready && !carriedby && !invehicle && (newy - (footy - y)) - y > 20) {
			onMiddleFloor();
			falling = true;
			return;
		}
		// and one whose foot room is the same but whose floor there is far
		// below (a tall room kept as the foot's room while the foot is
		// past its edge, the "forcing" above): norns at the east end of the
		// treehouse's upper platform (room 51 ends at x 3917; room 52
		// beside it runs down to the cave floor) were set 470 px down in
		// one step, no fall (2026-11-18, found from BIG MOVE)
		if (engine.version == 2 && hadroom && engine.worldready && !carriedby && !invehicle && (newy - (footy - y)) - y > 20 && !slope) {
			onMiddleFloor();
			falling = true;
			return;
		}
	}

	moveTo(x, newy - (footy - y));

	lastgoodfootx = footx - wrapshift; // kept in the creature's own (unwrapped) terms
	lastgoodfooty = footy;

	// Falling through a floor a creature is light enough to pass (C2: its size
	// against the floor value or the permeability of the door below). A 2008
	// change meant to extend this to C3 wrote "> 2" instead of ">= 2", so C2
	// creatures never fell through any floor: they stood on sea surfaces.
	if (engine.version >= 2) {
		if (engine.version == 2 && !belowfloor && downfootroom->floorpoints.size()) {
			// TODO: hilar hack: same as above for floorvalue
			if (size <= downfootroom->floorvalue) {
				falling = true;
				return;
			}
		} else {
			// TODO: hilar hack: same as above for perm
			std::shared_ptr<Room> downroom = world.map->roomAt(footx, downfootroom->y_left_floor + 1);
			if (world.map->hasDoor(downfootroom, downroom)) {
				int permsize = (engine.version == 2 ? size : perm);
				if (permsize <= world.map->getDoorPerm(downfootroom, downroom)) {
					falling = true;
					return;
				}
			}
		}
	}
}

SkeletalCreature::PoseMemo SkeletalCreature::memoPose() const {
	PoseMemo m;
	std::copy(pose, pose + 17, m.pose);
	m.posedirection = posedirection;
	m.facialexpression = facialexpression;
	m.eyesclosed = eyesclosed;
	m.calculated = calculated;
	m.downfoot_left = downfoot_left;
	m.blinkticks = blinkticks;
	m.oldfootx = oldfootx;
	m.oldfooty = oldfooty;
	m.lastgoodfootx = lastgoodfootx;
	m.lastgoodfooty = lastgoodfooty;
	m.width = width;
	m.height = height;
	m.adjustx = adjustx;
	m.adjusty = adjusty;
	std::copy(partx, partx + 17, m.partx);
	std::copy(party, party + 17, m.party);
	m.x = x;
	m.y = y;
	m.glide = glide;
	return m;
}

void SkeletalCreature::restorePose(const PoseMemo& m) {
	std::copy(m.pose, m.pose + 17, pose);
	posedirection = m.posedirection;
	facialexpression = m.facialexpression;
	eyesclosed = m.eyesclosed;
	calculated = m.calculated;
	downfoot_left = m.downfoot_left;
	blinkticks = m.blinkticks;
	oldfootx = m.oldfootx;
	oldfooty = m.oldfooty;
	lastgoodfootx = m.lastgoodfootx;
	lastgoodfooty = m.lastgoodfooty;
	width = m.width;
	height = m.height;
	adjustx = m.adjustx;
	adjusty = m.adjusty;
	std::copy(m.partx, m.partx + 17, partx);
	std::copy(m.party, m.party + 17, party);
	x = m.x;
	y = m.y;
	glide = m.glide;
}

void SkeletalCreature::setPose(unsigned int p) {
	posedirection = 0;
	for (int i = 0; i < SkeletalPartCount(); i++)
		pose[i] = p;
	recalculateSkeleton();
}

void SkeletalCreature::setPose(std::string s) {
	switch (s[0]) {
		case '?': // towards object of attention
			switch (direction) {
				case 0: posedirection = 3; break; // north, TODO
				case 1: posedirection = 2; break; // south, TODO
				case 2: posedirection = 0; break; // right
				case 3: posedirection = 1; break; // left
				default: assert(false);
			}
			break;
		case '!': // away from object of attention
			switch (direction) {
				case 0: posedirection = 2; break; // north, TODO
				case 1: posedirection = 3; break; // south, TODO
				case 2: posedirection = 1; break; // right
				case 3: posedirection = 0; break; // left
				default: assert(false);
			}
			break;
		case '0': posedirection = 3; break;
		case '1': posedirection = 2; break;
		case '2': posedirection = 0; break;
		case '3': posedirection = 1; break;
		case 'X': break; // do nothing
		default:
			fmt::print("internal warning: SkeletalCreature::setPose didn't understand direction {} in pose '{}'.\n", s[0], s);
			break;
	}

	for (int i = 0; i < 14; i++) {
		int newpose = -1;

		switch (s[i + 1]) {
			case '0': newpose = 0; break;
			case '1': newpose = 1; break;
			case '2': newpose = 2; break;
			case '3': newpose = 3; break;
			case '4': newpose = (engine.version < 3) ? 8 : 10; break; // 'to camera'
			case '?':
				assert(i == 0);
				{ // TODO
					// make the head look in the posedirection of _IT_
					float attachmenty = attachmentY(1, 0) + y; // head attachment point, which we'll use to 'look' from atm

					// TODO: this is horrible, but i have no idea how the head angle is calculated
					AgentRef attention = creature->getAttentionFocus();
					if (attention && attention->y > (attachmenty + 30))
						newpose = 0;
					else if (attention && attention->y < (attachmenty - 70))
						newpose = 3;
					else if (attention && attention->y < (attachmenty - 30))
						newpose = 2;
					else
						newpose = 1;
				}
				break;
			// TODO: '!' also?
			case 'X': continue; // do nothing
			default:
				fmt::print("internal warning: SkeletalCreature::setPose didn't understand {} in pose '{}'.\n", s[i + 1], s);
				continue;
		}

		assert(newpose != -1);

		if (newpose < 4) {
			// newpose gives the angle, now we need to add the offset for left/right/forward/back.

			if (engine.version < 3 && posedirection > 1)
				newpose = 6 + posedirection; // only one forward/back pose in c1/c2
			else
				newpose += (posedirection * 4);
		}

		pose[cee_lookup[i]] = newpose;

		// TODO: this is some hackery for CV,
		if (engine.gametype != "cv")
			continue;
		if (i == 0) { // head
			pose[14] = newpose;
			pose[15] = newpose; // ears
			pose[16] = newpose; // hair
		} else if (i == 1) {
			pose[6] = newpose; // tail root
			pose[13] = newpose; // tail tip
		}
	}

	recalculateSkeleton();
}

void SkeletalCreature::setPoseGene(unsigned int poseno) {
	std::map<unsigned int, creaturePoseGene*>::iterator i = posegenes.find(poseno);
	if (i == posegenes.end())
		return; // TODO: is there a better behaviour here?

	creaturePoseGene* g = i->second;
	assert(g->poseno == poseno);
	gaitgene = 0;
	walking = false; // TODO: doesn't belong here, does it? really the idea of a 'walking' bool is horrid
	setPose(g->getPoseString());
}

void SkeletalCreature::setGaitGene(unsigned int gaitdrive) { // TODO: not sure if this is *useful*
	std::map<unsigned int, creatureGaitGene*>::iterator i = gaitgenes.find(gaitdrive);
	if (i == gaitgenes.end())
		return; // TODO: is there a better behaviour here?

	creatureGaitGene* g = i->second;
	assert(g->drive == gaitdrive);

	if (g == gaitgene)
		return;

	// reset our gait details to default
	gaitgene = g;
	gaiti = 0;
	skeleton->animation.clear();
	// developer aid (OPENC2E_WATCH_CREATURES): each change of walk cycle,
	// and whether it has any steps (a first pose of 0 means none)
	static const bool watchgait = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
	if (watchgait) {
		std::string poses;
		for (int k = 0; k < 8; k++)
			poses += fmt::format(" {}", (int)g->pose[k]);
		fmt::print("* WATCH GAIT t{} unid {} gait {} poses{}{}\n", world.tickcount, unid, gaitdrive, poses, g->pose[0] == 0 ? " (no steps: it can't walk)" : "");
	}
}

void SkeletalCreature::tick() {
	{
		PROFILE_SCOPE("creature: agent tick (physics, scripts)");
		Agent::tick();
	}

	if (paused)
		return;

	{
		PROFILE_SCOPE("creature: creature tick (mind, chemistry)");
		CreatureAgent::tick();
	}
	PROFILE_SCOPE("creature: body (blink, walk, pose)");

	eyesclosed = creature->isAsleep() || !creature->isAlive();

	// blink every 3-8 seconds (C2 ticks 10 times a second), eyes shut for
	// about a fifth of a second (the head picture is chosen when drawing)
	if (!eyesclosed) {
		if (blinkticks > 0)
			blinkticks--;
		else if (--blinkwait <= 0) {
			blinkticks = 2;
			blinkwait = rand_int32(30, 80);
		}
	}

	// C2: a creature walking (WALK) goes the way it faces. The world's
	// walk scripts face it first (POSE 59 east, 60 west), and the walk's
	// poses face "towards IT", which reads its direction; that was set
	// only by approaching something (or DIRN) and not saved, so after a
	// load it was north, every walk pose became the facing-out pose and
	// the creature walked in place (6KBY, starving and drowning in the
	// west sea airlock's pool, 2026-11-17), and later each walk went the
	// way of whatever it had last approached, not the way chosen. 153 of
	// Home's 155 creatures walk by gait (HomeostatMind's gaitWalk).
	if (engine.version == 2 && walking && !approaching && !eyesclosed && !creature->isZombie()) {
		if (posedirection == 0)
			direction = 2; // east
		else if (posedirection == 1)
			direction = 3; // west
	}

	// TODO: every 2 ticks = correct? what about the engine var?
	ticks++;
	if (ticks % 2 == 0) {
		// C2: a walk's steps may come on these ticks too, so that at a pace
		// other than a step every two ticks they come as evenly as ticks
		// allow (at 0.8 of it every 2 or 3, not 2, 2, 2 and then 4: drawn
		// moving smoothly, a walk paused at each long one; a player,
		// 2026-11-22). Same pace on average.
		if (engine.version == 2 && walking && !approaching && !eyesclosed && !creature->isZombie() && walkSteps(0.5f)) {
			if (!falling && !carriedby && !invehicle)
				snapDownFoot();
			else
				downfootroom.reset();
		}
		glideSettle();
		return;
	}

	//if (eyesclosed) return; // TODO: hack, this is wrong :)

	// TODO: hack!
	if (!eyesclosed && !creature->isZombie()) {
		bool edgeStop = false;
		if (approaching && approachtarget) {
			// TODO: more sane approaching skillz
			// C2: by their middles. A creature reaching for something over its
			// head or under its feet turned to and fro past it at every step
			// (2026-11-22): after it has turned back twice it stands there
			// (for what moves it may have to turn, so a while later again).
			const float past = approachtarget->x + approachtarget->getWidth() / 2.0f - (x + getWidth() / 2.0f);
			const unsigned int way = (engine.version == 2 ? past < 0 : approachtarget->x < x) ? 3 : 2;
			if (engine.version == 2) {
				if (approachtarget->getUNID() != approachUnid || world.tickcount - approachTurnedAt > 50)
					approachTurns = 0;
				approachUnid = approachtarget->getUNID();
				if (approachTurns >= 2 && std::fabs(past) <= 30.0f)
					edgeStop = true;
				else if (way != direction) {
					approachTurns++;
					approachTurnedAt = world.tickcount;
				}
			}
			if (!edgeStop)
				direction = way;
			// C2: walking up to an object, a creature at its floor's edge
			// stops there when the next step would end in water it can't
			// breathe in (it walked off the east dock's end reaching for an
			// object there, and sank; 2026-11-12). It touches what it can
			// from the edge.
			if (engine.version == 2 && downfootroom && downfootroom->type != 2 && !(dynamic_cast<oldCreature*>(creature) && dynamic_cast<oldCreature*>(creature)->breathesWater())) {
				bool east = direction == 2;
				float fx = footX();
				float edge = east ? (float)downfootroom->x_right : (float)downfootroom->x_left;
				// within a stride of the edge (the feet take turns, and the
				// front one lands a stride on: measured from the foot on the
				// floor, 14 px let it step straight past the edge)
				float toEdge = east ? edge - fx : fx - edge;
				if (toEdge < 40.0f && toEdge > -20.0f && walkOffEndsInWater(*downfootroom, east, std::max(1, size)))
					edgeStop = true;
			}
		}

		// developer aid (OPENC2E_WATCH_CREATURES): what a walk ran into,
		// every 20 ticks while it is asked to walk
		static const bool watchwalk = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
		if (watchwalk && (walking || approaching) && world.tickcount % 20 == 0)
			fmt::print("* WATCH WALK t{} unid {} walking {} approaching {} edgeStop {} falling {} at {:.0f},{:.0f}\n", world.tickcount, unid,
				walking, approaching, edgeStop, (bool)falling, x, y);
		if ((walking || approaching) && !edgeStop) {
			if (engine.version == 2) {
				walkSteps(walking && !approaching ? 0.5f : 1.0f);
			} else {
				// TODO: we shouldn't bother with this unless it changed?
				setGaitGene(creature->getGait());
				gaitTick();
			}
		}

		approaching = false;
	}

	// TODO: this kinda duplicates what physicsTick is doing below
	if (shoveticks > 0) {
		if (!falling && !carriedby && !invehicle) {
			float step = 2.0f + shoveticks * 1.2f; // 14 px, easing to 3
			// not through a wall: a shove that would carry the feet into a
			// room it could not walk into ends there (norns pushing a norn
			// beside the wall of a sealed shaft west of the music room, x
			// 1546, shoved it into the shaft; 2026-11-13)
			bool walled = false;
			if (engine.version == 2 && world.map) {
				float cx = x + getWidth() / 2.0f, feet = y + getHeight();
				std::shared_ptr<Room> r = world.map->roomAt(cx, feet - 1);
				std::shared_ptr<Room> to = world.map->roomAt(cx + shovedir * step, feet - 1);
				if (r && to != r && (!to || !world.map->hasDoor(r, to) || world.map->getDoorPerm(r, to) < size))
					walled = true;
			}
			if (walled)
				shoveticks = 1;
			else
				moveTo(x + shovedir * step, y);
		}
		shoveticks--;
	}

	if ((engine.version == 1 || !falling) && !carriedby && !invehicle)
		snapDownFoot();
	else
		downfootroom.reset();
	glideSettle();
}

void SkeletalCreature::physicsTick() {
	// TODO: mmh

	if (carriedby || invehicle)
		lastHeldTick = world.tickcount;
	if (engine.version == 2 && falling && !fallTracking) {
		fallTracking = true;
		fallTopY = y;
		lastFall.fromX = x + getWidth() / 2.0f;
		lastFall.fromY = y + getHeight();
		fallOwn = engine.worldready && !carriedby && !invehicle && world.tickcount > lastHeldTick + 5 && world.tickcount > lastScriptMoveTick + 5;
	}
	if (engine.version > 1 && falling) {
		float fx0 = x, fy0 = y;
		// (where its foot was before this tick's move: see below)
		float footx0 = 0, footy0 = 0;
		if (engine.version == 2 && world.map) {
			footx0 = footX();
			footy0 = footY();
		}
		if (validInRoomSystem() || engine.version == 2) {
			Agent::physicsTick();
		} else if (!validInRoomSystem()) {
			falling = false;
		}
		// C2: a creature knocked sideways (another creature's hit sends it off
		// at 50 px a tick) keeps out of rooms its feet cannot walk into.
		// Physics checks only the box's corners, and a creature's box hangs
		// past its foot: in the crowd at the foot of the tree-trunk lift's
		// shaft (room 80, x 1546-1670) the boxes reached into the rock beside
		// it (room 103, sealed), so a hit slid creatures into the rock, and
		// the sealed-pocket rescue moved them back, over and over (29 times
		// in 5 minutes of a test run, 2026-11-15). If the foot crossed into a
		// room the one it stood in has no doorway to (open at its size), the
		// sideways part of the move is undone.
		if (engine.version == 2 && world.map && x != fx0 && !carriedby && !invehicle) {
			std::shared_ptr<Room> r0 = world.map->roomAt(footx0, footy0 - 2);
			std::shared_ptr<Room> r1 = world.map->roomAt(footX(), footY() - 2);
			if (r0 && r1 && r0 != r1 && (!world.map->hasDoor(r0, r1) || world.map->getDoorPerm(r0, r1) < size)) {
				// (unless it is falling straight down through an open floor,
				// with the sideways part only a pixel or two)
				std::shared_ptr<Room> below = world.map->roomAt(footx0, footY() - 2);
				if (!(below == r1 && std::fabs(x - fx0) < 3)) {
					moveTo(fx0, y);
					velx = 0;
					if (devenv("OPENC2E_WATCH_CREATURES"))
						fmt::print("* WATCH SHOVE t{} unid {}: kept out of room {} (from room {})\n", world.tickcount, unid, r1->id, r0->id);
				}
			}
		}
		// C2: physics checks an object's four corners against the rooms, so a
		// falling creature with a corner in rock that belongs to no room
		// cannot move at all, and hangs in the air (a frog norn stepped off
		// the upper-east deck's end with its left side over the rock under
		// the deck, x 7670-7745, and hung there; owner, 2026-11-12). After a
		// few ticks without moving, ease it away from the side in rock.
		if (engine.version == 2 && falling && !carriedby && !invehicle && x == fx0 && y == fy0) {
			if (++fallStuck >= 4) {
				auto inRoom = [](float px, float py) { return (bool)world.map->roomAt(px, py); };
				float w = getWidth(), h = getHeight();
				bool leftOut = !inRoom(x + 1, y + 1) || !inRoom(x + 1, y + h - 1);
				bool rightOut = !inRoom(x + w - 1, y + 1) || !inRoom(x + w - 1, y + h - 1);
				if (leftOut != rightOut) {
					moveTo(x + (leftOut ? 3 : -3), y);
					if (devenv("OPENC2E_WATCH_CREATURES"))
						fmt::print("* WATCH UNSTICK t{} unid {} at ({:.0f}, {:.0f}): eased {}\n", world.tickcount, unid, x, y, leftOut ? "right" : "left");
				}
			}
		} else
			fallStuck = 0;
	}
	// developer aid (OPENC2E_WATCH_FALLS): each tick of a creature's fall,
	// with whether it is walking, to see what moves it sideways
	static const bool watchfalls = devenv("OPENC2E_WATCH_FALLS") != nullptr;
	if (watchfalls && falling && fallTracking)
		fmt::print("* FALLTICK t{} unid {} at {:.0f},{:.0f} walking {} approaching {} velx {:.1f}\n", world.tickcount, unid, x + getWidth() / 2.0f, y + getHeight(),
			walking, approaching, velx);
	// C2: a creature sliding along a floor slows quickly. A hit from another
	// creature (the world's creature-on-creature script) knocks it back at
	// 50 px a tick, and with only air friction (a tenth a tick) it slid
	// about 450 px, off ledges: a norn on the volcano's east ledge was hit
	// twice and thrown into the volcano (2026-11-13). With its feet on the
	// floor and not moving up or down, sideways speed now halves each tick.
	if (engine.version == 2 && falling && !carriedby && !invehicle && std::fabs(velx) > 0 && std::fabs(vely) <= 3 && world.map) {
		float cx = x + getWidth() / 2.0f, feet = y + getHeight();
		std::shared_ptr<Room> r = world.map->roomAt(cx, feet - 1);
		if (r && std::fabs(r->floorYatX(cx) - feet) <= 4.0f) {
			velx = std::fabs(velx) < 2.0f ? 0.0f : velx * 0.5f;
		}
	}
	if (fallTracking && !falling) {
		fallTracking = false;
		if (fallOwn && !carriedby && !invehicle)
			landed(y - fallTopY);
	}

	if ((engine.version == 1 || !falling) && !carriedby && !invehicle)
		snapDownFoot();
	else
		downfootroom.reset();

	// developer aid: OPENC2E_WATCH_CREATURES logs any standing creature whose
	// foot is below the floor of its foot room, and its position every 200 ticks
	static const bool watch = devenv("OPENC2E_WATCH_CREATURES") != nullptr;
	if (watch && !carriedby && !invehicle) {
		int fp = downfoot_left ? 11 : 12;
		float footx = x + attachmentX(fp, 1), footy = y + attachmentY(fp, 1);
		if (!falling && downfootroom && downfootroom->floorvalue == 0) {
			float floory = downfootroom->floorYatX(footx);
			if (footy > floory + 3)
				fmt::print("* WATCH SINK t{} unid {} genus {} foot ({:.0f}, {:.0f}) room {} floor {:.0f} depth {:.0f}\n",
					world.tickcount, unid, (int)genus, footx, footy, downfootroom->id, floory, footy - floory);
		}
		static std::map<int, int> lastroom;
		int nowroom = downfootroom ? (int)downfootroom->id : -1;
		auto lr = lastroom.find(unid);
		if (lr != lastroom.end() && lr->second != nowroom)
			fmt::print("* WATCH ROOM t{} unid {} {} -> {} foot ({:.0f}, {:.0f}) falling {} floor {:.0f} script {} {}\n", world.tickcount, unid,
				lr->second, nowroom, footx, footy, (bool)falling, downfootroom ? downfootroom->floorYatX(footx) : -1.0f,
				vm && vm->currentscript ? fmt::format("{} {} {} {}", vm->currentscript->fmly, vm->currentscript->gnus, vm->currentscript->spcs, vm->currentscript->scrp) : std::string("none"),
				walking ? "walking" : "");
		lastroom[unid] = nowroom;
		static std::map<int, float> lastfooty;
		auto it = lastfooty.find(unid);
		if (it != lastfooty.end() && !falling && it->second - footy > 20)
			fmt::print("* WATCH JUMPUP t{} unid {} foot ({:.0f}, {:.0f}) up {:.0f} room {}\n", world.tickcount, unid, footx, footy,
				it->second - footy, downfootroom ? (int)downfootroom->id : -1);
		lastfooty[unid] = footy;
		if (world.tickcount % 200 == 0)
			fmt::print("* WATCH POS t{} unid {} genus {} foot ({:.0f}, {:.0f}) room {} falling {}\n",
				world.tickcount, unid, (int)genus, footx, footy, downfootroom ? (int)downfootroom->id : -1, (bool)falling);
	}
}

// A life stage's walking pace, as a share of the usual (0 baby ... 6 senile).
float SkeletalCreature::stagePace(int stage) {
	static const float pace[7] = {0.6f, 0.85f, 1.0f, 1.1f, 1.0f, 0.75f, 0.55f};
	return (stage >= 0 && stage < 7) ? pace[stage] : 1.0f;
}

// Old creatures tire sooner (owner, 2026-11-10: vigor with age): each step
// of a walk adds a little Tiredness (C2 chemical 6, which a creature's genes
// turn into the Tiredness drive and, past 174, the tired walk) with a chance
// that grows with age: none until old, 0.15 when old, 0.35 when senile.
void SkeletalCreature::stepTiredness() {
	static const float chance[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.15f, 0.35f};
	int stage = (int)creature->getStage();
	if (stage < 0 || stage > 6 || chance[stage] <= 0.0f)
		return;
	if (auto* oc = dynamic_cast<oldCreature*>(creature))
		if (rand_float(0, 1) < chance[stage])
			oc->addChemical(6, 1);
}

// C2: a walking pace by life stage (owner, 2026-11-10: vigor with age): the
// walk's steps come at stagePace() of the usual rate (a step every other
// tick), slow in infancy and old age, briskest as a youth, and by
// temperament: energetic creatures walk a little faster. `share` is how much
// of two ticks this call stands for. Says whether it took a step.
bool SkeletalCreature::walkSteps(float share) {
	// TODO: we shouldn't bother with this unless it changed?
	setGaitGene(creature->getGait());
	// TODO: we should only do this if we're moving :-P
	float energy = 1.0f;
	if (auto* oc = dynamic_cast<oldCreature*>(creature))
		energy = oc->temperament.factor(ENERGY, 0.12f);
	const float pace = stagePace((int)creature->getStage()) * energy * share;
	bool stepped = false;
	paceCredit += pace;
	while (paceCredit >= 1.0f) {
		paceCredit -= 1.0f;
		// (how many ticks until the next: it is drawn taking that long)
		nextStepIn = pace > 0.01f ? std::max(1.0f, std::ceil((1.0f - paceCredit) / pace)) * 2.0f * share : 2.0f;
		gaitTick();
		stepTiredness();
		stepped = true;
	}
	nextStepIn = 0;
	return stepped;
}

void SkeletalCreature::gaitTick() {
	if (!gaitgene)
		return;
	uint8_t pose = gaitgene->pose[gaiti];
	if (pose == 0) {
		if (gaiti == 0)
			return; // non-worky gait

		gaiti = 0;
		gaitTick();
		return;
	}

	std::map<unsigned int, creaturePoseGene*>::iterator i = posegenes.find(pose);
	if (i != posegenes.end()) {
		creaturePoseGene* poseg = i->second;
		assert(poseg->poseno == pose);
		std::string p = poseg->getPoseString();
		// C2: a walk goes the way the creature faces. The game's norns have
		// one walk (gait 13) whose four poses are all written to face east:
		// told to walk west, a norn with it faced west for an instant, turned
		// round and walked east (2026-11-22).
		if (engine.version == 2 && (p[0] == '2' || p[0] == '3'))
			p[0] = '?';
		setPose(p);
	}

	gaiti++;
	if (gaiti > 7)
		gaiti = 0;
}

CompoundPart* SkeletalCreature::part(unsigned int part_id) {
	// A C1/C2 creature is a single part; scripts that set PART for their own
	// object and then target a creature (C2's boats fling passengers ashore
	// with POSE 128 after PART 3) mean the creature itself. Throwing here
	// stopped the boat's script mid-trip with the boat still moving, so it
	// sailed round the world for ever.
	if (engine.version < 3)
		return skeleton;
	THROW_IFNOT(part_id == 0);
	return skeleton;
}

void SkeletalCreature::setZOrder(unsigned int plane) {
	Agent::setZOrder(plane);
	skeleton->zapZOrder();
	skeleton->addZOrder();
}

SkeletonPart::SkeletonPart(SkeletalCreature* p)
	: AnimatablePart(p, 0, 0, 0, 0) {
}

void SkeletonPart::tick() {
	updateAnimation();
}

void SkeletonPart::setPose(unsigned int p) {
	((SkeletalCreature*)parent)->setPoseGene(p);
}

void SkeletonPart::setFrameNo(unsigned int p) {
	assert(p < animation.size());
	frameno = p;
}

void SkeletonPart::partRender(class RenderTarget* renderer, int xoffset, int yoffset) {
	SkeletalCreature* c = dynamic_cast<SkeletalCreature*>(parent);
	c->render(renderer, xoffset, yoffset);
}

void SkeletalCreature::finishInit() {
	Agent::finishInit();

	processGenes();
	skeletonInit();
	setPose(0);
}

void SkeletalCreature::creatureAged() {
	// TODO: adjust position to account for any changes..

	processGenes();
	skeletonInit();
	recalculateSkeleton();
}

std::string SkeletalCreature::getFaceSpriteName() {
	// TODO: we should store the face sprite when we first search for sprites (since it
	// has to be the baby sprite), rather than this horrible hackery
	for (auto& gene : creature->getGenome()->genes) {
		//if ((*i)->header.switchontime != creature->getStage()) continue;

		if (creatureAppearanceGene* x = dynamic_cast<creatureAppearanceGene*>(gene.get())) {
			if (x->part == 0) {
				return std::string("a") + dataString(0, creature->isFemale(), engine.version > 1 ? geneBreedSpecies(x->species) : x->species,
											engine.version > 1 ? geneBreedSlot(x->species, x->variant) : x->variant);
			}
		}
	}

	THROW_IFNOT(false); // TODO: mmh
}

unsigned int SkeletalCreature::getFaceSpriteFrame() {
	return 9 + (eyesclosed ? 16 : 0) + (facialexpression * 32);
}

// the body part whose picture is drawn at a world point (the frontmost, as
// drawn), or -1 where the creature is see-through
int SkeletalCreature::partUnder(float wx, float wy) {
	for (int j = 16; j >= 0; j--) {
		int i = cee_zorder[posedirection][j];
		if (i >= SkeletalPartCount() || !images[i])
			continue;
		if (engine.version == 1 && (i == 6 || i == 13))
			continue;
		unsigned int frame = pose[i];
		if (cee_bodyparts[i].parent == -1)
			frame += pregnancy * (engine.version < 3 ? 10 : 16);
		else if (i == 1)
			frame += ((eyesclosed || blinkticks > 0) ? (engine.version < 3 ? 10 : 16) : 0) + facialexpression * (engine.version < 3 ? 20 : 32);
		if (frame >= images[i]->numframes())
			continue;
		int px = (int)std::floor(wx - (x + partx[i] + adjustx)), py = (int)std::floor(wy - (y + party[i] + adjusty));
		if (px < 0 || py < 0 || px >= (int)images[i]->width(frame) || py >= (int)images[i]->height(frame))
			continue;
		if (!images[i]->transparentAt(frame, px, py))
			return i;
	}
	return -1;
}

// the body or a leg (thigh, shin, foot): where the hand spanks and pushes
bool SkeletalCreature::bodyOrLeg(int p) {
	return p == 0 || p == 2 || p == 3 || p == 7 || p == 8 || p == 11 || p == 12;
}

// What a click of the hand does, by where it is on the creature (owner,
// 2026-11-15): over the head's picture (and ears and hair) it pets
// (tickle, the angel); on the body or a leg near the hips (the bottom
// half) it spanks (slap, the devil); elsewhere nothing
// (the click still selects the creature). Was: the top half of the
// creature's box tickled and the bottom half slapped. Pick up (right click)
// works anywhere on the creature; the push hand pushes on the body and legs.
int SkeletalCreature::handleClick(float clickx, float clicky) {
	if (engine.version <= 2 && images[0]) {
		int p = partUnder(clickx, clicky);
		if (p == 1 || p == 14 || p == 15 || p == 16)
			return 1; // pet
		// spank: on the body or a leg, within a circle round the hips (the
		// middle of the two hip joints) half the body picture's height
		// across, so the bottom half (owner, 2026-11-15: first a few pixels
		// round the hips, then the whole body and legs, then this)
		if (bodyOrLeg(p) && att[0].nolines > pose[0] && att[0].noattachments[pose[0]] >= 6) {
			float hx = x + (attachmentX(0, 1) + attachmentX(0, 2)) / 2.0f, hy = y + (attachmentY(0, 1) + attachmentY(0, 2)) / 2.0f;
			unsigned int bf = pose[0] + pregnancy * (engine.version < 3 ? 10 : 16);
			float r = (bf < images[0]->numframes() ? images[0]->height(bf) : 30) / 2.0f;
			if ((clickx - hx) * (clickx - hx) + (clicky - hy) * (clicky - hy) <= r * r)
				return 0; // spank
		}
		return -1;
	}
	// (no pictures yet: the old rule)
	return clicky - y >= getSkelHeight() / 2.0 ? 0 : 1;
}

// C2: a creature carries in the hand on the viewer's side, drawn in front
// of its body: facing right its right forearm (part 10) is drawn over the
// body and its left (9) behind, facing left the other way round. Before,
// everything was held in the left hand and drawn behind the body, so a toy
// gun jumped from the creature's hands to behind it as it turned (owner,
// 2026-11-10). Facing the viewer (posedirection 2) the hands are in front of
// the body too, so what it holds is in front; only facing away (3) is it
// behind (owner, 2026-11-10: the gun still flicked behind an ettin turning
// to face the viewer).
bool SkeletalCreature::carriesInFront() const {
	return engine.version == 2 && posedirection != 3;
}

std::pair<int, int> SkeletalCreature::getCarryPoint() {
	std::pair<int, int> carrypoint;

	int hand = (engine.version == 2 && posedirection == 0) ? 10 : 9;
	carrypoint.first = attachmentX(hand, 1);
	carrypoint.second = attachmentY(hand, 1);

	return carrypoint;
}

/* vim: set noet: */

// A creature landing from a fall of its own: up to 100 px is a step down and
// harmless; beyond that it hurts (Pain, chemical 1) more the further it
// fell, and past 300 px it is injured too (Injury, chemical 14). Landing in
// water does not hurt. The mind is told (lastFall), to learn which drops to
// keep away from (owner, 2026-11-13).
void SkeletalCreature::landed(float height) {
	if (height <= 0 || !world.map)
		return;
	std::shared_ptr<Room> r = world.map->roomAt(x + getWidth() / 2.0f, y + getHeight() - 2);
	bool water = r && r->type == 2;
	float pain = water ? 0.0f : std::clamp((height - 100.0f) * 0.6f, 0.0f, 255.0f);
	float injury = water ? 0.0f : std::clamp((height - 300.0f) * 0.4f, 0.0f, 255.0f);
	auto* oc = dynamic_cast<oldCreature*>(creature);
	if (oc && pain > 0)
		oc->addChemical(1, (unsigned char)pain);
	if (oc && injury > 0)
		oc->addChemical(14, (unsigned char)injury);
	lastFall.height = height;
	lastFall.hurt = (pain + injury) / 255.0f;
	lastFall.tick = world.tickcount;
	if (pain > 0 || devenv("OPENC2E_WATCH_FALLS"))
		fmt::print("* {} fell {:.0f} px from {:.0f},{:.0f} to {:.0f},{:.0f}{}\n", world.history ? world.history->findMoniker(creature->getGenome()) : std::string("?"), height,
			lastFall.fromX, lastFall.fromY, x + getWidth() / 2.0f, y + getHeight(), pain > 0 ? fmt::format(": hurt (pain {:.0f}, injury {:.0f})", pain, injury) : std::string(water ? " into water" : ""));
}

void SkeletalCreature::scriptMoved() {
	downfootroom.reset();
	lastScriptMoveTick = world.tickcount;
}

std::string SkeletalCreature::describeParts() {
	std::string s = fmt::format("stage {} {}", (int)creature->getStage(), creature->isFemale() ? "female" : "male");
	for (int i = 0; i < 17; i++)
		if (images[i])
			s += fmt::format(" {}:{}f{}k{:.2f}", i, images[i]->getName(), pose[i], partk[i]);
	return s;
}
