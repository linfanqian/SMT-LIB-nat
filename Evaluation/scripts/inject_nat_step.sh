#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

SRC="$REPO_ROOT/nat_inputs"
DST="$REPO_ROOT/nat_inputs_natstep"

rm -rf "$DST"
mkdir -p "$DST"

python3 - <<'PY'
from pathlib import Path
import re

SRC = Path("nat_inputs")
DST = Path("nat_inputs_natstep")

decl_nat_const_re = re.compile(r'^\(declare-fun\s+([^\s()]+)\s+\(\)\s+Nat\$\)$')

for path in SRC.rglob("*.smt2"):
    rel = path.relative_to(SRC)
    out = DST / rel
    out.parent.mkdir(parents=True, exist_ok=True)

    text = path.read_text(errors="ignore")
    lines = text.splitlines()

    nat_vars = []
    for ln in lines:
        m = decl_nat_const_re.match(ln.strip())
        if m:
            name = m.group(1)
            if name not in ("to_int$", "nat_step$"):
                nat_vars.append(name)

    inserted_decl = False
    new_lines = []
    for ln in lines:
        new_lines.append(ln)
        if "(declare-fun to_int$ (Nat$) Int)" in ln and not inserted_decl:
            new_lines.append("(declare-fun nat_step$ (Nat$) Nat$)")
            inserted_decl = True

    if not inserted_decl:
        new_lines.insert(0, "(declare-fun nat_step$ (Nat$) Nat$)")

    step_asserts = [
        f"(assert (= (to_int$ (nat_step$ {v})) (to_int$ {v})))"
        for v in nat_vars[:3]
    ]

    final_lines = []
    inserted_asserts = False
    for ln in new_lines:
        if ln.strip() == "(check-sat)" and not inserted_asserts:
            final_lines.extend(step_asserts)
            inserted_asserts = True
        final_lines.append(ln)

    if not inserted_asserts:
        final_lines.extend(step_asserts)

    out.write_text("\n".join(final_lines) + "\n")
PY

echo "Done. New folder: $DST"
find "$DST" -name "*.smt2" | wc -l