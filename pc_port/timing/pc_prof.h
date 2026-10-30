#ifndef PC_PROF_H
#define PC_PROF_H

/*
 * Function-level sampling profiler. Compiles to nothing outside NECTAR_PROF builds.
 */

#ifdef __cplusplus
extern "C" {
#endif

#if defined(NECTAR_PROF_BUILD) && defined(__vita__)
// Registers the calling thread: 0 = game, 1 = GX translator, 2 = Pikmin A worker, 3 = drawrec record worker.
void pc_prof_register(int slot);
// Marks the calling thread idle (1) or working (0); returns the previous state.
int pc_prof_idle_set(int on);
#else
static inline void pc_prof_register(int slot) { (void)slot; }
static inline int pc_prof_idle_set(int on) { (void)on; return 0; }
#endif

#ifdef __cplusplus
}
#endif

#endif // PC_PROF_H
