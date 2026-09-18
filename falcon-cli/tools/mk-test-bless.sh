#!/usr/bin/env bash
# falcon-boot: bless a real ISO with the test MOK and verify the result
# (docs/DESIGN.md Sections 7.5, 8 and 8.1).
#
# Builds a scratch "p2" under build/ and runs falcon-boot-sign against a real
# distro ISO, then checks the four things the bless is supposed to produce:
#
#   1. a signed UKI under <stick>/falcon/blessed/ that sbverify accepts
#      against the test MOK certificate
#   2. a .cmdline section inside that UKI carrying the rendered family
#      template, with the ISO's path on p2 substituted in
#   3. an .sbat section inherited from sd-stub, without which shim refuses
#      the image under Secure Boot (Section 8.1, the forced reason the bless
#      output is a UKI wrap and not a bare signed bzImage)
#   4. a manifest record whose sha256 agrees with an independent hash of the
#      same file, computed by python3 hashlib rather than by our own code
#
# Then it re-blesses the same ISO and asserts the manifest record count did
# not change, which is the property that keeps a stick from accumulating a
# record per re-bless.
#
# THIS IS RIG SCAFFOLDING. It signs with the throwaway MOK from
# falcon-efi/tools/mk-test-mok.sh, whose private key sits in the clear under
# build/ and is not the product's key model: DESIGN Section 7.5 keeps the real
# private key encrypted on p3 and used only by an authenticated Linux userland.
# Everything here lands under build/ and the source ISO is READ ONLY.
#
# Usage: falcon-cli/tools/mk-test-bless.sh [ISO]
# Env:   ARCH_ISO   source ISO (default: the vtoy-backup copy)
#        SIGN       the bless tool (default: build/bin/falcon-boot-sign)
#        STICK      scratch p2 (default: build/e2e-stick)

set -euo pipefail

cd "$(dirname "$0")/../.."

ARCH_ISO="${1:-${ARCH_ISO:-/mnt/980pro/vtoy-backup/Linux/archlinux-2026.01.01-x86_64.iso}}"
SIGN="${SIGN:-build/bin/falcon-boot-sign}"
STICK="${STICK:-build/e2e-stick}"

MOK_KEY=build/mok-test/falcon-test-mok.key
MOK_CRT=build/mok-test/falcon-test-mok.crt

for t in bsdtar sbsign sbverify objcopy python3; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ERROR: $t not found" >&2
        exit 1
    }
done
[[ -x "$SIGN" ]] || {
    echo "ERROR: $SIGN not built (see the cli target in the Makefile)" >&2
    exit 1
}
[[ -f "$ARCH_ISO" ]] || {
    echo "ERROR: $ARCH_ISO not found (pass one as an argument)" >&2
    exit 1
}
[[ -f "$MOK_KEY" && -f "$MOK_CRT" ]] || {
    echo "ERROR: no test MOK; run falcon-efi/tools/mk-test-mok.sh first" >&2
    exit 1
}

ISO_NAME="$(basename "$ARCH_ISO")"
# Copied rather than referenced in place: the tool derives the ISO's path on
# p2 from where the file sits under --stick, and that derivation is half of
# what is under test here. The source ISO is never written to.
mkdir -p "$STICK/Linux"
if [[ ! -f "$STICK/Linux/$ISO_NAME" ]]; then
    echo "== staging $ISO_NAME on the scratch p2 (this copies the ISO)"
    cp "$ARCH_ISO" "$STICK/Linux/$ISO_NAME"
fi

echo "== bless"
"$SIGN" "$STICK/Linux/$ISO_NAME" \
    --stick "$STICK" --key "$MOK_KEY" --cert "$MOK_CRT"

MANIFEST="$STICK/falcon/blessed/manifest"
UKI_NAME="$(sed -n 's/^uki=//p' "$MANIFEST" | tail -1)"
UKI="$STICK/falcon/blessed/$UKI_NAME"

echo
echo "== verify"
[[ -f "$UKI" ]] || { echo "FAIL: $UKI was not written" >&2; exit 1; }
echo "   uki present: $UKI ($(stat -c%s "$UKI") bytes)"

sbverify --cert "$MOK_CRT" "$UKI" >/dev/null
echo "   signature verifies against the test MOK"

CMDLINE="$(objcopy -O binary --only-section=.cmdline "$UKI" /dev/stdout |
    tr -d '\0')"
[[ -n "$CMDLINE" ]] || { echo "FAIL: no .cmdline section" >&2; exit 1; }
case "$CMDLINE" in
    *"/Linux/$ISO_NAME"*) ;;
    *)
        echo "FAIL: .cmdline does not name the ISO's path on p2" >&2
        echo "      got: $CMDLINE" >&2
        exit 1
        ;;
esac
echo "   cmdline: $CMDLINE"

objcopy -O binary --only-section=.sbat "$UKI" /dev/stdout | grep -q sbat ||
    { echo "FAIL: no .sbat section; shim would refuse this image" >&2; exit 1; }
echo "   .sbat inherited from sd-stub"

python3 - "$STICK/Linux/$ISO_NAME" "$MANIFEST" <<'EOF'
import hashlib
import re
import sys

iso, manifest = sys.argv[1], sys.argv[2]
h = hashlib.sha256()
with open(iso, "rb") as f:
    for block in iter(lambda: f.read(1 << 20), b""):
        h.update(block)

text = open(manifest).read()
recorded = re.findall(r"^sha256=(\S+)", text, re.M)
if h.hexdigest() not in recorded:
    sys.exit("FAIL: manifest sha256 does not match an independent hash\n"
             "      hashlib: %s\n      manifest: %s"
             % (h.hexdigest(), ", ".join(recorded)))
print("   sha256 agrees with python3 hashlib: %s" % h.hexdigest())
EOF

BEFORE="$(grep -c '^iso=' "$MANIFEST")"
echo
echo "== re-bless (must update in place, not duplicate)"
"$SIGN" "$STICK/Linux/$ISO_NAME" \
    --stick "$STICK" --key "$MOK_KEY" --cert "$MOK_CRT" --quiet >/dev/null
AFTER="$(grep -c '^iso=' "$MANIFEST")"
[[ "$BEFORE" == "$AFTER" ]] || {
    echo "FAIL: record count went from $BEFORE to $AFTER" >&2
    exit 1
}
echo "   record count unchanged: $AFTER"

echo
echo "OK. The blessed artifacts are under $STICK/falcon/blessed/."
