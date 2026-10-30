/**
 * @file vi_stubs.cpp
 * @brief Stub implementations for Dolphin VI (Video Interface) functions.
 *
 * In Stage 3, VIWaitForRetrace will be replaced with SDL2 vsync/frame limiting.
 */
#include "Dolphin/vi.h"
#include "pc_window.h"
#include "gl/pc_gfx.h"
#include <cstdio>
#include <cstring>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
extern "C" void nectar_gil_release(void);   // defined in os_stubs.cpp
extern "C" void nectar_gil_acquire(void);
extern "C" int nectar_gil_waiters(void);
#endif

#if defined(PIKI_PC_PORT) && defined(PIKI_PC_SETTINGS_MENU)
#include "settings/pc_settings.h"
#endif

static u32 sRetraceCount = 0;

static VIRetraceCallback sPostRetraceCallback = nullptr;

extern "C" {

void __VIInit(VITVMode mode)   { (void)mode; }
void VIInit(void)              { }
void VIFlush(void)             { }
void VIWaitForRetrace(void)    { 
    sRetraceCount++; 
    if (sPostRetraceCallback) {
        sPostRetraceCallback(sRetraceCount);
    }
#if defined(PIKI_PC_PORT) && defined(PIKI_PC_SETTINGS_MENU)
    // Draw the settings overlay through the game GX stack before the native
    // framebuffer is blitted to the window, so it appears on top.
    pc_settings_draw();
#endif
    pc_gfx_present();
    pc_window_swap_buffers();
    pc_window_poll_events(nullptr);
#ifdef __vita__
    // Per-frame cooperative yield to blocked workers.
    if (nectar_gil_waiters() > 0) {
        nectar_gil_release();
        sceKernelDelayThread(200);
        nectar_gil_acquire();
    }
#endif
}
void VIConfigure(const GXRenderModeObj* obj) { (void)obj; }

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback) {
    (void)callback; return nullptr;
}
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback) {
    VIRetraceCallback old = sPostRetraceCallback;
    sPostRetraceCallback = callback;
    return old;
}

void  VISetNextFrameBuffer(void* fb) { (void)fb; }
void* VIGetCurrentFrameBuffer()      { return nullptr; }
void  __VIGetCurrentPosition(s16* x, s16* y) { *x = 0; *y = 0; }
static volatile int sViBlack = 0;
void  VISetBlack(BOOL isBlack)       { sViBlack = isBlack ? 1 : 0; }
extern "C" int pc_vi_get_black(void) { return sViBlack; }
u32   VIGetRetraceCount(void)        { return sRetraceCount; }
u32   VIGetNextField(void)           { return 0; }
u32   VIGetCurrentLine(void)         { return 0; }
u32   VIGetTvFormat(void)            { return 0; } // NTSC
u32   VIGetDTVStatus(void)           { return 0; }

void  VIConfigurePan(u16 panPosX, u16 panPosY, u16 panSizeX, u16 panSizeY) {
    (void)panPosX; (void)panPosY; (void)panSizeX; (void)panSizeY;
}
void* VIGetNextFrameBuffer()         { return nullptr; }
void  VISetNextRightFrameBuffer(void* fb) { (void)fb; }
void  VISet3D()                      { }

} // extern "C"
