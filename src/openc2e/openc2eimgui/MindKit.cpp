// The Mind Kit (openc2e addition, owner 2026-11-11), in place of C2's
// Neuroscience Kit: our creatures have no neurons to show, so this shows
// what the mind is doing in its own terms, for the selected creature:
//   Feelings    its 17 needs around a dial, and how content it is
//   Choosing    the options it weighed at its last choice and why, and
//               how its last few choices turned out
//   Learned     what it expects doing each thing to each kind of thing does
//   Memory map  the world as it remembers it: where it has been, food,
//               lift rides, where it is
//   Mood        how content it has been over the last 15 minutes
//   Places      the rooms it remembers: what it saw and whom it met there,
//               how it felt there, and the ones it seeks out or keeps away from
// Things are named from the game's own word list (the player's copy).

#include "C2Kits.h"
#include "C2KitsInternal.h"
#include "Engine.h"
#include "Map.h"
#include "MetaRoom.h"
#include "World.h"
#include "common/backend/Backend.h"
#include "common/creaturesImage.h"
#include "creatures/CreatureMind.h"
#include "creatures/CreatureSpeech.h"
#include "creatures/oldCreature.h"
#include "fileformats/ImageUtils.h"
#include "historyManager.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <imgui.h>
#include <map>
#include <set>

namespace Openc2eImgui {

namespace {

const float PI = 3.14159265f;

// the needs, in the mind's order (C2's drives), and which kind each is
const char* const NEED_NAMES[17] = {"Pain", "Pleasure", "Hunger", "Cold", "Hot", "Tired", "Sleepy", "Lonely",
	"Crowded", "Fear", "Bored", "Anger", "Sex", "Injury", "Suffocate", "Thirst", "Stress"};
enum Kind { BODY, REST, COMPANY, MIND };
const Kind NEED_KIND[17] = {BODY, MIND, BODY, BODY, BODY, REST, REST, COMPANY, COMPANY, MIND, MIND, MIND, COMPANY, BODY, BODY, BODY, MIND};
const ImU32 KIND_COLOR[4] = {IM_COL32(232, 104, 74, 255), IM_COL32(96, 150, 230, 255), IM_COL32(226, 118, 190, 255), IM_COL32(160, 128, 230, 255)};
const char* const KIND_NAME[4] = {"body", "rest", "company", "mind"};

const ImU32 TEXT = IM_COL32(225, 228, 235, 255), DIM = IM_COL32(150, 156, 170, 255), TRACK = IM_COL32(44, 48, 60, 255);

// what each decision does, in plain words (from the world's creature
// scripts, events 16 + decision); `object`: done to a thing
const char* verb(int decision, bool object = true) {
	switch (decision) {
		case 0: return object ? "watch" : "stay still";
		case 1: return "push";
		case 2: return "pull";
		case 3: return "stop";
		case 4: return "go to";
		case 5: return "back away from";
		case 6: return "pick up";
		case 7: case 23: return "drop";
		case 8: case 24: return object ? "ask for help with" : "say what it needs";
		case 9: return "rest";
		case 10: case 26: return "walk west";
		case 11: case 27: return "walk east";
		case 12: return "eat";
		case 13: return "hit";
		case 16: return "fidget";
		case 25: return "sleep";
		default: return "do something";
	}
}

// a short word for a column heading
const char* shortVerb(int decision) {
	switch (decision) {
		case 0: return "watch";
		case 5: return "back off";
		case 6: return "pick up";
		case 8: case 24: return "ask";
		default: return verb(decision);
	}
}

// a category's noun from the game's word list (nouns are words 16-55)
std::string thing(int category) {
	// the mind's own kinds, past the game's 40 (they read as whatever words
	// follow the nouns in the list)
	if (category == MIND_HEALING_FRUIT)
		return "healing fruit";
	if (category == MIND_ROTTEN_FRUIT)
		return "rotten fruit";
	size_t w = 16 + (size_t)std::max(0, category);
	if (category >= 0 && w < engine.wordlist.size()) {
		std::string s = engine.wordlist[w];
		while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n'))
			s.pop_back();
		while (!s.empty() && s.front() == ' ')
			s.erase(s.begin());
		if (!s.empty()) {
			for (auto& ch : s)
				ch = (char)tolower((unsigned char)ch);
			return s;
		}
	}
	return fmt::format("thing {}", category);
}


// what a need is sought for, in a few words (a favourite place)
const char* needFor(const std::string& need) {
	if (need == "Cold")
		return "to warm up";
	if (need == "Hot")
		return "to cool down";
	if (need == "Tired")
		return "to rest";
	if (need == "Sleepy")
		return "to sleep";
	if (need == "Lonely")
		return "for company";
	return "for its needs";
}
const char* needForIndex(int need) {
	static const char* const names[17] = {"Pain", "Pleasure", "Hunger", "Cold", "Hot", "Tired", "Sleepy", "Lonely",
		"Crowded", "Fear", "Bored", "Anger", "Sex", "Injury", "Suffocate", "Thirst", "Stress"};
	return needFor(need >= 0 && need < 17 ? names[need] : "");
}

// a trip from the sense of place, in plain words
std::string nameOf(const std::string& moniker);
std::string placeTrip(const std::string& place) {
	if (place.rfind("mate:", 0) == 0)
		return "go where it last met " + nameOf(place.substr(5));
	if (place.rfind("favourite:", 0) == 0)
		return fmt::format("go to a favourite place {}", needFor(place.substr(10)));
	if (place.rfind("leave:", 0) == 0) {
		std::string need = place.substr(6);
		for (auto& ch : need)
			ch = (char)tolower((unsigned char)ch);
		return fmt::format("leave a room it dislikes ({})", need == "suffocate" ? "breathless" : need);
	}
	if (place == "explore")
		return "explore a room it has not been in";
	return place;
}
std::string nameOf(const std::string& moniker) {
	// following another ("toward <moniker>", the mind's own key)
	if (moniker.rfind("toward ", 0) == 0)
		return "towards " + nameOf(moniker.substr(7));
	if (world.history && world.history->hasMoniker(moniker)) {
		auto& m = world.history->getMoniker(moniker);
		if (!m.name.empty())
			return m.name;
	}
	return moniker;
}

std::string optionLabel(const MindView::Option& o) {
	if (!o.place.empty())
		return placeTrip(o.place);
	if (!o.who.empty())
		return fmt::format("{} {}", verb(o.decision), nameOf(o.who));
	if (o.category < 0)
		return verb(o.decision, false);
	return fmt::format("{} the {}", verb(o.decision), thing(o.category));
}

std::string secondsText(float s) {
	if (s < 90)
		return fmt::format("{:.0f} s", s);
	return fmt::format("{:.0f} min", s / 60);
}

// mind ticks to seconds (C1/C2: 2.5 a second)
float secondsOf(unsigned int ticks) {
	return ticks / 2.5f;
}

// text wrapped to a width, from the left
void wrappedText(const std::string& text, float width, ImU32 col) {
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
	ImGui::PopTextWrapPos();
}

void sector(ImDrawList* dl, ImVec2 c, float r0, float r1, float a0, float a1, ImU32 col) {
	const int steps = 8;
	for (int i = 0; i < steps; i++) {
		float t0 = a0 + (a1 - a0) * i / steps, t1 = a0 + (a1 - a0) * (i + 1) / steps;
		ImVec2 p0(c.x + r0 * std::cos(t0), c.y + r0 * std::sin(t0)), p1(c.x + r1 * std::cos(t0), c.y + r1 * std::sin(t0));
		ImVec2 p2(c.x + r1 * std::cos(t1), c.y + r1 * std::sin(t1)), p3(c.x + r0 * std::cos(t1), c.y + r0 * std::sin(t1));
		dl->AddQuadFilled(p0, p1, p2, p3, col);
	}
}

// Each need's pull on the mind: its level raised to the mind's power,
// times how much the need matters to this creature, as a share of the most
// any need could pull (the most-weighted need at its worst). A need that
// matters little stays short even when full; this is what drives choices,
// where the Health Kit's Drives show the body's levels as they are.
std::array<float, 17> pullsOf(const MindView& v, float* total = nullptr) {
	std::array<float, 17> p{};
	float most = 0.0001f, sum = 0;
	for (int i = 0; i < 17; i++)
		most = std::max(most, v.weight[i]);
	for (int i = 0; i < 17; i++) {
		p[i] = v.weight[i] * std::pow(std::clamp(v.drives[i], 0.0f, 1.0f), v.pullPower) / most;
		sum += p[i];
	}
	if (total)
		*total = sum;
	return p;
}

const char* mattersWord(float weight, float most) {
	float r = weight / std::max(0.0001f, most);
	return r > 0.75f ? "matters a great deal to it" : r > 0.4f ? "matters to it" : r > 0.2f ? "matters a little" : "hardly matters to it";
}

void FeelingsPage(const MindView& v) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	// the dial in the middle of the window, the words at the left
	const float W = 440, H = 370;
	wrappedText("How hard each need is pushing it now: its level times how much it matters to this creature.", W, DIM);
	ImGui::Dummy(ImVec2(W, 4));
	const float left = ImGui::GetCursorPosX();
	KitCenterNext(W);
	ImVec2 o = ImGui::GetCursorScreenPos();
	ImVec2 c(o.x + W / 2, o.y + 185);
	const float r0 = 52, r1 = 160;
	float total = 0;
	std::array<float, 17> pull = pullsOf(v, &total);
	float most = 0.0001f;
	for (float w : v.weight)
		most = std::max(most, w);
	for (int i = 0; i < 17; i++) {
		float a0 = -PI / 2 + i * 2 * PI / 17 + 0.02f, a1 = a0 + 2 * PI / 17 - 0.04f;
		sector(dl, c, r0, r1, a0, a1, TRACK);
		// (the need's own level, faintly, behind its pull)
		float lv = std::clamp(v.drives[i], 0.0f, 1.0f);
		if (lv > 0.005f)
			sector(dl, c, r0, r0 + (r1 - r0) * lv, a0, a1, (KIND_COLOR[NEED_KIND[i]] & 0x00FFFFFF) | 0x38000000);
		float pv = std::clamp(pull[i], 0.0f, 1.0f);
		if (pv > 0.003f)
			sector(dl, c, r0, r0 + (r1 - r0) * std::sqrt(pv), a0, a1, KIND_COLOR[NEED_KIND[i]]);
		float am = (a0 + a1) / 2;
		ImVec2 ts = ImGui::CalcTextSize(NEED_NAMES[i]);
		ImVec2 lp(c.x + (r1 + 20) * std::cos(am) - ts.x / 2, c.y + (r1 + 14) * std::sin(am) - ts.y / 2);
		dl->AddText(lp, pv > 0.15f ? TEXT : DIM, NEED_NAMES[i]);
	}
	dl->AddCircleFilled(c, r0 - 4, IM_COL32(30, 33, 41, 255), 40);
	std::string pct = fmt::format("{:.0f}%", v.content * 100);
	ImU32 pc = v.content > 0.66f ? IM_COL32(120, 210, 140, 255) : v.content > 0.4f ? IM_COL32(230, 200, 100, 255) : IM_COL32(232, 104, 74, 255);
	ImGui::SetWindowFontScale(1.5f);
	ImVec2 ps = ImGui::CalcTextSize(pct.c_str());
	dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(c.x - ps.x / 2, c.y - ps.y + 2), pc, pct.c_str());
	ImGui::SetWindowFontScale(1.0f);
	ImVec2 cs = ImGui::CalcTextSize("content");
	dl->AddText(ImVec2(c.x - cs.x / 2, c.y + 4), DIM, "content");
	ImVec2 m = ImGui::GetIO().MousePos;
	float dx = m.x - c.x, dy = m.y - c.y, dist = std::sqrt(dx * dx + dy * dy);
	if (dist <= r0)
		ImGui::SetTooltip("How content it is: how far its needs, as they push it, are from all at their worst");
	else if (dist < r1 + 30) {
		float a = std::atan2(dy, dx) + PI / 2;
		if (a < 0)
			a += 2 * PI;
		int i = std::clamp((int)(a / (2 * PI / 17)), 0, 16);
		ImGui::SetTooltip("%s: %.0f%% (%s); it %s\n%.0f%% of what is pushing it now",
			NEED_NAMES[i], v.drives[i] * 100, KIND_NAME[NEED_KIND[i]], mattersWord(v.weight[i], most), total > 0 ? pull[i] / total * 100 : 0.0f);
	}
	ImGui::Dummy(ImVec2(W, H));
	ImGui::SetCursorPosX(left);
	// the key, at the left
	ImVec2 kp = ImGui::GetCursorScreenPos() + ImVec2(0, 2);
	for (int k = 0; k < 4; k++) {
		dl->AddRectFilled(kp, kp + ImVec2(12, 12), KIND_COLOR[k]);
		dl->AddText(kp + ImVec2(18, -2), DIM, KIND_NAME[k]);
		kp.x += 18 + ImGui::CalcTextSize(KIND_NAME[k]).x + 22;
	}
	ImGui::Dummy(ImVec2(W, 20));
	std::vector<std::pair<float, int>> order;
	for (int i = 0; i < 17; i++)
		order.push_back({pull[i], i});
	std::sort(order.rbegin(), order.rend());
	std::string strongest = "Pushing it most:";
	for (int k = 0; k < 3 && order[k].first > 0.01f && total > 0; k++)
		strongest += fmt::format("{} {} ({:.0f}%)", k ? "," : "", NEED_NAMES[order[k].second], order[k].first / total * 100);
	if (order[0].first <= 0.01f)
		strongest = "Nothing is pushing it: all is well.";
	wrappedText(strongest, W, TEXT);
	ImGui::Dummy(ImVec2(W, 2));
	wrappedText("Faint behind each: the need's level. The body's levels are in the Health Kit's Drives.", W, DIM);
}

std::string outcomeLabel(const MindView::Outcome& oc) {
	if (oc.travel)
		return fmt::format("go to find a {}", thing(oc.category));
	MindView::Option o;
	o.category = oc.category;
	o.decision = oc.decision;
	o.who = oc.who;
	return optionLabel(o);
}

std::string outcomeWhy(const MindView::Outcome& oc) {
	if (oc.why == "finished")
		return "finished";
	if (oc.why == "time")
		return "ran its time";
	if (oc.why == "stalled")
		return "got nowhere";
	if (oc.why == "object gone")
		return "the thing went away";
	return "stopped: " + oc.why + " grew too strong";
}

// How its last few choices turned out (newest first): what it did, when and
// for how long, why it stopped, and a bar for how much its needs eased
// (green, to the right) or grew (red, to the left) on the whole, with the
// needs that changed most. This is what it learns from.
void OutcomesList(const MindView& v, float barX, float barW) {
	ImGui::Spacing();
	KitHeading("How its last choices turned out");
	if (v.outcomes.empty()) {
		KitNote("Nothing has finished yet.");
		return;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 o = ImGui::GetCursorScreenPos();
	const float rowH = 40, half = barW / 2, full = 0.25f; // reward that fills half the bar
	float mid = o.x + barX + half;
	size_t n = v.outcomes.size();
	for (size_t k = 0; k < n; k++) {
		const auto& oc = v.outcomes[n - 1 - k];
		float y = o.y + k * rowH + 4;
		dl->AddText(ImVec2(o.x + 6, y), TEXT, outcomeLabel(oc).c_str());
		std::string when = fmt::format("{} ago, for {}; {}", secondsText(secondsOf(v.tick > oc.tick ? v.tick - oc.tick : 0)),
			secondsText(secondsOf(oc.ticks)), outcomeWhy(oc));
		dl->AddText(ImVec2(o.x + 26, y + 17), DIM, when.c_str());
		// the bar from the middle line
		dl->AddRectFilled(ImVec2(mid - half, y + 3), ImVec2(mid + half, y + 15), TRACK);
		float w = std::clamp(oc.reward / full, -1.0f, 1.0f) * half;
		if (std::fabs(w) >= 1.0f)
			dl->AddRectFilled(ImVec2(std::min(mid, mid + w), y + 3), ImVec2(std::max(mid, mid + w), y + 15),
				w > 0 ? IM_COL32(88, 196, 104, 255) : IM_COL32(220, 84, 70, 255));
		dl->AddLine(ImVec2(mid, y), ImVec2(mid, y + 18), IM_COL32(110, 116, 130, 255));
		// the needs that changed most
		std::array<int, 17> idx;
		for (int i = 0; i < 17; i++)
			idx[i] = i;
		std::sort(idx.begin(), idx.end(), [&](int a, int b) { return std::fabs(oc.change[a]) > std::fabs(oc.change[b]); });
		std::string most;
		for (int k2 = 0; k2 < 2; k2++) {
			float c = oc.change[idx[k2]];
			if (std::fabs(c) < 0.02f)
				break;
			most += fmt::format("{}{} {}", most.empty() ? "" : ", ", NEED_NAMES[idx[k2]], c < 0 ? "eased" : "grew");
		}
		if (most.empty())
			most = std::fabs(oc.reward) < 0.005f ? "no change" : oc.reward > 0 ? "a little better" : "a little worse";
		dl->AddText(ImVec2(mid - half, y + 17), DIM, most.c_str());
		if (ImGui::IsMouseHoveringRect(ImVec2(o.x, y), ImVec2(mid + half + 40, y + rowH - 4))) {
			std::string tip = outcomeLabel(oc) + fmt::format("\n  overall: {:+.3f} ({})\n", oc.reward, oc.reward >= 0 ? "its needs eased" : "its needs grew");
			for (int i = 0; i < 17; i++)
				if (std::fabs(oc.change[i]) >= 0.005f)
					tip += fmt::format("  {}: {:+.2f}\n", NEED_NAMES[i], oc.change[i]);
			tip.pop_back();
			ImGui::SetTooltip("%s", tip.c_str());
		}
	}
	float ey = o.y + n * rowH + 6;
	dl->AddText(ImVec2(mid - half, ey), DIM, "worse");
	ImVec2 bt = ImGui::CalcTextSize("better");
	dl->AddText(ImVec2(mid + half - bt.x, ey), DIM, "better");
	ImGui::Dummy(ImVec2(barX + barW + 80, n * rowH + 24));
}

void ChoosingPage(const MindView& v) {
	if (v.options.empty()) {
		ImGui::TextWrapped("It has not chosen anything yet.");
		return;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 o = ImGui::GetCursorScreenPos();
	KitNoteColored(ImVec4(0.6f, 0.62f, 0.68f, 1), "What it weighed at its last choice (%s ago), and how much each should ease its needs:",
		secondsText(secondsOf(v.tick - v.choseAt)).c_str());
	o = ImGui::GetCursorScreenPos();
	float most = 0.01f;
	for (auto& op : v.options)
		most = std::max(most, std::max(0.0f, op.want) + std::max(0.0f, op.novelty));
	const float barX = 360, barW = 260, rowH = 54;
	for (size_t k = 0; k < v.options.size(); k++) {
		const auto& op = v.options[k];
		float y = o.y + k * rowH + 6;
		std::string label = (op.chosen ? "> " : "  ") + optionLabel(op);
		dl->AddText(ImVec2(o.x + 6, y), op.chosen ? IM_COL32(255, 220, 120, 255) : TEXT, label.c_str());
		// why, in a few words
		std::string why;
		int top = -1;
		float topv = 0;
		for (int i = 0; i < 17; i++)
			if (op.ease[i] > topv)
				topv = op.ease[i], top = i;
		if (op.escape)
			why = "to get out of the water";
		else if (op.food)
			why = "heading for food it remembers";
		else if (op.forage)
			why = "looking for food";
		else if (op.scent)
			why = "following a scent";
		else if (op.route)
			why = "a step on its way somewhere";
		else if (op.travel)
			why = "going to where it last saw one";
		else if (op.tried == 0)
			why = "never tried: curious what it does";
		else if (top >= 0)
			why = fmt::format("has eased {} before ({} time{})", NEED_NAMES[top], op.tried, op.tried == 1 ? "" : "s");
		else
			why = fmt::format("tried {} time{}; little to gain", op.tried, op.tried == 1 ? "" : "s");
		dl->AddText(ImVec2(o.x + 26, y + 18), DIM, why.c_str());
		// the bar: the ease split by kind of need, then curiosity in gray
		float bx = o.x + barX;
		float kinds[4] = {0, 0, 0, 0};
		for (int i = 0; i < 17; i++)
			kinds[NEED_KIND[i]] += op.ease[i];
		for (int kk = 0; kk < 4; kk++) {
			float w = kinds[kk] / most * barW;
			if (w > 0.5f) {
				dl->AddRectFilled(ImVec2(bx, y + 2), ImVec2(bx + w, y + 18), KIND_COLOR[kk]);
				bx += w;
			}
		}
		float nw = std::max(0.0f, op.novelty) / most * barW;
		if (nw > 0.5f) {
			dl->AddRectFilled(ImVec2(bx, y + 2), ImVec2(bx + nw, y + 18), IM_COL32(120, 124, 136, 255));
			bx += nw;
		}
		dl->AddText(ImVec2(bx + 6, y + 2), DIM, fmt::format("{:.2f}", std::max(0.0f, op.want) + std::max(0.0f, op.novelty)).c_str());
		if (ImGui::IsMouseHoveringRect(ImVec2(o.x, y), ImVec2(o.x + barX + barW + 60, y + rowH - 4))) {
			std::string tip = optionLabel(op) + "\n";
			for (int i = 0; i < 17; i++)
				if (op.ease[i] > 0.005f)
					tip += fmt::format("  eases {}: {:.2f}\n", NEED_NAMES[i], op.ease[i]);
			if (op.novelty > 0.005f)
				tip += fmt::format("  curiosity: {:.2f}\n", op.novelty);
			tip += fmt::format("  done {} time{} to this kind of thing", op.tried, op.tried == 1 ? "" : "s");
			ImGui::SetTooltip("%s", tip.c_str());
		}
	}
	float ey = o.y + v.options.size() * rowH + 8;
	dl->AddLine(ImVec2(o.x + barX, o.y), ImVec2(o.x + barX, ey - 6), IM_COL32(60, 66, 80, 255));
	for (int kk = 0; kk < 4; kk++) {
		ImVec2 kp(o.x + barX + kk * 66, ey);
		dl->AddRectFilled(kp, kp + ImVec2(10, 10), KIND_COLOR[kk]);
		dl->AddText(kp + ImVec2(14, -3), DIM, KIND_NAME[kk]);
	}
	dl->AddRectFilled(ImVec2(o.x + barX + 264, ey), ImVec2(o.x + barX + 274, ey + 10), IM_COL32(120, 124, 136, 255));
	dl->AddText(ImVec2(o.x + barX + 278, ey - 3), DIM, "curiosity");
	ImGui::Dummy(ImVec2(barX + barW + 80, v.options.size() * rowH + 26));
	ImGui::Text("Doing: %s%s", optionLabel(v.options[0]).c_str(), v.busy ? "" : " (done; about to choose again)");
	OutcomesList(v, barX, barW);
}

void LearnedPage(const MindView& v) {
	if (v.lessons.empty()) {
		ImGui::TextWrapped("It has not learned anything from experience yet.");
		return;
	}
	// rows: kinds of things (most experienced first), then individuals;
	// columns: the actions it has learned about
	std::map<std::string, int> rowCount;
	std::map<std::string, std::pair<int, std::string>> rowKey; // label -> (category, who)
	std::vector<std::string> cols; // headings: decisions with the same word share one
	for (auto& l : v.lessons) {
		if (l.decision == 10 || l.decision == 11 || l.decision == 26 || l.decision == 27 || l.decision == 0)
			continue; // walking and staying still: not about the thing
		std::string label = l.who.empty() ? (l.category < 0 ? std::string("(by itself)") : thing(l.category)) : nameOf(l.who);
		rowCount[label] += l.count;
		rowKey[label] = {l.category, l.who};
		if (std::find(cols.begin(), cols.end(), shortVerb(l.decision)) == cols.end())
			cols.push_back(shortVerb(l.decision));
	}
	std::vector<std::pair<int, std::string>> rows;
	for (auto& [label, n] : rowCount)
		rows.push_back({-n - (rowKey[label].second.empty() ? 1000000 : 0), label});
	std::sort(rows.begin(), rows.end());
	if (rows.size() > 13)
		rows.resize(13);
	// Needs rise on their own while it does anything, so nearly everything
	// raises some need: each lesson is judged against its usual result
	// (the median), green better, red worse, scaled to the widest spread
	std::vector<float> values;
	for (auto& l : v.lessons)
		values.push_back(l.value);
	std::sort(values.begin(), values.end());
	float usual = values[values.size() / 2], spread = 0.001f;
	for (float x : values)
		spread = std::max(spread, std::fabs(x - usual));
	// in the decisions' order
	std::sort(cols.begin(), cols.end(), [](const std::string& a, const std::string& b) {
		auto order = [](const std::string& w) { for (int d = 0; d < 64; d++) if (w == shortVerb(d)) return d; return 99; };
		return order(a) < order(b);
	});
	KitNoteColored(ImVec4(0.6f, 0.62f, 0.68f, 1), "What it expects from each, against its usual result: green better, red worse. Dot size: how often.");
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 o = ImGui::GetCursorScreenPos();
	const float labelW = 120, cw = 58, ch = 26;
	for (size_t j = 0; j < cols.size(); j++) {
		const char* t = cols[j].c_str();
		ImVec2 ts = ImGui::CalcTextSize(t);
		dl->AddText(ImVec2(o.x + labelW + j * cw + cw / 2 - ts.x / 2, o.y), DIM, t);
	}
	float y0 = o.y + 20;
	for (size_t i = 0; i < rows.size(); i++) {
		const std::string& label = rows[i].second;
		auto [cat, who] = rowKey[label];
		dl->AddText(ImVec2(o.x + 4, y0 + i * ch + 5), who.empty() ? TEXT : IM_COL32(226, 170, 210, 255), label.c_str());
		for (size_t j = 0; j < cols.size(); j++) {
			ImVec2 a(o.x + labelW + j * cw + 2, y0 + i * ch + 2), b(a.x + cw - 4, a.y + ch - 4);
			dl->AddRectFilled(a, b, IM_COL32(40, 44, 55, 255));
			const MindView::Lesson* l = nullptr;
			for (auto& x : v.lessons)
				if (cols[j] == shortVerb(x.decision) && x.who == who && (who.empty() ? x.category == cat : true) && (!l || x.count > l->count))
					l = &x;
			if (!l)
				continue;
			float val = std::clamp((l->value - usual) / spread, -1.0f, 1.0f);
			ImU32 col = val >= 0 ? IM_COL32((int)(70 + 60 * (1 - val)), (int)(150 + 90 * val), 110, 255) : IM_COL32((int)(190 - 60 * val), 80, 80, 255);
			float r = 4 + std::min(9.0f, l->count * 0.7f);
			ImVec2 cc((a.x + b.x) / 2, (a.y + b.y) / 2);
			dl->AddCircleFilled(cc, r, col, 20);
			if (ImGui::IsMouseHoveringRect(a, b)) {
				std::string tip = fmt::format("{} {}: done {} time{}\n", verb(l->decision), label, l->count, l->count == 1 ? "" : "s");
				std::vector<std::pair<float, int>> ch2;
				for (int k = 0; k < 17; k++)
					if (std::fabs(l->delta[k]) > 0.01f)
						ch2.push_back({std::fabs(l->delta[k]), k});
				std::sort(ch2.rbegin(), ch2.rend());
				for (size_t k = 0; k < ch2.size() && k < 5; k++) {
					int d = ch2[k].second;
					tip += fmt::format("  {} {} by {:.0f}%\n", l->delta[d] < 0 ? "eases" : "raises", NEED_NAMES[d], std::fabs(l->delta[d]) * 100);
				}
				if (ch2.empty())
					tip += "  changes nothing it can tell";
				ImGui::SetTooltip("%s", tip.c_str());
			}
		}
	}
	ImGui::Dummy(ImVec2(labelW + cols.size() * cw, 24 + rows.size() * ch));
	bool anyone = false;
	for (auto& r : rows)
		anyone |= !rowKey[r.second].second.empty();
	if (anyone)
		ImGui::TextDisabled("Names in pink are creatures it knows one by one.");
}

void MapPage(const MindView& v) {
	MetaRoom* m = world.map ? world.map->metaRoomAt(v.x, v.y) : nullptr;
	int w = 0, h = 0;
	Texture pic = KitMapPicture(m, w, h);
	if (!m || !pic) {
		ImGui::TextWrapped("No map to show here.");
		return;
	}
	ImGui::TextColored(ImVec4(0.6f, 0.62f, 0.68f, 1), "The world as it remembers it:");
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 o = ImGui::GetCursorScreenPos();
	ImGui::Image(pic.as<ImTextureID>(), ImVec2((float)w, (float)h));
	float sx = (float)w / m->width(), sy = (float)h / m->height();
	auto at = [&](float x, float y) { return ImVec2(o.x + (x - m->x()) * sx, o.y + (y - m->y()) * sy); };
	// where it has been: brighter, more recently
	for (auto& s : v.visited) {
		float age = s.last ? secondsOf(v.tick - s.last) / 600.0f : 1.0f; // fades over 10 minutes
		age = std::clamp(age, 0.0f, 1.0f);
		ImU32 col = IM_COL32((int)(60 + 150 * (1 - age)), (int)(90 + 140 * (1 - age)), (int)(150 + 100 * (1 - age)), 200);
		ImVec2 a = at(s.x, s.y), b = at(s.x + s.w, s.y + s.h);
		ImVec2 c((a.x + b.x) / 2, (a.y + b.y) / 2);
		dl->AddRectFilled(c - ImVec2(2, 2), c + ImVec2(2, 2), col);
	}
	for (auto& r : v.rides)
		dl->AddLine(at(r.x, r.fromY), at(r.x, r.toY), IM_COL32(255, 200, 90, 255), 2.0f);
	ImVec2 mouse = ImGui::GetIO().MousePos;
	std::string tip;
	for (auto& f : v.food) {
		ImVec2 c = at(f.x, f.y);
		float r = 3 + 4 * std::clamp(f.confidence, 0.0f, 1.0f);
		dl->AddCircleFilled(c, r, IM_COL32(120, 220, 110, 255), 16);
		dl->AddCircle(c, r, IM_COL32(20, 60, 20, 255), 16);
		float dx = mouse.x - c.x, dy = mouse.y - c.y;
		if (dx * dx + dy * dy < 49)
			tip = fmt::format("Food here: last seen or eaten {} ago, {} meal{}, {} sure", secondsText(secondsOf(v.tick - f.last)),
				f.meals, f.meals == 1 ? "" : "s", f.confidence > 0.7f ? "quite" : f.confidence > 0.4f ? "fairly" : "not very");
	}
	for (auto& r : v.rides) {
		ImVec2 a = at(r.x, std::min(r.fromY, r.toY)), b = at(r.x, std::max(r.fromY, r.toY));
		if (mouse.x > a.x - 4 && mouse.x < a.x + 4 && mouse.y > a.y && mouse.y < b.y)
			tip = fmt::format("A ride it knows: {} {} time{}", r.toY < r.fromY ? "up" : "down", r.uses, r.uses == 1 ? "" : "s");
	}
	for (auto& pm : v.places) {
		if (pm.visits == 0 || (pm.goodFor.empty() && !pm.avoided))
			continue;
		ImVec2 c = at(pm.x, pm.floor - 20);
		if (pm.avoided) {
			dl->AddLine(c - ImVec2(4, 4), c + ImVec2(4, 4), IM_COL32(235, 90, 80, 255), 2.0f);
			dl->AddLine(c + ImVec2(-4, 4), c + ImVec2(4, -4), IM_COL32(235, 90, 80, 255), 2.0f);
		} else {
			dl->AddCircleFilled(c, 4, IM_COL32(250, 215, 100, 255), 12);
			dl->AddCircle(c, 4, IM_COL32(90, 70, 20, 255), 12);
		}
		float mdx = mouse.x - c.x, mdy = mouse.y - c.y;
		if (mdx * mdx + mdy * mdy < 36) {
			std::string good;
			for (int need : pm.goodFor)
				good += (good.empty() ? "" : ", ") + std::string(needForIndex(need));
			tip = pm.avoided ? fmt::format("Room {}: a room it keeps away from", pm.room) : fmt::format("Room {}: a favourite place {}", pm.room, good);
		}
	}
	ImVec2 here = at(v.x, v.y);
	dl->AddTriangleFilled(here + ImVec2(0, -9), here + ImVec2(-6, 3), here + ImVec2(6, 3), IM_COL32(255, 255, 255, 255));
	if (!tip.empty())
		ImGui::SetTooltip("%s", tip.c_str());
	if (ImGui::IsMouseHoveringRect(here - ImVec2(8, 10), here + ImVec2(8, 5)))
		ImGui::SetTooltip("Where it is now");
	// key, each mark drawn as it is on the map
	ImVec2 k = ImGui::GetCursorScreenPos() + ImVec2(4, 4);
	const char* labels[6] = {"been there (brighter: lately)", "food it remembers", "lift rides it knows", "where it is now", "a favourite place", "a room it keeps away from"};
	for (int i = 0; i < 6; i++) {
		ImVec2 p = k + ImVec2((i % 2) * 300.0f, (i / 2) * 18.0f);
		ImVec2 c = p + ImVec2(5, 5);
		switch (i) {
			case 0: // dim to bright, as the dots fade
				dl->AddRectFilled(c - ImVec2(5, 2), c - ImVec2(1, -2), IM_COL32(60, 90, 150, 255));
				dl->AddRectFilled(c + ImVec2(1, -2), c + ImVec2(5, 2), IM_COL32(210, 230, 250, 255));
				break;
			case 1:
				dl->AddCircleFilled(c, 5, IM_COL32(120, 220, 110, 255), 16);
				dl->AddCircle(c, 5, IM_COL32(20, 60, 20, 255), 16);
				break;
			case 2:
				dl->AddLine(c - ImVec2(0, 6), c + ImVec2(0, 6), IM_COL32(255, 200, 90, 255), 2.0f);
				break;
			case 3:
				dl->AddTriangleFilled(c + ImVec2(0, -7), c + ImVec2(-5, 4), c + ImVec2(5, 4), IM_COL32(255, 255, 255, 255));
				break;
			case 4:
				dl->AddCircleFilled(c, 4, IM_COL32(250, 215, 100, 255), 12);
				dl->AddCircle(c, 4, IM_COL32(90, 70, 20, 255), 12);
				break;
			case 5:
				dl->AddLine(c - ImVec2(4, 4), c + ImVec2(4, 4), IM_COL32(235, 90, 80, 255), 2.0f);
				dl->AddLine(c + ImVec2(-4, 4), c + ImVec2(4, -4), IM_COL32(235, 90, 80, 255), 2.0f);
				break;
		}
		dl->AddText(p + ImVec2(16, -3), DIM, labels[i]);
	}
	ImGui::Dummy(ImVec2((float)w, 60));
	ImGui::TextDisabled("%zu places, %zu food places, %zu rides remembered", v.visited.size(), v.food.size(), v.rides.size());
}


void PlacesPage(const MindView& v) {
	std::vector<const MindView::PlaceMemory*> been;
	int heardOf = 0;
	for (auto& pm : v.places) {
		if (pm.visits > 0)
			been.push_back(&pm);
		else
			heardOf++;
	}
	if (been.empty()) {
		ImGui::TextWrapped("It does not remember any rooms yet.");
		return;
	}
	std::sort(been.begin(), been.end(), [](auto* a, auto* b) { return a->here != b->here ? a->here : a->ticks > b->ticks; });
	// the page's own width, its text wrapped to it (owner, 2026-11-21: the
	// list of who it longs for, on one line, made the kit as wide as the
	// screen)
	const float pageW = 660;
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + pageW);
	ImGui::TextColored(ImVec4(0.6f, 0.62f, 0.68f, 1), "The rooms it remembers, where it spent most time first. How it felt there is against how it feels anywhere.");
	if (!v.longing.empty()) {
		auto longing = v.longing;
		std::sort(longing.begin(), longing.end(), [](auto& a, auto& b) { return a.second > b.second; });
		// the strongest by name, the rest counted (all of them on hover)
		const size_t shown = 8;
		std::string who, all;
		for (size_t k = 0; k < longing.size(); k++) {
			auto& [m, L] = longing[k];
			std::string one = fmt::format("{} ({})", nameOf(m), L > 0.6f ? "strongly" : L > 0.15f ? "enough to look" : "a little");
			if (k < shown)
				who += (who.empty() ? "" : ", ") + one;
			all += (all.empty() ? "" : ", ") + one;
		}
		if (longing.size() > shown)
			who += fmt::format(", and {} more", longing.size() - shown);
		ImGui::TextColored(ImVec4(0.89f, 0.67f, 0.82f, 1), "Longs to find again: %s", who.c_str());
		if (longing.size() > shown && ImGui::BeginItemTooltip()) {
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 45);
			ImGui::TextUnformatted(all.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}
	ImGui::PopTextWrapPos();
	ImGui::BeginChild("places", ImVec2(pageW, 380), false);
	const ImVec4 green(0.55f, 0.85f, 0.55f, 1), red(0.92f, 0.5f, 0.45f, 1), pink(0.89f, 0.67f, 0.82f, 1);
	for (auto* pm : been) {
		float dx = pm->x - v.x, dy = pm->floor - v.y;
		std::string where = pm->here ? "here"
							: std::fabs(dx) < 400 ? "near by"
							: fmt::format("{} {}", std::fabs(dx) > 2500 ? "far" : "a way", dx < 0 ? "west" : "east");
		if (!pm->here && std::fabs(dy) >= 150)
			where += dy < 0 ? ", higher up" : ", lower down";
		ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.55f, 1), "Room %d", pm->room);
		ImGui::SameLine();
		ImGui::TextDisabled("(%s): %d visit%s, %s there, last %s", where.c_str(), pm->visits, pm->visits == 1 ? "" : "s",
			secondsText(secondsOf((unsigned int)pm->ticks)).c_str(), pm->here ? "now" : (secondsText(secondsOf(v.tick > pm->last ? v.tick - pm->last : 0)) + " ago").c_str());
		ImGui::Indent(14);
		if (!pm->goodFor.empty()) {
			std::string good;
			for (int need : pm->goodFor)
				good += (good.empty() ? "" : ", ") + std::string(needForIndex(need));
			ImGui::TextColored(green, "Favourite place %s", good.c_str());
		}
		if (pm->avoided) {
			std::vector<std::pair<float, int>> worse;
			for (int need : {0, 3, 4, 9, 13, 14})
				if (pm->feel[need] > 0.005f)
					worse.push_back({pm->feel[need], need});
			std::sort(worse.rbegin(), worse.rend());
			std::string why;
			for (size_t k = 0; k < worse.size() && k < 2; k++) {
				std::string n = NEED_NAMES[worse[k].second];
				for (auto& ch : n)
					ch = (char)tolower((unsigned char)ch);
				why += (why.empty() ? "" : " and ") + (n == "suffocate" ? std::string("breathless") : n);
			}
			ImGui::TextColored(red, "Keeps away: it felt %s there", why.empty() ? "worse" : why.c_str());
		}
		if (pm->felt && pm->goodFor.empty() && !pm->avoided) {
			// its needs' biggest differences there, either way
			std::vector<std::pair<float, int>> diff;
			for (int k = 0; k < 17; k++)
				if (std::fabs(pm->feel[k]) > 0.02f)
					diff.push_back({std::fabs(pm->feel[k]), k});
			std::sort(diff.rbegin(), diff.rend());
			std::string felt;
			for (size_t k = 0; k < diff.size() && k < 3; k++)
				felt += fmt::format("{}{} {}", felt.empty() ? "" : ", ", NEED_NAMES[diff[k].second], pm->feel[diff[k].second] < 0 ? "eased" : "rose");
			if (!felt.empty())
				ImGui::TextDisabled("Felt there: %s", felt.c_str());
		}
		if (!pm->things.empty()) {
			auto things = pm->things;
			std::sort(things.begin(), things.end(), [](auto& a, auto& b) { return a.second > b.second; });
			std::string seen;
			for (size_t k = 0; k < things.size() && k < 6; k++)
				seen += fmt::format("{}{}{}", seen.empty() ? "" : ", ", thing(things[k].first), things[k].second > 1 ? fmt::format(" ({})", things[k].second) : "");
			ImGui::TextWrapped("Saw: %s%s", seen.c_str(), things.size() > 6 ? ", ..." : "");
		}
		if (!pm->met.empty()) {
			std::string met;
			for (size_t k = 0; k < pm->met.size() && k < 5; k++)
				met += (met.empty() ? "" : ", ") + nameOf(pm->met[k].first);
			ImGui::PushStyleColor(ImGuiCol_Text, pink);
			ImGui::TextWrapped("Met: %s%s", met.c_str(), pm->met.size() > 5 ? ", ..." : "");
			ImGui::PopStyleColor();
		}
		ImGui::Unindent(14);
		ImGui::Spacing();
	}
	ImGui::EndChild();
	ImGui::TextDisabled("%zu room%s been in; %d more seen next door", been.size(), been.size() == 1 ? "" : "s", heardOf);
}

void MoodPage(const MindView& v) {
	if (v.mood.size() < 2) {
		ImGui::TextWrapped("Not enough time has passed to show its mood.");
		return;
	}
	KitNoteColored(ImVec4(0.6f, 0.62f, 0.68f, 1), "How content it has been; the colour below is its strongest need at the time.");
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float axisW = 74; // room for the y axis's labels
	ImVec2 o = ImGui::GetCursorScreenPos() + ImVec2(axisW, 6);
	const float W = 600, H = 220;
	// the y axis: how content, 0-100%
	for (int g = 0; g <= 4; g++) {
		std::string t = fmt::format("{}%", g * 25);
		ImVec2 ts = ImGui::CalcTextSize(t.c_str());
		dl->AddText(ImVec2(o.x - ts.x - 6, o.y + H * (1 - g / 4.0f) - ts.y / 2), DIM, t.c_str());
	}
	const char* axis = "content";
	ImVec2 as = ImGui::CalcTextSize(axis);
	dl->AddText(ImVec2(o.x - as.x - 6, o.y + H / 2 + 12), DIM, axis);
	dl->AddRectFilled(o, o + ImVec2(W, H), IM_COL32(30, 33, 41, 255));
	for (int g = 1; g < 4; g++)
		dl->AddLine(o + ImVec2(0, H * g / 4), o + ImVec2(W, H * g / 4), IM_COL32(48, 52, 64, 255));
	unsigned int t0 = v.mood.front().tick, t1 = std::max(v.mood.back().tick, t0 + 1);
	auto px = [&](unsigned int t) { return o.x + W * (t - t0) / (float)(t1 - t0); };
	for (size_t i = 1; i < v.mood.size(); i++) {
		auto &a = v.mood[i - 1], &b = v.mood[i];
		ImVec2 pa(px(a.tick), o.y + H * (1 - a.content)), pb(px(b.tick), o.y + H * (1 - b.content));
		dl->AddLine(pa, pb, IM_COL32(120, 210, 140, 255), 2.0f);
		if (b.strongest >= 0 && b.strength > 0.3f)
			dl->AddRectFilled(ImVec2(pa.x, o.y + H + 4), ImVec2(pb.x + 1, o.y + H + 16), KIND_COLOR[NEED_KIND[b.strongest]]);
	}
	ImVec2 mouse = ImGui::GetIO().MousePos;
	if (mouse.x >= o.x && mouse.x <= o.x + W && mouse.y >= o.y && mouse.y <= o.y + H + 16) {
		const MindView::Moment* best = &v.mood.front();
		for (auto& mm : v.mood)
			if (std::fabs(px(mm.tick) - mouse.x) < std::fabs(px(best->tick) - mouse.x))
				best = &mm;
		dl->AddLine(ImVec2(px(best->tick), o.y), ImVec2(px(best->tick), o.y + H), IM_COL32(255, 255, 255, 90));
		ImGui::SetTooltip("%s ago: %.0f%% content%s%s", secondsText(secondsOf(v.tick - best->tick)).c_str(), best->content * 100,
			best->strongest >= 0 && best->strength > 0.3f ? ", most of all " : "",
			best->strongest >= 0 && best->strength > 0.3f ? fmt::format("{} ({:.0f}%)", NEED_NAMES[best->strongest], best->strength * 100).c_str() : "");
	}
	std::string span = fmt::format("{} ago", secondsText(secondsOf(t1 - t0)));
	dl->AddText(o + ImVec2(0, H + 20), DIM, span.c_str());
	ImVec2 ns = ImGui::CalcTextSize("now");
	dl->AddText(o + ImVec2(W - ns.x, H + 20), DIM, "now");
	ImGui::Dummy(ImVec2(axisW + W, H + 46));
}

// the words it knows, those it is learning, and its own names for things
// (CreatureSpeech.h; owner, 2026-11-22: "build the made-up names part, too")
void WordsPage(oldCreature* c) {
	auto word = [](size_t i) {
		std::string w = engine.wordlist[i];
		while (!w.empty() && w.back() == ' ')
			w.pop_back();
		return w;
	};
	std::string known;
	std::vector<std::pair<int, std::string>> learning;
	int n = 0;
	for (size_t i = 0; i < c->vocab.size() && i < engine.wordlist.size(); i++) {
		std::string w = word(i);
		if (w.empty() || c->vocab[i] == 0)
			continue;
		if (c->vocab[i] >= WORD_KNOWN) {
			known += (n++ ? ", " : "") + w;
		} else
			learning.push_back({c->vocab[i], w});
	}
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 560);
	ImGui::Text("Words it knows (%d)", n);
	ImGui::TextDisabled("%s", known.empty() ? "None yet: it speaks its own tongue." : known.c_str());
	ImGui::Spacing();
	ImGui::TextUnformatted("Words it is learning");
	std::sort(learning.rbegin(), learning.rend());
	std::string some;
	for (size_t i = 0; i < learning.size() && i < 12; i++)
		some += fmt::format("{}{} ({:.0f}%)", i ? ", " : "", learning[i].second, 100.0f * learning[i].first / WORD_KNOWN);
	ImGui::TextDisabled("%s", some.empty() ? "None." : some.c_str());
	ImGui::Spacing();
	ImGui::TextUnformatted("Names of its own for things");
	bool any = false;
	for (auto& [name, for_] : c->ownNames)
		for (auto& [category, v] : for_) {
			if (v < 24)
				continue;
			any = true;
			std::string thing = (size_t)(16 + category) < engine.wordlist.size() ? word(16 + category) : std::string();
			bool knows = ownNameMeans(c, name) == category;
			ImGui::BulletText("%s", fmt::format("\"{}\": {}{}", name, thing.empty() ? "something" : thing, knows ? "" : fmt::format(" (learning, {:.0f}%)", std::min(99.0f, 100.0f * v / WORD_KNOWN))).c_str());
		}
	if (!any)
		ImGui::TextDisabled("None yet. Tickle it and type a name for your hand, or point the hand at a thing (or hold it) and type a name for that.");
	ImGui::PopTextWrapPos();
}

} // namespace

void DrawMindKit() {
	KitCreatureHeader();
	if (!KitNeedsCreature())
		return;
	Agent* a = KitCreatureAgent();
	c2Creature* c = KitCreature();
	CreatureMind* mind = c ? c->getMind() : nullptr;
	MindView v;
	if (!mind || !mind->view(v)) {
		ImGui::TextWrapped("%s runs the original brain, which this kit cannot show (start with --mind homeostat).", KitCreatureName(a).c_str());
		return;
	}
	if (ImGui::BeginTabBar("mindtabs")) {
		if (KitTab("Feelings")) {
			FeelingsPage(v);
			ImGui::EndTabItem();
		}
		if (KitTab("Choosing")) {
			ChoosingPage(v);
			ImGui::EndTabItem();
		}
		if (KitTab("Learned")) {
			LearnedPage(v);
			ImGui::EndTabItem();
		}
		if (KitTab("Memory map")) {
			MapPage(v);
			ImGui::EndTabItem();
		}
		if (KitTab("Places")) {
			PlacesPage(v);
			ImGui::EndTabItem();
		}
		if (KitTab("Mood")) {
			MoodPage(v);
			ImGui::EndTabItem();
		}
		if (KitTab("Words")) {
			WordsPage(c);
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

} // namespace Openc2eImgui
