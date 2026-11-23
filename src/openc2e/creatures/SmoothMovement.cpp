/*
 *  SmoothMovement.cpp
 *  openc2e
 *
 *  C2 creatures drawn moving smoothly (owner, 2026-11-22: "auto-generating
 *  additional sprite angles to make movement/animation more fluid"; chose
 *  turned pictures of the three ways tried). A walk is four poses, one every
 *  other tick, and the whole creature jumps with each: each body part has
 *  four pictures a side. Between one pose and the next each part's angle is
 *  worked out from its joints (the .att files: its own joint to its end),
 *  the skeleton is rebuilt from the joints, the foot on the ground stays
 *  where it is, and each part is drawn as its nearest picture turned about
 *  its joint. A part going from its first angle to its fourth passes through
 *  the game's own second and third. A walk's poses carry the body forward
 *  unevenly (in one, 1, 22, 4 and 25 px), which drawn smoothly would be a
 *  lurch and a halt by turns, so the body is drawn part of the way to an
 *  even speed over the cycle (the feet slip a few px for it). The turned pictures are made from the
 *  player's own sprites as they are first needed, and kept a while.
 *
 *  Drawing only: what the creature is, does and touches is unchanged, and
 *  it is drawn up to two ticks (0.2 s) behind. Turning round, and poses that
 *  face the viewer or away (one picture each), change at once as before.
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

#include "SkeletalCreature.h"

#include "Engine.h"
#include "Map.h"
#include "MetaRoom.h"
#include "World.h"
#include "common/backend/Backend.h"
#include "common/devenv.h"
#include "common/creaturesImage.h"
#include "creatures/CreatureSpeech.h"
#include "creatures/oldCreature.h"
#include "fileformats/ImageUtils.h"
#include "imageManager.h"

#include <algorithm>
#include <cmath>
#include <fmt/core.h>
#include <map>
#include <tuple>

extern unsigned int cee_zorder[4][17];

namespace {

const int PARTS = 14;
// each part's parent and the parent's joint it hangs from (cee_bodyparts)
const int PARENT[PARTS] = {-1, 0, 0, 0, 0, 0, 0, 2, 3, 4, 5, 7, 8, 6};
const int JOINT[PARTS] = {-1, 0, 1, 2, 3, 4, 5, 1, 1, 1, 1, 1, 1, 1};

const float PI = 3.14159265f;
const float STEP = 5.0f * PI / 180.0f; // turned pictures are made 5 degrees apart
const float SHORT_BONE = 4.0f; // px: a part whose joint and end are nearer than this has no angle to speak of
const float MOST_TURN = 100.0f * PI / 180.0f; // between neighboring pictures; more is a joint file's oddity
const float EVEN = 0.7f; // how much of a walk's unevenness is taken out
const float EVEN_FADE = 2.0f; // ticks for a stopped walk's to go
const float MOST_JUMP = 72.0f; // px: the feet further off than this were moved, not walked (or pushed back by a wall, some 55)

int s_on = -1;

float wrapped(float a) {
	while (a > PI)
		a -= 2 * PI;
	while (a < -PI)
		a += 2 * PI;
	return a;
}

// ---- turned pictures ----
struct Turned {
	std::weak_ptr<creaturesImage> source;
	std::shared_ptr<creaturesImage> image;
	int radius = 0;
	unsigned int used = 0;
};
std::map<std::tuple<const creaturesImage*, unsigned int, int, int, int>, Turned> s_turned;
const size_t KEEP = 4000; // about 25 MB
unsigned int s_pruned = 0;

Turned& turned(const std::shared_ptr<creaturesImage>& image, unsigned int frame, int cx, int cy, int step) {
	unsigned int now = engine.get_ticks_msec();
	if (s_turned.size() > KEEP && now - s_pruned > 1000) {
		// those not drawn for half a minute, and those whose picture is gone
		s_pruned = now;
		for (auto it = s_turned.begin(); it != s_turned.end();)
			it = (now - it->second.used > 30000 || it->second.source.expired()) ? s_turned.erase(it) : std::next(it);
	}
	Turned& t = s_turned[std::make_tuple(image.get(), frame, cx, cy, step)];
	if (!t.image || t.source.lock() != image) {
		std::vector<Image> one;
		one.push_back(ImageUtils::Turn(image->getImageForFrame(frame), cx, cy, step * STEP, t.radius));
		t.image = imageManager::fromFrames(image->getName(), std::move(one));
		t.source = image;
	}
	t.used = now;
	return t;
}

} // namespace

bool SkeletalCreature::smoothMovement() {
	if (engine.version != 2)
		return false;
	if (s_on < 0)
		s_on = readPlayerSetting("smooth-movement") == "0" ? 0 : 1;
	return s_on == 1;
}

void SkeletalCreature::setSmoothMovement(bool on) {
	s_on = on ? 1 : 0;
	writePlayerSetting("smooth-movement", on ? "1" : "0");
}

float SkeletalCreature::glideShare() const {
	if (glide.length <= 0)
		return 1.0f;
	// (a change in tick n is first drawn in tick n + 1; in the middle of a
	// tick, as the next change begins, the last frame drawn was at its end)
	float now = (float)(world.tickcount - glide.changed) + (glide.pending ? 1.0f : engine.tickFraction());
	return std::clamp((now - glide.start) / glide.length, 0.0f, 1.0f);
}

// How far across it is drawn now to even out its walk.
float SkeletalCreature::evenNow() const {
	if (glide.evenFrom == 0 && glide.evenTo == 0)
		return 0;
	const float t = glideShare();
	if (t < 1.0f)
		return glide.evenFrom + (glide.evenTo - glide.evenFrom) * t;
	// (the walk has stopped: fading)
	const float now = (float)(world.tickcount - glide.changed) + (glide.pending ? 1.0f : engine.tickFraction());
	return glide.evenTo * std::clamp(1.0f - (now - glide.start - std::max(glide.length, 0.0f)) / EVEN_FADE, 0.0f, 1.0f);
}

// A step of a walk has just been taken (gaiti: its place in the cycle): how
// far to draw the body from where this pose puts it, for an even speed. Worked
// out from the cycle's poses and the joints when a walk or its way changes: a
// step keeps the foot that was down where it is, so the neck moves on by how
// much further ahead of that foot the next pose has it.
float SkeletalCreature::evenWalk() {
	if (!gaitgene)
		return 0;
	WalkCycle& w = walkCycle;
	if (w.gene != gaitgene || w.dir != posedirection) {
		w = WalkCycle();
		w.gene = gaitgene;
		w.dir = posedirection;
		float ahead[8][2];
		bool leftDown[8];
		for (; w.size < 8 && gaitgene->pose[w.size] != 0; w.size++) {
			auto g = posegenes.find(gaitgene->pose[w.size]);
			if (g == posegenes.end() || posedirection > 1)
				return 0;
			const std::string pose = g->second->getPoseString();
			// a part's picture in this pose (setPose; the pose string's places
			// for the body and the legs)
			auto picture = [&](int part, int place) {
				const char c = place < (int)pose.size() ? pose[place] : 'X';
				return c >= '0' && c <= '3' ? (int)(c - '0') + (int)posedirection * 4 : (int)this->pose[part];
			};
			auto& body = att[0].attachments[picture(0, 2)];
			float footy[2];
			for (int leg = 0; leg < 2; leg++) {
				// the hip, then along the thigh, shin and foot
				float fx = (float)(int)body[2 + leg * 2], fy = (float)(int)body[3 + leg * 2];
				const int parts[3] = {leg ? 3 : 2, leg ? 8 : 7, leg ? 12 : 11};
				for (int k = 0; k < 3; k++) {
					auto& a = att[parts[k]].attachments[picture(parts[k], (leg ? 6 : 3) + k)];
					fx += (float)(int)a[2] - (float)(int)a[0];
					fy += (float)(int)a[3] - (float)(int)a[1];
				}
				ahead[w.size][leg] = (float)(int)body[0] - fx;
				footy[leg] = fy;
			}
			leftDown[w.size] = footy[1] < footy[0];
		}
		float step[8], whole = 0;
		for (int i = 0; i < w.size; i++) {
			const int f = leftDown[i] ? 0 : 1;
			step[i] = ahead[(i + 1) % w.size][f] - ahead[i][f];
			whole += step[i];
		}
		// where each pose has it, and where an even walk would
		float at = 0, mean = 0;
		for (int i = 0; i < w.size; i++) {
			w.even[i] = whole * i / w.size - at;
			mean += w.even[i] / w.size;
			at += step[i];
		}
		for (int i = 0; i < w.size; i++)
			w.even[i] = std::clamp((w.even[i] - mean) * EVEN, -14.0f, 14.0f);
	}
	// (gaitTick counts gaiti on after the pose is set)
	return gaiti < (unsigned int)w.size && w.size > 1 ? w.even[gaiti] : 0;
}

// Where a part's joint `id` is from its own joint (0), at a pose between two
// of its pictures: turned and stretched from the one's to the other's.
void SkeletalCreature::attachmentBetween(int part, int id, float p, float& ox, float& oy) const {
	const int a = (int)std::floor(p), b = (int)std::ceil(p);
	const float t = p - a;
	auto at = [&](int q, float& vx, float& vy) {
		auto& j = att[part].attachments[q];
		vx = (float)(int)j[id * 2] - (float)(int)j[0];
		vy = (float)(int)j[id * 2 + 1] - (float)(int)j[1];
	};
	float ax, ay;
	at(a, ax, ay);
	ox = ax;
	oy = ay;
	if (a == b)
		return;
	float bx, by;
	at(b, bx, by);
	const float ra = std::hypot(ax, ay), rb = std::hypot(bx, by);
	const float from = std::atan2(ay, ax), by_ = wrapped(std::atan2(by, bx) - from);
	if (ra < 1.0f || rb < 1.0f || std::fabs(by_) > MOST_TURN) {
		ox = ax + (bx - ax) * t;
		oy = ay + (by - ay) * t;
		return;
	}
	const float r = ra + (rb - ra) * t, an = from + by_ * t;
	ox = r * std::cos(an);
	oy = r * std::sin(an);
}

// The picture nearest a pose between two, and how far to turn it.
float SkeletalCreature::turnBetween(int part, float p, int& frame) const {
	frame = (int)std::floor(p + 0.5f);
	const int a = (int)std::floor(p), b = (int)std::ceil(p);
	if (a == b)
		return 0;
	// the part's line: its own joint to its end; the body's, its hips to its neck
	auto line = [&](int q, float& angle) {
		auto& j = att[part].attachments[q];
		float x0 = (float)(int)j[0], y0 = (float)(int)j[1], x1 = (float)(int)j[2], y1 = (float)(int)j[3];
		if (part == 0) {
			x0 = ((float)(int)j[2] + (float)(int)j[4]) / 2;
			y0 = ((float)(int)j[3] + (float)(int)j[5]) / 2;
			x1 = (float)(int)j[0];
			y1 = (float)(int)j[1];
		}
		angle = std::atan2(y1 - y0, x1 - x0);
		return std::hypot(x1 - x0, y1 - y0) >= SHORT_BONE;
	};
	float aa, ab;
	if (!line(a, aa) || !line(b, ab))
		return 0;
	const float turn = wrapped(ab - aa);
	if (std::fabs(turn) > MOST_TURN)
		return 0;
	return turn * (p - (float)frame);
}

bool SkeletalCreature::drawnSkeleton(float poses[14], float jx[14], float jy[14], float& bx, float& by) const {
	const float t = glideShare();
	const float even = evenNow();
	if (t >= 1.0f && std::fabs(even) < 0.25f)
		return false;
	for (int i = 0; i < PARTS; i++) {
		const float a = glide.from[i];
		const unsigned int b = glide.to[i];
		// (pictures 0-3 face right, 4-7 left, 8 and 9 the viewer and away)
		const bool same = b < 8 && a <= 7.0f && (a >= 4.0f) == (b >= 4) && (a <= 3.0f) == (b <= 3);
		poses[i] = same ? a + ((float)b - a) * t : (float)b;
	}
	jx[0] = jy[0] = 0;
	for (int i = 1; i < PARTS; i++) {
		float ox = 0, oy = 0;
		if (JOINT[i] > 0 || PARENT[i] != 0)
			attachmentBetween(PARENT[i], JOINT[i], poses[PARENT[i]], ox, oy);
		jx[i] = jx[PARENT[i]] + ox;
		jy[i] = jy[PARENT[i]] + oy;
	}
	// the foot that stood on the ground stays there (or goes where the
	// change took it), and the rest stands on it
	float fx = glide.footx, fy = glide.footy;
	if (glide.settled) {
		fx = x + adjustx + partx[glide.foot] + (int)att[glide.foot].attachments[pose[glide.foot]][2] + glide.offx * (1 - t);
		fy = y + adjusty + party[glide.foot] + (int)att[glide.foot].attachments[pose[glide.foot]][3] + glide.offy * (1 - t);
	}
	float ox, oy;
	attachmentBetween(glide.foot, 1, poses[glide.foot], ox, oy);
	bx = fx - jx[glide.foot] - ox + even;
	by = fy - jy[glide.foot] - oy;
	return true;
}

void SkeletalCreature::glideBegin() {
	glide.pending = false;
	if (!smoothMovement() || !glide.have || !calculated || posedirection != glide.dir)
		return;
	bool same = true;
	for (int i = 0; i < PARTS; i++)
		same = same && pose[i] == glide.to[i];
	if (same)
		return;
	glide.pending = true;
	// from the way it is drawn now. (x, y, partx ... are still the last
	// pose's, pose[] is already the new one's: put back for the sums)
	unsigned int newpose[PARTS];
	std::copy(pose, pose + PARTS, newpose);
	std::copy(glide.to, glide.to + PARTS, pose);
	const int foot = downfoot_left ? 11 : 12;
	float p[PARTS], jx[PARTS], jy[PARTS], bx, by;
	float ox = 0, oy = 0;
	if (!glide.settled) // (a second change in one tick)
		ox = glide.originx - x, oy = glide.originy - y;
	else
		drawnShift(ox, oy);
	if (drawnSkeleton(p, jx, jy, bx, by)) {
		float ax, ay;
		attachmentBetween(foot, 1, p[foot], ax, ay);
		glide.footx = bx - evenNow() + jx[foot] + ax;
		glide.footy = by + jy[foot] + ay;
		std::copy(p, p + PARTS, glide.from);
	} else {
		glide.footx = x + attachmentX(foot, 1);
		glide.footy = y + attachmentY(foot, 1);
		for (int i = 0; i < PARTS; i++)
			glide.from[i] = (float)pose[i];
	}
	glide.evenFrom = evenNow();
	glide.originx = x + ox;
	glide.originy = y + oy;
	glide.foot = foot;
	std::copy(newpose, newpose + PARTS, pose);
}

void SkeletalCreature::glideEnd() {
	bool changed = !glide.have || posedirection != glide.dir;
	for (int i = 0; i < PARTS; i++)
		changed = changed || pose[i] != glide.to[i];
	std::copy(pose, pose + PARTS, glide.to);
	glide.dir = posedirection;
	glide.have = true;
	if (glide.pending) {
		// to be there as the walk's next step comes (walkSteps knows when);
		// any other pose, in two ticks
		glide.length = nextStepIn > 0 ? std::min(nextStepIn, 8.0f) : 2.0f;
		glide.start = 1.0f;
		glide.settled = false;
		glide.pending = false;
		glide.evenTo = nextStepIn > 0 ? evenWalk() : 0;
		glide.changed = world.tickcount;
	} else if (changed) {
		glide.length = 0;
		glide.settled = true;
		glide.evenFrom = glide.evenTo = 0;
		glide.changed = world.tickcount;
	}
}

void SkeletalCreature::glideSettle() {
	if (glide.settled)
		return;
	glide.settled = true;
	glide.offx = glide.footx - (x + attachmentX(glide.foot, 1));
	glide.offy = glide.footy - (y + attachmentY(glide.foot, 1));
	// (over the join of a world that goes round: the short way)
	float round = 0;
	if (std::fabs(glide.offx) > 1000.0f && world.map)
		if (MetaRoom* m = world.map->metaRoomAt((int)x, (int)y); m && m->wraparound())
			round = glide.offx > 0 ? -(float)m->width() : (float)m->width();
	glide.offx += round;
	if (carriedby || invehicle || std::fabs(glide.offx) > MOST_JUMP || std::fabs(glide.offy) > MOST_JUMP)
		glide.offx = glide.offy = 0;
	// (the corner, relative to where it is now)
	glide.originx += round - x;
	glide.originy -= y;
	if (carriedby || invehicle || std::fabs(glide.originx) > 1.5f * MOST_JUMP || std::fabs(glide.originy) > 1.5f * MOST_JUMP)
		glide.originx = glide.originy = 0;
}

bool SkeletalCreature::drawnShift(float& dx, float& dy) const {
	dx = dy = 0;
	const float t = glideShare();
	if (t >= 1.0f || !glide.settled)
		return false;
	dx = glide.originx * (1 - t);
	dy = glide.originy * (1 - t);
	return true;
}

bool SkeletalCreature::drawnHeadShift(float& dx, float& dy) const {
	float p[PARTS], jx[PARTS], jy[PARTS], bx, by;
	dx = dy = 0;
	if (!drawnSkeleton(p, jx, jy, bx, by))
		return false;
	auto& a = att[1].attachments[pose[1]];
	dx = bx + jx[1] - (x + adjustx + partx[1] + (int)a[0]);
	dy = by + jy[1] - (y + adjusty + party[1] + (int)a[1]);
	return true;
}

bool SkeletalCreature::drawnCarryShift(float& dx, float& dy) const {
	float p[PARTS], jx[PARTS], jy[PARTS], bx, by;
	dx = dy = 0;
	if (!drawnSkeleton(p, jx, jy, bx, by))
		return false;
	const int hand = posedirection == 0 ? 10 : 9; // (getCarryPoint)
	float ox, oy;
	attachmentBetween(hand, 1, p[hand], ox, oy);
	auto& a = att[hand].attachments[pose[hand]];
	dx = bx + jx[hand] + ox - (x + adjustx + partx[hand] + (int)a[2]);
	dy = by + jy[hand] + oy - (y + adjusty + party[hand] + (int)a[3]);
	return true;
}

bool SkeletalCreature::drawnNeck(float& nx, float& ny) const {
	if (!smoothMovement() || !glide.have)
		return false;
	float p[PARTS], jx[PARTS], jy[PARTS];
	if (!drawnSkeleton(p, jx, jy, nx, ny)) {
		auto& a = att[0].attachments[pose[0]];
		nx = x + adjustx + (int)a[0];
		ny = y + adjusty + (int)a[1];
	}
	return true;
}

// Draws it between two poses; false when it is not (or not gliding), to be
// drawn in its pose.
bool SkeletalCreature::renderGliding(RenderTarget* renderer, int xoffset, int yoffset) {
	glideSettle(); // (a pose a script set this tick)
	// developer aid: OPENC2E_WATCH_GLIDE prints each time a drawn creature's
	// neck jumps more than 8 px from one frame to the next (within 60 ms)
	if (static const bool watch = devenv("OPENC2E_WATCH_GLIDE") != nullptr; watch && engine.version == 2 && glide.have) {
		static std::map<int, std::tuple<float, float, unsigned int, unsigned int>> last;
		float p[PARTS], jx[PARTS], jy[PARTS], nx, ny;
		if (!smoothMovement() || !drawnSkeleton(p, jx, jy, nx, ny)) {
			nx = x + adjustx + (int)att[0].attachments[pose[0]][0];
			ny = y + adjusty + (int)att[0].attachments[pose[0]][1];
		}
		const unsigned int now = engine.get_ticks_msec();
		if (auto it = last.find(getUNID()); it != last.end()) {
			auto& [lx, ly, when, dir] = it->second;
			if (now > when && now - when <= 60 && std::hypot(nx - lx, ny - ly) > 8.0f)
				fmt::print("* WATCH GLIDE t{} unid {} jumps {:.0f},{:.0f} in {} ms{}{}{}{}{} share {:.2f} off {:.0f},{:.0f}\n", world.tickcount, getUNID(), nx - lx, ny - ly, now - when, walking ? " walking" : "",
					falling ? " falling" : "", carriedby ? " carried" : "", invehicle ? " riding" : "", dir != posedirection ? " turned" : "", glideShare(), glide.offx, glide.offy);
		}
		last[getUNID()] = std::make_tuple(nx, ny, now, posedirection);
	}
	if (!smoothMovement())
		return false;
	float p[PARTS], jx[PARTS], jy[PARTS], bx, by;
	if (!drawnSkeleton(p, jx, jy, bx, by))
		return false;
	// whole game pixels, the body's once and each joint's from it, so that
	// the parts do not shimmer against each other. (xoffset has the whole
	// part of x in it: CompoundPart::render.)
	const int ox = xoffset + (int)std::lround(bx - (float)(int)x), oy = yoffset + (int)std::lround(by - (float)(int)y);
	for (int j = 0; j < 17; j++) {
		const int i = (int)cee_zorder[posedirection][j];
		if (i >= PARTS)
			continue;
		int picture;
		const float turn = turnBetween(i, p[i], picture);
		auto& a = att[i].attachments[picture];
		const int cx = (int)a[0], cy = (int)a[1];
		const int sx = ox + (int)std::lround(jx[i]), sy = oy + (int)std::lround(jy[i]);
		unsigned int frame = (unsigned int)picture;
		if (i == 0)
			frame += pregnancy * 10;
		else if (i == 1)
			frame += ((eyesclosed || blinkticks > 0) ? 10 : 0) + facialexpression * 20;
		const int step = (int)std::lround(turn / STEP);
		if (step == 0) {
			renderer->renderCreaturesImage(images[i], frame, sx - cx, sy - cy);
			continue;
		}
		Turned& t = turned(images[i], frame, cx, cy, step);
		renderer->renderCreaturesImage(t.image, 0, sx - t.radius, sy - t.radius);
	}
	return true;
}

/* vim: set noet: */
