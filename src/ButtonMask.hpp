#pragma once

#include <cstdint>

// One button out of a frontend-held mask of pressed buttons, as input_state
// answers it: 1 or 0, never the bit itself.
//
// The bit itself is what this used to return, and input_state returns an
// int16_t, so any id from 16 up came back as 0 whatever was held. The light
// gun's RELOAD is id 16 (RETRO_DEVICE_ID_LIGHTGUN_RELOAD), so no core was ever
// told a reload; and a joypad's R3, bit 15, came back as -32768. An id past the mask is not held, rather than a shift by 32 or
// more, which C++ leaves undefined.
inline int16_t ButtonFromMask(uint32_t mask, uint32_t id)
{
    return id < 32u ? static_cast<int16_t>((mask >> id) & 1u) : 0;
}
