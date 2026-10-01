# Static analysis

`tools/analyze.sh` is the single entry point. It runs every analyzer the repo
standard asks for, gates the result against a checked-in baseline of findings
a human has already judged, and exits nonzero when something new appears.

```sh
tools/analyze.sh                  # all stages, baseline-gated (what CI runs)
tools/analyze.sh tidy             # one stage
tools/analyze.sh -j 4             # cap parallelism
tools/analyze.sh --strict         # ignore the baseline; every finding fails
tools/analyze.sh --update-baseline
```

## Run it in the harness container

The baseline is only meaningful beside a fixed set of analyzer versions, so
the gate answers to one environment (recorded in
`tools/analyze-baseline.txt` Section 0):

```sh
tools/coverage/harness.sh -- tools/analyze.sh
tools/coverage/harness.sh -- tools/analyze.sh --update-baseline
```

A workstation run still works and is still worth doing while editing, but it
will disagree, because its analyzers are newer and report a different set.
Judge a finding on the code, not on which machine surfaced it, and do not
rewrite the baseline from a workstation run.

Raw analyzer output lands in `build/analyze/<stage>.log`, which is where to
look for a full `-fanalyzer` event trace. The whole directory is a throwaway;
the script wipes it on every run.

Total runtime is about 7 s on a warm tree, nearly all of it clang-tidy.

## Stages

| Stage | Tool | Covers | Time |
| --- | --- | --- | --- |
| `cppcheck` | cppcheck 2.18 | `--enable=warning,performance,portability` at `--check-level=exhaustive` over `src/` | <1 s |
| `fanalyzer` | mingw gcc 16 | `-fanalyzer` rebuild of every loader object into `build/analyze/obj/` | ~1 s |
| `tidy` | clang-tidy 22 | the check set in `/.clang-tidy`, over `src/` including our headers | ~5 s |

The repo standard names a fourth analyzer, the stack-usage check. It is not a
stage here because it is already `-Wstack-usage=32768` in the normal `CFLAGS`,
so `make` enforces it on every build; a second front end for it would only be
a way for the two to disagree.

All three stages take their flags from `compile_commands.json` rather than
restating `CFLAGS`. That is deliberate: it is what keeps the analyzers seeing
the same `-ffreestanding -fshort-wchar -mno-red-zone` world the loader is
built in, and what carries `-maes` onto `falcon-core/src/crypto/aesni.c` and onto nothing
else. If the flags are wrong here, they are wrong in the build too. The script
regenerates the database and `build/version.h` if either is missing.

`falcon-efi/tools/payload/payload.c` is excluded everywhere. It is a throwaway EFI
application for the QEMU rig and is never linked into the loader.

A missing analyzer is reported as `SKIP` and costs nothing: the script still
runs the stages it can and still gates them. Findings belonging to a stage
that was skipped or not selected are left out of the comparison entirely, so a
partial run never reports the rest of the baseline as stale.

## The baseline

`tools/analyze-baseline.txt`. Each data line is tab separated:

```
stage <TAB> file <TAB> check-id <TAB> message
```

Line and column are deliberately not part of the key, so an edit that only
moves code around does not churn the file. Duplicates are kept one line per
occurrence, which makes the count part of the gate: a second hit of an
already-baselined pattern in the same file is reported as new.

The run compares the current findings against it and

- fails on anything present now and not in the baseline, printing the original
  diagnostic with its real line number, not the normalised key;
- warns, without failing, about baseline entries that no longer fire, so a
  fixed finding does not silently keep its exemption.

Regenerate with `--update-baseline`. It keeps the file's leading comment block
and replaces every data line, so all per-entry reasoning has to live in that
header block or it will be lost.

## Suppression policy

In order of preference:

1. **Fix the code.** A finding that describes a real defect is not a
   suppression candidate.
2. **Turn the check off in `/.clang-tidy`**, when it is wrong about this
   codebase as a whole rather than about one line, and say why in the file.
   The current nine all fall out of two facts: the loader is freestanding with
   no libc, and UEFI dictates the shape of its own APIs. Two of them
   (`bugprone-casting-through-void`, `performance-no-int-to-ptr`) are checks
   that want the exact opposite of what `-Wcast-align` and `-Wcast-qual` in
   `CFLAGS` demand, and the gcc flags win.
3. **Baseline the individual finding**, with the reasoning written into the
   baseline's header block. This is the right answer when the check is worth
   keeping but a specific hit is a false positive. It is what
   `bugprone-implicit-widening-of-multiplication-result` gets: 23 audited
   false positives, and the check stays on because it is exactly the check
   that would catch a real offset overflow in the exFAT/ext4/ISO parsers.
4. **Inline `NOLINT` / `// cppcheck-suppress`** in the source. Supported
   (`--inline-suppr` is on for cppcheck) but currently unused.

Do not reach for a file-wide `-Wno-` on a gcc analyzer warning. The two
`-Wanalyzer-null-dereference` hits are both in `falcon-efi/src/iso/iso.c`, which parses
attacker-supplied media, and blinding that file to null-dereference analysis
to silence two infeasible paths is a bad trade.

There is no `.cppcheck-suppressions` file because cppcheck currently finds
nothing. If one becomes necessary it belongs at the repo root, passed with
`--suppressions-list=`, one documented entry per line.

## Current status

Current baseline: **27 entries, all triaged.**

| Stage | Findings | Baselined |
| --- | --- | --- |
| cppcheck | 0 | - |
| fanalyzer | 2 | 2 |
| tidy | 25 | 25 |

- 26 of the 27 are false positives, each argued in the baseline's header.
- **One is a real open finding**: `falcon-core/src/luks/luks.c:599`, `matched = 1` is a
  dead store, which makes the function tail's
  `return matched ? LUKS_ERR_NOKEY : LUKS_ERR_CORRUPT` a ternary that can only
  take one branch. cppcheck agrees independently (`unreadVariable`, visible
  with `CPPCHECK_ENABLE=style`). It is baselined so the gate is usable, and
  flagged in the baseline as `OPEN FINDING`. Resolve it and drop the entry.

`CPPCHECK_ENABLE=style tools/analyze.sh cppcheck` widens cppcheck beyond the
repo-standard set. It currently adds two findings: the `luks.c:599` dead store
above and a `constParameterPointer` in `falcon-core/src/luks/af.c:88`. Not part of the
gate, useful to run occasionally.
