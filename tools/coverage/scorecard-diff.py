#!/usr/bin/env python3
"""falcon-boot: compare two coverage scorecards (docs/coverage.md).

Usage: scorecard-diff.py [--cases] BASELINE NEW

Reads the Matrix table of each and lists what the release evaluation asks:
what regressed, what improved, what else changed, what was added and what
was dropped. A cell that read PASS in BASELINE and reads anything else in NEW
is a regression, and so is a baseline PASS whose ISO or column NEW does not
have: the release gate runs every state, so a missing cell is a case that did
not run. A cell that reached PASS improved. Exit 1 when there is a regression,
0 otherwise; nothing else fails.

--cases prints only the regressions, one `ID STATE ROUTE` per line in
run-corpus.sh's terms, for a caller that reruns them alone before it calls
any of them real.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scorecard import ROUTE_NAMES, STATES  # noqa: E402

# A Matrix column's label back to the state and route it was made from.
COLUMNS = {'%s %s' % (name, ROUTE_NAMES[rt]): (st, rt)
           for st, name in STATES for rt in ROUTE_NAMES}


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
    args = sys.argv[1:]
    cases = bool(args) and args[0] == '--cases'
    if cases:
        args = args[1:]
    if len(args) != 2:
        sys.exit(__doc__.strip().splitlines()[2])
    base, base_cols = matrix(args[0])
    new, new_cols = matrix(args[1])

    dropped = sorted(set(base) - set(new))
    dropped_cols = [c for c in base_cols if c not in new_cols]
    regressed, improved, changed, rerun = [], [], [], []
    for iso, cells in base.items():
        for col in base_cols:
            was = cells.get(col, "")
            now = new.get(iso, {}).get(col, "(not run)")
            if was == now:
                continue
            line = f"{iso:32} {col:36} {was} -> {now}"
            if was == "PASS":
                regressed.append(line)
                if col in COLUMNS:
                    rerun.append("%s %s %s" % ((iso,) + COLUMNS[col]))
            elif iso in dropped or col in dropped_cols:
                continue  # Named once below, not cell by cell.
            else:
                (improved if now == "PASS" else changed).append(line)
    if cases:
        print("\n".join(rerun))
        return 1 if regressed else 0
    added = sorted(set(new) - set(base))
    added_cols = [c for c in new_cols if c not in base_cols]

    for title, lines in (("Regressed", regressed), ("Improved", improved),
                         ("Changed", changed)):
        if lines:
            print(f"{title} ({len(lines)}):")
            print("\n".join(f"  {l}" for l in lines))
    for title, names in (("Added ISOs", added), ("Added columns", added_cols),
                         ("Dropped ISOs", dropped),
                         ("Dropped columns", dropped_cols)):
        if names:
            print(f"{title} ({len(names)}): {', '.join(names)}")
    if not (regressed or improved or changed or added or added_cols
            or dropped or dropped_cols):
        print("No cell changed.")
    return 1 if regressed else 0


if __name__ == "__main__":
    sys.exit(main())
