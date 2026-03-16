#!/usr/bin/env bash
set -euo pipefail

ARTIFACT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$ARTIFACT_ROOT/.." && pwd)"

BASE="$ARTIFACT_ROOT/base40_raw"
OUT="$ARTIFACT_ROOT/generated_benchmarks"
SCRIPTS="$ARTIFACT_ROOT/scripts"

TMP="$ROOT/_artifact_tmp_base40"
BACKUP="$ROOT/_artifact_backup_base40"

echo "== Clean old outputs =="
rm -rf "$OUT" "$TMP" "$BACKUP"
mkdir -p "$OUT/nat_to_int" "$OUT/int_to_nat" "$OUT/nat_to_nat"
mkdir -p "$TMP/raw_inputs" "$TMP/results" "$BACKUP"

echo "== Back up root state =="
[ -d "$ROOT/nat_inputs" ] && cp -R "$ROOT/nat_inputs" "$BACKUP/nat_inputs" || true
[ -d "$ROOT/nat_inputs_natstep" ] && cp -R "$ROOT/nat_inputs_natstep" "$BACKUP/nat_inputs_natstep" || true
[ -d "$ROOT/nat_inputs_natvar_200" ] && cp -R "$ROOT/nat_inputs_natvar_200" "$BACKUP/nat_inputs_natvar_200" || true
[ -d "$ROOT/nat_inputs_composed_bridge_200" ] && cp -R "$ROOT/nat_inputs_composed_bridge_200" "$BACKUP/nat_inputs_composed_bridge_200" || true
[ -f "$ROOT/results/selected_100.csv" ] && cp "$ROOT/results/selected_100.csv" "$BACKUP/selected_100.csv" || true

echo "== Prepare temporary raw inputs =="
cp "$BASE"/*.smt2 "$TMP/raw_inputs/"

python3 - <<PY
from pathlib import Path
import csv

root = Path("$ROOT")
tmp = Path("$TMP")

rows = []
for f in sorted((tmp / "raw_inputs").glob("*.smt2")):
    rows.append({
        "logic": "ARTIFACT",
        "file": str(tmp / "raw_inputs" / f.name),
        "result": "unknown",
        "time": "0.0",
    })

out = root / "results" / "selected_100.csv"
with out.open("w", newline="") as fp:
    w = csv.DictWriter(fp, fieldnames=["logic","file","result","time"])
    w.writeheader()
    w.writerows(rows)

print("wrote", out, "with", len(rows), "rows")
PY

echo "== Clean generated folders in root =="
rm -rf "$ROOT/nat_inputs" "$ROOT/nat_inputs_natstep" "$ROOT/nat_inputs_natvar_200" "$ROOT/nat_inputs_composed_bridge_200"

echo "== Run rewrite_selected_to_nat.py =="
python3 "$SCRIPTS/rewrite_selected_to_nat.py" > "$TMP/rewrite.log" 2>&1

echo "nat_inputs count:"
find "$ROOT/nat_inputs" -name "*.smt2" | wc -l

echo "== Build Nat$ -> Int control family =="
find "$ROOT/nat_inputs" -name "*.smt2" | while IFS= read -r f
do
  if grep -q "declare-fun .* () Nat\\$" "$f"; then
    cp "$f" "$OUT/nat_to_int/"
  fi
done
echo "nat_to_int count:"
find "$OUT/nat_to_int" -name "*.smt2" | wc -l

echo "== Build Nat$ -> Nat$ family =="
cp "$SCRIPTS/inject_nat_step.sh" "$ROOT/inject_nat_step.sh"
chmod +x "$ROOT/inject_nat_step.sh"
bash "$ROOT/inject_nat_step.sh"

find "$ROOT/nat_inputs_natstep" -name "*.smt2" | while IFS= read -r f
do
  if grep -q "to_int\\$ (nat_step\\$" "$f"; then
    cp "$f" "$OUT/nat_to_nat/"
  fi
done
echo "nat_to_nat count:"
find "$OUT/nat_to_nat" -name "*.smt2" | wc -l

echo "== Build Int -> Nat$ family =="
mkdir -p "$ROOT/nat_inputs_natvar_200"
rm -rf "$ROOT/nat_inputs_natvar_200"/*
find "$ROOT/nat_inputs" -name "*.smt2" | while IFS= read -r f
do
  if grep -q "declare-fun .* () Nat\\$" "$f"; then
    rel=${f#"$ROOT/nat_inputs/"}
    mkdir -p "$ROOT/nat_inputs_natvar_200/$(dirname "$rel")"
    cp "$f" "$ROOT/nat_inputs_natvar_200/$rel"
  fi
done

cp "$SCRIPTS/inject_composed_bridge_on_natvar_200.sh" "$ROOT/inject_composed_bridge_on_natvar_200.sh"
chmod +x "$ROOT/inject_composed_bridge_on_natvar_200.sh"
bash "$ROOT/inject_composed_bridge_on_natvar_200.sh"

if [ -d "$ROOT/nat_inputs_composed_bridge_200" ]; then
  BRIDGEDIR="$ROOT/nat_inputs_composed_bridge_200"
elif [ -d "$ROOT/nat_inputs_composed_bridge" ]; then
  BRIDGEDIR="$ROOT/nat_inputs_composed_bridge"
else
  echo "Could not find bridge output directory." >&2
  exit 1
fi

find "$BRIDGEDIR" -name "*.smt2" | while IFS= read -r f
do
  if grep -q "score\\$ (nat_bridge\\$ (to_int\\$" "$f"; then
    cp "$f" "$OUT/int_to_nat/"
  fi
done
echo "int_to_nat count:"
find "$OUT/int_to_nat" -name "*.smt2" | wc -l

echo "== Final generated counts =="
find "$OUT/nat_to_int" -name "*.smt2" | wc -l
find "$OUT/int_to_nat" -name "*.smt2" | wc -l
find "$OUT/nat_to_nat" -name "*.smt2" | wc -l

echo "== Restore root state =="
rm -rf "$ROOT/nat_inputs" "$ROOT/nat_inputs_natstep" "$ROOT/nat_inputs_natvar_200" "$ROOT/nat_inputs_composed_bridge_200"
[ -d "$BACKUP/nat_inputs" ] && cp -R "$BACKUP/nat_inputs" "$ROOT/nat_inputs" || true
[ -d "$BACKUP/nat_inputs_natstep" ] && cp -R "$BACKUP/nat_inputs_natstep" "$ROOT/nat_inputs_natstep" || true
[ -d "$BACKUP/nat_inputs_natvar_200" ] && cp -R "$BACKUP/nat_inputs_natvar_200" "$ROOT/nat_inputs_natvar_200" || true
[ -d "$BACKUP/nat_inputs_composed_bridge_200" ] && cp -R "$BACKUP/nat_inputs_composed_bridge_200" "$ROOT/nat_inputs_composed_bridge_200" || true
[ -f "$BACKUP/selected_100.csv" ] && cp "$BACKUP/selected_100.csv" "$ROOT/results/selected_100.csv" || true

rm -f "$ROOT/inject_nat_step.sh" "$ROOT/inject_composed_bridge_on_natvar_200.sh"

echo "Done."