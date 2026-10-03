/* falcon-boot appliance: the MOK keypair off p3, read in process (DESIGN
 * Section 7.5).
 *
 * p3 is opened with falcon-core's LUKS2 (the same unlock the loader runs),
 * read through its decrypting reader, and its ext4 is walked with core's
 * driver. Nothing is mapped or mounted: no dm-crypt device exists for
 * anything else on the machine to open, and cryptsetup, the device-mapper
 * modules and their libraries leave the image.
 *
 * The keypair reaches the sign tool as two memfds this process creates and
 * its children inherit, named to them as /proc/self/fd/N. A memfd is RAM
 * that no path on any filesystem reaches, it is sealed against writes before
 * anything else sees it, and it is gone when the last descriptor closes;
 * the key is never on a disk and never in a file name anyone could open
 * after this program ends. The one plaintext copy in this process's own
 * memory (the read buffer) is wiped as soon as it is in the memfd.
 *
 * Read-only by construction: the device is opened O_RDONLY and nothing here
 * has a write path to it.
 */

#include "appliance.h"

#include <luks/luks.h>
#include <fs/fs.h>
#include <crypto/crypto.h>
#include <common/jmem.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Files mkstick writes at the root of the LUKS volume (falcon-cli/src/
 * mkstick/mkstick.h, MKS_P3_KEY_NAME / MKS_P3_CERT_NAME). */
#define SHELF_KEY_NAME  "/falcon-mok.key"
#define SHELF_CERT_NAME "/falcon-mok.crt"

/* A PEM key or certificate is a few kilobytes; anything past this is not
 * one. */
#define SHELF_FILE_MAX ((uint64_t)64u * 1024u)

static int g_dev_fd = -1;
static char g_json[LUKS_JSON_CAP];
static luks_vol g_vol;
static luks_dev g_luks;
static int g_luks_open;
static fs_vol g_fs;
static int g_fs_mounted;
static fs_dir g_dir;
static fs_file g_file;
static uint8_t g_buf[SHELF_FILE_MAX];

static int dev_read(void *ctx, uint64_t off, uint64_t len, void *buf)
{
    uint64_t done = 0;

    (void)ctx;
    while (done < len) {
        ssize_t n = pread(g_dev_fd, (uint8_t *)buf + done,
                          (size_t)(len - done), (off_t)(off + done));

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (uint64_t)n;
    }
    return 0;
}

int shelf_open(const char *p3)
{
    shelf_close();
    g_dev_fd = open(p3, O_RDONLY | O_CLOEXEC);
    if (g_dev_fd < 0)
        return SHELF_ERR;
    if (luks_open(&g_vol, dev_read, (void *)0, g_json, sizeof g_json) !=
        LUKS_OK) {
        shelf_close();
        return SHELF_NOT_LUKS;
    }
    return SHELF_OK;
}

/* An argon2id keyslot's memory. The appliance runs on the machine it
 * blesses from, which has what the slot asks for or refuses it here. */
static void *kdf_get(void *ctx, uint32_t kib)
{
    (void)ctx;
    return malloc((size_t)kib * 1024u);
}

static void kdf_put(void *ctx, void *mem, uint32_t kib)
{
    (void)ctx;
    (void)kib;
    free(mem);
}

static const luks_kdf_mem g_kdf_mem = {
    kdf_get, kdf_put, (void *)0, (void *)0
};

int shelf_unlock(const void *pass, size_t len)
{
    luks_master_key mk;
    uint32_t slot = 0;
    const fs_ops *ops;
    fs_jinfo jinfo;
    int rc;

    if (g_dev_fd < 0)
        return SHELF_ERR;
    rc = luks_unlock(&g_vol, pass, len, &g_kdf_mem, &mk, &slot);
    if (rc == LUKS_ERR_NOKEY) {
        crypto_wipe(&mk, sizeof mk);
        return SHELF_WRONG;
    }
    if (rc != LUKS_OK) {
        crypto_wipe(&mk, sizeof mk);
        return SHELF_ERR;
    }
    rc = luks_dev_open(&g_luks, &g_vol, &mk);
    /* luks_dev_open has the key in its cipher context now. */
    crypto_wipe(&mk, sizeof mk);
    if (rc != LUKS_OK)
        return SHELF_ERR;
    g_luks_open = 1;

    ops = fs_identify(luks_read, &g_luks, FS_HINT_SHELF);
    if (ops == (const fs_ops *)0 ||
        fs_mount_journal(&g_fs, ops, luks_read, &g_luks, &cli_jmem,
                         &jinfo) != FS_OK)
        return SHELF_ERR;
    g_fs_mounted = 1;
    return SHELF_OK;
}

/* One file off the shelf into a sealed memfd. The descriptor is left
 * inheritable on purpose: the sign tool and the sbsign it runs are the
 * readers. -1 when the file is absent or unreadable. */
static int to_memfd(const char *path, const char *name)
{
    fs_dirent ent;
    uint64_t size = 0;
    int fd;

    /* The size comes from the open file: an ext4 directory entry does not
     * carry one. */
    if (fs_lookup(&g_fs, path, &g_dir, &ent) == FS_OK && !ent.is_dir &&
        fs_file_open(&g_fs, &g_file, &ent) == FS_OK)
        size = fs_file_size(&g_file);
    if (size == 0 || size > SHELF_FILE_MAX ||
        fs_file_read(&g_file, 0, size, g_buf) != FS_OK) {
        crypto_wipe(g_buf, sizeof g_buf);
        return -1;
    }
    fd = memfd_create(name, MFD_ALLOW_SEALING);
    if (fd >= 0 &&
        (write(fd, g_buf, (size_t)size) != (ssize_t)size ||
         fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE |
                                F_SEAL_SEAL) != 0)) {
        (void)close(fd);
        fd = -1;
    }
    crypto_wipe(g_buf, sizeof g_buf);
    return fd;
}

int shelf_keypair(shelf_keys *k)
{
    k->key_fd = -1;
    k->cert_fd = -1;
    k->key[0] = '\0';
    k->cert[0] = '\0';
    if (!g_fs_mounted)
        return SHELF_ERR;
    k->key_fd = to_memfd(SHELF_KEY_NAME, "falcon-mok.key");
    k->cert_fd = to_memfd(SHELF_CERT_NAME, "falcon-mok.crt");
    if (k->key_fd < 0 || k->cert_fd < 0) {
        shelf_keys_close(k);
        return SHELF_NO_KEY;
    }
    (void)snprintf(k->key, sizeof k->key, "/proc/self/fd/%d", k->key_fd);
    (void)snprintf(k->cert, sizeof k->cert, "/proc/self/fd/%d", k->cert_fd);
    return SHELF_OK;
}

void shelf_keys_close(shelf_keys *k)
{
    if (k->key_fd >= 0)
        (void)close(k->key_fd);
    if (k->cert_fd >= 0)
        (void)close(k->cert_fd);
    k->key_fd = -1;
    k->cert_fd = -1;
}

void shelf_close(void)
{
    g_fs_mounted = 0;
    cli_jmem_release();
    if (g_luks_open) {
        luks_dev_close(&g_luks);
        g_luks_open = 0;
    }
    crypto_wipe(&g_luks, sizeof g_luks);
    if (g_dev_fd >= 0) {
        (void)close(g_dev_fd);
        g_dev_fd = -1;
    }
}
