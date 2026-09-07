"""Exercise Stata's installation routing using the product manifest directives."""
from pathlib import Path
import argparse

parser = argparse.ArgumentParser()
parser.add_argument("mode", choices=["prepare", "check"])
parser.add_argument("repo", type=Path)
parser.add_argument("work", type=Path)
args = parser.parse_args()
source = args.work / "source"
installed = args.work / "plus" / "p"
notice_name = "parqit_openmp_license.txt"

if args.mode == "prepare":
    records = [line.split() for line in
               (args.repo / "src/ado/p/parqit.pkg").read_text().splitlines()]
    plugins = [r for r in records if len(r) == 4 and r[0] in {"g", "G"}
               and r[3] == "parqit.plugin"]
    notice = [r for r in records if len(r) == 2 and r[0] in {"f", "F"}
              and r[1] == notice_name]
    assert len(plugins) == 6 and len(notice) == 1
    assert not any('.dll' in field.lower() for r in records for field in r)
    args.work.mkdir()
    source.mkdir()
    (args.work / "plus").mkdir()
    (args.work / "other").mkdir()
    for name in sorted({r[2] for r in plugins}):
        (source / name).write_bytes(name.encode() + b"\x00\r\n\x1a" + bytes(range(256)))
    (source / notice_name).write_bytes((args.repo / "src/ado/p" / notice_name).read_bytes())
    (source / "parqit_runtime_probe.ado").write_text(
        "program define parqit_runtime_probe\n    version 16.0\nend\n")
    (source / "stata.toc").write_text(
        "v 3\nd Isolated runtime routing fixture\np parqit_runtime_probe Runtime routing\n")
    lines = ["v 3", "d Isolated runtime routing fixture", "f parqit_runtime_probe.ado",
             " ".join(notice[0])]
    lines += [" ".join(r) for r in plugins] + ["h parqit.plugin"]
    (source / "parqit_runtime_probe.pkg").write_text("\n".join(lines) + "\n")
else:
    assert (installed / 'parqit.plugin').read_bytes() in {
        path.read_bytes() for path in source.glob('parqit_*.plugin')}
    assert not list((args.work / 'plus').rglob('*.dll'))
    assert (installed / notice_name).read_bytes().replace(b"\r\n", b"\n") == \
        (source / notice_name).read_bytes().replace(b"\r\n", b"\n")
    (args.work / "runtime_install.ok").write_text("PASS\n")
