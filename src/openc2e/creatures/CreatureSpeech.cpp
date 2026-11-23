/*
 *  CreatureSpeech.cpp
 *  openc2e
 *
 *  C1/C2 creatures speaking under a mind. See CreatureSpeech.h.
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

#include "CreatureSpeech.h"
#include "common/devenv.h"

#include "Agent.h"
#include "Bubble.h"
#include "Engine.h"
#include "PathResolver.h"
#include "VoiceData.h"
#include "World.h"
#include "PointerAgent.h"
#include "historyManager.h"
#include "common/Random.h"
#include "creatures/CreatureAgent.h"
#include "creatures/Breeding.h"
#include "creatures/oldCreature.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fmt/core.h>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace {

// the game's word list numbers (C2) for the words our meanings use
const int W_EAT = 12, W_HIT = 13, W_ME = 16, W_FOOD = 22, W_NORN = 52, W_DRIVE0 = 56, W_ANGRY = 67,
		  W_FRIENDLY = 68, W_HURT = 69, W_PAIN = 56, W_YES = 73, W_NO = 74, W_LOOK = 75, W_VERY = 77;

bool watching() {
	static const bool watch = devenv("OPENC2E_WATCH_SPEECH") != nullptr;
	return watch;
}

std::string lower(std::string s) {
	for (auto& ch : s)
		ch = (char)std::tolower((unsigned char)ch);
	return s;
}

std::string trim(const std::string& s) {
	size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool isWord(int w) {
	return w >= 0 && (size_t)w < engine.wordlist.size() && !trim(engine.wordlist[w]).empty();
}

oldCreature* creatureOf(Agent* a) {
	CreatureAgent* ca = dynamic_cast<CreatureAgent*>(a);
	return ca ? dynamic_cast<oldCreature*>(ca->getCreature()) : nullptr;
}

// (an agent with no picture yet, such as a blackboard just made: its corner)
float centreX(Agent* a) { return a->x + (a->part(0) ? a->getWidth() / 2.0f : 0.0f); }
float centreY(Agent* a) { return a->y + (a->part(0) ? a->getHeight() / 2.0f : 0.0f); }

// within earshot of a sound at (x, y)
bool inEarshot(Agent* listener, float x, float y) {
	return std::fabs(centreX(listener) - x) <= 350 && std::fabs(centreY(listener) - y) <= 150;
}

// how readily each life stage takes in words (baby ... senile)
float stageLearning(int stage) {
	static const float rate[7] = {0.75f, 1.25f, 1.1f, 1.0f, 0.9f, 0.7f, 0.4f};
	return (stage >= 0 && stage < 7) ? rate[stage] : 1.0f;
}

// a word with no voice to speak it: syllables of our own, always the same
std::string fallbackNative(const std::string& word) {
	static const char* syl[] = {"ba", "doo", "mi", "bo", "na", "lu"};
	uint32_t h = 2166136261u;
	for (char ch : word)
		h = (h ^ (unsigned char)ch) * 16777619u;
	std::string out = syl[h % 6];
	out += "-";
	out += syl[(h >> 8) % 6];
	return out;
}

// the word as the creature's bubble shows it: its own name for the thing
// (ownNames), the learned word, or its own tongue's
std::string showWord(Agent* creature, oldCreature* c, int w) {
	std::string word = isWord(w) ? trim(engine.wordlist[w]) : std::string();
	if (w >= 16 && w < 56)
		if (std::string own = ownNameFor(c, w - 16); !own.empty())
			return own;
	if (knowsWord(c, w))
		return word;
	return nativeWord(creature, word);
}

// meaning "kind:a:b" split
std::vector<std::string> parts(const std::string& m) {
	std::vector<std::string> out;
	size_t start = 0;
	for (;;) {
		size_t c = m.find(':', start);
		out.push_back(m.substr(start, c == std::string::npos ? std::string::npos : c - start));
		if (c == std::string::npos)
			break;
		start = c + 1;
	}
	return out;
}

// one way of putting a meaning: its words and which one carries it
struct Wording {
	std::vector<int> words;
	int key; // index into words
};

std::map<int, AgentRef> bubbles; // each speaker's bubble, by UNID

// the player's settings file: "key value" lines
std::filesystem::path settingsFile() {
	return getUserDataDir() / "settings.txt";
}

std::string readSetting(const std::string& key) {
	std::ifstream in(settingsFile());
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream l(line);
		std::string k, v;
		if (l >> k >> v && k == key)
			return v;
	}
	return "";
}

void writeSetting(const std::string& key, const std::string& value) {
	std::vector<std::string> lines;
	{
		std::ifstream in(settingsFile());
		std::string line;
		while (std::getline(in, line)) {
			std::istringstream l(line);
			std::string k;
			if (!(l >> k && k == key))
				lines.push_back(line);
		}
	}
	lines.push_back(key + " " + value);
	std::ofstream out(settingsFile(), std::ios::binary);
	for (auto& l : lines)
		out << l << "\n";
}

int s_bubbles_shown = -1; // -1: not read yet

} // namespace

std::string readPlayerSetting(const std::string& key) {
	return readSetting(key);
}

void writePlayerSetting(const std::string& key, const std::string& value) {
	writeSetting(key, value);
}

static int s_population_limit = -1; // -1: not read yet

int populationLimit() {
	if (s_population_limit < 0) {
		// developer aid: OPENC2E_POPULATION_LIMIT=n starts with that limit
		// (not saved), for tests that must not touch the player's settings
		if (const char* e = devenv("OPENC2E_POPULATION_LIMIT"))
			return s_population_limit = std::max(0, atoi(e));
		// (Zeetures: 150 until the player sets one; 0 is no limit)
		std::string v = readSetting("population-limit");
		s_population_limit = v.empty() ? 150 : std::max(0, atoi(v.c_str()));
	}
	return s_population_limit;
}

void setPopulationLimit(int norns) {
	s_population_limit = std::max(0, norns);
	writeSetting("population-limit", std::to_string(s_population_limit));
}

static int s_autosave_minutes = -1; // -1: not read yet

int autosaveMinutes() {
	if (s_autosave_minutes < 0) {
		std::string v = readSetting("autosave-minutes");
		s_autosave_minutes = v.empty() ? 5 : std::max(0, atoi(v.c_str()));
	}
	return s_autosave_minutes;
}

void setAutosaveMinutes(int minutes) {
	s_autosave_minutes = std::max(0, minutes);
	writeSetting("autosave-minutes", std::to_string(s_autosave_minutes));
}

bool speechBubbleShowing(Agent* creature) {
	if (!creature)
		return false;
	auto it = bubbles.find(creature->getUNID());
	return it != bubbles.end() && it->second && !it->second->isDying();
}

bool speechBubblesShown() {
	if (s_bubbles_shown < 0)
		s_bubbles_shown = readSetting("speech-bubbles") == "0" ? 0 : 1;
	return s_bubbles_shown == 1;
}

void setSpeechBubblesShown(bool shown) {
	s_bubbles_shown = shown ? 1 : 0;
	writeSetting("speech-bubbles", shown ? "1" : "0");
	if (!shown) {
		for (auto& b : bubbles)
			if (b.second)
				b.second->kill();
		bubbles.clear();
	}
}

std::string nativeWord(Agent* creature, const std::string& word) {
	if (word.empty())
		return "";
	if (!creature || !creature->voice || !*creature->voice)
		return fallbackNative(lower(word));
	std::string out;
	for (auto& e : creature->voice->GetSyllablesFor(word)) {
		// the game's syllable sounds are named for the syllable, then where
		// it falls in a word (Doh0, Doh1, DohE; Do_0 for "do")
		std::string syl = lower(e.name.substr(0, 3));
		syl.erase(std::remove(syl.begin(), syl.end(), '_'), syl.end());
		if (syl.empty())
			continue;
		out += (out.empty() ? "" : "-") + syl;
	}
	return out.empty() ? fallbackNative(lower(word)) : out;
}

bool knowsWord(oldCreature* c, int word) {
	return c && word >= 0 && (size_t)word < c->vocab.size() && c->vocab[word] >= WORD_KNOWN;
}

int wordsKnown(oldCreature* c) {
	int n = 0;
	for (size_t i = 0; c && i < c->vocab.size(); i++)
		n += c->vocab[i] >= WORD_KNOWN && isWord((int)i);
	return n;
}

void hearWord(oldCreature* c, int word, int amount) {
	if (!c || !isWord(word) || !c->isAlive() || c->isAsleep())
		return;
	if (c->vocab.size() < engine.wordlist.size())
		c->vocab.resize(engine.wordlist.size(), 0);
	bool knew = c->vocab[word] >= WORD_KNOWN;
	int v = c->vocab[word] + (int)std::lround(amount * stageLearning((int)c->getStage()));
	c->vocab[word] = (unsigned char)std::min(v, 255);
	if (!knew && c->vocab[word] >= WORD_KNOWN) {
		int n = wordsKnown(c);
		std::string mon = world.history->findMoniker(c->getGenome());
		if (n == 1)
			world.history->tell(mon, storyEntry::firstWord, "", trim(engine.wordlist[word]));
		else if (n == 10 || n == 25 || n == 50)
			world.history->tell(mon, storyEntry::words, "", std::to_string(n));
		if (watching())
			fmt::print("* LEARNS \"{}\" ({} words known)\n", trim(engine.wordlist[word]), n);
	}
}

int ownNameMeans(oldCreature* c, const std::string& word) {
	if (!c)
		return -1;
	auto it = c->ownNames.find(word);
	if (it == c->ownNames.end())
		return -1;
	int best = -1, most = WORD_KNOWN - 1;
	for (auto& [category, v] : it->second)
		if (v > most)
			most = v, best = category;
	return best;
}

std::string ownNameFor(oldCreature* c, int category) {
	std::string best;
	int most = WORD_KNOWN - 1;
	if (c)
		for (auto& [word, for_] : c->ownNames)
			if (auto it = for_.find(category); it != for_.end() && it->second > most && ownNameMeans(c, word) == category)
				most = it->second, best = word;
	return best;
}

void hearOwnName(oldCreature* c, const std::string& word, int category, int amount) {
	const size_t mostNames = 40;
	if (!c || word.empty() || category < 1 || category > 35 || !c->isAlive() || c->isAsleep())
		return;
	bool knew = ownNameMeans(c, word) == category;
	int more = (int)std::lround(amount * stageLearning((int)c->getStage()));
	int& v = c->ownNames[word][category];
	v = std::min(255, v + more);
	// a word comes to mean the one thing, and a thing to have the one name:
	// what it meant before, and the thing's other names, grow fainter
	for (auto& [w, for_] : c->ownNames)
		for (auto& [cat, other] : for_)
			if ((w == word) != (cat == category))
				other = std::max(0, other - more / 2);
	if (c->ownNames.size() > mostNames) {
		auto least = c->ownNames.end();
		int fewest = 1 << 30;
		for (auto it = c->ownNames.begin(); it != c->ownNames.end(); ++it) {
			int most = 0;
			for (auto& [cat, n] : it->second)
				most = std::max(most, n);
			if (it->first != word && most < fewest)
				fewest = most, least = it;
		}
		if (least != c->ownNames.end())
			c->ownNames.erase(least);
	}
	if (!knew && ownNameMeans(c, word) == category) {
		if (wordsKnown(c) == 0 && ownNamesKnown(c) == 1)
			world.history->tell(world.history->findMoniker(c->getGenome()), storyEntry::firstWord, "", word);
		if (watching())
			fmt::print("* LEARNS the name \"{}\" for category {}\n", word, category);
	}
}

int ownNamesKnown(oldCreature* c) {
	int n = 0;
	if (c)
		for (auto& [word, for_] : c->ownNames)
			n += ownNameMeans(c, word) >= 0;
	return n;
}

void learnAllWords(oldCreature* c) {
	if (!c)
		return;
	c->vocab.assign(engine.wordlist.size(), 255);
	world.history->tell(world.history->findMoniker(c->getGenome()), storyEntry::allWords);
	if (watching())
		fmt::print("* LEARNS every word ({})\n", wordsKnown(c));
}

void hearSpeech(Agent* speaker, const std::string& text, int amount) {
	if (!speaker || engine.version >= 3 || engine.wordlist.empty())
		return;
	// the words in the text: lower case, punctuation as spaces, each padded
	std::string t = " ";
	for (char ch : lower(text))
		t += std::isalnum((unsigned char)ch) || (unsigned char)ch >= 128 ? ch : ' ';
	t += " ";
	std::vector<int> heard;
	for (size_t w = 0; w < engine.wordlist.size(); w++) {
		std::string word = lower(trim(engine.wordlist[w]));
		if (word.empty())
			continue;
		if (t.find(" " + word + " ") != std::string::npos)
			heard.push_back((int)w);
	}
	hearWords(speaker, heard, amount);
}

void hearWords(Agent* speaker, const std::vector<int>& heard, int amount) {
	if (!speaker || heard.empty() || engine.version >= 3)
		return;
	float sx = centreX(speaker), sy = centreY(speaker);
	// family heard twice as well (owner, 2026-11-20: learning from parents,
	// "family most")
	oldCreature* sc = creatureOf(speaker);
	std::string from = sc ? world.history->findMoniker(sc->getGenome()) : std::string();
	static const bool noWatching = devenv("OPENC2E_NO_WATCHING") != nullptr; // (to compare)
	for (auto& a : world.agents) {
		if (!a || a.get() == speaker || a->isDying())
			continue;
		oldCreature* c = creatureOf(a.get());
		if (!c || !inEarshot(a.get(), sx, sy))
			continue;
		int got = amount;
		if (!noWatching && !from.empty() && closeKin(from, world.history->findMoniker(c->getGenome())))
			got = amount * 2;
		for (int w : heard)
			hearWord(c, w, got);
	}
}

void hearMachineWord(Agent* machine, int word, bool aloud) {
	if (!machine || engine.version >= 3 || !isWord(word))
		return;
	float mx = centreX(machine), my = centreY(machine);
	// the machine's edges (a teaching computer is 157 px wide)
	float left = machine->x, right = machine->x + (machine->part(0) ? machine->getWidth() : 0.0f);
	float top = machine->y, bottom = machine->y + (machine->part(0) ? machine->getHeight() : 0.0f);
	for (auto& a : world.agents) {
		if (!a || a->isDying())
			continue;
		oldCreature* c = creatureOf(a.get());
		if (!c || !c->isAlive() || c->isAsleep())
			continue;
		float cx = centreX(a.get()), cy = centreY(a.get());
		float dx = std::max({0.0f, left - cx, cx - right}), dy = std::max({0.0f, top - cy, cy - bottom});
		bool looking = c->getAttentionFocus() == AgentRef(machine) || (dx <= 120 && dy <= 80);
		if (looking) {
			hearWord(c, word, HEAR_MACHINE);
			// being taught, it says the word back: in its own tongue until it
			// has learned it, then the word itself (as C2's creatures repeat
			// what the machine shows)
			int stage = (int)c->getStage();
			if (stage >= 1) {
				Utterance u;
				u.spoken = trim(engine.wordlist[word]);
				u.shown = knowsWord(c, word) ? u.spoken : nativeWord(a.get(), u.spoken);
				if (knowsWord(c, word))
					u.learned.push_back(word);
				creatureSays(a.get(), stage, u);
			}
		} else if (aloud && inEarshot(a.get(), mx, my))
			hearWord(c, word, HEAR_MACHINE_FAR);
	}
}

Utterance wordUtterance(Agent* creature, oldCreature* c, int word) {
	Utterance u;
	if (!isWord(word))
		return u;
	u.spoken = trim(engine.wordlist[word]);
	u.shown = showWord(creature, c, word);
	u.learned.push_back(word);
	return u;
}

Utterance phraseFor(const std::string& meaning, Agent* creature, oldCreature* c) {
	Utterance u;
	if (!creature || !c)
		return u;
	std::vector<std::string> p = parts(meaning);
	const std::string& kind = p[0];
	int a = p.size() > 1 ? atoi(p[1].c_str()) : 0;
	int b = p.size() > 2 ? atoi(p[2].c_str()) : 0;
	int stage = (int)c->getStage();
	std::string trail = stage >= 6 ? "..." : "";

	// says the given words: learned ones as words, the rest in its own tongue
	auto say = [&](const std::vector<int>& words, const std::string& end) {
		for (int w : words) {
			if (!isWord(w))
				continue;
			std::string shown = showWord(creature, c, w);
			if (shown.empty())
				continue;
			u.shown += (u.shown.empty() ? "" : " ") + shown;
			std::string own = w >= 16 && w < 56 ? ownNameFor(c, w - 16) : std::string();
			u.spoken += (u.spoken.empty() ? "" : " ") + (own.empty() ? trim(engine.wordlist[w]) : own);
			if (!own.empty())
				u.own.push_back({own, w - 16});
			else if (knowsWord(c, w))
				u.learned.push_back(w);
		}
		if (!u.shown.empty())
			u.shown += trail.empty() ? end : trail;
	};

	// babies: cries, and the sounds of their own tongue, no words
	if (stage <= 0) {
		if (kind == "ow") {
			u.shown = u.spoken = rand_float(0, 1) < 0.5f ? "waah!" : "eee!";
			return u;
		}
		if (kind == "yum" || kind == "love") {
			u.shown = u.spoken = "mmm";
			return u;
		}
		if (kind == "grr") {
			u.shown = u.spoken = "nnngh!";
			return u;
		}
		// one short word of its tongue, not yet meaning anything
		static const int shortwords[] = {W_ME, W_EAT, W_HIT, W_YES, W_NO, 6, 5, 8, 21, 29, 34, 57, 60};
		int w = shortwords[rand_size_t(0, sizeof(shortwords) / sizeof(int) - 1)];
		if (isWord(w)) {
			u.spoken = trim(engine.wordlist[w]);
			u.shown = nativeWord(creature, u.spoken);
		}
		return u;
	}

	// chosen words (the llm mind, robpippy-max fork 2026-11-22):
	// "words:A:B:C", word-list indices, said in order; learned ones as words,
	// the rest in its own tongue. A child says one word, the grown up to four.
	if (kind == "words") {
		std::vector<int> words;
		size_t most = stage == 1 ? 1 : 4;
		for (size_t i = 1; i < p.size() && words.size() < most; i++)
			words.push_back(atoi(p[i].c_str()));
		say(words, "");
		return u;
	}

	std::vector<Wording> ways;
	std::string end;
	if (kind == "need") {
		if (a < 0 || a > 16)
			return u;
		std::vector<int> w = {W_ME};
		if (b > 200)
			w.push_back(W_VERY);
		w.push_back(W_DRIVE0 + a);
		ways.push_back({w, (int)w.size() - 1});
		end = b > 200 ? "!" : "";
	} else if (kind == "ow") {
		ways = {{{W_PAIN}, 0}, {{W_HURT}, 0}, {{W_ME, W_HURT}, 1}};
		end = "!";
	} else if (kind == "yum") {
		ways = {{{W_FOOD}, 0}, {{W_EAT, W_FOOD}, 1}, {{W_YES, W_FOOD}, 1}};
		end = "!";
	} else if (kind == "grr") {
		ways = {{{W_ME, W_ANGRY}, 1}, {{W_NO}, 0}, {{W_HIT}, 0}};
		end = "!";
	} else if (kind == "love") {
		ways = {{{W_FRIENDLY}, 0}, {{W_YES}, 0}, {{W_ME, W_FRIENDLY}, 1}};
	} else if (kind == "greet") {
		if (a < 1 || a > 3)
			return u;
		ways = {{{W_NORN + a - 1}, 0}, {{W_LOOK, W_NORN + a - 1}, 1}};
		end = "!";
	} else if (kind == "name") {
		// a thing's name (owner, 2026-11-20: the young learn words by hearing
		// others name things): C2's noun for category a follows its 16 verbs
		int w = 16 + a;
		if (a < 0 || a > 39 || !isWord(w))
			return u;
		ways = {{{w}, 0}, {{W_LOOK, w}, 1}};
		end = "!";
	} else {
		// nothing in particular: trying out a word it knows, or chatter in
		// its own tongue
		std::vector<int> known;
		for (size_t w = 0; w < c->vocab.size(); w++)
			if (knowsWord(c, (int)w) && isWord((int)w))
				known.push_back((int)w);
		// or a name of its own for a thing (so names pass on)
		std::vector<int> named;
		for (int category = 1; category <= 35; category++)
			if (!ownNameFor(c, category).empty() && isWord(16 + category))
				named.push_back(16 + category);
		if (!named.empty() && rand_float(0, 1) < 0.25f) {
			say({named[rand_size_t(0, named.size() - 1)]}, "?");
			return u;
		}
		if (!known.empty() && rand_float(0, 1) < 0.35f) {
			say({known[rand_size_t(0, known.size() - 1)]}, "?");
			return u;
		}
		// chatter: words it has not learned, in its own tongue. Picked from
		// the words it doesn't know; before, it drew at random until it had
		// enough unknown ones, which never ended for a creature that knows
		// every word (the Stone of Knowledge teaches them all) and froze the
		// game (2026-11-12, two force-quits). Knowing them all, it tries out
		// a word it knows instead.
		std::vector<int> unknown;
		for (size_t w = 0; w < engine.wordlist.size(); w++)
			if (isWord((int)w) && !knowsWord(c, (int)w))
				unknown.push_back((int)w);
		if (unknown.empty()) {
			if (!known.empty())
				say({known[rand_size_t(0, known.size() - 1)]}, "?");
			return u;
		}
		std::vector<int> chatter;
		int n = (int)rand_size_t(1, stage == 1 ? 1 : 3);
		for (int i = 0; i < n; i++)
			chatter.push_back(unknown[rand_size_t(0, unknown.size() - 1)]);
		say(chatter, "");
		return u;
	}

	// the wording it knows most of (ties at random)
	int best = 0, bestscore = -1;
	for (size_t i = 0; i < ways.size(); i++) {
		int score = 0;
		for (size_t j = 0; j < ways[i].words.size(); j++)
			score += knowsWord(c, ways[i].words[j]) ? ((int)j == ways[i].key ? 4 : 1) : 0;
		score = score * 8 + (int)rand_size_t(0, 7);
		if (score > bestscore) {
			bestscore = score;
			best = (int)i;
		}
	}
	const Wording& way = ways[best];
	if (stage == 1)
		say({way.words[way.key]}, end); // children: the one word
	else
		say(way.words, end);
	return u;
}

float voicePitch(int stage) {
	static const float pitch[7] = {1.4f, 1.25f, 1.1f, 1.05f, 1.0f, 0.92f, 0.88f};
	return (stage >= 0 && stage < 7) ? pitch[stage] : 1.0f;
}

float voicePace(int stage) {
	static const float pace[7] = {0.8f, 1.0f, 1.1f, 1.15f, 1.0f, 0.85f, 0.7f};
	return (stage >= 0 && stage < 7) ? pace[stage] : 1.0f;
}

namespace {
std::deque<SpokenLine> s_history;
}

const std::deque<SpokenLine>& speechHistory() {
	return s_history;
}

void recordSpeech(Agent* speaker, const std::string& text) {
	if (!speaker || text.empty())
		return;
	s_history.push_front({AgentRef(speaker), text, world.worldtickcount});
	while (s_history.size() > 40)
		s_history.pop_back();
}

void handSays(const std::string& text) {
	Agent* hand = (Agent*)world.hand();
	if (!hand || text.empty() || engine.version >= 3)
		return;
	Bubble* b = Bubble::newBubble(hand, true, text);
	b->setTimeout(35);
	hearSpeech(hand, text, HEAR_HAND);
	recordSpeech(hand, text);
	answerHand(text);
}

void handSaysAgain() {
	for (auto& line : s_history)
		if (line.speaker && line.speaker.get() == (Agent*)world.hand()) {
			std::string text = line.text;
			handSays(text);
			return;
		}
}

// Being spoken to (a player, 2026-11-22: "the baby not looking at me, and
// there being no response to anything I say"; C2's manual has norns look,
// repeat and obey). Words are learned in hearWords, before this.
void answerHand(const std::string& text) {
	Agent* hand = (Agent*)world.hand();
	if (!hand || engine.version != 2 || engine.wordlist.empty())
		return;
	std::string t = " ";
	for (char ch : lower(text))
		t += std::isalnum((unsigned char)ch) || (unsigned char)ch >= 128 ? ch : ' ';
	t += " ";
	// the list's words in it, in the order said
	std::vector<std::pair<size_t, int>> said;
	for (size_t w = 0; w < engine.wordlist.size(); w++) {
		std::string word = lower(trim(engine.wordlist[w]));
		if (word.empty())
			continue;
		if (size_t at = t.find(" " + word + " "); at != std::string::npos)
			said.push_back({at, (int)w});
	}
	std::sort(said.begin(), said.end());
	// the words of it, and a name it may be teaching: the one word of at
	// least two letters that is no word of the list, no creature's name and
	// no everyday filler
	std::vector<std::string> tokens;
	{
		std::istringstream in(t);
		for (std::string w; in >> w;)
			tokens.push_back(w);
	}
	auto listed = [&](const std::string& w) {
		for (auto& l : engine.wordlist)
			if (lower(trim(l)) == w)
				return true;
		return false;
	};
	std::set<std::string> creatureNames;
	for (auto& a : world.agents)
		if (oldCreature* c = a && a->family == 4 ? creatureOf(a.get()) : nullptr) {
			std::string mon = world.history->findMoniker(c->getGenome());
			if (!mon.empty() && world.history->hasMoniker(mon)) {
				std::istringstream in(lower(world.history->getMoniker(mon).name));
				for (std::string w; in >> w;)
					creatureNames.insert(w);
			}
		}
	static const std::set<std::string> filler = {"hello", "hi", "hey", "good", "bad", "please", "thanks", "thank", "the", "an", "is", "it", "its",
		"this", "that", "my", "you", "your", "and", "to", "for", "of", "in", "on", "at", "am", "are", "do", "dont", "not", "ok", "okay", "norn", "norns"};
	std::string naming;
	int strange = 0;
	for (auto& w : tokens)
		if (w.size() >= 2 && std::isalpha((unsigned char)w[0]) && !listed(w) && !creatureNames.count(w) && !filler.count(w)) {
			naming = w;
			strange++;
		}
	if (strange != 1)
		naming.clear();

	struct Listener {
		Agent* a;
		oldCreature* c;
		float distance;
		bool named;
	};
	std::vector<Listener> all, named;
	const float hx = centreX(hand), hy = centreY(hand);
	for (auto& a : world.agents) {
		if (!a || a->isDying())
			continue;
		oldCreature* c = creatureOf(a.get());
		if (!c || !c->isAlive() || c->isAsleep() || c->isUnconscious() || !inEarshot(a.get(), hx, hy))
			continue;
		Listener l{a.get(), c, std::hypot(centreX(a.get()) - hx, centreY(a.get()) - hy), false};
		std::string mon = world.history->findMoniker(c->getGenome());
		std::string name = mon.empty() || !world.history->hasMoniker(mon) ? std::string() : lower(trim(world.history->getMoniker(mon).name));
		if (name.size() >= 2 && t.find(" " + name + " ") != std::string::npos) {
			l.named = true;
			named.push_back(l);
		}
		all.push_back(l);
	}
	// spoken to by name: only they are meant
	std::vector<Listener>& meant = named.empty() ? all : named;
	std::sort(meant.begin(), meant.end(), [](const Listener& a, const Listener& b) { return a.distance < b.distance; });
	// what the hand is showing: what it holds, or points at
	// (the smallest thing there: creatures in front, and scenery, don't count)
	Agent* shown = hand->carrying.get();
	if (!shown) {
		const float px = world.hand()->pointerX(), py = world.hand()->pointerY();
		float least = 1e9f;
		for (auto& a : world.agents) {
			if (!a || a->isDying() || a->category < 2 || a->category > 35 || a->invisible() || a->carriedby || !a->part(0))
				continue;
			const float w = a->getWidth(), h = a->getHeight();
			if (px >= a->x - 10 && px <= a->x + w + 10 && py >= a->y - 10 && py <= a->y + h + 10 && w * h < least)
				least = w * h, shown = a.get();
		}
	}
	const int shownCategory = shown && shown != hand && shown->category >= 2 && shown->category <= 35 ? shown->category : -1;
	int rank = 0;
	for (auto& l : meant) {
		oldCreature::Spoken s;
		int named = -1; // the thing it takes a strange name to be for
		// names of its own it knows
		for (auto& w : tokens)
			if (int own = ownNameMeans(l.c, w); own >= 0)
				s.noun = own;
		if (!naming.empty()) {
			const unsigned int now = world.tickcount;
			auto lately = [&](unsigned int then, unsigned int within) { return then != 0 && now >= then && now - then < within; };
			if (ownNameMeans(l.c, naming) >= 0 && shownCategory < 0 && !lately(l.c->touchedTick, 100))
				named = -1; // (known, and nothing new shown: simply understood)
			else if (l.c->namingWord == naming && lately(l.c->namingTick, 150))
				named = l.c->namingCategory; // the same again
			else if (lately(l.c->touchedTick, 100))
				named = 1; // just touched by the hand: the hand
			else if (shownCategory >= 0)
				named = shownCategory;
			else if (Agent* it = l.c->getAttentionFocus().get(); it && it->category >= 2 && it->category <= 35 && std::hypot(centreX(it) - hx, centreY(it) - hy) < 90)
				named = it->category; // what it is looking at, the hand beside it
			else
				named = 1;
			if (named >= 0) {
				hearOwnName(l.c, naming, named, HEAR_HAND);
				l.c->namingWord = naming;
				l.c->namingCategory = named;
				l.c->namingTick = std::max(1u, now);
				s.noun = named;
			}
		}
		for (auto& [at, w] : said) {
			if (!knowsWord(l.c, w))
				continue;
			if (w <= 13)
				s.verb = w;
			else if (w >= 16 && w < 16 + 40)
				s.noun = w - 16; // the noun's category
		}
		s.named = l.named;
		s.tick = world.tickcount;
		l.c->spoken = s;
		l.c->callTo(s.noun >= 0 ? s.noun : 1); // the thing, or the hand
		// the nearest two (or those named) say it back
		if (rank < 2 || l.named) {
			l.c->replyWord = said.empty() ? -1 : said.back().second;
			// (a name it is being taught, or one of its own, over the rest)
			l.c->replyName = naming;
			if (naming.empty())
				for (auto& w : tokens)
					if (ownNameMeans(l.c, w) >= 0)
						l.c->replyName = w;
			l.c->replyAt = world.tickcount + 8 + 9 * rank + rand_int32(0, 5);
		}
		rank++;
	}
	if (watching())
		fmt::print("* HAND SAYS \"{}\": {} listening{}\n", text, meant.size(), named.empty() ? "" : " (named)");
}

void tickReply(Agent* creature, oldCreature* c) {
	if (c->replyAt == 0 || world.tickcount < c->replyAt)
		return;
	c->replyAt = 0;
	if (!c->isAlive() || c->isAsleep() || c->isUnconscious())
		return;
	int stage = (int)c->getStage();
	if (!c->replyName.empty()) {
		Utterance u;
		u.spoken = c->replyName;
		int category = ownNameMeans(c, c->replyName);
		bool knows = stage > 0 && category >= 0;
		u.shown = knows ? u.spoken : nativeWord(creature, u.spoken);
		if (knows)
			u.own.push_back({c->replyName, category});
		c->replyName.clear();
		creatureSays(creature, stage, u);
		return;
	}
	if (c->replyWord < 0 || !isWord(c->replyWord)) {
		creatureSays(creature, stage, phraseFor("babble", creature, c));
		return;
	}
	// in its own tongue until it has learned the word (a baby always: it has
	// no words yet)
	Utterance u;
	u.spoken = trim(engine.wordlist[c->replyWord]);
	bool knows = stage > 0 && knowsWord(c, c->replyWord);
	u.shown = knows ? u.spoken : nativeWord(creature, u.spoken);
	if (knows)
		u.learned.push_back(c->replyWord);
	creatureSays(creature, stage, u);
}

void creatureSays(Agent* creature, int stage, const Utterance& u) {
	if (!creature || u.shown.empty() || engine.version >= 3)
		return;
	// one bubble at a time for each speaker (none with View > Speech
	// Bubbles off, or while it rides out of sight)
	auto it = bubbles.find(creature->getUNID());
	if (it != bubbles.end() && it->second)
		it->second->kill();
	if (speechBubblesShown() && !Bubble::hiddenRider(creature)) {
		Bubble* b = Bubble::newBubble(creature, true, u.shown);
		b->setTimeout(35); // 3.5 s
		bubbles[creature->getUNID()] = AgentRef(b);
	}
	hearWords(creature, u.learned, HEAR_CREATURE);
	// and its own names for things: names spread, family hearing them twice
	// as well
	if (!u.own.empty()) {
		std::string from = world.history->findMoniker(creatureOf(creature)->getGenome());
		float sx = centreX(creature), sy = centreY(creature);
		for (auto& a : world.agents) {
			oldCreature* c = a && a.get() != creature && !a->isDying() ? creatureOf(a.get()) : nullptr;
			if (!c || !inEarshot(a.get(), sx, sy))
				continue;
			int amount = HEAR_CREATURE * (!from.empty() && closeKin(from, world.history->findMoniker(c->getGenome())) ? 2 : 1);
			for (auto& [word, category] : u.own)
				hearOwnName(c, word, category, amount);
		}
	}
	recordSpeech(creature, u.shown);
	try {
		creature->speak(u.spoken.empty() ? u.shown : u.spoken, voicePitch(stage), voicePace(stage));
	} catch (std::exception&) {
		// no voice for this creature: the bubble alone
	}
	if (watching())
		fmt::print("* SAYS {} {} {} (stage {}): \"{}\" ({}){}\n", (int)creature->family, (int)creature->genus, creature->getUNID(), stage, u.shown, u.spoken, Bubble::hiddenRider(creature) ? " out of sight, no bubble" : "");
}
