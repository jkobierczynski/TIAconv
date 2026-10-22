#!/usr/bin/env python3
# tiaconv - TIA Portal project reader
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Prints the code tree of a --program-json file as SCL-like text, one block
# after the other, to read what a simulator gets. Operands are shown by the
# text of their reference entry, with the ref_id in brackets when -r is given.
# Usage: python3 tools/program_tree.py [-r] PLC.json [block ...]
import json
import sys

OPS = {"add": "+", "sub": "-", "mul": "*", "div": "/", "mod": "MOD", "pow": "**", "and": "AND",
       "or": "OR", "xor": "XOR", "eq": "=", "ne": "<>", "lt": "<", "le": "<=", "gt": ">", "ge": ">="}


def expr(e, refs, show_ids):
    op = e["op"]
    a = [expr(x, refs, show_ids) for x in e["args"]]
    rid = e["ref_id"]
    tag = "[%s]" % rid if show_ids and rid is not None else ""
    if op == "ref":
        return (refs.get(rid) or e["name"] or "?") + tag
    if op == "const":
        return (e["value"]["text"] if e["value"] else "?") + tag
    if op in OPS:
        return "(" + (" %s " % OPS[op]).join(a) + ")"
    if op == "not":
        return "NOT " + a[0]
    if op == "neg":
        return "-" + a[0]
    if op == "member":
        return a[0] + "." + (e["name"] or "?") + tag
    if op == "index":
        return a[0] + "[" + ", ".join(a[1:]) + "]" + tag
    if op == "slice":
        return a[0] + "." + (e["name"] or "?")
    if op == "call":
        callee = refs.get(e["ref_id"]) or e["name"] or "?"
        inst = refs.get(e["instance_ref_id"]) if e["instance_ref_id"] is not None else None
        head = (inst + "/" if inst else "") + callee
        return head + "(" + ", ".join(a) + ")"
    if op in ("in", "inout"):
        return (e["name"] + (" := " if op == "in" else " :=: ") if e["name"] else "") + (a[0] if a else "?")
    if op == "out":
        return e["name"] + " => " + (a[0] if a else "?")
    if op in ("edge_pos", "edge_neg"):
        return ("P" if op == "edge_pos" else "N") + "(" + ", ".join(a) + ")"
    if op == "wire":
        return "<" + e["name"] + ">"
    if op == "range":
        return "..".join(a)
    return "{%s %s}" % (op, e["name"] or "")


def stmts(list_, refs, ind, show_ids, out):
    for s in list_:
        stmt(s, refs, ind, show_ids, out)


def stmt(s, refs, ind, ids, out):
    p = "    " * ind
    op = s["op"]
    E = lambda e: expr(e, refs, ids)
    if op == "assign":
        out.append(p + E(s["exprs"][0]) + " := " + E(s["exprs"][1]) + ";")
    elif op == "call":
        out.append(p + E(s["exprs"][0]) + ";")
    elif op == "if":
        for i, b in enumerate(s["branches"]):
            out.append(p + ("IF " if i == 0 else "ELSIF ") + E(b["exprs"][0]) + " THEN")
            stmts(b["body"], refs, ind + 1, ids, out)
        if s["else"] is not None:
            out.append(p + "ELSE")
            stmts(s["else"], refs, ind + 1, ids, out)
        out.append(p + "END_IF;")
    elif op == "case":
        out.append(p + "CASE " + E(s["exprs"][0]) + " OF")
        for b in s["branches"]:
            out.append(p + "    " + ", ".join(E(x) for x in b["exprs"]) + ":")
            stmts(b["body"], refs, ind + 2, ids, out)
        if s["else"] is not None:
            out.append(p + "    ELSE")
            stmts(s["else"], refs, ind + 2, ids, out)
        out.append(p + "END_CASE;")
    elif op == "for":
        x = [E(e) for e in s["exprs"]]
        out.append(p + "FOR %s := %s TO %s%s DO" % (x[0], x[1], x[2], " BY " + x[3] if len(x) > 3 else ""))
        stmts(s["body"], refs, ind + 1, ids, out)
        out.append(p + "END_FOR;")
    elif op == "while":
        out.append(p + "WHILE " + E(s["exprs"][0]) + " DO")
        stmts(s["body"], refs, ind + 1, ids, out)
        out.append(p + "END_WHILE;")
    elif op == "repeat":
        out.append(p + "REPEAT")
        stmts(s["body"], refs, ind + 1, ids, out)
        out.append(p + "UNTIL " + E(s["exprs"][0]) + " END_REPEAT;")
    elif op == "region":
        out.append(p + "REGION " + (s["name"] or ""))
        stmts(s["body"], refs, ind + 1, ids, out)
        out.append(p + "END_REGION")
    elif op in ("return", "exit", "continue"):
        out.append(p + op.upper() + ";")
    elif op == "empty":
        out.append(p + ";")
    elif op == "eval":
        out.append(p + "(* evaluated for its edges *) " + E(s["exprs"][0]) + ";")
    elif op == "stl":
        out.append(p + s["name"] + " " + ", ".join(E(e) for e in s["exprs"]))
    elif op == "label":
        out.append(p + s["name"] + ":")
    else:
        out.append(p + "{%s %s}" % (op, s["name"] or ""))


def main(argv):
    ids = "-r" in argv
    argv = [a for a in argv if a != "-r"]
    doc = json.load(open(argv[0], encoding="utf-8"))
    wanted = set(argv[1:])
    for b in doc["blocks"]:
        if wanted and b["name"] not in wanted:
            continue
        print("== %s %s (%s)" % (b["type"], b["name"], b["language"]))
        if not b["code"]:
            print("   (no code: %s)" % (b["protection"] or "not read"))
            continue
        refs = {r["ref_id"]: r["text"] for r in b["code"]["references"]}
        for n in b["code"]["networks"]:
            print("-- network %d %s" % (n["number"], n["content"]))
            if n["statements"] is None:
                print("   (no tree)")
                continue
            out = []
            stmts(n["statements"], refs, 1, ids, out)
            print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1:])
