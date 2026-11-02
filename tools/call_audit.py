#!/usr/bin/env python3
"""Find which game functions a mod may not call yet (docs/CALL_LAYER_PLAN.md, step 5).

Reads every source of the decomp through clang's text AST and follows what each function refers to,
through global tables too, so a dispatch table's entries count as reached. A function is listed
under the first class it reaches. tools/raw_layer.py holds back every function listed.

  frame        presents or waits for a frame: unless listed as allowed, called from a mod's callback it
               would run the callback again
  file_write   writes the player's files, which a mod may not
  loads        reads a file or allocates from the game's arenas
  current      uses the current actor, effect or map event, which nothing a mod passes can set

  python3 tools/call_audit.py --compile-commands build/linux/compile_commands.json \
      --decomp-src build/linux/decomp_src --out port/ext/call_layer_audit.toml [--check]

Names merge overloads, and a function pointer set at run time is not seen, so the fuzzer's call mode
also reports a frame reached during a call. With --check it writes nothing and fails if the file is
stale. It reads every source (about a minute), so scripts/check.sh runs it and the build does not.
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
from collections import defaultdict

import raw_layer

CLASSES = ("frame", "file_write", "loads", "current")
SEEDS = {
    "frame": (
        "host_read_buttons", "host_present_faces", "host_present_frame", "host_begin_frame", "host_wait_frame",
        "host_wait_update", "host_wait_until_tick", "host_wait_buttons_released", "host_wait_button_press",
    ),
    "file_write": ("save_card_write", "automap_save", "settings_page_save", "settings_page_save_text"),
    "loads": (
        "memory_allocate", "memory_allocation_reset", "memory_destroy_arena", "memory_set_allocation_mode",
        "memory_release_last", "data_file_open", "data_file_read", "data_file_read_into",
    ),
}
# The globals' fields that hold a current object.
CURRENT_FIELDS = {
    "actor_state": ("current", "current_definition", "current_index"),
    "effect_state": ("current_record", "current_magic"),
    "map_runtime_state": ("current_event",),
}
TOP_KINDS = ("FunctionDecl", "CXXMethodDecl", "FunctionTemplateDecl", "VarDecl")
SCOPE_KINDS = raw_layer.NAMESPACE_KINDS + raw_layer.RECORD_KINDS + ("ClassTemplateDecl",)
REFERENCE = re.compile(r"\b(Function|CXXMethod|Var) 0x[0-9a-f]+ '([^']+)'")
MEMBER = re.compile(r"(?:\.|->)(\w+) 0x[0-9a-f]+")


def scan_source(argv, source, include_root):
    """Each function and variable a translation unit defines: what it refers to, and whether it is
    declared in the decomp's headers. Runs in a worker process."""
    proc = subprocess.Popen(
        [*argv, "-w", "-fsyntax-only", "-Xclang", "-ast-dump", source],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    state = raw_layer._LocationState()
    nodes = {}
    stack = []
    open_node, open_depth = None, 0
    for line in proc.stdout:
        match = raw_layer.TEXT_NODE.match(line)
        if match is None:
            continue
        depth = len(match.group(1)) // 2 + 1
        kind, rest = match.group(2), match.group(3)
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if open_node is not None and depth <= open_depth:
            open_node = None
        loc_file, name = raw_layer._text_node(rest, state)
        if open_node is None and kind in TOP_KINDS and name and all(k in SCOPE_KINDS for _, k in stack):
            open_node = nodes.setdefault(name, {"refers": set(), "vars": set(), "members": set(), "header": False})
            open_depth = depth
            open_node["header"] = open_node["header"] or kind != "VarDecl" and loc_file.startswith(include_root)
        elif open_node is not None:
            for ref_kind, target in REFERENCE.findall(rest):
                open_node["refers"].add(target)
                if ref_kind == "Var":
                    open_node["vars"].add(target)
            if kind == "MemberExpr":
                member = MEMBER.search(rest)
                if member:
                    open_node["members"].add(member.group(1))
        stack.append((depth, kind))
    stderr = proc.stderr.read()
    if proc.wait() != 0:
        raise raw_layer.ConfigError(f"clang failed on {source}:\n{stderr[-2000:]}")
    return nodes


def read_graph(commands, decomp_src):
    src_prefix = os.path.join(decomp_src, "src") + os.sep
    # The build compiles the hook layer's copies (tools/hook_layer.py); the audit reads the sources.
    routed_prefix = os.path.join(os.path.dirname(decomp_src), "hook_src") + os.sep
    include_root = os.path.join(decomp_src, "include", "kf") + os.sep
    jobs = []
    for entry in commands:
        source = entry["file"]
        if source.startswith(routed_prefix):
            source = os.path.join(decomp_src, source[len(routed_prefix):])
        if source.startswith(src_prefix):
            compiler, flags = raw_layer._compile_flags(raw_layer._command_tokens(entry))
            jobs.append(([compiler, *flags], source))
    if not jobs:
        raise raw_layer.ConfigError(f"no sources under {src_prefix} in the compile commands")
    graph = {}
    with concurrent.futures.ProcessPoolExecutor(max_workers=max(1, min(6, os.cpu_count() or 1))) as pool:
        futures = [pool.submit(scan_source, argv, source, include_root) for argv, source in jobs]
        for future in futures:
            for name, node in future.result().items():
                into = graph.setdefault(name, {"refers": set(), "vars": set(), "members": set(), "header": False})
                into["refers"] |= node["refers"]
                into["vars"] |= node["vars"]
                into["members"] |= node["members"]
                into["header"] = into["header"] or node["header"]
    return graph


def touches_current(node):
    return any(var in node["vars"] and node["members"] & set(fields) for var, fields in CURRENT_FIELDS.items())


def classify(graph):
    """class -> the functions declared in the headers that reach it, each in its first class."""
    callers = defaultdict(set)
    for name, node in graph.items():
        for target in node["refers"]:
            callers[target].add(name)
    seeds = {name: set(names) for name, names in SEEDS.items()}
    seeds["current"] = {name for name, node in graph.items() if touches_current(node)}
    for name, names in SEEDS.items():
        unseen = [seed for seed in names if seed not in callers and seed not in graph]
        if unseen:
            raise raw_layer.ConfigError(f"{name}: nothing refers to {', '.join(unseen)}: a seed went stale")
    listed, placed = {}, set()
    for name in CLASSES:
        seen, stack = set(seeds[name]), list(seeds[name])
        while stack:
            for caller in callers[stack.pop()]:
                if caller not in seen:
                    seen.add(caller)
                    stack.append(caller)
        listed[name] = sorted(fn for fn in seen - placed if fn in graph and graph[fn]["header"])
        placed |= seen
    return listed


def render(listed):
    lines = [
        "# Written by tools/call_audit.py from the decomp's sources: do not edit. scripts/check.sh fails if it is stale.",
        "# tools/raw_layer.py holds back every function here (docs/CALL_LAYER_PLAN.md).",
    ]
    for name in CLASSES:
        lines += ["", f"{name} = ["]
        lines += [f'    "{fn}",' for fn in listed[name]]
        lines.append("]")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--compile-commands", required=True)
    parser.add_argument("--decomp-src", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--check", action="store_true", help="fail if --out is stale; write nothing")
    args = parser.parse_args(argv)
    try:
        graph = read_graph(raw_layer.load_commands(args.compile_commands), os.path.abspath(args.decomp_src))
        text = render(classify(graph))
    except raw_layer.ConfigError as error:
        print(f"call_audit: {error}", file=sys.stderr)
        return 1
    if args.check:
        with open(args.out, encoding="utf-8") as handle:
            if handle.read() != text:
                print(f"call_audit: {args.out} is stale: run tools/call_audit.py without --check", file=sys.stderr)
                return 1
        print("call audit: up to date")
        return 0
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(text)
    print(f"call audit: wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
