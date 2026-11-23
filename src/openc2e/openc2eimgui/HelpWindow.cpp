// The Help window (openc2e addition, owner 2026-11-11): how to play here and
// what this version adds to the game. In depth, with a topic for each kit,
// since 2026-11-22 (owner: "really get in-depth on topics like this and lots
// of other stuff, including each kit"). Our own text, in
// data/help/help-topics.md (built into the program):
//
//   # A group of topics, as the list on the left shows it
//   ## A topic
//   ### A heading in it
//   - a bullet
//   any other line: a paragraph
//   **words** in gold; [[A topic]] or [[A topic|words]] links to it
//
// Player-visible: British spelling.

#include "HelpWindow.h"

#include "EmbeddedFiles.h"
#include "common/Ascii.h"
#include "common/devenv.h"

#include <algorithm>
#include <fmt/core.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <vector>

namespace Openc2eImgui {

namespace {

// a run of text in one style
struct Run {
	std::string text;
	bool strong = false;
	int link = -1; // a topic
};
struct Line {
	enum Kind { paragraph, bullet, heading } kind = paragraph;
	std::vector<Run> runs;
};
struct Topic {
	std::string title;
	std::string group;
	std::vector<Line> lines;
	std::string plain; // its whole text, lower case, for the search
};

std::vector<Topic> s_topics;

int findTopic(const std::string& title) {
	for (size_t i = 0; i < s_topics.size(); i++)
		if (s_topics[i].title == title)
			return (int)i;
	return -1;
}

std::vector<Run> runsOf(const std::string& line) {
	std::vector<Run> out;
	bool strong = false;
	std::string text;
	auto flush = [&] {
		if (!text.empty())
			out.push_back({text, strong, -1});
		text.clear();
	};
	for (size_t k = 0; k < line.size();) {
		if (line.compare(k, 2, "**") == 0) {
			flush();
			strong = !strong;
			k += 2;
		} else if (line.compare(k, 2, "[[") == 0) {
			size_t end = line.find("]]", k);
			if (end == std::string::npos) {
				text += line.substr(k);
				break;
			}
			flush();
			std::string to = line.substr(k + 2, end - k - 2), shown = to;
			if (size_t bar = to.find('|'); bar != std::string::npos) {
				shown = to.substr(bar + 1);
				to = to.substr(0, bar);
			}
			int topic = findTopic(to);
			if (topic < 0)
				fmt::print(stderr, "Help: no topic called {}\n", to);
			out.push_back({shown, strong, topic});
			k = end + 2;
		} else
			text += line[k++];
	}
	flush();
	return out;
}

void load() {
	static bool done = false;
	if (done)
		return;
	done = true;
	auto bytes = EmbeddedFile("help-topics.md");
	std::string text(bytes.begin(), bytes.end());
	std::vector<std::pair<int, std::string>> raw; // topic, line
	std::string group;
	for (size_t at = 0; at < text.size();) {
		size_t nl = std::min(text.find('\n', at), text.size());
		std::string line = text.substr(at, nl - at);
		at = nl + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.rfind("## ", 0) == 0) {
			s_topics.push_back({line.substr(3), group, {}, {}});
		} else if (line.rfind("# ", 0) == 0)
			group = line.substr(2);
		else if (!line.empty() && !s_topics.empty())
			raw.push_back({(int)s_topics.size() - 1, line});
	}
	// (the links are read once every topic is known)
	for (auto& [t, line] : raw) {
		Line l;
		std::string rest = line;
		if (line.rfind("### ", 0) == 0) {
			l.kind = Line::heading;
			rest = line.substr(4);
		} else if (line.rfind("- ", 0) == 0) {
			l.kind = Line::bullet;
			rest = line.substr(2);
		}
		l.runs = runsOf(rest);
		for (auto& r : l.runs)
			s_topics[t].plain += r.text;
		s_topics[t].plain += "\n";
		s_topics[t].lines.push_back(std::move(l));
	}
	for (auto& t : s_topics)
		t.plain = to_ascii_lowercase(t.title + "\n" + t.plain);
}

bool s_open = false;
bool s_focus = false;
int s_topic = 0;
std::vector<int> s_back;
bool s_to_top = false;
int s_in_list = 0; // the topic the list was last scrolled to
char s_search[64] = "";

void goTo(int topic) {
	if (topic < 0 || topic >= (int)s_topics.size() || topic == s_topic)
		return;
	s_back.push_back(s_topic);
	s_topic = topic;
	s_to_top = true;
}

std::vector<std::string> searchWords() {
	std::vector<std::string> out;
	std::string word;
	for (const char* p = s_search;; p++) {
		if (*p && *p != ' ')
			word += *p;
		else {
			if (!word.empty())
				out.push_back(to_ascii_lowercase(word));
			word.clear();
		}
		if (!*p)
			break;
	}
	return out;
}

const ImU32 GOLD = IM_COL32(255, 224, 140, 255);
const ImU32 LINK = IM_COL32(240, 190, 90, 255);

// One paragraph, wrapped word by word in its own styles, with the words
// searched for marked and the links clickable. Returns the topic of a link
// that was clicked, or -1.
int drawRuns(const std::vector<Run>& runs, const std::vector<std::string>& marks, ImU32 plainColour) {
	// the pieces between which a line may break: a word and what is joined to
	// it ("[[Health Kit]]." is three: Health, Kit and the full stop, the last
	// two without a break)
	struct Piece {
		std::string text;
		const Run* run;
		bool space; // a space before it
	};
	std::vector<Piece> pieces;
	bool space = false;
	for (auto& r : runs) {
		std::string word;
		auto flush = [&] {
			if (!word.empty()) {
				pieces.push_back({word, &r, space});
				space = false;
			}
			word.clear();
		};
		for (char ch : r.text) {
			if (ch == ' ') {
				flush();
				space = true;
			} else
				word += ch;
		}
		flush();
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float width = std::max(80.0f, ImGui::GetContentRegionAvail().x);
	const float row = ImGui::GetTextLineHeight() + 3.0f;
	const float gap = ImGui::CalcTextSize(" ").x;
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 mouse = ImGui::GetMousePos();
	const bool here = ImGui::IsWindowHovered();
	int clicked = -1;
	float x = 0, y = 0;
	for (size_t i = 0; i < pieces.size();) {
		// this piece and those joined to it
		size_t end = i + 1;
		float w = ImGui::CalcTextSize(pieces[i].text.c_str()).x;
		for (; end < pieces.size() && !pieces[end].space; end++)
			w += ImGui::CalcTextSize(pieces[end].text.c_str()).x;
		float before = x > 0 && pieces[i].space ? gap : 0;
		if (x > 0 && x + before + w > width) {
			x = 0;
			y += row;
			before = 0;
		}
		// (the gap inside a link is underlined with it)
		bool joined = before > 0 && i > 0 && pieces[i].run == pieces[i - 1].run && pieces[i].run->link >= 0;
		x += before;
		for (; i < end; i++) {
			const Piece& p = pieces[i];
			ImVec2 size = ImGui::CalcTextSize(p.text.c_str());
			ImVec2 at(origin.x + x, origin.y + y);
			std::string low = marks.empty() ? std::string() : to_ascii_lowercase(p.text);
			for (auto& m : marks)
				for (size_t f = low.find(m); f != std::string::npos; f = low.find(m, f + m.size())) {
					float x0 = ImGui::CalcTextSize(p.text.substr(0, f).c_str()).x, x1 = ImGui::CalcTextSize(p.text.substr(0, f + m.size()).c_str()).x;
					dl->AddRectFilled(ImVec2(at.x + x0 - 1, at.y), ImVec2(at.x + x1 + 1, at.y + size.y + 1), IM_COL32(120, 96, 20, 255), 2);
				}
			ImU32 colour = p.run->link >= 0 ? LINK : p.run->strong ? GOLD : plainColour;
			if (p.run->link >= 0) {
				float under = at.y + size.y;
				dl->AddLine(ImVec2(at.x - (joined ? gap : 0), under), ImVec2(at.x + size.x, under), IM_COL32(240, 190, 90, 140));
				if (here && mouse.x >= at.x - (joined ? gap : 0) && mouse.x < at.x + size.x && mouse.y >= at.y && mouse.y < at.y + row) {
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
					colour = IM_COL32(255, 255, 255, 255);
					if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
						clicked = p.run->link;
				}
			}
			joined = false;
			dl->AddText(at, colour, p.text.c_str());
			x += size.x;
		}
	}
	ImGui::Dummy(ImVec2(width, y + row));
	return clicked;
}

void drawTopic(const Topic& t, const std::vector<std::string>& marks) {
	ImGui::SetWindowFontScale(1.45f);
	ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(GOLD), "%s", t.title.c_str());
	ImGui::SetWindowFontScale(1.0f);
	ImGui::TextDisabled("%s", t.group.c_str());
	ImGui::Separator();
	ImGui::Spacing();
	const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
	int to = -1;
	bool firstLine = true;
	for (auto& l : t.lines) {
		int clicked = -1;
		if (l.kind == Line::heading) {
			if (!firstLine)
				ImGui::Dummy(ImVec2(1, 4));
			ImGui::SetWindowFontScale(1.15f);
			clicked = drawRuns(l.runs, marks, GOLD);
			ImGui::SetWindowFontScale(1.0f);
		} else if (l.kind == Line::bullet) {
			const float indent = 16;
			ImVec2 at = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + 6, at.y + ImGui::GetTextLineHeight() * 0.55f), 2.2f, GOLD);
			ImGui::Indent(indent);
			clicked = drawRuns(l.runs, marks, text);
			ImGui::Unindent(indent);
		} else {
			clicked = drawRuns(l.runs, marks, text);
			ImGui::Dummy(ImVec2(1, 2));
		}
		if (clicked >= 0)
			to = clicked;
		firstLine = false;
	}
	if (to >= 0)
		goTo(to);
}

} // namespace

void SetHelpOpen(bool open) {
	s_open = open;
	s_focus = open;
}

void OpenHelpTopic(const std::string& topic) {
	load();
	SetHelpOpen(true);
	goTo(findTopic(topic));
}

// the topic of the kit (or the like) in front now, or ""
static std::string topicInFront() {
	ImGuiWindow* w = ImGui::GetCurrentContext() ? GImGui->NavWindow : nullptr;
	if (!w)
		return "";
	std::string name = w->RootWindow->Name;
	name = name.substr(0, name.find("##"));
	load();
	return findTopic(name) >= 0 ? name : "";
}

void OpenHelpHere() {
	std::string topic = topicInFront();
	if (topic.empty())
		SetHelpOpen(true);
	else
		OpenHelpTopic(topic);
}

void DrawHelpWindow() {
	// developer aid: OPENC2E_TEST_HELP="Topic" or "Topic|search words" opens
	// it there, for snapshots
	static bool tested = false;
	if (const char* test = devenv("OPENC2E_TEST_HELP"); test && !tested) {
		tested = true;
		std::string t = test;
		if (size_t bar = t.find('|'); bar != std::string::npos) {
			snprintf(s_search, sizeof(s_search), "%s", t.substr(bar + 1).c_str());
			t = t.substr(0, bar);
		}
		OpenHelpTopic(t);
	}
	// F1 (handled here, where the keys reach ImGui even while typing in a box)
	if (ImGui::IsKeyPressed(ImGuiKey_F1, false))
		OpenHelpHere();
	if (!s_open)
		return;
	load();
	const ImVec2 screen = ImGui::GetMainViewport()->WorkSize;
	ImGui::SetNextWindowSize(ImVec2(std::min(900.0f, screen.x * 0.94f), std::min(600.0f, screen.y * 0.8f)), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(520, 260), ImVec2(FLT_MAX, FLT_MAX));
	if (s_focus) {
		ImGui::SetNextWindowFocus();
		s_focus = false;
	}
	if (!ImGui::Begin("Help", &s_open, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	if (s_topics.empty()) {
		ImGui::TextDisabled("(the topics are missing from this build)");
		ImGui::End();
		return;
	}
	const std::vector<std::string> words = searchWords();
	auto matches = [&](const Topic& t) {
		for (auto& w : words)
			if (t.plain.find(w) == std::string::npos)
				return false;
		return true;
	};

	// the topics, by group
	ImGui::BeginChild("topics", ImVec2(256, 0), ImGuiChildFlags_None);
	ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - 4);
	bool typed = ImGui::InputTextWithHint("##search", "Search the Help...", s_search, sizeof(s_search));
	ImGui::SameLine(0, 4);
	ImGui::BeginDisabled(s_search[0] == 0);
	if (ImGui::Button("x##clear", ImVec2(ImGui::GetFrameHeight(), 0)))
		s_search[0] = 0;
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Clear the search");
	ImGui::BeginChild("list", ImVec2(0, 0), ImGuiChildFlags_Borders);
	std::string group;
	int shown = 0, first = -1;
	for (size_t i = 0; i < s_topics.size(); i++) {
		const Topic& t = s_topics[i];
		if (!matches(t))
			continue;
		// (a title with the words in it is the likeliest one meant)
		std::string title = to_ascii_lowercase(t.title);
		bool inTitle = !words.empty() && std::all_of(words.begin(), words.end(), [&](const std::string& w) { return title.find(w) != std::string::npos; });
		if (first < 0 || (inTitle && to_ascii_lowercase(s_topics[first].title).find(words[0]) == std::string::npos))
			first = (int)i;
		shown++;
		if (t.group != group) {
			if (!group.empty())
				ImGui::Spacing();
			group = t.group;
			ImGui::TextDisabled("%s", group.c_str());
		}
		ImGui::Indent(8);
		if (inTitle)
			ImGui::PushStyleColor(ImGuiCol_Text, GOLD);
		if (ImGui::Selectable(t.title.c_str(), (int)i == s_topic))
			goTo((int)i);
		// (reached by a link or F1: brought into sight in the list)
		if ((int)i == s_topic && s_in_list != s_topic) {
			if (!ImGui::IsItemVisible())
				ImGui::SetScrollHereY(0.5f);
			s_in_list = s_topic;
		}
		if (inTitle)
			ImGui::PopStyleColor();
		ImGui::Unindent(8);
	}
	if (shown == 0)
		ImGui::TextWrapped("No topic has all of those words in it.");
	ImGui::EndChild();
	ImGui::EndChild();
	// typing goes to the first topic found, unless this one has the words too
	if (typed && !words.empty() && first >= 0 && (!matches(s_topics[s_topic]) || to_ascii_lowercase(s_topics[first].title).find(words[0]) != std::string::npos)) {
		s_topic = first; // (not a step for Back: it would be one for each letter)
		s_to_top = true;
	}

	// the topic
	ImGui::SameLine();
	ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_None);
	ImGui::BeginDisabled(s_back.empty());
	if (ImGui::Button("Back")) {
		s_topic = s_back.back();
		s_back.pop_back();
		s_to_top = true;
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("The topic before");
	ImGui::SameLine();
	ImGui::BeginDisabled(s_topic == 0);
	if (ImGui::ArrowButton("##previous", ImGuiDir_Left))
		goTo(s_topic - 1);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("The topic above this in the list");
	ImGui::EndDisabled();
	ImGui::SameLine(0, 2);
	ImGui::BeginDisabled(s_topic + 1 >= (int)s_topics.size());
	if (ImGui::ArrowButton("##next", ImGuiDir_Right))
		goTo(s_topic + 1);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("The topic below this in the list");
	ImGui::EndDisabled();
	ImGui::BeginChild("text", ImVec2(0, 0), ImGuiChildFlags_Borders);
	if (s_to_top) {
		ImGui::SetScrollY(0);
		s_to_top = false;
	}
	drawTopic(s_topics[s_topic], words);
	ImGui::EndChild();
	ImGui::EndChild();
	ImGui::End();
}

} // namespace Openc2eImgui
