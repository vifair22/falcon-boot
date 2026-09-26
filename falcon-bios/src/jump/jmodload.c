/* falcon jump: reading a module off the floppy into its slot and starting
 * it. See jmodload.h and jmod.h. */

#include <start/bios.h>
#include <jump/jmodload.h>
#include <mem/e820.h>

#include <lib/string.h>

static void svc_log(const char *s)
{
    bios_puts(s);
}

static void svc_log_hex(uint64_t v)
{
    bios_put_hex64(v);
}

static const jmod_services g_svc = {
    JMOD_ABI, svc_log, svc_log_hex
};

static const char *check_why(jmod_status rc)
{
    switch (rc) {
    case JMOD_ERR_MAGIC: return "the file is not a falcon jump module.";
    case JMOD_ERR_ABI:
        return "the module is from another build of falcon jump. Copy the "
               "floppy's modules from the same build as its boot sector.";
    case JMOD_ERR_SLOT:
        return "the module is built for another slot; it may have been "
               "copied under the wrong name.";
    case JMOD_ERR_SIZE:
        return "the module is cut short on the floppy, or does not fit its "
               "slot.";
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
        *why = "the driver is not on the floppy.";
        return -1;
    }
    if (e.size > JMOD_SLOT_BYTES || fs_file_open(vol, &f, &e) != FS_OK ||
        fs_file_read(&f, 0, e.size, at) != FS_OK) {
        *why = "the driver could not be read off the floppy.";
        return -1;
    }
    rc = jmod_check(at, (size_t)e.size, e.size, slot, JMOD_SLOT_BYTES, &info);
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
    if (((jmod_init_fn)(uintptr_t)info.init)(&g_svc, ops) != 0 ||
        ops->probe == 0 || ops->dev == 0 || ops->read == 0 ||
        ops->serve == 0 || ops->int13 == 0) {
        *why = "the driver would not start.";
        return -1;
    }
    return 0;
}
