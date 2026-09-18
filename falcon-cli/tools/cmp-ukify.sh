#!/usr/bin/env bash
# falcon-boot: check that falcon-boot-sign's UKI builder still matches ukify.
#
# falcon-cli/src/sign/uki.c replaced ukify in the bless path and in the
# appliance build, on the claim that its output is byte-identical to ukify's
# for the same inputs. This is that claim, as a check to repeat whenever
# systemd ships a new ukify or stub, or uki.c changes. It is a development
# tool: nothing in the build depends on ukify any more, only this.
#
# Each kernel is wrapped four ways (with and without an initrd, each with a
# short and a long cmdline) by both builders, and the outputs are compared
# with cmp. Every kernel given should be an EFI-stub bzImage; the kernels
# inside the ISOs on a falcon stick's p2 are the useful set, and the script
# extracts them itself when given ISOs.
#
# Usage: falcon-cli/tools/cmp-ukify.sh <kernel-or-iso>...
# Env:
#   STUB   sd-stub to wrap with (default: systemd-boot's x64 stub)

set -euo pipefail

cd "$(dirname "$0")/../.."

SIGN=build/bin/falcon-boot-sign
STUB="${STUB:-/usr/lib/systemd/boot/efi/linuxx64.efi.stub}"

[[ $# -gt 0 ]] || { echo "usage: $0 <kernel-or-iso>..." >&2; exit 2; }
command -v ukify >/dev/null || { echo "ERROR: ukify not found" >&2; exit 1; }
[[ -x "$SIGN" ]] || { echo "ERROR: $SIGN not built (run: make cli)" >&2; exit 1; }
[[ -f "$STUB" ]] || { echo "ERROR: no stub at $STUB" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'chmod -R u+w "$WORK" 2>/dev/null; rm -rf "$WORK"' EXIT

printf 'NAME="cmp-ukify"\nID=cmp-ukify\nPRETTY_NAME="cmp-ukify check"\n' \
    > "$WORK/osrel"
head -c 3000000 /dev/urandom > "$WORK/initrd"
LONG="root=live:CDLABEL=x rd.live.image quiet splash $(printf 'opt%d=1 ' $(seq 1 300))"

# The kernels inside an ISO, found the way a human would: vmlinuz*, bzImage,
# or Gentoo's boot/gentoo, anywhere in the ISO9660 tree.
kernels_of() {
    local iso="$1" out="$2" m
    bsdtar -tf "$iso" 2>/dev/null |
        grep -E '(^|/)(vmlinuz[^/]*|bzImage|boot/gentoo)$' | head -n 2 |
        while IFS= read -r m; do
            mkdir -p "$out"
            bsdtar -x -f "$iso" -C "$out" "$m"
            printf '%s\n' "$out/$m"
        done
}

KERNELS=()
for a in "$@"; do
    case "$a" in
        *.iso) mapfile -t -O "${#KERNELS[@]}" KERNELS < \
                   <(kernels_of "$a" "$WORK/iso$RANDOM") ;;
        *)     KERNELS+=("$a") ;;
    esac
done
[[ ${#KERNELS[@]} -gt 0 ]] || { echo "ERROR: no kernels found" >&2; exit 1; }

# No distro kernel met so far carries its own .sbat, so the merge of a
# kernel's generations into the stub's is exercised on a copy of the first
# kernel with one added. It need not boot; only the layout is compared.
if command -v objcopy >/dev/null; then
    printf 'sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\nlinux,1,Test,linux,6.0,https://example.invalid\n' \
        > "$WORK/kernel.sbat"
    if objcopy --add-section .sbat="$WORK/kernel.sbat" \
            --set-section-flags .sbat=data,readonly \
            "${KERNELS[0]}" "$WORK/with-sbat" 2>/dev/null; then
        KERNELS+=("$WORK/with-sbat")
    else
        echo "note: objcopy could not add a .sbat; that case is not compared" >&2
    fi
fi

fail=0
n=0
for k in "${KERNELS[@]}"; do
    for cmd in "quiet" "$LONG"; do
        for rd in "" "$WORK/initrd"; do
            n=$(( n + 1 ))
            label="${k#"$WORK"/} (cmdline ${#cmd}, initrd $([[ -n $rd ]] && echo yes || echo no))"
            ref="$WORK/ref.efi"
            ours="$WORK/ours.efi"
            ukify build --linux="$k" --cmdline="$cmd" --stub="$STUB" \
                --os-release="@$WORK/osrel" ${rd:+--initrd="$rd"} \
                --output="$ref" >/dev/null 2>&1
            "$SIGN" uki --linux "$k" --cmdline "$cmd" --stub "$STUB" \
                --os-release "$WORK/osrel" ${rd:+--initrd "$rd"} \
                --output "$ours"
            if cmp -s "$ref" "$ours"; then
                echo "OK    $label"
            else
                echo "FAIL  $label"
                cmp -l "$ref" "$ours" | head -n 5
                fail=1
            fi
        done
    done
done

echo
[[ $fail -eq 0 ]] && echo "PASS: $n builds byte-identical to $(ukify --version)" \
                  || { echo "FAIL: output differs from $(ukify --version)"; exit 1; }
