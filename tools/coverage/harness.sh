#!/usr/bin/env bash
# falcon-boot: run the coverage harness inside its container
# (tools/coverage/Dockerfile, tools/coverage/run-case.sh,
# tools/coverage/scorecard.py).
#
# Wraps `docker run` with the four things the harness needs and nothing else:
# /dev/kvm, privileged (mk-case-stick.sh calls `sudo losetup`), the repository
# at /work and the corpus read-only at /corpus.
#
# /work, not /src, and the name is load-bearing. tools/analyze.sh keeps test
# sources out of the gate by matching the substring "/src/" against each
# absolute path from the compile database, which is how the repository's
# own src/ directories are recognised. Mounting the tree at /src made every
# path in it match, so the gate silently linted the whole test suite: 357 of
# 403 findings in a run here were test files that were never meant to be in
# it. Any mount point whose name collides with a directory the tree uses
# would do the same.
#
# With no command it runs the full corpus. Any other command runs in the same
# environment, which is what makes `harness.sh make test` and
# `harness.sh bash` work without a second wrapper.
#
# Usage: tools/coverage/harness.sh [options] [--] [command...]
#   --build         (re)build the image before running
#   --image NAME    image tag (default falcon-boot-harness)
#   --corpus DIR    corpus on the host (default /srv/falcon/corpus)
#   --build-dir DIR host directory bound over the repo's build/, so a clone
#                   on a small disk can put artifacts on a big one; empty
#                   to use the repo's own build/ (default /srv/falcon/build)
#   --sticks DIR    host directory bound over build/coverage/sticks; empty
#                   to keep the sticks inside the build directory (default
#                   /srv/build/cache/falcon/sticks when it exists and the
#                   build directory is the default one)
# Env: the same four as the options, FALCON_HARNESS_IMAGE, FALCON_CORPUS,
#      FALCON_BUILD_DIR, FALCON_STICKS_DIR.
#
# The case sticks live apart from the build directory on the binhost because
# they are most of its size (about 200 GB for the corpus) and the work disk is
# shared with the binhost's build roots. They need a local disk: every case
# reads its stick. A symlink in their place would dangle inside the container,
# whose build/ is a bind mount, so the directory is mounted instead.

set -euo pipefail

cd "$(dirname "$0")/../.."
REPO=$PWD

IMAGE="${FALCON_HARNESS_IMAGE:-falcon-boot-harness}"
CORPUS="${FALCON_CORPUS:-/srv/falcon/corpus}"
# The binhost's layout is the default where it exists; elsewhere the repo's
# own build/ and no corpus, which is enough for `harness.sh make ...`.
if [[ -d /srv/falcon/build ]]; then
    BUILD_DIR="${FALCON_BUILD_DIR-/srv/falcon/build}"
else
    BUILD_DIR="${FALCON_BUILD_DIR-}"
fi
STICKS_DEFAULT=/srv/build/cache/falcon/sticks
STICKS_DIR="${FALCON_STICKS_DIR-unset}"
BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
    --build) BUILD=1; shift ;;
    --image) IMAGE=$2; shift 2 ;;
    --corpus) CORPUS=$2; shift 2 ;;
    --build-dir) BUILD_DIR=$2; shift 2 ;;
    --sticks) STICKS_DIR=$2; shift 2 ;;
    -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --) shift; break ;;
    -*) echo "ERROR: unknown option $1" >&2; exit 1 ;;
    *) break ;;
    esac
done

command -v docker >/dev/null 2>&1 || {
    echo "ERROR: docker is not on PATH" >&2; exit 1; }
KVM=()
if [[ -e /dev/kvm ]]; then
    KVM=(--device /dev/kvm)
    [[ -w /dev/kvm ]] || echo "warning: /dev/kvm is not writable by $(id -un)" >&2
elif [[ $# -eq 0 ]]; then
    echo "ERROR: /dev/kvm is absent; the corpus boots every case under KVM" >&2
    exit 1
else
    echo "warning: no /dev/kvm; a build works, a rig will not" >&2
fi

if [[ $BUILD -eq 1 ]] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "building $IMAGE"
    docker build -t "$IMAGE" -f tools/coverage/Dockerfile tools/coverage
    # Every edit to the Dockerfile's package list makes a new apt layer of
    # about 3 GB and leaves the old one in the build cache, where nothing
    # else removes it. The filter is host-wide, which is the binhost's own
    # policy for its build cache; records newer than two days survive, so a
    # rebuild that follows soon after still finds its layers.
    docker builder prune -f --filter until=48h >/dev/null ||
        echo "warning: could not prune the docker build cache" >&2
fi

# A missing corpus is worth saying plainly: every case would be skipped and the
# run would look like a pass with an empty scorecard.
if [[ ! -d $CORPUS && $# -eq 0 ]]; then
    echo "ERROR: no corpus at $CORPUS" >&2; exit 1
fi

# The host's /dev, not the container's private copy.
#
# Docker gives a privileged container a fresh devtmpfs that already holds
# /dev/loop-control and a few /dev/loopN, so `losetup -f` succeeds and looks
# like it worked. What is missing is everything created AFTER the container
# started: `losetup -P` asks the kernel to scan the partition table, the
# partition nodes appear in the host's /dev, and the container never sees
# them. mk-case-stick.sh then fails at the first mkfs, and `mount -o loop`
# fails earlier still with "loop device with autoclear flag is required".
#
# Sharing /dev lets the container see every device on the box. It is already
# --privileged, so this widens nothing that was closed; it is listed here as
# what it is rather than buried.
MOUNTS=(-v "$REPO:/work" -v /dev:/dev)
[[ -d $CORPUS ]] && MOUNTS+=(-v "$CORPUS:/corpus:ro")
if [[ -n $BUILD_DIR ]]; then
    mkdir -p "$BUILD_DIR"
    MOUNTS+=(-v "$BUILD_DIR:/work/build")
fi
if [[ $STICKS_DIR == unset ]]; then
    STICKS_DIR=
    [[ $BUILD_DIR == /srv/falcon/build && -d $STICKS_DEFAULT ]] &&
        STICKS_DIR=$STICKS_DEFAULT
fi
if [[ -n $STICKS_DIR ]]; then
    [[ -n $BUILD_DIR ]] || {
        echo "ERROR: --sticks needs a build directory to mount under" >&2
        exit 1; }
    mkdir -p "$STICKS_DIR"
    MOUNTS+=(-v "$STICKS_DIR:/work/build/coverage/sticks")
fi

# Interactive only when there is a terminal, so the same line works from a
# shell and from a pipeline job.
TTY=()
[[ -t 0 && -t 1 ]] && TTY=(-it)

CMD=("$@")
[[ ${#CMD[@]} -eq 0 ]] && CMD=(tools/coverage/run-corpus.sh)
# The container runs as root (losetup); what it wrote under /work is handed
# back to the caller afterwards, whatever the command's exit status.
CMDQ=$(printf '%q ' "${CMD[@]}")

exec docker run --rm "${TTY[@]}" \
    --privileged "${KVM[@]}" \
    "${MOUNTS[@]}" \
    -e FALCON_CORPUS=/corpus \
    -e FALCON_CASE_WJLOG \
    -e FALCON_CASE_MEM \
    -w /work \
    "$IMAGE" bash -c "$CMDQ; rc=\$?; chown -R $(id -u):$(id -g) /work/build /work/compile_commands.json 2>/dev/null; exit \$rc"
