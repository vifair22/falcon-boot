/* Tests for falcon-efi/src/loader/bli.c, the boot-timing variables
 * (docs/DESIGN.md Section 5.5).
 *
 * The module's platform is one SetVariable and four system-table fields, so
 * a fake that records every write is the whole of it. What is asserted is
 * what systemd-analyze and bootctl read: the name, the namespace, volatile
 * attributes, and UTF-16 with its terminator counted in the size. The GUIDs
 * are transcribed apart from the module's copies, so a typo there fails
 * here. */

#include "tests.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>

#include <string.h>

#include <loader/bli.h>

/* 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f, systemd's Loader GUID. */
static const EFI_GUID guid_loader = {
    0x4a67b082, 0x0a4c, 0x41cf,
    { 0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f }
};
/* 50614485-081b-4b7c-8b54-2abeffdf2fb3, falcon's. */
static const EFI_GUID guid_falcon = {
    0x50614485, 0x081b, 0x4b7c,
    { 0x8b, 0x54, 0x2a, 0xbe, 0xff, 0xdf, 0x2f, 0xb3 }
};

#define REC_MAX 8
#define NAME_MAX 40
#define DATA_MAX 128

typedef struct {
    char     name[NAME_MAX];
    EFI_GUID guid;
    UINT32   attrs;
    UINTN    size;
    uint8_t  data[DATA_MAX];
} rec;

static rec recs[REC_MAX];
static UINTN nrec;
/* Writes whose name is this fail with EFI_DEVICE_ERROR, after being
 * recorded. */
static const char *fail_name;

static EFI_STATUS EFIAPI fake_set(CHAR16 *name, EFI_GUID *guid, UINT32 attrs,
                                  UINTN size, void *data)
{
    rec *r;

    assert_true(nrec < REC_MAX);
    assert_true(size <= DATA_MAX);
    r = &recs[nrec++];
    ucs2_to_ascii((const uint16_t *)name, r->name, sizeof r->name);
    r->guid = *guid;
    r->attrs = attrs;
    r->size = size;
    if (size > 0)
        memcpy(r->data, data, size);
    if (fail_name != NULL && strcmp(r->name, fail_name) == 0)
        return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}

static EFI_RUNTIME_SERVICES fake_rt;
static EFI_SYSTEM_TABLE fake_st;
static uint16_t vendor[300];

static void reset(void)
{
    static const uint16_t edk[] = u"EDK II";

    memset(recs, 0, sizeof recs);
    nrec = 0;
    fail_name = NULL;
    memset(&fake_rt, 0, sizeof fake_rt);
    memset(&fake_st, 0, sizeof fake_st);
    fake_rt.SetVariable = fake_set;
    fake_st.RuntimeServices = &fake_rt;
    memcpy(vendor, edk, sizeof edk);
    fake_st.FirmwareVendor = vendor;
    fake_st.FirmwareRevision = 0x10000u;
    fake_st.Hdr.Revision = (2u << 16) | 70u;
}

static const rec *find(const char *name)
{
    UINTN i;

    for (i = 0; i < nrec; i++)
        if (strcmp(recs[i].name, name) == 0)
            return &recs[i];
    return NULL;
}

/* name was written once, under guid, volatile, as want in UTF-16 with its
 * terminator. */
static void assert_var(const char *name, const EFI_GUID *guid,
                       const char *want)
{
    const rec *r = find(name);
    char got[DATA_MAX];
    size_t n = strlen(want);

    assert_non_null(r);
    assert_memory_equal(&r->guid, guid, sizeof *guid);
    assert_int_equal(r->attrs, EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                   EFI_VARIABLE_RUNTIME_ACCESS);
    assert_int_equal(r->attrs & EFI_VARIABLE_NON_VOLATILE, 0);
    assert_int_equal(r->size, (n + 1u) * 2u);
    assert_int_equal(r->data[n * 2u], 0);
    assert_int_equal(r->data[n * 2u + 1u], 0);
    ucs2_to_ascii((const uint16_t *)(const void *)r->data, got, sizeof got);
    assert_string_equal(got, want);
}

static void test_init(void **state)
{
    static const uint16_t info[] = u"falcon-boot 0.9.0_20261002.1200.release";

    (void)state;
    reset();
    assert_int_equal(bli_set_init(&fake_st, 4123456u, info), EFI_SUCCESS);
    assert_int_equal(nrec, 4u);
    assert_var("LoaderTimeInitUSec", &guid_loader, "4123456");
    assert_var("LoaderInfo", &guid_loader,
               "falcon-boot 0.9.0_20261002.1200.release");
    /* "%ls %u.%02u" and "UEFI %u.%02u", as systemd-boot formats them. */
    assert_var("LoaderFirmwareInfo", &guid_loader, "EDK II 1.00");
    assert_var("LoaderFirmwareType", &guid_loader, "UEFI 2.70");
}

static void test_init_revisions(void **state)
{
    (void)state;
    reset();
    fake_st.FirmwareRevision = (1u << 16) | 0x1234u;
    fake_st.Hdr.Revision = (2u << 16) | 3u;
    (void)bli_set_init(&fake_st, 1u, NULL);
    assert_var("LoaderFirmwareInfo", &guid_loader, "EDK II 1.4660");
    assert_var("LoaderFirmwareType", &guid_loader, "UEFI 2.03");
    assert_null(find("LoaderInfo"));
}

static void test_init_without_clock_or_vendor(void **state)
{
    (void)state;
    reset();
    fake_st.FirmwareVendor = NULL;
    (void)bli_set_init(&fake_st, 0, NULL);
    /* No clock: no init time. No vendor: no firmware info. */
    assert_null(find("LoaderTimeInitUSec"));
    assert_null(find("LoaderFirmwareInfo"));
    assert_var("LoaderFirmwareType", &guid_loader, "UEFI 2.70");
    assert_int_equal(nrec, 1u);
}

static void test_init_long_vendor_left_unset(void **state)
{
    size_t i;

    (void)state;
    reset();
    for (i = 0; i < 250u; i++)
        vendor[i] = u'V';
    vendor[250] = 0;
    (void)bli_set_init(&fake_st, 1u, NULL);
    assert_null(find("LoaderFirmwareInfo"));
}

static void test_exec(void **state)
{
    (void)state;
    reset();
    assert_int_equal(bli_set_exec(&fake_st, 9876543u, 4500000u), EFI_SUCCESS);
    assert_int_equal(nrec, 2u);
    assert_var("LoaderTimeExecUSec", &guid_loader, "9876543");
    /* falcon's own, under falcon's GUID, never the Loader one. */
    assert_var("FalconTimeWaitUSec", &guid_falcon, "4500000");
}

/* A UKI that returns leaves no exec time behind for a later boot: both are
 * deleted (size 0) under the GUIDs they were written with. */
static void test_clear_exec(void **state)
{
    (void)state;
    reset();
    bli_clear_exec(&fake_st);
    assert_int_equal(nrec, 2u);
    assert_string_equal(recs[0].name, "LoaderTimeExecUSec");
    assert_memory_equal(&recs[0].guid, &guid_loader, sizeof(EFI_GUID));
    assert_int_equal(recs[0].size, 0u);
    assert_string_equal(recs[1].name, "FalconTimeWaitUSec");
    assert_memory_equal(&recs[1].guid, &guid_falcon, sizeof(EFI_GUID));
    assert_int_equal(recs[1].size, 0u);
}

static void test_exec_zero_wait_is_written(void **state)
{
    (void)state;
    reset();
    (void)bli_set_exec(&fake_st, 9876543u, 0);
    assert_var("FalconTimeWaitUSec", &guid_falcon, "0");
}

static void test_exec_without_clock_writes_nothing(void **state)
{
    (void)state;
    reset();
    assert_int_equal(bli_set_exec(&fake_st, 0, 100u), EFI_SUCCESS);
    assert_int_equal(nrec, 0u);
}

static void test_failure_is_reported_and_the_rest_written(void **state)
{
    (void)state;
    reset();
    fail_name = "LoaderInfo";
    assert_int_equal(bli_set_init(&fake_st, 5u, u"falcon-boot x"),
                     EFI_DEVICE_ERROR);
    assert_int_equal(nrec, 4u);
    reset();
    fail_name = "LoaderTimeExecUSec";
    assert_int_equal(bli_set_exec(&fake_st, 5u, 1u), EFI_DEVICE_ERROR);
    assert_non_null(find("FalconTimeWaitUSec"));
}

static void test_no_set_variable(void **state)
{
    (void)state;
    reset();
    fake_rt.SetVariable = NULL;
    assert_int_equal(bli_set_init(&fake_st, 5u, u"x"), EFI_UNSUPPORTED);
    assert_int_equal(bli_set_exec(&fake_st, 5u, 1u), EFI_UNSUPPORTED);
    fake_st.RuntimeServices = NULL;
    assert_int_equal(bli_set_init(&fake_st, 5u, u"x"), EFI_UNSUPPORTED);
    assert_int_equal(bli_set_exec(&fake_st, 5u, 1u), EFI_UNSUPPORTED);
    assert_int_equal(bli_set_init(NULL, 5u, u"x"), EFI_INVALID_PARAMETER);
    assert_int_equal(bli_set_exec(NULL, 5u, 1u), EFI_INVALID_PARAMETER);
    assert_int_equal(nrec, 0u);
}

int run_bli_tests(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_init),
        cmocka_unit_test(test_init_revisions),
        cmocka_unit_test(test_init_without_clock_or_vendor),
        cmocka_unit_test(test_init_long_vendor_left_unset),
        cmocka_unit_test(test_exec),
        cmocka_unit_test(test_clear_exec),
        cmocka_unit_test(test_exec_zero_wait_is_written),
        cmocka_unit_test(test_exec_without_clock_writes_nothing),
        cmocka_unit_test(test_failure_is_reported_and_the_rest_written),
        cmocka_unit_test(test_no_set_variable),
    };

    return cmocka_run_group_tests_name("bli", tests, NULL, NULL);
}
