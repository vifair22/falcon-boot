/* falcon-bios: reporting a processor exception (idt.S).
 *
 * Linked into stage 2, which is resident, so an exception in stage 3 is
 * reported by this code and not by a copy of it: the IDT idt.S builds
 * names these stubs and the thunk reloads that IDT, not one of stage 3's.
 */

#include <bios.h>

/* The architectural names, for the vectors that have one. */
static const char *const k_vectors[] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow",
    "bound range", "invalid opcode", "no FPU", "double fault",
    "coprocessor overrun", "invalid TSS", "segment not present",
    "stack fault", "general protection", "page fault", "reserved",
    "x87 error", "alignment check", "machine check", "SIMD error",
    "virtualization", "control protection",
};

/* Appends v in hex to buf at *n, within cap. The screen has no printf. */
static void put_hex(char *buf, size_t cap, size_t *n, uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    int leading = 1;

    for (shift = 60; shift >= 0; shift -= 4) {
        unsigned nibble = (unsigned)((v >> shift) & 0xFu);

        if (nibble == 0 && leading && shift != 0)
            continue;
        leading = 0;
        if (*n + 1u < cap)
            buf[(*n)++] = digits[nibble];
    }
    buf[*n] = '\0';
}

/* Appends v in decimal: the vector, which the manuals number that way. */
static void put_dec(char *buf, size_t cap, size_t *n, uint64_t v)
{
    char rev[20];
    size_t k = 0;

    do {
        rev[k++] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    } while (v > 0 && k < sizeof rev);
    while (k > 0 && *n + 1u < cap)
        buf[(*n)++] = rev[--k];
    buf[*n] = '\0';
}

static void put_str(char *buf, size_t cap, size_t *n, const char *s)
{
    while (*s != '\0' && *n + 1u < cap)
        buf[(*n)++] = *s++;
    buf[*n] = '\0';
}

/* Called from fault_common with the stack realigned. Never returns. */
__attribute__((noreturn)) void bios_fault(uint64_t vec, uint64_t err,
                                          uint64_t rip, uint64_t cr2);

__attribute__((noreturn)) void bios_fault(uint64_t vec, uint64_t err,
                                          uint64_t rip, uint64_t cr2)
{
    char line[160];
    size_t n = 0;

    line[0] = '\0';
    put_str(line, sizeof line, &n, "processor exception ");
    put_dec(line, sizeof line, &n, vec);
    if (vec < sizeof k_vectors / sizeof k_vectors[0]) {
        put_str(line, sizeof line, &n, " (");
        put_str(line, sizeof line, &n, k_vectors[vec]);
        put_str(line, sizeof line, &n, ")");
    }
    put_str(line, sizeof line, &n, "\n  rip 0x");
    put_hex(line, sizeof line, &n, rip);
    put_str(line, sizeof line, &n, ", error 0x");
    put_hex(line, sizeof line, &n, err);
    if (vec == 14u) {
        put_str(line, sizeof line, &n, ", address 0x");
        put_hex(line, sizeof line, &n, cr2);
    }
    bios_fatal(line);
}
