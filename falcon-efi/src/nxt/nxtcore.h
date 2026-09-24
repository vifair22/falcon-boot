/* falcon-boot: the firmware's own boot entries, as the stationary build's NXT
 * rows (docs/DESIGN.md Section 13.4).
 *
 * The EFI-free half: a Boot#### variable's bytes in, a row or a refusal out,
 * and BootOrder's bytes in, a list of option numbers out. Every byte here is
 * written by whoever last wrote NVRAM, which includes every OS installer that
 * has ever run on the machine, so each length is checked against the buffer
 * before it is believed. The firmware half (nxt.c) reads the variables and
 * sets BootNext; nothing here touches the platform.
 */

#ifndef FALCON_NXTCORE_H
#define FALCON_NXTCORE_H

#include <stddef.h>
#include <stdint.h>

enum {
    NXT_OK        = 0,
    NXT_ERR_SHORT = -1,   /* the variable ends before its header does */
    NXT_ERR_DESC  = -2,   /* no NUL-terminated description inside it */
    NXT_ERR_PATH  = -3    /* FilePathListLength runs past its end */
};

/* EFI_LOAD_OPTION attributes (UEFI 2.10 Section 3.1.3). */
#define NXT_LOAD_OPTION_ACTIVE          0x00000001u
#define NXT_LOAD_OPTION_HIDDEN          0x00000008u
#define NXT_LOAD_OPTION_CATEGORY        0x00001F00u
#define NXT_LOAD_OPTION_CATEGORY_BOOT   0x00000000u

/* A row's label, in UTF-16 code units, terminator included. Firmware
 * descriptions are short ("Windows Boot Manager", "UEFI OS"); a longer one
 * is cut rather than refused, since it still names the entry. */
#define NXT_LABEL_MAX 96u

/* Parses one Boot#### variable. On NXT_OK, *attrs holds its attributes and
 * label its description, cut to NXT_LABEL_MAX - 1 units and terminated; an
 * empty description leaves label empty for the caller to name. */
int nxt_option_parse(const uint8_t *buf, size_t len, uint32_t *attrs,
                     uint16_t label[NXT_LABEL_MAX]);

/* Is an entry with these attributes one the firmware's own boot menu would
 * offer: active, not hidden, and in the boot category (an application
 * category entry, such as a firmware's setup UI, is not an OS). */
int nxt_option_offered(uint32_t attrs);

/* BootOrder's option numbers into out, in order, with any repeat dropped
 * (a repeat is a firmware bug and would be a duplicate row). A trailing odd
 * byte is ignored. Returns how many were written, at most cap. */
size_t nxt_order_parse(const uint8_t *buf, size_t len, uint16_t *out,
                       size_t cap);

#endif /* FALCON_NXTCORE_H */
