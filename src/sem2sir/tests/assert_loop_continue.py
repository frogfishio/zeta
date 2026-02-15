#!/usr/bin/env python3

import json
import sys
from dataclasses import dataclass


@dataclass(frozen=True)
class TermBr:
    to: str


@dataclass(frozen=True)
class TermCondBr:
    cond: str
    then_to: str
    else_to: str


@dataclass(frozen=True)
class TermRet:
    value: str | None


def die(msg: str) -> None:
    print(f"assert_loop_continue: {msg}", file=sys.stderr)
    raise SystemExit(2)


def load_jsonl(path: str):
    objs = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            objs.append(json.loads(line))
    return objs


def node_ref_id(ref):
    if not isinstance(ref, dict) or ref.get("t") != "ref" or "id" not in ref:
        die(f"expected ref object, got {ref!r}")
    return ref["id"]


def parse_term(nodes_by_id: dict, term_node_id: str):
    n = nodes_by_id.get(term_node_id)
    if not n:
        die(f"missing term node {term_node_id}")

    tag = n.get("tag")
    fields = n.get("fields", {})

    if tag == "term.br":
        to_id = node_ref_id(fields.get("to"))
        return TermBr(to=to_id)

    if tag == "term.condbr":
        cond_id = node_ref_id(fields.get("cond"))
        then_to = node_ref_id(fields.get("then", {}).get("to"))
        else_to = node_ref_id(fields.get("else", {}).get("to"))
        return TermCondBr(cond=cond_id, then_to=then_to, else_to=else_to)

    if tag == "term.ret":
        value_ref = fields.get("value")
        value_id = node_ref_id(value_ref) if value_ref is not None else None
        return TermRet(value=value_id)

    die(f"unsupported terminator tag {tag!r} at node {term_node_id}")


def block_terminator(nodes_by_id: dict, block_node: dict):
    stmts = block_node.get("fields", {}).get("stmts")
    if not isinstance(stmts, list) or not stmts:
        die(f"block has no stmts: {block_node.get('id')}")
    term_id = node_ref_id(stmts[-1])
    return parse_term(nodes_by_id, term_id)


def find_fn(objs):
    for o in objs:
        if o.get("k") == "node" and o.get("tag") == "fn":
            return o
    die("missing fn node")


def build_nodes(objs):
    nodes_by_id = {}
    for o in objs:
        if o.get("k") == "node" and "id" in o:
            nodes_by_id[o["id"]] = o
    return nodes_by_id


def find_blocks(nodes_by_id: dict):
    return {
        node_id: n
        for node_id, n in nodes_by_id.items()
        if n.get("tag") == "block"
    }


def assert_dowhile_continue(nodes_by_id: dict, blocks_by_id: dict, fn_node: dict):
    entry_id = node_ref_id(fn_node.get("fields", {}).get("entry"))
    entry_block = blocks_by_id.get(entry_id)
    if not entry_block:
        die(f"fn entry block not found: {entry_id}")

    entry_term = block_terminator(nodes_by_id, entry_block)
    if not isinstance(entry_term, TermBr):
        die(f"expected entry terminator to be term.br, got {entry_term}")
    body_id = entry_term.to

    body_block = blocks_by_id.get(body_id)
    if not body_block:
        die(f"body block not found: {body_id}")

    cond_candidates = []
    for blk_id, blk in blocks_by_id.items():
        term = block_terminator(nodes_by_id, blk)
        if isinstance(term, TermCondBr) and term.then_to == body_id:
            cond_candidates.append(blk_id)

    if len(cond_candidates) != 1:
        die(f"expected exactly 1 cond-check block targeting body; got {cond_candidates}")
    cond_id = cond_candidates[0]

    body_term = block_terminator(nodes_by_id, body_block)
    if not isinstance(body_term, TermBr):
        die(f"expected body terminator to be term.br, got {body_term}")

    if body_term.to != cond_id:
        die(f"expected body to branch to cond-check block {cond_id}, got {body_term.to}")


def assert_for_like_continue(nodes_by_id: dict, blocks_by_id: dict):
    # Look for a loop header H with condbr, with then_to == body, else_to == exit.
    # And a step block S that branches back to H.
    # Continue semantics are validated by asserting the body terminator branches to S.
    for header_id, header_blk in blocks_by_id.items():
        header_term = block_terminator(nodes_by_id, header_blk)
        if not isinstance(header_term, TermCondBr):
            continue

        body_id = header_term.then_to
        body_blk = blocks_by_id.get(body_id)
        if not body_blk:
            continue

        # Find a step block that unconditionally branches back to the header.
        step_ids = []
        for step_id, step_blk in blocks_by_id.items():
            step_term = block_terminator(nodes_by_id, step_blk)
            if isinstance(step_term, TermBr) and step_term.to == header_id:
                step_ids.append(step_id)

        if not step_ids:
            continue

        body_term = block_terminator(nodes_by_id, body_blk)
        if not isinstance(body_term, TermBr):
            continue

        if body_term.to in step_ids:
            return

    die("did not find for-like loop where body branches to step and step branches to header")


def assert_for_no_step_continue(nodes_by_id: dict, blocks_by_id: dict):
    # Look for a loop header H with condbr, with then_to == body.
    # For no-step form, continue-target is the header, so body terminator must branch to H.
    for header_id, header_blk in blocks_by_id.items():
        header_term = block_terminator(nodes_by_id, header_blk)
        if not isinstance(header_term, TermCondBr):
            continue

        body_id = header_term.then_to
        body_blk = blocks_by_id.get(body_id)
        if not body_blk:
            continue

        body_term = block_terminator(nodes_by_id, body_blk)
        if isinstance(body_term, TermBr) and body_term.to == header_id:
            return

    die("did not find for(no-step) loop where body branches back to header")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        die("usage: assert_loop_continue.py <sir.jsonl> <dowhile|for|forint>")

    path = argv[1]
    kind = argv[2]

    objs = load_jsonl(path)
    nodes_by_id = build_nodes(objs)
    blocks_by_id = find_blocks(nodes_by_id)
    fn_node = find_fn(objs)

    if kind == "dowhile":
        assert_dowhile_continue(nodes_by_id, blocks_by_id, fn_node)
        return 0

    if kind in ("for", "forint"):
        assert_for_like_continue(nodes_by_id, blocks_by_id)
        return 0

    if kind == "for-nostep":
        assert_for_no_step_continue(nodes_by_id, blocks_by_id)
        return 0

    die(f"unknown kind {kind!r}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
