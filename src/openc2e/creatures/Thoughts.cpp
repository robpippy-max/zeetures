// Thought bubbles: see Thoughts.h.

#include "Thoughts.h"
#include "common/devenv.h"
#include "caosValue.h"
#include "creatures/CreatureMind.h"

#include "Agent.h"
#include "EmbeddedFiles.h"
#include "Engine.h"
#include "Camera.h"
#include "SpritePart.h"
#include "World.h"
#include "Bubble.h"
#include "imageManager.h"
#include "historyManager.h"
#include "common/backend/Backend.h"
#include "common/creaturesImage.h"
#include "common/io/SpanReader.h"
#include "creatures/CreatureAgent.h"
#include "creatures/CreatureSpeech.h"
#include "creatures/SkeletalCreature.h"
#include "creatures/oldCreature.h"
#include "fileformats/pngImage.h"

#include <algorithm>
#include <cmath>
#include <fmt/core.h>
#include <cstring>
#include <map>

namespace Thoughts {

namespace {

// the symbols, in the order data/thoughts/make_art.py draws them
const char* const SYMBOLS[] = {"hunger", "nose", "question", "heart", "feet", "sun", "snow", "zzz", "home", "people", "cross", "bang",
	"fear", "arrow_w", "arrow_e", "arrow_up", "arrow_down", "pain", "injury", "breath", "tired"};
const int CELL = 64;

const unsigned int SHOW_TICKS = 35; // 3.5 s, as a speech bubble
const unsigned int Z_TICKS = 2; // a sleeper's Z: 0.2 s a frame
const unsigned int SAME_AGAIN_TICKS = 300; // the same thought within 30 s: not shown again
const int PIC = 19; // a picture's size in the bubble
const int GAP = 3;

struct Item {
	int symbol = -1;
	AgentRef thing; // a thing or creature to picture
	bool face = false; // a creature: its face
};
struct Showing {
	AgentRef creature;
	std::vector<Item> items;
	std::string key;
	unsigned int start = 0;
};
std::map<unsigned int, Showing> s_showing; // by creature UNID
int s_mode = -1;

// our symbols as a game picture, one frame each (64 x 64)
std::shared_ptr<creaturesImage> symbols() {
	static std::shared_ptr<creaturesImage> img;
	static bool tried = false;
	if (!tried) {
		tried = true;
		auto bytes = EmbeddedFile("icons.png");
		if (!bytes.empty()) {
			try {
				SpanReader r(bytes);
				auto ims = ReadPngFile(r);
				if (!ims.empty() && ims[0].format == if_rgba32) {
					const Image& strip = ims[0];
					std::vector<Image> frames;
					for (int i = 0; i * CELL + CELL <= (int)strip.width; i++) {
						Image f;
						f.width = CELL;
						f.height = CELL;
						f.format = if_rgba32;
						f.data = shared_array<uint8_t>(size_t(CELL) * CELL * 4);
						for (int y = 0; y < CELL; y++)
							std::memcpy(&f.data[size_t(y) * CELL * 4], &strip.data[(size_t(y) * strip.width + size_t(i) * CELL) * 4], CELL * 4);
						frames.push_back(f);
					}
					img = imageManager::fromFrames("thought symbols", std::move(frames));
				}
			} catch (std::exception& e) {
				fmt::print("* Thoughts: icons.png: {}\n", e.what());
			}
		}
	}
	return img;
}

std::string monikerOf(Agent* a) {
	auto* ca = dynamic_cast<CreatureAgent*>(a);
	if (!ca || !ca->getCreature() || !world.history)
		return "";
	return world.history->findMoniker(ca->getCreature()->getGenome());
}

// a thing of a category to picture: the nearest one to the creature
AgentRef thingOf(Agent* creature, int category) {
	AgentRef best;
	float bestd = 1e18f;
	for (auto& a : world.agents) {
		// (the mind's own healing fruit: the volcanic bushes' fruit, 2 25 x OV02 2)
		bool match = category == MIND_HEALING_FRUIT
			? (a && a->family == 2 && a->genus == 25 && a->var[2].hasInt() && a->var[2].getInt() == 2)
			: category == MIND_ROTTEN_FRUIT ? (a && a->family == 2 && a->genus == 25 && a->species == 149)
			: (a && a->category == category);
		if (!a || !match || a->isDying() || dynamic_cast<CreatureAgent*>(a.get()))
			continue;
		if (!dynamic_cast<SpritePart*>(a->part(0)))
			continue;
		float d = std::hypot(a->x - creature->x, a->y - creature->y);
		if (d < bestd) {
			bestd = d;
			best = AgentRef(a);
		}
	}
	return best;
}

AgentRef creatureCalled(const std::string& moniker) {
	for (auto& a : world.agents)
		if (a && dynamic_cast<CreatureAgent*>(a.get()) && monikerOf(a.get()) == moniker)
			return AgentRef(a);
	return AgentRef();
}

// a picture fitted into a PIC x PIC square at (x, y)
void drawFitted(RenderTarget* surface, creaturesImage& img, unsigned int frame, float x, float y, int size) {
	if (frame >= img.numframes())
		return;
	float w = (float)img.width(frame), h = (float)img.height(frame);
	if (w <= 0 || h <= 0)
		return;
	float k = std::min(size / w, size / h);
	RenderOptions o;
	o.smooth = true;
	// (an explicit size: RenderOptions::scale is applied twice on this path)
	o.override_drawsize = true;
	o.overridden_drawwidth = std::max(1, (int)std::lround(w * k));
	o.overridden_drawheight = std::max(1, (int)std::lround(h * k));
	surface->renderCreaturesImage(img, frame, (int)std::lround(x + (size - w * k) / 2), (int)std::lround(y + (size - h * k) / 2), o);
}

} // namespace

Mode mode() {
	if (s_mode < 0) {
		std::string v = readPlayerSetting("thought-bubbles");
		s_mode = v == "0" ? OFF : v == "1" ? SELECTED : EVERY;
	}
	return (Mode)s_mode;
}

void setMode(Mode m) {
	s_mode = m;
	writePlayerSetting("thought-bubbles", std::to_string((int)m));
}

void think(Agent* creature, const std::vector<std::string>& items, const std::string& key) {
	if (!creature || items.empty() || engine.version != 2)
		return;
	unsigned int now = world.tickcount;
	auto it = s_showing.find(creature->getUNID());
	if (it != s_showing.end() && it->second.key == key && now < it->second.start + SAME_AGAIN_TICKS)
		return;
	Showing sh;
	sh.creature = AgentRef(creature);
	sh.key = key;
	sh.start = now;
	for (auto& t : items) {
		Item item;
		if (t.rfind("cat:", 0) == 0) {
			item.thing = thingOf(creature, atoi(t.c_str() + 4));
			if (!item.thing)
				continue; // none of that kind about to picture
		} else if (t.rfind("who:", 0) == 0) {
			item.thing = creatureCalled(t.substr(4));
			item.face = true;
			if (!item.thing)
				continue;
		} else {
			for (int i = 0; i < (int)(sizeof(SYMBOLS) / sizeof(SYMBOLS[0])); i++)
				if (t == SYMBOLS[i])
					item.symbol = i;
			if (item.symbol < 0)
				continue;
		}
		sh.items.push_back(item);
		if (sh.items.size() >= 4)
			break;
	}
	if (sh.items.empty())
		return;
	s_showing[creature->getUNID()] = sh;
	static const bool watch = devenv("OPENC2E_WATCH_THOUGHTS") != nullptr;
	if (watch)
		fmt::print("* THINKS {}: {}\n", creature->getUNID(), key);
}

// whether a thought bubble of its own is up now
static bool thoughtShowing(const Agent* c) {
	auto it = s_showing.find(c->getUNID());
	if (it == s_showing.end() || mode() == OFF || (mode() == SELECTED && world.selectedcreature.get() != c))
		return false;
	return world.tickcount >= it->second.start && world.tickcount < it->second.start + SHOW_TICKS;
}

// C2's stars for a creature out cold (owner, 2026-11-13: "a halo of stars
// starts circling around their heads in the same location as the
// indicator halo"): the player's own star.s16, ten frames of a ring of
// stars turning, drawn by the engine (no world script uses it) over each
// unconscious creature, where the selection halo sits: over its head,
// the ring's middle a little above the head's top
//
// And its Z's over a sleeping one, for as long as it sleeps (a player,
// 2026-11-22): the player's own zzzz.s16, a Z drifting up from the head,
// shrinking and fading over 16 frames (the 17th is empty), which no world
// script uses either. Each creature starts its own Z at its own time. Not
// while a bubble of its own is up there, nor over a rider hidden in a lift.
static void drawStates(RenderTarget* surface, int adjustx, int adjusty, int wrapWidth, Agent* skip) {
	std::shared_ptr<creaturesImage> star, zzzz;
	// (the creatures, gone over all the world's objects ten times a second,
	// not every frame)
	static std::vector<AgentRef> creatures;
	static unsigned int looked = 0;
	if (unsigned int now = engine.get_ticks_msec(); now - looked >= 100 || creatures.empty()) {
		looked = now;
		creatures.clear();
		for (auto& a : world.agents)
			if (a && a->family == 4)
				creatures.push_back(AgentRef(a));
	}
	for (auto& a : creatures) {
		if (!a || a->family != 4 || a->isDying())
			continue;
		auto* ca = dynamic_cast<CreatureAgent*>(a.get());
		auto* oc = ca ? dynamic_cast<oldCreature*>(ca->getCreature()) : nullptr;
		if (!oc || !oc->isAlive() || !(oc->isUnconscious() || oc->isAsleep()))
			continue;
		bool out = oc->isUnconscious();
		if (!out && (a.get() == skip || Bubble::hiddenRider(a.get()) || speechBubbleShowing(a.get()) || thoughtShowing(a.get())))
			continue;
		auto& image = out ? star : zzzz;
		if (!image) {
			image = world.gallery ? world.gallery->getImage(out ? "star" : "zzzz") : nullptr;
			if (!image || image->numframes() == 0)
				continue;
		}
		unsigned int frame = out ? world.tickcount % image->numframes() : (world.tickcount / Z_TICKS + a->getUNID() * 5) % image->numframes();
		int w = (int)image->width(frame), h = (int)image->height(frame);
		// round its head (lying down, its head is off to one side)
		float cx = a->x + a->getWidth() / 2.0f;
		float top = a->y;
		if (auto* sc = dynamic_cast<SkeletalCreature*>(a.get()))
			if (float dx, dy; sc->headTop(cx, top) && sc->drawnHeadShift(dx, dy)) // (as drawn: SmoothMovement.cpp)
				cx += dx, top += dy;
		for (int pass = 0; pass < (wrapWidth > 0 ? 3 : 1); pass++) {
			float shift = pass == 1 ? (float)wrapWidth : pass == 2 ? -(float)wrapWidth : 0.0f;
			float sx = cx - w / 2.0f - adjustx + shift;
			float sy = top - h * (out ? 0.6f : 0.85f) - adjusty;
			if (sx + w < 0 || sx > surface->getWidth())
				continue;
			surface->renderCreaturesImage(*image, frame, (int)sx, (int)sy);
		}
	}
}

void draw(RenderTarget* surface, int adjustx, int adjusty, int wrapWidth, Agent* skip) {
	if (engine.version == 2)
		drawStates(surface, adjustx, adjusty, wrapWidth, skip);
	// developer aid: OPENC2E_TEST_THOUGHT="hungry,east" (symbol names) makes
	// the creature nearest the view's middle think that, again whenever its
	// last has gone, for snapshots of the bubble
	if (static const char* tt = devenv("OPENC2E_TEST_THOUGHT"); tt && engine.version == 2) {
		std::vector<std::string> items;
		std::string s = tt;
		for (size_t p = 0; p != std::string::npos;) {
			size_t q = s.find(',', p);
			items.push_back(s.substr(p, q == std::string::npos ? q : q - p));
			p = q == std::string::npos ? q : q + 1;
		}
		float mx = adjustx + surface->getWidth() / 2.0f, my = adjusty + surface->getHeight() / 2.0f;
		Agent* best = nullptr;
		float bestd = 1e18f;
		for (auto& a : world.agents)
			if (a && a->family == 4 && !a->isDying() && !speechBubbleShowing(a.get()) && !Bubble::hiddenRider(a.get())) {
				float d = (a->x - mx) * (a->x - mx) + (a->y - my) * (a->y - my);
				if (d < bestd)
					bestd = d, best = a.get();
			}
		if (best && s_showing.find(best->getUNID()) == s_showing.end()) {
			if (mode() == SELECTED)
				world.selectCreature(AgentRef(best).lock());
			think(best, items, fmt::format("test{}", world.tickcount));
		}
	}
	if (s_showing.empty())
		return;
	unsigned int now = world.tickcount;
	Mode m = mode();
	std::shared_ptr<creaturesImage> syst = world.gallery ? world.gallery->getImage("syst") : nullptr;
	for (auto it = s_showing.begin(); it != s_showing.end();) {
		Showing& sh = it->second;
		Agent* c = sh.creature.get();
		if (!c || c->isDying() || now >= sh.start + SHOW_TICKS || now < sh.start) {
			it = s_showing.erase(it);
			continue;
		}
		++it;
		// one bubble at a time: while it speaks, its thought is not shown
		if (m == OFF || (m == SELECTED && world.selectedcreature.get() != c) || Bubble::hiddenRider(c) || speechBubbleShowing(c) || c == skip)
			continue;
		if (!syst || syst->numframes() < 30)
			continue;
		// C2's thought bubble: to the right of the creature when it is on
		// the left half of the view (tail bottom left), else to its left
		// (tail bottom right), its foot at the creature's top, as the hand's
		// typing bubble is placed
		int n = (int)sh.items.size();
		int need = 16 + n * PIC + (n - 1) * GAP;
		int size = need <= 112 ? 0 : need <= 176 ? 1 : 2;
		float cx = c->x + c->getWidth() / 2.0f - adjustx;
		// (a little to the left of the middle: a creature the camera follows
		// is at the middle, where its bubble would go from side to side)
		bool leftside = cx < engine.camera->getWidth() / 2.0f - 40.0f;
		unsigned int frame = (leftside ? 27 : 24) + size;
		int bw = (int)syst->width(frame), bh = (int)syst->height(frame);
		for (int pass = 0; pass < (wrapWidth > 0 ? 3 : 1); pass++) {
			float shift = pass == 1 ? (float)wrapWidth : pass == 2 ? -(float)wrapWidth : 0.0f;
			float bx = leftside ? c->x + c->getWidth() - 2 - adjustx + shift : c->x - bw + 2 - adjustx + shift;
			float by = c->y - bh - adjusty;
			float dx, dy;
			if (auto* sc = dynamic_cast<SkeletalCreature*>(c)) // (as drawn: SmoothMovement.cpp)
				if (sc->drawnShift(dx, dy))
					bx += dx, by += dy;
			if (bx + bw < 0 || bx > surface->getWidth())
				continue;
			surface->renderCreaturesImage(*syst, frame, (int)bx, (int)by);
			// the pictures, centred in the bubble's body (its top 22 px; the
			// tail's dots are below), set 2.5 px lower than the arithmetic
			// middle: the body's rim is heavier at the top, and at the middle
			// they looked 2-3 px high (owner, 2026-11-13)
			float x = bx + (bw - (n * PIC + (n - 1) * GAP)) / 2.0f;
			float y = by + 4.5f;
			for (auto& item : sh.items) {
				if (item.symbol >= 0) {
					if (auto sym = symbols())
						drawFitted(surface, *sym, (unsigned int)item.symbol, x, y, PIC);
				} else if (Agent* a = item.thing.get()) {
					if (item.face) {
						if (auto* sc = dynamic_cast<SkeletalCreature*>(a); sc && sc->headSprite())
							drawFitted(surface, *sc->headSprite(), 8, x - 2, y - 2, PIC + 4);
					} else if (auto* sp = dynamic_cast<SpritePart*>(a->part(0)); sp && sp->getSprite()) {
						drawFitted(surface, *sp->getSprite(), sp->getCurrentSprite(), x, y, PIC);
					}
				}
				x += PIC + GAP;
			}
		}
	}
}

} // namespace Thoughts
