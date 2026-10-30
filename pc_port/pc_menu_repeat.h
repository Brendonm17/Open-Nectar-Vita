#ifndef PC_MENU_REPEAT_H
#define PC_MENU_REPEAT_H

#include <cstdint>

/// Held menu input to one-shot presses with auto-repeat.
bool pc_menu_edge(bool pressed, int slot, std::uint32_t nowMs);

/// Hold time before repeat.
constexpr std::uint32_t kPcMenuRepeatDelayMs = 450;

/// Repeat interval.
constexpr std::uint32_t kPcMenuRepeatRateMs = 175;

/// Clears all slot state.
void pc_menu_edge_reset(void);

#endif // PC_MENU_REPEAT_H
