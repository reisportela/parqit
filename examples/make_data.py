#!/usr/bin/env python3
"""parqit tour — artificial datasets (small, deterministic, pyarrow only).

Usage: python3 make_data.py <output_dir>

Creates:
  workers.parquet   worker-year panel: ids, wages (with missings), strings,
                    dates, categorical-ish columns
  firms.parquet     one row per firm: tfp, capital, industry
  patents.parquet   several patents per firm (for joinby)
  wide_income.parquet  one row per person, inc2018..inc2021 (for reshape)
  messy.parquet     hostile schema: reserved/space/duplicate column names,
                    uint32 beyond 2^31, decimal128, a NULL-typed column
"""
import math
import random
import sys
from datetime import date, timedelta

import pyarrow as pa
import pyarrow.parquet as pq


def main(outdir: str) -> None:
    rng = random.Random(20260612)

    # ---------------- workers: 600 rows, 200 workers × 3 years -----------
    n_workers, years = 200, [2019, 2020, 2021]
    ids, yrs, wages, ages, genders, educ, region, sector = [], [], [], [], [], [], [], []
    firm_id, hours, tenure, hire, note = [], [], [], [], []
    for i in range(1, n_workers + 1):
        base_wage = math.exp(rng.gauss(2.0, 0.6)) * 8
        age0 = rng.randint(19, 62)
        g = rng.choice(["F", "M"])
        ed = rng.choice([1, 2, 2, 3, 3, 3, 4])
        reg = rng.choice(["norte", "centro", "lisboa", "alentejo", "algarve"])
        sec = rng.choice(["manuf", "serv", "constr", "agri"])
        f = rng.randint(1, 40)
        h0 = date(2000, 1, 1) + timedelta(days=rng.randint(0, 7000))
        for k, y in enumerate(years):
            ids.append(i)
            yrs.append(y)
            w = base_wage * (1 + 0.04 * k) * math.exp(rng.gauss(0, 0.08))
            wages.append(None if rng.random() < 0.06 else round(w, 2))
            ages.append(age0 + k)
            genders.append(g)
            educ.append(ed)
            region.append(reg)
            sector.append(sec)
            firm_id.append(f)
            hours.append(rng.choice([20, 35, 40, 40, 40, 44]))
            tenure.append(max(0, (date(y, 12, 31) - h0).days // 365))
            hire.append(h0)
            note.append("" if rng.random() < 0.15 else f"obs {i}-{y}")
    workers = pa.table({
        "id": pa.array(ids, pa.int64()),
        "year": pa.array(yrs, pa.int32()),
        "wage": pa.array(wages, pa.float64()),
        "age": pa.array(ages, pa.int32()),
        "gender": pa.array(genders),
        "education": pa.array(educ, pa.int32()),
        "region": pa.array(region),
        "sector": pa.array(sector),
        "firm_id": pa.array(firm_id, pa.int64()),
        "hours": pa.array(hours, pa.int32()),
        "tenure": pa.array(tenure, pa.int32()),
        "hire_date": pa.array(hire, pa.date32()),
        "note": pa.array(note),
    })
    pq.write_table(workers, f"{outdir}/workers.parquet")

    # ---------------- firms: 40 rows ------------------------------------
    fids = list(range(1, 41))
    firms = pa.table({
        "firm_id": pa.array(fids, pa.int64()),
        "tfp": pa.array([round(math.exp(rng.gauss(0, 0.4)), 4) for _ in fids]),
        "capital": pa.array([round(rng.uniform(50, 5000), 1) for _ in fids]),
        "industry": pa.array([rng.choice(["A", "B", "C"]) for _ in fids]),
    })
    pq.write_table(firms, f"{outdir}/firms.parquet")

    # ---------------- patents: 0-4 per firm ------------------------------
    pf, pyr, ptxt = [], [], []
    for f in fids:
        for p in range(rng.randint(0, 4)):
            pf.append(f)
            pyr.append(rng.choice(years))
            ptxt.append(f"PT-{f:03d}-{p}")
    patents = pa.table({
        "firm_id": pa.array(pf, pa.int64()),
        "pat_year": pa.array(pyr, pa.int32()),
        "patent": pa.array(ptxt),
    })
    pq.write_table(patents, f"{outdir}/patents.parquet")

    # ---------------- wide incomes: 50 persons --------------------------
    pid = list(range(1, 51))
    cols = {"pid": pa.array(pid, pa.int64()),
            "grp": pa.array([rng.choice(["x", "y"]) for _ in pid])}
    for y in [2018, 2019, 2020, 2021]:
        cols[f"inc{y}"] = pa.array(
            [None if rng.random() < 0.05 else round(rng.uniform(8, 60) * 1000, 0)
             for _ in pid])
    pq.write_table(pa.table(cols), f"{outdir}/wide_income.parquet")

    # ---------------- messy: hostile schema -----------------------------
    messy = pa.Table.from_arrays(
        [
            pa.array([1, 2, 3], pa.int32()),                 # reserved name
            pa.array([10, 20, 30], pa.int32()),              # space in name
            pa.array([0, 2**31, 2**32 - 1], pa.uint32()),    # beyond int32
            pa.array([None, None, None], pa.null()),         # null-typed
            pa.array([100, 200, 300], pa.int32()),           # dup 1
            pa.array([7, 8, 9], pa.int32()),                 # dup 2
        ],
        names=["if", "x y", "u32", "allnull", "dup", "dup"],
    )
    import decimal
    messy = messy.append_column(
        "money", pa.array([decimal.Decimal("12.34"), decimal.Decimal("-0.5"),
                           None], pa.decimal128(10, 2)))
    pq.write_table(messy, f"{outdir}/messy.parquet")

    print("parqit tour data written to", outdir)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
