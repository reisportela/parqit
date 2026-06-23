#!/usr/bin/env python3
"""Create deterministic synthetic Parquet data for parqit performance work.

The examples/ datasets stay deliberately tiny for feature and precision tests.
This script creates larger analogues with the same broad schema and
relationships, sized to make repeated Stata/DuckDB timings distinguishable from
machine noise without producing multi-GB artifacts.
"""

from __future__ import annotations

import argparse
import json
from decimal import Decimal
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq


DEFAULT_OUTPUT = Path("benchmarks/_out/synthetic_medium_data")
DEFAULT_SEED = 20260613
DEFAULT_ROW_GROUP_SIZE = 65_536


def date32_array(days_since_epoch: np.ndarray) -> pa.Array:
    return pa.array(days_since_epoch.astype(np.int32), type=pa.date32())


def string_take(values: list[str], codes: np.ndarray) -> pa.Array:
    dictionary = np.asarray(values, dtype=object)
    return pa.array(dictionary[codes])


def masked_float(values: np.ndarray, missing: np.ndarray) -> pa.Array:
    clean = values.astype(np.float64, copy=True)
    clean[missing] = 0.0
    return pa.array(clean, mask=missing, type=pa.float64())


def write_table(
    table: pa.Table,
    path: Path,
    *,
    compression: str,
    row_group_size: int,
) -> dict[str, Any]:
    pq.write_table(
        table,
        path,
        compression=compression,
        row_group_size=row_group_size,
        use_dictionary=True,
    )
    meta = pq.ParquetFile(path).metadata
    return {
        "path": str(path),
        "rows": meta.num_rows,
        "columns": meta.num_columns,
        "row_groups": meta.num_row_groups,
        "size_bytes": path.stat().st_size,
    }


def make_workers(
    rng: np.random.Generator,
    *,
    n_workers: int,
    n_firms: int,
    years: np.ndarray,
) -> pa.Table:
    n_years = len(years)
    worker_id = np.repeat(np.arange(1, n_workers + 1, dtype=np.int64), n_years)
    year = np.tile(years.astype(np.int32), n_workers)

    base_wage = rng.lognormal(mean=2.0, sigma=0.55, size=n_workers) * 8.0
    base_age = rng.integers(19, 62, size=n_workers, dtype=np.int32)
    base_firm = rng.integers(1, n_firms + 1, size=n_workers, dtype=np.int64)
    gender_code = rng.integers(0, 2, size=n_workers, dtype=np.int8)
    educ_code = rng.choice(np.array([1, 2, 2, 3, 3, 3, 4], dtype=np.int32), size=n_workers)
    region_code = rng.integers(0, 5, size=n_workers, dtype=np.int8)
    sector_code = rng.integers(0, 4, size=n_workers, dtype=np.int8)
    hire_days = 10_957 + rng.integers(0, 8_000, size=n_workers, dtype=np.int32)

    age = np.repeat(base_age, n_years) + (year - years[0])
    firm_id = np.repeat(base_firm, n_years)
    trend = 1.0 + 0.035 * (year - years[0])
    shock = rng.lognormal(mean=0.0, sigma=0.08, size=worker_id.size)
    wage_values = np.round(np.repeat(base_wage, n_years) * trend * shock, 2)
    wage_missing = rng.random(worker_id.size) < 0.055
    hours = rng.choice(np.array([20, 35, 40, 40, 40, 44], dtype=np.int32), size=worker_id.size)
    tenure = np.maximum(0, ((year - 1970) * 365 - np.repeat(hire_days, n_years)) // 365).astype(np.int32)
    note_code = ((worker_id + year) % 16).astype(np.int16)

    return pa.table(
        {
            "id": pa.array(worker_id, type=pa.int64()),
            "year": pa.array(year, type=pa.int32()),
            "wage": masked_float(wage_values, wage_missing),
            "age": pa.array(age, type=pa.int32()),
            "gender": string_take(["F", "M"], np.repeat(gender_code, n_years)),
            "education": pa.array(np.repeat(educ_code, n_years), type=pa.int32()),
            "region": string_take(
                ["norte", "centro", "lisboa", "alentejo", "algarve"],
                np.repeat(region_code, n_years),
            ),
            "sector": string_take(
                ["manuf", "serv", "constr", "agri"],
                np.repeat(sector_code, n_years),
            ),
            "firm_id": pa.array(firm_id, type=pa.int64()),
            "hours": pa.array(hours, type=pa.int32()),
            "tenure": pa.array(tenure, type=pa.int32()),
            "hire_date": date32_array(np.repeat(hire_days, n_years)),
            "note": string_take(
                ["", "obs_a", "obs_b", "obs_c", "obs_d", "obs_e", "obs_f", "obs_g",
                 "obs_h", "obs_i", "obs_j", "obs_k", "obs_l", "obs_m", "obs_n", "obs_o"],
                note_code,
            ),
        }
    )


def make_firms(rng: np.random.Generator, *, n_firms: int) -> pa.Table:
    firm_id = np.arange(1, n_firms + 1, dtype=np.int64)
    industry_code = rng.integers(0, 3, size=n_firms, dtype=np.int8)
    return pa.table(
        {
            "firm_id": pa.array(firm_id, type=pa.int64()),
            "tfp": pa.array(np.round(rng.lognormal(0.0, 0.4, n_firms), 4), type=pa.float64()),
            "capital": pa.array(np.round(rng.uniform(50.0, 5_000.0, n_firms), 1), type=pa.float64()),
            "industry": string_take(["A", "B", "C"], industry_code),
        }
    )


def make_patents(
    rng: np.random.Generator,
    *,
    n_firms: int,
    years: np.ndarray,
) -> pa.Table:
    counts = rng.integers(0, 5, size=n_firms, dtype=np.int16)
    firm_id = np.repeat(np.arange(1, n_firms + 1, dtype=np.int64), counts)
    total = int(firm_id.size)
    offsets = np.repeat(np.cumsum(counts, dtype=np.int64) - counts, counts)
    patent_no = np.arange(total, dtype=np.int64) - offsets
    patent_year = rng.choice(years.astype(np.int32), size=total)
    patent = [f"PT-{firm:06d}-{num:02d}" for firm, num in zip(firm_id, patent_no)]
    return pa.table(
        {
            "firm_id": pa.array(firm_id, type=pa.int64()),
            "pat_year": pa.array(patent_year, type=pa.int32()),
            "patent": pa.array(patent),
        }
    )


def make_wide_income(
    rng: np.random.Generator,
    *,
    n_persons: int,
    years: np.ndarray,
) -> pa.Table:
    pid = np.arange(1, n_persons + 1, dtype=np.int64)
    grp_code = rng.integers(0, 2, size=n_persons, dtype=np.int8)
    cols: dict[str, pa.Array] = {
        "pid": pa.array(pid, type=pa.int64()),
        "grp": string_take(["x", "y"], grp_code),
    }
    base_income = rng.lognormal(mean=10.0, sigma=0.35, size=n_persons)
    for pos, year in enumerate(years):
        values = np.round(base_income * (1.0 + 0.025 * pos) * rng.lognormal(0.0, 0.05, n_persons), 0)
        missing = rng.random(n_persons) < 0.04
        cols[f"inc{int(year)}"] = masked_float(values, missing)
    return pa.table(cols)


def make_messy(rng: np.random.Generator, *, n_rows: int) -> pa.Table:
    seq = np.arange(n_rows, dtype=np.int64)
    cents = ((seq * 37) % 900_000) - 100_000
    money = [Decimal(int(v)).scaleb(-2) for v in cents]
    table = pa.Table.from_arrays(
        [
            pa.array((seq % 1_000_000).astype(np.int32), type=pa.int32()),
            pa.array(((seq * 10) % 1_000_000).astype(np.int32), type=pa.int32()),
            pa.array(((seq * 65_537) % (2**32)).astype(np.uint32), type=pa.uint32()),
            pa.nulls(n_rows),
            pa.array(((seq * 3) % 2_000_000).astype(np.int32), type=pa.int32()),
            pa.array(((seq * 7 + 11) % 2_000_000).astype(np.int32), type=pa.int32()),
        ],
        names=["if", "x y", "u32", "allnull", "dup", "dup"],
    )
    return table.append_column("money", pa.array(money, type=pa.decimal128(10, 2)))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outdir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--workers", type=int, default=2_000_000)
    parser.add_argument("--worker-years", type=int, default=5)
    parser.add_argument("--firms", type=int, default=500_000)
    parser.add_argument("--wide-persons", type=int, default=1_500_000)
    parser.add_argument("--wide-years", type=int, default=8)
    parser.add_argument("--messy-rows", type=int, default=750_000)
    parser.add_argument("--row-group-size", type=int, default=DEFAULT_ROW_GROUP_SIZE)
    parser.add_argument("--compression", default="zstd")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.workers <= 0 or args.worker_years <= 0 or args.firms <= 0:
        raise SystemExit("workers, worker-years and firms must be positive")
    if args.wide_persons <= 0 or args.wide_years <= 0 or args.messy_rows <= 0:
        raise SystemExit("wide-persons, wide-years and messy-rows must be positive")

    outdir = args.outdir.resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    worker_years = np.arange(2018, 2018 + args.worker_years, dtype=np.int32)
    wide_years = np.arange(2016, 2016 + args.wide_years, dtype=np.int32)

    manifest: dict[str, Any] = {
        "purpose": "medium synthetic benchmark analogues of examples/make_data.py",
        "seed": args.seed,
        "compression": args.compression,
        "row_group_size": args.row_group_size,
        "parameters": {
            "workers": args.workers,
            "worker_years": args.worker_years,
            "firms": args.firms,
            "wide_persons": args.wide_persons,
            "wide_years": args.wide_years,
            "messy_rows": args.messy_rows,
        },
        "datasets": {},
    }

    datasets = [
        ("workers_perf.parquet", make_workers(rng, n_workers=args.workers, n_firms=args.firms, years=worker_years)),
        ("firms_perf.parquet", make_firms(rng, n_firms=args.firms)),
        ("patents_perf.parquet", make_patents(rng, n_firms=args.firms, years=worker_years)),
        ("wide_income_perf.parquet", make_wide_income(rng, n_persons=args.wide_persons, years=wide_years)),
        ("messy_perf.parquet", make_messy(rng, n_rows=args.messy_rows)),
    ]

    for name, table in datasets:
        meta = write_table(
            table,
            outdir / name,
            compression=args.compression,
            row_group_size=args.row_group_size,
        )
        manifest["datasets"][name] = meta
        print(
            f"{name}: rows={meta['rows']:,} cols={meta['columns']} "
            f"row_groups={meta['row_groups']} size={meta['size_bytes'] / 1048576:.1f} MiB"
        )

    manifest_path = outdir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"manifest: {manifest_path}")


if __name__ == "__main__":
    main()
