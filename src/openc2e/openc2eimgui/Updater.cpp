// Updating Zeetures from inside the game (owner, 2026-11-22: "update from the
// app ... check for updates periodically and then prompt to update ... a
// setting to turn that update check off, and to turn auto-updates (i.e.,
// automatically download and install updates) off and on").
//
// Each nightly release carries zeetures-update.txt (the nightly workflow
// writes it):
//
//   version 0.33.0
//   file Zeetures-Setup.exe 31456789 <sha256 in hex>
//   ...
//
// listing only the files built that night (a download kept from an earlier
// night is an older version, and is left out). The game fetches it with the
// system's curl, as it fetches the breed packs: at start and every six hours
// while it runs, unless Help > Check for Updates Automatically is off, and
// whenever the player chooses Help > Check for Updates.
//
// A newer version is offered in a small window; with Help > Install Updates
// Automatically on (off to begin with) it is downloaded without asking. The
// download is checked against its SHA-256 and kept in the user folder's
// Updates folder, and installed as the game ends, after the world is saved:
//   - the Linux AppImage: the new file takes the old one's place ($APPIMAGE);
//   - Windows, installed with Zeetures-Setup.exe: the new installer runs on
//     its own (/SILENT), waits for the game to close (the ZeeturesRunning
//     mutex), and starts the game again if asked (/RELAUNCH);
//   - the Mac app: a small script waits for the game to close, copies the
//     new Zeetures.app out of the disk image in place of the old one, and
//     opens it; if it cannot, it opens the disk image for the player.
// Other copies (the .zip and .tar.gz, a build of one's own) can only be
// told that a new version is out, with a button to the download page.
//
// Player-visible text: British spelling.

#include "Updater.h"

#include "Engine.h"
#include "PathResolver.h"
#include "common/devenv.h"
#include "creatures/CreatureSpeech.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <imgui.h>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// (after windows.h, which it needs)
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace Openc2eImgui {

const char* ZeeturesVersion(); // (C2ToolBar.cpp)

namespace {

const char* const kReleaseUrl = "https://github.com/DotairZee/zeetures/releases/download/nightly/";
const char* const kPageUrl = "https://github.com/DotairZee/zeetures/releases/tag/nightly";
const char* const kChangesUrl = "https://github.com/DotairZee/zeetures/blob/creature-mind/CHANGELOG.md";
const double kEvery = 6 * 3600.0; // seconds between checks

std::string baseUrl() {
	// developer aid: OPENC2E_UPDATE_URL=http://127.0.0.1:8000/ checks a test server
	if (const char* u = devenv("OPENC2E_UPDATE_URL"))
		return u;
	return kReleaseUrl;
}

// ---- SHA-256 (FIPS 180-4) ---------------------------------------------------------

struct Sha256 {
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	uint8_t block[64];
	size_t used = 0;
	uint64_t bits = 0;
	static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
	void compress() {
		static const uint32_t k[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
			0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
			0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb,
			0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f,
			0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
		}
		h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
	}
	void add(const uint8_t* p, size_t n) {
		bits += (uint64_t)n * 8;
		while (n--) {
			block[used++] = *p++;
			if (used == 64) {
				compress();
				used = 0;
			}
		}
	}
	std::string hex() {
		uint64_t total = bits;
		uint8_t one = 0x80, zero = 0;
		add(&one, 1);
		while (used != 56)
			add(&zero, 1);
		for (int i = 7; i >= 0; i--) {
			uint8_t b = (uint8_t)(total >> (i * 8));
			add(&b, 1);
		}
		std::string out;
		for (uint32_t v : h)
			out += fmt::format("{:08x}", v);
		return out;
	}
};

std::string sha256Of(const fs::path& p) {
	std::ifstream in(p, std::ios::binary);
	if (!in)
		return "";
	Sha256 s;
	std::vector<char> buf(1 << 16);
	while (in) {
		in.read(buf.data(), buf.size());
		s.add(reinterpret_cast<const uint8_t*>(buf.data()), (size_t)in.gcount());
	}
	return s.hex();
}

// ---- downloading --------------------------------------------------------------------

#ifndef _WIN32
std::string quoted(const std::string& s) {
	std::string q = "'";
	for (char c : s)
		q += c == '\'' ? std::string("'\\''") : std::string(1, c);
	return q + "'";
}
#endif

// the system's curl, as the breed packs are fetched (BreedPacks.cpp)
bool curl(const std::string& url, const fs::path& to, int seconds) {
	std::error_code ec;
	fs::remove(to, ec);
#ifdef _WIN32
	auto wide = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
	std::wstring cmd = L"curl.exe -fsSL --max-time " + std::to_wstring(seconds) + L" -A Zeetures -o \"" + to.wstring() + L"\" \"" + wide(url) + L"\"";
	int rc = -1;
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD code = 1;
		if (GetExitCodeProcess(pi.hProcess, &code))
			rc = (int)code;
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
#else
	int rc = std::system(fmt::format("curl -fsSL --max-time {} -A Zeetures -o {} {}", seconds, quoted(to.string()), quoted(url)).c_str());
#endif
	return rc == 0 && fs::exists(to, ec);
}

// ---- this copy of the game ----------------------------------------------------------

enum class Kind { none, appimage, windows, mac };
struct Install {
	Kind kind = Kind::none;
	std::string file; // the release's file for it
	fs::path where; // the AppImage, the program's folder, or the .app
	bool allUsers = false; // Windows: installed for every user
};

Install thisInstall() {
	Install in;
#if defined(_WIN32)
	// installed by Zeetures-Setup.exe: its uninstaller is beside the program
	const char* base = SDL_GetBasePath();
	if (!base)
		return in;
	fs::path dir = fs::path(reinterpret_cast<const char8_t*>(base));
	std::error_code ec;
	if (!fs::exists(dir / "unins000.exe", ec))
		return in;
	in.kind = Kind::windows;
	in.file = "Zeetures-Setup.exe";
	in.where = dir;
	wchar_t pf[MAX_PATH];
	if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
		std::wstring d = dir.wstring(), p = pf;
		in.allUsers = _wcsnicmp(d.c_str(), p.c_str(), p.size()) == 0;
	}
#elif defined(__APPLE__)
	const char* base = SDL_GetBasePath();
	if (!base)
		return in;
	for (fs::path p = base; !p.empty() && p != p.parent_path(); p = p.parent_path())
		if (p.extension() == ".app") {
			in.kind = Kind::mac;
			in.file = "Zeetures-macOS.dmg";
			in.where = p;
			break;
		}
#else
	if (const char* ai = getenv("APPIMAGE"); ai && *ai) {
		in.kind = Kind::appimage;
		in.file = sizeof(void*) == 8 ? "Zeetures-x86_64.AppImage" : "Zeetures-i686.AppImage";
		in.where = ai;
	}
#endif
	return in;
}

const Install& install() {
	static const Install in = thisInstall();
	return in;
}

// "0.32.1" < "0.33.0"
bool newer(const std::string& a, const std::string& b) {
	auto parts = [](const std::string& v) {
		std::vector<int> out;
		std::istringstream in(v);
		for (std::string p; std::getline(in, p, '.');)
			out.push_back(std::atoi(p.c_str()));
		return out;
	};
	return parts(a) > parts(b);
}

// ---- the state, shared with the check's and the download's threads ----------------

enum class State { idle, checking, upToDate, available, downloading, ready, failed };
std::mutex s_mutex;
State s_state = State::idle;
std::string s_version, s_sha, s_message;
uint64_t s_size = 0;
fs::path s_download; // the checked file, ready to install
bool s_manual = false; // the player asked: say so even when there is nothing
bool s_window = false; // the update window is shown
std::string s_skipped; // "Not now" for this version, this run
bool s_installAtExit = false, s_restart = false;
double s_lastCheck = -1e9;
std::atomic<bool> s_busy{false};

fs::path updatesDir() {
	return getUserDataDir() / "Updates";
}

bool setting(const char* key, bool fallback) {
	std::string v = readPlayerSetting(key);
	return v.empty() ? fallback : v != "0";
}

bool checkOn() {
	return setting("update-check", true);
}

bool autoOn() {
	return setting("update-auto", false);
}

void check() {
	fs::path dir = updatesDir();
	std::error_code ec;
	fs::create_directories(dir, ec);
	fs::path f = dir / "zeetures-update.txt";
	bool ok = curl(baseUrl() + "zeetures-update.txt", f, 30);
	std::string version, sha;
	uint64_t size = 0;
	if (ok) {
		std::ifstream in(f);
		for (std::string line; std::getline(in, line);) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			std::istringstream l(line);
			std::string key, name;
			l >> key;
			if (key == "version")
				l >> version;
			else if (key == "file") {
				uint64_t n = 0;
				std::string h;
				if (l >> name >> n >> h && name == install().file)
					size = n, sha = h;
			}
		}
	}
	fs::remove(f, ec);
	std::lock_guard<std::mutex> lock(s_mutex);
	if (!ok || version.empty()) {
		s_state = State::failed;
		s_message = "Zeetures could not reach GitHub to check for a new version.";
	} else if (!newer(version, ZeeturesVersion())) {
		s_state = State::upToDate;
	} else {
		s_state = State::available;
		s_version = version;
		s_size = size;
		s_sha = sha;
	}
	fmt::print("* Update check: {} (this is {})\n", ok ? (version.empty() ? "no version given" : version) : "no answer", ZeeturesVersion());
}

void download() {
	std::string version, sha;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		version = s_version;
		sha = s_sha;
		s_state = State::downloading;
	}
	fs::path dir = updatesDir();
	std::error_code ec;
	fs::create_directories(dir, ec);
	// (older downloads are of no use now)
	for (auto& e : fs::directory_iterator(dir, ec))
		if (e.path().filename().string().rfind("Zeetures", 0) == 0)
			fs::remove(e.path(), ec);
	fs::path part = dir / (install().file + ".part");
	bool ok = curl(baseUrl() + install().file, part, 1800);
	bool good = ok && !sha.empty() && sha256Of(part) == sha;
	fs::path done = dir / install().file;
	if (good)
		fs::rename(part, done, ec);
	else
		fs::remove(part, ec);
	std::lock_guard<std::mutex> lock(s_mutex);
	if (good && !ec) {
		s_state = State::ready;
		s_download = done;
		fmt::print("* Update: Zeetures {} downloaded and checked ({})\n", version, done.string());
	} else {
		s_state = State::failed;
		s_message = ok ? "The download did not match what it should be, so it was thrown away." : "Zeetures could not download the new version.";
		fmt::print("* Update: download of {} failed ({})\n", version, ok ? "checksum" : "curl");
	}
}

void startThread(void (*job)()) {
	if (s_busy.exchange(true))
		return;
	std::thread([job] {
		job();
		s_busy = false;
	}).detach();
}

std::string sizeText(uint64_t n) {
	return n >= 1024 * 1024 ? fmt::format("{:.0f} MB", n / (1024.0 * 1024.0)) : fmt::format("{:.0f} KB", (std::max)(1.0, n / 1024.0));
}

void drawWindow() {
	State state;
	std::string version, message;
	uint64_t size;
	bool canInstall;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		state = s_state;
		version = s_version;
		message = s_message;
		size = s_size;
		canInstall = install().kind != Kind::none && s_sha.size() == 64;
	}
	const ImVec2 screen = ImGui::GetMainViewport()->WorkSize;
	ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, 110), ImGuiCond_Appearing, ImVec2(0.5f, 0));
	ImGui::SetNextWindowSizeConstraints(ImVec2(420, 0), ImVec2(520, FLT_MAX));
	if (!ImGui::Begin("Zeetures Update", &s_window, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	ImGui::PushTextWrapPos(500);
	const ImVec4 gold(1.0f, 0.88f, 0.55f, 1);
	switch (state) {
		case State::idle:
		case State::checking:
			ImGui::TextUnformatted("Looking for a new version...");
			break;
		case State::upToDate:
			ImGui::Text("You have the newest version of Zeetures (%s).", ZeeturesVersion());
			break;
		case State::failed:
			ImGui::TextUnformatted(message.c_str());
			if (ImGui::Button("Try Again"))
				startThread(version.empty() ? check : download);
			ImGui::SameLine();
			if (ImGui::Button("Open the Download Page"))
				SDL_OpenURL(kPageUrl);
			break;
		case State::available:
			ImGui::TextColored(gold, "Zeetures %s is out.", version.c_str());
			ImGui::Text("You have %s. Your worlds are kept as they are.", ZeeturesVersion());
			if (canInstall) {
				if (ImGui::Button(fmt::format("Update Now ({})", sizeText(size)).c_str()))
					startThread(download);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Downloads it now; you choose when to restart");
			} else {
				ImGui::TextDisabled("This copy cannot update itself (it was unpacked from a .zip or .tar.gz, or built from the source).");
				if (ImGui::Button("Open the Download Page"))
					SDL_OpenURL(kPageUrl);
			}
			ImGui::SameLine();
			if (ImGui::Button("What's New"))
				SDL_OpenURL(kChangesUrl);
			ImGui::SameLine();
			if (ImGui::Button("Not Now")) {
				s_skipped = version;
				s_window = false;
			}
			break;
		case State::downloading:
			ImGui::Text("Downloading Zeetures %s (%s)...", version.c_str(), sizeText(size).c_str());
			ImGui::TextDisabled("You can play on meanwhile.");
			break;
		case State::ready:
			ImGui::TextColored(gold, "Zeetures %s is downloaded.", version.c_str());
			if (s_installAtExit)
				ImGui::TextUnformatted("It will be installed when you quit.");
			else
				ImGui::TextUnformatted("To install it, Zeetures saves your world, closes, and starts again.");
			if (ImGui::Button("Restart Now")) {
				s_installAtExit = true;
				s_restart = true;
				engine.done = true;
			}
			if (!s_installAtExit) {
				ImGui::SameLine();
				if (ImGui::Button("When I Quit")) {
					s_installAtExit = true;
					s_window = false;
				}
			}
			break;
	}
	ImGui::PopTextWrapPos();
	ImGui::Separator();
	bool on = checkOn(), au = autoOn();
	if (ImGui::Checkbox("Check for updates", &on))
		writePlayerSetting("update-check", on ? "1" : "0");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("When Zeetures starts, and every six hours while it runs");
	ImGui::SameLine();
	ImGui::BeginDisabled(!on || !canInstall);
	if (ImGui::Checkbox("Install updates automatically", &au))
		writePlayerSetting("update-auto", au ? "1" : "0");
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Download new versions without asking, and install them when you quit");
	ImGui::End();
}

} // namespace

void UpdaterTick() {
	if (engine.version != 2)
		return;
	double now = ImGui::GetTime();
	// the periodic check (not in developer builds unless pointed at a test
	// server: tests stay off the network)
#ifdef OPENC2E_DEV_HOOKS
	const bool periodic = devenv("OPENC2E_UPDATE_URL") != nullptr;
#else
	const bool periodic = true;
#endif
#ifdef _WIN32
	// (the installer waits while this is held: Updater's /SILENT install)
	static HANDLE running = CreateMutexW(nullptr, FALSE, L"ZeeturesRunning");
	(void)running;
#endif
	if (periodic && checkOn() && now > 20 && now - s_lastCheck >= kEvery && !s_busy) {
		s_lastCheck = now;
		bool go = false;
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			if (s_state != State::downloading && s_state != State::ready) {
				s_state = State::checking;
				go = true;
			}
		}
		if (go) {
			s_manual = false;
			startThread(check);
		}
	}
	State state;
	std::string version;
	bool canInstall;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		state = s_state;
		version = s_version;
		canInstall = install().kind != Kind::none && s_sha.size() == 64;
	}
	// what a check found: an automatic one says nothing unless there is
	// something new, and with automatic installing it downloads it at once
	static State seen = State::idle;
	if (state != seen) {
		seen = state;
		if (state == State::available && version != s_skipped) {
			if (autoOn() && canInstall && !s_manual) {
				s_installAtExit = true;
				startThread(download);
			} else
				s_window = true;
		} else if (state == State::ready && s_installAtExit && !s_window) {
			// (installed automatically: a word that it will be)
			s_window = true;
		} else if ((state == State::upToDate || state == State::failed) && s_manual)
			s_window = true;
	}
	if (s_window)
		drawWindow();
}

void CheckForUpdatesNow() {
	s_manual = true;
	s_window = true;
	std::lock_guard<std::mutex> lock(s_mutex);
	if (s_state != State::downloading && s_state != State::ready && !s_busy) {
		s_state = State::checking;
		startThread(check);
	}
}

void UpdaterMenuItems() {
	if (ImGui::MenuItem("Check for Updates..."))
		CheckForUpdatesNow();
	bool on = checkOn(), au = autoOn();
	if (ImGui::MenuItem("Check for Updates Automatically", nullptr, on))
		writePlayerSetting("update-check", on ? "0" : "1");
	if (ImGui::MenuItem("Install Updates Automatically", nullptr, au, on && install().kind != Kind::none))
		writePlayerSetting("update-auto", au ? "0" : "1");
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip(install().kind == Kind::none ? "This copy cannot update itself: get new versions from the download page"
													   : "Download new versions without asking, and install them when you quit");
}

void UpdaterAtExit() {
	fs::path file;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (s_state != State::ready || !s_installAtExit)
			return;
		file = s_download;
	}
	const Install& in = install();
	const std::string world = engine.currentworld;
	std::error_code ec;
	fmt::print("* Update: installing {}{}\n", file.string(), s_restart ? ", then starting again" : "");
	std::fflush(stdout);
	switch (in.kind) {
		case Kind::appimage: {
#ifndef _WIN32
			// the new file in the old one's place (the running copy goes on
			// from its own mounted image), executable as the old one was
			fs::path next = in.where;
			next += ".new";
			fs::copy_file(file, next, fs::copy_options::overwrite_existing, ec);
			if (!ec)
				fs::permissions(next, fs::status(in.where, ec).permissions() | fs::perms::owner_exec, ec);
			if (!ec)
				fs::rename(next, in.where, ec);
			if (ec) {
				fmt::print(stderr, "* Update: couldn't replace {}: {}\n", in.where.string(), ec.message());
				return;
			}
			fs::remove(file, ec);
			if (s_restart) {
				std::string self = in.where.string();
				std::vector<std::string> args = {self};
				if (!world.empty())
					args.insert(args.end(), {"--world", world});
				std::vector<const char*> argv;
				for (auto& a : args)
					argv.push_back(a.c_str());
				argv.push_back(nullptr);
				if (SDL_Process* p = SDL_CreateProcess(argv.data(), false))
					SDL_DestroyProcess(p);
			}
#endif
			break;
		}
		case Kind::windows: {
#ifdef _WIN32
			// the installer waits for the game to close (the ZeeturesRunning
			// mutex) and, with /RELAUNCH, starts it again in this world
			std::wstring params = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCANCEL ";
			params += in.allUsers ? L"/ALLUSERS" : L"/CURRENTUSER";
			if (s_restart) {
				params += L" /RELAUNCH=1";
				if (!world.empty()) {
					int n = MultiByteToWideChar(CP_UTF8, 0, world.c_str(), -1, nullptr, 0);
					std::wstring w(n > 0 ? n - 1 : 0, L'\0');
					if (n > 1)
						MultiByteToWideChar(CP_UTF8, 0, world.c_str(), -1, w.data(), n);
					params += L" \"/WORLD=" + w + L"\"";
				}
			}
			std::wstring exe = file.wstring();
			SHELLEXECUTEINFOW sei{};
			sei.cbSize = sizeof(sei);
			sei.fMask = SEE_MASK_NOASYNC;
			sei.lpVerb = L"open";
			sei.lpFile = exe.c_str();
			sei.lpParameters = params.c_str();
			sei.nShow = SW_SHOWNORMAL;
			if (!ShellExecuteExW(&sei))
				fmt::print(stderr, "* Update: couldn't start the installer ({})\n", (unsigned long)GetLastError());
#endif
			break;
		}
		case Kind::mac: {
#ifdef __APPLE__
			// a script waits for this program to end, then puts the new app
			// in the old one's place; if it cannot, it opens the disk image
			fs::path script = updatesDir() / "install-update.sh";
			std::ofstream s(script, std::ios::binary);
			auto q = [](const std::string& t) { return quoted(t); };
			std::string app = in.where.string(), dmg = file.string();
			s << "#!/bin/sh\n"
			  << "while kill -0 " << getpid() << " 2>/dev/null; do sleep 0.5; done\n"
			  << "mnt=$(mktemp -d)\n"
			  << "if hdiutil attach -nobrowse -readonly -mountpoint \"$mnt\" " << q(dmg) << " >/dev/null"
			  << " && rm -rf " << q(app + ".new") << " && ditto \"$mnt/Zeetures.app\" " << q(app + ".new")
			  << " && rm -rf " << q(app) << " && mv " << q(app + ".new") << " " << q(app) << "; then\n"
			  << "  hdiutil detach \"$mnt\" >/dev/null 2>&1; rmdir \"$mnt\"; rm -f " << q(dmg) << "\n"
			  << (s_restart ? "  open " + q(app) + (world.empty() ? "" : " --args --world " + q(world)) + "\n" : std::string())
			  << "else\n"
			  << "  hdiutil detach \"$mnt\" >/dev/null 2>&1; open " << q(dmg) << "\n"
			  << "fi\n";
			s.close();
			std::system(("/bin/sh " + q(script.string()) + " >/dev/null 2>&1 &").c_str());
#endif
			break;
		}
		case Kind::none:
			break;
	}
	(void)ec;
	(void)world;
}

} // namespace Openc2eImgui
