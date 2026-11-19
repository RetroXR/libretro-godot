#pragma once

// The NFC reader interface, RETRO_ENVIRONMENT_GET_NFC_INTERFACE, interface
// version 5, as proposed upstream in libretro/RetroArch#19754 (not merged).
//
// A COPY of the lines that PR adds to libretro.h, unchanged. The libretro.h
// beside this file tracks upstream and is not patched for an interface that is
// not there yet; when it lands, this file goes away and the definitions come
// from libretro.h. The guard below makes that switch-over safe: a libretro.h
// that already has the call wins. Copies: RetroXR's Cemu fork, src/libretro/NfcInterface.h (a core using it).
//
// One change from the PR: 113 here, provisional. The PR's 99 is upstream's
// RETRO_ENVIRONMENT_GET_VIDEO_DECODE_INTERFACE.

#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <libretro.h>

#ifndef RETRO_ENVIRONMENT_GET_NFC_INTERFACE

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Gets an interface to an NFC tag reader (e.g. for amiibo / NTAG21x and
 * Mifare Classic).
 *
 * The frontend exposes either a physical reader or a virtual reader
 * served from tag dumps. Tag memory is passed raw and unmodified; any
 * tag-format crypto (e.g. amiibo encryption) is the core's concern.
 *
 * Before the call the core sets \c retro_nfc_interface::version to the
 * \c RETRO_NFC_INTERFACE_VERSION it was built against; the frontend
 * lowers it to the version it provides and fills only the function
 * pointers up to that version. Structs the frontend writes into
 * (\c retro_nfc_tag_info) are likewise only filled up to the fields
 * that exist at the negotiated version.
 *
 * @param[in,out] data <tt>struct retro_nfc_interface *</tt>.
 * @return \c true if the frontend provides an NFC interface.
 * @see retro_nfc_interface
 */
#define RETRO_ENVIRONMENT_GET_NFC_INTERFACE (113 | RETRO_ENVIRONMENT_EXPERIMENTAL)

/** Version of the NFC interface described by this header. */
#define RETRO_NFC_INTERFACE_VERSION 5

/** Physical tag technology reported by \c retro_nfc_get_tag_info_t. */
enum retro_nfc_tag_type
{
   RETRO_NFC_TAG_TYPE_UNKNOWN = 0,
   RETRO_NFC_TAG_TYPE_NTAG213,
   RETRO_NFC_TAG_TYPE_NTAG215,          /* amiibo */
   RETRO_NFC_TAG_TYPE_NTAG216,
   RETRO_NFC_TAG_TYPE_MIFARE_CLASSIC_1K,
   RETRO_NFC_TAG_TYPE_MIFARE_CLASSIC_4K,
   RETRO_NFC_TAG_TYPE_ISO14443A,        /* ISO14443-3A, unspecified product */
   RETRO_NFC_TAG_TYPE_DUMMY = INT_MAX
};

/** Reader/scan state reported by \c retro_nfc_get_status_t. */
enum retro_nfc_status
{
   RETRO_NFC_STATUS_UNSUPPORTED = 0,    /* no reader / backend unavailable */
   RETRO_NFC_STATUS_IDLE,               /* reader present, not scanning    */
   RETRO_NFC_STATUS_SCANNING,           /* polling, no tag in field        */
   RETRO_NFC_STATUS_TAG_PRESENT,        /* a tag is in the field           */
   RETRO_NFC_STATUS_DUMMY = INT_MAX
};

/* --- interface version 5 --- */

/* Result codes for the I/O ops. read and write return a byte count >= 0
 * on success; the Mifare block ops return RETRO_NFC_OK (0); transceive
 * returns the number of bytes received. All return a negative
 * retro_nfc_result on failure so a core can tell "wrong key" from "tag
 * gone" from "unsupported" and react. (RETRO_NFC_ERR_UNSUPPORTED is -1, so
 * a v1-v4 core that only checks for -1 still sees failures.) A write MUST
 * succeed only if the data actually reached the tag. */
enum retro_nfc_result
{
   RETRO_NFC_OK              =  0,
   RETRO_NFC_ERR_UNSUPPORTED = -1,   /* op/cap not provided by this backend  */
   RETRO_NFC_ERR_NO_TAG      = -2,   /* no tag present / tag left the field   */
   RETRO_NFC_ERR_AUTH        = -3,   /* Mifare auth failed (wrong key or slot)*/
   RETRO_NFC_ERR_IO          = -4,   /* transceive error / tag NAK           */
   RETRO_NFC_ERR_PERM        = -5,   /* refused by the tag (locked block etc.)*/
   RETRO_NFC_ERR_PARAM       = -6,   /* bad arguments (range, length, null)   */
   RETRO_NFC_RESULT_DUMMY    = INT_MAX
};

/* Capability flags, reported per tag in retro_nfc_tag_info.caps: the
 * intersection of what the backend can do and what this tag supports, so a
 * core can check "can I do X with this tag on this backend?" and degrade
 * gracefully instead of calling an op that will just fail. */
#define RETRO_NFC_CAP_HARDWARE        (1u << 0) /* real reader, not a file    */
#define RETRO_NFC_CAP_NTAG            (1u << 1) /* linear read()/write() ok   */
#define RETRO_NFC_CAP_MIFARE_CLASSIC  (1u << 2) /* mifare_*_block() ok        */
#define RETRO_NFC_CAP_RAW_TRANSCEIVE  (1u << 3) /* transceive() ok            */

/* Mifare Classic key slot supplied by the core per block op. */
enum retro_nfc_mifare_key_type
{
   RETRO_NFC_MIFARE_KEY_A = 0,
   RETRO_NFC_MIFARE_KEY_B,
   RETRO_NFC_MIFARE_KEY_DUMMY = INT_MAX
};

#define RETRO_NFC_MIFARE_KEY_LEN   6
#define RETRO_NFC_MIFARE_BLOCK_LEN 16

/** Identifying info for the tag currently in the field. */
struct retro_nfc_tag_info
{
   uint8_t  uid[10];                    /* NFCID1 (4 or 7 bytes typical)        */
   unsigned uid_len;
   enum retro_nfc_tag_type type;
   unsigned mem_size;                   /* total readable bytes (540 = NTAG215) */

   /* --- interface version 5 --- */
   /* RETRO_NFC_CAP_* bitmask of what this tag supports on this backend.
    * Only written when the negotiated version is >= 5, so a v1-v4 core's
    * smaller struct is never overrun; a v5 core should zero the struct
    * before the call. */
   uint32_t caps;
};

/**
 * Begin polling for a tag. Non-blocking: poll \c get_status to learn when
 * a tag enters or leaves the field.
 * @return \c true if scanning started (or was already active).
 */
typedef bool (RETRO_CALLCONV *retro_nfc_start_scan_t)(void);

/** Stop polling and release any held tag. */
typedef void (RETRO_CALLCONV *retro_nfc_stop_scan_t)(void);

/** @return the current reader/scan state. */
typedef enum retro_nfc_status (RETRO_CALLCONV *retro_nfc_get_status_t)(void);

/**
 * Fill \c info for the tag currently in the field.
 * @return \c true if a tag is present and \c info was populated.
 */
typedef bool (RETRO_CALLCONV *retro_nfc_get_tag_info_t)(
      struct retro_nfc_tag_info *info);

/**
 * Read raw tag memory. \c offset and \c len are byte quantities; the
 * frontend maps them onto the tag's native page/block reads.
 * @return number of bytes read, or a negative \c retro_nfc_result
 * (-1 before v5) on error: no tag, out of range, or I/O failure.
 */
typedef int (RETRO_CALLCONV *retro_nfc_read_t)(
      unsigned offset, uint8_t *buf, unsigned len);

/**
 * Write raw tag memory (e.g. amiibo save-back).
 * Same byte semantics as \c retro_nfc_read_t.
 * @return number of bytes written, or a negative \c retro_nfc_result
 * (-1 before v5) on error.
 */
typedef int (RETRO_CALLCONV *retro_nfc_write_t)(
      unsigned offset, const uint8_t *buf, unsigned len);

/** Which reader the frontend should serve tags from (interface v2+). */
enum retro_nfc_source
{
   RETRO_NFC_SOURCE_AUTO = 0,   /* frontend default (hardware if present)     */
   RETRO_NFC_SOURCE_HARDWARE,   /* a physical reader                          */
   RETRO_NFC_SOURCE_SOFTWARE,   /* a virtual reader served from tag dumps     */
   RETRO_NFC_SOURCE_DUMMY = INT_MAX
};

/**
 * Ask the frontend to serve tags from a particular source (v2+).
 * @return \c true if the source was selected (or is already active).
 * A frontend that does not offer the requested source may fall back to
 * another and still return \c true.
 */
typedef bool (RETRO_CALLCONV *retro_nfc_set_source_t)(enum retro_nfc_source source);

/** One selectable NFC source the frontend can offer (interface v3+). */
struct retro_nfc_source_info
{
   char name[64];       /* display name: a physical reader, or a stored tag
                           named from the amiibo database when known, else
                           its file name */
   bool is_hardware;    /* true for a physical reader, false for a dump */
};

/**
 * Number of selectable NFC sources available right now (v3+): a physical
 * reader if one is present, plus each stored tag the frontend can serve.
 */
typedef unsigned (RETRO_CALLCONV *retro_nfc_get_source_count_t)(void);

/**
 * Fill \c info for source \c index (0 <= index < get_source_count()).
 * @return \c true if the index was valid and \c info was populated.
 */
typedef bool (RETRO_CALLCONV *retro_nfc_get_source_info_t)(unsigned index,
      struct retro_nfc_source_info *info);

/**
 * Make source \c index the active reader/tag. Enumeration order and
 * indices are only guaranteed stable between a get_source_count() call and
 * the next; re-query before selecting.
 * @return \c true if the source was selected.
 */
typedef bool (RETRO_CALLCONV *retro_nfc_select_source_t)(unsigned index);

/**
 * Whether a hardware reader is available right now (interface v4+).
 * A reader can come and go at runtime (e.g. with a controller), so this is
 * a live query: a core can use it to offer a hardware/software choice only
 * while hardware is connected.
 */
typedef bool (RETRO_CALLCONV *retro_nfc_has_hardware_t)(void);

/* --- interface version 5 --- */

/* Mifare Classic block I/O, addressed by (sector, block-within-sector): the
 * key authenticates the SECTOR, the block selects the 16-byte line within it.
 * The core supplies the key and A/B slot with every call; auth is effectively
 * stateless to the core (a failed op drops hardware auth state and the next
 * call re-authenticates). Sector-trailer and block-0 writes are permitted --
 * some tags legitimately need them; the core owns that risk. Returns
 * RETRO_NFC_OK or a negative retro_nfc_result. key: 6 bytes; out/data: 16. */
typedef int (RETRO_CALLCONV *retro_nfc_mifare_read_block_t)(
      unsigned sector, unsigned block,
      enum retro_nfc_mifare_key_type key_type,
      const uint8_t *key, uint8_t *out);
typedef int (RETRO_CALLCONV *retro_nfc_mifare_write_block_t)(
      unsigned sector, unsigned block,
      enum retro_nfc_mifare_key_type key_type,
      const uint8_t *key, const uint8_t *data);

/* Raw ISO14443-3 transceive escape hatch (RETRO_NFC_CAP_RAW_TRANSCEIVE), for
 * cores driving tags/commands the typed primitives do not cover. Sends tx_len
 * bytes, writes up to rx_cap bytes into rx; returns bytes received (>= 0) or a
 * negative retro_nfc_result. The backend frames/CRCs at ISO14443-3; the core
 * supplies the command payload. Optional -- a frontend may leave it NULL. */
typedef int (RETRO_CALLCONV *retro_nfc_transceive_t)(
      const uint8_t *tx, unsigned tx_len,
      uint8_t *rx, unsigned rx_cap);

/**
 * NFC reader interface exposed by the frontend.
 * @see RETRO_ENVIRONMENT_GET_NFC_INTERFACE
 */
struct retro_nfc_interface
{
   /* Negotiated interface version. The core sets this to the
    * RETRO_NFC_INTERFACE_VERSION it built against before issuing
    * GET_NFC_INTERFACE; the frontend lowers it to the version it provides
    * and only populates function pointers up to that version. A core must
    * not call a pointer introduced after the value it reads back here. */
   unsigned version;

   retro_nfc_start_scan_t   start_scan;
   retro_nfc_stop_scan_t    stop_scan;
   retro_nfc_get_status_t   get_status;
   retro_nfc_get_tag_info_t get_tag_info;
   retro_nfc_read_t         read;
   retro_nfc_write_t        write;

   /* --- interface version 2 --- */
   retro_nfc_set_source_t   set_source;

   /* --- interface version 3 --- */
   retro_nfc_get_source_count_t get_source_count;
   retro_nfc_get_source_info_t  get_source_info;
   retro_nfc_select_source_t    select_source;

   /* --- interface version 4 --- */
   retro_nfc_has_hardware_t     has_hardware;

   /* --- interface version 5 --- */
   /* Present only when version >= 5. Guard each call on version >= 5 AND the
    * matching RETRO_NFC_CAP_* flag in retro_nfc_tag_info.caps. */
   retro_nfc_mifare_read_block_t  mifare_read_block;
   retro_nfc_mifare_write_block_t mifare_write_block;
   retro_nfc_transceive_t         transceive;
};

#ifdef __cplusplus
}
#endif

#endif /* RETRO_ENVIRONMENT_GET_NFC_INTERFACE */
