#include "pc_menu_pad.h"

namespace {

bool acceptBindingDown(int bind, const PcMenuPadKeys& keys)
{
	if (bind == SDL_SCANCODE_RETURN)
		return keys.enter;
	if (bind == SDL_SCANCODE_KP_ENTER)
		return keys.kpEnter;
	if (bind == SDL_SCANCODE_SPACE)
		return keys.space;
	if (bind == PC_BIND_MOUSE_BASE + SDL_BUTTON_LEFT)
		return keys.clickLeft;
	return false;
}

void setAxis(bool neg, bool pos, std::uint16_t negBit, std::uint16_t posBit,
             std::uint16_t& button, std::int8_t& axis)
{
	const int dir = (pos ? 1 : 0) - (neg ? 1 : 0);
	if (dir < 0) {
		button = static_cast<std::uint16_t>((button | negBit) & ~posBit);
		axis   = -127;
	} else if (dir > 0) {
		button = static_cast<std::uint16_t>((button | posBit) & ~negBit);
		axis   = 127;
	} else {
		button = static_cast<std::uint16_t>(button & ~(negBit | posBit));
		axis   = 0;
	}
}

} // namespace

void pc_menu_pad_apply(const PcMenuPadKeys& keys, bool menuMode, bool nonShiftB,
                       int bindStart, int bindStart2,
                       std::uint16_t& button, std::int8_t& stickX, std::int8_t& stickY)
{
	if (!menuMode)
		return;

	const bool accept = keys.enter || keys.kpEnter || keys.space || keys.clickLeft;
	if (accept)
		button = static_cast<std::uint16_t>(button | PC_MENU_PAD_A);
	// Enter is accept. It must not also be Start, or the world map opens its
	// leave dialog and a pause menu closes instead of confirming.
	if (acceptBindingDown(bindStart, keys) || acceptBindingDown(bindStart2, keys))
		button = static_cast<std::uint16_t>(button & ~PC_MENU_PAD_START);

	button = static_cast<std::uint16_t>(button & ~PC_MENU_PAD_B);
	if (keys.escape || keys.backspace || keys.clickRight || nonShiftB)
		button = static_cast<std::uint16_t>(button | PC_MENU_PAD_B);

	const bool left  = keys.left || keys.keyA || stickX < 0 || (button & PC_MENU_PAD_LEFT) != 0;
	const bool right = keys.right || keys.keyD || stickX > 0 || (button & PC_MENU_PAD_RIGHT) != 0;
	const bool down  = keys.down || keys.keyS || stickY < 0 || (button & PC_MENU_PAD_DOWN) != 0;
	const bool up    = keys.up || keys.keyW || stickY > 0 || (button & PC_MENU_PAD_UP) != 0;
	setAxis(left, right, PC_MENU_PAD_LEFT, PC_MENU_PAD_RIGHT, button, stickX);
	setAxis(down, up, PC_MENU_PAD_DOWN, PC_MENU_PAD_UP, button, stickY);
	(void)keys.shiftL;
	(void)keys.shiftR;
}

bool pc_menu_pad_click(std::uint32_t held, std::uint32_t pressed, int sdlButton)
{
	const std::uint32_t bit = SDL_BUTTON(sdlButton);
	return (held & bit) != 0 || (pressed & bit) != 0;
}
