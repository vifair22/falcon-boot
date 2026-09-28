# shellcheck shell=bash
# A software TPM for the rigs, sourced (falcon-efi/tools/run-tpm-rig.sh;
# docs/notes/argon2-totp.md Section 2).
#
# One swtpm per boot, in the version a case names: "2" for a TPM 2.0 on the
# TIS interface, "2crb" for a TPM 2.0 on CRB (what firmware TPMs, Intel PTT
# and AMD fTPM, present on real machines), "1.2" for a TPM 1.2, which has
# only TIS. Every start begins from an empty state directory, so each case
# gets a TPM that has never been measured into: PCRs at their reset values,
# no NV indices, no owner. That is the whole of the PCR reset between cases;
# nothing is carried from one case to the next except by a case that asks
# for the state to be kept (SWTPM_KEEP=1: the TOTP cases, where later boots
# must see the NV index the host enrolled between them).
#
# swtpm is started with only a control socket. QEMU hands it the data
# channel over that socket (CMD_SET_DATAFD), which swtpm refuses when it
# was also given --server, so the host cannot talk to the same instance
# while QEMU does, and swtpm exits when QEMU closes the control channel.
# What the TPM saw is therefore read from inside the guest (the probe), and
# from swtpm's own log when SWTPM_LOG_LEVEL is raised (5 dumps every
# command and response in both versions, and on 1.2 also traces each
# TPM_Extend with the PCR's old and new value).
#
# The socket path is relative to the caller's directory (the rigs cd to the
# repository root): a unix socket path is limited to 107 bytes, and this
# repository's worktrees sit deep enough that an absolute one under build/
# comes close.
#
#   swtpm_have                       swtpm is installed
#   swtpm_start VERSION DIR          start one; DIR is its own, under build/
#   swtpm_stop                       stop it (also safe with none running)
#   SWTPM_QEMU                       array: the QEMU arguments for it
#   swtpm_setup12 DIR                a 1.2 state as a PC's TPM is: owned
#                                    with the well-known secrets, NV locked;
#                                    swtpm_setup keeps each as SHA-1 of the
#                                    20 zero bytes, not the bytes
#   swtpm_serve VERSION DIR          the same state for the host, on the
#                                    data socket DIR/server.sock, between
#                                    boots (the TOTP cases' enrollment)
#   swtpm_serve_stop                 stop that one

SWTPM_PID=
# shellcheck disable=SC2034  # read by the rig that sources this
SWTPM_QEMU=()

swtpm_have() {
    command -v swtpm >/dev/null 2>&1
}

swtpm_start() {  # swtpm_start <2|2crb|1.2> <dir>
    local ver=$1 dir=$2 sock=$2/swtpm.sock i
    local args=(socket --tpmstate "dir=$dir/state"
                --ctrl "type=unixio,path=$sock"
                --pid "file=$dir/swtpm.pid")
    local dev=tpm-tis

    swtpm_stop
    case $ver in
    2)    args+=(--tpm2) ;;
    2crb) args+=(--tpm2); dev=tpm-crb ;;
    1.2)  ;;
    *) echo "swtpm_start: unknown TPM version '$ver'" >&2; return 2 ;;
    esac
    if [[ "${SWTPM_LOG_LEVEL:-0}" -gt 0 ]]; then
        args+=(--log "file=$dir/swtpm.log,level=$SWTPM_LOG_LEVEL")
    fi

    mkdir -p "$dir/state"
    if [[ "${SWTPM_KEEP:-0}" != 1 ]]; then
        rm -f "$dir"/state/*
    fi
    rm -f "$sock" "$dir/swtpm.log"

    swtpm "${args[@]}" >"$dir/swtpm.out" 2>&1 &
    SWTPM_PID=$!
    for (( i = 0; i < 50; i++ )); do
        [[ -S "$sock" ]] && break
        kill -0 "$SWTPM_PID" 2>/dev/null || break
        sleep 0.1
    done
    if [[ ! -S "$sock" ]]; then
        echo "swtpm_start: swtpm did not come up (see $dir/swtpm.out)" >&2
        swtpm_stop
        return 1
    fi
    # shellcheck disable=SC2054  # QEMU's own comma syntax, one argument each
    SWTPM_QEMU=(-chardev "socket,id=chrtpm,path=$sock"
                -tpmdev emulator,id=tpm0,chardev=chrtpm
                -device "$dev,tpmdev=tpm0")
}

swtpm_setup12() {  # swtpm_setup12 <dir>
    mkdir -p "$1/state"
    rm -f "$1"/state/*
    swtpm_setup --tpmstate "$1/state" --take-ownership --owner-well-known \
        --srk-well-known --lock-nvram --logfile "$1/setup.log" >/dev/null
}

SWTPM_SERVE_PID=

# A second process on the same state, never at the same time as QEMU's:
# swtpm refuses a data socket to QEMU's instance (see above). It sends
# TPM_Startup itself, as the firmware would.
swtpm_serve() {  # swtpm_serve <2|1.2> <dir>
    local ver=$1 dir=$2 sock=$2/server.sock i
    local args=(socket --tpmstate "dir=$dir/state"
                --server "type=unixio,path=$sock"
                --ctrl "type=unixio,path=$dir/server.ctrl"
                --flags not-need-init,startup-clear
                --pid "file=$dir/serve.pid")

    swtpm_serve_stop
    [[ $ver == 2* ]] && args+=(--tpm2)
    if [[ "${SWTPM_LOG_LEVEL:-0}" -gt 0 ]]; then
        args+=(--log "file=$dir/serve.log,level=$SWTPM_LOG_LEVEL")
    fi
    rm -f "$sock" "$dir/server.ctrl"
    swtpm "${args[@]}" >"$dir/serve.out" 2>&1 &
    SWTPM_SERVE_PID=$!
    for (( i = 0; i < 50; i++ )); do
        [[ -S "$sock" ]] && return 0
        kill -0 "$SWTPM_SERVE_PID" 2>/dev/null || break
        sleep 0.1
    done
    echo "swtpm_serve: swtpm did not come up (see $dir/serve.out)" >&2
    swtpm_serve_stop
    return 1
}

swtpm_serve_stop() {
    if [[ -n "$SWTPM_SERVE_PID" ]]; then
        kill "$SWTPM_SERVE_PID" 2>/dev/null || true
        wait "$SWTPM_SERVE_PID" 2>/dev/null || true
    fi
    SWTPM_SERVE_PID=
}

swtpm_stop() {
    if [[ -n "$SWTPM_PID" ]]; then
        kill "$SWTPM_PID" 2>/dev/null || true
        wait "$SWTPM_PID" 2>/dev/null || true
    fi
    SWTPM_PID=
    # shellcheck disable=SC2034
    SWTPM_QEMU=()
}
