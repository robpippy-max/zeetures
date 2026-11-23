/*
 *  ImportCreature.cpp
 *  openc2e
 *
 *  File > Import Creature: a C2 exported creature (.exp) comes into the
 *  world as a living creature (owner, 2026-11-10). openc2e does not read
 *  C2's whole saved creature (its body, chemistry and C2 brain); what is
 *  taken from the export is what makes it that creature: its genome, name,
 *  sex and life stage. It is made anew from those (its chemistry at its
 *  genes' starting levels, its mind fresh) and handed to the hand to place.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 */

#include "ImportCreature.h"
#include "common/devenv.h"

#include "Engine.h"
#include "PointerAgent.h"
#include "World.h"
#include "common/creaturesImage.h"
#include "common/io/SpanReader.h"
#include "creatures/BreedSlots.h"
#include "creatures/Creature.h"
#include "creatures/oldCreature.h"
#include "creatures/SkeletalCreature.h"
#include "fileformats/genomeFile.h"
#include "historyManager.h"
#include "imageManager.h"

#include <SDL3/SDL_dialog.h>
#include <algorithm>
#include <cstring>
#include <fmt/core.h>
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <mutex>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> readAll(const fs::path& p) {
	std::ifstream in(p, std::ios::binary);
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

size_t find(const std::vector<uint8_t>& d, const std::string& s, size_t from = 0) {
	auto it = std::search(d.begin() + std::min(from, d.size()), d.end(), s.begin(), s.end());
	return it == d.end() ? std::string::npos : (size_t)(it - d.begin());
}

size_t rfind(const std::vector<uint8_t>& d, const std::string& s, size_t before) {
	if (s.size() > before)
		return std::string::npos;
	for (size_t i = before - s.size() + 1; i-- > 0;)
		if (std::memcmp(d.data() + i, s.data(), s.size()) == 0)
			return i;
	return std::string::npos;
}

int32_t i32(const std::vector<uint8_t>& d, size_t at) {
	if (at + 4 > d.size())
		return 0;
	int32_t v;
	std::memcpy(&v, d.data() + at, 4);
	return v;
}

// a length-prefixed string at "at" (one byte of length); false past the end
bool pstr(const std::vector<uint8_t>& d, size_t& at, std::string& out) {
	if (at >= d.size() || at + 1 + d[at] > d.size())
		return false;
	out.assign((const char*)d.data() + at + 1, d[at]);
	at += 1 + d[at];
	return true;
}

struct Exported {
	std::vector<uint8_t> genome; // a genome file ("dna2" and the genes)
	std::string moniker, name;
	int sex = 0; // 1 male, 2 female, 0 unknown
	std::vector<std::pair<int, int>> headFrames; // the first pictures of its gallery: its head
};

// What an export holds, as far as is used here. The layout was read from
// exports of four C2 breed packs (Midsummer, Scorpio): a "Creature" record
// whose picture gallery (CGallery) is named after its moniker and starts with
// its head's pictures (width and height of each); near the end its history
// record (moniker, name, parents, birth time and place, then numbers, the
// third of them its sex) and its genome (CGenome: the genes, ending "gend").
bool readExport(const std::vector<uint8_t>& d, Exported& e, std::string& err) {
	size_t g = find(d, "CGenome");
	if (g == std::string::npos) {
		err = "no genome in it (is it a Creatures 2 exported creature?)";
		return false;
	}
	size_t first = find(d, "gene", g), end = find(d, "gend", first);
	if (first == std::string::npos || end == std::string::npos) {
		err = "its genome is incomplete";
		return false;
	}
	e.genome = {'d', 'n', 'a', '2'};
	e.genome.insert(e.genome.end(), d.begin() + first, d.begin() + end + 4);

	// the gallery: picture count, its name (the moniker), then entries of
	// three bytes and a width, height and offset (32-bit each)
	size_t gal = find(d, "CGallery");
	if (gal != std::string::npos && gal + 8 + 8 + 8 < d.size()) {
		size_t p = gal + 8;
		e.moniker.assign((const char*)d.data() + p + 4, 4);
		p += 8 + 8;
		for (int k = 0; k < 6 && p + 15 <= d.size(); k++) {
			p += 3;
			e.headFrames.push_back({i32(d, p), i32(d, p + 4)});
			p += 12;
		}
	}
	// the history record: the moniker, then the name
	if (!e.moniker.empty()) {
		size_t h = rfind(d, e.moniker, g);
		size_t p = h == std::string::npos ? std::string::npos : h + 4;
		std::string date, place;
		if (p != std::string::npos && pstr(d, p, e.name)) {
			p += 10; // the parents' monikers
			if (pstr(d, p, date) && pstr(d, p, place)) {
				int sex = i32(d, p + 8);
				if (sex == 1 || sex == 2)
					e.sex = sex;
			}
		}
	}
	return true;
}

// The life stage it was exported at: the age whose head pictures (of its
// breed and sex, as the engine would find them) have the same sizes as the
// export's own; the youngest if several do, a baby if none
int stageOf(const Exported& e, std::shared_ptr<genomeFile> genome, bool female) {
	int species = -1, variant = -1;
	for (auto& gn : genome->genes)
		if (auto* a = dynamic_cast<creatureAppearanceGene*>(gn.get()))
			if (a->part == 0) {
				species = geneBreedSpecies(a->species);
				variant = geneBreedSlot(a->species, a->variant);
				break;
			}
	if (species < 0 || e.headFrames.empty())
		return 0;
	for (int age = 0; age <= 6; age++) {
		std::string name = "a";
		name += (char)('0' + species + (female ? 4 : 0));
		name += (char)('0' + age);
		name += breedSlotName(variant);
		auto img = world.gallery->getImage(name);
		if (!img)
			continue;
		bool same = img->numframes() >= e.headFrames.size();
		for (size_t k = 0; same && k < e.headFrames.size(); k++)
			same = (int)img->width(k) == e.headFrames[k].first && (int)img->height(k) == e.headFrames[k].second;
		if (same)
			return age;
	}
	return 0;
}

std::mutex s_mutex;
std::vector<std::string> s_picked;
std::string s_message;

void chosen(void*, const char* const* files, int) {
	if (!files)
		return;
	std::lock_guard<std::mutex> lock(s_mutex);
	for (auto f = files; *f; f++)
		s_picked.push_back(*f);
}

} // namespace

bool ImportExportedCreature(const fs::path& path, std::string& message) {
	if (engine.version != 2) {
		message = "Importing creatures works in Creatures 2 worlds.";
		return false;
	}
	Exported e;
	std::string err;
	if (!readExport(readAll(path), e, err)) {
		message = fmt::format("Couldn't import {}: {}", path.filename().string(), err);
		return false;
	}
	auto genome = std::make_shared<genomeFile>();
	try {
		SpanReader r(std::span<const uint8_t>(e.genome.data(), e.genome.size()));
		r >> *genome;
	} catch (std::exception& ex) {
		message = fmt::format("Couldn't import {}: its genome can't be read ({})", path.filename().string(), ex.what());
		return false;
	}
	bool female = e.sex ? e.sex == 2 : rand() % 2;
	int stage = stageOf(e, genome, female);

	SkeletalCreature* a = new SkeletalCreature(4);
	oldCreature* c;
	try {
		c = new c2Creature(genome, female, 0, a);
	} catch (std::exception& ex) {
		delete a;
		message = fmt::format("Couldn't import {}: {}", path.filename().string(), ex.what());
		return false;
	}
	a->setCreature(c);
	try {
		a->finishInit();
	} catch (std::exception& ex) {
		a->kill(); // (never left half made in the world: its next step crashes the game)
		message = fmt::format("Couldn't import {}: {}", path.filename().string(), ex.what());
		return false;
	}
	a->genome_slots[0] = genome;
	world.newMoniker(genome, "imported", a);
	std::string mon = world.history->findMoniker(genome);
	world.history->getMoniker(mon).moveToCreature(a);
	if (!e.name.empty())
		world.history->getMoniker(mon).name = e.name;
	for (int i = 0; i < stage; i++)
		c->ageCreature();
	c->born();
	world.hand()->addCarried(a);
	world.selectCreature(AgentRef(a).lock());

	static const char* stages[] = {"baby", "child", "adolescent", "youth", "adult", "old", "senile"};
	message = fmt::format("{} came in from {}: a {} {} (the hand holds it; drop it somewhere).",
		e.name.empty() ? "A creature" : e.name, path.filename().string(), female ? "female" : "male", stages[std::clamp(stage, 0, 6)]);
	fmt::print("* Imported creature: {}\n", message);
	return true;
}

void ImportCreatureMenuItem() {
	if (ImGui::MenuItem("Import Creature...")) {
		static const SDL_DialogFileFilter filters[] = {{"Exported creatures (exp)", "exp"}, {"All files", "*"}};
		SDL_ShowOpenFileDialog(chosen, nullptr, nullptr, filters, 2, nullptr, false);
	}
}

void ImportCreatureTick() {
	// developer aid: OPENC2E_TEST_IMPORT_CREATURE="path.exp" imports it once
	// the world is running
	static bool tested = false;
	if (const char* t = devenv("OPENC2E_TEST_IMPORT_CREATURE"); t && !tested && world.hand()) {
		tested = true;
		std::lock_guard<std::mutex> lock(s_mutex);
		s_picked.push_back(t);
	}
	std::vector<std::string> picked;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		picked.swap(s_picked);
	}
	for (auto& f : picked) {
		std::string msg;
		ImportExportedCreature(f, msg);
		s_message = msg;
		ImGui::OpenPopup("Import Creature");
	}
	if (ImGui::BeginPopupModal("Import Creature", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(s_message.c_str());
		if (ImGui::Button("OK"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}
