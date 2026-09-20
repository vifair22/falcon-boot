# falcon-boot: the Secure Boot states the coverage harness boots under,
# sourced by tools/coverage/run-case.sh and tools/coverage/stage-loader.sh.
#
# The scorecard columns of docs/notes/coverage-roadmap.md Section 2:
#
#   off  plain OVMF, blank varstore. Nothing verifies anything.
#   ms   Secure Boot OVMF, Microsoft's KEK and both UEFI CAs in db, the test
#        MOK in MokList (falcon-efi/tools/mk-test-mok.sh). A retail laptop
#        with the stick's MOK enrolled.
#   own  Secure Boot OVMF whose PK, KEK and db hold ONE certificate the rig
#        generated, and nothing from Microsoft. The machine that started all
#        this (DESIGN Section 7.8's Dell): its owner's keys only. Shim boots
#        there because the rig adds a second signature to it with the db key,
#        which is what sbctl did to the real stick's shim. The test MOK is in
#        MokList, as it would be after enrollment.
#
# Those three boot the same stick with the same shim: a dual-signed one, which
# the ms firmware accepts on Microsoft's signature and the own firmware on the
# rig's. An extra signature changes nothing the Authenticode hash covers.
#
#   bios SeaBIOS, and not a Secure Boot state at all: there is no pflash, no
#        varstore, no db and no shim (docs/notes/bios-route.md Section 3). It
#        is a fourth column because a BIOS machine shares nothing with the
#        other three, which Section 7.4 of that note says is the cost of a
#        second platform.
#
#        The stick is the second drive, not the first. A case stick has no
#        BIOS boot partition, because whether a falcon stick should have one
#        is the layout decision Section 7.3 is still waiting on; the rig
#        boots falcon-bios from build/test-bios.img instead and reads the
#        stick from it. That measures every route without pre-empting the
#        decision.

. falcon-efi/tools/ovmf.sh

STATES_DIR=build/coverage/states
OWN_DIR=$STATES_DIR/own
OWN_KEY=$OWN_DIR/own-db.key
OWN_CRT=$OWN_DIR/own-db.crt
OWN_VARS=$OWN_DIR/OVMF_VARS.own.fd
MS_VARS=build/mok-test/OVMF_VARS.secboot.mok.fd
MOK_CRT_STATES=build/mok-test/falcon-test-mok.crt
SHIM_SRC=build/shim/bins/fedora-16.1-shimx64.efi
SHIM_DUAL=$STATES_DIR/shimx64-dual.efi
MOK_OWNER_GUID=605dab50-e046-4300-abb6-3dd810dd8b23
OWN_OWNER_GUID=4f5d5a1e-7b1c-4c35-9a6e-6f616c636f6e

# Appends the WIN_CERTIFICATE entries of SIGNED to the certificate table of
# BASE, writing OUT. Both must be the same image apart from their signatures,
# which is checked: the Authenticode hash excludes the certificate table, so
# the entries of one are valid signatures over the other.
pe_merge_sigs() {   # pe_merge_sigs BASE SIGNED OUT
    python3 - "$1" "$2" "$3" <<'PY'
import struct, sys

def table(d):
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    assert d[pe:pe+4] == b'PE\0\0', 'not a PE'
    opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]
    assert magic == 0x20b, 'not PE32+'
    dd = opt + 112 + 4 * 8           # data directory 4, the certificate table
    off, size = struct.unpack_from('<II', d, dd)
    return dd, off, size

base = bytearray(open(sys.argv[1], 'rb').read())
signed = open(sys.argv[2], 'rb').read()
bdd, boff, bsize = table(base)
sdd, soff, ssize = table(signed)
assert boff and soff, 'both images must already carry a signature'
# Same image apart from signatures: identical up to each table's start, with
# only the checksum and directory entry allowed to differ.
pe = struct.unpack_from('<I', base, 0x3c)[0]
cks = pe + 24 + 64
def mask(d, dd, off):
    d = bytearray(d[:off])
    d[cks:cks+4] = b'\0' * 4
    d[dd:dd+8] = b'\0' * 8
    return bytes(d)
assert mask(base, bdd, boff) == mask(signed, sdd, soff), 'images differ'
extra = signed[soff:soff+ssize]
body = bytes(base[:boff + bsize])
pad = (-len(body)) % 8
out = bytearray(body + b'\0' * pad + extra)
struct.pack_into('<II', out, bdd, boff, bsize + pad + len(extra))
open(sys.argv[3], 'wb').write(out)
PY
}

# Builds the own-keys material once: the db key, the varstore that trusts only
# it (and the test MOK through shim), and the dual-signed shim.
states_prepare() {
    mkdir -p "$OWN_DIR"
    if [[ ! -f "$OWN_KEY" ]]; then
        openssl req -new -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 \
            -subj "/CN=falcon-boot TEST owner db key (throwaway)/" \
            -keyout "$OWN_KEY" -out "$OWN_CRT" 2>/dev/null
        chmod 600 "$OWN_KEY"
    fi
    if [[ ! -f "$OWN_VARS" || "$OWN_CRT" -nt "$OWN_VARS" ]]; then
        local blank
        blank=$(ovmf_find VARS plain)
        cp -f "$blank" "$OWN_VARS"
        chmod u+w "$OWN_VARS"
        # Errors are NOT swallowed here, and the result is checked.
        #
        # This started as `>/dev/null 2>&1`, which is a dangerous place for it:
        # the varstore is copied from a blank one first, so any failure leaves
        # a SETUP MODE varstore in place, and setup mode enforces nothing.
        # Every case in the `own` column would then pass and the column would
        # measure air. Found 2026-09-19 porting the harness to a machine whose
        # virt-firmware was old enough to reject --secure-boot.
        if ! virt-fw-vars --inplace "$OWN_VARS" --no-microsoft \
            --set-pk "$OWN_OWNER_GUID" "$OWN_CRT" \
            --add-kek "$OWN_OWNER_GUID" "$OWN_CRT" \
            --add-db "$OWN_OWNER_GUID" "$OWN_CRT" \
            --add-mok "$MOK_OWNER_GUID" "$MOK_CRT_STATES" \
            --secure-boot >/dev/null; then
            echo "ERROR: virt-fw-vars could not build the own-keys varstore" >&2
            rm -f "$OWN_VARS"
            return 1
        fi
        if ! ovmf_vars_have_pk virt-fw-vars "$OWN_VARS"; then
            echo "ERROR: $OWN_VARS has no PK; it would boot in setup mode" >&2
            rm -f "$OWN_VARS"
            return 1
        fi
    fi
    if [[ ! -f "$SHIM_DUAL" || "$SHIM_SRC" -nt "$SHIM_DUAL" ||
          "$OWN_CRT" -nt "$SHIM_DUAL" ]]; then
        local tmp=$STATES_DIR/shim-own-only.efi
        sbsign --key "$OWN_KEY" --cert "$OWN_CRT" --output "$tmp" "$SHIM_SRC" \
            >/dev/null 2>&1
        pe_merge_sigs "$SHIM_SRC" "$tmp" "$SHIM_DUAL"
        rm -f "$tmp"
    fi
}

# Sets STATE_CODE and STATE_SECURE (1 when the firmware needs SMM and a
# secure flash), STATE_BIOS (1 for the SeaBIOS column, which has neither),
# and copies the state's varstore to DEST, which the run may then write to
# freely.
state_firmware() {   # state_firmware STATE DEST
    STATE_BIOS=0
    case "$1" in
    bios)
        STATE_CODE=
        STATE_SECURE=0
        STATE_BIOS=1
        [[ -f build/test-bios.img ]] || {
            echo "ERROR: build/test-bios.img missing (run make test-bios-img)" >&2
            return 1; }
        return 0
        ;;
    off)
        STATE_CODE=$(ovmf_find CODE plain)
        STATE_SECURE=0
        cp -f "$(ovmf_find VARS plain)" "$2"
        ;;
    ms)
        STATE_CODE=$(ovmf_find CODE secboot)
        STATE_SECURE=1
        [[ -f "$MS_VARS" ]] || {
            echo "ERROR: $MS_VARS missing (run falcon-efi/tools/mk-test-mok.sh)" >&2
            return 1; }
        cp -f "$MS_VARS" "$2"
        ;;
    own)
        STATE_CODE=$(ovmf_find CODE secboot)
        STATE_SECURE=1
        states_prepare
        cp -f "$OWN_VARS" "$2"
        ;;
    *)
        echo "ERROR: unknown state $1 (off, ms, own, bios)" >&2
        return 1
        ;;
    esac
    chmod u+w "$2"
}
