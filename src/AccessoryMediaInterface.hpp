#pragma once

// The accessory media interface, as a frontend sees it.
//
// A COPY, kept byte-identical (the block between the two marker lines) with
// the cores that publish it: RetroXR's Cemu fork
// (src/libretro/AccessoryMediaInterface.h) and mGBA fork
// (src/platform/libretro/accessory_media.h). It is not part of upstream
// libretro; when something like it lands there, this file goes away.
//
// Diff two copies with:
//   extract() { awk '/^\/\* -+$/{f=1} f{print} f&&/^\/\* --- end/{exit}' "$1"; }
//   diff <(extract A) <(extract B)

#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Accessory media interface (retroXR extension).
 *
 * Some machines read media that is not their game: an amiibo held to a Wii U
 * GamePad, a dot-code strip swiped through a Game Boy Advance e-Reader, a
 * figure stood on a portal. libretro has one interface for handing a running
 * core a new medium, disk control, and it is the wrong one: it describes a
 * drive with a tray, an image list and a current index, so a frontend shows a
 * disc menu for a card reader, and a core that means "a card was swiped" has
 * to spell it as "the tray closed on image N".
 *
 * Here the core describes its READERS instead -- what kind of medium each one
 * takes, which player it belongs to, and how it holds what it is given -- and
 * the frontend hands a medium to a reader, or takes it away. Nothing is an
 * image list: a reader holds one medium at a time.
 *
 * The core calls RETRO_ENVIRONMENT_SET_ACCESSORY_MEDIA_INTERFACE with a filled
 * struct retro_accessory_media_interface, from retro_load_game or later (from
 * retro_run too, while running). It calls it again whenever its readers change
 * -- a cartridge with a reader is reset, a GamePad is unplugged -- and with
 * num_readers 0 to withdraw them all. The struct and the reader array it points
 * to belong to the core and stay valid until the next call or retro_unload_game;
 * the frontend copies what it keeps.
 *
 * A frontend that does not know this call returns false, and the core does
 * whatever it did before -- carrying on with disk control, say. Nothing about
 * the call needs a frontend to answer it.
 *
 * The number is in the RETRO_ENVIRONMENT_PRIVATE range on purpose: upstream
 * libretro has since claimed the experimental numbers 94-104, where earlier
 * retroXR extensions were provisionally placed.
 * ------------------------------------------------------------------------- */

#define RETRO_ENVIRONMENT_SET_ACCESSORY_MEDIA_INTERFACE (2 | RETRO_ENVIRONMENT_PRIVATE)

/* Bumped whenever the structs below change. Written by the core; a frontend
 * that only knows an earlier version must leave the interface unused. */
#define RETRO_ACCESSORY_MEDIA_INTERFACE_VERSION 1

/* What a reader takes. Append only: the values are on the wire between
 * independently built cores and frontends. */
enum retro_accessory_media_kind
{
   /* An NFC tag dump. For an amiibo, the NTAG215's whole memory: 540 bytes,
    * or 532 / 572 as other tools write it. */
   RETRO_ACCESSORY_MEDIA_NFC_TAG = 0,
   /* A Nintendo e-Reader dot-code strip, as raw decoded data (the .raw / .bin
    * a strip scanner or nedcenc writes). */
   RETRO_ACCESSORY_MEDIA_DOT_CODE = 1
};

/* The reader takes a medium only for a moment and lets go of it by itself: a
 * strip passes through a slot, a tag is touched and lifted. remove() is then
 * rarely needed; is_present() says when the core has let go. Without this flag
 * a medium stays on the reader until remove(). */
#define RETRO_ACCESSORY_READER_MOMENTARY (1u << 0)

/* The core may WRITE the medium back to the path it was given (an amiibo game
 * saving to the figure). A frontend that wants to keep the original hands the
 * core a copy. */
#define RETRO_ACCESSORY_READER_WRITES    (1u << 1)

/* reader.port for a reader on the console rather than on a player's
 * controller (an e-Reader cartridge). */
#define RETRO_ACCESSORY_PORT_CONSOLE     (~0u)

struct retro_accessory_reader
{
   /* enum retro_accessory_media_kind. */
   unsigned kind;
   /* The controller port the reader is part of (0 is player 1), or
    * RETRO_ACCESSORY_PORT_CONSOLE. */
   unsigned port;
   /* RETRO_ACCESSORY_READER_* bits. */
   unsigned flags;
   /* What to call it, for a player: "Wii U GamePad NFC", "e-Reader". */
   const char *name;
};

struct retro_accessory_media_interface
{
   /* Written by the core: RETRO_ACCESSORY_MEDIA_INTERFACE_VERSION. */
   unsigned interface_version;

   unsigned num_readers;
   const struct retro_accessory_reader *readers;

   /* Put the medium at `path` on reader `reader` (an index into `readers`).
    * The core reads the file during the call or keeps the path for later
    * (and, with RETRO_ACCESSORY_READER_WRITES, writes it back there), so the
    * file must stay where it is while the medium is on the reader. A medium
    * already on the reader is replaced.
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

/* --- end of the accessory media interface block -------------------------- */

#ifdef __cplusplus
}
#endif
