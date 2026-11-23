#include <cfloat>
#include "common/devenv.h"
#include "C2ToolBar.h"

#include <algorithm>
#include <array>
#include <functional>

#include "C2KitsInternal.h"
#include "HelpWindow.h"
#include "Updater.h"
#include "creatures/CreatureSpeech.h"
#include "creatures/Thoughts.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_misc.h>
#include "GameFolder.h"

#include "AgentInjector.h"
#include "C2Kits.h"
#include "C2EyeView.h"
#include "WorldChooser.h"
#include "common/backend/Backend.h"
#include "WorldSave.h"
#include "MusicManager.h"
#include "SoundManager.h"
#include "C2StatusBar.h"
#include "C2ScrollBars.h"
#include "creatures/CreatureAgent.h"
#include "creatures/Creature.h"
#include "creatures/SkeletalCreature.h"
#include "Camera.h"
#include "Engine.h"
#include "EmbeddedFiles.h"
#include "Hatchery.h"
#include "ImportCreature.h"
#include "ImGuiUtils.h"
#include "MainMenu.h"
#include "PointerAgent.h"
#include "SpritePart.h"
#include "World.h"
#include "Scriptorium.h"
#include "caosVM.h"
#include "fileformats/ImageUtils.h"
#include "fileformats/peFile.h"

#include <fmt/core.h>
#include <imgui.h>
#include <vector>
#include <imgui_internal.h>

using namespace ImGuiUtils;

namespace Openc2eImgui {

static int s_last_c2toolbar_height = 0;

// View > Toolbars, as in Creatures 2
static bool s_show_standard = true;
static bool s_show_pointer = true;
static bool s_show_applets = true;
static bool s_show_favorites = true;

static bool readFlag(const char* key, bool def) {
	std::string v = readPlayerSetting(key);
	return v.empty() ? def : v != "0";
}

static void writeFlag(const char* key, bool on) {
	writePlayerSetting(key, on ? "1" : "0");
}

// Camera > Smooth Scrolling
static bool s_smooth_scrolling = false;

panstyle UiPanStyle() {
	return s_smooth_scrolling ? smoothscroll : jump;
}

// View > Toolbars > Text Labels: each toolbar button's name under it
static bool s_text_labels = false;

static bool ToolButton(TextureRect icon, bool enabled, const char* label) {
	if (!s_text_labels)
		return ImageButton(icon, enabled);
	const ImGuiStyle& st = ImGui::GetStyle();
	ImFont* font = ImGui::GetFont();
	float fs = ImGui::GetFontSize(); // (the pixel font does not scale cleanly)
	ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label);
	ImVec2 p = ImGui::GetCursorScreenPos();
	float bw = icon.w + st.FramePadding.x * 2, bh = icon.h + st.FramePadding.y * 2;
	float w = std::max(bw, ts.x + 6);
	ImGui::BeginGroup();
	ImGui::SetCursorScreenPos(ImVec2(p.x + (w - bw) / 2, p.y));
	bool clicked = ImageButton(icon, enabled);
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + bh));
	ImGui::Dummy(ImVec2(w, ts.y + 1));
	ImGui::EndGroup();
	ImGui::GetWindowDrawList()->AddText(font, fs, ImVec2(p.x + (w - ts.x) / 2, p.y + bh),
		ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
	return clicked;
}

// View > ToolTips for Objects: resting the hand on a creature names it, on
// an object says what kind of thing it is (the game's own word for it, as
// creatures learn it). Creatures first, even behind something; otherwise
// what the hand would click (transparent scenery passed over). Not over the
// kits or menus, and only once the hand has rested on it a moment.
static bool s_object_tips = false;
static const char* GenusName(int genus);

static void DrawObjectToolTip() {
	static Agent* s_last = nullptr;
	static double s_since = 0;
	if (!s_object_tips || !world.hand() || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemActive()) {
		s_last = nullptr;
		return;
	}
	int px = world.hand()->pointerX(), py = world.hand()->pointerY();
	Agent* a = world.creatureAt(px, py);
	if (!a)
		if (CompoundPart* part = world.partAt(px, py, true, false))
			a = part->getParent();
	if (a && (a == world.hand() || a->family == 1 || a->invisible()))
		a = nullptr;
	if (a != s_last) {
		s_last = a;
		s_since = ImGui::GetTime();
	}
	if (!a || ImGui::GetTime() - s_since < 0.4)
		return;
	std::string text;
	if (auto* ca = dynamic_cast<CreatureAgent*>(a); ca && ca->getCreature()) {
		std::string given = KitGivenName(a);
		std::string kind = fmt::format("{} {}", ca->getCreature()->isFemale() ? "female" : "male", GenusName(a->genus));
		for (auto& ch : kind)
			ch = (char)tolower((unsigned char)ch);
		text = given.empty() ? kind : fmt::format("{} ({})", given, kind);
		if (!ca->getCreature()->isAlive())
			text += ", dead";
	} else if (a->category >= 0)
		text = KitCategoryNoun(a->category);
	if (!text.empty())
		ImGui::SetTooltip("%s", text.c_str());
}

// The halo over the selected creature (the toolbar's Halo button, and
// Camera > Norn Indicator, C2's name for it): the game's own system macro
// (world-file script 0 1 0 0) makes or removes the halo object (2 1 3),
// whose timer script keeps it over the creature's head.
static bool HaloShown() {
	// (the one found last time, while it is there; else looked for again)
	static AgentRef last;
	if (last && !last->isDying() && last->family == 2 && last->genus == 1 && last->species == 3)
		return true;
	static unsigned int looked = 0;
	unsigned int now = engine.get_ticks_msec();
	if (now - looked < 100)
		return false;
	looked = now;
	for (auto& a : world.agents)
		if (a && a->family == 2 && a->genus == 1 && a->species == 3) {
			last = AgentRef(a);
			return true;
		}
	return false;
}

static bool CanToggleHalo() {
	return (bool)world.selectedcreature && world.scriptorium->getScript(0, 1, 0, 0) != nullptr;
}

static void ToggleHalo() {
	std::shared_ptr<script> macro = world.scriptorium->getScript(0, 1, 0, 0);
	if (!macro)
		return;
	caosVM vm(0);
	try {
		vm.runEntirely(macro);
	} catch (std::exception& e) {
		fmt::print("* Halo macro failed: {}\n", e.what());
	}
}

int GetC2ToolBarHeight() {
	return s_last_c2toolbar_height;
}


// Moves the camera to a favorite place (stops following a creature). Places
// store the camera's top-left scroll position, like the world file's own
// starting scroll position.
static void GoToFavoritePlace(int i) {
	if (i < 0 || i >= (int)world.favoriteplaces.size())
		return;
	auto& f = world.favoriteplaces[i];
	engine.camera->trackAgent(AgentRef(), 0, 0, brittle, none);
	engine.camera->moveToGlobal(f.x, f.y, UiPanStyle());
}

// Selects the next living creature after the current selection and centers
// the camera on it.
static void SelectNextCreature() {
	std::vector<std::shared_ptr<Agent>> creatures;
	for (auto& a : world.agents) {
		if (!a)
			continue;
		CreatureAgent* ca = dynamic_cast<CreatureAgent*>(a.get());
		if (ca && ca->getCreature() && ca->getCreature()->isAlive())
			creatures.push_back(a);
	}
	if (creatures.empty())
		return;
	size_t next = 0;
	std::shared_ptr<Agent> current = world.selectedcreature.lock();
	for (size_t i = 0; i < creatures.size(); i++) {
		if (creatures[i] == current)
			next = (i + 1) % creatures.size();
	}
	std::shared_ptr<Agent> c = creatures[next];
	world.selectCreature(c);
	int cx = (int)(c->x + c->getWidth() / 2.0f) - (int)engine.camera->getWidth() / 2;
	int cy = (int)(c->y + c->getHeight() / 2.0f) - (int)engine.camera->getHeight() / 2;
	engine.camera->moveToGlobal(cx, cy, UiPanStyle());
}

static const char* GenusName(int genus) {
	switch (genus) {
		case 1: return "Norn";
		case 2: return "Grendel";
		case 3: return "Ettin";
		default: return "Creature";
	}
}

// This fork's version, shown in Help > About openC2e (raise it for releases).
// 0.3.0 (2026-11-12): openc2e's last release was v0.2 (June 2020), and the
// code this fork started from already ran 825 commits past it, so the fork
// numbers on from there; still 0.x, as there is more to do before a 1.0.
// 0.4.0 (2026-11-14): food that cannot be eaten no longer counts as food,
// stalled meals, starving creatures leave, lift links, floating bodies,
// the toolbar speed control, vivid babies (version log in the project's
// NOTES.md).
// 0.5.0 (2026-11-14, night): every object kept between launches (stage 3).
// 0.6.0 (2026-11-14, night): signature scents; the hand takes things from
// creatures and keeps its mode; the gene lab's east end sealed.
#define OPENC2E_FORK_VERSION "0.33.0"

const char* ZeeturesVersion() {
	return OPENC2E_FORK_VERSION;
}

// CITATIONS.md, as Help > About shows it: "## " headings as separators,
// "- " entries wrapped, each Markdown link [text](url) as a small button
// that opens it; italics marks dropped
static void DrawCitations() {
	static std::vector<std::string> lines = [] {
		std::vector<std::string> v;
		auto bytes = EmbeddedFile("CITATIONS.md");
		std::string text(bytes.begin(), bytes.end());
		size_t at = 0;
		while (at <= text.size()) {
			size_t nl = text.find('\n', at);
			if (nl == std::string::npos)
				nl = text.size();
			v.push_back(text.substr(at, nl - at));
			at = nl + 1;
		}
		return v;
	}();
	if (lines.empty() || (lines.size() == 1 && lines[0].empty())) {
		ImGui::TextDisabled("(the list is missing from this build)");
		return;
	}
	// (filling its window, less a row for the Close button)
	ImGui::BeginChild("citations", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
	int id = 0;
	for (auto& raw : lines) {
		if (raw.rfind("# ", 0) == 0 || raw.empty())
			continue;
		if (raw.rfind("## ", 0) == 0) {
			ImGui::Spacing();
			ImGui::SeparatorText(raw.substr(3).c_str());
			continue;
		}
		bool bullet = raw.rfind("- ", 0) == 0;
		// "  - ": a line under the entry above ("Since then": newer
		// research on the same idea), indented
		bool sub = raw.rfind("  - ", 0) == 0;
		std::string line = bullet ? raw.substr(2) : sub ? raw.substr(4) : raw;
		std::string shown;
		std::vector<std::pair<std::string, std::string>> links;
		for (size_t k = 0; k < line.size(); k++) {
			if (line[k] == '[') {
				size_t close = line.find("](", k);
				size_t end = close == std::string::npos ? std::string::npos : line.find(')', close + 2);
				if (end != std::string::npos) {
					links.push_back({line.substr(k + 1, close - k - 1), line.substr(close + 2, end - close - 2)});
					k = end;
					continue;
				}
			}
			if (line[k] == '*')
				continue;
			shown += line[k];
		}
		// (a link taken out leaves two spaces)
		for (size_t sp; (sp = shown.find("  ")) != std::string::npos;)
			shown.erase(sp, 1);
		while (!shown.empty() && shown.back() == ' ')
			shown.pop_back();
		if (sub)
			ImGui::Indent();
		if (bullet)
			ImGui::Bullet();
		ImGui::TextWrapped("%s", shown.c_str());
		for (size_t n = 0; n < links.size(); n++) {
			if (n == 0)
				ImGui::Indent();
			else
				ImGui::SameLine();
			std::string label = links[n].first + "##cite" + std::to_string(id++);
			if (ImGui::SmallButton(label.c_str()))
				SDL_OpenURL(links[n].second.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", links[n].second.c_str());
			if (n + 1 == links.size())
				ImGui::Unindent();
		}
		if (sub)
			ImGui::Unindent();
	}
	ImGui::EndChild();
}
// the openc2e this fork started from (an unreleased development build)
#define OPENC2E_UPSTREAM_BASE "commit dfcd46f2, 7 October 2026 (825 commits after its v0.2 release)"
#define OPENC2E_WEB "https://openc2e.github.io/"
// this fork's name and home (owner, 2026-11-15: "let's call the game Zeetures")
#define FORK_NAME "Zeetures"
#define FORK_WEB "https://github.com/DotairZee/zeetures"
static bool s_open_about = false;

// Add Favourite Place (the Camera menu or the + by Places) opens the naming
// popup on the menu bar's next draw
static bool s_open_add_place = false;

// a world has six favourite place slots
static bool HasFreePlace() {
	if (world.favoriteplaces.size() < 6)
		return true;
	for (auto& f : world.favoriteplaces)
		if (f.name.empty())
			return true;
	return false;
}

// The Creatures 2 menu bar (layout read from the game's own menu resource).
// Items that openc2e can't do yet are shown grayed out, as C2 itself does.
static void DrawC2MenuBar() {
	static bool s_open_new_world = false;
	static bool s_open_save_as = false;
	static bool s_open_save_message = false;
static std::string s_set_aside_error;
static bool s_set_aside_was_paused = false;
	static std::string s_save_message;

	if (ImGui::BeginMenu("File")) {
		// worlds (C2 used a separate World Switcher; openc2e restarts into the chosen world)
		if (ImGui::MenuItem("New World..."))
			s_open_new_world = true;
		auto worlds = ListWorlds();
		if (ImGui::BeginMenu("Open World", worlds.size() > 1 || (worlds.size() == 1 && worlds[0].name != engine.currentworld))) {
			for (auto& w : worlds) {
				bool current = w.name == engine.currentworld;
				if (ImGui::MenuItem(w.name.c_str(), nullptr, current, !current))
					RelaunchIntoWorld(w.name);
			}
			ImGui::EndMenu();
		}
		if (!engine.currentworlddir.empty()) {
			bool atlaunch = DefaultWorld() == engine.currentworld;
			if (ImGui::MenuItem("Open This World at Launch", nullptr, atlaunch))
				SetDefaultWorld(atlaunch ? "" : engine.currentworld);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Starts in this world, without asking which");
		}
		if (ImGui::MenuItem("Save World", nullptr, false, !engine.currentworlddir.empty())) {
			std::string err;
			s_save_message = WorldSave::SaveWorldState(engine.currentworlddir, err) ? "World saved." : err;
			s_open_save_message = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Saves the whole world now: creatures (bodies and minds), eggs and objects. It also saves itself (Autosave, and when you quit).");
		if (ImGui::MenuItem("Save World As...", nullptr, false, !engine.currentworlddir.empty()))
			s_open_save_as = true;
		// openc2e addition (owner, 2026-11-11): saves every few minutes
		if (ImGui::BeginMenu("Autosave")) {
			int cur = autosaveMinutes();
			for (int m : {1, 5, 10, 15, 30})
				if (ImGui::MenuItem(fmt::format("Every {} minute{}", m, m == 1 ? "" : "s").c_str(), nullptr, cur == m))
					setAutosaveMinutes(m);
			if (ImGui::MenuItem("Only when quitting", nullptr, cur == 0))
				setAutosaveMinutes(0);
			ImGui::EndMenu();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How often the world saves itself (it always saves when you quit).");
		ImGui::Separator();
		ImGui::MenuItem("Export Current Creature...", nullptr, false, false);
		ImportCreatureMenuItem();
		ImGui::Separator();
		if (ImGui::BeginMenu("Sound")) {
			bool music = !engine.musicmanager->isMuted();
			if (ImGui::MenuItem("Ambient Music", nullptr, music)) {
				engine.musicmanager->setMuted(music);
				engine.musicmanager->setMIDIMuted(music);
				writeFlag("ambient-music", !music);
			}
			bool world_sounds = !soundmanager.isMuted();
			if (ImGui::MenuItem("World Sounds", nullptr, world_sounds)) {
				soundmanager.setMuted(world_sounds);
				writeFlag("world-sounds", !world_sounds);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Machines, objects, creatures' noises: every sound but music and speech");
			bool bibble = !soundmanager.areVoicesMutedByPlayer();
			if (ImGui::MenuItem("Bibble", nullptr, bibble)) {
				soundmanager.setVoicesMutedByPlayer(bibble);
				writeFlag("bibble", !bibble);
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Creatures' speech");
			ImGui::EndMenu();
		}
		ImGui::Separator();
		// not in C2's menu: the player's own game folder (GameFolder.cpp)
		if (ImGui::MenuItem("Creatures 2 Folder..."))
			GameFolder::chooseAnother(SDL_GetKeyboardFocus());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Playing from %s\nChoose another folder for the next start",
				(const char*)GameFolder::currentFolder().u8string().c_str());
		ImGui::Separator();
		if (ImGui::MenuItem("Exit"))
			engine.done = true; // (as the window's close button: paused too)
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("View")) {
		if (ImGui::MenuItem("Creature's View", nullptr, IsEyeViewOpen(), (bool)world.selectedcreature))
			SetEyeViewOpen(!IsEyeViewOpen());
		ImGui::Separator();
		// not in C2's menu; openc2e addition (owner, 2026-11-09)
		if (ImGui::MenuItem("Zoom In", "Ctrl+Wheel", false, engine.zoom < 3.0f))
			engine.zoomStep(1);
		if (ImGui::MenuItem("Zoom Out", "Ctrl+Wheel", false, engine.zoom > 0.5f))
			engine.zoomStep(-1);
		if (ImGui::MenuItem(fmt::format("Actual Size ({:.0f}%)###actualsize", engine.zoom * 100).c_str(), "Ctrl+0", false, engine.zoom != 1.0f))
			engine.setZoom(1.0f);
		ImGui::Separator();
		if (ImGui::BeginMenu("Toolbars")) {
			if (ImGui::MenuItem("Standard", nullptr, &s_show_standard))
				writeFlag("toolbar-standard", s_show_standard);
			if (ImGui::MenuItem("Pointer", nullptr, &s_show_pointer))
				writeFlag("toolbar-pointer", s_show_pointer);
			if (ImGui::MenuItem("Applets", nullptr, &s_show_applets))
				writeFlag("toolbar-applets", s_show_applets);
			if (ImGui::MenuItem("Favourites", nullptr, &s_show_favorites))
				writeFlag("toolbar-favourites", s_show_favorites);
			ImGui::Separator();
			if (ImGui::MenuItem("Text Labels", nullptr, &s_text_labels))
				writeFlag("text-labels", s_text_labels);
			ImGui::EndMenu();
		}
		// not in C2's menu; openc2e addition (owner, 2026-11-11)
		bool bubbles = speechBubblesShown();
		if (ImGui::MenuItem("Speech Bubbles", nullptr, &bubbles))
			setSpeechBubblesShown(bubbles);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Creatures' words in bubbles (their voices go on; Bibble under File > Sound)");
		// openc2e addition (owner, 2026-11-13): what creatures are thinking,
		// as pictures in thought bubbles
		if (ImGui::BeginMenu("Thought Bubbles")) {
			Thoughts::Mode tm = Thoughts::mode();
			if (ImGui::MenuItem("Every Creature", nullptr, tm == Thoughts::EVERY))
				Thoughts::setMode(Thoughts::EVERY);
			if (ImGui::MenuItem("Selected Creature Only", nullptr, tm == Thoughts::SELECTED))
				Thoughts::setMode(Thoughts::SELECTED);
			if (ImGui::MenuItem("Off", nullptr, tm == Thoughts::OFF))
				Thoughts::setMode(Thoughts::OFF);
			ImGui::EndMenu();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("What creatures are thinking, in pictures, when they set about something new");
		bool statusbar = IsC2StatusBarShown();
		if (ImGui::MenuItem("Status Bar", nullptr, &statusbar)) {
			SetC2StatusBarShown(statusbar);
			writeFlag("status-bar", statusbar);
		}
		if (ImGui::MenuItem("ToolTips for Objects", nullptr, &s_object_tips))
			writeFlag("object-tooltips", s_object_tips);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Name what the hand rests on: a creature, or the kind of object");
		bool scrollbars = AreC2ScrollBarsShown();
		if (ImGui::MenuItem("Scroll Bars", nullptr, &scrollbars)) { // not in C2's menu; openc2e addition
			SetC2ScrollBarsShown(scrollbars);
			writeFlag("scroll-bars", scrollbars);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Full Screen", "F12", get_backend()->isFullscreen()))
			get_backend()->setFullscreen(!get_backend()->isFullscreen());
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Camera")) {
		bool tracking = world.selectedcreature && engine.camera->trackedAgent() == world.selectedcreature;
		if (ImGui::MenuItem("Track Norn", nullptr, tracking, (bool)world.selectedcreature)) {
			if (tracking)
				engine.camera->trackAgent(AgentRef(), 0, 0, brittle, none);
			else
				engine.camera->trackAgent(world.selectedcreature, 0, 0, flexible, none);
		}
		if (ImGui::MenuItem("Norn Indicator", nullptr, HaloShown(), CanToggleHalo()))
			ToggleHalo();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("A halo over the selected creature (the toolbar's Halo button)");
		if (ImGui::MenuItem("Smooth Scrolling", nullptr, &s_smooth_scrolling))
			writeFlag("smooth-scrolling", s_smooth_scrolling);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Glide to places and creatures you choose, instead of jumping");
		bool smoothmove = SkeletalCreature::smoothMovement();
		if (ImGui::MenuItem("Smooth Movement", nullptr, &smoothmove))
			SkeletalCreature::setSmoothMovement(smoothmove);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Creatures move smoothly from pose to pose, instead of jumping five times a second");
		ImGui::Separator();
		bool hasfree = false, hasnamed = false;
		for (auto& f : world.favoriteplaces) {
			if (f.name.empty())
				hasfree = true;
			else
				hasnamed = true;
		}
		if (world.favoriteplaces.size() < 6)
			hasfree = true;
		if (ImGui::MenuItem("Add Favourite Place...", nullptr, false, hasfree))
			s_open_add_place = true;
		if (ImGui::BeginMenu("Remove Favourite Place...", hasnamed)) {
			for (auto& f : world.favoriteplaces) {
				if (!f.name.empty() && ImGui::MenuItem(f.name.c_str())) {
					f.name.clear();
					f.x = f.y = 0;
				}
			}
			ImGui::EndMenu();
		}
		ImGui::EndMenu();
	}
	if (ImGui::BeginMenu("Tools")) {
		// in the toolbar's order, with its groups (owner, 2026-11-12)
		if (ImGui::MenuItem("Hatchery", nullptr, false, IsHatcheryEnabled()))
			SetHatcheryOpen(true);
		if (ImGui::MenuItem("Owner's Kit", nullptr, false, IsC2KitAvailable("owners")))
			OpenC2Kit("owners");
		if (ImGui::MenuItem("Health Kit", nullptr, false, IsC2KitAvailable("health")))
			OpenC2Kit("health");
		if (ImGui::MenuItem("Graveyard Kit", nullptr, false, IsC2KitAvailable("graveyard")))
			OpenC2Kit("graveyard");
		ImGui::Separator();
		if (ImGui::MenuItem("Breeder's Kit", nullptr, false, IsC2KitAvailable("breeders")))
			OpenC2Kit("breeders");
		if (ImGui::MenuItem("Science Kit", nullptr, false, IsC2KitAvailable("science")))
			OpenC2Kit("science");
		if (ImGui::MenuItem("Mind Kit", nullptr, false, IsC2KitAvailable("mind")))
			OpenC2Kit("mind");
		if (ImGui::MenuItem("Observation Kit", nullptr, false, IsC2KitAvailable("observation")))
			OpenC2Kit("observation");
		ImGui::Separator();
		if (ImGui::MenuItem("Agent Injector", nullptr, false, IsAgentInjectorEnabled())) // openc2e's
			SetAgentInjectorOpen(true);
		if (ImGui::MenuItem("Chronicles Kit", nullptr, false, IsC2KitAvailable("chronicles")))
			OpenC2Kit("chronicles");
		if (ImGui::MenuItem("Ecology Kit", nullptr, false, IsC2KitAvailable("ecology")))
			OpenC2Kit("ecology");
		ImGui::EndMenu();
	}
	// the Creatures menu stops short of the screen's foot and scrolls
	// (owner, 2026-11-13): at most 20 rows, or 60% of the window if less
	{
		float row = ImGui::GetTextLineHeightWithSpacing();
		float maxh = std::min(20 * row, ImGui::GetIO().DisplaySize.y * 0.6f) + ImGui::GetStyle().WindowPadding.y * 2;
		ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxh));
	}
	// developer aid: OPENC2E_TEST_MENU="Creatures" opens that menu once, for
	// a snapshot of it
	if (static const char* tm = devenv("OPENC2E_TEST_MENU"); tm && std::string(tm) == "Creatures" && world.tickcount > 0) {
		static int frames = 0;
		if (++frames == 30)
			ImGui::OpenPopup("Creatures");
		// (the world does not tick while a menu is open, so the picture is
		// taken from here: OPENC2E_TEST_MENU_SHOT=file.bmp)
		if (frames == 60)
			if (const char* shot = devenv("OPENC2E_TEST_MENU_SHOT"))
				get_backend()->requestScreenshot(shot);
	}
	if (ImGui::BeginMenu("Creatures")) {
		int n = 0;
		std::shared_ptr<Agent> current = world.selectedcreature.lock();
		for (auto& a : world.agents) {
			if (!a)
				continue;
			CreatureAgent* ca = dynamic_cast<CreatureAgent*>(a.get());
			if (!ca || !ca->getCreature() || !ca->getCreature()->isAlive())
				continue;
			n++;
			std::string given = KitGivenName(a.get());
			std::string label = given.empty() ? fmt::format("{} {} ({})", GenusName(a->genus), n, ca->getCreature()->isFemale() ? "female" : "male")
											   : fmt::format("{} ({} {})##{}", given, ca->getCreature()->isFemale() ? "female" : "male", GenusName(a->genus), n);
			if (ImGui::MenuItem(label.c_str(), nullptr, a == current)) {
				world.selectCreature(a);
				int cx = (int)(a->x + a->getWidth() / 2.0f) - (int)engine.camera->getWidth() / 2;
				int cy = (int)(a->y + a->getHeight() / 2.0f) - (int)engine.camera->getHeight() / 2;
				engine.camera->moveToGlobal(cx, cy, UiPanStyle());
			}
		}
		if (n == 0)
			ImGui::MenuItem("None Available", nullptr, false, false);
		// (the life stage chooser is in the Owner's Kit, Registration;
		// owner, 2026-11-12)
		ImGui::EndMenu();
	}
	// Help: what the toolbar's Help and Web buttons do, in words, and About
	// (C2's items we cannot offer are left out; owner, 2026-11-12)
	if (ImGui::BeginMenu("Help")) {
		if (ImGui::MenuItem("Help Topics", "F1"))
			SetHelpOpen(true);
		if (ImGui::MenuItem(FORK_NAME " on GitHub"))
			SDL_OpenURL(FORK_WEB);
		if (ImGui::MenuItem("openc2e on the Web"))
			SDL_OpenURL(OPENC2E_WEB);
		ImGui::Separator();
		UpdaterMenuItems();
		ImGui::Separator();
		if (ImGui::MenuItem("About " FORK_NAME "..."))
			s_open_about = true;
		ImGui::EndMenu();
	}

	// About Zeetures (developer aid: OPENC2E_OPEN_KITS="about" opens it)
	static bool s_test_about = devenv("OPENC2E_OPEN_KITS") && strstr(devenv("OPENC2E_OPEN_KITS"), "about");
	if (s_test_about && world.worldtickcount > 5) {
		s_test_about = false;
		s_open_about = true;
	}
	if (s_open_about) {
		ImGui::OpenPopup("About " FORK_NAME);
		s_open_about = false;
	}
	// (never taller than the window: it scrolls instead)
	ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetMainViewport()->WorkSize.y * 0.95f));
	if (ImGui::BeginPopupModal("About " FORK_NAME, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::SetWindowFontScale(1.6f);
		ImGui::TextUnformatted(FORK_NAME);
		ImGui::SetWindowFontScale(1.0f);
		// (said plainly under the name, with what it forks from: owner, 2026-11-15)
		ImGui::TextUnformatted("A fork of openc2e (from its commit dfcd46f2, 7 October 2026)");
		ImGui::Text("Version %s (built %s)", OPENC2E_FORK_VERSION, __DATE__);
		ImGui::TextUnformatted("by DotairZee, developed alongside Claude (Anthropic's AI)");
		// (owner, 2026-11-15: "this build is a labor of love by an old fan";
		// British spelling, as all the game's own text)
		ImGui::TextUnformatted("A labour of love by an old fan.");
		ImGui::Spacing();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 420);
		ImGui::TextUnformatted("Creatures 2 with creatures that think for themselves: a mind of their own, words, temperament and "
							   "relationships, breeding, and a full set of kits.");
		ImGui::Spacing();
		ImGui::TextDisabled(FORK_NAME " is a fork of openc2e, the open-source Creatures engine, by the developers below. openc2e "
							"is free software under the GNU Lesser General Public License 2.1, and so are " FORK_NAME "'s changes to it.");
		ImGui::TextDisabled("Based on openc2e " OPENC2E_UPSTREAM_BASE ".");
		ImGui::TextDisabled("The game's art, sounds and worlds come from your own copy of Creatures 2.");
		ImGui::Spacing();
		// openc2e's developers, from its AUTHORS file and its history up to
		// the commit this fork started from (most commits first)
		if (ImGui::CollapsingHeader("openc2e was written by", ImGuiTreeNodeFlags_DefaultOpen)) {
			static const char* const devs[][2] = {
				{"Alyssa Milburn (fuzzie)", "lead developer"},
				{"Michael Maltese (ligfx)", "developer and maintainer since 2019"},
				{"Bryan Donlan (bdonlan)", "virtual machine and core developer"},
				{"Jeremy Apthorp (nornagon)", "core developer"},
				{"Eric Goodwin (muffin)", "Windows builds, documentation and bug fixes"},
				{"Dylan William Hardison (dylan)", ""},
				{"Sven Eckelmann", ""},
				{"Bertram Bourdrez (bbz)", ""},
			};
			for (auto& d : devs) {
				ImGui::BulletText("%s", d[0]);
				if (d[1][0]) {
					ImGui::SameLine();
					ImGui::TextDisabled("%s", d[1]);
				}
			}
			ImGui::TextWrapped("with contributions from expwnent, Michael Lynch, Shreddan, pushfoo, Jelle De Loecker and callym.");
		}
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		// the research behind Zeetures (owner, 2026-11-16: "a citations
		// list ... in the About section"): CITATIONS.md, built into the
		// program, the same file as on GitHub, in a window of its own
		// sized to the screen (inside About it made About taller than the
		// game's window; owner's screenshot)
		// (developer aid: OPENC2E_OPEN_KITS="about,citations" opens it)
		static bool s_open_citations = devenv("OPENC2E_OPEN_KITS") && strstr(devenv("OPENC2E_OPEN_KITS"), "citations");
		if (ImGui::Button("Research behind " FORK_NAME) || s_open_citations) {
			ImGui::OpenPopup("Research behind " FORK_NAME);
			s_open_citations = false;
		}
		ImGui::SameLine();
		if (ImGui::Button("openc2e on the Web"))
			SDL_OpenURL(OPENC2E_WEB);
		ImGui::SameLine();
		if (ImGui::Button("Check for Updates")) {
			CheckForUpdatesNow();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		bool researchOpen = ImGui::IsPopupOpen("Research behind " FORK_NAME);
		if (ImGui::Button("OK") || (!researchOpen && ImGui::IsKeyPressed(ImGuiKey_Escape)))
			ImGui::CloseCurrentPopup();
		{
			const ImGuiViewport* vp = ImGui::GetMainViewport();
			ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, vp->WorkSize.x * 0.9f), vp->WorkSize.y * 0.8f), ImGuiCond_Appearing);
			ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			if (ImGui::BeginPopupModal("Research behind " FORK_NAME, nullptr)) {
				DrawCitations();
				if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape))
					ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
		}
		ImGui::EndPopup();
	}

	// Save World As: a new world folder from the same starting world file,
	// with the current state saved into it; the session continues there
	ImportCreatureTick();
	if (s_open_save_as) {
		ImGui::OpenPopup("Save World As");
		s_open_save_as = false;
	}
	if (ImGui::BeginPopupModal("Save World As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		static char s_name[64] = "";
		static std::string s_error;
		ImGui::Text("Name for the new world:");
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
			s_error.clear();
		}
		bool enter = ImGui::InputText("##saveasname", s_name, sizeof(s_name), ImGuiInputTextFlags_EnterReturnsTrue);
		if (!s_error.empty())
			ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "%s", s_error.c_str());
		if (ImGui::Button("Save") || enter) {
			if (CreateWorld(s_name, WorldFile(engine.currentworlddir), s_error)) {
				auto w = FindWorld(s_name);
				if (w && CopyWorldExtras(engine.currentworlddir, w->dir, s_error) && WorldSave::SaveWorldState(w->dir, s_error)) {
					engine.currentworld = w->name;
					engine.currentworlddir = w->dir;
					get_backend()->setWindowTitle(w->name + " - " FORK_NAME);
					s_name[0] = 0;
					ImGui::CloseCurrentPopup();
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			s_name[0] = 0;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	if (s_open_save_message) {
		ImGui::OpenPopup("Save World##message");
		s_open_save_message = false;
	}
	if (ImGui::BeginPopupModal("Save World##message", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted(s_save_message.c_str());
		if (ImGui::Button("OK"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// a save this version could not read was moved aside at the load: say so
	// before anything else (owner, 2026-11-21), the world held still meanwhile
	static bool s_set_aside_told = false;
	if (WorldSave::LastSetAside().happened && !s_set_aside_told && engine.worldready) {
		s_set_aside_told = true;
		s_set_aside_error.clear();
		s_set_aside_was_paused = world.paused;
		world.paused = true;
		ImGui::OpenPopup("Saved Game Kept Safe");
	}
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	if (ImGui::BeginPopupModal("Saved Game Kept Safe", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		auto& aside = WorldSave::LastSetAside();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34);
		if (aside.missing > 0) {
			ImGui::TextWrapped("%d %s of \"%s\" could not be loaded.", aside.missing, aside.missing == 1 ? "creature" : "creatures", engine.currentworld.c_str());
			ImGui::Spacing();
			ImGui::TextWrapped("Their breed's files may be missing or damaged: a breed pack removed, perhaps. The rest of the world is here.");
		} else {
			ImGui::TextWrapped("Zeetures could not read the saved game of \"%s\".", engine.currentworld.c_str());
			ImGui::Spacing();
			ImGui::TextWrapped("It may be damaged, or have been saved by a newer version of Zeetures. The world has started again from its original map.");
		}
		ImGui::Spacing();
		if (!aside.kept.empty()) {
			ImGui::TextWrapped("Your saved game has not been lost. It is kept in the world's folder as:");
			ImGui::TextColored(ImVec4(1, 0.85f, 0.45f, 1), "%s", aside.kept.filename().string().c_str());
		} else
			ImGui::TextWrapped("Your saved game has not been lost: it is still in place, and saving is switched off so that nothing can overwrite it.");
		if (!s_set_aside_error.empty())
			ImGui::TextColored(ImVec4(1, 0.5f, 0.45f, 1), "%s", s_set_aside_error.c_str());
		ImGui::PopTextWrapPos();
		ImGui::Separator();
		if (ImGui::Button(aside.missing > 0 ? "Quit Without Saving" : "Quit and Put It Back")) {
			if (WorldSave::PutBackSetAsideSave(s_set_aside_error)) {
				engine.autosave = false; // (nothing is written over it)
				engine.done = true;
			}
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(aside.missing > 0 ? "Closes Zeetures without saving, so that you can\nput the breed's files back and try again." : "Closes Zeetures without saving and puts the saved game back,\nso that another version can open it.");
		ImGui::SameLine();
		if (ImGui::Button("Open World Folder"))
		{
			std::string url;
			for (char ch : aside.worlddir.generic_string())
				url += ch == ' ' ? std::string("%20") : std::string(1, ch);
			// (a Windows path starts with its drive: file:///C:/...)
			SDL_OpenURL(((url.empty() || url[0] != '/' ? "file:///" : "file://") + url).c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button("Carry On")) {
			world.paused = s_set_aside_was_paused;
			ImGui::CloseCurrentPopup();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(aside.missing > 0 ? "Plays on without them.\nThe saved game with them in it stays in the folder." : "Plays the world from its original map.\nThe old saved game stays in the folder.");
		ImGui::EndPopup();
	}

	// New World: create one, then restart into it
	if (s_open_new_world) {
		ImGui::OpenPopup("New World");
		DrawNewWorldForm(true);
		s_open_new_world = false;
	}
	if (ImGui::BeginPopupModal("New World", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		std::string created = DrawNewWorldForm(false);
		ImGui::SameLine();
		bool cancel = ImGui::Button("Cancel");
		if (!created.empty()) {
			ImGui::CloseCurrentPopup();
			RelaunchIntoWorld(created);
		} else if (cancel) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	// Add Favorite Place: name the current camera position
	if (s_open_add_place) {
		ImGui::OpenPopup("Add Favourite Place");
		s_open_add_place = false;
	}
	if (ImGui::BeginPopupModal("Add Favourite Place", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		static char s_name[64] = "";
		ImGui::Text("Name for this place:");
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		bool enter = ImGui::InputText("##placename", s_name, sizeof(s_name), ImGuiInputTextFlags_EnterReturnsTrue);
		if ((ImGui::Button("OK") || enter) && s_name[0]) {
			while (world.favoriteplaces.size() < 6)
				world.favoriteplaces.push_back({"", 0, 0});
			for (auto& f : world.favoriteplaces) {
				if (f.name.empty()) {
					f.name = s_name;
					f.x = (int)engine.camera->getX();
					f.y = (int)engine.camera->getY();
					break;
				}
			}
			s_name[0] = 0;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			s_name[0] = 0;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

// The View, Camera and Sound menus' switches are kept between runs in the
// player's settings file (settings.txt in the user data folder, with Speech
// Bubbles and Autosave): read once at the first frame, written when the
// player changes one. Not read for headless tests that set their own view
// (OPENC2E_VIEW, OPENC2E_OPEN_KITS), so their pictures don't depend on it.
// ---- movable toolbars ------------------------------------------------------
// As in C2 (a Windows rebar): each toolbar is a band with a grip at its
// left; drag the grip to move the band along its row, onto another row, or
// above or below the others onto a row of its own (owner, 2026-11-12). A
// band that doesn't fit in the window wraps onto the next line. The layout
// is kept in settings.txt ("toolbar-layout", rows split by ';', bands by
// ','). Bands hidden under View > Toolbars keep their places.
enum { BAND_STANDARD, BAND_POINTER, BAND_APPLETS, BAND_FAVOURITES, BAND_COUNT };
static const char* const BAND_KEYS[BAND_COUNT] = {"standard", "pointer", "applets", "favourites"};
static const char* const BAND_NAMES[BAND_COUNT] = {"Standard", "Pointer", "Applets", "Favourites"};
static std::vector<std::vector<int>> s_band_rows = {{BAND_STANDARD, BAND_POINTER, BAND_APPLETS}, {BAND_FAVOURITES}};
static ImVec2 s_band_min[BAND_COUNT], s_band_max[BAND_COUNT]; // last frame (screen)
static bool s_band_drawn[BAND_COUNT];
static int s_drag_band = -1;
static const float GRIP_W = 9;

static bool BandShown(int b) {
	switch (b) {
		case BAND_STANDARD: return s_show_standard;
		case BAND_POINTER: return s_show_pointer;
		case BAND_APPLETS: return s_show_applets;
		default: return s_show_favorites;
	}
}

static std::string BandLayoutText() {
	std::string t;
	for (auto& row : s_band_rows) {
		if (!t.empty())
			t += ";";
		for (size_t i = 0; i < row.size(); i++)
			t += (i ? "," : "") + std::string(BAND_KEYS[row[i]]);
	}
	return t;
}

// reads a layout; bands it leaves out go on the last row, unknown names and
// repeats are dropped
static void ParseBandLayout(const std::string& text) {
	if (text.empty())
		return;
	std::vector<std::vector<int>> rows(1);
	bool seen[BAND_COUNT] = {};
	std::string name;
	auto flush = [&] {
		for (int b = 0; b < BAND_COUNT; b++)
			if (name == BAND_KEYS[b] && !seen[b]) {
				seen[b] = true;
				rows.back().push_back(b);
			}
		name.clear();
	};
	for (char ch : text + ";") {
		if (ch == ',' || ch == ';') {
			flush();
			if (ch == ';' && !rows.back().empty())
				rows.emplace_back();
		} else
			name += ch;
	}
	while (!rows.empty() && rows.back().empty())
		rows.pop_back();
	for (int b = 0; b < BAND_COUNT; b++)
		if (!seen[b]) {
			if (rows.empty())
				rows.emplace_back();
			rows.back().push_back(b);
		}
	s_band_rows = rows;
}

// where a band dropped at the mouse would go: before band `before` (in its
// row), after band `after` (at the end of its line), or on a new row first
// or last
struct BandDrop {
	int before = -1, after = -1;
	int newRow = 0; // -1 first, 1 last
	ImVec2 a, b; // the marker line (screen)
};

static BandDrop FindBandDrop(int dragged, ImVec2 m) {
	BandDrop d;
	float top = 1e9f, bottom = -1e9f, left = 1e9f, right = -1e9f;
	for (int b = 0; b < BAND_COUNT; b++)
		if (s_band_drawn[b]) {
			top = std::min(top, s_band_min[b].y);
			bottom = std::max(bottom, s_band_max[b].y);
			left = std::min(left, s_band_min[b].x);
			right = std::max(right, s_band_max[b].x);
		}
	if (top > bottom)
		return d;
	if (m.y < top + 3 || m.y > bottom + 4) { // above or below them all: a row of its own
		d.newRow = m.y < top + 3 ? -1 : 1;
		float y = d.newRow < 0 ? top - 1 : bottom + 2;
		d.a = ImVec2(left, y);
		d.b = ImVec2(right, y);
		return d;
	}
	// the line of bands nearest the mouse, by height
	int line = -1;
	float best = 1e9f;
	for (int b = 0; b < BAND_COUNT; b++) {
		if (!s_band_drawn[b])
			continue;
		float dy = m.y < s_band_min[b].y ? s_band_min[b].y - m.y : m.y > s_band_max[b].y ? m.y - s_band_max[b].y : 0;
		if (dy < best)
			best = dy, line = b;
	}
	float ly = s_band_min[line].y;
	int lastOnLine = -1;
	for (int b = 0; b < BAND_COUNT; b++) {
		if (!s_band_drawn[b] || std::fabs(s_band_min[b].y - ly) > 2)
			continue;
		if (m.x < (s_band_min[b].x + s_band_max[b].x) / 2 && (d.before < 0 || s_band_min[b].x < s_band_min[d.before].x))
			d.before = b;
		if (lastOnLine < 0 || s_band_min[b].x > s_band_min[lastOnLine].x)
			lastOnLine = b;
	}
	if (d.before >= 0) {
		float x = s_band_min[d.before].x - 2;
		d.a = ImVec2(x, s_band_min[d.before].y);
		d.b = ImVec2(x, s_band_max[d.before].y);
	} else {
		d.after = lastOnLine;
		float x = s_band_max[lastOnLine].x + 2;
		d.a = ImVec2(x, s_band_min[lastOnLine].y);
		d.b = ImVec2(x, s_band_max[lastOnLine].y);
	}
	if (d.before == dragged || d.after == dragged) // where it is already
		d.before = d.after = -1, d.newRow = 0, d.a = d.b = ImVec2(-1, -1);
	return d;
}

static void ApplyBandDrop(int dragged, const BandDrop& d) {
	if (d.before < 0 && d.after < 0 && d.newRow == 0)
		return;
	for (auto& row : s_band_rows)
		row.erase(std::remove(row.begin(), row.end(), dragged), row.end());
	if (d.newRow != 0)
		s_band_rows.insert(d.newRow < 0 ? s_band_rows.begin() : s_band_rows.end(), std::vector<int>{dragged});
	else
		for (auto& row : s_band_rows) {
			int anchor = d.before >= 0 ? d.before : d.after;
			auto it = std::find(row.begin(), row.end(), anchor);
			if (it == row.end())
				continue;
			row.insert(d.before >= 0 ? it : it + 1, dragged);
			break;
		}
	s_band_rows.erase(std::remove_if(s_band_rows.begin(), s_band_rows.end(), [](auto& r) { return r.empty(); }), s_band_rows.end());
	if (!devenv("OPENC2E_TEST_TOOLBAR_DRAG")) // (tests leave the player's settings alone)
		writePlayerSetting("toolbar-layout", BandLayoutText());
}

// the grip: two etched bars, as on C2's toolbars
static void DrawGrip(ImVec2 p, float h, bool hot) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImU32 light = hot ? IM_COL32(230, 236, 250, 255) : IM_COL32(150, 160, 180, 255);
	ImU32 dark = IM_COL32(20, 22, 28, 255);
	for (float x : {p.x + 2, p.x + 5}) {
		dl->AddLine(ImVec2(x, p.y + 3), ImVec2(x, p.y + h - 3), light);
		dl->AddLine(ImVec2(x + 1, p.y + 3), ImVec2(x + 1, p.y + h - 3), dark);
	}
}

static void DrawToolbarBands(const std::array<std::function<void()>, BAND_COUNT>& draw) {
	const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
	bool anyDrawn = false;
	bool drawnNow[BAND_COUNT] = {};
	for (auto& row : s_band_rows) {
		bool onLine = false;
		for (int b : row) {
			if (!BandShown(b))
				continue;
			float w = s_band_drawn[b] ? s_band_max[b].x - s_band_min[b].x : 0;
			if (onLine) {
				ImGui::SameLine(0, 6);
				if (ImGui::GetCursorScreenPos().x + w > right) // no room: wrap
					ImGui::NewLine();
			} else if (anyDrawn)
				ImGui::NewLine();
			onLine = anyDrawn = true;
			ImGui::BeginGroup();
			ImVec2 g = ImGui::GetCursorScreenPos();
			float h = s_band_drawn[b] ? s_band_max[b].y - s_band_min[b].y : ImGui::GetFrameHeight() + 6;
			ImGui::InvisibleButton(BAND_KEYS[b], ImVec2(GRIP_W, h));
			bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
			if (hovered || active)
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
			if (hovered && !active && s_drag_band < 0)
				ImGui::SetTooltip("%s toolbar: drag to move it", BAND_NAMES[b]);
			if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3))
				s_drag_band = b;
			DrawGrip(g, h, hovered || active);
			draw[b]();
			ImGui::EndGroup();
			s_band_min[b] = ImGui::GetItemRectMin();
			s_band_max[b] = ImGui::GetItemRectMax();
			drawnNow[b] = true;
		}
	}
	for (int b = 0; b < BAND_COUNT; b++)
		s_band_drawn[b] = drawnNow[b];
	// dragging: a marker where it would go; dropped: move it there
	if (s_drag_band >= 0) {
		BandDrop d = FindBandDrop(s_drag_band, ImGui::GetMousePos());
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			if (d.a.x >= 0)
				ImGui::GetForegroundDrawList()->AddLine(d.a, d.b, IM_COL32(255, 210, 90, 255), 3);
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
		} else {
			ApplyBandDrop(s_drag_band, d);
			s_drag_band = -1;
		}
	}
}

static void LoadViewSettings() {
	ParseBandLayout(readPlayerSetting("toolbar-layout"));
	s_show_standard = readFlag("toolbar-standard", s_show_standard);
	s_show_pointer = readFlag("toolbar-pointer", s_show_pointer);
	s_show_applets = readFlag("toolbar-applets", s_show_applets);
	s_show_favorites = readFlag("toolbar-favourites", s_show_favorites);
	s_text_labels = readFlag("text-labels", s_text_labels);
	s_object_tips = readFlag("object-tooltips", s_object_tips);
	s_smooth_scrolling = readFlag("smooth-scrolling", s_smooth_scrolling);
	SetC2StatusBarShown(readFlag("status-bar", IsC2StatusBarShown()));
	SetC2ScrollBarsShown(readFlag("scroll-bars", AreC2ScrollBarsShown()));
	bool music = readFlag("ambient-music", !engine.musicmanager->isMuted());
	engine.musicmanager->setMuted(!music);
	engine.musicmanager->setMIDIMuted(!music);
	soundmanager.setMuted(!readFlag("world-sounds", !soundmanager.isMuted()));
	soundmanager.setVoicesMutedByPlayer(!readFlag("bibble", !soundmanager.areVoicesMutedByPlayer()));
}

void DrawC2Toolbar() {
	GameFolder::pollChooseAnother(SDL_GetKeyboardFocus());
	// developer aid: OPENC2E_VIEW="labels,tips,smooth" starts with Text
	// Labels, ToolTips for Objects and Smooth Scrolling on (headless tests)
	static bool s_view_aid = [] {
		if (!devenv("OPENC2E_VIEW") && !devenv("OPENC2E_OPEN_KITS"))
			LoadViewSettings();
		// developer aid: OPENC2E_TOOLBAR_LAYOUT="applets;standard,pointer;favourites"
		if (const char* l = devenv("OPENC2E_TOOLBAR_LAYOUT"))
			ParseBandLayout(l);
		if (const char* v = devenv("OPENC2E_VIEW")) {
			std::string s = v;
			s_text_labels = s.find("labels") != std::string::npos;
			s_object_tips = s.find("tips") != std::string::npos;
			s_smooth_scrolling = s.find("smooth") != std::string::npos;
		}
		return true;
	}();
	(void)s_view_aid;
	ImGuiIO& io = ImGui::GetIO();
	// developer aid: OPENC2E_TEST_TOOLBAR_DRAG="band,x,y" drags that band's
	// grip to (x, y) in the window with the mouse, a few seconds in, and
	// prints the layout after (headless tests of moving toolbars)
	// developer aid: OPENC2E_TEST_UI_CLICKS="frame,x,y;frame,x,y" clicks the
	// left mouse button at those screen points on those frames (headless
	// tests of the toolbar's controls); prints the game speed after each
	if (const char* tc = devenv("OPENC2E_TEST_UI_CLICKS")) {
		static int s_uiframe = 0;
		s_uiframe++;
		std::string all = tc;
		size_t pos = 0;
		while (pos <= all.size()) {
			size_t semi = all.find(';', pos);
			std::string one = all.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
			int f = 0;
			float x = 0, y = 0;
			if (sscanf(one.c_str(), "%d,%f,%f", &f, &x, &y) == 3) {
				if (s_uiframe == f)
					io.AddMousePosEvent(x, y);
				if (s_uiframe == f + 2)
					io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				if (s_uiframe == f + 4)
					io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				if (s_uiframe == f + 8)
					fmt::print("* TEST UI CLICK at {:.0f},{:.0f}: speed {:g}\n", x, y, engine.speed);
			}
			if (semi == std::string::npos)
				break;
			pos = semi + 1;
		}
	}
	if (const char* td = devenv("OPENC2E_TEST_TOOLBAR_DRAG")) {
		static int s_frame = 0, s_band = -1;
		static ImVec2 s_from, s_to;
		s_frame++;
		if (s_frame == 120) {
			char key[32] = {};
			float x = 0, y = 0;
			if (sscanf(td, "%31[^,],%f,%f", key, &x, &y) == 3)
				for (int b = 0; b < BAND_COUNT; b++)
					if (std::string(key) == BAND_KEYS[b] && s_band_drawn[b]) {
						s_band = b;
						s_from = ImVec2(s_band_min[b].x + GRIP_W / 2, (s_band_min[b].y + s_band_max[b].y) / 2);
						s_to = ImVec2(x, y);
					}
			fmt::print("* TEST TOOLBAR DRAG: {} from {:.0f},{:.0f} to {:.0f},{:.0f}; layout {}\n", key, s_from.x, s_from.y, x, y, BandLayoutText());
		}
		if (s_band >= 0) {
			if (s_frame == 121)
				io.AddMousePosEvent(s_from.x, s_from.y);
			if (s_frame == 123)
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			if (s_frame >= 125 && s_frame <= 145) {
				float t = (s_frame - 125) / 20.0f;
				io.AddMousePosEvent(s_from.x + (s_to.x - s_from.x) * t, s_from.y + (s_to.y - s_from.y) * t);
			}
			if (s_frame == 150)
				io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			if (s_frame == 155)
				fmt::print("* TEST TOOLBAR DRAG: layout now {}\n", BandLayoutText());
		}
	}
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, 0));

	if (ImGui::Begin("MainMenuBar", nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
		if (ImGui::BeginMenuBar()) {
			DrawC2MenuBar();
			ImGui::EndMenuBar();
		}

		static Texture s_stdicons_tex;
		if (!s_stdicons_tex) {
			s_stdicons_tex = GetTextureFromExeFileWithTransparentTopLeft(0xe3);
		}

		TextureRect icon_next{s_stdicons_tex, 0, 0, 22, 21};
		TextureRect icon_eyeview{s_stdicons_tex, 22, 0, 22, 21};
		TextureRect icon_track{s_stdicons_tex, 44, 0, 22, 21};
		TextureRect icon_halo{s_stdicons_tex, 66, 0, 22, 21};
		TextureRect icon_play{s_stdicons_tex, 88, 0, 22, 21};
		TextureRect icon_pause{s_stdicons_tex, 110, 0, 22, 21};
		TextureRect icon_help{s_stdicons_tex, 132, 0, 22, 21};
		TextureRect icon_web{s_stdicons_tex, 154, 0, 22, 21};

		static Texture s_handicons_tex;
		if (!s_handicons_tex) {
			s_handicons_tex = GetTextureFromExeFileWithTransparentTopLeft(0xe4);
		}

		TextureRect icon_invisible{s_handicons_tex, 0, 0, 22, 21};
		TextureRect icon_teach{s_handicons_tex, 22, 0, 22, 21};
		TextureRect icon_push{s_handicons_tex, 44, 0, 22, 21};

		ImGui::GetCurrentWindow()->DC.LayoutType = ImGuiLayoutType_Horizontal;

		// each toolbar is a band (see DrawToolbarBands)
		auto bandStandard = [&] {
		if (ToolButton(icon_next, true, "Next")) {
			SelectNextCreature();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Next Creature: select the next creature in the world");
		if (ToolButton(icon_eyeview, (bool)world.selectedcreature, "Eye View"))
			SetEyeViewOpen(!IsEyeViewOpen());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Creature's Eye View: see the world as the selected creature sees it");
		if (ToolButton(icon_track, (bool)world.selectedcreature, "Follow")) {
			// toggle: follow the selected creature, or stop following
			if (engine.camera->trackedAgent() == world.selectedcreature)
				engine.camera->trackAgent(AgentRef(), 0, 0, brittle, none);
			else
				engine.camera->trackAgent(world.selectedcreature, 0, 0, flexible, none);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(engine.camera->trackedAgent() && engine.camera->trackedAgent() == world.selectedcreature ? "Follow: stop following the selected creature" : "Follow: keep the view on the selected creature");
		bool halo_on = HaloShown();
		if (ToolButton(icon_halo, CanToggleHalo(), "Halo"))
			ToggleHalo();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(halo_on ? "Halo: hide the halo" : "Halo: show a halo over the selected creature");
		ImGui::Separator();
		if (ToolButton(icon_play, world.paused, "Play")) {
			world.paused = false;
		};
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Play: let the world run");
		if (ToolButton(icon_pause, !world.paused, "Pause")) {
			world.paused = true;
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Pause: stop the world");
		ImGui::Separator();
		if (ToolButton(icon_help, true, "Help"))
			SetHelpOpen(true);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Help: how to play, what this version adds, and every kit (F1)");
		if (ToolButton(icon_web, true, "Web"))
			SDL_OpenURL(OPENC2E_WEB);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Web: openc2e on the web (the open-source engine this runs on)");
		};
		auto bandPointer = [&] {
		// the Pointer toolbar: an invisible hand (creatures do not see it)
		// as a switch, and the hand's two modes: teach (a click tickles or
		// slaps) and push (a click shoves a creature away from the hand)
		auto toggled = [](bool on, auto icon, const char* label) {
			if (on)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
			bool clicked = ToolButton(icon, true, label);
			if (on)
				ImGui::PopStyleColor();
			return clicked;
		};
		if (toggled(world.handInvisible, icon_invisible, "Invisible")) {
			world.handInvisible = !world.handInvisible;
			// see-through at once, even while the world is paused
			if (world.hand())
				if (auto* sp = dynamic_cast<SpritePart*>(world.hand()->part(0)))
					sp->setStippled(world.handInvisible);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(world.handInvisible ? "Invisible Hand: creatures can't see the hand (click to show it again)" : "Invisible Hand: hide the hand from creatures");
		if (toggled(!world.handPush, icon_teach, "Teach"))
			world.handPush = false;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Teach: click a creature's head to tickle it, lower down to slap it");
		if (toggled(world.handPush, icon_push, "Push"))
			world.handPush = true;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Push: click a creature to shove it away from the hand");
		};

		static Texture s_appleticons_tex;
		if (!s_appleticons_tex) {
			s_appleticons_tex = GetTextureFromExeFileWithTransparentTopLeft(0xe6);
		}

		TextureRect icon_hatchery{s_appleticons_tex, 0, 0, 22, 21};
		TextureRect icon_unknown_maybe_ecologykit{s_appleticons_tex, 22, 0, 22, 21};
		TextureRect icon_ownerskit{s_appleticons_tex, 44, 0, 22, 21};
		TextureRect icon_healthkit{s_appleticons_tex, 66, 0, 22, 21};
		TextureRect icon_unknown_maybe_sciencekit{s_appleticons_tex, 88, 0, 22, 21};
		TextureRect icon_unknown_maybe_breederskit{s_appleticons_tex, 110, 0, 22, 21};
		TextureRect icon_unknown_maybe_observationkit{s_appleticons_tex, 132, 0, 22, 21};
		TextureRect icon_agent_injector{s_appleticons_tex, 154, 0, 22, 21};
		TextureRect icon_unknown_maybe_historykit{s_appleticons_tex, 176, 0, 22, 21};
		TextureRect icon_graveyard{s_appleticons_tex, 198, 0, 22, 21};
		TextureRect icon_unknown_globe{s_appleticons_tex, 220, 0, 22, 21};
		TextureRect icon_unknown_magnifyingglass{s_appleticons_tex, 242, 0, 22, 21};
		TextureRect icon_unknown_maybe_neurosciencekit{s_appleticons_tex, 264, 0, 22, 21};

		auto bandApplets = [&] {
		if (ToolButton(icon_hatchery, IsHatcheryEnabled(), "Hatchery")) {
			SetHatcheryOpen(true);
		};
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Hatchery: choose eggs to hatch");
		if (ToolButton(icon_ownerskit, IsC2KitAvailable("owners"), "Owner's"))
			OpenC2Kit("owners");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Owner's Kit: name, photos, family, temperament, relationships and life story");
		if (ToolButton(icon_healthkit, IsC2KitAvailable("health"), "Health"))
			OpenC2Kit("health");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Health Kit: fitness, mind, drives and medicine");
		if (ToolButton(icon_graveyard, IsC2KitAvailable("graveyard"), "Graveyard"))
			OpenC2Kit("graveyard");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Graveyard Kit: the gravestones of the creatures that have died");
		ImGui::Separator();
		if (ToolButton(icon_unknown_maybe_breederskit, IsC2KitAvailable("breeders"), "Breeder's"))
			OpenC2Kit("breeders");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Breeder's Kit: fertility and pregnancy, aphrodisiacs and breeds");
		if (ToolButton(icon_unknown_maybe_sciencekit, IsC2KitAvailable("science"), "Science"))
			OpenC2Kit("science");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Science Kit: body, genes, biochemistry and the syringe");
		// (C2's Neuroscience Kit button opens our Mind Kit: our creatures
		// have no neurons to show; owner, 2026-11-11)
		if (ToolButton(icon_unknown_maybe_neurosciencekit, IsC2KitAvailable("mind"), "Mind"))
			OpenC2Kit("mind");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Mind Kit: what the creature feels, is weighing, has learned and remembers");
		if (ToolButton(icon_unknown_maybe_observationkit, IsC2KitAvailable("observation"), "Observation"))
			OpenC2Kit("observation");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Observation Kit: every creature at a glance, and alarms");
		ImGui::Separator();
		if (ToolButton(icon_agent_injector, IsAgentInjectorEnabled(), "Injector")) {
			SetAgentInjectorOpen(true);
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Agent Injector: add objects to the world");
		if (ToolButton(icon_unknown_maybe_historykit, IsC2KitAvailable("chronicles"), "Chronicles"))
			OpenC2Kit("chronicles");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Chronicles Kit: the world's history and statistics");
		if (ToolButton(icon_unknown_maybe_ecologykit, IsC2KitAvailable("ecology"), "Ecology"))
			OpenC2Kit("ecology");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Ecology Kit: the world's rooms (light, warmth, air, soil), watching rooms, and a census");
		};
		auto bandFavourites = [&] {

		static Texture s_favtoolbaricons_tex;
		if (!s_favtoolbaricons_tex) {
			s_favtoolbaricons_tex = GetTextureFromExeFileWithTransparentTopLeft(0xe5);
		}
		TextureRect icon_say{s_favtoolbaricons_tex, 0, 0, 22, 21};

		// ImGui::GetCurrentWindow()->DC.LayoutType = ImGuiLayoutType_Vertical;
		ImGui::Text("Speech History:");
		ImGui::SetNextItemWidth(150);
		// what creatures and the hand said lately (newest first); pick a
		// line and the lips button has the hand say it again
		static std::string s_line;
		if (ImGui::BeginCombo("##Speech History:", s_line.c_str())) {
			const auto& hist = speechHistory();
			if (hist.empty())
				ImGui::TextDisabled("Nothing said yet");
			for (size_t i = 0; i < hist.size(); i++) {
				const auto& h = hist[i];
				std::string who = !h.speaker ? "someone" : h.speaker.get() == (Agent*)world.hand() ? "You" : KitCreatureName(h.speaker.get());
				if (ImGui::Selectable(fmt::format("{}: {}##{}", who, h.text, i).c_str(), false))
					s_line = h.text;
			}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Speech History: what creatures and you said lately");
		if (ToolButton(icon_say, !s_line.empty(), "Say"))
			handSays(s_line);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Say: the hand says the chosen line again (creatures nearby hear it)");
		ImGui::Separator();

		ImGui::Text("Places:");
		ImGui::SetNextItemWidth(150);
		static int s_place = 0;
		std::vector<int> places; // indexes of named slots
		for (size_t i = 0; i < world.favoriteplaces.size(); i++)
			if (!world.favoriteplaces[i].name.empty())
				places.push_back((int)i);
		if (s_place >= (int)world.favoriteplaces.size() || (s_place < (int)world.favoriteplaces.size() && world.favoriteplaces[s_place].name.empty()))
			s_place = places.empty() ? 0 : places[0];
		const char* preview = places.empty() ? "" : world.favoriteplaces[s_place].name.c_str();
		bool placesOpen = ImGui::BeginCombo("##Places:", preview);
		if (ImGui::IsItemHovered() && !placesOpen)
			ImGui::SetTooltip("Places: your favourite places; choose one to go there");
		if (placesOpen) {
			for (int i : places) {
				if (ImGui::Selectable(world.favoriteplaces[i].name.c_str(), i == s_place)) {
					s_place = i;
					GoToFavoritePlace(i);
				}
			}
			ImGui::EndCombo();
		}
		// no Go button: choosing a place goes there; + adds one, as the
		// Camera menu does (owner, 2026-11-12)
		{
			bool free = HasFreePlace();
			ImGui::BeginDisabled(!free);
			float side = ImGui::GetFrameHeight(); // square, as tall as the Places list
			if (ImGui::Button("+##addplace", ImVec2(side, side)))
				s_open_add_place = true;
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip(free ? "Add a favourite place: name the spot the view shows now" : "All six favourite places are named; remove one in the Camera menu first");
		}
		// zoom, as a dropdown beside Places (owner, 2026-11-09; also
		// Ctrl+wheel and the View menu)
		ImGui::Text("Zoom:");
		ImGui::SetNextItemWidth(70);
		static const float s_zooms[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f};
		std::string zoomLabel = fmt::format("{:.0f}%", engine.zoom * 100);
		if (ImGui::BeginCombo("##Zoom", zoomLabel.c_str())) {
			for (float z : s_zooms)
				if (ImGui::Selectable(fmt::format("{:.0f}%", z * 100).c_str(), std::fabs(engine.zoom - z) < 0.01f))
					engine.setZoom(z);
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Zoom: the size of the view (also Ctrl+wheel; Ctrl+0 for actual size)");
		// game speed, beside the zoom (owner, 2026-11-14: on the toolbar,
		// not in a menu; arrows change it by 0.5 up to 10x, or type a
		// number): 0.5x (slow) to 10x
		{
			auto speedText = [](float s) { return fmt::format("{:g}x", std::round(s * 10.0f) / 10.0f); };
			auto setSpeed = [](float s) { engine.speed = std::clamp(std::round(s * 10.0f) / 10.0f, 0.5f, 10.0f); };
			ImGui::Text("Speed:");
			static char s_speedBuf[16] = "1x";
			static bool s_speedEditing = false;
			if (!s_speedEditing)
				snprintf(s_speedBuf, sizeof s_speedBuf, "%s", speedText(engine.speed).c_str());
			// one field with its arrows stacked inside its right end, a spin
			// box (owner, 2026-11-14: "stacked on top of each other and in
			// the same field as the one that has the number")
			const float arrowW = 15.0f;
			ImGui::SetNextItemWidth(46 + arrowW);
			ImGui::SetNextItemAllowOverlap(); // the arrows drawn over its right end take their own clicks
			if (ImGui::InputText("##Speed", s_speedBuf, sizeof s_speedBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
				std::string t = s_speedBuf;
				t.erase(std::remove_if(t.begin(), t.end(), [](char ch) { return ch == 'x' || ch == 'X' || ch == ' '; }), t.end());
				char* end = nullptr;
				float v = std::strtof(t.c_str(), &end);
				if (end != t.c_str() && v > 0)
					setSpeed(v);
			}
			s_speedEditing = ImGui::IsItemActive();
			if (ImGui::IsItemDeactivated())
				s_speedEditing = false;
			auto speedTip = [&]() {
				std::string tip = "Game speed: how fast the world runs (1x is normal). The arrows change it by 0.5, or type a number from 0.5 to 10.";
				if (engine.speed > 1.0f && engine.measuredSpeed < engine.speed * 0.9f)
					tip += fmt::format("\nThe world is busy and is running at about {:.1f}x.", engine.measuredSpeed);
				ImGui::SetTooltip("%s", tip.c_str());
			};
			ImVec2 fmin = ImGui::GetItemRectMin(), fmax = ImGui::GetItemRectMax();
			bool fieldHovered = ImGui::IsItemHovered();
			ImVec2 after = ImGui::GetCursorScreenPos();
			ImDrawList* dl = ImGui::GetWindowDrawList();
			float bx = fmax.x - arrowW, midY = (fmin.y + fmax.y) * 0.5f;
			// a thin line between the number and the arrows, and between the arrows
			ImU32 lineCol = ImGui::GetColorU32(ImGuiCol_Border);
			dl->AddLine(ImVec2(bx, fmin.y + 2), ImVec2(bx, fmax.y - 2), lineCol);
			dl->AddLine(ImVec2(bx + 2, midY), ImVec2(fmax.x - 2, midY), lineCol);
			bool anyArrowHovered = false;
			auto arrow = [&](const char* id, bool up, bool enabled, const char* tip) {
				ImVec2 a(bx + 1, up ? fmin.y + 1 : midY), b(fmax.x - 1, up ? midY : fmax.y - 1);
				ImGui::SetCursorScreenPos(a);
				ImGui::BeginDisabled(!enabled);
				bool pressed = ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y), ImGuiButtonFlags_PressedOnClick);
				ImGui::EndDisabled();
				bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
				anyArrowHovered |= hov;
				if (enabled && (hov || ImGui::IsItemActive()))
					dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered), 2.0f);
				float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f, r = 3.0f;
				ImU32 col = ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
				if (up)
					dl->AddTriangleFilled(ImVec2(cx - r, cy + r * 0.6f), ImVec2(cx + r, cy + r * 0.6f), ImVec2(cx, cy - r * 0.6f), col);
				else
					dl->AddTriangleFilled(ImVec2(cx - r, cy - r * 0.6f), ImVec2(cx + r, cy - r * 0.6f), ImVec2(cx, cy + r * 0.6f), col);
				if (hov)
					ImGui::SetTooltip("%s", tip);
				return pressed && enabled;
			};
			if (arrow("##speedup", true, engine.speed < 10.0f, "Faster (+0.5)"))
				setSpeed(engine.speed + 0.5f);
			if (arrow("##speeddown", false, engine.speed > 0.5f, "Slower (-0.5)"))
				setSpeed(engine.speed - 0.5f);
			if (fieldHovered && !anyArrowHovered)
				speedTip();
			ImGui::SetCursorScreenPos(after);
			ImGui::Dummy(ImVec2(0, 0));
		}
		};

		DrawToolbarBands({bandStandard, bandPointer, bandApplets, bandFavourites});

		s_last_c2toolbar_height = ImGui::GetWindowSize().y;
		ImGui::End();
	}
	DrawObjectToolTip();
}

} // namespace Openc2eImgui