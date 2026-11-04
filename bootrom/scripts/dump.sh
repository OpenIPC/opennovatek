#!/bin/sh
# Dump a Novatek mask-ROM from a live camera over SSH, then verify it
# against the tracked MANIFEST.
#
# Usage: dump.sh <ssh-host> <chip>
#
# Requires root SSH on <ssh-host> and busybox devmem there. The ROM is read
# a word at a time from its home address (MANIFEST "home:"), because a
# plain dd of /dev/mem returns nothing on these kernels.

set -eu

if [ $# -ne 2 ]; then
    echo "usage: $0 <ssh-host> <chip>" >&2
    exit 64
fi

HOST=$1
CHIP=$2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CHIPDIR=$ROOT/$CHIP
MANIFEST=$CHIPDIR/MANIFEST

if [ ! -f "$MANIFEST" ]; then
    echo "$0: $MANIFEST missing" >&2
    exit 65
fi

HOME_ADDR=$(awk '/^home:/ { print $2 }' "$MANIFEST")
SIZE=$(awk '/^size:/ { print $2 }' "$MANIFEST")
OUT=$CHIPDIR/bootrom.bin

ssh -o BatchMode=yes "root@$HOST" \
    "a=\$(($HOME_ADDR)); e=\$((a + $SIZE)); while [ \$a -lt \$e ]; do devmem \$a 32; a=\$((a + 4)); done" |
    python3 -c '
import struct, sys
words = [int(l, 16) for l in sys.stdin if l.startswith("0x")]
sys.stdout.buffer.write(b"".join(struct.pack("<I", w) for w in words))
' > "$OUT"

echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
exec "$ROOT/scripts/verify.sh" "$CHIP"
