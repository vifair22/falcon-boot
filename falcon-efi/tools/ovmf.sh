# shellcheck shell=bash
# Shared OVMF discovery, sourced by the rig scripts.
#
# Distros disagree about both the directory and the file names, and this stick
# roams between an Arch laptop and a Gentoo workstation, so one hardcoded path
# is wrong on one of them by construction.
#
#   Gentoo   /usr/share/edk2/OvmfX64/  OVMF_CODE.fd, OVMF_VARS.secboot.fd
#   Arch     /usr/share/edk2/x64/      OVMF_CODE.4m.fd, OVMF_CODE.secboot.4m.fd
#   Debian   /usr/share/OVMF/          OVMF_CODE_4M.fd
#
# OVMF_DIR still wins when set, so an unusual install needs no code change.
#
# The varstore difference that matters: Gentoo ships OVMF_VARS.secboot.fd with
# Microsoft's PK/KEK/db already enrolled, Arch ships only a BLANK varstore that
# comes up in setup mode. A blank one is not a Secure Boot test - shim would
# find nothing enforcing - so callers must enroll into it; ovmf_vars_have_pk
# is how they find out whether they need to.

ovmf_find() {   # ovmf_find <CODE|VARS> <plain|secboot> -> path, or empty
    local what=$1 mode=$2 d f
    local dirs=("${OVMF_DIR:-}" /usr/share/edk2/OvmfX64 /usr/share/edk2/x64 \
                /usr/share/edk2-ovmf /usr/share/edk2-ovmf/x64 /usr/share/OVMF \
                /usr/share/edk2)
    local names=()

    if [ "$what" = CODE ]; then
        if [ "$mode" = secboot ]; then
            names=(OVMF_CODE.secboot.fd OVMF_CODE.secboot.4m.fd
                   OVMF_CODE_4M.secboot.fd OVMF_CODE.fd OVMF_CODE.4m.fd
                   OVMF_CODE_4M.fd)
        else
            names=(OVMF_CODE.fd OVMF_CODE.4m.fd OVMF_CODE_4M.fd)
        fi
    else
        if [ "$mode" = secboot ]; then
            names=(OVMF_VARS.secboot.fd OVMF_VARS.4m.fd OVMF_VARS_4M.fd
                   OVMF_VARS.fd)
        else
            names=(OVMF_VARS.fd OVMF_VARS.4m.fd OVMF_VARS_4M.fd)
        fi
    fi
    for d in "${dirs[@]}"; do
        [ -n "$d" ] && [ -d "$d" ] || continue
        for f in "${names[@]}"; do
            [ -f "$d/$f" ] && { printf '%s' "$d/$f"; return 0; }
        done
    done
    return 1
}

# Does this varstore already carry a Platform Key? No PK means setup mode,
# which means nothing enforces and a "Secure Boot" run would prove nothing.
ovmf_vars_have_pk() {   # ovmf_vars_have_pk <virt-fw-vars> <varstore>
    "$1" --input "$2" --print 2>/dev/null | grep -qiE '^[[:space:]]*-?[[:space:]]*PK\b'
}

# The same for AAVMF, edk2's AArch64 virt firmware, for the --aa64 rigs.
#
#   Debian   /usr/share/AAVMF/  AAVMF_CODE.fd, AAVMF_CODE.secboot.fd,
#            AAVMF_VARS.fd (blank), AAVMF_VARS.ms.fd (Microsoft's keys)
#   Fedora   /usr/share/edk2/aarch64/  QEMU_EFI-pflash.raw,
#            vars-template-pflash.raw
#   Gentoo   /usr/share/edk2/ArmVirtQemu-AARCH64/  QEMU_EFI.qcow2,
#            QEMU_EFI.secboot_INSECURE.qcow2, QEMU_VARS.qcow2
#
# AAVMF_DIR wins when set. Debian's secboot VARS is the one with keys
# enrolled; the others need mk-test-mok.sh's --enroll-microsoft, like Arch's
# OVMF.
aavmf_find() {   # aavmf_find <CODE|VARS> <plain|secboot> -> path, or empty
    local what=$1 mode=$2 d f
    local dirs=("${AAVMF_DIR:-}" /usr/share/AAVMF /usr/share/edk2/aarch64 \
                /usr/share/edk2/ArmVirtQemu-AARCH64)
    local names=()

    if [ "$what" = CODE ]; then
        if [ "$mode" = secboot ]; then
            names=(AAVMF_CODE.secboot.fd QEMU_EFI.secboot_INSECURE.qcow2)
        else
            names=(AAVMF_CODE.fd QEMU_EFI-pflash.raw QEMU_EFI.qcow2)
        fi
    else
        if [ "$mode" = secboot ]; then
            names=(AAVMF_VARS.ms.fd QEMU_VARS.secboot_INSECURE.qcow2)
        else
            names=(AAVMF_VARS.fd vars-template-pflash.raw QEMU_VARS.qcow2)
        fi
    fi
    for d in "${dirs[@]}"; do
        [ -n "$d" ] && [ -d "$d" ] || continue
        for f in "${names[@]}"; do
            [ -f "$d/$f" ] && { printf '%s' "$d/$f"; return 0; }
        done
    done
    return 1
}

# The -drive format a firmware file is in: Gentoo ships qcow2.
fw_format() {   # fw_format <path> -> raw|qcow2
    case "$1" in *.qcow2) printf qcow2 ;; *) printf raw ;; esac
}
