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
runtime_name = "parqit_vcomp140.dll"
notice_name = "parqit_openmp_license.txt"

if args.mode == "prepare":
    records = [line.split() for line in
               (args.repo / "src/ado/p/parqit.pkg").read_text().splitlines()]
    runtime = [r for r in records if len(r) == 4 and r[0] in {"g", "G"}
               and r[1] == "WIN64" and r[3] == runtime_name]
    notice = [r for r in records if len(r) == 2 and r[0] in {"f", "F"}
              and r[1] == notice_name]
    assert len(runtime) == len(notice) == 1
    platforms = sorted({r[1] for r in records if len(r) == 4
                        and r[0] in {"g", "G"} and r[3] == "parqit.plugin"})
    assert platforms
    args.work.mkdir()
    source.mkdir()
    (args.work / "plus").mkdir()
    (args.work / "other").mkdir()
    (source / runtime_name).write_bytes(b"INERT ROUTING FIXTURE\x00\r\n\x1a" + bytes(range(256)))
    (source / notice_name).write_bytes((args.repo / "src/ado/p" / notice_name).read_bytes())
    (source / "parqit_runtime_probe.ado").write_text(
        "program define parqit_runtime_probe\n    version 16.0\nend\n")
    (source / "stata.toc").write_text(
        "v 3\nd Isolated runtime routing fixture\np parqit_runtime_probe Runtime routing\n")
    lines = ["v 3", "d Isolated runtime routing fixture", "f parqit_runtime_probe.ado",
             " ".join(notice[0])]
    # Only the selector changes: exercise the Windows directive on the test host.
    lines += [f"{runtime[0][0]} {platform} {runtime_name} {runtime_name}"
              for platform in platforms]
    (source / "parqit_runtime_probe.pkg").write_text("\n".join(lines) + "\n")
else:
    assert (installed / runtime_name).read_bytes() == (source / runtime_name).read_bytes()
    assert (installed / notice_name).read_bytes().replace(b"\r\n", b"\n") == \
        (source / notice_name).read_bytes().replace(b"\r\n", b"\n")
    (args.work / "runtime_install.ok").write_text("PASS\n")
