#!/usr/bin/env bash
set -euo pipefail

if [ $# -ne 1 ]; then
  echo "Usage: $0 total|partial" >&2
  exit 1
fi

MODE="$1"
ROOT="$HOME/Downloads/non-incremental"
CVC5DIR="$ROOT/SMT-LIB-nat-main/cvc5-1.3.2"

if [ "$MODE" != "total" ] && [ "$MODE" != "partial" ]; then
  echo "Mode must be 'total' or 'partial'" >&2
  exit 1
fi

cd "$ROOT"

if [ "$MODE" = "total" ]; then
  bash "$CVC5DIR/build_total.sh"
else
  bash "$CVC5DIR/build_partial.sh"
fi

grep "USE_TOTAL_DEFINITION" "$CVC5DIR/src/preprocessing/passes/nat_to_int.cpp"

find nat_inputs -name "*.smt2" | while IFS= read -r f
do
  if grep -q "declare-fun .* () Nat\\$" "$f"; then
    echo "$f"
  fi
done | sort > manifest_nat_to_int_control.txt

grep -R -l "score\\$ (nat_bridge\\$ (to_int\\$" combined_benchmarks_108 \
  | sort > manifest_int_to_nat_to_int.txt

if [ -f meaningful_nat_inputs_natstep.txt ]; then
  sort meaningful_nat_inputs_natstep.txt > manifest_nat_to_nat.txt
else
  find nat_inputs_natstep -name "*.smt2" | while IFS= read -r f
  do
    if grep -q "to_int\\$ (nat_step\\$" "$f"; then
      echo "$f"
    fi
  done | sort > manifest_nat_to_nat.txt
fi

echo "Manifest sizes:"
wc -l manifest_nat_to_int_control.txt
wc -l manifest_int_to_nat_to_int.txt
wc -l manifest_nat_to_nat.txt

LIFTDIR="lifted_${MODE}"
LOGDIR="eval_logs_${MODE}"
CSV="eval_${MODE}.csv"

rm -rf "$LIFTDIR"
mkdir -p "$LIFTDIR/nat_to_int" "$LIFTDIR/int_to_nat" "$LIFTDIR/nat_to_nat"
mkdir -p "$LOGDIR"

while IFS= read -r f
do
  name=$(basename "$f")
  bash "$CVC5DIR/nat_to_int.sh" "$f" > "$LIFTDIR/nat_to_int/$name"
done < manifest_nat_to_int_control.txt

while IFS= read -r f
do
  name=$(basename "$f")
  bash "$CVC5DIR/nat_to_int.sh" "$f" > "$LIFTDIR/int_to_nat/$name"
done < manifest_int_to_nat_to_int.txt

while IFS= read -r f
do
  name=$(basename "$f")
  bash "$CVC5DIR/nat_to_int.sh" "$f" > "$LIFTDIR/nat_to_nat/$name"
done < manifest_nat_to_nat.txt

echo "Lifted count:"
find "$LIFTDIR" -name "*.smt2" | wc -l

echo "family,file,result" > "$CSV"

find "$LIFTDIR" -name "*.smt2" | while IFS= read -r f
do
  family=$(echo "$f" | cut -d/ -f2)
  name=$(basename "$f")

  gtimeout 10s "$CVC5DIR/build/bin/cvc5" "$f" \
    > "$LOGDIR/${family}__${name}.out" \
    2> "$LOGDIR/${family}__${name}.err" || true

  out="$LOGDIR/${family}__${name}.out"

  if grep -Eq '^unsat$' "$out"; then
    r="unsat"
  elif grep -Eq '^sat$' "$out"; then
    r="sat"
  else
    r="unknown"
  fi

  echo "${family},${name},${r}" >> "$CSV"
done

echo
echo "Wrote $CSV"
echo "Overall results:"
cut -d, -f3 "$CSV" | tail -n +2 | grep -E '^(sat|unsat|unknown)$' | sort | uniq -c

echo
echo "Per-family results:"
awk -F, 'NR>1 && $3 ~ /^(sat|unsat|unknown)$/ {print $1,$3}' "$CSV" | sort | uniq -c
