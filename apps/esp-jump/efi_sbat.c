/* falcon-esp-jump: its .sbat section.
 *
 * Everything apps/stick/efi_sbat.c says about why this section exists and
 * how it has to be emitted holds here unchanged; read it there.
 *
 * The component name is its own, falcon-esp-jump, for the reason
 * apps/station/efi_sbat.c gives for falcon-boot-pos: a generation is revoked
 * per component, and this image carries none of the loaders' parsers. It is
 * also how mkstick tells this image from someone else's BOOTX64.EFI
 * (stn_is_esp_jump), so the row's first field is not to be renamed.
 *
 * The version field is this image's own, not release_version. The jumper is
 * measured into PCR 4 ahead of the loader on every boot, and the TOTP seal
 * holds PCR 4 (DESIGN Section 12.15): an image that changes only when its
 * code does lets mkstick update reseal across a release. Bump it with the
 * code.
 */

__attribute__((section(".sbat"), used))
static const char sbat_csv[] =
    "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
    "falcon-esp-jump,1,falcon,falcon-esp-jump,1"
    ",https://git.airies.net/vifair22/falcon-boot\n";
