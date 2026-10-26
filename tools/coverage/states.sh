# shellcheck shell=bash
# falcon-boot: the Secure Boot states the coverage harness boots under,
# sourced by tools/coverage/run-case.sh and tools/coverage/stage-loader.sh.
#
# The scorecard columns (tools/coverage/run-case.sh):
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
#   moksb the ms firmware with the ms varstore plus MokSBState, the byte
#        MokManager writes after `mokutil --disable-validation`
#        (falcon-efi/tools/mk-test-mok.sh writes it; the attribute mask is
#        load-bearing and the script's comment says why). DESIGN Section 7.2's
#        "Secure Boot on, validation off": shim installs its loader protocol
#        and permits everything, but does not hook the boot-services table,
#        so every ISO boots and no UKI does (Section 7.4). Run on request
#        (run-corpus.sh --states moksb); the headline columns are the three
#        above.
#
#   bios SeaBIOS, and not a Secure Boot state at all: there is no pflash, no
#        varstore, no db and no shim (DESIGN Section 2). It
#        is a fourth column because a BIOS machine shares nothing with the
#        other three; that is the cost of a second platform.
#
#        The case stick is the only drive, and boots itself: it carries
#        falcon-bios's stages where every stick keeps them (DESIGN Section
#        3.3), put on by stage-loader.sh, so the column measures a stick
#        booting on a BIOS machine and nothing else.
#
#   bios32 the bios column on an i686 (QEMU's pentium3 model): no long
#        mode, no SSE2, no AES-NI. The floor falcon-bios runs on, and the
#        column the 32-bit ISOs are judged in. A 64-bit ISO there fails
#        by design, with the loader saying why.
#
#   a64-off, a64-ms, a64-own
#        off, ms and own on an AArch64 machine: QEMU's virt under AAVMF,
#        emulated (TCG), the stick on xhci as USB storage (WinPE has no
#        virtio driver; a stick is USB anyway). a64-ms enrolls Microsoft's
#        2023 UEFI CA, the one shimaa64 is signed by
#        (falcon-efi/tools/mk-test-mok.sh); a64-own trusts the rig's db key
#        and a shimaa64 that carries its signature too. The arm64 ISOs run
#        in these columns only.

. falcon-efi/tools/ovmf.sh

STATES_DIR=build/coverage/states
OWN_DIR=$STATES_DIR/own
OWN_KEY=$OWN_DIR/own-db.key
OWN_CRT=$OWN_DIR/own-db.crt
OWN_VARS=$OWN_DIR/OVMF_VARS.own.fd
MS_VARS=build/mok-test/OVMF_VARS.secboot.mok.fd
MOKSB_VARS=build/mok-test/OVMF_VARS.secboot.moksb.fd
MOK_CRT_STATES=build/mok-test/falcon-test-mok.crt
SHIM_SRC=build/shim/bins/fedora-16.1-shimx64.efi
SHIM_DUAL=$STATES_DIR/shimx64-dual.efi
A64_MS_VARS=build/mok-test/AAVMF_VARS.secboot.mok.fd
A64_OWN_VARS=$OWN_DIR/AAVMF_VARS.own.fd
SHIM_SRC_AA64=build/shim/bins/fedora-16.1-shimaa64.efi
SHIM_DUAL_AA64=$STATES_DIR/shimaa64-dual.efi
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
        # measure air. A virt-firmware old enough to reject --secure-boot
        # fails exactly this way.
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
    states_dual "$SHIM_SRC" "$SHIM_DUAL"
    if [[ -f "$SHIM_SRC_AA64" ]]; then
        states_dual "$SHIM_SRC_AA64" "$SHIM_DUAL_AA64"
        states_own_vars "$(aavmf_find VARS plain)" "$A64_OWN_VARS"
    fi
}

# The shim SRC with the rig's db signature added beside Microsoft's, as OUT.
states_dual() {   # states_dual SRC OUT
    if [[ ! -f "$2" || "$1" -nt "$2" || "$OWN_CRT" -nt "$2" ]]; then
        local tmp=$STATES_DIR/shim-own-only.efi
        sbsign --key "$OWN_KEY" --cert "$OWN_CRT" --output "$tmp" "$1" \
            >/dev/null 2>&1
        pe_merge_sigs "$1" "$tmp" "$2"
        rm -f "$tmp"
    fi
}

# The own-keys varstore for AArch64, from AAVMF's blank one; the same
# enrollment and the same check as OVMF's above.
states_own_vars() {   # states_own_vars BLANK OUT
    [[ -f "$2" && ! "$OWN_CRT" -nt "$2" ]] && return 0
    cp -f "$1" "$2"
    chmod u+w "$2"
    if ! virt-fw-vars --inplace "$2" --no-microsoft \
        --set-pk "$OWN_OWNER_GUID" "$OWN_CRT" \
        --add-kek "$OWN_OWNER_GUID" "$OWN_CRT" \
        --add-db "$OWN_OWNER_GUID" "$OWN_CRT" \
        --add-mok "$MOK_OWNER_GUID" "$MOK_CRT_STATES" \
        --secure-boot >/dev/null ||
       ! ovmf_vars_have_pk virt-fw-vars "$2"; then
        echo "ERROR: virt-fw-vars could not build $2" >&2
        rm -f "$2"
        return 1
    fi
}

# Sets STATE_CODE and STATE_SECURE (1 when the firmware needs SMM and a
# secure flash), STATE_BIOS (1 for the SeaBIOS columns, which have neither),
# STATE_CPU (the QEMU CPU model), STATE_ARCH (x64 or aa64), and copies the
# state's varstore to DEST, which the run may then write to freely.
state_firmware() {   # state_firmware STATE DEST
    STATE_BIOS=0
    STATE_CPU=host
    STATE_ARCH=x64
    case "$1" in
    a64-off|a64-ms|a64-own)
        # AAVMF's Secure Boot needs no SMM: its store is not behind one.
        STATE_ARCH=aa64
        STATE_CPU=max
        STATE_SECURE=0
        case "$1" in
        a64-off)
            STATE_CODE=$(aavmf_find CODE plain)
            cp -f "$(aavmf_find VARS plain)" "$2"
            ;;
        a64-ms)
            STATE_CODE=$(aavmf_find CODE secboot || aavmf_find CODE plain)
            [[ -f "$A64_MS_VARS" ]] || {
                echo "ERROR: $A64_MS_VARS missing (run falcon-efi/tools/mk-test-mok.sh)" >&2
                return 1; }
            cp -f "$A64_MS_VARS" "$2"
            ;;
        a64-own)
            STATE_CODE=$(aavmf_find CODE secboot || aavmf_find CODE plain)
            states_prepare
            cp -f "$A64_OWN_VARS" "$2"
            ;;
        esac
        ;;
    bios|bios32)
        STATE_CODE=
        STATE_SECURE=0
        STATE_BIOS=1
        [[ "$1" == bios32 ]] && STATE_CPU=pentium3
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
    moksb)
        STATE_CODE=$(ovmf_find CODE secboot)
        STATE_SECURE=1
        [[ -f "$MOKSB_VARS" ]] || {
            echo "ERROR: $MOKSB_VARS missing (run falcon-efi/tools/mk-test-mok.sh)" >&2
            return 1; }
        cp -f "$MOKSB_VARS" "$2"
        ;;
    *)
        echo "ERROR: unknown state $1 (off, ms, own, moksb, bios, bios32," \
             "a64-off, a64-ms, a64-own)" >&2
        return 1
        ;;
    esac
    chmod u+w "$2"
}
