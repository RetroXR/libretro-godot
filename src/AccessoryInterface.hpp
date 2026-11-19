#pragma once

// The accessory interface, RETRO_ENVIRONMENT_SET_ACCESSORY_INTERFACE.
//
// A COPY, kept byte-identical (the block between the two marker lines) with
// RetroXR's mGBA fork (src/platform/libretro/accessory_interface.h), the core
// that publishes it. It is not part of upstream libretro; when it lands there,
// this file goes away and the definitions come from libretro.h.
//
// Diff two copies with:
//   extract() { awk '/^\/\* -+$/{f=1} f{print} f&&/^\/\* --- end/{exit}' "$1"; }
//   diff <(extract A) <(extract B)

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Accessory interface (retroXR extension).
 *
 * Some machines read media that is not their game and not an NFC tag: a
 * dot-code strip swiped through a Game Boy Advance e-Reader, and the like.
 * libretro has one interface for handing a running core a new medium, disk
 * control, and it is the wrong one: it describes a drive with a tray, an image
 * list and a current index, so a frontend shows a disc menu for a card reader,
 * and a core that means "a card was swiped" has to spell it as "the tray closed
 * on image N".
 *
 * Here the core describes its READERS instead -- what kind of medium each one
 * takes, which player it belongs to, and how it holds what it is given -- and
 * the frontend hands a medium to a reader, or takes it away. A reader holds one
 * medium at a time.
 *
 * NFC tags are not here: RETRO_ENVIRONMENT_GET_NFC_INTERFACE (proposed in
 * libretro/RetroArch#19754) is a reader the frontend owns and the core reads
 * and writes in bytes, which is what an amiibo needs.
 *
 * The core calls RETRO_ENVIRONMENT_SET_ACCESSORY_INTERFACE with a filled
 * struct retro_accessory_interface, from retro_load_game or later (from
 * retro_run too, while running). It calls it again whenever its readers change
 * -- a cartridge with a reader is reset -- and with num_readers 0 to withdraw
 * them all. The struct and the reader array it points to belong to the core
 * and stay valid until the next call or retro_unload_game; the frontend copies
 * what it keeps.
 *
 * A frontend that does not know this call returns false, and the core does
 * whatever it did before -- carrying on with disk control, say.
 * ------------------------------------------------------------------------- */

#define RETRO_ENVIRONMENT_SET_ACCESSORY_INTERFACE (107 | RETRO_ENVIRONMENT_EXPERIMENTAL)

/* Bumped whenever the structs below change. Written by the core; a frontend
 * that only knows an earlier version must leave the interface unused. */
#define RETRO_ACCESSORY_INTERFACE_VERSION 1

/* What a reader takes. Append only: the values are on the wire between
 * independently built cores and frontends. */
enum retro_accessory_kind
{
   /* A Nintendo e-Reader dot-code strip, as raw decoded data (the .raw / .bin
    * a strip scanner or nedcenc writes). */
   RETRO_ACCESSORY_DOT_CODE = 0
};

/* The reader takes a medium only for a moment and lets go of it by itself: a
 * strip passes through a slot. remove() is then rarely needed; is_present()
 * says when the core has let go. Without this flag a medium stays on the
 * reader until remove(). */
#define RETRO_ACCESSORY_READER_MOMENTARY (1u << 0)

/* reader.port for a reader on the console rather than on a player's
 * controller (an e-Reader cartridge). */
#define RETRO_ACCESSORY_PORT_CONSOLE     (~0u)

struct retro_accessory_reader
{
   /* enum retro_accessory_kind. */
   unsigned kind;
   /* The controller port the reader is part of (0 is player 1), or
    * RETRO_ACCESSORY_PORT_CONSOLE. */
   unsigned port;
   /* RETRO_ACCESSORY_READER_* bits. */
   unsigned flags;
   /* What to call it, for a player: "e-Reader". */
   const char *name;
};

struct retro_accessory_interface
{
   /* Written by the core: RETRO_ACCESSORY_INTERFACE_VERSION. */
   unsigned interface_version;

   unsigned num_readers;
   const struct retro_accessory_reader *readers;

   /* Put the medium at `path` on reader `reader` (an index into `readers`).
    * The core reads the file during the call or keeps the path for later, so
    * the file must stay where it is while the medium is on the reader. A
    * medium already on the reader is replaced.
    *
    * Returns false when the medium is refused -- the file cannot be read, is
    * not that kind of medium, or nothing is running to read it -- and the
    * reader is then as it was. May be called from any thread; a core that must
    * act on its own thread takes the medium there, before its next frame. */
   bool (*insert)(unsigned reader, const char *path);

   /* Take whatever is on reader `reader` off it. True when the reader is empty
    * afterwards, including when it already was. */
   bool (*remove)(unsigned reader);

   /* Whether a medium is on reader `reader` now. NULL when the core cannot
    * tell, in which case a frontend should treat a MOMENTARY reader as empty
    * once insert() has returned. */
   bool (*is_present)(unsigned reader);
};

/* --- end of the accessory interface block -------------------------------- */

#ifdef __cplusplus
}
#endif
