/* falcon-boot mkstick: the pure half of `next-boot` (docs/DESIGN.md Section
 * 6.3.1), apart from the code that writes a variable or a sector so a test
 * can reach it.
 *
 * The marker's format and value rules are falcon-core's
 * (media/nextboot.h), shared with both loaders; what is here is only what
 * the Linux side adds: the efivarfs file's bytes, the RTC reading's text,
 * and whether a sector may be written.
 */

#ifndef FALCON_MKSTICK_NEXTCORE_H
#define FALCON_MKSTICK_NEXTCORE_H

#include <stddef.h>
#include <stdint.h>

/* The variable the UEFI loader reads and deletes, under falcon's vendor GUID
 * (falcon-efi/src/loader/loadops.c, FALCON_VENDOR_GUID), as efivarfs names
 * it. */
#define NEXTCORE_EFIVAR "FalconNextBoot-50614485-081b-4b7c-8b54-2abeffdf2fb3"
#define NEXTCORE_EFIVARS_DIR "/sys/firmware/efi/efivars"

/* The attributes loadops.c sets it with: NON_VOLATILE, BOOTSERVICE_ACCESS,
 * RUNTIME_ACCESS. */
#define NEXTCORE_EFIVAR_ATTRS 0x00000007u

/* The RTC reading nextboot_rtc_seconds matches: Linux's own count of the
 * RTC's digits read as UTC. */
#define NEXTCORE_RTC_EPOCH "/sys/class/rtc/rtc0/since_epoch"

/* efivarfs's file bytes for a value: the attributes, little endian, then
 * the value and one NUL, as the loader writes it. The length, or 0 when the
 * value is not one nextboot_decode accepts or does not fit. */
size_t nextcore_efivar(const char *value, uint8_t *buf, size_t cap);

/* since_epoch's text, decimal digits and an optional newline. 0 and *out
 * set, or -1. */
int nextcore_parse_epoch(const char *text, size_t len, uint64_t *out);

/* Nonzero when the sector may be written by next-boot: all zeros (what
 * `mkstick bios` left), or already a marker. Anything else is not falcon's
 * marker sector and is left alone. len is the disk's whole sector (512, or
 * 4096 on a 4Kn disk), every byte of which is judged. */
int nextcore_sector_ours(const uint8_t *sec, size_t len);

#endif /* FALCON_MKSTICK_NEXTCORE_H */
