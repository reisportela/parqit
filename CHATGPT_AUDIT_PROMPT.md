# Prompt — Adversarial audit of the `parqit` Stata package (give this to ChatGPT Pro together with `parqit_audit_bundle.zip`)

You are an **elite adversarial code auditor** specialising in Stata↔columnar data
bridges. Attached is the full source of **`parqit` v0.1.10** (zip). Audit it with
**maximum extension and depth** for correctness, precision, atomicity, determinism,
metadata-fidelity and performance defects across **every function of the package**,
and produce a **single, downloadable Markdown report** (`parqit_adversarial_audit.md`)
whose findings each carry **explicit, step-by-step instructions for an AI coding agent**
to reproduce, fix and verify them.

Treat this as red-teaming: your job is to find what the package's own test suite and
three prior audit rounds **missed**, and to prove each issue from the actual code.

---

## 1. What `parqit` is (read before auditing)

`parqit` is "dbplyr's architecture with Stata's vocabulary on a DuckDB engine": the user
writes lazy Stata verbs (`keep`, `gen`, `collapse`, `merge`, `reshape`, …) that compile
to **one DuckDB SQL query** executed out-of-core over Parquet. Only two paths move data:
`parqit collect` streams the result into Stata's in-memory dataset; `parqit save` writes
Parquet→Parquet without touching Stata memory. An ado↔C++‑plugin boundary carries every
user string as **hex-encoded UTF-8**; results come back as `kind|field|...` line records.

**Architecture / where to look (in the zip):**
- `src/ado/p/parqit.ado` — subcommand dispatch, argument parsing, the Mata wire protocol
  (hex codec + request writers + response readers/printers), and the in-memory `mergein`/
  `appendin`. **~3.3k lines; this is the largest attack surface.**
- `src/ado/p/parqit.sthlp` — the documented contract (every promise here must hold).
- `src/engine/exprtrans.cpp` — the Stata-expression → SQL translator (lexer + parser +
  per-function lowering). **The parity-critical module.**
- `src/engine/view.cpp` — the verb→plan compiler (each verb appends a CTE stage):
  keep/drop/filter/gen/replace/rename/order/sort/collapse/contract/duplicates/sample/
  egen/merge/append/joinby/reshape_long/reshape_wide.
- `src/engine/typemap.cpp` — Stata type/format ↔ DuckDB/Arrow logical type, on read and save.
- `src/engine/session.cpp` — DuckDB session, locale-independent number↔text (`dtoa`/`atod`),
  the registered scalars `parqit_stata_string` / `parqit_substr_bytes`.
- `src/engine/sanitize.cpp`, `hexcodec.cpp`, `request.cpp` — identifier sanitiser, the hex
  codec, and the field-delimited wire protocol.
- `src/plugin/parqit_plugin.cpp` (entry + dispatch + the `extern "C"` catch-all),
  `plugin_io.cpp` (use/describe/save + the SF_* collect fill + parallel fill + strL sidecar),
  `plugin_view.cpp` (lazy-view subcommands, two-table exec, reshape exec, stats/introspection).
- `tests/` — `unit/` (doctest C++), `verify_suite/` (one self-contained do-file per invariant,
  each asserting an exact signature against an **independent oracle** — pyarrow/duckdb/native
  Stata — and printing `VERDICT(...): PASS/FAIL`), `integration/`, `roundtrip/`, `fixtures/`,
  plus `run_stata.sh` and `release_lint.sh`.
- Context docs: `README.md`, `CHANGELOG.md`, `ASSUMPTIONS.md`, `parqit_build_prompt.md`
  (the authoritative build brief — its "must/do not" decisions are fixed), `AGENTS.md`
  (non-regression rule), and the prior audit reports `PARQIT_*AUDIT*.md`, `PARITY_*`,
  `RELATORIO_*`.

---

## 2. Current state, and what NOT to merely re-report

The package is **green**: ~50 C++ unit cases and **46 Stata suites** (`v02`–`v36`,
`t01`–`t11`, `m0_smoke`) all pass; `release_lint` is clean. So the easy bugs are gone —
you must find the **residual** ones the tests don't cover.

The following clusters are **already fixed and locked by tests** (see `CHANGELOG.md`,
`ASSUMPTIONS.md`, and the named verify tests). **Do NOT report them as new.** You MAY
report one **only if you prove from the current code that the fix is incomplete or wrong**
(cite file:line and give a failing repro):
- Expression translator: `string()`/`strofreal()` = Stata `%9.0g`; `substr()`/`strpos()`
  byte-indexed; `^` left-associative; `mod(x,y≤0)`=missing; `round()` ties toward +∞;
  `/` and `^` guarded to missing on non-finite; `upper`/`lower` ASCII-only vs `ustr*`
  Unicode; `inrange()` missing bound = ±∞; `&|!` treat missing-as-true; `cond()` arity;
  `real('inf'/'nan')`→missing; chained comparisons; `strpos(s,"")`=0; `length(numeric)`
  errors. (`test_exprtrans.cpp`, `v28`, `v31`, `v33`, `v35`.)
- `gen byte/int/long/float` truncates-toward-zero + out-of-range→missing + sizes to type;
  `gen str#` truncates to the declared byte width. (`v33`, `v35`.)
- Dates: `%tm/%tq/%th/%ty/%tw` stay INTEGER period counts; save bound-checks before
  narrowing; `mdy()/dofm()` row-local missing via `try()`. (`v03`, `v05`, `v22`, `v28`.)
- Types: DECIMAL→double (warns >2^53); uint/hugeint widening; float32 widening; ±Inf
  loud-missing / NaN silent-NA; embedded NUL; str#/strL 2045 boundary. (`v06`, `v11`,
  `v15`, `v19`, `test_typemap.cpp`.)
- Two-table: `merge`/`joinby` keys fold ""≡NULL≡NaN; `merge m:m` deterministic +
  missing-key folding; `_merge` value labels; `keep()` is a set; append type conflicts
  loud; append `generate()` collision loud. (`v14`, `v25`, `v27`, `v28`, `v33`, `v35`.)
- collapse/contract/duplicates/egen by-keys fold ""/NaN to missing; collapse (first/last)
  deterministic; weights rejected loudly; no-by collapse over 0 rows → 0 obs. (`v33`,
  `v35`, `v36`.)
- reshape long stub/`i()`/leading-zero-suffix handling; reshape wide numeric-j column
  order + invalid generated names errored. (`v27`, `v34`, `v33`.)
- I/O & atomicity: `collect`/`use,clear` atomic temp-frame swap; `save` onto own source
  refused (incl. glob/dir); partitioned + Windows replace via rename-aside; glob-escaped
  internal self-reads; aborted collect drops temp table; long dataset-label truncated not
  fatal. (`v08`, `v09`, `v30`, `v33`, `v35`, `v36`.)
- Identity/metadata/protocol: full hex request encoding; sanitiser reversibility;
  fresh-helper internal names; UTF-8 validated on save; locale-independent numbers;
  `_merge`/value-label round-trip. (`v02`, `v07`, `v10`, `v12`, `v16`, `v17`, `v18`,
  `v32`, `test_sanitize.cpp`, `test_request.cpp`, `test_hexcodec.cpp`.)
- Introspection/perf: codebook single-scan; two-way tabulate single-scan; tabulate integer
  rendering + numeric axis order; PERF-1 `_n` streaming. (`v33`, `v35`, `v26`.)

**Known-deferred (documented in `ASSUMPTIONS.md` #47, #50, #51) — flag if you can show
real harm, otherwise do not pad the report:** strL save return codes unchecked;
`summarize, detail` and multi-percentile collapse/tabstat re-sort per percentile; reshape
`i()/j()` grouping does not yet fold ""/NaN; partitioned write is not fully transactional.

The SQL-default missing-comparison semantics are **intentional** (the brief mandates it;
`parqit set statamissing on` reproduces Stata). Do **not** propose changing the default —
at most flag a *documentation* inaccuracy.

---

## 3. Audit discipline (non-negotiable — this is what makes the report trustworthy)

1. **Verify against the ACTUAL source, never the CHANGELOG or comments.** Comments lie;
   prove behaviour from the code path (ado parsing → Mata request → C++ handler → emitted
   SQL → result decode).
2. **For every claim, state precisely (a) what native Stata 16+ does and why, and (b) what
   parqit actually does** (quote the generated SQL or the C++). A finding without both is
   not admissible.
3. **Independent oracle for every repro.** Never "parqit says X so X is right." Compare to
   native Stata, to `pyarrow`/`duckdb` reading the same bytes, or to hand-computation. The
   on-disk payload must be checked by a non-parqit reader.
4. **Adversarially verify before reporting.** For each candidate, actively try to *refute*
   it: is there a guard upstream? an existing test that covers it? a documented assumption?
   Only report what survives. Assign `confidence: high|medium|low` and keep low-confidence
   items in a separate section.
5. **No fabrication.** If you cannot run code, reason from the source and say so; never
   invent benchmark numbers, DuckDB/Stata API behaviour, or test output. If unsure of a
   DuckDB function's semantics, say "unverified — agent must confirm" in the agent steps.
6. **Exhaustiveness over brevity.** Cover the entire surface in §5. A function you did not
   examine is a gap; list it in the coverage matrix as "not audited" rather than implying
   it is clean.

---

## 4. Constraints on every proposed fix (the maintainer's hard rules)

- **MUST NOT remove a feature or reduce precision** (no narrowing, no silent drops, no
  metadata loss).
- **MUST NOT increase computation time materially.** A slight per-feature slowdown is
  acceptable **only** if offset by a net gain elsewhere; say so explicitly.
- For every finding give an honest `regression_risk` and `perf_impact` for the proposed fix.
  If the only available fix violates these rules, say so and recommend deferral.

---

## 5. Coverage checklist — audit EACH of these (tick every box in the coverage matrix)

**Public subcommands (`src/ado/p/parqit.ado`):**
`version selftest use save describe glimpse open close keep drop gen egen replace rename
order sort gsort collapse contract duplicates sample collect count head list show explain
set merge append joinby reshape sql query summarize tabulate path view views misstable
levelsof ds lookfor codebook distinct tabstat correlate pwcorr histogram mergein appendin`
— for each: argument parsing (gettoken/syntax edge cases, quoting, embedded commas/spaces/
quotes), option handling, and the lazy-vs-materialised behaviour.

**Expression-translator functions (`exprtrans.cpp`):**
`missing mi abs exp ln log log10 sqrt floor ceil int trunc round mod min max cond inrange
inlist strlen length ustrlen upper strupper ustrupper lower strlower ustrlower trim strtrim
ltrim rtrim substr strpos subinstr string strofreal real regexm year month day quarter dow
doy mdy dofm mofd yofd` and the date literals `td tm tq th tw ty tc tC`; plus the operators
(`+ - * / ^ & | ! == != < > <= >=`), unary sign, precedence/associativity, `_n`/`_N`
row-context, string vs numeric typing, and the `statamissing` mode. **Also probe commonly
used Stata functions that may be unsupported or subtly wrong:** `sign reldif mreldif word
regexr regexs proper char uchar autocode recode cond-on-string clip floor/ceil/int of
missing exp/ln overflow`, and whether `min()/max()` ignore missing like Stata.

**Plugin entry points (`parqit_plugin.cpp` dispatch):**
`ping echo selftest version use_prepare use_fetch describe save_data save_data_direct
view_open view_op view_collect_prepare view_save view_info view_close view_switch view_list
view_twotable view_reshape view_query view_sql view_stats view_alive set path` — for each:
malformed/hostile request handling, `ST_retcode` correctness, and the `extern "C"` exception
boundary.

**Data paths & cross-cutting concerns:** collect vs save; lazy view vs eager `use, clear`;
the unchanged-source `save_data_direct` fast path; partitioned save; strL binary sidecar;
parallel fill (≥50k rows); the temp-Parquet bridge for `.dta`/`.xls(x)`/`.csv` and for
`open _data`; the column manifest & positional-vs-by-name discipline; the type map on read
**and** save (every DuckDB type, every Stata format class); metadata round-trip (variable/
value labels, notes, formats, characteristics, extended missings, dataset label) under the
`parqit.*` namespace; the hex wire protocol in both directions (is every foreign-data
response field hex-encoded?); the sanitiser (reserved words, leading digit, >32 chars,
spaces, unicode, duplicates, collisions); locale independence; determinism (sampling seeds,
window ORDER BY, NULLS ordering); resource hygiene (temp files/frames/tables on failure
paths); and `r()`/`c()` correctness (`c(changed)`, `c(filename)`, returned scalars).

---

## 6. Hazard taxonomy to probe (the charter invariants — see `parqit_build_prompt.md` §6)

Positional corruption on subset/reorder; missing-value semantics divergence; date/period
mis-scaling; integer overflow → silent null; label/value-label corruption; **loud errors**
(every failure must be nonzero `rc` + message, never `rc 0` with a stale/missing file);
atomic validate-then-mutate; silent column loss (duplicates); unsupported types
(drop-with-message vs silent all-missing); internal-name clobber; range validation
(`keep in`/`in()`); pathological/hostile names; SQL/format injection through any
non-hex-encoded field; locale (decimal comma); determinism; UTF-8 well-formedness; and
out-of-core join key normalisation across tools (pandas NaN vs DuckDB NULL vs Stata `.`).

For each, ask: *can I construct an input or sequence where parqit silently returns a wrong
result, loses precision/metadata, leaves a stale file with `rc 0`, or diverges from native
Stata?*

---

## 7. How to work (method)

1. Build the architectural model from §1's file map; skim `README.md` + `parqit.sthlp` for
   the **promised** contract, and `ASSUMPTIONS.md` for the **claimed** behaviour.
2. Go function by function through §5. For each, read the **full** code path and derive the
   emitted SQL / fill logic; then construct adversarial inputs (boundary values, mixed
   missing encodings, hostile names, empty/1-row/wide/multi-row-group data, UTF-8/emoji,
   extreme integers/floats/dates, foreign-tool Parquet) and predict parqit vs Stata.
3. For each candidate, do the §3 adversarial-verification pass and assign confidence.
4. Fill the coverage matrix as you go so nothing is silently skipped.

**Build/test commands the agent will use to verify (quote these in agent instructions):**
```bash
cmake --preset dev && cmake --build build/dev -j      # first build compiles DuckDB (slow)
ctest --preset dev                                    # C++ unit tests (no Stata)
bash tests/run_stata.sh                               # all Stata suites; nonzero exit on FAIL
bash tests/run_stata.sh <fragment>                    # one suite/family
bash tests/release_lint.sh                            # version/date drift + path-leak gate
```
Verify-test convention: one self-contained do-file that generates its own synthetic data,
asserts the exact failure signature, checks the on-disk payload with an **independent
oracle** (pyarrow and/or duckdb CLI), and prints `VERDICT(<NAME>): PASS/FAIL`.

---

## 8. Required output — a single downloadable Markdown report

Produce `parqit_adversarial_audit.md` and offer it as a **file download**. Use exactly this
structure:

1. **Executive summary** — counts by severity, the top 5 must-fix issues, and an overall
   risk verdict.
2. **Methodology & scope** — what you read, how you verified, what you could not verify.
3. **Findings table** — `ID | severity (S1–S4) | dimension | file:line | one-line title |
   confidence`. Sort by severity then confidence.
4. **Detailed findings** — one section per finding, in this **exact schema**:
   - **ID & title**
   - **Severity** (S1 silent wrong result; S2 silent-wrong/high-impact gap or precision
     loss; S3 functional/determinism/atomicity/loud-but-wrong; S4 minor/cosmetic/latent)
     and **Confidence** (high/medium/low)
   - **Location** — `file:line` (and the function)
   - **What the code does** — quote the relevant source / generated SQL
   - **What native Stata does** — precise expected behaviour, with reasoning
   - **Why it is a bug** — the divergence and its impact
   - **Reproduction** — a self-contained snippet (parqit verbs + an **independent oracle**:
     native Stata twin, pyarrow, or duckdb CLI) with expected vs actual
   - **Proposed fix** — concrete, minimal, citing the exact code to change
   - **regression_risk** and **perf_impact** (per §4)
   - **🤖 AI Agent Instructions** — a numbered, copy-pastable task list for an autonomous
     coding agent: (1) reproduce (exact files/commands), (2) confirm the diagnosis against
     the live code, (3) apply the fix (exact edit), (4) add/extend a `verify_suite` test
     with an independent oracle (give the test skeleton + `VERDICT` line), (5) run
     `ctest --preset dev` and `bash tests/run_stata.sh <name>` and the full suite, (6)
     update `CHANGELOG.md`/`ASSUMPTIONS.md` and bump the four version surfaces if releasing.
     Note any step the agent must independently confirm (e.g. a DuckDB semantic you did not
     verify).
5. **False positives considered and rejected** — candidates you refuted, with the reason
   (a guard, an existing test, a documented assumption). This proves rigour.
6. **Coverage matrix** — every item in §5 with status `audited-clean | finding(s) | not
   audited`, so the maintainer sees exactly what was and was not examined.
7. **Prioritised remediation plan** — the recommended order to fix, grouped so an agent can
   batch related changes, respecting the §4 constraints.

Be exhaustive, be precise, prove every claim from the code, and make every finding directly
actionable by an AI agent. Begin by confirming the bundle contents and your audit plan,
then produce the report file.
