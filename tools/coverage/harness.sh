#!/usr/bin/env bash
# falcon-boot: run the coverage harness inside its container
# (tools/coverage/Dockerfile, tools/coverage/run-case.sh,
# tools/coverage/scorecard.py).
#
# Wraps `docker run` with the four things the harness needs and nothing else:
# /dev/kvm, privileged (mk-case-stick.sh calls `sudo losetup`), the repository
# at its own path and the corpus read-only at /corpus.
#
# The repository is mounted where it is on the host, so the paths a build
# records (compile_commands.json, gcov data, compiler messages) name files
# the host's editor and tools can open.
#
# With no command it runs the full corpus. Any other command runs in the same
# environment, which is what makes `harness.sh make test` and
# `harness.sh bash` work without a second wrapper. Outside the container the
# Makefile hands every goal here (its "where the build runs").
#
# The image is the registry's, tagged by the Dockerfile's hash, so a checkout
# runs the image its Dockerfile describes: pulled when absent, built when the
# pull fails (no `docker login` to the registry, or a Dockerfile no pipeline
# has published yet), and pushed when the registry lacks it and
# FALCON_HARNESS_PUSH=1, which the pipeline sets.
#
# Usage: tools/coverage/harness.sh [options] [--] [command...]
#   --build         (re)build the image before running
#   --image NAME    image (default $FALCON_HARNESS_REPO:<Dockerfile hash>)
#   --corpus DIR    corpus on the host (default /srv/falcon/corpus)
#   --build-dir DIR host directory bound over the repo's build/, so a clone
#                   on a small disk can put artifacts on a big one; empty
#                   to use the repo's own build/ (default /srv/falcon/build)
#   --sticks DIR    host directory bound over build/coverage/sticks; empty
#                   to keep the sticks inside the build directory (default
#                   /srv/build/cache/falcon/sticks when it exists and the
#                   build directory is the default one)
# Env: the same four as the options, FALCON_HARNESS_IMAGE, FALCON_CORPUS,
#      FALCON_BUILD_DIR, FALCON_STICKS_DIR;
#      FALCON_HARNESS_REPO          the image's repository (default
#                                   registry.git.airies.net:443/vifair22/
#                                   falcon-boot/harness)
#      FALCON_HARNESS_PUSH          1 pushes the image when the registry lacks it
#      FALCON_CASE_*, FALCON_TEST_*, FALCON_TUI_*, FALCON_KEEP,
#      FALCON_UDFEDIT_KEEP, MAKEFLAGS
#                                   passed through: the cases', tests' and
#                                   rigs' switches and the Makefile's flags.
#                                   Not every FALCON_*: a tag pipeline's
#                                   signing and deploy keys are FALCON_*;
#      and for a pipeline job:
#      FALCON_HARNESS_NAME          the container's name, so a cancelled job
#                                   can remove the container it left running
#      FALCON_DOCKER_CGROUP_PARENT  the cgroup the container runs under; the
#                                   daemon starts it, so the job's own
#                                   cgroup does not reach it
#      FALCON_FIXTURES              a directory mounted read-only at
#                                   /fixtures: the rigs' disk images
#                                   (tools/fixtures/README.md)
#      ARCH_ISO, FALCON_TEST_BOOTWIM, A21M_ORIG, W98_IOS_CF
#                                   passed through (tools/arch-iso.sh,
#                                   falcon-core/tests/wim_test.c, the
#                                   A21m and CF station rigs)
#
# The case sticks live apart from the build directory on the binhost because
# they are most of its size (about 200 GB for the corpus) and the work disk is
# shared with the binhost's build roots. They need a local disk: every case
# reads its stick. A symlink in their place would dangle inside the container,
# whose build/ is a bind mount, so the directory is mounted instead.

set -euo pipefail

cd "$(dirname "$0")/../.."
REPO=$PWD

HARNESS_REPO="${FALCON_HARNESS_REPO:-registry.git.airies.net:443/vifair22/falcon-boot/harness}"
IMAGE="${FALCON_HARNESS_IMAGE:-$HARNESS_REPO:$(sha256sum tools/coverage/Dockerfile | cut -c1-12)}"
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
    echo "ERROR: docker is not on PATH (FALCON_NATIVE=1 builds with the host's toolchain)" >&2
    exit 1; }
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

if [[ $BUILD -eq 0 ]] && ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "pulling $IMAGE" >&2
    docker pull -q "$IMAGE" >/dev/null ||
        { echo "harness: $IMAGE could not be pulled; building it" \
            "(docker login ${IMAGE%%/*} fetches it next time, once a" \
            "pipeline has published this Dockerfile)" >&2; BUILD=1; }
fi
if [[ $BUILD -eq 1 ]]; then
    echo "building $IMAGE" >&2
    docker build -t "$IMAGE" -f tools/coverage/Dockerfile tools/coverage
    # Every edit to the Dockerfile's package list makes a new apt layer of
    # about 3 GB and leaves the old one in the build cache, where nothing
    # else removes it. The filter is host-wide, which is the binhost's own
    # policy for its build cache; records newer than two days survive, so a
    # rebuild that follows soon after still finds its layers.
    docker builder prune -f --filter until=48h >/dev/null ||
        echo "warning: could not prune the docker build cache" >&2
fi
# Whatever the local copy came from (this build, an earlier one, a hand-run
# harness on the same daemon), the registry is what workstations pull.
if [[ ${FALCON_HARNESS_PUSH:-} == 1 ]] &&
    ! docker manifest inspect "$IMAGE" >/dev/null 2>&1; then
    echo "pushing $IMAGE" >&2
    docker push -q "$IMAGE"
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
MOUNTS=(-v "$REPO:$REPO" -v /dev:/dev)
[[ -d $CORPUS ]] && MOUNTS+=(-v "$CORPUS:/corpus:ro")
[[ -n ${FALCON_FIXTURES:-} ]] && MOUNTS+=(-v "$FALCON_FIXTURES:/fixtures:ro")
# A bind target that does not exist is made by the daemon, as root, and the
# hand-back below sees the mount, not the directory under it; so each target
# is made here first, as the caller.
if [[ -n $BUILD_DIR ]]; then
    mkdir -p "$BUILD_DIR" "$REPO/build"
    MOUNTS+=(-v "$BUILD_DIR:$REPO/build")
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
    mkdir -p "$STICKS_DIR" "$BUILD_DIR/coverage/sticks"
    MOUNTS+=(-v "$STICKS_DIR:$REPO/build/coverage/sticks")
fi

# Interactive only when there is a terminal, so the same line works from a
# shell and from a pipeline job.
TTY=()
[[ -t 0 && -t 1 ]] && TTY=(-it)

# The daemon starts the container, so a job's cgroup and a cancelled job's
# signals both stop at the docker client; these carry them across.
NAME=${FALCON_HARNESS_NAME:-falcon-harness-$$}
RUNOPTS=(--name "$NAME")
[[ -n ${FALCON_DOCKER_CGROUP_PARENT:-} ]] &&
    RUNOPTS+=(--cgroup-parent "$FALCON_DOCKER_CGROUP_PARENT")

ENVS=(-e FALCON_HARNESS=1 -e FALCON_CORPUS=/corpus)
for v in ARCH_ISO A21M_ORIG W98_IOS_CF VARIANT MAKEFLAGS FALCON_KEEP FALCON_UDFEDIT_KEEP \
    $(compgen -e | grep -E '^FALCON_(CASE|TEST|TUI)_' || true); do
    [[ -n ${!v+set} ]] && ENVS+=(-e "$v")
done

CMD=("$@")
[[ ${#CMD[@]} -eq 0 ]] && CMD=(tools/coverage/run-corpus.sh)
# The container runs as root (losetup); what it wrote in the repository and
# the build directory is handed back to the caller afterwards, whatever the
# command's exit status. Only root's files: the case sticks and the rest are
# someone's already.
CMDQ=$(printf '%q ' "${CMD[@]}")
REPOQ=$(printf '%q' "$REPO")
GIVEBACK="find $REPOQ -path $REPOQ/.git -prune -o -uid 0 -print0 | xargs -0r chown -h $(id -u):$(id -g)"

RUN=(docker run --rm "${TTY[@]}" \
    --privileged "${KVM[@]}" "${RUNOPTS[@]}" \
    "${MOUNTS[@]}" "${ENVS[@]}" \
    -w "$REPO" \
    "$IMAGE" bash -c "$CMDQ; rc=\$?; $GIVEBACK; exit \$rc")

# At a terminal the client owns it, and ^C reaches the container through it.
# Without one (a pipeline job), the container's bash is PID 1 and ignores
# the TERM a cancelled job sends, so the run would go on to its end; the
# container is removed instead. What it wrote is then root's, which the
# pipeline's cleanup job deletes through the harness.
[[ ${#TTY[@]} -gt 0 ]] && exec "${RUN[@]}"
trap 'echo "harness: removing $NAME" >&2; docker rm -f "$NAME" >/dev/null; exit 143' TERM INT HUP
"${RUN[@]}" &
wait $!
