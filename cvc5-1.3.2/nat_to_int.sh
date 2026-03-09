#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -ne 1 ]; then
    echo "Usage: $0 <smt2-file>" >&2
    exit 1
fi

cp "$SCRIPT_DIR/build/src/libcvc5.dll"             "$SCRIPT_DIR/build/bin/libcvc5.dll"
cp "$SCRIPT_DIR/build/src/parser/libcvc5parser.dll" "$SCRIPT_DIR/build/bin/libcvc5parser.dll"

"$SCRIPT_DIR/build/bin/cvc5.exe" -o nat-to-int "$1" 2>/dev/null \
    | awk '/^;; nat-to-int start$/{p=1; next} /^;; nat-to-int end$/{p=0} p'
