#!/usr/bin/env python3
"""Benchmark representative pq/parqit feature workflows against Python/DuckDB.

The goal is breadth, not a large-data stress test.  Each row is a representative
workflow for a public command or option family.  For parqit lazy verbs, the timed
workflow includes the materializer that makes DuckDB execute the plan.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import duckdb
import pandas as pd
import pyarrow as pa
import pyarrow.dataset as ds
import pyarrow.parquet as pq


@dataclass(frozen=True)
class Feature:
    name: str
    surface: str
    description: str
    methods: tuple[str, ...]


FEATURES: list[Feature] = [
    Feature("path", "pq_parqit_common", "resolve an absolute path", ("python", "pq", "parqit")),
    Feature("describe", "pq_parqit_common", "file schema/row metadata", ("python", "pq", "parqit")),
    Feature("describe_detailed", "pq_parqit_common", "detailed schema metadata", ("python", "pq", "parqit")),
    Feature("use", "pq_parqit_common", "read Parquet into host memory", ("python", "pq", "parqit")),
    Feature("use_varlist", "pq_parqit_common", "read selected columns", ("python", "pq", "parqit")),
    Feature("use_filter", "pq_parqit_common", "read rows satisfying a predicate", ("python", "pq", "parqit")),
    Feature("use_range", "pq_parqit_common", "read a row range", ("python", "pq", "parqit")),
    Feature("use_sort", "pq_parqit_common", "read sorted output", ("python", "pq", "parqit")),
    Feature("use_random_n", "pq_parqit_common", "read/sample a fixed row count", ("python", "pq", "parqit")),
    Feature("use_drop", "pq_parqit_common", "read all except selected columns", ("python", "pq", "parqit")),
    Feature("save", "pq_parqit_common", "write in-memory data to Parquet", ("python", "pq", "parqit")),
    Feature("save_varlist", "pq_parqit_common", "write selected columns", ("python", "pq", "parqit")),
    Feature("save_filter", "pq_parqit_common", "write filtered rows", ("python", "pq", "parqit")),
    Feature("save_partition_by", "pq_parqit_common", "write a partitioned Parquet dataset", ("python", "pq", "parqit")),
    Feature("save_chunk", "pq_parqit_common", "write with explicit row-group/chunk size", ("python", "pq", "parqit")),
    Feature("save_compression_zstd", "pq_parqit_common", "write with explicit zstd compression", ("python", "pq", "parqit")),
    Feature("append", "pq_parqit_common", "append/union-by-name one Parquet file", ("python", "pq", "parqit")),
    Feature("append_filter", "pq_parqit_common", "append filtered rows", ("python", "pq", "parqit")),
    Feature("merge", "pq_parqit_common", "many-to-one join", ("python", "pq", "parqit")),
    Feature("merge_keepusing", "pq_parqit_common", "join with projected using columns", ("python", "pq", "parqit")),
    Feature("use_lazy", "parqit_only", "open a lazy view without reading rows", ("python", "parqit")),
    Feature("collect", "parqit_only", "materialize a lazy view into Stata memory", ("python", "parqit")),
    Feature("open_data", "parqit_only", "promote current Stata data to a lazy view", ("python", "parqit")),
    Feature("keep_vars", "parqit_only", "lazy projection by varlist", ("python", "parqit")),
    Feature("drop_vars", "parqit_only", "lazy drop by varlist", ("python", "parqit")),
    Feature("keep_if", "parqit_only", "lazy row filter", ("python", "parqit")),
    Feature("drop_if", "parqit_only", "lazy anti-filter", ("python", "parqit")),
    Feature("keep_in", "parqit_only", "validated row range", ("python", "parqit")),
    Feature("gen", "parqit_only", "computed column", ("python", "parqit")),
    Feature("egen_by", "parqit_only", "group/window egen", ("python", "parqit")),
    Feature("replace", "parqit_only", "conditional replacement", ("python", "parqit")),
    Feature("rename", "parqit_only", "column rename", ("python", "parqit")),
    Feature("order", "parqit_only", "column order", ("python", "parqit")),
    Feature("sort", "parqit_only", "ascending sort", ("python", "parqit")),
    Feature("gsort", "parqit_only", "descending/compound sort", ("python", "parqit")),
    Feature("collapse", "parqit_only", "grouped aggregates", ("python", "parqit")),
    Feature("contract", "parqit_only", "grouped frequencies", ("python", "parqit")),
    Feature("duplicates_drop", "parqit_only", "deduplicate rows", ("python", "parqit")),
    Feature("duplicates_report", "parqit_only", "duplicate report", ("python", "parqit")),
    Feature("sample_count", "parqit_only", "deterministic count sample", ("python", "parqit")),
    Feature("reshape_wide", "parqit_only", "long-to-wide reshape", ("python", "parqit")),
    Feature("reshape_long", "parqit_only", "wide-to-long reshape", ("python", "parqit")),
    Feature("count", "parqit_only", "pushed-down row count", ("python", "parqit")),
    Feature("count_if", "parqit_only", "pushed-down conditional count", ("python", "parqit")),
    Feature("head", "parqit_only", "preview first rows", ("python", "parqit")),
    Feature("list", "parqit_only", "preview selected rows/columns", ("python", "parqit")),
    Feature("show", "parqit_only", "show generated SQL", ("python", "parqit")),
    Feature("explain", "parqit_only", "show DuckDB plan", ("python", "parqit")),
    Feature("sql_clear", "parqit_only", "raw SQL materialized to memory", ("python", "parqit")),
    Feature("sql_save", "parqit_only", "raw SQL view saved to Parquet", ("python", "parqit")),
    Feature("query_qualify", "parqit_only", "raw SQL fragment in the pipeline", ("python", "parqit")),
    Feature("summarize", "parqit_only", "pushed-down summary statistics", ("python", "parqit")),
    Feature("summarize_detail", "parqit_only", "pushed-down detailed summary", ("python", "parqit")),
    Feature("tabulate_oneway", "parqit_only", "one-way frequency table", ("python", "parqit")),
    Feature("tabulate_twoway", "parqit_only", "two-way frequency table", ("python", "parqit")),
    Feature("misstable", "parqit_only", "missing-value summary", ("python", "parqit")),
    Feature("misstable_patterns", "parqit_only", "missing-value pattern table", ("python", "parqit")),
    Feature("levelsof", "parqit_only", "distinct levels as r(levels)", ("python", "parqit")),
    Feature("ds", "parqit_only", "varlist discovery", ("python", "parqit")),
    Feature("lookfor", "parqit_only", "search variable names/labels", ("python", "parqit")),
    Feature("codebook", "parqit_only", "compact variable diagnostics", ("python", "parqit")),
    Feature("distinct", "parqit_only", "distinct count", ("python", "parqit")),
    Feature("tabstat", "parqit_only", "table of summary stats", ("python", "parqit")),
    Feature("correlate", "parqit_only", "correlation matrix", ("python", "parqit")),
    Feature("pwcorr", "parqit_only", "pairwise correlation", ("python", "parqit")),
    Feature("histogram_nodraw", "parqit_only", "histogram bin computation", ("python", "parqit")),
    Feature("joinby", "parqit_only", "many-to-many join", ("python", "parqit")),
    Feature("mergein", "parqit_only", "native merge with disk lookup read by parqit", ("python", "parqit")),
    Feature("appendin", "parqit_only", "native append with disk data read by parqit", ("python", "parqit")),
    Feature("glimpse", "parqit_only", "describe alias", ("python", "parqit")),
]


def q(path: Path) -> str:
    return str(path).replace("'", "''")


def run(cmd: list[str], *, cwd: Path, log: Path | None = None) -> None:
    proc = subprocess.run(cmd, cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if log is not None:
        log.write_text(proc.stdout or "", encoding="utf-8")
    if proc.returncode:
        sys.stdout.write(proc.stdout or "")
        raise SystemExit(f"command failed rc={proc.returncode}: {' '.join(cmd)}")


def make_data(src: Path, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect()
    con.execute("PRAGMA threads=4")
    workers = src / "workers_perf.parquet"
    firms = src / "firms_perf.parquet"
    patents = src / "patents_perf.parquet"
    con.execute(
        f"""
        COPY (
          SELECT * FROM read_parquet('{q(workers)}')
          WHERE id <= 40000
        ) TO '{q(out / 'workers.parquet')}'
        (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 32768)
        """
    )
    con.execute(
        f"""
        COPY (
          SELECT * FROM read_parquet('{q(firms)}')
          WHERE firm_id <= 500000
        ) TO '{q(out / 'firms.parquet')}'
        (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 32768)
        """
    )
    con.execute(
        f"""
        COPY (
          SELECT * FROM read_parquet('{q(patents)}')
          WHERE firm_id <= 40000
          LIMIT 50000
        ) TO '{q(out / 'patents.parquet')}'
        (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 32768)
        """
    )
    con.execute(
        f"""
        COPY (
          SELECT id,
                 max(CASE WHEN year=2018 THEN wage END) AS wage2018,
                 max(CASE WHEN year=2019 THEN wage END) AS wage2019,
                 max(CASE WHEN year=2020 THEN wage END) AS wage2020,
                 max(CASE WHEN year=2021 THEN wage END) AS wage2021,
                 max(CASE WHEN year=2022 THEN wage END) AS wage2022
          FROM read_parquet('{q(out / 'workers.parquet')}')
          GROUP BY id
        ) TO '{q(out / 'workers_wide.parquet')}'
        (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 32768)
        """
    )
    con.execute(
        f"COPY (SELECT * FROM read_parquet('{q(out / 'workers.parquet')}') LIMIT 5000) "
        f"TO '{q(out / 'workers_small.parquet')}' (FORMAT PARQUET, COMPRESSION ZSTD)"
    )
    con.execute(
        f"COPY (SELECT * FROM read_parquet('{q(out / 'workers.parquet')}') LIMIT 5000) "
        f"TO '{q(out / 'workers.csv')}' (HEADER, DELIMITER ',')"
    )


def parquet_shape(path: Path) -> tuple[int | None, int | None]:
    if path.exists() and path.is_file() and path.suffix == ".parquet":
        md = pq.ParquetFile(path).metadata
        return md.num_rows, md.num_columns
    return None, None


def write_parquet_from_sql(con: duckdb.DuckDBPyConnection, sql: str, artifact: Path) -> tuple[int | None, int | None]:
    artifact.parent.mkdir(parents=True, exist_ok=True)
    con.execute(f"COPY ({sql}) TO '{q(artifact)}' (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 32768)")
    return parquet_shape(artifact)


def py_time_feature(feature: str, datadir: Path, outdir: Path, iteration: int) -> tuple[float, int, int | None, int | None, str]:
    con = duckdb.connect()
    con.execute("PRAGMA threads=4")
    workers = datadir / "workers.parquet"
    firms = datadir / "firms.parquet"
    patents = datadir / "patents.parquet"
    wide = datadir / "workers_wide.parquet"
    artifact = outdir / "artifacts" / "python" / f"{feature}_{iteration}.parquet"
    started = time.perf_counter()
    rows: int | None = None
    cols: int | None = None
    path = str(artifact)
    rc = 0
    try:
        if feature == "path":
            path = str(workers.resolve())
        elif feature in {"describe", "describe_detailed", "glimpse"}:
            md = pq.ParquetFile(workers).metadata
            rows, cols = md.num_rows, md.num_columns
            path = str(workers)
        elif feature == "use":
            table = pq.read_table(workers)
            rows, cols = table.num_rows, table.num_columns
        elif feature == "use_varlist":
            table = pq.read_table(workers, columns=["id", "year", "wage"])
            rows, cols = table.num_rows, table.num_columns
        elif feature == "use_filter":
            table = con.execute(f"SELECT * FROM read_parquet('{q(workers)}') WHERE year=2022").fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        elif feature == "use_range":
            table = con.execute(f"SELECT * FROM read_parquet('{q(workers)}') LIMIT 5000 OFFSET 999").fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        elif feature == "use_sort":
            table = con.execute(f"SELECT * FROM read_parquet('{q(workers)}') ORDER BY firm_id, year, id").fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        elif feature in {"use_random_n", "sample_count"}:
            table = con.execute(f"SELECT * FROM read_parquet('{q(workers)}') ORDER BY hash(id, year, 12345) LIMIT 5000").fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        elif feature == "use_drop":
            table = con.execute(f"SELECT * EXCLUDE(note) FROM read_parquet('{q(workers)}')").fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        elif feature == "save":
            table = pq.read_table(workers)
            pq.write_table(table, artifact, compression="zstd", row_group_size=32768)
            rows, cols = parquet_shape(artifact)
        elif feature == "save_varlist":
            table = pq.read_table(workers, columns=["id", "year", "wage"])
            pq.write_table(table, artifact, compression="zstd", row_group_size=32768)
            rows, cols = parquet_shape(artifact)
        elif feature in {"save_filter", "keep_if"}:
            rows, cols = write_parquet_from_sql(con, f"SELECT * FROM read_parquet('{q(workers)}') WHERE year=2022", artifact)
        elif feature == "save_partition_by":
            dataset_dir = outdir / "artifacts" / "python" / f"{feature}_{iteration}"
            if dataset_dir.exists():
                shutil.rmtree(dataset_dir)
            table = pq.read_table(workers)
            ds.write_dataset(table, dataset_dir, format="parquet", partitioning=["year"], existing_data_behavior="delete_matching")
            path = str(dataset_dir)
        elif feature == "save_chunk":
            table = pq.read_table(workers)
            pq.write_table(table, artifact, compression="zstd", row_group_size=8192)
            rows, cols = parquet_shape(artifact)
        elif feature == "save_compression_zstd":
            table = pq.read_table(workers)
            pq.write_table(table, artifact, compression="zstd", row_group_size=32768)
            rows, cols = parquet_shape(artifact)
        elif feature == "append":
            rows, cols = write_parquet_from_sql(
                con,
                f"SELECT * FROM read_parquet('{q(patents)}') UNION ALL SELECT * FROM read_parquet('{q(patents)}')",
                artifact,
            )
        elif feature == "append_filter":
            rows, cols = write_parquet_from_sql(
                con,
                f"SELECT * FROM read_parquet('{q(patents)}') UNION ALL SELECT * FROM read_parquet('{q(patents)}') WHERE pat_year=2022",
                artifact,
            )
        elif feature == "merge":
            rows, cols = write_parquet_from_sql(
                con,
                f"SELECT w.*, f.tfp, f.capital, f.industry FROM read_parquet('{q(workers)}') w "
                f"INNER JOIN read_parquet('{q(firms)}') f USING(firm_id)",
                artifact,
            )
        elif feature == "merge_keepusing":
            rows, cols = write_parquet_from_sql(
                con,
                f"SELECT w.*, f.tfp FROM read_parquet('{q(workers)}') w INNER JOIN read_parquet('{q(firms)}') f USING(firm_id)",
                artifact,
            )
        elif feature == "use_lazy":
            con.execute(f"DESCRIBE SELECT * FROM read_parquet('{q(workers)}')").fetchall()
        elif feature == "collect":
            table = con.execute(f"SELECT * FROM read_parquet('{q(workers)}')").fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        elif feature == "open_data":
            table = pq.read_table(workers)
            pq.write_table(table, artifact, compression="zstd", row_group_size=32768)
            rows, cols = parquet_shape(artifact)
        elif feature == "keep_vars":
            rows, cols = write_parquet_from_sql(con, f"SELECT id, year, wage FROM read_parquet('{q(workers)}')", artifact)
        elif feature == "drop_vars":
            rows, cols = write_parquet_from_sql(con, f"SELECT * EXCLUDE(note) FROM read_parquet('{q(workers)}')", artifact)
        elif feature == "drop_if":
            rows, cols = write_parquet_from_sql(con, f"SELECT * FROM read_parquet('{q(workers)}') WHERE NOT (year < 2020)", artifact)
        elif feature == "keep_in":
            rows, cols = write_parquet_from_sql(con, f"SELECT * FROM read_parquet('{q(workers)}') LIMIT 5000 OFFSET 999", artifact)
        elif feature == "gen":
            rows, cols = write_parquet_from_sql(con, f"SELECT *, ln(wage) AS lwage FROM read_parquet('{q(workers)}') WHERE wage > 0", artifact)
        elif feature == "egen_by":
            rows, cols = write_parquet_from_sql(con, f"SELECT *, avg(wage) OVER (PARTITION BY firm_id) AS firm_mean FROM read_parquet('{q(workers)}')", artifact)
        elif feature == "replace":
            rows, cols = write_parquet_from_sql(con, f"SELECT * REPLACE (CASE WHEN wage IS NULL THEN 0 ELSE wage END AS wage) FROM read_parquet('{q(workers)}')", artifact)
        elif feature == "rename":
            rows, cols = write_parquet_from_sql(con, f"SELECT id, year, wage AS pay, age, education, firm_id, hours, tenure, gender, region, sector, hire_date, note FROM read_parquet('{q(workers)}')", artifact)
        elif feature == "order":
            rows, cols = write_parquet_from_sql(con, f"SELECT wage, id, year, * EXCLUDE(wage, id, year) FROM read_parquet('{q(workers)}')", artifact)
        elif feature == "sort":
            rows, cols = write_parquet_from_sql(con, f"SELECT * FROM read_parquet('{q(workers)}') ORDER BY firm_id, year, id", artifact)
        elif feature == "gsort":
            rows, cols = write_parquet_from_sql(con, f"SELECT * FROM read_parquet('{q(workers)}') ORDER BY wage DESC, id ASC", artifact)
        elif feature == "collapse":
            rows, cols = write_parquet_from_sql(con, f"SELECT firm_id, year, avg(wage) AS wage, stddev_samp(wage) AS sd_wage, count(wage) AS n FROM read_parquet('{q(workers)}') GROUP BY firm_id, year", artifact)
        elif feature == "contract":
            rows, cols = write_parquet_from_sql(con, f"SELECT region, sector, count(*) AS _freq FROM read_parquet('{q(workers)}') GROUP BY region, sector", artifact)
        elif feature == "duplicates_drop":
            rows, cols = write_parquet_from_sql(con, f"SELECT DISTINCT id, year, firm_id FROM read_parquet('{q(workers)}') ORDER BY id, year, firm_id", artifact)
        elif feature == "duplicates_report":
            con.execute(f"SELECT id, count(*) FROM read_parquet('{q(workers)}') GROUP BY id HAVING count(*) > 1 LIMIT 20").fetchall()
        elif feature == "reshape_wide":
            rows, cols = write_parquet_from_sql(
                con,
                f"SELECT * FROM (SELECT id, year, wage FROM read_parquet('{q(workers)}')) PIVOT(max(wage) FOR year IN (2018,2019,2020,2021,2022))",
                artifact,
            )
        elif feature == "reshape_long":
            rows, cols = write_parquet_from_sql(
                con,
                f"SELECT id, CAST(substr(variable,5) AS INTEGER) AS year, value AS wage "
                f"FROM (UNPIVOT read_parquet('{q(wide)}') ON wage2018,wage2019,wage2020,wage2021,wage2022 INTO NAME variable VALUE value)",
                artifact,
            )
        elif feature == "count":
            rows = con.execute(f"SELECT count(*) FROM read_parquet('{q(workers)}')").fetchone()[0]
            cols = 1
        elif feature == "count_if":
            rows = con.execute(f"SELECT count(*) FROM read_parquet('{q(workers)}') WHERE year=2022").fetchone()[0]
            cols = 1
        elif feature == "head":
            table = con.execute(f"SELECT * FROM read_parquet('{q(workers)}') LIMIT 10").fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        elif feature == "list":
            table = con.execute(f"SELECT id, year, wage FROM read_parquet('{q(workers)}') WHERE year=2022 LIMIT 20").fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        elif feature == "show":
            str(f"SELECT * FROM read_parquet('{q(workers)}')")
        elif feature == "explain":
            con.execute(f"EXPLAIN SELECT * FROM read_parquet('{q(workers)}') WHERE year=2022").fetchall()
        elif feature == "sql_clear":
            table = con.execute(f"SELECT id, year, wage FROM read_parquet('{q(workers)}') WHERE year=2022").fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        elif feature == "sql_save":
            rows, cols = write_parquet_from_sql(con, f"SELECT id, year, wage FROM read_parquet('{q(workers)}') WHERE year=2022", artifact)
        elif feature == "query_qualify":
            rows, cols = write_parquet_from_sql(
                con,
                f"SELECT id, year, wage FROM read_parquet('{q(workers)}') QUALIFY row_number() OVER (PARTITION BY id ORDER BY year) = 1",
                artifact,
            )
        elif feature == "summarize":
            con.execute(f"SELECT count(wage), avg(wage), stddev_samp(wage), min(wage), max(wage) FROM read_parquet('{q(workers)}')").fetchall()
        elif feature == "summarize_detail":
            con.execute(
                f"SELECT count(wage), avg(wage), var_samp(wage), skewness(wage), kurtosis(wage), "
                f"quantile_cont(wage, [0.01,0.05,0.1,0.25,0.5,0.75,0.9,0.95,0.99]) "
                f"FROM read_parquet('{q(workers)}')"
            ).fetchall()
        elif feature == "tabulate_oneway":
            con.execute(f"SELECT region, count(*) FROM read_parquet('{q(workers)}') GROUP BY region").fetchall()
        elif feature == "tabulate_twoway":
            con.execute(f"SELECT region, sector, count(*) FROM read_parquet('{q(workers)}') GROUP BY region, sector").fetchall()
        elif feature == "misstable":
            con.execute(f"SELECT count(*) FILTER (WHERE wage IS NULL), count(*) FILTER (WHERE note IS NULL) FROM read_parquet('{q(workers)}')").fetchall()
        elif feature == "misstable_patterns":
            con.execute(f"SELECT wage IS NULL, note IS NULL, count(*) FROM read_parquet('{q(workers)}') GROUP BY 1,2").fetchall()
        elif feature == "levelsof":
            con.execute(f"SELECT DISTINCT region FROM read_parquet('{q(workers)}') ORDER BY region").fetchall()
        elif feature == "ds":
            pq.ParquetFile(workers).schema_arrow.names
        elif feature == "lookfor":
            [c for c in pq.ParquetFile(workers).schema_arrow.names if "wage" in c]
        elif feature == "codebook":
            con.execute(f"SELECT count(*), count(DISTINCT wage), min(wage), max(wage) FROM read_parquet('{q(workers)}')").fetchall()
        elif feature == "distinct":
            con.execute(f"SELECT count(DISTINCT region) FROM read_parquet('{q(workers)}')").fetchall()
        elif feature == "tabstat":
            con.execute(f"SELECT avg(wage), stddev_samp(wage), min(wage), max(wage) FROM read_parquet('{q(workers)}') GROUP BY year").fetchall()
        elif feature in {"correlate", "pwcorr"}:
            con.execute(f"SELECT corr(wage, hours), corr(wage, tenure), corr(hours, tenure) FROM read_parquet('{q(workers)}')").fetchall()
        elif feature == "histogram_nodraw":
            con.execute(f"SELECT histogram(wage) FROM read_parquet('{q(workers)}')").fetchall()
        elif feature == "joinby":
            rows, cols = write_parquet_from_sql(
                con,
                f"SELECT w.*, f.tfp FROM read_parquet('{q(workers)}') w JOIN read_parquet('{q(firms)}') f USING(firm_id)",
                artifact,
            )
        elif feature == "mergein":
            table = con.execute(
                f"SELECT w.*, f.tfp FROM read_parquet('{q(workers)}') w LEFT JOIN read_parquet('{q(firms)}') f USING(firm_id)"
            ).fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        elif feature == "appendin":
            table = con.execute(f"SELECT * FROM read_parquet('{q(workers)}') UNION ALL SELECT * FROM read_parquet('{q(workers)}')").fetch_arrow_table()
            rows, cols = table.num_rows, table.num_columns
        else:
            raise ValueError(feature)
    except Exception:
        rc = 1
        raise
    finally:
        seconds = time.perf_counter() - started
    return seconds, rc, rows, cols, path


DO_TEMPLATE = r'''
clear all
set more off
set varabbrev off

args repo plugin datadir outdir reps

adopath ++ "`repo'/src/ado/p"
global PARQIT_PLUGIN_PATH "`plugin'"

local workers "`datadir'/workers.parquet"
local workers_small "`datadir'/workers_small.parquet"
local firms "`datadir'/firms.parquet"
local patents "`datadir'/patents.parquet"
local wide "`datadir'/workers_wide.parquet"
local outart "`outdir'/artifacts"
capture mkdir "`outart'"
capture mkdir "`outart'/pq"
capture mkdir "`outart'/parqit"

confirm file "`workers'"
confirm file "`firms'"
confirm file "`patents'"
confirm file "`wide'"
confirm file "`plugin'"

log using "`outdir'/feature_surface_stata.log", text replace
which pq
which parqit
parqit version

tempname rawpost
tempfile raw
postfile `rawpost' str32 feature str8 method int iteration int sequence ///
    double seconds int rc double rows int cols str244 artifact using "`raw'", replace

program define _feature_dims, rclass
    version 16.0
    args path
    return scalar rows = .
    return scalar cols = .
    capture confirm file "`path'"
    if (_rc) exit
    capture noisily parqit describe "`path'"
    if (_rc == 0) {
        return scalar rows = r(n_rows)
        return scalar cols = r(n_cols)
    }
end

program define _bench_feature
    version 16.0
    args feature method iter seq handle
    local workers "$FS_WORKERS"
    local workers_small "$FS_WORKERS_SMALL"
    local firms "$FS_FIRMS"
    local patents "$FS_PATENTS"
    local wide "$FS_WIDE"
    local outart "$FS_OUTART"
    local artifact "`outart'/`method'/`feature'_`iter'.parquet"
    local rc 0
    local rows .
    local cols .
    capture parqit close _all
    clear

    * Setup that is intentionally outside the timer for memory-to-file save features.
    if "`method'" == "pq" & inlist("`feature'", "save", "save_varlist", "save_filter", "save_partition_by", "save_chunk", "save_compression_zstd") {
        quietly pq use using "`workers'", clear
    }
    if "`method'" == "parqit" & inlist("`feature'", "save", "save_varlist", "save_partition_by", "save_chunk", "save_compression_zstd") {
        quietly parqit use using "`workers'", clear
    }
    if "`method'" == "parqit" & "`feature'" == "save_filter" {
        quietly parqit use using "`workers'"
    }
    if "`method'" == "parqit" & "`feature'" == "open_data" {
        quietly parqit use using "`workers_small'", clear
    }
    if "`method'" == "parqit" & inlist("`feature'", "mergein", "appendin") {
        quietly parqit use using "`workers'", clear
    }

    timer clear 1
    timer on 1

    if "`method'" == "pq" {
        if "`feature'" == "path" {
            capture noisily pq path "`workers'"
            local rc = _rc
            if (`rc' == 0) local artifact "`r(fullpath)'"
        }
        else if "`feature'" == "describe" {
            capture noisily pq describe using "`workers'", quietly
            local rc = _rc
            if (`rc' == 0) {
                local rows = real("`r(n_rows)'")
                local cols = real("`r(n_columns)'")
            }
        }
        else if "`feature'" == "describe_detailed" {
            capture noisily pq describe using "`workers'", quietly detailed
            local rc = _rc
            if (`rc' == 0) {
                local rows = real("`r(n_rows)'")
                local cols = real("`r(n_columns)'")
            }
        }
        else if "`feature'" == "use" {
            capture noisily pq use using "`workers'", clear
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_varlist" {
            capture noisily pq use id year wage using "`workers'", clear
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_filter" {
            capture noisily pq use using "`workers'", clear if(year == 2022)
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_range" {
            capture noisily pq use using "`workers'", clear in(1000/5999)
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_sort" {
            capture noisily pq use using "`workers'", clear sort(firm_id year id)
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_random_n" {
            capture noisily pq use using "`workers'", clear random_n(5000) random_seed(12345)
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_drop" {
            capture noisily pq use using "`workers'", clear drop(note)
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "save" {
            capture noisily pq save using "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "save_varlist" {
            capture noisily pq save id year wage using "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "save_filter" {
            capture noisily pq save using "`artifact'", replace if(year == 2022)
            local rc = _rc
        }
        else if "`feature'" == "save_partition_by" {
            local artifact "`outart'/`method'/`feature'_`iter'"
            capture noisily pq save using "`artifact'", replace partition_by(year)
            local rc = _rc
        }
        else if "`feature'" == "save_chunk" {
            capture noisily pq save using "`artifact'", replace chunk(8192)
            local rc = _rc
        }
        else if "`feature'" == "save_compression_zstd" {
            capture noisily pq save using "`artifact'", replace compression(zstd)
            local rc = _rc
        }
        else if "`feature'" == "append" {
            capture noisily pq use using "`patents'", clear
            local rc = _rc
            if (`rc' == 0) {
                capture noisily pq append using "`patents'"
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily pq save using "`artifact'", replace
                local rc = _rc
            }
        }
        else if "`feature'" == "append_filter" {
            capture noisily pq use using "`patents'", clear
            local rc = _rc
            if (`rc' == 0) {
                capture noisily pq append using "`patents'", if(pat_year == 2022)
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily pq save using "`artifact'", replace
                local rc = _rc
            }
        }
        else if "`feature'" == "merge" {
            capture noisily pq use using "`workers'", clear
            local rc = _rc
            if (`rc' == 0) {
                capture noisily pq merge m:1 firm_id using "`firms'", keep(match) nogenerate
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily pq save using "`artifact'", replace
                local rc = _rc
            }
        }
        else if "`feature'" == "merge_keepusing" {
            capture noisily pq use using "`workers'", clear
            local rc = _rc
            if (`rc' == 0) {
                capture noisily pq merge m:1 firm_id using "`firms'", keep(match) keepusing(tfp) nogenerate
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily pq save using "`artifact'", replace
                local rc = _rc
            }
        }
        else local rc = 198
    }
    else if "`method'" == "parqit" {
        if "`feature'" == "path" {
            capture noisily parqit path "`workers'"
            local rc = _rc
            if (`rc' == 0) local artifact "`r(path)'"
        }
        else if inlist("`feature'", "describe", "describe_detailed", "glimpse") {
            capture noisily parqit describe "`workers'"
            local rc = _rc
            if (`rc' == 0) {
                local rows = r(n_rows)
                local cols = r(n_cols)
            }
        }
        else if "`feature'" == "use" {
            capture noisily parqit use using "`workers'", clear
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_varlist" {
            capture noisily parqit use id year wage using "`workers'", clear
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_filter" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit keep if year == 2022
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit collect, clear
                local rc = _rc
            }
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_range" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit keep in 1000/5999
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit collect, clear
                local rc = _rc
            }
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_sort" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit sort firm_id year id
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit collect, clear
                local rc = _rc
            }
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_random_n" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit sample 5000, count seed(12345)
                local rc = _rc
            }
            if (`rc' == 0) {
                capture noisily parqit collect, clear
                local rc = _rc
            }
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "use_drop" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit drop note
            local rc = _rc
            if (`rc' == 0) capture noisily parqit collect, clear
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "save" {
            capture noisily parqit save "`artifact'", replace data
            local rc = _rc
        }
        else if "`feature'" == "save_varlist" {
            capture noisily keep id year wage
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit save "`artifact'", replace data
                local rc = _rc
            }
        }
        else if "`feature'" == "save_filter" {
            capture noisily parqit keep if year == 2022
            local rc = _rc
            if (`rc' == 0) {
                capture noisily parqit save "`artifact'", replace
                local rc = _rc
            }
        }
        else if "`feature'" == "save_partition_by" {
            local artifact "`outart'/`method'/`feature'_`iter'"
            capture noisily parqit save "`artifact'", replace data partition_by(year)
            local rc = _rc
        }
        else if "`feature'" == "save_chunk" {
            capture noisily parqit save "`artifact'", replace data chunk(8192)
            local rc = _rc
        }
        else if "`feature'" == "save_compression_zstd" {
            capture noisily parqit save "`artifact'", replace data compression(zstd)
            local rc = _rc
        }
        else if "`feature'" == "append" {
            capture noisily parqit use using "`patents'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit append using "`patents'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "append_filter" {
            capture noisily parqit use using "`patents'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit append using "`patents'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit keep if pat_year == 2022
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "merge" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit merge m:1 firm_id using "`firms'", keep(match) nogenerate
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "merge_keepusing" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit merge m:1 firm_id using "`firms'", keep(match) keepusing(tfp) nogenerate
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "use_lazy" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
        }
        else if "`feature'" == "collect" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit collect, clear
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "open_data" {
            capture noisily parqit open _data
            local rc = _rc
        }
        else if "`feature'" == "keep_vars" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit keep id year wage
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "drop_vars" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit drop note
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "keep_if" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit keep if year == 2022
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "drop_if" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit drop if year < 2020
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "keep_in" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit keep in 1000/5999
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "gen" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit keep if wage > 0
            local rc = _rc
            if (`rc' == 0) capture noisily parqit gen double lwage = log(wage)
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "egen_by" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit egen double firm_mean = mean(wage), by(firm_id)
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "replace" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit replace wage = 0 if missing(wage)
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "rename" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit rename wage pay
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "order" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit order wage id year
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "sort" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit sort firm_id year id
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "gsort" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit gsort -wage id
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "collapse" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit collapse (mean) wage (sd) sd_wage = wage (count) n = wage, by(firm_id year)
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "contract" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit contract region sector
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "duplicates_drop" {
            capture noisily parqit use id year firm_id using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit sort id year firm_id
            local rc = _rc
            if (`rc' == 0) capture noisily parqit duplicates drop id year firm_id, force
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "duplicates_report" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit duplicates report id
            local rc = _rc
        }
        else if "`feature'" == "sample_count" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit sample 5000, count seed(12345)
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "reshape_wide" {
            capture noisily parqit use id year wage using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit reshape wide wage, i(id) j(year)
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "reshape_long" {
            capture noisily parqit use using "`wide'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit reshape long wage, i(id) j(year)
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "count" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit count
            local rc = _rc
            if (`rc' == 0) {
                local rows = r(N)
                local cols = 1
            }
        }
        else if "`feature'" == "count_if" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit count if year == 2022
            local rc = _rc
            if (`rc' == 0) {
                local rows = r(N)
                local cols = 1
            }
        }
        else if "`feature'" == "head" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit head 10
            local rc = _rc
        }
        else if "`feature'" == "list" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit list id year wage if year == 2022 in 1/20
            local rc = _rc
        }
        else if "`feature'" == "show" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit show
            local rc = _rc
        }
        else if "`feature'" == "explain" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit keep if year == 2022
            local rc = _rc
            if (`rc' == 0) capture noisily parqit explain
            local rc = _rc
        }
        else if "`feature'" == "sql_clear" {
            capture noisily parqit sql `"SELECT id, year, wage FROM read_parquet('`workers'') WHERE year=2022"', clear
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "sql_save" {
            capture noisily parqit sql `"SELECT id, year, wage FROM read_parquet('`workers'') WHERE year=2022"'
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "query_qualify" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit keep id year wage
            local rc = _rc
            if (`rc' == 0) capture noisily parqit query `"QUALIFY row_number() OVER (PARTITION BY id ORDER BY year) = 1"'
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "summarize" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit summarize wage
            local rc = _rc
        }
        else if "`feature'" == "summarize_detail" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit summarize wage, detail
            local rc = _rc
        }
        else if "`feature'" == "tabulate_oneway" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit tabulate region
            local rc = _rc
        }
        else if "`feature'" == "tabulate_twoway" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit tabulate region sector
            local rc = _rc
        }
        else if "`feature'" == "misstable" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit misstable wage note
            local rc = _rc
        }
        else if "`feature'" == "misstable_patterns" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit misstable patterns wage note
            local rc = _rc
        }
        else if "`feature'" == "levelsof" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit levelsof region
            local rc = _rc
        }
        else if "`feature'" == "ds" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit ds
            local rc = _rc
        }
        else if "`feature'" == "lookfor" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit lookfor wage
            local rc = _rc
        }
        else if "`feature'" == "codebook" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit codebook wage
            local rc = _rc
        }
        else if "`feature'" == "distinct" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit distinct region
            local rc = _rc
        }
        else if "`feature'" == "tabstat" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit tabstat wage, statistics(mean sd min max) by(year)
            local rc = _rc
        }
        else if "`feature'" == "correlate" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit correlate wage hours tenure
            local rc = _rc
        }
        else if "`feature'" == "pwcorr" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit pwcorr wage hours tenure
            local rc = _rc
        }
        else if "`feature'" == "histogram_nodraw" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit histogram wage, bins(20) nodraw
            local rc = _rc
        }
        else if "`feature'" == "joinby" {
            capture noisily parqit use using "`workers'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit joinby firm_id using "`firms'"
            local rc = _rc
            if (`rc' == 0) capture noisily parqit save "`artifact'", replace
            local rc = _rc
        }
        else if "`feature'" == "mergein" {
            capture noisily parqit mergein m:1 firm_id using "`firms'", keepusing(tfp) nogenerate noreport
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else if "`feature'" == "appendin" {
            capture noisily parqit appendin using "`workers'"
            local rc = _rc
            if (`rc' == 0) {
                local rows = _N
                local cols = c(k)
            }
        }
        else local rc = 198
    }
    else local rc = 198

    timer off 1
    quietly timer list 1
    local seconds = r(t1)

    if (`rc' == 0 & missing(`rows')) {
        _feature_dims "`artifact'"
        local rows = r(rows)
        local cols = r(cols)
    }
    di as txt "FEATURE_RESULT feature=`feature' method=`method' iter=`iter' rc=`rc' seconds=" ///
        as res %12.3f `seconds' as txt " rows=" as res %12.0fc `rows' as txt " cols=" as res `cols'
    post `handle' ("`feature'") ("`method'") (`iter') (`seq') (`seconds') (`rc') (`rows') (`cols') ("`artifact'")
    capture parqit close _all
    clear
end

global FS_WORKERS "`workers'"
global FS_WORKERS_SMALL "`workers_small'"
global FS_FIRMS "`firms'"
global FS_PATENTS "`patents'"
global FS_WIDE "`wide'"
global FS_OUTART "`outart'"

local sequence = 0
forvalues i = 1/`reps' {
    foreach feature in $FS_FEATURES {
        local methods "${FS_METHODS_`feature'}"
        foreach method of local methods {
            local ++sequence
            _bench_feature `feature' `method' `i' `sequence' `rawpost'
        }
    }
}
postclose `rawpost'

use "`raw'", clear
gen byte ok = (rc == 0)
gen byte failed = (rc != 0)
order feature method iteration sequence seconds rc ok failed rows cols artifact
save "`outdir'/feature_stata_raw.dta", replace
export delimited using "`outdir'/feature_stata_raw.csv", replace

collapse (count) runs=seconds (sum) failures=failed ///
    (mean) mean_seconds=seconds (sd) sd_seconds=seconds ///
    (min) min_seconds=seconds (p50) p50_seconds=seconds ///
    (max) max_seconds=seconds, by(feature method)
sort feature method
save "`outdir'/feature_stata_summary.dta", replace
export delimited using "`outdir'/feature_stata_summary.csv", replace

log close
'''


def write_stata_do(outdir: Path) -> Path:
    dofile = outdir / "feature_surface_stata.do"
    dofile.write_text(DO_TEMPLATE, encoding="utf-8")
    return dofile


def python_timings(datadir: Path, outdir: Path, reps: int) -> pd.DataFrame:
    rows: list[dict[str, object]] = []
    for iteration in range(1, reps + 1):
        order = FEATURES[iteration - 1 :] + FEATURES[: iteration - 1]
        for sequence, feature in enumerate(order, start=1):
            if "python" not in feature.methods:
                continue
            seconds, rc, nrows, ncols, artifact = py_time_feature(feature.name, datadir, outdir, iteration)
            rows.append(
                {
                    "feature": feature.name,
                    "method": "python",
                    "iteration": iteration,
                    "sequence": sequence,
                    "seconds": seconds,
                    "rc": rc,
                    "ok": rc == 0,
                    "failed": rc != 0,
                    "rows": nrows,
                    "cols": ncols,
                    "artifact": artifact,
                }
            )
    df = pd.DataFrame(rows)
    df.to_csv(outdir / "feature_python_raw.csv", index=False)
    return df


def summarize(outdir: Path) -> tuple[pd.DataFrame, pd.DataFrame]:
    frames = [pd.read_csv(outdir / "feature_python_raw.csv")]
    stata_raw = outdir / "feature_stata_raw.csv"
    if stata_raw.exists():
        frames.append(pd.read_csv(stata_raw))
    raw = pd.concat(frames, ignore_index=True, sort=False)
    raw.to_csv(outdir / "feature_raw.csv", index=False)
    summary = (
        raw.groupby(["feature", "method"], as_index=False)
        .agg(
            runs=("seconds", "count"),
            failures=("failed", "sum"),
            mean_seconds=("seconds", "mean"),
            sd_seconds=("seconds", "std"),
            min_seconds=("seconds", "min"),
            p50_seconds=("seconds", "median"),
            max_seconds=("seconds", "max"),
        )
        .sort_values(["feature", "method"])
    )
    meta = pd.DataFrame([f.__dict__ for f in FEATURES])
    long = summary.merge(meta[["name", "surface", "description"]], left_on="feature", right_on="name", how="left").drop(columns=["name"])
    long.to_csv(outdir / "feature_summary_long.csv", index=False)
    pivot = long.pivot(index=["surface", "feature", "description"], columns="method", values="p50_seconds").reset_index()
    fail_pivot = long.pivot(index=["surface", "feature"], columns="method", values="failures").reset_index()
    pivot.to_csv(outdir / "feature_p50_wide.csv", index=False)
    fail_pivot.to_csv(outdir / "feature_failures_wide.csv", index=False)
    return long, pivot


def write_report(outdir: Path, pivot: pd.DataFrame, reps: int, datadir: Path) -> None:
    show = pivot.copy()
    for col in ["python", "pq", "parqit"]:
        if col not in show.columns:
            show[col] = pd.NA
    for col in ["python", "pq", "parqit"]:
        show[col] = show[col].map(lambda x: "" if pd.isna(x) else f"{x:.3f}")
    common = show[show["surface"] == "pq_parqit_common"]
    parqit_only = show[show["surface"] == "parqit_only"]
    lines = [
        "# pq/parqit full feature-surface timing supplement",
        "",
        f"- Repetitions: {reps}",
        f"- Data: `{datadir}`",
        "- Unit: representative end-to-end workflow for each feature. For lazy parqit verbs, time includes the materializer that executes the plan.",
        "- Python is the canonical implementation: pyarrow for file I/O and DuckDB SQL for relational/statistical workflows.",
        "- Administrative commands without a data-processing equivalent are excluded; `path`, `show`, `explain`, and `glimpse` are included because they have direct metadata/introspection analogues.",
        "",
        "## Common pq/parqit features: p50 seconds",
        "",
        common[["feature", "description", "python", "pq", "parqit"]].to_markdown(index=False),
        "",
        "## parqit-only features: p50 seconds",
        "",
        parqit_only[["feature", "description", "python", "parqit"]].to_markdown(index=False),
        "",
        "## Files",
        "",
        "- `feature_raw.csv`",
        "- `feature_summary_long.csv`",
        "- `feature_p50_wide.csv`",
        "- `feature_surface_stata.log`",
        "",
    ]
    (outdir / "REPORT.md").write_text("\n".join(lines), encoding="utf-8")


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
        "pyarrow": pa.__version__,
    }
    for name, cmd in {
        "git_head": ["git", "rev-parse", "HEAD"],
        "git_status": ["git", "status", "--short"],
        "plugin_sha256": ["sha256sum", str(plugin)],
        "uptime": ["uptime"],
    }.items():
        try:
            info[name] = subprocess.check_output(cmd, cwd=repo, text=True, stderr=subprocess.STDOUT).strip()
        except Exception as exc:
            info[name] = f"unavailable: {exc}"
    (outdir / "environment.json").write_text(json.dumps(info, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--plugin", type=Path)
    parser.add_argument("--source-datadir", type=Path)
    parser.add_argument("--datadir", type=Path)
    parser.add_argument("--outdir", type=Path)
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument("--stata-bin", default="stata-mp")
    parser.add_argument("--skip-stata", action="store_true")
    args = parser.parse_args()

    repo = args.repo.resolve()
    plugin = (args.plugin or repo / "build/dev/parqit.plugin").resolve()
    source_datadir = (args.source_datadir or repo / "benchmarks/_out/synthetic_medium_data").resolve()
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    outdir = (args.outdir or repo / f"benchmarks/_out/feature_surface_{stamp}").resolve()
    datadir = (args.datadir or outdir / "data").resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "artifacts" / "python").mkdir(parents=True, exist_ok=True)
    shutil.copy2(Path(__file__), outdir / "benchmark_feature_surface.py")

    make_data(source_datadir, datadir)
    write_environment(outdir, repo, plugin, datadir, args.reps)
    dofile = write_stata_do(outdir)
    python_timings(datadir, outdir, args.reps)

    if not args.skip_stata:
        feature_names = " ".join(f.name for f in FEATURES)
        env = os.environ.copy()
        # Pass feature/method catalogs through globals to avoid quoting a large
        # matrix through Stata command-line arguments.
        globals_do = outdir / "feature_surface_globals.do"
        lines = [f'global FS_FEATURES "{feature_names}"']
        for feature in FEATURES:
            methods = " ".join(m for m in feature.methods if m != "python")
            lines.append(f'global FS_METHODS_{feature.name} "{methods}"')
        globals_do.write_text("\n".join(lines) + "\n", encoding="utf-8")
        wrapper = outdir / "run_feature_surface.do"
        wrapper.write_text(
            f'do "{globals_do}"\n'
            f'do "{dofile}" "{repo}" "{plugin}" "{datadir}" "{outdir}" "{args.reps}"\n',
            encoding="utf-8",
        )
        run([args.stata_bin, "-b", "do", str(wrapper)], cwd=repo, log=outdir / "stata_process_output.log")

    _, pivot = summarize(outdir)
    write_report(outdir, pivot, args.reps, datadir)
    print(outdir)


if __name__ == "__main__":
    main()
