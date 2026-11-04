#!/bin/sh
# Disassemble a local bootrom dump into a working text file (gitignored).
#
# Usage: disasm.sh <chip> [base-vma]
#
# The base defaults to the MANIFEST's "base:", the address the ROM's code
# runs at.

set -eu

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "usage: $0 <chip> [base-vma]" >&2
    exit 64
fi

CHIP=$1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CHIPDIR=$ROOT/$CHIP
BIN=$CHIPDIR/bootrom.bin
OUT=$CHIPDIR/disasm.txt
BASE=${2:-$(awk '/^base:/ { print $2 }' "$CHIPDIR/MANIFEST")}

if [ ! -f "$BIN" ]; then
    echo "$0: $BIN missing — run dump.sh first" >&2
    exit 66
fi

arm-none-eabi-objdump -D -b binary -m arm --adjust-vma="$BASE" "$BIN" > "$OUT"
echo "wrote $OUT"
