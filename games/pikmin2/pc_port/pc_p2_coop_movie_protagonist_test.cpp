/**
 * @file pc_p2_coop_movie_protagonist_test.cpp
 * @brief One-shot queue for the co-op captain who starts a non-navi cutscene.
 */

#include "pc_p2_coop.h"

#include <cstdio>

namespace {

int sFailures = 0;

void expect_ptr(void* actual, void* expected, const char* what)
{
	if (actual != expected) {
		std::printf("FAIL: %s -> got %p, expected %p\n", what, actual, expected);
		++sFailures;
	}
}

} // namespace

int main()
{
	char olimar;
	char louie;

	expect_ptr(pc_p2_coop_take_movie_protagonist(), nullptr, "empty queue");

	pc_p2_coop_set_movie_protagonist(&louie);
	expect_ptr(pc_p2_coop_take_movie_protagonist(), &louie, "dequeue louie");

	expect_ptr(pc_p2_coop_take_movie_protagonist(), nullptr, "consumed once");

	pc_p2_coop_set_movie_protagonist(&olimar);
	pc_p2_coop_set_movie_protagonist(&louie);
	expect_ptr(pc_p2_coop_take_movie_protagonist(), &louie, "last writer wins");

	if (sFailures == 0) {
		std::printf("pc_p2_coop_movie_protagonist_test: all checks passed\n");
		return 0;
	}
	std::printf("pc_p2_coop_movie_protagonist_test: %d failure(s)\n", sFailures);
	return 1;
}
