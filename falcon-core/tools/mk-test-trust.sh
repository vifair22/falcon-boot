#!/usr/bin/env bash
# falcon-boot: the certificates and signed images the boot-prediction code
# (src/secboot, DESIGN Section 7.8) is tested against.
#
# Every trust relationship the verdict has to get right, built from keys made
# here so the expected answer is known by construction:
#
#   root.der, inter.der   a two-level CA with key identifiers, the shape of
#                         Microsoft's hierarchy
#   leaf.der              code-signing leaf issued by inter
#   dba.der, dbb.der      two DIFFERENT keys with the SAME Name and no
#                         extensions at all: sbctl's "Database Key" exactly,
#                         and the case a name-only match gets wrong
#   mok.der               a self-signed MOK
#
#   unsigned.efi          the loader as built, no certificate table
#   leaf.efi              signed by leaf, inter included in the SignedData
#   leaf-bare.efi         signed by leaf, inter NOT included: reaches an inter
#                         or root anchor only through the name rule
#   dba.efi, dbb.efi      signed by each sbctl-shaped key
#   mok.efi               signed by the MOK
#   dual.efi              dba first, then leaf appended: two WIN_CERTIFICATE
#                         entries, the shape of the stick's own shim
#
# The PE is build/bin/falcon-boot.efi because it is a real mingw image with a
# .sbat section and, usefully, the gaps between sections that make sbsign warn
# "data remaining": the one layout where the Authenticode hash's tail rule
# matters.
#
# When the shim-spike binaries and the secboot OVMF varstore are present, the
# Microsoft certificates are extracted from the varstore's db into ms/ so the
# real-world group can run a distro shim against them. Their absence skips
# that group only.
#
# Needs openssl and sbsign; virt-fw-vars for the optional ms/ set. No sudo.

set -euo pipefail

cd "$(dirname "$0")/../.."

OUT=build/test-trust
PE=build/bin/falcon-boot.efi

# Where the real-world set's Microsoft certificates are extracted from.
#
# mk-test-mok.sh's varstore is preferred because a script in this repository
# produces it on demand, and it carries Microsoft's PK/KEK/db whether or not
# the distro's pristine varstore did. build/OVMF_VARS.secboot.fd is the older
# source and stays as the fallback, but it is a side effect of having run the
# interactive rig at some point, so a fresh machine never has it and the
# real-world group silently never ran there.
VARS=build/mok-test/OVMF_VARS.secboot.mok.fd
[ -f "$VARS" ] || VARS=build/OVMF_VARS.secboot.fd

for t in openssl sbsign; do
    command -v "$t" >/dev/null || { echo "ERROR: $t not found" >&2; exit 1; }
done
[ -f "$PE" ] || { echo "ERROR: $PE missing; run make first" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

ossl() { openssl "$@" 2>/dev/null; }

# A CA or leaf with openssl's default key identifiers, issued by $3 (or
# self-signed when $3 is empty). $4 is the extension section to apply.
cat > "$W/ext.cnf" <<'EOF'
[ca]
basicConstraints = critical, CA:TRUE
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[leaf]
basicConstraints = CA:FALSE
extendedKeyUsage = codeSigning
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[none]
subjectKeyIdentifier = none
authorityKeyIdentifier = none
EOF

mk() {
    local name=$1 subj=$2 issuer=$3 section=$4

    ossl req -new -newkey rsa:2048 -nodes -keyout "$W/$name.key" \
        -subj "$subj" -out "$W/$name.csr"
    if [ -z "$issuer" ]; then
        ossl x509 -req -in "$W/$name.csr" -signkey "$W/$name.key" \
            -days 3650 -extfile "$W/ext.cnf" -extensions "$section" \
            -out "$W/$name.crt"
    else
        ossl x509 -req -in "$W/$name.csr" -CA "$W/$issuer.crt" \
            -CAkey "$W/$issuer.key" -CAcreateserial -days 3650 \
            -extfile "$W/ext.cnf" -extensions "$section" -out "$W/$name.crt"
    fi
    ossl x509 -in "$W/$name.crt" -outform der -out "$OUT/$name.der"
}

mk root  "/CN=Falcon Test Root"          ""     ca
mk inter "/CN=Falcon Test Intermediate"  root   ca
mk leaf  "/CN=Falcon Test Signer"        inter  leaf
mk dba   "/CN=Database Key"              ""     none
mk dbb   "/CN=Database Key"              ""     none
mk mok   "/CN=falcon-boot TEST MOK"      ""     ca

sign() {
    local out=$1 in=$2 key=$3; shift 3

    sbsign --key "$W/$key.key" --cert "$W/$key.crt" "$@" \
        --output "$OUT/$out" "$in" >/dev/null 2>&1
}

cp "$PE" "$OUT/unsigned.efi"
sign leaf.efi      "$PE" leaf --addcert "$W/inter.crt"
sign leaf-bare.efi "$PE" leaf
sign dba.efi       "$PE" dba
sign dbb.efi       "$PE" dbb
sign mok.efi       "$PE" mok
sign dual.efi      "$OUT/dba.efi" leaf --addcert "$W/inter.crt"

# The real-world set, when the rig has it.
if command -v virt-fw-vars >/dev/null && [ -f "$VARS" ]; then
    mkdir -p "$OUT/ms" "$W/ms"
    (cd "$W/ms" && virt-fw-vars -i "$OLDPWD/$VARS" --extract-certs \
        >/dev/null 2>&1) || true
    for pem in "$W"/ms/db-*.pem; do
        [ -e "$pem" ] || continue
        base=$(basename "$pem" .pem)
        ossl x509 -in "$pem" -outform der -out "$OUT/ms/${base##*-}.der"
    done
fi

echo "test-trust: $(ls "$OUT" | wc -l) files in $OUT"

# Guarded rather than `ls ... 2>/dev/null`, which hides the message and keeps
# the status: ls still exits 2, it is the last command here, and so the whole
# target failed on every machine without the rig's varstore even though all
# thirteen fixtures had been written. Found on binhost-builder 2026-09-19.
if [ -d "$OUT/ms" ]; then
    ls "$OUT/ms" | sed 's/^/  ms\//'
fi
