#!/usr/bin/env bash
# The processor floor, checked on what was linked rather than on the flags
# it was compiled with (DESIGN Section 11.3 and 13.8). falcon-bios and
# falcon-hook32 are built -march=i586; stage 2's cpu_check refuses anything
# older. What an i586 does not have and an i686 does is CMOV, FCMOV and
# FCOMI/FUCOMI, and
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
# --userland is the other floor: a static Linux program that runs on the
# machine itself, the station bundle's mkstick (DESIGN Section 12.14), whose
# floor is the i686 it was first used on. CMOV and the rest of the
# i686 set are fine there, and so are MMX and SSE; SSE2 and everything after
# are not, in any function the program can reach without asking CPUID first.
# What does ask is exempt by name: glibc's string routines chosen at run time
# (__memcpy_sse2 and the like, _sse2, _ssse3, _sse4_2, _sse42, _avx*,
# _evex*) and falcon-core's aesni_*. This is the check that would have
# stopped a mkstick linked against a libc built for the build host: a
# distro glibc built with a newer -march can put AVX in _dl_aux_init, and
# the station then takes #UD on the first instruction of the program.
#
# Usage: check-floor.sh [--userland] ELF...
set -euo pipefail

userland=0
if [[ ${1:-} == --userland ]]; then
    userland=1
    shift
fi

bad=0
for elf in "$@"; do
    if [[ $userland -eq 0 ]]; then
        hits=$(objdump -d "$elf" |
            awk '/^[0-9a-f]+ <.*>:$/ { fn = $2 }
                 /\t(f?cmov[a-z]*|fu?comip?) / { n[fn]++ }
                 END { for (f in n) printf "  %s %d\n", f, n[f] }')
    else
        # An instruction on an xmm or ymm register that is not SSE1's (a
        # packed or scalar single, or one of its conversions and compares),
        # or any VEX-encoded one, outside the exempt functions.
        hits=$(objdump -d "$elf" |
            awk '/^[0-9a-f]+ <.*>:$/ { fn = $2; next }
                 fn ~ /_(sse2|ssse3|sse4_?2|sse42|avx[0-9a-z_]*|evex[0-9]*)(_[a-z_]+)?>:$/ { next }
                 fn ~ /^<aesni_/ { next }
                 /%[xy]mm/ {
                     split($0, a, "\t"); m = a[3]; sub(/ .*/, "", m)
                     if (m ~ /^v/ || (m !~ /(ps|ss)$/ &&
                         m !~ /^(cvttss2si|cvtss2si|cvtsi2ss|ucomiss|comiss)$/))
                         n[fn]++
                 }
                 END { for (f in n) printf "  %s %d\n", f, n[f] }')
    fi
    if [[ -n "$hits" ]]; then
        if [[ $userland -eq 0 ]]; then
            echo "ERROR: $elf has instructions a Pentium does not (function, count):" >&2
        else
            echo "ERROR: $elf has instructions a Pentium III does not, outside" \
                "CPUID-dispatched code (function, count):" >&2
        fi
        echo "$hits" >&2
        bad=1
    fi
done
if [[ $bad -ne 0 && $userland -eq 0 ]]; then
    echo "       an object built for i686 is left over (make clean), or code" >&2
    echo "       asks for more than the i586 floor" >&2
elif [[ $bad -ne 0 ]]; then
    echo "       the static libc it was linked against was built for a newer" >&2
    echo "       processor; build it in the harness container" >&2
    echo "       (tools/coverage/harness.sh make station-bundle)" >&2
fi
exit "$bad"
