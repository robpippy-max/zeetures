// The Science Kit (Creatures 2): Physical, dDNA, Biochemistry and Syringe
// pages. C2's own art from the player's Applet Data; our layout. In C2 the
// dDNA page and the syringe's advanced mode needed a power-up; here they are
// always available (owner, 2026-11-09).

#include "C2KitsInternal.h"
#include "common/devenv.h"
#include "creatures/BreedSlots.h"
#include "PathResolver.h"
#include "World.h"
#include "creatures/oldCreature.h"
#include "fileformats/genomeFile.h"
#include "fileformats/verticalbarFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fmt/format.h>
#include <imgui.h>
#include <map>

using namespace ImGuiUtils;

namespace Openc2eImgui {

static std::string chemName(int i) {
	const auto& n = KitChemicalNames();
	return (i >= 0 && i < (int)n.size()) ? n[i] : fmt::format("<{}>", i);
}

// ---- Physical ------------------------------------------------------------------

static void PhysicalPage() {
	if (!KitNeedsCreature())
		return;
	c2Creature* c = KitCreature();
	auto& organs = c->getOrgans();
	ImGui::Text("%d organs. Hover a row for more.", (int)organs.size());
	ImGui::SameLine();
	ImGui::TextDisabled("(names from the species' gene notes)");
	if (ImGui::BeginTable("organs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(660, 300))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Organ", ImGuiTableColumnFlags_WidthFixed, 250);
		ImGui::TableSetupColumn("Health", ImGuiTableColumnFlags_WidthFixed, 130);
		ImGui::TableSetupColumn("Clock");
		ImGui::TableSetupColumn("Energy");
		ImGui::TableSetupColumn("Reactions");
		ImGui::TableSetupColumn("Rec / Emit");
		ImGui::TableHeadersRow();
		for (size_t i = 0; i < organs.size(); i++) {
			auto& o = organs[i];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			bool brain = o->getGene() && o->getGene()->isBrain();
			const KitGeneNote* note = KitNoteFor(o->getGene());
			if (note && !note->caption.empty())
				ImGui::TextWrapped("%s", note->caption.c_str()); // long names take a second line
			else
				ImGui::Text("%s %d", brain ? "Brain organ" : "Organ", (int)i + 1);
			bool hov = ImGui::IsItemHovered();
			ImGui::TableNextColumn();
			float start = o->getInitialLifeforce();
			float st = start > 0 ? std::clamp(o->getShortTermLifeforce() / start, 0.0f, 1.0f) : 0;
			float lt = start > 0 ? std::clamp(o->getLongTermLifeforce() / start, 0.0f, 1.0f) : 0;
			ImVec4 col = st > 0.75f ? ImVec4(0.3f, 0.8f, 0.3f, 1) : st > 0.4f ? ImVec4(0.9f, 0.7f, 0.2f, 1) : ImVec4(0.9f, 0.3f, 0.2f, 1);
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
			ImGui::ProgressBar(st, ImVec2(124, 0), fmt::format("{:.0f}%", st * 100).c_str());
			ImGui::PopStyleColor();
			hov |= ImGui::IsItemHovered();
			ImGui::TableNextColumn();
			ImGui::Text("%d", o->getClockRate());
			ImGui::TableNextColumn();
			ImGui::Text("%d", o->getEnergyCost());
			ImGui::TableNextColumn();
			ImGui::Text("%d", (int)o->getReactionCount());
			ImGui::TableNextColumn();
			ImGui::Text("%d / %d", (int)o->getReceptorCount(), (int)o->getEmitterCount());
			if (hov || ImGui::IsItemHovered())
				ImGui::SetTooltip("%sLife force now %.0f%%, long-term %.0f%%\nRepair rate %d, damage rate %d, injury to apply %d\nATP damage coefficient %d",
					(note && !note->comment.empty()) ? (note->comment + "\n\n").c_str() : "",
					st * 100, lt * 100, o->getRepairRate(), o->getDamageRate(), o->getInjuryToApply(), o->getATPDamageCoefficient());
		}
		ImGui::EndTable();
	}
}

// ---- dDNA ------------------------------------------------------------------------

static std::string geneSummary(gene* g) {
	if (auto* r = dynamic_cast<bioReactionGene*>(g)) {
		std::string s;
		auto side = [&](int a, int b) {
			std::string t;
			for (int k = a; k < b; k++) {
				if (r->reactant[k] == 0 || r->quantity[k] == 0)
					continue;
				if (!t.empty())
					t += " + ";
				t += fmt::format("{} {}", (int)r->quantity[k], chemName(r->reactant[k]));
			}
			return t.empty() ? std::string("nothing") : t;
		};
		return fmt::format("{} -> {}  (rate {})", side(0, 2), side(2, 4), (int)r->rate);
	}
	if (auto* r = dynamic_cast<bioReceptorGene*>(g))
		return fmt::format("{} at organ {} tissue {} locus {}  (threshold {}, gain {})", chemName(r->chemical), (int)r->organ, (int)r->tissue, (int)r->locus, (int)r->threshold, (int)r->gain);
	if (auto* e = dynamic_cast<bioEmitterGene*>(g))
		return fmt::format("{} from organ {} tissue {} locus {}  (threshold {}, rate {}, gain {})", chemName(e->chemical), (int)e->organ, (int)e->tissue, (int)e->locus, (int)e->threshold, (int)e->rate, (int)e->gain);
	if (auto* ic = dynamic_cast<bioInitialConcentrationGene*>(g))
		return fmt::format("{} {}", (int)ic->quantity, chemName(ic->chemical));
	if (auto* a = dynamic_cast<creatureAppearanceGene*>(g))
		return fmt::format("body part {} breed {}  (species {})", (int)a->part, breedSlotName(geneBreedSlot(a->species, a->variant)), geneBreedSpecies(a->species));
	if (auto* s = dynamic_cast<creatureStimulusGene*>(g))
		return fmt::format("stimulus {}  (significance {}, intensity {})", (int)s->stim, (int)s->significance, (int)s->intensity);
	if (auto* in = dynamic_cast<creatureInstinctGene*>(g))
		return fmt::format("action {}, drive {}, level {}", (int)in->action, (int)in->drive, (int)in->level);
	if (auto* o = dynamic_cast<organGene*>(g))
		return fmt::format("clock rate {}, life force {}, damage rate {}", (int)o->clockrate, (int)o->lifeforce, (int)o->damagerate);
	return {};
}

static void DnaPage() {
	if (!KitNeedsCreature())
		return;
	c2Creature* c = KitCreature();
	auto genome = c->getGenome();
	if (!genome) {
		ImGui::TextDisabled("No genome.");
		return;
	}
	// grouped by type, then kind, in genome order
	std::map<std::string, std::map<std::string, std::vector<std::pair<int, gene*>>>> groups;
	for (size_t i = 0; i < genome->genes.size(); i++) {
		gene* g = genome->genes[i].get();
		groups[g->typeName()][g->name()].push_back({(int)i, g});
	}
	ImGui::Text("%d genes.", (int)genome->genes.size());
	ImGui::BeginChild("genes", ImVec2(520, 320), ImGuiChildFlags_Borders);
	for (auto& [type, kinds] : groups) {
		int n = 0;
		for (auto& k : kinds)
			n += (int)k.second.size();
		// a creature with its own mind (--mind) never reads its brain lobe
		// genes: they are kept and passed on, but do nothing
		bool inert = type == "Brain" && c->getMind();
		bool open = ImGui::TreeNode(type.c_str(), "%s (%d)%s", type.c_str(), n, inert ? ": inert" : "");
		if (inert && ImGui::IsItemHovered())
			ImGui::SetTooltip("Inert: this creature's mind does not use its brain genes.\nThey are still passed on to its young, and would matter to a creature\nrunning the original brain.");
		if (!open)
			continue;
		if (inert)
			ImGui::TextDisabled("Inert: this creature's mind does not use these genes.");
		for (auto& [kind, list] : kinds) {
			if (!ImGui::TreeNode(kind.c_str(), "%s (%d)", kind.c_str(), (int)list.size()))
				continue;
			for (auto& [idx, g] : list) {
				std::string flags;
				if (g->header.flags.maleonly)
					flags += " male-only";
				if (g->header.flags.femaleonly)
					flags += " female-only";
				if (!g->header.flags._mutable)
					flags += " fixed";
				if (g->header.flags.notexpressed)
					flags += " silent";
				const KitGeneNote* note = KitNoteFor(g);
				std::string summary = geneSummary(g);
				if (note && !note->caption.empty())
					ImGui::BulletText("#%d  %s%s%s", idx, note->caption.c_str(), summary.empty() ? "" : ":  ", summary.c_str());
				else
					ImGui::BulletText("#%d  %s", idx, summary.c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					ImGui::Text("Gene %d: %s %s", idx, type.c_str(), kind.c_str());
					ImGui::Text("Switches on: %s", fmt::format("{}", g->header.switchontime).c_str());
					ImGui::Text("Generation %d, mutation weight %d%s", (int)g->header.generation, (int)g->header.mutweighting, flags.c_str());
					if (note && !note->comment.empty()) {
						ImGui::Separator();
						ImGui::PushTextWrapPos(380);
						ImGui::TextUnformatted(note->comment.c_str());
						ImGui::PopTextWrapPos();
					}
					ImGui::EndTooltip();
				}
			}
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
	ImGui::EndChild();
}

// ---- Biochemistry ------------------------------------------------------------

static constexpr int HISTORY = 240; // samples, one every 5 world ticks (2 minutes)
static constexpr int MAX_SHOWN = 6;
static const ImU32 s_lineColors[MAX_SHOWN] = {
	IM_COL32(255, 90, 90, 255), IM_COL32(90, 220, 90, 255), IM_COL32(110, 160, 255, 255),
	IM_COL32(255, 220, 70, 255), IM_COL32(230, 110, 255, 255), IM_COL32(90, 230, 230, 255)};

struct ChemHistory {
	Agent* creature = nullptr;
	std::vector<std::array<uint8_t, 256>> samples;
	size_t next = 0;
	unsigned int lastTick = 0;
};

static ChemHistory s_history;

// keeps sampling while the Science Kit is drawn (open, on any page)
static void sampleChemistry() {
	Agent* a = KitCreatureAgent();
	c2Creature* c = KitCreature();
	if (!c) {
		s_history = {};
		return;
	}
	if (a != s_history.creature) {
		s_history = {};
		s_history.creature = a;
	}
	if (world.tickcount - s_history.lastTick < 5 && !s_history.samples.empty())
		return;
	s_history.lastTick = world.tickcount;
	std::array<uint8_t, 256> s;
	for (int i = 0; i < 256; i++)
		s[i] = c->getChemical(i);
	if (s_history.samples.size() < HISTORY)
		s_history.samples.push_back(s);
	else
		s_history.samples[s_history.next] = s;
	s_history.next = (s_history.next + 1) % HISTORY;
}

static void BiochemistryPage() {
	if (!KitNeedsCreature())
		return;
	c2Creature* c = KitCreature();
	static std::vector<int> shown = {99, 72, 172}; // ATP, glycogen, adrenaline
	static int group = 0;

	// the graph
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 size(480, 170);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + size, IM_COL32(8, 14, 30, 255));
	for (int k = 1; k < 4; k++) {
		float y = p.y + size.y * k / 4;
		dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + size.x, y), IM_COL32(40, 60, 90, 255));
	}
	size_t n = s_history.samples.size();
	for (size_t s = 0; s < shown.size(); s++) {
		int chem = shown[s];
		std::vector<ImVec2> pts;
		for (size_t k = 0; k < n; k++) {
			size_t idx = (n < HISTORY) ? k : (s_history.next + k) % HISTORY;
			float x = p.x + size.x * (float)(HISTORY - n + k) / (HISTORY - 1);
			float y = p.y + size.y - 2 - (size.y - 4) * s_history.samples[idx][chem] / 255.0f;
			pts.push_back(ImVec2(x, y));
		}
		if (pts.size() > 1)
			dl->AddPolyline(pts.data(), (int)pts.size(), s_lineColors[s], 0, 1.8f);
	}
	dl->AddRect(p, p + size, IM_COL32(90, 110, 150, 255));
	dl->AddText(p + ImVec2(4, 2), IM_COL32(150, 170, 200, 255), "255");
	dl->AddText(p + ImVec2(4, size.y - 16), IM_COL32(150, 170, 200, 255), "0");
	dl->AddText(p + ImVec2(size.x - 92, size.y - 16), IM_COL32(150, 170, 200, 255), "last 2 minutes");
	ImGui::Dummy(size);

	// the legend: click to take one off
	for (size_t s = 0; s < shown.size(); s++) {
		if (s % 3)
			ImGui::SameLine(16 + 160 * (float)(s % 3));
		ImVec4 col = ImGui::ColorConvertU32ToFloat4(s_lineColors[s]);
		std::string label = fmt::format("{} {}###shown{}", chemName(shown[s]), (int)c->getChemical(shown[s]), s);
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		bool remove = ImGui::Selectable(label.c_str(), false, 0, ImVec2(150, 0));
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Click to stop showing it");
		if (remove) {
			shown.erase(shown.begin() + s);
			break;
		}
	}

	// choosing: group, then chemical
	const auto& groups = KitChemicalGroups();
	ImGui::Separator();
	ImGui::SetNextItemWidth(160);
	if (ImGui::BeginCombo("##group", groups.empty() ? "" : groups[std::min<size_t>(group, groups.size() - 1)].first.c_str())) {
		for (size_t g = 0; g < groups.size(); g++)
			if (ImGui::Selectable(groups[g].first.c_str(), (int)g == group))
				group = (int)g;
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::TextDisabled(shown.size() < MAX_SHOWN ? "Click a chemical to graph it (up to 6)" : "Six shown: click one above to make room");
	if (!groups.empty() && ImGui::BeginListBox("##chems", ImVec2(480, 120))) {
		for (int chem : groups[std::min<size_t>(group, groups.size() - 1)].second) {
			if (chem == 0)
				continue; // not a chemical: 0 stands for none in the genes
			bool on = std::find(shown.begin(), shown.end(), chem) != shown.end();
			std::string label = fmt::format("{:3d}  {}  ({})", chem, chemName(chem), (int)c->getChemical(chem));
			if (ImGui::Selectable(label.c_str(), on) && !on && shown.size() < MAX_SHOWN)
				shown.push_back(chem);
		}
		ImGui::EndListBox();
	}
}

// ---- Syringe ---------------------------------------------------------------------

struct Mixture {
	std::string name, description;
	ImU32 color;
	std::vector<std::pair<int, int>> chems; // chemical, amount
};

static const std::vector<Mixture>& mixtures() {
	static std::vector<Mixture> list;
	static bool loaded = false;
	if (loaded)
		return list;
	loaded = true;
	auto path = findMainDirectoryFile("Applet Data/ChemMixtures");
	if (path.empty())
		return list;
	for (auto& l : ReadVerticalBarSeparatedValuesFile(path)) {
		if (l.size() < 5)
			continue;
		try {
			Mixture m;
			m.name = l[0];
			m.description = l[1];
			m.color = IM_COL32(std::stoi(l[2]), std::stoi(l[3]), std::stoi(l[4]), 255);
			for (size_t k = 5; k + 1 < l.size(); k += 2)
				m.chems.push_back({std::stoi(l[k]), std::stoi(l[k + 1])});
			list.push_back(m);
		} catch (...) {
		}
	}
	return list;
}

static void SyringePage() {
	if (!KitNeedsCreature())
		return;
	c2Creature* c = KitCreature();
	static int mode = 0; // 0 a mixture, 1 one chemical
	static int mix = 0, group = 0, chem = 99, amount = 50;
	static std::string message;
	auto& mixes = mixtures();

	// the syringe, filled to the dose in the mixture's color
	ImVec2 p = ImGui::GetCursorScreenPos();
	float fill;
	ImU32 tint;
	if (mode == 0 && mix < (int)mixes.size()) {
		int total = 0;
		for (auto& [ch, amt] : mixes[mix].chems)
			total += amt;
		fill = std::min(1.0f, total / 1000.0f + 0.15f);
		tint = mixes[mix].color == IM_COL32(0, 0, 0, 255) ? IM_COL32(120, 120, 130, 255) : mixes[mix].color;
	} else {
		fill = amount / 255.0f;
		tint = IM_COL32(150, 210, 255, 255);
	}
	// Inject animates as in C2: the plunger presses down and the fluid goes
	// down with it, then, after a moment, the syringe draws up the next dose
	// (owner, 2026-11-09): pressing 0.9 s, empty 0.4 s, refilling 0.6 s
	static double pressed = -10;
	float since = (float)(ImGui::GetTime() - pressed);
	// developer aid: OPENC2E_TEST_SYRINGE=0.45 holds the animation at that time
	static const char* holdAt = devenv("OPENC2E_TEST_SYRINGE");
	if (holdAt)
		since = (float)atof(holdAt);
	auto ease = [](float t) { t = std::clamp(t, 0.0f, 1.0f); return t * t * (3 - 2 * t); };
	float shown = fill;
	if (since < 0.9f)
		shown = fill * (1 - ease(since / 0.9f));
	else if (since < 1.3f)
		shown = 0;
	else if (since < 1.9f)
		shown = fill * ease((since - 1.3f) / 0.6f);
	// our own syringe, drawn: barrel, fluid, plunger, needle
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float bx0 = 16, bx1 = 48, by0 = 44, by1 = 204; // the barrel's inside
	const float maxFluid = by1 - by0 - 10;
	float h = maxFluid * shown;
	// the plunger: its head resting on the fluid, the rod up through the flange
	float head = by1 - h - 8;
	dl->AddRectFilled(p + ImVec2(29, 0), p + ImVec2(35, head), IM_COL32(150, 156, 168, 255));
	dl->AddRectFilled(p + ImVec2(20, 0), p + ImVec2(44, 6), IM_COL32(170, 176, 188, 255), 2);
	// the barrel's glass
	dl->AddRectFilled(p + ImVec2(bx0, by0), p + ImVec2(bx1, by1), IM_COL32(40, 46, 60, 170), 4);
	if (h > 0)
		dl->AddRectFilled(p + ImVec2(bx0 + 2, by1 - h), p + ImVec2(bx1 - 2, by1 - 2), (tint & 0x00FFFFFF) | IM_COL32(0, 0, 0, 220), 3);
	dl->AddRectFilled(p + ImVec2(bx0 + 2, head), p + ImVec2(bx1 - 2, head + 8), IM_COL32(60, 62, 70, 255), 2);
	// a highlight down the glass, and the dose marks
	dl->AddRectFilled(p + ImVec2(bx0 + 4, by0 + 6), p + ImVec2(bx0 + 7, by1 - 6), IM_COL32(255, 255, 255, 40), 1);
	for (int k = 1; k < 10; k++) {
		float y = by1 - 4 - k * (maxFluid / 10);
		dl->AddLine(p + ImVec2(bx1 - (k % 5 ? 7 : 12), y), p + ImVec2(bx1 - 1, y), IM_COL32(200, 215, 235, 150));
	}
	dl->AddRect(p + ImVec2(bx0, by0), p + ImVec2(bx1, by1), IM_COL32(150, 170, 200, 255), 4, 0, 1.5f);
	// the finger flange, the hub and the needle
	dl->AddRectFilled(p + ImVec2(6, by0 - 6), p + ImVec2(58, by0), IM_COL32(150, 170, 200, 255), 2);
	dl->AddRectFilled(p + ImVec2(26, by1), p + ImVec2(38, by1 + 12), IM_COL32(150, 170, 200, 255), 2);
	dl->AddLine(p + ImVec2(32, by1 + 12), p + ImVec2(32, 252), IM_COL32(205, 212, 222, 255), 2);
	ImGui::Dummy(ImVec2(64, 256));
	ImGui::SameLine();

	ImGui::BeginGroup();
	ImGui::RadioButton("A mixture", &mode, 0);
	ImGui::SameLine();
	ImGui::RadioButton("One chemical", &mode, 1);
	if (mode == 0) {
		if (ImGui::BeginListBox("##mixes", ImVec2(330, 90))) {
			for (size_t i = 0; i < mixes.size(); i++)
				if (ImGui::Selectable(mixes[i].name.c_str(), (int)i == mix))
					mix = (int)i;
			ImGui::EndListBox();
		}
		if (mix < (int)mixes.size()) {
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 330);
			ImGui::TextDisabled("%s", mixes[mix].description.c_str());
			std::string what;
			for (auto& [ch, amt] : mixes[mix].chems)
				what += fmt::format("{}{} {}", what.empty() ? "" : ", ", amt, chemName(ch));
			ImGui::TextWrapped("%s", what.c_str());
			ImGui::PopTextWrapPos();
		}
	} else {
		const auto& groups = KitChemicalGroups();
		ImGui::SetNextItemWidth(160);
		if (!groups.empty() && ImGui::BeginCombo("##sgroup", groups[std::min<size_t>(group, groups.size() - 1)].first.c_str())) {
			for (size_t g = 0; g < groups.size(); g++)
				if (ImGui::Selectable(groups[g].first.c_str(), (int)g == group))
					group = (int)g;
			ImGui::EndCombo();
		}
		if (!groups.empty() && ImGui::BeginListBox("##schems", ImVec2(330, 120))) {
			for (int ch : groups[std::min<size_t>(group, groups.size() - 1)].second) {
				std::string label = fmt::format("{:3d}  {}  ({})", ch, chemName(ch), (int)c->getChemical(ch));
				if (ImGui::Selectable(label.c_str(), ch == chem))
					chem = ch;
			}
			ImGui::EndListBox();
		}
		ImGui::SetNextItemWidth(250);
		ImGui::SliderInt("Amount", &amount, 1, 255);
	}
	bool can = c->isAlive() || mode == 0; // the kick-start mixtures are for the dead
	if (!can)
		ImGui::BeginDisabled();
	if (ImGui::Button("Inject", ImVec2(120, 0)) && since >= 1.9f) {
		pressed = ImGui::GetTime();
		if (mode == 0 && mix < (int)mixes.size()) {
			for (auto& [ch, amt] : mixes[mix].chems)
				c->addChemical((unsigned char)ch, (unsigned char)std::clamp(amt, 0, 255));
			message = fmt::format("Injected {}.", mixes[mix].name);
			if (!c->isAlive()) {
				int energy = 0, atp = 0;
				for (auto& [ch, amt] : mixes[mix].chems) {
					if (ch == 98)
						energy += amt;
					if (ch == 99)
						atp += amt;
				}
				switch (c->kickstart(energy, atp)) {
					case c2Creature::revived:
						message += " Its heart is beating! Keep its strength up until it comes round.";
						break;
					case c2Creature::primed:
						message += " Its body is ready.";
						break;
					case c2Creature::stillDead:
						message += " Nothing happens.";
						break;
					case c2Creature::tooLate:
						message += " It is too late.";
						break;
					case c2Creature::notDead:
						break;
				}
			}
		} else if (mode == 1) {
			c->addChemical((unsigned char)chem, (unsigned char)amount);
			message = fmt::format("Injected {} {}.", amount, chemName(chem));
		}
	}
	if (!can)
		ImGui::EndDisabled();
	if (!message.empty())
		ImGui::TextUnformatted(message.c_str());
	// the newly dead can be brought back (c2Creature::kickstart)
	if (!c->isAlive()) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 330);
		// (the mixtures by their names in the player's language: the first
		// rich in Energy, the first rich in ATP)
		std::string fuel = "a mixture rich in Energy", spark = "one rich in ATP";
		for (auto it = mixes.rbegin(); it != mixes.rend(); ++it)
			for (auto& [ch, amt] : it->chems) {
				if (ch == 98 && amt >= 200)
					fuel = "the " + it->name;
				if (ch == 99 && amt >= 200)
					spark = "the " + it->name;
			}
		switch (c->kickstartState()) {
			case c2Creature::stillDead:
				ImGui::TextColored(ImVec4(1, 0.75f, 0.45f, 1), "It has only just died. While its body still lies there, %s and then %s may bring it back.", fuel.c_str(), spark.c_str());
				break;
			case c2Creature::primed:
				ImGui::TextColored(ImVec4(1, 0.75f, 0.45f, 1), "Its body has fuel again. Now %s, quickly.", spark.c_str());
				break;
			default:
				ImGui::TextDisabled("It cannot be brought back.");
		}
		ImGui::PopTextWrapPos();
	}
	ImGui::EndGroup();
}

void DrawScienceKit() {
	sampleChemistry();
	KitCreatureHeader();
	if (ImGui::BeginTabBar("pages")) {
		if (KitTab("Physical")) {
			PhysicalPage();
			ImGui::EndTabItem();
		}
		if (KitTab("dDNA")) {
			DnaPage();
			ImGui::EndTabItem();
		}
		if (KitTab("Biochemistry")) {
			BiochemistryPage();
			ImGui::EndTabItem();
		}
		if (KitTab("Syringe")) {
			SyringePage();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

} // namespace Openc2eImgui
