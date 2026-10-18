/**
 * @file pc_pad_clamp_test.cpp
 * @brief Checks GameCube PADClamp on already-scaled stick bytes.
 *
 * No window and no controller. The vectors are the integer results of
 * src/pad/Padclamp.c (dead zone 15, stick octagon 72/40, C-stick 59/31,
 * triggers 30..180 then shifted by 30).
 */

#include "Dolphin/pad.h"

#include <cstdio>
#include <cstring>

namespace {

int sFailures = 0;

void expect(int actual, int expected, const char* what)
{
	if (actual != expected) {
		std::printf("FAIL: %s -> got %d, expected %d\n", what, actual, expected);
		++sFailures;
	}
}

void fillDisconnected(PADStatus pads[4])
{
	std::memset(pads, 0, sizeof(PADStatus) * 4);
	for (int i = 0; i < 4; ++i) {
		pads[i].err = PAD_ERR_NO_CONTROLLER;
	}
}

struct StickCase {
	const char* name;
	s8 sx, sy, cx, cy;
	s8 esx, esy, ecx, ecy;
	u8 tl, tr, etl, etr;
};

void checkStick(const StickCase& c)
{
	PADStatus pads[4];
	fillDisconnected(pads);
	pads[0].err          = PAD_ERR_NONE;
	pads[0].button       = PAD_BUTTON_A;
	pads[0].stickX       = c.sx;
	pads[0].stickY       = c.sy;
	pads[0].substickX    = c.cx;
	pads[0].substickY    = c.cy;
	pads[0].triggerLeft  = c.tl;
	pads[0].triggerRight = c.tr;
	pads[0].analogA      = 9;
	pads[0].analogB      = 8;

	// A disconnected channel must keep whatever bytes it already holds.
	pads[1].stickX      = 127;
	pads[1].stickY      = -127;
	pads[1].substickX   = 127;
	pads[1].substickY   = 127;
	pads[1].triggerLeft = 255;
	pads[1].triggerRight = 255;
	pads[1].button      = PAD_BUTTON_B;

	PADClamp(pads);

	expect(pads[0].stickX, c.esx, c.name);
	expect(pads[0].stickY, c.esy, c.name);
	expect(pads[0].substickX, c.ecx, c.name);
	expect(pads[0].substickY, c.ecy, c.name);
	expect(pads[0].triggerLeft, c.etl, c.name);
	expect(pads[0].triggerRight, c.etr, c.name);
	expect(pads[0].button, PAD_BUTTON_A, c.name);
	expect(pads[0].analogA, 9, c.name);
	expect(pads[0].analogB, 8, c.name);
	expect(pads[0].err, PAD_ERR_NONE, c.name);

	expect(pads[1].stickX, 127, "disconnected stick X");
	expect(pads[1].stickY, -127, "disconnected stick Y");
	expect(pads[1].substickX, 127, "disconnected C-stick X");
	expect(pads[1].substickY, 127, "disconnected C-stick Y");
	expect(pads[1].triggerLeft, 255, "disconnected trigger L");
	expect(pads[1].triggerRight, 255, "disconnected trigger R");
	expect(pads[1].button, PAD_BUTTON_B, "disconnected buttons");
	expect(pads[1].err, PAD_ERR_NO_CONTROLLER, "disconnected err");
}

} // namespace

int main()
{
	// Dead zone is inclusive. One step past it is shifted down by 15, not scaled.
	const StickCase cases[] = {
	    {"neutral", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	    {"stick dead zone", 15, -15, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0},
	    {"just outside dead zone", 16, 0, 0, 16, 1, 0, 0, 1, 0, 0, 0, 0},
	    {"inside octagon", 30, 30, 20, 0, 15, 15, 5, 0, 0, 0, 0, 0},
	    // 127 - 15 = 112, then the cardinal cap is maxStick 72.
	    {"stick cardinal cap", 127, 0, 0, 0, 72, 0, 0, 0, 0, 0, 0, 0},
	    {"stick cardinal cap negative", -127, 0, 0, 0, -72, 0, 0, 0, 0, 0, 0, 0},
	    // 87 - 15 = 72, so this raw tilt already sits on the cap.
	    {"stick at cap edge", 87, 0, 0, 0, 72, 0, 0, 0, 0, 0, 0, 0},
	    {"stick one below cap", 86, 0, 0, 0, 71, 0, 0, 0, 0, 0, 0, 0},
	    // Equal axes take the diagonal corner (xyStick 40).
	    {"stick diagonal", 127, 127, 0, 0, 40, 40, 0, 0, 0, 0, 0, 0},
	    {"stick diagonal signs", -127, 127, 0, 0, -40, 40, 0, 0, 0, 0, 0, 0},
	    // First octagon branch: (90,20) -> (75,5) -> (68,4).
	    {"stick octagon x-major", 90, 20, 0, 0, 68, 4, 0, 0, 0, 0, 0, 0},
	    // Second octagon branch: (20,127) -> (5,112) -> (3,69).
	    {"stick octagon y-major", 20, 127, 0, 0, 3, 69, 0, 0, 0, 0, 0, 0},
	    // C-stick cap is 59 cardinal and 31 on the diagonal.
	    {"c-stick cardinal", 0, 0, 127, 0, 0, 0, 59, 0, 0, 0, 0, 0},
	    {"c-stick diagonal", 0, 0, -127, -127, 0, 0, -31, -31, 0, 0, 0, 0},
	    // Triggers: <= 30 becomes 0; above that, cap at 180 then subtract 30.
	    {"trigger dead zone", 0, 0, 0, 0, 0, 0, 0, 0, 30, 31, 0, 1},
	    {"trigger cap", 0, 0, 0, 0, 0, 0, 0, 0, 255, 180, 150, 150},
	    {"trigger one past cap", 0, 0, 0, 0, 0, 0, 0, 0, 181, 179, 150, 149},
	};

	for (const StickCase& c : cases) {
		checkStick(c);
	}

	// Channel 3 is clamped too. The routine walks every SI channel.
	{
		PADStatus pads[4];
		fillDisconnected(pads);
		pads[3].err    = PAD_ERR_NONE;
		pads[3].stickX = 127;
		pads[3].stickY = 127;
		PADClamp(pads);
		expect(pads[3].stickX, 40, "channel 3 diagonal X");
		expect(pads[3].stickY, 40, "channel 3 diagonal Y");
		expect(pads[0].err, PAD_ERR_NO_CONTROLLER, "channel 0 still disconnected");
	}

	if (sFailures == 0) {
		std::printf("pc_pad_clamp_test: all checks passed\n");
		return 0;
	}
	std::printf("pc_pad_clamp_test: %d failure(s)\n", sFailures);
	return 1;
}
