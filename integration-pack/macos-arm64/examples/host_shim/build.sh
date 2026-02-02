#!/bin/sh
set -eu

if [ $# -ne 1 ]; then
  echo "usage: $0 <program.jsonl>" >&2
  exit 2
fi

jsonl="$1"

# Resolve the platform pack root (…/dist/integration-pack/<platform>)
pack_platform_root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"

lower="$pack_platform_root/bin/lower"

out_o="./guest.o"
out_bin="./guest"

"$lower" --input "$jsonl" --o "$out_o"

cc \
  -I"$pack_platform_root/include" \
  "$(dirname -- "$0")/runner.c" \
  "$out_o" \
  "$pack_platform_root/lib/libzingcore25.a" \
  -o "$out_bin"

echo "Wrote $out_bin"
