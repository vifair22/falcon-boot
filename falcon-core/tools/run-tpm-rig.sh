#!/usr/bin/env bash
# falcon-core/src/tpm against swtpm, TPM 2.0 then TPM 1.2
# (DESIGN Section 12.15).
#
# Each version gets a fresh swtpm with its state under build/tpm-rig/:
#
#   2.0  a new state; swtpm manufactures the seeds on first start and sends
#        TPM2_Startup itself (--flags startup-clear)
#   1.2  swtpm_setup first: an EK, ownership taken with the well-known owner
#        and SRK secrets (20 zero bytes), and NV locked, which is what a
#        PC's TPM looks like and what makes a 1.2 NV area's PCR gate hold
#
# then falcon-tpm-rig runs the whole flow against its unixio socket and
# judges it. No host TPM is touched.
#
# Usage: falcon-core/tools/run-tpm-rig.sh [--record FILE] RIG
#   RIG      build/bin/falcon-tpm-rig
#   --record re-record the exchanges the unit suite replays into FILE
#            (falcon-core/tests/tpm_transcripts.h)

set -euo pipefail

cd "$(dirname "$0")/../.."

RECORD=
if [[ "${1:-}" == "--record" ]]; then
    RECORD=$2
    shift 2
fi
[[ $# -eq 1 ]] || { sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
RIG=$1

if ! command -v swtpm >/dev/null || ! command -v swtpm_setup >/dev/null; then
    echo "test-tpm: skipped, swtpm and swtpm_setup are not installed."
    exit 0
fi

DIR=build/tpm-rig
rm -rf "$DIR"
mkdir -p "$DIR/tpm2" "$DIR/tpm12"

PIDS=()
cleanup() {
    local p
    for p in "${PIDS[@]}"; do
        kill "$p" 2>/dev/null || true
    done
}
trap cleanup EXIT

# start_swtpm STATE SOCKET [--tpm2]: a daemon on a unixio socket.
start_swtpm() {
    local state=$1 sock=$2
    shift 2
    swtpm socket "$@" --tpmstate dir="$state" \
        --server type=unixio,path="$sock" \
        --ctrl type=unixio,path="$sock.ctrl" \
        --flags not-need-init,startup-clear \
        --log file="$state/swtpm.log" \
        --pid file="$state/swtpm.pid" -d
    for _ in $(seq 50); do
        [[ -S "$sock" && -s "$state/swtpm.pid" ]] && break
        sleep 0.1
    done
    [[ -S "$sock" ]] || { echo "ERROR: swtpm did not start ($state)" >&2; exit 1; }
    PIDS+=("$(cat "$state/swtpm.pid")")
}

rc=0

start_swtpm "$DIR/tpm2" "$DIR/tpm2/sock" --tpm2
if [[ -n "$RECORD" ]]; then
    "$RIG" --tpm2 "$DIR/tpm2/sock" --record "$DIR/tpm2.h" --prefix t2_ || rc=1
else
    "$RIG" --tpm2 "$DIR/tpm2/sock" || rc=1
fi

swtpm_setup --tpmstate "$DIR/tpm12" --take-ownership --owner-well-known \
    --srk-well-known --lock-nvram --logfile "$DIR/tpm12/setup.log" >/dev/null
start_swtpm "$DIR/tpm12" "$DIR/tpm12/sock"
if [[ -n "$RECORD" ]]; then
    "$RIG" --tpm12 "$DIR/tpm12/sock" --record "$DIR/tpm12.h" --prefix t12_ || rc=1
else
    "$RIG" --tpm12 "$DIR/tpm12/sock" || rc=1
fi

if [[ -n "$RECORD" && $rc -eq 0 ]]; then
    {
        echo "/* Exchanges falcon-tpm-rig recorded against swtpm $(swtpm --version | sed -n 's/.*version \([0-9.]*\).*/\1/p' | head -n 1),"
        echo " * both TPM versions, with its deterministic randomness. Replayed by"
        echo " * falcon-core/tests/tpm_test.c. Regenerate with make tpm-transcripts;"
        echo " * do not edit. */"
        echo
        echo "typedef struct {"
        echo "    const uint8_t *cmd;"
        echo "    size_t         cmd_len;"
        echo "    const uint8_t *rsp;"
        echo "    size_t         rsp_len;"
        echo "} xchg;"
        echo
        cat "$DIR/tpm2.h" "$DIR/tpm12.h"
    } > "$RECORD"
    echo "recorded $RECORD"
fi

if [[ $rc -eq 0 ]]; then
    echo "test-tpm: PASS"
else
    echo "test-tpm: FAIL"
fi
exit $rc
