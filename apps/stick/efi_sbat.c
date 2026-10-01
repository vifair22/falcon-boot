/* falcon-boot: the .sbat section shim demands of its second stage.
 *
 * DESIGN Section 9: shim's
 * pe.c verify_sbat_section() returns EFI_SECURITY_VIOLATION when the image it
 * is about to start carries no .sbat section, because in_protocol is false on
 * shim's own second-stage load. So a correctly MOK-signed falcon-boot.efi is
 * still refused under Secure Boot if this section is missing, and the console
 * shows the same opaque refusal a bad signature gives. That is why this lives
 * in a file of its own with the reason written down rather than as one stray
 * array somewhere in main.c.
 *
 * Build mechanics, also measured: only a compiler-emitted section works here.
 * The mingw linker places an attribute-named section in sequence with .text
 * and .data, so the PE headers describe it correctly; a post-link
 * `objcopy --add-section` instead appends a section past SizeOfImage, which
 * shim does not find. Do not "simplify" this into an objcopy step.
 *
 * Images that our loader goes on to load through shim (the ISO payloads, and
 * later the blessed kernels) are loaded in_protocol and are exempt from the
 * requirement; this section is about our own binary only.
 */

/* SBAT CSV per shim's SBAT.md: the mandatory sbat,1 version row first, then
 * one row per component, each
 *   name,generation,vendor,package,version,url
 *
 * Field 2 is the generation, and it is the revocation lever: bump it only
 * when a released falcon-boot has a security defect that must stop booting on
 * machines whose SbatLevel is updated, and never for an ordinary release.
 * Field 5 is human-readable and carries no enforcement weight. It is
 * release_version (FALCON_SEMVER, generated into build/version.h), not the
 * full build string: that one has a timestamp in it and would make the
 * section non-reproducible.
 */
#include <version.h>

__attribute__((section(".sbat"), used))
static const char sbat_csv[] =
    "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
    "falcon-boot,1,falcon,falcon-boot," FALCON_SEMVER
    ",https://git.airies.net/vifair22/falcon-boot\n";
