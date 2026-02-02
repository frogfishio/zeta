#!/bin/bash
set -euo pipefail

root_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
zld_bin="$root_dir/bin/zld"
fix_dir="$root_dir/test/conform_zld"
build_dir="$root_dir/build/conform_zld"
rg_bin="$(command -v rg 2>/dev/null || command -v grep 2>/dev/null)"

if [ ! -x "$zld_bin" ]; then
  echo "missing executable: $zld_bin" >&2
  exit 2
fi

mkdir -p "$build_dir"

if ! "$zld_bin" --conform --verify < "$fix_dir/ok.jsonl" >/dev/null 2>"$build_dir/ok.err"; then
  echo "expected ok.jsonl to pass" >&2
  cat "$build_dir/ok.err" >&2
  exit 1
fi

if "$zld_bin" --conform --verify < "$fix_dir/ok_no_loc.jsonl" >/dev/null 2>"$build_dir/ok_no_loc.err"; then
  echo "expected ok_no_loc.jsonl to fail in non-strict" >&2
  exit 1
fi

if "$zld_bin" --conform --verify < "$fix_dir/missing_loc.jsonl" >/dev/null 2>"$build_dir/missing_loc.err"; then
  echo "expected missing_loc.jsonl to fail" >&2
  exit 1
fi
"$rg_bin" -q "conformance: missing loc.line" "$build_dir/missing_loc.err"

if "$zld_bin" --conform --verify < "$fix_dir/empty_sym.jsonl" >/dev/null 2>"$build_dir/empty_sym.err"; then
  echo "expected empty_sym.jsonl to fail" >&2
  exit 1
fi
"$rg_bin" -q "conformance: empty operand string" "$build_dir/empty_sym.err"

if ! "$zld_bin" --conform=strict --verify < "$fix_dir/ok_no_loc.jsonl" >/dev/null 2>"$build_dir/ok_no_loc_strict.err"; then
  echo "expected ok_no_loc.jsonl to pass in strict" >&2
  cat "$build_dir/ok_no_loc_strict.err" >&2
  exit 1
fi

if "$zld_bin" --conform=strict --verify < "$fix_dir/bad_ident.jsonl" >/dev/null 2>"$build_dir/bad_ident.err"; then
  echo "expected bad_ident.jsonl to fail in strict" >&2
  exit 1
fi
"$rg_bin" -q "conformance: invalid label" "$build_dir/bad_ident.err"

echo "zld conformance tests passed."
