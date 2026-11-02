"""The hook layer's part of tools/raw_layer.py (docs/HOOK_LAYER_PLAN.md): which of the game's functions
have a hook point, how far each may be hooked, and the build's routing of every call through it.

The routing, with no run-time patching: the game's definition of f is renamed kf_game_f in a copy of
its source (the build's hook_src/), and the generated source defines f, which tests one flag and then
runs kf_game_f or the layer. A function's own file reaches it through the header's name too, so no
caller is missed. A name this misreads is a link error: f defined twice, or kf_game_f not at all.

A function has a hook point if its signature crosses as the call layer's does, one definition in the
game's sources takes the new name, and port/ext/hook_layer.toml does not hold it. Its reach:

  Watch             any such function
  ChangeArguments   a function kf.call can call, whose arguments have been read for its ranges
  Replace           and whose return a caller can take from a mod: none, a bool, an enum, a pool's
                    element, or a value the listed file gives a range for
"""

import dataclasses
import os
import re
import tomllib

LAYER_PATH = "kf.hook"
TOP_LEVEL_KEYS = ("instructions_per_fire", "held_headers", "held", "functions")
FUNCTION_KEYS = ("watch_only", "no_replace", "returns_range", "returns_check", "returns_nil", "why")
RENAMED_PREFIX = "kf_game_"
IDENTIFIER = re.compile(r"[A-Za-z_]\w*")
AFTER_PARAMETERS = re.compile(r"\s*(?:noexcept\b\s*)?(\{|;|:|=)")


@dataclasses.dataclass
class Listing:
    instructions_per_fire: int
    held_headers: dict
    held: dict
    functions: dict


@dataclasses.dataclass
class HookEmit:
    name: str
    emit: object            # call_layer.FunctionEmit
    returns: str
    arguments: list         # the game's parameters: (name, type text)
    noexcept: bool
    reach: str
    returned_range: object = None
    returned_check: str = ""
    returned_nil: bool = False


def load(path, config_error):
    try:
        with open(path, "rb") as handle:
            data = tomllib.load(handle)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise config_error(f"{path}: {error}") from error
    unknown = set(data) - set(TOP_LEVEL_KEYS)
    if unknown:
        raise config_error(f"{path}: unknown key(s) {', '.join(sorted(unknown))}")
    for name, entry in data.get("functions", {}).items():
        unknown = set(entry) - set(FUNCTION_KEYS)
        if unknown:
            raise config_error(f"[functions.{name}]: unknown key(s) {', '.join(sorted(unknown))}")
    return Listing(data.get("instructions_per_fire", 0), data.get("held_headers", {}), data.get("held", {}),
                   data.get("functions", {}))


def _blank_comments_and_strings(text):
    """The text with comments, strings and preprocessor lines blanked, offsets kept."""
    out = list(text)
    at, length = 0, len(text)
    line_start = True

    def blank(end):
        for i in range(at, end):
            if out[i] != "\n":
                out[i] = " "

    while at < length:
        char = text[at]
        if text.startswith("//", at):
            end = text.find("\n", at)
            end = length if end < 0 else end
        elif text.startswith("/*", at):
            end = text.find("*/", at + 2)
            end = length if end < 0 else end + 2
        elif char in "\"'":
            end = at + 1
            while end < length and text[end] != char:
                end += 2 if text[end] == "\\" else 1
            end = min(end + 1, length)
        elif char == "#" and line_start:
            end = at
            while end < length and not (text[end] == "\n" and text[end - 1] != "\\"):
                end += 1
        else:
            if char == "\n":
                line_start = True
            elif not char.isspace():
                line_start = False
            at += 1
            continue
        blank(end)
        at = end
    return "".join(out)


def _close_of(text, opening):
    depth = 0
    for at in range(opening, len(text)):
        if text[at] == "(":
            depth += 1
        elif text[at] == ")":
            depth -= 1
            if depth == 0:
                return at
    return -1


def find_definitions(text):
    """Each function defined at file scope, not in a namespace: its name's offsets, by name."""
    code = _blank_comments_and_strings(text)
    found = {}
    depth = 0
    at = 0
    while at < len(code):
        char = code[at]
        if char == "{":
            depth += 1
            at += 1
        elif char == "}":
            depth -= 1
            at += 1
        elif depth == 0 and (match := IDENTIFIER.match(code, at)):
            end = match.end()
            after = end
            while after < len(code) and code[after].isspace():
                after += 1
            if after < len(code) and code[after] == "(":
                close = _close_of(code, after)
                body = AFTER_PARAMETERS.match(code, close + 1) if close > 0 else None
                if body and body.group(1) == "{":
                    found.setdefault(match.group(0), []).append(at)
                at = close + 1 if close > 0 else end
            else:
                at = end
        else:
            at += 1
    return found


def scan_sources(sources):
    """name -> the sources defining it, among the game's. {source: text} too."""
    defined, texts = {}, {}
    for source in sources:
        with open(source, encoding="utf-8", errors="surrogateescape") as handle:
            texts[source] = handle.read()
        for name, places in find_definitions(texts[source]).items():
            defined.setdefault(name, []).extend([source] * len(places))
    return defined, texts


def _return_crosses_back(text, classifier):
    """Whether any value a mod gives of this type is one a caller can take with no range listed."""
    t = text.strip()
    if t == "void":
        return True
    if t.endswith(("*", "&")):
        return classifier._is_pool(t[:-1].strip())
    ty = classifier.layer.parse_type(re.sub(r"^const ", "", t))
    return ty.kind == "enum" or t in ("bool", "const bool")


def choose(classifier, call_layer, declared, callable_emits, listing, defined, include_dir):
    """(hook points, (name, why) with none)."""
    by_name = {}
    for function in declared:
        by_name.setdefault(function.short, []).append(function)
    callable_by_name = {emit.lua_name: emit for emit in callable_emits}
    points, held = [], []
    for short, overloads in sorted(by_name.items()):
        function = overloads[0]
        why = _why_none(short, function, overloads, listing, defined, classifier, call_layer, include_dir)
        if why:
            held.append((function.name, why))
            continue
        entry = classifier.listing.functions.get(short, {})
        emit = callable_by_name.get(short) or classifier._emit(short, function, entry, include_dir)
        points.append(_point(short, function, emit, short in callable_by_name, listing, classifier, call_layer))
    for table, label in ((listing.held, "held"), (listing.functions, "functions")):
        for name in table:
            if name not in by_name:
                raise classifier.config_error(f"hook_layer.toml [{label}] {name}: no function of that name in the headers")
    return points, held


def _why_none(short, function, overloads, listing, defined, classifier, call_layer, include_dir):
    if short in listing.held:
        return listing.held[short]
    header = function.file[len(include_dir) + 1:]
    if header in listing.held_headers:
        return listing.held_headers[header]
    if function.name != short:
        return "in a namespace: the routing renames file-scope functions"
    if short.startswith("operator"):
        return "an operator"
    if len({f.signature for f in overloads}) > 1:
        return "overloaded: a Lua name would not say which"
    if "..." in function.signature:
        return "variadic: its arguments have no description"
    places = defined.get(short, [])
    if len(places) != 1:
        return f"{len(places)} definitions in the game's sources: the routing needs one"
    entry = classifier.listing.functions.get(short, {})
    problem = classifier._signature_problem(function, entry)
    if problem:
        return problem
    returns = call_layer._return_text(function.signature)
    if returns.endswith("&") and not classifier._is_pool(returns[:-1].strip()):
        return f"returns {returns}: a copy would not outlive the call"
    return ""


def _point(short, function, emit, is_callable, listing, classifier, call_layer):
    entry = listing.functions.get(short, {})
    returns = call_layer._return_text(function.signature)
    reach = "Watch"
    returned_range = entry.get("returns_range")
    returned_check = entry.get("returns_check", "")
    if is_callable and "watch_only" not in entry:
        reach = "ChangeArguments"
        if "no_replace" not in entry and (returned_range or _return_crosses_back(returns, classifier)):
            reach = "Replace"
    bare = re.sub(r"^(const )?(struct )?", "", returns.rstrip("*& ")).strip()
    if not returned_check and classifier._is_pool(bare):
        returned_check = classifier.listing.pools[bare].get("check", "")
    return HookEmit(short, emit, returns, list(function.params), function.signature.strip().endswith("noexcept"), reach,
                    tuple(returned_range) if returned_range else None, returned_check, "returns_nil" in entry)


def _renamed(name):
    return RENAMED_PREFIX + name


def _declaration(point, name):
    params = ", ".join(f"{text} {param}" for param, text in point.arguments)
    return f"{point.returns} {name}({params}){' noexcept' if point.noexcept else ''}"


def _subject_core_name(point):
    return f"kf_hook_on_subject_{point.name}"


def render(points, call_listing, instructions_per_fire, call_layer, cpp_string):
    """The generated source's hook layer, after the call layer's."""
    lines = ["", f"static std::array<std::atomic<bool>, {len(points)}> kf_hook_active{{}};",
             "static verdite::RawHookDispatch kf_hook_dispatch;", ""]
    lines += [_declaration(point, _renamed(point.name)) + ";" for point in points]
    lines.append("")
    for point in points:
        if point.emit.subject:
            binder = call_listing.subjects[point.emit.subject]["binder"]
            params = ", ".join([f"{point.emit.subject} *subject", *(f"{text} {name}" for name, text in point.arguments)])
            arguments = ", ".join(name for name, _ in point.arguments)
            lines += [f"static {point.returns} {_subject_core_name(point)}({params}) {{",
                      f"    const {binder} bound(subject);",
                      f"    return {_renamed(point.name)}({arguments});",
                      "}"]
    lines += ["", "const verdite::RawHookLayer &kf_hook_layer() {", "    static const std::array points = {"]
    for point in points:
        core = _subject_core_name(point) if point.emit.subject else _renamed(point.name)
        if point.emit.params:
            params = ", ".join(call_layer._param_text(param, cpp_string) for param in point.emit.params)
            array = f"std::array{{{params}}}"
        else:
            array = "std::array<verdite::RawParamListing, 0>{}"
        doc = cpp_string(point.emit.doc) if point.emit.doc else "{}"
        check = point.emit.check or "nullptr"
        returned = "{}"
        if point.returned_range or point.returned_check or point.returned_nil:
            limits = call_layer._limits_text((point.returned_range, ""), cpp_string)
            returned = (f"verdite::RawParamListing{{{{}}, {limits}, verdite::RawParamRole::In, "
                        f"{point.returned_check or 'nullptr'}, {'true' if point.returned_nil else 'false'}}}")
        lines.append(
            f"        verdite::raw_hook_point<&{core}>(verdite::raw_function<&{core}>({cpp_string(point.name)}, {array}, "
            f"verdite::RawFunctionListing{{verdite::RawCallWhen::{point.emit.when}, {doc}, {check}}}), "
            f"verdite::RawHookReach::{point.reach}, {returned}),"
        )
    lines += [
        "    };",
        f"    static const verdite::RawHookLayer layer{{{cpp_string(LAYER_PATH)}, kf_hook_doc, points, kf_hook_active, "
        f"&kf_hook_dispatch, {instructions_per_fire}}};",
        "    return layer;",
        "}",
        "",
    ]
    for index, point in enumerate(points):
        arguments = [name for name, _ in point.arguments]
        direct = f"{_renamed(point.name)}({', '.join(arguments)})"
        if point.emit.subject:
            current = call_listing.subjects[point.emit.subject]["current"]
            core = _subject_core_name(point)
            through = ", ".join(["kf_hook_layer()", str(index), current, *arguments])
        else:
            core = _renamed(point.name)
            through = ", ".join(["kf_hook_layer()", str(index), *arguments])
        lines += [
            f"{_declaration(point, point.name)} {{",
            f"    if (!kf_hook_active[{index}].load(std::memory_order_relaxed))",
            f"        return {direct};",
            f"    return verdite::raw_hook_fire<&{core}>({through});",
            "}",
        ]
    lines.append("")
    return lines


def write_sources(points, texts, decomp_src, hook_src, write_if_changed):
    """Each game source's copy for the build: its hook points' definitions renamed, and #line so that
    messages and debuggers still name the file in decomp_src."""
    names = {point.name for point in points}
    for source, text in texts.items():
        places = sorted((at, name) for name, offsets in find_definitions(text).items() if name in names for at in offsets)
        pieces, last = [], 0
        for at, name in places:
            pieces += [text[last:at], _renamed(name)]
            last = at + len(name)
        pieces.append(text[last:])
        out = os.path.join(hook_src, os.path.relpath(source, decomp_src))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        write_if_changed(out, f'#line 1 "{source}"\n' + "".join(pieces))


def render_report(points, held):
    lines = ["", "hook layer", "", f"hook points: {len(points)}", f"functions with none: {len(held)}", "",
             "hook points (W watch, A change arguments, R replace):"]
    letters = {"Watch": "W", "ChangeArguments": "A", "Replace": "R"}
    lines += [f"  {letters[point.reach]} {point.name}" for point in points]
    lines += ["", "with none:"] + [f"  {name}: {why}" for name, why in sorted(held)]
    return lines
