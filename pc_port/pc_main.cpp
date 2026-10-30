/**
 * @file pc_main.cpp
 * @brief Native Linux entry point for the Pikmin PC port.
 *
 * Replaces the original sysBootup.cpp main() which called
 * gsys->Initialise() and gsys->run(new PlugPikiApp()).
 *
 * This file will grow in later stages to include SDL2 window creation,
 * OpenGL context setup, and input polling.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if PIKI_USE_JAUDIO
int pc_jaudio_integration_test();
#endif

// Game headers
#include "system.h"
#include "App.h"
#include "sysNew.h"

/**
 * @brief Entry point for the Linux PC port.
 *
 * Currently this initializes the game system with stubs and enters
 * the main loop. Since all Dolphin SDK calls are stubbed, this will
 * "run" but produce no visible output yet.
 */
#include <SDL.h>

#include "pc_window.h"
#ifdef __vita__
#include "vita_platform.h"
#endif
#include "settings/pc_settings.h"
#include "timing/pc_prof.h"

int main(int argc, char* argv[])
{
    // Disable stdout buffering so we see logs immediately before any crash
    setvbuf(stdout, NULL, _IONBF, 0);
    piki_pc_mark_game_thread();   // Allocation heap tag
#ifdef __vita__
    vita_platform_early_init();
    pc_prof_register(0);   // Game thread (NECTAR_PROF)
#endif

    // SDL_MAIN_HANDLED is defined for this build, which means the application
    // owns main() and SDL2main is not linked. The other half of that contract
    // is telling SDL so before the first SDL_Init. It is close to a no-op on
    // Linux, which is why its absence went unnoticed there, but Windows needs
    // it to set up the instance handle and command line.
    SDL_SetMainReady();

#if PIKI_USE_JAUDIO
    if (argc == 2 && std::strcmp(argv[1], "--audio-self-test") == 0)
        return pc_jaudio_integration_test();
#endif
    (void)argc;
    (void)argv;

    // Initialize SDL2 Window and OpenGL Context FIRST
    if (!pc_window_init("Open Nectar", 1280, 720)) {
        printf("[PC Port Fatal Error] Could not initialize window/OpenGL!\n");
        fflush(stdout);
        return 1;
    }

    pc_settings_init();
    gsys->Initialise();
    nodeMgr = new NodeMgr();
    gsys->run(new PlugPikiApp());
    return 0;
}
