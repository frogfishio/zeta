#!/bin/sh
set -eu

# Resolve the platform pack root (…/dist/integration-pack/<platform>)
pack_platform_root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"

cc \
  -I"$pack_platform_root/include" \
  "$(dirname -- "$0")/sys_info_time_now.c" \
  "$pack_platform_root/lib/libzingcore25.a" \
  -o ./sys_info_time_now

echo "Wrote ./sys_info_time_now"
