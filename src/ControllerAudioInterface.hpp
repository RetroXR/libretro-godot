#pragma once

/* Controller audio interface: sound that comes out of a controller rather than
 * out of the console -- a Wii Remote's speaker, a VMU's buzzer.
 *
 * Not part of upstream libretro. Kept byte-identical in libretro-godot
 * (src/ControllerAudioInterface.hpp) and the Dolphin fork
 * (Source/Core/DolphinLibretro/ControllerAudioInterface.h) so the two can be
 * diffed directly.
 *
 * Without it a core can only mix such sound into its main stream, which a
 * frontend plays from wherever the picture is. With it the core hands each
 * device's sound over on its own and the frontend can play it from the
 * controller. A frontend that does not answer, or that declines a block, leaves
 * the core mixing that sound into the main stream as before.
 */

#include <stddef.h>
#include <stdint.h>

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Provisional, and EXPERIMENTAL until upstream assigns it: a retroXR number
 * past every one upstream libretro has assigned or has an open proposal for.
 * Upstream took 94-106 after retroXR first numbered these calls there. */
#define RETRO_ENVIRONMENT_GET_CONTROLLER_AUDIO_INTERFACE       (110 | RETRO_ENVIRONMENT_EXPERIMENTAL)

/* Bumped whenever the struct below changes. The frontend writes the version it
 * filled in; a core needing a later one must leave the interface unused. */
#define RETRO_CONTROLLER_AUDIO_INTERFACE_VERSION 1

struct retro_controller_audio_interface
{
   /* Written by the frontend. */
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
};

#ifdef __cplusplus
}
#endif
