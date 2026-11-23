#include "WorldChooser.h"
#include "common/devenv.h"

#include "Engine.h"
#include "PathResolver.h"
#include "creatures/CreatureSpeech.h"
#include "common/Ascii.h"
#include "common/backend/Backend.h"
#include "common/backend/BackendEvent.h"

#include <algorithm>
#include <fmt/core.h>
#include <imgui.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_process.h>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#ifdef __linux__
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace Openc2eImgui {

// C1/C2 worlds, each a folder under <user data>/Worlds/<name>/ holding the
// world file it starts from (world.sfc, copied from a game world file such as
// Eden.sfc) and, later, the saved state.

fs::path GetWorldsDirectory() {
	return getUserDataDir() / "Worlds";
}

fs::path WorldFile(const fs::path& worlddir) {
	return worlddir / "world.sfc";
}

std::vector<WorldEntry> ListWorlds() {
	std::vector<WorldEntry> result;
	std::error_code ec;
	for (auto& entry : fs::directory_iterator(GetWorldsDirectory(), ec)) {
		if (entry.is_directory() && fs::exists(WorldFile(entry.path())))
			result.push_back({entry.path().filename().string(), entry.path()});
	}
	std::sort(result.begin(), result.end(), [](const WorldEntry& a, const WorldEntry& b) {
		return to_ascii_lowercase(a.name) < to_ascii_lowercase(b.name);
	});
	return result;
}

std::optional<WorldEntry> FindWorld(const std::string& name) {
	for (auto& w : ListWorlds())
		if (to_ascii_lowercase(w.name) == to_ascii_lowercase(name))
			return w;
	return std::nullopt;
}

std::vector<fs::path> ListWorldTemplates() {
	// .sfc files in the game's own data folders (not the user folder);
	// Eden.sfc, the game's fresh starting world, first
	std::vector<fs::path> result;
	auto dirs = getMainDirectories();
	fs::path user = getUserDataDir();
	for (auto& dir : dirs) {
		if (dir == user)
			continue;
		std::error_code ec;
		for (auto& entry : fs::directory_iterator(dir, ec)) {
			if (entry.is_regular_file() && to_ascii_lowercase(entry.path().extension().string()) == ".sfc")
				result.push_back(entry.path());
		}
	}
	std::sort(result.begin(), result.end(), [](const fs::path& a, const fs::path& b) {
		bool aeden = to_ascii_lowercase(a.filename().string()) == "eden.sfc";
		bool beden = to_ascii_lowercase(b.filename().string()) == "eden.sfc";
		if (aeden != beden)
			return aeden;
		return a < b;
	});
	return result;
}

static bool ValidWorldName(const std::string& name, std::string& error) {
	if (name.empty()) {
		error = "Please give the world a name.";
		return false;
	}
	for (char c : name) {
		if (!(is_ascii_alnum(c) || c == ' ' || c == '-' || c == '_' || c == '\'')) {
			error = "Names can use letters, numbers, spaces, - _ and '.";
			return false;
		}
	}
	// (on every system, so worlds can be copied between them: Windows drops
	// spaces at the end of a folder's name, so "Home " would open Home's
	// folder, and it keeps some names for devices)
	if (name.front() == ' ' || name.back() == ' ') {
		error = "Names can't start or end with a space.";
		return false;
	}
	{
		std::string stem = to_ascii_lowercase(name);
		static const char* const reserved[] = {"con", "prn", "aux", "nul"};
		bool bad = std::find(std::begin(reserved), std::end(reserved), stem) != std::end(reserved);
		if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) && stem[3] >= '0' && stem[3] <= '9')
			bad = true;
		if (bad) {
			error = "That name is kept by Windows for devices; please choose another.";
			return false;
		}
	}
	if (FindWorld(name)) {
		error = "There is already a world with that name.";
		return false;
	}
	return true;
}

bool CreateWorld(const std::string& name, const fs::path& tmpl, std::string& error) {
	if (!ValidWorldName(name, error))
		return false;
	fs::path dir = GetWorldsDirectory() / name;
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (!ec)
		fs::copy_file(tmpl, WorldFile(dir), fs::copy_options::overwrite_existing, ec);
	if (ec) {
		error = fmt::format("Couldn't create the world: {}", ec.message());
		return false;
	}
	fmt::print("* Created world '{}' from {}\n", name, tmpl.filename().string());
	return true;
}

bool CopyWorldExtras(const fs::path& from, const fs::path& to, std::string& error) {
	for (const char* name : {"photos", "departed", "Additions"}) {
		std::error_code ec;
		if (!fs::is_directory(from / name, ec))
			continue;
		fs::copy(from / name, to / name, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
		if (ec) {
			error = fmt::format("Couldn't copy the world's {} folder: {}", name, ec.message());
			return false;
		}
	}
	return true;
}

// The command line to start this program again in the named world: the same
// options, with --world NAME in place of any --world given before.
static std::vector<std::string> RelaunchArgs(const std::string& name) {
	std::vector<std::string> args;
	const auto& orig = engine.getCommandLineArgs();
	for (size_t i = 0; i < orig.size(); i++) {
		if (orig[i] == "--world" && i + 1 < orig.size()) {
			i++;
			continue;
		}
		if (orig[i].rfind("--world=", 0) == 0)
			continue;
		args.push_back(orig[i]);
	}
	args.push_back("--world");
	args.push_back(name);
	return args;
}

// Windows and macOS (and Linux with OPENC2E_TEST_SPAWN_RELAUNCH set, to test
// this path): start a new copy of the program with SDL, after this one has
// saved and shut down, then leave. The program's own path is argv[0] made
// absolute (Windows and macOS give the full path when started from Explorer,
// Finder or a shortcut), or the base folder plus its name when it was found
// on the PATH.
static bool SpawnRelaunch(std::vector<std::string> args) {
	if (args.empty())
		return false;
	fs::path exe = args[0];
	std::error_code ec;
	if (!exe.has_parent_path()) {
		if (const char* base = SDL_GetBasePath())
			exe = fs::path(base) / exe;
	}
	exe = fs::absolute(exe, ec);
	if (ec)
		return false;
	args[0] = exe.string();
	std::vector<const char*> argv;
	for (auto& a : args)
		argv.push_back(a.c_str());
	argv.push_back(nullptr);
	SDL_Process* child = SDL_CreateProcess(argv.data(), false);
	if (!child) {
		fmt::print(stderr, "Couldn't start {}: {}\n", args[0], SDL_GetError());
		return false;
	}
	SDL_DestroyProcess(child); // (leaves it running)
	return true;
}

void RelaunchIntoWorld(const std::string& name) {
	std::vector<std::string> args = RelaunchArgs(name);
	fmt::print("* Switching to world '{}'\n", name);
	engine.shutdown();
#ifdef __linux__
	if (!devenv("OPENC2E_TEST_SPAWN_RELAUNCH")) {
		std::vector<char*> argv;
		for (auto& a : args)
			argv.push_back(a.data());
		argv.push_back(nullptr);
		std::fflush(stdout); // (exec doesn't flush)
		std::fflush(stderr);
		execv("/proc/self/exe", argv.data());
		fmt::print(stderr, "Couldn't restart into world '{}'\n", name);
		_exit(1);
	}
#endif
	bool ok = SpawnRelaunch(args);
	if (!ok)
		fmt::print(stderr, "Couldn't restart into world '{}': run again with --world \"{}\"\n", name, name);
	std::fflush(stdout); // (_Exit doesn't flush)
	std::fflush(stderr);
	std::_Exit(ok ? 0 : 1);
}

// The new-world form, shared by the start-up chooser and the File menu.
// Returns the created world's name, or "" while still being filled in.
// With reset = true it only clears the form and draws nothing.
std::string DrawNewWorldForm(bool reset) {
	static char s_name[64] = "";
	static int s_template = 0;
	static std::string s_error;
	static std::vector<fs::path> s_templates;
	static bool s_focus = false;
	if (reset)
		s_focus = true;
	if (reset || s_templates.empty()) {
		s_templates = ListWorldTemplates();
		s_template = 0;
		s_error.clear();
		if (reset)
			s_name[0] = 0;
	}
	if (reset)
		return ""; // reset only; may be called outside an ImGui frame

	ImGui::Text("World name:");
	ImGui::SetNextItemWidth(260);
	if (ImGui::IsWindowAppearing() || s_focus)
		ImGui::SetKeyboardFocusHere(); // (ready to type the name)
	s_focus = false;
	bool enter = ImGui::InputText("##worldname", s_name, sizeof(s_name), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::Text("Start from:");
	ImGui::SetNextItemWidth(260);
	std::string preview = s_templates.empty() ? "(no world files found)" : s_templates[s_template].filename().string();
	if (ImGui::BeginCombo("##template", preview.c_str())) {
		for (int i = 0; i < (int)s_templates.size(); i++) {
			std::string label = s_templates[i].filename().string();
			if (to_ascii_lowercase(label) == "eden.sfc")
				label += "  (a fresh Albia)";
			if (ImGui::Selectable(label.c_str(), i == s_template))
				s_template = i;
		}
		ImGui::EndCombo();
	}
	if (!s_error.empty())
		ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "%s", s_error.c_str());
	if ((ImGui::Button("Create World") || enter) && !s_templates.empty()) {
		if (CreateWorld(s_name, s_templates[s_template], s_error)) {
			std::string created = s_name;
			s_name[0] = 0;
			return created;
		}
	}
	return "";
}

// (a setting is one word, and a world's name may have spaces: kept as slashes,
// which no name has)
static std::string worldSetting(const std::string& key) {
	std::string v = readPlayerSetting(key);
	std::replace(v.begin(), v.end(), '/', ' ');
	return v;
}

static void setWorldSetting(const std::string& key, std::string name) {
	std::replace(name.begin(), name.end(), ' ', '/');
	writePlayerSetting(key, name);
}

std::string DefaultWorld() {
	return worldSetting("default-world");
}

void SetDefaultWorld(const std::string& name) {
	setWorldSetting("default-world", name);
}

// The world opened without asking, if the player has chosen one (the
// chooser's checkbox, File > Open This World at Launch; a player's wish
// passed on by the owner, 2026-11-22). While it opens, a setting says so: if
// that start never finished, the next one shows the chooser, so a world that
// will not open can be got away from.
void DefaultWorldOpened() {
	if (!readPlayerSetting("default-world-opening").empty())
		writePlayerSetting("default-world-opening", "");
}

std::string RunWorldChooser() {
	std::string chosen;
	bool quit = false;
	bool creating = ListWorlds().empty();
	std::string byDefault = DefaultWorld();
	if (!byDefault.empty() && !FindWorld(byDefault))
		byDefault.clear(); // (renamed or deleted since)
	const bool failed = !byDefault.empty() && worldSetting("default-world-opening") == byDefault;
	if (!byDefault.empty() && !failed) {
		setWorldSetting("default-world-opening", byDefault);
		return byDefault;
	}
	DefaultWorldOpened();
	bool always = failed; // (kept ticked, not lost)
	DrawNewWorldForm(true);

	get_backend()->run([&] {
		BackendEvent e;
		while (get_backend()->pollEvent(e)) {
			if (e.type == eventquit)
				quit = true;
		}
		if (quit)
			return false;
		get_backend()->getMainRenderTarget()->renderClear();

		ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextFrameWantCaptureMouse(true); // (the pointer shows everywhere: there is no hand yet)
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x / 2, io.DisplaySize.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(360, 0));
		ImGui::Begin("Choose a World", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
		static int s_selected = 0;
		auto worlds = ListWorlds();
		if (static bool first = true; first) {
			first = false;
			for (int i = 0; i < (int)worlds.size(); i++)
				if (worlds[i].name == byDefault)
					s_selected = i;
		}
		if (!creating && !worlds.empty()) {
			if (failed) {
				ImGui::TextWrapped("%s was not opened by itself this time, as Zeetures closed while opening it the last time.", byDefault.c_str());
				ImGui::Spacing();
			}
			ImGui::Text("Your worlds:");
			if (ImGui::BeginListBox("##worlds", ImVec2(-1, 160))) {
				for (int i = 0; i < (int)worlds.size(); i++) {
					if (ImGui::Selectable(worlds[i].name.c_str(), i == s_selected, ImGuiSelectableFlags_AllowDoubleClick)) {
						s_selected = i;
						if (ImGui::IsMouseDoubleClicked(0))
							chosen = worlds[i].name;
					}
				}
				ImGui::EndListBox();
			}
			s_selected = std::clamp(s_selected, 0, (int)worlds.size() - 1);
			ImGui::Checkbox("Always open this world at launch", &always);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Skips this window from now on.\nFile > Open This World at Launch turns it off again.");
			if (ImGui::Button("Open"))
				chosen = worlds[s_selected].name;
			ImGui::SameLine();
			if (ImGui::Button("New World..."))
				creating = true;
			ImGui::SameLine();
			if (ImGui::Button("Quit"))
				quit = true;
		} else {
			ImGui::TextWrapped(worlds.empty() ? "Welcome! Name your first world." : "New world");
			ImGui::Separator();
			std::string created = DrawNewWorldForm(false);
			if (!created.empty())
				chosen = created;
			if (!worlds.empty()) {
				ImGui::SameLine();
				if (ImGui::Button("Back"))
					creating = false;
			}
			ImGui::SameLine();
			if (ImGui::Button("Quit"))
				quit = true;
		}
		ImGui::End();
		return chosen.empty() && !quit;
	});

	if (quit)
		return "";
	if (always) {
		SetDefaultWorld(chosen);
		setWorldSetting("default-world-opening", chosen);
	} else if (!DefaultWorld().empty())
		SetDefaultWorld("");
	return chosen;
}

} // namespace Openc2eImgui
