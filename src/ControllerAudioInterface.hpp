#pragma once

/* Controller audio interface: sound that comes out of a device rather than out
 * of the console -- a Wii Remote's speaker, a VMU's buzzer, a docked
 * PocketStation's speaker.
 *
 * Not part of upstream libretro. Kept byte-identical in libretro-godot
 * (src/ControllerAudioInterface.hpp), the Dolphin fork
 * (Source/Core/DolphinLibretro/ControllerAudioInterface.h), the flycast fork
 * (shell/libretro/ControllerAudioInterface.h) and the pcsx_rearmed fork
 * (frontend/ControllerAudioInterface.h).
 *
 * Without it a core can only mix such sound into its main stream, which a
 * frontend plays from wherever the picture is. With it the core hands each
 * device's sound over on its own and the frontend can play it from the
 * device. A frontend that does not answer, or that declines a block, leaves
 * the core mixing that sound into the main stream as before.
 */

#include <stddef.h>
#include <stdint.h>

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Provisional; EXPERIMENTAL until upstream assigns a number. */
#define RETRO_ENVIRONMENT_GET_CONTROLLER_AUDIO_INTERFACE       (110 | RETRO_ENVIRONMENT_EXPERIMENTAL)

/* Bumped whenever the struct below changes.
 *
 * Before the call the core sets interface_version to the version it was built
 * against; a core built against version 1 passes 0 or 1. The frontend fills no
 * member past that version and writes back the version it filled. A core
 * needing a later one than it gets back must leave those members unused. */
#define RETRO_CONTROLLER_AUDIO_INTERFACE_VERSION 2

/* Where a device is (version 2). Append only. The same values as the device
 * display interface's locations. */
enum retro_controller_audio_location
{
   /* On the controller in `port`; `index` is its slot there. */
   RETRO_CONTROLLER_AUDIO_CONTROLLER  = 0,
   /* In memory card slot `port` (0 is slot 1); `index` is 0. */
   RETRO_CONTROLLER_AUDIO_MEMORY_CARD = 1
};

struct retro_controller_audio_interface
{
   unsigned interface_version;

   /* Passed back to every call below. */
   void *frontend_data;

   /* One block of sound from device `index` on the controller in `port`: a Wii
    * Remote's speaker is index 0, a VMU's buzzer the slot the card sits in.
    *
    * `data` is `frames` frames of interleaved stereo s16 at the core's own output
    * rate, the rate of its audio batch callback. Call it on the thread that calls
    * that callback, just before the batch covering the same stretch of emulated
    * time, and keep calling it with silence while the device is quiet so the
    * frontend can hold the two streams in step.
    *
    * Returns false when the frontend cannot play this block on its own, and the
    * core should mix it into the main stream instead. Returns true when the block
    * was taken, including when the frontend chose to discard it. */
   bool (*push)(void *frontend_data, unsigned port, unsigned index,
                const int16_t *data, size_t frames);

   /* Version 2. push() for the device at `location`
    * (retro_controller_audio_location); push() is this with
    * RETRO_CONTROLLER_AUDIO_CONTROLLER. Same rules as push(). */
   bool (*push_device)(void *frontend_data, unsigned location, unsigned port,
                       unsigned index, const int16_t *data, size_t frames);
};

#ifdef __cplusplus
}
#endif
