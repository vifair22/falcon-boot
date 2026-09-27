/* falcon-boot: the next-boot marker, as both loaders read it (DESIGN Section
 * 6.3.1). Shared by both loaders.
 *
 * The marker says "on the next boot, start this entry instead of drawing the
 * menu". Its VALUE is the same on both platforms: the path of an image on p2,
 * or a reserved name such as the personal OS. Where it is KEPT differs,
 * because a BIOS has no variable store:
 *
 *   - UEFI: the FalconNextBoot variable, read and deleted by
 *     falcon-efi/src/loader/loadops.c. The variable's bytes are the value.
 *   - BIOS: one sector of the `boot` partition, at the sector its identity
 *     record names (DESIGN Section 3.3), read and zeroed by
 *     falcon-bios/src/disk/marker.c. The sector carries the value
 *     inside a small text record, below.
 *
 * This module is the part with no firmware in it: what a value may be, how
 * it is matched against what the survey found, what the BIOS sector looks
 * like, and the arithmetic that dates it. The UEFI loader had the value
 * rules to itself (falcon-efi/src/bless/blesscore.c) until falcon-bios needed
 * the same ones; they moved rather than being written twice, so the two
 * loaders cannot disagree about what a marker names.
 *
 * EVERYTHING HERE IS UNTRUSTED INPUT. The variable is runtime-writable and the
 * sector is writable by anything that can open the disk, including a machine
 * that has never seen this stick. A value is decoded, never believed, and it
 * is only ever compared against paths the survey found; nothing indexes
 * anything with it.
 */

#ifndef FALCON_NEXTBOOT_H
#define FALCON_NEXTBOOT_H

#include <stddef.h>
#include <stdint.h>

#include <manifest/manifest.h>

typedef enum {
    NEXTBOOT_OK          =  0,
    NEXTBOOT_ERR_BAD_ARG = -1,
    NEXTBOOT_ERR_VALUE   = -2   /* the marker is not a usable value */
} nextboot_status;

/* Longest value worth looking at: the path of an image on p2, so the
 * manifest's own path cap is the bound, terminator included. */
#define NEXTBOOT_MAX MANIFEST_PATH_MAX

/* ---- the value ---- */

/* Decodes raw marker bytes into a value, or refuses them.
 *
 * The bytes must be printable ASCII, non-empty, and short enough to be a path
 * a manifest record could carry, with at most one trailing NUL (the UEFI
 * loader and archusb write one; a tool that did not is not wrong). Anything
 * else is NEXTBOOT_ERR_VALUE and the marker is discarded. The result is still
 * only a NAME: whether it may be acted on is nextboot_names against the
 * survey's paths, or one of the reserved values below.
 *
 * out is NUL-terminated on success and left empty otherwise. */
int nextboot_decode(const void *buf, size_t len, char *out, size_t cap);

/* Does this survey entry's path name the file the marker names?
 *
 * path is the entry's path relative to p2's root, as the survey built it.
 * Nonzero when they are the same file by manifest_path_eq's rule
 * (case-insensitive, either separator). This is the only way a value is ever
 * turned into an entry: matched against paths the survey found, so a hostile
 * value can name a file that is not there and nothing else. */
int nextboot_names(const uint16_t *path, const char *name);

/* ---- reserved values ----
 *
 * One target is not a file at all: the personal OS is a pinned menu row, not
 * something the survey found, so "boot straight back into the personal OS"
 * needs a value that names it without naming a path.
 *
 * The leading ':' is what makes that unambiguous, and it is chosen rather than
 * decorative: exFAT forbids ':' in a filename, so no file on p2 can ever
 * produce a path containing one, and a reserved value can never shadow a real
 * entry. A '@' prefix would have read more nicely and been wrong - a file
 * called "@personal" is perfectly legal and would have been silently
 * unbootable.
 *
 * Naming the personal OS grants nothing the menu does not: the row prompts for
 * the passphrase exactly as it would if the user had chosen it by hand. */
#define NEXTBOOT_PERSONAL ":personal"

/* Nonzero when the value names the personal-OS row rather than a path.
 * Callers test this BEFORE matching against paths; a reserved value never
 * matches one, but testing after would report a stale marker as "not on the
 * stick any more", which is a confusing way to say "unrecognised". */
int nextboot_is_personal(const char *name);

/* Nonzero when the value is in the reserved namespace at all, i.e. begins
 * with ':'. A reserved value this build does not recognise comes from a newer
 * writer or a typo, and either way it is refused rather than looked up. */
int nextboot_is_reserved(const char *name);

/* ---- the BIOS sector ----
 *
 * One 512-byte sector, text so that a shell script can write it with printf
 * and a person can read it with xxd:
 *
 *     falcon-next-boot 1\n
 *     <the RTC's reading when it was set, decimal seconds>\n
 *     <the value>\n
 *     <zeros to the end of the sector>
 *
 * The first line is the magic and the format version. A sector that does not
 * begin with "falcon-next-boot " is not a marker, and the loader leaves it
 * exactly as it found it: that prefix is the one thing that entitles the
 * loader to write the sector at all. A sector that does begin with it is
 * the loader's to clear, whatever else is wrong with it.
 *
 * No checksum, deliberately. Its job would be catching a sector that is
 * almost a marker, and the parse already refuses every byte out of place: the
 * exact version, digits only in the stamp, the value decoded as above, and
 * every byte after the value's newline zero. A torn write fails that. What a
 * checksum would add is a helper program on the writing side, where today
 * printf and dd are enough.
 *
 * The stamp is the RTC's own reading, not the time. BIOS machines keep the RTC
 * in local time or UTC as the installed OS prefers, and nothing on either
 * side can tell which; Linux's /sys/class/rtc/rtc0/since_epoch is the RTC's
 * digits read as though they were UTC, and nextboot_rtc_seconds reads the
 * same digits the same way. Both ends therefore count on one clock and the
 * difference is the time between setting and booting, whatever zone the
 * digits are in. */
#define NEXTBOOT_SECTOR_BYTES 512u
#define NEXTBOOT_MAGIC        "falcon-next-boot "
#define NEXTBOOT_VERSION      "1"

/* How long a sector marker stays good. A reboot that follows the boot order
 * rather than coming back to the stick (a BIOS has no BootNext) leaves the
 * marker waiting until the stick is next booted, which could be days later
 * and would then start something nobody asked for. Long enough for a shutdown
 * and an old machine's POST, short enough that "later" is not "now". */
#define NEXTBOOT_WINDOW_SECONDS 600u

/* How far the clock may appear to have gone BACK between setting and booting
 * and still be the same clock. Something may correct the RTC in between (an
 * NTP-synced kernel writing it, hwclock at shutdown), and an RTC a few seconds
 * fast then reads earlier at boot than it did when the marker was set. Found
 * in the rig, where the host's RTC ran four seconds ahead of the time QEMU's
 * RTC follows. */
#define NEXTBOOT_SKEW_SECONDS 120u

typedef enum {
    NEXTBOOT_SECTOR_NONE = 0,  /* not a marker: leave the sector alone */
    NEXTBOOT_SECTOR_VALUE,     /* a marker; stamp and value filled in */
    NEXTBOOT_SECTOR_BAD        /* a marker by its magic, and nothing usable */
} nextboot_sector;

/* Nonzero when the sector begins with the magic, which is what decides
 * whether the loader may write it. */
int nextboot_sector_is_marker(const uint8_t *sec, size_t len);

/* Parses a sector. VALUE fills *stamp and out (NUL-terminated); NONE and BAD
 * leave out empty. len must be NEXTBOOT_SECTOR_BYTES. */
nextboot_sector nextboot_sector_parse(const uint8_t *sec, size_t len,
                                      uint64_t *stamp, char *out, size_t cap);

/* Writes a marker into a caller's sector buffer, zeros included: the format's
 * other half, for tests and for anything in C that sets one. NEXTBOOT_OK, or
 * NEXTBOOT_ERR_VALUE when the value would not decode. */
int nextboot_sector_write(uint8_t *sec, size_t len, uint64_t stamp,
                          const char *value);

/* ---- dating it ---- */

typedef enum {
    NEXTBOOT_AGE_FRESH = 0,  /* set within the window */
    NEXTBOOT_AGE_STALE,      /* set longer ago than the window */
    NEXTBOOT_AGE_FUTURE      /* set after now: the clock moved backwards */
} nextboot_age;

/* How old a stamp is against now, on the same clock. Equal counts as fresh:
 * both sides read whole seconds. A stamp up to `skew` seconds after now is
 * fresh too (NEXTBOOT_SKEW_SECONDS says why); one further ahead is FUTURE and
 * refused, because a clock that went back that far says nothing about when
 * the marker was set. */
nextboot_age nextboot_age_of(uint64_t stamp, uint64_t now, uint64_t window,
                             uint64_t skew);

/* The RTC's BCD digits as INT 1Ah reports them, century first
 * (bcd[0] century, [1] year, [2] month, [3] day, [4] hour, [5] minute,
 * [6] second), as seconds since 1970-01-01 read as UTC: the number Linux's
 * since_epoch gives for the same digits. NEXTBOOT_OK, or NEXTBOOT_ERR_VALUE
 * for a digit that is not BCD, a field out of range, a 12-hour-mode hour, or
 * a date before 1970. */
int nextboot_rtc_seconds(const uint8_t bcd[7], uint64_t *out);

#endif /* FALCON_NEXTBOOT_H */
