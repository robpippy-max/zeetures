/*
 *  LlmMind.cpp
 *  openc2e (Zeetures fork: robpippy-max/zeetures, branch llm-mind)
 *
 *  The "llm" mind. Select it with --mind llm.
 *
 *  It wraps Zeetures's homeostat mind, which keeps running every tick and
 *  stays in charge of the body. On top of that, each creature now and then
 *  sends what it perceives to a sidecar program on this machine (the
 *  creatures-ai "mind bridge", which asks a local language model through
 *  Ollama), and shows what comes back as its own speech and thought
 *  bubbles. The game never waits for the sidecar: a background thread owns
 *  the connection, and with no sidecar running the creature is simply the
 *  homeostat. See docs/architecture.md in the creatures-ai repo.
 *
 *  When a creature asks: on a timer (awake every $ZEETURES_MIND_THINK_EVERY
 *  mind ticks, default 20, about eight seconds; asleep every
 *  $ZEETURES_MIND_SLEEP_EVERY, default 150), and sooner when something
 *  happens: the hand speaks to it, pats or slaps it, or comes near; a
 *  notable stimulus; someone new in view. The hand's whole sentence goes to
 *  the sidecar ("handSaid"), so a creature can understand more than the
 *  words it knows.
 *  One question at a time per creature.
 *
 *  Connection: 127.0.0.1, port $ZEETURES_MIND_PORT (default 47474), lines
 *  of text. Game to sidecar: one JSON object per line ("hello" with the
 *  game's word list on connecting; then "think" and "died"). Sidecar to
 *  game: "R<TAB>moniker<TAB>say meaning<TAB>thought,symbols<TAB>key<TAB>plan".
 *  The plan (2026-11-23, optional) is up to three wishes separated by ";",
 *  each "kind:target:decision:strength" (kind cat, who, act, avoid-cat or
 *  avoid-who; strength 0-100). For $ZEETURES_MIND_WISH_TICKS mind ticks
 *  (default 75, about 30 s), or until the next plan, they pull the
 *  homeostat's choices toward (or away from) what they name; survival
 *  needs always win (HomeostatMind::applyWishes).
 *  $ZEETURES_MIND_BRIDGE=0 turns the bridge off.
 *
 *  Diary: what each creature perceives and chooses is also written to a
 *  JSON-lines log, one file per creature, in $ZEETURES_MIND_LOG_DIR, else
 *  ~/Documents/Creatures/logs/mind: every $ZEETURES_MIND_LOG_EVERY mind
 *  ticks (default 10), and on any tick with a notable stimulus, a new
 *  decision, speech or thought. Shouts and signs heard (STIM SHOU / STIM
 *  SIGN, which arrive almost every tick) are only counted ("heard").
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
#include "CreatureSpeech.h"
#include "Engine.h"
#include "World.h"
#include "historyManager.h"

#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

std::unique_ptr<CreatureMind> createHomeostatMind(); // HomeostatMind.cpp

namespace {

const char* const LLM_DRIVE_NAMES[17] = {
	"pain", "pleasure", "hunger", "cold", "hot", "tired", "sleepy", "lonely",
	"crowded", "fear", "bored", "anger", "sex", "injury", "suffocate", "thirst", "stress"};

const int HAND_CATEGORY = 1; // C2: the hand (2 1 1)
const float HAND_NEAR = 150; // world pixels

std::string jsonString(const std::string& s) {
	std::string o = "\"";
	for (unsigned char c : s) {
		switch (c) {
			case '"': o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\n': o += "\\n"; break;
			case '\r': o += "\\r"; break;
			case '\t': o += "\\t"; break;
			default:
				if (c < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof buf, "\\u%04x", c);
					o += buf;
				} else
					o += (char)c;
		}
	}
	return o + "\"";
}

std::string num(float f) {
	char buf[32];
	std::snprintf(buf, sizeof buf, "%.3g", f);
	return buf;
}

std::string trimmed(std::string s) {
	while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n'))
		s.pop_back();
	while (!s.empty() && s.front() == ' ')
		s.erase(s.begin());
	return s;
}

unsigned int envCount(const char* name, unsigned int fallback) {
	if (const char* e = std::getenv(name)) {
		int n = std::atoi(e);
		if (n > 0)
			return (unsigned int)n;
	}
	return fallback;
}

// creature kinds as perceptions number them (0-based in practice; checked in
// play 2026-11-22: 1YRK a Grendel logs 1, 2LRD an Ettin logs 2)
const char* kindName(int genus) {
	switch (genus) {
		case 0: return "norn";
		case 1: return "grendel";
		case 2: return "ettin";
		case 3: return "shee";
		default: return "";
	}
}

// the game's own word for a kind of thing (its word list: nouns from 16)
std::string thingName(int category) {
	if (category == MIND_HEALING_FRUIT)
		return "healing fruit";
	if (category == MIND_ROTTEN_FRUIT)
		return "rotten fruit";
	size_t w = 16 + (size_t)category;
	std::string s = category >= 0 && w < engine.wordlist.size() ? trimmed(engine.wordlist[w]) : "";
	for (char& c : s)
		c = (char)tolower((unsigned char)c);
	return s.empty() ? "kind " + std::to_string(category) : s;
}

// action labels (as HomeostatMind's log uses; 10/26 go west, 11/27 east)
std::string actionLabel(int decision) {
	switch (decision) {
		case 0: return "idle";
		case 1: return "push";
		case 2: return "pull";
		case 3: return "stop";
		case 4: return "approach";
		case 6: return "pick up";
		case 7: return "drop";
		case 8: return "express";
		case 10: return "travel west";
		case 11: return "travel east";
		case 12: return "eat";
		case 13: return "hit";
		case 25: return "sleep";
		case 26: return "walk west";
		case 27: return "walk east";
		default: return "action " + std::to_string(decision);
	}
}

// the name the player gave a creature (Zeetures's records), or empty
std::string givenName(const std::string& moniker) {
	if (moniker.empty() || !world.history || !world.history->hasMoniker(moniker))
		return {};
	return world.history->getMoniker(moniker).name;
}

// the hand's latest line, if said within `within` world ticks: its text and tick
bool handLine(unsigned int within, std::string& text, unsigned int& tick) {
	Agent* hand = (Agent*)world.hand();
	if (!hand)
		return false;
	for (const SpokenLine& l : speechHistory()) {
		if (l.speaker.lock().get() != hand)
			continue;
		if (world.worldtickcount >= l.tick && world.worldtickcount - l.tick < within) {
			text = l.text;
			tick = l.tick;
			return true;
		}
		return false; // only the newest of the hand's lines
	}
	return false;
}

bool background(const StimulusEvent& s) {
	return s.command == "STIM SHOU" || s.command == "STIM SIGN";
}

std::filesystem::path logDir() {
	if (const char* d = std::getenv("ZEETURES_MIND_LOG_DIR"))
		return d;
	const char* home = std::getenv("HOME");
	return std::filesystem::path(home ? home : ".") / "Documents" / "Creatures" / "logs" / "mind";
}

// ---- the bridge to the sidecar ----------------------------------------------

struct Reply {
	std::string say; // a speech meaning (CreatureSpeech.h), or empty
	std::vector<std::string> thought; // thought-bubble symbols, or empty
	std::string key;
	// its plan (2026-11-23): what it means to do next, as wishes for the
	// homeostat; hasPlan false when the sidecar sent no plan field at all
	bool hasPlan = false;
	std::vector<MindWish> plan;
};


std::vector<std::string> split(const std::string& s, char sep) {
	std::vector<std::string> out;
	size_t start = 0;
	for (;;) {
		size_t c = s.find(sep, start);
		out.push_back(s.substr(start, c == std::string::npos ? std::string::npos : c - start));
		if (c == std::string::npos)
			break;
		start = c + 1;
	}
	return out;
}

// One wish from the plan field: "kind:target:decision:strength", kind one of
// cat, who, act (no object), avoid-cat, avoid-who; strength 0-100.
bool parseWish(const std::string& s, MindWish& w) {
	auto f = split(s, ':');
	if (f.size() != 4)
		return false;
	try {
		const std::string& kind = f[0];
		w.avoid = kind.rfind("avoid-", 0) == 0;
		std::string base = w.avoid ? kind.substr(6) : kind;
		if (base == "cat")
			w.category = std::stoi(f[1]);
		else if (base == "who")
			w.who = f[1];
		else if (base != "act")
			return false;
		w.decision = f[2].empty() ? -1 : std::stoi(f[2]);
		w.strength = std::clamp(std::stoi(f[3]) / 100.0f, 0.0f, 1.0f);
	} catch (...) {
		return false;
	}
	if (w.who.empty() && w.category < 0 && w.decision < 0)
		return false; // a wish for nothing in particular
	return true;
}

// One connection for all creatures, owned by a background thread so that the
// game never waits on the network. Lines to send are queued; replies are kept
// by moniker until that creature's next tick picks them up.
class Bridge {
  public:
	static Bridge& get() {
		static Bridge b;
		return b;
	}
	bool enabled() const { return on; }
	bool connected() const { return up; }
	void setHello(const std::string& line) {
		std::lock_guard<std::mutex> l(m);
		if (hello.empty()) {
			hello = line;
			if (up)
				outq.push_back(line);
		}
	}
	bool hasHello() {
		std::lock_guard<std::mutex> l(m);
		return !hello.empty();
	}
	void send(const std::string& line) {
		if (!up)
			return; // nothing waits for a sidecar that isn't there
		std::lock_guard<std::mutex> l(m);
		if (outq.size() > 256)
			outq.pop_front();
		outq.push_back(line);
	}
	bool take(const std::string& who, Reply& r) {
		std::lock_guard<std::mutex> l(m);
		auto it = replies.find(who);
		if (it == replies.end())
			return false;
		r = it->second;
		replies.erase(it);
		return true;
	}

  private:
	bool on = true;
	int port = 47474;
	std::atomic<bool> stop{false}, up{false};
	std::mutex m;
	std::string hello;
	std::deque<std::string> outq;
	std::map<std::string, Reply> replies;
	std::thread th;

	Bridge() {
		if (const char* e = std::getenv("ZEETURES_MIND_BRIDGE"))
			on = std::string(e) != "0";
		port = (int)envCount("ZEETURES_MIND_PORT", 47474);
#ifdef _WIN32
		on = false; // the bridge is written for macOS and Linux
#endif
		if (on) {
			fmt::print("* llm mind: looking for the mind bridge on 127.0.0.1:{}\n", port);
			th = std::thread([this] { run(); });
		}
	}
	~Bridge() {
		stop = true;
		if (th.joinable())
			th.join();
	}

	void pause(int ms) {
		for (int i = 0; i < ms / 100 && !stop; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	void received(const std::string& line) {
		auto f = split(line, '\t');
		if (f.size() < 2 || f[0] != "R" || f[1].empty())
			return;
		Reply r;
		if (f.size() > 2)
			r.say = f[2];
		if (f.size() > 3 && !f[3].empty())
			r.thought = split(f[3], ',');
		if (f.size() > 4)
			r.key = f[4];
		if (f.size() > 5) {
			r.hasPlan = true;
			for (const auto& part : split(f[5], ';')) {
				MindWish w;
				if (!part.empty() && parseWish(part, w) && r.plan.size() < 3)
					r.plan.push_back(w);
			}
		}
		std::lock_guard<std::mutex> l(m);
		replies[f[1]] = r;
	}

	void run() {
#ifndef _WIN32
		bool told = false;
		while (!stop) {
			int fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0) {
				pause(2000);
				continue;
			}
#ifdef SO_NOSIGPIPE
			int one = 1;
			setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
			sockaddr_in a{};
			a.sin_family = AF_INET;
			a.sin_port = htons((uint16_t)port);
			inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
			if (connect(fd, (sockaddr*)&a, sizeof a) != 0) {
				close(fd);
				pause(2000);
				continue;
			}
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
			fmt::print("* llm mind: connected to the mind bridge\n");
			told = false;
			std::string wbuf, rbuf;
			{
				std::lock_guard<std::mutex> l(m);
				outq.clear();
				if (!hello.empty())
					wbuf = hello;
			}
			up = true;
			while (!stop) {
				{
					std::lock_guard<std::mutex> l(m);
					while (!outq.empty()) {
						wbuf += outq.front();
						outq.pop_front();
					}
				}
				pollfd pf{fd, (short)(POLLIN | (wbuf.empty() ? 0 : POLLOUT)), 0};
				int r = poll(&pf, 1, 100);
				if (r < 0)
					break;
				if (pf.revents & (POLLERR | POLLHUP | POLLNVAL))
					break;
				if (pf.revents & POLLIN) {
					char buf[8192];
					ssize_t n = recv(fd, buf, sizeof buf, 0);
					if (n <= 0)
						break;
					rbuf.append(buf, (size_t)n);
					size_t nl;
					while ((nl = rbuf.find('\n')) != std::string::npos) {
						received(rbuf.substr(0, nl));
						rbuf.erase(0, nl + 1);
					}
					if (rbuf.size() > 1 << 20)
						rbuf.clear();
				}
				if (!wbuf.empty() && (pf.revents & POLLOUT)) {
#ifdef MSG_NOSIGNAL
					ssize_t n = ::send(fd, wbuf.data(), wbuf.size(), MSG_NOSIGNAL);
#else
					ssize_t n = ::send(fd, wbuf.data(), wbuf.size(), 0);
#endif
					if (n < 0)
						break;
					wbuf.erase(0, (size_t)n);
				}
			}
			up = false;
			close(fd);
			if (!stop && !told) {
				fmt::print("* llm mind: lost the mind bridge; creatures carry on with the homeostat\n");
				told = true;
			}
			pause(2000);
		}
#endif
	}
};

std::string helloLine() {
	std::ostringstream j;
	j << "{\"event\":\"hello\",\"version\":1,\"words\":[";
	for (size_t i = 0; i < engine.wordlist.size(); i++)
		j << (i ? "," : "") << jsonString(trimmed(engine.wordlist[i]));
	j << "]}\n";
	return j.str();
}

// ---- the mind ------------------------------------------------------------------

class LlmMind : public CreatureMind {
	std::unique_ptr<CreatureMind> inner = createHomeostatMind();
	std::ofstream log;
	bool logTried = false;
	unsigned int every = envCount("ZEETURES_MIND_LOG_EVERY", 10);
	unsigned int thinkEvery = envCount("ZEETURES_MIND_THINK_EVERY", 20);
	unsigned int sleepEvery = envCount("ZEETURES_MIND_SLEEP_EVERY", 150);
	unsigned int sinceLog = 0;
	unsigned int heard = 0; // shouts and signs since the last line
	// asking the sidecar
	bool pending = false;
	unsigned int askedAt = 0, lastAsk = 0;
	bool askedOnce = false;
	std::set<std::string> lastSeen; // creatures in view when it last asked
	bool handWasNear = false;
	unsigned int handLineTick = 0; // the hand's line it last asked about
	std::string handText; // set when the hand has just spoken to it
	// its plan: wishes that steer the homeostat until wishUntil (mind ticks),
	// and how many choices served each since it was made
	std::vector<MindWish> wishes;
	std::vector<int> wishDone;
	unsigned int wishUntil = 0;
	unsigned int wishTicks = envCount("ZEETURES_MIND_WISH_TICKS", 75); // about 30 s

	std::string id(const MindPerception& p) const {
		return p.self.empty() ? "creature-" + std::to_string(p.creatureId) : p.self;
	}

	void openLog(const MindPerception& p) {
		if (logTried)
			return;
		logTried = true;
		std::error_code ec;
		auto dir = logDir();
		std::filesystem::create_directories(dir, ec);
		std::string name = id(p);
		for (char& c : name)
			if (!(isalnum((unsigned char)c) || c == '-' || c == '_'))
				c = '_';
		log.open(dir / (name + ".jsonl"), std::ios::app);
	}

	// what it perceives (and chose), as one JSON object without a newline
	std::string perceptionJson(const char* event, const MindPerception& p, const MindOutput* out, const std::string& extra) const {
		std::ostringstream j;
		j << "{\"event\":" << jsonString(event)
		  << ",\"tick\":" << p.tick
		  << ",\"self\":" << jsonString(id(p))
		  << ",\"name\":" << jsonString(givenName(p.self))
		  << ",\"kind\":" << jsonString(kindName(p.genus))
		  << ",\"female\":" << (p.female ? "true" : "false")
		  << ",\"stage\":" << jsonString(format_as(p.stage))
		  << ",\"asleep\":" << (p.asleep || p.dozing ? "true" : "false") // dozing (event 69, Zeetures 0.28.3) is sleep too
		  << ",\"dozing\":" << (p.dozing ? "true" : "false")
		  << ",\"room\":" << p.room
		  << ",\"x\":" << num(p.x) << ",\"y\":" << num(p.y)
		  << ",\"season\":" << p.season << ",\"timeOfDay\":" << p.timeOfDay
		  << ",\"inWater\":" << (p.inWater ? "true" : "false")
		  << ",\"carrying\":" << (p.carryingCategory >= 0 ? jsonString(thingName(p.carryingCategory)) : "null");

		j << ",\"drives\":{";
		for (int i = 0; i < 17; i++)
			j << (i ? "," : "") << "\"" << LLM_DRIVE_NAMES[i] << "\":" << num(p.drives[i] / 255.0f);
		j << "}";

		j << ",\"temperament\":[";
		for (size_t i = 0; i < p.temperament.size(); i++)
			j << (i ? "," : "") << num(p.temperament[i]);
		j << "]";

		j << ",\"sees\":[";
		bool first = true;
		for (const SeenAgent& s : p.visible) {
			j << (first ? "" : ",") << "{\"what\":" << jsonString(thingName(s.category)) << ",\"cat\":" << s.category << ",\"dist\":" << num(s.distance);
			if (!s.who.empty())
				j << ",\"who\":" << jsonString(s.who) << ",\"name\":" << jsonString(givenName(s.who)) << ",\"kind\":" << jsonString(kindName(s.genus))
				  << ",\"female\":" << (s.female ? "true" : "false") << ",\"stage\":" << s.stage
				  << ",\"affinity\":" << num(s.affinity) << ",\"fear\":" << num(s.fear)
				  << ",\"familiar\":" << num(s.familiarity) << ",\"kin\":" << (s.kin ? "true" : "false");
			if (s.edible > 0)
				j << ",\"edible\":" << num(s.edible);
			j << "}";
			first = false;
		}
		j << "]";

		j << ",\"heard\":" << heard;
		if (p.handPat)
			j << ",\"handPat\":true";
		if (p.handSlap)
			j << ",\"handSlap\":true";
		if (p.heardAge >= 0) {
			if (p.heardVerb >= 0 && (size_t)p.heardVerb < engine.wordlist.size())
				j << ",\"heardVerb\":" << jsonString(trimmed(engine.wordlist[p.heardVerb]));
			if (p.heardNoun >= 0)
				j << ",\"heardNoun\":" << jsonString(thingName(p.heardNoun));
			if (p.heardNamed)
				j << ",\"heardNamed\":true";
		}
		j << ",\"stimuli\":[";
		first = true;
		for (const StimulusEvent& s : p.stimuli) {
			if (background(s))
				continue;
			j << (first ? "" : ",") << "{\"cmd\":" << jsonString(s.command);
			if (s.numbered)
				j << ",\"id\":" << s.id;
			j << ",\"sense\":" << s.feltSense << ",\"chem\":[";
			bool f2 = true;
			for (int k = 0; k < 4; k++)
				if (s.feltChem[k]) {
					j << (f2 ? "" : ",") << "[" << s.feltChem[k] << "," << s.feltAmount[k] << "]";
					f2 = false;
				}
			j << "]}";
			first = false;
		}
		j << "]";

		if (out) {
			j << ",\"chose\":{\"attention\":" << out->attentionCategory;
			if (out->attentionCategory >= 0)
				j << ",\"attend\":" << jsonString(thingName(out->attentionCategory));
			j << ",\"decision\":" << out->decision;
			if (out->decision >= 0)
				j << ",\"do\":" << jsonString(actionLabel(out->decision));
			j << ",\"sleep\":" << out->sleep;
			if (out->wishFollowed >= 0)
				j << ",\"wish\":" << out->wishFollowed;
			if (!out->say.empty())
				j << ",\"say\":" << jsonString(out->say);
			if (!out->thought.empty()) {
				j << ",\"thought\":[";
				for (size_t i = 0; i < out->thought.size(); i++)
					j << (i ? "," : "") << jsonString(out->thought[i]);
				j << "]";
			}
			j << "}";
		}
		if (!wishes.empty() && p.tick < wishUntil) {
			j << ",\"wishes\":[";
			for (size_t i = 0; i < wishes.size(); i++) {
				const MindWish& w = wishes[i];
				j << (i ? "," : "") << "{\"cat\":" << w.category << ",\"who\":" << jsonString(w.who) << ",\"do\":" << w.decision
				  << ",\"avoid\":" << (w.avoid ? "true" : "false") << ",\"strength\":" << w.strength << ",\"done\":" << (i < wishDone.size() ? wishDone[i] : 0) << "}";
			}
			j << "]";
		}
		j << extra << "}";
		return j.str();
	}

	void write(const char* event, const MindPerception& p, const MindOutput* out, const std::string& extra = "") {
		openLog(p);
		if (!log)
			return;
		log << perceptionJson(event, p, out, extra) << "\n";
		log.flush();
	}

	// whether to ask the sidecar now, and why
	std::string askReason(const MindPerception& p, bool notable) {
		std::set<std::string> seen;
		bool handNear = false;
		for (const SeenAgent& s : p.visible) {
			if (!s.who.empty())
				seen.insert(s.who);
			if (s.category == HAND_CATEGORY && s.distance < HAND_NEAR)
				handNear = true;
		}
		std::string met;
		for (const auto& w : seen)
			if (!lastSeen.count(w)) {
				met = w;
				break;
			}
		bool handCame = handNear && !handWasNear;
		handWasNear = handNear;
		unsigned int since = p.tick >= lastAsk ? p.tick - lastAsk : thinkEvery;
		const bool sleeping = p.asleep || p.dozing;
		// the hand spoke to it (it heard; the line is new)
		handText.clear();
		std::string line;
		unsigned int lineTick = 0;
		if (p.heardAge >= 0 && handLine(400, line, lineTick) && lineTick != handLineTick) {
			handText = line;
			handLineTick = lineTick;
		}
		std::string why;
		if (!askedOnce)
			why = "start";
		else if (!handText.empty())
			why = "hand-spoke";
		else if (since >= 2 && p.handSlap)
			why = "slapped";
		else if (since >= 2 && p.handPat)
			why = "patted";
		else if (!sleeping && since >= 10 && notable)
			why = "event";
		else if (!sleeping && since >= 5 && !met.empty())
			why = "met:" + met;
		else if (!sleeping && since >= 5 && handCame)
			why = "hand";
		else if (since >= (sleeping ? sleepEvery : thinkEvery))
			why = sleeping ? "asleep" : "timer";
		if (!why.empty())
			lastSeen = seen;
		return why;
	}

  public:
	std::string name() const override { return "llm"; }

	void born(const MindPerception& p) override {
		inner->born(p);
		write("start", p, nullptr); // a hatching, or a saved world loading
	}

	MindOutput tick(const MindPerception& p) override {
		Bridge& bridge = Bridge::get();
		// a reply from the sidecar: its own words, thoughts and plan
		Reply r;
		bool replied = bridge.take(id(p), r);
		if (replied && r.hasPlan) {
			wishes = r.plan;
			wishDone.assign(wishes.size(), 0);
			wishUntil = p.tick + wishTicks;
		}
		MindOutput out;
		if (!wishes.empty() && p.tick < wishUntil) {
			// the homeostat chooses, pulled by the creature's own wishes
			MindPerception q = p;
			q.wishes = wishes;
			out = inner->tick(q);
			if (out.wishFollowed >= 0 && out.wishFollowed < (int)wishDone.size())
				wishDone[(size_t)out.wishFollowed]++;
		} else {
			wishes.clear();
			wishDone.clear();
			out = inner->tick(p);
		}

		if (replied) {
			pending = false;
			if (!r.say.empty())
				out.say = r.say;
			if (!r.thought.empty()) {
				out.thought = r.thought;
				out.thoughtKey = r.key.empty() ? "llm" : r.key;
			}
		}
		if (pending && p.tick - askedAt > 50)
			pending = false; // lost (the sidecar restarted, or is very busy)

		sinceLog++;
		bool notable = false;
		for (const StimulusEvent& s : p.stimuli) {
			if (background(s))
				heard++;
			else
				notable = true;
		}

		// ask, if it is time
		if (bridge.enabled() && bridge.connected() && !pending) {
			if (!bridge.hasHello())
				bridge.setHello(helloLine());
			std::string why = askReason(p, notable);
			if (!why.empty()) {
				std::ostringstream extra;
				extra << ",\"reason\":" << jsonString(why);
				if (!handText.empty())
					extra << ",\"handSaid\":" << jsonString(handText);
				extra << ",\"known\":[";
				for (size_t i = 0; i < p.knownWords.size(); i++)
					extra << (i ? "," : "") << p.knownWords[i];
				extra << "]";
				bridge.send(perceptionJson("think", p, &out, extra.str()) + "\n");
				pending = true;
				askedOnce = true;
				askedAt = lastAsk = p.tick;
			}
		}

		if (sinceLog >= every || notable || out.decision >= 0 || !out.say.empty() || !out.thought.empty()) {
			write("tick", p, &out);
			sinceLog = 0;
			heard = 0;
		}
		return out;
	}

	void died(const MindPerception& p) override {
		write("died", p, nullptr);
		Bridge& bridge = Bridge::get();
		if (bridge.connected())
			bridge.send(perceptionJson("died", p, nullptr, "") + "\n");
		inner->died(p);
	}

	std::string debugState() const override { return inner->debugState(); }
	bool view(MindView& v) const override { return inner->view(v); }
	// It keeps no state of its own in the world save yet (its memories live
	// with the sidecar, by moniker), so its saved state is exactly the
	// homeostat's (WorldSave lets the two load each other's saves).
	std::string saveState() const override { return inner->saveState(); }
	void loadState(const std::string& s) override { inner->loadState(s); }
	std::vector<std::string> wantedSignatures() const override { return inner->wantedSignatures(); }
};

} // namespace

std::unique_ptr<CreatureMind> createLlmMind() {
	return std::make_unique<LlmMind>();
}
