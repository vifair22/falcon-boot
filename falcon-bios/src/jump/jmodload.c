/* falcon jump: reading a module off the floppy into its slot and starting
 * it. See jmodload.h and jmod.h. */

#include <start/bios.h>
#include <jump/jmodload.h>
#include <mem/e820.h>

#include <gz/gz.h>
#include <lib/string.h>
#include <tui/tui.h>

static void svc_log(const char *s)
{
    bios_puts(s);
}

static void svc_log_hex(uint64_t v)
{
    bios_put_hex64(v);
}

static jmod_services g_svc = {
    JMOD_ABI, svc_log, svc_log_hex, 0, tui_tick
};

/* One past the highest byte of usable RAM in the map (jmod.h, mem_top). */
static uint64_t ram_top(void)
{
    unsigned n = e820_count();
    unsigned i;
    uint64_t top = 0;

    for (i = 0; i < n; i++) {
        const e820_entry *e = e820_at(i);

        if (e != (const e820_entry *)0 && e->type == E820_USABLE &&
            e->base + e->len > top)
            top = e->base + e->len;
    }
    return top;
}

/* What the modules are read off, as the sentences name it. */
static const char *g_medium = "floppy";
static char g_why[192];

void jmod_set_medium(const char *noun)
{
    if (noun != (const char *)0 && noun[0] != '\0')
        g_medium = noun;
}

/* a, the medium's name, then b: a sentence naming what the module was read
 * off. */
static const char *say(const char *a, const char *b)
{
    size_t n = 0;
    const char *parts[3];
    size_t i;

    parts[0] = a;
    parts[1] = g_medium;
    parts[2] = b;
    for (i = 0; i < 3u; i++) {
        const char *p = parts[i];

        while (*p != '\0' && n + 1u < sizeof g_why)
            g_why[n++] = *p++;
    }
    g_why[n] = '\0';
    return g_why;
}

static const char *check_why(jmod_status rc)
{
    switch (rc) {
    case JMOD_ERR_MAGIC: return "the file is not a falcon jump module.";
    case JMOD_ERR_ABI:
        return say("the module is from another build of falcon jump. Copy "
                   "the ", "'s modules from the same build as its boot "
                   "sector.");
    case JMOD_ERR_SLOT:
        return "the module is built for another slot; it may have been "
               "copied under the wrong name.";
    case JMOD_ERR_SIZE:
        return say("the module is cut short on the ", ", or does not fit its "
                   "slot.");
    case JMOD_ERR_ENTRY: return "the module's entry point is not inside it.";
    case JMOD_ERR_ARG:
    case JMOD_OK:
        break;
    }
    return "the module could not be checked.";
}

int jmod_load(fs_vol *vol, const char *path, unsigned slot_idx,
              jmod_ops *ops, const char **why)
{
    uint32_t slot = JMOD_SLOT_ADDR(slot_idx);
    uint8_t *at = (uint8_t *)(uintptr_t)slot;
    uint8_t *stage = (uint8_t *)(uintptr_t)JMOD_GZ_STAGE;
    size_t len = 0;
    unsigned attempt;
    fs_dir scratch;
    fs_dirent e;
    fs_file f;
    jmod_info info;
    jmod_status rc;

    if (slot_idx >= JMOD_SLOTS) {
        *why = "the module names no slot this build has.";
        return -1;
    }
    /* The whole region at once: a machine that has it has every slot. */
    if (!e820_is_ram(JMOD_REGION, JMOD_REGION_END - JMOD_REGION)) {
        *why = "this machine's memory map has no RAM at 15-16 MB, where "
               "falcon jump's drivers run.";
        return -1;
    }
    if (fs_lookup(vol, path, &scratch, &e) != FS_OK || e.is_dir) {
        *why = say("the driver is not on the ", ".");
        return -1;
    }
    if (e.size > JMOD_SLOT_BYTES ||
        !e820_is_ram(JMOD_GZ_STAGE, JMOD_SLOT_BYTES)) {
        *why = say("the driver could not be read off the ", ".");
        return -1;
    }
    /* A module is always a gzip member: its CRC-32 is the check that the
     * floppy gave back what was written. Read twice at most, the second
     * time past the track cache, since a floppy that misreads once often
     * reads right again. */
    for (attempt = 0; ; attempt++) {
        if (fs_file_open(vol, &f, &e) != FS_OK ||
            fs_file_read(&f, 0, e.size, stage) != FS_OK) {
            *why = say("the driver could not be read off the ", ".");
            return -1;
        }
        if (e.size < 2u || stage[0] != 0x1Fu || stage[1] != 0x8Bu) {
            *why = "the file is not a falcon jump module.";
            return -1;
        }
        if (gz_member(stage, (size_t)e.size, at, JMOD_SLOT_BYTES,
                      &len) == GZ_OK)
            break;
        if (attempt == 1) {
            *why = say("the driver did not read back as it was written, "
                       "twice. The ", " may be failing.");
            return -1;
        }
        bios_puts("falcon-bios: ");
        bios_puts(path);
        bios_puts(" failed its check; reading it again\n");
        bios_disk_forget();
    }
    rc = jmod_check(at, len, len, slot, JMOD_SLOT_BYTES, &info);
    if (rc != JMOD_OK) {
        *why = check_why(rc);
        return -1;
    }
    memset(at + info.image_len, 0, (size_t)(info.mem_len - info.image_len));

    bios_puts("falcon-bios: module ");
    bios_puts(info.name);
    bios_puts(" at ");
    bios_put_hex64(slot);
    bios_puts(", ");
    bios_put_dec(info.image_len);
    bios_puts(" bytes\n");

    memset(ops, 0, sizeof *ops);
    g_svc.mem_top = ram_top();
    if (((jmod_init_fn)(uintptr_t)info.init)(&g_svc, ops) != 0 ||
        ops->probe == 0 || ops->dev == 0 || ops->read == 0 ||
        ops->serve == 0 || ops->int13 == 0) {
        *why = "the driver would not start.";
        return -1;
    }
    return 0;
}
