#pragma once

// Transfer Pak interface: how the frontend says which Game Boy cartridge is in
// which controller's Transfer Pak.
//
// Not part of upstream libretro, and deliberately NOT patched into the vendored
// external/libretro-common submodule, which tracks upstream and should stay
// updatable. The block below is maintained byte-identically in the core fork's
// libretro/transferpak_interface.h so the two can be diffed directly.
//
// mupen64plus-core already resolves the cartridge PER CONTROLLER: init_gb_rom
// and init_gb_ram are handed a control_id and ask g_media_loader for that port's
// files. What the libretro layer never had is a way for the frontend to answer
// per port -- the two routes that exist, the `gb` subsystem and the .gb sidecar,
// both set one pair of globals shared by all four ports and both are fixed at
// load. One cartridge, in every pak, for the whole session.
//
// This is the missing half: the core ASKS, at the moment a pak is populated,
// naming the port. A frontend that does not answer leaves the core exactly as it
// was -- the media loader hooks stay NULL and the global/sidecar fallback still
// applies -- so a RetroArch user sees no change at all.

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The number is provisional. A core probes the EXPERIMENTAL form first and falls
 * back to the plain one, so a core built today keeps working against a frontend
 * that later adopts the unflagged number. */
#define RETRO_ENVIRONMENT_GET_TRANSFER_PAK_INTERFACE       (95 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_TRANSFER_PAK_INTERFACE_FINAL 95

/* How many controllers may hold a pak. Matches GAME_CONTROLLERS_COUNT; declared
 * here too so the frontend side of this contract does not have to include the
 * core's headers to know the range of `port`. */
#define RETRO_TRANSFER_PAK_PORTS 4

struct retro_transfer_pak_interface
{
   /* Passed back to every call below. */
   void *frontend_data;

   /* The Game Boy ROM in the Transfer Pak on `port` (0-3), or NULL for a pak
    * with no cartridge in it -- or for a port with no Transfer Pak at all.
    *
    * The string is owned by the FRONTEND and must stay valid until the next call
    * on the same port; the core copies it before use. */
   const char *(*get_rom)(void *frontend_data, unsigned port);

   /* Where that cartridge's battery save lives. Same ownership as get_rom.
    *
    * NULL leaves the core to name a file itself, which on this core means the
    * save is silently discarded -- get_gb_ram_path is stubbed. A frontend that
    * wants the save kept must answer this. */
   const char *(*get_ram)(void *frontend_data, unsigned port);

   /* A counter the frontend bumps whenever the CARTRIDGE in `port`'s pak changes
    * without the pak itself being removed.
    *
    * Needed because the core only re-reads a cartridge when the pak TYPE
    * transitions, so swapping one cartridge for another while the pak stays
    * seated would otherwise go unnoticed for the rest of the session. May be
    * NULL, in which case only a pak change re-reads. */
   unsigned (*generation)(void *frontend_data, unsigned port);
};

/* Where the real-time clock of the cartridge in a Transfer Pak is kept.
 *
 * A call of its own rather than a member above: retro_transfer_pak_interface has
 * no version field, and a frontend filling a longer struct than an older core
 * allocated would write past its end. A core that never asks keeps no clock, and a
 * frontend that does not answer leaves the clock unkept. */
#define RETRO_ENVIRONMENT_GET_TRANSFER_PAK_CLOCK_INTERFACE       (97 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#define RETRO_ENVIRONMENT_GET_TRANSFER_PAK_CLOCK_INTERFACE_FINAL 97

struct retro_transfer_pak_clock_interface
{
   /* Passed back to every call below. */
   void *frontend_data;

   /* The file that keeps the clock of the cartridge in `port`'s pak, or NULL/""
    * to keep none. Same ownership as get_ram. Asked only for a cartridge that has
    * a clock. The core keeps 48 bytes: little-endian uint32 seconds, minutes,
    * hours, days low and days high, the same five latched, then a uint64 host
    * time. */
   const char *(*get_rtc)(void *frontend_data, unsigned port);
};

#ifdef __cplusplus
}
#endif
