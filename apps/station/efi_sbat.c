/* falcon-boot, the stationary build: its .sbat section.
 *
 * Everything apps/stick/efi_sbat.c says about why this section exists
 * and how it has to be emitted holds here unchanged; read it there.
 *
 * What differs is the component name, and that is the decision of
 * 2026-09-24 (DESIGN Section 12.1). A generation is revoked per component,
 * and the stick's loader carries parsers this build does not link (WIM, UDF,
 * the boot predictions' X.509). A defect in one of them bumps falcon-boot's
 * generation, and a stationary loader sharing that name would stop booting
 * the machine for code it never contained. Its own name keeps its own
 * generation.
 */
__attribute__((section(".sbat"), used))
static const char sbat_csv[] =
    "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
    "falcon-boot-pos,1,falcon,falcon-boot-pos,0.1.0,https://git.airies.net/vifair22/falcon-boot\n";
