#!/usr/bin/env bash
# Build the third Secure Boot test case: shim validation DISABLED.
#
# mok-setup.sh builds the enforcing case (Secure Boot on, a MOK enrolled).
# This one starts from that varstore and adds the MokSBState byte MokManager
# would have written after `mokutil --disable-validation`, which is the state
# DESIGN Section 7.4 calls the platform opt-out. Same shortcut as the MokList
# injection, for the same reason: MokManager's confirmation screen is
# interactive by design. shim still reads the variable and still makes its own
# decision about it, which is exactly what the probe then measures.
#
# The attribute mask is load-bearing and fails silently when wrong. MokSBState
# must be NON_VOLATILE|BOOTSERVICE_ACCESS (0x3) and must NOT carry
# RUNTIME_ACCESS: shim deletes and ignores any copy the running OS could have
# written, so 0x7 yields a varstore that reads as permissive to a human and
# boots fully enforcing. Same finding as tools/mk-test-mok.sh, restated here
# because the two rigs are independent.
#
# Usage: moksb-setup.sh   (run mok-setup.sh first), then:
#   TAG=-moksb PROBE_BIN=build/shim/probe-mok.efi \
#     VARS_TEMPLATE=build/shim/vars-template-moksb.fd \
#     tools/shim-spike/run-probe.sh fedora-16.1 secboot

set -euo pipefail

cd "$(dirname "$0")/../.."

OUT=build/shim
VENV=$OUT/venv
SRC=$OUT/vars-template-mok.fd
DST=$OUT/vars-template-moksb.fd
JSON=$OUT/moksbstate.json

# The vendor GUID every shim-owned EFI variable lives under.
SHIM_LOCK_GUID=605dab50-e046-4300-abb6-3dd810dd8b23

[[ -f $SRC ]] || { echo "ERROR: run mok-setup.sh first ($SRC missing)" >&2; exit 1; }
[[ -x $VENV/bin/virt-fw-vars ]] || { echo "ERROR: $VENV/bin/virt-fw-vars missing" >&2; exit 1; }

cat > "$JSON" <<EOF
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

echo "== seeding a validation-disabled varstore -> $DST"
cp -f "$SRC" "$DST"
chmod u+w "$DST"
"$VENV/bin/virt-fw-vars" --inplace "$DST" --set-json "$JSON"
echo "== ready"
