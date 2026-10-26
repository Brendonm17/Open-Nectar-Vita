#include "pc_p2_coop.h"

namespace {
void* sMovieProtagonist = nullptr;
} // namespace

extern "C" void pc_p2_coop_set_movie_protagonist(void* navi) { sMovieProtagonist = navi; }

extern "C" void* pc_p2_coop_take_movie_protagonist(void)
{
	void* navi = sMovieProtagonist;
	sMovieProtagonist = nullptr;
	return navi;
}
