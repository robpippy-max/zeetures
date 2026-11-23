/*
 *  Camera.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Tue May 25 2004.
 *  Copyright (c) 2004 Alyssa Milburn. All rights reserved.
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

#include "Camera.h"

#include "Agent.h"
#include "CameraPart.h"
#include "Engine.h"

#include <cmath>
#include "Map.h"
#include "MetaRoom.h"
#include "World.h"
#include "creatures/SkeletalCreature.h"
#include "common/NumericCast.h"
#include "common/backend/Backend.h"

#include <cassert>

Camera::Camera() {
	metaroom = 0;
	panning = false;
	x = 0;
	y = 0;
}

MetaRoom* Camera::getMetaRoom() const {
	return world.map->getMetaRoom(metaroom);
}

void Camera::goToMetaRoom(unsigned int m) {
	metaroom = m;
	moveTo(getMetaRoom()->x(), getMetaRoom()->y());
}

void Camera::goToMetaRoom(unsigned int m, int _x, int _y, cameratransition transition) {
	metaroom = m;
	moveTo(_x, _y);
	// TODO: transition
	(void)transition;

	checkBounds();
}

void Camera::moveTo(int _x, int _y, panstyle pan) {
	// TODO: we don't break tracking according to trackingstyle.. atm updateTracking() calls this, also

	// smooth scrolling (the main camera): glide there over a moment, eased
	// in and out, stepped once per frame drawn (stepPan); "if visible" only
	// when the place is within a screen of the view. Any plain move (keys,
	// scroll bars, following a creature) ends a glide.
	bool near = std::abs(_x - x) <= (int)getWidth() && std::abs(_y - y) <= (int)getHeight();
	if (canPan() && (pan == smoothscroll || (pan == smoothscrollifvisible && near)) && (_x != x || _y != y)) {
		panFromX = x;
		panFromY = y;
		panToX = _x;
		panToY = _y;
		panStart = engine.get_ticks_msec();
		float dist = std::hypot((float)(_x - x), (float)(_y - y));
		panTime = 300 + (unsigned int)(std::min(dist, 3000.0f) / 10.0f); // 0.3-0.6 s
		panning = true;
		return;
	}
	panning = false;
	x = _x;
	y = _y;

	checkBounds();
}

void Camera::stepPan() {
	// following a creature that is drawn moving between ticks
	if (!panning && !world.paused && SkeletalCreature::smoothMovement() && dynamic_cast<SkeletalCreature*>(trackedagent.get()))
		updateTracking();
	if (!panning)
		return;
	float t = std::min(1.0f, (engine.get_ticks_msec() - panStart) / (float)std::max(panTime, 1u));
	float e = t * t * (3 - 2 * t); // eased in and out
	int nx = (int)std::lround(panFromX + (panToX - panFromX) * e);
	int ny = (int)std::lround(panFromY + (panToY - panFromY) * e);
	moveTo(nx, ny, jump); // (ends the glide)
	panning = t < 1.0f;
}

void Camera::moveToGlobal(int _x, int _y, panstyle pan) {
	MetaRoom* m = world.map->metaRoomAt(_x, _y);
	if (m) {
		if (m->id != metaroom)
			pan = jump; // inter-metaroom panning is always jump
		metaroom = m->id;
	}

	moveTo(_x, _y, pan);
}

void MainCamera::moveTo(int _x, int _y, panstyle pan) {
	// checkBounds clamps a move past the world's top or bottom (or wraps one
	// past a looping world's side) by calling moveTo again from inside this
	// one: only the outer call moves the floated agents, by the whole move.
	// (The inner call used to move them back by the overshoot too, so the
	// hand crept across the window while the view scrolled against the top
	// or bottom: owner, 2026-11-15.)
	static int depth = 0;
	if (depth > 0) {
		Camera::moveTo(_x, _y, pan);
		return;
	}
	int oldx = x, oldy = y;
	depth++;
	Camera::moveTo(_x, _y, pan);
	depth--;
	int xoffset = x - oldx; // (none yet when a glide starts)
	int yoffset = y - oldy;

	for (auto& i : floated) {
		assert(i);
		i->moveTo(i->x + xoffset, i->y + yoffset);
	}
}

void MainCamera::addFloated(AgentRef a) {
	assert(a);
	floated.push_back(a);
}

void MainCamera::delFloated(AgentRef a) {
	assert(a);
	std::vector<AgentRef>::iterator i = std::find(floated.begin(), floated.end(), a);
	if (i == floated.end())
		return;
	floated.erase(i);
}

void Camera::trackAgent(AgentRef a, int xp, int yp, trackstyle s, cameratransition transition) {
	// TODO: xp and yp
	(void)xp;
	(void)yp;
	// TODO: transition
	(void)transition;
	trackedagent = a;
	trackingstyle = s;
	updateTracking();
}

void Camera::checkBounds() {
	MetaRoom* m = getMetaRoom();
	if (!m)
		return;

	if (m->wraparound()) {
		// handle wrapping around, if necessary
		if (x < (int)m->x()) {
			moveTo(x + m->width(), y);
		} else if (x > (int)m->x() + (int)m->width()) {
			moveTo(x - m->width(), y);
		}
	} else {
		// refuse to move beyond the boundaries
		if (x < (int)m->x()) {
			moveTo(m->x(), y);
		} else if (x + getWidth() > m->x() + m->width()) {
			moveTo(m->x() + m->width() - getWidth(), y);
		}
	}

	// refuse to move beyond the boundaries
	if (y < (int)m->y()) {
		moveTo(x, m->y());
	} else if (y + getHeight() > m->y() + m->height()) {
		moveTo(x, m->y() + m->height() - getHeight());
	}
}

void Camera::tick() {
	updateTracking();
}

void Camera::updateTracking() {
	if (!trackedagent)
		return;

	// TODO: not very intelligent :) also, are int casts correct?
	int trackx = (int)trackedagent->x + ((int)trackedagent->getWidth() / 2) - (int)(getWidth() / 2);
	int tracky = (int)trackedagent->y + ((int)trackedagent->getHeight() / 2) - (int)(getHeight() / 2);
	// a creature drawn moving smoothly: its neck as drawn (its corner, which
	// the above goes by, shifts about with each pose)
	float nx, ny;
	if (auto* sc = dynamic_cast<SkeletalCreature*>(trackedagent.get()); sc && sc->drawnNeck(nx, ny)) {
		trackx = (int)std::lround(nx) - (int)(getWidth() / 2);
		tracky = (int)std::lround(ny) + 20 - (int)(getHeight() / 2);
	}
	moveToGlobal(trackx, tracky);
}

unsigned int MainCamera::getWidth() const {
	auto target_width = numeric_cast<unsigned int>(std::ceil(get_backend()->getMainRenderTarget()->getWidth() / engine.zoom));
	if ((!getMetaRoom()) || (target_width < getMetaRoom()->width()))
		return target_width;
	else
		return getMetaRoom()->width();
}

unsigned int MainCamera::getHeight() const {
	auto target_height = numeric_cast<unsigned int>(std::ceil(get_backend()->getMainRenderTarget()->getHeight() / engine.zoom));
	if ((!getMetaRoom()) || (target_height < getMetaRoom()->height()))
		return target_height;
	else
		return getMetaRoom()->height();
}

unsigned int PartCamera::getWidth() const {
	// TODO: update from ZOOM values
	return part->cameraWidth();
}

unsigned int PartCamera::getHeight() const {
	// TODO: update from ZOOM values
	return part->cameraHeight();
}

/* vim: set noet: */
