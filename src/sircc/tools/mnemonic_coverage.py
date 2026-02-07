#!/usr/bin/env python3
"""
Mnemonic coverage checker for sircc.

Goal:
- Parse the normative mnemonic vocabulary from schema/sir/v1.0/mnemonics.html
- Infer what sircc currently implements (best-effort) from the compiler sources in src/sircc/
- Enforce Milestone 3 (core / ungated) coverage for a curated “base set”

This tool intentionally avoids dependencies (no BeautifulSoup) to keep the repo
lightweight and runnable in minimal environments.
"""

from __future__ import annotations

import argparse
import html as htm
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Iterable


@dataclass(frozen=True)
class SpecMnemonic:
    name: str
    pack: str | None


def strip_tags(s: str) -> str:
    s = re.sub(r"<[^>]+>", "", s)
    s = htm.unescape(s)
    s = " ".join(s.split())
    return s.strip()

def strip_parens_notes(s: str) -> str:
    # Some mnemonic cells include prose notes like "(and i16/i32/i64.*)".
    # Those slashes are not mnemonic separators; drop parenthesized notes before tokenizing.
    return re.sub(r"\([^)]*\)", "", s).strip()


def extract_pack_from_heading(heading_text: str) -> str | None:
    # Headings are like: "SIMD / vector types and operations (simd:v1)"
    m = re.search(r"\(([a-z]+:v1)\)", heading_text)
    if not m:
        return None
    return m.group(1)


def parse_spec_mnemonics(mnemonics_html: str) -> list[SpecMnemonic]:
    out: list[SpecMnemonic] = []
    cur_pack: str | None = None

    # Track the currently active feature pack by scanning headings in order.
    # Pack sections use <h3>... (atomics:v1) ...</h3>, etc.
    for line in mnemonics_html.splitlines():
        if "<h3" in line:
            # In this file, headings are single-line.
            h = strip_tags(line)
            cur_pack = extract_pack_from_heading(h)

        if 'data-label="Mnemonic"' not in line:
            continue

        # We need to capture full <td ...>...</td>. The HTML uses multi-line
        # <td> blocks, so we parse from the full document below instead.

    # Parse from the full document with a regex that also captures the td attrs.
    td_re = re.compile(r"<td([^>]*)data-label=\"Mnemonic\"([^>]*)>(.*?)</td>", re.S | re.I)
    # To associate each cell with a pack, we do a second linear scan that tracks
    # pack transitions and indexes td matches in order.
    td_iter = list(td_re.finditer(mnemonics_html))

    # Build an index of (pos -> pack) using <h3> locations.
    pack_markers: list[tuple[int, str | None]] = [(0, None)]
    for hm in re.finditer(r"<h3[^>]*>.*?</h3>", mnemonics_html, re.S | re.I):
        heading = strip_tags(hm.group(0))
        pack_markers.append((hm.start(), extract_pack_from_heading(heading)))
    pack_markers.sort(key=lambda x: x[0])

    def pack_for_pos(pos: int) -> str | None:
        # last marker whose pos <= current
        cur = None
        for p, pk in pack_markers:
            if p <= pos:
                cur = pk
            else:
                break
        return cur

    for m in td_iter:
        attrs = (m.group(1) + m.group(2)).lower()
        if "colspan" in attrs:
            continue  # section header rows
        text = strip_parens_notes(strip_tags(m.group(3)))
        if not text:
            continue
        # Split combined rows like:
        # - "i8.add / i16.add / i32.add / i64.add"
        # - "atomic.load.i8/i16/i32/i64"
        # - "ptr.cmp.eq/ne"
        #
        # Some rows use a shared-prefix shorthand (no repeated "atomic.load.") which we must expand.
        parts = [p.strip() for p in text.split("/") if p.strip()]
        if not parts:
            continue
        prefix: str | None = None
        expanded: list[str] = []
        for i, part in enumerate(parts):
            if "." in part:
                expanded.append(part)
                # If the next part is a shorthand suffix (no dots), treat this as establishing a prefix.
                if i + 1 < len(parts) and "." not in parts[i + 1]:
                    prefix = part.rsplit(".", 1)[0] + "."
                else:
                    prefix = None
                continue
            if prefix:
                expanded.append(prefix + part)
            else:
                expanded.append(part)

        for part in expanded:
            # Filter out any junk that isn't a mnemonic-like token.
            if not re.match(r"^[A-Za-z][A-Za-z0-9_.]*$", part):
                continue
            out.append(SpecMnemonic(name=part, pack=pack_for_pos(m.start())))
    return out


def extract_prims_from_compiler(csrc: str) -> set[str]:
    # Parse the lower_type_prim table to keep this in sync with sircc.
    m = re.search(r"(?:static\s+)?LLVMTypeRef\s+lower_type_prim\([^{]+\)\s*\{", csrc)
    if not m:
        return set()
    start = m.end()
    end = csrc.find("return NULL;", start)
    if end == -1:
        end = start
    block = csrc[start:end]
    prims = set(re.findall(r'strcmp\(\s*prim\s*,\s*"([^"]+)"\s*\)\s*==\s*0', block))
    return prims


def infer_implemented_mnemonics(csrc: str) -> set[str]:
    impl: set[str] = set()

    # Exact tag handlers.
    for tag in re.findall(r'strcmp\(\s*n->tag\s*,\s*"([A-Za-z0-9_.]+)"\s*\)\s*==\s*0', csrc):
        impl.add(tag)

    # Prefix + op handlers (pattern used by packs like fun/closure/adt):
    #   if (strncmp(n->tag, "closure.", 8) == 0) { const char* op = n->tag + 8; if (strcmp(op, "make") == 0) ... }
    # Infer implemented mnemonics as "<prefix><op>" for any strcmp(op, "...") in the same scope window.
    for pm in re.finditer(r'strncmp\(\s*n->tag\s*,\s*"([A-Za-z0-9_.]+)"\s*,\s*(\d+)\s*\)\s*==\s*0', csrc):
        prefix = pm.group(1)
        n = int(pm.group(2))
        if len(prefix) != n:
            continue
        # Scan forward a bounded window to find a matching "op" slice and its cases.
        window = csrc[pm.end() : pm.end() + 60000]
        m_op = re.search(rf'const\s+char\s*\*\s*op\s*=\s*n->tag\s*\+\s*{n}\s*;', window)
        if not m_op:
            continue
        # Collect strcmp(op, "<suffix>") occurrences after the op definition.
        tail = window[m_op.end() :]
        for suf in re.findall(r'strcmp\(\s*op\s*,\s*"([A-Za-z0-9_.]+)"\s*\)\s*==\s*0', tail):
            impl.add(prefix + suf)

    # Prefix + cc handlers for vec.cmp.<cc>:
    #   if (strncmp(n->tag, "vec.cmp.", 8) == 0) { const char* cc = n->tag + 8; if (strcmp(cc, "eq") == 0) ... }
    for pm in re.finditer(r'strncmp\(\s*n->tag\s*,\s*"([A-Za-z0-9_.]+)"\s*,\s*(\d+)\s*\)\s*==\s*0', csrc):
        prefix = pm.group(1)
        n = int(pm.group(2))
        if len(prefix) != n:
            continue
        if not prefix.endswith("."):
            continue
        window = csrc[pm.end() : pm.end() + 60000]
        m_cc = re.search(rf'const\s+char\s*\*\s*cc\s*=\s*n->tag\s*\+\s*{n}\s*;', window)
        if not m_cc:
            continue
        tail = window[m_cc.end() :]
        for cc in re.findall(r'strcmp\(\s*cc\s*,\s*"([A-Za-z0-9_.]+)"\s*\)\s*==\s*0', tail):
            impl.add(prefix + cc)

    prims = extract_prims_from_compiler(csrc)

    # const.<prim>, load.<prim>, store.<prim>, alloca.<prim> (plus ptr), excluding void.
    for t in sorted(prims | {"ptr"}):
        if t == "void":
            continue
        impl.add(f"const.{t}")
        impl.add(f"load.{t}")
        impl.add(f"store.{t}")
        impl.add(f"alloca.{t}")

    # bool.*
    impl |= {f"bool.{op}" for op in ["not", "and", "or", "xor"]}

    # ptr.*
    impl |= {f"ptr.{op}" for op in ["sym", "sizeof", "alignof", "offset", "cmp.eq", "cmp.ne", "add", "sub", "to_i64", "from_i64"]}

    # f32.* / f64.* (as implemented in sircc lowering).
    float_ops = ["add", "sub", "mul", "div", "neg", "abs", "sqrt", "min", "max"]
    float_cmps = [
        "cmp.oeq",
        "cmp.one",
        "cmp.olt",
        "cmp.ole",
        "cmp.ogt",
        "cmp.oge",
        "cmp.ueq",
        "cmp.une",
        "cmp.ult",
        "cmp.ule",
        "cmp.ugt",
        "cmp.uge",
    ]
    float_from = [f"from_i{w}.{su}" for w in [32, 64] for su in ["s", "u"]]
    for w in [32, 64]:
        for op in float_ops + float_cmps + float_from:
            impl.add(f"f{w}.{op}")

    # i8/i16/i32/i64 mnemonics (as implemented in sircc lowering).
    int_widths = [8, 16, 32, 64]
    int_ops = [
        "add",
        "sub",
        "mul",
        "and",
        "or",
        "xor",
        "not",
        "neg",
        "eqz",
        "min.s",
        "min.u",
        "max.s",
        "max.u",
        "shl",
        "shr.s",
        "shr.u",
        "div.s.trap",
        "div.u.trap",
        "rem.s.trap",
        "rem.u.trap",
        "div.s.sat",
        "div.u.sat",
        "rem.s.sat",
        "rem.u.sat",
        "rotl",
        "rotr",
        "clz",
        "ctz",
        "popc",
    ]
    int_cmps = [f"cmp.{cc}" for cc in ["eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge"]]
    for w in int_widths:
        for op in int_ops + int_cmps:
            impl.add(f"i{w}.{op}")

    # iN.(zext|sext|trunc).iM
    for dst in int_widths:
        for src in int_widths:
            if dst > src:
                impl.add(f"i{dst}.zext.i{src}")
                impl.add(f"i{dst}.sext.i{src}")
            if dst < src:
                impl.add(f"i{dst}.trunc.i{src}")

    # iN.trunc_sat_f{32,64}.{s,u}
    for dst in int_widths:
        for src in [32, 64]:
            for su in ["s", "u"]:
                impl.add(f"i{dst}.trunc_sat_f{src}.{su}")

    return impl


def pick_m3_core_candidates(spec: Iterable[SpecMnemonic]) -> set[str]:
    # Milestone 3 is the “base” set (ungated). Pack association in the HTML is
    # descriptive, but we derive gating from mnemonic prefixes to stay robust
    # even if heading markup changes.
    core = {m.name for m in spec if pack_for_mnemonic(m.name) == "core"}

    # Note: `alloca` is a bare mnemonic in the spec.
    m3_re = re.compile(
        r"^(alloca$|"
        r"i(8|16|32|64)\.|"
        r"f(32|64)\.|"
        r"bool\.|"
        r"ptr\.|"
        r"const\.|"
        r"load\.|"
        r"store\.|"
        r"alloca\.|"
        r"mem\.(copy|fill)$|"
        r"eff\.fence$|"
        r"call(\.indirect)?$|"
        r"term\.)"
    )
    return {m for m in core if m3_re.match(m)}


def pack_for_mnemonic(m: str) -> str:
    if m.startswith("atomic."):
        return "atomics:v1"
    if m == "load.vec" or m == "store.vec":
        return "simd:v1"
    if m.startswith("vec."):
        return "simd:v1"
    if m.startswith("adt."):
        return "adt:v1"
    if m.startswith("fun."):
        return "fun:v1"
    if m == "call.fun":
        return "fun:v1"
    if m.startswith("closure.") or m.startswith("call.closure"):
        return "closure:v1"
    if m.startswith("coro."):
        return "coro:v1"
    if m.startswith("eh.") or m.startswith("term.invoke") or m.startswith("term.throw") or m.startswith("term.resume"):
        return "eh:v1"
    if m.startswith("gc."):
        return "gc:v1"
    if m.startswith("sem."):
        return "sem:v1"
    return "core"


def c_escape(s: str) -> str:
    # Minimal C string literal escaping (UTF-8 preserved).
    return (
        s.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def emit_support_table(spec: list[SpecMnemonic], impl: set[str], out_c: pathlib.Path, out_h: pathlib.Path) -> None:
    spec_all = sorted({m.name for m in spec})
    spec_core = sorted([m for m in spec_all if pack_for_mnemonic(m) == "core"])
    impl_in_spec = sorted([m for m in impl if m in set(spec_all)])

    # Missing from spec, grouped by pack.
    missing_by_pack: dict[str, list[str]] = {}
    for pack in ["core", "atomics:v1", "simd:v1", "adt:v1", "fun:v1", "closure:v1", "coro:v1", "eh:v1", "gc:v1", "sem:v1"]:
        missing_by_pack.setdefault(pack, [])

    impl_set = set(impl_in_spec)
    for m in spec_all:
        if m in impl_set:
            continue
        pack = pack_for_mnemonic(m)
        missing_by_pack.setdefault(pack, []).append(m)
    for k in list(missing_by_pack.keys()):
        missing_by_pack[k] = sorted(set(missing_by_pack[k]))

    # Milestone 3 core candidates and missing.
    m3 = pick_m3_core_candidates(spec)
    m3_missing = sorted([m for m in m3 if m not in impl_set])

    # Header (externs).
    out_h.write_text(
        "\n".join(
            [
                "// Generated by src/sircc/tools/mnemonic_coverage.py (do not edit).",
                "#pragma once",
                "",
                "#include <stddef.h>",
                "",
                "typedef struct {",
                "  const char* pack; // \"core\" or \"<pack>:v1\"",
                "  const char* const* items;",
                "  size_t count;",
                "} SirccSupportList;",
                "",
                'extern const char* const sircc_support_spec_all[];',
                "extern const size_t sircc_support_spec_all_count;",
                'extern const char* const sircc_support_spec_core[];',
                "extern const size_t sircc_support_spec_core_count;",
                'extern const char* const sircc_support_impl_in_spec[];',
                "extern const size_t sircc_support_impl_in_spec_count;",
                "extern const SirccSupportList sircc_support_missing_by_pack[];",
                "extern const size_t sircc_support_missing_by_pack_count;",
                'extern const char* const sircc_support_m3_candidates[];',
                "extern const size_t sircc_support_m3_candidates_count;",
                'extern const char* const sircc_support_m3_missing[];',
                "extern const size_t sircc_support_m3_missing_count;",
                "",
                'extern const char* const sircc_support_ir;',
                'extern const char* const sircc_support_spec_source;',
                "",
                "",
            ]
        )
        + "\n",
        encoding="utf-8",
    )

    def emit_str_array(name: str, items: list[str]) -> str:
        # In C11 with -Werror, empty initializer lists / zero-length arrays are
        # rejected, so always emit at least a NULL sentinel and keep count
        # separately as the logical size.
        lines = [f"const char* const {name}[] = {{"]
        if items:
            for it in items:
                lines.append(f'  "{c_escape(it)}",')
        lines.append("  NULL,")
        lines.append("};")
        lines.append(f"const size_t {name}_count = {len(items)};")
        return "\n".join(lines)

    # Source (defs).
    src_lines: list[str] = [
        "// Generated by src/sircc/tools/mnemonic_coverage.py (do not edit).",
        '#include "sircc_support_table.generated.h"',
        "",
        'const char* const sircc_support_ir = "sir-v1.0";',
        'const char* const sircc_support_spec_source = "schema/sir/v1.0/mnemonics.html";',
        "",
        emit_str_array("sircc_support_spec_all", spec_all),
        "",
        emit_str_array("sircc_support_spec_core", spec_core),
        "",
        emit_str_array("sircc_support_impl_in_spec", impl_in_spec),
        "",
        emit_str_array("sircc_support_m3_candidates", sorted(m3)),
        "",
        emit_str_array("sircc_support_m3_missing", m3_missing),
        "",
    ]

    # Emit missing-by-pack arrays + table.
    pack_entries: list[str] = []
    for pack in sorted(missing_by_pack.keys()):
        arr_name = "sircc_support_missing_" + re.sub(r"[^A-Za-z0-9_]+", "_", pack)
        src_lines.append(emit_str_array(arr_name, missing_by_pack[pack]))
        src_lines.append("")
        pack_entries.append(f'  {{"{c_escape(pack)}", {arr_name}, {arr_name}_count}},')

    src_lines.append("const SirccSupportList sircc_support_missing_by_pack[] = {")
    src_lines.extend(pack_entries)
    src_lines.append("};")
    src_lines.append(
        "const size_t sircc_support_missing_by_pack_count = sizeof(sircc_support_missing_by_pack) / sizeof(sircc_support_missing_by_pack[0]);"
    )
    src_lines.append("")

    out_c.write_text("\n".join(src_lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description="sircc mnemonic coverage checker")
    ap.add_argument("--spec", default="schema/sir/v1.0/mnemonics.html", help="path to mnemonics.html")
    ap.add_argument(
        "--compiler",
        default="src/sircc",
        help="path to sircc compiler source file or directory (directory scans compiler*.c, compiler*.h)",
    )
    ap.add_argument("--enforce-m3", action="store_true", help="fail if any Milestone 3 core mnemonic is missing")
    ap.add_argument("--emit-support-c", help="write generated support table C source to this path")
    ap.add_argument("--emit-support-h", help="write generated support table C header to this path")
    args = ap.parse_args()

    spec_path = pathlib.Path(args.spec)
    comp_path = pathlib.Path(args.compiler)
    if not spec_path.exists():
        print(f"mnemonic_coverage: missing spec file: {spec_path}", file=sys.stderr)
        return 2
    if not comp_path.exists():
        print(f"mnemonic_coverage: missing compiler file: {comp_path}", file=sys.stderr)
        return 2

    spec = parse_spec_mnemonics(spec_path.read_text(encoding="utf-8"))

    if comp_path.is_dir():
        parts: list[str] = []
        for p in sorted(comp_path.glob("compiler*.c")) + sorted(comp_path.glob("compiler*.h")):
            if p.is_file():
                parts.append(p.read_text(encoding="utf-8"))
        csrc = "\n".join(parts)
    else:
        csrc = comp_path.read_text(encoding="utf-8")

    impl = infer_implemented_mnemonics(csrc)

    if args.emit_support_c or args.emit_support_h:
        if not args.emit_support_c or not args.emit_support_h:
            print("mnemonic_coverage: --emit-support-c and --emit-support-h must be provided together", file=sys.stderr)
            return 2
        out_c = pathlib.Path(args.emit_support_c)
        out_h = pathlib.Path(args.emit_support_h)
        emit_support_table(spec, impl, out_c=out_c, out_h=out_h)
        return 0

    m3 = pick_m3_core_candidates(spec)
    missing = sorted(m3 - impl)

    # Also catch obvious typos: implemented mnemonics that are not in spec at all.
    spec_all = {m.name for m in spec}
    impl_like = {m for m in impl if re.match(r"^[A-Za-z][A-Za-z0-9_.]*$", m)}
    unknown = sorted([m for m in impl_like if m not in spec_all and m.startswith(("i", "f", "bool.", "ptr.", "const.", "load.", "store.", "alloca", "mem.", "eff.", "call", "term."))])

    print(f"spec mnemonics: {len(spec_all)}  (core pack: {sum(1 for m in spec if m.pack is None)})")
    print(f"milestone3 core candidates: {len(m3)}")
    print(f"inferred implemented: {len(impl)}")
    print(f"missing milestone3: {len(missing)}")
    if missing:
        for m in missing:
            print(f"  MISSING {m}")
    print(f"implemented-not-in-spec (heuristic): {len(unknown)}")
    if unknown:
        for m in unknown[:40]:
            print(f"  UNKNOWN {m}")
        if len(unknown) > 40:
            print(f"  ... ({len(unknown) - 40} more)")

    if args.enforce_m3 and missing:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
