# falcon-boot: the coverage harness's environment
# (tools/coverage/run-corpus.sh, tools/coverage/scorecard.py).
#
# One image that builds the tree and boots the corpus under OVMF. It exists so
# the measurement is not tied to whichever machine the stick last booted on:
# docs/coverage.md is evidence, and evidence produced on one laptop with an
# undeclared toolchain is evidence nobody else can reproduce.
#
# Debian trixie because the build host runs trixie, so the container's kernel
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
# arm64 as a foreign architecture, for the few arm64 packages below: the
# libc and libgcc a static arm64 program links (the fixture UKI's init),
# arm64 kernel headers (falcon-hook's AArch64 build), and systemd-boot's
# arm64 stub. Debian's cross gcc for arm64 conflicts with gcc-multilib,
# which the BIOS loader needs; llvm-mingw's clang and ld.lld below link
# against these instead.
RUN dpkg --add-architecture arm64
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates git make python3 sudo \
        build-essential binutils pkg-config xxd \
        gcc-multilib \
        gcc-mingw-w64-x86-64 gcc-mingw-w64-i686 \
        libcmocka-dev \
        qemu-system-x86 qemu-utils ovmf swtpm swtpm-tools imagemagick \
        qemu-system-arm qemu-efi-aarch64 xz-utils \
        libc6-dev:arm64 libgcc-14-dev:arm64 systemd-boot-efi:arm64 \
        tcpdump \
        python3-venv python3-dev libcrypt-dev \
        gdisk util-linux fdisk \
        dosfstools exfatprogs e2fsprogs mtools udftools ntfs-3g \
        cryptsetup-bin openssl sbsigntool \
        systemd-ukify systemd-boot-efi \
        wimtools 7zip libarchive-tools xorriso cpio zstd \
        socat aria2 curl \
        cppcheck clang-tidy gcovr \
        ninja-build libglib2.0-dev libpixman-1-dev \
    && rm -rf /var/lib/apt/lists/*

# swtpm-tools is for swtpm_setup, which the TPM rig's totp cases make their
# TPM state with; without it they skip.
# fdisk is for sfdisk, which mk-test-biosboot.sh partitions its fixtures with;
# trixie moved it out of util-linux.
# ntfs-3g is for ntfsresize and ntfsfix, which mk-a21m-station.sh takes the
# A21m's NTFS partition down with before station-legacy goes on.
# gcovr is `make coverage`'s report.
# ninja-build, libglib2.0-dev and libpixman-1-dev are what QEMU's own build
# needs (it brings its meson), for falcon-bios/tools/qemu-pcc/build-qemu.sh:
# the jump rig's cf cases run on a QEMU with falcon-pcc.c built in.

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

# llvm-mingw for the AArch64 UEFI images and the ARM64 winjump. Debian has
# no aarch64-w64-mingw32 toolchain, and plain clang with lld cannot build
# winjump, which needs a mingw CRT. Its clang and ld.lld also build the
# arm64 Linux programs (falcon-hook, the fixture init) against the arm64
# packages above. A pinned upstream release, checked
# against the digest its release page publishes; the x64 images stay on
# Debian's gcc-mingw. xz-utils above unpacks it.
ARG LLVM_MINGW_VERSION=20261020
ARG LLVM_MINGW_SHA256=55064b60b991a1e1e8090b8c2f84e82b22cfe5903d12c159d65064cf5e8ae335
RUN set -eu; \
    t=llvm-mingw-${LLVM_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64; \
    curl -fsSL -o /tmp/$t.tar.xz \
        https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VERSION}/$t.tar.xz; \
    echo "${LLVM_MINGW_SHA256}  /tmp/$t.tar.xz" | sha256sum -c -; \
    tar -xJf /tmp/$t.tar.xz -C /opt; \
    mv /opt/$t /opt/llvm-mingw; \
    rm /tmp/$t.tar.xz
# Last on PATH: llvm-mingw also installs x86_64- and i686-w64-mingw32-gcc,
# clang wrappers that would otherwise take the x64 and BIOS-side builds off
# Debian's gcc-mingw.
ENV PATH=$PATH:/opt/llvm-mingw/bin

# Fail the build rather than the first corpus run.
#
# REGENERATE THIS LIST, DO NOT EXTEND IT FROM MEMORY. A hand-extended list
# degrades silently: the symptom is not "command not found" but a silently
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
# was recorded FAIL.
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
    case "$(command -v x86_64-w64-mingw32-gcc)" in /usr/bin/*) ;; \
        *) echo "MISSING: x86_64-w64-mingw32-gcc is not Debian's gcc-mingw"; \
           missing=1 ;; esac; \
    for t in x86_64-w64-mingw32-gcc aarch64-w64-mingw32-clang \
             clang ld.lld llvm-strip \
             qemu-system-x86_64 qemu-system-aarch64 sgdisk losetup blkid \
             openssl cryptsetup sbsign sbverify mkfs.fat mkfs.exfat \
             mkfs.ext4 mkudffs mformat bsdtar ukify wimlib-imagex xorriso \
             cpio zstd socat aria2c readelf sha256sum virt-fw-vars 7z sudo \
             pkg-config xxd tcpdump qemu-img mcopy mmd fsck.ext4 truncate \
             cmp find xargs swtpm swtpm_setup sfdisk ntfsresize ntfsfix \
             gcovr ninja; do \
        command -v "$t" >/dev/null 2>&1 || { echo "MISSING: $t"; missing=1; }; \
    done; \
    test -n "$(pkg-config --libs cmocka)" || { \
        echo "MISSING: pkg-config cannot answer for cmocka"; missing=1; }; \
    test -f /usr/lib/systemd/boot/efi/linuxaa64.efi.stub || { \
        echo "MISSING: the arm64 sd-stub; an arm64 kernel cannot be wrapped"; \
        missing=1; }; \
    test -f /usr/lib/systemd/boot/efi/linuxx64.efi.stub || { \
        echo "MISSING: sd-stub; every blessed stick would fail to build"; \
        missing=1; }; \
    test -f /usr/include/aarch64-linux-gnu/asm/unistd.h || { \
        echo "MISSING: arm64 kernel headers; falcon-hook-a64 cannot build"; \
        missing=1; }; \
    test -f /usr/share/AAVMF/AAVMF_CODE.fd || { \
        echo "MISSING: AAVMF; the AArch64 rigs have no firmware"; missing=1; }; \
    for f in --enroll-microsoft --secure-boot --add-mok --set-pk; do \
        virt-fw-vars --help 2>&1 | grep -q -- "$f" || { \
            echo "MISSING: virt-fw-vars has no $f"; missing=1; }; \
    done; \
    test "$missing" -eq 0

# Where harness.sh mounts the corpus. Declared so a `docker run` that forgets
# it fails on an empty directory instead of writing into the image. The
# repository is mounted at its host path, which harness.sh also makes the
# working directory.
VOLUME ["/corpus"]

CMD ["/bin/bash"]
