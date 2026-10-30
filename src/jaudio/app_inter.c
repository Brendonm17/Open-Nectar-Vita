#include "jaudio/app_inter.h"

#include "jaudio/hvqm_play.h"
#include "jaudio/piki_bgm.h"
#include "jaudio/piki_scene.h"

static BOOL demo_fade_flag;

/**
 * @TODO: Documentation
 * @note UNUSED Size: 000094
 */
void Jac_Pause(void)
{
	PC_JAC_JOIN();
	TRAP_UNIMPLEMENTED;
}

/**
 * @TODO: Documentation
 */
void Jac_StreamMovieInit(immut char* filepath, u8* movieWorkBuffer, int movieWorkSize)
{
	PC_JAC_JOIN();
	if (Jac_GetCurrentScene() == SCENE_Title && !demo_fade_flag) {
		Jac_EasyCrossFade(1, 50);
		demo_fade_flag = TRUE;
	}
	Jac_HVQM_Init(filepath, movieWorkBuffer, movieWorkSize);
	Jac_UpdateStreamLevel();
}

/**
 * @TODO: Documentation
 */
void Jac_StreamMovieStop(void)
{
	PC_JAC_JOIN();
	Jac_HVQM_ForceStop();
	if (Jac_GetCurrentScene() == SCENE_Title && demo_fade_flag) {
		Jac_EasyCrossFade(0, 100);
		demo_fade_flag = FALSE;
	}
}

/**
 * @TODO: Documentation
 */
void Jac_StreamMovieUpdate(void)
{
	PC_JAC_JOIN();
	Jac_HVQM_Update();
}

/**
 * @TODO: Documentation
 */
int Jac_StreamMovieGetPicture(void* pictureBuffer, int* widthOut, int* heightOut)
{
	PC_JAC_JOIN();
	int picture;

	picture = Jac_GetPicture(pictureBuffer, widthOut, heightOut);
	if (picture == -1) {
		if (Jac_GetCurrentScene() == SCENE_Title && demo_fade_flag) {
			Jac_EasyCrossFade(0, 100);
			demo_fade_flag = FALSE;
		}
	}
	return picture;
}
