/**
 * @file pc_pad_axis_test.cpp
 * @brief Checks the SDL-axis to pad-axis conversion.
 *
 * Written after a player reported that slamming the left stick fully up
 * registered as holding down, while easing it up worked. SDL axes reach
 * -32768; negating that gives 32768, and 32768/256 is 128, one past what a
 * signed byte holds, so it wrapped to -128 -- the opposite extreme.
 *
 * No controller needed: the conversion is arithmetic, and the failure lives at
 * the very ends of the range.
 */

#include "pc_window.h"

#include <cstdio>

namespace {

int sFailures = 0;

void expect(int actual, int expected, const char* what)
{
	if (actual != expected) {
		std::printf("FAIL: %s -> got %d, expected %d\n", what, actual, expected);
		++sFailures;
	}
}

/// What the port does for a vertical axis: SDL is Y-down, the pad is Y-up.
int verticalFromSdl(int sdl) { return pc_pad_axis_from_sdl(-sdl); }

} // namespace

int main()
{
	// The reported bug. Fully up is SDL -32768; it must read as fully up on the
	// pad, not fully down.
	expect(verticalFromSdl(-32768), 127, "stick fully up");
	expect(verticalFromSdl(32767), -127, "stick fully down");

	// The horizontal axis has the same range and the same trap.
	expect(pc_pad_axis_from_sdl(-32768), -127, "stick fully left");
	expect(pc_pad_axis_from_sdl(32767), 127, "stick fully right");

	// Neutral and the gentle deflections that always worked.
	expect(pc_pad_axis_from_sdl(0), 0, "centred");
	expect(verticalFromSdl(-8000), 31, "eased up");
	expect(verticalFromSdl(8000), -31, "eased down");

	// Nothing may leave the signed-byte range, at any input, and the sign must
	// follow the input throughout.
	for (int v = -32768; v <= 32767; v++) {
		const int out = pc_pad_axis_from_sdl(v);
		if (out < -127 || out > 127) {
			std::printf("FAIL: input %d left the range: %d\n", v, out);
			++sFailures;
			break;
		}
		if (v <= -256 && out >= 0) {
			std::printf("FAIL: negative input %d gave %d\n", v, out);
			++sFailures;
			break;
		}
		if (v >= 256 && out <= 0) {
			std::printf("FAIL: positive input %d gave %d\n", v, out);
			++sFailures;
			break;
		}
	}

    // A trigger axis normally supplies its matching GameCube trigger as a
    // fallback. Once remapped to another action, that fallback must stop or a
    // swapped L/R press would report both triggers at once.
    int bindings[PC_KEY_ACT_COUNT];
    for (int& binding : bindings) binding = -1;
    const int leftTrigger = PC_GP_AXIS_BIND + SDL_CONTROLLER_AXIS_TRIGGERLEFT * 2 + 1;
    const int rightTrigger = PC_GP_AXIS_BIND + SDL_CONTROLLER_AXIS_TRIGGERRIGHT * 2 + 1;
    expect(pc_gamepad_axis_is_mapped_elsewhere(bindings, PC_KEY_ACT_COUNT,
                                               SDL_CONTROLLER_AXIS_TRIGGERLEFT, PC_KEY_ACT_L),
           false, "unmapped left trigger keeps native fallback");
    bindings[PC_KEY_ACT_L] = rightTrigger;
    bindings[PC_KEY_ACT_R] = leftTrigger;
    expect(pc_gamepad_axis_is_mapped_elsewhere(bindings, PC_KEY_ACT_COUNT,
                                               SDL_CONTROLLER_AXIS_TRIGGERLEFT, PC_KEY_ACT_L),
           true, "swapped left trigger suppresses native L");
    expect(pc_gamepad_axis_is_mapped_elsewhere(bindings, PC_KEY_ACT_COUNT,
                                               SDL_CONTROLLER_AXIS_TRIGGERRIGHT, PC_KEY_ACT_R),
           true, "swapped right trigger suppresses native R");

    // Explicitly binding a trigger to its own native action is not a remap.
    bindings[PC_KEY_ACT_L] = leftTrigger;
    bindings[PC_KEY_ACT_R] = rightTrigger;
    expect(pc_gamepad_axis_is_mapped_elsewhere(bindings, PC_KEY_ACT_COUNT,
                                               SDL_CONTROLLER_AXIS_TRIGGERLEFT, PC_KEY_ACT_L),
           false, "native left binding keeps analog fallback");

	if (sFailures == 0) {
		std::printf("pc_pad_axis_test: all checks passed\n");
		return 0;
	}
	std::printf("pc_pad_axis_test: %d failure(s)\n", sFailures);
	return 1;
}
