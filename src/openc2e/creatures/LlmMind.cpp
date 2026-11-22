/*
 *  LlmMind.cpp
 *  openc2e (Zeetures fork: robpippy-max/zeetures, branch llm-mind)
 *
 *  The "llm" mind, phase 1: a stub. It wraps Zeetures's homeostat mind and
 *  changes nothing about how the creature behaves. It only writes what the
 *  creature perceives, and what the homeostat chose, to a JSON-lines log, one
 *  file per creature. Later phases add a local language model (via a Python
 *  sidecar and Ollama) as a slow layer on top; see docs/architecture.md in
 *  the creatures-ai repo.
 *
 *  Select it with --mind llm.
 *
 *  Log folder: $ZEETURES_MIND_LOG_DIR, else ~/Documents/Creatures/logs/mind.
 *  Logging pace: every $ZEETURES_MIND_LOG_EVERY mind ticks (default 10, about
 *  four seconds), and also on any tick with stimuli or a new decision.
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

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

std::unique_ptr<CreatureMind> createHomeostatMind(); // HomeostatMind.cpp

namespace {

const char* const LLM_DRIVE_NAMES[17] = {
	"pain", "pleasure", "hunger", "cold", "hot", "tired", "sleepy", "lonely",
	"crowded", "fear", "bored", "anger", "sex", "injury", "suffocate", "thirst", "stress"};

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

std::filesystem::path logDir() {
	if (const char* d = std::getenv("ZEETURES_MIND_LOG_DIR"))
		return d;
	const char* home = std::getenv("HOME");
	return std::filesystem::path(home ? home : ".") / "Documents" / "Creatures" / "logs" / "mind";
}

unsigned int logEvery() {
	if (const char* e = std::getenv("ZEETURES_MIND_LOG_EVERY")) {
		int n = std::atoi(e);
		if (n > 0)
			return (unsigned int)n;
	}
	return 10;
}

class LlmMind : public CreatureMind {
	std::unique_ptr<CreatureMind> inner = createHomeostatMind();
	std::ofstream log;
	bool logTried = false;
	unsigned int every = logEvery();
	unsigned int sinceLog = 0;

	void openLog(const MindPerception& p) {
		if (logTried)
			return;
		logTried = true;
		std::error_code ec;
		auto dir = logDir();
		std::filesystem::create_directories(dir, ec);
		std::string name = p.self.empty() ? "creature-" + std::to_string(p.creatureId) : p.self;
		for (char& c : name)
			if (!(isalnum((unsigned char)c) || c == '-' || c == '_'))
				c = '_';
		log.open(dir / (name + ".jsonl"), std::ios::app);
	}

	void write(const char* event, const MindPerception& p, const MindOutput* out) {
		openLog(p);
		if (!log)
			return;
		std::ostringstream j;
		j << "{\"event\":" << jsonString(event)
		  << ",\"tick\":" << p.tick
		  << ",\"self\":" << jsonString(p.self)
		  << ",\"genus\":" << p.genus
		  << ",\"female\":" << (p.female ? "true" : "false")
		  << ",\"stage\":" << (int)p.stage
		  << ",\"asleep\":" << (p.asleep ? "true" : "false")
		  << ",\"room\":" << p.room
		  << ",\"x\":" << num(p.x) << ",\"y\":" << num(p.y)
		  << ",\"season\":" << p.season << ",\"timeOfDay\":" << p.timeOfDay
		  << ",\"inWater\":" << (p.inWater ? "true" : "false")
		  << ",\"carrying\":" << p.carryingCategory;

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
			j << (first ? "" : ",") << "{\"cat\":" << s.category << ",\"dist\":" << num(s.distance);
			if (!s.who.empty())
				j << ",\"who\":" << jsonString(s.who) << ",\"genus\":" << s.genus
				  << ",\"affinity\":" << num(s.affinity) << ",\"fear\":" << num(s.fear)
				  << ",\"familiar\":" << num(s.familiarity) << ",\"kin\":" << (s.kin ? "true" : "false");
			if (s.edible > 0)
				j << ",\"edible\":" << num(s.edible);
			j << "}";
			first = false;
		}
		j << "]";

		j << ",\"stimuli\":[";
		first = true;
		for (const StimulusEvent& s : p.stimuli) {
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
			j << ",\"chose\":{\"attention\":" << out->attentionCategory
			  << ",\"decision\":" << out->decision
			  << ",\"sleep\":" << out->sleep;
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
		j << "}\n";
		log << j.str();
		log.flush();
	}

  public:
	std::string name() const override { return "llm"; }

	void born(const MindPerception& p) override {
		inner->born(p);
		write("born", p, nullptr);
	}

	MindOutput tick(const MindPerception& p) override {
		MindOutput out = inner->tick(p);
		sinceLog++;
		if (sinceLog >= every || !p.stimuli.empty() || out.decision >= 0 || !out.say.empty()) {
			write("tick", p, &out);
			sinceLog = 0;
		}
		return out;
	}

	void died(const MindPerception& p) override {
		write("died", p, nullptr);
		inner->died(p);
	}

	std::string debugState() const override { return inner->debugState(); }
	bool view(MindView& v) const override { return inner->view(v); }
	// Phase 1 keeps no state of its own, so its saved state is exactly the
	// homeostat's (WorldSave lets the two load each other's saves).
	std::string saveState() const override { return inner->saveState(); }
	void loadState(const std::string& s) override { inner->loadState(s); }
	std::vector<std::string> wantedSignatures() const override { return inner->wantedSignatures(); }
};

} // namespace

std::unique_ptr<CreatureMind> createLlmMind() {
	return std::make_unique<LlmMind>();
}
