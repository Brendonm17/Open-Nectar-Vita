/**
 * @file pc_pad_axis.cpp
 * @brief SDL axis to pad axis conversion.
 *
 * Its own translation unit so a test can link it without dragging in the whole
 * window, renderer and audio layer.
 */

#include "pc_window.h"

s8 pc_pad_axis_from_sdl(int sdlAxisValue) {
    // SDL axes run to -32768, so negating one gives 32768, and 32768/256 is
    // 128 -- one past what a signed byte holds, which wraps to -128. Pushing a
    // stick fully in one direction therefore registered as fully the other way,
    // and only at the very end of its travel, which is why easing the stick
    // worked and slamming it did not.
    const int scaled = sdlAxisValue / 256;
    if (scaled > 127) return 127;
    if (scaled < -127) return -127;
    return static_cast<s8>(scaled);
}

bool pc_gamepad_axis_is_mapped_elsewhere(const int* bindings, int bindingCount,
                                         int axis, int nativeAction) {
    if (!bindings || bindingCount <= 0 || axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX)
        return false;

    for (int action = 0; action < bindingCount; ++action) {
        if (action == nativeAction)
            continue;
        const int bind = bindings[action];
        if (bind < PC_GP_AXIS_BIND)
            continue;
        const int boundAxis = (bind - PC_GP_AXIS_BIND) / 2;
        if (boundAxis == axis)
            return true;
    }
    return false;
}
