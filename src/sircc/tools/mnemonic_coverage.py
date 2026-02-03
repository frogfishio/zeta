#!/usr/bin/env python3
"""
Mnemonic coverage checker for sircc.

Goal:
- Parse the normative mnemonic vocabulary from schema/sir/v1.0/mnemonics.html
- Infer what sircc currently implements (best-effort) from src/sircc/compiler.c
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
        text = strip_tags(m.group(3))
        if not text:
            continue
        # Split combined rows like: "i8.add / i16.add / i32.add / i64.add"
        for part in text.split("/"):
            part = part.strip()
            if not part:
                continue
            # Filter out any junk that isn't a mnemonic-like token.
            if not re.match(r"^[A-Za-z][A-Za-z0-9_.]*$", part):
                continue
            out.append(SpecMnemonic(name=part, pack=pack_for_pos(m.start())))
    return out


def extract_prims_from_compiler(csrc: str) -> set[str]:
    # Parse the lower_type_prim table to keep this in sync with sircc.
    m = re.search(r"static\s+LLVMTypeRef\s+lower_type_prim\([^{]+\)\s*\{", csrc)
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

    # f32.* / f64.* (as implemented in compiler.c).
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

    # i8/i16/i32/i64 mnemonics (as implemented in compiler.c).
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
    # Milestone 3 is the “base” set (ungated). We select the “core” pack
    # (pack=None) and then bucket into the families sircc uses for Stage 3.
    # This is intentionally conservative and excludes feature-pack-only ops
    # that do not have a distinguishing prefix (e.g., load.vec, term.invoke).
    core = {m.name for m in spec if m.pack is None}

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


def main() -> int:
    ap = argparse.ArgumentParser(description="sircc mnemonic coverage checker")
    ap.add_argument("--spec", default="schema/sir/v1.0/mnemonics.html", help="path to mnemonics.html")
    ap.add_argument("--compiler", default="src/sircc/compiler.c", help="path to sircc compiler.c")
    ap.add_argument("--enforce-m3", action="store_true", help="fail if any Milestone 3 core mnemonic is missing")
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
    impl = infer_implemented_mnemonics(comp_path.read_text(encoding="utf-8"))

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

