#!/usr/bin/env python3

import csv
import sys
from pathlib import Path

sys.setrecursionlimit(20000)

# repo root
BASE = Path(__file__).resolve().parents[2]
RESULTS = BASE / "results"
SELECTED = RESULTS / "selected_100.csv"
OUTDIR = BASE / "nat_inputs"

ARITH_HEADS = {"+", "-", "*", "div", "mod", "abs", "<", "<=", ">", ">=", "="}
BOOL_HEADS = {"and", "or", "not", "=>", "xor", "ite"}
QUANT_HEADS = {"forall", "exists"}


def tokenize(text):
    toks = []
    i = 0
    n = len(text)

    while i < n:
        c = text[i]

        if c.isspace():
            i += 1
            continue

        if c == ";":
            while i < n and text[i] != "\n":
                i += 1
            continue

        if c in "()":
            toks.append(c)
            i += 1
            continue

        if c == "|":
            j = i + 1
            while j < n:
                if text[j] == "\\" and j + 1 < n:
                    j += 2
                elif text[j] == "|":
                    j += 1
                    break
                else:
                    j += 1
            toks.append(text[i:j])
            i = j
            continue

        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\" and j + 1 < n:
                    j += 2
                elif text[j] == '"':
                    j += 1
                    break
                else:
                    j += 1
            toks.append(text[i:j])
            i = j
            continue

        j = i
        while j < n and (not text[j].isspace()) and text[j] not in '();"|':
            j += 1
        toks.append(text[i:j])
        i = j

    return toks


def parse_many(tokens):
    def parse_one(i):
        if tokens[i] == "(":
            out = []
            i += 1
            while tokens[i] != ")":
                node, i = parse_one(i)
                out.append(node)
            return out, i + 1
        return tokens[i], i + 1

    exprs = []
    i = 0
    while i < len(tokens):
        node, i = parse_one(i)
        exprs.append(node)
    return exprs


def to_str(x):
    if isinstance(x, list):
        return "(" + " ".join(to_str(y) for y in x) + ")"
    return x


def is_sym(x):
    return isinstance(x, str)


def is_list(x):
    return isinstance(x, list)


def is_set_logic(expr):
    return is_list(expr) and len(expr) >= 2 and expr[0] == "set-logic"


def is_int_const_decl(expr):
    return (
        is_list(expr)
        and len(expr) == 4
        and expr[0] == "declare-fun"
        and is_list(expr[2])
        and len(expr[2]) == 0
        and expr[3] == "Int"
    )


def is_nonneg_guard(expr, v):
    return (
        is_list(expr)
        and len(expr) == 3
        and (
            (expr[0] == ">=" and expr[1] == v and expr[2] == "0")
            or
            (expr[0] == "<=" and expr[1] == "0" and expr[2] == v)
        )
    )


def contains_nonneg_guard(expr, v):
    if is_nonneg_guard(expr, v):
        return True
    if is_list(expr):
        return any(contains_nonneg_guard(ch, v) for ch in expr)
    return False


def collect_declared_int_constants(exprs):
    out = set()
    for e in exprs:
        if is_int_const_decl(e):
            out.add(e[1])
    return out


def collect_natlike_constants(exprs, int_consts):
    nat = set()

    def visit(e):
        if is_list(e):
            if len(e) == 3 and e[0] in (">=", "<="):
                a, b = e[1], e[2]
                if e[0] == ">=" and a in int_consts and b == "0":
                    nat.add(a)
                if e[0] == "<=" and a == "0" and b in int_consts:
                    nat.add(b)
            for ch in e:
                visit(ch)

    for e in exprs:
        visit(e)
    return nat


def collect_quant_natvars(expr):
    qmap = {}

    def visit(e):
        if is_list(e) and len(e) >= 3 and is_sym(e[0]) and e[0] in QUANT_HEADS and is_list(e[1]):
            nat_here = set()
            for b in e[1]:
                if is_list(b) and len(b) == 2 and b[1] == "Int":
                    if contains_nonneg_guard(e[2], b[0]):
                        nat_here.add(b[0])
            qmap[id(e)] = nat_here
            visit(e[2])
        elif is_list(e):
            for ch in e:
                visit(ch)

    visit(expr)
    return qmap


def wrap_to_int(term):
    return ["to_int$", term]


def in_nat_scope(name, nat_scope_stack):
    for s in reversed(nat_scope_stack):
        if name in s:
            return True
    return False


def is_nat_var_symbol(sym, global_nat_consts, nat_scope_stack):
    return sym in global_nat_consts or in_nat_scope(sym, nat_scope_stack)


def rewrite_term_in_int_context(term, global_nat_consts, qmap, nat_scope_stack):
    if is_sym(term):
        if is_nat_var_symbol(term, global_nat_consts, nat_scope_stack):
            return wrap_to_int(term)
        return term

    if not is_list(term) or len(term) == 0:
        return term

    head = term[0]

    # Do not wrap again if already coerced
    if is_sym(head) and head == "to_int$":
        return term

    if not is_sym(head):
        return [
            rewrite_term_in_int_context(ch, global_nat_consts, qmap, nat_scope_stack)
            for ch in term
        ]

    if head in QUANT_HEADS and len(term) >= 3 and is_list(term[1]):
        nat_here = qmap.get(id(term), set())
        new_binders = []
        for b in term[1]:
            if is_list(b) and len(b) == 2 and b[1] == "Int" and b[0] in nat_here:
                new_binders.append([b[0], "Nat$"])
            else:
                new_binders.append(b)

        nat_scope_stack.append(nat_here)
        new_body = rewrite_expr(term[2], global_nat_consts, qmap, nat_scope_stack)
        nat_scope_stack.pop()
        return [head, new_binders, new_body]

    return [head] + [
        rewrite_term_in_int_context(ch, global_nat_consts, qmap, nat_scope_stack)
        for ch in term[1:]
    ]

def rewrite_expr(expr, global_nat_consts, qmap, nat_scope_stack):
    if is_sym(expr):
        return expr

    if not is_list(expr) or len(expr) == 0:
        return expr

    head = expr[0]

    if not is_sym(head):
        return [rewrite_expr(ch, global_nat_consts, qmap, nat_scope_stack) for ch in expr]

    if head == "set-logic":
        return ["set-logic", "ALL"]

    if head == "declare-fun" and len(expr) == 4:
        name, args, ret = expr[1], expr[2], expr[3]
        if name == "to_int$":
            return expr
        if name in global_nat_consts and is_list(args) and len(args) == 0 and ret == "Int":
            return ["declare-fun", name, [], "Nat$"]
        return expr

    if head in QUANT_HEADS and len(expr) >= 3 and is_list(expr[1]):
        nat_here = qmap.get(id(expr), set())
        new_binders = []
        for b in expr[1]:
            if is_list(b) and len(b) == 2 and b[1] == "Int" and b[0] in nat_here:
                new_binders.append([b[0], "Nat$"])
            else:
                new_binders.append(b)

        nat_scope_stack.append(nat_here)
        new_body = rewrite_expr(expr[2], global_nat_consts, qmap, nat_scope_stack)
        nat_scope_stack.pop()
        return [head, new_binders, new_body]

    if head == "assert" and len(expr) == 2:
        return ["assert", rewrite_expr(expr[1], global_nat_consts, qmap, nat_scope_stack)]

    if head in ARITH_HEADS:
        return [head] + [
            rewrite_term_in_int_context(ch, global_nat_consts, qmap, nat_scope_stack)
            for ch in expr[1:]
        ]

    if head in BOOL_HEADS:
        return [head] + [
            rewrite_expr(ch, global_nat_consts, qmap, nat_scope_stack)
            for ch in expr[1:]
        ]

    new_args = []
    for ch in expr[1:]:
        r = rewrite_expr(ch, global_nat_consts, qmap, nat_scope_stack)

        if is_sym(r):
            if is_nat_var_symbol(r, global_nat_consts, nat_scope_stack):
                r = wrap_to_int(r)
        elif is_list(r):
            # If already to_int$, leave it alone
            if not (len(r) > 0 and is_sym(r[0]) and r[0] == "to_int$"):
                r = rewrite_term_in_int_context(r, global_nat_consts, qmap, nat_scope_stack)

        new_args.append(r)

    return [head] + new_args

def add_header(exprs):
    out = []
    saw_logic = False

    for e in exprs:
        if is_set_logic(e):
            if not saw_logic:
                out.append(["set-logic", "ALL"])
                saw_logic = True
            continue
        out.append(e)

    if not saw_logic:
        out.insert(0, ["set-logic", "ALL"])

    has_nat_sort = any(is_list(e) and e == ["declare-sort", "Nat$", "0"] for e in out)
    has_to_int = any(
        is_list(e)
        and len(e) == 4
        and e[0] == "declare-fun"
        and e[1] == "to_int$"
        for e in out
    )

    final = []
    inserted = False

    for e in out:
        final.append(e)
        if not inserted and is_set_logic(e):
            if not has_nat_sort:
                final.append(["declare-sort", "Nat$", "0"])
            if not has_to_int:
                final.append(["declare-fun", "to_int$", ["Nat$"], "Int"])
            inserted = True

    if not inserted:
        if not has_nat_sort:
            final.append(["declare-sort", "Nat$", "0"])
        if not has_to_int:
            final.append(["declare-fun", "to_int$", ["Nat$"], "Int"])

    return final


def load_selected_paths(csv_path):
    with csv_path.open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None:
            raise ValueError("selected_100.csv has no header")

        lowered = [c.lower() for c in reader.fieldnames]
        if "file" in lowered:
            col = reader.fieldnames[lowered.index("file")]
        elif "path" in lowered:
            col = reader.fieldnames[lowered.index("path")]
        else:
            raise ValueError(f"Need a 'file' or 'path' column, got {reader.fieldnames}")

        return [row[col].strip() for row in reader if row[col].strip()]


def resolve_path(p):
    p = Path(p)

    if p.is_absolute() and p.exists():
        return p

    q = BASE / p
    if q.exists():
        return q

    s = str(p)
    if s.startswith("non-incremental/"):
        q = BASE / s[len("non-incremental/"):]
        if q.exists():
            return q

    raise FileNotFoundError(str(p))


def rewrite_file(inpath, outpath):
    text = inpath.read_text(errors="ignore")
    exprs = parse_many(tokenize(text))

    int_consts = collect_declared_int_constants(exprs)
    nat_consts = collect_natlike_constants(exprs, int_consts)

    qmap = {}
    for e in exprs:
        qmap.update(collect_quant_natvars(e))

    rewritten = [rewrite_expr(e, nat_consts, qmap, []) for e in exprs]
    rewritten = add_header(rewritten)

    outpath.parent.mkdir(parents=True, exist_ok=True)
    outpath.write_text("\n".join(to_str(e) for e in rewritten) + "\n")


def main():
    OUTDIR.mkdir(parents=True, exist_ok=True)
    selected_paths = load_selected_paths(SELECTED)

    ok = 0
    bad = 0

    for p in selected_paths:
        try:
            inpath = resolve_path(p)
            try:
                rel = inpath.relative_to(BASE)
            except ValueError:
                rel = Path(inpath.name)
            outpath = OUTDIR / rel

            rewrite_file(inpath, outpath)
            ok += 1
            print(f"[OK]   {rel}")
        except Exception as e:
            bad += 1
            print(f"[FAIL] {p}: {e}")

    print(f"\nDone. OK={ok}, FAIL={bad}")
    print(f"Output dir: {OUTDIR}")


if __name__ == "__main__":
    main()
