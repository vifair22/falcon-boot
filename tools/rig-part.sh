# shellcheck shell=bash
# falcon-boot: find a rig image's partition by its identity record. Sourced,
# not run.
#
# The rig and corpus scripts mount a stick's partitions to stage or check
# files. Which partition is the ISO library is what its record says (DESIGN
# Section 3.1), not its number, so they ask `falcon-boot-mkstick ident`,
# which reads the table and every record with the loaders' own code and
# needs no root on an image file.
#
#   rig_part IMG ROLE    the number of the first partition whose record has
#                        ROLE (boot, library, shelf); fails when none does
#   rig_part_start IMG ROLE   its first LBA
#
# Env: MKSTICK  the tool (default build/bin/falcon-boot-mkstick)

RIG_MKSTICK=${MKSTICK:-build/bin/falcon-boot-mkstick}

rig__field() {   # rig__field IMG ROLE COLUMN
    local out

    [[ -x "$RIG_MKSTICK" ]] || {
        echo "ERROR: $RIG_MKSTICK not built (run: make cli)" >&2
        return 1
    }
    out=$("$RIG_MKSTICK" ident "$1" |
        awk -F '\t' -v role="$2" -v col="$3" '$2 == role { print $col; exit }')
    [[ -n "$out" ]] || {
        echo "ERROR: no partition of $1 carries a $2 record" >&2
        return 1
    }
    printf '%s\n' "$out"
}

rig_part() {
    rig__field "$1" "$2" 1
}

rig_part_start() {
    rig__field "$1" "$2" 4
}
