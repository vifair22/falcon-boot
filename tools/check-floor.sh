#!/usr/bin/env bash
# The processor floor, checked on what was linked rather than on the flags
# it was compiled with (DESIGN Section 12.3, docs/notes/bios-route.md
# Section 6h). falcon-bios and falcon-hook32 are built -march=i586 and run
# on a Pentium; stage 2's cpu_check refuses anything older. What a Pentium
# does not have and a Pentium Pro does is CMOV, FCMOV and FCOMI/FUCOMI, and
# an image carrying one faults with #UD in whichever function the compiler
# chose to put it, which is the worst place to learn it.
#
# The flags alone do not prove it. An object compiled before the floor
# moved is not rebuilt when the flags change, libgcc is whatever the
# distribution built it as, and an inline asm or a target attribute can say
# anything. So every image is disassembled and refused with the functions
# that carry one. What is CPUID-gated at run time (AES-NI in falcon-core)
# needs nothing from this list, so nothing is exempt.
#
# Usage: check-floor.sh ELF...
set -euo pipefail

bad=0
for elf in "$@"; do
    hits=$(objdump -d "$elf" |
        awk '/^[0-9a-f]+ <.*>:$/ { fn = $2 }
             /\t(f?cmov[a-z]*|fu?comip?) / { n[fn]++ }
             END { for (f in n) printf "  %s %d\n", f, n[f] }')
    if [[ -n "$hits" ]]; then
        echo "ERROR: $elf has instructions a Pentium does not (function, count):" >&2
        echo "$hits" >&2
        bad=1
    fi
done
if [[ $bad -ne 0 ]]; then
    echo "       an object built for i686 is left over (make clean), or code" >&2
    echo "       asks for more than the i586 floor" >&2
fi
exit "$bad"
