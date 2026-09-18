#!/usr/bin/env bash
# falcon-hook: boot one ISO's own kernel with the hook, without the loader
# (docs/notes/coverage-roadmap.md M1).
#
# The rig the M1 exit is measured in. OVMF starts the ISO's kernel directly
# (QEMU -kernel/-initrd), with:
#
#   initrd   the ISO's own initrd(s), padded to 4 bytes, then a cpio holding
#            build/bin/falcon-hook at /falcon/hook: what the loader appends
#   cmdline  exactly what the caller gives, plus rdinit=/falcon/hook and a
#            serial console
#   NVRAM    FalconIsoMap for the ISO on the case stick's p2, computed by
#            falcon-hook-mkextmap with the loader's own GPT and exFAT code
#   disk     the coverage harness's case stick (tools/coverage/
#            mk-case-stick.sh), which holds the ISO as a file on exFAT and
#            nothing else that could find it
#
# A PASS is a login prompt on the serial console: the distro found its root
# through the device the hook built.
#
# Usage: falcon-hook/tools/run-hook-rig.sh ID KERNEL INITRD... -- CMDLINE
#   ID       a corpus id (tools/coverage/corpus.tsv) whose case stick exists
#   KERNEL   path inside the ISO; INITRD... likewise, in load order
# Env: TIMEOUT (seconds, default 240), FALCON_CORPUS

set -euo pipefail

cd "$(dirname "$0")/../.."

[[ $# -ge 4 ]] || { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
ID=$1 KERNEL=$2
shift 2
INITRDS=()
while [[ $# -gt 0 && "$1" != "--" ]]; do INITRDS+=("$1"); shift; done
[[ "${1:-}" == "--" ]] || { echo "ERROR: missing -- CMDLINE" >&2; exit 1; }
CMDLINE=${2:-}

CORPUS="${FALCON_CORPUS:-/mnt/falcon-corpus/corpus}"
FILE=$(awk -F'\t' -v id="$ID" '$1 == id { print $5 }' tools/coverage/corpus.tsv)
[[ -n "$FILE" ]] || { echo "ERROR: $ID is not in the corpus" >&2; exit 1; }
ISO="$CORPUS/$FILE"
STICK="build/coverage/sticks/$ID.img"
[[ -f "$STICK" ]] || { echo "ERROR: $STICK missing (run-corpus.sh builds it)" >&2; exit 1; }
make -s hook build/bin/falcon-hook-mkextmap >/dev/null

W="build/coverage/hook-rig/$ID"
mkdir -p "$W/cpio/falcon"
build/bin/falcon-hook-mkextmap "$STICK" "$FILE" > "$W/map.json"

# The ISO's files, read with the sign tool's reader rather than bsdtar,
# which extracts Debian's hard-linked kernel as an empty file.
python3 - "$ISO" "$W" "$KERNEL" "${INITRDS[@]}" <<'PY'
import subprocess, sys
iso, w, kernel, *initrds = sys.argv[1:]
def extract(member, out):
    subprocess.run(['bsdtar', '-xOf', iso, member.lstrip('/')], stdout=open(out, 'wb'),
                   check=True)
extract(kernel, w + '/kernel')
data = b''
for i, r in enumerate(initrds):
    extract(r, w + '/part')
    data += open(w + '/part', 'rb').read()
open(w + '/initrd.orig', 'wb').write(data)
PY
cp build/bin/falcon-hook "$W/cpio/falcon/hook"
(cd "$W/cpio" && printf 'falcon\nfalcon/hook\n' | cpio -o -H newc --owner=0:0 2>/dev/null) > "$W/hook.cpio"
python3 -c "
import sys
a = open('$W/initrd.orig', 'rb').read(); h = open('$W/hook.cpio', 'rb').read()
open('$W/initrd', 'wb').write(a + b'\0' * ((-len(a)) % 4) + h)"

. falcon-efi/tools/ovmf.sh
cp -f "$(ovmf_find VARS plain)" "$W/vars.fd"
chmod u+w "$W/vars.fd"
virt-fw-vars --inplace "$W/vars.fd" --set-json "$W/map.json" >/dev/null 2>&1

timeout "${TIMEOUT:-240}" qemu-system-x86_64 -machine q35 -enable-kvm -cpu host \
    -m 4096 -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file="$(ovmf_find CODE plain)" \
    -drive if=pflash,format=raw,file="$W/vars.fd" \
    -drive if=none,id=stick,format=raw,file="$STICK",snapshot=on \
    -device ahci,id=ahci -device ide-hd,drive=stick,bus=ahci.0 \
    -netdev user,id=n0 -device e1000e,netdev=n0,romfile= \
    -kernel "$W/kernel" -initrd "$W/initrd" \
    -append "$CMDLINE rdinit=/falcon/hook console=ttyS0,115200" \
    -display none -serial file:"$W/serial.log" -monitor none \
    >/dev/null 2>&1 || true

sed -e 's/\x1b\[[0-9;]*m//g' "$W/serial.log" | grep -a -E 'falcon-hook:' || true
if sed -e 's/\x1b\[[0-9;]*m//g' "$W/serial.log" | grep -a -q -E ' login: *$|^[a-zA-Z0-9-]+ login:'; then
    echo "PASS $ID: login prompt"
else
    echo "FAIL $ID: no login prompt (serial: $W/serial.log)"
    exit 1
fi
