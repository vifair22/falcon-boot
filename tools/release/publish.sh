#!/usr/bin/env bash
# falcon-boot: publish a built release (release.mk) from a tag pipeline.
#
# Uploads every file in DIR to the project's generic package registry as
# falcon-boot/<version>/<file>, then creates the GitLab release for the tag
# with a link to each. The version is the tag's, which the caller has
# already checked against release_version. SHA256SUMS goes up with the
# rest; its signature is the release key holder's and is added to the
# release afterwards (docs/Manual.md Section 14).
#
# Usage: tools/release/publish.sh DIR
# Env:   the pipeline's CI_API_V4_URL, CI_PROJECT_ID, CI_JOB_TOKEN,
#        CI_COMMIT_TAG
set -euo pipefail

DIR=${1:?usage: publish.sh DIR}
TAG=${CI_COMMIT_TAG:?not a tag pipeline}
VER=${TAG#v}
PKG="$CI_API_V4_URL/projects/$CI_PROJECT_ID/packages/generic/falcon-boot/$VER"

links=()
for f in "$DIR"/*; do
    name=$(basename "$f")
    echo "== uploading $name"
    curl -sS --fail-with-body -H "JOB-TOKEN: $CI_JOB_TOKEN" \
        --upload-file "$f" "$PKG/$name" >/dev/null
    links+=("$name")
done

# The release, its notes the checksums a person compares a download with.
python3 - "$TAG" "$PKG" "$DIR/SHA256SUMS" "${links[@]}" >release.json <<'PY'
import json, sys
tag, pkg, sums, names = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
with open(sums) as f:
    checksums = f.read()
print(json.dumps({
    "tag_name": tag,
    "name": "falcon-boot " + tag[1:],
    "description": "Unsigned loaders and the tools that install them; "
                   "docs/Manual.md Section 14 says how to check and install "
                   "them.\n\n```\n" + checksums + "```\n",
    "assets": {"links": [
        {"name": n, "url": pkg + "/" + n, "link_type": "package"}
        for n in names]},
}))
PY
echo "== creating the release $TAG"
curl -sS --fail-with-body -H "JOB-TOKEN: $CI_JOB_TOKEN" \
    -H "Content-Type: application/json" --data @release.json \
    "$CI_API_V4_URL/projects/$CI_PROJECT_ID/releases" >/dev/null
echo "released $TAG"
