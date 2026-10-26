#include "pc_menu_pad.h"

#include "MoviePlayer.h"
#include "Section.h"
#include "gameflow.h"
#include "pc_photo_mode.h"

#include <cstring>

// Menu mode follows the open interface, the same frame it opens. Photo mode
// sets the pause flags and reads Space and Shift itself, so it stays on
// gameplay keys. A fresh gameplay heartbeat is ignored once a menu is up.
int pc_menu_mode_active(void)
{
	if (pc_photo_mode_active())
		return 0;
	// Title, logo, and the cutscene player are menus. Story and challenge
	// live under OnePlayer and decide from the subsection that is open.
	if (gameflow.mCurrGameSectionID != SECTION_OnePlayer)
		return 1;
	if (gameflow.mGameSection) {
		for (CoreNode* child = gameflow.mGameSection->Child(); child; child = child->Next()) {
			const char* name = child->Name();
			if (!name || std::strcmp(name, "<NewPikiGameSection>") != 0)
				continue;
			if (gameflow.mMoviePlayer && gameflow.mMoviePlayer->mIsActive)
				return 1;
			if (gameflow.mIsUIOverlayActive || gameflow.mPauseAll)
				return 1;
			return 0;
		}
	}
	return 1;
}
