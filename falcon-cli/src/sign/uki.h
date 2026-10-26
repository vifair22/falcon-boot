/* falcon-boot-sign: build a UKI from sd-stub, a kernel and an initrd.
 *
 * What ukify's `build` did for this tool, and nothing it did beyond that: the
 * stub is copied, the payload sections are appended after its own, its .sbat
 * is rewritten in place with the merged generations, and the optional-header
 * fields that describe the image are brought up to date. No signing happens
 * here; the result goes to sbsign exactly as ukify's output did.
 *
 * Why this is in-repo and not a helper (DESIGN Section 8, the vendoring line):
 * ukify is a Python program, and the appliance runs it AFTER p3 is unlocked.
 * Carrying it meant carrying an interpreter and its standard library into the
 * one window where the private key is readable, for a job that is a few
 * hundred lines of section-table arithmetic. The output is byte-identical to
 * ukify 261's for the same inputs (falcon-cli/tools/cmp-ukify.sh), which is
 * how this was checked and is the check to repeat when either side moves.
 *
 * The layout rules, all ukify's (pe_add_sections in ukify 261):
 *
 *   - each new section starts at the next SectionAlignment boundary after the
 *     last one in memory, and at the next FileAlignment boundary after the end
 *     of the file on disk; raw data is zero-padded to FileAlignment
 *   - .linux is CNT_CODE, everything else CNT_INITIALIZED_DATA, all MEM_READ
 *   - .sbat is not appended: the stub's own is rewritten in place and must be
 *     large enough to take the merged text
 *   - if the kernel is not NX_COMPAT, neither is the UKI
 *   - CheckSum is zeroed, SizeOfImage and SizeOfInitializedData recomputed
 *   - the cmdline's whitespace is collapsed (ukify's main, not its layout)
 *
 * Stubs that ukify had to repair (a symbol table left in, unaligned raw data)
 * are refused rather than repaired. Every systemd-stub in circulation that
 * this tool will meet is stripped and aligned, and a repair path nobody
 * exercises is a repair path nobody has tested.
 */

#ifndef FALCON_CLI_SIGN_UKI_H
#define FALCON_CLI_SIGN_UKI_H

#include <stddef.h>
#include <stdint.h>

/* The .osrel every blessed wrap carries. It names what the image IS, which
 * does not depend on where it was built: ukify's default was the build host's
 * own os-release, so the same ISO blessed on a host and in the appliance
 * carried two different names. */
#define UKI_BLESSED_OSREL \
    "NAME=\"falcon-boot blessed media\"\n" \
    "ID=falcon-boot\n" \
    "PRETTY_NAME=\"falcon-boot blessed media\"\n"

/* The generation line ukify adds for the UKI as a whole (its STUB_SBAT). */
#define UKI_SBAT_UKI_LINE \
    "uki,1,UKI,uki,1,https://uapi-group.org/specifications/specs/unified_kernel_image/"

#define UKI_UNAME_MAX 128u

/* Where systemd-boot installs its stubs, one per architecture. With no
 * --stub, the kernel's own PE machine picks one (uki_default_stub). */
#define UKI_STUB_DIR "/usr/lib/systemd/boot/efi"
#define UKI_STUB_X64 UKI_STUB_DIR "/linuxx64.efi.stub"
#define UKI_STUB_AA64 UKI_STUB_DIR "/linuxaa64.efi.stub"

/* What to build. Paths for the two large inputs, which are streamed and never
 * held in memory whole (an embedded initrd is 100-250 MB); text for the rest. */
typedef struct {
    const char *stub;       /* NULL: the stub for the kernel's machine */
    const char *kernel;
    const char *initrd;     /* NULL: no .initrd section */
    const char *cmdline;
    const char *osrel;      /* .osrel text */
    const char *out;
    /* Take a bzImage with no EFI stub (its setup header, "HdrS" at 0x202,
     * and no PE header): a UKI that sd-stub cannot start, for falcon-bios,
     * which unwraps .linux and never runs the stub. DevLink recovery's
     * 32-bit kernel in station-legacy's /uki (DESIGN Section 12.10). */
    int         bios_only;
} uki_spec;

/* Builds spec->out. SGN_OK or a negative SGN_ERR_* with the error buffer set.
 * The stub and the kernel must be one machine's: an x64 stub cannot start an
 * arm64 kernel. */
int uki_build(const uki_spec *spec);

/* The stub systemd-boot installs for a PE machine, NULL for one it has
 * none for. */
const char *uki_default_stub(uint16_t machine);

/* The PE machine of the kernel at path, 0 when it is not a PE (a bzImage
 * with no EFI stub, or a kernel still compressed as a whole). */
uint16_t uki_kernel_machine(const char *path);

/* A kernel that is one gzip member, as Debian's and Ubuntu's arm64 vmlinuz
 * is, unpacked over itself; any other file is left as it is. For a staged
 * copy only: the file is rewritten. */
int uki_kernel_unpack(const char *path);

/* `falcon-boot-sign uki ...`: the same build from the command line, for the
 * images this tool does not bless (the appliance). argv[0] is "uki". */
int sgn_uki_main(int argc, char **argv);

/* ---- the pure parts, exposed for the tests ---- */

/* ukify's treatment of a cmdline given as a string: runs of whitespace become
 * one space and both ends are trimmed. The kernel reads the result the same
 * way, and every UKI this tool built through ukify carried it, so it is kept.
 * `out` must hold strlen(in) + 1 bytes. */
void uki_cmdline_norm(const char *in, char *out);

/* The kernel version from an x86 bzImage's real-mode header, as ukify's
 * scrape_x86 reads it: the first word of the version string, and only when
 * the string has the "version (builder) #..." shape. 1 when found, 0 when the
 * image carries none (the UKI then has no .uname, as ukify's does not). */
int uki_uname(const uint8_t *k, size_t len, char *out, size_t cap);

/* The same from any other kernel, as ukify's scrape_generic reads it: the
 * first "Linux version <digit><non-space>* (" in its bytes. An arm64 Image
 * carries the banner in the clear. 1 when found, 0 when not. */
int uki_uname_generic(const uint8_t *k, size_t len, char *out, size_t cap);

/* ukify's merge_sbat: the "sbat," header line once, then every input's lines
 * after its own header, in order. An input whose first line is not a header is
 * skipped whole. Trailing NULs (section padding) are ignored. SGN_ERR_NOSPACE
 * when the result does not fit `cap`. */
int uki_sbat_merge(const char *const *in, const size_t *in_len, size_t n,
                   char *out, size_t cap, size_t *out_len);

/* One section to append. `name` and `len` are the caller's; `raw_ptr` and
 * `raw_size` are filled in by uki_layout and say where the bytes go. */
typedef struct {
    const char *name;
    uint64_t    len;
    int         code;
    uint32_t    raw_ptr;
    uint32_t    raw_size;
} uki_part;

/* Rewrites a copy of the stub in place: section headers for `parts`, the
 * merged .sbat, and the header fields. `img` holds the whole stub; the parts'
 * bytes are not needed, only their lengths. *out_len is the length of the
 * finished file. SGN_ERR_FORMAT for a stub this refuses, with the reason. */
int uki_layout(uint8_t *img, size_t img_len, uki_part *parts, size_t n,
               const char *sbat, size_t sbat_len, int clear_nx,
               uint64_t *out_len);

#endif /* FALCON_CLI_SIGN_UKI_H */
