/* falcon-bios: which processors take argon2id lanes. See mpcore.h. */

#include <mp/mpcore.h>

#define MADT_HEAD  44u      /* the ACPI header, the LAPIC address, flags */

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void add(mpb_cpu *cpus, size_t cap, size_t *n, uint32_t id,
                uint8_t enabled)
{
    size_t i;

    for (i = 0; i < *n; i++) {
        if (cpus[i].apic_id == id) {
            cpus[i].enabled = (uint8_t)(cpus[i].enabled | enabled);
            return;
        }
    }
    if (*n < cap) {
        cpus[*n].apic_id = id;
        cpus[*n].enabled = enabled;
        (*n)++;
    }
}

int mpb_parse_madt(const uint8_t *madt, size_t len, uint32_t *lapic,
                   mpb_cpu *cpus, size_t cap, size_t *n)
{
    size_t at = MADT_HEAD;

    *n = 0;
    *lapic = 0;
    if (len < MADT_HEAD || madt[0] != 'A' || madt[1] != 'P' ||
        madt[2] != 'I' || madt[3] != 'C' || le32(madt + 4) > len)
        return -1;
    len = le32(madt + 4);
    *lapic = le32(madt + 36);
    while (at + 2u <= len) {
        uint8_t type = madt[at];
        uint8_t elen = madt[at + 1u];

        if (elen < 2u || at + elen > len)
            return -1;
        if (type == 0u && elen >= 8u) {
            /* Processor Local APIC: ACPI ID, APIC ID, flags. */
            add(cpus, cap, n, madt[at + 3u], (uint8_t)(madt[at + 4u] & 1u));
        } else if (type == 9u && elen >= 16u) {
            /* Processor Local x2APIC: reserved, x2APIC ID, flags, UID. */
            add(cpus, cap, n, le32(madt + at + 4u),
                (uint8_t)(madt[at + 8u] & 1u));
        } else if (type == 5u && elen >= 12u) {
            /* Local APIC Address Override, 64 bits. */
            *lapic = le32(madt + at + 8u) == 0u ? le32(madt + at + 4u) : 0u;
        }
        at += elen;
    }
    return 0;
}

uint32_t mpb_smt_shift(uint32_t max, uint32_t eax_b0, uint32_t ecx_b0)
{
    /* Sub-leaf 0's level type, ECX bits 15:8, is 1 for SMT; EAX bits 4:0
     * are how far to shift an x2APIC ID right to leave the core. */
    if (max < 0xBu || ((ecx_b0 >> 8) & 0xFFu) != 1u)
        return 0;
    return eax_b0 & 0x1Fu;
}

/* Whether a worker already picked, ids[0..k), is on id's core. */
static int core_taken(const uint32_t *ids, uint32_t k, uint32_t id,
                      uint32_t shift)
{
    uint32_t j;

    for (j = 0; j < k; j++) {
        if ((ids[j] >> shift) == (id >> shift))
            return 1;
    }
    return 0;
}

uint32_t mpb_pick(const mpb_cpu *cpus, size_t n, uint32_t bsp,
                  uint32_t smt_shift, uint32_t want, uint32_t *ids)
{
    uint32_t k = 0;
    size_t i;

    if (smt_shift > 31u)
        return 0;
    for (i = 0; i < n && k < want; i++) {
        uint32_t id = cpus[i].apic_id;

        if (!cpus[i].enabled || id == bsp ||
            (id >> smt_shift) == (bsp >> smt_shift) ||
            core_taken(ids, k, id, smt_shift))
            continue;
        ids[k++] = id;
    }
    return k;
}
