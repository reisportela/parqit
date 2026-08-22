# A4 — Atomicity, error paths, concurrency, I/O and resource edges

Adversarial audit of `parqit` (tree at 2026-08-22, build in `ado/plus/p`, HEAD `ddc7140` v0.1.27 plus uncommitted working-tree changes).
All experiments live under `local/audit_2026-08-22/A4/` (do-files `p0*.do`, shell drivers `p04_concurrency.sh`, `p06_crash.sh`, `p08b_run.sh`, `p09_partial.sh`, oracle helper `a4oracle.py`, logs `*.log`/`*.out`). Oracles: pyarrow 24 (Stata's own Python and system python3), `duckdb` CLI, md5sum, directory listings, native Stata (`datasignature`, `c(changed)`, frames).

Nothing tracked was modified; no git state was touched; large synthetic files (20M-row and 2.4 GB fixtures) were deleted after use.

## Findings

### A4-1 — S0 — The unchanged-source direct save writes the SOURCE FILE, not memory, after `sort`/`gsort` and after Mata writes (`c(changed)` stays 0)

The fast path (`_parqit_save` → `cmd_save_data_direct`) runs `COPY (SELECT … FROM read_parquet(source))` whenever the dataset was loaded by `parqit use <one file>, clear`, `c(changed)==0`, `c(filename)==""`, the nonce matches and the source fingerprint (abs path, size, mtime, row count) matches. Stata does **not** set `c(changed)` for `sort`, `gsort`, `order`, `tsset`, `label drop`, `datasignature`, nor for Mata `st_store()`/`st_sstore()`/`st_view()` writes (probe `p00_changed_probe.log`, lines `CHANGED-PROBE: [sort y] … changed=0`, `[gsort -y] … changed=0`, and `p01_fastpath.log` `after mata st_store: changed=0 x[1]=999999`). So the file written can differ from memory in row order or in values, with rc 0 and no message.

Repro (`p01_fastpath.do`, `p01c_fastpath3.do`, `p07_misc.do`, `p08c_misc3.do`; oracle = pyarrow read vs `sfi.Data`):

```
parqit use fp/f2.parquet, clear        // x is a permutation of 0..2002
sort x                                  // c(changed)==0, sortedby=x
parqit save fp/g30.parquet, replace     // rc 0
```
- `FAIL FP-30 sort x then save: id: 1999 differing rows; first at row 1: file=1 mem=1723; x: … file=1910.0 mem=1.0` — the file is in the source's physical order; duckdb CLI `SELECT x … LIMIT 3` → `1910.0 1817.0 1724.0` while memory holds `1,2,3`.
- `FAIL FP-30b`: the written `parqit.schema` says `"sortedby":["x"]` but the rows are not sorted by x — third-party readers get a false sort claim. A parqit reload re-sorts on load (ASSUMPTIONS #79) and *re-propagates* the lie: `M7-1 h.parquet physically sorted by x? False | sortedby = ['x']`.
- `gsort -id` (FP-02, FP-08 via `frame copy`, FP-19 with `data` and a view open, FP-27, M7-4b to /tmp, RS-3b with 2,500 columns): every save keeps the source order, memory is descending.
- `mata: st_store(1,"x",999999)` (FP-06a), `st_view` write (FP-06b), `st_sstore(2,"s","ZZZ")` (FP-06c), `st_store((1,10),"x",J(10,1,-1))` (M8-3f): file has the **old** values, rc 0, no note.

Not triggered (correct, verified): `order` (column order follows the manifest), `replace x = x`, `keep in 1/N`, `preserve/restore`, `compress`, `format`, `rename`, `drop var`, `label var/values`, `tsset` chars, `label drop _all`, `clonevar`, `st_addvar`, `st_addobs`, `st_dropvar`, `st_dropobsin`, `mergein` — all either set `c(changed)`, change N/k, or carry metadata from Stata.

Regression from today: **no** — the fast path and its `c(changed)==0` test are in HEAD (`git show HEAD:src/ado/p/parqit.ado` line 1515; introduced 2026-06-23 seed, CHANGELOG "Faster parqit save …, data after an unchanged parqit use").

Fix location: `src/ado/p/parqit.ado` `_parqit_save` gate (≈L1529–1537) and `src/plugin/plugin_io.cpp` `cmd_save_data_direct` (≈L3223). `c(changed)` cannot prove "identical to the file" (Stata exempts sort and Mata stores by design). Options: make the direct path opt-in (`parqit set fastsave on`) and document the Mata/sort caveat; or keep it but prove order/content cheaply before COPY (e.g. compare a deterministic row sample incl. first/last rows of every column through the SPI with the same rows of the source — catches every sort and most Mata edits but not all), and at minimum refuse the fast path when `: sortedby` is non-empty and differs from the source file's `parqit.schema.sortedby`.

### A4-2 — S0 — The direct save fingerprint (size + mtime + row count) is not content-sensitive: a same-size in-place rewrite with preserved mtime makes `parqit save` write the NEW source content

Repro (`p01_fastpath.do` FP-04): `fp/f.parquet` and `fp/f_alt.parquet` are pyarrow files with identical byte size (60 839 B), different values. After `parqit use fp/f.parquet, clear`, Python overwrites `f.parquet` in place with `f_alt`'s bytes and restores `st_mtime_ns` (`os.utime`), then `parqit save fp/g04.parquet, replace` → rc 0 and `FP-04 g04 s[0] = t00001 (memory has 's00001')`; `FAIL FP-04 … saved file must equal MEMORY`. With a new mtime (FP-05) the general path correctly runs. Realistic triggers: `cp -p`, `rsync -a`, `tar -x` over a same-size regenerated file during the session, or a writer racing between the fingerprint check and the COPY (TOCTOU window inside `cmd_save_data_direct`).

Regression from today: no. Fix: `regular_file_fingerprint` (`plugin_io.cpp` ≈L91): add inode and **ctime** (`stat()` `st_ctim`, which `utime` cannot restore) and ideally the footer length/offset or a hash of the Parquet footer bytes; re-check immediately before the COPY.

### A4-3 — S1 — Torn read under a concurrent replace: `parqit use, clear` loaded the NEW file's rows under the OLD file's schema (rc 0)

`cc/r_reader_schema.do` read `shared.parquet` 40× while `cc/w_writer_schema.do` alternated two payloads (A: 3M rows, x=1, 3 vars; B: 1M rows, x=2, 4 vars incl. `extra`). Iteration 1: `(3 vars, 1000000 obs read from shared.parquet)` → `READER iter 1 INCONSISTENT: N=1000000 k=3 min=2 max=2 hasextra=0` — B's rows without B's `extra` column, a dataset no file version ever had, with rc 0. Two other iterations failed loudly (`Invalid Error: TProtocolException: Invalid data`, rc 920) — acceptable.

Cause: `plan_columns()` (`plugin_io.cpp` ≈L621) probes the schema with `SELECT * … LIMIT 0` and the row count with a separate `SELECT count(*)` (≈L940); `use_fetch` scans a third time. The prepare→fetch row-count guard (`row count changed between prepare and fetch`) does not cover a replace between the two prepare probes or a same-count schema change. Regression: no. Fix: take schema + row count from one snapshot (one `parquet_metadata()`/file-handle read), fingerprint the source files (size/mtime/inode) at prepare and re-verify before fetch, and have `cmd_use_fetch` compare the fetched Arrow schema (names/types/count) with the planned manifest.

### A4-4 — S1 — Valid long destination names (≳200 chars, ≤ NAME_MAX) are refused

`p02_save_errors.log` / `p02b_save_errors2.log`: `parqit save se/<200×a>.parquet, replace` → `could not reserve package-owned output staging: File name too long`, rc 920; threshold measured between 198 (ok) and 203 chars (fail). `reserve_output_transaction()` (`plugin_io.cpp` ≈L182) names the staging dir `<filename>.parqit_txn_<pid>_<seq>_<32hex>` (+≈55 chars) and the lock `<filename>.parqit_lock` (+12). Regression: no (REL-001, 2026-07-14). Fix: derive short sibling names (e.g. `.pqt_<hash16>.lock` / `.pqt_<hash16>_<nonce>`), or truncate the filename prefix when `len(filename)+suffix > NAME_MAX`.

### A4-5 — S1 — `parqit save` into the open view's own *directory* source is allowed and silently changes the view's result

`p02b_save_errors2.log` SE-14d/SE-14f: `parqit use se/dsrc` (dir with p1,p2 × 10 rows each) → `parqit keep if id <= 10` → `parqit save se/dsrc/subset.parquet, replace` → rc 0, then `parqit count` → `r(N)=40 (was 20)`; a partitioned dest `se/dsrc/tree` inside the source dir is also accepted (SE-14f rc 0). The equivalent glob source (`se/dsrc/p*.parquet`) is refused (SE-14e rc 198), and ancestor-directory dests are refused (SE-14g/h rc 198). Regression: no (SAVE-SELFGLOB-2 unchanged vs HEAD). Fix: `cmd_view_save` overlap check (`plugin_view.cpp` ≈L1448–1540): for a directory source also treat `path_contains(source_dir, dest)` as a clash (or refuse only when the dest would match the directory's recursive `**/*.parquet` scan).

### A4-6 — S2 — Symlink and read-only destinations behave differently from native `save, replace`

- SE-13a: dest is a symlink → the symlink is replaced by a regular file; the link target keeps the stale payload (native `save` writes through the link). SE-13b: partitioned replace onto a symlink-to-dir removes only the link; target dir untouched. M7-5: dangling-symlink dest without `replace` → `could not move temporary file onto m7/dangling.parquet: File exists` (loud but confusing); with `replace` the link becomes a file.
- SE-16: a 0444 destination file is silently replaced (rename ignores the mode) where native `save, replace` returns r(608).
Fix/doc: resolve `dest` through `weakly_canonical` before staging (write to the link target) and document the read-only behaviour, or check `access(W_OK)` on the existing file. `copy_out_parquet` (`plugin_io.cpp` ≈L1073).

### A4-7 — S2 — Foreign Hive trees with `=` inside a partition value fail with a raw DuckDB message

`uf/hive_eq/city=a=b/` → `parqit use: Binder Error: Hive partition mismatch … key "city" not found` (rc 920); pyarrow reads it. Spaces, unicode, `%20` and Spark-style `%3D` values all work (p03c). parqit's own writer percent-encodes values (`city=a%3Db`, SE-20), so parqit-written trees round-trip. Doc/message only.

### A4-8 — S2/S3 — Contract and message gaps observed (loud, no data at risk)

- `PARQIT_TEST_FAIL_OUTPUT_PUBLISH` is inert for POSIX flat-file saves (SE-12d: rc 0, file replaced) — only the Windows flat branch and the partitioned branch honour it; ASSUMPTIONS #81 does not say so.
- Crash mid-write (p06: `ulimit -f 2000`, SIGXFSZ, core dumped): the old file is byte-identical (PASS) but both `<dest>.parqit_lock` and `<dest>.parqit_txn_<pid>_…` remain and every later save to that dest is refused (`already being written, or its package lock path … already exists`) — by design (#77), but the message names only the lock, not the orphan txn dir the user must also remove.
- `parqit set memory_limit 200MB`: 20M-row sort/collapse/collect fail loudly with DuckDB `Out of Memory Error … (183.4 MiB/190.7 MiB used)` instead of spilling (RS-1/RS-2); with 1 GB or 3 GB the same jobs spill (up to 582 MB seen in the unicode/space tempdir) and are exact (p08b). Worth a note in the help that very small limits do not spill.
- `parqit set tempdir <nonexistent>` warns (SET-TEMPDIR-1) and the later spill-needing save fails loudly (RS-1a rc 920) — fine, but the failure text is DuckDB's OOM, not "tempdir missing".

## PASS / FAIL table (all hypotheses tested)

| ID | Experiment | Result |
|---|---|---|
| FP-01 | `parqit use f, clear` → `parqit save` (fast path) equals memory | PASS |
| FP-02/08/19/27, M7-4b, RS-3b | `gsort` then save (incl. frame copy, `data` with view open, /tmp dest, 2,500 cols) | FAIL (A4-1) |
| FP-30/32/33 | plain `sort x`, `sort s x`, sort then save onto the source itself | FAIL (A4-1) |
| FP-30b, M7-1 | `parqit.schema.sortedby` vs physical order; propagation on reload | FAIL (A4-1) |
| FP-03 | `order` then save — column order follows memory | PASS |
| FP-04 | source rewritten in place, same size + restored mtime | FAIL (A4-2) |
| FP-05/17 | source rewritten with new mtime / erased → general path, correct | PASS |
| FP-06a/b/c, M8-3f | Mata `st_store`/`st_view`/`st_sstore` then save | FAIL (A4-1) |
| FP-07/09/10 | `replace x=x`, preserve/restore, no-op keep/drop/expand/set obs | PASS |
| FP-11/12a/12c/15 | compress, format changes, drop/rename → general path, exact | PASS |
| FP-12b | `format x %td` on fractional doubles → rounded with the documented note | PASS (documented loss) |
| FP-13 | save `replace` onto the fast-path source itself, then re-save | PASS |
| FP-14/26 | column subset / `in` on use then save | PASS |
| FP-16/21 | `label drop _all`, `tsset` → metadata follows memory | PASS |
| FP-18 | view open → save materialises the view (documented), close → memory | PASS |
| FP-22, M7-3/M8-2 | fast path + `partition_by` content | PASS |
| M7-2, M8-1 | fast vs general path honour `compression()`/`chunk()` identically | PASS |
| M8-3a..e, M8-7 | `st_addvar`, `st_dropvar`, `clonevar`, `st_addobs`, `st_dropobsin`, `mergein` | PASS |
| SE-01 | save without `replace` onto existing file: rc 602, md5 + listing unchanged | PASS |
| SE-02 | `replace` while another handle reads the old file | PASS |
| SE-03 | dest is a directory (with/without replace) refused, intact | PASS |
| SE-04a-f | partitioned onto plain file refused; tree no-replace refused; tree replace exact, no leftovers | PASS |
| SE-05 | partitioned onto existing empty dir without replace refused | PASS |
| SE-06a/b/c/f/g/i | spaces+unicode, `'`, `"`, `[1]`, `*`, no-extension dest names | PASS |
| SE-06d | 203–248-char valid names refused | FAIL (A4-4) |
| SE-06e/h | > NAME_MAX name, trailing slash → loud, no leftovers | PASS |
| SE-07 | relative dest after `cd`; `r(filename)` absolute | PASS |
| SE-08/09a/09b | nonexistent parent; unwritable dir (new and replace): loud, nothing touched | PASS |
| SE-10a/b | stale `.parqit_lock` dir/file → refused, lock kept, no dest | PASS |
| SE-11 | foreign `.parqit_txn_*` dir beside dest never touched | PASS |
| SE-12a/b/c | partitioned PUBLISH hook: old tree byte-identical, no leftovers; double failure retains old tree under recovery root | PASS |
| SE-12d | flat POSIX PUBLISH hook inert | INFO (A4-8) |
| SE-12e | ARROW_REGISTER hook: dest untouched, no leftovers | PASS |
| SE-13a/b, M7-5 | symlink / dangling-symlink destinations | INFO (A4-6) |
| SE-14a/b/g/h | save onto the view's flat source, via symlink, ancestor dirs → refused | PASS |
| SE-14c | hard link to source as dest: source inode intact, view still reads 10 rows | PASS |
| SE-14d/f | dest inside directory source allowed, view count doubles | FAIL (A4-5) |
| SE-14e | dest matching glob source refused | PASS |
| SE-15a/b | 0-obs save writes 0-row file; 0 vars → r(111) | PASS |
| SE-16 | 0444 dest replaced (native save r(608)) | INFO (A4-6) |
| SE-17/18 | `/dev/null` dest loud, no leftovers; tab in name ok | PASS |
| SE-20 | partition values with space/`=`/unicode/empty → encoded, pyarrow + parqit round-trip | PASS |
| UF use ×9 | missing, truncated (half/footer/mid-footer), magic-only, empty, garbage, bad footer len, corrupt snappy page: rc 920, memory + `c(changed)` intact (changed=0 and =1 states) | PASS |
| UF listcol | LIST column dropped with warning, load succeeds | PASS (documented) |
| UF corrupt_plain | corrupted PLAIN page read as garbage — identical in pyarrow and duckdb (no CRC in Parquet) | PASS (not parqit) |
| UF varlist-missing / glob-nomatch / emptydir / glob-diff-schema | loud, memory intact; `relaxed` unions | PASS |
| UF zero_rg / empty_rg | 0 rows, 3 vars | PASS |
| UF dirmeta | `_metadata`/`_common_metadata`/`_SUCCESS`/`.crc` ignored | PASS |
| UF glob_order | same schema, different column order → aligned by name | PASS |
| UF hive default / space / unicode / %20 / Spark `%3D` | PASS |
| UF hive `=` in value | raw Binder Error | INFO (A4-7) |
| UF collect: source vanished / truncated under view / unsaved changes (r(4)) | memory intact, loud | PASS |
| UF save from truncated view | rc 920, nothing written | PASS |
| M7-6a/b/c/d | failed and successful `use, clear` in a non-default frame / `frame:` prefix / frlink target | PASS |
| M7-7a/b/c | lazy view, source swapped: schema change → loud; same schema → consistent re-read | PASS |
| CC-1 | two writers same dest: each save rc 0 or rc 920 (lock), final file one complete payload | PASS |
| CC-2 | 40 reads during 25 replaces (same N) all consistent | PASS |
| CC-3 | reads during schema/N-changing replaces | FAIL 1/40 (A4-3) |
| CC-4 | two readers same source | PASS |
| C-1/C-2 | SIGXFSZ crash mid-write: old file byte-identical; later saves fail-closed | PASS / INFO (A4-8) |
| RS-1a/b/c, RS-2, RS-2b | 200 MB limit: nonexistent/read-only/unicode tempdir → loud OOM, no output | PASS (loud) / INFO (A4-8) |
| SP 1GB/3GB | 20M-row sort + 2M-group collapse + collect with unicode/space tempdir: spill observed, exact vs duckdb | PASS |
| RS-3 | 2,500-column save, read, fast-path re-save identical | PASS |
| RS-4 | 1.2M × 2,000-byte strL (2.4 GB) save: complete and exact | PASS |
| RS-5 | same with case-clashing names `big`/`BIG`: names + content exact, reload ok | PASS |
| RS-6a/b | ARROW_REGISTER hook: 0.2 GB strings fail (hook active), 2.4 GB succeed → proves the int32-offset retry ran the staged writer | PASS |
| P9 | `parqit use` while pyarrow streams row groups: loud until footer exists, then complete | PASS |
| Break/interrupt | not testable in batch | skipped |
