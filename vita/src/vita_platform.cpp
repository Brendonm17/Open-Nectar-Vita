/* PS Vita shell: memory budget, data directory, logging, vitaGL context and present. */
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <unistd.h>
#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/ctrl.h>
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <string>

#include "vita_platform.h"
#include "dolphin_stubs/dvd_image.h"

extern "C" {
// newlib heap for malloc/new.
__attribute__((used)) unsigned int _newlib_heap_size_user = NECTAR_VITA_HEAP_MB * 1024 * 1024;
// main() stack size.
__attribute__((used)) unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
}

static const char* kDataDir = NECTAR_VITA_DATA_DIR;
static const char* kLogPath = NECTAR_VITA_DATA_DIR "/nectar.log";
// vitaGL vblank count per flip.
extern "C" uint32_t vsync_interval;

#if !PIKI_USE_JAUDIO
// Defined only in src/jaudio/pikidemo.c.
void Jac_NoteDemoSkipped(void) {}
#endif

/* Logging */

// Crash-safe log.
static SceUID sLogMutex = -1;

#if NECTAR_LOGGING
// Persistent fd and line buffer.
static SceUID sLogFd   = -1;
static char   sLogBuf[16 * 1024];
static size_t sLogUsed = 0;

// Caller holds sLogMutex.
static void log_flush_locked() {
    if (sLogUsed == 0) return;
    if (sLogFd < 0) sLogFd = sceIoOpen(kLogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (sLogFd >= 0) sceIoWrite(sLogFd, sLogBuf, (SceSize)sLogUsed);
    sLogUsed = 0;
}

static void log_write(const char* text, size_t len) {
    if (sLogMutex < 0) sLogMutex = sceKernelCreateMutex("nectar_log", 0, 0, NULL);
    if (sLogMutex >= 0) sceKernelLockMutex(sLogMutex, 1, NULL);
#if NECTAR_LOG_CONSOLE
    sceClibPrintf("%s", text);
#endif
    if (len >= sizeof(sLogBuf)) {
        // Too big to buffer: write straight through.
        log_flush_locked();
        if (sLogFd < 0) sLogFd = sceIoOpen(kLogPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (sLogFd >= 0) sceIoWrite(sLogFd, text, (SceSize)len);
    } else {
        if (sLogUsed + len > sizeof(sLogBuf)) log_flush_locked();
        memcpy(sLogBuf + sLogUsed, text, len);
        sLogUsed += len;
    }
    if (sLogMutex >= 0) sceKernelUnlockMutex(sLogMutex, 1);
}

extern "C" void vita_log_flush(void) {
    if (sLogMutex >= 0) sceKernelLockMutex(sLogMutex, 1, NULL);
    log_flush_locked();
    if (sLogMutex >= 0) sceKernelUnlockMutex(sLogMutex, 1);
}
#else
// Logging compiled out.
static void log_write(const char*, size_t) { }
extern "C" void vita_log_flush(void) { }
#endif

// Safe formatter

struct SafeOut {
    char* p; size_t left;
    void put(char c) { if (left > 1) { *p++ = c; --left; } }
    void puts(const char* s, size_t n) { while (n--) put(*s++); }
};

static bool ptr_readable(const void* ptr, size_t* outMax) {
    if (!ptr || (uintptr_t)ptr < 0x1000) return false;
    SceKernelMemBlockInfo info;
    info.size = sizeof(info);
    if (sceKernelGetMemBlockInfoByAddr((void*)ptr, &info) < 0) return false;
    uintptr_t base = (uintptr_t)info.mappedBase, end = base + info.mappedSize;
    if ((uintptr_t)ptr < base || (uintptr_t)ptr >= end) return false;
    *outMax = end - (uintptr_t)ptr;
    return true;
}

static void emit_padded(SafeOut& o, const char* body, size_t blen, int width, bool left, char pad) {
    int padn = width > (int)blen ? width - (int)blen : 0;
    if (!left) while (padn-- > 0) o.put(pad);
    o.puts(body, blen);
    if (left)  while (padn-- > 0) o.put(' ');
}

static void fmt_uint(SafeOut& o, unsigned long long v, int base, bool upper, bool neg,
                     bool plus, bool space, bool alt, int width, int prec, bool left, bool zero) {
    char digits[32]; int n = 0;
    const char* dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) digits[n++] = '0';
    while (v) { digits[n++] = dig[v % base]; v /= base; }
    char body[48]; int b = 0;
    if (neg) body[b++] = '-'; else if (plus) body[b++] = '+'; else if (space) body[b++] = ' ';
    if (alt && base == 16) { body[b++] = '0'; body[b++] = upper ? 'X' : 'x'; }
    int zeros = prec > n ? prec - n : 0;
    if (zero && !left && prec < 0) { int tot = b + n; if (width > tot) zeros = width - tot; }
    while (zeros-- > 0) body[b++] = '0';
    while (n > 0) body[b++] = digits[--n];
    emit_padded(o, body, (size_t)b, width, left, ' ');
}

static int safe_vformat(char* out, size_t outsz, const char* fmt, va_list ap) {
    SafeOut o { out, outsz };
    for (const char* f = fmt; *f; ++f) {
        if (*f != '%') { o.put(*f); continue; }
        ++f;
        if (*f == '%') { o.put('%'); continue; }
        bool left = false, plus = false, space = false, alt = false, zero = false;
        for (;; ++f) {
            if (*f == '-') left = true; else if (*f == '+') plus = true;
            else if (*f == ' ') space = true; else if (*f == '#') alt = true;
            else if (*f == '0') zero = true; else break;
        }
        int width = 0;
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = true; width = -width; } ++f; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int prec = -1;
        if (*f == '.') { ++f; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); ++f; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
        int len = 0; // 0=int, 1=long, 2=long long, 3=size_t/ptrdiff, 4=short, 5=char, 6=long double
        if (*f == 'h') { ++f; len = 4; if (*f == 'h') { ++f; len = 5; } }
        else if (*f == 'l') { ++f; len = 1; if (*f == 'l') { ++f; len = 2; } }
        else if (*f == 'j') { ++f; len = 2; }
        else if (*f == 'z' || *f == 't') { ++f; len = 3; }
        else if (*f == 'L') { ++f; len = 6; }
        char conv = *f;
        if (!conv) break;
        switch (conv) {
        case 'd': case 'i': {
            long long v = (len == 2) ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            if (len == 4) v = (short)v; else if (len == 5) v = (signed char)v;
            bool neg = v < 0; unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ull : (unsigned long long)v;
            fmt_uint(o, u, 10, false, neg, plus, space, false, width, prec, left, zero);
            break; }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v = (len == 2) ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int);
            if (len == 4) v = (unsigned short)v; else if (len == 5) v = (unsigned char)v;
            int base = conv == 'o' ? 8 : (conv == 'u' ? 10 : 16);
            fmt_uint(o, v, base, conv == 'X', false, false, false, alt, width, prec, left, zero);
            break; }
        case 'c': { char c = (char)va_arg(ap, int); emit_padded(o, &c, 1, width, left, ' '); break; }
        case 'p': {
            unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void*);
            fmt_uint(o, v, 16, false, false, false, false, true, width, 8, left, false);
            break; }
        case 's': {
            const char* s = va_arg(ap, const char*);
            size_t maxr = 0; const char* body = "(null)"; size_t blen = 6;
            if (s == nullptr) { /* keep "(null)" */ }
            else if (!ptr_readable(s, &maxr)) { body = "(badptr)"; blen = 8; }
            else {
                size_t cap = maxr < 512 ? maxr : 512;
                if (prec >= 0 && (size_t)prec < cap) cap = (size_t)prec;
                blen = strnlen(s, cap); body = s;
            }
            emit_padded(o, body, blen, width, left, ' ');
            break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = (len == 6) ? (double)va_arg(ap, long double) : va_arg(ap, double);
            char spec[24]; int k = 0; spec[k++] = '%';
            if (left) spec[k++] = '-'; if (plus) spec[k++] = '+'; if (space) spec[k++] = ' ';
            if (alt) spec[k++] = '#'; if (zero) spec[k++] = '0';
            if (width) k += snprintf(spec + k, sizeof(spec) - k, "%d", width);
            if (prec >= 0) k += snprintf(spec + k, sizeof(spec) - k, ".%d", prec);
            spec[k++] = conv; spec[k] = 0;
            char tmp[64]; int n = snprintf(tmp, sizeof(tmp), spec, v);
            if (n > 0) o.puts(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
            break; }
        case 'n': (void)va_arg(ap, int*); break;
        default:  o.put('%'); o.put(conv); break;
        }
    }
    *o.p = 0;
    return (int)(outsz - o.left);
}

#ifdef NECTAR_TESTER_BUILD
// Tester build: only NOT BAKED lines.
static bool tester_log_allowed(const char* s, size_t n) {
    static const char* const kKeep[] = { "[VITA TEV] ", "[VITA VS] " };
    for (const char* k : kKeep) {
        const size_t kl = strlen(k);
        if (n >= kl && memcmp(s, k, kl) == 0) return true;
    }
    return false;
}
#define NECTAR_LOG_ALLOWED(s, n) tester_log_allowed((s), (n))
#else
// Release: errors and unbaked shaders.
static bool ci_contains(const char* s, size_t n, const char* key) {
    const size_t kl = strlen(key);
    if (kl > n) return false;
    for (size_t i = 0; i + kl <= n; i++) {
        size_t j = 0;
        for (; j < kl; j++) {
            unsigned char a = (unsigned char)s[i + j], b = (unsigned char)key[j];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (a != b) break;
        }
        if (j == kl) return true;
    }
    return false;
}
static bool release_log_allowed(const char* s, size_t n) {
    // Unbaked shader lines.
    if (ci_contains(s, n, "not baked")) return true;
    // Error wordings.
    static const char* const kError[] = {
        "error", "fail", "crash", "fatal", "abort", "invalid", "corrupt", "unknown op",
        "missing", "not found", "not a file", "unavailable", "refused", "timeout",
        "exception", "out of memory", "cannot", "could not", "panic",
        "exhausted", "broken", "resynced", "abandon", "too large", "no vram", "not allocated", "not created",
        "out of program", "tripwire", "off-lock worker reached", " oom", "disc revision",
    };
    for (const char* k : kError) {
        if (ci_contains(s, n, k)) return true;
    }
    return false;
}
#define NECTAR_LOG_ALLOWED(s, n) release_log_allowed((s), (n))
#endif

static int log_vprintf(const char* fmt, va_list ap) {
    char buf[1024];
    int n = safe_vformat(buf, sizeof(buf), fmt, ap);
    if (n < 0) return n;
    if (NECTAR_LOG_ALLOWED(buf, (size_t)n)) log_write(buf, (size_t)n);
    return n;
}

extern "C" int vita_log_printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = log_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

// Linker --wrap hooks for printf and friends.
extern "C" {
int __real_vfprintf(FILE* stream, const char* fmt, va_list ap);
int __real_fputs(const char* s, FILE* stream);

int __wrap_printf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); int n = log_vprintf(fmt, ap); va_end(ap); return n;
}
int __wrap_vprintf(const char* fmt, va_list ap) { return log_vprintf(fmt, ap); }

int __wrap_fprintf(FILE* stream, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = (stream == stdout || stream == stderr) ? log_vprintf(fmt, ap)
                                                   : __real_vfprintf(stream, fmt, ap);
    va_end(ap); return n;
}
int __wrap_vfprintf(FILE* stream, const char* fmt, va_list ap) {
    return (stream == stdout || stream == stderr) ? log_vprintf(fmt, ap)
                                                  : __real_vfprintf(stream, fmt, ap);
}
int __wrap_puts(const char* s) {
    if (NECTAR_LOG_ALLOWED(s, strlen(s))) { log_write(s, strlen(s)); log_write("\n", 1); }
    return 0;
}
int __wrap_fputs(const char* s, FILE* stream) {
    if (stream == stdout || stream == stderr) { if (NECTAR_LOG_ALLOWED(s, strlen(s))) log_write(s, strlen(s)); return 0; }
    return __real_fputs(s, stream);
}
}

/* Platform */

static void ensure_dir(const char* path) {
    SceIoStat st;
    if (sceIoGetstat(path, &st) >= 0 && SCE_S_ISDIR(st.st_mode)) return;
    sceIoMkdir(path, 0777);
}

/* Game data: a disc image, or the extracted assets/ tree */

static bool ascii_iequals(const char* a, const char* b) {
    for (;; ++a, ++b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

static bool has_disc_extension(const char* name) {
    const size_t n = strlen(name);
    if (n < 5) return false;
    const char* ext = name + n - 4;
    if (ascii_iequals(ext, ".iso") || ascii_iequals(ext, ".gcm") || ascii_iequals(ext, ".rvz") || ascii_iequals(ext, ".wia"))
        return true;
    return n >= 6 && ascii_iequals(name + n - 5, ".ciso");
}

// Counts disc images in dir; first one found goes in first.
static int scan_disc_images(const char* dir, std::string& first) {
    const SceUID d = sceIoDopen(dir);
    if (d < 0) return 0;
    int found = 0;
    SceIoDirent ent;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (SCE_S_ISDIR(ent.d_stat.st_mode) || !has_disc_extension(ent.d_name)) continue;
        if (first.empty()) { first = dir; first += '/'; first += ent.d_name; }
        ++found;
    }
    sceIoDclose(d);
    return found;
}

// Disc image in the data folder or rom/, else an extracted assets/ tree.
static void vita_locate_disc(void) {
    std::string image;
    scan_disc_images(kDataDir, image);
    scan_disc_images(NECTAR_VITA_DATA_DIR "/rom", image);   // first image found

    if (image.empty()) {
        // Directory mode: the extracted assets/ tree.
        SceIoStat st;
        if (sceIoGetstat(NECTAR_VITA_DATA_DIR "/assets/dataDir/parms/gamePrms.bin", &st) >= 0) return;
        vita_platform_fatal("Pikmin (USA) disc image not found.\n\n"
                            "Copy your Pikmin .iso, .gcm, .rvz or .ciso into " NECTAR_VITA_DATA_DIR
                            " and restart.");
    }

    std::string error;
    if (!dvd_image_mount(image.c_str(), error)) {
        vita_log_printf("[VITA] disc image mount FAILED: %s: %s\n", image.c_str(), error.c_str());
        const std::string text = "Could not load the disc image\n" + image + "\n\n" + error + ".";
        vita_platform_fatal(text.c_str());
    }
    const char* id = dvd_image_game_id();
    if (strcmp(id, "GPIE01") != 0) {
        vita_log_printf("[VITA] disc image %s has game id %s, not GPIE01\n", image.c_str(), id);
        const std::string text = std::string("Unsupported disc ") + id
                               + ": Pikmin (USA), GPIE01, is required.\n\n" + image;
        dvd_image_unmount();
        vita_platform_fatal(text.c_str());
    }
    if (dvd_image_revision() != 1) {
        vita_log_printf("[VITA] WARNING: disc revision %u; the port targets revision 1 (GPIE01_01). Continuing.\n",
                        (unsigned)dvd_image_revision());
    }
}

void vita_platform_early_init(void) {
    ensure_dir("ux0:data");
    ensure_dir(kDataDir);
    sceIoRemove(kLogPath);   // fresh log per run

    // All port paths are relative to the data folder.
    if (chdir(kDataDir) != 0) vita_log_printf("[VITA] ERROR: chdir(%s) failed\n", kDataDir);

    // Game data, or the fatal dialog.
    vita_locate_disc();
}

// vitaGL context creation. Idempotent.
static bool sGlContextUp = false;
static void vita_gl_create_context(void) {
    if (sGlContextUp) return;
    sGlContextUp = true;
    // Tiler parameter buffer: 40 MB.
    vglSetParamBufferSize(40u * 1024u * 1024u);
    // Shader patcher pools, in VRAM.
    vglUseVramForUSSE(GL_TRUE);
    vglSetupShaderPatcher(8 * 1024 * 1024, 4 * 1024 * 1024, 4 * 1024 * 1024);
    // Client-array and vitaGL scratch pool.
    vglSetCircularPoolSize(36 * 1024 * 1024);
    // vitaGL init. Returns GL_TRUE on resolution fallback.
    vglSetupDisplayRenderTarget(8);
    vglInitExtended(256 * 1024, NECTAR_VITA_SCREEN_W, NECTAR_VITA_SCREEN_H, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    vglUseVram(GL_TRUE);
    // vitaGL off the newlib heap.
    vglUseExtraMem(GL_FALSE);
}

bool vita_platform_gl_init(int* outWidth, int* outHeight) {
    vita_gl_create_context();
    // vsync interval follows the game's frame clamp.
    vita_platform_set_vsync_interval(1);

    // Clear all back buffers.
    for (int i = 0; i < 3; i++) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        vglSwapBuffers(GL_FALSE);
    }
    *outWidth = NECTAR_VITA_SCREEN_W;
    *outHeight = NECTAR_VITA_SCREEN_H;
    return true;
}

// sysNew.cpp, C linkage.
extern "C" void piki_pc_retire_tick(void);

void vita_platform_present(void) {
    // Flush the frame's buffered log.
    vita_log_flush();
    // Free large blocks the pipeline has finished reading.
    piki_pc_retire_tick();
}

void vita_platform_present_impl(void) {
    vglSwapBuffers(GL_FALSE);
}

// vitaGL vblank count per flip.
void vita_platform_set_vsync_interval(int interval) {
    int v = interval;
    if (v < 1) v = 1;   // no 120 Hz on the Vita
    if (v > 2) v = 2;
    vsync_interval = (uint32_t)v;
}

// True while the system has the controller (SCE_CTRL_INTERCEPTED).
extern "C" int vita_platform_intercepted(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return 0;
    return (pad.buttons & 0x00010000u) != 0;
}

void vita_platform_shutdown(void) {
    sceKernelExitProcess(0);
}

[[noreturn]] void vita_platform_fatal(const char* message) {
    vita_log_printf("[VITA] FATAL: %s\n", message);
    vita_log_flush();

    // Dialog GXM context, on the main thread.
    vita_gl_create_context();

    // Message capped at SCE_MSG_DIALOG_USER_MSG_SIZE bytes.
    static char text[SCE_MSG_DIALOG_USER_MSG_SIZE];
    strncpy(text, message, sizeof(text) - 1);
    text[sizeof(text) - 1] = 0;

    SceMsgDialogUserMessageParam user;
    memset(&user, 0, sizeof(user));
    user.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    user.msg = (const SceChar8*)text;
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;
    const int r = sceMsgDialogInit(&param);
    if (r < 0) vita_log_printf("[VITA] ERROR: sceMsgDialogInit failed 0x%08X\n", (unsigned)r);
    if (r >= 0) {
        // vitaGL runs sceCommonDialogUpdate per frame.
        while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            vglSwapBuffers(GL_TRUE);
        }
        sceMsgDialogTerm();
    } else {
        // No dialog: message is in the log.
        sceKernelDelayThread(3 * 1000 * 1000);
    }
    vita_log_flush();
    sceKernelExitProcess(0);
    for (;;) sceKernelDelayThread(1000 * 1000);
}
