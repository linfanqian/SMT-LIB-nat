#!/usr/bin/env bash
set -euo pipefail

cd ~/Downloads/non-incremental

SRC="nat_inputs_natvar_200"
DST="nat_inputs_composed_bridge_200"

rm -rf "$DST"
mkdir -p "$DST"

python3 - <<'PY'
from pathlib import Path
import re

SRC = Path("nat_inputs_natvar_200")
DST = Path("nat_inputs_composed_bridge_200")

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
            if name not in ("to_int$", "nat_bridge$", "score$"):
                nat_vars.append(name)

    inserted_bridge = False
    new_lines = []
    for ln in lines:
        new_lines.append(ln)
        if "(declare-fun to_int$ (Nat$) Int)" in ln and not inserted_bridge:
            new_lines.append("(declare-fun nat_bridge$ (Int) Nat$)")
            new_lines.append("(declare-fun score$ (Nat$) Int)")
            inserted_bridge = True

    if not inserted_bridge:
        new_lines.insert(0, "(declare-fun score$ (Nat$) Int)")
        new_lines.insert(0, "(declare-fun nat_bridge$ (Int) Nat$)")

    bridge_asserts = [
        f"(assert (= (score$ (nat_bridge$ (to_int$ {v}))) (to_int$ {v})))"
        for v in nat_vars[:3]
    ]

    final_lines = []
    inserted_asserts = False
    for ln in new_lines:
        if ln.strip() == "(check-sat)" and not inserted_asserts:
            final_lines.extend(bridge_asserts)
            inserted_asserts = True
        final_lines.append(ln)

    if not inserted_asserts:
        final_lines.extend(bridge_asserts)

    out.write_text("\n".join(final_lines) + "\n")
    print(f"[BRIDGE {len(bridge_asserts)} vars] {rel}")
PY

echo
echo "Done. New folder: $DST"
find "$DST" -name "*.smt2" | wc -l
