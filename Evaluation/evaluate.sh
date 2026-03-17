#!/usr/bin/env bash
# =============================================================================
# evaluate.sh  —  Nat-to-Int preprocessing pass evaluation
#
# Usage:  bash evaluate.sh
#
# Expects the following layout (all relative to ~/Downloads/SMT-LIB-nat-main):
#   cvc5-1.3.2/                       — cvc5 source tree
#   cvc5-1.3.2/src/preprocessing/passes/nat_to_int.cpp
#   Evaluation/base40_raw/            — 40 baseline .smt2 files
#   Evaluation/rewritten_problems/nat_to_nat/   — 40 .smt2
#   Evaluation/rewritten_problems/int_to_nat/   — 40 .smt2
#   Evaluation/rewritten_problems/nat_to_int/   — 40 .smt2
#
# What the script does:
#   1. Patches USE_TOTAL_DEFINITION and compiles cvc5 TWICE
#      (once for total-def, once for partial-def).
#   2. Runs each binary against:
#        a) base40_raw         (ground truth — no nat-to-int pass involved)
#        b) rewritten_problems (total-def binary)
#        c) rewritten_problems (partial-def binary)
#   3. Records result (sat/unsat/unknown/timeout/error) and wall-clock time.
#   4. Prints a summary table with per-category counts and
#      median + mean runtime for each mode.
# =============================================================================
set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
TIMEOUT_SEC=10                         # wall-clock kill threshold
TLIMIT_MS=10000                        # cvc5 --tlimit value (ms)
ROOT=~/Downloads/SMT-LIB-nat-main
CVC5_SRC="$ROOT/cvc5-1.3.2"
NAT_TO_INT_CPP="$CVC5_SRC/src/preprocessing/passes/nat_to_int.cpp"
BUILD_DIR="$CVC5_SRC/build"
EVAL_DIR="$ROOT/Evaluation"
BASE_DIR="$EVAL_DIR/base40_raw"
REWRITTEN_DIR="$EVAL_DIR/rewritten_problems"
RESULTS_DIR="$ROOT/results"
TOTAL_BIN="$BUILD_DIR/bin/cvc5_total"
PARTIAL_BIN="$BUILD_DIR/bin/cvc5_partial"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log()  { echo "[$(date '+%H:%M:%S')] $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }

# Portable timeout: macOS ships 'gtimeout' via coreutils; Linux has 'timeout'
if command -v gtimeout &>/dev/null; then
    TIMEOUT_CMD="gtimeout"
elif command -v timeout &>/dev/null; then
    TIMEOUT_CMD="timeout"
else
    die "Neither 'timeout' nor 'gtimeout' found. Install coreutils: brew install coreutils"
fi

# Portable stat for file size (used only for sanity; not critical)
python3_available() { command -v python3 &>/dev/null; }

# Compute median of a newline-separated list of numbers
median() {
    python3 - "$@" <<'PYEOF'
import sys, statistics
nums = [float(x) for x in sys.stdin.read().split() if x]
if not nums:
    print("N/A")
else:
    print(f"{statistics.median(nums):.3f}")
PYEOF
}

mean() {
    python3 - "$@" <<'PYEOF'
import sys
nums = [float(x) for x in sys.stdin.read().split() if x]
if not nums:
    print("N/A")
else:
    print(f"{sum(nums)/len(nums):.3f}")
PYEOF
}

# ---------------------------------------------------------------------------
# Step 0 — Sanity checks
# ---------------------------------------------------------------------------
log "Checking prerequisites..."
[ -d "$CVC5_SRC" ]        || die "cvc5 source not found at $CVC5_SRC"
[ -f "$NAT_TO_INT_CPP" ]  || die "nat_to_int.cpp not found at $NAT_TO_INT_CPP"
[ -d "$BASE_DIR" ]        || die "base40_raw not found at $BASE_DIR"
[ -d "$REWRITTEN_DIR" ]   || die "rewritten_problems not found at $REWRITTEN_DIR"
command -v cmake &>/dev/null || die "cmake not found — install it first (brew install cmake)"
command -v python3 &>/dev/null || die "python3 required for statistics"

mkdir -p "$RESULTS_DIR"

# ---------------------------------------------------------------------------
# Step 1 — Build helper: patch flag and compile
# ---------------------------------------------------------------------------
build_cvc5() {
    local mode="$1"          # "total" or "partial"
    local dest_bin="$2"      # where to copy the finished binary

    local flag_value
    if [ "$mode" = "total" ]; then
        flag_value="true"
    else
        flag_value="false"
    fi

    log "=== Building cvc5 with USE_TOTAL_DEFINITION=$flag_value ==="

    # Patch the constexpr line in nat_to_int.cpp (macOS sed requires '' after -i)
    sed -i '' \
        "s/static constexpr bool USE_TOTAL_DEFINITION = .*;/static constexpr bool USE_TOTAL_DEFINITION = ${flag_value};/" \
        "$NAT_TO_INT_CPP"

    # Verify patch
    grep "USE_TOTAL_DEFINITION" "$NAT_TO_INT_CPP" | grep "= ${flag_value};" \
        || die "sed patch for $mode failed — check $NAT_TO_INT_CPP"

    # Configure using cvc5's own configure.sh (only on first build)
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        log "Configuring via configure.sh (first time — will download missing deps)..."
        cd "$CVC5_SRC"
        bash configure.sh production \
            --auto-download \
            --name=build \
        || die "configure.sh failed"
        cd - > /dev/null
    fi

    log "Compiling... (this may take 30-40 minutes on first build, <1 min on subsequent)"
    cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc)" \
        || die "cmake build failed for $mode"

    # Copy binary out before the next patch overwrites the object files
    # cvc5 binary location differs: static builds go to build/bin/cvc5 or build/cvc5
    local built_bin=""
    if [ -f "$BUILD_DIR/bin/cvc5" ]; then
        built_bin="$BUILD_DIR/bin/cvc5"
    elif [ -f "$BUILD_DIR/cvc5" ]; then
        built_bin="$BUILD_DIR/cvc5"
    else
        die "Could not find compiled cvc5 binary under $BUILD_DIR — searched bin/cvc5 and cvc5"
    fi
    cp "$built_bin" "$dest_bin"
    chmod +x "$dest_bin"
    log "Binary saved to $dest_bin"
}

# ---------------------------------------------------------------------------
# Step 2 — Run one .smt2 file with a given binary, return result + time
#           Writes two variables into the caller's scope via stdout:
#             RESULT  (sat|unsat|unknown|timeout|error)
#             ELAPSED (seconds, float)
# ---------------------------------------------------------------------------
run_instance() {
    local binary="$1"
    local smt2="$2"
    local extra_flags="${3:-}"       # e.g. "-o nat-to-int"

    local elapsed output exit_code start_time end_time

    # Use python3 for sub-second timing (macOS date does not support %3N)
    start_time=$(python3 -c "import time; print(time.time())")

    # Run with both OS timeout and cvc5's own --tlimit
    set +e
    output=$( $TIMEOUT_CMD "$TIMEOUT_SEC" \
                  "$binary" --tlimit="$TLIMIT_MS" $extra_flags "$smt2" 2>&1 )
    exit_code=$?
    set -e

    end_time=$(python3 -c "import time; print(time.time())")
    elapsed=$(python3 -c "print(f'{${end_time} - ${start_time}:.3f}')")

    if [ $exit_code -eq 124 ] || [ $exit_code -eq 143 ]; then
        # 124 = timeout killed by gtimeout/timeout; 143 = SIGTERM
        echo "timeout $elapsed"
    elif echo "$output" | grep -qE '^unsat$'; then
        echo "unsat $elapsed"
    elif echo "$output" | grep -qE '^sat$'; then
        echo "sat $elapsed"
    elif echo "$output" | grep -qE '^unknown$'; then
        echo "unknown $elapsed"
    else
        echo "error $elapsed"
    fi
}

# ---------------------------------------------------------------------------
# Step 3 — Run a whole directory of .smt2 files, write CSV
#   CSV columns: filename, result, elapsed_sec
# ---------------------------------------------------------------------------
run_benchmark_set() {
    local label="$1"          # e.g. "total_nat_to_nat"
    local binary="$2"
    local smt2_dir="$3"
    local extra_flags="${4:-}"
    local csv="$RESULTS_DIR/${label}.csv"

    log "--- Running [$label] on $smt2_dir ---"
    echo "file,result,elapsed_sec" > "$csv"

    local count=0 total_files
    total_files=$(find "$smt2_dir" -maxdepth 1 -name "*.smt2" | wc -l | tr -d ' ')

    for smt2 in "$smt2_dir"/*.smt2; do
        [ -f "$smt2" ] || continue
        count=$((count + 1))
        local name
        name=$(basename "$smt2")
        read -r result elapsed <<< "$(run_instance "$binary" "$smt2" "$extra_flags")"
        echo "${name},${result},${elapsed}" >> "$csv"
        printf "  [%3d/%3d] %-45s  %-8s  %ss\n" "$count" "$total_files" "$name" "$result" "$elapsed"
    done

    log "Results written to $csv"
}

# ---------------------------------------------------------------------------
# Step 4 — Summary statistics from a CSV
# ---------------------------------------------------------------------------
summarize() {
    local label="$1"
    local csv="$RESULTS_DIR/${label}.csv"

    [ -f "$csv" ] || { echo "  (no CSV found for $label)"; return; }

    local n_sat n_unsat n_unknown n_timeout n_error times
    n_sat=$(    awk -F, 'NR>1 && $2=="sat"     {c++} END{print c+0}' "$csv")
    n_unsat=$(  awk -F, 'NR>1 && $2=="unsat"   {c++} END{print c+0}' "$csv")
    n_unknown=$(awk -F, 'NR>1 && $2=="unknown" {c++} END{print c+0}' "$csv")
    n_timeout=$(awk -F, 'NR>1 && $2=="timeout" {c++} END{print c+0}' "$csv")
    n_error=$(  awk -F, 'NR>1 && $2=="error"   {c++} END{print c+0}' "$csv")

    # Runtimes for non-timeout, non-error instances only
    times=$(awk -F, 'NR>1 && $2!="timeout" && $2!="error" {print $3}' "$csv")

    local med mean_val
    med=$(     echo "$times" | median)
    mean_val=$(echo "$times" | mean)

    printf "  %-30s  sat=%-4s unsat=%-4s unknown=%-4s timeout=%-4s error=%-4s  median=%-8s mean=%s\n" \
           "$label" "$n_sat" "$n_unsat" "$n_unknown" "$n_timeout" "$n_error" "${med}s" "${mean_val}s"
}

# ---------------------------------------------------------------------------
# MAIN
# ---------------------------------------------------------------------------

# --- Build both binaries ---
build_cvc5 "total"   "$TOTAL_BIN"
build_cvc5 "partial" "$PARTIAL_BIN"

# Restore to total (default) after both builds
sed -i '' \
    "s/static constexpr bool USE_TOTAL_DEFINITION = .*/static constexpr bool USE_TOTAL_DEFINITION = true;/" \
    "$NAT_TO_INT_CPP"

# --- Ground truth: base40_raw with total binary (no nat-to-int pass output needed) ---
# base40_raw problems have no Nat$, so the pass is a no-op; we just want solve times
run_benchmark_set "baseline_base40"   "$TOTAL_BIN"   "$BASE_DIR"  ""

# --- Total definition on all three rewritten categories ---
for cat in nat_to_nat int_to_nat nat_to_int; do
    run_benchmark_set "total_${cat}"   "$TOTAL_BIN"   "$REWRITTEN_DIR/$cat"  "-o nat-to-int"
done

# --- Partial definition on all three rewritten categories ---
for cat in nat_to_nat int_to_nat nat_to_int; do
    run_benchmark_set "partial_${cat}" "$PARTIAL_BIN" "$REWRITTEN_DIR/$cat"  "-o nat-to-int"
done

# ---------------------------------------------------------------------------
# Print final summary
# ---------------------------------------------------------------------------
python3 - "$RESULTS_DIR" <<'PYEOF'
import os, sys, csv, statistics

results_dir = sys.argv[1]

def load(label):
    path = os.path.join(results_dir, f"{label}.csv")
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows

cats = ["nat_to_nat", "int_to_nat", "nat_to_int"]

def compute(rows):
    n_sat   = sum(1 for r in rows if r["result"] == "sat")
    n_unsat = sum(1 for r in rows if r["result"] == "unsat")
    n_other = sum(1 for r in rows if r["result"] not in ("sat", "unsat"))
    times   = [float(r["elapsed_sec"]) for r in rows if r["result"] not in ("timeout", "error")]
    med     = f"{statistics.median(times):.3f}" if times else "N/A"
    mean    = f"{sum(times)/len(times):.3f}"    if times else "N/A"
    return n_sat, n_unsat, n_other, med, mean

baseline     = load("baseline_base40")
total_rows   = sum([load(f"total_{c}")   for c in cats], [])
partial_rows = sum([load(f"partial_{c}") for c in cats], [])

rows_data = [
    ("Baseline (40)",    *compute(baseline)),
    ("Total def (120)",  *compute(total_rows)),
    ("Partial def (120)",*compute(partial_rows)),
]

# Print to terminal
print()
print("=" * 75)
print("  EVALUATION SUMMARY")
print("=" * 75)
header = "  {:<22} {:<7} {:<7} {:<15} {:<12} {}".format("Mode","sat","unsat","unkn/timeout","median(s)","mean(s)")
print(header)
print("-" * 75)
for label, n_sat, n_unsat, n_other, med, mean in rows_data:
    print("  {:<22} {:<7} {:<7} {:<15} {:<12} {}".format(label, n_sat, n_unsat, n_other, med, mean))
print("=" * 75)

# Save summary CSV
summary_path = os.path.join(results_dir, "summary.csv")
with open(summary_path, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["mode", "sat", "unsat", "unknown_timeout", "median_sec", "mean_sec"])
    for label, n_sat, n_unsat, n_other, med, mean in rows_data:
        writer.writerow([label, n_sat, n_unsat, n_other, med, mean])
print("\n  Summary CSV saved to: " + summary_path)
PYEOF
