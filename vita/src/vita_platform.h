#pragma once
// PS Vita shell entry points used by pc_port/pc_window.cpp under __vita__.
#ifndef NECTAR_VITA_DATA_DIR
#define NECTAR_VITA_DATA_DIR "ux0:data/Pikmin"
#endif
#ifndef NECTAR_VITA_HEAP_MB
// Newlib heap size.
#define NECTAR_VITA_HEAP_MB 200

// Log output switches.
#ifndef NECTAR_LOGGING
#define NECTAR_LOGGING 1
#endif
#ifndef NECTAR_LOG_CONSOLE
#define NECTAR_LOG_CONSOLE 1
#endif

// Flushes the buffered log.
extern "C" void vita_log_flush(void);
#endif
#define NECTAR_VITA_SCREEN_W 960
#define NECTAR_VITA_SCREEN_H 544

void vita_platform_early_init(void);                       // clocks, chdir, log file
bool vita_platform_gl_init(int* outWidth, int* outHeight); // vitaGL context
void vita_platform_present(void);                          // no-op; the render thread swaps
void vita_platform_present_impl(void);                     // render thread: vglSwapBuffers
// Vblanks per flip.
void vita_platform_set_vsync_interval(int interval);
void vita_platform_shutdown(void);
// Fatal boot error dialog; exits.
[[noreturn]] void vita_platform_fatal(const char* message);
