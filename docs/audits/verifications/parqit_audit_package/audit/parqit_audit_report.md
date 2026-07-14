# parqit adversarial test-pack audit

Date: 2026-06-24
Audited upstream commit: `722a2c83e809aca4422b0f020ab595ada55f62a4` (== HEAD)
Installed version tested locally: `0.1.11`
Main artefact: `tests/run_all_parqit_tests.do`
Reproduction environment: StataNow 19.5 MP, Windows 11

## Scope

Inspected artefacts:

- `upstream/parqit/src/ado/p/parqit.ado`
- `upstream/parqit/src/plugin/plugin_io.cpp`, `plugin_view.cpp`, `src/engine/view.cpp`
- `upstream/parqit/src/ado/p/parqit.sthlp`
- `upstream/parqit/README.md`, `CHANGELOG.md`, `ASSUMPTIONS.md`
- representative upstream integration and verify-suite tests
- local run log `tests/outputs/parqit_adversarial_tests.log`

All claims below were cross-checked against source and help, and the headline
finding was reproduced live against `parqit 0.1.11` (not read from the log only).

Auxiliary-tool decision: the shareable test pack is Stata-only by default so
colleagues can run it without Python, pyarrow, R, or build tools. Shell tools
were used only for repository inventory and log/source inspection.

## Coverage Summary

The aggregate `.do` file creates all core fixtures itself, downloads bounded
public test data, opens its own log, and runs examples by topic: setup, synthetic
data, `use/save`, lazy views, exploration commands, single-table verbs,
materialisers, `merge/append/joinby`, `mergein/appendin`, SQL/query, settings,
live downloads, and expected errors.

The latest local run reached the final verdict:

```text
VERDICT(PARQIT_ADVERSARIAL_TEST_PACK): FAIL (1 failures)
```

All normal examples passed except the single finding below.

## Finding

### PARQIT-CHAR-01 - Restoring a characteristic/note of a projected-away variable aborts materialisation

Severity: high by reach, but it is a LOUD failure (rc 3300 + message), not silent
corruption: a failed `use, clear` leaves memory intact, so there is no
stale/missing-data hazard. Under the project's own charter the top severity tier
is reserved for "rc 0 with stale/missing data"; this is a robustness/abort bug
with broad reach.

Contract: the help says `parqit use [varlist] using ...` and `parqit contract
varlist, freq(newvar)` are supported (`parqit.sthlp:59`), and that
characteristics AND notes are stored and restored (`parqit.sthlp:129-131`;
README `README.md:361-362`).

Mechanism: during decoration the ado runs `st_global(tgt + "[" + cname + "]",
val)` for every characteristic stored in the file. Stata notes are
characteristics (`var[note0]`, `var[note1]`, `_dta[note#]`), so they travel the
same path. If `tgt` is a syntactically legal Stata name that is NOT a variable in
the materialised result, `st_global` aborts with rc 3300.

Observed signature:

```text
st_global(): 3300 argument out of range
_parqit_resp_decorate(): function returned error
```

Reach (reproduced live against parqit 0.1.11):

| Trigger                                                          | rc     |
|------------------------------------------------------------------|--------|
| `parqit use <subset> using f, clear` excludes a char-bearing col | 3300   |
| `parqit use <subset> using f, clear` excludes a note-bearing col | 3300   |
| `contract` drops a char-bearing col, then `collect`              | 3300   |
| `collapse` drops a char-bearing col, then `collect`              | 3300   |
| `contract` drops a note-bearing col, then `collect`              | 3300   |
| `rename` of a char-bearing col, then `collect` (NOT a bug)       | 0 (ok) |
| full eager `use` (all columns present)                           | 0 (ok) |

The most common trigger is a plain column-subset `use` of any Parquet file that
parqit itself wrote with a variable label/note/characteristic, so this is not a
niche combination. `rename` is NOT affected (the view remaps the characteristic
to the new name, `src/engine/view.cpp:401-404`).

Test-pack evidence:

- `tests/run_all_parqit_tests.do:139` adds `char wage[source] "synthetic"`.
- `tests/run_all_parqit_tests.do:458-461` runs `parqit contract region sector,
  freq(freq)` then `collect`.
- `tests/outputs/parqit_adversarial_tests.log:1512-1520` shows the rc 3300.

Root cause (proximal): the `char` branch of `_parqit_resp_decorate()` validates
name legality but not variable existence before `st_global`:

- guard at `src/ado/p/parqit.ado:2544`, vulnerable call at
  `src/ado/p/parqit.ado:2545` (branch `2539-2549`).

Root cause (upstream): an emitter asymmetry. `var` records are pruned to surviving
result columns (`ctx.active`, `src/plugin/plugin_io.cpp:488-499`); `char` records
are emitted from the file metadata blob with NO column filter
(`src/plugin/plugin_io.cpp:519-527`). The `save` path already filters chars to
live columns (`src/plugin/plugin_view.cpp:311-318`), but `collect` copies the
whole map (`src/plugin/plugin_view.cpp:923`), and the view's char map is pruned
only on `rename` (`src/engine/view.cpp:401-404`).

Fix direction (detailed brief in `audit/RELATORIO_PARA_CLAUDE_CORRIGIR_PARQIT.md`):

1. ado guard in the `char` branch: apply only when `tgt == "_dta"` or the target
   variable exists. Use `_st_varindex(tgt) < .`, NOT `st_varindex(tgt)` -- the
   latter aborts rc 3500 on an absent name (verified live), and
   `st_varindex("_dta")` is missing, so the `_dta` case must stay explicit. Skip
   with a note, not silently.
2. plugin: filter emitted `char` records to live columns, mirroring the save path.

This class is NOT acknowledged in CHANGELOG/ASSUMPTIONS (META-1 fixed a different
rc 3300 from char-line truncation; META-2 made chars follow a rename), so an
upstream issue/PR is justified as new.

## Test-pack defects found by critical self-review

The pack is well-structured and genuinely adversarial, but it is not yet robustly
shareable, and it under-tests the very class of bug it found. Fix before sharing:

- P0 portability: `tests/run_all_parqit_tests.do:40-43` converts `/` to `\` with
  no `c(os)` guard and feeds backslash paths to `parqit set tempdir` at `:670`
  and `:673`. On macOS/Linux a backslash is a literal filename character, so the
  whole suite FAILs spuriously. Stata accepts forward slashes on Windows: delete
  lines 40-43 and pass the native `DATA`/`TEMPDIR` paths. The `:672` message also
  mislabels the backslash case as "path with spaces".
- P1 network as FAIL not SKIP: net install (`:92-98`) and the two downloads
  (`:690-694`, `:707-712`) route failures into `_pq_assert`, so an offline or
  firewalled colleague (raw.githubusercontent is frequently blocked) gets a false
  FAIL. Add a `_pq_skip` helper; only count a FAIL when a download succeeded but
  parqit mis-read it.
- P1 version pinning: hardcoded v0.1.11 install URL (`:90`); the mismatch note
  (`:105-107`) correctly does not fail, but the exact-value asserts are not gated
  behind `if tested_version=="0.1.11"`, so a newer parqit can fail instead of
  skip.
- P1 missing coverage for `partition_by` replace: `:556-558` writes to a fresh
  `tempfile`, so `replace` never overwrites an existing directory -- the risky
  path is no longer tested. Write the partitioned directory twice over the same
  target and assert success + row count.
- P1 missing coverage for PARQIT-CHAR-01: the only test that drops a char-bearing
  variable before materialising is `contract` (`:458-461`). `collapse` (`:485`),
  `reshape` (`:507-514`), `keep`/`drop` (`:426`,`:535`,`:555`) and the two-table
  verbs run over char-stripped fixtures or keep the char-bearing variable, so the
  broader reach (including subset `use`) is untested. Add a fixture that RETAINS a
  characteristic/note on the column that gets dropped.
- P2 determinism/robustness: RUNID from `c(current_time)` collides within the
  same second (`:28`); brittle exact counts (`r(n_row_groups)==5` at `:527`,
  `r(surplus)==160` at `:370`, and other hardcoded literals) should derive from
  source counts or be relaxed; `capture mkdir` (`:34-39`) swallows real
  permission errors; `capture parqit close _all` at `:468` masks cleanup errors
  (use plain `parqit close _all`).

## Validation Notes

Successful coverage in the final run included:

- install/version/selftest checks
- Parquet, DTA, CSV, and Excel inputs
- public downloads from Stata Press and GitHub
- paths with spaces
- metadata round-trip checks (labels, value labels, characteristics) on full reads
- lazy view registry and view-prefix execution
- pushed-down exploration commands and stored-result checks
- native-Stata oracles for `collapse`, `merge`, `joinby`, `mergein`, and `appendin`
- partitioned output, `compression()`, `compression_level()`, and `chunk()`
- expected failures for missing files, unsupported functions, duplicate generated
  variables, invalid date literals, invalid `keep in` at execution, append
  `generate()` collision, no-replace save, and bad compression codec

Implementation note on `partition_by()`: the test writes its Hive-style output to
a Stata `tempfile` path, not the Dropbox-backed workspace. The prior Windows
failure (`could not move temporary partition tree ... Access is denied`) is NOT
the naive "rename onto an existing directory" bug: the existing tree is renamed
aside before the final `fs::rename` (`src/plugin/plugin_io.cpp:751-778`), so the
failing rename targets a path that no longer exists. The real hazard is an open
handle on the freshly written tree (DuckDB / antivirus / Windows Search Indexer /
cloud sync), and the code does a single `fs::rename` with no retry. This is a real
Windows-portability fragility (reproducible without Dropbox), but it is a separate
P2 item, not the main bug. Treating it as a harness/cloud-sync limitation is
partially justified but under-claims the portability risk.

Residual risk:

- This pack does not build the plugin from source.
- It does not run C++ unit tests or upstream CI.
- It intentionally avoids Python/pyarrow-only adversarial Parquet fixtures to keep
  the shared Stata file portable.
- Until the P0/P1 test-pack defects above are fixed, the suite is not reliable on
  macOS/Linux or offline, and it does not yet exercise the full reach of
  PARQIT-CHAR-01.
