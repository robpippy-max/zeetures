/*
 *  Bubble.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Sat Apr 26 2008.
 *  Copyright (c) 2008 Alyssa Milburn. All rights reserved.
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

#include "Bubble.h"

#include "Additions.h"
#include "creatures/CreatureSpeech.h"

#include "Camera.h"
#include "Engine.h"
#include "SpritePart.h"
#include "World.h"
#include "historyManager.h"
#include "common/backend/Backend.h"
#include "common/backend/Keycodes.h"
#include "common/creaturesImage.h"
#include "imageManager.h"
#include "PathResolver.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>

namespace Openc2eImgui {
std::string KitDateText(unsigned int worldtick); // (the kits: "Summer, day 3, year 001")
}

Bubble::Bubble(unsigned char family, unsigned char genus, unsigned short species, unsigned int plane,
	std::string spritefile, unsigned int firstimage, unsigned int imagecount,
	unsigned int tx, unsigned int ty, unsigned int twidth, unsigned int theight,
	unsigned int tcolour, unsigned int bgcolour)
	: CompoundAgent(family, genus, species, plane, spritefile, firstimage, imagecount) {
	ourPart = new BubblePart(this, 1, tx, ty, tcolour, bgcolour);
	addPart(ourPart);
	ourPart->textwidth = twidth;
	ourPart->textheight = theight;

	ourPart->editable = false;
}

void Bubble::setText(std::string s) {
	ourPart->setText(s);
}

std::string Bubble::getText() {
	return ourPart->text;
}

void Bubble::setEditing(bool e) {
	ourPart->editable = e;
	if (e)
		world.setFocus(ourPart); // gain focus
	else if (world.focusagent == AgentRef(this) && world.focuspart == ourPart->id)
		world.setFocus(0); // lose focus
}

void Bubble::setTimeout(unsigned int t) {
	timeout = t;
}

bool Bubble::hiddenRider(Agent* a) {
	return a && a->invehicle && a->invehicle->invisible();
}

void Bubble::tick() {
	CompoundAgent::tick();

	// a speaker carried off out of sight (an airlock's hidden car; owner,
	// 2026-11-12: its bubble floated over the rock) takes its bubble with it
	if (floatingagent && hiddenRider(floatingagent.get())) {
		kill();
		return;
	}

	if (!paused && timeout) {
		timeout--;
		if (timeout == 0)
			kill();
	}
}

void Bubble::turnIntoSpeech() {
	newBubble((Agent*)world.hand(), true, getText());
	// creatures in earshot of the hand hear the words in it (CreatureSpeech)
	hearSpeech((Agent*)world.hand(), getText(), HEAR_HAND);
	recordSpeech((Agent*)world.hand(), getText());
	answerHand(getText());

	// TODO: announce via shou
	// TODO: add to speech history

	kill();
}

Bubble* Bubble::newBubble(Agent* parent, bool speech, std::string text) {
	assert(engine.version < 3);

	assert(parent);

	bool leftside = false;
	// TODO: cope with wrap
	if (parent->x - engine.camera->getX() < engine.camera->getWidth() / 2)
		leftside = true;

	int pose;
	if (engine.version == 1) {
		// C1 poses
		if (speech)
			pose = leftside ? 10 : 9;
		else
			pose = leftside ? 12 : 11;
	} else {
		// C2 poses
		// pose adjusted on setText() if necessary
		if (speech)
			pose = leftside ? 21 : 18;
		else
			pose = leftside ? 27 : 24;
	}

	int plane;
	if (engine.version == 1) {
		plane = 9000;
	} else {
		// C2
		if (parent == (Agent*)world.hand()) {
			if (speech)
				plane = 9996;
			else
				plane = 9999;
		} else
			plane = 9995;
	}

	int xoffset = (engine.version == 1) ? 6 : 8;
	int yoffset = (engine.version == 1) ? 3 : 8;
	int twidth = (engine.version == 1) ? 144 : 95; // extended to fit text upon setText()

	unsigned int textcolor = (engine.version == 1) ? 0xb : 0x080810; // TODO
	unsigned int bgcolor = (engine.version == 1) ? 0x1 : 0xC0FFFF; // TODO

	Bubble* ourBubble = new Bubble(2, 1, 2, plane, "syst", pose, engine.version == 1 ? 1 : 3, xoffset, yoffset, twidth, 12, textcolor, bgcolor);
	ourBubble->finishInit();
	ourBubble->leftside = leftside;

	ourBubble->attr = 32; // floating
	ourBubble->floatTo(parent);

	// TODO: fix positioning
	if (leftside)
		ourBubble->moveTo(parent->x + parent->getWidth() - 2, parent->y - ourBubble->getHeight());
	else
		ourBubble->moveTo(parent->x - ourBubble->getWidth() + 2, parent->y - ourBubble->getHeight());

	ourBubble->setText(text);

	if (speech)
		ourBubble->setTimeout(2 * 10); // TODO: 2s is probably not right
	else
		ourBubble->setEditing(true);

	return ourBubble;
}

BubblePart::BubblePart(Bubble* p, unsigned int _id, int x, int y, unsigned int tcolour, unsigned int bgcolour)
	: CompoundPart(p, _id, x, y, 0) {
	editable = false;
	textwidth = 0;
	textheight = 0;
	textoffset = 0; // doesn't matter when text is empty
	textcolour = tcolour;
	backgroundcolour = bgcolour;

	if (engine.version == 1) {
		charsetsprite = world.gallery->getCharsetDta(if_index8, bgcolour, tcolour, tcolour);
	} else {
		charsetsprite = world.gallery->getCharsetDta(if_bgr24, bgcolour, tcolour, tcolour);
	}
}

void BubblePart::partRender(RenderTarget* renderer, int xoffset, int yoffset) {
	unsigned int charpos = 0;
	for (unsigned char c : text) {
		assert(c < charsetsprite->numframes()); // handled in setText
		renderer->renderCreaturesImage(charsetsprite, c, xoffset + x + textoffset + charpos, yoffset + y);
		charpos += charsetsprite->width(c) + 1;
	}
}

void BubblePart::gainFocus() {
	assert(editable);
}

void BubblePart::loseFocus() {
	parent->kill();
}

void BubblePart::handleTranslatedChar(unsigned char c) {
	setText(text + (char)c);
}

void BubblePart::handleRawKey(uint8_t key) {
	switch (key) {
		case OPENC2E_KEY_BACKSPACE:
			if (text.size() == 0) {
				loseFocus();
				return;
			}
			{
				std::string s = text;
				s.erase(s.begin() + (s.size() - 1));
				setText(s);
			}
			if (text.size() == 0) {
				loseFocus();
				return;
			}
			break;

		case OPENC2E_KEY_RETURN:
			((Bubble*)parent)->turnIntoSpeech(); // TODO: omg hax
			break;
	}
}

unsigned int BubblePart::poseForWidth(unsigned int width) {
	if (engine.version == 1)
		return 0;
	SpritePart* s;
	if (!(s = dynamic_cast<SpritePart*>(parent->part(0))))
		return 0;
	unsigned int sprite = s->getFirstImg();
	while (sprite < s->getFirstImg() + 2 && width > s->getSprite()->width(sprite) - 17)
		sprite++;
	return sprite - s->getFirstImg();
}

void BubblePart::setText(std::string str) {
	unsigned int twidth = 0;
	std::string newtext;
	for (unsigned char c : str) {
		if (c >= charsetsprite->numframes()) {
			// skip accented characters when we only have CHARSET.DTA
			// TODO: convert to non-accented characters (CP1252->ASCII)
			continue;
		}
		twidth += charsetsprite->width(c) + 1;
		newtext.push_back(c);
	}
	if (twidth > 0) {
		twidth -= 1;
	}

	if (engine.version == 2) {
		unsigned int pose = poseForWidth(twidth);
		SpritePart* s;
		if ((s = dynamic_cast<SpritePart*>(parent->part(0)))) {
			if (pose != s->getPose()) {
				s->setPose(pose);
				textwidth = s->getWidth() - 17;
				Bubble* p = (Bubble*)parent; // TODO: omg hax
				if (!p->leftside)
					p->moveTo(p->floatingagent->x - p->getWidth() + 2, p->floatingagent->y - p->getHeight());
			}
		}
	}
	if (twidth > textwidth)
		return;

	text = newtext;
	textoffset = (textwidth - twidth) / 2;
}

/* vim: set noet: */

AnnouncementBubble::AnnouncementBubble(std::string spritefile, unsigned int firstimage, unsigned int imagecount, int sid)
	: CompoundAgent(spritefile, firstimage, imagecount), stringid(sid) {
}

std::string AnnouncementBubble::announcementText(int stringid, int index) {
	// "<number> <text>" lines; later files (the world's own Additions) win
	static std::map<int, std::string> texts;
	static std::string loadedFor = "\x01";
	std::string key = engine.currentworlddir.string();
	if (loadedFor != key) {
		loadedFor = key;
		texts.clear();
		// the player's own (or the built-in) one, then the world folder's
		std::vector<std::string> files{Additions::text("announcements.txt")};
		if (!engine.currentworlddir.empty()) {
			std::ifstream w(engine.currentworlddir / "Additions" / "announcements.txt", std::ios::binary);
			std::ostringstream ws;
			ws << w.rdbuf();
			files.push_back(ws.str());
		}
		for (auto& fileText : files) {
			std::istringstream in(fileText);
			std::string line;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (line.empty() || line[0] == '*' || line[0] == '#')
					continue;
				size_t sp = line.find(' ');
				if (sp == std::string::npos)
					continue;
				int n = atoi(line.substr(0, sp).c_str());
				texts[n] = line.substr(sp + 1);
			}
		}
	}
	(void)index; // C2's windows show their first string (BBTX 0 0)
	auto it = texts.find(stringid);
	std::string text = it == texts.end() ? std::string() : it->second;
	// the memorial stone (memorial.cos): the latest group that grew old
	// together in this world, after its own story ("{names}" and "{date}" in
	// the file's line 41)
	if (stringid == 40 && world.history) {
		unsigned int tick = 0;
		std::string names = world.history->latestTogether(&tick);
		auto later = texts.find(41);
		if (!names.empty() && later != texts.end()) {
			std::string add = later->second;
			for (auto [key, value] : {std::pair<std::string, std::string>{"{names}", names}, {"{date}", Openc2eImgui::KitDateText(tick)}})
				for (size_t at; (at = add.find(key)) != std::string::npos;)
					add.replace(at, key.size(), value);
			text += "||" + add;
		}
	}
	return text;
}

AnnouncementTextPart::AnnouncementTextPart(Agent* p, unsigned int _id, int _x, int _y, int _z, unsigned int w)
	: CompoundPart(p, _id, _x, _y, _z), width(w), colour(0xB1F3E7) {
	charset = world.gallery->getCharsetDta(if_bgr24, 0, colour, colour);
	clearCharsetBackground();
}

// the letters' background (palette 0) see-through, not black: the game's
// own windows have a black text area, so it never showed, but our light
// panels (the memorial's tablet) do
void AnnouncementTextPart::clearCharsetBackground() {
	if (!charset)
		return;
	for (auto& im : charset->images)
		if (im.palette)
			im.palette[0].a = 0;
}

void AnnouncementTextPart::setColour(uint32_t c) {
	if (c == colour)
		return;
	colour = c;
	charset = world.gallery->getCharsetDta(if_bgr24, 0, colour, colour);
	clearCharsetBackground();
}

void AnnouncementTextPart::setText(const std::string& s) {
	text = s;
	layout();
}

// word wrap to the part's width; "|" breaks a line
void AnnouncementTextPart::layout() {
	lines.clear();
	height = 0;
	if (!charset)
		return;
	auto widthOf = [&](const std::string& w) {
		unsigned int n = 0;
		for (unsigned char c : w)
			if (c < charset->numframes())
				n += charset->width(c) + 1;
		return n;
	};
	std::string line;
	std::string word;
	auto flushWord = [&]() {
		if (word.empty())
			return;
		std::string tryLine = line.empty() ? word : line + " " + word;
		if (!line.empty() && widthOf(tryLine) > width) {
			lines.push_back(line);
			line = word;
		} else {
			line = tryLine;
		}
		word.clear();
	};
	for (char c : text) {
		if (c == ' ') {
			flushWord();
		} else if (c == '|') {
			flushWord();
			lines.push_back(line);
			line.clear();
		} else {
			word += c;
		}
	}
	flushWord();
	if (!line.empty())
		lines.push_back(line);
	unsigned int lh = charset->numframes() > 'A' ? charset->height('A') + 2 : 12;
	height = lh * lines.size();
}

void AnnouncementTextPart::partRender(RenderTarget* renderer, int xoffset, int yoffset) {
	if (!charset)
		return;
	unsigned int lh = charset->numframes() > 'A' ? charset->height('A') + 2 : 12;
	for (size_t i = 0; i < lines.size(); i++) {
		unsigned int charpos = 0;
		for (unsigned char c : lines[i]) {
			if (c >= charset->numframes())
				continue;
			renderer->renderCreaturesImage(charset, c, xoffset + x + charpos, yoffset + y + (int)(i * lh));
			charpos += charset->width(c) + 1;
		}
	}
}
