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


def audit_dialog(path: Path) -> list[str]:
    raw = path.read_text(encoding="utf-8")
    text = uncomment(raw)
    lines = text.splitlines()
    controls, errors = controls_by_tab(lines)
    lists = {name: list_values(body) for name, body in named_blocks(lines, "LIST").items()}

    if text.count("INCLUDE _std_wide") != 1:
        errors.append("must include _std_wide exactly once")
    if 'HELP hlp1, view("help parqit##menu")' not in text:
        errors.append("Help must target help parqit##menu")
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
    if "main.ck_data" not in populate or 'put ", data"' not in populate:
        failures.append("parqit_write.dlg: Populate does not select in-memory variables when data is checked")

    ado = (repo / "src/ado/p/parqit.ado").read_text(encoding="utf-8")
    if re.search(r"if\s*\(\s*`i'\s*>\s*\d+", ado):
        failures.append("parqit.ado: _dlgvars silently caps the populated variable list")
    if "capture .`dlgname'.`listname'.Arrdropall" not in ado:
        failures.append("parqit.ado: _dlgvars does not clear stale list entries before repopulating")
    if "program define _parqit__dlgvars, rclass" not in ado or "[, Data]" not in ado:
        failures.append("parqit.ado: _dlgvars lacks the testable rclass/data contract")

    if failures:
        for failure in failures:
            print(f"dialog-lint FAIL: {failure}", file=sys.stderr)
        return 1
    print("dialog-lint OK: 10 dialogs; controls/lists/options/help/quoting/populate contracts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
