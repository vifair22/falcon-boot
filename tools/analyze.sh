#!/usr/bin/env bash
#
# Static analysis for falcon-boot: one entry point for every analyzer the repo
# standard asks for, gated against a checked-in baseline of known-benign
# findings so that a regression fails and a known false positive does not.
#
# Stages:
#   cppcheck    cppcheck --enable=warning,performance,portability over src/
#   fanalyzer   gcc -fanalyzer rebuild of every loader object
#   tidy        clang-tidy over src/, configured by .clang-tidy at the root
#
# The fourth analyzer in the repo standard, the stack-usage check, is not a
# stage here: it is -Wstack-usage=32768 in the normal CFLAGS, so it is already
# enforced by `make` on every build and does not need a second front end.
#
# Exit status is 0 when every stage is clean-or-baselined, 1 otherwise. A
# missing analyzer is reported and skipped, not an error, so the script stays
# usable on a machine that has only part of the toolchain.
#
# See docs/notes/analyze.md for usage and suppression policy.

set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
WORK="$ROOT/build/analyze"
BASELINE="$ROOT/docs/notes/analyze-baseline.txt"
DB="$ROOT/compile_commands.json"

# gcc quotes identifiers with U+2018/U+2019 under a UTF-8 locale, which would
# put non-ASCII into the baseline file. C locale gives plain ASCII quotes.
export LC_ALL=C

JOBS=$(nproc 2>/dev/null || echo 4)
MODE=baseline
STAGES=()

# The repo standard's set. Override to add `style` for a wider sweep; it finds
# real things (constParameterPointer, unreadVariable) but is not part of the
# gate, so it is opt-in rather than default.
CPPCHECK_ENABLE=${CPPCHECK_ENABLE:-warning,performance,portability}

usage()
{
	cat <<'EOF'
usage: tools/analyze.sh [options] [stage ...]

stages (default: all three)
  cppcheck    cppcheck over src/
  fanalyzer   gcc -fanalyzer rebuild of every loader object
  tidy        clang-tidy over src/

options
  -j N, --jobs N      parallelism for the rebuild/lint stages (default: nproc)
  --strict            ignore the baseline; fail on any finding at all
  --update-baseline   rewrite docs/notes/analyze-baseline.txt from this run
                      and exit 0 (the file's leading comment block is kept)
  -h, --help          this text

environment
  CPPCHECK_ENABLE     cppcheck --enable= value
                      (default: warning,performance,portability)

exit status
  0  every stage clean or fully covered by the baseline
  1  at least one finding not in the baseline
EOF
}

while [ $# -gt 0 ]; do
	case $1 in
	-j | --jobs)
		JOBS=$2
		shift 2
		;;
	--jobs=*)
		JOBS=${1#*=}
		shift
		;;
	--strict)
		MODE=strict
		shift
		;;
	--update-baseline)
		MODE=update
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	cppcheck | fanalyzer | tidy)
		STAGES+=("$1")
		shift
		;;
	*)
		echo "analyze: unknown argument: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

[ ${#STAGES[@]} -gt 0 ] || STAGES=(cppcheck fanalyzer tidy)

rm -rf "$WORK"
mkdir -p "$WORK"

# ---- prerequisites -------------------------------------------------------
#
# Both the fanalyzer and the clang-tidy stage read the real per-file flags out
# of the compilation database rather than duplicating CFLAGS, which is what
# keeps this script from drifting away from the Makefile. main.c also needs
# the generated version header to preprocess at all.

# Refreshed unconditionally rather than only when absent: the database is
# generated from SRCS, so a source added to the Makefile since the last build
# would otherwise be silently left out of every stage. make rebuilds it only
# when the Makefile is newer, so this is free on a warm tree.
if ! make -C "$ROOT" --no-print-directory compile_commands.json \
	version-stamp >/dev/null 2>&1; then
	if [ ! -f "$DB" ] || [ ! -f "$ROOT/build/version.h" ]; then
		echo "analyze: cannot generate compile_commands.json or" \
			"build/version.h; run 'make' first" >&2
		exit 2
	fi
	echo "analyze: WARNING: could not refresh compile_commands.json," \
		"using the existing one" >&2
fi

# What the gate covers, by path relative to the repository: a component's
# src/ or an app under apps/. Relative, because the absolute path carries
# wherever the tree is mounted, and a mount point named like a directory the
# tree uses would put everything in the gate (tools/coverage/harness.sh has
# the story). Test sources and falcon-efi/tools/payload, a throwaway test
# artifact never linked into a loader, are outside both. The database also
# carries falcon-bios's .S files, for editors; the analyzers are C tools and
# take the .c entries only.
GATED_PY='
import os, re
def gated(path, root):
    rel = os.path.relpath(path, root)
    return (rel.endswith(".c") and
            re.match(r"(falcon-[^/]+/src|apps)/", rel) is not None)
'

# Every gated source, absolute, from the database.
mapfile -t SOURCES < <(python3 -c "$GATED_PY"'
import json, sys
for e in json.load(open(sys.argv[1])):
    if gated(e["file"], sys.argv[2]):
        print(e["file"])
' "$DB" "$ROOT" | sort)

if [ ${#SOURCES[@]} -eq 0 ]; then
	echo "analyze: no src/ entries in $DB" >&2
	exit 2
fi

# ---- module names --------------------------------------------------------
#
# <pos/pos.h> means one file only while no two component src/ directories on
# one include line both hold a pos/. The rule is per include set, not per
# tree (falcon-efi and falcon-bios each have a pos/ and never share a set), so
# it is checked per set, and the sets are read from the database rather than
# restated: every -I<component>/src a real command carries. A top-level header
# (falcon-winjump's flat src/) is a name in the same namespace as a directory.
# It is not a baselined stage: a collision silently resolves to the first
# directory on the line, so there is no version of it worth keeping.
if ! python3 - "$DB" "$ROOT" <<'EOF'; then
import json, os, re, sys

db, root = sys.argv[1], sys.argv[2]
sets = {}
for e in json.load(open(db)):
    incs = []
    for d in re.findall(r"(?:^|\s)-I(\S+)", e["command"]):
        d = os.path.normpath(os.path.join(e["directory"], d))
        rel = os.path.relpath(d, root)
        if re.fullmatch(r"falcon-[^/]+/src", rel) and rel not in incs:
            incs.append(rel)
    if len(incs) > 1:
        sets.setdefault(tuple(sorted(incs)), e["file"])

bad = 0
for incs, example in sorted(sets.items()):
    seen = {}
    for inc in incs:
        for name in sorted(os.listdir(os.path.join(root, inc))):
            if not (os.path.isdir(os.path.join(root, inc, name)) or
                    name.endswith(".h")):
                continue
            if name in seen:
                print(f"analyze: module name '{name}' is in both "
                      f"{seen[name]} and {inc}, which share an include set "
                      f"(e.g. {os.path.relpath(example, root)})")
                bad = 1
            else:
                seen[name] = inc
sys.exit(bad)
EOF
	echo "analyze: FAIL, rename one of each pair above." >&2
	exit 1
fi

# ---- finding normalisation ----------------------------------------------
#
# All three analyzers can be made to emit "file:line:col: severity: message
# [check-id]". Findings are keyed on everything in that except the line and
# column, so the baseline survives an edit that only moves code around; a
# finding that genuinely changes, or a new one, still shows up. Repeated
# identical keys are kept as repeated lines, so the count is part of the key
# set and a second instance of a baselined pattern is a new finding.
#
# Fields are tab separated: stage, file, check id, message.

NORMALISE_PY=$WORK/normalise.py
cat >"$NORMALISE_PY" <<'EOF'
import re, sys

stage, root = sys.argv[1], sys.argv[2].rstrip("/") + "/"
# file:line:col: severity: message [check-id]
pat = re.compile(r"^(.+?):\d+:\d+: [a-z]+: (.*) \[([^\[\]]+)\]$")
out = []
for line in sys.stdin:
    m = pat.match(line.rstrip("\n"))
    if not m:
        continue
    path, msg, check = m.group(1), m.group(2), m.group(3)
    if path.startswith(root):
        path = path[len(root):]
    out.append("\t".join((stage, path, check, msg)))
out.sort()
sys.stdout.write("".join(s + "\n" for s in out))
EOF

normalise()
{
	python3 "$NORMALISE_PY" "$1" "$ROOT"
}

# ---- stages --------------------------------------------------------------

have()
{
	command -v "$1" >/dev/null 2>&1
}

declare -A STATUS

stage_cppcheck()
{
	if ! have cppcheck; then
		echo "cppcheck: SKIP (not installed)"
		STATUS[cppcheck]=skip
		return 0
	fi
	# --project gives cppcheck the real -I set and the per-file -maes without
	# restating them; the --file-filter pair then drops the test sources and
	# falcon-efi/tools/payload back out.
	# --check-level=exhaustive costs well under a second on this tree and
	# removes the "limiting analysis of branches" caveat entirely.
	cppcheck \
		--enable="$CPPCHECK_ENABLE" \
		--project="$DB" \
		--file-filter='*/src/*' \
		--file-filter='*/apps/*' \
		--check-level=exhaustive \
		--inline-suppr \
		--suppress=missingIncludeSystem \
		--quiet \
		-j "$JOBS" \
		--template='{file}:{line}:{column}: {severity}: {message} [{id}]' \
		>"$WORK/cppcheck.log" 2>&1 || true
	normalise cppcheck <"$WORK/cppcheck.log" >"$WORK/cppcheck.findings"
}

stage_fanalyzer()
{
	local cc
	cc=$(python3 -c '
import json, shlex, sys
print(shlex.split(json.load(open(sys.argv[1]))[0]["command"])[0])
' "$DB")
	if ! have "$cc"; then
		echo "fanalyzer: SKIP ($cc not installed)"
		STATUS[fanalyzer]=skip
		return 0
	fi

	# One command per object, taken verbatim from the database minus the
	# dependency-file and output flags, plus -fanalyzer and an output path
	# under build/analyze/. Reusing the database is what carries -maes onto
	# aesni.c and nothing else, exactly as the Makefile does.
	python3 -c "$GATED_PY"'
import json, os, shlex, sys

db, work, root = sys.argv[1], sys.argv[2], sys.argv[3]
for e in json.load(open(db)):
    if not gated(e["file"], root):
        continue
    argv, skip = [], 0
    for t in shlex.split(e["command"]):
        if skip:
            skip -= 1
            continue
        if t == "-MMD":
            continue
        if t in ("-MF", "-o"):
            skip = 1
            continue
        if t == "-c" or t.endswith(".c"):
            continue
        argv.append(t)
    rel = os.path.relpath(e["file"], e["directory"])
    obj = os.path.join(work, os.path.splitext(rel)[0] + ".o")
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    argv += ["-fdiagnostics-plain-output", "-fanalyzer", "-c", "-o", obj,
             os.path.relpath(e["file"], os.path.dirname(db))]
    print(shlex.join(argv))
' "$DB" "$WORK/obj" "$ROOT" >"$WORK/fanalyzer.cmds"

	(
		cd "$ROOT"
		xargs -P "$JOBS" -d '\n' -I CMD sh -c 'CMD' \
			<"$WORK/fanalyzer.cmds"
	) >"$WORK/fanalyzer.log" 2>&1 || true
	normalise fanalyzer <"$WORK/fanalyzer.log" >"$WORK/fanalyzer.findings"
}

stage_tidy()
{
	if ! have clang-tidy; then
		echo "tidy: SKIP (clang-tidy not installed)"
		STATUS[tidy]=skip
		return 0
	fi
	# clang-tidy infers the mingw target from the compiler name in the
	# database, so no --extra-arg is needed. One process per file, because
	# clang-tidy itself is single threaded.
	printf '%s\n' "${SOURCES[@]}" |
		xargs -P "$JOBS" -d '\n' -I FILE \
			clang-tidy --quiet -p "$ROOT" FILE \
			>"$WORK/tidy.raw" 2>&1 || true
	mv "$WORK/tidy.raw" "$WORK/tidy.log"
	# HeaderFilterRegex lets a finding inside a header be reported once per
	# translation unit that includes it. Identical diagnostic lines are the
	# same finding, so collapse them before they reach the key set. The log
	# itself is left as clang-tidy wrote it, so it stays readable.
	sort -u "$WORK/tidy.log" | normalise tidy >"$WORK/tidy.findings"
}

RAN=()

for stage in "${STAGES[@]}"; do
	start=$SECONDS
	"stage_$stage"
	elapsed=$((SECONDS - start))
	if [ "${STATUS[$stage]:-}" = skip ]; then
		continue
	fi
	RAN+=("$stage")
	n=$(wc -l <"$WORK/$stage.findings")
	STATUS[$stage]=$n
	printf '%-10s %3d finding(s)  %ds  (raw log: build/analyze/%s.log)\n' \
		"$stage:" "$n" "$elapsed" "$stage"
done

# ---- baseline ------------------------------------------------------------

{ cat "$WORK"/*.findings 2>/dev/null || true; } | sort >"$WORK/all.findings"

if [ "$MODE" = update ]; then
	if [ ${#STAGES[@]} -ne 3 ]; then
		echo "analyze: --update-baseline needs all three stages" >&2
		exit 2
	fi
	for s in "${STAGES[@]}"; do
		if [ "${STATUS[$s]:-skip}" = skip ]; then
			echo "analyze: cannot rewrite the baseline, $s was skipped" >&2
			exit 2
		fi
	done
	mkdir -p "$(dirname "$BASELINE")"
	tmp=$WORK/baseline.new
	if [ -f "$BASELINE" ]; then
		# Keep everything up to the first data line: that block is where
		# the per-finding reasoning lives.
		awk '/^[^#]/ && NF { exit } { print }' "$BASELINE" >"$tmp"
	else
		printf '# falcon-boot static analysis baseline.\n#\n' >"$tmp"
		printf '# Generated by tools/analyze.sh --update-baseline.\n\n' \
			>>"$tmp"
	fi
	cat "$WORK/all.findings" >>"$tmp"
	mv "$tmp" "$BASELINE"
	echo "analyze: baseline rewritten ($(wc -l <"$WORK/all.findings") entries)"
	echo "analyze: re-check the reasoning in its header block by hand"
	exit 0
fi

if [ "$MODE" = strict ] || [ ! -f "$BASELINE" ]; then
	: >"$WORK/baseline.sorted"
	[ "$MODE" = strict ] ||
		echo "analyze: no baseline at $BASELINE; treating every finding as new"
else
	# Only the stages that actually ran: entries belonging to a stage that
	# was skipped or not selected must not be reported as stale.
	{
		for s in ${RAN[@]+"${RAN[@]}"}; do
			grep -e "^$s"$'\t' "$BASELINE" || true
		done
	} | sort >"$WORK/baseline.sorted"
fi

comm -23 "$WORK/all.findings" "$WORK/baseline.sorted" >"$WORK/new.findings"
comm -13 "$WORK/all.findings" "$WORK/baseline.sorted" >"$WORK/stale.findings"

if [ -s "$WORK/stale.findings" ]; then
	echo
	echo "analyze: baseline entries that no longer fire (run --update-baseline):"
	sed 's/^/  - /' "$WORK/stale.findings"
fi

if [ -s "$WORK/new.findings" ]; then
	echo
	echo "analyze: FAIL, findings not covered by the baseline:"
	echo
	# Print the full original diagnostic for each new finding rather than the
	# normalised key, so the line number and any analyzer path are right there.
	while IFS=$'\t' read -r stage file check _; do
		grep -h -F -e "[$check]" "$WORK/$stage.log" |
			grep -F -e "$file" | sed 's/^/  /' || true
	done <"$WORK/new.findings" | sort -u
	echo
	echo "analyze: fix them, or add them to $BASELINE with a reason."
	exit 1
fi

echo
echo "analyze: OK (all findings covered by the baseline)"
exit 0
