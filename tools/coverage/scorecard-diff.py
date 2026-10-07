#!/usr/bin/env python3
"""falcon-boot: compare two coverage scorecards (docs/coverage.md).

Usage: scorecard-diff.py BASELINE NEW

Reads the Matrix table of each and lists every cell that changed. A cell
that read PASS in BASELINE and reads anything else in NEW is a regression,
and so is a baseline PASS whose ISO or column NEW does not have: the release
gate runs every state, so a missing cell is a case that did not run. Exit 1
when there is a regression, 0 otherwise. New ISOs and new columns are
listed and do not fail.
"""

import sys


def matrix(path):
    """Return ({iso: {column: cell}}, [columns]) from the Matrix section."""
    rows, header, inside = {}, None, False
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("## "):
                inside = line == "## Matrix"
                continue
            if not inside or not line.startswith("|"):
                continue
            cells = [c.strip() for c in line.strip("|").split("|")]
            if header is None:
                header = cells
            elif set(cells[0]) <= set("-"):
                continue
            else:
                rows[cells[0]] = dict(zip(header[3:], cells[3:]))
    if header is None:
        sys.exit(f"ERROR: no Matrix table in {path}")
    return rows, header[3:]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2])
    base, base_cols = matrix(sys.argv[1])
    new, new_cols = matrix(sys.argv[2])

    regressed, changed = [], []
    for iso, cells in base.items():
        for col in base_cols:
            was = cells.get(col, "")
            now = new.get(iso, {}).get(col, "(not run)")
            if was == now:
                continue
            line = f"{iso:32} {col:36} {was} -> {now}"
            (regressed if was == "PASS" else changed).append(line)
    added = sorted(set(new) - set(base))
    added_cols = [c for c in new_cols if c not in base_cols]

    for title, lines in (("Regressed", regressed), ("Changed", changed)):
        if lines:
            print(f"{title} ({len(lines)}):")
            print("\n".join(f"  {l}" for l in lines))
    if added:
        print(f"New ISOs: {', '.join(added)}")
    if added_cols:
        print(f"New columns: {', '.join(added_cols)}")
    if not (regressed or changed or added or added_cols):
        print("No cell changed.")
    return 1 if regressed else 0


if __name__ == "__main__":
    sys.exit(main())
