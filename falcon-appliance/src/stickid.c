/* Which partitions are the stick's. See stickid.h. */

#include "stickid.h"

#include <ident/identfs.h>
#include <luks/luks.h>

#include <string.h>

int stickid_from_cred(const char *buf, size_t len, ident_rec *out)
{
    ident_rec r;
    int rc;

    if (buf == NULL || out == NULL)
        return IDENT_ERR_ARG;
    rc = ident_decode_text(buf, len, &r);
    if (rc != IDENT_OK)
        return rc;
    if (r.role != IDENT_ROLE_BOOT)
        return IDENT_ERR_BAD;
    *out = r;
    return IDENT_OK;
}

/* Kilobytes each, and this runs once per partition in a single thread. */
static ident_fs_scratch g_fs;
static luks_vol g_vol;
static char g_json[LUKS_JSON_CAP];

int stickid_read(fs_read_fn read, void *ctx, ident_rec *out, int *luks)
{
    static const unsigned char magic[6] = { 'L', 'U', 'K', 'S', 0xba, 0xbe };
    unsigned char head[sizeof magic];
    uint32_t version = 0;
    const char *role = NULL;
    const char *id = NULL;
    size_t role_len = 0;
    size_t id_len = 0;

    if (read == NULL || out == NULL || luks == NULL)
        return IDENT_ERR_ARG;
    *luks = 0;
    if (read(ctx, 0, sizeof head, head) != 0)
        return IDENT_ERR_NONE;
    if (memcmp(head, magic, sizeof magic) != 0)
        return ident_probe(read, ctx, &g_fs, out);

    *luks = 1;
    if (luks_open(&g_vol, read, ctx, g_json, sizeof g_json) != LUKS_OK ||
        luks_falcon_token(&g_vol, &version, &role, &role_len, &id,
                          &id_len) != LUKS_OK)
        return IDENT_ERR_NONE;
    return ident_from_strings(version, role, role_len, id, id_len, out);
}

stickid_take stickid_judge(const ident_rec *install, const ident_rec *rec,
                           int luks)
{
    if (install == NULL || rec == NULL || !ident_same_install(install, rec))
        return STICKID_NONE;
    if (rec->role == IDENT_ROLE_LIBRARY && !luks)
        return STICKID_P2;
    if (rec->role == IDENT_ROLE_SHELF && luks)
        return STICKID_P3;
    return STICKID_NONE;
}
