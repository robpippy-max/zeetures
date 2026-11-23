#include "C2EyeView.h"
#include "common/devenv.h"

#include "Agent.h"
#include "C2KitsInternal.h"
#include "AgentHelpers.h"
#include "Camera.h"
#include "Catalogue.h"
#include "Engine.h"
#include "Map.h"
#include "MetaRoom.h"
#include "ImGuiUtils.h"
#include "Vehicle.h"
#include "World.h"
#include "common/backend/Backend.h"
#include "creatures/Creature.h"
#include "creatures/CreatureAgent.h"
#include "creatures/oldCreature.h"
#include "Room.h"
#include "common/Image.h"
#include "fileformats/genomeFile.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <imgui.h>
#include <map>
#include <memory>

namespace Openc2eImgui {

// A small window showing what the selected creature is paying attention to,
// seen through its eye (owner, 2026-11-18, after C2's own: a norn's round
// pupil, a grendel's slit, an ettin's coarse sight; our own art, drawn
// here). The view glides to centre whatever it attends to (no brackets);
// with nothing in particular in mind, it shows where the creature is (the
// creature itself is left out of the picture). The eye is alive: its pupil
// narrows in bright rooms and widens in the dark and with fear; its lids
// blink, droop as it grows sleepy and close when it sleeps; its iris takes
// the creature's own pigment; its sight reddens and doubles when it is hurt
// or ill, and ripples under water. A caption says what it is doing.

static constexpr int VIEW_W = 320;
static constexpr int VIEW_H = 240;

class EyeViewCamera : public Camera {
  public:
	unsigned int getWidth() const override { return VIEW_W; }
	unsigned int getHeight() const override { return VIEW_H; }
};

static bool s_eye_view_open = false;
static std::unique_ptr<EyeViewCamera> s_camera;
static std::shared_ptr<RenderTarget> s_target;
static AgentRef s_followed;
static float s_cx = 0, s_cy = 0; // the view's center, in world pixels

bool IsEyeViewOpen() {
	return s_eye_view_open;
}

void SetEyeViewOpen(bool open) {
	s_eye_view_open = open;
}

// What the creature is doing, in plain words (C1/C2 decisions; the world's
// creature scripts carry them out).
static const char* actionWords(int decision) {
	switch (decision) {
		case 0: return "resting";
		case 1: return "pushing";
		case 2: return "pulling";
		case 3: return "stopping";
		case 4: return "going to";
		case 5: return "backing away from";
		case 6: return "picking up";
		case 7: return "dropping";
		case 8: return "asking about";
		case 10: return "walking west past";
		case 11: return "walking east past";
		case 12: return "eating";
		case 13: return "hitting";
		case 25: return "sleeping by";
		case 26: return "walking west past";
		case 27: return "walking east past";
		default: return "looking at";
	}
}

static std::string describe(Agent* a) {
	if (!a)
		return "nothing in particular";
	if (dynamic_cast<CreatureAgent*>(a))
		return "another creature";
	std::string name = catalogue.getAgentName(a->family, a->genus, a->species);
	if (!name.empty())
		return name;
	if (dynamic_cast<Vehicle*>(a))
		return "a lift or vehicle";
	float edible = edibleScent(a);
	if (edible < 0)
		return "something poisonous";
	if (edible > 0)
		return "some food";
	// the game's own word for its kind ("a toy"), as creatures know it
	if (a->category >= 0) {
		std::string noun = KitCategoryNoun(a->category);
		if (noun.rfind("kind ", 0) != 0) {
			for (char& ch : noun)
				ch = (char)tolower((unsigned char)ch);
			// the words that make no sense with "a" before them ("dropping
			// a nature", as a player saw; 2026-11-22)
			static const std::map<std::string, const char*> phrases = {
				{"me", "itself"},
				{"hand", "the hand"},
				{"nature", "something growing wild"},
				{"food", "some food"},
				{"drink", "something to drink"},
				{"dispensor", "a dispenser"},
				{"implement", "a tool"},
				{"detritus", "some litter"},
				{"medicine", "some medicine"},
				{"weather", "the weather"},
				{"badplant", "a harmful plant"},
				{"badbug", "a harmful bug"},
				{"badcritter", "a harmful critter"},
				{"mover", "a vehicle"},
				{"mediabox", "a media box"},
				{"leftright", "a sign"},
			};
			if (auto it = phrases.find(noun); it != phrases.end())
				return it->second;
			return fmt::format("{} {}", std::string("aeiou").find(noun[0]) != std::string::npos ? "an" : "a", noun);
		}
	}
	return "something";
}

std::string DescribeCreatureActivity(Agent* self) {
	auto* ca = dynamic_cast<CreatureAgent*>(self);
	Creature* cr = ca ? ca->getCreature() : nullptr;
	if (!cr)
		return {};
	if (!cr->isAlive())
		return "Dead";
	if (cr->isAsleep())
		return "Asleep";
	Agent* focus = cr->getAttentionFocus().get();
	if (focus == self || (focus && focus->isDying()))
		focus = nullptr;
	if (!focus)
		return "Not paying attention to anything";
	std::string s;
	if (cr->getDecisionId() == 7) // (drop: what it holds, whatever it is looking at)
		s = self->carrying ? fmt::format("putting down {}", describe(self->carrying.get())) : fmt::format("looking at {}", describe(focus));
	else
		s = fmt::format("{} {}", actionWords(cr->getDecisionId()), describe(focus));
	if (!s.empty())
		s[0] = (char)toupper((unsigned char)s[0]);
	return s;
}

// the shorter way from x0 to x1 in the creature's metaroom, if it wraps
static MetaRoom* s_room = nullptr;
static float wrapDelta(float x0, float x1) {
	float d = x1 - x0;
	MetaRoom* m = s_room;
	if (m && m->wraparound()) {
		float w = (float)m->width();
		if (d > w / 2)
			d -= w;
		else if (d < -w / 2)
			d += w;
	}
	return d;
}


// ---- the eye ------------------------------------------------------------------

namespace {

struct EyeLook {
	int kind = 0; // 0 norn (round pupil), 1 grendel (slit), 2 ettin (coarse sight)
	float iris[3] = {60, 90, 200};
	float lid[3] = {205, 160, 110};
};

// the creature's eye: its kind's look, tinted by its own pigment genes
// (C2: red, green and blue pigment amounts, 128 neutral)
EyeLook lookOf(Agent* c, Creature* cr) {
	EyeLook l;
	int genus = c ? c->genus : 1;
	if (genus == 2) {
		l.kind = 1;
		l.iris[0] = 200, l.iris[1] = 160, l.iris[2] = 40; // amber
		l.lid[0] = 90, l.lid[1] = 115, l.lid[2] = 70;
	} else if (genus == 3) {
		l.kind = 2;
		l.iris[0] = 150, l.iris[1] = 40, l.iris[2] = 35; // deep red
		l.lid[0] = 135, l.lid[1] = 115, l.lid[2] = 105;
	}
	if (cr && cr->getGenome()) {
		float pig[3] = {0, 0, 0};
		int n[3] = {0, 0, 0};
		for (auto& g : cr->getGenome()->genes)
			if (auto* p = dynamic_cast<creaturePigmentGene*>(g.get()); p && p->color < 3) {
				pig[p->color] += p->amount;
				n[p->color]++;
			}
		for (int i = 0; i < 3; i++)
			if (n[i]) {
				float f = std::clamp((pig[i] / n[i]) / 128.0f, 0.4f, 1.8f);
				l.iris[i] = std::clamp(l.iris[i] * f, 0.0f, 255.0f);
			}
	}
	return l;
}

struct EyeState {
	AgentRef who;
	EyeLook look;
	float pupil = 0.5f; // 0 narrow .. 1 wide, eased
	float open = 1.0f; // lids: 1 wide open .. 0 shut, eased
	unsigned int nextBlink = 0, blinkUntil = 0;
	Texture tex;
	Image img;
};
EyeState s_eye;

float noise2(int x, int y) {
	unsigned int h = (unsigned int)x * 374761393u + (unsigned int)y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

} // namespace

// The view, then the eye over it: whatever is not the pupil is iris,
// white of the eye or lid. Built as a picture each frame (the eye moves).
static void drawThroughEye(ImTextureID view, ImVec2 o, Agent* c, Creature* cr) {
	EyeState& e = s_eye;
	if (e.who != world.selectedcreature) {
		e.who = world.selectedcreature;
		e.look = lookOf(c, cr);
		e.pupil = 0.5f;
		e.open = 1.0f;
	}
	auto* oc = dynamic_cast<oldCreature*>(cr);
	auto drive = [&](int i) { return oc ? oc->getDrive(i) / 255.0f : 0.0f; };
	bool asleep = cr && cr->isAsleep();
	bool dead = cr && !cr->isAlive();
	float cx = c->x + c->getWidth() / 2.0f, cy = c->y + c->getHeight() / 2.0f;
	auto room = world.map ? world.map->roomAt(cx, cy) : nullptr;
	auto headRoom = world.map ? world.map->roomAt(cx, c->y + c->getHeight() * 0.2f) : nullptr;
	bool underwater = headRoom && headRoom->type == 2;
	float light = room ? std::clamp(room->lite / 255.0f, 0.0f, 1.0f) : 0.5f;
	float fear = drive(9), sleepy = std::max(drive(5), drive(6)), hurt = std::max(drive(0), drive(13));
	unsigned int now = world.tickcount;

	// the pupil: narrow in bright light, wide in the dark and with fear
	float wantPupil = std::clamp(1.0f - 0.8f * light + 0.5f * fear, 0.15f, 1.0f);
	e.pupil += (wantPupil - e.pupil) * 0.08f;
	// the lids: a blink every few seconds, drooping as it grows sleepy,
	// shut in sleep (and in death)
	if (now >= e.nextBlink) {
		e.blinkUntil = now + 2;
		e.nextBlink = now + 30 + (unsigned int)(noise2((int)now, 7) * 50);
	}
	float wantOpen = asleep || dead ? 0.0f : now < e.blinkUntil ? 0.05f : 1.0f - 0.5f * std::clamp((sleepy - 0.5f) / 0.5f, 0.0f, 1.0f);
	e.open += (wantOpen - e.open) * (now < e.blinkUntil ? 0.7f : 0.25f);

	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 o1(o.x + VIEW_W, o.y + VIEW_H);
	dl->PushClipRect(o, o1, true);
	// what it sees: rippling under water, doubled when hurt
	if (underwater) {
		const int strips = 30;
		float t = now * 0.25f;
		for (int i = 0; i < strips; i++) {
			float v0 = (float)i / strips, v1 = (float)(i + 1) / strips;
			float dx = 4.0f * std::sin(t + i * 0.6f);
			dl->AddImage(view, ImVec2(o.x + dx, o.y + v0 * VIEW_H), ImVec2(o1.x + dx, o.y + v1 * VIEW_H), ImVec2(0, v0), ImVec2(1, v1));
		}
		dl->AddRectFilled(o, o1, IM_COL32(20, 90, 110, 70));
	} else
		dl->AddImage(view, o, o1);
	if (hurt > 0.4f) {
		float k = std::clamp((hurt - 0.4f) / 0.6f, 0.0f, 1.0f);
		int off = 1 + (int)(4 * k);
		dl->AddImage(view, ImVec2(o.x + off, o.y), ImVec2(o1.x + off, o1.y), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(90 * k)));
		dl->AddImage(view, ImVec2(o.x - off, o.y + 1), ImVec2(o1.x - off, o1.y + 1), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(70 * k)));
		dl->AddRectFilled(o, o1, IM_COL32(170, 20, 10, (int)(80 * k)));
	}

	// the eye, as a picture
	const int W = VIEW_W, H = VIEW_H;
	if (!e.img.data) {
		e.img.width = W;
		e.img.height = H;
		e.img.format = if_rgba32;
		e.img.data = shared_array<uint8_t>(size_t(W) * H * 4);
	}
	const EyeLook& L = e.look;
	const float mx = W / 2.0f, my = H / 2.0f;
	// (owner, 2026-11-18: less eye, more view: the opening reaches past
	// the view's sides so the lids only take its corners, the pupil is
	// large, and the white, the iris and the lids' inner edge are a
	// checkered screen the world shows through, in C2's way)
	const float irisR = H * 0.55f;
	const float pupilR = irisR * (0.72f + 0.25f * e.pupil); // round pupils (at their smallest still most of the iris)
	const float slitW = irisR * (0.30f + 0.65f * e.pupil); // a grendel's slit, half width at its middle
	const float eyeW = W * 0.68f, eyeH = H * 0.82f * e.open;
	uint8_t* px = e.img.data.data();
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			float dx = x + 0.5f - mx, dy = y + 0.5f - my;
			float r = std::sqrt(dx * dx + dy * dy);
			float R = 0, G = 0, B = 0, A = 0;
			// the lids: an almond opening, shut as open falls to 0
			float across = std::clamp(dx / eyeW, -1.0f, 1.0f);
			float half = eyeH * std::sqrt(std::max(0.0f, 1.0f - across * across));
			float lidEdge = std::fabs(dy) - half;
			bool checker = ((x + y) & 1) == 0; // every other pixel, as C2 drew see-through
			if (lidEdge > 0 || std::fabs(dx) >= eyeW) {
				float shade = std::clamp(1.0f - lidEdge / 120.0f, 0.55f, 1.0f) * (0.92f + 0.08f * noise2(x / 2, y / 2));
				if (lidEdge < 4 && std::fabs(dx) < eyeW)
					shade *= 0.35f; // the lash line
				R = L.lid[0] * shade, G = L.lid[1] * shade, B = L.lid[2] * shade, A = 255;
				// the lids' inner edge: checkered, solid further out (and
				// solid when shut, so sleep is dark)
				if (e.open > 0.2f && lidEdge >= 4 && lidEdge < 40 && checker)
					A = 0;
			} else {
				bool inPupil;
				if (L.kind == 1) {
					float t = std::clamp(dy / irisR, -1.0f, 1.0f);
					inPupil = std::fabs(dx) < slitW * std::sqrt(std::max(0.0f, 1.0f - t * t)) && r < irisR;
				} else
					inPupil = r < pupilR;
				if (inPupil) {
					A = 0;
					// an ettin's coarse sight: a dotted fringe at the pupil's
					// edge, its middle clear (owner, 2026-11-18: still see
					// clearly what it is focused on)
					if (L.kind == 2) {
						float rim = r / pupilR;
						if (rim > 0.78f && ((x / 2 + y / 2) & 1) == 0 && noise2(x / 2, y / 2) < (rim - 0.78f) * 3.0f)
							R = L.iris[0] * 0.5f, G = L.iris[1] * 0.5f, B = L.iris[2] * 0.5f, A = 200;
					}
				} else if (r < irisR) {
					// the iris: fibres round the pupil, darker at its rim
					float ang = std::atan2(dy, dx);
					float fib = 0.75f + 0.25f * std::sin(ang * 46.0f + noise2((int)(ang * 30), 3) * 6.0f);
					float rim = std::clamp((r - irisR * 0.85f) / (irisR * 0.15f), 0.0f, 1.0f);
					float k = fib * (1.0f - 0.55f * rim);
					R = L.iris[0] * k, G = L.iris[1] * k, B = L.iris[2] * k, A = 255;
					// a screen over the iris: half clear, solid at its dark rim
					if (rim < 0.6f && checker)
						A = 0;
				} else {
					// the white, shaded toward the lids: half see-through
					float s = std::clamp(1.0f - (std::fabs(dy) / std::max(1.0f, half)) * 0.25f - (std::fabs(dx) / eyeW) * 0.2f, 0.6f, 1.0f);
					R = 245 * s, G = 238 * s, B = 230 * s, A = checker ? 0 : 255;
				}
			}
			// a glint, up and to the left of the pupil
			float gx = dx + irisR * 0.45f, gy = dy + irisR * 0.45f;
			float glint = std::exp(-(gx * gx + gy * gy) / 120.0f);
			if (lidEdge <= 0 && glint > 0.02f) {
				R = R + (255 - R) * glint, G = G + (255 - G) * glint, B = B + (255 - B) * glint;
				A = std::max(A, 255.0f * glint * 0.8f);
			}
			uint8_t* q = px + (size_t(y) * W + x) * 4;
			q[0] = (uint8_t)R, q[1] = (uint8_t)G, q[2] = (uint8_t)B, q[3] = (uint8_t)A;
		}
	if (!e.tex)
		e.tex = get_backend()->createTexture(W, H);
	get_backend()->updateTexture(e.tex, Rect2i{0, 0, W, H}, e.img);
	dl->AddImage(e.tex.as<ImTextureID>(), o, o1);
	dl->PopClipRect();
}

void DrawEyeView() {
	// developer aid: OPENC2E_OPEN_EYE_VIEW=1 selects the first norn and
	// opens this view (for headless snapshots); =2 a grendel, =3 an ettin
	static bool test_opened = false;
	if (!test_opened && devenv("OPENC2E_OPEN_EYE_VIEW")) {
		int want = std::clamp(atoi(devenv("OPENC2E_OPEN_EYE_VIEW")), 1, 3);
		for (auto& a : world.agents) {
			if (a && dynamic_cast<CreatureAgent*>(a.get()) && a->genus == want) {
				world.selectedcreature = a;
				s_eye_view_open = test_opened = true;
				break;
			}
		}
	}
	if (!s_eye_view_open)
		return;
	if (!world.selectedcreature) {
		s_eye_view_open = false;
		return;
	}
	if (!s_camera)
		s_camera = std::make_unique<EyeViewCamera>();
	if (!s_target)
		s_target = get_backend()->newRenderTarget(VIEW_W, VIEW_H);

	Agent* c = world.selectedcreature.get();
	Creature* cr = nullptr;
	if (auto* ca = dynamic_cast<CreatureAgent*>(c))
		cr = ca->getCreature();
	Agent* focus = cr ? cr->getAttentionFocus().get() : nullptr;
	if (focus == c || (focus && focus->isDying()))
		focus = nullptr;
	int decision = cr ? cr->getDecisionId() : -1;

	// where to look
	float crx = c->x + c->getWidth() / 2.0f, cry = c->y + c->getHeight() / 2.0f;
	s_room = world.map->metaRoomAt((unsigned int)std::max(0.0f, crx), (unsigned int)std::max(0.0f, cry));
	float tx = crx, ty = cry;
	if (focus) {
		// what it looks at, centred (owner, 2026-11-18: no brackets)
		tx = crx + wrapDelta(crx, focus->x + focus->getWidth() / 2.0f);
		ty = focus->y + focus->getHeight() / 2.0f;
	}
	// glide there (jump when a new creature is chosen or the target is far)
	float gdx = wrapDelta(s_cx, tx), gdy = ty - s_cy;
	if (s_followed != world.selectedcreature || std::fabs(gdx) > 2000 || std::fabs(gdy) > 2000) {
		s_followed = world.selectedcreature;
		s_cx = tx;
		s_cy = ty;
	} else {
		s_cx += gdx * 0.18f;
		s_cy += gdy * 0.18f;
	}
	s_camera->moveToGlobal((int)std::lround(s_cx) - VIEW_W / 2, (int)std::lround(s_cy) - VIEW_H / 2, jump);
	// (without the creature itself: the view is, roughly, what it sees, and
	// it does not see itself; owner, 2026-11-13)
	world.drawWorld(s_camera.get(), s_target.get(), false, nullptr, false, c);

	ImGui::SetNextWindowSize(ImVec2(VIEW_W + 16, VIEW_H + 62), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Creature's View", &s_eye_view_open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar)) {
		void* tex = s_target->getNativeTexture();
		if (tex) {
			ImVec2 o = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(VIEW_W, VIEW_H));
			drawThroughEye((ImTextureID)(uintptr_t)tex, o, c, cr);
		}
		if (cr && cr->isAsleep())
			ImGui::TextUnformatted("Asleep");
		else if (focus)
			ImGui::TextWrapped("%s %s", actionWords(decision), describe(focus).c_str());
		else
			ImGui::TextUnformatted("Not paying attention to anything");
	}
	ImGui::End();
}

} // namespace Openc2eImgui
