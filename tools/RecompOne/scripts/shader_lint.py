#!/usr/bin/env python3
"""Lint the runtime's GLSL for what one driver forgives and another does not.

The shaders are C# raw string literals (GlShaders.cs, and the snippets spliced
into them with .Replace("//@name", X.Glsl)). This pulls each one out as the
runtime would compile it and checks what one driver forgives and another does
not. Each rule has its own reviewed-exception marker, a comment on the line:
`// <rule>-ok: <why>`.

1. derivatives: dFdx/dFdy/fwidth, and the texture calls that take an implicit
   level of detail, are differences across a 2x2 quad; once a pixel of the quad
   has left by `discard` or `return` they are undefined. At a polygon's edge
   part of the quad is helper pixels, whose interpolants run past the polygon,
   so a discard there is ordinary. Mesa answered anyway; a Windows driver gave
   every edge pixel a wrong normal and the occlusion pass drew a dark line along
   every edge (RENDERING.md, "A dark line along every edge on Windows"). Rule:
   in any function, no derivative (directly, or through a function that takes
   one) textually after a `discard`, a `return`, or a call to a function that
   can discard. Take them at the top of main.

2. domain: pow, sqrt, inversesqrt, log, log2, acos, asin, atan(y, x) and
   smoothstep outside the range the spec defines them on. Vendors answer 0, NaN
   or the nearest finite value, and a NaN then clamps differently again. The
   argument must be visibly in range from the text (max/clamp/abs/a literal).

3. int: `%` on a signed int (undefined if either operand is negative) and an
   integer division by a non-literal (undefined at 0; NVIDIA gives -1).

4. uninit: a local declared without a value whose first mention is a read.
   NVIDIA tends to hand back 0, AMD and Intel whatever the register held.
   Branches are not followed.

5. output: a fragment output not written before a `return` from main or its
   end, which leaves that attachment's pixel undefined.

6. Programs: every vertex/fragment pair a GlShaders.Build* call links, read
   from the runtime's C#. A fragment input no vertex output writes, a type or
   interpolation qualifier that differs between the stages, or a uniform
   declared with two types: link errors on a strict driver, accepted by a
   lenient one.

7. glslangValidator, when installed (--glslang): every stage compiled, and
   every program linked (-l), by the reference front end, which is strict
   where a vendor's compiler is lenient.

Exit status 1 on any finding.

    python3 scripts/shader_lint.py [--glslang] [--dump DIR]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "RecompOne.Runtime", "Gpu")
SOURCES = [
    "Backends/Common/GlShaders.cs",
    "DistanceFade.cs",
    "LinearDepthCue.cs",
    "NeighbourBlend.cs",
]

DERIV = re.compile(r"\b(dFdx|dFdy|fwidth|dFdxFine|dFdyFine|dFdxCoarse|dFdyCoarse)\s*\(")
# An implicit-LOD lookup takes derivatives too, but on a texture with one level
# they only choose between the min and mag filters, so only the samplers that
# carry a mip chain count. Keep this in step with the runtime's GenerateMipmap /
# *Mipmap* filters: the texel atlas (GlTexCache), replacement textures (0073),
# the reflection mip chains and the retained cubemap.
MIPMAPPED = {"uMip", "uAtlas", "uRepTex", "uColorMip", "uPlanarMip", "uCube"}
TEX = re.compile(r"\b(texture|texture2D|textureProj|textureOffset|texture2DProj|textureCube)\s*\(\s*(\w+)")
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
OK = "derivatives-ok:"

# --- extraction ------------------------------------------------------------

LITERAL = re.compile(
    r'(?:const|static\s+readonly)\s+string\s+(\w+)\s*=\s*"""\n(.*?)\n([ \t]*)"""((?:\s*\.Replace\(\s*"[^"]*"\s*,\s*[\w.]+\s*\))*)\s*;',
    re.S)
REPLACE = re.compile(r'\.Replace\(\s*"([^"]*)"\s*,\s*([\w.]+)\s*\)')


def raw_literal(body, indent):
    out = []
    for line in body.split("\n"):
        out.append(line[len(indent):] if line.startswith(indent) else line.lstrip())
    return "\n".join(out)


def extract():
    lits, chains = {}, {}
    for rel in SOURCES:
        text = open(os.path.join(ROOT, rel)).read()
        owner = os.path.splitext(os.path.basename(rel))[0]
        for m in LITERAL.finditer(text):
            name = m.group(1)
            key = name if owner == "GlShaders" else f"{owner}.{name}"
            lits[key] = (raw_literal(m.group(2), m.group(3)), rel, text[:m.start()].count("\n") + 2)
            chains[key] = REPLACE.findall(m.group(4) or "")
    shaders = {}
    for key, (src, rel, line) in lits.items():
        for marker, ref in chains[key]:
            ref = ref.removeprefix("GlShaders.")
            if ref not in lits:
                sys.exit(f"shader_lint: {key} splices {ref}, which was not found")
            src = src.replace(marker, lits[ref][0])
        shaders[key] = (src, rel, line)
    return shaders


def stage(name, src):
    if "#version" not in src or "void main" not in src:
        return None
    if re.search(r"Fs(120)?$", name):
        return "frag"
    if re.search(r"Vs(120)?$", name):
        return "vert"
    return None

# --- the derivative rule ---------------------------------------------------


def strip_comments(src, tag=OK):
    """Blank comments out, keeping offsets and lines; note lines with the marker."""
    ok_lines = set()
    out = list(src)
    for m in re.finditer(r"//[^\n]*|/\*.*?\*/", src, re.S):
        if tag in m.group(0):
            ok_lines.add(src.count("\n", 0, m.start()))
        for i in range(m.start(), m.end()):
            if out[i] != "\n":
                out[i] = " "
    return "".join(out), ok_lines


FUNC = re.compile(r"^[ \t]*(?:[A-Za-z_]\w*\s+)+?([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{", re.M)


def functions(code):
    """name -> (body start, body end) for top-level functions."""
    out = {}
    for m in FUNC.finditer(code):
        name = m.group(1)
        if name in ("if", "for", "while", "switch"):
            continue
        depth, i = 1, m.end()
        while depth and i < len(code):
            depth += {"{": 1, "}": -1}.get(code[i], 0)
            i += 1
        out[name] = (m.end(), i - 1)
    return out


def mip_lookup(text):
    return any(m.group(2) in MIPMAPPED for m in TEX.finditer(text))


def check(name, src):
    code, ok_lines = strip_comments(src)
    funcs = functions(code)
    # Which functions take a derivative, and which can discard, transitively.
    uses, discards = set(), set()
    changed = True
    while changed:
        changed = False
        for f, (a, b) in funcs.items():
            body = code[a:b]
            calls = {c for c in CALL.findall(body) if c in funcs and c != f}
            if f not in uses and (DERIV.search(body) or mip_lookup(body) or calls & uses):
                uses.add(f)
                changed = True
            if f not in discards and (re.search(r"\bdiscard\b", body) or calls & discards):
                discards.add(f)
                changed = True
    findings = []
    for f, (a, b) in funcs.items():
        body = code[a:b]
        leave = None
        for m in re.finditer(r"\b(discard|return)\b|\b([A-Za-z_]\w*)\s*\(", body):
            what = m.group(1) or m.group(2)
            line = code.count("\n", 0, a + m.start())
            if m.group(1):
                if leave is None:
                    leave = (what, line)
                continue
            deriv = (DERIV.fullmatch(what + "(") is not None or what in uses and what != f
                     or mip_lookup(body[m.start():m.start() + 200].split(";")[0]) and TEX.match(body, m.start()))
            if what in discards and leave is None:
                leave = (f"{what}() (can discard)", line)
            if deriv and leave and line not in ok_lines:
                findings.append(
                    f"{name}: line {line + 1} of the shader: {what}() in {f}() after "
                    f"{leave[0]} at line {leave[1] + 1}: undefined in non-uniform control flow; "
                    f"take it at the top of main (or mark `// {OK} <why>`)\n"
                    f"    {src.splitlines()[line].strip()}")
    return findings

# --- helpers shared by the rules below ---------------------------------------

TYPES = r"(?:float|int|uint|bool|[iub]?vec[234]|mat[234](?:x[234])?)"


def split_args(text, start):
    """Arguments of the call whose '(' is at text[start]: [(arg, offset)], end."""
    args, depth, a = [], 0, start + 1
    for i in range(start, len(text)):
        c = text[i]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
            if depth == 0:
                args.append((text[a:i].strip(), a))
                return args, i
        elif c == "," and depth == 1:
            args.append((text[a:i].strip(), a))
            a = i + 1
    return args, len(text)


def literal(e):
    m = re.fullmatch(r"\(?\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)[fF]?\s*\)?", e)
    if m:
        return float(m.group(1))
    m = re.fullmatch(r"[iu]?vec[234]\(\s*([-+]?(?:\d+\.?\d*|\.\d+))\s*\)", e)
    return float(m.group(1)) if m else None


def at_least(e, bound, strict):
    """Can e be seen, from its text alone, to be >= bound (> bound if strict)?"""
    e = e.strip()
    while e.startswith("(") and split_args(e, 0)[1] == len(e) - 1:
        e = e[1:-1].strip()
    v = literal(e)
    if v is not None:
        return v > bound if strict else v >= bound
    m = re.match(r"(max|clamp|abs|exp|exp2)\s*\(", e)
    if not m or split_args(e, m.end() - 1)[1] != len(e) - 1:
        return False
    args = [a for a, _ in split_args(e, m.end() - 1)[0]]
    if m.group(1) == "max":
        return any(at_least(a, bound, strict) for a in args)
    if m.group(1) == "clamp":
        return len(args) == 3 and at_least(args[1], bound, strict)
    if m.group(1) == "abs":
        return bound < 0 or (bound == 0 and not strict)
    return bound <= 0  # exp, exp2: > 0


def findings_at(name, src, ok_lines, code, pos, rule, msg):
    line = code.count("\n", 0, pos)
    if line in ok_lines:
        return []
    return [f"{name}: line {line + 1} of the shader: {msg} (or mark `// {rule}-ok: <why>`)\n"
            f"    {src.splitlines()[line].strip()}"]

# --- builtins outside their domain -------------------------------------------
#
# The spec leaves these undefined, and vendors differ: one returns 0, another
# NaN, another the nearest finite value, and a NaN then compares and clamps
# differently again. Each needs its argument visibly in range from the text
# (max/clamp/abs/a literal), or a reviewed `domain-ok:`.


def check_domain(name, src):
    code, ok = strip_comments(src, "domain-ok:")
    out = []
    for m in re.finditer(r"\b(pow|sqrt|inversesqrt|log|log2|acos|asin|smoothstep|atan)\s*\(", code):
        f = m.group(1)
        args = [a for a, _ in split_args(code, m.end() - 1)[0]]
        bad = None
        if f == "pow" and len(args) == 2:
            if not at_least(args[0], 0.0, False):
                bad = "pow(x, y) is undefined for x < 0"
            elif not at_least(args[0], 0.0, True) and not at_least(args[1], 0.0, True):
                bad = "pow(x, y) is undefined for x == 0 and y <= 0"
        elif f == "sqrt" and not at_least(args[0], 0.0, False):
            bad = "sqrt(x) is undefined for x < 0"
        elif f in ("inversesqrt", "log", "log2") and not at_least(args[0], 0.0, True):
            bad = f"{f}(x) is undefined for x <= 0"
        elif f in ("acos", "asin") and not re.fullmatch(r"clamp\(.*,\s*-1\.0?\s*,\s*1\.0?\s*\)", args[0]):
            bad = f"{f}(x) is undefined for |x| > 1"
        elif f == "atan" and len(args) == 2:
            bad = "atan(y, x) is undefined for x == 0 and y == 0"
        elif f == "smoothstep" and len(args) == 3:
            e0, e1 = literal(args[0]), literal(args[1])
            same = re.sub(r"\s+", "", args[0]), re.sub(r"\s+", "", args[1])
            if e0 is not None and e1 is not None:
                bad = None if e0 < e1 else "smoothstep(e0, e1, x) with e0 >= e1 is undefined; use 1.0 - smoothstep(e1, e0, x)"
            elif not (re.fullmatch(re.escape(same[1]) + r"-[\d.]+", same[0])
                      or re.fullmatch(re.escape(same[0]) + r"\+[\d.]+", same[1])):
                bad = "smoothstep(e0, e1, x) is undefined for e0 >= e1"
        if bad:
            out += findings_at(name, src, ok, code, m.start(), "domain", bad)
    return out

# --- signed integer % and integer division -----------------------------------
#
# `a % b` is undefined when either is negative, and an integer division by zero
# is undefined (NVIDIA gives -1, others 0 or the dividend). Both need a
# `int-ok:` saying why the operands cannot be.


def int_typed(code, e, scope=None):
    """Is expression e, by its text, a signed int? (None when it cannot tell)
    A name is looked up in scope (the enclosing function, parameters included)
    first, then in the whole shader."""
    e = e.strip()
    while e.startswith("(") and split_args(e, 0)[1] == len(e) - 1:
        e = e[1:-1].strip()
    if re.fullmatch(r"\d+u|0x[\da-fA-F]+u|uint\(.*|uvec[234]\(.*", e):
        return False
    if re.fullmatch(r"-?\d+|0x[\da-fA-F]+|int\(.*|ivec[234]\(.*", e):
        return True
    m = re.match(r"([A-Za-z_]\w*)", e)
    if not m:
        return None
    decl = r"\b(" + TYPES + r")\s+(?:\w+\s*(?:=[^,;]*)?,\s*)*" + m.group(1) + r"\b"
    d = scope and re.search(decl, scope) or re.search(decl, code)
    if not d:
        return None
    return d.group(1) in ("int", "ivec2", "ivec3", "ivec4")


def operand(code, i, step):
    """The operand of a binary operator at i: the text to its left (step -1) or right."""
    depth, j = 0, i
    stop = set(",;=?:&|<>!+-*/%") if step < 0 else set(",;=?:&|<>!+-*/%")
    while 0 <= j + step < len(code):
        j += step
        c = code[j]
        if c in "([":
            depth += 1 if step > 0 else -1
        elif c in ")]":
            depth += -1 if step > 0 else 1
        if depth < 0 or (depth == 0 and c in stop) or (depth == 0 and c in "{}"):
            j -= step
            break
    a, b = (j, i) if step < 0 else (i + 1, j + 1)
    return code[a:b].strip()


def check_ints(name, src):
    code, ok = strip_comments(src, "int-ok:")
    spans = [(m.start(), functions(code)[m.group(1)][1]) for m in FUNC.finditer(code) if m.group(1) in functions(code)]
    out = []
    for m in re.finditer(r"(?<![/*])([%/])(?![=/*])", code):
        lhs, rhs = operand(code, m.start(), -1), operand(code, m.start(), 1)
        if not lhs or not rhs:
            continue
        scope = next((code[a:b] for a, b in spans if a <= m.start() < b), None)
        int_typed = lambda c, e, _s=scope: globals()["int_typed"](c, e, _s)
        if m.group(1) == "%":
            if int_typed(code, lhs) or int_typed(code, rhs):
                out += findings_at(name, src, ok, code, m.start(), "int",
                                   f"`{lhs} % {rhs}` on a signed int is undefined if either is negative; use uint or mod()")
        elif int_typed(code, rhs) and literal(rhs) is None and not re.fullmatch(r"\d+|0x[\da-fA-F]+", rhs) \
                and int_typed(code, lhs) is not False:
            out += findings_at(name, src, ok, code, m.start(), "int",
                               f"integer division by `{rhs}` is undefined when it is 0")
    return out

# --- locals read before they are written --------------------------------------
#
# An uninitialised local is whatever the register held: NVIDIA tends to give 0,
# AMD and Intel give garbage. This finds a local declared without a value whose
# first mention is a read, not an assignment or an `out` argument. It does not
# follow branches, so a local written in only one arm of an `if` is not seen.


def check_uninit(name, src):
    code, ok = strip_comments(src, "uninit-ok:")
    funcs = functions(code)
    params = {}
    for m in FUNC.finditer(code):
        params[m.group(1)] = [p.strip() for p in m.group(2).split(",")]
    out = []
    for f, (a, b) in funcs.items():
        body = code[a:b]
        for d in re.finditer(r"(?:^|[;{}])\s*(" + TYPES + r")\s+([^;=(){}]*(?:=[^;]*)?);", body):
            for part in [p for p, _ in split_args("(" + d.group(2) + ")", 0)[0]]:
                v = re.fullmatch(r"([A-Za-z_]\w*)\s*(\[[^\]]*\])?", part)
                if not v:
                    continue
                var = v.group(1)
                use = re.search(r"\b" + var + r"\b", body[d.end():])
                if not use:
                    continue
                at = d.end() + use.start()
                after = body[at + len(var):]
                if re.match(r"\s*(\[[^\]]*\]|\.[xyzwrgba]+)?\s*=(?!=)", after):
                    continue
                # an out argument?
                depth, j, commas = 0, at - 1, 0
                while j >= 0:
                    c = body[j]
                    if c == ")":
                        depth += 1
                    elif c == "(":
                        if depth == 0:
                            break
                        depth -= 1
                    elif c == "," and depth == 0:
                        commas += 1
                    elif c in ";{}" and depth == 0:
                        j = -1
                        break
                    j -= 1
                if j >= 0:
                    callee = re.search(r"([A-Za-z_]\w*)\s*$", body[:j])
                    ps = params.get(callee.group(1)) if callee else None
                    if ps and commas < len(ps) and re.match(r"(?:\w+\s+)*out\b", ps[commas]):
                        continue
                out += findings_at(name, src, ok, code, a + at, "uninit",
                                   f"`{var}` in {f}() is read before it is written")
    return out

# --- fragment outputs left unwritten -------------------------------------------
#
# A `return` from main before an output is written leaves that pixel's value
# undefined in that attachment; drivers differ on what lands there.


def check_outputs(name, src):
    code, ok = strip_comments(src, "output-ok:")
    outs = re.findall(r"^\s*(?:layout\s*\([^)]*\)\s*)?out\s+" + TYPES + r"\s+(\w+)\s*;", code, re.M)
    funcs = functions(code)
    if not outs or "main" not in funcs:
        return []
    writes = {}
    for f, (a, b) in funcs.items():
        writes[f] = {o for o in outs if re.search(r"\b" + o + r"\b\s*(\.\w+|\[[^\]]*\])?\s*=(?!=)", code[a:b])}
    changed = True
    while changed:
        changed = False
        for f, (a, b) in funcs.items():
            for c in CALL.findall(code[a:b]):
                if c in writes and c != f and not writes[c] <= writes[f]:
                    writes[f] |= writes[c]
                    changed = True
    a, b = funcs["main"]
    body = code[a:b]
    findings = []
    exits = [m.start() for m in re.finditer(r"\breturn\b", body)] + [len(body)]
    for e in exits:
        seen = {o for o in outs if re.search(r"\b" + o + r"\b\s*(\.\w+|\[[^\]]*\])?\s*=(?!=)", body[:e])}
        for c in CALL.findall(body[:e]):
            seen |= writes.get(c, set())
        missing = [o for o in outs if o not in seen]
        if missing:
            where = "the end of main" if e == len(body) else "this return"
            findings += findings_at(name, src, ok, code, a + min(e, len(body) - 1), "output",
                                    f"{', '.join(missing)} not written before {where}: undefined in that attachment")
    return findings

# --- programs: the stages the runtime links together ---------------------------

BUILD = re.compile(r"GlShaders\.Build(?:Prim|Fullscreen)?\(\s*\w+\s*,\s*([\w.]+)\s*,\s*([\w.]+)")


def programs(shaders):
    """(vs, fs) pairs from every GlShaders.Build* call under the runtime's Gpu tree."""
    pairs = set()
    for dirpath, _, files in os.walk(ROOT):
        for fn in files:
            if not fn.endswith(".cs"):
                continue
            text = open(os.path.join(dirpath, fn)).read()

            def resolve(n):
                n = n.removeprefix("GlShaders.")
                if n in shaders:
                    return [n]
                m = re.search(r"\bstring\s+" + n + r"\s*=\s*[^;?]*\?\s*(?:GlShaders\.)?(\w+)\s*:\s*(?:GlShaders\.)?(\w+)\s*;", text)
                return [x for x in m.groups() if x in shaders] if m else []

            for m in BUILD.finditer(text):
                for vs in resolve(m.group(1)):
                    for fs in resolve(m.group(2)):
                        if version(shaders[vs][0]) == version(shaders[fs][0]):
                            pairs.add((vs, fs))
    return sorted(pairs)


def version(src):
    m = re.search(r"#version\s+(\d+)", src)
    return m.group(1) if m else None


IO = re.compile(r"^\s*((?:(?:flat|smooth|noperspective|centroid)\s+)*)(in|out|varying|uniform)\s+(\w+)\s+(\w+)\s*(\[[^\]]*\])?\s*;", re.M)


def interface(src):
    code, _ = strip_comments(src)
    return [(m.group(2), m.group(4), m.group(3) + (m.group(5) or ""), m.group(1).split()) for m in IO.finditer(code)], code


def check_link(vs, vsrc, fs, fsrc):
    """What a strict linker refuses and a lenient one lets through."""
    vio, _ = interface(vsrc)
    fio, fcode = interface(fsrc)
    vouts = {n: (t, q) for k, n, t, q in vio if k in ("out", "varying")}
    vunis = {n: t for k, n, t, q in vio if k == "uniform"}
    out = []
    for k, n, t, q in fio:
        if k in ("in", "varying"):
            used = len(re.findall(r"\b" + n + r"\b", fcode)) > 1
            if n not in vouts:
                if used:
                    out.append(f"{vs}+{fs}: `{n}` is read by the fragment stage and written by no vertex output: a link error on a strict driver")
            elif vouts[n][0] != t:
                out.append(f"{vs}+{fs}: `{n}` is {vouts[n][0]} out of the vertex stage and {t} into the fragment stage")
            elif set(vouts[n][1]) & {"flat", "noperspective"} != set(q) & {"flat", "noperspective"}:
                out.append(f"{vs}+{fs}: `{n}`'s interpolation differs between the stages ({' '.join(vouts[n][1]) or 'smooth'} / "
                           f"{' '.join(q) or 'smooth'}): a link error before GLSL 4.30")
        elif k == "uniform" and n in vunis and vunis[n] != t:
            out.append(f"{vs}+{fs}: uniform `{n}` is {vunis[n]} in the vertex stage and {t} in the fragment stage")
    return out

# --- glslang ---------------------------------------------------------------


def glslang(name, src, kind, tmp):
    path = os.path.join(tmp, f"{name}.{kind}")
    with open(path, "w") as f:
        f.write(src)
    r = subprocess.run(["glslangValidator", path], capture_output=True, text=True)
    if r.returncode == 0:
        return []
    lines = [l for l in (r.stdout + r.stderr).splitlines() if "ERROR" in l or "error" in l]
    return [f"{name}: glslangValidator: {l.replace(path, name)}" for l in lines] or [f"{name}: glslangValidator failed"]


def link(vs, vsrc, fs, fsrc, tmp):
    paths = []
    for n, src, kind in ((vs, vsrc, "vert"), (fs, fsrc, "frag")):
        paths.append(os.path.join(tmp, f"link_{n}.{kind}"))
        with open(paths[-1], "w") as f:
            f.write(src)
    r = subprocess.run(["glslangValidator", "-l"] + paths, capture_output=True, text=True)
    if r.returncode == 0:
        return []
    text = (r.stdout + r.stderr).replace("\n    ", " ")
    lines = [l for l in text.splitlines() if "ERROR" in l]
    return [f"{vs}+{fs}: glslangValidator -l: {l}" for l in lines] or [f"{vs}+{fs}: glslangValidator -l failed"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--glslang", action="store_true", help="also compile every stage with glslangValidator")
    ap.add_argument("--dump", metavar="DIR", help="write each extracted shader to DIR")
    args = ap.parse_args()

    shaders = {n: s for n, s in extract().items() if stage(n, s[0])}
    if not shaders:
        sys.exit("shader_lint: no shaders found")
    if args.dump:
        os.makedirs(args.dump, exist_ok=True)
        for n, (src, _, _) in shaders.items():
            with open(os.path.join(args.dump, f"{n}.{stage(n, src)}"), "w") as f:
                f.write(src)

    findings = []
    for n, (src, rel, line) in shaders.items():
        found = check_domain(n, src) + check_ints(n, src) + check_uninit(n, src)
        if stage(n, src) == "frag":
            found = check(n, src) + found + check_outputs(n, src)
        findings += [f"{rel}:{line}: {x}" for x in found]
    pairs = programs(shaders)
    for vs, fs in pairs:
        findings += [f"{shaders[fs][1]}:{shaders[fs][2]}: {x}" for x in check_link(vs, shaders[vs][0], fs, shaders[fs][0])]
    if args.glslang:
        if not shutil.which("glslangValidator"):
            sys.exit("shader_lint: --glslang needs glslangValidator (glslang-tools)")
        with tempfile.TemporaryDirectory() as tmp:
            for n, (src, rel, line) in shaders.items():
                findings += [f"{rel}:{line}: {x}" for x in glslang(n, src, stage(n, src), tmp)]
            for vs, fs in pairs:
                findings += [f"{shaders[fs][1]}:{shaders[fs][2]}: {x}" for x in link(vs, shaders[vs][0], fs, shaders[fs][0], tmp)]

    for f in findings:
        print(f)
    frags = sum(1 for n, s in shaders.items() if stage(n, s[0]) == "frag")
    print(f"shader_lint: {len(shaders)} stages ({frags} fragment), {len(pairs)} programs, {len(findings)} finding(s)"
          + ("" if args.glslang else "; glslang not run"))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
