#!/usr/bin/env bash
# Build the Secure Boot ON test case: a MOK-signed probe plus an OVMF varstore
# with that MOK pre-enrolled.
#
# MokManager enrollment is interactive by design (physical presence), which
# would make the SB-ON half of this spike unrunnable in a script. Writing the
# MokList variable straight into the varstore image sidesteps only the UI, not
# the trust decision: shim still reads MokList and still does the verification
# itself, so what the probe observes afterwards is the real MOK load path.
#
# Nothing here belongs in the product. It is a test-rig shortcut, and the
# private key it generates is a throwaway that exists only under build/.
#
# Deps: openssl, sbsign, and python3-virt-firmware in a local venv (this
# script creates it; it is the only host tool the spike could not find
# preinstalled).
#
# Usage: mok-setup.sh   (then: TAG=-mok PROBE_BIN=build/shim/probe-mok.efi \
#                              VARS_TEMPLATE=build/shim/vars-template-mok.fd \
#                              falcon-efi/tools/shim-spike/run-probe.sh fedora-16.1 secboot)

set -euo pipefail

cd "$(dirname "$0")/../../.."

OUT=build/shim
KEYS=$OUT/keys
VENV=$OUT/venv
# shim's own vendor GUID; the owner GUID on a MokList entry is informational.
MOK_OWNER=605dab50-e046-4300-abb6-3dd810dd8b23

[[ -f $OUT/probe.efi ]] || { echo "ERROR: build the probe first" >&2; exit 1; }

if [[ ! -x $VENV/bin/virt-fw-vars ]]; then
    echo "== creating venv for virt-firmware"
    python3 -m venv "$VENV"
    "$VENV/bin/pip" -q install virt-firmware
fi

if [[ ! -f $KEYS/spike.key ]]; then
    echo "== generating throwaway spike MOK keypair"
    mkdir -p "$KEYS"
    openssl req -new -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 \
        -subj "/CN=falcon-boot shim spike MOK/" \
        -keyout "$KEYS/spike.key" -out "$KEYS/spike.crt" 2>/dev/null
    chmod 600 "$KEYS/spike.key"
fi

echo "== seeding varstore with MokList"
cp -f /usr/share/edk2/OvmfX64/OVMF_VARS.secboot.fd "$OUT/vars-template-mok.fd"
chmod u+w "$OUT/vars-template-mok.fd"
"$VENV/bin/virt-fw-vars" --inplace "$OUT/vars-template-mok.fd" \
    --add-mok "$MOK_OWNER" "$KEYS/spike.crt"

echo "== signing the probe with the spike MOK"
sbsign --key "$KEYS/spike.key" --cert "$KEYS/spike.crt" \
    --output "$OUT/probe-mok.efi" "$OUT/probe.efi"

# Three payloads for the probe's LoadImage test, differing in exactly one
# property each, so a refusal names its own cause:
#   pay-mok-sbat    MOK-signed, has .sbat   -> must load
#   pay-mok-nosbat  MOK-signed, no .sbat    -> tests whether SBAT is required
#                                              on the protocol/hook path
#   pay-unsigned    no signature at all     -> must be refused
echo "== building LoadImage test payloads"
cp -f "$OUT/probe-mok.efi" "$OUT/pay-mok-sbat.efi"
x86_64-w64-mingw32-objcopy --remove-section .sbat \
    "$OUT/probe.efi" "$OUT/pay-nosbat.efi"
sbsign --key "$KEYS/spike.key" --cert "$KEYS/spike.crt" \
    --output "$OUT/pay-mok-nosbat.efi" "$OUT/pay-nosbat.efi"
cp -f "$OUT/probe.efi" "$OUT/pay-unsigned.efi"

sbverify --list "$OUT/probe-mok.efi"
