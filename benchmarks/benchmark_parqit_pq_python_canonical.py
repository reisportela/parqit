#!/usr/bin/env python3
"""Run parqit/pq benchmarks and validate both against Python/DuckDB canonical outputs."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

import duckdb
import pandas as pd
import pyarrow
import pyarrow.parquet as pq


WORKERS_COLUMNS = {
    "key": ["id", "year"],
    "numeric": ["id", "year", "wage", "age", "education", "firm_id", "hours", "tenure"],
    "other": ["gender", "region", "sector", "hire_date", "note"],
}

WORKFLOWS = {
    "describe": {
        "category": "common_command",
        "kind": "metadata",
        "source": "workers",
        "description": "common pq/parqit describe over the source parquet",
    },
    "path": {
        "category": "common_command",
        "kind": "path",
        "source": "workers",
        "description": "common pq/parqit path resolution",
    },
    "use": {
        "category": "common_command",
        "kind": "parquet",
        "python_action": "read",
        "source": "workers",
        "sql": "SELECT * FROM read_parquet('{workers}')",
        "description": "common pq/parqit use into host memory; validation dump is outside the timer",
        **WORKERS_COLUMNS,
    },
    "save": {
        "category": "common_command",
        "kind": "parquet",
        "python_action": "write",
        "source": "workers",
        "sql": "SELECT * FROM read_parquet('{workers}')",
        "description": "common pq/parqit save from an already-loaded in-memory dataset",
        **WORKERS_COLUMNS,
    },
    "merge": {
        "category": "common_command",
        "kind": "parquet",
        "python_action": "duckdb_copy",
        "source": "workers/firms",
        "sql": (
            "SELECT w.*, f.tfp, f.capital, f.industry "
            "FROM read_parquet('{workers}') AS w "
            "INNER JOIN read_parquet('{firms}') AS f USING (firm_id) "
            "WHERE w.year = 2022"
        ),
        "description": "common pq/parqit merge materialised to parquet",
        "key": ["id", "year"],
        "numeric": [
            "id",
            "year",
            "wage",
            "age",
            "education",
            "firm_id",
            "hours",
            "tenure",
            "tfp",
            "capital",
        ],
        "other": ["gender", "region", "sector", "hire_date", "note", "industry"],
    },
    "append": {
        "category": "common_command",
        "kind": "parquet",
        "python_action": "duckdb_copy",
        "source": "patents",
        "sql": "SELECT * FROM read_parquet('{patents}') UNION ALL SELECT * FROM read_parquet('{patents}')",
        "description": "common pq/parqit append materialised to parquet",
        "key": [],
        "numeric": ["firm_id", "pat_year"],
        "other": ["patent"],
        "multiset": ["firm_id", "pat_year", "patent"],
    },
    "workflow_filter_gen": {
        "category": "extra_workflow",
        "kind": "parquet",
        "python_action": "duckdb_copy",
        "source": "workers",
        "sql": (
            "SELECT *, ln(wage) AS lwage FROM read_parquet('{workers}') "
            "WHERE wage IS NOT NULL AND year >= 2020 AND wage > 0"
        ),
        "description": "additional manipulation workflow; pq uses native Stata after pq use",
        "key": ["id", "year"],
        "numeric": ["id", "year", "wage", "age", "education", "firm_id", "hours", "tenure", "lwage"],
        "other": ["gender", "region", "sector", "hire_date", "note"],
    },
    "workflow_collapse": {
        "category": "extra_workflow",
        "kind": "parquet",
        "python_action": "duckdb_copy",
        "source": "workers",
        "sql": (
            "SELECT firm_id, year, avg(wage) AS wage, stddev_samp(wage) AS sd_wage, "
            "count(wage) AS n FROM read_parquet('{workers}') GROUP BY firm_id, year"
        ),
        "description": "additional manipulation workflow; pq uses native Stata after pq use",
        "key": ["firm_id", "year"],
        "numeric": ["firm_id", "year", "wage", "sd_wage", "n"],
        "other": [],
    },
}


def q(path: Path) -> str:
    return str(path).replace("'", "''")


def run(cmd: list[str], *, cwd: Path, env: dict[str, str] | None = None, log: Path | None = None) -> None:
    started = time.perf_counter()
    proc = subprocess.run(cmd, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    elapsed = time.perf_counter() - started
    output = proc.stdout or ""
    if log is not None:
        log.write_text(output, encoding="utf-8")
    if proc.returncode:
        sys.stdout.write(output)
        raise SystemExit(f"command failed rc={proc.returncode} after {elapsed:.1f}s: {' '.join(cmd)}")


def source_paths(datadir: Path) -> dict[str, Path]:
    return {
        "workers": datadir / "workers_perf.parquet",
        "firms": datadir / "firms_perf.parquet",
        "patents": datadir / "patents_perf.parquet",
    }


def write_environment(outdir: Path, repo: Path, plugin: Path, datadir: Path, reps: int) -> None:
    info = {
        "timestamp": dt.datetime.now().isoformat(timespec="seconds"),
        "repo": str(repo),
        "plugin": str(plugin),
        "datadir": str(datadir),
        "reps": reps,
        "python": sys.version.replace("\n", " "),
        "platform": platform.platform(),
        "duckdb": duckdb.__version__,
        "pyarrow": pyarrow.__version__,
        "common_commands": [k for k, v in WORKFLOWS.items() if v["category"] == "common_command"],
        "extra_workflows": [k for k, v in WORKFLOWS.items() if v["category"] == "extra_workflow"],
    }
    for name, cmd in {
        "git_head": ["git", "rev-parse", "HEAD"],
        "git_status": ["git", "status", "--short"],
        "plugin_sha256": ["sha256sum", str(plugin)],
        "uptime": ["uptime"],
        "uname": ["uname", "-a"],
    }.items():
        try:
            info[name] = subprocess.check_output(cmd, cwd=repo, text=True, stderr=subprocess.STDOUT).strip()
        except Exception as exc:  # pragma: no cover - diagnostic best effort
            info[name] = f"unavailable: {exc}"
    (outdir / "environment.json").write_text(json.dumps(info, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_parquet(table: pyarrow.Table, artifact: Path) -> None:
    pq.write_table(table, artifact, compression="zstd", row_group_size=65_536, use_dictionary=True)


def python_canonical(repo: Path, datadir: Path, outdir: Path, reps: int) -> list[dict[str, object]]:
    artifacts = outdir / "artifacts" / "python"
    artifacts.mkdir(parents=True, exist_ok=True)
    paths = source_paths(datadir)
    sql_paths = {k: q(v) for k, v in paths.items()}
    con = duckdb.connect()
    con.execute(f"SET temp_directory='{q(outdir / 'duckdb_tmp')}'")
    (outdir / "duckdb_tmp").mkdir(exist_ok=True)
    rows: list[dict[str, object]] = []

    for iteration in range(1, reps + 1):
        shift = (iteration - 1) % len(WORKFLOWS)
        order = list(WORKFLOWS)
        order = order[shift:] + order[:shift]
        for workflow in order:
            spec = WORKFLOWS[workflow]
            source = paths[spec["source"].split("/")[0]]
            artifact = artifacts / f"{workflow}_{iteration}.parquet"
            started = time.perf_counter()
            rc = 0
            nrows: int | None = None
            ncols: int | None = None
            artifact_value = str(artifact)
            try:
                if spec["kind"] == "metadata":
                    meta = pq.ParquetFile(source).metadata
                    nrows = meta.num_rows
                    ncols = meta.num_columns
                    artifact_value = str(source.resolve())
                elif spec["kind"] == "path":
                    artifact_value = str(source.resolve())
                elif spec.get("python_action") == "read":
                    table = pq.read_table(source)
                    seconds = time.perf_counter() - started
                    write_parquet(table, artifact)
                    rows.append(
                        {
                            "workflow": workflow,
                            "method": "python",
                            "iteration": iteration,
                            "sequence": len(rows) + 1,
                            "seconds": seconds,
                            "rc": 0,
                            "ok": 1,
                            "failed": 0,
                            "rows": table.num_rows,
                            "cols": table.num_columns,
                            "artifact": str(artifact),
                        }
                    )
                    continue
                elif spec.get("python_action") == "write":
                    table = pq.read_table(source)
                    started = time.perf_counter()
                    write_parquet(table, artifact)
                    nrows = table.num_rows
                    ncols = table.num_columns
                else:
                    sql = spec["sql"].format(**sql_paths)
                    con.execute(
                        f"COPY ({sql}) TO '{q(artifact)}' "
                        "(FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 65536)"
                    )
                    meta = pq.ParquetFile(artifact).metadata
                    nrows = meta.num_rows
                    ncols = meta.num_columns
            except Exception as exc:
                rc = 1
                (outdir / f"python_{workflow}_{iteration}.error.txt").write_text(str(exc), encoding="utf-8")
            seconds = time.perf_counter() - started
            rows.append(
                {
                    "workflow": workflow,
                    "method": "python",
                    "iteration": iteration,
                    "sequence": len(rows) + 1,
                    "seconds": seconds,
                    "rc": rc,
                    "ok": int(rc == 0),
                    "failed": int(rc != 0),
                    "rows": nrows,
                    "cols": ncols,
                    "artifact": artifact_value,
                }
            )
    pd.DataFrame(rows).to_csv(outdir / "python_raw.csv", index=False)
    return rows


def relation(path: Path) -> str:
    return f"read_parquet('{q(path)}')"


def sql_ident(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def compare_keyed(con: duckdb.DuckDBPyConnection, workflow: str, canonical: Path, candidate: Path) -> dict[str, object]:
    spec = WORKFLOWS[workflow]
    key = spec["key"]
    numeric = [c for c in spec["numeric"] if c not in key]
    other = [c for c in spec["other"] if c not in key]
    using = " AND ".join([f"b.{sql_ident(c)} = t.{sql_ident(c)}" for c in key])
    full_join = f"FROM {relation(canonical)} b FULL OUTER JOIN {relation(candidate)} t ON {using}"
    row = con.execute(
        f"""
        SELECT
          (SELECT count(*) FROM {relation(canonical)}) AS canonical_rows,
          (SELECT count(*) FROM {relation(candidate)}) AS candidate_rows,
          sum(CASE WHEN {" OR ".join([f"b.{sql_ident(c)} IS NULL" for c in key])} THEN 1 ELSE 0 END) AS missing_in_canonical,
          sum(CASE WHEN {" OR ".join([f"t.{sql_ident(c)} IS NULL" for c in key])} THEN 1 ELSE 0 END) AS missing_in_candidate
        {full_join}
        """
    ).fetchone()
    result: dict[str, object] = {
        "workflow": workflow,
        "canonical_rows": int(row[0]),
        "candidate_rows": int(row[1]),
        "missing_in_canonical": int(row[2] or 0),
        "missing_in_candidate": int(row[3] or 0),
        "max_abs_diff": 0.0,
        "numeric_null_mismatches": 0,
        "other_mismatches": 0,
    }
    for col in numeric:
        diff, null_mismatch = con.execute(
            f"""
            SELECT
              max(CASE
                    WHEN b.{sql_ident(col)} IS NOT NULL AND t.{sql_ident(col)} IS NOT NULL
                    THEN abs(CAST(b.{sql_ident(col)} AS DOUBLE) - CAST(t.{sql_ident(col)} AS DOUBLE))
                    ELSE NULL
                  END) AS max_abs_diff,
              sum(CASE WHEN (b.{sql_ident(col)} IS NULL) <> (t.{sql_ident(col)} IS NULL) THEN 1 ELSE 0 END) AS null_mismatch
            {full_join}
            WHERE {" AND ".join([f"b.{sql_ident(c)} IS NOT NULL AND t.{sql_ident(c)} IS NOT NULL" for c in key])}
            """
        ).fetchone()
        result["max_abs_diff"] = max(float(result["max_abs_diff"]), float(diff or 0.0))
        result["numeric_null_mismatches"] = int(result["numeric_null_mismatches"]) + int(null_mismatch or 0)
    for col in other:
        mismatches = con.execute(
            f"""
            SELECT sum(CASE
              WHEN CAST(b.{sql_ident(col)} AS VARCHAR) IS DISTINCT FROM CAST(t.{sql_ident(col)} AS VARCHAR)
              THEN 1 ELSE 0 END)
            {full_join}
            WHERE {" AND ".join([f"b.{sql_ident(c)} IS NOT NULL AND t.{sql_ident(c)} IS NOT NULL" for c in key])}
            """
        ).fetchone()[0]
        result["other_mismatches"] = int(result["other_mismatches"]) + int(mismatches or 0)
    return result


def compare_multiset(con: duckdb.DuckDBPyConnection, workflow: str, canonical: Path, candidate: Path) -> dict[str, object]:
    cols = WORKFLOWS[workflow]["multiset"]
    select_cols = ", ".join(sql_ident(c) for c in cols)
    using = " AND ".join([f"b.{sql_ident(c)} IS NOT DISTINCT FROM t.{sql_ident(c)}" for c in cols])
    row = con.execute(
        f"""
        WITH
        b AS (SELECT {select_cols}, count(*) AS cnt FROM {relation(canonical)} GROUP BY {select_cols}),
        t AS (SELECT {select_cols}, count(*) AS cnt FROM {relation(candidate)} GROUP BY {select_cols}),
        j AS (SELECT b.cnt AS bcnt, t.cnt AS tcnt FROM b FULL OUTER JOIN t ON {using})
        SELECT
          (SELECT count(*) FROM {relation(canonical)}) AS canonical_rows,
          (SELECT count(*) FROM {relation(candidate)}) AS candidate_rows,
          sum(CASE WHEN bcnt IS NULL THEN 1 ELSE 0 END) AS missing_in_canonical,
          sum(CASE WHEN tcnt IS NULL THEN 1 ELSE 0 END) AS missing_in_candidate,
          max(abs(coalesce(bcnt, 0) - coalesce(tcnt, 0))) AS max_count_diff
        FROM j
        """
    ).fetchone()
    return {
        "workflow": workflow,
        "canonical_rows": int(row[0]),
        "candidate_rows": int(row[1]),
        "missing_in_canonical": int(row[2] or 0),
        "missing_in_candidate": int(row[3] or 0),
        "max_abs_diff": float(row[4] or 0.0),
        "numeric_null_mismatches": 0,
        "other_mismatches": 0,
    }


def raw_rows(outdir: Path) -> pd.DataFrame:
    frames = [pd.read_csv(outdir / "python_raw.csv")]
    stata_csv = outdir / "stata_raw.csv"
    if stata_csv.exists():
        frames.append(pd.read_csv(stata_csv))
    return pd.concat(frames, ignore_index=True, sort=False)


def validate_outputs(outdir: Path, reps: int, tolerance: float) -> pd.DataFrame:
    con = duckdb.connect()
    raw = raw_rows(outdir)
    rows: list[dict[str, object]] = []
    methods = ["python", "pq", "parqit"]

    for workflow, spec in WORKFLOWS.items():
        canonical_artifact = outdir / "artifacts" / "python" / f"{workflow}_1.parquet"
        canonical_raw = raw[(raw["workflow"] == workflow) & (raw["method"] == "python") & (raw["iteration"] == 1)]
        if canonical_raw.empty:
            raise SystemExit(f"missing python canonical raw row for {workflow}")
        canon = canonical_raw.iloc[0]

        for method in methods:
            for iteration in range(1, reps + 1):
                raw_match = raw[
                    (raw["workflow"] == workflow)
                    & (raw["method"] == method)
                    & (raw["iteration"] == iteration)
                ]
                base = {
                    "workflow": workflow,
                    "category": spec["category"],
                    "method": method,
                    "iteration": iteration,
                }
                if raw_match.empty:
                    base.update({"exists": False, "passed": False, "reason": "missing timing row"})
                    rows.append(base)
                    continue
                row = raw_match.iloc[0]
                base.update({"artifact": row.get("artifact", ""), "exists": True})

                if spec["kind"] == "metadata":
                    passed = (
                        int(row["rc"]) == 0
                        and int(row["rows"]) == int(canon["rows"])
                        and int(row["cols"]) == int(canon["cols"])
                    )
                    base.update(
                        {
                            "canonical_rows": int(canon["rows"]),
                            "candidate_rows": int(row["rows"]) if pd.notna(row["rows"]) else None,
                            "canonical_cols": int(canon["cols"]),
                            "candidate_cols": int(row["cols"]) if pd.notna(row["cols"]) else None,
                            "max_abs_diff": 0.0,
                            "numeric_null_mismatches": 0,
                            "other_mismatches": 0,
                            "missing_in_canonical": 0,
                            "missing_in_candidate": 0,
                            "passed": bool(passed),
                            "reason": "" if passed else "metadata mismatch",
                        }
                    )
                    rows.append(base)
                    continue

                if spec["kind"] == "path":
                    passed = int(row["rc"]) == 0 and str(row["artifact"]) == str(canon["artifact"])
                    base.update(
                        {
                            "canonical_path": str(canon["artifact"]),
                            "candidate_path": str(row["artifact"]),
                            "max_abs_diff": 0.0,
                            "numeric_null_mismatches": 0,
                            "other_mismatches": 0,
                            "missing_in_canonical": 0,
                            "missing_in_candidate": 0,
                            "passed": bool(passed),
                            "reason": "" if passed else "path mismatch",
                        }
                    )
                    rows.append(base)
                    continue

                candidate = outdir / "artifacts" / method / f"{workflow}_{iteration}.parquet"
                if not candidate.exists():
                    base.update({"exists": False, "passed": False, "reason": "missing artifact"})
                    rows.append(base)
                    continue
                try:
                    if spec.get("multiset"):
                        result = compare_multiset(con, workflow, canonical_artifact, candidate)
                    else:
                        result = compare_keyed(con, workflow, canonical_artifact, candidate)
                    passed = (
                        result["canonical_rows"] == result["candidate_rows"]
                        and result["missing_in_canonical"] == 0
                        and result["missing_in_candidate"] == 0
                        and result["numeric_null_mismatches"] == 0
                        and result["other_mismatches"] == 0
                        and float(result["max_abs_diff"]) <= tolerance
                    )
                    base.update(result)
                    base.update({"passed": bool(passed), "reason": "" if passed else "mismatch"})
                except Exception as exc:
                    base.update({"passed": False, "reason": str(exc)})
                rows.append(base)
    df = pd.DataFrame(rows)
    df.to_csv(outdir / "validation.csv", index=False)
    return df


def summarize_timings(outdir: Path, python_rows: list[dict[str, object]]) -> pd.DataFrame:
    raw = raw_rows(outdir)
    raw.to_csv(outdir / "benchmark_raw.csv", index=False)
    summary = (
        raw.groupby(["workflow", "method"], as_index=False)
        .agg(
            runs=("seconds", "count"),
            failures=("failed", "sum"),
            mean_seconds=("seconds", "mean"),
            sd_seconds=("seconds", "std"),
            min_seconds=("seconds", "min"),
            p50_seconds=("seconds", "median"),
            max_seconds=("seconds", "max"),
        )
        .sort_values(["workflow", "method"])
    )
    summary["category"] = summary["workflow"].map(lambda w: WORKFLOWS[w]["category"])
    summary = summary[
        ["category", "workflow", "method", "runs", "failures", "mean_seconds", "sd_seconds",
         "min_seconds", "p50_seconds", "max_seconds"]
    ]
    summary.to_csv(outdir / "benchmark_summary.csv", index=False)
    return summary


def write_report(outdir: Path, summary: pd.DataFrame, validation: pd.DataFrame, reps: int, tolerance: float) -> None:
    validation_summary = (
        validation.groupby(["category", "workflow", "method"], as_index=False)
        .agg(
            checks=("passed", "count"),
            passed=("passed", "sum"),
            max_abs_diff=("max_abs_diff", "max"),
            numeric_null_mismatches=("numeric_null_mismatches", "sum"),
            other_mismatches=("other_mismatches", "sum"),
            missing_in_canonical=("missing_in_canonical", "sum"),
            missing_in_candidate=("missing_in_candidate", "sum"),
        )
        .sort_values(["category", "workflow", "method"])
    )
    validation_summary["verdict"] = validation_summary.apply(
        lambda r: "PASS" if int(r["checks"]) == int(r["passed"]) else "FAIL", axis=1
    )
    validation_summary.to_csv(outdir / "validation_summary.csv", index=False)

    common_validation = validation_summary[validation_summary["category"] == "common_command"]
    extra_validation = validation_summary[validation_summary["category"] == "extra_workflow"]
    common_summary = summary[summary["category"] == "common_command"]
    extra_summary = summary[summary["category"] == "extra_workflow"]

    lines = [
        "# parqit vs pq vs Python canonical benchmark",
        "",
        f"- Repetitions: {reps}",
        f"- Numeric tolerance: {tolerance:g}",
        "- Canonical implementation: Python/pyarrow for use/save/describe/path; Python DuckDB for relational outputs.",
        "- Common pq/parqit commands covered: `describe`, `path`, `use`, `save`, `merge`, `append`.",
        "- Additional workflows retained: `workflow_filter_gen`, `workflow_collapse`; pq uses native Stata after `pq use` there.",
        "- For `use`, timing covers read into host memory only; the Parquet validation dump is written after the timer.",
        "- For `save`, the input table is loaded before the timer; timing covers memory-to-Parquet write only.",
        "",
        "## Precision: Common Commands",
        "",
        common_validation.to_markdown(index=False, floatfmt=".3g"),
        "",
        "## Performance: Common Commands",
        "",
        common_summary.to_markdown(index=False, floatfmt=".3f"),
        "",
        "## Precision: Additional Workflows",
        "",
        extra_validation.to_markdown(index=False, floatfmt=".3g"),
        "",
        "## Performance: Additional Workflows",
        "",
        extra_summary.to_markdown(index=False, floatfmt=".3f"),
        "",
        "## Files",
        "",
        "- Raw timings: `benchmark_raw.csv`",
        "- Timing summary: `benchmark_summary.csv`",
        "- Validation detail: `validation.csv`",
        "- Validation summary: `validation_summary.csv`",
        "- Stata log: `stata_parqit_pq.log`",
        "- Environment: `environment.json`",
        "",
    ]
    (outdir / "REPORT.md").write_text("\n".join(lines), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--plugin", type=Path)
    parser.add_argument("--datadir", type=Path)
    parser.add_argument("--outdir", type=Path)
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--stata-bin", default="stata-mp")
    parser.add_argument("--tolerance", type=float, default=1e-8)
    parser.add_argument("--skip-stata", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    repo = args.repo.resolve()
    plugin = (args.plugin or repo / "build/dev/parqit.plugin").resolve()
    datadir = (args.datadir or repo / "benchmarks/_out/synthetic_medium_data").resolve()
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    outdir = (args.outdir or repo / f"benchmarks/_out/parqit_pq_python_canonical_{stamp}").resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo / "benchmarks/benchmark_parqit_pq_python_canonical.do", outdir / "benchmark_parqit_pq_python_canonical.do")
    shutil.copy2(Path(__file__), outdir / "benchmark_parqit_pq_python_canonical.py")

    paths = source_paths(datadir)
    for path in [plugin, paths["workers"], paths["firms"], paths["patents"]]:
        if not path.exists():
            raise SystemExit(f"missing required input: {path}")

    write_environment(outdir, repo, plugin, datadir, args.reps)
    python_rows = python_canonical(repo, datadir, outdir, args.reps)

    if not args.skip_stata:
        env = os.environ.copy()
        env["PARQIT_REPO"] = str(repo)
        run(
            [
                args.stata_bin,
                "-b",
                "do",
                str(repo / "benchmarks/benchmark_parqit_pq_python_canonical.do"),
                str(repo),
                str(plugin),
                str(datadir),
                str(outdir),
                str(args.reps),
            ],
            cwd=repo,
            env=env,
            log=outdir / "stata_process_output.log",
        )

    validation = validate_outputs(outdir, args.reps, args.tolerance)
    summary = summarize_timings(outdir, python_rows)
    write_report(outdir, summary, validation, args.reps, args.tolerance)
    print(outdir)


if __name__ == "__main__":
    main()
