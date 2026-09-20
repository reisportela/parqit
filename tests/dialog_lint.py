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
    'parqit_write': 'materialisers', 'parqit_views': 'options',
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
    routes = {"parqit_read": ("main", ["main.rb_use"], []),
              "parqit_write": ("main", ["main.rb_collect"], ["main.rb_save | main.rb_data"]),
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


def audit_dialog(path: Path) -> list[str]:
    raw = path.read_text(encoding="utf-8")
    text = uncomment(raw)
    lines = text.splitlines()
    controls, errors = controls_by_tab(lines)
    lists = {name: list_values(body) for name, body in named_blocks(lines, "LIST").items()}

    if text.count("INCLUDE _std_wide") != 1:
        errors.append("must include _std_wide exactly once")
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

    for match in re.finditer(r"\b(main|opt)\.(\w+)", text):
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

    for match in re.finditer(r"\b(main|opt)\.(\w+)\.(hide|show)\b", text):
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

    return sorted(set(errors))


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
    if "main.rb_data" not in populate or 'put ", data"' not in populate:
        failures.append("parqit_write.dlg: Populate does not select in-memory variables in memory-save mode")

    ado = (repo / "src/ado/p/parqit.ado").read_text(encoding="utf-8")
    failures.extend(current_option_contracts(repo, ado))
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
    print("dialog-lint OK: 10 dialogs; controls/lists/options/help/quoting/populate contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
