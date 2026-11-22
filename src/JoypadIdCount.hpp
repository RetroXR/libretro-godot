#pragma once

// How many RETRO_DEVICE_JOYPAD ids the frontend answers:
// RETRO_ENVIRONMENT_GET_JOYPAD_ID_COUNT, a RetroXR call (114, provisional, as
// 110-113 are). Copies, kept identical: libretro-godot
// src/JoypadIdCount.hpp (the frontend) and RetroXR's Cemu fork
// src/libretro/JoypadIdCount.h (a core using it).
//
// libretro.h defines joypad ids 0-15 (and the MASK, 256). A core must never ask
// for an id past them on its own: RetroArch reads RETRO_DEVICE_JOYPAD ids
// 16 and up as its own binds (input_state_wrap: id >= RARCH_FIRST_CUSTOM_BIND,
// so 16 is RARCH_ANALOG_LEFT_X_PLUS, the left stick pushed right). A frontend
// that answers this call says ids 0 .. count-1 are buttons it means; one that
// does not know it returns false, and the core keeps to the sixteen.
//
// Ids by RetroXR's numbering past the sixteen:
//   16  HOME (a Wii U GamePad's or Pro Controller's)

#include <libretro.h>

#ifndef RETRO_ENVIRONMENT_GET_JOYPAD_ID_COUNT

/**
 * @param[out] data <tt>unsigned *</tt>: the number of joypad ids the frontend
 * answers, at least 16.
 * @return \c true if the frontend knows the call.
 */
#define RETRO_ENVIRONMENT_GET_JOYPAD_ID_COUNT (114 | RETRO_ENVIRONMENT_EXPERIMENTAL)

/** HOME, past the RetroPad's sixteen; valid only when the count is above it. */
#define RETRO_DEVICE_ID_JOYPAD_RETROXR_HOME 16

#endif
