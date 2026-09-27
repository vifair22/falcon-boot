/* falcon-boot mkstick: the cross-check between what this tool stamps into a
 * partition table and what falcon-core/src/part/part.h makes the loader look
 * for.
 *
 * part.h stores each GUID as the 16-byte on-disk sequence; sgdisk takes the
 * textual form. Two spellings of one constant is exactly the kind of pair
 * that drifts silently, and the failure it produces (a stick that formats
 * perfectly and whose partitions the loader then cannot find) costs a full
 * build-and-boot cycle to diagnose. So the strings are parsed back into bytes
 * and compared against the loader's own constants before a partition table is
 * written or read. It is the runtime form of a static assertion; it cannot be
 * a real one because the byte arrays are `extern const` objects, not
 * constant expressions.
 */

#include <mkstick/mkstick.h>

#include <stdio.h>

#include <part/part.h>

/* GPT type GUIDs are mixed-endian: the first three fields are stored
 * little-endian, the last two byte-for-byte as written. This is the same
 * layout EFI_GUID uses, which is the whole reason part.h keeps them as bytes. */
static int guid_from_text(const char *text, uint8_t out[GPT_GUID_LEN])
{
    static const int order[GPT_GUID_LEN] = {
        3, 2, 1, 0,  5, 4,  7, 6,  8, 9,  10, 11, 12, 13, 14, 15
    };
    uint8_t flat[GPT_GUID_LEN];
    size_t n = 0;
    size_t i;

    for (i = 0; text[i] != '\0'; i++) {
        int hi;
        int lo;

        if (text[i] == '-')
            continue;
        if (text[i + 1] == '\0' || n >= GPT_GUID_LEN)
            return 0;

        hi = (int)text[i];
        lo = (int)text[i + 1];
        i++;

        {
            int v = 0;
            int k;

            for (k = 0; k < 2; k++) {
                int c = (k == 0) ? hi : lo;

                v <<= 4;
                if (c >= '0' && c <= '9')
                    v |= c - '0';
                else if (c >= 'a' && c <= 'f')
                    v |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')
                    v |= c - 'A' + 10;
                else
                    return 0;
            }
            flat[n++] = (uint8_t)v;
        }
    }
    if (n != GPT_GUID_LEN)
        return 0;

    for (i = 0; i < GPT_GUID_LEN; i++)
        out[i] = flat[order[i]];
    return 1;
}

static cli_status check_one(const char *text, const uint8_t *expect,
                            const char *what)
{
    uint8_t got[GPT_GUID_LEN];

    if (!guid_from_text(text, got))
        return set_error(CLI_ERR_INTERNAL, "%s: '%s' is not a GUID",
                         what, text);
    if (!gpt_guid_eq(got, expect))
        return set_error(CLI_ERR_INTERNAL,
                         "%s: the type GUID this tool writes (%s) is not the "
                         "one falcon-core/src/part/part.h makes the loader "
                         "look for",
                         what, text);
    return CLI_OK;
}

cli_status mkstick_guid_selfcheck(void)
{
    cli_status rc;

    /* p2 wears Microsoft basic data, so Windows gives it a drive letter
     * (DESIGN Section 3.1); its record, not its type, says what it is. */
    rc = check_one(MKS_TYPE_ISO, gpt_guid_msbasic, "p2 (ISO library)");
    if (rc != CLI_OK)
        return rc;
    rc = check_one(MKS_TYPE_SHELF, falcon_guid_shelf, "p3 (UKI shelf)");
    if (rc != CLI_OK)
        return rc;
    rc = check_one(MKS_TYPE_ESP, gpt_guid_esp, "p1 (EFI system partition)");
    return rc;
}
