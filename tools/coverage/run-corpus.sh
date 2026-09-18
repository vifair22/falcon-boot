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
#   --states "off ms own"     which Secure Boot states (default all three)
#   --routes "dnd blessed"    which routes (default dnd blessed; chain once
#                             the loader has the key, M2)
#   --jobs N                  cases in flight at once (default 3; each takes
#                             FALCON_CASE_MEM, 4 GiB by default)
#   --timeout S               seconds from the keypress to a verdict (300)
#   --force                   rerun cases that already have a result
#   --no-scorecard            leave docs/coverage.md alone
# Env: FALCON_CORPUS  corpus directory (default /mnt/falcon-corpus/corpus)

set -euo pipefail

cd "$(dirname "$0")/../.."

MANIFEST=tools/coverage/corpus.tsv
CORPUS="${FALCON_CORPUS:-/mnt/falcon-corpus/corpus}"
STICKS=build/coverage/sticks
RUNS=build/coverage/runs

STATES="off ms own"
ROUTES="dnd blessed"
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

# ---- sticks ----
CASES=()
while IFS=$'\t' read -r id tier family sha file source; do
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

    # A bless the sign tool refused is remembered beside the stick, so it is
    # not retried every run; deleting the marker retries it.
    if [[ ! -f "$stick.img" || "$iso" -nt "$stick.img" ]]; then
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
