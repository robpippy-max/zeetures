/*
 *  Engine.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Tue Nov 28 2006.
 *  Copyright (c) 2006-2008 Alyssa Milburn. All rights reserved.
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
#include "creatures/SkeletalCreature.h"
#include <chrono>
#include "Engine.h"
#include "creatures/GenomeTraits.h"
#include <ctime>

#include "Additions.h"
#include "GameFolder.h"
#include "AgentHelpers.h"
#include "madeart/MadeArt.h"

#include <SDL3/SDL_events.h>

#include "Blackboard.h"
#include "creatures/Pheromones.h"
#include "creatures/Disease.h"
#include "creatures/Exposure.h"
#include "historyManager.h"
#include "creatures/Breeding.h"

#include <climits>
#include "creatures/CreatureAgent.h"
#include "creatures/oldCreature.h"
#include "creatures/Creature.h"
#include "CobManager.h"

#include "Bubble.h"
#include "Camera.h"
#include "CompoundAgent.h"
#include "DullPart.h"
#include "Map.h"
#include "MetaRoom.h"
#include "MusicManager.h"
#include "NetBackend.h"
#include "PathResolver.h"
#include "PointerAgent.h"
#include "Room.h"
#include "SFCFile.h"
#include "SoundManager.h"
#include "SpritePart.h"
#include "World.h"
#include "caosScript.h" // for executeNetwork()
#include "caosVM.h"
#include "common/Ranges.h"
#include "common/audio/NullAudioBackend.h"
#include "common/backend/Keycodes.h"
#include "common/backend/NullBackend.h"
#include "common/case_insensitive_filesystem.h"
#include "common/encoding.h"
#include "common/io/FileWriter.h"
#include "common/io/StringWriter.h"
#include "common/io/WriterFmt.h"
#include "common/userlocale.h"
#include "fileformats/cfgFile.h"
#include "fileformats/peFile.h"
#include "imageManager.h"
#include "openc2eimgui/ImGuiUtils.h"
#include <imgui.h>
#include <imgui_internal.h>
#include "openc2eimgui/Openc2eImGui.h"
#include "openc2eimgui/C2Kits.h"
#include "Scriptorium.h"
#include "WorldSave.h"
#include "creatures/CreatureSpeech.h"
#include "openc2eimgui/WorldChooser.h"
#include "creatures/CreatureMind.h"
#include "prayManager.h"

#ifdef _WIN32
#include "common/ComPtr.h"
#endif

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#define CXXOPTS_VECTOR_DELIMITER '\0'
#include <cxxopts.hpp>
#include <cstring>
#include <fmt/core.h>
#include <memory>
#include <cstdio>
#include <tuple>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#else
#include <pwd.h> // getpwuid
#include <sys/types.h> // passwd*
#endif

namespace fs = std::filesystem;

Engine engine;

Engine::Engine() {
	done = false;
	dorendering = true;
	fastticks = false;
	refreshdisplay = false;

	bmprenderer = false;

	std::vector<std::string> languages = get_preferred_languages();
	if (!languages.empty()) {
		language = languages.front();
	} else {
		language = "en";
	}

	tickdata = 0;
	for (unsigned int& ticktime : ticktimes)
		ticktime = 0;
	ticktimeptr = 0;
	version = 0; // TODO: something something

	cmdline_enable_sound = true;
	cmdline_norun = false;

	exefile = 0;

	addPossibleBackend("null", NullBackend::get_instance());
	addPossibleAudioBackend("null", NullAudioBackend::get_instance());

	camera.reset(new MainCamera);
}

Engine::~Engine() {
}

void Engine::addPossibleBackend(std::string s, Backend* b) {
	assert(!get_backend());
	assert(b);
	preferred_backend = s;
	possible_backends[s] = b;
}

void Engine::addPossibleAudioBackend(std::string s, AudioBackend* b) {
	assert(b);
	preferred_audiobackend = s;
	possible_audiobackends[s] = b;
}

void Engine::setBackend(Backend* b) {
	set_backend(b);
	lasttimestamp = get_ticks_msec();
}

static std::vector<std::string> read_wordlist(peFile* exefile, PeLanguage lang) {
	std::optional<resourceInfo> r = exefile->findResource(PE_RESOURCETYPE_STRING, lang, 14);
	if (!r) {
		fmt::print("Warning: Couldn't load word list (couldn't find resource)!\n");
		return {};
	}

	std::vector<std::string> strings = exefile->getResourceStrings(*r);
	if (strings.size() < 6) {
		fmt::print("Warning: Couldn't load word list (string table too small)!\n");
		return {};
	}

	std::string wordlistdata = utf8_to_cp1252(strings[5]);
	std::vector<std::string> wordlist;
	std::string s;
	for (char i : wordlistdata) {
		if (i == '|') {
			wordlist.push_back(s);
			s.clear();
		} else
			s += i;
	}

	return wordlist;
}

void Engine::loadGameData() {
	// load word list translations for C1
	// C1 does not keep translations for all languages, so we embed them
	if (gametype == "c1") {
		if (language == "de") {
			wordlist_translations = std::map<std::string, std::string>{
				{"come", "komm"},
				{"drop", "lass"},
				{"get", "hol"},
				{"left", "links"},
				{"look", "guck"},
				{"no", "nein"},
				{"pull", "zieh"},
				{"push", "druck"},
				{"right", "rechts"},
				{"run", "lauf"},
				{"sleep", "raste"},
				{"stop", "halt"},
				{"what", "was"},
				{"yes", "ja"}};
		} else if (language == "fr") {
			wordlist_translations = std::map<std::string, std::string>{
				{"come", "venir"},
				{"drop", "lacher"},
				{"get", "prendre"},
				{"left", "gauche"},
				{"look", "regarder"},
				{"no", "non"},
				{"pull", "tirer"},
				{"push", "pousser"},
				{"right", "droite"},
				{"run", "courir"},
				{"sleep", "se-reposer"},
				{"stop", "arreter"},
				{"what", "quoi"},
				{"yes", "oui"}};
		} else if (language == "it") {
			wordlist_translations = std::map<std::string, std::string>{
				{"come", "vieni"},
				{"drop", "molla"},
				{"get", "prendi"},
				{"left", "sinistra"},
				{"look", "guarda"},
				{"no", "no"},
				{"pull", "tira"},
				{"push", "premi"},
				{"right", "destra"},
				{"run", "corri"},
				{"sleep", "riposa"},
				{"stop", "stop"},
				{"what", "cosa"},
				{"yes", "si"}};
		} else if (language == "nl") {
			wordlist_translations = std::map<std::string, std::string>{
				{"come", "komen"},
				{"drop", "latenvallen"},
				{"get", "pakken"},
				{"left", "links"},
				{"look", "kijken"},
				{"no", "nee"},
				{"pull", "trekken"},
				{"push", "duwen"},
				{"right", "rechts"},
				{"run", "rennen"},
				{"sleep", "rusten"},
				{"stop", "stoppen"},
				{"what", "wat"},
				{"yes", "ja"}};
		} else if (language == "es") {
			wordlist_translations = std::map<std::string, std::string>{
				{"come", "venir"},
				{"drop", "soltar"},
				{"get", "coger"},
				{"left", "izquierda"},
				{"look", "mirar"},
				{"no", "no"},
				{"pull", "tirar"},
				{"push", "empujar"},
				{"right", "derecha"},
				{"run", "correr"},
				{"sleep", "descansar"},
				{"stop", "parar"},
				{"what", "que"},
				{"yes", "si"}};
		}
	}

	// load word list for C2
	if (gametype == "c2") {
		fs::path exepath(findMainDirectoryFile("Creatures2.exe"));
		if (fs::exists(exepath) && !fs::is_directory(exepath)) {
			try {
				exefile = new peFile(exepath);
			} catch (Exception& e) {
				fmt::print("Warning: Couldn't load word list ({})!\n", e.what());
			}
		} else
			fmt::print("Warning: Couldn't load word list (couldn't find Creatures2.exe)!\n");

		if (exefile) {
			auto english_wordlist = read_wordlist(exefile, PE_LANGUAGE_ENGLISH);

			wordlist.clear();
			wordlist_translations.clear();
			if (language == "de") {
				wordlist = read_wordlist(exefile, PE_LANGUAGE_GERMAN);
			} else if (language == "fr") {
				wordlist = read_wordlist(exefile, PE_LANGUAGE_FRENCH);
			} else if (language == "it") {
				wordlist = read_wordlist(exefile, PE_LANGUAGE_ITALIAN);
			} else if (language == "nl") {
				wordlist = read_wordlist(exefile, PE_LANGUAGE_DUTCH);
			} else if (language == "es") {
				wordlist = read_wordlist(exefile, PE_LANGUAGE_SPANISH);
			}

			if (wordlist.empty()) {
				wordlist = english_wordlist;
			} else {
				for (size_t i = 0; i < english_wordlist.size() && i < wordlist.size(); ++i) {
					wordlist_translations[english_wordlist[i]] = wordlist[i];
				}
			}
		}
		// developer aid: OPENC2E_PRINT_WORDS prints the word list (C2: nouns,
		// the names of object categories, are words 16-55)
		if (devenv("OPENC2E_PRINT_WORDS"))
			for (size_t i = 0; i < wordlist.size(); i++)
				fmt::print("* WORD {} {}\n", i, wordlist[i]);
	}
}

std::string Engine::translateWordlistWord(const std::string& s) {
	auto it = wordlist_translations.find(s);
	if (it == wordlist_translations.end()) {
		return s;
	} else {
		return it->second;
	}
}

std::string Engine::executeNetwork(std::string in) {
	// now parse and execute the CAOS we obtained
	// (C1/C2: OWNR, and so TARG at the start, is the selected creature in a
	// script injected for immediate execution, as a COB's install script:
	// C2 CAOS guide. BunchaStuff's "Get Target!" is just INST, EDIT: the
	// hand picks up the selected creature)
	AgentRef owner;
	if (version < 3)
		owner = world.selectedcreature;
	caosVM vm(owner); // needs to be outside 'try' so we can reset outputstream on exception
	try {
		caosScript script(gametype, "<network>"); // XXX
		script.parse(in);
		script.installScripts();
		StringWriter o;
		vm.outputstream = &o;
		vm.runEntirely(script.installer);
		vm.outputstream = 0; // otherwise would point to dead stack
		return o.string();
	} catch (Exception& e) {
		vm.outputstream = 0; // otherwise would point to dead stack
		return std::string("### EXCEPTION: ") + e.what();
	}
}

unsigned int Engine::tickInterval() const {
	unsigned int t = (unsigned int)std::lround(world.ticktime / std::clamp(speed, 0.1f, 100.0f));
	return std::max(1u, t);
}

unsigned int Engine::msUntilTick() {
	if (fastticks)
		return 0;
	if (world.paused)
		return world.ticktime; // TODO: correct?

	int ival = (tickdata + tickInterval()) - get_ticks_msec();
	return (ival < 0) ? 0 : ival;
}

// The hand's x from the view's left edge, in world pixels, the short way
// round a world that wraps: across Albia's seam (east end joined to west)
// the view can start near x 8300 while the hand is at x 600, which is
// 650 px into the view, not 7700 px left of it (owner, 2026-11-12: the hand
// vanished while panning across the seam at any zoom but 100%, whose world
// drawing already wraps)
static int handViewX(Camera* cam, PointerAgent* hand) {
	int dx = hand->pointerX() - (int)cam->getX();
	MetaRoom* m = cam->getMetaRoom();
	if (m && m->wraparound() && m->width() > 0) {
		int w = (int)m->width();
		dx = ((dx % w) + w) % w;
		if (dx > w / 2)
			dx -= w;
	}
	return dx;
}

void Engine::drawWorld() {
	// -k null: no window to draw in (the null backend's render targets draw
	// nothing, and the world's drawing asserted on its missing surface)
	if (get_backend() == NullBackend::get_instance())
		return;
	PROFILE_SCOPE("draw: world and interface (per tick)");
	if (Profile::enabled())
		Profile::totals()["frames drawn (count, per tick)"] += 1.0;
	// draw the world

	// developer aid: OPENC2E_TEST_PAN="dx,dy" starts a smooth pan that far
	// from the view at world tick 30 and prints the camera's place each
	// frame while it glides
	if (const char* tp = devenv("OPENC2E_TEST_PAN")) {
		static bool started = false;
		int dx = 0, dy = 0;
		if (!started && world.tickcount >= 30 && sscanf(tp, "%d,%d", &dx, &dy) == 2) {
			started = true;
			fmt::print("* TEST PAN from {},{} by {},{}\n", camera->getX(), camera->getY(), dx, dy);
			camera->moveTo((int)camera->getX() + dx, (int)camera->getY() + dy, smoothscroll);
		}
		if (camera->isPanning())
			fmt::print("* TEST PAN t{} at {},{}\n", get_ticks_msec(), camera->getX(), camera->getY());
	}
	// developer aid: OPENC2E_TEST_HANG=N spins the main loop for N seconds
	// at world tick 50 (to test the hang reports, common/backtrace.h)
	if (const char* th = devenv("OPENC2E_TEST_HANG")) {
		static bool hung = false;
		if (!hung && world.tickcount >= 50) {
			hung = true;
			unsigned int until = get_ticks_msec() + 1000u * (unsigned int)atoi(th);
			volatile unsigned long spin = 0;
			while (get_ticks_msec() < until)
				spin = spin + 1;
		}
	}
	camera->stepPan(); // smooth scrolling, frame by frame
	Openc2eImGui::Update();
	// C1/C2: following goes with the selection (owner, 2026-11-22): while the
	// view follows the creature selected, choosing another, by whatever
	// button, kit or key, goes on to follow that one
	if (version <= 2) {
		static AgentRef lastSelected;
		static bool following = false;
		AgentRef selected(world.selectedcreature);
		if (selected) {
			if (selected != lastSelected && following)
				camera->trackAgent(selected, 0, 0, flexible, none);
			lastSelected = selected;
			following = camera->trackedAgent() == selected;
		}
	}
	get_backend()->getMainRenderTarget()->setViewportOffsetTop(Openc2eImGui::GetViewportOffsetTop());
	get_backend()->getMainRenderTarget()->setViewportOffsetBottom(Openc2eImGui::GetViewportOffsetBottom());
	get_backend()->getMainRenderTarget()->setViewportOffsetRight(Openc2eImGui::GetViewportOffsetRight());

	if (dorendering || refreshdisplay) {
		refreshdisplay = false;
		RenderTarget* main = get_backend()->getMainRenderTarget();
		// while the mouse is over a kit, the toolbar or any other window, the
		// hand is not drawn: it went on moving behind the kit (owner,
		// 2026-11-11)
		const bool overWindow = ImGui::GetCurrentContext() && ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		if (zoom == 1.0f) {
			world.drawWorld(camera.get(), main, overWindow);
		} else {
			// draw the world at 1:1 into a texture the size of the view in
			// world pixels, scale that onto the window, then draw the hand on
			// top at its usual size
			int zw = (int)std::ceil(main->getWidth() / zoom), zh = (int)std::ceil(main->getHeight() / zoom);
			if (!zoomtarget || zoomtarget->getWidth() != zw || zoomtarget->getHeight() != zh)
				zoomtarget = get_backend()->newRenderTarget(zw, zh);
			world.drawWorld(camera.get(), zoomtarget.get(), true);
			main->renderClear();
			main->blitRenderTarget(zoomtarget.get(), Rect2f(0, 0, zw * zoom, zh * zoom));
			PointerAgent* hand = world.hand();
			if (hand && hand->visible && !overWindow) {
				int mx = (int)std::lround(handViewX(camera.get(), hand) * zoom);
				int my = (int)std::lround((hand->pointerY() - (int)camera->getY()) * zoom);
				if (CompoundPart* p = hand->part(0))
					p->render(main, mx - hand->pointerX(), my - hand->pointerY());
				// and what it shows with it: the angel (pet), the devil
				// (spank), the push hand's arrows (owner, 2026-11-15: none
				// of them showed at any zoom but 100%)
				if (CompoundPart* o = (CompoundPart*)hand->overlayPart())
					o->render(main, mx - hand->pointerX(), my - hand->pointerY());
			}
		}
	}
}

static const float zoom_levels[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f};

void Engine::setZoom(float z, int anchorx, int anchory) {
	if (version > 2 || z == zoom)
		return;
	RenderTarget* main = get_backend()->getMainRenderTarget();
	// keep the world point under the anchor (window pixels; default the
	// view's centre) where it is
	if (anchorx < 0 || anchory < 0) {
		anchorx = main->getWidth() / 2;
		anchory = main->getHeight() / 2;
	}
	float wx = camera->getX() + anchorx / zoom, wy = camera->getY() + anchory / zoom;
	zoom = z;
	camera->trackAgent(AgentRef(), 0, 0, brittle, none);
	camera->moveTo((int)std::lround(wx - anchorx / zoom), (int)std::lround(wy - anchory / zoom), jump);
	if (world.hand())
		world.hand()->moveTo((int)std::lround(wx) - (world.hand()->pointerX() - (int)world.hand()->x), (int)std::lround(wy) - (world.hand()->pointerY() - (int)world.hand()->y));
	refreshdisplay = true;
}

void Engine::zoomStep(int steps, int anchorx, int anchory) {
	int n = sizeof(zoom_levels) / sizeof(zoom_levels[0]);
	int i = 0;
	while (i < n - 1 && zoom_levels[i] < zoom - 0.001f)
		i++;
	i = std::max(0, std::min(n - 1, i + steps));
	setZoom(zoom_levels[i], anchorx, anchory);
}

float Engine::tickFraction() const {
	if (world.paused || world.ticktime == 0)
		return 1.0f;
	float f = (float)(get_ticks_msec() - tickdata) / (float)tickInterval();
	return f < 0 ? 0.0f : (f > 1 ? 1.0f : f);
}

static std::chrono::steady_clock::time_point program_started = std::chrono::steady_clock::now();
uint32_t Engine::get_ticks_msec() const {
	// TODO: this will wrap every ~49 days, not good. Use a 64-bit result or handle this differently
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - program_started).count();
}

// developer aid: OPENC2E_DUMP_AGENT="family,genus,species" prints each
// matching agent's position and its parts (sprite, frames, offsets, depth)
// on every snapshot (and every world tick with OPENC2E_DUMP_AGENT_EVERY_TICK),
// so part changes can be compared frame by frame
// developer aid: OPENC2E_DUMP_RENDERS="x0,x1" prints, on every snapshot, the
// draw order (bottom to top) of the agent parts whose agents lie between x0
// and x1, with their planes, to check what is drawn over what
static void dumpRenders() {
	// developer aid: OPENC2E_DUMP_CREATURE_PARTS lists every creature's part
	// pictures, frames and growth scales at each snapshot
	if (devenv("OPENC2E_DUMP_CREATURE_PARTS"))
		for (auto& a : world.agents)
			if (auto* sc = dynamic_cast<SkeletalCreature*>(a.get()))
				fmt::print("* PARTS unid {} at {:.0f},{:.0f}: {}\n", a->getUNID(), a->x, a->y, sc->describeParts());
	int x0, x1;
	const char* s = devenv("OPENC2E_DUMP_RENDERS");
	if (!s || sscanf(s, "%d,%d", &x0, &x1) != 2)
		return;
	int i = 0;
	for (auto* r : world.renders) {
		i++;
		auto* p = dynamic_cast<CompoundPart*>(r);
		if (!p)
			continue;
		Agent* a = p->getParent();
		if ((a->x < x0 || a->x > x1) && a->family != 4)
			continue;
		auto* sp = dynamic_cast<SpritePart*>(p);
		fmt::print("  render #{} {} part {} z {} at {:.0f},{:.0f} sprite {} frame {}\n", i, a->identify(), p->id, p->getZOrder(), a->x + p->x, a->y + p->y,
			sp && sp->getSprite() ? sp->getSprite()->getName() : "-", sp ? (int)sp->getCurrentSprite() : -1);
	}
}

static void dumpAgents() {
	int f, g, sp;
	const char* s = devenv("OPENC2E_DUMP_AGENT");
	if (!s || sscanf(s, "%d,%d,%d", &f, &g, &sp) != 3)
		return;
	for (auto& a : world.agents) {
		if (!a || a->family != f || a->genus != g || a->species != sp)
			continue;
		fmt::print("* Agent {} {} {} at ({}, {}) zorder {} ov {} {} {}\n", f, g, sp, a->x, a->y, a->getZOrder(), a->var[0].dump(), a->var[1].dump(), a->var[2].dump());
		// every part, of simple agents too (their one part is part 0)
		auto* c = dynamic_cast<CompoundAgent*>(a.get());
		unsigned int parts = c ? c->partCount() : (a->part(0) ? 1 : 0);
		for (unsigned int i = 0; i < parts; i++) {
			CompoundPart* cp = c ? c->part(i) : a->part(0);
			auto* p = dynamic_cast<SpritePart*>(cp);
			if (!p)
				continue;
			fmt::print("    part {} at ({}, {}) z {} sprite {} first {} base {} pose {} frame {} size {}x{} transparent {}\n",
				p->id, p->x, p->y, p->getZOrder(), p->getSprite() ? p->getSprite()->getName() : "-", p->getFirstImg(),
				p->getBase(), p->getPose(), p->getCurrentSprite(), p->getWidth(), p->getHeight(), p->is_transparent);
		}
	}
}


void Engine::update() {
	tickdata = get_ticks_msec();

	// tick the world
	// developer aid: OPENC2E_TICKRATE=s prints, every s seconds, how many
	// world ticks ran and the average time one took (ms), with the number of
	// creatures (for performance work)
	static const int tickrate = devenv("OPENC2E_TICKRATE") ? std::max(1, atoi(devenv("OPENC2E_TICKRATE"))) : 0;
	auto t0 = std::chrono::steady_clock::now();
	world.tick();
	// the pace reached, measured over about two seconds of real time, for
	// the toolbar's speed control (a busy world may not keep up)
	{
		unsigned int now = get_ticks_msec();
		if (!measureStart)
			measureStart = now;
		measureTicks++;
		if (now - measureStart >= 2000) {
			measuredSpeed = measureTicks * (float)world.ticktime / (float)(now - measureStart);
			measureStart = now;
			measureTicks = 0;
		}
	}
	if (tickrate) {
		static double spent = 0;
		static int ticks = 0;
		static auto since = t0;
		spent += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		ticks++;
		double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
		if (secs >= tickrate) {
			int creatures = 0;
			for (auto& a : world.agents)
				if (a && a->family == 4)
					creatures++;
			fmt::print("* TICKRATE {:.1f} ticks/s, {:.2f} ms a tick, {} creatures, {} agents\n", ticks / secs, spent / ticks, creatures, world.agents.size());
			std::vector<std::pair<double, std::string>> parts;
			for (auto& [name, ms] : Profile::totals())
				parts.push_back({ms / ticks, name});
			std::sort(parts.rbegin(), parts.rend());
			for (auto& [ms, name] : parts)
				fmt::print("*   {:7.2f} ms  {}\n", ms, name);
			Profile::totals().clear();
			spent = 0;
			ticks = 0;
			since = std::chrono::steady_clock::now();
		}
	}

	// C1/C2: save the open world every few minutes of play (File > Autosave;
	// 5 by default, owner 2026-11-11: a crash had lost an hour). Not with
	// --no-autosave (test runs).
	if (version < 3 && worldready && autosave && !currentworlddir.empty()) {
		static unsigned int s_last_autosave = tickdata;
		int minutes = autosaveMinutes();
		if (minutes > 0 && tickdata - s_last_autosave >= (unsigned int)minutes * 60000u) {
			s_last_autosave = tickdata;
			std::string err;
			if (WorldSave::SaveWorldState(currentworlddir, err))
				fmt::print("* Autosaved world '{}' (every {} min)\n", currentworld, minutes);
			else
				fmt::print(stderr, "Autosave failed: {}\n", err);
		}
	}
	// developer aid: OPENC2E_TEST_KILL=N kills the selected creature at world
	// tick N, or "+N" N ticks into the session (headless; OPENC2E_TEST_DEATH
	// needs the Owner's Kit drawn)
	static unsigned int killAt = [] {
		const char* k = devenv("OPENC2E_TEST_KILL");
		return !k ? 0u : k[0] == '+' ? world.tickcount + (unsigned int)atoi(k + 1) : (unsigned int)atoi(k);
	}();
	if (killAt && world.tickcount == killAt && world.selectedcreature)
		if (auto* ca = dynamic_cast<CreatureAgent*>(world.selectedcreature.get()))
			if (ca->getCreature()->isAlive())
				ca->getCreature()->die();
	// developer aid: OPENC2E_TEST_CLICK="N,X,Y" clicks the hand's left
	// button at world position X,Y at world tick N, through the hand's own
	// event handling (a mouse move there, then the button down and up);
	// "N,X,Y,right" clicks the right button (pick up or drop)
	// developer aid: OPENC2E_TEST_HITGRID="N,x0,y0,x1,y1,step" at tick N
	// prints what a click (C) and a pickup (P) would land on at each point:
	// the first letter of its genus's place in the alphabet of agents seen,
	// listed after the grid ('.' nothing)
	if (static const char* k = devenv("OPENC2E_TEST_HITGRID"); k) {
		int n = 0, x0, y0, x1, y1, st = 4;
		if (sscanf(k, "%d,%d,%d,%d,%d,%d", &n, &x0, &y0, &x1, &y1, &st) >= 5 && world.tickcount == (unsigned int)n) {
			std::vector<Agent*> seen;
			auto key = [&](CompoundPart* p) {
				if (!p)
					return '.';
				Agent* a = p->getParent();
				auto it = std::find(seen.begin(), seen.end(), a);
				if (it == seen.end()) {
					seen.push_back(a);
					it = seen.end() - 1;
				}
				return (char)('a' + (it - seen.begin()) % 26);
			};
			for (const char* what : {"click", "pickup"}) {
				fmt::print("* HITGRID {} ({},{} to {},{} every {}):\n", what, x0, y0, x1, y1, st);
				for (int y = y0; y <= y1; y += st) {
					std::string row;
					for (int x = x0; x <= x1; x += st)
						row += key(std::string(what) == "click" ? world.partAt(x, y, true, false, true) : world.partAt(x, y, true, true, false));
					fmt::print("*   {:5} {}\n", y, row);
				}
			}
			for (size_t i = 0; i < seen.size(); i++)
				fmt::print("* HITGRID {} = {} {} {} attr {} at {:.0f},{:.0f} {}x{}\n", (char)('a' + i % 26), (int)seen[i]->family, (int)seen[i]->genus, seen[i]->species, (int)seen[i]->attr, seen[i]->x, seen[i]->y, seen[i]->getWidth(), seen[i]->getHeight());
		}
	}
	if (static const char* k = devenv("OPENC2E_TEST_CLICK"); k && world.hand()) {
		int n = 0, cx = 0, cy = 0;
		// ("+N,x,y": N ticks after the world loaded, as OPENC2E_TEST_KILL=+N)
		static unsigned int clickAt = 0;
		if (k[0] == '+' && sscanf(k + 1, "%d,%d,%d", &n, &cx, &cy) == 3) {
			if (!clickAt)
				clickAt = world.tickcount + (unsigned int)n;
			n = (int)clickAt;
		}
		if (sscanf(k, "%*[+]%d,%d,%d", &n, &cx, &cy) == 3 || sscanf(k, "%d,%d,%d", &n, &cx, &cy) == 3)
			if (k[0] == '+')
				n = (int)clickAt;
		if (n && world.tickcount == (unsigned int)n) {
			int prevsel = world.selectedcreature ? (int)world.selectedcreature->genus : -1;
			if (CompoundPart* hit = world.partAt(cx, cy, true, false, true))
				fmt::print("* TEST CLICK hits {} {} {} plane {}\n", (int)hit->getParent()->family, (int)hit->getParent()->genus, hit->getParent()->species, hit->getParent()->getZOrder());
			else
				fmt::print("* TEST CLICK hits nothing\n");
			for (auto& a : world.agents)
				if (a && a->family == 4)
					fmt::print("* TEST CLICK creature genus {} at {:.0f},{:.0f} {}x{}\n", (int)a->genus, a->x, a->y, a->getWidth(), a->getHeight());
			auto* hand = dynamic_cast<PointerAgent*>(world.hand());
			BackendEvent e{};
			hand->moveTo(cx - (hand->pointerX() - hand->x), cy - (hand->pointerY() - hand->y));
			e.type = eventmousebuttondown;
			e.button = strstr(k, "right") ? buttonright : buttonleft;
			hand->handleEvent(e);
			e.type = eventmousebuttonup;
			hand->handleEvent(e);
			std::string before = fmt::format("{}", prevsel);
			fmt::print("* TEST CLICK at {},{}: selected genus {} (before: genus {})\n", cx, cy, world.selectedcreature ? (int)world.selectedcreature->genus : -1, before);
		}
	}
	// developer aid: OPENC2E_TEST_TYPE="N,words": at tick N the hand goes beside
	// the selected creature (or the first creature) and the words are typed
	// through the engine's own key handlers, then Enter: the hand's speech
	// bubble opens, fills and is spoken, as when the player types (tests the
	// hand teaching creatures words)
	if (static const char* k = devenv("OPENC2E_TEST_TYPE"); k && version == 2) {
		// (OPENC2E_TEST_TYPE_REPEAT=R types it R times, 30 ticks apart)
		static int times = 0;
		static const int repeat = devenv("OPENC2E_TEST_TYPE_REPEAT") ? std::max(1, atoi(devenv("OPENC2E_TEST_TYPE_REPEAT"))) : 1;
		static unsigned int nextAt = (unsigned int)atoi(k);
		const char* words = strchr(k, ',');
		if (times < repeat && words && world.tickcount >= nextAt) {
			times++;
			nextAt = world.tickcount + 30;
			Agent* c = world.selectedcreature.get();
			if (!c)
				for (auto& a : world.agents)
					if (a && dynamic_cast<CreatureAgent*>(a.get())) {
						c = a.get();
						break;
					}
			if (c && world.hand()) {
				world.hand()->moveTo(c->x + c->getWidth() + 10, c->y - 20);
				for (const char* ch = words + 1; *ch; ch++) {
					BackendEvent e;
					e.type = eventtextinput;
					e.text = std::string(1, *ch);
					handleTextInput(e);
				}
				BackendEvent ret;
				ret.type = eventrawkeydown;
				ret.key = OPENC2E_KEY_RETURN;
				handleRawKeyDown(ret);
				// how well it knows each typed word now (0-255; known from WORD_KNOWN)
				std::string known;
				if (auto* ca = dynamic_cast<CreatureAgent*>(c))
					if (auto* oc = dynamic_cast<oldCreature*>(ca->getCreature()))
						for (size_t w = 0; w < wordlist.size(); w++) {
							std::string word = wordlist[w];
							while (!word.empty() && word.back() == ' ')
								word.pop_back();
							std::string t = std::string(" ") + (words + 1) + " ";
							if (!word.empty() && t.find(" " + word + " ") != std::string::npos)
								known += fmt::format(" {}={}", word, w < oc->vocab.size() ? (int)oc->vocab[w] : 0);
						}
				fmt::print("* TEST TYPE tick {} ({} of {}): typed \"{}\" beside creature {} at {:.0f},{:.0f}{}; vocabulary:{}\n", world.tickcount, times, repeat,
					words + 1, c->getUNID(), c->x, c->y, c->isDying() ? " (dying)" : "", known);
			}
		}
	}

	// developer aid: OPENC2E_TEST_MIDDLE_DRAG=N: from tick N, a middle-button
	// drag across the view sent as the system's own mouse events (through the
	// backend and the interface, as a real one comes): the button down, a
	// move each tick for 40 ticks, then the button up
	// ("N,ZOOM,FILE" sets that zoom first and saves pictures mid-drag and
	// after it, FILE-during.bmp and FILE-after.bmp)
	if (static const char* k = devenv("OPENC2E_TEST_MIDDLE_DRAG"); k) {
		static unsigned int last = 0;
		unsigned int n = (unsigned int)atoi(k), t = world.tickcount;
		float z = 0;
		char shot[400] = "";
		sscanf(k, "%*d,%f,%399s", &z, shot);
		if (t == n && z > 0)
			setZoom(z);
		if (shot[0] && t != last && (t == n + 20 || t == n + 50))
			get_backend()->requestScreenshot(fmt::format("{}-{}.bmp", shot, t == n + 20 ? "during" : "after"));
		if (t != last && t >= n && t <= n + 41) {
			last = t;
			float x = 500.0f + (t - n) * 6, y = 380.0f + (t - n) * 2;
			SDL_Event ev{};
			if (t == n || t == n + 41) {
				ev.type = t == n ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
				ev.button.button = SDL_BUTTON_MIDDLE;
				ev.button.down = t == n;
				ev.button.x = x;
				ev.button.y = y;
			} else {
				ev.type = SDL_EVENT_MOUSE_MOTION;
				ev.motion.state = SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE);
				ev.motion.x = x;
				ev.motion.y = y;
				ev.motion.xrel = 6;
				ev.motion.yrel = 2;
			}
			SDL_PushEvent(&ev);
			fmt::print("* TEST MIDDLE DRAG tick {}: pointer {},{} camera {},{} zoom {}\n", t, world.hand()->pointerX(), world.hand()->pointerY(), camera->getX(), camera->getY(), zoom);
		}
	}
	// developer aid: OPENC2E_TEST_BLACKBOARDS=N, at world tick N, lists the
	// world's blackboards (teaching machines): classifier, place, attributes
	if (static const char* k = devenv("OPENC2E_TEST_BLACKBOARDS"); k && world.tickcount == (unsigned int)atoi(k))
		for (auto& a : world.agents)
			if (a && dynamic_cast<Blackboard*>(a.get()))
				fmt::print("* BLACKBOARD {} {} {} at {:.0f},{:.0f} size {}x{} attr {}\n", (int)a->family, (int)a->genus, a->species, a->x, a->y,
					a->getWidth(), a->getHeight(), (int)a->attr);

	// developer aid: OPENC2E_TEST_KIN=N, at world tick N, prints for each
	// pair of living creatures whether they are close kin and barred as mates
	if (static const char* k = devenv("OPENC2E_TEST_KIN"); k && world.tickcount == (unsigned int)atoi(k)) {
		std::vector<CreatureAgent*> all;
		for (auto& a : world.agents)
			if (auto* ca = a ? dynamic_cast<CreatureAgent*>(a.get()) : nullptr; ca && ca->getCreature()->isAlive())
				all.push_back(ca);
		for (size_t i = 0; i < all.size(); i++)
			for (size_t j = i + 1; j < all.size(); j++) {
				std::string mi = world.history->findMoniker(all[i]->getCreature()->getGenome());
				std::string mj = world.history->findMoniker(all[j]->getCreature()->getGenome());
				fmt::print("* TEST KIN {} {} and {} {}: kin {} barred {}\n", mi, all[i]->getCreature()->isFemale() ? "F" : "M", mj,
					all[j]->getCreature()->isFemale() ? "F" : "M", closeKin(mi, mj) ? 1 : 0, kinBarred(all[i], all[j]) ? 1 : 0);
			}
	}

	// developer aid: OPENC2E_TEST_MATE=N, at world tick N, has the first
	// grown female and male of a genus mate (MATE's path: tryConceive)
	if (static const char* k = devenv("OPENC2E_TEST_MATE"); k && world.tickcount == (unsigned int)atoi(k)) {
		CreatureAgent *she = nullptr, *he = nullptr;
		for (auto& a : world.agents) {
			auto* ca = a ? dynamic_cast<CreatureAgent*>(a.get()) : nullptr;
			if (!ca || !ca->getCreature()->isAlive() || ca->getCreature()->getStage() < 2)
				continue;
			if (ca->getCreature()->isFemale() && !she)
				she = ca;
			else if (!ca->getCreature()->isFemale() && !he && (!she || dynamic_cast<Agent*>(she)->genus == a->genus))
				he = ca;
		}
		fmt::print("* TEST MATE: {}\n", tryConceive(she, he) ? "conceived" : "no");
	}
	// developer aid: OPENC2E_TEST_WHEEL_PAN="N,ZOOM,FILE": at tick N the zoom
	// is set and the pointer put at 500,380; then 20 ticks of scrolling (as a
	// TrackPoint's middle button + stick sends, or a wheel), the pointer not
	// moving; FILE.bmp is the picture after
	if (static const char* k = devenv("OPENC2E_TEST_WHEEL_PAN"); k) {
		static unsigned int last = 0;
		unsigned int n = (unsigned int)atoi(k), t = world.tickcount;
		float z = 1;
		char shot[400] = "";
		sscanf(k, "%*d,%f,%399s", &z, shot);
		if (t != last && t >= n && t <= n + 22) {
			last = t;
			SDL_Event ev{};
			if (t == n) {
				setZoom(z);
				ev.type = SDL_EVENT_MOUSE_MOTION;
				ev.motion.x = 500;
				ev.motion.y = 380;
			} else if (t <= n + 20) {
				ev.type = SDL_EVENT_MOUSE_WHEEL;
				ev.wheel.x = 0.5f;
				ev.wheel.y = -0.3f;
				// (OPENC2E_TEST_WHEEL_XY="X,Y": other amounts per tick)
				if (const char* xy = devenv("OPENC2E_TEST_WHEEL_XY"))
					sscanf(xy, "%f,%f", &ev.wheel.x, &ev.wheel.y);
				ev.wheel.mouse_x = 500;
				ev.wheel.mouse_y = 380;
			}
			if (ev.type)
				SDL_PushEvent(&ev);
			if (t == n + 22 && shot[0])
				get_backend()->requestScreenshot(fmt::format("{}.bmp", shot));
			fmt::print("* TEST WHEEL PAN tick {}: hand {},{} camera {},{}\n", t, world.hand()->pointerX(), world.hand()->pointerY(), camera->getX(), camera->getY());
		}
	}
	// developer aid: OPENC2E_TEST_CREATURE_ZONES=N prints, at tick N, the
	// selected creature as the hand finds it, every 2 px: P pet, S spank,
	// B the rest of the body and legs (push), a letter for any other part,
	// '.' see-through
	if (static const char* k = devenv("OPENC2E_TEST_CREATURE_ZONES"); k && world.tickcount == (unsigned int)atoi(k)) {
		if (auto* sk = dynamic_cast<SkeletalCreature*>(world.selectedcreature.get())) {
			fmt::print("* ZONES creature at {:.0f},{:.0f} {}x{}\n", sk->x, sk->y, sk->getWidth(), sk->getHeight());
			for (int yy = -4; yy < (int)sk->getHeight() + 4; yy += 2) {
				std::string row;
				for (int xx = -4; xx < (int)sk->getWidth() + 4; xx += 2) {
					float wx = sk->x + xx, wy = sk->y + yy;
					int act = sk->handleClick(wx, wy), p = sk->partUnder(wx, wy);
					row += act == 1 ? 'P' : act == 0 ? 'S' : SkeletalCreature::bodyOrLeg(p) ? 'B' : p < 0 ? '.' : (char)('a' + p);
				}
				fmt::print("*   {}\n", row);
			}
		}
	}
	// developer aid: OPENC2E_TEST_ZONE_CLICK="N,Z[,push]": at tick N, clicks
	// the selected creature with the hand at the middle of its zone Z (P pet,
	// S spank, B the rest of the body and legs; ",push" with the push hand
	// on, ",hover" to leave the hand there without clicking), printing what
	// happened
	if (static const char* k = devenv("OPENC2E_TEST_ZONE_CLICK"); k && world.hand()) {
		int n = 0;
		char z = 0;
		// (the selected creature, or, when it is riding or carried, hidden
		// from the view, the first creature that is not; kept for later ticks)
		// (",@X,Y" anywhere in it: the free creature nearest that point)
		static AgentRef zoneTarget;
		if (!zoneTarget && sscanf(k, "%d", &n) == 1 && world.tickcount >= (unsigned int)n) {
			Agent* sel = world.selectedcreature.get();
			float ax = 0, ay = 0;
			const char* at = strchr(k, '@');
			if (at && sscanf(at + 1, "%f,%f", &ax, &ay) == 2) {
				float best = 1e18f;
				for (auto& a : world.agents)
					if (a && dynamic_cast<SkeletalCreature*>(a.get()) && !a->invehicle && !a->carriedby) {
						float d = (a->x - ax) * (a->x - ax) + (a->y - ay) * (a->y - ay);
						if (d < best)
							best = d, zoneTarget = AgentRef(a);
					}
			} else if (sel && !sel->invehicle && !sel->carriedby)
				zoneTarget = AgentRef(sel);
			else
				for (auto& a : world.agents)
					if (a && dynamic_cast<SkeletalCreature*>(a.get()) && !a->invehicle && !a->carriedby && a->family == 4) {
						zoneTarget = AgentRef(a);
						break;
					}
		}
		if (sscanf(k, "%d,%c", &n, &z) == 2 && (world.tickcount == (unsigned int)n || (strstr(k, "hover") && world.tickcount > (unsigned int)n)))
			if (auto* sk = dynamic_cast<SkeletalCreature*>(zoneTarget.get())) {
				float sx = 0, sy = 0;
				int cnt = 0;
				for (int yy = 0; yy < (int)sk->getHeight(); yy++)
					for (int xx = 0; xx < (int)sk->getWidth(); xx++) {
						float wx = sk->x + xx + 0.5f, wy = sk->y + yy + 0.5f;
						int act = sk->handleClick(wx, wy), p = sk->partUnder(wx, wy);
						char c = act == 1 ? 'P' : act == 0 ? 'S' : SkeletalCreature::bodyOrLeg(p) ? 'B' : '.';
						if (c == z)
							sx += wx, sy += wy, cnt++;
					}
				if (cnt) {
					sx /= cnt, sy /= cnt;
					bool push = strstr(k, "push") != nullptr;
					world.handPush = push;
					float vx0 = sk->x;
					auto* hand = dynamic_cast<PointerAgent*>(world.hand());
					hand->moveTo(sx - (hand->pointerX() - hand->x), sy - (hand->pointerY() - hand->y));
					// ",hover": the creature kept in the middle of the view
					// and the hand over the zone, no click (for snapshots of
					// what the hand shows there)
					if (strstr(k, "hover")) { // (every tick from N on: the view and hand follow the creature)
						camera->moveTo((int)(sx - camera->getWidth() / 2), (int)(sy - camera->getHeight() / 2), jump);
						hand->moveTo(sx - (hand->pointerX() - hand->x), sy - (hand->pointerY() - hand->y));
						CompoundPart* under = world.partAt(sx, sy);
						fmt::print("* ZONE HOVER {} at {:.0f},{:.0f} (zoom {:.2f}): overlay {}, part under hand {}\n", z, sx, sy, zoom,
							hand->overlayPart() ? "shown" : "none", under ? under->getParent()->identify() : std::string("nothing"));
					} else {
						BackendEvent e{};
						e.type = eventmousebuttondown;
						e.button = buttonleft;
						hand->handleEvent(e);
						e.type = eventmousebuttonup;
						hand->handleEvent(e);
						auto* hp = dynamic_cast<SpritePart*>(hand->part(0));
						unsigned int hpose = hp ? hp->getPose() : 0;
						fmt::print("* ZONE CLICK {} at {:.0f},{:.0f} ({} px of it): handleClick {} part {}{}\n", z, sx, sy, cnt, sk->handleClick(sx, sy), sk->partUnder(sx, sy),
							push ? fmt::format(", push hand: {} (hand pose {})", hpose == 8 || hpose == 10 ? "shoved" : "not shoved", hpose) : std::string());
					}
					(void)vx0;
				} else
					fmt::print("* ZONE CLICK {}: none on this creature\n", z);
			}
	}
	// developer aid: OPENC2E_FOOD_CENSUS=N: at session tick N, every object
	// that smells of food (its eat script nourishes), by classifier: how
	// many, how many edible as they are now (edibleNow), how many carried,
	// and where (for "why is food left uneaten", 2026-11-16)
	if (static const char* k = devenv("OPENC2E_FOOD_CENSUS"); k && world.tickcount == (unsigned int)atoi(k)) {
		struct Tally {
			int n = 0, edible = 0, carried = 0;
			std::string where;
		};
		std::map<std::tuple<int, int, int>, Tally> t;
		for (auto& a : world.agents) {
			if (!a || a->isDying() || a->family == 4 || edibleScent(a.get()) <= 0)
				continue;
			auto& e = t[{(int)a->family, (int)a->genus, a->species}];
			e.n++;
			if (edibleNow(a.get()) > 0)
				e.edible++;
			if (a->carriedby)
				e.carried++;
			if (e.n <= 4)
				e.where += fmt::format(" {:.0f},{:.0f}", a->x, a->y + a->getHeight());
		}
		for (auto& [c, e] : t)
			fmt::print("* FOOD CENSUS {} {} {}: {} ({} edible now, {} carried) at{}\n", std::get<0>(c), std::get<1>(c), std::get<2>(c), e.n, e.edible, e.carried, e.where);
	}
	// C2: a census of the creatures and their food (owner, 2026-11-16: "add a
	// logger", to see hunger across seasons and overnight; a single sample
	// misled, as the hungry share swings widely in minutes). Every
	// OPENC2E_CENSUS_EVERY ticks (3000, about 5 minutes at 10 a second) a
	// row is added to census.csv in the world's folder and a line printed:
	// season, creatures, how many hungry (Hunger 150+) and starving (216+,
	// what counts as starvation at death), by life stage (adults: stage 4),
	// eggs, edible food lying within reach of the floor, and deaths by
	// cause since the last row. OPENC2E_CENSUS_EVERY=0 turns it off.
	if (version == 2 && world.map && !currentworlddir.empty()) {
		static const long every = devenv("OPENC2E_CENSUS_EVERY") ? atol(devenv("OPENC2E_CENSUS_EVERY")) : 3000;
		static unsigned int lastCensus = 0;
		if (every > 0 && world.tickcount >= lastCensus + (unsigned int)every) {
			lastCensus = world.tickcount;
			int creatures = 0, hungry = 0, starving = 0, adults = 0, adultsHungry = 0, eggs = 0, food = 0;
			// the population's mind genes and gene-given temperament, on
			// average (genes.csv: to watch them drift over generations)
			std::array<double, NUM_MIND_GENES> mindSum{};
			std::array<double, 6> shiftSum{};
			for (auto& a : world.agents) {
				if (!a || a->isDying())
					continue;
				if (a->family == 2 && a->genus == 5 && (a->species == 2 || a->species == 3)) {
					eggs++;
					continue;
				}
				if (auto* ca = dynamic_cast<CreatureAgent*>(a.get())) {
					auto* c = dynamic_cast<oldCreature*>(ca->getCreature());
					if (!c || !c->isAlive())
						continue;
					creatures++;
					for (int i = 0; i < NUM_MIND_GENES; i++)
						mindSum[i] += c->mindGenes.z[i];
					for (int i = 0; i < 6; i++)
						shiftSum[i] += c->geneShift[i];
					int h = c->getDrive(2);
					bool adult = (int)c->getStage() == 4;
					adults += adult;
					if (h >= 150) {
						hungry++;
						adultsHungry += adult;
					}
					if (h >= 216)
						starving++;
					continue;
				}
				if (a->family != 2 || a->carriedby || edibleNow(a.get()) <= 0.0f)
					continue;
				auto r = world.map->roomAt(a->x + a->getWidth() / 2.0f, a->y + a->getHeight() / 2.0f);
				if (r && r->floorYatX(a->x + a->getWidth() / 2.0f) - (a->y + a->getHeight()) <= 60.0f)
					food++;
			}
			auto& deaths = censusDeaths();
			auto take = [&](const char* k) {
				int n = deaths.count(k) ? deaths[k] : 0;
				return n;
			};
			int dStarve = take("starvation"), dDrown = take("drowning"), dOld = take("old age"), dOther = 0;
			for (auto& [k, n] : deaths)
				if (k != "starvation" && k != "drowning" && k != "old age")
					dOther += n;
			deaths.clear();
			std::filesystem::path csv = currentworlddir / "census.csv";
			std::error_code ec;
			bool fresh = !std::filesystem::exists(csv, ec);
			std::ofstream out(csv, std::ios::app | std::ios::binary);
			if (fresh)
				out << "time,world tick,season,year,creatures,hungry,starving,adults,adults hungry,eggs,food in reach,died starving,died drowning,died old,died other\n";
			std::time_t now = std::time(nullptr);
			char stamp[32];
			std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", std::localtime(&now));
			out << fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", stamp, world.worldtickcount, world.season, world.year, creatures, hungry, starving,
				adults, adultsHungry, eggs, food, dStarve, dDrown, dOld, dOther);
			{
				std::filesystem::path gcsv = currentworlddir / "genes.csv";
				bool gfresh = !std::filesystem::exists(gcsv, ec);
				std::ofstream gout(gcsv, std::ios::app | std::ios::binary);
				if (gfresh) {
					gout << "time,world tick,season,year,creatures";
					for (int i = 0; i < NUM_MIND_GENES; i++)
						gout << "," << MindGeneSet::name(i);
					for (int i = 0; i < 6; i++)
						gout << ",genes: " << Temperaments::traitName(i);
					gout << "\n";
				}
				gout << stamp << "," << world.worldtickcount << "," << world.season << "," << world.year << "," << creatures;
				for (int i = 0; i < NUM_MIND_GENES; i++)
					gout << fmt::format(",{:.3f}", creatures ? mindSum[i] / creatures : 0.0);
				for (int i = 0; i < 6; i++)
					gout << fmt::format(",{:.3f}", creatures ? shiftSum[i] / creatures : 0.0);
				gout << "\n";
			}
			fmt::print("* CENSUS season {} year {}: {} creatures, {} hungry ({:.0f}%), {} starving; adults {} ({} hungry); {} eggs; {} food in reach; died: {} starving, {} drowning, {} old, {} other\n",
				world.season, world.year, creatures, hungry, creatures ? 100.0 * hungry / creatures : 0.0, starving, adults, adultsHungry, eggs, food, dStarve, dDrown, dOld, dOther);
		}
	}
	// developer aid: OPENC2E_TEST_PINCH="N,scale,count": from tick N, one SDL
	// pinch update of that scale each tick, count of them, printing the zoom
	if (static const char* k = devenv("OPENC2E_TEST_PINCH"); k) {
		static unsigned int last = 0;
		int n = 0, count = 0;
		float sc = 1;
		if (sscanf(k, "%d,%f,%d", &n, &sc, &count) == 3 && world.tickcount != last && world.tickcount >= (unsigned int)n &&
			world.tickcount <= (unsigned int)(n + count)) {
			last = world.tickcount;
			fmt::print("* TEST PINCH tick {}: zoom {:.2f} camera {},{}\n", world.tickcount, zoom, camera->getX(), camera->getY());
			if (world.tickcount < (unsigned int)(n + count)) {
				SDL_Event ev{};
				ev.type = SDL_EVENT_PINCH_UPDATE;
				ev.pinch.scale = sc;
				SDL_PushEvent(&ev);
			}
		}
	}
	// developer aid: OPENC2E_TEST_EGG_DROP=N: at session tick N a creature
	// egg (2 5 2, no genome) appears by the incubator, and 10 ticks later it
	// gets the dropped event (5) from the hand, as when the hand puts it down
	if (static const char* k = devenv("OPENC2E_TEST_EGG_DROP"); k) {
		unsigned int n = (unsigned int)atoi(k);
		if (world.tickcount == n) {
			std::string err = executeNetwork("new: simp eggs 8 0 2000 0\nsetv cls2 2 5 2\nsetv attr 195\nmvto 5100 750\nsetv grav 1\n");
			fmt::print("* TEST EGG DROP: egg made{}\n", err.empty() ? "" : ": " + err);
		} else if (world.tickcount == n + 10) {
			for (auto& a : world.agents)
				if (a && a->family == 2 && a->genus == 5 && a->species == 2) {
					fmt::print("* TEST EGG DROP: dropped event: {}\n", a->queueScript(5, world.hand()) ? "ran" : "no script");
					break;
				}
		}
	}
	// developer aid: OPENC2E_TEST_LAY=N, at world tick N, has the first
	// pregnant creature lay her egg now (the world's egg-laying involuntary
	// action, creature event 65)
	if (static const char* k = devenv("OPENC2E_TEST_LAY"); k && world.tickcount == (unsigned int)atoi(k)) {
		bool laid = false;
		for (auto& a : world.agents) {
			auto* ca = a ? dynamic_cast<CreatureAgent*>(a.get()) : nullptr;
			if (!ca || !ca->getCreature()->isAlive() || a->babymoniker == 0)
				continue;
			laid = a->queueScript(65);
			fmt::print("* TEST LAY: {} at {:.0f},{:.0f}: {}\n", world.history->findMoniker(ca->getCreature()->getGenome()), a->x, a->y, laid ? "laying" : "no script");
			break;
		}
		if (!laid)
			fmt::print("* TEST LAY: no pregnant creature\n");
	}
	if (devenv("OPENC2E_DUMP_AGENT_EVERY_TICK")) {
		fmt::print("* Tick {}\n", world.tickcount);
		dumpAgents();
	}

	// keep the camera on a followed agent (View > Track Norn, the toolbar's
	// follow button, CAOS camera tracking); before, it only centred once
	camera->tick();

	// update sounds
	soundmanager.tick();

	// play C1 background wavs and MNG and MIDI music
	musicmanager->tick();

	// update our data for things like pace, race, ticktime, etc
	ticktimes[ticktimeptr] = get_ticks_msec() - tickdata;
	ticktimeptr++;
	if (ticktimeptr == 10)
		ticktimeptr = 0;
	float avgtime = 0;
	for (unsigned int ticktime : ticktimes)
		avgtime += ((float)ticktime / world.ticktime);
	world.pace = avgtime / 10;

	world.race = get_ticks_msec() - lasttimestamp;
	lasttimestamp = get_ticks_msec();
}

// developer aid (the trailer, owner 2026-11-21): OPENC2E_DIRECTOR=FILE, a
// list of timed instructions, one a line, "SECONDS INSTRUCTION ...", the time
// in seconds of real time from the end of startup ('#' starts a comment):
//   zoom Z            the view's zoom (keeps the middle where it is)
//   cam X Y           the view's middle jumps to world X, Y
//   pan X Y           the view's middle glides to world X, Y
//   follow MONIKER    the view follows that creature ("follow off" stops)
//   select MONIKER    that creature is the one chosen (for the kits)
//   bars off|on       the toolbar, status bar and scroll bars hidden or shown
//   kit ID [PAGE]     opens a kit (health, breeders, owners, ecology...) at a page
//   kits off          closes the kits
//   say X Y TEXT      the hand goes to world X, Y and says it
//   caos TEXT         runs CAOS (as the world's additions run)
//   die               the creature chosen dies
// For recording scripted shots with OPENC2E_RECORD.
static void runDirector() {
	static const char* file = devenv("OPENC2E_DIRECTOR");
	if (!file || !engine.worldready || !engine.camera)
		return;
	struct Cue {
		double at;
		std::string what;
	};
	static std::vector<Cue> cues;
	static size_t next = 0;
	static uint64_t startMs = 0;
	if (startMs == 0) {
		startMs = engine.get_ticks_msec();
		std::ifstream in(file);
		std::string line;
		while (std::getline(in, line)) {
			// ('#' starts a comment, alone; STM# is CAOS)
			if (auto h = !line.empty() && line[0] == '#' ? 0 : line.find(" # "); h != std::string::npos)
				line = line.substr(0, h);
			std::istringstream l(line);
			Cue c;
			if (l >> c.at) {
				std::getline(l, c.what);
				c.what.erase(0, c.what.find_first_not_of(" \t"));
				if (!c.what.empty())
					cues.push_back(c);
			}
		}
		std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.at < b.at; });
		fmt::print("* Director: {} cues from {}\n", cues.size(), file);
	}
	double t = (engine.get_ticks_msec() - startMs) / 1000.0;
	auto creature = [](const std::string& mon) -> AgentRef {
		if (!world.history || !world.history->hasMoniker(mon))
			return AgentRef();
		return world.history->getMoniker(mon).owner;
	};
	auto middleTo = [](float x, float y, panstyle style) {
		float z = engine.zoom > 0 ? engine.zoom : 1.0f;
		engine.camera->trackAgent(AgentRef(), 0, 0, brittle, none);
		engine.camera->moveTo((int)(x - engine.camera->getWidth() / (2 * z)), (int)(y - engine.camera->getHeight() / (2 * z)), style);
	};
	while (next < cues.size() && cues[next].at <= t) {
		std::istringstream l(cues[next].what);
		std::string cmd;
		l >> cmd;
		fmt::print("* Director {:.1f}s: {}\n", t, cues[next].what);
		if (cmd == "zoom") {
			float z;
			if (l >> z)
				engine.setZoom(z);
		} else if (cmd == "cam" || cmd == "pan") {
			float x, y;
			if (l >> x >> y)
				middleTo(x, y, cmd == "cam" ? jump : smoothscroll);
		} else if (cmd == "follow") {
			std::string who;
			l >> who;
			if (who == "off")
				engine.camera->trackAgent(AgentRef(), 0, 0, brittle, none);
			else if (AgentRef a = creature(who))
				engine.camera->trackAgent(a, 50, 50, flexible, none);
		} else if (cmd == "select") {
			std::string who;
			l >> who;
			if (AgentRef a = creature(who))
				world.selectedcreature = a;
		} else if (cmd == "bars") {
			std::string v;
			l >> v;
			Openc2eImGui::g_hideBars = v == "off";
		} else if (cmd == "kit") {
			std::string id, page;
			l >> id;
			std::getline(l, page);
			page.erase(0, page.find_first_not_of(" \t"));
			Openc2eImgui::OpenC2KitAt(id.c_str(), page.empty() ? nullptr : page.c_str());
		} else if (cmd == "kits") {
			Openc2eImgui::CloseC2Kits();
		} else if (cmd == "say") {
			float sx = 0, sy = 0;
			std::string text;
			l >> sx >> sy;
			std::getline(l, text);
			text.erase(0, text.find_first_not_of(" \t"));
			if (world.hand()) {
				world.hand()->moveTo(sx, sy);
				handSays(text);
			}
		} else if (cmd == "caos") {
			std::string text;
			std::getline(l, text);
			world.executeInitScriptText("director", text);
		} else if (cmd == "die") {
			auto* ca = dynamic_cast<CreatureAgent*>(world.selectedcreature.get());
			if (ca && ca->getCreature() && ca->getCreature()->isAlive())
				ca->getCreature()->die();
		}
		next++;
	}
}

bool Engine::tick() {
	runDirector();
	// developer aid: OPENC2E_SNAPSHOTS="x,y,file;x,y,file" moves the camera to
	// each (x, y) in turn and saves a screenshot there (BMP), one every 40 ticks
	// (-1,-1: where the view already is)
	static std::vector<std::tuple<int, int, std::string>> snapshots = [] {
		std::vector<std::tuple<int, int, std::string>> v;
		if (const char* s = devenv("OPENC2E_SNAPSHOTS")) {
			std::stringstream ss(s);
			std::string item;
			while (std::getline(ss, item, ';')) {
				int x, y;
				char file[512];
				if (sscanf(item.c_str(), "%d,%d,%511s", &x, &y, file) == 3)
					v.emplace_back(x, y, file);
			}
		}
		return v;
	}();
	// developer aid: OPENC2E_DUMP_ROOMS="x0,y0,x1,y1" prints the rooms overlapping
	// that area with their floors, C2 floor values and doors (with permeability);
	// OPENC2E_SHOW_ROOMS draws room outlines (as View > Show Map does)
	static bool rooms_dumped = false;
	if (!rooms_dumped && world.map) {
		rooms_dumped = true;
		if (devenv("OPENC2E_SHOW_ROOMS"))
			world.showrooms = true;
		// OPENC2E_SHOW_SCENT=name starts with View > Scents on that scent
		if (const char* sc = devenv("OPENC2E_SHOW_SCENT"))
			for (int ch = 0; ch < Pheromones::channelCount(); ch++)
				if (Pheromones::channelName(ch) == sc)
					world.showScent = ch;
		if (devenv("OPENC2E_FAST"))
			fastticks = true; // run the world as fast as possible (headless tests)
		// developer aid: OPENC2E_SPEED=n starts at that game speed
		if (const char* sp = devenv("OPENC2E_SPEED"))
			speed = std::clamp((float)atof(sp), 0.5f, 10.0f);
		int x0, y0, x1, y1;
		if (const char* s = devenv("OPENC2E_DUMP_ROOMS"); s && sscanf(s, "%d,%d,%d,%d", &x0, &y0, &x1, &y1) == 4) {
			for (unsigned int i = 0; i < world.map->getMetaRoomCount(); i++) {
				MetaRoom* mr = world.map->getMetaRoom(i);
				if (mr)
					fmt::print("* MetaRoom {}: x {} y {} size {}x{} (background {}x{}) wraps {}\n", i, mr->x(), mr->y(), mr->width(), mr->height(), mr->fullwidth(), mr->fullheight(), mr->wraparound());
			}
			for (unsigned int id = 0, found = 0; found < world.map->getRoomCount() && id < 100000; id++) {
				{
					auto r = world.map->getRoom(id);
					if (!r)
						continue;
					found++;
					if ((int)r->x_right < x0 || (int)r->x_left > x1 || (int)std::max(r->y_left_floor, r->y_right_floor) < y0 || (int)std::min(r->y_left_ceiling, r->y_right_ceiling) > y1)
						continue;
					fmt::print("* Room {} type {}: x {}-{}, ceiling {}/{}, floor {}/{}, floorvalue {}, dropstatus {}, floorpoints {}, temp {} lite {} radn {} pres {} ontr {} intr {} sources heat {} light {} radiation {} pressure {} wind {},{}\n",
						r->id, r->type, r->x_left, r->x_right, r->y_left_ceiling, r->y_right_ceiling,
						r->y_left_floor, r->y_right_floor, r->floorvalue, r->dropstatus, r->floorpoints.size(),
						r->temp, r->lite, r->radn, r->pres, r->ontr, r->intr, r->hsrc, r->lsrc, r->rsrc, r->psrc, r->windx, r->windy);
					if (r->floorpoints.size()) {
						std::string fp;
						for (auto& pt : r->floorpoints)
							fp += fmt::format(" ({}, {})", pt.first, pt.second);
						fmt::print("    floorpoints (x from left wall, height above room bottom):{}\n", fp);
					}
					for (auto& d : r->getDoors())
						fmt::print("    door to room {} (x {}-{}, floor {}/{}): perm {}\n", d.first->id,
							d.first->x_left, d.first->x_right, d.first->y_left_floor, d.first->y_right_floor, d.second.perm);
				}
			}
		}
	}

	static size_t snapshot_index = 0;
	static unsigned int snapshot_wait = 0;
	// OPENC2E_SNAPSHOT_EVERY=n takes the steps n frames apart (default 40),
	// for sequences of a creature moving; OPENC2E_SNAPSHOT_START=n waits n
	// frames before the first
	static const unsigned int snap_every = devenv("OPENC2E_SNAPSHOT_EVERY") ? std::max(1, atoi(devenv("OPENC2E_SNAPSHOT_EVERY"))) : 40;
	static unsigned int snap_start = devenv("OPENC2E_SNAPSHOT_START") ? atoi(devenv("OPENC2E_SNAPSHOT_START")) : 0;
	if (snap_start > 0 && snapshot_index < snapshots.size()) {
		snap_start--;
	} else if (snapshot_index < snapshots.size() && ++snapshot_wait % snap_every == 0) {
		if (devenv("OPENC2E_SNAPSHOTS_PAUSED"))
			world.paused = true; // freeze the world so captures differ only by camera
		auto& [sx, sy, sfile] = snapshots[snapshot_index];
		// OPENC2E_SNAPSHOT_ZOOM=z draws the snapshots at that zoom
		if (const char* z = devenv("OPENC2E_SNAPSHOT_ZOOM"))
			engine.zoom = (float)atof(z);
		// move the camera first whenever this snapshot is somewhere new
		static int camx = INT_MIN, camy = INT_MIN;
		// -2,-2: at a sleeping creature (found once, then stayed at)
		if (sx == -2 && sy == -2)
			for (auto& a : world.agents)
				if (auto* ca = a ? dynamic_cast<CreatureAgent*>(a.get()) : nullptr; ca && ca->getCreature() && ca->getCreature()->isAlive() && ca->getCreature()->isAsleep()) {
					for (auto& snap : snapshots)
						if (std::get<0>(snap) == -2)
							std::get<0>(snap) = std::max(0, (int)a->x - 375), std::get<1>(snap) = (int)a->y - 300;
					break;
				}
		if (sx == -2 && sy == -2)
			return false;
		// -3,-3: following a walking creature (OPENC2E_SNAPSHOT_GENUS=n:
		// of that genus; OPENC2E_SNAPSHOT_CARRYING=1: carrying something)
		if (sx == -3 && sy == -3) {
			const char* g = devenv("OPENC2E_SNAPSHOT_GENUS");
			for (auto& a : world.agents)
				if (auto* sc = dynamic_cast<SkeletalCreature*>(a.get()); sc && (sc->isWalking() || sc->isApproaching()) && sc->facing() >= 2 && !sc->invehicle && (!g || a->genus == atoi(g)) && (!devenv("OPENC2E_SNAPSHOT_CARRYING") || a->carrying)) {
					world.selectCreature(AgentRef(a).lock());
					camera->trackAgent(a, 0, 0, flexible, none);
					for (auto& snap : snapshots)
						if (std::get<0>(snap) == -3)
							std::get<0>(snap) = std::get<1>(snap) = -1;
					break;
				}
			if (sx == -3)
				return false;
		}
		if (sx == -1 && sy == -1) // -1,-1: wherever the view is
			camx = sx, camy = sy;
		if (sx != camx || sy != camy) {
			camera->trackAgent(AgentRef(), 0, 0, brittle, none);
			camera->moveToGlobal(sx, sy, jump);
			camx = sx;
			camy = sy;
		} else {
			get_backend()->requestScreenshot(sfile);
			dumpAgents();
			dumpRenders();
			snapshot_index++;
		}
	}

	// developer aid: OPENC2E_TEST_SAVE_AFTER_TICKS=N saves the world after N
	// ticks and quits (for testing world saving without the UI)
	static long test_save_after = devenv("OPENC2E_TEST_SAVE_AFTER_TICKS") ? atol(devenv("OPENC2E_TEST_SAVE_AFTER_TICKS")) : -1;
	if (test_save_after >= 0 && (long)world.tickcount >= test_save_after) {
		std::string err;
		if (!WorldSave::SaveWorldState(currentworlddir, err))
			fmt::print("* Test save failed: {}\n", err);
		test_save_after = -1;
		done = true;
	}
	// developer aid: OPENC2E_TEST_QUIT_AFTER_TICKS=N quits as CAOS's QUIT
	// does, saving the world (scripts/check_saves.py: the world must be
	// whole when it is saved)
	static long test_quit_after = devenv("OPENC2E_TEST_QUIT_AFTER_TICKS") ? atol(devenv("OPENC2E_TEST_QUIT_AFTER_TICKS")) : -1;
	if (test_quit_after >= 0 && (long)world.tickcount >= test_quit_after)
		world.quitting = true;
	// developer aid: OPENC2E_TEST_SWITCH_WORLD="N,NAME" switches to world
	// NAME after N ticks, as File > Switch World does (with
	// OPENC2E_TEST_SPAWN_RELAUNCH, by the Windows and macOS way)
	if (const char* sw = devenv("OPENC2E_TEST_SWITCH_WORLD")) {
		const char* comma = strchr(sw, ',');
		if (comma && currentworld != comma + 1 && world.tickcount >= (unsigned int)atoi(sw))
			Openc2eImgui::RelaunchIntoWorld(comma + 1);
	}

	assert(get_backend());

	// tick if necessary
	const unsigned int interval = tickInterval();
	bool needupdate = fastticks || !get_ticks_msec() || (get_ticks_msec() - tickdata >= (speed != 1.0f ? interval : world.ticktime - 5));
	if (needupdate && !world.paused) {
		if (!fastticks && speed > 1.0f) {
			// fast forward: ticks fall due faster than frames are drawn, so
			// run every tick that is due, keeping to the schedule, but no
			// more than about 40 ms of them before the next frame (a machine
			// that cannot keep up runs as fast as it can, and the view and
			// the hand stay responsive)
			unsigned int t0 = get_ticks_msec();
			unsigned int sched = tickdata + interval;
			for (int n = 0; n < 50; n++) {
				update(); // (sets tickdata to now)
				unsigned int now = get_ticks_msec();
				tickdata = sched; // this tick's due time: the next is due one interval on
				sched += interval;
				if (now < sched)
					break; // caught up
				if (now - sched > 4 * interval) {
					// far behind: let the schedule go rather than spiral
					tickdata = now;
					break;
				}
				if (now - t0 > 40 || world.paused || done)
					break;
			}
		} else if (fastticks) {
			using clock = std::chrono::steady_clock;
			using std::chrono::duration_cast;
			using std::chrono::milliseconds;
			auto start = clock::now();
			while (duration_cast<milliseconds>(clock::now() - start).count() < 1000 / world.ticktime) {
				update();
			}
		} else {
			update();
		}
	}

	processEvents();
	if (needupdate)
		handleKeyboardScrolling();

	return needupdate;
}

void Engine::handleKeyboardScrolling() {
	// keyboard-based scrolling (not while a box in a window is typed in: its
	// arrow keys are its own)
	if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput)
		return;
	static float accelspeed = 8, decelspeed = .5, maxspeed = 64;
	static float velx = 0;
	static float vely = 0;

	bool wasdMode = false;
	caosValue v = world.variables["engine_wasd"];
	if (v.hasInt()) {
		switch (v.getInt()) {
			case 1: // enable if CTRL is held
				wasdMode = get_backend()->keyDown(OPENC2E_KEY_CTRL);
				break;
			case 2: // enable unconditionally
				// (this needs agent support to suppress chat bubbles etc)
				wasdMode = true;
				break;
			case 0: // disable
				wasdMode = false;
				break;
			default: // disable
				fmt::print("Warning: engine_wasd_scrolling is set to unknown value {}\n", v.getInt());
				world.variables["engine_wasd_scrolling"] = caosValue(0);
				wasdMode = false;
				break;
		}
	}

	// check keys
	bool leftdown = get_backend()->keyDown(OPENC2E_KEY_LEFT) || (wasdMode && a_down);
	bool rightdown = get_backend()->keyDown(OPENC2E_KEY_RIGHT) || (wasdMode && d_down);
	bool updown = get_backend()->keyDown(OPENC2E_KEY_UP) || (wasdMode && w_down);
	bool downdown = get_backend()->keyDown(OPENC2E_KEY_DOWN) || (wasdMode && s_down);

	if (leftdown)
		velx -= accelspeed;
	if (rightdown)
		velx += accelspeed;
	if (!leftdown && !rightdown) {
		velx *= decelspeed;
		if (fabs(velx) < 0.1)
			velx = 0;
	}
	if (updown)
		vely -= accelspeed;
	if (downdown)
		vely += accelspeed;
	if (!updown && !downdown) {
		vely *= decelspeed;
		if (fabs(vely) < 0.1)
			vely = 0;
	}

	// enforced maximum speed
	if (velx >= maxspeed)
		velx = maxspeed;
	else if (velx <= -maxspeed)
		velx = -maxspeed;
	if (vely >= maxspeed)
		vely = maxspeed;
	else if (vely <= -maxspeed)
		vely = -maxspeed;

	// do the actual movement
	if (velx || vely) {
		// C1/C2: moving the view stops following, as the wheel does (owner,
		// 2026-11-22); not while the view only glides on after the key
		if (version <= 2 && (leftdown || rightdown || updown || downdown))
			camera->trackAgent(AgentRef(), 0, 0, brittle, none);
		int adjustx = engine.camera->getX(), adjusty = engine.camera->getY();
		int adjustbyx = (int)velx, adjustbyy = (int)vely;

		engine.camera->moveTo(adjustx + adjustbyx, adjusty + adjustbyy, jump);
	}
}

void Engine::processEvents() {
	net->handleEvents();

	BackendEvent event;
	while (get_backend()->pollEvent(event)) {
		switch (event.type) {
			case eventresizewindow:
				handleResizedWindow();
				break;

			case eventmousemove:
				handleMouseMove(event);
				break;

			case eventpinch: {
				// a two-finger pinch zooms about the hand, as Ctrl+wheel does
				// (owner, 2026-11-15): a zoom step for each half again of
				// spread (or a third of squeeze), at most one step each quarter
				// second (owner: a step per fifth "moves way too fast");
				// OPENC2E_PINCH_STEP=s sets the spread for a step (1.5)
				static float acc = 0;
				static uint32_t lastStep = 0;
				static const float step = std::log(std::max(1.05f, devenv("OPENC2E_PINCH_STEP") ? (float)atof(devenv("OPENC2E_PINCH_STEP")) : 1.5f));
				if (version <= 2 && event.pinch > 0) {
					acc += std::log(event.pinch);
					if (std::fabs(acc) >= step && get_ticks_msec() - lastStep >= 250) {
						int dir = acc > 0 ? 1 : -1;
						acc = 0; // (what was pinched beyond the step is let go)
						lastStep = get_ticks_msec();
						zoomStep(dir, (int)std::lround(handViewX(camera.get(), world.hand()) * zoom),
							(int)std::lround((world.hand()->pointerY() - (int)camera->getY()) * zoom));
					}
					acc = std::clamp(acc, -step, step);
				}
				break;
			}
			case eventmousebuttonup:
			case eventmousebuttondown:
				handleMouseButton(event);
				break;

			case eventtextinput:
				handleTextInput(event);
				break;

			case eventrawkeydown:
				handleRawKeyDown(event);
				break;

			case eventrawkeyup:
				handleRawKeyUp(event);
				break;

			case eventquit:
				done = true;
				break;

			default:
				break;
		}
	}
}

void Engine::handleResizedWindow() {
	// notify agents
	for (auto& a : world.agents) {
		if (!a)
			continue;
		a->queueScript(123, 0); // window resized script
	}
}

void Engine::handleMouseMove(BackendEvent& event) {
	// move the cursor
	world.hand()->handleEvent(event);

	// notify agents
	for (std::list<std::shared_ptr<Agent>>::iterator i = world.agents.begin(); i != world.agents.end(); i++) {
		if (!*i)
			continue;
		if ((*i)->imsk_mouse_move) {
			caosValue x;
			x.setFloat(world.hand()->pointerX());
			caosValue y;
			y.setFloat(world.hand()->pointerY());
			(*i)->queueScript(75, 0, x, y); // Raw Mouse Move
		}
	}
}

void Engine::handleMouseButton(BackendEvent& event) {
	// C1/C2: wheel and two-finger touchpad scrolling pan the world view in any
	// direction (their scripts have no use for the wheel). Directions follow
	// the system's natural-scrolling setting, as SDL reports them.
	if (version <= 2 && event.type == eventmousebuttondown && (event.wheel_x != 0 || event.wheel_y != 0)) {
		// with Ctrl held, the wheel zooms about the pointer instead
		if (get_backend()->keyDown(OPENC2E_KEY_CTRL)) {
			static float acc = 0;
			acc += event.wheel_y;
			if (std::fabs(acc) >= 1.0f) {
				zoomStep(acc > 0 ? 1 : -1, (int)std::lround(handViewX(camera.get(), world.hand()) * zoom),
					(int)std::lround((world.hand()->pointerY() - (int)camera->getY()) * zoom));
				acc = 0;
			}
			return;
		}
		const float speed = 51.0f; // pixels per wheel unit (owner-tuned)
		int dx = (int)std::lround(event.wheel_x * speed);
		int dy = (int)std::lround(-event.wheel_y * speed);
		if (dx != 0 || dy != 0) {
			camera->trackAgent(AgentRef(), 0, 0, brittle, none);
			camera->moveTo((int)camera->getX() + dx, (int)camera->getY() + dy);
		}
		return;
	}

	// notify agents
	for (auto& agent : world.agents) {
		if (!agent)
			continue;
		if ((event.type == eventmousebuttonup && agent->imsk_mouse_up) ||
			(event.type == eventmousebuttondown && agent->imsk_mouse_down)) {
			// set the button value as necessary
			caosValue button;
			switch (event.button) { // Backend guarantees that only one button will be set on a mousebuttondown event.
				// the values here make fuzzie suspicious that c2e combines these events
				// nornagon seems to think c2e doesn't
				case buttonleft: button.setInt(1); break;
				case buttonright: button.setInt(2); break;
				case buttonmiddle: button.setInt(4); break;
				default: break;
			}

			// if it was a mouse button we're interested in, then fire the relevant raw event
			if (button.getInt() != 0) {
				if (event.type == eventmousebuttonup)
					agent->queueScript(77, 0, button); // Raw Mouse Up
				else
					agent->queueScript(76, 0, button); // Raw Mouse Down
			}
		}
		if ((event.type == eventmousebuttondown &&
				(event.button == buttonwheelup || event.button == buttonwheeldown) &&
				agent->imsk_mouse_wheel)) {
			// fire the mouse wheel event with the relevant delta value
			caosValue delta;
			if (event.button == buttonwheeldown)
				delta.setInt(-120);
			else
				delta.setInt(120);
			agent->queueScript(78, 0, delta); // Raw Mouse Wheel
		}
	}

	world.hand()->handleEvent(event);
}

void Engine::handleTextInput(BackendEvent& event) {
	std::string cp1252_text;
	try {
		cp1252_text = utf8_to_cp1252(event.text);
	} catch (std::domain_error& e) {
		// ignore, will be printed next
	}
	if (cp1252_text.size() != 1) {
		fmt::print(stderr, "bad text input: ");
		for (unsigned char c : event.text) {
			fmt::print(stderr, "0x{:02x} ", c);
		}
		fmt::print(stderr, "\n");
	}

	int translated_char = cp1252_text[0];

	if (cp1252_isprint(translated_char)) {
		if (version < 3 && !world.focusagent) {
			Bubble::newBubble(world.hand(), false, std::string());
		}
	}

	// tell the agent with keyboard focus (only characters that print: some
	// systems send Backspace as a character too, "\b", after its key, and
	// the hand's typing showed it as a block where the letter had been; a
	// player, 2026-11-22)
	if (world.focusagent && cp1252_isprint(translated_char)) {
		CompoundPart* t = world.focusagent.get()->part(world.focuspart);
		if (t && t->canGainFocus())
			t->handleTranslatedChar(translated_char);
	}

	// notify agents
	caosValue k;
	k.setInt(translated_char);
	for (auto& agent : world.agents) {
		if (!agent)
			continue;
		if (agent->imsk_translated_char)
			agent->queueScript(79, 0, k); // translated char script
	}
}

void Engine::handleRawKeyUp(BackendEvent& event) {
	switch (event.key) {
		case OPENC2E_KEY_W:
			w_down = false;
			break;
		case OPENC2E_KEY_A:
			a_down = false;
			break;
		case OPENC2E_KEY_S:
			s_down = false;
			break;
		case OPENC2E_KEY_D:
			d_down = false;
			break;
		default:
			break;
	}
}

void Engine::handleRawKeyDown(BackendEvent& event) {
	// C1/C2: Ctrl+0 shows the world at its actual size
	if (version <= 2 && event.key == OPENC2E_KEY_0 && get_backend()->keyDown(OPENC2E_KEY_CTRL))
		setZoom(1.0f);
	// C1/C2: F12 switches full screen on and off
	if (version <= 2 && event.key == OPENC2E_KEY_F12)
		get_backend()->setFullscreen(!get_backend()->isFullscreen());
	// C2: Ctrl+S says the last thing typed again (its manual: "CTRL and S:
	// Repeat the last piece of text you typed in"; a player, 2026-11-22), at
	// most twice a second (a key held down repeats)
	if (version == 2 && event.key == OPENC2E_KEY_S && get_backend()->keyDown(OPENC2E_KEY_CTRL)) {
		static unsigned int said = 0;
		if (world.tickcount - said >= 5 || world.tickcount < said) {
			said = world.tickcount;
			handSaysAgain();
		}
		return;
	}
	switch (event.key) {
		case OPENC2E_KEY_W:
			w_down = true;
			break;
		case OPENC2E_KEY_A:
			a_down = true;
			break;
		case OPENC2E_KEY_S:
			s_down = true;
			break;
		case OPENC2E_KEY_D:
			d_down = true;
			break;
		default:
			break;
	}

	// handle debug keys, if they're enabled
	caosValue v = world.variables["engine_debug_keys"];
	if (v.hasInt() && v.getInt() == 1) {
		if (get_backend()->keyDown(OPENC2E_KEY_SHIFT)) {
			MetaRoom* n; // for pageup/pagedown

			switch (event.key) {
				case OPENC2E_KEY_INSERT:
					world.showrooms = !world.showrooms;
					break;

				case OPENC2E_KEY_PAUSE:
					// TODO: debug pause game
					break;

				case OPENC2E_KEY_SPACE:
					// TODO: force tick
					break;

				case OPENC2E_KEY_PAGEUP:
					// TODO: previous metaroom
					if ((world.map->getMetaRoomCount() - 1) == engine.camera->getMetaRoom()->id)
						break;
					n = world.map->getMetaRoom(engine.camera->getMetaRoom()->id + 1);
					if (n)
						engine.camera->goToMetaRoom(n->id);
					break;

				case OPENC2E_KEY_PAGEDOWN:
					// TODO: next metaroom
					if (engine.camera->getMetaRoom()->id == 0)
						break;
					n = world.map->getMetaRoom(engine.camera->getMetaRoom()->id - 1);
					if (n)
						engine.camera->goToMetaRoom(n->id);
					break;

				default: break; // to shut up warnings
			}
		}
	}

	// tell the agent with keyboard focus
	if (world.focusagent) {
		CompoundPart* t = world.focusagent.get()->part(world.focuspart);
		if (t && t->canGainFocus())
			t->handleRawKey(event.key);
	}

	// notify agents
	caosValue k;
	k.setInt(event.key);
	for (auto& agent : world.agents) {
		if (!agent)
			continue;
		if (agent->imsk_key_down)
			agent->queueScript(73, 0, k); // key down script
	}

	// certain raw keys get passed as translated chars, too, after the raw key event
	// these correspond to the CP1252/ASCII control codes
	// TODO: should this be handled in Backend instead?
	BackendEvent translatedevent;
	translatedevent.type = eventtextinput;
	switch (event.key) {
		case OPENC2E_KEY_BACKSPACE:
			translatedevent.text = "\x08";
			break;
		case OPENC2E_KEY_TAB:
			translatedevent.text = "\t";
			break;
		case OPENC2E_KEY_RETURN:
			translatedevent.text = "\n";
			break;
		case OPENC2E_KEY_ESCAPE:
			translatedevent.text = "\x1b";
			break;
		case OPENC2E_KEY_DELETE:
			translatedevent.text = "\x7f";
			break;
		default:
			break;
	}
	if (translatedevent.text.size()) {
		handleTextInput(translatedevent);
	}
}

static const char data_default[] = "./data";

static void opt_version() {
	// We already showed the primary version bit, just throw in some random legalese
	fmt::print("This is free software; see the source for copying conditions.  There is NO\n"
			   "warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n"
			   "\n"
			   "...please don't sue us.\n");
}

static std::string detectGameType(fs::path directory) {
	if (case_insensitive_filesystem::exists(directory / "creatures.exe")) {
		return "c1";
	}
	if (case_insensitive_filesystem::exists(directory / "creatures2.exe")) {
		return "c2";
	}

	auto catalogue_directory = directory / "Catalogue";
	std::error_code err;
	auto machine_cfg_filename = case_insensitive_filesystem::canonical(directory / "machine.cfg", err);
	if (!err) {
		auto machine_cfg = readcfgfile(machine_cfg_filename);
		if (machine_cfg.count("Catalogue Directory")) {
			catalogue_directory = machine_cfg["Catalogue Directory"];
			if (!catalogue_directory.is_absolute()) {
				catalogue_directory = directory / catalogue_directory;
			}
		}
	}

	if (case_insensitive_filesystem::exists(catalogue_directory / "Docking Station.catalogue")) {
		return "ds";
	}
	if (case_insensitive_filesystem::exists(catalogue_directory / "Creatures 3.catalogue")) {
		return "c3";
	}
	if (case_insensitive_filesystem::exists(catalogue_directory / "Creatures Adventures.catalogue")) {
		return "cv";
	}
	if (case_insensitive_filesystem::exists(catalogue_directory / "Sea Monkeys.catalogue")) {
		return "sm";
	}
	throw_exception("Couldn't auto-detect game type from data path {:?}", directory.string());
}

static std::vector<DataDirectory> data_directories_from_machine_cfg(fs::path machine_cfg_filename) {
	auto parent_directory = machine_cfg_filename.parent_path();
	std::error_code err;
	machine_cfg_filename = case_insensitive_filesystem::canonical(machine_cfg_filename, err);
	if (err) {
		return {parent_directory};
	}

	std::vector<DataDirectory> result;
	auto normalize = [&](fs::path s) {
		if (s.empty()) {
			return s;
		}
		if (s.is_absolute()) {
			return s.lexically_normal();
		}
		return fs::absolute((parent_directory / s).lexically_normal());
	};
	auto machine_cfg = readcfgfile(machine_cfg_filename);
	if (!machine_cfg.count("Main Directory")) {
		throw Exception("machine.cfg missing \"Main Directory\"");
	}
	DataDirectory dir(normalize(machine_cfg["Main Directory"]));
	dir.backgrounds = normalize(machine_cfg["Backgrounds Directory"]);
	dir.body_data = normalize(machine_cfg["Body Data Directory"]);
	dir.bootstrap = normalize(machine_cfg["Bootstrap Directory"]);
	dir.catalogue = normalize(machine_cfg["Catalogue Directory"]);
	dir.creature_galleries = normalize(machine_cfg["Creature Galleries Directory"]);
	dir.exported_creatures = normalize(machine_cfg["Exported Creatures Directory"]);
	dir.genetics = normalize(machine_cfg["Genetics Directory"]);
	dir.images = normalize(machine_cfg["Images Directory"]);
	dir.journal = normalize(machine_cfg["Journal Directory"]);
	dir.overlay_data = normalize(machine_cfg["Overlay Data Directory"]);
	dir.agents = normalize(machine_cfg["Resource Files Directory"]);
	dir.sounds = normalize(machine_cfg["Sounds Directory"]);
	dir.users = normalize(machine_cfg["Users Directory"]);
	dir.worlds = normalize(machine_cfg["Worlds Directory"]);
	result.push_back(dir);

	for (int i = 1;; ++i) {
		std::string prefix = fmt::format("Auxiliary {} ", i);
		if (!find_if(machine_cfg, [&](auto& kv) { return kv.first.rfind(prefix) == 0; })) {
			break;
		}
		if (!machine_cfg.count(prefix + "Main Directory")) {
			throw Exception(fmt::format("machine.cfg missing \"{}Main Directory\"", prefix));
		}
		DataDirectory dir(normalize(machine_cfg[prefix + "Main Directory"]));
		dir.backgrounds = normalize(machine_cfg[prefix + "Backgrounds Directory"]);
		dir.body_data = normalize(machine_cfg[prefix + "Body Data Directory"]);
		dir.bootstrap = normalize(machine_cfg[prefix + "Bootstrap Directory"]);
		dir.catalogue = normalize(machine_cfg[prefix + "Catalogue Directory"]);
		dir.creature_galleries = normalize(machine_cfg[prefix + "Creature Galleries Directory"]);
		dir.exported_creatures = normalize(machine_cfg[prefix + "Exported Creatures Directory"]);
		dir.genetics = normalize(machine_cfg[prefix + "Genetics Directory"]);
		dir.images = normalize(machine_cfg[prefix + "Images Directory"]);
		dir.journal = normalize(machine_cfg[prefix + "Journal Directory"]);
		dir.overlay_data = normalize(machine_cfg[prefix + "Overlay Data Directory"]);
		dir.agents = normalize(machine_cfg[prefix + "Resource Files Directory"]);
		dir.sounds = normalize(machine_cfg[prefix + "Sounds Directory"]);
		dir.users = normalize(machine_cfg[prefix + "Users Directory"]);
		dir.worlds = normalize(machine_cfg[prefix + "Worlds Directory"]);
		result.push_back(dir);
	}
	return result;
}


bool Engine::parseCommandLine(int argc, char* argv[]) {
	// variables for command-line flags
	std::vector<fs::path> data_vec;

	// generate help for backend options
	std::string available_backends;
	for (auto& possible_backend : possible_backends) {
		if (available_backends.empty())
			available_backends = possible_backend.first;
		else
			available_backends += ", " + possible_backend.first;
	}
	available_backends = "Select the backend (options: " + available_backends + "); default is " + preferred_backend;

	std::string available_audiobackends;
	for (auto& possible_audiobackend : possible_audiobackends) {
		if (available_audiobackends.empty())
			available_audiobackends = possible_audiobackend.first;
		else
			available_audiobackends += ", " + possible_audiobackend.first;
	}
	available_audiobackends = "Select the audio backend (options: " + available_audiobackends + "); default is " + preferred_audiobackend;

	// parse the command-line flags
	cxxopts::Options desc("openc2e", "");
	desc.add_options()("h,help", "Display help on command-line options");
	desc.add_options()("V,version", "Display openc2e version");
	desc.add_options()("s,silent", "Disable all sounds");
	desc.add_options()("l,language", "Select the language; default is '" + language + "'", cxxopts::value<std::string>(language));
	desc.add_options()("k,backend", available_backends, cxxopts::value<std::string>(preferred_backend));
	desc.add_options()("o,audiobackend", available_audiobackends, cxxopts::value<std::string>(preferred_audiobackend));
	desc.add_options()("d,data-path", "Sets or adds a path to a data directory", cxxopts::value<std::vector<fs::path>>(data_vec));
	desc.add_options()("b,bootstrap", "Sets or adds a path or COS file to bootstrap from", cxxopts::value<std::vector<std::string>>(cmdline_bootstrap));
	desc.add_options()("m,gamename", "Set the game name", cxxopts::value<std::string>(gamename));
	desc.add_options()("n,norun", "Don't run the game, just execute scripts");
	desc.add_options()("a,autokill", "Enable autokill");
	desc.add_options()("autostop", "Enable autostop (or disable it, for CV)");
	desc.add_options()("world", "C1/C2: open the named world from the Worlds folder (skips the world chooser)", cxxopts::value<std::string>(cmdline_world));
	desc.add_options()("no-autosave", "C1/C2: don't save the world automatically on quit or when switching worlds");
	desc.add_options()("no-world-chooser", "C1/C2: don't show the world chooser; load the game's Eden.sfc as before");
	desc.add_options()("mind", "The creature mind for C1/C2 creatures (options: " + availableCreatureMinds() + ", original); default homeostat, Zeetures's own; original is openc2e's unfinished copy of the original brain", cxxopts::value<std::string>(mindname));
	for (int i = 0; i < argc; i++)
		cmdline_args.push_back(argv[i]);
	auto vm = desc.parse(argc, argv);
	cmdline_no_world_chooser = vm.count("no-world-chooser");
	autosave = !vm.count("no-autosave");
	cmdline_enable_sound = !vm.count("silent");
	cmdline_norun = vm.count("norun");

	if (vm.count("help")) {
		fmt::print("{}\n", desc.help());
		return false;
	}

	if (vm.count("version")) {
		opt_version();
		return false;
	}

	if (vm.count("autokill")) {
		world.autokill = true;
	}

	if (vm.count("autostop")) {
		world.autostop = true;
	}

	// Zeetures: its own mind unless the player asks otherwise (owner,
	// 2026-11-15: the same world as Home's on any machine)
	if (mindname.empty())
		mindname = "homeostat";
	if (mindname == "original") {
		fmt::print("* Creature mind: original brain code\n");
	} else {
		if (!createCreatureMind(mindname)) {
			fmt::print(stderr, "Unknown creature mind '{}' (options: {})\n", mindname, availableCreatureMinds());
			return false;
		}
		fmt::print("* Creature mind: {}\n", mindname);
	}

	// Zeetures (owner, 2026-11-20): no command line needed. Without -d, the
	// remembered or usual Creatures 2 folder; then ./data, as in openc2e,
	// if it holds a game; then a window asks the player
	bool data_from_cmdline = !data_vec.empty();
	if (data_vec.empty()) {
		fs::path found = GameFolder::find();
		if (found.empty()) {
			fs::path d = data_default;
			for (auto* marker : {"creatures2.exe", "creatures.exe", "machine.cfg", "Catalogue"}) {
				if (case_insensitive_filesystem::exists(d / marker)) {
					fmt::print("* Using the data folder {:?}\n", data_default);
					found = d;
					break;
				}
			}
		}
		if (found.empty())
			found = GameFolder::ask();
		if (found.empty()) {
			fmt::print("No Creatures 2 folder chosen; see --help to give one with -d.\n");
			return false;
		}
		data_vec.push_back(found);
	}
	GameFolder::setCurrent(data_vec[0], data_from_cmdline);

	// check first data directory exists
	if (!fs::exists(data_vec[0])) {
		throw_exception("data path {:?} not found", data_vec[0].string());
	}

	// detect game type from first data directory
	gametype = detectGameType(data_vec[0]);
	fmt::print("* Detected game type: {}\n", gametype);

	// set engine version
	// TODO: set gamename
	if (gametype == "c1") {
		if (gamename.empty())
			gamename = "Creatures 1";
		version = 1;
	} else if (gametype == "c2") {
		if (gamename.empty())
			gamename = "Creatures 2";
		version = 2;
	} else if (gametype == "c3") {
		if (gamename.empty()) {
			gamename = "Creatures 3";
		}
		version = 3;
	} else if (gametype == "ds") {
		gametype = "c3";
		if (gamename.empty()) {
			gamename = "Docking Station";
		}
		version = 3;
	} else if (gametype == "cv") {
		if (gamename.empty())
			gamename = "Creatures Village";
		version = 3;
		world.autostop = !world.autostop;
	} else if (gametype == "sm") {
		if (gamename.empty())
			gamename = "Sea-Monkeys";
		version = 3;
		bmprenderer = true;
	} else
		throw Exception(fmt::format("unknown gametype '{}'!", gametype));

	// try to read machine.cfg
	if (!fs::exists(data_vec[0])) {
		throw_exception("data path {:?} doesn't exist", data_vec[0].string());
	}
	if (engine.version == 3) {
		data_directories = data_directories_from_machine_cfg(fs::path(data_vec[0]) / "machine.cfg");
	} else {
		data_directories.push_back(fs::path(data_vec[0]));
	}

	// add remaining data directories
	for (auto it = data_vec.begin() + 1; it != data_vec.end(); ++it) {
		if (!fs::exists(*it)) {
			throw_exception("data path {:?} doesn't exist", it->string());
		}
		if (find_if(data_directories, [&](auto d) { return fs::absolute(d.main) == fs::absolute(*it); })) {
			fmt::print("* Warning: ignoring duplicate data directory {}", it->string());
			continue;
		}
		data_directories.push_back(fs::path(*it));
	}

	// make a vague attempt at blacklisting some characters inside the gamename
	// (it's used in directory names, registry keys, etc)
	std::string invalidchars = "\\/:*?\"<>|";
	for (char invalidchar : invalidchars) {
		if (gamename.find(invalidchar) != gamename.npos)
			throw Exception(std::string("The character ") + invalidchar + " is not valid in a gamename.");
	}

	return true;
}

void Engine::injectMissingBuiltinCobs() {
	if (missingBuiltinCobs.empty())
		return;
	CobManager cobs;
	cobs.update();
	std::vector<std::string> still;
	for (auto& name : missingBuiltinCobs) {
		auto it = std::find_if(cobs.objects.begin(), cobs.objects.end(), [&](auto& o) { return o.name == name; });
		if (it == cobs.objects.end()) {
			still.push_back(name);
			continue;
		}
		try {
			cobs.inject(*it);
			fmt::print("* Built-in object \"{}\" injected (its COB arrived)\n", name);
		} catch (std::exception& e) {
			fmt::print("* Built-in object \"{}\" failed: {}\n", name, e.what());
		}
	}
	missingBuiltinCobs = still;
}

// C1/C2: patches to the world's own scripts, applied on the player's
	// machine to the player's own copy (the project ships only the recipe):
	// "script-patches.txt" in the Additions folders, in blocks of
	//   scrp <family> <genus> <species> <event>
	//   find <text in the script>
	//   replace <text to put there>
	// Lines starting with * or # are notes. A patch whose text is not found
	// (already applied, or a different game version) is reported and skipped.

// whether the open world started from the game's own world file (Eden.sfc;
// Home and worlds made from it hold a byte-for-byte copy as world.sfc)
static bool worldStartedFromEden(const fs::path& worlddir) {
	if (worlddir.empty())
		return true;
	fs::path mine = worlddir / "world.sfc";
	fs::path eden = findMainDirectoryFile("Eden.sfc");
	std::error_code ec;
	if (eden.empty() || !fs::exists(mine, ec) || !fs::exists(eden, ec))
		return true; // (nothing to compare: as before)
	if (fs::file_size(mine, ec) != fs::file_size(eden, ec))
		return false;
	std::ifstream a(mine, std::ios::binary), b(eden, std::ios::binary);
	std::istreambuf_iterator<char> ia(a), ib(b), end;
	for (; ia != end && ib != end; ++ia, ++ib)
		if (*ia != *ib)
			return false;
	return ia == end && ib == end;
}

// an addition marked "* any world" in its opening comment lines
static bool anyWorldAddition(const std::string& text) {
	size_t at = 0;
	while (at < text.size()) {
		size_t eol = text.find('\n', at);
		std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
		size_t first = line.find_first_not_of(" \t\r");
		if (first == std::string::npos) {
			// (blank lines between comments)
		} else if (line[first] != '*')
			return false;
		else if (line.find("* any world") != std::string::npos)
			return true;
		if (eol == std::string::npos)
			break;
		at = eol + 1;
	}
	return false;
}

void Engine::applyScriptPatches(bool quiet) {
	if (version < 3) {
		// the player's own (or the built-in) one, then the world folder's
		std::vector<std::string> texts{Additions::text("script-patches.txt")};
		if (!currentworlddir.empty()) {
			std::ifstream w(fs::path(currentworlddir) / "Additions" / "script-patches.txt", std::ios::binary);
			std::ostringstream ws;
			ws << w.rdbuf();
			texts.push_back(ws.str());
		}
		for (auto& patchText : texts) {
			if (patchText.empty())
				continue;
			std::istringstream in(patchText);
			std::string line;
			int f = -1, g = -1, sp = -1, ev = -1;
			std::string find;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty() || line[0] == '*' || line[0] == '#')
					continue;
				std::istringstream l(line);
				std::string key;
				l >> key;
				std::string rest;
				std::getline(l >> std::ws, rest);
				if (key == "scrp") {
					std::istringstream r(rest);
					r >> f >> g >> sp >> ev;
					find.clear();
				} else if (key == "find") {
					find = rest;
				} else if (key == "replace" && f >= 0 && !find.empty()) {
					auto sc = world.scriptorium->getScript(f, g, sp, ev);
					size_t at = sc ? sc->code.find(find) : std::string::npos;
					if (at == std::string::npos) {
						// nothing to say when the patch is already in (this
						// load's COB injections apply the patches as they go;
						// a replacement never contains its find text), was
						// applied earlier this load and changed again by a
						// later patch of the same script, or is for a script
						// this world doesn't have (an add-on not installed)
						if (!sc || sc->code.find(rest) != std::string::npos || patchesApplied.count(fmt::format("{} {} {} {}|{}", f, g, sp, ev, find)))
							continue;
						if (!quiet)
							fmt::print("* Script patch for scrp {} {} {} {}: text not found (already patched, or another version); skipped\n", f, g, sp, ev);
						continue;
					}
					std::string code = sc->code;
					code.replace(at, find.size(), rest);
					std::string text = fmt::format("scrp {} {} {} {} {}", f, g, sp, ev, code);
					if (text.find("endm") == std::string::npos)
						text += " endm";
					try {
						caosScript patched(engine.gametype, "script-patches.txt");
						patched.parse(text);
						caosVM vm(0);
						patched.installScripts();
						vm.runEntirely(patched.installer);
						patchesApplied.insert(fmt::format("{} {} {} {}|{}", f, g, sp, ev, find));
						fmt::print("* Patched scrp {} {} {} {} ({})\n", f, g, sp, ev, "script-patches.txt");
					} catch (Exception& e) {
						fmt::print(stderr, "* Script patch for scrp {} {} {} {} failed: {}\n", f, g, sp, ev, e.prettyPrint());
					}
				}
			}
		}
	}
}

bool Engine::initialSetup() {
	assert(data_directories.size() > 0);

	// finally, add our cache directory to the end
	data_directories.push_back(storageDirectory());

	// initialize backends
	if (cmdline_norun)
		preferred_backend = "null";
	if (preferred_backend != "null")
		fmt::print("* Initialising backend {}...\n", preferred_backend);
	Backend* b = possible_backends[preferred_backend];
	if (!b)
		throw Exception("No such backend " + preferred_backend);
	int window_width = OPENC2E_DEFAULT_WIDTH, window_height = OPENC2E_DEFAULT_HEIGHT;
	if (const char* size = getenv("OPENC2E_WINDOW_SIZE")) // developer aid, e.g. 1200x950
		sscanf(size, "%dx%d", &window_width, &window_height);
	b->init(
		version == 2 ? std::string("Zeetures") : gamename.size() ? gamename + " - openc2e (development build)" : "openc2e (development build)",
		window_width,
		window_height);
	setBackend(b);
	possible_backends.clear();
	Openc2eImGui::Init();

	// -s (silent): no audio device at all. Only muting at the start was not
	// enough: the player's own sound choices in settings.txt are loaded
	// later and win, so headless test copies (-s) played creature voices
	// and world sounds through the speakers (2026-11-16)
	if (cmdline_norun || !cmdline_enable_sound)
		preferred_audiobackend = "null";
	if (preferred_audiobackend != "null")
		fmt::print("* Initialising audio backend {}...\n", preferred_audiobackend);
	AudioBackend* a = possible_audiobackends[preferred_audiobackend];
	if (!a)
		throw Exception("No such audio backend " + preferred_audiobackend);
	try {
		a->init();
		set_audio_backend(a);
	} catch (Exception& e) {
		fmt::print("* Couldn't initialize backend {}: {}\n", preferred_audiobackend, e.what());
		fmt::print("* Continuing without sound.\n");
		set_audio_backend(NullAudioBackend::get_instance());
		get_audio_backend()->init();
	}
	possible_audiobackends.clear();

	net = std::make_shared<NetBackend>();
	int listenport = net->init();
	if (listenport != -1) {
		// inform the user of the port used, and store it in the relevant file
		fmt::print("* Listening for connections on port {}.\n", listenport);
#ifndef _WIN32
		fs::path p = homeDirectory() / ".creaturesengine";
		if (!fs::exists(p))
			fs::create_directory(p);
		if (fs::is_directory(p)) {
			FileWriter f(p / "port");
			fmt::print(f, "{}", listenport);
		}
#endif
	}

	// load palette for C1
	world.gallery->loadDefaultPalette();

	// audio
	musicmanager = std::make_unique<MusicManager>();
	// Zeetures (owner, 2026-11-15: "new compiles start with all sounds
	// on"): music, world sounds and creature speech all begin on, unless
	// sound is disabled on the command line (-s); the player's own choices
	// from File > Sound are kept in settings.txt and win (C2ToolBar's
	// LoadViewSettings). Until now they all began muted (2a793d9b).
	const bool quiet = !cmdline_enable_sound;
	soundmanager.setMuted(quiet);
	soundmanager.setVoicesMutedByPlayer(quiet);
	musicmanager->setMuted(quiet);
	musicmanager->setMIDIMuted(quiet);

	// initial setup
	if (engine.version == 3) {
		fmt::print("* Reading catalogue files...\n");
		world.initCatalogue();
	}
	fmt::print("* Initial setup...\n");
	world.init(); // just reads mouse cursor (we want this after the catalogue reading so we don't play "guess the filename")
	if (engine.version > 2) {
		fmt::print("* Reading PRAY files...\n");
		world.praymanager->update();
	}

#ifdef _WIN32
	// Here we need to set the working directory since apparently windows != clever
	char exepath[MAX_PATH] = "";
	GetModuleFileName(0, exepath, sizeof(exepath) - 1);
	char* exedir = strrchr(exepath, '\\');
	if (exedir) {
		// null terminate the string
		*exedir = 0;
		// Set working directory
		SetCurrentDirectory(exepath);
	} else // err, oops
		fmt::print(stderr, "Warning: Setting working directory to {} failed.\n", exepath);
#endif

	loadGameData();

	// execute the initial scripts!
	// C1/C2 worlds: open the requested world, or let the player choose one
	// (unless a world file was given with -b, or the chooser is turned off)
	if (version < 3 && cmdline_bootstrap.empty() && (!cmdline_world.empty() || !cmdline_no_world_chooser)) {
		std::string name = cmdline_world.empty() ? Openc2eImgui::RunWorldChooser() : cmdline_world;
		if (name.empty()) {
			shutdown();
			return false;
		}
		auto w = Openc2eImgui::FindWorld(name);
		if (!w) {
			fmt::print(stderr, "No world named '{}' in {}\n", name, Openc2eImgui::GetWorldsDirectory().string());
			shutdown();
			return false;
		}
		currentworld = w->name;
		currentworlddir = w->dir;
		cmdline_bootstrap = {Openc2eImgui::WorldFile(w->dir).string()};
		fmt::print("* World: {}\n", currentworld);
		// (C2: this fork's name, Zeetures; owner, 2026-11-15)
		get_backend()->setWindowTitle(currentworld + " - " + (version == 1 ? "Creatures" : "Zeetures"));
	}

	fmt::print("* Executing initial scripts...\n");
	if (cmdline_bootstrap.size() == 0) {
		world.executeBootstrap(false);
	} else {
		std::vector<std::string> scripts;

		if (engine.version < 3 && cmdline_bootstrap.size() != 1)
			throw Exception("multiple bootstrap files provided in C1/C2 mode");

		for (auto& bsi : cmdline_bootstrap) {
			fs::path scriptdir(bsi);
			if (engine.version > 2 || scriptdir.extension().string() == ".cos") {
				// pass it to the world to execute (it handles both files and directories)

				if (!fs::exists(scriptdir)) {
					fmt::print(stderr, "Warning: Couldn't find a specified script directory (trying {})!\n", bsi);
					continue;
				}

				world.executeBootstrap(scriptdir);
			} else {
				// in c1/c2 mode, if not a cos file, assume it's an SFC file
				if (!fs::exists(scriptdir) || fs::is_directory(scriptdir))
					throw Exception("non-existant bootstrap file provided in C1/C2 mode");
				// TODO: the default SFCFile loading code is in World, maybe this should be too..
				SFCFile sfc;
				FileReader f(scriptdir);
				sfc.read(&f);
				sfc.copyToWorld();
			}
		}
	}

	// if there aren't any metarooms, we can't run a useful game, the user probably
	// wanted to execute a CAOS script or something went badly wrong.
	if (!cmdline_norun && world.map->getMetaRoomCount() == 0) {
		shutdown();
		throw Exception("No metarooms found in given bootstrap directories or files");
	}

	// C1/C2 worlds: apply the world folder's saved state, if any
	if (version < 3 && !currentworlddir.empty()) {
		Disease::reset(); // (the save, if any, then fills it)
		Exposure::reset();
		// (a save that breaks the loader part way is kept aside, not
		// overwritten by the next autosave; the world goes on from what loaded)
		try {
			WorldSave::LoadWorldState(currentworlddir);
			WorldSave::SetAsideIncompleteSave(currentworlddir);
		} catch (std::exception& e) {
			WorldSave::SetAsideUnreadableSave(currentworlddir, e.what());
		}
		worldready = true;
		Openc2eImgui::DefaultWorldOpened();
		fflush(stdout); // (how the load went is in the log whatever follows)
	}

	// C2: the scents and their rules (pheromones.txt), read again for each world
	if (version == 2)
		Pheromones::load();

	// C2: objects built into every world (owner, 2026-11-09): the agents
	// named in the Additions folder's builtin-cobs.txt are injected from the
	// player's own COB files at each load (only objects added by hand from
	// the Agent Injector are saved with the world, so, like the additions
	// below, these are made anew each time), before
	// the additions, which may then adjust them. A COB the player does not
	// have is skipped.
	patchesApplied.clear(); // (script patches applied during this load)
	if (version == 2) {
		missingBuiltinCobs.clear();
		auto names = CobManager::readNameList(CobManager::builtinListFile());
		if (!names.empty()) {
			CobManager cobs;
			cobs.update();
			for (auto& name : names) {
				auto it = std::find_if(cobs.objects.begin(), cobs.objects.end(), [&](auto& o) { return o.name == name; });
				if (it == cobs.objects.end()) {
					fmt::print("* Built-in object \"{}\": no COB for it\n", name);
					missingBuiltinCobs.push_back(name);
					continue;
				}
				// this world already has ones the player added by hand (saved
				// before it was built in): those become the built-in ones, so
				// there are not two sets (owner, 2026-11-13)
				if (int n = CobManager::adoptHandAdded(name)) {
					fmt::print("* Built-in object \"{}\": {} added by hand already here, kept as the built-in ones\n", name, n);
					continue;
				}
				try {
					cobs.inject(*it);
					fmt::print("* Built-in object \"{}\" injected\n", name);
				} catch (std::exception& e) {
					fmt::print("* Built-in object \"{}\" failed: {}\n", name, e.what());
				}
			}
		}
	}

	// C1/C2: additions to the world, run after it has loaded (for example the
	// boats' call switches): Zeetures's built-in additions (C2) and the .cos
	// files in the user data folder's "Additions" folder (all worlds; one of
	// the same name replaces the built-in one, an empty one switches it off;
	// Additions.h), then the world folder's own.
	// Objects they create are not saved, so they are made anew at each load.
	if (version < 3) {
		// They are written for the game's own world: Eden's map, rooms and
		// objects. A world started from another world file (a custom world
		// such as The Albian Sea Bay, owner's COBs, 2026-11-20: thirteen of
		// them failed there, changing rooms it does not have) runs only
		// those marked "* any world" in their opening comment; its own
		// folder's additions (below) run anyway.
		bool edenMap = worldStartedFromEden(currentworlddir);
		for (auto& s : Additions::scripts()) {
			if (s.text.find_first_not_of(" \t\r\n") == std::string::npos)
				continue;
			if (!edenMap && !anyWorldAddition(s.text)) {
				fmt::print("* World addition {} skipped: written for Eden's map, and this world is not from Eden.sfc\n", s.name);
				continue;
			}
			if (!s.missingArt.empty()) {
				fmt::print("* World addition {} (built in) skipped: its art ({}) is not installed\n", s.name, s.missingArt);
				continue;
			}
			fmt::print("* Running world addition {}{}\n", s.name, s.fromPlayer ? "" : " (built in)");
			creatingAdditions = true;
			world.executeInitScriptText(s.name, s.text);
			creatingAdditions = false;
		}
		std::vector<fs::path> dirs;
		if (!currentworlddir.empty())
			dirs.push_back(fs::path(currentworlddir) / "Additions");
		for (auto& dir : dirs) {
			if (!fs::is_directory(dir))
				continue;
			std::vector<fs::path> files;
			for (auto& f : fs::directory_iterator(dir))
				if (f.path().extension() == ".cos")
					files.push_back(f.path());
			std::sort(files.begin(), files.end());
			for (auto& f : files) {
				fmt::print("* Running world addition {}\n", f.filename().string());
				creatingAdditions = true;
				world.executeInitScript(f);
				creatingAdditions = false;
			}
		}
	}

	applyScriptPatches(false);

	// developer aid: OPENC2E_TEST_SEASON=n (0 spring, 1 summer, 2 autumn,
	// 3 winter) sets the season after loading, for testing seasonal
	// behavior in a short run (a season is 2 hours of play)
	if (const char* sea = devenv("OPENC2E_TEST_SEASON")) {
		world.season = (unsigned int)(std::atoi(sea) % 4);
		world.dayofseason = 0;
		fmt::print("* TEST SEASON {}\n", world.season);
	}

	if (const char* dumppath = devenv("OPENC2E_DUMP_SCRIPTS")) {
		std::ofstream dump(dumppath, std::ios::binary);
		world.scriptorium->dumpScripts(dump);
		fmt::print("* Dumped world scripts to {}\n", dumppath);
	}
	MadeArt::dumpIfAsked();
	GenomeTraits::dumpIfAsked();
	fmt::print("* Done startup.\n");

	if (cmdline_norun) {
		// TODO: see comment above about avoiding backend when norun is set
		fmt::print("Told not to run the world, so stopping now.\n");
		shutdown();
		return false;
	}

	// Let agents know the window size (makes the DS sound options panel update
	// to match actual engine state when starting muted)
	// TODO: does this happen in real c2e?
	handleResizedWindow();

	// C1/C2: the view where it was when the world was saved (objects
	// installed during startup moved it)
	if (version < 3)
		WorldSave::ApplySavedView();

	return true;
}

bool Engine::saveForQuit() {
	if (!(version < 3 && worldready && autosave && !currentworlddir.empty()))
		return false;
	std::string err;
	fmt::print("* Auto-saving world '{}'\n", currentworld);
	if (!WorldSave::SaveWorldState(currentworlddir, err)) {
		fmt::print(stderr, "Auto-save failed: {}\n", err);
		return false;
	}
	fmt::print("* Saved; quitting\n");
	return true;
}

void Engine::shutdown() {
	// C1/C2: auto-save the open world (on quit and when switching worlds)
	if (version < 3 && worldready && autosave && !currentworlddir.empty()) {
		std::string err;
		fmt::print("* Auto-saving world '{}'\n", currentworld);
		if (!WorldSave::SaveWorldState(currentworlddir, err))
			fmt::print(stderr, "Auto-save failed: {}\n", err);
	}
	worldready = false;

	// first kill all world state, then start killing backend components
	world.shutdown();

	// musicmanager might still hold audio handles, so release them before
	// (1) the audio backend gets shut down and (2) this engine object is
	// destructed which may happen after audio backend statics get destructed
	musicmanager.reset();

	// backends should be okay to kill now
	get_audio_backend()->shutdown();
	get_backend()->shutdown();
	net->shutdown();
}
