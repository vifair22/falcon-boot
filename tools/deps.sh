#!/usr/bin/env bash
# The tool inventory, from tools/deps.tsv (the one list).
#   deps.sh check      what is missing on this machine; exit 1 if a required one
#   deps.sh markdown   the table docs/Manual.md 13.1 carries
#   deps.sh sync       the Dockerfile's packages and the Manual's table agree
#                      with the list; exit 1 when they do not
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TSV="$ROOT/tools/deps.tsv"

rows() { grep -v '^#' "$TSV" | grep -v '^$'; }

have() {                       # $1 = check spec
    local kind=${1%%:*} arg=${1#*:} a
    case "$kind" in
    cmd)  IFS='|' read -ra alts <<< "$arg"
          for a in "${alts[@]}"; do command -v "$a" >/dev/null 2>&1 && return 0; done; return 1 ;;
    pkgconf) pkg-config --exists "$arg" 2>/dev/null ;;
    file) IFS='|' read -ra alts <<< "$arg"
          for a in "${alts[@]}"; do [ -e "$a" ] && return 0; done; return 1 ;;
    m32)  [ "$(${CC_HOST:-gcc} -m32 -print-multi-directory 2>/dev/null)" = 32 ] ;;
    *)    echo "deps.sh: unknown check '$1'" >&2; return 2 ;;
    esac
}

case "${1:-check}" in
check)
    missing=0
    while IFS=$'\t' read -r name chk pkg group need purpose; do
        have "$chk" && continue
        if [ "$need" = required ]; then
            echo "MISSING (required): $name [$pkg]: $purpose"; missing=1
        else
            echo "missing ($group): $name [$pkg]: $purpose"
        fi
    done < <(rows)
    [ "$missing" = 0 ] && echo "Toolchain OK." || exit 1 ;;
markdown)
    echo '| tool | Debian package | group | needed for |'
    echo '|---|---|---|---|'
    while IFS=$'\t' read -r name chk pkg group need purpose; do
        n="\`$name\`"; [ "$need" = required ] && n="$n (required)"
        echo "| $n | ${pkg//(/ (} | $group | $purpose |"
    done < <(rows) ;;
sync)
    rc=0
    # packages the Dockerfile installs, versus the list (pip, base, source and
    # release entries aside)
    df=$(grep -vE '^\s*#' "$ROOT/tools/coverage/Dockerfile" | tr -s ' \\\n' ' ' |
         grep -oE 'apt-get install[^&]*' | sed 's/apt-get install//; s/ -y//; s/--no-install-recommends//' |
         tr -s ' ' '\n' | grep -v '^$' | sort -u)
    ls=$(rows | cut -f3 | tr ' ' '\n' | grep -vE '\(pip\)|\(base\)|\(source\)|\(release\)|^$' | sort -u)
    for p in $(comm -23 <(echo "$df") <(echo "$ls")); do
        case "$p" in ca-certificates|sudo) continue ;; esac   # the container's own
        echo "deps: Dockerfile installs $p, which tools/deps.tsv does not list"; rc=1
    done
    for p in $(comm -13 <(echo "$df") <(echo "$ls")); do
        echo "deps: tools/deps.tsv lists $p, which the Dockerfile does not install"; rc=1
    done
    # the Manual's table is the rendered list
    man=$(sed -n '/<!-- deps:begin -->/,/<!-- deps:end -->/p' "$ROOT/docs/Manual.md" | sed '1d;$d')
    if [ "$man" != "$("$0" markdown)" ]; then
        echo "deps: docs/Manual.md 13.1's table differs from 'tools/deps.sh markdown'"; rc=1
    fi
    [ $rc = 0 ] && echo "deps: in sync"
    exit $rc ;;
*) echo "usage: deps.sh check|markdown|sync" >&2; exit 2 ;;
esac
