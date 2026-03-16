#!/usr/bin/env bash
set -euo pipefail

EVAL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$EVAL_ROOT/.." && pwd)"

BASE="$EVAL_ROOT/base40_raw"
OUT="$EVAL_ROOT/generated_benchmarks"
SCRIPTS="$EVAL_ROOT/scripts"

TMP="$ROOT/_eval_tmp"
BACKUP="$ROOT/_eval_backup"

echo "== Clean old outputs =="
rm -rf "$OUT" "$TMP" "$BACKUP"
mkdir -p "$OUT/nat_to_int" "$OUT/int_to_nat" "$OUT/nat_to_nat"
mkdir -p "$TMP/raw_inputs" "$TMP/results" "$BACKUP"
mkdir -p "$ROOT/results"
echo "== Backup current repo state =="
[ -d "$ROOT/nat_inputs" ] && cp -R "$ROOT/nat_inputs" "$BACKUP/nat_inputs" || true
[ -f "$ROOT/results/selected_100.csv" ] && cp "$ROOT/results/selected_100.csv" "$BACKUP/selected_100.csv" || true

echo "== Copy base40 raw inputs =="
cp "$BASE"/*.smt2 "$TMP/raw_inputs/"

python3 - <<PY
from pathlib import Path
import csv

root = Path("$ROOT")
tmp = Path("$TMP")

rows = []
for f in sorted((tmp / "raw_inputs").glob("*.smt2")):
    rows.append({
        "logic": "EVAL",
        "file": str(tmp / "raw_inputs" / f.name),
        "result": "unknown",
        "time": "0.0",
    })

out = root / "results" / "selected_100.csv"
with out.open("w", newline="") as fp:
    w = csv.DictWriter(fp, fieldnames=["logic","file","result","time"])
    w.writeheader()
    w.writerows(rows)

print("Created temporary selected_100.csv with", len(rows), "rows")
PY

echo "== Remove old nat_inputs folders =="
rm -rf "$ROOT/nat_inputs" "$ROOT/nat_inputs_natstep" "$ROOT/nat_inputs_natvar_200" "$ROOT/nat_inputs_composed_bridge_200"

echo "== Run rewrite_selected_to_nat.py =="
python3 "$SCRIPTS/rewrite_selected_to_nat.py"

echo "nat_inputs count:"
find "$ROOT/nat_inputs" -name "*.smt2" | wc -l

echo "== Nat$ -> Int family =="
find "$ROOT/nat_inputs" -name "*.smt2" | while IFS= read -r f
do
  if grep -q "declare-fun .* () Nat\\$" "$f"; then
    cp "$f" "$OUT/nat_to_int/"
  fi
done

echo "== Nat$ -> Nat$ family =="
chmod +x "$SCRIPTS/inject_nat_step.sh"
bash "$SCRIPTS/inject_nat_step.sh"

find "$ROOT/nat_inputs_natstep" -name "*.smt2" | while IFS= read -r f
do
  if grep -q "to_int\\$ (nat_step\\$" "$f"; then
    cp "$f" "$OUT/nat_to_nat/"
  fi
done

echo "== Int -> Nat$ family =="

mkdir -p "$ROOT/nat_inputs_natvar_200"
find "$ROOT/nat_inputs" -name "*.smt2" | while IFS= read -r f
do
  if grep -q "declare-fun .* () Nat\\$" "$f"; then
    rel=${f#"$ROOT/nat_inputs/"}
    mkdir -p "$ROOT/nat_inputs_natvar_200/$(dirname "$rel")"
    cp "$f" "$ROOT/nat_inputs_natvar_200/$rel"
  fi
done

chmod +x "$SCRIPTS/inject_composed_bridge_on_natvar_200.sh"
bash "$SCRIPTS/inject_composed_bridge_on_natvar_200.sh"

BRIDGE="$ROOT/nat_inputs_composed_bridge_200"

find "$BRIDGE" -name "*.smt2" | while IFS= read -r f
do
  if grep -q "score\\$ (nat_bridge\\$ (to_int\\$" "$f"; then
    cp "$f" "$OUT/int_to_nat/"
  fi
done

echo "== Final counts =="
find "$OUT/nat_to_int" -name "*.smt2" | wc -l
find "$OUT/int_to_nat" -name "*.smt2" | wc -l
find "$OUT/nat_to_nat" -name "*.smt2" | wc -l

echo "== Restore original repo state =="

rm -rf "$ROOT/nat_inputs" "$ROOT/nat_inputs_natstep" "$ROOT/nat_inputs_natvar_200" "$ROOT/nat_inputs_composed_bridge_200"

[ -d "$BACKUP/nat_inputs" ] && cp -R "$BACKUP/nat_inputs" "$ROOT/nat_inputs"
[ -f "$BACKUP/selected_100.csv" ] && cp "$BACKUP/selected_100.csv" "$ROOT/results/selected_100.csv"


echo "Done."