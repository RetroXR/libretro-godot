#pragma once

/* Controller display interface: a screen on a controller, or on a card plugged
 * into one -- a Dreamcast VMU's LCD.
 *
 * Not part of upstream libretro. Kept byte-identical in libretro-godot
 * (src/ControllerDisplayInterface.hpp) and the flycast fork
 * (shell/libretro/ControllerDisplayInterface.h) so the two can be diffed
 * directly.
 *
 * A core that gets this interface hands each device screen over whenever it
 * changes. A frontend that does not answer, or that declines a screen, leaves
 * the core showing that screen however it otherwise would.
 */

#include <stdbool.h>
#include <stdint.h>

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RETRO_ENVIRONMENT_GET_CONTROLLER_DISPLAY_INTERFACE (1 | RETRO_ENVIRONMENT_PRIVATE)

/* Bumped whenever the struct below changes. The frontend writes the version it
 * filled in; a core needing a later one must leave the interface unused. */
#define RETRO_CONTROLLER_DISPLAY_INTERFACE_VERSION 1

struct retro_controller_display_interface
{
   /* Written by the frontend. */
   unsigned interface_version;

   /* Passed back to every call below. */
   void *frontend_data;

   /* The screen of device `index` on the controller in `port` changed. For a
    * Dreamcast, `index` is the expansion slot the card sits in.
    *
    * `pixels` is `width` * `height` XRGB8888 words, row by row from the top, in
    * the device's own addressing: row 0 and column 0 are the top left of the
    * screen as the device is held on its own, whichever way round it sits in
    * the controller. The frontend copies what it keeps. May be called from any
    * thread.
    *
    * Returns false when the frontend does not show this screen. */
   bool (*refresh)(void *frontend_data, unsigned port, unsigned index,
                   const uint32_t *pixels, unsigned width, unsigned height);
};

#ifdef __cplusplus
}
#endif
