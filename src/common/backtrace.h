#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#include <stdint.h>
// #include <psapi.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "dbghelp.lib")

LONG WINAPI TopLevelExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo) {
	HANDLE process = GetCurrentProcess();
	SymInitialize(process, NULL, TRUE);
	DWORD sym_options = SymGetOptions();
	sym_options |= SYMOPT_LOAD_LINES | SYMOPT_UNDNAME;
	SymSetOptions(sym_options);

	// StackWalk64() may modify context record passed to it, so we will
	// use a copy.
	CONTEXT context_record = *pExceptionInfo->ContextRecord;
	// Initialize stack walking.
	STACKFRAME64 stack_frame;
	memset(&stack_frame, 0, sizeof(stack_frame));
#if defined(_WIN64)
	int machine_type = IMAGE_FILE_MACHINE_AMD64;
	stack_frame.AddrPC.Offset = context_record.Rip;
	stack_frame.AddrFrame.Offset = context_record.Rbp;
	stack_frame.AddrStack.Offset = context_record.Rsp;
#else
	int machine_type = IMAGE_FILE_MACHINE_I386;
	stack_frame.AddrPC.Offset = context_record.Eip;
	stack_frame.AddrFrame.Offset = context_record.Ebp;
	stack_frame.AddrStack.Offset = context_record.Esp;
#endif
	stack_frame.AddrPC.Mode = AddrModeFlat;
	stack_frame.AddrFrame.Mode = AddrModeFlat;
	stack_frame.AddrStack.Mode = AddrModeFlat;


	fprintf(stderr, "Exception occurred: 0x%lx\n", pExceptionInfo->ExceptionRecord->ExceptionCode);
	// fprintf(stderr, "Exception occurred: %i\n", pExceptionInfo->ExceptionRecord->NumberParameters);
	// for (int j = 0; j < pExceptionInfo->ExceptionRecord->NumberParameters; ++j) {
	// 	fprintf(stderr, "0x%I64x ", pExceptionInfo->ExceptionRecord->ExceptionInformation[j]);
	//   fprintf(stderr, "%s\n", typeid((void*)pExceptionInfo->ExceptionRecord->ExceptionInformation[j]).name());
	// }
	// type_info *t = (type_info*)(pExceptionInfo->ExceptionRecord->ExceptionInformation[2] + 0x10000);
	// fprintf(stderr, "%p\n", t);
	// fprintf(stderr, "%s\n", t->name());
	// fprintf(stderr, "%s\n", ((std::exception*)pExceptionInfo->ExceptionRecord->ExceptionInformation[1])->what());
	// fprintf(stderr, "%p\n", dynamic_cast<std::exception*>((void*)pExceptionInfo->ExceptionRecord->ExceptionInformation[1]));
	// fprintf(stderr, "%s\n", ((type_info*)pExceptionInfo->ExceptionRecord->ExceptionInformation[2])->name());
	// fprintf(stderr, "%s\n", ((type_info*)pExceptionInfo->ExceptionRecord->ExceptionInformation[3])->name());
	fprintf(stderr, "Stack trace (most recent call first):\n");

	int i = 1;
	while (StackWalk64(machine_type,
		GetCurrentProcess(),
		GetCurrentThread(),
		&stack_frame,
		&context_record,
		NULL,
		&SymFunctionTableAccess64,
		&SymGetModuleBase64,
		NULL)) {
		DWORD64 modulebase = SymGetModuleBase64(process, stack_frame.AddrPC.Offset);

		fprintf(stderr, "  [%i] ", i);
		i++;

		IMAGEHLP_MODULE64 moduleInfo;
		moduleInfo = {};
		moduleInfo.SizeOfStruct = sizeof(moduleInfo);
		if (::SymGetModuleInfo64(process, modulebase, &moduleInfo)) {
			if (moduleInfo.ImageName) {
				char* basename = moduleInfo.ImageName;
				while (true) {
					char* next = strchr(basename, '\\');
					if (next) {
						basename = next + 1;
					} else {
						break;
					}
				}
				fprintf(stderr, "%s", basename);
			} else if (moduleInfo.ModuleName) {
				fprintf(stderr, "%s", moduleInfo.ModuleName);
			} else {
				fprintf(stderr, "???");
			}
		} else {
			fprintf(stderr, "???");
		}

		fprintf(stderr, " ! ");

		char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
		PSYMBOL_INFO symbol = (PSYMBOL_INFO)buffer;
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = MAX_SYM_NAME;

		DWORD64 displacement = 0;
		if (SymFromAddr(process, (DWORD64)stack_frame.AddrPC.Offset, &displacement, symbol)) {
			fprintf(stderr, "%s ", symbol->Name);

			IMAGEHLP_LINE64 lineInfo;
			lineInfo = {};
			lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

			DWORD linedisplacement = 0;
			if (::SymGetLineFromAddr64(process, (DWORD64)stack_frame.AddrPC.Offset, &linedisplacement, &lineInfo)) {
				char* basename = lineInfo.FileName;
				while (true) {
					char* next = strchr(basename, '\\');
					if (next) {
						basename = next + 1;
					} else {
						break;
					}
				}
				fprintf(stderr, "in %s at line %i", basename, lineInfo.LineNumber);
			} else {
				fprintf(stderr, "+ 0x%I64x", displacement);
			}
		} else {
			fprintf(stderr, "0x%I64x", stack_frame.AddrPC.Offset);
		}
		fprintf(stderr, "\n");
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

#else // _WIN32

#include <dlfcn.h>
#include <execinfo.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __GNUG__
#include <cxxabi.h>
#endif

static void print_frames(bool demangle);

static void signalhandler(int) {
	// (ended in 5 s whatever happens: a crash inside the memory allocator left
	// the handler waiting for its lock, and the game frozen instead of closed)
	signal(SIGALRM, SIG_DFL);
	alarm(5);
	print_frames(true);
	exit(1);
}

// Hang reports (2026-11-12: Home froze twice and had to be force-quit, with
// nothing in its log): main loop beats once a frame (hang_watch_beat); a
// watcher thread, when no beat has come for 8 s, signals the main thread
// (SIGUSR2), which prints its own stack to stderr and carries on. Repeated
// every 20 s while it stays stuck, to tell a loop that moves from one that
// doesn't. Names are not demangled (no allocating in the handler); offsets
// are for addr2line.
#include <atomic>
#include <chrono>
#include <pthread.h>
#include <thread>

static std::atomic<unsigned long> s_hang_beats{0};
static pthread_t s_main_thread;

static void hangsignalhandler(int) {
	fprintf(stderr, "* HANG: the main loop has not finished a frame for a while; its stack:\n");
	print_frames(false);
	fprintf(stderr, "* HANG: end of stack\n");
}

inline void hang_watch_beat() {
	s_hang_beats.fetch_add(1, std::memory_order_relaxed);
}

static void start_hang_watch() {
	s_main_thread = pthread_self();
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = hangsignalhandler;
	sa.sa_flags = SA_ONSTACK | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR2, &sa, nullptr);
	std::thread([] {
		unsigned long last = 0;
		auto since = std::chrono::steady_clock::now();
		auto reported = since;
		bool stuck = false;
		while (true) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			unsigned long now_beats = s_hang_beats.load(std::memory_order_relaxed);
			auto now = std::chrono::steady_clock::now();
			if (now_beats != last || now_beats == 0) { // moving, or not started yet
				if (stuck)
					fprintf(stderr, "* HANG: over; frames again after %.0f s\n", std::chrono::duration<double>(now - since).count());
				last = now_beats;
				since = now;
				stuck = false;
				continue;
			}
			double idle = std::chrono::duration<double>(now - since).count();
			if ((!stuck && idle >= 8) || (stuck && std::chrono::duration<double>(now - reported).count() >= 20)) {
				fprintf(stderr, "* HANG: no frame for %.0f s\n", idle);
				stuck = true;
				reported = now;
				pthread_kill(s_main_thread, SIGUSR2);
			}
		}
	}).detach();
}

static void print_frames(bool demangle) {
	void* callstack[128];
	int num_frames = backtrace(callstack, 128);
	for (int i = 0; i < num_frames; i++) {
		Dl_info info;
		memset(&info, 0, sizeof(info));
		dladdr(callstack[i], &info);

		// frame number
		fprintf(stderr, "%-3d ", i);

		// filename
		if (info.dli_fname) {
			const char* basename = strrchr(info.dli_fname, '/');
			if (basename) {
				basename++;
			} else {
				basename = info.dli_fname;
			}
			fprintf(stderr, "%-35s ", basename);
		} else {
			fprintf(stderr, "%-35s ", "???");
		}

		// memory address, and its offset in the module (for addr2line)
		fprintf(stderr, "0x%.*" PRIxPTR " ", (int)(sizeof(void*) * 2), (uintptr_t)callstack[i]);
		if (info.dli_fbase)
			fprintf(stderr, "[+0x%" PRIxPTR "] ", (uintptr_t)callstack[i] - (uintptr_t)info.dli_fbase);

		// function name
		if (info.dli_sname) {
#ifdef __GNUG__
			int status = -1;
			char* demangled = demangle ? abi::__cxa_demangle(info.dli_sname, NULL, NULL, &status) : nullptr;
			if (status == 0 && demangled) {
				fprintf(stderr, "%s ", demangled);
				free(demangled);
			} else {
				fprintf(stderr, "%s ", info.dli_sname);
			}
#else
			fprintf(stderr, "%s ", info.dli_sname);
#endif
		} else {
			fprintf(stderr, "0x0 ");
		}

		// offset from function start
		fprintf(stderr, "+ %" PRIuPTR, (uintptr_t)callstack[i] - (uintptr_t)info.dli_saddr);

		// end of frame
		fprintf(stderr, "\n");
	}
}

#endif

#ifdef _WIN32
inline void hang_watch_beat() {}
#endif

void install_backtrace_printer() {
#ifdef _WIN32
	SetUnhandledExceptionFilter(TopLevelExceptionHandler);
#else
	// Run the handler on its own stack, so a stack overflow (runaway recursion)
	// still prints a backtrace instead of the process vanishing silently.
	static char altstack[256 * 1024];
	stack_t ss;
	memset(&ss, 0, sizeof(ss));
	ss.ss_sp = altstack;
	ss.ss_size = sizeof(altstack);
	sigaltstack(&ss, nullptr);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signalhandler;
	sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	for (int sig : {SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGTRAP})
		sigaction(sig, &sa, nullptr);
	start_hang_watch();
#endif
}