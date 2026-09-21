# shellcheck shell=bash
# falcon-boot: where the rig's Arch ISO comes from. Sourced, not run.
#
# Three scripts take a kernel (and the appliance its modules) out of one Arch
# ISO: falcon-appliance/image/mk-appliance.sh, falcon-efi/tools/mk-test-uki.sh
# and falcon-cli/tools/mk-test-bless.sh. The work happens on the archusb
# stick, and the ISO is already on that stick's p2, so that is the default
# rather than a path on some other machine's disk.
#
# p2 is not normally mounted. arch_iso_resolve uses it where it already is,
# and otherwise mounts it READ-ONLY for the run; arch_iso_release undoes only
# a mount this file made. A caller puts arch_iso_release in its EXIT path.
#
# FALCON-ISO is also the label on every rig stick's p2, so a stick attached
# with losetup -P carries it too. Loop devices are skipped, and more than one
# candidate left is an error rather than a guess: the kernel and modules have
# to come from the ISO the fixtures were built against.
#
# Env: ARCH_ISO  a path to use instead; nothing is mounted then

ARCH_ISO_REL=Linux/archlinux-2026.01.01-x86_64.iso
ARCH_ISO_MNT=

arch_iso_resolve() {
    local devs=() dev mnt

    [[ -n "${ARCH_ISO:-}" ]] && return 0

    while read -r dev; do
        [[ "$dev" == /dev/loop* ]] || devs+=("$dev")
    done < <(sudo blkid -t LABEL=FALCON-ISO -o device 2>/dev/null)
    if [[ ${#devs[@]} -ne 1 ]]; then
        echo "ERROR: ${#devs[@]} FALCON-ISO partitions outside loop devices" \
             "(${devs[*]:-none}); set ARCH_ISO" >&2
        return 1
    fi

    # findmnt exits 1 for "not mounted", which is an answer, not a failure.
    mnt=$(findmnt -rn --first-only -S "${devs[0]}" -o TARGET || true)
    if [[ -z "$mnt" ]]; then
        ARCH_ISO_MNT=$(mktemp -d)
        sudo mount -o ro "${devs[0]}" "$ARCH_ISO_MNT" || {
            rmdir "$ARCH_ISO_MNT"
            ARCH_ISO_MNT=
            echo "ERROR: could not mount ${devs[0]} read-only" >&2
            return 1
        }
        mnt=$ARCH_ISO_MNT
    fi
    ARCH_ISO="$mnt/$ARCH_ISO_REL"
}

arch_iso_release() {
    [[ -n "$ARCH_ISO_MNT" ]] || return 0
    sudo umount "$ARCH_ISO_MNT" 2>/dev/null || true
    rmdir "$ARCH_ISO_MNT" 2>/dev/null || true
    ARCH_ISO_MNT=
}
