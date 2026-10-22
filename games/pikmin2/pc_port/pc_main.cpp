/**
 * @file pc_main.cpp
 * @brief Native entry point for the Pikmin 2 PC port (Phase 2).
 *
 * Replaces pikmin2-decomp/src/sysBootupU/sysBootup.cpp::main(), which is
 * excluded from pikmin2_legacy so this file owns the process entry.
 *
 * GameCube boot:
 *   System::initialize();
 *   return (new System())->run();  // GameFlow::run() loops sections
 *
 * Pikmin 1 used gsys->run(new PlugPikiApp()); that path does not exist here.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL.h>
#include "pc_discord.h"

#ifdef _WIN32
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement                  = 1;
__declspec(dllexport) int           AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#ifdef __ANDROID__
#include "android/pc_android.h"
#endif
#include "pc_gpu_preference.h"
#include "pc_window.h"
#include "settings/pc_settings.h"

#include "System.h"
#include <ctime>
#include <cwchar>
#ifndef _WIN32
#include <unistd.h> // getcwd
#endif

#ifndef NECTAR_RELEASE_VERSION
#define NECTAR_RELEASE_VERSION "unknown"
#endif
#ifndef NECTAR_GIT_COMMIT
#define NECTAR_GIT_COMMIT "unknown"
#endif
#if defined(_WIN32)
#define PC_CRASH_PLATFORM "Windows x86-64"
#elif defined(__ANDROID__)
#define PC_CRASH_PLATFORM "Android arm64"
#else
#define PC_CRASH_PLATFORM "Linux x86-64"
#endif

// Informe de crash junto a los datos del juego. El launcher lo encuentra (al
// fallar, porque el juego lo lanza, o la próxima vez que se abre) y ofrece
// copiarlo al portapapeles y abrir la página de issues de GitHub.
static const char kPcCrashFile[] = "pikmin2_crash.log";

// Motivo de un abort() (PANIC de un assert, OSPanic...): lo escribe quien
// aborta y el informe lo incluye. Sin él un assert solo decía "SIGABRT".
// Definido en JUTException.cpp, que va en la librería del juego.
extern "C" char pc_crash_note[512];
// Launcher que abrió el juego (NECTAR_LAUNCHER, lo pone él). Se copia al
// arrancar: dentro del gestor de crash no se reserva memoria.
#ifdef _WIN32
static wchar_t sPcCrashLauncher[1024];
static wchar_t sPcCrashPath[1024]; // ruta absoluta del informe, para el launcher
#else
static char sPcCrashLauncher[1024];
static char sPcCrashPath[1024];
#endif

static void pc_crash_header(FILE* out)
{
	if (pc_crash_note[0]) fprintf(out, "reason: %.511s\n", pc_crash_note);
	char stamp[32] = "?";
	const time_t now = time(nullptr);
	struct tm utc;
#ifdef _WIN32
	if (gmtime_s(&utc, &now) == 0)
#else
	if (gmtime_r(&now, &utc))
#endif
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &utc);
	fprintf(out, "game: Pikmin 2\nversion: %s (commit %s)\nplatform: %s\ntime: %s UTC\n", NECTAR_RELEASE_VERSION,
	        NECTAR_GIT_COMMIT, PC_CRASH_PLATFORM, stamp);
}

static void pc_crash_remember_launcher()
{
#ifdef _WIN32
	const wchar_t* launcher = _wgetenv(L"NECTAR_LAUNCHER");
	if (launcher && wcslen(launcher) < 1024) wcscpy(sPcCrashLauncher, launcher);
	if (!_wfullpath(sPcCrashPath, L"pikmin2_crash.log", 1024)) sPcCrashPath[0] = 0;
#else
	const char* launcher = getenv("NECTAR_LAUNCHER");
	if (launcher && strlen(launcher) < sizeof(sPcCrashLauncher)) strcpy(sPcCrashLauncher, launcher);
	char cwd[900];
	if (getcwd(cwd, sizeof(cwd))) snprintf(sPcCrashPath, sizeof(sPcCrashPath), "%s/%s", cwd, kPcCrashFile);
#endif
}

// Vigilante de cuelgues. Un juego congelado no dejaba informe: el hilo
// principal late en cada frame (VIWaitForRetrace) y, si pasa kPcHangMs sin
// latir fuera del bucle de eventos (donde Android lo bloquea en segundo
// plano), se escribe un informe HANG con su pila y se abre el del launcher.
// No se mata el juego: si se recupera, sigue.
#include <atomic>
#include <chrono>
#include <thread>
static std::atomic<long long> sPcHeartbeatMs { 0 };
static std::atomic<int> sPcInPoll { 0 };
static std::atomic<int> sPcBusy { 0 }; // diálogos nativos, instalar packs: largos a propósito
static const long long kPcHangMs = 30000;
static long long pc_now_ms()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}
extern "C" void pc_hang_heartbeat(void) { sPcHeartbeatMs.store(pc_now_ms(), std::memory_order_relaxed); }
extern "C" void pc_hang_set_in_poll(int inPoll)
{
	sPcInPoll.store(inPoll, std::memory_order_relaxed);
	if (!inPoll && sPcHeartbeatMs.load(std::memory_order_relaxed)) pc_hang_heartbeat();
}
extern "C" void pc_hang_busy(int begin)
{
	if (begin) {
		sPcBusy.fetch_add(1, std::memory_order_relaxed);
	} else {
		sPcBusy.fetch_sub(1, std::memory_order_relaxed);
		if (sPcHeartbeatMs.load(std::memory_order_relaxed)) pc_hang_heartbeat();
	}
}
static void pc_hang_report(long long stuckMs);
static void pc_hang_watchdog_start()
{
	std::thread([] {
		bool reported = false;
		for (;;) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			const long long last = sPcHeartbeatMs.load(std::memory_order_relaxed);
			if (!last || sPcInPoll.load(std::memory_order_relaxed) || sPcBusy.load(std::memory_order_relaxed) > 0)
				continue;
			const long long age = pc_now_ms() - last;
			if (age < kPcHangMs) {
				reported = false;
			} else if (!reported) {
				reported = true;
				pc_hang_report(age);
			}
		}
	}).detach();
}

#if defined(__linux__) || defined(__ANDROID__)
#include <csignal>
#include <unistd.h>
#include <execinfo.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <link.h>

#ifdef __ANDROID__
// backtrace() de bionic solo existe desde API 33: en el resto la pila salía
// vacía. _Unwind_Backtrace (libunwind del NDK) atraviesa el marco de la señal.
#include <unwind.h>
#include <ucontext.h>
struct PcUnwindState {
	void** cur;
	void** end;
};
static _Unwind_Reason_Code pc_unwind_frame(struct _Unwind_Context* ctx, void* arg)
{
	PcUnwindState* state = static_cast<PcUnwindState*>(arg);
	const uintptr_t pc   = _Unwind_GetIP(ctx);
	if (pc) {
		if (state->cur == state->end) return _URC_END_OF_STACK;
		*state->cur++ = (void*)pc;
	}
	return _URC_NO_REASON;
}
static int pc_backtrace(void** frames, int max)
{
	PcUnwindState state = { frames, frames + max };
	_Unwind_Backtrace(pc_unwind_frame, &state);
	return int(state.cur - frames);
}

// _Unwind_Backtrace empieza dentro del manejador en algunas versiones de
// Android. El PC guardado por el kernel siempre señala la instruccion que
// fallo y hace simbolizable el informe incluso cuando no se puede desenrollar.
static uintptr_t pc_fault_pc(void* context)
{
	if (!context) return 0;
#if defined(__aarch64__)
	return (uintptr_t)static_cast<ucontext_t*>(context)->uc_mcontext.pc;
#elif defined(__arm__)
	return (uintptr_t)static_cast<ucontext_t*>(context)->uc_mcontext.arm_pc;
#elif defined(__x86_64__)
	return (uintptr_t)static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__i386__)
	return (uintptr_t)static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_EIP];
#else
	return 0;
#endif
}
#else
static int pc_backtrace(void** frames, int max) { return backtrace(frames, max); }
#endif

static uintptr_t pc_module_base()
{
	Dl_info info;
#ifdef __ANDROID__
	// bionic no tiene RTLD_DL_LINKMAP: la base de la .so la da dladdr.
	if (dladdr((void*)&pc_module_base, &info)) {
		return (uintptr_t)info.dli_fbase;
	}
#else
	struct link_map* map = nullptr;
	if (dladdr1((void*)&pc_module_base, &info, (void**)&map, RTLD_DL_LINKMAP) && map) {
		return (uintptr_t)map->l_addr;
	}
#endif
	return 0;
}

#if defined(__x86_64__) && !defined(__ANDROID__)
#include <sys/ucontext.h>
#include "pc_intdiv_recover.h"

// Una línea por dirección (solo write: estamos dentro de una señal).
static void pc_intdiv_log(uint64_t at)
{
	static uint64_t seen[32];
	static int      seenCount = 0;
	for (int i = 0; i < seenCount; i++) {
		if (seen[i] == at) return;
	}
	if (seenCount < 32) seen[seenCount++] = at;
	char line[96] = "[PC Port] integer division by zero recovered at rel 0x";
	size_t len    = strlen(line);
	const uint64_t rel = at - pc_module_base();
	for (int shift = 36; shift >= 0; shift -= 4) line[len++] = "0123456789abcdef"[(rel >> shift) & 0xF];
	line[len++] = '\n';
	ssize_t ignored = write(2, line, len);
	(void)ignored;
}
#endif

extern "C" void pc_crash_handler(int sig, siginfo_t* info, void* ucontext)
{
#if defined(__x86_64__) && !defined(__ANDROID__)
	if (sig == SIGFPE && info && ucontext && (info->si_code == FPE_INTDIV || info->si_code == FPE_INTOVF)) {
		greg_t* regs      = static_cast<ucontext_t*>(ucontext)->uc_mcontext.gregs;
		const uint64_t at = (uint64_t)regs[REG_RIP];
		if (pc_intdiv_recover((uint64_t*)&regs[REG_RIP], (uint64_t*)&regs[REG_RAX], (uint64_t*)&regs[REG_RDX],
		                      info->si_code == FPE_INTOVF)) {
			pc_intdiv_log(at);
			return;
		}
	}
#elif !defined(__ANDROID__)
	(void)ucontext;
#endif
	static volatile sig_atomic_t entered = 0;
	if (entered) {
		_exit(128 + sig);
	}
	entered = 1;

	const char* name = "signal";
	switch (sig) {
	case SIGSEGV: name = "SIGSEGV (bad memory access)"; break;
	case SIGBUS:  name = "SIGBUS (misaligned/bad address)"; break;
	case SIGABRT: name = "SIGABRT (abort/assert)"; break;
	case SIGFPE:  name = "SIGFPE (arithmetic)"; break;
	case SIGILL:  name = "SIGILL (illegal instruction)"; break;
	}

	void* frames[64];
	int   count = 0;
#ifdef __ANDROID__
	const uintptr_t faultPc = pc_fault_pc(ucontext);
	if (faultPc) frames[count++] = (void*)faultPc;
#endif
	count += pc_backtrace(frames + count, 64 - count);

	uintptr_t base = pc_module_base();
	FILE* outs[2] = { stderr, fopen(kPcCrashFile, "w") };
	for (FILE* out : outs) {
		if (!out) continue;
		fprintf(out, "\n=== [PC Port] CRASH: %s ===\n", name);
		pc_crash_header(out);
		if (info) {
			fprintf(out, "faulting address = %p\n", info->si_addr);
		}
		fprintf(out, "module base = 0x%lx  (addr2line -Cfe pikmin2_pc <rel>)\n", (unsigned long)base);
		for (int i = 0; i < count; i++) {
			uintptr_t abs = (uintptr_t)frames[i];
			Dl_info   dl;
			const char* sym = (dladdr(frames[i], &dl) && dl.dli_sname) ? dl.dli_sname : "?";
			fprintf(out, "  #%-2d abs=0x%016lx rel=0x%010lx  %s\n", i, (unsigned long)abs,
			        (unsigned long)(abs - base), sym);
		}
		fprintf(out, "=== end backtrace ===\n");
		fflush(out);
	}
	if (outs[1]) fclose(outs[1]);

#ifndef __ANDROID__
	// Ventana del informe al momento: un proceso nuevo (el launcher), no este,
	// que puede tener la memoria corrupta. En Android la muestra el launcher
	// al volver a abrirse.
	if (sPcCrashLauncher[0] && fork() == 0) {
		setsid();
		execl(sPcCrashLauncher, sPcCrashLauncher, "--crash-report", sPcCrashPath, (char*)nullptr);
		_exit(127);
	}
#endif

	signal(sig, SIG_DFL);
	raise(sig);
}

// Pila del hilo principal para el informe de cuelgue: una señal que solo la
// copia (el archivo lo escribe el vigilante, fuera de la señal).
static pthread_t sPcMainThread;
static void* sPcHangFrames[64];
static std::atomic<int> sPcHangFrameCount { -1 };
static void pc_hang_signal(int)
{
	sPcHangFrameCount.store(pc_backtrace(sPcHangFrames, 64), std::memory_order_release);
}
static void pc_hang_report(long long stuckMs)
{
	sPcHangFrameCount.store(-1, std::memory_order_relaxed);
	pthread_kill(sPcMainThread, SIGRTMIN + 3);
	int count = -1;
	for (int i = 0; i < 100 && (count = sPcHangFrameCount.load(std::memory_order_acquire)) < 0; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const uintptr_t base = pc_module_base();
	FILE* outs[2] = { stderr, fopen(kPcCrashFile, "w") };
	for (FILE* out : outs) {
		if (!out) continue;
		fprintf(out, "\n=== [PC Port] HANG: main thread stuck for %lld s ===\n", stuckMs / 1000);
		pc_crash_header(out);
		fprintf(out, "module base = 0x%lx  (addr2line -Cfe pikmin2_pc <rel>)\n", (unsigned long)base);
		for (int i = 0; i < count; i++) {
			uintptr_t abs = (uintptr_t)sPcHangFrames[i];
			Dl_info   dl;
			const char* sym = (dladdr(sPcHangFrames[i], &dl) && dl.dli_sname) ? dl.dli_sname : "?";
			fprintf(out, "  #%-2d abs=0x%016lx rel=0x%010lx  %s\n", i, (unsigned long)abs,
			        (unsigned long)(abs - base), sym);
		}
		fprintf(out, "=== end backtrace ===\n");
		fflush(out);
	}
	if (outs[1]) fclose(outs[1]);
#ifndef __ANDROID__
	if (sPcCrashLauncher[0] && fork() == 0) {
		setsid();
		execl(sPcCrashLauncher, sPcCrashLauncher, "--crash-report", sPcCrashPath, (char*)nullptr);
		_exit(127);
	}
#endif
}

static void pc_install_crash_handler()
{
	pc_crash_remember_launcher();
#ifndef __ANDROID__
	// Sin terminal (abierto desde el launcher o el escritorio) la salida se
	// perdía: va a pikmin2.log, como en Windows, y entra en el informe de crash.
	// Una tubería o un archivo se respetan (el launcher lee --settings-dump así).
	struct stat outStat;
	const bool outCaptured
	    = fstat(STDOUT_FILENO, &outStat) == 0 && (S_ISFIFO(outStat.st_mode) || S_ISREG(outStat.st_mode));
	if ((!isatty(STDOUT_FILENO) && !outCaptured) || getenv("PIKMIN2_LOG_TO_FILE")) {
		if (freopen("pikmin2.log", "w", stdout)) {
			dup2(fileno(stdout), STDERR_FILENO);
			setvbuf(stderr, NULL, _IONBF, 0);
		}
	}
#endif
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = pc_crash_handler;
	sa.sa_flags     = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGABRT, &sa, nullptr);
	sPcMainThread = pthread_self();
	struct sigaction hang;
	memset(&hang, 0, sizeof(hang));
	hang.sa_handler = pc_hang_signal;
	hang.sa_flags   = SA_RESTART;
	sigemptyset(&hang.sa_mask);
	sigaction(SIGRTMIN + 3, &hang, nullptr);
	pc_hang_watchdog_start();
	sigaction(SIGFPE, &sa, nullptr);
	sigaction(SIGILL, &sa, nullptr);
}
#elif defined(_WIN32)
#include <io.h>
#include <windows.h>
#include <csignal>

// Equivalente al de Linux: excepción, dirección y pila (desenrollada con las
// tablas de x64, sin dbghelp) en stderr y en pikmin2_crash.log, junto a los
// datos del juego. rel = dirección - base del módulo, para addr2line/nm.
static void pc_crash_frame(FILE* out, int index, DWORD64 pc)
{
	HMODULE module = nullptr;
	wchar_t path[MAX_PATH] = L"?";
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCWSTR)(uintptr_t)pc, &module);
	if (module) GetModuleFileNameW(module, path, MAX_PATH);
	const wchar_t* name = wcsrchr(path, L'\\');
	fprintf(out, "  #%-2d abs=0x%016llx rel=0x%010llx  %ls\n", index, (unsigned long long)pc,
	        (unsigned long long)(pc - (DWORD64)(uintptr_t)module), name ? name + 1 : path);
}

#include "pc_intdiv_recover.h"

// División entera entre cero: se recupera como en ARM64 (pc_intdiv_recover.h)
// antes de que llegue al filtro de crash.
static LONG WINAPI pc_intdiv_handler(EXCEPTION_POINTERS* ep)
{
	const DWORD code = ep->ExceptionRecord->ExceptionCode;
	if (code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_INT_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
	CONTEXT* ctx      = ep->ContextRecord;
	const DWORD64 at  = ctx->Rip;
	if (!pc_intdiv_recover(&ctx->Rip, &ctx->Rax, &ctx->Rdx, code == EXCEPTION_INT_OVERFLOW)) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	static DWORD64 seen[32];
	static LONG    seenCount = 0;
	bool known = false;
	for (LONG i = 0; i < seenCount; i++) known = known || seen[i] == at;
	if (!known) {
		if (seenCount < 32) seen[seenCount++] = at;
		fprintf(stderr, "[PC Port] integer division by zero recovered at rel 0x%llx\n",
		        (unsigned long long)(at - (DWORD64)(uintptr_t)GetModuleHandleW(nullptr)));
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

// abort() (assert, std::terminate) levanta SIGABRT, que el filtro de
// excepciones no ve: el juego se cerraba sin informe. El manejador lo
// convierte en una excepción para que el filtro saque la pila de siempre.
static const DWORD kPcAbortException = 0xE0000ABD;
static void pc_abort_signal(int)
{
	RaiseException(kPcAbortException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

static void pc_crash_open_launcher();
static int pc_crash_walk(CONTEXT ctx, DWORD64* frames, int max);

static LONG WINAPI pc_crash_filter(EXCEPTION_POINTERS* ep)
{
	static volatile LONG entered = 0;
	if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH;

	FILE* outs[2] = { stderr, fopen(kPcCrashFile, "w") };
	for (FILE* out : outs) {
		if (!out) continue;
		const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
		fprintf(out, "\n=== [PC Port] CRASH: exception 0x%08lx%s at %p ===\n", (unsigned long)rec->ExceptionCode,
		        rec->ExceptionCode == kPcAbortException ? " (abort/assert)" : "", rec->ExceptionAddress);
		pc_crash_header(out);
		if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
			fprintf(out, "%s address = 0x%llx\n", rec->ExceptionInformation[0] ? "write" : "read",
			        (unsigned long long)rec->ExceptionInformation[1]);
		}
		fprintf(out, "module base = %p  (rel: nm -C / addr2line -Cfe pikmin2_pc.exe sin strip)\n",
		        (void*)GetModuleHandleW(nullptr));
		DWORD64 frames[64];
		const int count = pc_crash_walk(*ep->ContextRecord, frames, 64);
		for (int i = 0; i < count; ++i) pc_crash_frame(out, i, frames[i]);
		fprintf(out, "=== end backtrace ===\n");
		fflush(out);
	}
	if (outs[1]) fclose(outs[1]);
	pc_crash_open_launcher();
	return EXCEPTION_CONTINUE_SEARCH;
}

// Ventana del informe al momento, en un proceso nuevo (el launcher).
static void pc_crash_open_launcher()
{
	if (sPcCrashLauncher[0]) {
		wchar_t command[2200];
		_snwprintf(command, 2200, L"\"%ls\" --crash-report \"%ls\"", sPcCrashLauncher, sPcCrashPath);
		command[2199] = 0;
		STARTUPINFOW startup = {};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process = {};
		if (CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &startup,
		                   &process)) {
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
		}
	}
}

// Pila con las tablas de x64 (sin dbghelp), de una excepción o de un hilo
// suspendido.
static int pc_crash_walk(CONTEXT ctx, DWORD64* frames, int max)
{
	int count = 0;
	for (; count < max && ctx.Rip; ++count) {
		frames[count]        = ctx.Rip;
		DWORD64 imageBase    = 0;
		PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
		if (!fn) {
			// Función hoja: la dirección de vuelta está en la cima de la pila.
			ctx.Rip = *(DWORD64*)(uintptr_t)ctx.Rsp;
			ctx.Rsp += 8;
			continue;
		}
		void* handlerData   = nullptr;
		DWORD64 establisher = 0;
		RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
	}
	return count;
}

static HANDLE sPcMainThread = nullptr;
static void pc_hang_report(long long stuckMs)
{
	// La pila se copia con el hilo suspendido y se escribe después: si el
	// hilo tiene tomado el cerrojo del CRT, escribir con él parado colgaría.
	DWORD64 frames[64];
	int count = 0;
	if (sPcMainThread && SuspendThread(sPcMainThread) != (DWORD)-1) {
		CONTEXT ctx      = {};
		ctx.ContextFlags = CONTEXT_FULL;
		if (GetThreadContext(sPcMainThread, &ctx)) count = pc_crash_walk(ctx, frames, 64);
		ResumeThread(sPcMainThread);
	}
	FILE* outs[2] = { stderr, fopen(kPcCrashFile, "w") };
	for (FILE* out : outs) {
		if (!out) continue;
		fprintf(out, "\n=== [PC Port] HANG: main thread stuck for %lld s ===\n", stuckMs / 1000);
		pc_crash_header(out);
		fprintf(out, "module base = %p  (rel: nm -C / addr2line -Cfe pikmin2_pc.exe sin strip)\n",
		        (void*)GetModuleHandleW(nullptr));
		for (int i = 0; i < count; ++i) pc_crash_frame(out, i, frames[i]);
		fprintf(out, "=== end backtrace ===\n");
		fflush(out);
	}
	if (outs[1]) fclose(outs[1]);
	pc_crash_open_launcher();
}

static void pc_install_crash_handler()
{
	// Sin consola (abierto desde el launcher) la salida del juego se perdería:
	// va a pikmin2.log, junto a los datos.
	// Si stdout ya es una tubería o un archivo (el launcher lee así la
	// respuesta de --settings-dump), se respeta: redirigirlo dejaba al
	// launcher sin ajustes (issue #127).
	const DWORD outType = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
	const bool outCaptured = outType == FILE_TYPE_PIPE || outType == FILE_TYPE_DISK;
	if ((GetConsoleWindow() == nullptr && !outCaptured) || std::getenv("PIKMIN2_LOG_TO_FILE")) {
		if (freopen("pikmin2.log", "w", stdout)) {
			_dup2(_fileno(stdout), _fileno(stderr));
			setvbuf(stderr, NULL, _IONBF, 0);
		}
	}
	// El CRT de Windows limita a 512 FILE* abiertos (Linux deja ~1024 fd):
	// el juego mantiene abiertos archivos de audio y DVD a la vez.
	// 2048 es el máximo que acepta msvcrt (MinGW); la UCRT admite más.
	_setmaxstdio(2048);
	pc_crash_remember_launcher();
	SetUnhandledExceptionFilter(pc_crash_filter);
	AddVectoredExceptionHandler(1, pc_intdiv_handler);
	signal(SIGABRT, pc_abort_signal);
	DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sPcMainThread,
	                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
	pc_hang_watchdog_start();
}
#else
static void pc_hang_report(long long) { }
static void pc_install_crash_handler() { }
#endif

int main(int argc, char* argv[])
{
	pc_install_crash_handler();
	setvbuf(stdout, NULL, _IONBF, 0);
	SDL_SetMainReady();

	// El launcher Fusion consulta y modifica los ajustes sin crear la ventana
	// del juego. El backend ya implementaba este protocolo, pero el entry point
	// de Pikmin 2 todavía no lo despachaba.
	{
		const int settingsResult = pc_settings_cli(argc, argv);
		if (settingsResult >= 0) return settingsResult;
	}

	if (argc == 2 && std::strcmp(argv[1], "--audio-self-test") == 0) {
		// jaudio_integration_test.cpp drives Pikmin 1's src/jaudio engine
		// (PIKMIN_NATIVE_JAUDIO). Pikmin 2 already runs its own JSystem JAS/JAI
		// engine through the pc_dsp_host software DSP, so that backend has
		// nothing to plug into here: an explicit skip, not a missing-asset one.
		std::puts("[jaudio-test] SKIP: this suite targets Pikmin 1's src/jaudio engine; pikmin2_pc plays audio "
		          "through JSystem JAS/JAI + pc_dsp_host (covered by p2_aram_test / p2_envelope_test)");
		return 77;
	}

#ifdef __ANDROID__
	if (!pc_android_init()) {
		printf("[PC Port Fatal Error] Android storage unavailable\n");
		return 1;
	}
#endif

	pc_gpu_preference_apply();

	printf("╔══════════════════════════════════════════╗\n");
	printf("║   Pikmin 2 - Native PC Port              ║\n");
	printf("╚══════════════════════════════════════════╝\n\n");

	printf("[PC Port] Initializing SDL2 window and GL context...\n");
	// Ventana, contexto GL y entrada compartidos con el port de Pikmin 1.
	if (!pc_window_init("Open Nectar 2", 1280, 720)) {
		printf("[PC Port Fatal Error] Could not initialize window/OpenGL!\n");
		return 1;
	}
	// Menu F1: carga y aplica la configuracion guardada (video, controles, graficos).
	pc_settings_init();
	pc_discord_init("Pikmin 2");

	printf("[PC Port] System::initialize() (JFW heaps, render mode)...\n");
	System::initialize();

	printf("[PC Port] Constructing System / GameFlow...\n");
	System* gameSys = new System();
#if defined(VERSION_PAL)
	gameSys->setFlag(System::SF_TVModeSelected); // solo PAL: elección 50/60 Hz
	gameSys->setFlag(System::SF_RestoredRenderMode);
#endif

	printf("[PC Port] GameFlow::run() — section loop (boot → title → ...)\n");
	const int status = gameSys->run();

	printf("[PC Port] Game exited (status %d).\n", status);
	return status;
}
