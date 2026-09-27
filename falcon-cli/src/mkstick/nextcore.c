/* falcon-boot mkstick: the pure half of `next-boot`. See nextcore.h. */

#include <mkstick/nextcore.h>

#include <string.h>

#include <media/nextboot.h>

size_t nextcore_efivar(const char *value, uint8_t *buf, size_t cap)
{
    char check[NEXTBOOT_MAX];
    size_t len;

    if (value == NULL || buf == NULL)
        return 0;
    len = strlen(value);
    if (nextboot_decode(value, len, check, sizeof check) != NEXTBOOT_OK)
        return 0;
    if (cap < 4u + len + 1u)
        return 0;
    buf[0] = (uint8_t)(NEXTCORE_EFIVAR_ATTRS & 0xFFu);
    buf[1] = (uint8_t)((NEXTCORE_EFIVAR_ATTRS >> 8) & 0xFFu);
    buf[2] = (uint8_t)((NEXTCORE_EFIVAR_ATTRS >> 16) & 0xFFu);
    buf[3] = (uint8_t)((NEXTCORE_EFIVAR_ATTRS >> 24) & 0xFFu);
    memcpy(buf + 4, value, len);
    buf[4u + len] = 0u;
    return 4u + len + 1u;
}

int nextcore_efivar_value(const uint8_t *buf, size_t len, char *out,
                          size_t cap)
{
    if (buf == NULL || out == NULL || cap == 0u)
        return -1;
    out[0] = '\0';
    if (len <= 4u)
        return -1;
    return nextboot_decode(buf + 4, len - 4u, out, cap) == NEXTBOOT_OK ? 0
                                                                       : -1;
}

int nextcore_parse_epoch(const char *text, size_t len, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;

    if (text == NULL || out == NULL)
        return -1;
    if (len > 0u && text[len - 1u] == '\n')
        len--;
    if (len == 0u || len > 19u)
        return -1;
    for (i = 0; i < len; i++) {
        if (text[i] < '0' || text[i] > '9')
            return -1;
        v = v * 10u + (uint64_t)(text[i] - '0');
    }
    *out = v;
    return 0;
}

int nextcore_sector_ours(const uint8_t *sec, size_t len)
{
    size_t i;

    if (sec == NULL || !nextboot_sector_size_ok(len))
        return 0;
    if (nextboot_sector_is_marker(sec, len))
        return 1;
    for (i = 0; i < len; i++) {
        if (sec[i] != 0u)
            return 0;
    }
    return 1;
}
