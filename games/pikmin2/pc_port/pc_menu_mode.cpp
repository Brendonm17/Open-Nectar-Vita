#include "pc_menu_pad.h"

#include "Game/GameSystem.h"
#include "Game/MoviePlayer.h"
#include "Game/SingleGame.h"
#include "Game/VsGame.h"
#include "Screen/Enums.h"
#include "Screen/Game2DMgr.h"
#include "pc_photo_mode.h"

namespace {

bool hudScene(SceneType scene)
{
	// These draw over free play and do not take a confirm. Anything else,
	// including every pause page, is a menu the moment it is the scene.
	switch (scene) {
	case SCENE_DUMMY:
	case SCENE_GROUND:
	case SCENE_CAVE:
	case SCENE_VS:
	case SCENE_CHALLENGE_1P:
	case SCENE_CHALLENGE_2P:
	case SCENE_FLOOR:
	case SCENE_COUNT_DOWN:
	case SCENE_HURRY_UP:
	case SCENE_READY_GO:
		return true;
	default:
		return false;
	}
}

bool freePlaySection(Game::BaseGameSection* section)
{
	if (auto* single = dynamic_cast<Game::SingleGameSection*>(section)) {
		Game::SingleGame::State* state = single->getCurrState();
		if (!state)
			return false;
		const int id = state->getCurrStateID();
		return id == Game::SingleGame::SGS_Game || id == Game::SingleGame::SGS_Cave;
	}
	if (auto* versus = dynamic_cast<Game::VsGameSection*>(section)) {
		Game::VsGame::State* state = versus->getCurrState();
		if (!state)
			return false;
		const int id = state->getCurrStateID();
		return id == Game::VsGame::VGS_Game || id == Game::VsGame::VGS_VS;
	}
	return false;
}

} // namespace

// A confirming scene wins the same frame it opens, including the frames of
// the pause countdown while the captain is still updating. Photo mode stays
// on gameplay keys. Boot, title, file select, the world map, options and
// extras are menus even when no 2D scene is up yet.
int pc_menu_mode_active(void)
{
	if (pc_photo_mode_active())
		return 0;

	if (Screen::gGame2DMgr && Screen::gGame2DMgr->mScreenMgr) {
		if (!hudScene(Screen::gGame2DMgr->mScreenMgr->getSceneType()))
			return 1;
	}

	if (Game::moviePlayer && Game::moviePlayer->mDemoState != Game::DEMOSTATE_Inactive)
		return 1;

	if (!Game::gameSystem || !Game::gameSystem->mSection)
		return 1;

	if (freePlaySection(Game::gameSystem->mSection))
		return 0;
	return 1;
}
