#!/usr/bin/env bash
# Usage: test.sh <total|partial>
# Runs the nat-to-int transformation on each example and diffs against
# the corresponding [problem]_<mode>.exp file.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAT_TO_INT="$SCRIPT_DIR/../cvc5-1.3.2/nat_to_int.sh"

if [ $# -ne 1 ] || { [ "$1" != "total" ] && [ "$1" != "partial" ]; }; then
    echo "Usage: $0 <total|partial>" >&2
    exit 1
fi

MODE="$1"

PROBLEMS=(
    var
    numeral
    nest_fun
    arith
    midterm_example1
    midterm_example2
    midterm_example3
    midterm_example4
)

PASS=0
FAIL=0
SKIP=0

# Strip comment lines (starting with ;) so .exp files can carry documentation
# without affecting the diff.
strip_comments() {
    grep -v '^[[:space:]]*;'
}

for prob in "${PROBLEMS[@]}"; do
    input="$SCRIPT_DIR/$prob.smt2"
    expected="$SCRIPT_DIR/${prob}_${MODE}.exp"

    if [ ! -f "$input" ]; then
        echo "SKIP  $prob  ($input not found)"
        ((SKIP++))
        continue
    fi
    if [ ! -f "$expected" ]; then
        echo "SKIP  $prob  (${prob}_${MODE}.exp not found)"
        ((SKIP++))
        continue
    fi

    actual=$("$NAT_TO_INT" "$input" 2>&1)
    diff_out=$(diff \
        <(echo "$actual"   | strip_comments) \
        <(cat "$expected"  | strip_comments))

    if [ -z "$diff_out" ]; then
        echo "PASS  $prob"
        ((PASS++))
    else
        echo "FAIL  $prob"
        # Indent diff output for readability
        echo "$diff_out" | sed 's/^/      /'
        ((FAIL++))
    fi
done

echo
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
