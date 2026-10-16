/* Tests for falcon-core/src/media/shelf.c.
 *
 * The shelf is read through the filesystem registry, so these run it over a
 * driver of their own: an in-memory tree behind an fs_ops, which lets every
 * shape the chooser meets be written down directly (a /uki, an /EFI/Linux,
 * decoys beside the images, images that do and do not name themselves).
 * The images are minimal PE files carrying .osrel and .uname, the two
 * sections the labels come from. falcon-efi/tools/mk-test-labels.sh is the
 * same set of shapes on a real ext4, read by both loaders in the rig.
 */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <fs/fs.h>
#include <media/shelf.h>

/* ---- an in-memory tree behind fs_ops ---- */

#define NODES_MAX (SHELF_MAX + 8u)
#define DATA_MAX  1024u

typedef struct {
    const char *name;
    int         parent;     /* -1 for the root's children's parent */
    uint8_t     is_dir;
    uint8_t     data[DATA_MAX];
    size_t      len;
} node;

static node   g_nodes[NODES_MAX];
static size_t g_n;

static int add(int parent, const char *name, int is_dir)
{
    node *n = &g_nodes[g_n];

    memset(n, 0, sizeof *n);
    n->name = name;
    n->parent = parent;
    n->is_dir = (uint8_t)is_dir;
    return (int)g_n++;
}

/* The dir and file iterators keep a node index in their private bytes. */
typedef struct {
    int dir;
    size_t next;
} mock_dir;

static int mock_probe(fs_read_fn read, void *ctx)
{
    (void)read;
    (void)ctx;
    return FS_OK;
}

static int mock_mount(fs_vol *v, fs_read_fn read, void *ctx)
{
    (void)v;
    (void)read;
    (void)ctx;
    return FS_OK;
}

static int open_at(fs_dir *d, int dir)
{
    mock_dir m = { dir, 0 };

    memcpy(d->priv.b, &m, sizeof m);
    return FS_OK;
}

static int mock_root(fs_vol *v, fs_dir *d)
{
    (void)v;
    return open_at(d, -1);
}

static int mock_open(fs_vol *v, fs_dir *d, const fs_dirent *e)
{
    int idx;

    (void)v;
    memcpy(&idx, e->priv.b, sizeof idx);
    return open_at(d, idx);
}

static int mock_next(fs_dir *d, fs_dirent *out)
{
    mock_dir m;

    memcpy(&m, d->priv.b, sizeof m);
    while (m.next < g_n && g_nodes[m.next].parent != m.dir)
        m.next++;
    if (m.next >= g_n)
        return 0;
    {
        const node *n = &g_nodes[m.next];
        int idx = (int)m.next;
        size_t i;

        memset(out, 0, sizeof *out);
        for (i = 0; n->name[i] != '\0'; i++)
            out->name[i] = (uint16_t)(unsigned char)n->name[i];
        out->name_len = (uint16_t)i;
        out->size = n->len;
        out->is_dir = n->is_dir;
        out->is_regular = (uint8_t)!n->is_dir;
        memcpy(out->priv.b, &idx, sizeof idx);
    }
    m.next++;
    memcpy(d->priv.b, &m, sizeof m);
    return 1;
}

static int mock_file_open(fs_vol *v, fs_file *f, const fs_dirent *e)
{
    (void)v;
    memcpy(f->priv.b, e->priv.b, sizeof(int));
    return FS_OK;
}

static int mock_file_read(fs_file *f, uint64_t off, uint64_t len, void *buf)
{
    int idx;
    const node *n;

    memcpy(&idx, f->priv.b, sizeof idx);
    n = &g_nodes[idx];
    if (off > n->len || len > n->len - off)
        return FS_ERR_RANGE;
    memcpy(buf, n->data + off, (size_t)len);
    return FS_OK;
}

static uint64_t mock_file_size(const fs_file *f)
{
    int idx;

    memcpy(&idx, f->priv.b, sizeof idx);
    return g_nodes[idx].len;
}

static const fs_ops g_mock = {
    "mock", FS_HINT_NONE, mock_probe, mock_mount, mock_root, mock_open,
    mock_next, mock_file_open, mock_file_read, mock_file_size,
    NULL, NULL, NULL
};

/* ---- images ---- */

/* A minimal PE32+ whose sections are .osrel and .uname, each present only
 * when given. Layout as in osrel_test.c: DOS stub, PE at 0x80, COFF header,
 * the optional header, the section table, then the bodies. */
#define PE_AT   0x80u
#define OPTSIZE 0xF0u
#define SECT_AT (PE_AT + 24u + OPTSIZE)
#define BODY_AT (SECT_AT + 40u * 2u)

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void image(int idx, const char *osrel, const char *uname)
{
    node *n = &g_nodes[idx];
    uint8_t *img = n->data;
    uint16_t nsect = 0;
    size_t at = BODY_AT;

    img[0] = 'M';
    img[1] = 'Z';
    put32(img + 0x3C, PE_AT);
    img[PE_AT] = 'P';
    img[PE_AT + 1] = 'E';
    put16(img + PE_AT + 20, OPTSIZE);
    if (osrel != NULL) {
        uint8_t *s = img + SECT_AT + (size_t)40u * nsect++;
        size_t len = strlen(osrel);

        memcpy(s, ".osrel", 6);
        put32(s + 16, (uint32_t)len);
        put32(s + 20, (uint32_t)at);
        memcpy(img + at, osrel, len);
        at += len;
    }
    if (uname != NULL) {
        uint8_t *s = img + SECT_AT + (size_t)40u * nsect++;
        size_t len = strlen(uname);

        memcpy(s, ".uname", 6);
        put32(s + 16, (uint32_t)len);
        put32(s + 20, (uint32_t)at);
        memcpy(img + at, uname, len);
        at += len;
    }
    put16(img + PE_AT + 6, nsect);
    n->len = at;
}

/* ---- helpers ---- */

static shelf_list g_shelf;
static fs_vol     g_vol;

static int setup(void **state)
{
    (void)state;
    g_n = 0;
    memset(&g_shelf, 0, sizeof g_shelf);
    return fs_mount(&g_vol, &g_mock, (fs_read_fn)0, (void *)0) == FS_OK ? 0
                                                                          : -1;
}

static const char *narrow(const uint16_t *s)
{
    static char buf[SHELF_LABEL_MAX];
    size_t i;

    for (i = 0; s[i] != 0 && i + 1u < sizeof buf; i++)
        buf[i] = (s[i] < 128u) ? (char)s[i] : '?';
    buf[i] = '\0';
    return buf;
}

/* ---- collecting ---- */

static void test_collect_uki(void **state)
{
    int uki;

    (void)state;
    uki = add(-1, "uki", 1);
    image(add(uki, "b.efi", 0), NULL, NULL);
    image(add(uki, "a.efi", 0), NULL, NULL);
    (void)add(uki, "README", 0);
    (void)add(uki, "sub.efi", 1);        /* a directory, not an image */
    (void)add(uki, ".efi", 0);           /* a name, not an image */

    assert_int_equal(shelf_collect(&g_shelf, &g_vol), 1);
    assert_int_equal(g_shelf.n, 2);
    assert_string_equal(narrow(g_shelf.uki[0].ent.name), "a.efi");
    assert_string_equal(narrow(g_shelf.uki[1].ent.name), "b.efi");
}

static void test_collect_efi_linux(void **state)
{
    int efi;
    int linux_dir;

    (void)state;
    efi = add(-1, "EFI", 1);
    linux_dir = add(efi, "Linux", 1);
    image(add(linux_dir, "arch.efi", 0), NULL, NULL);

    assert_int_equal(shelf_collect(&g_shelf, &g_vol), 1);
    assert_int_equal(g_shelf.n, 1);
    assert_string_equal(narrow(g_shelf.uki[0].ent.name), "arch.efi");
}

static void test_collect_nothing(void **state)
{
    int efi;

    (void)state;
    assert_int_equal(shelf_collect(&g_shelf, &g_vol), 0);

    efi = add(-1, "EFI", 1);
    (void)add(efi, "BOOT", 1);
    assert_int_equal(shelf_collect(&g_shelf, &g_vol), 0);

    (void)add(-1, "uki", 1);
    assert_int_equal(shelf_collect(&g_shelf, &g_vol), 0);
}

static void test_collect_cap(void **state)
{
    static char names[SHELF_MAX + 2u][8];
    int uki;
    size_t i;

    (void)state;
    uki = add(-1, "uki", 1);
    for (i = 0; i < SHELF_MAX + 2u; i++) {
        snprintf(names[i], sizeof names[i], "u%02u.efi", (unsigned)i);
        (void)add(uki, names[i], 0);
    }
    assert_int_equal(shelf_collect(&g_shelf, &g_vol), 1);
    assert_int_equal(g_shelf.n, SHELF_MAX);
}

/* ---- labels and order ---- */

/* The rig's label fixtures (mk-test-labels.sh), in the rows the UEFI and
 * BIOS choosers both drew on 2026-09-24. */
static void test_label_fixture_set(void **state)
{
    int uki;

    (void)state;
    uki = add(-1, "uki", 1);
    image(add(uki, "appliance-1.efi", 0),
          "PRETTY_NAME=\"falcon appliance\"\n", "6.1.0");
    image(add(uki, "arch-a.efi", 0), "PRETTY_NAME=\"Arch Linux\"\n",
          "7.2.6-arch2-1");
    image(add(uki, "arch-b.efi", 0), "PRETTY_NAME=\"Arch Linux\"\n",
          "6.18.52-1-lts\n");
    image(add(uki, "personal.efi", 0), "NAME=\"Arch Linux\"\n", NULL);
    image(add(uki, "solo.efi", 0), "PRETTY_NAME=\"Solo Linux 42\"\n", NULL);
    image(add(uki, "twin-a.efi", 0), "PRETTY_NAME=\"Twin Linux\"\n",
          "9.9.9-twin");
    image(add(uki, "twin-b.efi", 0), "PRETTY_NAME=\"Twin Linux\"\n",
          "9.9.9-twin");
    image(add(uki, "zzz.efi", 0), NULL, NULL);

    assert_int_equal(shelf_collect(&g_shelf, &g_vol), 1);
    assert_int_equal(g_shelf.n, 8);
    shelf_label(&g_shelf, &g_vol);

    /* Versioned images first, newest first; the appliance last. */
    assert_string_equal(narrow(g_shelf.uki[0].label),
                        "Personal OS: Twin Linux (twin-a.efi)");
    assert_string_equal(narrow(g_shelf.uki[1].label),
                        "Personal OS: Twin Linux (twin-b.efi)");
    assert_string_equal(narrow(g_shelf.uki[2].label),
                        "Personal OS: Arch Linux 7.2.6-arch2-1");
    assert_string_equal(narrow(g_shelf.uki[3].label),
                        "Personal OS: Arch Linux 6.18.52-1-lts");
    assert_string_equal(narrow(g_shelf.uki[4].label),
                        "Personal OS: Arch Linux (personal.efi)");
    assert_string_equal(narrow(g_shelf.uki[5].label),
                        "Personal OS: Solo Linux 42");
    assert_string_equal(narrow(g_shelf.uki[6].label),
                        "Personal OS: zzz.efi");
    assert_string_equal(narrow(g_shelf.uki[7].label),
                        "Personal OS: falcon appliance");
    assert_string_equal(g_shelf.uki[2].uname, "7.2.6-arch2-1");
    assert_string_equal(g_shelf.uki[3].uname, "6.18.52-1-lts");
}

static void test_label_unique_needs_nothing(void **state)
{
    int uki;

    (void)state;
    uki = add(-1, "uki", 1);
    image(add(uki, "a.efi", 0), "PRETTY_NAME=\"Alpha\"\n", "1.0");
    image(add(uki, "b.efi", 0), "PRETTY_NAME=\"Beta\"\n", "2.0");

    assert_int_equal(shelf_collect(&g_shelf, &g_vol), 1);
    shelf_label(&g_shelf, &g_vol);
    assert_string_equal(narrow(g_shelf.uki[0].label), "Personal OS: Beta");
    assert_string_equal(narrow(g_shelf.uki[1].label), "Personal OS: Alpha");
}

static void test_label_unreadable_keeps_name(void **state)
{
    int uki;
    int bad;

    (void)state;
    uki = add(-1, "uki", 1);
    bad = add(uki, "bad.efi", 0);
    g_nodes[bad].len = 4;               /* too short to be anything */
    memcpy(g_nodes[bad].data, "MZ\0\0", 4);

    assert_int_equal(shelf_collect(&g_shelf, &g_vol), 1);
    shelf_label(&g_shelf, &g_vol);
    assert_string_equal(narrow(g_shelf.uki[0].label), "Personal OS: bad.efi");
    assert_int_equal(g_shelf.uki[0].named, 0);
}

static void test_is_appliance(void **state)
{
    fs_dirent e;
    const char *yes = "Appliance-2026.efi";
    const char *no = "applianc.efi";
    size_t i;

    (void)state;
    memset(&e, 0, sizeof e);
    for (i = 0; yes[i] != '\0'; i++)
        e.name[i] = (uint16_t)(unsigned char)yes[i];
    e.name_len = (uint16_t)i;
    assert_true(shelf_is_appliance(&e));

    memset(&e, 0, sizeof e);
    for (i = 0; no[i] != '\0'; i++)
        e.name[i] = (uint16_t)(unsigned char)no[i];
    e.name_len = (uint16_t)i;
    assert_false(shelf_is_appliance(&e));

    e.name_len = 3;
    assert_false(shelf_is_appliance(&e));
}

static void test_wipe(void **state)
{
    int uki;
    size_t i;
    const unsigned char *b = (const unsigned char *)&g_shelf;

    (void)state;
    uki = add(-1, "uki", 1);
    image(add(uki, "a.efi", 0), "PRETTY_NAME=\"Alpha\"\n", "1.0");
    assert_int_equal(shelf_collect(&g_shelf, &g_vol), 1);
    shelf_label(&g_shelf, &g_vol);
    shelf_wipe(&g_shelf);
    for (i = 0; i < sizeof g_shelf; i++)
        assert_int_equal(b[i], 0);
}

int run_shelf_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_collect_uki, setup),
        cmocka_unit_test_setup(test_collect_efi_linux, setup),
        cmocka_unit_test_setup(test_collect_nothing, setup),
        cmocka_unit_test_setup(test_collect_cap, setup),
        cmocka_unit_test_setup(test_label_fixture_set, setup),
        cmocka_unit_test_setup(test_label_unique_needs_nothing, setup),
        cmocka_unit_test_setup(test_label_unreadable_keeps_name, setup),
        cmocka_unit_test_setup(test_is_appliance, setup),
        cmocka_unit_test_setup(test_wipe, setup),
    };

    return cmocka_run_group_tests_name("media shelf", tests, NULL, NULL);
}
