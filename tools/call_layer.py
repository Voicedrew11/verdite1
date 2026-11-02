"""The call layer's part of tools/raw_layer.py (docs/CALL_LAYER_PLAN.md): which of the game's
functions a mod may call, and their table in the generated source.

Everything the generator can describe is callable, less what port/ext/call_layer.toml holds back and
what tools/call_audit.py found (port/ext/call_layer_audit.toml). A parameter crosses by its type:

  a value                 as the raw layer's value; a struct only if every field is described
  const T &               a copy
  T &, T *, const T *     only if the listed file gives its role, which also says the pointer is to
                          one T and not to an array
  a pool's element        its slot; [pools] names the pools
"""

import dataclasses
import re
import tomllib

LAYER_PATH = "kf.call"
TOP_LEVEL_KEYS = ("instructions_per_call", "any_time_headers", "held_headers", "held", "pools", "functions")
FUNCTION_KEYS = ("any_time", "why", "params")
PARAM_KEYS = ("range", "role", "check", "nil", "why")
POOL_KEYS = ("pool", "check")
ROLES = {"in": "In", "out": "Out", "inout": "InOut"}
AUDIT_REASONS = {
    "frame": "runs frames of its own: API stage two",
    "file_write": "writes the player's files, outside a mod's folder",
    "loads": "reads a file or allocates from the game's arenas: API stage two",
    "current": "uses the current actor, effect or map event: API stage two",
}


@dataclasses.dataclass
class Listing:
    instructions_per_call: int
    any_time_headers: set
    held_headers: dict
    held: dict
    pools: dict
    functions: dict
    audit: dict


@dataclasses.dataclass
class ParamEmit:
    name: str
    role: str
    limits: object
    check: str
    nil: bool


@dataclasses.dataclass
class FunctionEmit:
    name: str
    lua_name: str
    params: list
    any_time: bool
    doc: str
    file: str


def load(path, audit_path, config_error):
    def read(where):
        try:
            with open(where, "rb") as handle:
                return tomllib.load(handle)
        except (OSError, tomllib.TOMLDecodeError) as error:
            raise config_error(f"{where}: {error}") from error

    data = read(path)
    unknown = set(data) - set(TOP_LEVEL_KEYS)
    if unknown:
        raise config_error(f"{path}: unknown key(s) {', '.join(sorted(unknown))}")
    audit = {}
    for name, functions in read(audit_path).items():
        if name not in AUDIT_REASONS:
            raise config_error(f"{audit_path}: unknown class {name}")
        audit.update({function: AUDIT_REASONS[name] for function in functions})
    return Listing(
        instructions_per_call=data.get("instructions_per_call", 0),
        any_time_headers=set(data.get("any_time_headers", [])),
        held_headers=data.get("held_headers", {}),
        held=data.get("held", {}),
        pools=data.get("pools", {}),
        functions=data.get("functions", {}),
        audit=audit,
    )


def _strip_const(text):
    return re.sub(r"^const ", "", text.strip()).strip()


def _return_text(signature):
    """The return type in a function's type text, "s32 (u8, u8)" or "void (int) noexcept"."""
    text = signature.strip()
    if text.endswith(" noexcept"):
        text = text[: -len(" noexcept")]
    depth = 0
    for at in range(len(text) - 1, -1, -1):
        if text[at] == ")":
            depth += 1
        elif text[at] == "(":
            depth -= 1
            if depth == 0:
                return text[:at].strip()
    return ""


class Classifier:
    """Decides each function, and describes the types its parameters reach through the raw layer."""

    def __init__(self, layer, listing, config_error, held_type, record_emit):
        self.layer = layer
        self.listing = listing
        self.config_error = config_error
        self.held_type = held_type
        self.record_emit = record_emit
        self.pool_names = set(listing.pools)
        self.used_keys = set()

    def _is_pool(self, text):
        return _strip_const(re.sub(r"^struct ", "", _strip_const(text))) in self.pool_names

    def _fully_described(self, ty, seen=()):
        """A struct given to the game as a temporary: every field described, none const, or a
        field the game reads would be left zero."""
        if ty.kind == "array":
            return self._fully_described(ty.element, seen)
        if ty.kind != "record":
            return True
        if ty.ref in seen:
            return False
        emitted = self.layer.record(ty.ref)
        if not isinstance(emitted, self.record_emit) or emitted.held:
            return False
        rec = self.layer.index.records[ty.ref]
        if any(member.type_text.startswith("const ") or member.bitfield or not member.name for member in rec.members):
            return False
        return all(self._fully_described(field.ty, (*seen, ty.ref)) for field in emitted.fields)

    def _value_problem(self, text):
        ty = self.layer.parse_type(text)
        reason = self.layer._ensure(ty)
        if reason is not None:
            return reason.detail
        if ty.kind == "enum_storage":
            return f"{text} is stored, not passed"
        if not self._fully_described(ty):
            return f"{text} has fields the layer does not describe"
        return ""

    def _param_problem(self, text, listed):
        """Why this parameter cannot cross, or empty. listed is the parameter's entry, if any."""
        t = text.strip()
        role = listed.get("role", "")
        if t.endswith(("*", "&")):
            inner = t[:-1].strip()
            if inner.endswith(("*", "&")):
                return f"{t}: a pointer to a pointer"
            if self._is_pool(inner):
                return "" if not role else f"{t}: a pool's element has no role"
            const = inner.startswith("const ")
            problem = self._value_problem(_strip_const(inner))
            if problem:
                return problem
            ty = self.layer.parse_type(_strip_const(inner))
            if ty.kind != "record":
                return f"{t}: a pointer to anything but a struct may be a buffer"
            if t.endswith("&") and const:
                return "" if not role or role == "in" else f"{t}: a const reference is only in"
            if not role:
                return f"{t}: the listed file gives no role, which says it is one {ty.kind} and not an array"
            if const and role != "in":
                return f"{t}: const is only in"
            if not const and role == "in":
                return f"{t}: non-const must be out or inout"
            return ""
        if role:
            return f"{t}: a value has no role"
        return self._value_problem(t)

    def _return_problem(self, text):
        t = text.strip()
        if t in ("void", ""):
            return "" if t else "cannot read its return type"
        if t.endswith("*"):
            inner = t[:-1].strip()
            return "" if self._is_pool(inner) else f"returns {t}: only a pool's element may"
        if t.endswith("&"):
            inner = _strip_const(t[:-1].strip())
            if self._is_pool(inner):
                return ""
            return self._value_problem(inner)
        return self._value_problem(_strip_const(t))

    def _role_cpp(self, listed):
        return ROLES[listed.get("role", "in")]

    def _param_emit(self, function, param, listed, where):
        known = set(listed) - set(PARAM_KEYS)
        if known:
            raise self.config_error(f"{where}: unknown key(s) {', '.join(sorted(known))}")
        if listed.get("role", "in") not in ROLES:
            raise self.config_error(f"{where}: role must be in, out or inout")
        limits = None
        if "range" in listed or "why" in listed:
            ty = self.layer.parse_type(_strip_const(param[1].rstrip("*& ")))
            spec = listed.get("range")
            spec = tuple(spec) if isinstance(spec, list) else spec
            entry_range = None
            if spec is not None:
                if self._is_pool(_strip_const(param[1].rstrip("*& "))):
                    entry_range = spec
                else:
                    entry_range = self.layer._resolve_range(spec, ty, where)
            limits = (entry_range, listed.get("why", ""))
        check = listed.get("check", "")
        if self._is_pool(_strip_const(param[1].rstrip("*& "))) and not check:
            check = self.listing.pools[_strip_const(re.sub(r"^struct ", "", _strip_const(param[1].rstrip("*& "))))].get("check", "")
        return ParamEmit(param[0], self._role_cpp(listed), limits, check, bool(listed.get("nil", False)))

    def run(self, declared, include_dir):
        """(functions callable, (name, why) held back)."""
        by_name = {}
        for function in declared:
            by_name.setdefault(function.short, []).append(function)
        emitted, held = [], []
        for short, overloads in sorted(by_name.items()):
            distinct = {f.signature for f in overloads}
            function = overloads[0]
            entry = self.listing.functions.get(short, {})
            if entry:
                self.used_keys.add(short)
            why = self._why_held(short, function, distinct, include_dir)
            if not why:
                why = self._signature_problem(function, entry)
            if why:
                held.append((function.name, why))
                continue
            emitted.append(self._emit(short, function, entry, include_dir))
        self._check_keys(by_name)
        return emitted, held

    def _why_held(self, short, function, distinct, include_dir):
        if short in self.listing.held:
            return self.listing.held[short]
        header = function.file[len(include_dir) + 1:]
        if header in self.listing.held_headers:
            return self.listing.held_headers[header]
        if short in self.listing.audit:
            return self.listing.audit[short]
        if short.startswith("operator"):
            return "an operator"
        if len(distinct) > 1:
            return "overloaded: a Lua name would not say which"
        if "..." in function.signature:
            return "variadic: its arguments have no description"
        return ""

    def _signature_problem(self, function, entry):
        params_listed = entry.get("params", {})
        for name, text in function.params:
            problem = self._param_problem(text, params_listed.get(name, {}))
            if problem:
                return problem
        return self._return_problem(_return_text(function.signature))

    def _emit(self, short, function, entry, include_dir):
        unknown = set(entry) - set(FUNCTION_KEYS)
        if unknown:
            raise self.config_error(f"[functions.{short}]: unknown key(s) {', '.join(sorted(unknown))}")
        params_listed = entry.get("params", {})
        names = {name for name, _ in function.params}
        stale = set(params_listed) - names
        if stale:
            raise self.config_error(f"[functions.{short}]: no parameter named {', '.join(sorted(stale))}")
        params = [
            self._param_emit(short, param, params_listed.get(param[0], {}), f"[functions.{short}.params.{param[0]}]")
            for param in function.params
        ]
        header = function.file[len(include_dir) + 1:]
        any_time = entry.get("any_time", header in self.listing.any_time_headers)
        return FunctionEmit(function.name, short, params, any_time, entry.get("why", ""), function.file)

    def _check_keys(self, by_name):
        for table, label in ((self.listing.held, "held"), (self.listing.functions, "functions")):
            for name in table:
                if name not in by_name:
                    raise self.config_error(f"[{label}] {name}: no function of that name in the headers")
        for name in self.listing.audit:
            if name not in by_name:
                raise self.config_error(f"the audit lists {name}, which is in no header: rerun tools/call_audit.py")
        for name, table in self.listing.pools.items():
            unknown = set(table) - set(POOL_KEYS)
            if unknown or "pool" not in table:
                raise self.config_error(f"[pools.{name}]: needs pool, and may have check")
            if name not in self.layer.index.record_names:
                raise self.config_error(f"[pools.{name}]: no struct of that name")


def _limits_text(limits, cpp_string):
    if limits is None:
        return "{}"
    entry_range, why = limits
    range_text = "{}" if entry_range is None else f"verdite::RawRange{{{entry_range[0]}, {entry_range[1]}}}"
    return f"{{.range = {range_text}, .read_only = {{}}, .doc = {cpp_string(why) if why else '{}'}}}"


def _param_text(param, cpp_string):
    check = param.check if param.check else "nullptr"
    nil = "true" if param.nil else "false"
    return (
        f"verdite::RawParamListing{{{cpp_string(param.name)}, {_limits_text(param.limits, cpp_string)}, "
        f"verdite::RawParamRole::{param.role}, {check}, {nil}}}"
    )


def render(functions, listing, cpp_string):
    """The lines after the raw layer's: each pool's description, and kf_call_layer."""
    lines = []
    for name, table in sorted(listing.pools.items()):
        lines += [
            f"template <> struct verdite::RawPoolOf<{name}> {{",
            f"    static std::span<{name}> pool() {{ return {table['pool']}; }}",
            "};",
        ]
    lines += ["", "const verdite::RawCallLayer &kf_call_layer() {", "    static const std::array functions = {"]
    for function in functions:
        if function.params:
            params = ", ".join(_param_text(param, cpp_string) for param in function.params)
            array = f"std::array{{{params}}}"
        else:
            array = "std::array<verdite::RawParamListing, 0>{}"
        when = "AnyTime" if function.any_time else "BetweenUpdates"
        doc = cpp_string(function.doc) if function.doc else "{}"
        lines.append(
            f"        verdite::raw_function<&{function.name}>({cpp_string(function.lua_name)}, {array}, "
            f"verdite::RawFunctionListing{{verdite::RawCallWhen::{when}, {doc}}}),"
        )
    lines += [
        "    };",
        f"    static const verdite::RawCallLayer layer{{{cpp_string(LAYER_PATH)}, kf_call_doc, functions, "
        f"{listing.instructions_per_call}, kf_call_began, kf_call_ended}};",
        "    return layer;",
        "}",
        "",
    ]
    return lines


def render_report(functions, held):
    reasons = {}
    for _, why in held:
        reasons[why] = reasons.get(why, 0) + 1
    lines = ["", "call layer", "", f"functions callable: {len(functions)}", f"functions held back: {len(held)}", ""]
    lines += ["callable:"] + [f"  {f.lua_name}{' (any time)' if f.any_time else ''}" for f in functions]
    lines += ["", "held back:"] + [f"  {name}: {why}" for name, why in sorted(held)]
    return lines
