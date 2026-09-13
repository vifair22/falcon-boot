/* falcon-boot mkstick: read a shim's compiled-in next-stage loader name out of
 * the shim binary itself (DESIGN Sections 4 and 8).
 *
 * Why this is not a constant. Shim launches one hard-coded filename as its
 * second stage; which filename that is, is a compile-time option of the shim
 * build, and our loader has to be installed under exactly that name or shim
 * finds nothing. The 2026-09-12 spike measured `\\grubx64.efi` on Fedora and
 * Debian 16.1 and `\\grub.efi` on openSUSE 16.1 (docs/notes/shim-spike.md
 * Section 2), which is precisely why DESIGN Section 4 says to read the name
 * from the binary rather than assume it: a future shim swap must be a data
 * change, not a code change nobody remembers to make.
 *
 * The technique, and why it is sound. Shim's Makefile passes the default as
 *   -DDEFAULT_LOADER='L"\\\\grubx64.efi"'
 * so the string that lands in the binary is UTF-16LE with TWO leading
 * backslashes. Nothing else in a shim looks like that: MokManager and the
 * fallback loader are stored with one (`\mmx64.efi`, `\fbx64.efi`), and the
 * only other path-shaped strings carry interior separators
 * (`\EFI\BOOT\fbx64.efi`). So "a UTF-16LE string that is exactly two
 * backslashes followed by one .efi basename" identifies the next-stage name
 * with no disassembly and no version knowledge.
 *
 * Measured against every binary in build/shim/bins: exactly one match in each
 * of the four shims (fedora 15.8 and 16.1, openSUSE 16.1, debian 16.1) and
 * zero in each MokManager. Ambiguity is therefore a real signal that the
 * assumption has expired, and it is reported as a hard failure with every
 * candidate listed rather than guessed past.
 */

#ifndef FALCON_MKSTICK_SHIMNAME_H
#define FALCON_MKSTICK_SHIMNAME_H

#include <stddef.h>

#include <cli/common/status.h>

#define SHIM_NEXT_STAGE_MAX 64u

/* Writes the bare filename (no leading backslash), e.g. "grubx64.efi". */
cli_status shim_next_stage_name(const char *shim_path, char *out, size_t cap);

/* The name shim launches MokManager under, read the same way. It is stored
 * with ONE backslash, so it cannot share the rule above; the two names shim
 * is known to use (`\mmx64.efi`, `\MokManager.efi`) are recognised by name
 * and whichever one this binary carries is returned. Falls back to
 * "mmx64.efi" with a warning when neither appears, because installing
 * MokManager under the wrong name costs only the enrollment UI, not the
 * boot. */
cli_status shim_mokmanager_name(const char *shim_path, char *out, size_t cap);

#endif /* FALCON_MKSTICK_SHIMNAME_H */
