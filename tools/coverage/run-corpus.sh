#!/usr/bin/env bash
# falcon-boot: run the coverage corpus through the harness and regenerate the
# scorecard (docs/notes/coverage-roadmap.md Section 2).
#
# For every corpus row whose ISO is present and verified (tools/coverage/
# corpus-fetch.sh), and for every requested state and route:
#
#   1. build the case sticks once per ISO (tools/coverage/mk-case-stick.sh);
#      they are reused across runs until the ISO changes;
#   2. stage the current loader into every stick (tools/coverage/
#      stage-loader.sh), so the run measures the tree as it is;
#   3. boot each case (tools/coverage/run-case.sh), JOBS at a time;
#   4. write docs/coverage.md (tools/coverage/scorecard.py).
#
# Results live under build/coverage/runs/<id>/<state>-<route>/, and a case
# whose result exists is skipped unless --force: an interrupted run resumes
# where it stopped. A stale result from an older loader is still a result; the
# scorecard prints the loader version each case ran against.
#
# Usage: tools/coverage/run-corpus.sh [options] [ID...]
#   --states "off ms own"     which states (default the three Secure Boot
#                             ones; `bios` and `bios32` are the SeaBIOS
#                             columns and are not among them, so they are
#                             asked for by name)
#   --routes "dnd chain blessed"  which routes (default all three)
#   --jobs N                  cases in flight at once (default 3; each takes
#                             FALCON_CASE_MEM, 4 GiB by default). KEEP IT
#                             LOW. Measured 2026-09-20: at --jobs 10 a Kali
#                             blessed case panicked in the guest with
#                             "Attempted to kill init", passed on its own,
#                             and passed in another Secure Boot state off the
#                             same stick. Disk contention starves a guest
#                             into failures that read as product bugs, and a
#                             flaky cell is worse than a slow run.
#                             2026-09-26: --jobs 8 over the UEFI columns
#                             (339 cases) matched the baseline exactly; at
#                             --jobs 12 over the BIOS columns 3 of 372
#                             failed (two panics, one timeout) and all three
#                             passed alone. Run wide, then rerun every
#                             changed case alone before calling it.
#   --timeout S               seconds from the keypress to a verdict (300)
#   --force                   rerun cases that already have a result
#   --no-scorecard            leave docs/coverage.md alone
# Env: FALCON_CORPUS  corpus directory (default /srv/falcon/corpus)

set -euo pipefail

cd "$(dirname "$0")/../.."

MANIFEST=tools/coverage/corpus.tsv
CORPUS="${FALCON_CORPUS:-/srv/falcon/corpus}"
STICKS=build/coverage/sticks
RUNS=build/coverage/runs

STATES="off ms own"
# All three by default.
#
# chain was on request only, which meant it had no baseline, and that showed:
# when the survey moved off the firmware the only way to argue the chainload
# was unaffected was to diff the functions on it, because there was no run to
# compare against. It is the route that still uses the firmware's volumes, so
# it is the one most worth watching.
ROUTES="dnd chain blessed"
JOBS=3
TIMEOUT=300
FORCE=0
SCORECARD=1
WANT=()
while [[ $# -gt 0 ]]; do
    case "$1" in
    --states) STATES=$2; shift 2 ;;
    --routes) ROUTES=$2; shift 2 ;;
    --jobs) JOBS=$2; shift 2 ;;
    --timeout) TIMEOUT=$2; shift 2 ;;
    --force) FORCE=1; shift ;;
    --no-scorecard) SCORECARD=0; shift ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "ERROR: unknown option $1" >&2; exit 1 ;;
    *) WANT+=("$1"); shift ;;
    esac
done

wanted() {
    local w
    [[ ${#WANT[@]} -eq 0 ]] && return 0
    for w in "${WANT[@]}"; do [[ "$w" == "$1" ]] && return 0; done
    return 1
}
has_route() { [[ " $ROUTES " == *" $1 "* ]]; }

mkdir -p "$STICKS" "$RUNS"
VERSION=$(sed -n 's/.*FALCON_VERSION "\(.*\)".*/\1/p' build/version.h 2>/dev/null)

# The loader and the sign tool under test are the tree's: both are built
# first, so neither a stale signed loader nor a sign tool carrying an older
# falcon-hook can be what the corpus measures. falcon-bios and mkstick too:
# every stick carries the BIOS stages, whichever columns this run measures
# (mk-case-stick.sh sizes p1 for them, stage-loader.sh puts them on).
make --no-print-directory -s sign build/bin/falcon-boot-sign bios cli >/dev/null
VERSION=$(sed -n 's/.*FALCON_VERSION "\(.*\)".*/\1/p' build/version.h 2>/dev/null)

# ---- sticks ----
CASES=()
while IFS=$'\t' read -r id tier family sha file source arch; do
    [[ -z "$id" || "$id" == \#* ]] && continue
    wanted "$id" || continue
    iso="$CORPUS/$file"
    if [[ ! -f "$iso.sha256-ok" ]]; then
        echo "== $id: not in the corpus (run tools/coverage/corpus-fetch.sh)" >&2
        continue
    fi

    stick="$STICKS/$id"
    # Windows media has nothing for bless to wrap; its Secure Boot story is
    # Microsoft's signature (DESIGN Section 6.3), so it has no blessed route.
    blessable=1
    [[ "$family" == windows ]] && blessable=0
    # A 32-bit ISO boots in the BIOS columns only (corpus.tsv, arch), and
    # blessing is a UEFI act.
    arch=${arch:-x86_64}
    [[ "$arch" == i686 ]] && blessable=0

    # A bless the sign tool refused is remembered beside the stick, so it is
    # not retried every run; deleting the marker retries it.
    # A stick older than the identity records (no boot record on p1, no room
    # for stage 3) is remade rather than patched.
    recs=
    [[ -f "$stick.img" ]] &&
        recs=$(build/bin/falcon-boot-mkstick ident "$stick.img" 2>/dev/null ||
               true)
    if [[ ! -f "$stick.img" || "$iso" -nt "$stick.img" ||
          "$recs" != *$'\tboot\t'* ]]; then
        rm -f "$stick.blessed.img"
        echo "== $id: building sticks"
        rm -f "$stick.unblessable"
        args=()
        if has_route blessed && [[ $blessable -eq 1 ]]; then
            args+=(--blessed)
        fi
        tools/coverage/mk-case-stick.sh "$iso" "$stick" "${args[@]}" ||
            touch "$stick.unblessable"
    elif has_route blessed && [[ $blessable -eq 1 && ! -f "$stick.blessed.img" &&
                                 ! -f "$stick.unblessable" ]]; then
        echo "== $id: blessing"
        tools/coverage/mk-case-stick.sh "$iso" "$stick" --blessed --reuse ||
            touch "$stick.unblessable"
    fi

    for st in $STATES; do
        for rt in $ROUTES; do
            out="$RUNS/$id/$st-$rt"
            # Blessing is a Secure Boot act: it signs an image with the MOK so
            # shim will load it. A BIOS machine has no shim and enforces
            # nothing, so the column has no blessed cell to fill.
            if [[ "$st" == bios* && "$rt" == blessed ]]; then
                mkdir -p "$out"
                printf 'N/A\tno blessed route on a BIOS machine\t0\n' \
                    > "$out/result"
                continue
            fi
            # Width: the UEFI loader is x86-64, and the Pentium III column
            # has no long mode for a 64-bit kernel.
            if [[ "$arch" == i686 && "$st" != bios* ]]; then
                mkdir -p "$out"
                printf 'N/A\ta 32-bit ISO; the UEFI loader is x86-64\t0\n' \
                    > "$out/result"
                continue
            fi
            if [[ "$arch" != i686 && "$st" == bios32 ]]; then
                mkdir -p "$out"
                printf 'N/A\ta 64-bit ISO on a processor without long mode\t0\n' \
                    > "$out/result"
                continue
            fi
            if [[ "$rt" == blessed ]]; then
                if [[ $blessable -eq 0 ]]; then
                    mkdir -p "$out"
                    printf 'N/A\tno blessed route for this family\t0\n' > "$out/result"
                    continue
                fi
                if [[ ! -f "$stick.blessed.img" ]]; then
                    mkdir -p "$out"
                    printf 'UNBLESSABLE\tthe sign tool refused this ISO\t0\n' \
                        > "$out/result"
                    continue
                fi
            fi
            if [[ $FORCE -eq 0 && -f "$out/result" ]]; then
                continue
            fi
            CASES+=("$id $st $rt")
        done
    done
done < "$MANIFEST"


shopt -s nullglob
STAGE=("$STICKS"/*.img)
if [[ ${#STAGE[@]} -gt 0 ]]; then
    echo "== staging loader $VERSION into ${#STAGE[@]} stick(s)"
    tools/coverage/stage-loader.sh "${STAGE[@]}"
fi

# ---- cases ----
run_one() {   # run_one ID STATE ROUTE
    local id=$1 st=$2 rt=$3 stick out r
    stick="$STICKS/$id.img"
    [[ "$rt" == blessed ]] && stick="$STICKS/$id.blessed.img"
    out="$RUNS/$id/$st-$rt"
    r=$(tools/coverage/run-case.sh "$stick" "$st" "$rt" "$out" "$TIMEOUT" 2>&1 |
        tail -1)
    printf '%s\n' "$VERSION" > "$out/loader"
    printf '%-24s %-4s %-8s %s\n' "$id" "$st" "$rt" "$r"
}
export -f run_one
export STICKS RUNS TIMEOUT VERSION

echo "== ${#CASES[@]} case(s), $JOBS at a time"
if [[ ${#CASES[@]} -gt 0 ]]; then
    printf '%s\n' "${CASES[@]}" |
        xargs -P "$JOBS" -L 1 bash -c 'run_one "$0" "$1" "$2"'
fi

if [[ $SCORECARD -eq 1 ]]; then
    python3 tools/coverage/scorecard.py > docs/coverage.md
    echo "== docs/coverage.md regenerated"
fi
