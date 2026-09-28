# shellcheck shell=bash
# Shared OVMF discovery, sourced by the rig scripts.
#
# Distros disagree about both the directory and the file names, and this stick
# roams between an Arch laptop and a Gentoo workstation, so one hardcoded path
# is wrong on one of them by construction. It was: every rig target failed on
# the laptop with "cannot stat .../OvmfX64/OVMF_VARS.secboot.fd".
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
