/* SDL axis to pad axis conversion. */

#include "pc_window.h"

s8 pc_pad_axis_from_sdl(int sdlAxisValue) {
    // Clamped to [-127, 127].
    const int scaled = sdlAxisValue / 256;
    if (scaled > 127) return 127;
    if (scaled < -127) return -127;
    return static_cast<s8>(scaled);
}
