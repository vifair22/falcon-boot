# falcon-boot: the boot-timing variables judged from a rig's serial log
# (docs/DESIGN.md Section 5.5). Sourced by run-core-rig.sh and
# run-station-rig.sh.
#
# Two witnesses meet in the log. The loader's own serial lines
# ("falcon-bli: ...", console/serial.c) say what it measured and set. The
# fixture UKI's /init (falcon-efi/tools/uki-init/init.c) says what the
# booted kernel reads back from efivarfs, which is what systemd-analyze
# reads, and the kernel's own TSC rate with the counter now at that rate.
#
# bli_judge TEXT-LOG MIN-WAIT-US [VIA]
#   Sets BLI_WHY to the first thing that does not hold (empty when all do)
#   and BLI_DETAIL to the values. What must hold:
#   - the loader logged its rate, within 0.5-10 GHz, measured by VIA
#     (cpuid or stall) when given, and both timestamps;
#   - efivarfs has LoaderTimeInitUSec, LoaderTimeExecUSec,
#     FalconTimeWaitUSec and FalconTimeKdfUSec with exactly the values the
#     loader logged, FalconKdfInfo exactly as the unlock's
#     "falcon-pos: argon2id ran on ... ms: <info>" line says it,
#     FalconReadInfo exactly as the loader logged it at the launch, and
#     LoaderInfo, LoaderFirmwareInfo and LoaderFirmwareType in
#     systemd-boot's formats; every one volatile (attributes 0x6: boot
#     services and runtime, not non-volatile) and NUL-terminated;
#   - 0 < init < exec, MIN-WAIT-US <= wait < exec - init, and
#     0 < kdf <= exec - init - wait, with the info's milliseconds kdf's;
#   - the boot stretch of FalconReadInfo read something (the UKI) in no
#     more milliseconds than exec - init;
#   - under a hypervisor (the kernel's CPUID.1:ECX bit 31), the info names
#     no clock: the loader did not read APERF/MPERF there;
#   - the loader's rate is within 1% of the rate the kernel found for the
#     same counter, and exec is earlier than the kernel's "now" by less
#     than two minutes.

bli_judge() {
    local log=$1 minwait=$2 via=${3:-} out
    BLI_WHY=
    BLI_DETAIL=
    out=$(awk -v minwait="$minwait" -v want_via="$via" '
        function field(line, key,    m) {
            if (match(line, key "=[^ ]*")) {
                m = substr(line, RSTART + length(key) + 1,
                           RLENGTH - length(key) - 1)
                return m
            }
            return ""
        }
        /falcon-bli: tsc_hz=/ {
            hz = field($0, "tsc_hz"); how = field($0, "via")
            li = field($0, "LoaderTimeInitUSec"); ninit++
        }
        /falcon-pos: argon2id ran on .* ms: / {
            kinfo = substr($0, index($0, " ms: ") + 5)
            sub(/[ \t\r]+$/, "", kinfo)
        }
        /falcon-bli: LoaderTimeExecUSec=/ {
            le = field($0, "LoaderTimeExecUSec")
            lw = field($0, "FalconTimeWaitUSec")
            lk = field($0, "FalconTimeKdfUSec")
            kinfo_exec = kinfo
        }
        /falcon-bli: FalconReadInfo=/ {
            rinfo = substr($0, index($0, "FalconReadInfo=") + 15)
            sub(/[ \t\r]+$/, "", rinfo)
        }
        /FALCON-BLI-VAR / {
            s = substr($0, index($0, "FALCON-BLI-VAR ") + 15)
            split(s, f, " ")
            name = f[1]
            if (f[2] == "missing") { miss[name] = 1; next }
            attrs[name] = field(s, "attrs")
            nul[name] = field(s, "nul")
            v = s; sub(/^.* value=/, "", v); sub(/[ \t]+$/, "", v)
            val[name] = v
        }
        /FALCON-BLI-KERNEL / {
            khz = field($0, "tsc_khz"); now = field($0, "now_usec")
            hv = field($0, "hypervisor")
        }
        /FALCON-BLI-EFIVARFS unavailable/ { noefivarfs = 1 }
        function fail(why) { print "WHY " why; bad = 1; exit }
        END {
            if (bad) exit
            if (ninit == 0) fail("the loader logged no rate or init time")
            if (hz + 0 < 500000000 || hz + 0 > 10000000000)
                fail("the loader rate " hz " Hz is outside 0.5-10 GHz")
            if (want_via != "" && how != want_via)
                fail("the rate came by " how ", not " want_via)
            if (le == "") fail("the loader logged no exec time")
            if (noefivarfs) fail("the kernel could not mount efivarfs")
            n = split("LoaderTimeInitUSec LoaderTimeExecUSec LoaderInfo " \
                      "LoaderFirmwareInfo LoaderFirmwareType " \
                      "FalconTimeWaitUSec FalconTimeKdfUSec FalconKdfInfo " \
                      "FalconReadInfo",
                      names, " ")
            for (i = 1; i <= n; i++) {
                k = names[i]
                if (miss[k] || !(k in val)) fail(k " is not in efivarfs")
                if (attrs[k] != "0x6")
                    fail(k " has attributes " attrs[k] ", not 0x6")
                if (nul[k] != "1") fail(k " is not NUL-terminated")
            }
            if (val["LoaderTimeInitUSec"] != li)
                fail("LoaderTimeInitUSec reads " val["LoaderTimeInitUSec"] \
                     ", the loader set " li)
            if (val["LoaderTimeExecUSec"] != le)
                fail("LoaderTimeExecUSec reads " val["LoaderTimeExecUSec"] \
                     ", the loader set " le)
            if (val["FalconTimeWaitUSec"] != lw)
                fail("FalconTimeWaitUSec reads " val["FalconTimeWaitUSec"] \
                     ", the loader set " lw)
            if (lk == "") fail("the loader logged no KDF time at the launch")
            if (kinfo_exec == "") fail("the unlock logged no KDF info")
            if (val["FalconTimeKdfUSec"] != lk)
                fail("FalconTimeKdfUSec reads " val["FalconTimeKdfUSec"] \
                     ", the loader set " lk)
            if (val["FalconKdfInfo"] != kinfo_exec)
                fail("FalconKdfInfo reads \"" val["FalconKdfInfo"] \
                     "\", the unlock logged \"" kinfo_exec "\"")
            if (!match(kinfo_exec, / [0-9]+ ms/))
                fail("FalconKdfInfo names no time")
            kms = substr(kinfo_exec, RSTART + 1, RLENGTH - 4)
            if (kms + 0 != int(lk / 1000))
                fail("FalconKdfInfo says " kms " ms, FalconTimeKdfUSec " lk " us")
            if (rinfo == "") fail("the loader logged no read info at the launch")
            if (val["FalconReadInfo"] != rinfo)
                fail("FalconReadInfo reads \"" val["FalconReadInfo"] \
                     "\", the loader logged \"" rinfo "\"")
            if (!match(rinfo, /boot [0-9]+ calls [0-9]+ KiB [0-9]+ ms/))
                fail("FalconReadInfo has no boot stretch: \"" rinfo "\"")
            split(substr(rinfo, RSTART, RLENGTH), rb, " ")
            if (!(rb[2] + 0 > 0) || !(rb[4] + 0 > 0))
                fail("FalconReadInfo says the boot read nothing: \"" rinfo "\"")
            if (rb[6] * 1000 > le - li)
                fail("FalconReadInfo boot reads took " rb[6] " ms, over " \
                     "exec - init " le - li " us")
            if (val["LoaderInfo"] !~ /^falcon-boot [0-9]/)
                fail("LoaderInfo is \"" val["LoaderInfo"] "\"")
            if (val["LoaderFirmwareInfo"] !~ /^.+ [0-9]+\.[0-9][0-9]+$/)
                fail("LoaderFirmwareInfo is \"" val["LoaderFirmwareInfo"] "\"")
            if (val["LoaderFirmwareType"] !~ /^UEFI [0-9]+\.[0-9][0-9]+$/)
                fail("LoaderFirmwareType is \"" val["LoaderFirmwareType"] "\"")
            if (!(li + 0 > 0)) fail("init is 0")
            if (!(le + 0 > li + 0)) fail("exec " le " is not after init " li)
            if (lw + 0 < minwait + 0)
                fail("the wait " lw " us is under the " minwait " us the rig spent")
            if (!(lw + 0 < le - li))
                fail("the wait " lw " us is not under exec - init " le - li)
            if (!(lk + 0 > 0)) fail("the KDF time is 0")
            if (!(lk + 0 <= le - li - lw))
                fail("the KDF time " lk " us is over exec - init - wait " \
                     le - li - lw)
            if (hv == "") fail("the kernel did not say whether a hypervisor is present")
            if (hv == "1" && kinfo_exec ~ / MHz$/)
                fail("FalconKdfInfo names a clock under a hypervisor")
            if (khz + 0 <= 0) fail("the kernel logged no TSC rate")
            dev = (hz / 1000 - khz) / khz
            if (dev > 0.01 || dev < -0.01)
                fail(sprintf("the loader rate %d Hz is %.2f%% off the kernel %d kHz",
                             hz, dev * 100, khz))
            if (!(now + 0 > le + 0) || now - le > 120000000)
                fail("exec " le " us does not precede the kernel now " now " us")
            printf "OK tsc %d Hz by %s (kernel %d kHz, %+.3f%%); " \
                   "init %d us, exec %d us, wait %d us, kdf %d us, " \
                   "kernel now %d us; %s; %s; %s; %s (hypervisor %s); " \
                   "reads %s\n",
                   hz, how, khz, dev * 100, li, le, lw, lk, now,
                   val["LoaderInfo"], val["LoaderFirmwareInfo"],
                   val["LoaderFirmwareType"], kinfo_exec, hv, rinfo
        }' "$log")
    case $out in
    "OK "*)  BLI_DETAIL=${out#OK } ;;
    "WHY "*) BLI_WHY=${out#WHY } ;;
    *)       BLI_WHY="the judge read nothing" ;;
    esac
}
