/* The MOK keypair off a shelf into sealed memfds. shelfkey.h has why. */

#include <mkstick/shelfkey.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <common/jmem.h>
#include <common/log.h>
#include <crypto/crypto.h>
#include <fs/fs.h>
#include <luks/luks.h>
#include <mkstick/disk.h>
#include <mkstick/mkstick.h>

/* A key or certificate is a few kilobytes; anything larger on the shelf
 * under these names is not one. */
#define SHELFKEY_MAX 65536u

typedef struct {
    disk_io         io;
    luks_vol        vol;
    luks_master_key mk;
    luks_dev        dev;
    fs_vol          fs;
    char            json[LUKS_JSON_CAP];
    fs_dir          scratch;
    fs_file         file;
    uint8_t         buf[SHELFKEY_MAX];
} shelfkey_ctx;

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

static const luks_kdf_mem kdf_mem = { kdf_get, kdf_put, NULL };

/* One file off the shelf into a sealed memfd; the read buffer is wiped as
 * soon as the bytes are in it. The descriptor, or -1 with the error set. */
static int to_memfd(shelfkey_ctx *c, const char *path, const char *name)
{
    fs_dirent ent;
    uint64_t size;
    size_t done = 0;
    int fd;

    if (fs_lookup(&c->fs, path, &c->scratch, &ent) != FS_OK || ent.is_dir ||
        fs_file_open(&c->fs, &c->file, &ent) != FS_OK) {
        (void)set_error(CLI_ERR_NOTFOUND, "the shelf has no %s", path);
        return -1;
    }
    size = fs_file_size(&c->file);
    if (size == 0u || size > sizeof c->buf) {
        (void)set_error(CLI_ERR_INVALID, "%s on the shelf is not a key or "
                        "certificate", path);
        return -1;
    }
    if (fs_file_read(&c->file, 0, size, c->buf) != FS_OK) {
        crypto_wipe(c->buf, sizeof c->buf);
        (void)set_error(CLI_ERR_IO, "reading %s off the shelf", path);
        return -1;
    }
    fd = memfd_create(name, MFD_ALLOW_SEALING);
    if (fd < 0) {
        crypto_wipe(c->buf, sizeof c->buf);
        (void)set_error_errno(CLI_ERR_IO, "memfd_create");
        return -1;
    }
    while (done < (size_t)size) {
        ssize_t n = write(fd, c->buf + done, (size_t)size - done);

        if (n <= 0) {
            crypto_wipe(c->buf, sizeof c->buf);
            (void)close(fd);
            (void)set_error_errno(CLI_ERR_IO, "writing the memfd");
            return -1;
        }
        done += (size_t)n;
    }
    crypto_wipe(c->buf, sizeof c->buf);
    if (lseek(fd, 0, SEEK_SET) != 0 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE |
                               F_SEAL_SEAL) != 0) {
        (void)close(fd);
        (void)set_error_errno(CLI_ERR_IO, "sealing the memfd");
        return -1;
    }
    return fd;
}

cli_status shelf_key_open(shelf_key *s, const char *disk, uint64_t off,
                          uint64_t len, int luks, const mks_pass *pass)
{
    static shelfkey_ctx c;
    const fs_ops *ops;
    fs_jinfo jinfo;
    fs_read_fn read = disk_io_read;
    void *ctx = &c.io;
    uint32_t slot = 0;
    cli_status rc = CLI_OK;

    memset(s, 0, sizeof *s);
    s->key_fd = -1;
    s->cert_fd = -1;
    memset(&c, 0, sizeof c);
    rc = disk_io_open(&c.io, disk, off, len);
    if (rc != CLI_OK)
        return rc;
    if (luks) {
        if (pass == NULL) {
            rc = set_error(CLI_ERR_USAGE, "the shelf is encrypted and no "
                           "passphrase was given");
            goto out;
        }
        if (luks_open(&c.vol, disk_io_read, &c.io, c.json, sizeof c.json) !=
            LUKS_OK) {
            rc = set_error(CLI_ERR_INVALID, "the shelf's LUKS2 header does "
                           "not read");
            goto out;
        }
        if (luks_unlock(&c.vol, pass->buf, pass->len, &kdf_mem, &c.mk,
                        &slot) != LUKS_OK) {
            rc = set_error(CLI_ERR_PERM, "the passphrase does not unlock "
                           "the shelf");
            goto out;
        }
        if (luks_dev_open(&c.dev, &c.vol, &c.mk) != LUKS_OK) {
            rc = set_error(CLI_ERR_INVALID, "the shelf's cipher is not one "
                           "the loaders read");
            goto out;
        }
        crypto_wipe(&c.mk, sizeof c.mk);
        read = luks_read;
        ctx = &c.dev;
    }
    ops = fs_identify(read, ctx, FS_HINT_SHELF);
    if (ops == NULL ||
        fs_mount_journal(&c.fs, ops, read, ctx, &cli_jmem, &jinfo) != FS_OK) {
        rc = set_error(CLI_ERR_INVALID, "the shelf holds no filesystem the "
                       "loaders read");
        goto out;
    }
    s->key_fd = to_memfd(&c, "/" MKS_P3_KEY_NAME, "falcon-mok.key");
    if (s->key_fd >= 0)
        s->cert_fd = to_memfd(&c, "/" MKS_P3_CERT_NAME, "falcon-mok.crt");
    if (s->key_fd < 0 || s->cert_fd < 0) {
        rc = cli_error_code();
        shelf_key_close(s);
        goto out;
    }
    (void)snprintf(s->k.key, sizeof s->k.key, "/proc/self/fd/%d", s->key_fd);
    (void)snprintf(s->k.crt, sizeof s->k.crt, "/proc/self/fd/%d", s->cert_fd);
    log_debug("the shelf's keypair is in memfds %d and %d", s->key_fd,
              s->cert_fd);
out:
    cli_jmem_release();
    if (luks)
        luks_dev_close(&c.dev);
    crypto_wipe(&c.mk, sizeof c.mk);
    crypto_wipe(&c.fs, sizeof c.fs);
    crypto_wipe(&c.file, sizeof c.file);
    disk_io_close(&c.io);
    return rc;
}

void shelf_key_close(shelf_key *s)
{
    if (s->key_fd >= 0)
        (void)close(s->key_fd);
    if (s->cert_fd >= 0)
        (void)close(s->cert_fd);
    s->key_fd = -1;
    s->cert_fd = -1;
    memset(&s->k, 0, sizeof s->k);
}
