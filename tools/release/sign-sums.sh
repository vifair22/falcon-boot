#!/usr/bin/env bash
# falcon-boot: sign a published release's SHA256SUMS with the release key and
# attach the signature to the release (docs/Manual.md Section 14.1).
#
# Run by the release key's holder, on the machine that keeps the key, after
# the tag's pipeline has published the release. The key never goes near CI
# and is not the pacman repository's (14.3). The script fetches SHA256SUMS
# and every file it names from the release's package registry, checks them,
# signs SHA256SUMS (detached, armored), verifies the signature against the
# public key in this repository, tools/release/falcon-release.asc, and
# attaches SHA256SUMS.asc and that public key to the release.
#
# Usage: tools/release/sign-sums.sh vX.Y.Z
# Env:   GITLAB_HOST  default git.airies.net; glab must be logged in there
set -euo pipefail
cd "$(dirname "$0")/../.."

TAG=${1:?usage: sign-sums.sh vX.Y.Z}
[[ $TAG =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "sign-sums: $TAG is not vX.Y.Z" >&2; exit 1; }
VER=${TAG#v}
PUB=$PWD/tools/release/falcon-release.asc
export GITLAB_HOST=${GITLAB_HOST:-git.airies.net}
PROJECT=261
[[ -f $PUB ]] || { echo "sign-sums: no $PUB; the release key's public half goes there" >&2; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/falcon-sign.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

get() { glab api "projects/$PROJECT/packages/generic/falcon-boot/$VER/$1" > "$1"; }

get SHA256SUMS
while read -r _ name; do
    echo "== fetching $name"
    get "$name"
done < SHA256SUMS
sha256sum -c SHA256SUMS

# The key is the one whose public half is in the repository: its
# fingerprint, from that file, picks the secret key gpg signs with.
fpr=$(gpg --batch --with-colons --show-keys "$PUB" |
      awk -F: '$1 == "fpr" { print $10; exit }')
[[ -n $fpr ]] || { echo "sign-sums: $PUB holds no key" >&2; exit 1; }
gpg --batch --yes --armor --detach-sign --local-user "$fpr" \
    --output SHA256SUMS.asc SHA256SUMS

# Checked as a download is (14.1): against the published key alone.
gpg --batch --dearmor < "$PUB" > release.gpg
gpgv --keyring "$WORK/release.gpg" SHA256SUMS.asc SHA256SUMS

cp "$PUB" falcon-release.asc
glab release upload "$TAG" SHA256SUMS.asc falcon-release.asc -R "vifair22/falcon-boot"
echo "sign-sums: $TAG's SHA256SUMS signed by $fpr; SHA256SUMS.asc attached"
