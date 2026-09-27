#!/usr/bin/env python3
"""Run the parqit teaching notebooks and store their outputs in place.

    python run_notebooks.py [--adopath DIR] [NOTEBOOK ...]

Each notebook runs top to bottom in a fresh Stata session through the nbstata
kernel (pip install nbstata; python -m nbstata.install). A Stata error stops
the run and leaves that notebook unchanged. Without --adopath, Stata finds
parqit where it is installed. With --adopath, DIR goes first on the ado-path
for this run only (for example a folder holding the release files); the cell
that does it is removed before saving, so no local path is written into a
notebook. Without NOTEBOOK arguments, every parqit_*.ipynb beside this script
is run.
"""
import argparse
import pathlib
import re
import sys

import nbformat
from nbclient import NotebookClient

HERE = pathlib.Path(__file__).resolve().parent
STATA_ERROR = re.compile(r"^r\(\d+\);", re.MULTILINE)


def merge_streams(outputs):
    # The kernel sends Stata's output in chunks; store each cell's text as one block.
    merged = []
    for out in outputs:
        last = merged[-1] if merged else None
        if (out.output_type == "stream" and last is not None
                and last.output_type == "stream" and last.name == out.name):
            last.text += out.text
        else:
            merged.append(out)
    return merged


def output_text(out):
    if out.output_type == "stream":
        return out.text
    if out.output_type == "error":
        return "\n".join(out.get("traceback", [])) + "\n" + out.get("evalue", "")
    return out.get("data", {}).get("text/plain", "")


def run(path, adopath, timeout):
    nb = nbformat.read(path, as_version=4)
    # A setup cell, removed before saving, also takes the style block and blank line
    # nbstata prints with the first cell of a session.
    setup = f'quietly adopath ++ "{adopath}"' if adopath else 'quietly display ""'
    nb.cells.insert(0, nbformat.v4.new_code_cell(setup))
    NotebookClient(nb, kernel_name="nbstata", timeout=timeout, record_timing=False,
                   resources={"metadata": {"path": str(path.parent)}}).execute()
    del nb.cells[0]
    count = 0
    for cell in nb.cells:
        if cell.cell_type != "code":
            continue
        count += 1
        cell.execution_count = count
        cell.outputs = merge_streams(cell.outputs)
        for out in cell.outputs:
            if "execution_count" in out:
                out.execution_count = count
            text = output_text(out)
            if STATA_ERROR.search(text):
                sys.exit(f"{path.name}: Stata error in code cell {count}:\n{text}")
            if adopath and adopath in text:
                sys.exit(f"{path.name}: code cell {count} prints the --adopath folder")
    nbformat.validate(nb)
    nbformat.write(nb, path)
    print(f"{path.name}: {count} code cells run and saved")


def main():
    ap = argparse.ArgumentParser(description="Run the parqit notebooks in place.")
    ap.add_argument("--adopath", help="folder put first on Stata's ado-path for this run")
    ap.add_argument("--timeout", type=int, default=1800, help="seconds allowed per cell")
    ap.add_argument("notebooks", nargs="*", type=pathlib.Path)
    args = ap.parse_args()
    adopath = str(pathlib.Path(args.adopath).resolve()) if args.adopath else None
    for path in args.notebooks or sorted(HERE.glob("parqit_*.ipynb")):
        run(path.resolve(), adopath, args.timeout)


if __name__ == "__main__":
    main()
