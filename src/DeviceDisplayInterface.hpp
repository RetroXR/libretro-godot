#pragma once

/* Device display interface: a screen that is not the console's -- a Dreamcast VMU's
 * LCD in a controller, a PocketStation's in a PlayStation memory card slot.
 *
 * Not part of upstream libretro. Kept byte-identical in libretro-godot
 * (src/DeviceDisplayInterface.hpp), the flycast fork
 * (shell/libretro/DeviceDisplayInterface.h) and the pcsx_rearmed fork
 * (frontend/DeviceDisplayInterface.h) so they can be diffed directly.
 *
 * Without it a core can only draw such a screen over its own picture, or not at
 * all. With it the core hands each device's screen over whenever it changes and
 * the frontend can show it on the device. A frontend that does not answer, or that
 * declines a screen, leaves the core showing that screen however it otherwise
 * would.
 *
 * Replaces the controller display interface (RETRO_ENVIRONMENT_PRIVATE | 1),
 * which could only name a screen on a controller. A frontend may still answer
 * that call for cores built before this one.
 */

#include <stdbool.h>
#include <stdint.h>

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The number is provisional. A core probes the EXPERIMENTAL form first and falls
 * back to the plain one, so a core built today keeps working against a frontend
 * that later adopts the unflagged number. */
#define RETRO_ENVIRONMENT_GET_DEVICE_DISPLAY_INTERFACE       (97 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_DEVICE_DISPLAY_INTERFACE_FINAL 97

/* Bumped whenever the struct below changes. The frontend writes the version it
 * filled in; a core needing a later one must leave the interface unused. */
#define RETRO_DEVICE_DISPLAY_INTERFACE_VERSION 1

/* Where the device is. Append only. */
enum retro_device_display_location
{
   /* On the controller in `port`; `index` is its slot there. For a Dreamcast,
    * the expansion slot a VMU sits in. */
   RETRO_DEVICE_DISPLAY_CONTROLLER = 0,
   /* In the memory card slot `port` (0 is slot 1); `index` is 0. A PocketStation
    * docked in a PlayStation. */
   RETRO_DEVICE_DISPLAY_MEMORY_CARD = 1
};

struct retro_device_display_interface
{
   /* Written by the frontend. */
   unsigned interface_version;

   /* Passed back to every call below. */
   void *frontend_data;

   /* The screen of the device at `location`, `port`, `index` changed.
    *
    * `pixels` is `width` * `height` XRGB8888 words, row by row from the top, in
    * the device's own addressing: row 0 and column 0 are the top left of the
    * screen as the device is held on its own, whichever way round it sits in the
    * controller or slot. A screen the device itself turns (a PocketStation in its
    * dock) is handed over as the device draws it. The frontend copies what it
    * keeps. May be called from any thread.
    *
    * Returns false when the frontend does not show this screen. */
   bool (*refresh)(void *frontend_data, unsigned location, unsigned port, unsigned index,
                   const uint32_t *pixels, unsigned width, unsigned height);
};

#ifdef __cplusplus
}
#endif
