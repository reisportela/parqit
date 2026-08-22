# A1 — Type and value fidelity across the Stata↔Parquet boundary (adversarial audit, 2026-08-22)

| | |
|---|---|
| **Target** | `parqit` worktree at v0.1.27 + today's uncommitted ENC-2 / NAME-CASE-1 changes; plugin `build/dev/parqit.plugin` built 2026-08-22 16:10 (install tree `ado/plus/p`) |
| **Environment** | Linux EL9 x86_64 · StataNow MP 19.5 (`stata-mp -b`) · embedded DuckDB 1.5.3 · oracles: pyarrow 24.0.0, python-duckdb 1.4.4, `duckdb` CLI 1.5.0-dev, native Stata on the same data |
| **Method** | 12 self-contained do-files + 1 pyarrow/duckdb oracle script + fixture generator under `local/audit_2026-08-22/A1/` (all artefacts there; nothing tracked was modified). Every do-file prints `PASS/FAIL <id>` lines and a `SUMMARY`; every surprising result was re-run and cross-checked with a second oracle (pyarrow/duckdb on the physical file, or native Stata) |
| **Scope** | memory → `parqit save` → file → `parqit use, clear`; lazy `parqit use using` + `collect`; view `save`; `parqit open _data`; the unchanged-source fast path; both in-memory writers (Arrow default and `PARQIT_SAVE_NOARROW=1`); foreign pyarrow files of every Arrow type; partitioned trees |
| **Tally** | ~395 individual checks in 13 experiment files; **11 findings** (3×S0, 2×S1, 6×S2); regression-from-today: **none** (all defective lines predate 2026-08-22 per `git blame`) |

Severity key: S0 = silent data-integrity hazard (rc 0, no or misleading note); S1 = loud-but-wrong / visibly bad output with rc 0; S2 = documentation/contract gap or type-only drift; S3 = performance.

---

## 1. Findings

### A1-1 · S0 · `%tc` datetimes beyond year ~4253 are written 1 ms early by both in-memory writers (Arrow and staged)

- **Repro**: `e09_tc_precision.do` (checks `E09-TC-5000`, `E09-TC-4300`, `E09-MANY-9999`, `E09-MANY-5000`), cross-checked by `e05_foreign.do` (`E05-FARTS-general`: a pyarrow-written `timestamp[ms]` source re-saved from memory: 3 094 of 10 002 instants change; the lazy `compile_for_save` path of the same source: 0 mismatches) and `e11_noarrow.do` (`E11-FARTC-STAGED`, same 12 489/60 000 mismatches with `PARQIT_SAVE_NOARROW=1`). Physical evidence: `oracle_phys.py` `O03-TC-9999-EXACT-US` — `tc(31dec9999 23:59:59.999)` is stored as epoch-µs `253402300799999008` (8 µs off) instead of `…999000`.
- **Observed**: `gen double tc = tc(01jan5000 12:00:00.001); format tc %tc; parqit save f; parqit use f, clear` → `01jan5000 12:00:00.000`. Over 20 000 random ms instants: year 9999 → 37.8 % come back 1 ms earlier, year 5000 → 24.7 %, year 2400 → 0 %. rc 0, no note. `%tC` and `%td` are NOT affected (`E12-TC-LEAP-EXACT` passes).
- **Expected**: integer-millisecond instants are exact on disk (`timestamp[us]` = ms·1000) and round-trip unchanged.
- **Root cause**: `convert_save_numeric` case `WTs` computes `double us = ms * 1000.0` and `llround(us)`. For |µs| ≥ 2^56 (≈ year 4253) the double spacing is 16 µs and `ms·1000` is not representable; half of the products round *down* (−8 µs), and the reader's `floordiv(us, 1000)` then lands in the previous millisecond. The lazy writer (`epoch_ms(CAST(... AS BIGINT))`) and the direct fast path are exact, so the eager writer disagrees with them on the same data.
- **Regression from today?** No — `git blame`: line introduced 2026-06-24 (DT-001 rewrite).
- **Fix**: `src/plugin/plugin_io.cpp` `convert_save_numeric` (case `WTs`): do the product in integer arithmetic — bound `ms` against ±2^63/1000 (or ±(2^53)), then `int64_t us = static_cast<int64_t>(ms) * 1000;` — keeping the existing DT-001 range refusal. Pin with a verify test that writes year-5000/9999 instants and compares against pyarrow `epoch_us == ms*1000` exactly.

### A1-2 · S0 · The unchanged-source fast path ignores the in-memory row order (`sort`/`gsort` leave `c(changed)==0`)

- **Repro**: `e06_direct.do` (`E06-GSORT-ORDER-PARQIT`, `E06-SUBSET-SORT`); physical order via `oracle_phys.py` (`O06-SORTED-PHYSICAL`, `O06-GSORTED-PHYSICAL`); `probe_changed.do` documents which Stata commands leave `c(changed)` at 0 (`sort`, `gsort`, `order`, `xtset/tsset`, no-op `duplicates drop`, `reshape` no-op).
- **Observed**: `parqit use src.parquet, clear` (ids 1..6) → `gsort -x` (memory order 6 5 4 3 2 1) → `parqit save out.parquet` → pyarrow and `parqit use out.parquet, clear` both give 1 2 3 4 5 6. With `sort key id` the file is physically unsorted (`[1,2,3,4,5,6]`) while `parqit.schema.sortedby = ["key","id"]` claims the sort; parqit's own reload re-sorts from the claim (`sort …, stable`), so only the tie order can differ (`E06-SUBSET-SORT`: memory 6 5 4 3 2 1, file 6 4 5 3 2 1), but every third-party reader sees the source order. Control: after any command that flips `c(changed)` the general writer is used and the order is right (`E06-GSORT-ORDER-GENERAL`).
- **Expected**: the written file has the in-memory observation order (a Stata "save" contract); `sortedby` never claims an order the file does not have.
- **Root cause**: `_parqit_save` (ado) selects `save_data_direct` on nonce + `c(changed)==0` + empty `c(filename)`; `cmd_save_data_direct` then `COPY`s `SELECT … FROM read_parquet(source)` — source physical order — and stamps the current `: sortedby`.
- **Regression from today?** No (fast path dates from v0.1.1x; ASSUMPTIONS #20c).
- **Fix**: `src/ado/p/parqit.ado` `_parqit_save` + `src/plugin/plugin_io.cpp` `cmd_save_data_direct`. Options, from safest: (a) record the load-time `sortedby` and refuse the fast path whenever the current `: sortedby` differs (catches `sort`), **and** add a cheap order fingerprint (e.g. first/last 2 048 values of the first numeric/string variable read through `SF_vdata/SF_sdata` vs the source) to catch `gsort` on a file with empty `sortedby`; (b) when the current `sortedby` is non-empty and differs, `ORDER BY` those keys in the direct `COPY` (still loses tie order — so (a) is preferable); (c) drop the fast path. Either way `sortedby` must only be written when physically true.

### A1-3 · S0 · A `float`/`double`/`%tc` partition key comes back as a **string** variable (`"2020.0"`) on every read path

- **Repro**: `e13_more.do` (`E13-PART-FLOATYEAR-*`, `E13-PART-DBLYEAR-TYPESIG`, `E13-PART-LAZY-FLOATYEAR-TYPESIG`, `E13-PART-FLOATYEAR-LAZYREAD-TYPESIG`), `e08_partition.do` (`E08-PF-CF`), `e12_misc.do` (`E12-PART-dk-*`, `E12-PART-tck-*`); duckdb CLI probe on `e08_f/` confirms DuckDB reads the Hive value as `VARCHAR`; pyarrow sees a dictionary<string> column.
- **Observed**: `gen year = 2019 + mod(_n,3)` (Stata default type **float**) → `parqit save tree, partition_by(year) replace` → `parqit use tree, clear` → `year` is `str6` with values `"2020.0"`, `"2021.0"`, …; same for a `double` key (`0.1` → `str19`), and a `%tc` key (`str19` text timestamps). Lazy `collect` and a lazy view `save … partition_by()` behave identically. Integer keys (`byte/int/long`, `%tm`, `%td` date keys, value-labelled bytes) round-trip correctly; string keys keep leading zeros and special characters (`01`, `a b`, `x=y`, `100%`, `é`, `""`), `.`-missing keys restore as missing. rc 0; the only trace is a "skipping display format … not accepted by Stata" note because `%9.0g` cannot be applied to a string.
- **Expected**: the recorded Stata type (`parqit.schema` says `float`/`double`/fmt `%tc`) is restored — or, at minimum, the save is refused for key types DuckDB cannot round-trip through a Hive path.
- **Root cause**: `source_for` reads with `hive_partitioning = true` and relies on DuckDB's auto-cast, which only recognises BIGINT/DATE/TIMESTAMP-looking strings (`"2020.0"` stays VARCHAR); `plan_columns`' `apply_meta_type` never converts a VARCHAR column to a numeric manifest type.
- **Regression from today?** No (2026-06-23 seed).
- **Fix**: `src/plugin/plugin_io.cpp`: when `parqit.schema` names a partition column with a numeric type, either pass `hive_types = {col: DOUBLE/FLOAT/TIMESTAMP}` (DuckDB `read_parquet` option) in `source_for`/`strict_schema_gate`/lazy `cmd_view_open`, or cast the virtual column to the recorded type in the plan (`cast_sql`) before sizing; alternatively refuse non-integer/non-string/non-date partition keys in `cmd_save_data`/`cmd_view_save` with a clear message. Pin with a verify test covering float/double/%tc/%td/%tm/string keys.

### A1-4 · S1 · A variable named `str` (legal in Stata) is renamed `_str` on every read of a parqit-written file

- **Repro**: `e04_shape_meta.do` (`E04-NAME-STR-SURVIVES`), `e12_misc.do` native probe (`gen int str = 1` → rc 0 in StataNow 19.5; `strL`, `str4`, `byte`, `_se`, … are genuinely refused natively).
- **Observed**: `gen int str = _n; parqit save f; parqit use f, clear` → variable `_str` with `note: column "str" loaded as _str (original name kept in char varname[src_name])`; `cf _all` against the twin fails; scripts referencing `str` break. Same on lazy open (`_str` in `describe`).
- **Expected**: exact name round-trip (`str` is not a Stata reserved word; only `str#`/`strL` are).
- **Root cause**: `src/engine/sanitize.cpp` `kReserved` lists `"str"`.
- **Regression from today?** No (2026-07-02).
- **Fix**: remove `"str"` from `kReserved` (keep `strL` and the `str#` family check); unit test in `tests/unit/test_sanitize.cpp`.

### A1-5 · S1 · Zero-row `partition_by()` save fails with a raw engine error (rc 920)

- **Repro**: `e04_shape_meta.do` (block "zero rows partitioned"): `clear; gen byte g = .; gen double x = .; parqit save tree, partition_by(g) replace` → `IO Error: No files found that match the pattern "…/tree.parqit_txn_…/new/**/*.parquet"`, rc 920 (the verify scan of an empty staged tree). No stale directory is left behind (checked). Loud, so no data loss — but the message is an internal path, not a parqit message, and a 0-row single-file save works (`E04-ZERO-*` all pass).
- **Fix**: `copy_out_parquet` (partitioned branch): treat "COPY wrote 0 rows" as either a documented refusal ("partition_by needs at least one observation") or write/verify an empty tree explicitly.

### A1-6 · S2 · Storage-type drift on round trip, and eager/lazy disagreement, for date/period formats

- **Repro**: `e03_dates.do` (`E03-EAGER-TYPES-INT-TD`, `-BYTE-TD`, `-FLOAT-TC`, `-BYTE-TH`, `E03-LAZY-TYPESIG-VS-EAGER`, `E03-OPENDATA-TYPESIG-VS-EAGER`), `e12_misc.do` (`E12-TS1960-TYPE-PARITY`).
- **Observed** (values identical everywhere, only storage types drift):
  - `int %td` and `byte %td` → `long` on the eager `use, clear` path (typemap: `DATE → Long` unconditionally; `apply_meta_type` only widens), but → `int` on the lazy `collect` and `open _data` paths (range-sized, "period/date formats keep at least int"). So eager and lazy disagree on the same file.
  - `float %tc` (and `long %tc`) → `double` on every path.
  - `byte %th` → `int` (documented: "only a genuine date/period format keeps integer storage at int or wider").
  - A foreign `TIMESTAMP` column whose ms counts fit `long` (instants within ±24.8 days of 1960-01-01) collects as `long` lazily vs `double` eagerly (`ts_ms_sql` uses integer `//`, TIME uses float `/`).
- **Contract**: help §Type mapping promises "a byte comes back byte, a long comes back long … unless the observed values require a wider safe type"; no value here requires it. Memory impact: 2× for `int %td` panels.
- **Fix**: `src/engine/typemap.cpp` `apply_meta_type` (let a recorded `byte/int %td` win when the observed range fits; let `float %tc` stay float when the values are float-exact) and make the lazy overlay in `plugin_view.cpp` `cmd_view_collect_prepare` use the same rule; document whichever is chosen.

### A1-7 · S2 · `floor(x + 0.5)` temporal rounding changes exact odd integers ≥ 2^52 by +1 and reports them as "fractional"

- **Repro**: `e03_dates.do` (`E03-EAGER-TC-2P53M1-EXACT`, `E03-SAVE-FRAC` lists `tc tcs tcF tC` only because row 9 holds 2^53−1), `e12_misc.do` (`E12-TC-2P52-ODD`: `%tC` 2^52+1 → 2^52+2, `r(frac_dates)` names it).
- **Observed**: for integer-valued `x ∈ [2^52, 2^53)` with `x` odd, `x + 0.5` is not representable and rounds to even, so `floor(x+0.5) = x+1`; the value is written +1 and the fractional-date note fires. Only astronomically far counts (≥ 2^52 ms ≈ year 144 000; 2^52 days/periods) — practically unreachable, hence S2.
- **Fix**: `stata_round_unit` in `plugin_io.cpp` and the `floor(ref + 0.5)` SQL in `compile_for_save`: return `d` unchanged when `d == trunc(d)` (or use `nearbyint` with explicit tie handling only when `d` is non-integer).

### A1-8 · S2 · Partition columns move to the end of the variable list on read-back (undocumented)

- **Repro**: `e08_partition.do` (every `E08-P*-TYPESIG` differs from the twin only by order: `id sp h d f pay lz g` for `partition_by(g)`), `e12_misc.do`, `e13_more.do`.
- **Observed**: eager and lazy reads of a parqit-written tree append the partition key(s) after the payload columns (DuckDB's Hive virtual-column placement); `cf _all` (by name) passes; positional code (`order`, `_all` column indexes, `v1-v5` ranges) breaks. Neither README nor help mention the reorder.
- **Fix**: `plan_columns`/`cmd_view_open` can reorder the plan to the `parqit.schema` var order when that metadata is present (it records the original order); otherwise document.

### A1-9 · S2 · Nanosecond timestamps before 1970 within 1 µs below a millisecond boundary land 1 ms *later* than the documented floor

- **Repro**: `e05_foreign.do` (`E05-TEMP-TSNS-NEG-FLOOR`), `e13_more.do` block (f): `timestamp[ns]` −1 ns and −999 ns load as `01jan1970 00:00:00.000`, documented semantics (ASSUMPTIONS #4/#16 "ns truncates toward −∞") give `31dec1969 23:59:59.999`.
- **Root cause**: DuckDB `CAST(TIMESTAMP_NS AS TIMESTAMP)` truncates toward zero for negative values (duckdb CLI confirms); parqit floors afterwards at ms. Sub-µs, pre-1970, ns-resolution data only.
- **Fix**: `typemap.cpp` `DUCKDB_TYPE_TIMESTAMP_NS` cast: use `epoch_ns(x) // 1000` (floor division) instead of `CAST(... AS TIMESTAMP)`; same in `boundary_for`.

### A1-10 · S2 · Value labels not attached to any variable are not written (native `save` keeps them)

- **Repro**: `e07_extmiss_labels.do` (`E07-VL-UNATTACHED-DROPPED`: `label define unattached 1 "orphan"` without `label values` → absent after `parqit use`).
- **Contract gap**: help says `parqit.vallabs` "carries the value-label definitions"; `_parqit_wr_save_request` only serialises labels referenced by a variable. Low impact (a `.dta` keeps orphans), but differs from native semantics and from "every metadatum exactly".
- **Fix**: `src/ado/p/parqit.ado` `_parqit_wr_save_request` — iterate `st_vlexists`/`label dir` instead of attached names (or document).

### A1-11 · S2 · `%tc` instants in years ≥ 4253 are stored with a sub-millisecond error even when parqit reads them back correctly

- Companion of A1-1 for the cases that happen to round *up*: `tc(31dec9999 23:59:59.999)` is stored as `23:59:59.999008` (pyarrow/duckdb-visible; `O03-TC-9999-EXACT-US`). Same fix as A1-1.

---

## 2. Verified correct (documented behaviour confirmed, no defect)

- Extended missings `.a`–`.z` collapse to `.` with the `r(ext_missing)` note on save (all numeric types, all-extended columns keep their type); value-label definitions with `.a`/`.z`, −2147483647, 2147483620, quotes/backticks/`$`/pipes/tabs/UTF-8 texts, notes, characteristics, data label, variable labels round-trip byte-exact (`label save` checksum oracle) — `e07`, `e04`.
- Every Stata storage type at its extremes round-trips bit-exact through eager, lazy, view-save, `open _data` and the staged writer: byte −127/100, int ±32 767/32 740, long ±2 147 483 6xx, float ±maxfloat, FLT_MIN, 0.1, 16 777 217→16 777 216, double 2^53±k, ±maxdouble, 4.9e−324 subnormal, 1/3, 2^63, 1e±300, −0.0 (sign bit verified with a Mata `bufio` probe: preserved for doubles in both directions; Stata itself normalises float −0) — `e01`, `probe_negzero2.do`, `oracle_phys.py`.
- Strings: `""`, single space, trailing spaces, `str20` declared width kept with shorter data, `str2045` full, UTF-8 ending exactly at 2 045 bytes, `strL` 2 046 B / 1 MiB / 5 MiB, newline/CR/quote/pipe/tab/backtick/`$`/backslash, `str244` all-blank; NULL≡"" canonicalisation; ENC-2 (today's change) transcodes Latin-1 cells/labels/notes/chars in both writers with correct counts (`r(transcoded_cells)=7`), widens `str5`→`str10` and `str2045`→`strL` (4 090 B) — `e02`, `e11`.
- Dates: `%td` year 100 … 9999, negative day counts, `td(01jan1970)`, leap day; `%tc` at 0/±1 ms, the 1960→1970 epoch shift ±1 ms, `tc(01jan0100)`, −2^53, 2^53−1 magnitude, near the int64-µs ceiling (`9.2233720368547e15` round-trips; the DT-001 guard refuses `9223372036854775+shift` loudly); fractional `%td/%tc` round per native `round()` with the note (100.5→101, −2.5→−2, −0.4→0, 2.4999→2); `%tC` year-9999 exact; `%tm/%tq/%th/%ty/%tw/%tb` stay INTEGER counts including −2147483647/2147483620/−2147483648/2147483647 and a real `%tbsimple` calendar; `%tg` stays a raw double; out-of-range `%ty` 3e9 / `%td` 1e10 refused loudly; all display formats preserved (`%tdDD/NN/CCYY`, `%-td`, `%tcHH:MM:SS`, `%tcDDmonCCYY_HH:MM:SS.sss`, `%9.0g`, `%12.2fc`, `%21x`, `%10.0gc`, `%-12.3f`, `%9.2e`, `%-20s`, `%~12s`) — `e03`, `oracle_phys.py`.
- Shapes: 0 rows with full metadata (eager/lazy/view-save), 1 var × 1 row, 2 502 vars with UTF-8 labels (order, labels, values, lazy and view-save), `chunk(2048)` over 20 000 rows (≈10 row groups; `keep in` slice exact), Unicode names (`año`, `变量`), 32-char and underscore-leading names, and today's NAME-CASE-1: `abc/ABC/Abc/aBC` + `Str`/`strata` round-trip exactly on eager/lazy/view-save/`open _data`, aliases `ABC_1/Abc_2/aBC_3` usable in lazy `gen`, `partition_by()` refused as documented — `e04`.
- Foreign pyarrow files (eager = lazy = view-save = re-save, type signatures and `cf` exact in every family): int8/16/32/64 (edges → `int`/`long`/`double`; 2^53+1 rounds with the note), uint8/16/32/64 (255→int, 65535→long, 4294967295 and 2^64−1 → double with note), float32 (NaN→`.`, ±Inf→`.` with note, 3.4e38 widens the column to double with note, 1e−45 subnormal kept), float64 (1e308→`.` with note, 9007199254740993→…992), float16 (loads as `float`), decimal(9,2)/(38,10)/(18,0)/(5,1) and decimal256(60,0) (→ double, note), date32/date64, timestamp s/ms/us/ns/us-UTC/ms-Europe/Lisbon (UTC instant kept), INT96, time32 s/ms, time64 us/ns (`%tcHH:MM:SS`), bool→byte, string/large_string/dictionary, NUL-bearing strings (truncated with note), binary/fixed/large_binary dropped with message, list/struct/map dropped (all-nested file refused rc 198 on both paths), null-typed column → all-missing byte, 5-row-group files with an all-null row group with and without footer statistics (F2 sizing → identical `long`/`double`/`strL` plans, min/max exact), varlist subsets/wildcards/order, unknown var rc 111, strict-schema glob refusal with a clear message (`column "a" … is missing from …; pass relaxed`), `relaxed` union — `e05`, `e13`.
- Direct fast path: `order`, `recast`, `xtset` chars, varlist subsets are handled correctly (only the row order, A1-2, is wrong).

## 3. Hypotheses tested (coverage table)

| # | Hypothesis | File / check ids | Result |
|---|---|---|---|
| 1 | byte/int/long/float/double extremes round-trip (eager) | e01 `E01-EAGER-*`, `O01-*` | PASS |
| 2 | same via lazy collect / view save / `open _data` | e01 `E01-LAZY-*`, `E01-VSAVE-*`, `E01-OPENDATA-*` | PASS |
| 3 | staged writer (`PARQIT_SAVE_NOARROW=1`) byte-identical to Arrow writer | e11 `E11-e01/e02-*` | PASS (and identical defects on e03/%tc) |
| 4 | double −0.0 / subnormal / maxdouble / 2^53 neighbourhood | e01, probe_negzero2 | PASS |
| 5 | float −0.0 | probe_negzero2 | Stata normalises natively; parqit faithful — PASS |
| 6 | extended missings → `.` + note, in every type; all-extended columns | e01 `E01-SAVE-EXTNOTE`, e07 `E07-*` | PASS (documented loss) |
| 7 | value labels incl. `.a/.z` keys, min/max keys, hostile text | e07 `E07-VL-*` | PASS |
| 8 | unattached value labels written | e07 `E07-VL-UNATTACHED-DROPPED` | **FAIL → A1-10 (S2)** |
| 9 | notes/chars/data label/varlabels round-trip | e07, e04 | PASS |
| 10 | strings: "", " ", trailing spaces, str# width kept, 2045 boundary, UTF-8 at 2045 | e02 `E02-EAGER-*`, `O02-*` | PASS |
| 11 | strL 2046 B / 1 MiB / 5 MiB, newline/quote/pipe/tab/backtick/$ | e02, `O02-STRL-LENGTHS` | PASS |
| 12 | ENC-2 legacy-text transcoding (today): cells, strL, wide→strL, labels, notes, chars, both writers | e02 `E02-ENC-*`, e11 `E11-ENC-*` | PASS |
| 13 | `%td` year 100 / 9999 / negative / far-BC / leap day; formats kept | e03 `E03-EAGER-TD-*`, `O03-TD-*` | PASS |
| 14 | `%tc` ms precision near epoch shift and ±2^53, `%tcHH:MM:SS`, `%tC` | e03 `E03-EAGER-TC-*`, `O03-TC-EPOCH` | PASS |
| 15 | `%tc` far-future (≥ year 4253) exact on disk and on read-back | e09, e05 `E05-FARTS-*`, e11, `O03-TC-9999-EXACT-US` | **FAIL → A1-1 (S0), A1-11** |
| 16 | DT-001 int64-µs ceiling guard; `%ty`/`%td` out-of-range loud | e03 `E03-LOUD-*`, `E03-TC-NEARCEIL-ROUNDTRIP` | PASS |
| 17 | fractional `%td/%tc` rounding per native `round()` + note | e03 `E03-EAGER-TDFRAC/TCFRAC/TCNEG`, `O03-FRAC-ROUNDED` | PASS |
| 18 | exact integers ≥ 2^52 untouched by the rounding rule | e03 `E03-EAGER-TC-2P53M1-EXACT`, e12 `E12-TC-2P52-ODD` | **FAIL → A1-7 (S2)** |
| 19 | `%tm/%tq/%th/%ty/%tw/%tb` stay INTEGER counts incl. negative/large; `%tg` raw | e03 `E03-EAGER-PERIODS`, `O03-TM/TY-INT32` | PASS |
| 20 | storage type of `int/byte %td`, `float %tc`, `byte %th` preserved; eager = lazy | e03 `E03-EAGER-TYPES-*`, `E03-LAZY-TYPESIG-VS-EAGER` | **FAIL → A1-6 (S2)** |
| 21 | all display formats preserved | e03 `E03-EAGER-FORMATS`, `O03-META-FMTS` | PASS |
| 22 | 0-row dataset with full metadata (eager/lazy/view-save) | e04 `E04-ZERO-*` | PASS |
| 23 | 0-row `partition_by` save | e04 block | **FAIL (rc 920 raw message) → A1-5 (S1)** |
| 24 | 1 var × 1 row; 2 502 vars; `chunk(2048)` row groups; `keep in` | e04 `E04-ONE*`, `E04-WIDE-*`, `E04-CHUNK-*` | PASS |
| 25 | Unicode / 32-char / underscore names | e04 `E04-NAMES-*` | PASS |
| 26 | NAME-CASE-1 (today): case-distinct names exact on every path, aliases usable, partition refused | e04 `E04-NAMES-*` | PASS |
| 27 | legal name `str` survives | e04 `E04-NAME-STR-SURVIVES`, e12 native probe | **FAIL → A1-4 (S1)** |
| 28 | `c(changed)==0`, `c(filename)==""` after load | e04 `E04-CHANGED-ZERO` | PASS (documented) |
| 29 | fast path: `sort` then save — physical order and `sortedby` | e06 `E06-SORT-*`, `O06-SORTED-PHYSICAL` | **FAIL → A1-2 (S0)** |
| 30 | fast path: `gsort` then save | e06 `E06-GSORT-ORDER-PARQIT`, `O06-GSORTED-PHYSICAL` | **FAIL → A1-2 (S0)** |
| 31 | fast path: `order`, `recast`, `xtset`, varlist subset | e06 `E06-ORDER-*`, `E06-RECAST*`, `E06-XTSET-CHAR`, `E06-SUBSET-COLS` | PASS |
| 32 | general path after `gsort` (control) | e06 `E06-GSORT-ORDER-GENERAL` | PASS |
| 33 | partition_by string key: leading zeros, numeric-looking, empty, special chars | e08 `E08-PG-*`, `E08-PSP-CF`, `E08-PLZ-CF` | PASS (values) |
| 34 | partition_by int / %tm / labelled byte / %td / missing key | e08 `E08-PH-CF`, `E08-PD-CF`, e12 `E12-PART-tmk/bk-*`, e13 `E13-PART-INTYEAR-*` | PASS |
| 35 | partition_by float / double / %tc key | e08 `E08-PF-CF`, e12 `E12-PART-dk/tck-*`, e13 `E13-PART-*YEAR-*` | **FAIL → A1-3 (S0)** |
| 36 | partition read keeps column order | e08 `E08-P*-TYPESIG` | **FAIL → A1-8 (S2)** |
| 37 | foreign pyarrow Hive tree with string keys `01/02/10` | e08 `E08-FHIVE-*` | PASS |
| 38 | foreign int8…uint64 edges (types + values + >2^53 note) | e05 `E05-INTS-*` | PASS |
| 39 | foreign float32/float64 NaN/±Inf/3.4e38/1e308/subnormal; float16 | e05 `E05-F32*/F64*`, e13 `E13-F32-*`, `E05-F16-*` | PASS |
| 40 | foreign decimal(9,2)/(38,10)/(18,0)/(5,1)/decimal256 | e05 `E05-DEC*` | PASS |
| 41 | foreign date32/date64/timestamp s-ms-us-ns/tz/INT96/time32/time64 | e05 `E05-TEMP-*`, `E05-INT96*` | PASS except #42 |
| 42 | ns pre-1970 sub-µs floor semantics as documented | e05 `E05-TEMP-TSNS-NEG-FLOOR` | **FAIL → A1-9 (S2)** |
| 43 | foreign strings: utf8/large/dictionary/NUL; binary dropped; nested dropped / all-nested refused; null column; bool | e05 `E05-STR-*`, `E05-BIN-*`, `E05-NESTED-*`, `E05-NULL-*`, `E05-BOOL-*` | PASS |
| 44 | F2 footer-stats sizing: 5 row groups incl. all-null group, with/without stats | e05 `E05-RG*`, e13 `E13-RG-*` | PASS |
| 45 | eager = lazy = view-save = re-save for every foreign family | e05 `E05-*-LAZY/VSAVE/RESAVE-*` | PASS |
| 46 | eager/lazy storage parity for TIMESTAMP near 1960 | e12 `E12-TS1960-TYPE-PARITY` | **FAIL (type only) → A1-6** |
| 47 | varlist subsets / wildcards / unknown var / strict glob / relaxed | e05 `E05-SUBSET-*`, e13 `E13-STRICT-*` | PASS |
| 48 | lazy `compile_for_save` of far-future `%tc` exact | e05 `E05-FARTS-lazy` | PASS |

(Residual FAIL lines in `e05_foreign.log` for `E05-F32-VALUES`, `E05-RG*-VALUES`, `E05-STRICT-SCHEMA-LOUD`, and in `oracle_phys.py` for `O01-F-NEGZERO`, `O02-META-TYPES`, were traced to oracle/fixture mistakes — `float(3.4e38)` is `.` in Stata, `2044*"b"+"é"` is 2 046 bytes, `/*` opened a comment — and re-verified as PASS in `e13_more.do`/`probe_negzero2.do`; they are not defects.)

## 4. Artefacts

`/home/mangelo/Documents/GitHub/parqit/local/audit_2026-08-22/A1/`: `prelude.doh` (shared checkers), `gen_foreign.py` (pyarrow fixtures `f_*.parquet`, `f_hive/`), `e01_numeric.do`, `e02_strings.do`, `e03_dates.do`, `e04_shape_meta.do`, `e05_foreign.do`, `e06_direct.do`, `e07_extmiss_labels.do`, `e08_partition.do`, `e09_tc_precision.do`, `e11_noarrow.do` (run with `PARQIT_SAVE_NOARROW=1`), `e12_misc.do`, `e13_more.do`, `probe_changed.do`, `probe_names.do`, `probe_negzero2.do`, `probe_enclab.do`, `oracle_phys.py`, and the matching `*.log` files plus every Parquet/dta written during the runs (≈17 MB).
