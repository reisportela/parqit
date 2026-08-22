# A6 — Performance / precision regression audit: released v0.1.27 vs today's build (2026-08-22)

**Verdict: no performance regression and no precision difference between the released
v0.1.27 plugin and today's build on any of the ten workloads, once machine noise is
controlled with a same-compiler control build.** Two small, systematic costs of the new
code paths were measured and are reported below (A6-1 `parqit sql` +7–46 ms per call from
the extra `DESCRIBE`; A6-2 case-clash save path +7–10 % of a 3 s save, of which the footer
rewrite itself is ~5 ms), plus the expected cost of the new ENC-2 transcoding (A6-3), an
engine-level floating-point non-reproducibility that predates today (A6-4), and a build
drift observed during the audit (A6-5). All written files and all collected datasets are
value-identical across installs (A6-6).

Everything lives in `/home/mangelo/Documents/GitHub/parqit/local/audit_2026-08-22/A6/`
(do-files, runner scripts, `results.csv`, `logs/`, fingerprints `fp_*.txt`,
`logs/analysis.md`, `logs/compare.out`). The large Parquet/.dta files were deleted at the end.

## 1. Setup

### 1.1 Installs compared (one fresh `stata-mp -b` process per install × workload × rep)

| tag | what | plugin (sha256 prefix, size) | compiler / flags | ado |
|---|---|---|---|---|
| **B** | BASELINE = `scratchpad/backup_ado_plus_p_0.1.27` (the v0.1.27 release install) | `c8fe1499…`, 43.4 MB, stripped | **GCC 12.2.1 (AlmaLinux 8 gcc-toolset-12) — the CI `linux` preset = Release `-O3 -DNDEBUG`** (strings in the binary: `GCC: (GNU) 12.2.1 … / 8.5.0`) | `*! version 0.1.27 9aug2026`, 138,394 B |
| **N** | NEW = `<repo>/ado/plus/p` as it was at audit start (build/dev 16:10) | `f6efd5fe…`, 41.5 MB, stripped | GCC 11.5.0 (AlmaLinux 9), `dev` preset = RelWithDebInfo `-O2 -g -DNDEBUG` | 15:49, 140,595 B (ENC-2 + NAME-CASE-1) |
| **BD** | CONTROL = v0.1.27 source (`git archive ddc7140` = tag `v0.1.27`) built inside A6 with the **same** `dev` preset, compiler and pinned DuckDB 1.5.3 tarball (`src_0.1.27/build/dev`) | `3e11765e…`, 41.6 MB, stripped | GCC 11.5.0, `-O2 -g -DNDEBUG` | 0.1.27 |
| N2 | the repo tree after the implementer's rebuilds during the audit (plugin `83e5224c…` at 21:13, `4951bb26…` at 21:26) — spot checks only (§4, A6-5) | — | GCC 11.5.0 dev | 21:12, 140,717 B |

Why BD: B and N differ not only in code but in compiler (GCC 12 vs 11), optimisation
(`-O3` vs `-O2 -g`) and libstdc++ (static EL8). B-vs-N therefore conflates build
configuration with today's changes; **N-vs-BD is the apples-to-apples code comparison**,
B-vs-BD isolates the build-configuration effect. All three embed DuckDB v1.5.3 and report
`parqit version 0.1.27`; the install actually used is proven in every log by `which parqit`
and `A6 PLUGIN_PATH:` (the plugin is selected by `adopath ++ "<dir>"` as the first line
after `clear all`/`set more off` **and** `global PARQIT_PLUGIN_PATH` pointing at the same
directory; the plugin is loaded by `parqit version` *before* the timer starts).

### 1.2 Machine and noise

athena, 48 cores, 1 TB RAM (≥ 925 GB available throughout), AlmaLinux 9 (kernel
5.14.0-611), `/home` XFS with 1 TB free, Stata 19.5 (`stata-mp -b`; `c(flavor)` = IC),
python 3.12 + pyarrow 24.0 as the independent oracle. The machine was shared: load average
3.4 at the start, **8–18 during the campaign** (other users' `xstata-mp` jobs at 100–150 %
CPU, other auditors' Stata batch runs). Consequences: ±20 % run-to-run scatter on the
20–30 s workloads and occasional "lucky" single runs 15–20 % faster than the rest of the
same install (visible in the raw values, §2.2). Mitigation: installs alternated
B, N, BD inside every (workload, rep); 5 repetitions for the large workloads (W1, W1z,
W2, W5, W6, W7), 3 for the rest; both **min and median** reported; a ratio is only called
a regression when it is supported by the BD control and by the raw distributions.
`timer` resolution is 10 ms, so steps under ~0.1 s (lazy open/verbs/sort) are compared
pooled and qualitatively.

### 1.3 Method (files in A6/)

* `a6_make_fixtures.do` (run once, BASELINE writer): generates all synthetic data with
  `set seed 1234`; asserts every string cell is valid UTF-8 and within its width; writes
  the read fixtures `w1.parquet` (1.63 GB), `a.parquet`/`b.parquet` (90 MB each) and caches
  the in-memory sources as `.dta` (`w1_src.dta` 3.21 GB, `w8*_src.dta`, `w9*_src.dta`) so
  every save workload starts from byte-identical data without re-generating.
* W1 data: 5,000,000 obs × 82 vars = 60 numeric (12 byte, 12 int, 12 float, 12 double,
  10 long, each with ~5 % missing, + keys `k1` int 1–1000, `k2` long 1–100000), 20 strings
  `s1`–`s20` of type str1–str20 (ASCII + one 2-byte accented char in ~50 %), 2 strL
  (`sl1`, `sl2`, ~80–100 bytes, 5 % empty); variable labels with accents, a value label, a
  note, a data label. W4: `a`/`b` 2,000,000 rows each (`id` 1..2M, 5 doubles, str10; `b`
  shuffled). W7: 20,000 × 2,500 (float/double/long/int cycling). W8: 5M × 30 (5 pairs
  `nuemp/NUEMP`, `ano/ANO`, `sexo/Sexo`, `wage/WAGE`, `idade/IDADE` + 20 others; the
  `noclash` variant renames the second of each pair `nuemp2`…). W9: 5M × 10 str20, every
  cell `"caf" + ustrto("é","latin1",1) + " " + …` (raw 0xE9, `ustrinvalidcnt()>0`) or the
  valid-UTF-8 `"café …"` twin.
* One do-file per workload (`w1.do` … `w10.do`), each taking `<installdir> <tag> <rep>
  [variant]` as `args`; `runone.sh` writes a one-line driver so the batch log gets a clean
  name (`logs/<wl>_<tag>_r<rep><variant>.log`); `run_all.sh` / `run_more.sh` /
  `run_extra.sh` orchestrate; timings append to `results.csv`; `analyze.py` builds the
  table; `compare.py` (pyarrow) compares two Parquet files (schema, row groups, `parqit.*`
  KV metadata, every column's values incl. nulls, floats bitwise with NaN==NaN);
  `a6_fingerprint` (Mata `hash1()` per variable + `datasignature`) fingerprints datasets in
  memory; `a6_cf.do` runs native `cf _all` on 500k-row subsamples.
* Timed: only the parqit command(s) (`timer on/off`), after the plugin is loaded.
  Not timed: data generation/`use`, fingerprints, `describe`.

## 2. Results

### 2.1 Summary table (seconds; min and median over n reps; ratios on min / median)

Verdict rule from the brief: ratio > 1.10 → regression (S3), > 1.25 → serious; applied to
N vs B and, decisively, to N vs BD (same build flags). Raw per-rep values are in §2.2 and
`logs/analysis.md`; the discussion column says why a mechanical flag is or is not a
regression.

| workload | step | B min / med | BD min / med | N min / med | N/B min · med | N/BD min · med | n | assessment |
|---|---|---|---|---|---|---|---|---|
| **W1** save 5M×82 snappy | save | 20.07 / 20.19 | 16.07 / 19.89 | **15.52 / 18.90** | 0.77 · 0.94 | 0.97 · 0.95 | 5 | **no regression** (N fastest) |
| **W1z** save zstd | save | 15.69 / 16.64 | 16.24 / 20.59 | 16.48 / 20.68 | 1.05 · 1.24 | 1.02 · 1.00 | 5 | **no code regression**: BD (v0.1.27 code, N's build flags) reproduces N exactly; B's lower median is a build-configuration effect (CI `-O3`/GCC 12 zstd) and/or quieter runs — see §2.2 |
| **W2** eager read 5M×82 | read | 24.86 / 26.57 | 21.73 / 26.65 | 25.67 / 26.58 | 1.03 · 1.00 | 1.18 · 1.00 | 5 | **no regression**; BD's 21.7 s is a single lucky run (its other four: 25.7–27.0) |
| **W3** lazy use+keep if+collapse+collect | open | 0.070 / 0.076 | 0.070 / 0.074 | 0.081 / 0.099 | 1.16 · 1.30 | 1.16 · 1.34 | 3 | timer-resolution noise on a 70 ms step: the *same* `parqit use using w1.parquet` is faster for N in W5/W6; pooled over W3+W5+W6 (13 runs each): B 0.063/0.076, BD 0.064/0.074, N 0.050/0.081 (×1.07–1.09 med) → **no regression** |
|  | verbs | 0.021 / 0.023 | 0.021 / 0.041 | 0.021 / 0.030 | 1.00 · 1.30 | 1.00 · 0.73 | 3 | noise (20–40 ms) |
|  | collect | 0.291 / 0.300 | 0.294 / 0.316 | 0.263 / 0.300 | 0.90 · 1.00 | 0.89 · 0.95 | 3 | no regression |
|  | **total** | 0.384 / 0.397 | 0.412 / 0.415 | 0.365 / 0.430 | 0.95 · 1.08 | 0.89 · 1.04 | 3 | **no regression** |
| **W4** lazy 1:1 merge 2M+2M + collect | open | 0.035 / 0.037 | 0.036 / 0.039 | 0.037 / 0.038 | 1.06 · 1.03 | 1.03 · 0.97 | 3 | no regression |
|  | merge | 0.133 / 0.135 | 0.141 / 0.148 | 0.134 / 0.144 | 1.01 · 1.07 | 0.95 · 0.97 | 3 | no regression (`prepare_using` alias alignment not measurable) |
|  | collect | 0.647 / 0.660 | 0.653 / 0.655 | 0.639 / 0.642 | 0.99 · 0.97 | 0.98 · 0.98 | 3 | no regression |
|  | **total** | 0.823 / 0.830 | 0.842 / 0.843 | 0.811 / 0.824 | 0.99 · 0.99 | 0.96 · 0.98 | 3 | **no regression** |
| **W5** lazy sort k1 k2 + collect 5M×82 | open | 0.070 / 0.088 | 0.072 / 0.075 | 0.063 / 0.067 | 0.90 · 0.76 | 0.88 · 0.89 | 5 | faster/noise |
|  | sort | 0.009 / 0.014 | 0.008 / 0.009 | 0.009 / 0.009 | 1.00 · 0.64 | 1.13 · 1.00 | 5 | 1 ms — below resolution |
|  | collect | 22.36 / 27.10 | 26.95 / 27.06 | 26.79 / 26.96 | 1.20 · 0.995 | 0.99 · 1.00 | 5 | **no regression**; B's 22.4 s is a single lucky run (its next: 26.8) |
|  | **total** | 22.46 / 27.21 | 27.04 / 27.14 | 26.87 / 27.05 | 1.20 · 0.99 | 0.99 · 1.00 | 5 | **no regression** |
| **W6** view save (keep if k1<=500 → 2.5M×82) | open | 0.063 / 0.074 | 0.064 / 0.070 | 0.050 / 0.081 | 0.79 · 1.10 | 0.78 · 1.16 | 5 | noise (see W3 open) |
|  | verbs | 0.008 / 0.009 | 0.009 / 0.011 | 0.007 / 0.020 | 0.88 · 2.22 | 0.78 · 1.82 | 5 | 7–20 ms — below resolution |
|  | save | 2.447 / 2.487 | 2.498 / 2.602 | 2.581 / 2.605 | 1.05 · 1.05 | 1.03 · 1.00 | 5 | no regression (≤ 1.06) |
|  | **total** | 2.521 / 2.573 | 2.572 / 2.686 | 2.641 / 2.706 | 1.05 · 1.05 | 1.03 · 1.01 | 5 | **no regression** |
| **W7** wide 20k×2,500 | save | 2.330 / 2.366 | 1.760 / 2.333 | 2.076 / 2.358 | 0.89 · 1.00 | 1.18 · 1.01 | 5 | no regression (BD's 1.76 is one lucky run; medians equal) |
|  | read | 1.147 / 1.201 | 0.960 / 1.170 | 0.930 / 1.181 | 0.81 · 0.98 | 0.97 · 1.01 | 5 | no regression (2,500-name handling: no cost) |
|  | **total** | 3.489 / 3.557 | 3.102 / 3.503 | 3.073 / 3.467 | 0.88 · 0.97 | 0.99 · 0.99 | 5 | **no regression** |
| **W8 clash** 5M×30, 5 case-pairs | save | 2.273 / 2.903 | 2.869 / 2.906 | 3.015 / 3.083 | 1.33 · 1.06 | 1.05 · 1.06 | 3 | vs BD ≤ 1.06 → not flagged; B's 2.27 is one lucky run (next 2.90). Intrinsic cost of the new alias/footer path: **+7–10 %** vs N's own no-clash save (paired, §3.2) → **A6-2** |
|  | read | 0.576 / 0.599 | 0.554 / 0.673 | 0.626 / 0.628 | 1.09 · 1.05 | 1.13 · 0.93 | 3 | ≤ 1.09 on medians → no regression (eager read restores exact names) |
|  | **total** | 2.901 / 3.497 | 3.423 / 3.579 | 3.643 / 3.709 | 1.26 · 1.06 | 1.06 · 1.04 | 3 | see A6-2 |
| **W8 noclash** | save | 2.920 / 2.969 | 2.965 / 2.973 | 2.770 / 2.776 | 0.95 · 0.94 | 0.93 · 0.93 | 3 | faster |
|  | read | 0.589 / 0.618 | 0.589 / 0.623 | 0.564 / 0.747 | 0.96 · 1.21 | 0.96 · 1.20 | 3 | median inflated by one 0.89 s run (B and BD also have one 0.94/0.95 run) → noise |
|  | **total** | 3.509 / 3.587 | 3.554 / 3.596 | 3.340 / 3.517 | 0.95 · 0.98 | 0.94 · 0.98 | 3 | **no regression** |
| **W9 legacy** 5M×10 str20, every cell Latin-1 | save | 0.044 / 0.048 (**rc 198, refused**) | 0.054 / 0.056 (rc 198) | 9.248 / 9.355 (rc 0, 50,000,000 cells transcoded) | n/a | n/a | 3 | **new capability**, not comparable: v0.1.27 stops at the first cell → A6-3 |
| **W9 utf8** same shape, valid UTF-8 | save | 7.194 / 7.723 | 7.112 / 7.240 | 7.134 / 7.136 | 0.99 · 0.92 | 1.00 · 0.99 | 3 | **no regression** of the per-cell UTF-8 check |
| **W10** `parqit sql` + collect | sql | 0.027 / 0.028 | 0.027 / 0.028 | 0.033 / 0.036 | 1.22 · 1.29 | 1.22 · 1.29 | 3 | **+6–8 ms per call (the extra DESCRIBE) → A6-1** (S3 by the rule, tiny in absolute terms) |
|  | collect | 0.274 / 0.275 | 0.258 / 0.261 | 0.274 / 0.286 | 1.00 · 1.04 | 1.06 · 1.10 | 3 | no regression |
|  | **total** | 0.301 / 0.303 | 0.286 / 0.289 | 0.307 / 0.322 | 1.02 · 1.06 | 1.07 · 1.11 | 3 | ≤ 1.11, driven by the sql step |

Build-configuration effect (B vs BD, same source): within noise everywhere except W1z
(B's zstd save median 16.6 s vs BD 20.6 s) and W1 (B never produced a fast run: 20.1–21.9 s
vs BD/N 15.5–22.3 s) — neither is a property of today's code. The released binary of
today's code will be built like B (CI `-O3`, GCC 12), so if anything the shipped plugin
should be slightly faster than the N numbers above.

### 2.2 Raw per-rep values (seconds, sorted; from `results.csv`)

```
W1   save    B: 20.066 20.120 20.187 21.633 21.944 | BD: 16.072 16.078 19.894 21.033 22.226 | N: 15.518 18.289 18.901 20.519 20.791
W1z  save    B: 15.685 15.742 16.643 17.630 20.974 | BD: 16.242 16.409 20.591 21.197 22.295 | N: 16.481 19.644 20.675 21.848 22.110
W2   read    B: 24.859 25.637 26.566 26.903 26.934 | BD: 21.729 25.663 26.647 26.809 27.010 | N: 25.668 25.982 26.581 26.831 27.288
W3   open    B: 0.070 0.076 0.079 | BD: 0.070 0.074 0.089 | N: 0.081 0.099 0.103
W3   verbs   B: 0.021 0.023 0.038 | BD: 0.021 0.041 0.044 | N: 0.021 0.030 0.032
W3   collect B: 0.291 0.300 0.306 | BD: 0.294 0.316 0.324 | N: 0.263 0.300 0.301
W4   open    B: 0.035 0.037 0.040 | BD: 0.036 0.039 0.041 | N: 0.037 0.038 0.038
W4   merge   B: 0.133 0.135 0.136 | BD: 0.141 0.148 0.149 | N: 0.134 0.144 0.146
W4   collect B: 0.647 0.660 0.681 | BD: 0.653 0.655 0.691 | N: 0.639 0.642 0.648
W5   open    B: 0.070 0.075 0.088 0.099 0.102 | BD: 0.072 0.074 0.075 0.079 0.088 | N: 0.063 0.066 0.067 0.073 0.092
W5   sort    B: 0.009 0.009 0.014 0.014 0.021 | BD: 0.008 0.009 0.009 0.010 0.015 | N: 0.009 0.009 0.009 0.019 0.020
W5   collect B: 22.355 26.790 27.100 28.004 28.801 | BD: 26.952 26.958 27.060 28.107 28.430 | N: 26.794 26.866 26.961 28.234 29.052
W6   open    B: 0.063 0.066 0.074 0.077 0.079 | BD: 0.064 0.065 0.070 0.073 0.080 | N: 0.050 0.067 0.081 0.084 0.090
W6   verbs   B: 0.008 0.009 0.009 0.011 0.021 | BD: 0.009 0.010 0.011 0.014 0.022 | N: 0.007 0.010 0.020 0.021 0.021
W6   save    B: 2.447 2.464 2.487 2.499 3.128 | BD: 2.498 2.503 2.602 2.617 2.654 | N: 2.581 2.584 2.605 2.629 2.825
W7   save    B: 2.330 2.340 2.366 2.372 2.376 | BD: 1.760 2.142 2.333 2.418 2.436 | N: 2.076 2.286 2.358 2.391 2.454
W7   read    B: 1.147 1.159 1.201 1.217 1.239 | BD: 0.960 1.159 1.170 1.176 1.354 | N: 0.930 0.997 1.181 1.183 1.339
W8clash   save B: 2.273 2.903 2.921 | BD: 2.869 2.906 3.047 | N: 3.015 3.083 3.196
W8clash   read B: 0.576 0.599 0.628 | BD: 0.554 0.673 0.910 | N: 0.626 0.628 0.948
W8noclash save B: 2.920 2.969 3.012 | BD: 2.965 2.973 3.082 | N: 2.770 2.776 2.972
W8noclash read B: 0.589 0.618 0.944 | BD: 0.589 0.623 0.951 | N: 0.564 0.747 0.891
W9legacy  save B: 0.044 0.048 0.051 (rc198) | BD: 0.054 0.056 0.060 (rc198) | N: 9.248 9.355 9.710
W9utf8    save B: 7.194 7.723 7.730 | BD: 7.112 7.240 7.723 | N: 7.134 7.136 7.766
W10  sql     B: 0.027 0.028 0.028 | BD: 0.027 0.028 0.028 | N: 0.033 0.036 0.039
W10  collect B: 0.274 0.275 0.284 | BD: 0.258 0.261 0.267 | N: 0.274 0.286 0.312
```

Output sizes (bytes, identical across installs unless noted): `w1_*.parquet` 1,633,607,261
(snappy, 41 row groups); `w1z_*` 1,402,723,694; `w6_*` 817,679,907; `w7_*` 225,550,979;
`w8clash` B 737,855,407 vs N 737,854,987 (−420 B = the five `_1` suffixes removed from the
5 schema elements and 41×5 `path_in_schema` entries — the footer rewrite touches nothing
else); `w8noclash` 737,855,208; `w9*` 50,776,724.

## 3. Focused follow-ups

### 3.1 `parqit sql` per-call cost (A6-1) — `w10x.do`, 20 consecutive calls in one process

| query | B (ms/call) | BD | N |
|---|---|---|---|
| narrow GROUP BY over `w1.parquet` (82 cols) | 6.9 | 9.7 | 11.0 (+1.4 ms vs BD, +4 ms vs B) |
| `SELECT *` over a 2,500-column Parquet (`w7`) | 40 | 35 | **81** (+46 ms, ×2.3) |

The extra `DESCRIBE <sql>` binds the query a second time (footer read + planning), so the
overhead scales with the schema width; `collect` is unaffected. (Measured on plugin
`83e5224c`/`4951bb26`; the campaign's W10 on `f6efd5fe` shows the same +6–8 ms.)

### 3.2 Case-clash save path (A6-2) — `w8x.do`, clash/no-clash alternated 4× in one process

| install | clash save (s) | no-clash save (s) | paired difference |
|---|---|---|---|
| B | 2.999 3.169 3.202 2.922 | 3.223 3.048 3.178 3.145 | −0.08 avg (clash ≈ no-clash) |
| BD | 3.111 3.007 3.140 3.132 | 3.087 3.091 3.112 3.107 | +0.00 |
| N (`83e5224c`) | 3.013 3.263 3.216 3.411 | 2.726 3.030 3.041 3.057 | **+0.18 … +0.35 in 4/4 rounds (+7–10 %)** |
| N2 (`4951bb26`, 1 rep, separate processes) | 3.140 | 2.845 | +0.30 |

A standalone harness (`harness/rename_bench.cpp`, links `src/engine/parquet_footer.cpp`)
renames the five leaf columns of the real 738 MB `w8clash` file (97 KB footer) back and
forth: **3.4–5.5 ms per rename incl. the post-rename verification**. So the footer rewrite
explains ~5 ms of the ~250 ms; the remainder of the alias path (Arrow scan under alias names
→ COPY → rename → verify → publish) is where the time goes and was not isolated here
(candidates: the verify/publish sequence after `resize_file` on a file that DuckDB has just
written, or DuckDB-side differences when the scan names are the aliases). It is a cost that
only datasets with case-clashing names pay, and B wrote wrong column names for them
(`NUEMP_1`), so it is a cost of correctness rather than a regression; against BD it stays
below the 1.10 gate (1.05/1.06).

### 3.3 Reproducibility of floating-point aggregates (A6-4) — `w3det.do`

Five identical `keep if … ; collapse (mean) (sum) (count) …; collect` runs in one process:
with the default thread count the `double` means (`m_d2`, `m_f1`, `m_d3`) differ between
runs of the **same** install at the ~1e-15 relative level (sums of integers and counts are
exact); with `parqit set threads 1` consecutive runs are bitwise identical and N and B give
bitwise-identical means. This is DuckDB's parallel, non-associative summation — engine
behaviour present in v0.1.27 and today alike (B vs BD differ too), not a regression.

### 3.4 Cross-install precision (A6-6)

* pyarrow `compare.py` (values incl. nulls, bitwise floats, schema, row groups, `parqit.*`
  KV metadata): **IDENTICAL** for `w1` B/N/BD, fixture `w1.parquet` (B) vs `w1_N`, `w1z` B/N,
  `w6` B/N/BD, `w7` B/N, `w8noclash` B/N, `w9utf8` B/N, and `w1`/`w8clash` N vs N2.
  `w8clash` B vs N / BD vs N: all 30 columns value-identical; **only the five names differ**
  (`NUEMP_1 ANO_1 Sexo_1 WAGE_1 IDADE_1` in v0.1.27 — the silently-renamed defect — vs the
  exact `NUEMP ANO Sexo WAGE IDADE` today; v0.1.27 also reads its own file back as
  `NUEMP_1`, label lost).
* `w9legacy` (N): all 5,000,000 cells of every column decode to `"café …"`; note
  `50,000,000 string cell(s) in t1 … t10` transcoded from windows-1252, `r(transcoded_cells)`
  = 50,000,000, `r(encoding)` = windows-1252; v0.1.27/BD refuse with
  `parqit save: t1[1] contains invalid UTF-8 … run -unicode translate- first` (rc 198).
* W2 eager read (5M×82): `datasignature` `5000000:82(56800):1304131303:4185533203` and
  every per-variable `hash1()` identical for B, N, BD, N2 — **and identical to the in-memory
  source before it was ever written** (`fp_w1src_gen.txt`): the round trip is lossless
  (types incl. strL, formats, variable/value labels, note, data label); `cf _all` on the
  first 500,000 rows B vs N and B vs BD: rc 0.
* W3 collapse: identical up to the parallel-summation ulps of §3.3 (integers/counts exact);
  W4 merge (2M rows, 14 vars, `_merge`): identical; W5 sort+collect: identical after an
  order-free total sort (`fp_w5det_*`), and identical to the source `.dta`; W7: identical;
  W8: identical values; W10: identical up to ulps.

## 4. Findings

| id | severity | finding | evidence / repro |
|---|---|---|---|
| **A6-1** | S3 (by the >1.10 rule; ≤ 50 ms absolute) | `parqit sql` costs one extra `DESCRIBE` per call: +6–8 ms on an 82-column source (0.027→0.033–0.036 s, ×1.22–1.29 vs B and BD), **+46 ms (×2.3) on a 2,500-column source**; `collect`/results unchanged. | W10 rows; `w10x.do` (§3.1); code: `cmd_view_sql` `s.query("DESCRIBE " + sql)`. Suggest: derive the names from the existing subquery probe when no case clash is possible, or cache. |
| **A6-2** | S4 / observation (vs BD: 1.05–1.06, below the gate) | Saving a dataset whose names differ only by case takes +7–10 % longer than the same save without the clash (+0.2–0.3 s on a 3 s, 738 MB file), consistently in paired runs; the footer rewrite itself accounts for ~5 ms of it — the rest of the alias path is not yet explained. Correctness cost (v0.1.27 wrote wrong names here), not a regression of an existing path. | §3.2, `w8x.do`, `harness/rename_bench.cpp` |
| **A6-3** | informational | ENC-2 transcoding: an all-legacy 5M×10 str20 dataset (50M bad cells) saves in 9.25–9.36 s vs 7.13 s for its valid-UTF-8 twin (+30 %, ≈ 42 ns per transcoded cell); v0.1.27 refused it in 0.04 s. The valid-UTF-8 path (W1, W9utf8) is **not** slower today (N ≤ B, N ≈ BD). | W9 rows; logs `w9_N_r1legacy.log`, `w9_B_r1legacy.log` |
| **A6-4** | informational (engine, pre-existing) | DuckDB parallel `avg()` is not bitwise reproducible run-to-run (1e-15 relative); single-threaded it is, and then B ≡ N. Worth a sentence in ASSUMPTIONS/help if absent ("floating-point aggregates may differ in the last bits between runs; `parqit set threads 1` for bitwise reproducibility"). | §3.3, `fp_w3det_*` |
| **A6-5** | observation — build drift during the audit | The NEW tree was rebuilt while A6 ran (ado 21:12, plugin `83e5224c` 21:13, `4951bb26` 21:26; `src/plugin/plugin_io.cpp` edited again 21:25). The campaign measured `f6efd5fe` (preserved, read-only, at `~/ado/plus/p`). Spot checks on the rebuilt plugin: W1 20.6 s, W2 25.9 s, W8clash/noclash 3.14/2.85 s, W10 sql 0.033 — same picture; outputs value-identical. **Behaviour change seen in the rebuilt plugin:** `collapse (count) n = k2` + `collect` now yields `n long %12.0g` labelled `(count) k2` (native `collapse` does exactly that), whereas v0.1.27 and `f6efd5fe` yield `n int %8.0g` unlabelled. Not a precision loss (wider type, native parity) but a public-surface change that should be in CHANGELOG/ASSUMPTIONS and was not part of the build this audit was asked to measure. | `fp_w3_N0.txt` (int) vs `fp_w3_N2.txt` (long); `logs/w3_N0_r1.log`, `logs/w3_N2_r1.log`, `logs/native_collapse_type.log` |
| **A6-6** | pass | No precision difference anywhere (§3.4); the only file-level difference is the intended NAME-CASE-1 fix. | `logs/compare.out`, `fp_*.txt`, `logs/a6_cf.log` |

No S0/S1/S2 finding. Nothing in today's ENC-2 / NAME-CASE-1 / `DESCRIBE` / `prepare_using`
changes slows the common paths (save from memory, eager read, lazy verbs, merge, sort, view
save, wide files) beyond noise; the per-cell UTF-8 validation replacement costs nothing
measurable on valid data; name handling on 2,500 columns costs nothing measurable.

## 5. Caveats

* B is the CI release build (GCC 12.2.1, `-O3`, static libstdc++ on AlmaLinux 8); N/BD are
  local `-O2 -g` GCC 11.5 builds. B-vs-N ratios are therefore **not** pure code comparisons;
  the BD control is. Where B-vs-N mechanically exceeds 1.10 (W1z median, W2 min, W5 min,
  W3 open, W8clash min) the BD comparison and the raw values show noise/build effects, as
  annotated in the table.
* Shared machine (load 8–18 during the runs). Min-of-5 and medians are reported; single
  "lucky" runs (B W5 22.4 s, BD W2 21.7 s, BD W7 1.76 s, B W8clash 2.27 s) are identified
  in the raw values and not counted as evidence.
* Sub-100 ms steps are at the 10 ms `timer` resolution; only pooled/paired comparisons
  were used for them.
* The W1 dataset of the brief is 5M × (60 numeric + 20 str + 2 strL); the 60 numerics are
  58 random + the two keys; the strL are named `sl1`/`sl2` (an early draft named them
  `L1`/`L2`, which differ only by case from `l1`/`l2` and would have routed W1 through the
  NAME-CASE-1 alias path — v0.1.27 wrote them as `L1_1`/`L2_1`; fixed before the campaign).

## 6. Artefacts (A6/)

`a6_gen.do` (generators, fingerprint), `a6_make_fixtures.do`, `w1.do … w10.do`, `w3det.do`,
`w5det.do`, `w8x.do`, `w10x.do`, `a6_cf.do`, `native_collapse_type.do`, `runone.sh`,
`run_all.sh`, `run_more.sh`, `run_extra.sh`, `pilot.sh`, `build_bd.sh` (control build),
`compare.py`, `analyze.py`, `harness/rename_bench.cpp`, `results.csv` (every timing),
`logs/` (one log per Stata process, `analysis.md`, `compare.out`, `run_*.out`,
`build_bd.log`), `fp_*.txt` (fingerprints), `src_0.1.27/` (v0.1.27 source + the BD install
in `src_0.1.27/ado/plus/p`; its `build/` tree was deleted). All Parquet/.dta data files
(29.8 GB) were deleted at the end; they are regenerated deterministically by
`a6_make_fixtures.do` + `run_all.sh`.
