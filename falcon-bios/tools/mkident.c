/* falcon-bios-mkident: writes an identity record (docs/DESIGN.md Section
 * 3.1) into the sector at a byte offset of an image file.
 *
 * For a volume with no partition table and fields of its own: jump's
 * floppy, whose boot sector carries a boot record naming stage 3 at
 * FD_STAGE3_SECTOR and no marker. A partition of a table, with the fields
 * `mkstick create` writes, is `falcon-boot-mkstick ident --write-boot`'s
 * job; this is the one the floppy's recipe can run with every field its
 * own. The record goes at IDENT_BIN_OFF of the sector, written by
 * falcon-core's encoder and read back through its decoder before the tool
 * says it is done; the sector's other bytes are left as they are.
 *
 * Usage: falcon-bios-mkident IMAGE BYTE_OFFSET ROLE INSTALL_ID STAGE3 MARKER
 *
 * ROLE is boot, library or shelf; INSTALL_ID 32 lowercase hex digits;
 * STAGE3 and MARKER the sectors a boot record names, relative to the
 * volume, and 0 for any other role. Exit 0 on success, 1 on a failure, 2
 * on bad usage.
 */

#include "mkidentargs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s IMAGE BYTE_OFFSET ROLE INSTALL_ID STAGE3 MARKER\n",
            argv0);
    return 2;
}

int main(int argc, char **argv)
{
    uint8_t sec[512];
    const char *why = NULL;
    uint64_t off = 0;
    ident_rec r;
    ident_rec back;
    int fd;
    unsigned i;

    if (argc != 7)
        return usage(argv[0]);
    if (mkident_offset(argv[2], &off) != 0) {
        fprintf(stderr, "ERROR: %s is not a byte offset on a 512-byte "
                "sector boundary\n", argv[2]);
        return 2;
    }
    if (mkident_record(argv[3], argv[4], argv[5], argv[6], &r, &why) != 0) {
        fprintf(stderr, "ERROR: %s\n", why);
        return 2;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ERROR: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    if (pread(fd, sec, sizeof sec, (off_t)off) != (ssize_t)sizeof sec) {
        fprintf(stderr, "ERROR: %s: no whole sector at byte %llu\n", argv[1],
                (unsigned long long)off);
        close(fd);
        return 1;
    }
    ident_encode_bin(&r, sec);
    if (ident_decode_bin(sec, &back) != IDENT_OK ||
        pwrite(fd, sec, sizeof sec, (off_t)off) != (ssize_t)sizeof sec ||
        fsync(fd) != 0) {
        fprintf(stderr, "ERROR: %s: the record was not written\n", argv[1]);
        close(fd);
        return 1;
    }
    close(fd);

    printf("%s: %s record at byte %llu (+0x%X), install ", argv[1],
           ident_role_name(back.role), (unsigned long long)off,
           IDENT_BIN_OFF);
    for (i = 0; i < IDENT_ID_LEN; i++)
        printf("%02x", back.install_id[i]);
    printf(", stage 3 +%u, marker +%u\n", (unsigned)back.stage3_sector,
           (unsigned)back.marker_sector);
    return 0;
}
