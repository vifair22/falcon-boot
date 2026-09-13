#!/usr/bin/env bash
# Build the Secure Boot test rig's key material: a throwaway MOK keypair and a
# copy of the OVMF Secure Boot varstore that already trusts it.
#
# THIS IS RIG SCAFFOLDING, NOT THE PRODUCT'S KEY MODEL. DESIGN Section 7.5 is
# the real one: the MOK private key lives encrypted on p3, never travels in the
# clear, and is only ever used by Linux userland (the host tool or the on-stick
# signing appliance). The loader neither signs nor holds a key, here or there.
# Everything this script writes lands under build/ and is disposable; delete
# build/mok-test to start over with a new key.
#
# MokManager enrollment is interactive by design, which would make a scripted
# SB-ON test impossible, so the MokList variable is written straight into a
# copy of the varstore with virt-fw-vars. That shortcut skips the enrollment
# UI only: shim still reads MokList and still performs the verification
# itself, so what the rig then measures is the real MOK load path. Testing the
# enrollment UX is a separate, interactive job (DESIGN Section 11 step 10).
#
# Deps: openssl, and python3 virt-firmware in a venv this script creates
# (no system pip). Idempotent: existing artifacts are left alone.

set -euo pipefail

cd "$(dirname "$0")/.."

OUT=build/mok-test
# The venv is shared with the shim spike's, since it is the same one tool.
VENV=build/shim/venv
OVMF_DIR="${OVMF_DIR:-/usr/share/edk2/OvmfX64}"
VARS_SRC="$OVMF_DIR/OVMF_VARS.secboot.fd"

KEY=$OUT/airies-test-mok.key
CRT=$OUT/airies-test-mok.crt
CER=$OUT/airies-test-mok.cer
VARS=$OUT/OVMF_VARS.secboot.mok.fd
# The permissive-mode varstore (DESIGN Section 7.4): the same enrolled MOK,
# plus the MokSBState byte MokManager would have written after the user ran
# `mokutil --disable-validation`. Used with MOK_VARS= to prove the loader's
# banner, notice and "everything boots" behaviour in that state.
VARS_SB=$OUT/OVMF_VARS.secboot.moksb.fd
MOKSB_JSON=$OUT/moksbstate.json

# The owner GUID on a MokList entry is informational; shim's own vendor GUID
# is what MokManager itself would stamp there.
MOK_OWNER=605dab50-e046-4300-abb6-3dd810dd8b23
# The vendor GUID every shim-owned EFI variable lives under. Numerically the
# same value as MOK_OWNER above and named apart on purpose: that one is a
# signature's owner field, this one is a variable namespace. Measured: neither
# MokSBState nor MokSBStateRT exists under the MOK owner GUID that stamps
# MokList signatures (c451ed2b-9694-45d3-baba-ed9f8988a389).
SHIM_LOCK_GUID=605dab50-e046-4300-abb6-3dd810dd8b23

mkdir -p "$OUT"

if [[ ! -f $KEY ]]; then
    echo "== generating throwaway test MOK keypair (build/ only, never shipped)"
    openssl req -new -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 \
        -subj "/CN=airies-boot TEST MOK (throwaway)/" \
        -keyout "$KEY" -out "$CRT" 2>/dev/null
    chmod 600 "$KEY"
    # DER form, which is what MokManager wants when the enrollment UX is
    # driven by hand instead of by virt-fw-vars.
    openssl x509 -in "$CRT" -outform DER -out "$CER"
fi

if [[ ! -x $VENV/bin/virt-fw-vars ]]; then
    echo "== creating venv for virt-firmware"
    python3 -m venv "$VENV"
    "$VENV/bin/pip" -q install virt-firmware
fi

# Rebuilt from the pristine varstore every time, so a re-run cannot stack
# duplicate MokList entries or inherit state an earlier boot wrote.
echo "== seeding a varstore copy with the test MOK"
cp -f "$VARS_SRC" "$VARS"
chmod u+w "$VARS"
"$VENV/bin/virt-fw-vars" --inplace "$VARS" --add-mok "$MOK_OWNER" "$CRT"

# Second varstore: same MokList, plus MokSBState. This is the same shortcut
# the MokList injection above takes, for the same reason (MokManager's
# confirmation screen is interactive by design and cannot be scripted), and it
# skips exactly as little: shim still reads the variable and still makes its
# own decision about it, which is what the rig then measures.
#
# The attribute mask is load-bearing, and getting it wrong fails silently.
# MokSBState must be NON_VOLATILE|BOOTSERVICE_ACCESS (0x3) and must NOT carry
# RUNTIME_ACCESS: shim deletes and ignores any MokSBState that the running OS
# could have written, so a 0x7 mask produces a varstore that looks permissive
# to a human reading it and boots fully enforcing. Measured both ways under
# OVMF secboot with Fedora shim 16.1 on 2026-09-13; 0x3 yields MokSBStateRT
# (0x6, volatile) beside it and unsigned images load, 0x7 yields neither
# variable and unsigned images are refused.
echo "== seeding a second varstore with MokSBState (validation disabled)"
cat > "$MOKSB_JSON" <<EOF
{
  "version": 2,
  "variables": [
    {
      "name": "MokSBState",
      "guid": "$SHIM_LOCK_GUID",
      "attr": 3,
      "data": "01"
    }
  ]
}
EOF
cp -f "$VARS" "$VARS_SB"
chmod u+w "$VARS_SB"
"$VENV/bin/virt-fw-vars" --inplace "$VARS_SB" --set-json "$MOKSB_JSON"

echo "== ready:"
ls -la "$OUT"
echo
echo "enforcing rig:  tools/run-qemu.sh --secboot"
echo "permissive rig: MOK_VARS=$VARS_SB VARS_FILE=build/OVMF_VARS.secboot.moksb.fd \\"
echo "                tools/run-qemu.sh --secboot"
