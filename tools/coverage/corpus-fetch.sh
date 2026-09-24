#!/usr/bin/env bash
# falcon-boot: fetch and verify the coverage corpus (tools/coverage/corpus.tsv).
#
# Every file is checked against its pinned SHA-256 before it is given its
# final name, so a file present in the corpus directory under its manifest
# name is always the pinned bytes. A download lands as NAME.part first and is
# resumed from there if interrupted; a finished file that fails the check is
# moved aside as NAME.bad rather than deleted, so a mirror that changed a file
# under a stable name is visible instead of silently refetched forever.
#
# Rows whose source is local:<note> have no URL. They are copied from the
# directories named with --local (searched in order, by file name) and then
# verified like anything else; without a match they are reported and skipped.
#
# Rows whose source is torrent:<url> name a .torrent (Kali's live ISO is
# published no other way). aria2c fetches it into NAME.torrent.d beside the
# corpus, without seeding, and the file is then verified like anything else.
#
# A verified file is recorded in NAME.sha256-ok beside it (the hash, and the
# file's size and mtime), so a re-run does not rehash a hundred gigabytes to
# find nothing to do. A file whose size or mtime moved is hashed again.
#
# Usage: tools/coverage/corpus-fetch.sh [--local DIR]... [ID...]
#   ID      fetch only these rows (default: every row)
# Env:   FALCON_CORPUS  corpus directory (default /srv/falcon/corpus)
#
# Exit status: 0 when every requested row is present and verified, 1
# otherwise.

set -euo pipefail

cd "$(dirname "$0")/../.."

MANIFEST=tools/coverage/corpus.tsv
CORPUS="${FALCON_CORPUS:-/srv/falcon/corpus}"

LOCAL_DIRS=()
WANT=()
while [[ $# -gt 0 ]]; do
    case "$1" in
    --local)
        [[ $# -ge 2 ]] || { echo "ERROR: --local needs a directory" >&2; exit 1; }
        LOCAL_DIRS+=("$2")
        shift 2
        ;;
    -h|--help)
        sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    -*)
        echo "ERROR: unknown option $1" >&2
        exit 1
        ;;
    *)
        WANT+=("$1")
        shift
        ;;
    esac
done

[[ -d "$CORPUS" ]] || { echo "ERROR: corpus directory $CORPUS does not exist" >&2; exit 1; }

wanted() {
    local id=$1 w
    [[ ${#WANT[@]} -eq 0 ]] && return 0
    for w in "${WANT[@]}"; do
        [[ "$w" == "$id" ]] && return 0
    done
    return 1
}

# The stamp holds "sha256 size mtime"; it vouches for the file only while the
# last two still match.
stamp_ok() {
    local f=$1 sha=$2 st
    [[ -f "$f.sha256-ok" && -f "$f" ]] || return 1
    st="$sha $(stat -c '%s %Y' "$f")"
    [[ "$(cat "$f.sha256-ok")" == "$st" ]]
}

verify() {
    local f=$1 sha=$2 got
    got=$(sha256sum "$f" | cut -d' ' -f1)
    if [[ "$got" != "$sha" ]]; then
        echo "   sha256 MISMATCH: got $got" >&2
        return 1
    fi
    echo "$sha $(stat -c '%s %Y' "$f")" > "$f.sha256-ok"
}

rc=0
seen=0
while IFS=$'\t' read -r id tier family sha file source arch; do
    [[ -z "$id" || "$id" == \#* ]] && continue
    wanted "$id" || continue
    seen=$((seen + 1))
    dst="$CORPUS/$file"

    if stamp_ok "$dst" "$sha"; then
        echo "== $id: present"
        continue
    fi

    if [[ -f "$dst" ]]; then
        echo "== $id: present, verifying"
        if verify "$dst" "$sha"; then
            continue
        fi
        mv -f "$dst" "$dst.bad"
        echo "   moved aside to $dst.bad" >&2
    fi

    case "$source" in
    local:*)
        found=
        for d in "${LOCAL_DIRS[@]}"; do
            if [[ -f "$d/$file" ]]; then
                found="$d/$file"
                break
            fi
        done
        if [[ -z "$found" ]]; then
            echo "== $id: local media, not supplied (${source#local:})" >&2
            rc=1
            continue
        fi
        echo "== $id: copying from $found"
        cp "$found" "$dst.part"
        ;;
    torrent:*)
        echo "== $id: fetching by torrent ($tier, $family)"
        command -v aria2c >/dev/null ||
            { echo "   aria2c is not installed" >&2; rc=1; continue; }
        tdir="$dst.torrent.d"
        mkdir -p "$tdir"
        if ! aria2c --seed-time=0 --follow-torrent=mem --dir="$tdir" \
                --summary-interval=60 --console-log-level=warn \
                "${source#torrent:}" ||
           [[ ! -f "$tdir/$file" ]]; then
            echo "   torrent fetch failed; $tdir kept for resume" >&2
            rc=1
            continue
        fi
        mv -f "$tdir/$file" "$dst.part"
        rm -rf "$tdir"
        ;;
    https://*)
        echo "== $id: downloading ($tier, $family)"
        if ! curl -fL --retry 5 --retry-delay 10 -C - -o "$dst.part" "$source"; then
            echo "   download failed; $dst.part kept for resume" >&2
            rc=1
            continue
        fi
        ;;
    *)
        echo "== $id: unrecognised source '$source'" >&2
        rc=1
        continue
        ;;
    esac

    if verify "$dst.part" "$sha"; then
        mv -f "$dst.part" "$dst"
        # The stamp was written against the .part name's mtime, which the
        # rename keeps; rewrite it under the final name.
        mv -f "$dst.part.sha256-ok" "$dst.sha256-ok"
        echo "   verified"
    else
        mv -f "$dst.part" "$dst.bad"
        echo "   moved aside to $dst.bad" >&2
        rc=1
    fi
done < "$MANIFEST"

if [[ ${#WANT[@]} -gt 0 && $seen -ne ${#WANT[@]} ]]; then
    echo "ERROR: some requested ids are not in $MANIFEST" >&2
    rc=1
fi
exit $rc
