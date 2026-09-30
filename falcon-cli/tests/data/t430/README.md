# The T430's two TPM 1.2 event logs

From one boot of the ThinkPad T430 (coreboot with an edk2 payload patched to
measure the images it starts, Secure Boot on with the machine's own key),
2026-09-29, recorded in the thinkpad-t430 repository's `firmware/tpm/`:

- `coreboot-evlog.bin`: coreboot's log (PCR 2), as Linux reads it from
  `/sys/kernel/security/tpm0/binary_bios_measurements`.
- `edk2-evlog.bin`: edk2's log, saved through `EFI_TCG_PROTOCOL` by the TPM
  rig's probe (`falcon-efi/tools/tpm-probe/`) from the UEFI Shell.

The probe read PCRs 0-15 in the same boot; `totpcore_test.c` carries them.
Replaying coreboot's log and then edk2's gives those values, which is what
`totpc_merge12` is tested against.
