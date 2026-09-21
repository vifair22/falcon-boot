/* falcon-boot: the synthetic credential filesystem (docs/DESIGN.md Section
 * 5.1).
 *
 * Purpose. Publish a small, read-only, in-RAM filesystem on a handle of its
 * own, so that the personal-OS UKI can be loaded BY DEVICE PATH rather than
 * from a memory buffer. That is the whole point of the module: a device-path
 * load gives the started image a LoadedImage->DeviceHandle, and sd-stub finds
 * its companion credentials by scanning the directory next to the image on
 * that device. A buffer load leaves DeviceHandle NULL and the credential is
 * never seen.
 *
 * What travels on it. The UKI bytes read off p3, and a `<uki>.extra.d/`
 * directory holding the passphrase as a `.cred` file. sd-stub packs that file
 * into a cpio it merges into the initrd at /.extra/credentials/, systemd's
 * PID 1 imports it (dropping the `.cred` suffix), and systemd-cryptsetup
 * consumes it as the `cryptsetup.passphrase` credential to unlock p4. See
 * falcon-efi/src/pos/pos.c for the naming that chain requires.
 *
 * Security posture. The credential is DATA handed to an image the platform
 * has already verified, never code and never a verification decision. This
 * module installs no security protocol, overrides nothing, and its existence
 * changes nothing about what will load: the image on it is loaded through the
 * same shim-verified path as everything else (DESIGN Section 7.6), and is
 * refused exactly when it would have been refused from anywhere else. All it
 * changes is where the bytes come from.
 *
 * Lifetime. RAM only; nothing here is ever written to storage. Entries marked
 * secret are wiped by credfs_teardown when the image comes back (DESIGN
 * Section 5.1: "if LoadImage/StartImage fails, the loader zeroes the
 * credential buffer and tears down the virtual filesystem"), and at
 * ExitBootServices when it does not: a publish that carries a secret also
 * registers a notification there, because a boot that succeeds never
 * returns to teardown, and the credential's last reader (sd-stub) is done
 * with it before the kernel exits boot services. Teardown is also what
 * releases the handle, and it refuses to let a second publish reuse an
 * instance a firmware caller might still hold.
 */

#ifndef FALCON_CREDFS_H
#define FALCON_CREDFS_H

#include <efi/uefi.h>
#include <efi/devpath.h>

/* The tree is three entries today (image, drop-in directory, credential).
 * The cap is what bounds the static state; raise it with the tree, not
 * ahead of it. */
#define CREDFS_MAX_ENTRIES 8u

/* Marks a top-level entry, and names the root directory itself. */
#define CREDFS_ROOT ((UINTN)-1)

/* One node of the caller-provided tree.
 *
 * The data is NOT copied: it stays the caller's buffer, which is what keeps a
 * tens-of-megabytes UKI from being duplicated in firmware memory. The caller
 * therefore has to keep those buffers alive until credfs_teardown returns.
 *
 * data is mutable rather than const precisely because of `secret`: the wipe
 * has to be able to write through it, and a module that promises to wipe a
 * buffer should not have to cast away a promise not to touch it. */
typedef struct {
    /* One path component, NUL-terminated. Matched case-insensitively (FAT
     * semantics), because a caller that opens \Foo.EFI must find \foo.efi. */
    const CHAR16 *name;
    void         *data;    /* NULL for a directory */
    UINT64        size;    /* 0 for a directory */
    /* Index of the parent entry in the same array, which must be an earlier
     * directory, or CREDFS_ROOT for a top-level entry. */
    UINTN         parent;
    UINT8         is_dir;
    UINT8         secret;  /* teardown wipes data[0..size) */
} credfs_entry;

/* Publishes the tree on a fresh handle carrying Simple File System and a
 * device path. Nothing is connected and no driver binds: the filesystem is
 * served by this module directly, which is the whole reason it can exist
 * without a Block I/O device under it.
 *
 * EFI_ACCESS_DENIED means a previous instance is still published; that is the
 * in-use guard, not a transient condition. */
EFI_STATUS credfs_publish(EFI_SYSTEM_TABLE *st, const credfs_entry *entries,
                          UINTN count);

/* Builds the device path naming one file on the published filesystem, for
 * LoadImage. `path` is a File Protocol path (backslash-separated, absolute:
 * L"\\personal.efi"). The caller frees the result with FreePool. */
EFI_STATUS credfs_load_path(EFI_BOOT_SERVICES *bs, const CHAR16 *path,
                            EFI_DEVICE_PATH_PROTOCOL **out);

/* Uninstalls the protocols, closes every handle this module handed out, and
 * wipes every entry marked secret. Safe to call when nothing is published.
 *
 * On failure the instance stays marked in use and is never reused, mirroring
 * the ISO path's discipline: firmware that still points at our interfaces
 * must not find a second filesystem's state behind the same pointers. */
EFI_STATUS credfs_teardown(EFI_BOOT_SERVICES *bs);

#endif /* FALCON_CREDFS_H */
