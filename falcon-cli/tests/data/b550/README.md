# An AMD fTPM whose event log does not account for its PCRs

From one boot of the desktop (ASUS ROG STRIX B550-F GAMING WIFI II, BIOS
3636, Ryzen 9 5900XT's firmware TPM 2.0, Secure Boot on with the machine's
own keys), 2026-10-07, started by the UEFI station through shim:

- `evlog.bin`: the firmware's log, as Linux reads it from
  `/sys/kernel/security/tpm0/binary_bios_measurements`. SHA-256 only.

`totpcore_test.c` carries PCRs 0-15 as the TPM held them in the same boot.
The log replayed to its end gives none of PCRs 0-7: the firmware extends
them with measurements it does not log. PCRs 3 and 6 show it plainly: the
log holds one separator for each, which gives `3d458cfe...`, and the TPM
holds `8d22c738...`. PCR 4's applications are shim, the station
(`3d8e399c...`) and the UKI.
