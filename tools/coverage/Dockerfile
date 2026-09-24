# falcon-boot: the coverage harness's environment
# (docs/notes/coverage-roadmap.md Section 2, tools/coverage/run-corpus.sh).
#
# One image that builds the tree and boots the corpus under OVMF. It exists so
# the measurement is not tied to whichever machine the stick last booted on:
# docs/coverage.md is evidence, and evidence produced on one laptop with an
# undeclared toolchain is evidence nobody else can reproduce.
#
# Debian trixie because binhost-builder runs trixie, so the container's kernel
# interfaces (loop, dm, kvm) are the ones its packages were built against.
#
# It runs as root and still installs sudo, which looks redundant and is not:
# mk-case-stick.sh calls `sudo losetup`, and a shell where sudo is missing
# fails differently from one where it is a no-op. Installing the real thing
# keeps the scripts identical whether they run here or on a workstation.
#
# Run it privileged. losetup needs it, and the alternative (SYS_ADMIN plus a
# hand-maintained list of loop devices) buys nothing here: this image exists to
# run one repository's test harness on a machine we own.
FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive

# Grouped by the job each package does, because "why is xorriso in here" is the
# question a reader of this file actually has. The command each one provides is
# named when the package name does not give it away.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git make python3 rsync sudo \
        build-essential binutils pkg-config xxd \
        gcc-multilib \
        gcc-mingw-w64-x86-64 \
        libcmocka-dev \
        qemu-system-x86 qemu-utils ovmf \
        tcpdump \
        python3-venv python3-dev libcrypt-dev \
        gdisk util-linux \
        dosfstools exfatprogs e2fsprogs mtools udftools \
        cryptsetup-bin openssl sbsigntool \
        systemd-ukify systemd-boot-efi \
        wimtools 7zip libarchive-tools xorriso cpio zstd \
        socat aria2 curl \
        cppcheck clang-tidy \
    && rm -rf /var/lib/apt/lists/*

# check-tools looks for 7z or 7zz; trixie's 7zip package installs only 7zz.
RUN ln -sf /usr/bin/7zz /usr/local/bin/7z

# virt-firmware from PyPI rather than trixie's python3-virt-firmware.
#
# Not a preference for pip. Debian ships 24.11, which has neither
# --enroll-microsoft (mk-test-mok.sh, to make a Secure Boot varstore out of a
# blank one) nor --secure-boot (states.sh, building the `own` state). The
# second matters most: states.sh runs it under `>/dev/null 2>&1`, so on 24.11
# it would leave a setup-mode varstore behind without a word, and a setup-mode
# varstore enforces nothing - every case in the `own` column would pass and
# the column would mean nothing.
#
# Pinned to the version the work was measured against, for the reason any
# other rig input is pinned: a harness whose tools move is a harness whose
# results cannot be compared.
# python3-dev and libcrypt-dev above are for this step alone: virt-firmware
# pulls crypt_r, a compiled backport of the crypt module Python 3.13 removed,
# and it has no wheel for trixie's interpreter.
ARG VIRT_FIRMWARE_VERSION=26.9
RUN python3 -m venv /opt/virt-firmware \
    && /opt/virt-firmware/bin/pip -q --no-cache-dir install \
        "virt-firmware==${VIRT_FIRMWARE_VERSION}" \
    && ln -sf /opt/virt-firmware/bin/virt-fw-vars /usr/local/bin/virt-fw-vars

# Fail the build rather than the first corpus run.
#
# REGENERATE THIS LIST, DO NOT EXTEND IT FROM MEMORY. It has been wrong three
# times, and each time the symptom was not "command not found" but a silently
# degraded result. From the repository root:
#
#   grep -hoE '^[[:space:]]*(if +)?[a-z][a-z0-9_.-]{2,}' \
#       tools/coverage/*.sh falcon-efi/tools/*.sh falcon-core/tools/*.sh |
#       sed 's/^[[:space:]]*//; s/^if //' | sort -u
#
# then drop the shell builtins and keep what `command -v` finds on a working
# machine. tcpdump is the one that proves the point: run-case.sh calls it
# unguarded for one of its four PASS conditions, so without it every case
# whose only evidence is the DHCP capture ran to the 300 second timeout and
# was recorded FAIL. 34 of them, on the first corpus run here.
#
# Two entries here are scar tissue, both from the same failure mode: a build
# step that shells out, swallows the error and yields an empty result.
#
#   pkg-config  the root Makefile sets CMOCKA_LIBS from
#               `pkg-config --libs cmocka 2>/dev/null`. Without it there is no
#               configure error, just an empty variable and several hundred
#               undefined references to `_assert_int_equal` at link time. So
#               the binary is not enough; the query has to answer.
#   xxd         the loader embeds falcon-hook and falcon-winjump as generated
#               C arrays. Without it the recipe prints "xxd: not found",
#               carries on, and the loader fails to link on
#               `falcon_hook_bin`, four steps and several minutes later.
RUN set -eu; \
    missing=0; \
    for t in x86_64-w64-mingw32-gcc qemu-system-x86_64 sgdisk losetup blkid \
             openssl cryptsetup sbsign sbverify mkfs.fat mkfs.exfat \
             mkfs.ext4 mkudffs mformat bsdtar ukify wimlib-imagex xorriso \
             cpio zstd socat aria2c readelf sha256sum virt-fw-vars 7z sudo \
             pkg-config xxd tcpdump qemu-img mcopy mmd fsck.ext4 truncate \
             cmp find xargs; do \
        command -v "$t" >/dev/null 2>&1 || { echo "MISSING: $t"; missing=1; }; \
    done; \
    test -n "$(pkg-config --libs cmocka)" || { \
        echo "MISSING: pkg-config cannot answer for cmocka"; missing=1; }; \
    test -f /usr/lib/systemd/boot/efi/linuxx64.efi.stub || { \
        echo "MISSING: sd-stub; every blessed stick would fail to build"; \
        missing=1; }; \
    for f in --enroll-microsoft --secure-boot --add-mok --set-pk; do \
        virt-fw-vars --help 2>&1 | grep -q -- "$f" || { \
            echo "MISSING: virt-fw-vars has no $f"; missing=1; }; \
    done; \
    test "$missing" -eq 0

# Where harness.sh mounts things. Declared so a `docker run` that forgets one
# fails on an empty directory instead of writing into the image.
#
# /work rather than /src: tools/analyze.sh recognises the repository's own
# src/ directories by matching "/src/" in an absolute path, and a tree
# mounted at /src makes every path match, which puts the whole test suite
# into a gate built to exclude it. harness.sh says more.
VOLUME ["/work", "/corpus"]
WORKDIR /work

CMD ["/bin/bash"]
