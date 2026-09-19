/* falcon-hook's pure half. See hookcore.h. */

#include "hookcore.h"

#include <lib/string.h>

#include <linux/dm-ioctl.h>

/* One linear target's parameters: "MAJ:MIN SECTOR", at most
 * 10 + 1 + 10 + 1 + 20 characters and a NUL, padded to keep the next target
 * spec 8-byte aligned as the kernel requires. */
#define PARAMS_CAP 48u
#define SPEC_SIZE  (sizeof(struct dm_target_spec) + PARAMS_CAP)

static size_t slen(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0')
        n++;
    return n;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int hook_cmdline_arg(const char *cmdline, size_t len, const char *key,
                     char *out, size_t cap)
{
    size_t kl, i = 0;
    int found = 0;

    if (cmdline == (const char *)0 || key == (const char *)0 ||
        out == (char *)0 || cap == 0)
        return 0;
    kl = slen(key);
    while (i < len) {
        size_t start, n;
        int q = 0;

        while (i < len && is_space(cmdline[i]))
            i++;
        start = i;
        while (i < len && (q || !is_space(cmdline[i])) && cmdline[i] != '\0') {
            if (cmdline[i] == '"')
                q = !q;
            i++;
        }
        n = i - start;
        if (n > kl && memcmp(cmdline + start, key, kl) == 0 &&
            cmdline[start + kl] == '=') {
            const char *v = cmdline + start + kl + 1u;
            size_t vl = n - kl - 1u, o = 0, j;

            for (j = 0; j < vl; j++) {
                if (v[j] == '"')
                    continue;
                if (o + 1u >= cap)
                    return 0;
                out[o++] = v[j];
            }
            out[o] = '\0';
            found = 1;
        }
        if (i < len && cmdline[i] == '\0')
            break;
    }
    return found;
}

const uint8_t *hook_efivar_data(const uint8_t *file, size_t len,
                                size_t *data_len)
{
    if (file == (const uint8_t *)0 || data_len == (size_t *)0 || len <= 4u)
        return (const uint8_t *)0;
    *data_len = len - 4u;
    return file + 4;
}

static int starts(const char *s, const char *p)
{
    while (*p != '\0') {
        if (*s != *p)
            return 0;
        s++;
        p++;
    }
    return 1;
}

int hook_disk_candidate(const char *name)
{
    static const char *const skip[] = {
        "loop", "ram", "zram", "dm-", "md", "nbd", "sr", "fd", (const char *)0
    };
    size_t i;

    if (name == (const char *)0 || name[0] == '\0' || name[0] == '.')
        return 0;
    for (i = 0; skip[i] != (const char *)0; i++) {
        if (starts(name, skip[i]))
            return 0;
    }
    return 1;
}

int hook_parse_devnum(const char *s, size_t len, uint32_t *maj, uint32_t *min)
{
    uint64_t a = 0, b = 0;
    size_t i = 0;
    int da = 0, db = 0;

    if (s == (const char *)0 || maj == (uint32_t *)0 || min == (uint32_t *)0)
        return 0;
    while (i < len && s[i] >= '0' && s[i] <= '9' && a <= 0xFFFFFFFFu) {
        a = a * 10u + (uint64_t)(s[i] - '0');
        i++;
        da = 1;
    }
    if (!da || i >= len || s[i] != ':')
        return 0;
    i++;
    while (i < len && s[i] >= '0' && s[i] <= '9' && b <= 0xFFFFFFFFu) {
        b = b * 10u + (uint64_t)(s[i] - '0');
        i++;
        db = 1;
    }
    if (!db || a > 0xFFFFFFFFu || b > 0xFFFFFFFFu)
        return 0;
    while (i < len && is_space(s[i]))
        i++;
    if (i != len)
        return 0;
    *maj = (uint32_t)a;
    *min = (uint32_t)b;
    return 1;
}

/* Decimal, into p; returns the characters written. */
static size_t put_dec(char *p, uint64_t v)
{
    char tmp[24];
    size_t n = 0, i;

    do {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0);
    for (i = 0; i < n; i++)
        p[i] = tmp[n - 1u - i];
    return n;
}

static void header_fill(struct dm_ioctl *io, size_t size, int with_uuid)
{
    memset(io, 0, sizeof(*io));
    io->version[0] = DM_VERSION_MAJOR;
    io->version[1] = 0;
    io->version[2] = 0;
    io->data_size = (uint32_t)size;
    io->data_start = (uint32_t)sizeof(*io);
    memcpy(io->name, HOOK_DM_NAME, sizeof(HOOK_DM_NAME));
    if (with_uuid)
        memcpy(io->uuid, HOOK_DM_UUID, sizeof(HOOK_DM_UUID));
}

int hook_dm_header(void *buf, size_t cap, int with_uuid)
{
    if (buf == (void *)0 || cap < sizeof(struct dm_ioctl))
        return HOOK_ERR_BAD_ARG;
    header_fill((struct dm_ioctl *)buf, sizeof(struct dm_ioctl), with_uuid);
    return HOOK_OK;
}

size_t hook_dm_table_size(uint32_t n_extents)
{
    return sizeof(struct dm_ioctl) + (size_t)n_extents * SPEC_SIZE;
}

unsigned long hook_devnum(uint32_t maj, uint32_t min)
{
    return (unsigned long)((min & 0xffu) | ((maj & 0xfffu) << 8) |
                           ((unsigned long)(min & ~0xffu) << 12) |
                           ((unsigned long)(maj & ~0xfffu) << 32));
}

int hook_dm_table(const extmap_hdr *hdr, const uint8_t *map, uint32_t maj,
                  uint32_t min, uint64_t base, void *buf, size_t cap,
                  size_t *len)
{
    struct dm_ioctl *io = (struct dm_ioctl *)buf;
    uint8_t *p;
    uint64_t logical = 0;
    size_t need;
    uint32_t i;

    if (hdr == (const extmap_hdr *)0 || map == (const uint8_t *)0 ||
        buf == (void *)0 || len == (size_t *)0 || hdr->n_extents == 0)
        return HOOK_ERR_BAD_ARG;
    *len = 0;
    if ((base % 512u) != 0)
        return HOOK_ERR_RANGE;
    need = hook_dm_table_size(hdr->n_extents);
    if (cap < need)
        return HOOK_ERR_NOSPACE;

    header_fill(io, need, 0);
    io->target_count = hdr->n_extents;
    io->flags = DM_READONLY_FLAG;

    p = (uint8_t *)buf + sizeof(*io);
    for (i = 0; i < hdr->n_extents; i++) {
        struct dm_target_spec *t = (struct dm_target_spec *)(void *)p;
        char *params = (char *)(p + sizeof(*t));
        extmap_extent e;
        uint64_t sectors, dev_off;
        size_t n = 0;

        extmap_extent_at(map, i, &e);
        if ((e.offset % 512u) != 0 ||
            (i + 1u < hdr->n_extents && (e.length % 512u) != 0) ||
            e.offset > (~(uint64_t)0) - base)
            return HOOK_ERR_RANGE;
        sectors = (e.length + 511u) / 512u;
        dev_off = (base + e.offset) / 512u;

        memset(t, 0, SPEC_SIZE);
        t->sector_start = logical;
        t->length = sectors;
        t->next = (uint32_t)SPEC_SIZE;
        memcpy(t->target_type, "linear", 7);
        n += put_dec(params + n, maj);
        params[n++] = ':';
        n += put_dec(params + n, min);
        params[n++] = ' ';
        n += put_dec(params + n, dev_off);
        params[n] = '\0';
        logical += sectors;
        p += SPEC_SIZE;
    }
    *len = need;
    return HOOK_OK;
}

/* udev's encoding of a link name: the characters it leaves alone, and \xHH
 * for the rest. */
static int udev_safe(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '#' || c == '+' || c == '-' ||
           c == '.' || c == ':' || c == '=' || c == '@' || c == '_';
}

/* An ECMA-119 8.4.26.1 date: 16 digits (YYYYMMDDHHMMSScc) and a zone byte.
 * Nonzero when it holds a date at all (all-'0' or all-NUL means unset). */
static int iso_date_set(const uint8_t *d)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (d[i] != '0' && d[i] != 0)
            return 1;
    }
    return 0;
}

int hook_iso_names(const uint8_t *pvd, char *label, size_t label_cap,
                   char *uuid, size_t uuid_cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 32, o = 0, i;
    const uint8_t *date;

    if (pvd == (const uint8_t *)0 || label == (char *)0 ||
        uuid == (char *)0 || label_cap == 0 || uuid_cap == 0)
        return 0;
    label[0] = '\0';
    uuid[0] = '\0';
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0)
        return 0;

    while (n > 0 && (pvd[40 + n - 1u] == ' ' || pvd[40 + n - 1u] == 0))
        n--;
    for (i = 0; i < n; i++) {
        uint8_t c = pvd[40 + i];

        if (udev_safe(c)) {
            if (o + 1u >= label_cap)
                break;
            label[o++] = (char)c;
        } else {
            if (o + 4u >= label_cap)
                break;
            label[o++] = '\\';
            label[o++] = 'x';
            label[o++] = hex[c >> 4];
            label[o++] = hex[c & 15u];
        }
    }
    label[o] = '\0';

    /* libblkid's choice: the modification date, else the creation date. */
    date = pvd + 830;
    if (!iso_date_set(date))
        date = pvd + 813;
    if (iso_date_set(date) && uuid_cap >= 23u) {
        static const unsigned groups[] = { 4, 2, 2, 2, 2, 2, 2 };
        size_t g, k = 0;

        o = 0;
        for (g = 0; g < 7; g++) {
            unsigned j;

            if (g > 0)
                uuid[o++] = '-';
            for (j = 0; j < groups[g]; j++)
                uuid[o++] = (char)date[k++];
        }
        uuid[o] = '\0';
    }
    return 1;
}

/* ---- nudges ---- */

static const hook_nudge g_nudges[] = {
    /* Proxmox VE's installer initrd scans /sys/block/{hd,sr,scd,sd,nvme}*
     * for its ISO and never a dm-* (its /init, "searching for block device
     * containing the ISO"). Its kernel has dm-mod built in, so the device
     * exists; the scan only has to look at it. */
    { "proxmox", "/init",
      "for i in /sys/block/hd* /sys/block/sr* ",
      "for i in /sys/block/dm* /sys/block/loop* /sys/block/hd* "
      "/sys/block/sr* " },
    /* antiX's live init (MX Linux) scans only USB and CD devices for
     * antiX/linuxfs by default, and a dm device is neither. Its boot-file
     * search takes a device outright (bdev=, BOOT_ID=name=...); this makes
     * the ISO's device that search's default, and a bdev= on the cmdline
     * still wins. */
    { "antix", "/init",
      "find_boot_file \"$files\" \"$BOOT_MP\" \"$BOOT_ID\"",
      "find_boot_file \"$files\" \"$BOOT_MP\" "
      "\"${BOOT_ID:-name=/dev/mapper/falcon-iso}\"" },
};

const hook_nudge *hook_nudges(size_t *count)
{
    if (count != (size_t *)0)
        *count = sizeof g_nudges / sizeof g_nudges[0];
    return g_nudges;
}

static size_t find_at(const char *text, size_t len, const char *pat,
                      size_t pl)
{
    size_t i;

    if (pl == 0 || pl > len)
        return (size_t)-1;
    for (i = 0; i + pl <= len; i++) {
        if (memcmp(text + i, pat, pl) == 0)
            return i;
    }
    return (size_t)-1;
}

int hook_patch(const char *text, size_t len, const char *find,
               const char *replace, char *out, size_t cap, size_t *out_len)
{
    size_t fl, rl, at;

    if (text == (const char *)0 || find == (const char *)0 ||
        replace == (const char *)0 || out == (char *)0 ||
        out_len == (size_t *)0)
        return HOOK_ERR_BAD_ARG;
    fl = slen(find);
    rl = slen(replace);
    if (find_at(text, len, replace, rl) != (size_t)-1)
        return 0;
    at = find_at(text, len, find, fl);
    if (at == (size_t)-1)
        return 0;
    if (len - fl + rl > cap)
        return HOOK_ERR_NOSPACE;
    memcpy(out, text, at);
    memcpy(out + at, replace, rl);
    memcpy(out + at + rl, text + at + fl, len - at - fl);
    *out_len = len - fl + rl;
    return 1;
}

/* ---- pre-mounts and module packages ---- */

static const hook_premount g_premounts[] = {
    /* debian-installer's cdrom-detect.postinst takes a mounted /cdrom as the
     * installation medium; its list-devices finds CDs by udev properties a
     * loop or dm device does not carry. */
    { "debian-installer", "/var/lib/dpkg/info/cdrom-detect.postinst",
      "/cdrom" },
};

const hook_premount *hook_premounts(size_t *count)
{
    if (count != (size_t *)0)
        *count = sizeof g_premounts / sizeof g_premounts[0];
    return g_premounts;
}

int hook_module_package(const char *name, const char *release,
                        const char *prefix)
{
    static const char mid[] = "-di_";
    static const char suf[] = ".udeb";
    size_t nl, pl, rl, ml = sizeof mid - 1u, sl = sizeof suf - 1u;

    if (name == (const char *)0 || release == (const char *)0 ||
        prefix == (const char *)0)
        return 0;
    nl = slen(name);
    pl = slen(prefix);
    rl = slen(release);
    if (rl == 0 || nl < pl + rl + ml + sl)
        return 0;
    return memcmp(name, prefix, pl) == 0 &&
           memcmp(name + pl, release, rl) == 0 &&
           memcmp(name + pl + rl, mid, ml) == 0 &&
           memcmp(name + nl - sl, suf, sl) == 0;
}
