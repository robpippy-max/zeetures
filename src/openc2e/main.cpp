/*
 *  main.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Wed 02 Jun 2004.
 *  Copyright (c) 2004-2008 Alyssa Milburn. All rights reserved.
 *  Copyright (c) 2005-2008 Bryan Donlan. All rights reserved.
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

#include "Engine.h"
#include "common/devenv.h"
#include "PathResolver.h"
#include "common/backtrace.h"
#include "common/Exception.h"
#include "sdlbackend/SDLBackend.h"
#include "sdlbackend/SDLMixerBackend.h"
#include "version.h"
#include "openc2eimgui/Updater.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_timer.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/core.h>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__linux__)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/time.h>
// developer aid: OPENC2E_SAMPLE=FILE, a sampling profiler (2026-11-16: the
// system's perf is not open to users here). Every 2 ms of CPU time the call
// stack is recorded; at exit each sample goes to FILE as one line of
// frames, the program's own as offsets ("+0x1234", for addr2line) and
// others by library name.
namespace {
constexpr int kMaxSamples = 60000, kDepth = 24;
void* g_samples[kMaxSamples][kDepth];
int g_depths[kMaxSamples];
std::atomic<int> g_nsamples{0};
void sampleHandler(int) {
	int i = g_nsamples.fetch_add(1);
	if (i < kMaxSamples)
		g_depths[i] = backtrace(g_samples[i], kDepth);
}
void dumpSamples() {
	const char* path = devenv("OPENC2E_SAMPLE");
	if (!path)
		return;
	itimerval off{};
	setitimer(ITIMER_PROF, &off, nullptr);
	FILE* f = fopen(path, "w");
	if (!f)
		return;
	int n = std::min(g_nsamples.load(), kMaxSamples);
	for (int i = 0; i < n; i++) {
		for (int k = 0; k < g_depths[i]; k++) {
			Dl_info info{};
			if (dladdr(g_samples[i][k], &info) && info.dli_fname) {
				const char* base = strrchr(info.dli_fname, '/');
				base = base ? base + 1 : info.dli_fname;
				if (strcmp(base, "openc2e") == 0)
					fprintf(f, "%s+0x%lx", k ? " " : "", (unsigned long)((char*)g_samples[i][k] - (char*)info.dli_fbase));
				else
					fprintf(f, "%s%s", k ? " " : "", base);
			} else
				fprintf(f, "%s?", k ? " " : "");
		}
		fputc('\n', f);
	}
	fclose(f);
}
void startSampler() {
	if (!devenv("OPENC2E_SAMPLE"))
		return;
	void* warm[4];
	backtrace(warm, 4); // (loads what backtrace needs before any signal)
	signal(SIGPROF, sampleHandler);
	itimerval t{};
	t.it_interval.tv_usec = 2000;
	t.it_value.tv_usec = 2000;
	setitimer(ITIMER_PROF, &t, nullptr);
	std::atexit(dumpSamples);
}
} // namespace
#endif

// Zeetures: a program started from a file manager, the Start menu or the
// Dock has no terminal, so a fatal error is shown in a window too
static bool hasTerminal() {
#ifdef _WIN32
	return GetConsoleWindow() != nullptr;
#else
	return isatty(fileno(stderr));
#endif
}

static void fatalError(const std::string& message) {
	fmt::print(stderr, "error: {}\n", message);
	fflush(stdout);
	fflush(stderr);
	if (!hasTerminal())
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Zeetures", message.c_str(), nullptr);
}

#ifdef _WIN32
static std::filesystem::path s_logPath; // (zeetures-log.txt when the output goes there)

// Zeetures.exe is a windowed program (no console window behind the game).
// Started from a command prompt it writes there; otherwise its output goes
// to zeetures-log.txt in the user folder, for bug reports.
static void connectOutput() {
	HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN)
		return; // (redirected to a file or pipe already)
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
		return;
	}
	try {
		std::filesystem::path log = storageBaseDirectory() / "zeetures-log.txt";
		std::error_code ec;
		std::filesystem::remove(log, ec);
		// (buffered: written to disk once a second by tendLog, not a disk
		// write for every line as before)
		if (_wfreopen(log.c_str(), L"a", stdout)) {
			setvbuf(stdout, nullptr, _IOFBF, 64 * 1024);
			s_logPath = log;
		}
		_wfreopen(log.c_str(), L"a", stderr);
	} catch (std::exception&) {
	}
}
#endif

// The log: written out once a second, so a crash loses at most a second of it
// (it is quiet now, and a file's buffer could hold minutes). On Windows the
// file is also started afresh if it grows past 20 MB in one session.
static void tendLog() {
	static Uint64 last = 0;
	Uint64 now = SDL_GetTicks();
	if (now - last < 1000)
		return;
	last = now;
	fflush(stdout);
	fflush(stderr);
#ifdef _WIN32
	std::error_code ec;
	if (!s_logPath.empty() && std::filesystem::file_size(s_logPath, ec) > 20u * 1024 * 1024 && !ec) {
		if (_wfreopen(s_logPath.c_str(), L"w", stdout)) {
			setvbuf(stdout, nullptr, _IOFBF, 64 * 1024);
			fmt::print("* (the log passed 20 MB and was started afresh)\n");
		}
		_wfreopen(s_logPath.c_str(), L"a", stderr);
	}
#endif
}

// SDL tries stealing main on some platforms, which we don't want.
#undef main

int main(int argc, char* argv[]) {
#ifdef _WIN32
	connectOutput();
#endif
	install_backtrace_printer();
	SDL_SetAppMetadata("Zeetures", nullptr, "io.github.dotairzee.zeetures");
#if defined(__linux__)
	startSampler();
#endif

	std::string version;
#ifdef DEV_BUILD
	version = "development build";
#else
	version = RELEASE_VERSION;
#endif
	fmt::print("openc2e ({}), built " __DATE__ " " __TIME__ "\nCopyright (c) 2004-2008 Alyssa Milburn and others\n\n", version);

	engine.addPossibleBackend("sdl", SDLBackend::get_instance());
	engine.addPossibleAudioBackend("sdlmixer", SDLMixerBackend::get_instance());

	// pass command-line flags to the engine, but do no other setup
	try {
		if (!engine.parseCommandLine(argc, argv))
			return 1;
	} catch (const std::exception& e) {
		fatalError(e.what());
		return 1;
	}

	// get the engine to do all the startup (read catalogue, loading world, etc)
	try {
		if (!engine.initialSetup())
			return 0;
	} catch (const std::exception& e) {
		fatalError(e.what());
		return 1;
	}

	get_backend()->run([] {
		hang_watch_beat(); // (see install_backtrace_printer: hang reports)
		tendLog();
		engine.tick();
		if (engine.done) {
			return false;
		}
		engine.drawWorld();
		return true;
	});

	// C1/C2: once the world is saved, end at once. Tearing the world and
	// the audio down after the save gained nothing and could hang: on
	// 2026-11-13 stopping the mixer made PipeWire free corrupted memory,
	// the C library aborted, and the crash report deadlocked in the abort
	// (Home never closed). Test runs (--no-autosave) still shut down fully.
	if (engine.saveForQuit()) {
		Openc2eImgui::UpdaterAtExit(); // (a downloaded update, once the world is saved)
		fflush(stdout);
		fflush(stderr);
		std::_Exit(0);
	}

	// we're done, be sure to shut stuff down
	engine.shutdown();
	Openc2eImgui::UpdaterAtExit();

	return 0;
}

/* vim: set noet: */
