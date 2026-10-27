#!/usr/bin/env python3
"""Static contracts for the ten shipped Stata dialog resources."""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


CONTROL_TYPES = {
    "BUTTON", "CHECKBOX", "COMBOBOX", "EDIT", "EXP", "FILE",
    "GROUPBOX", "LISTBOX", "RADIO", "SPINNER", "TEXT", "VARNAME",
    "VARLIST",
}

HELP_ANCHORS = {
    'parqit_read': 'lazy', 'parqit_explore': 'explore', 'parqit_stats': 'explore',
    'parqit_filter': 'verbs', 'parqit_vars': 'verbs', 'parqit_gen': 'expressions',
    'parqit_pivot': 'verbs', 'parqit_combine': 'verbs',
    'parqit_write': 'materialisers', 'parqit_views': 'settings',
}


@dataclass
class Control:
    kind: str
    name: str
    block: str
    line: int


def uncomment(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"(?m)^\s*//.*$", "", text)


def named_blocks(lines: list[str], keyword: str) -> dict[str, list[tuple[int, str]]]:
    blocks: dict[str, list[tuple[int, str]]] = {}
    head = re.compile(rf"^\s*{keyword}\s+(\w+)\b")
    i = 0
    while i < len(lines):
        match = head.match(lines[i])
        if not match:
            i += 1
            continue
        name = match.group(1)
        i += 1
        while i < len(lines) and not re.match(r"^\s*BEGIN\s*$", lines[i]):
            i += 1
        i += 1
        body: list[tuple[int, str]] = []
        while i < len(lines) and not re.match(r"^\s*END\s*$", lines[i]):
            body.append((i + 1, lines[i]))
            i += 1
        blocks[name] = body
        i += 1
    return blocks


def controls_by_tab(lines: list[str]) -> tuple[dict[str, dict[str, Control]], list[str]]:
    errors: list[str] = []
    controls: dict[str, dict[str, Control]] = {}
    decl = re.compile(r"^\s*(" + "|".join(sorted(CONTROL_TYPES)) + r")\s+(\w+)\b")
    for tab, body in named_blocks(lines, "DIALOG").items():
        controls[tab] = {}
        starts: list[tuple[int, int, re.Match[str]]] = []
        for position, (lineno, line) in enumerate(body):
            match = decl.match(line)
            if match:
                starts.append((position, lineno, match))
        for index, (position, lineno, match) in enumerate(starts):
            end = starts[index + 1][0] if index + 1 < len(starts) else len(body)
            block = "\n".join(line for _, line in body[position:end])
            kind, name = match.group(1), match.group(2)
            if name in controls[tab]:
                errors.append(f"line {lineno}: duplicate control {tab}.{name}")
            controls[tab][name] = Control(kind, name, block, lineno)
    return controls, errors


def list_values(body: list[tuple[int, str]]) -> list[str]:
    return [line.strip() for _, line in body if line.strip()]


def attribute(control: Control, name: str) -> str | None:
    match = re.search(rf"\b{name}\(([^)]*)\)", control.block, flags=re.I)
    return match.group(1).strip() if match else None


def conditional_body(text: str, selector: str) -> str:
    match = re.search(rf"\bif\s+{re.escape(selector)}\s*\{{", text)
    if not match:
        return ""
    start = match.end()
    depth = 1
    for pos in range(start, len(text)):
        depth += (text[pos] == "{") - (text[pos] == "}")
        if depth == 0:
            return text[start:pos]
    return ""


def current_option_contracts(repo: Path, ado: str) -> list[str]:
    errors = []
    set_body = ado.split("program define _parqit_set", 1)[1].split("\nend", 1)[0]
    guard = next(line for line in set_body.splitlines() if "if !inlist(" in line)
    settings = set(re.findall(r'"([a-z][a-z0-9_]*)"', guard))
    views = uncomment((repo / "src/ado/p/parqit_views.dlg").read_text())
    lists = named_blocks(views.splitlines(), "LIST")
    values = set(list_values(lists.get("op_vals", [])))
    command = "\n".join(line for _, line in named_blocks(views.splitlines(), "PROGRAM")["command"])
    for setting in sorted(settings):
        if setting not in values or f'put "parqit set {setting} "' not in command:
            errors.append(f"parqit_views.dlg: missing set {setting} selection/emission")
    routes = {"parqit_read": ("opt", ["main.rb_use"], []),
              "parqit_write": ("opt", ["main.rb_collect"], ["main.rb_save | main.rb_data"]),
              "parqit_combine": ("opt", ["main.rb_mergein", "main.rb_appendin"],
                                  ["main.rb_merge", "main.rb_append", "main.rb_joinby"])}
    for name, (tab, enabled, forbidden) in routes.items():
        text = uncomment((repo / f"src/ado/p/{name}.dlg").read_text())
        controls, _ = controls_by_tab(text.splitlines())
        control = controls.get(tab, {}).get("cb_int64")
        if not control or attribute(control, "default") != '"default"':
            errors.append(f"{name}.dlg: int64 must distinguish inheritance from explicit refuse")
        command = "\n".join(line for _, line in named_blocks(text.splitlines(), "PROGRAM")["command"])
        emit = f"optionarg /hidedefault {tab}.cb_int64"
        for selector in enabled:
            if emit not in conditional_body(command, selector):
                errors.append(f"{name}.dlg: {selector} does not emit the chosen int64 option")
        for selector in forbidden:
            if f"{tab}.cb_int64" in conditional_body(command, selector):
                errors.append(f"{name}.dlg: {selector} must not emit int64")
    return errors


def put_program_spacing(raw: str) -> list[str]:
    # [P] dialog programming 5.1.4: the output of put /program reaches the
    # command trimmed at both ends, so the literal put before it must end in
    # a blank and the next put must begin with one; otherwise the pieces are
    # glued ("parqit collapse(mean) price", GitHub #2; "keep(match)keepusing(x)").
    # Comments are blanked line for line so that the reported lines are the
    # file's; if/else lines and braces are walked through, and any other
    # statement (beginoptions, option, optionarg, require, ...) ends the walk.
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), raw, flags=re.S)
    text = re.sub(r"(?m)^\s*//.*$", "", text)
    structural = re.compile(r"^(?:\{|\}|if\b|else\b)")
    plain_put = re.compile(r"^put\s+(?!/program\b)\S")
    errors = []
    for body in named_blocks(text.splitlines(), "PROGRAM").values():
        stmts: list[tuple[int, str]] = []
        for lineno, line in body:
            part = line.strip()
            if stmts and stmts[-1][1].endswith("///"):
                stmts[-1] = (stmts[-1][0], stmts[-1][1][:-3].rstrip() + " " + part)
            elif part:
                stmts.append((lineno, part))
        for index, (lineno, stmt) in enumerate(stmts):
            if not re.match(r"^put\s+/program\b", stmt):
                continue
            for before, prev in reversed(stmts[:index]):
                if structural.match(prev):
                    continue
                if plain_put.match(prev):
                    last = re.search(r'"([^"]*)"$', prev)
                    if not last or not last.group(1).endswith(" "):
                        errors.append(f"line {before}: {prev} is followed by put /program but has no trailing blank")
                break
            for after, nxt in stmts[index + 1:]:
                if structural.match(nxt):
                    continue
                if plain_put.match(nxt) and not re.match(r'^put\s+" ', nxt):
                    errors.append(f"line {after}: {nxt} follows put /program (line {lineno}) but has no leading blank")
                break
    return errors


def audit_dialog(path: Path) -> list[str]:
    raw = path.read_text(encoding="utf-8")
    text = uncomment(raw)
    lines = text.splitlines()
    controls, errors = controls_by_tab(lines)
    lists = {name: list_values(body) for name, body in named_blocks(lines, "LIST").items()}

    if text.count("INCLUDE _std_wide") != 1:
        errors.append("must include _std_wide exactly once")
    selector = controls.get("main", {}).get("cb_view")
    if not selector or attribute(selector, "contents") != "view_list" or "dropdownlist" not in selector.block:
        errors.append("must offer a named open-view selector")
    height = re.search(r"^DEFINE _dlght (\d+)$", text, re.M)
    if not height or int(height.group(1)) > 395:
        errors.append("dialog must fit the compact 395-unit height limit; use tabs for extra options")
    anchor = HELP_ANCHORS[path.stem]
    if f'HELP hlp1, view("help parqit##{anchor}")' not in text:
        errors.append(f"Help must target help parqit##{anchor}")
    if 'tx_context' not in controls.get('main', {}) or 'bu_context' not in controls.get('main', {}):
        errors.append('missing view-context label or Refresh control')
    programs = named_blocks(lines, 'PROGRAM')
    pending = ["command"]
    seen = set()
    emitted = ""
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        body = "\n".join(line for _, line in programs.get(name, []))
        emitted += "\n" + body
        pending.extend(re.findall(r"\bput\s+/program\s+(\w+)", body))
    for tab, members in controls.items():
        for name, control in members.items():
            if attribute(control, "option") is None:
                continue
            ref = re.escape(f"{tab}.{name}")
            forwarded = re.search(rf"^\s*(?:optionarg|option|put)\b[^\n]*\b{ref}\b", emitted, re.M)
            if not forwarded and control.kind == "CHECKBOX":
                option = re.escape(attribute(control, "option"))
                for condition in re.findall(rf"\bif\s+([^\n{{}}]*\b{ref}\b[^\n{{}}]*)\{{", emitted):
                    block = conditional_body(emitted, condition.strip())
                    if re.search(rf'\bput\s+"{option}(?:\s|"|$)', block):
                        forwarded = True
            if not forwarded:
                errors.append(f"option control {tab}.{name} never reaches PROGRAM command output")
    context = '\n'.join(line for _, line in programs.get('main_context', []))
    if 'stata hidden queue' not in context or not re.search(r'^\s*clear\s*$',context,re.M):
        errors.append('context must queue its Stata query and clear its command buffer')
    if not re.search(r"^VERSION 16\.0$", text, flags=re.M):
        errors.append("dialog VERSION must be 16.0")
    if "SYNCHRONOUS_ONLY" in text:
        errors.append("SYNCHRONOUS_ONLY is not justified")

    tabs = "|".join(re.escape(tab) for tab in controls)
    for match in re.finditer(rf"\b({tabs})\.(\w+)", text):
        tab, name = match.group(1), match.group(2)
        if name not in controls.get(tab, {}):
            errors.append(f"unknown control reference {tab}.{name}")

    for tab, tab_controls in controls.items():
        radios = [control for control in tab_controls.values() if control.kind == "RADIO"]
        if radios:
            first = sum(bool(re.search(r"\bfirst\b", control.block)) for control in radios)
            last = sum(bool(re.search(r"\blast\b", control.block)) for control in radios)
            if first != 1 or last != 1:
                errors.append(f"{tab}: radio markers first/last are {first}/{last}, expected 1/1")

        for control in tab_controls.values():
            for name in ("contents", "values", "onselchangelist"):
                target = attribute(control, name)
                if target and target not in lists:
                    errors.append(
                        f"line {control.line}: {tab}.{control.name} {name}() references missing LIST {target}"
                    )
            if control.kind == "LISTBOX":
                targets = {
                    name: attribute(control, name)
                    for name in ("contents", "values", "onselchangelist")
                }
                lengths = {name: len(lists[target]) for name, target in targets.items() if target in lists}
                if lengths and len(set(lengths.values())) != 1:
                    errors.append(f"line {control.line}: {tab}.{control.name} LIST lengths differ: {lengths}")

    for match in re.finditer(rf"\b({tabs})\.(\w+)\.(hide|show)\b", text):
        tab, name, action = match.groups()
        control = controls.get(tab, {}).get(name)
        if control and control.kind == "RADIO":
            errors.append(f"radio {tab}.{name} is individually {action}n")

    option_line = re.compile(
        r"^\s*(optionarg|option)\s+(?:(/\w+)\s+)?(main|opt)\.(\w+)\b", re.M
    )
    for match in option_line.finditer(text):
        command, style, tab, name = match.groups()
        control = controls.get(tab, {}).get(name)
        if not control:
            continue
        if attribute(control, "option") is None:
            errors.append(f"{command} target {tab}.{name} has no option()")
        if style == "/hidedefault" and attribute(control, "default") is None:
            errors.append(f"/hidedefault target {tab}.{name} has no default()")

    for name, body in named_blocks(lines, "SCRIPT").items():
        if name.upper().startswith("PREINIT"):
            joined = "\n".join(line for _, line in body)
            if re.search(r"\b(stata|plugin call|parqit\s+[_A-Za-z])\b", joined):
                errors.append(f"PREINIT script {name} invokes Stata/plugin/parqit")

    # Stata's documented /smartquote is required for FILE controls: ordinary
    # double quotes make a legal filename containing a double quote unparseable.
    if "put `\"\"\"' main.fi_" in raw or "put `\"\"\"' opt.fi_" in raw:
        errors.append("FILE control is wrapped in raw double quotes; use put /smartquote")

    # .formatfilter is undocumented: it takes ONE format string whose {0}, {1}
    # are filled by the arguments that follow, so a type list passed as separate
    # strings kept only its first entry (Browse offered Parquet only).
    if "formatfilter" in text:
        errors.append("uses the undocumented .formatfilter; give the FILE control a documented filter()")

    errors.extend(put_program_spacing(raw))

    return sorted(set(errors))


def source_filter_contracts(repo: Path, ado: str) -> list[str]:
    # The Browse button of each source picker must offer every input type the
    # resolver routes, plus Parquet (its default) and all files.
    body = ado.split("program define _parqit_resolve_source", 1)[1].split("\nend", 1)[0]
    exts = {"parquet", "*"}
    for group in re.findall(r"inlist\(\"`ext'\",\s*([^)]*)\)", body):
        exts.update(re.findall(r'"(\w+)"', group))
    exts.update(re.findall(r"\"`ext'\"\s*==\s*\"(\w+)\"", body))
    errors = []
    for name in ("parqit_read", "parqit_combine"):
        text = uncomment((repo / f"src/ado/p/{name}.dlg").read_text(encoding="utf-8"))
        controls, _ = controls_by_tab(text.splitlines())
        control = controls.get("main", {}).get("fi_using")
        spec = re.search(r'\bfilter\("([^"]*)"\)', control.block) if control else None
        # extensions are routed case-insensitively (x.RData = x.rdata)
        offered = {e.lower() for e in re.findall(r"\*\.(\w+|\*)", spec.group(1))} if spec else set()
        missing = sorted(exts - offered)
        if missing:
            errors.append(f"{name}.dlg: fi_using filter() does not offer " + ", ".join(f"*.{e}" for e in missing))
    return errors


def explore_view_contracts(repo: Path) -> list[str]:
    text = uncomment((repo / "src/ado/p/parqit_explore.dlg").read_text())
    controls, _ = controls_by_tab(text.splitlines())
    selector = controls.get("main", {}).get("cb_view")
    errors = []
    if not selector or attribute(selector, "contents") != "view_list" or "dropdownlist" not in selector.block:
        errors.append("parqit_explore.dlg: must offer an explicit open-view selector")
    blocks = named_blocks(text.splitlines(), "PROGRAM")
    command = "\n".join(line for _, line in blocks.get("command", []))
    lists = named_blocks(text.splitlines(), "LIST")
    for op in list_values(lists["op_vals"]):
        body = conditional_body(command, f'main.lb_op.equals("{op}")')
        if op == "descfile":
            if 'put "parqit describe "' not in body or "main.cb_view" in body:
                errors.append("parqit_explore.dlg: file description must be independent of view selection")
        elif 'put "parqit view " main.cb_view ": ' not in body:
            errors.append(f"parqit_explore.dlg: {op} does not explicitly target the selected view")
    return errors


def view_scope_contracts(repo: Path) -> list[str]:
    # Existing-view work must name its target; explicit source/memory/session
    # commands and management actions have their own, deliberately unprefixed
    # command grammar. A new raw verb here requires a reviewed mode exception.
    raw_allowed = {
        "read": {"use", "open", "path", "describe"},
        "explore": {"describe"}, "stats": set(), "filter": set(),
        "vars": set(), "gen": {"spssencode"}, "pivot": set(),
        "combine": {"mergein", "appendin"}, "write": {"save"},
        "views": {"view", "close", "views", "sql", "set", "version", "selftest"},
    }
    errors = []
    for name, allowed in raw_allowed.items():
        text = uncomment((repo / f"src/ado/p/parqit_{name}.dlg").read_text())
        for verb in re.findall(r'\bput "parqit ([A-Za-z_]\w*)', text):
            if not verb.startswith("_") and verb != "view" and verb not in allowed:
                errors.append(f"parqit_{name}.dlg: unscoped {verb} command")
        programs = named_blocks(text.splitlines(), "PROGRAM")
        for callback in ("main_context", "main_refresh", "main_target"):
            body = "\n".join(line for _, line in programs.get(callback, []))
            if 'put "parqit _dlgviews "' not in body or "stata hidden queue" not in body:
                errors.append(f"parqit_{name}.dlg: {callback} must queue the shared view callback")
    return errors


def main() -> int:
    repo = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
    dialogs = sorted((repo / "src/ado/p").glob("parqit_*.dlg"))
    failures: list[str] = []
    if len(dialogs) != 10:
        failures.append(f"expected 10 dialogs, found {len(dialogs)}")
    for dialog in dialogs:
        for error in audit_dialog(dialog):
            failures.append(f"{dialog.name}: {error}")

    write_programs = named_blocks(
        uncomment((repo / "src/ado/p/parqit_write.dlg").read_text(encoding="utf-8")).splitlines(),
        "PROGRAM",
    )
    populate = "\n".join(line for _, line in write_programs.get("main_populate", []))
    ado = (repo / "src/ado/p/parqit.ado").read_text(encoding="utf-8")
    if re.search(r"capture local [^=\n]+ = \.`dlg'\.[^\n]+\.value", ado):
        failures.append("numeric GUI .value members require class macro expansion, not scalar-expression lookup")
    glue = ado.split("program define _parqit__dlgviews, rclass", 1)[1].split("\nend", 1)[0]
    if "call program main_refresh" not in populate or '_parqit__dlgvars `dlg\' vv_list, data' not in glue:
        failures.append("parqit_write.dlg: memory-save Populate is not routed to the data schema")
    failures.extend(current_option_contracts(repo, ado))
    failures.extend(source_filter_contracts(repo, ado))
    failures.extend(explore_view_contracts(repo))
    failures.extend(view_scope_contracts(repo))
    if re.search(r"if\s*\(\s*`i'\s*>\s*\d+", ado):
        failures.append("parqit.ado: _dlgvars silently caps the populated variable list")
    if "capture .`dlgname'.`listname'.Arrdropall" not in ado:
        failures.append("parqit.ado: _dlgvars does not clear stale list entries before repopulating")
    if "program define _parqit__dlgvars, rclass" not in ado or "[, Data Numeric(name)]" not in ado:
        failures.append("parqit.ado: _dlgvars lacks the testable rclass/data contract")

    if failures:
        for failure in failures:
            print(f"dialog-lint FAIL: {failure}", file=sys.stderr)
        return 1
    print("dialog-lint OK: 10 dialogs; compact-height/controls/lists/options/help/quoting/populate/file-filter/put-program-blank contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
