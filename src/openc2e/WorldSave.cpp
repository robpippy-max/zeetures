/*
 *  WorldSave.cpp
 *  openc2e
 *
 *  Saving and restoring C1/C2 world state in openc2e's own format (stage 2:
 *  creatures, their bodies and minds, and eggs). A world folder holds the
 *  world file it started from (world.sfc); its saved state lives in
 *  save/state.txt with the creatures' genomes in save/genomes/. On load, the
 *  world file is read as usual and the saved creatures and eggs then replace
 *  the ones the world file contains. Other objects still start as they are in
 *  the world file.
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

#include "WorldSave.h"
#include "common/devenv.h"
#include "creatures/Disease.h"
#include "creatures/Exposure.h"
#include "creatures/Relationships.h"
#include "creatures/Breeding.h"

#include "Agent.h"
#include "Camera.h"
#include "CobManager.h"
#include "CompoundAgent.h"
#include "DullPart.h"
#include "SimpleAgent.h"
#include "Vehicle.h"
#include "Lift.h"
#include "CallButton.h"
#include "Engine.h"
#include "SpritePart.h"
#include "World.h"
#include "Map.h"
#include "Room.h"
#include "caosValue.h"
#include "caosVM.h"
#include "caosScript.h"
#include "common/creaturesImage.h"
#include "common/io/FileReader.h"
#include "common/io/FileWriter.h"
#include "creatures/CreatureAgent.h"
#include "creatures/CreatureMind.h"
#include "creatures/SkeletalCreature.h"
#include "creatures/oldCreature.h"
#include "fileformats/genomeFile.h"
#include "historyManager.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fmt/core.h>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>
#include <typeinfo>

namespace fs = std::filesystem;

namespace WorldSave {

static constexpr const char* MAGIC = "openc2e-world-save 1";

static SetAsideSave s_setAside; // (see SetAsideUnreadableSave)

static fs::path stateFile(const fs::path& worlddir) {
	return worlddir / "save" / "state.txt";
}

bool HasSavedState(const fs::path& worlddir) {
	return !worlddir.empty() && fs::exists(stateFile(worlddir));
}

static bool isEgg(Agent* a) {
	return engine.version <= 2 && a->family == 2 && a->genus == 5;
}

// Objects a script asks to keep: OV99 set to KEEP_MARK. They are saved and
// restored like eggs (sprite, picture, plane, attributes, timer, place and
// object variables), so things scripts make at load time, such as plants
// that grow over game days, carry on between sessions.
static const int KEEP_MARK = 7461;
static bool isKept(Agent* a) {
	return engine.version <= 2 && !isEgg(a) && a->var[99].hasInt() && a->var[99].getInt() == KEEP_MARK;
}

static oldCreature* oldCreatureOf(Agent* a) {
	CreatureAgent* ca = dynamic_cast<CreatureAgent*>(a);
	return ca ? dynamic_cast<oldCreature*>(ca->getCreature()) : nullptr;
}

static std::string timestamp() {
	std::time_t t = std::time(nullptr);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
	return buf;
}

static std::string escapeLine(const std::string& t);

// Objects added by hand from the Agent Injector (owner, 2026-11-10), and
// what they make later (Agent::injectedFrom). Unlike kept objects these can
// be compound objects and vehicles, so each is saved in full: its parts,
// their pictures and animations, hotspots and their functions, a vehicle's
// cabin, physics, pickup points, and object variables, including ones that
// point to other objects (another saved object, or one from the world file,
// found again by kind and place). Their COB's scripts are saved with the
// world too (world.injectedScripts) and installed again at load; saves made
// before that reinstall them from the COB file. Only plain C2
// objects, vehicles and compound objects are saved this way.
static bool isInjected(Agent* a) {
	if (engine.version != 2 || a->injectedFrom.empty() || isEgg(a) || isKept(a))
		return false;
	const std::type_info& t = typeid(*a);
	return t == typeid(SimpleAgent) || t == typeid(CompoundAgent) || t == typeid(Vehicle);
}

// Every other object (stage 3, owner 2026-11-14: "the objects to stay as-is
// between relaunches"): plain and compound objects from the world file or
// made during play (fallen fruit, critters, seeds), not scenery (family 1),
// creatures, eggs, kept or injected objects, nor objects the Additions make
// at each load. Bubbles and the hand are other types and still start as in
// the world file.
// Lifts, boats, the submarine and other vehicles, and call buttons, from the
// world file (2026-11-19, the rest of stage 3): kept as they were, with
// their riders, a lift's stop and where it is going, but only updated in
// place, never removed or made again (saves before this have none of them).
static bool isMachine(Agent* a) {
	const std::type_info& t = typeid(*a);
	return t == typeid(Vehicle) || t == typeid(Lift) || t == typeid(CallButton);
}

static bool isWorldObject(Agent* a) {
	if (engine.version != 2 || a->family == 1 || a->fromAddition || !a->injectedFrom.empty())
		return false;
	if (oldCreatureOf(a) || isEgg(a) || isKept(a))
		return false;
	const std::type_info& t = typeid(*a);
	if (t == typeid(SimpleAgent) || t == typeid(CompoundAgent))
		return true;
	// (developer aid: OPENC2E_NO_SAVED_MACHINES keeps them out, as before)
	static const bool noMachines = devenv("OPENC2E_NO_SAVED_MACHINES") != nullptr;
	return !noMachines && a->sfcIndex >= 0 && isMachine(a);
}

// in the middle of a script other than its timer when saved: its pictures
// and ACTV are left as the world file has them (the script will not run on
// after a load, so a machine would stay half way through)
static bool isBusy(Agent* a) {
	return a->vm && a->vm->currentscript && a->vm->currentscript->scrp != 9;
}

static void savePart(std::ostream& out, CompoundPart* cp) {
	SpritePart* p = dynamic_cast<SpritePart*>(cp);
	if (!p || !p->getSprite())
		return;
	out << fmt::format("part {} {} {} {} {} {} {} {} {} {} anim {}", cp->id, p->getSprite()->getName(), p->getFirstImg(), cp->x, cp->y,
		(int)cp->zorder, p->getBase(), p->getPose(), (int)p->framerate, p->getFrameNo(), p->animation.size());
	for (auto f : p->animation)
		out << " " << (int)f;
	out << "\n";
}

// an object's kind, place, state, variables and pictures (iobject/wobject)
static void writeObjectBody(std::ostream& out, Agent* ag, std::map<Agent*, int>& ids) {
	CompoundAgent* comp = dynamic_cast<CompoundAgent*>(ag);
	Vehicle* vhcl = dynamic_cast<Vehicle*>(ag);
		out << "kind " << (vhcl ? "vhcl" : comp ? "comp" : ag->scenery ? "scen" : "simp") << "\n";
		out << "classifier " << (int)ag->family << " " << (int)ag->genus << " " << ag->species << "\n";
		out << "plane " << (int)ag->zorder << "\n";
		out << fmt::format("pos {} {}\n", ag->x, ag->y);
		out << fmt::format("vel {} {}\n", ag->velx, ag->vely);
		out << "attr " << ag->attr << "\n";
		out << "actv " << ag->actv << "\n";
		out << "clac " << ag->clac[0] << " " << ag->clac[1] << " " << ag->clac[2] << " " << ag->clik << "\n";
		out << "timer " << ag->timerrate << " " << (ag->timerrate ? ag->getTimerPhase() : 0) << "\n";
		// (C2 physics only: friction, perm and elas are not used by C2
		// objects, and openc2e leaves them unset for them)
		out << fmt::format("physics {} {} {} {} {} {} {} {} {}\n", ag->accg, ag->aero, ag->rest, ag->size, ag->thrt, ag->range,
			ag->falling ? 1 : 0, ag->paused ? 1 : 0, ag->visible ? 1 : 0);
		for (auto& [pose, pt] : ag->carried_points)
			out << "puhl " << pose << " " << pt.first << " " << pt.second << "\n";
		for (auto& [pose, pt] : ag->carry_points)
			out << "pupt " << pose << " " << pt.first << " " << pt.second << "\n";
		if (ag->has_custom_core_size)
			out << fmt::format("core {} {} {} {}\n", ag->custom_core_ytop, ag->custom_core_ybottom, ag->custom_core_xleft, ag->custom_core_xright);
		for (int i = 0; i < 100; i++) {
			const caosValue& v = ag->var[i];
			if (v.hasInt()) {
				if (v.getInt() != 0) // a new object's variables start at 0
					out << "var " << i << " i " << v.getInt() << "\n";
			} else if (v.hasFloat())
				out << fmt::format("var {} f {}\n", i, v.getFloat());
			else if (v.hasString())
				out << "var " << i << " s " << escapeLine(v.getString()) << "\n";
			else if (v.hasAgent()) {
				Agent* t = v.getAgent().get();
				if (!t)
					continue;
				if (ids.count(t))
					out << "var " << i << " a " << ids[t] << "\n";
				else
					out << fmt::format("var {} near {} {} {} {} {}\n", i, (int)t->family, (int)t->genus, t->species, t->x, t->y);
			}
		}
		if (comp) {
			out << "comp " << comp->getSpriteFile() << " " << comp->getFirstImage() << " " << comp->getImageCount() << "\n";
			for (auto* p : comp->parts)
				savePart(out, p);
			for (int i = 0; i < 6; i++) {
				auto& h = comp->hotspots[i];
				auto& f = comp->hotspotfunctions[i];
				// (an unbound function's message and mask are never set)
				out << "spot " << i << " " << h.left << " " << h.top << " " << h.right << " " << h.bottom << " " << f.hotspot << " "
					<< (f.hotspot >= 0 ? f.message : 0) << " " << (f.hotspot >= 0 ? (int)f.mask : 3) << "\n";
			}
		} else {
			savePart(out, ag->part(0));
		}
		if (Lift* lift = dynamic_cast<Lift*>(ag))
			out << "lift " << lift->currentbutton << " " << lift->newbutton << "\n";
		// who rides it: creatures by moniker, objects by their id in this list
		if (vhcl)
			for (auto& p : vhcl->passengers) {
				Agent* r = p.get();
				if (!r || r->isDying())
					continue;
				if (oldCreatureOf(r) && world.history)
					out << "rider c " << world.history->findMoniker(oldCreatureOf(r)->getGenome()) << "\n";
				else if (ids.count(r))
					out << "rider o " << ids[r] << "\n";
			}
		if (vhcl)
			out << "cabin " << vhcl->cabinleft << " " << vhcl->cabintop << " " << vhcl->cabinright << " " << vhcl->cabinbottom << " " << vhcl->cabinplane << " "
				<< vhcl->getCapacity() << " " << vhcl->getBump() << " " << vhcl->xvec << " " << vhcl->yvec << "\n";
}

static void saveInjected(std::ostream& out, int& count) {
	for (auto& name : world.injectedCobs) {
		out << "injected " << escapeLine(name) << "\n";
		// and its scripts, so it works without its COB file
		if (auto sc = world.injectedScripts.find(name); sc != world.injectedScripts.end())
			for (auto& script : sc->second)
				out << "injscript " << escapeLine(name) << "\t" << escapeLine(script) << "\n";
	}
	std::map<Agent*, int> ids;
	for (auto& a : world.agents)
		if (a && !a->isDying() && isInjected(a.get())) {
			int n = (int)ids.size();
			ids[a.get()] = n;
		}
	for (auto& a : world.agents) {
		if (!a || a->isDying() || !ids.count(a.get()))
			continue;
		Agent* ag = a.get();
		out << "iobject\n";
		out << "id " << ids[ag] << "\n";
		out << "from " << escapeLine(ag->injectedFrom) << "\n";
		writeObjectBody(out, ag, ids);
		out << "end\n";
		count++;
	}
}


static void saveWorldObjects(std::ostream& out, int& count) {
	out << "world-objects 1\n"; // (a save without this line has none: nothing is removed at load)
	std::map<Agent*, int> ids;
	for (auto& a : world.agents)
		if (a && !a->isDying() && isWorldObject(a.get())) {
			int n = (int)ids.size();
			ids[a.get()] = n;
		}
	for (auto& a : world.agents) {
		if (!a || a->isDying() || !ids.count(a.get()))
			continue;
		Agent* ag = a.get();
		out << "wobject\n";
		out << "id " << ids[ag] << "\n";
		if (ag->sfcIndex >= 0)
			out << "sfc " << ag->sfcIndex << "\n";
		if (isBusy(ag))
			out << "busy 1\n";
		writeObjectBody(out, ag, ids);
		out << "end\n";
		count++;
	}
}

struct SavedPart {
	unsigned int id = 0, firstimg = 0, base = 0, pose = 0, frameno = 0;
	std::string sprite;
	int x = 0, y = 0, z = 0, framerate = 0;
	std::vector<unsigned char> anim;
};

struct SavedInjected {
	int id = -1, family = 0, genus = 0, species = 0, plane = 0;
	std::string from, kind = "simp", compsprite;
	unsigned int compfirst = 0, compcount = 0;
	float x = 0, y = 0, velx = 0, vely = 0, accg = 0, range = 0;
	unsigned int attr = 0, timer = 0, since = 0;
	int actv = 0, clac[3] = {-1, -1, -1}, clik = -1, aero = 0, rest = 0, size = 0, thrt = 0;
	bool falling = false, paused = false, visible = true;
	std::map<unsigned int, std::pair<int, int>> puhl, pupt;
	bool core = false;
	float coretop = 0, corebottom = 0, coreleft = 0, coreright = 0;
	struct Var {
		int n;
		std::string type, value;
		int family = 0, genus = 0, species = 0;
		float x = 0, y = 0;
	};
	std::vector<Var> vars;
	std::vector<SavedPart> parts;
	struct Spot {
		int l = -1, t = -1, r = -1, b = -1, func = -1, message = 0, mask = 3;
	};
	Spot spots[6];
	bool hasCabin = false;
	int cabin[5] = {0, 0, 0, 0, 1};
	unsigned int capacity = 0, bump = 0;
	int xvec = 0, yvec = 0;
	int sfc = -1; // a world object's place in the world file's list
	bool busy = false;
	bool hasLift = false;
	unsigned int liftcur = 0, liftnew = 0;
	std::vector<std::string> riderCreatures; // monikers
	std::vector<int> riderObjects; // ids
};

static void readInjectedLine(SavedInjected& o, const std::string& key, std::istringstream& l, const std::function<std::string()>& rest) {
	if (key == "id")
		l >> o.id;
	else if (key == "sfc")
		l >> o.sfc;
	else if (key == "busy")
		l >> o.busy;
	else if (key == "from")
		o.from = rest();
	else if (key == "kind")
		l >> o.kind;
	else if (key == "classifier")
		l >> o.family >> o.genus >> o.species;
	else if (key == "plane")
		l >> o.plane;
	else if (key == "pos")
		l >> o.x >> o.y;
	else if (key == "vel")
		l >> o.velx >> o.vely;
	else if (key == "attr")
		l >> o.attr;
	else if (key == "actv")
		l >> o.actv;
	else if (key == "clac")
		l >> o.clac[0] >> o.clac[1] >> o.clac[2] >> o.clik;
	else if (key == "timer")
		l >> o.timer >> o.since;
	else if (key == "physics")
		l >> o.accg >> o.aero >> o.rest >> o.size >> o.thrt >> o.range >> o.falling >> o.paused >> o.visible;
	else if (key == "puhl" || key == "pupt") {
		unsigned int pose;
		int px, py;
		if (l >> pose >> px >> py)
			(key == "puhl" ? o.puhl : o.pupt)[pose] = {px, py};
	} else if (key == "core") {
		o.core = (bool)(l >> o.coretop >> o.corebottom >> o.coreleft >> o.coreright);
	} else if (key == "var") {
		SavedInjected::Var v;
		l >> v.n >> v.type;
		if (v.type == "s")
			v.value = rest();
		else if (v.type == "near")
			l >> v.family >> v.genus >> v.species >> v.x >> v.y;
		else
			l >> v.value;
		if (v.n >= 0 && v.n < 100)
			o.vars.push_back(v);
	} else if (key == "comp") {
		l >> o.compsprite >> o.compfirst >> o.compcount;
	} else if (key == "part") {
		SavedPart p;
		std::string animkey;
		size_t n = 0;
		if (l >> p.id >> p.sprite >> p.firstimg >> p.x >> p.y >> p.z >> p.base >> p.pose >> p.framerate >> p.frameno >> animkey >> n) {
			for (size_t i = 0; i < n; i++) {
				int f;
				if (l >> f)
					p.anim.push_back((unsigned char)f);
			}
			o.parts.push_back(p);
		}
	} else if (key == "spot") {
		int i;
		SavedInjected::Spot sp;
		if (l >> i >> sp.l >> sp.t >> sp.r >> sp.b >> sp.func >> sp.message >> sp.mask && i >= 0 && i < 6)
			o.spots[i] = sp;
	} else if (key == "lift") {
		o.hasLift = (bool)(l >> o.liftcur >> o.liftnew);
	} else if (key == "rider") {
		std::string kind, who;
		if (l >> kind >> who) {
			if (kind == "c")
				o.riderCreatures.push_back(who);
			else if (kind == "o")
				o.riderObjects.push_back(atoi(who.c_str()));
		}
	} else if (key == "cabin") {
		o.hasCabin = (bool)(l >> o.cabin[0] >> o.cabin[1] >> o.cabin[2] >> o.cabin[3] >> o.cabin[4] >> o.capacity >> o.bump >> o.xvec >> o.yvec);
	}
}

static void applyPart(SpritePart* p, const SavedPart& s) {
	p->setBase(s.base);
	p->setPose(s.pose);
	p->setFramerate((unsigned char)s.framerate);
	p->animation.assign(s.anim.begin(), s.anim.end());
	// the saved frame, or (an animation run to its end, frame == its length)
	// none: the part otherwise kept the frame number from the world file's
	// own animation, which may be longer (owner's Home, 2026-11-15: a glow,
	// 2 18 5, saved at frame 3 of 3 kept the world file's frame 4, and the
	// game closed on loading)
	if (p->animation.empty())
		return;
	if (s.frameno < p->animation.size() && p->animation[s.frameno] != 255)
		p->setFrameNo(s.frameno);
	else if (s.frameno >= p->animation.size())
		p->animation.clear(); // (finished, as an empty animation is to OVER)
	else
		p->setFrameNo(0);
}

static void applyVars(Agent* ag, const SavedInjected& o, const std::map<int, Agent*>& made);
static void restoreInjected(std::vector<SavedInjected>& objects, std::map<int, Agent*>* shared = nullptr);

// an object's saved variables: numbers, text, and other objects (one saved
// with it, by id, or one from the world file or a creature: the nearest of
// its kind to where it was)
static void applyVars(Agent* ag, const SavedInjected& o, const std::map<int, Agent*>& made) {
	for (auto& v : o.vars) {
		caosValue& dest = ag->var[v.n];
		if (v.type == "i")
			dest.setInt(std::atoi(v.value.c_str()));
		else if (v.type == "f")
			dest.setFloat((float)std::atof(v.value.c_str()));
		else if (v.type == "s")
			dest.setString(v.value);
		else if (v.type == "a") {
			auto it = made.find(std::atoi(v.value.c_str()));
			if (it != made.end())
				dest.setAgent(it->second);
		} else if (v.type == "near") {
			Agent* best = nullptr;
			float bestd = 300.0f * 300.0f;
			for (auto& a : world.agents) {
				if (!a || a->isDying() || a->family != v.family || a->genus != v.genus || a->species != v.species)
					continue;
				float d = (a->x - v.x) * (a->x - v.x) + (a->y - v.y) * (a->y - v.y);
				if (d <= bestd) {
					bestd = d;
					best = a.get();
				}
			}
			if (best)
				dest.setAgent(best);
		}
	}
}

// stage 3: the world file's objects take back their saved state; those not
// in the save (eaten, gone) are removed; objects made during play are made
// again (as restoreInjected does)
static void restoreWorldObjects(std::vector<SavedInjected>& objects, int& matched, int& removed, int& made_n) {
	std::map<int, Agent*> bySfc;
	for (auto& a : world.agents)
		if (a && !a->isDying() && a->sfcIndex >= 0 && isWorldObject(a.get()))
			bySfc[a->sfcIndex] = a.get();
	std::map<int, Agent*> made;
	std::set<Agent*> kept;
	std::vector<std::pair<Agent*, SavedInjected*>> fromFile;
	std::vector<SavedInjected> fresh;
	for (auto& o : objects) {
		if (o.sfc < 0) {
			if (o.kind != "vhcl")
				fresh.push_back(o);
			continue;
		}
		auto it = bySfc.find(o.sfc);
		if (it == bySfc.end())
			continue;
		Agent* ag = it->second;
		if (ag->family != o.family || ag->genus != o.genus || ag->species != o.species)
			continue; // another world file: leave it
		kept.insert(ag);
		made[o.id] = ag;
		fromFile.push_back({ag, &o});
		ag->moveTo(o.x, o.y);
		ag->setAttributes(o.attr);
		ag->clik = o.clik;
		for (int i = 0; i < 3; i++)
			ag->clac[i] = o.clac[i];
		ag->timerrate = o.timer;
		ag->setTimerPhase(o.since);
		ag->velx = o.velx;
		ag->vely = o.vely;
		// A machine saved in the middle of a script is left still: the script
		// that would have stopped it does not run on after a load, so one
		// moving under its script kept that speed for good. The submarine
		// (3 1 2), saved on its way between docks, went on west through the
		// rock and across the world (owner's screencast, 2026-11-20)
		if (o.busy) {
			ag->velx = 0;
			ag->vely = 0;
		}
		ag->accg = o.accg;
		ag->aero = o.aero;
		ag->rest = o.rest;
		ag->size = o.size;
		ag->thrt = o.thrt;
		ag->range = o.range;
		ag->falling = o.falling;
		ag->paused = o.paused;
		ag->visible = o.visible;
		if (Vehicle* v = dynamic_cast<Vehicle*>(ag); v && o.hasCabin) {
			v->xvec = o.xvec;
			v->yvec = o.yvec;
		}
		if (Lift* lift = dynamic_cast<Lift*>(ag); lift && o.hasLift && o.liftcur < lift->callbuttony.size() && o.liftnew < lift->callbuttony.size()) {
			lift->currentbutton = o.liftcur;
			lift->newbutton = o.liftnew;
		}
		if (!o.busy) {
			ag->actv = o.actv;
			for (auto& sp : o.parts)
				if (auto* p = dynamic_cast<SpritePart*>(ag->part(sp.id)))
					applyPart(p, sp);
		}
		matched++;
	}
	// gone during play: removed
	for (auto& [idx, ag] : bySfc)
		if (!kept.count(ag) && !isMachine(ag)) {
			ag->kill();
			removed++;
		}
	// made during play: made again
	restoreInjected(fresh, &made);
	made_n = (int)fresh.size();
	// a variable at 0 is not written to the save, so a world file object's
	// variables are cleared first: else one the save has at 0 kept the world
	// file's value (the fish then laid eggs again and again after a load)
	for (auto& [ag, o] : fromFile) {
		for (int i = 0; i < 100; i++)
			ag->var[i].setInt(0);
		applyVars(ag, *o, made);
	}
	// riders back aboard (creatures were restored before this)
	int riders = 0;
	for (auto& [ag, o] : fromFile) {
		Vehicle* v = dynamic_cast<Vehicle*>(ag);
		if (!v)
			continue;
		std::vector<Agent*> aboard;
		for (int id : o->riderObjects)
			if (made.count(id))
				aboard.push_back(made[id]);
		if (!o->riderCreatures.empty() && world.history)
			for (auto& a : world.agents)
				if (oldCreature* c = a ? oldCreatureOf(a.get()) : nullptr)
					if (std::find(o->riderCreatures.begin(), o->riderCreatures.end(), world.history->findMoniker(c->getGenome())) != o->riderCreatures.end())
						aboard.push_back(a.get());
		for (Agent* r : aboard)
			if (r && !r->isDying() && !r->carriedby && !r->invehicle) {
				v->carry(r);
				riders++;
			}
	}
	if (riders)
		fmt::print("* Riders back aboard their vehicles: {}\n", riders);
}


static void restoreInjected(std::vector<SavedInjected>& objects, std::map<int, Agent*>* shared) {
	std::map<int, Agent*> ownMade;
	std::map<int, Agent*>& made = shared ? *shared : ownMade;
	std::vector<std::pair<Agent*, SavedInjected*>> all;
	for (auto& o : objects) {
		try {
			if (o.parts.empty())
				continue;
			Agent* ag = nullptr;
			if (o.kind == "simp" || o.kind == "scen") {
				const SavedPart& p0 = o.parts[0];
				SimpleAgent* a = new SimpleAgent(o.family, o.genus, o.species, o.plane, p0.sprite, p0.firstimg, 0);
				a->scenery = o.kind == "scen";
				a->finishInit();
				if (auto* sp = dynamic_cast<SpritePart*>(a->part(0)))
					applyPart(sp, p0);
				ag = a;
			} else {
				CompoundAgent* a = o.kind == "vhcl" ? new Vehicle(o.compsprite, o.compfirst, o.compcount) : new CompoundAgent(o.compsprite, o.compfirst, o.compcount);
				a->setClassifier(o.family, o.genus, o.species);
				a->finishInit();
				if (o.plane != 0)
					a->setZOrder(o.plane);
				for (auto& sp : o.parts) {
					DullPart* p = new DullPart(a, sp.id, sp.sprite, sp.firstimg, sp.x, sp.y, sp.z);
					a->addPart(p);
					applyPart(p, sp);
				}
				for (int i = 0; i < 6; i++) {
					auto& h = o.spots[i];
					a->setHotspotLoc(i, h.l, h.t, h.r, h.b);
					a->setHotspotFunc(i, h.func);
					a->setHotspotFuncDetails(i, (uint16_t)h.message, (uint8_t)h.mask);
				}
				if (Vehicle* v = dynamic_cast<Vehicle*>(a)) {
					if (o.hasCabin) {
						v->setCabinRect(o.cabin[0], o.cabin[1], o.cabin[2], o.cabin[3]);
						v->cabinplane = o.cabin[4];
						v->setCapacity(o.capacity);
						v->setBump(o.bump);
						v->xvec = o.xvec;
						v->yvec = o.yvec;
					}
				}
				ag = a;
			}
			ag->setClassifier(o.family, o.genus, o.species);
			ag->moveTo(o.x, o.y);
			ag->setAttributes(o.attr);
			ag->actv = o.actv;
			for (int i = 0; i < 3; i++)
				ag->clac[i] = o.clac[i];
			ag->clik = o.clik;
			ag->timerrate = o.timer;
			ag->setTimerPhase(o.since);
			ag->velx = o.velx;
			ag->vely = o.vely;
			ag->accg = o.accg;
			ag->aero = o.aero;
			ag->rest = o.rest;
			ag->size = o.size;
			ag->thrt = o.thrt;
			ag->range = o.range;
			ag->falling = o.falling;
			ag->paused = o.paused;
			ag->visible = o.visible;
			ag->carried_points = o.puhl;
			ag->carry_points = o.pupt;
			if (o.core) {
				ag->has_custom_core_size = true;
				ag->custom_core_ytop = o.coretop;
				ag->custom_core_ybottom = o.corebottom;
				ag->custom_core_xleft = o.coreleft;
				ag->custom_core_xright = o.coreright;
			}
			ag->injectedFrom = o.from;
			made[o.id] = ag;
			all.push_back({ag, &o});
		} catch (std::exception& e) {
			fmt::print("* Couldn't restore an injected object ({} {} {}): {}\n", o.family, o.genus, o.species, e.what());
		}
	}
	// object variables last, once every object they may point to exists
	for (auto& [ag, o] : all)
		applyVars(ag, *o, made);
	// as for objects from the world file: "enter scope"
	for (auto& [ag, o] : all)
		ag->queueScript(7);
}

bool SaveWorldState(const fs::path& worlddir, std::string& error) {
	if (worlddir.empty()) {
		error = "No world folder is open.";
		return false;
	}
	// a world with nothing at all in it is one already being taken down:
	// never written over a good save
	if (world.agents.empty()) {
		error = "There is nothing in the world to save.";
		return false;
	}
	// write into save.new/, then swap it in, so a failed save never damages
	// the previous one
	// a save that could be neither read nor moved aside is never overwritten
	if (s_setAside.happened && s_setAside.kept.empty() && s_setAside.worlddir == worlddir) {
		error = "This world's saved game could not be read or moved, so saving is off to protect it.";
		return false;
	}
	fs::path tmp = worlddir / "save.new";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp / "genomes", ec);
	if (ec) {
		error = "Couldn't create the save folder: " + ec.message();
		return false;
	}

	std::ofstream out(tmp / "state.txt", std::ios::binary); // (binary: the same \n line ends on every system)
	out << MAGIC << "\n";
	out << "saved " << timestamp() << "\n";
	out << "chemical-relays 1\n"; // body chemistry relays were running when saved
	out << "camera " << engine.camera->getX() << " " << engine.camera->getY() << " " << engine.zoom << "\n"; // the view's top left and zoom
	// the hand's mode: invisible, push (owner, 2026-11-14: start as it was)
	out << "hand " << (world.handInvisible ? 1 : 0) << " " << (world.handPush ? 1 : 0) << "\n";
	out << "all-fertile " << (world.allFertile ? 1 : 0) << "\n"; // the Hatchery's setting
	// the calendar: world ticks, part of the day, day of the season, season,
	// year (without this every session began again in spring of year 0)
	out << "calendar " << world.worldtickcount << " " << world.timeofday << " " << world.dayofseason << " " << world.season << " " << world.year << "\n";
	// default names: how many of each kind this world has had
	for (auto& [genus, n] : world.history->namecount)
		out << "names " << genus << " " << n << "\n";
	out << "names-given " << (world.history->namesgiven ? 1 : 0) << "\n";
	// the Ecology Kit's pinned rooms
	for (auto& [room, color] : world.history->ecologypins)
		out << "ecology-pin " << room << " " << color << "\n";
	// the nutrient cycle (World::tickNutrients): each room's organic and
	// inorganic nutrients and the world file's levels they settle toward
	if (world.map)
		for (auto& r : world.map->getRooms())
			if (r->ontr || r->intr || r->ontrBase > 0 || r->intrBase > 0)
				out << fmt::format("nutrients {} {:.2f} {:.2f} {} {}\n", r->id, r->ontrf >= 0 ? r->ontrf : (float)r->ontr, r->intrf >= 0 ? r->intrf : (float)r->intr, r->ontrBase, r->intrBase);
	// bacteria and disease (Disease.h): strains, germs in rooms, who is ill
	Disease::save(out);
	// radiation doses (Exposure.h): creatures', and babies' before birth
	Exposure::save(out);
	// the life log (Owner's Kit): kind, tick, genus, female, stage, age, age
	// known, old age, moniker, then the name
	auto entry = [&](const char* key, const lifeLogEntry& e) {
		out << key << " " << e.kind << " " << e.tick << " " << e.genus << " " << (e.female ? 1 : 0) << " " << e.stage << " " << e.age << " "
			<< (e.ageknown ? 1 : 0) << " " << (e.oldage ? 1 : 0) << " " << (e.moniker.empty() ? "-" : e.moniker) << " " << escapeLine(e.name) << "\n"
			<< (e.child.empty() ? "" : "life-egg " + e.child + "\n") // (belongs to the line before)
			<< (e.cause.empty() ? "" : "life-cause " + escapeLine(e.cause) + "\n")
			<< (e.x < 0 ? "" : fmt::format("life-place {} {} {}\n", e.x, e.y, e.room));
	};
	for (auto& e : world.history->lifelog)
		entry("life", e);
	// the status bar's notices not yet clicked or dismissed, and any not yet
	// put on it (the same fields; owner, 2026-11-15: they were lost at
	// every relaunch)
	for (auto& e : world.history->shownNotices)
		entry("notice", e);
	for (auto& e : world.history->notices)
		entry("notice", e);

	int ncreatures = 0, neggs = 0;
	for (auto& a : world.agents) {
		if (!a || a->isDying())
			continue;
		oldCreature* c = oldCreatureOf(a.get());
		if (c) {
			if (!c->isAlive())
				continue; // the dead are not saved
			std::string genomename = fmt::format("c{}.gen", ncreatures);
			{
				FileWriter w(tmp / "genomes" / genomename);
				w << *c->getGenome();
			}
			out << "creature\n";
			out << "genome " << genomename << "\n";
			out << "female " << (c->isFemale() ? 1 : 0) << "\n";
			out << "variant " << c->getVariant() << "\n";
			out << "selected " << (world.selectedcreature.lock().get() == a.get() ? 1 : 0) << "\n";
			out << "pos " << a->x << " " << a->y << "\n";
			// where its feet were: a creature restored in another pose (it
			// was saved lying down, asleep or fainted) stands there, not
			// by its top (its feet went 29 px under a floor and it fell to
			// the room below)
			if (auto* sc = dynamic_cast<SkeletalCreature*>(a.get()))
				out << "foot " << sc->footY() << "\n";
			out << "stage " << (int)c->getStage() << "\n";
			out << "age " << c->getAge() << "\n";
			out << "ticks " << c->getTicks() << "\n";
			out << "ageing " << (c->isAgeing() ? 1 : 0) << "\n";
			out << "asleep " << (c->isAsleep() ? 1 : 0) << "\n";
			// a pregnancy: the baby's genome name (in the user Genetics folder)
			if (a->babymoniker != 0)
				out << "baby " << genomeNameFromInt(a->babymoniker) << "\n";
			out << "temperament " << Temperaments::toLine(c->temperament) << "\n";
			out << "temperament-born " << Temperaments::toLine(c->temperamentBorn) << "\n";
			if (c->temperamentDrift.any())
				out << "temperament-drift " << Temperaments::driftLine(c->temperamentDrift) << "\n";
			out << "seasonal " << fmt::format("{:.3f}", c->seasonalZ) << "\n";
			out << "lifespan-scale " << fmt::format("{:.3f}", c->lifespanScale) << "\n";
			out << "mind-genes " << MindGeneSet::toLine(c->mindGenes) << "\n";
			if (c->fertilityOverride >= 0)
				out << "fertility " << c->fertilityOverride << "\n"; // the owner's setting (Breeder's Kit)
			// what it feels about the creatures it has met (Relationships.h)
			for (auto& [other, b] : c->bonds)
				out << "bond " << Relationships::toLine(other, b) << "\n";
			// words heard: word-list number:familiarity, for each one heard
			{
				std::string words;
				for (size_t i = 0; i < c->vocab.size(); i++)
					if (c->vocab[i])
						words += fmt::format(" {}:{}", i, (int)c->vocab[i]);
				if (!words.empty())
					out << "words" << words << "\n";
			}
			{
				std::string mon = world.history->findMoniker(c->getGenome());
				monikerData& m = world.history->getMoniker(mon);
				out << "moniker " << mon << "\n";
				if (!m.mother.empty() || !m.father.empty())
					out << "parents " << (m.mother.empty() ? "-" : m.mother) << " " << (m.father.empty() ? "-" : m.father) << (m.spliced ? " spliced" : "") << "\n";
				if (!m.name.empty())
					out << "name " << escapeLine(m.name) << "\n";
				out << "registered " << (m.registered ? 1 : 0) << "\n";
				out << "born " << m.borntick << " " << (m.bornknown ? 1 : 0) << "\n";
				for (auto& ph : m.photos)
					out << "photo " << escapeLine(ph) << "\n";
				for (auto& e : m.story)
					out << "story " << e.tick << " " << e.kind << " " << (e.other.empty() ? "-" : e.other) << " " << escapeLine(e.detail) << "\n";
				if (!m.notes.empty())
					out << "notes " << escapeLine(m.notes) << "\n";
			}
			out << "chemicals";
			for (int i = 0; i < 256; i++)
				out << " " << (int)c->getChemical(i);
			out << "\norgans";
			for (float f : c->getOrganState())
				out << " " << f;
			out << "\n";
			if (CreatureMind* m = c->getMind()) {
				out << "mind " << m->name() << "\n";
				out << "mindstate\n"
					<< m->saveState() << "endmindstate\n";
			}
			out << "end\n";
			ncreatures++;
		} else if (isEgg(a.get()) || isKept(a.get())) {
			SpritePart* sp = dynamic_cast<SpritePart*>(a->part(0));
			if (!sp)
				continue;
			if (isKept(a.get())) {
				out << "object\n";
				out << "frames " << (int)sp->getSprite()->numframes() - (int)sp->getFirstImg() << "\n";
				out << "plane " << (int)a->zorder << "\n"; // signed: planes can be below 0
			} else {
				out << "egg\n";
			}
			out << "classifier " << (int)a->family << " " << (int)a->genus << " " << a->species << "\n";
			out << "sprite " << sp->getSprite()->getName() << " " << sp->getFirstImg() << " " << sp->getPose() << "\n";
			out << "attr " << a->attr << "\n";
			out << "timer " << a->timerrate << "\n";
			out << "pos " << a->x << " " << a->y << "\n";
			for (int i = 0; i < 100; i++) {
				const caosValue& v = a->var[i];
				if (v.hasInt())
					out << "var " << i << " i " << v.getInt() << "\n";
				else if (v.hasFloat())
					out << "var " << i << " f " << v.getFloat() << "\n";
			}
			out << "end\n";
			neggs++;
		}
	}
	int ninjected = 0;
	saveInjected(out, ninjected);
	int nworld = 0;
	saveWorldObjects(out, nworld);
	// the records of the dead (Owner's Kit family tree, life log): no body,
	// just who they were
	int nrecords = 0;
	for (auto& [mon, m] : world.history->all()) {
		if (!m.died)
			continue;
		out << "record\n";
		out << "moniker " << mon << "\n";
		out << "genus " << (int)m.genus << " " << m.gender << "\n";
		if (!m.mother.empty() || !m.father.empty())
			out << "parents " << (m.mother.empty() ? "-" : m.mother) << " " << (m.father.empty() ? "-" : m.father) << (m.spliced ? " spliced" : "") << "\n";
		if (!m.name.empty())
			out << "name " << escapeLine(m.name) << "\n";
		out << "registered " << (m.registered ? 1 : 0) << "\n";
		out << "born " << m.borntick << " " << (m.bornknown ? 1 : 0) << "\n";
		out << "died " << m.diedtick << " " << m.diedstage << "\n";
		for (auto& ph : m.photos)
			out << "photo " << escapeLine(ph) << "\n";
		for (auto& e : m.story)
			out << "story " << e.tick << " " << e.kind << " " << (e.other.empty() ? "-" : e.other) << " " << escapeLine(e.detail) << "\n";
		if (!m.notes.empty())
			out << "notes " << escapeLine(m.notes) << "\n";
		if (!m.epitaph.empty())
			out << "epitaph " << escapeLine(m.epitaph) << "\n";
		if (!m.gravephoto.empty())
			out << "gravephoto " << escapeLine(m.gravephoto) << "\n";
		out << "end\n";
		nrecords++;
	}
	out.close();
	if (!out) {
		error = "Couldn't write the save file.";
		return false;
	}

	fs::path dest = worlddir / "save";
	fs::path old = worlddir / "save.old";
	fs::remove_all(old, ec);
	if (fs::exists(dest))
		fs::rename(dest, old, ec);
	fs::rename(tmp, dest, ec);
	if (ec) {
		error = "Couldn't put the new save in place: " + ec.message();
		return false;
	}
	fs::remove_all(old, ec);
	fmt::print("* Saved world: {} creatures, {} eggs, {} injected objects, {} other objects, {} records of the dead\n", ncreatures, neggs, ninjected, nworld, nrecords);
	return true;
}

struct SavedCreature {
	std::string genome, mind, mindstate;
	bool female = false, selected = false, ageing = false, asleep = false;
	int variant = 0, stage = 0;
	std::string baby; // the genome name of the baby she carries, if pregnant
	std::vector<std::pair<int, int>> words; // word-list number, familiarity
	std::vector<std::string> bonds; // bond lines (Relationships::fromLine)
	std::string temperament; // Temperaments::toLine; empty: keep the one drawn at hatching
	std::string temperamentBorn, temperamentDrift; // (saves before 2026-11-17 have neither: born is the temperament)
	std::string seasonal; // its seasonal rhythm (SeasonalRhythm); empty: keep the one it has
	std::string lifespanScale; // its own pace of ageing; empty (saves before 0.18.3): keep the one drawn now
	std::string mindGenes; // MindGenes.h; empty (saves before 0.20.0): keep the ones drawn now
	int fertility = -1; // the owner's setting: -1 the world's, 0 fertile, 1 infertile
	float x = 0, y = 0;
	float foot = -1; // where its feet were (y), or -1 (older saves)
	unsigned int age = 0, ticks = 0;
	std::vector<int> chemicals;
	std::vector<float> organs;
	// the owner's records (monikerData)
	std::string name, notes, epitaph, gravephoto;
	bool registered = false, hasborn = false, bornknown = false;
	unsigned int born = 0;
	std::vector<std::string> photos;
	std::vector<storyEntry> story; // its life story
	std::string moniker, mother, father; // as saved (monikers used to be new each load)
	bool spliced = false; // "parents M F spliced": made by the gene splicer
	// a record only (a creature that has died): its kind, sex and death
	bool record = false;
	int genus = 1, gender = -1, diedstage = -1;
	unsigned int diedtick = 0;
};

// one line of free text in state.txt: backslashes and line breaks escaped
static std::string escapeLine(const std::string& t) {
	std::string o;
	for (char ch : t) {
		if (ch == '\\')
			o += "\\\\";
		else if (ch == '\n')
			o += "\\n";
		else if (ch != '\r')
			o += ch;
	}
	return o;
}

static std::string unescapeLine(const std::string& t) {
	std::string o;
	for (size_t i = 0; i < t.size(); i++) {
		if (t[i] == '\\' && i + 1 < t.size()) {
			o += t[i + 1] == 'n' ? '\n' : t[i + 1];
			i++;
		} else {
			o += t[i];
		}
	}
	return o;
}

// the rest of a line after its key and one space
static std::string restOfLine(std::istringstream& l) {
	std::string rest;
	std::getline(l, rest);
	if (!rest.empty() && rest[0] == ' ')
		rest.erase(0, 1);
	return rest;
}

struct SavedEgg {
	bool kept = false; // a kept object rather than an egg
	int frames = 8, plane = 2000;
	int family = 2, genus = 5, species = 0;
	std::string sprite = "eggs";
	unsigned int firstimg = 0, pose = 0, attr = 195, timer = 0;
	float x = 0, y = 0;
	std::vector<std::string> vars; // "setv ovNN value" lines
};

// Saves made before chemical relay lobes ran (see oldBrain::tickChemicalRelays)
// can hold creatures whose ATP ran out because of that missing relay. With no
// ATP, C2 organs do nothing, and the relay alone cannot restart them (the
// enzyme it releases is itself emitted by an organ). When loading such a save,
// a living creature with no ATP gets its genome's starting ATP back, once.
static unsigned char genomeInitialLevel(genomeFile& g, unsigned char chemical, unsigned char fallback) {
	for (auto& gn : g.genes)
		if (auto* ic = dynamic_cast<bioInitialConcentrationGene*>(gn.get()))
			if (ic->chemical == chemical)
				return ic->quantity;
	return fallback;
}

// Builds a creature from a genome and a saved state. With a moniker, the new
// body takes over that existing moniker (and its history) instead of a new one.
static Agent* buildCreature(std::shared_ptr<genomeFile> genome, const SavedCreature& s, bool revive_atp, const std::string& moniker = "") {
	SkeletalCreature* a = new SkeletalCreature(4);
	oldCreature* c;
	if (engine.version == 1)
		c = new c1Creature(genome, s.female, (unsigned char)s.variant, a);
	else
		c = new c2Creature(genome, s.female, (unsigned char)s.variant, a);
	a->setCreature(c);
	a->finishInit();
	a->genome_slots[0] = genome;
	if (moniker.empty()) {
		world.newMoniker(genome, "saved", a);
		world.history->getMoniker(world.history->findMoniker(genome)).moveToCreature(a);
	} else {
		world.history->getMoniker(moniker).transferCreature(a);
	}

	{
		bool wasquiet = world.history->quiet;
		world.history->quiet = true; // stages reached before, not now
		for (int i = 0; i < s.stage; i++)
			c->ageCreature(); // also updates genes and appearance for each stage
		world.history->quiet = wasquiet;
	}
	if (s.ageing) {
		bool wasquiet = world.history->quiet;
		world.history->quiet = true; // not a new birth
		c->born();
		world.history->quiet = wasquiet;
	}
	c->restoreLifeCounters(s.ticks, s.age);
	if (s.baby.size() == 4) {
		a->babymoniker = genomeNameToInt(s.baby);
		c->setPregnant(255);
	}
	if (!s.temperament.empty())
		Temperaments::fromLine(s.temperament, c->temperament);
	c->temperamentBorn = c->temperament;
	if (!s.temperamentBorn.empty())
		Temperaments::fromLine(s.temperamentBorn, c->temperamentBorn);
	if (!s.temperamentDrift.empty())
		Temperaments::driftFromLine(s.temperamentDrift, c->temperamentDrift);
	// (saves before 0.20.0 have no born line: the saved temperament is it)
	c->refreshTemperament();
	if (!s.seasonal.empty())
		c->seasonalZ = std::clamp((float)std::atof(s.seasonal.c_str()), -2.0f, 2.0f);
	if (!s.lifespanScale.empty())
		c->lifespanScale = std::clamp((float)std::atof(s.lifespanScale.c_str()), 0.5f, 2.0f);
	if (!s.mindGenes.empty())
		MindGeneSet::fromLine(s.mindGenes, c->mindGenes);
	c->fertilityOverride = s.fertility;
	for (auto& line : s.bonds) {
		std::string other;
		Bond b;
		if (Relationships::fromLine(line, other, b))
			c->bonds[other] = b;
	}
	for (auto& w : s.words) {
		if (w.first < 0 || w.first > 1000)
			continue;
		if (c->vocab.size() <= (size_t)w.first)
			c->vocab.resize(std::max((size_t)w.first + 1, engine.wordlist.size()), 0);
		c->vocab[w.first] = (unsigned char)std::clamp(w.second, 0, 255);
	}
	for (size_t i = 0; i < s.chemicals.size() && i < 256; i++)
		c->setChemicalLevel((unsigned char)i, (unsigned char)s.chemicals[i]);
	c->setOrganState(s.organs);
	if (revive_atp && engine.version == 2 && c->isAlive() && c->getChemical(99) == 0) { // 99 = ATP
		unsigned char atp = genomeInitialLevel(*genome, 99, 200);
		c->setChemicalLevel(99, atp);
		fmt::print("* Restarted a creature's metabolism (ATP 0 -> {}) after loading an older save\n", (int)atp);
	}
	if (s.asleep)
		c->setAsleep(true);
	a->moveTo(s.x, s.y, true);
	if (s.foot >= 0)
		if (auto* sc = dynamic_cast<SkeletalCreature*>(a)) {
			float dy = s.foot - sc->footY();
			if (std::fabs(dy) > 0.5f && std::fabs(dy) < 120)
				a->moveTo(a->x, a->y + dy, true);
		}
	if (CreatureMind* m = c->getMind()) {
		// the llm mind (phase 1) saves exactly the homeostat's state, so a
		// world can move between the two without losing what creatures learned
		auto homeostatFamily = [](const std::string& n) { return n == "homeostat" || n == "llm"; };
		if (m->name() == s.mind || (homeostatFamily(m->name()) && homeostatFamily(s.mind)))
			m->loadState(s.mindstate);
		else if (!s.mind.empty())
			fmt::print("* Note: creature saved with mind '{}' loaded with '{}'; learned state not restored\n", s.mind, m->name());
	}
	if (s.selected)
		world.selectCreature(AgentRef(a).lock());
	return a;
}

static void restoreCreature(const fs::path& savedir, const SavedCreature& s, bool revive_atp) {
	auto genome = std::make_shared<genomeFile>();
	{
		FileReader r(savedir / "genomes" / s.genome);
		r >> *genome;
	}
	Agent* a = buildCreature(genome, s, revive_atp);
	if (!a)
		return;
	std::string mon = world.history->findMoniker(genome);
	if (!s.moniker.empty() && world.history->renameMoniker(mon, s.moniker))
		mon = s.moniker; // the same moniker as before, so parents and the log still find it
	monikerData& m = world.history->getMoniker(mon);
	m.name = s.name;
	m.mother = s.mother;
	m.father = s.father;
	m.spliced = s.spliced;
	historyManager::lineageGeneration++;
	m.story = s.story;
	m.registered = s.registered;
	m.photos = s.photos;
	m.notes = s.notes;
	if (s.hasborn) {
		m.borntick = s.born;
		m.bornknown = s.bornknown;
	} else {
		m.bornknown = false; // saved before birth times were kept: counts from now
	}
}

static void restoreEgg(const SavedEgg& e) {
	std::string script = fmt::format("new: simp {} {} {} {} 0\n", e.sprite, e.frames, e.firstimg, e.plane);
	script += fmt::format("pose {}\n", e.pose);
	script += fmt::format("setv cls2 {} {} {}\nsetv attr {}\n", e.family, e.genus, e.species, e.attr);
	for (auto& v : e.vars)
		script += v + "\n";
	if (e.timer)
		script += fmt::format("tick {}\n", e.timer);
	script += fmt::format("mvto {} {}\n", e.x, e.y);
	if (!e.kept)
		script += "setv grav 1\n";
	std::string err = engine.executeNetwork(script);
	if (!err.empty())
		fmt::print("* Couldn't restore an egg: {}\n", err);
}

namespace {
struct SavedView {
	bool set = false;
	int x = 0, y = 0;
	float zoom = 1.0f;
} s_view;
} // namespace

void ApplySavedView() {
	if (!s_view.set)
		return;
	if (s_view.zoom >= 0.25f && s_view.zoom <= 4.0f)
		engine.setZoom(s_view.zoom);
	engine.camera->trackAgent(AgentRef(), 0, 0, brittle, none);
	engine.camera->moveToGlobal(s_view.x, s_view.y, jump);
}

// A save this version cannot read is moved aside, never left to be replaced
// by the next autosave (owner, 2026-11-21: old worlds must survive every new
// version). It stays in the world folder as save.unreadable-DATE for a later
// version, or a person, to recover.
const SetAsideSave& LastSetAside() {
	return s_setAside;
}

void SetAsideUnreadableSave(const fs::path& worlddir, const std::string& why) {
	fs::path dest = worlddir / "save";
	if (!fs::exists(dest))
		return;
	std::time_t now = std::time(nullptr);
	char stamp[32];
	std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
	fs::path aside = worlddir / (std::string("save.unreadable-") + stamp);
	std::error_code ec;
	fs::rename(dest, aside, ec);
	s_setAside = {worlddir, ec ? fs::path() : aside, why, true};
	if (ec)
		fmt::print("* This world's save could not be read ({}) nor moved aside ({}); this session will not save over it\n", why, ec.message());
	else
		fmt::print("* This world's save could not be read ({}); it was kept as {} and the world starts from its world file\n", why, aside.filename().string());
}

bool PutBackSetAsideSave(std::string& error) {
	if (!s_setAside.happened || s_setAside.kept.empty())
		return true; // (never moved: still where it was)
	std::error_code ec;
	fs::path dest = s_setAside.worlddir / "save";
	if (fs::exists(dest, ec)) {
		error = "This world has been saved since, so the old save stays where it was kept.";
		return false;
	}
	fs::rename(s_setAside.kept, dest, ec);
	if (ec) {
		error = "Couldn't put the save back: " + ec.message();
		return false;
	}
	s_setAside.kept.clear();
	return true;
}

bool LoadWorldState(const fs::path& worlddir) {
	s_view = SavedView();
	world.allFertile = true; // (saves made before the setting have every creature fertile)
	world.injectedCobs.clear();
	world.injectedScripts.clear();
	if (!HasSavedState(worlddir))
		return false;
	fs::path savedir = worlddir / "save";
	std::ifstream in(stateFile(worlddir), std::ios::binary);
	std::string line;
	std::getline(in, line);
	// (a save copied from elsewhere, or edited, may have \r\n line ends)
	auto dropCR = [](std::string& t) {
		if (!t.empty() && t.back() == '\r')
			t.pop_back();
	};
	dropCR(line);
	if (line != MAGIC) {
		in.close();
		SetAsideUnreadableSave(worlddir, "an unknown format: " + line.substr(0, 40));
		return false;
	}

	std::vector<SavedCreature> creatures;
	std::vector<SavedEgg> eggs;
	std::vector<lifeLogEntry> lifelog, savedNotices;
	std::vector<lifeLogEntry>* lastEntry = nullptr; // (the list the last life/notice line went to)
	std::vector<SavedCreature> records;
	std::map<int, unsigned int> namecount;
	bool namesgiven = false;
	std::vector<std::pair<unsigned int, int>> ecologypins;
	struct SavedNutrients {
		unsigned int room;
		float ontr, intr;
		int ontrBase, intrBase;
	};
	std::vector<SavedNutrients> nutrients;
	int camx = -1, camy = -1;
	float camzoom = 1.0f;
	bool relays_saved = false;
	SavedCreature* c = nullptr;
	SavedEgg* e = nullptr;
	std::vector<SavedInjected> injected;
	std::vector<SavedInjected> worldObjects;
	bool hasWorldObjects = false;
	std::vector<std::string> injectedCobs;
	SavedInjected* io = nullptr;
	while (std::getline(in, line)) {
		dropCR(line);
		std::istringstream l(line);
		std::string key;
		l >> key;
		if (key == "chemical-relays") {
			relays_saved = true;
		} else if (key == "camera") {
			l >> camx >> camy;
			if (!(l >> camzoom)) // saved before the zoom was
				camzoom = 1.0f;
		} else if (key == "all-fertile") {
			int v = 1;
			if (l >> v)
				world.allFertile = v != 0;
		} else if (key == "hand") {
			int inv = 0, push = 0;
			if (l >> inv >> push) {
				world.handInvisible = inv != 0;
				world.handPush = push != 0;
			}
		} else if (key == "calendar") {
			unsigned int wt, tod, dos, sea, yr;
			if (l >> wt >> tod >> dos >> sea >> yr) {
				world.worldtickcount = wt;
				world.timeofday = tod % 5;
				world.dayofseason = dos % 4;
				world.season = sea % 4;
				world.year = yr;
			}
		} else if (key == "life" || key == "notice") {
			lifeLogEntry le;
			int female = 0, ageknown = 1, oldage = 0;
			if (l >> le.kind >> le.tick >> le.genus >> female >> le.stage >> le.age >> ageknown >> oldage >> le.moniker) {
				le.female = female != 0;
				le.ageknown = ageknown != 0;
				le.oldage = oldage != 0;
				le.name = unescapeLine(restOfLine(l));
				if (le.moniker == "-")
					le.moniker.clear();
				(key == "life" ? lifelog : savedNotices).push_back(le);
				lastEntry = key == "life" ? &lifelog : &savedNotices;
			}
		} else if (key == "life-cause") {
			if (lastEntry && !lastEntry->empty())
				lastEntry->back().cause = unescapeLine(restOfLine(l));
		} else if (key == "life-place") {
			if (lastEntry && !lastEntry->empty())
				l >> lastEntry->back().x >> lastEntry->back().y >> lastEntry->back().room;
		} else if (key == "life-egg") {
			if (lastEntry && !lastEntry->empty())
				l >> lastEntry->back().child;
		} else if (key == "names") {
			int genus;
			unsigned int n;
			if (l >> genus >> n)
				namecount[genus] = n;
		} else if (key == "names-given") {
			l >> namesgiven;
		} else if (key.rfind("disease-", 0) == 0) {
			Disease::readLine(key, l);
		} else if (key.rfind("exposure-", 0) == 0) {
			Exposure::readLine(key, l);
		} else if (key == "nutrients") {
			SavedNutrients n;
			if (l >> n.room >> n.ontr >> n.intr >> n.ontrBase >> n.intrBase)
				nutrients.push_back(n);
		} else if (key == "ecology-pin") {
			unsigned int room;
			int color;
			if (l >> room >> color)
				ecologypins.push_back({room, color});
		} else if (key == "injected" && !c && !e && !io) {
			injectedCobs.push_back(unescapeLine(restOfLine(l)));
		} else if (key == "injscript" && !c && !e && !io) {
			std::string rest = restOfLine(l);
			size_t tab = rest.find('\t');
			if (tab != std::string::npos)
				world.injectedScripts[unescapeLine(rest.substr(0, tab))].push_back(unescapeLine(rest.substr(tab + 1)));
		} else if (key == "creature" || key == "record") {
			auto& list = key == "creature" ? creatures : records;
			list.emplace_back();
			c = &list.back();
			c->record = key == "record";
			e = nullptr;
			io = nullptr;
		} else if (key == "egg" || key == "object") {
			eggs.emplace_back();
			e = &eggs.back();
			e->kept = key == "object";
			c = nullptr;
			io = nullptr;
		} else if (key == "world-objects") {
			hasWorldObjects = true;
		} else if (key == "wobject") {
			worldObjects.emplace_back();
			io = &worldObjects.back();
			c = nullptr;
			e = nullptr;
		} else if (key == "iobject") {
			injected.emplace_back();
			io = &injected.back();
			c = nullptr;
			e = nullptr;
		} else if (key == "end") {
			c = nullptr;
			e = nullptr;
			io = nullptr;
		} else if (io) {
			readInjectedLine(*io, key, l, [&] { return unescapeLine(restOfLine(l)); });
		} else if (c) {
			if (key == "genome")
				l >> c->genome;
			else if (key == "female")
				l >> c->female;
			else if (key == "variant")
				l >> c->variant;
			else if (key == "selected")
				l >> c->selected;
			else if (key == "pos")
				l >> c->x >> c->y;
			else if (key == "foot")
				l >> c->foot;
			else if (key == "stage")
				l >> c->stage;
			else if (key == "age")
				l >> c->age;
			else if (key == "ticks")
				l >> c->ticks;
			else if (key == "ageing")
				l >> c->ageing;
			else if (key == "asleep")
				l >> c->asleep;
			else if (key == "baby")
				l >> c->baby;
			else if (key == "temperament")
				std::getline(l, c->temperament);
			else if (key == "temperament-born")
				std::getline(l, c->temperamentBorn);
			else if (key == "temperament-drift")
				std::getline(l, c->temperamentDrift);
			else if (key == "seasonal")
				std::getline(l, c->seasonal);
			else if (key == "lifespan-scale")
				l >> c->lifespanScale;
			else if (key == "mind-genes")
				std::getline(l, c->mindGenes);
			else if (key == "fertility")
				l >> c->fertility;
			else if (key == "bond") {
				std::string rest;
				std::getline(l, rest);
				c->bonds.push_back(rest);
			} else if (key == "words") {
				std::string pair;
				while (l >> pair) {
					size_t colon = pair.find(':');
					if (colon != std::string::npos)
						c->words.push_back({atoi(pair.substr(0, colon).c_str()), atoi(pair.substr(colon + 1).c_str())});
				}
			}
			else if (key == "moniker")
				l >> c->moniker;
			else if (key == "parents") {
				l >> c->mother >> c->father;
				std::string how;
				c->spliced = (l >> how) && how == "spliced";
				if (c->mother == "-")
					c->mother.clear();
				if (c->father == "-")
					c->father.clear();
			} else if (key == "genus")
				l >> c->genus >> c->gender;
			else if (key == "died")
				l >> c->diedtick >> c->diedstage;
			else if (key == "name")
				c->name = unescapeLine(restOfLine(l));
			else if (key == "registered")
				l >> c->registered;
			else if (key == "born") {
				l >> c->born >> c->bornknown;
				c->hasborn = true;
			} else if (key == "photo")
				c->photos.push_back(unescapeLine(restOfLine(l)));
			else if (key == "story") {
				storyEntry e;
				l >> e.tick >> e.kind >> e.other;
				if (e.other == "-")
					e.other.clear();
				e.detail = unescapeLine(restOfLine(l));
				c->story.push_back(e);
			}
			else if (key == "notes")
				c->notes = unescapeLine(restOfLine(l));
			else if (key == "epitaph")
				c->epitaph = unescapeLine(restOfLine(l));
			else if (key == "gravephoto")
				c->gravephoto = unescapeLine(restOfLine(l));
			else if (key == "chemicals") {
				int v;
				while (l >> v)
					c->chemicals.push_back(v);
			} else if (key == "organs") {
				float v;
				while (l >> v)
					c->organs.push_back(v);
			} else if (key == "mind")
				l >> c->mind;
			else if (key == "mindstate") {
				while (std::getline(in, line)) {
					dropCR(line);
					if (line == "endmindstate")
						break;
					c->mindstate += line + "\n";
				}
			}
		} else if (e) {
			if (key == "classifier")
				l >> e->family >> e->genus >> e->species;
			else if (key == "sprite")
				l >> e->sprite >> e->firstimg >> e->pose;
			else if (key == "attr")
				l >> e->attr;
			else if (key == "frames")
				l >> e->frames;
			else if (key == "plane")
				l >> e->plane;
			else if (key == "timer")
				l >> e->timer;
			else if (key == "pos")
				l >> e->x >> e->y;
			else if (key == "var") {
				int n;
				std::string type, value;
				l >> n >> type >> value;
				if (n >= 0 && n < 100)
					e->vars.push_back(fmt::format("setv ov{:02d} {}", n, value));
			}
		}
	}

	// the saved creatures and eggs replace those in the world file
	std::vector<std::shared_ptr<Agent>> remove;
	for (auto& a : world.agents)
		if (a && (oldCreatureOf(a.get()) || isEgg(a.get()) || isKept(a.get()) || isInjected(a.get())))
			remove.push_back(a);
	for (auto& a : remove)
		a->kill();

	world.history->lifelog = lifelog;
	world.history->notices.clear();
	world.history->shownNotices = savedNotices;
	world.history->namecount = namecount;
	world.history->namesgiven = namesgiven;
	world.history->ecologypins = ecologypins;
	// the nutrient cycle: the world file's levels are the rooms' own (taken
	// before the saved ones are put back, for saves from before 2026-11-17)
	if (world.map) {
		for (auto& r : world.map->getRooms()) {
			if (r->ontrBase < 0)
				r->ontrBase = r->ontr;
			if (r->intrBase < 0)
				r->intrBase = r->intr;
		}
		for (auto& n : nutrients)
			if (auto r = world.map->getRoom(n.room)) {
				r->ontrf = std::clamp(n.ontr, 0.0f, 255.0f);
				r->intrf = std::clamp(n.intr, 0.0f, 255.0f);
				r->ontr = (int)std::lround(r->ontrf);
				r->intr = (int)std::lround(r->intrf);
				if (n.ontrBase >= 0)
					r->ontrBase = n.ontrBase;
				if (n.intrBase >= 0)
					r->intrBase = n.intrBase;
			}
	}
	for (auto& s : creatures) {
		try {
			restoreCreature(savedir, s, !relays_saved);
		} catch (std::exception& ex) {
			fmt::print("* Couldn't restore a creature ({}): {}\n", s.genome, ex.what());
		}
	}
	for (auto& r : records) {
		if (r.moniker.empty() || world.history->hasMoniker(r.moniker))
			continue;
		monikerData& m = world.history->addRecord(r.moniker, (unsigned char)r.genus, r.gender);
		m.name = r.name;
		m.registered = r.registered;
		m.photos = r.photos;
		m.story = r.story;
		m.notes = r.notes;
		m.epitaph = r.epitaph;
		m.gravephoto = r.gravephoto;
		m.borntick = r.born;
		m.bornknown = r.bornknown;
		m.died = true;
		m.diedtick = r.diedtick;
		m.diedstage = r.diedstage;
		m.mother = r.mother;
		m.father = r.father;
		m.spliced = r.spliced;
		historyManager::lineageGeneration++;
	}
	for (auto& s : eggs)
		restoreEgg(s);
	// objects added by hand from the Agent Injector: their scripts, then
	// the objects (none of a COB that is no longer there)
	std::set<std::string> missing;
	for (auto& name : injectedCobs) {
		std::string err;
		if (CobManager::reinstallScripts(name, err)) {
			world.injectedCobs.push_back(name);
		} else {
			missing.insert(name);
			fmt::print("* Injected object \"{}\" not restored: {}\n", name, err);
		}
	}
	injected.erase(std::remove_if(injected.begin(), injected.end(), [&](const SavedInjected& o) { return missing.count(o.from) > 0; }), injected.end());
	restoreInjected(injected);
	int wmatched = 0, wremoved = 0, wmade = 0;
	// (developer aid: OPENC2E_NO_WORLD_OBJECTS loads as before stage 3)
	if (devenv("OPENC2E_NO_WORLD_OBJECTS"))
		hasWorldObjects = false;
	// a save without a single object was written by File > Exit up to 0.28.1,
	// which emptied the world first: the world file's own objects stay
	if (hasWorldObjects && worldObjects.empty()) {
		fmt::print("* The save lists no objects, so the world's own are kept\n");
		hasWorldObjects = false;
	}
	if (hasWorldObjects)
		restoreWorldObjects(worldObjects, wmatched, wremoved, wmade);
	s_view = {camx >= 0 && camy >= 0, camx, camy, camzoom};
	ApplySavedView(); // and again once the world has finished starting
	int nkept = (int)std::count_if(eggs.begin(), eggs.end(), [](const SavedEgg& e) { return e.kept; });
	fmt::print("* Restored saved world: {} creatures, {} eggs, {} kept objects, {} injected objects ({} add-ons)\n", creatures.size(), (int)eggs.size() - nkept, nkept,
		injected.size(), world.injectedCobs.size());
	// the player's Genetics folder tidied of genomes no world uses (Breeding.h)
	tidyGeneratedGenomes();
	if (hasWorldObjects)
		fmt::print("* Restored world objects: {} from the world file as saved, {} gone during play removed, {} made during play made again\n", wmatched, wremoved, wmade);
	return true;
}

} // namespace WorldSave

// Changing a creature's life stage at will (owner request, 2026-11-08). C1/C2
// life stages follow a "life" chemical: the genome's ageing receptors (organ
// 1, tissue 0, loci 0-6) each fire when it falls to their threshold, and the
// one for the current stage moves the creature on. So besides the stage, the
// chemical is set to where the new stage begins (the previous stage's
// threshold; the starting level for a baby), or the creature would at once
// age back or keep its old timing.
static int lifeLevelForStage(oldCreature* c, int stage) {
	std::shared_ptr<genomeFile> g = c->getGenome();
	int chem = -1, threshold = -1;
	for (auto& gn : g->genes) {
		auto* r = dynamic_cast<bioReceptorGene*>(gn.get());
		if (!r || r->organ != 1 || r->tissue != 0)
			continue;
		if (r->header.flags.notexpressed)
			continue;
		chem = r->chemical;
		if (stage > 0 && r->locus == stage - 1)
			threshold = r->threshold;
	}
	if (chem < 0)
		return -1;
	if (stage == 0)
		threshold = WorldSave::genomeInitialLevel(*g, (unsigned char)chem, 255);
	if (threshold < 0)
		return -1;
	return (chem << 8) | threshold;
}

namespace WorldSave {

bool SetCreatureLifeStage(std::shared_ptr<Agent>& agent, int stage, std::string& error) {
	oldCreature* c = agent ? oldCreatureOf(agent.get()) : nullptr;
	if (!c || !c->isAlive()) {
		error = "not a living C1/C2 creature";
		return false;
	}
	stage = std::clamp(stage, 0, 6);
	int current = (int)c->getStage();
	if (stage == current)
		return true;
	if (stage > current) {
		while ((int)c->getStage() < stage)
			c->ageCreature(); // genes and appearance for each stage, as natural ageing
	} else {
		// genes that have switched on cannot be switched off: rebuild the
		// creature at the younger stage with its chemistry, organs and mind
		SavedCreature s;
		s.female = c->isFemale();
		s.variant = c->getVariant();
		s.selected = world.selectedcreature.lock() == agent;
		s.x = agent->x;
		s.y = agent->y;
		s.stage = stage;
		s.age = c->getAge();
		s.ticks = c->getTicks();
		s.ageing = c->isAgeing();
		s.asleep = c->isAsleep();
		for (int i = 0; i < 256; i++)
			s.chemicals.push_back(c->getChemical(i));
		s.organs = c->getOrganState();
		if (CreatureMind* m = c->getMind()) {
			s.mind = m->name();
			s.mindstate = m->saveState();
		}
		// and what it inherited (a child restored past its saved stage is
		// rebuilt here at load: its temperament, rhythm, pace of ageing and
		// mind genes were drawn afresh; found 2026-11-20)
		s.temperament = Temperaments::toLine(c->temperament);
		s.temperamentBorn = Temperaments::toLine(c->temperamentBorn);
		if (c->temperamentDrift.any())
			s.temperamentDrift = Temperaments::driftLine(c->temperamentDrift);
		s.seasonal = fmt::format("{:.3f}", c->seasonalZ);
		s.lifespanScale = fmt::format("{:.3f}", c->lifespanScale);
		s.mindGenes = MindGeneSet::toLine(c->mindGenes);
		s.fertility = c->fertilityOverride;
		std::shared_ptr<genomeFile> genome = c->getGenome();
		std::string moniker = world.history->findMoniker(genome);
		agent->kill();
		Agent* a = buildCreature(genome, s, false, moniker);
		c = oldCreatureOf(a);
		agent = AgentRef(a).lock();
	}
	int life = lifeLevelForStage(c, stage);
	if (life >= 0)
		c->setChemicalLevel((unsigned char)(life >> 8), (unsigned char)(life & 255));
	fmt::print("* Creature life stage {} -> {}{}\n", current, (int)c->getStage(),
		life >= 0 ? fmt::format(" (life chemical {} set to {})", life >> 8, life & 255) : "");
	return true;
}

} // namespace WorldSave

// Changing life stage "naturally" (owner request): one stage at a time, with
// a short pause between stages. Older: the life chemical is lowered to where
// the current stage ends, so the genome's own ageing receptor fires and the
// creature ages exactly as it would naturally (genes, body, life event); if
// the genome has no such receptor, it is aged directly after a while.
// Younger has no natural process; each stage back rebuilds the creature (see
// SetCreatureLifeStage), at the same pace.
namespace WorldSave {

struct StageChange {
	AgentRef creature;
	int target = 0;
	unsigned int wait = 0; // ticks before the next step
	int stepFrom = -1; // stage when the current ageing step began (-1: none)
	unsigned int stepTicks = 0;
};
static std::vector<StageChange> s_stagechanges;
static const unsigned int stagePause = 20; // ticks between stages (2 s in C2)
static const unsigned int stepTimeout = 60; // ticks to wait for the ageing receptor

void RequestLifeStage(std::shared_ptr<Agent> creature, int stage) {
	if (!creature || !oldCreatureOf(creature.get()))
		return;
	s_stagechanges.erase(std::remove_if(s_stagechanges.begin(), s_stagechanges.end(),
							 [&](StageChange& sc) { return sc.creature.lock() == creature; }),
		s_stagechanges.end());
	StageChange sc;
	sc.creature = AgentRef(creature);
	sc.target = std::clamp(stage, 0, 6);
	s_stagechanges.push_back(sc);
}

void TickLifeStageChanges() {
	for (auto it = s_stagechanges.begin(); it != s_stagechanges.end();) {
		std::shared_ptr<Agent> a = it->creature.lock();
		oldCreature* c = a ? oldCreatureOf(a.get()) : nullptr;
		if (!c || !c->isAlive() || (int)c->getStage() == it->target) {
			it = s_stagechanges.erase(it);
			continue;
		}
		int stage = (int)c->getStage();
		if (it->stepFrom >= 0) {
			// waiting for the body to age itself
			if (stage != it->stepFrom) {
				it->stepFrom = -1;
				it->wait = stagePause;
			} else if (++it->stepTicks >= stepTimeout) {
				c->ageCreature();
				it->stepFrom = -1;
				it->wait = stagePause;
			}
			++it;
			continue;
		}
		if (it->wait > 0) {
			it->wait--;
			++it;
			continue;
		}
		if (it->target > stage) {
			int life = lifeLevelForStage(c, stage + 1);
			if (life >= 0 && c->getChemical((unsigned char)(life >> 8)) > (life & 255))
				c->setChemicalLevel((unsigned char)(life >> 8), (unsigned char)(life & 255));
			it->stepFrom = stage;
			it->stepTicks = 0;
		} else {
			std::string err;
			if (!SetCreatureLifeStage(a, stage - 1, err)) {
				it = s_stagechanges.erase(it);
				continue;
			}
			it->creature = AgentRef(a);
			it->wait = stagePause;
		}
		++it;
	}
}

} // namespace WorldSave

