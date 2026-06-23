# Independent adversarial audit of `parqit`

You are an independent auditor. You did NOT write this code. Your job is to find
real problems — especially **silent wrong results** — and write a report. You fix
nothing. Assume the authors were competent but optimistic; your default stance is
distrust: every claim in README/help/comments is a hypothesis to verify against
the actual code and, where possible, against running behaviour.

## Subject

`parqit` is a Stata package: lazy Stata-flavoured verbs (`keep`, `gen`, `collapse`,
`merge`, `reshape`, …) compile into a single DuckDB SQL query executed
out-of-core over Parquet. Architecture:

```
src/ado/p/parqit.ado          dispatcher (49 subcommands) + Mata: builds JSON request
                            files (ALL user strings hex-encoded), parses pipe-
                            separated response files
src/plugin/*.cpp            C++ Stata plugin (SPI: stplugin.h, extern "C"):
                            dispatch, SF_* data bridge, named-view registry
src/engine/*.cpp            DuckDB session, SQL builder, Stata-expr→SQL translator,
                            type map, identifier sanitiser, hex codec
embedded DuckDB 1.5.3       built from source, statically linked (parquet +
                            core_functions extensions)
```

Key invariants the product claims (these are the audit targets):
- Only two materialisers move data: `collect` (into Stata, atomically) and
  `save` (Parquet→Parquet, never touching Stata memory).
- A column manifest travels with every operation; engine keyed by source name.
- Loud errors: nonzero `_rc` + message, never rc 0 with a stale/missing file.
- Type contract: %tm/%tq/%th/%ty/%tw stay INTEGER period counts; %tc/%tC
  milliseconds (BIGINT on disk for %tC); uint32/uint64/decimal are bound-checked
  values, never silent nulls; LIST/STRUCT error or drop loudly; string lengths
  are bytes (UTF-8), 2045-byte str#/strL boundary; byte/int/long use Stata's
  exact ranges (max 100 / 32740 / 2147483620; Stata missing = values above).
- Stata dates inside pipelines are NUMBERS (day/ms counts); conversion to DATE/
  TIMESTAMP happens only at the Parquet boundary (epoch shift: 3653 days,
  315619200000 ms — Stata epoch 1960, Unix 1970).
- Expressions default to SQL missing semantics; `parqit set statamissing on`
  emulates Stata's "missing sorts above everything"; `x < .` idioms map to
  IS NULL tests either way.
- Variable/value labels, notes, formats, characteristics round-trip via Parquet
  key-value metadata under a `parqit.*` namespace.
- strL writes use a binary sidecar file (SPI cannot store strLs) poured by Mata.

## Authoritative references (read first, in this order)

1. `parqit_build_prompt.md` — the build brief; its "must"/"do not" statements are
   the contract.
2. `CLAUDE.md` — condensed architecture + invariants.
3. `ASSUMPTIONS.md` — decisions taken where the brief was silent. Audit these
   too: an assumption can be wrong.
4. `README.md` and `src/ado/p/parqit.sthlp` — the public promises. Every testable
   claim in them is in scope ("promise audit").
5. If available on this machine (skip + note if not):
   `/home/mangelo/Documents/BPLIM_GitHub/pq_audit_2026-06-11` — a 14-finding
   audit of a comparable package (`pq 3.0`); each finding is an engine-independent
   hazard class. Verify parqit is immune to each BY DESIGN, not by accident.
   `/home/mangelo/Documents/GitHub/xhdfe` — house style reference.

## Environment

Work from the repo root. Toolchain available: gcc/g++, CMake ≥3.26, python3 +
pyarrow, `duckdb` CLI, GNU make. Stata: `stata-mp` on PATH (batch:
`stata-mp -b do file.do`; CAUTION: batch Stata always exits 0 and names the log
after the last CLI token — never trust its exit code, grep `^VERDICT` lines or
`r(...)` from logs). System locale is Portuguese (pt_PT) — tool output may be in
Portuguese, and this is itself an audit angle (see F below). A build may already
exist in `build/dev`; you may rebuild (`cmake --preset dev && cmake --build
build/dev -j`). Run the existing suites:

```bash
ctest --test-dir build/dev --output-on-failure   # C++ unit tests (doctest)
bash tests/run_stata.sh                          # full Stata suite (~10 min)
```

## Audit scope — sweep these areas; the bullet questions are starters, not limits

**A. Protocol & injection surface.** The ado builds JSON requests; user data
(column names, file paths, expressions, labels) ultimately lands inside SQL
strings executed by DuckDB. Trace EVERY path from user input to SQL text: are
identifiers always quoted/escaped through the sanitiser? What happens with a
column literally named `x"; DROP TABLE t; --`, with embedded quotes, backslashes,
newlines, `|` (the response-field separator), non-UTF8 bytes? Are ALL free-text
fields hex-encoded in BOTH directions (request and response), or do some travel
raw? Check `view_query`/`parqit query` raw fragments and `parqit sql` (raw by design
— but does anything from them leak into later non-raw stages?).

**B. Type map & numeric boundaries.** Exact Stata ranges (byte >100 is missing,
int >32740, long >2147483620; float vs double demotion); uint64 values above
2^53 (double precision loss — silent?); DECIMAL scale/rounding; NaN and ±Inf
arriving from Parquet (Stata has no Inf — what lands?); date epoch shifts and
floor-division for pre-1960 dates (negative values!); %tC (leap seconds) vs %tc;
DuckDB `//` vs true floor for negatives; the positive-modulus ms formula. Write
tiny adversarial Parquet files with pyarrow and round-trip them.

**C. Missing-value semantics.** SQL mode vs `statamissing` mode in every
comparison operator, `!=`, boolean contexts, `cond()`, min/max aggregates,
`sort` order (Stata puts missings LAST ascending; DuckDB default NULLS order —
verify what parqit emits in ORDER BY and in window functions used for _n/_N);
extended missings .a–.z collapse to NULL on save (is the warning actually
emitted? do their value labels survive?); strings: NULL vs "" equivalence
claimed — consistent everywhere (joins! group-by keys! distinct!)?

**D. Atomicity & error discipline.** Force failures at every stage (nonexistent
file, schema mismatch mid-pipeline, full disk if cheap to simulate, kill -9 the
plugin mid-collect if feasible): does the in-memory dataset survive intact? Does
`save` ever leave a half-written/zero-byte Parquet that a later run could read?
Is every plugin entry returning a real ST_retcode and does the ado check `_rc`
EVERY time (grep for `plugin call` not followed by rc checks)? Any path where
rc==0 but the response file is stale/absent (the ado must detect, not reuse)?

**E. Plugin global state & resource hygiene.** Named-view registry lifetime;
collect-does-NOT-consume semantics (re-collect re-executes — deterministic?);
temp files (requests, responses, strL sidecars, temp Parquet for `parqit open
_data`) — created where, cleaned when, collisions if two Stata instances run
simultaneously? DuckDB temp/spill directory; memory growth across many
open/close cycles.

**F. Locale.** The machine runs pt_PT (decimal COMMA). Audit every place a
number becomes text or text becomes a number: C++ `snprintf`/`to_string`/
`stod` (locale-dependent!), Mata `strofreal`, SQL literals built from doubles
(e.g. histogram bin edges use %.17g — with which locale?), Stata `set dp comma`
(a user setting!) interacting with number parsing of responses. A decimal comma
inside generated SQL = silently wrong results. Test: run a numeric pipeline
under `LC_ALL=pt_PT.UTF-8` AND with `set dp comma` in Stata.

**G. Expression translator.** Operator precedence (`^`, unary minus, `!`/`~`),
string functions byte-vs-character semantics (substr/strlen/strpos on UTF-8 and
emoji), `inrange`/`inlist` with missings, date pseudo-literals td()/tm()/tc(),
`_n`/`_N` when no sort is declared (is the order pinned or nondeterministic —
and is THAT documented/loud?), unsupported functions (must error loudly, never
pass through as SQL hoping DuckDB has same-named function with different
semantics — e.g. Stata `mod()` vs SQL `%` sign behaviour, `round()` half-away
vs half-even, `log()` base!).

**H. The tests themselves.** Does `tests/run_stata.sh` actually fail (nonzero)
when a suite fails or a log is missing? Can a VERDICT line be PASS while an
earlier assertion was skipped (e.g. `capture` swallowing)? Are the "independent
oracles" really independent (pyarrow/duckdb CLI reading the file parqit wrote),
or do any tests validate parqit against parqit? Is there a coverage hole: which of
the 49 subcommands have NO test exercising them? (List them.)

**I. Build / CI / release.** Do `.github/workflows/build.yml` presets exist in
`CMakePresets.json`? Old-glibc claim (AlmaLinux 8) consistent with any newer-
glibc symbols used? Version script + `-fvisibility` exports exactly
`stata_call`/`pginit`? Static libstdc++ verified? Release zip names match
README install instructions and `parqit.pkg` platform lines? Any chance the CI
release step packages a stale or debug (unstripped, ~1GB) plugin?

**J. C++ safety at the boundary.** `stata_call` is `extern "C"`: can ANY C++
exception escape it (instant Stata crash)? Audit every entry point for a
catch-all. Buffer arithmetic in the strL sidecar writer/reader (the
`%010d`/`%012d` fixed-width header parsing) — off-by-one, lengths that don't
match payload, truncated file. SF_* calls: obs/var indices 1-based everywhere?
`SF_macro_save` size limits? Long column lists (2500+ vars) overflowing macro
length limits in the ado layer (Stata macros cap at ~645k chars in MP —
manifest serialisation for wide tables)?

**K. Promise audit.** Extract every objectively testable claim from README.md
and parqit.sthlp (the type-mapping table, "atomic", "never touching Stata
memory", "m:m reproduces Stata's sequential pairing exactly", "lossless
round-trip", limitations section) and mark each: VERIFIED / FALSE / UNTESTABLE,
with evidence.

## Rules of evidence

- Read the actual code; never trust a comment, docstring, or report.
- Prefer a runnable reproduction for every finding. Put repro scripts under a
  new `audit_repro/` directory (do-files, .py, .sql). If you cannot reproduce,
  say why and lower your confidence.
- Use pyarrow / the duckdb CLI as independent oracles for on-disk payloads.
- When you cannot verify something (missing tool, no licence, time), record it
  in "Not verified" — an unverified area is a finding in itself, not a PASS.
- Quote evidence as `file:line` plus the relevant snippet (short).
- No style opinions, no rewrites-for-taste. Correctness, safety, robustness,
  portability, test integrity, and broken promises only.

## Hard constraints

- DO NOT modify any tracked file. You write exactly two things:
  `PARQIT_AUDIT_REPORT.md` (repo root) and `audit_repro/` contents.
- DO NOT commit, push, tag, or touch git state.
- DO NOT install anything system-wide or write outside the repo and /tmp.
- Long Stata runs: fine. Destructive tests (full disk, kill -9) only against
  scratch copies in /tmp, never against `tests/fixtures/` or tracked files.

## Report format — `PARQIT_AUDIT_REPORT.md`

```markdown
# parqit audit report
Date / auditor (model) / repo commit audited (git rev-parse HEAD) / time spent.

## Executive summary
≤15 lines. Overall verdict. Then "Top recommended actions", ranked, max 5,
each one line with finding IDs.

## Findings table
| ID | Severity | Confidence | Area | Title | Repro? |
(sorted by severity)

## Findings
### PARQIT-NN — title
- **Severity:** S0 silent wrong results/data corruption · S1 crash or data loss
  with signal · S2 wrong behaviour, rare or loud or workaroundable · S3
  robustness/portability hazard · S4 docs/cosmetic
- **Confidence:** certain (reproduced) / likely (code-read, clear) / possible
  (suspicious, not proven)
- **Where:** file:line(s)
- **Evidence:** snippet + reasoning, or repro transcript
- **Repro:** audit_repro/<file> (if any)
- **Impact:** who hits this and what they see
- **Suggested direction:** one or two sentences (direction, not a patch)

## Promise audit
Claim-by-claim table: claim / source (README §, sthlp section) / verdict /
evidence pointer.

## Coverage map
Area (A–K) / what you actually did (files read, tests run, repros written) /
depth (deep, partial, skim).

## Not verified
What you could not check and why; what it would take.

## Test-suite gaps
Subcommands or invariants with no covering test.

## Questions for the maintainer
Anything ambiguous where intent matters.
```

Severity discipline: a bug that silently produces wrong numbers in a research
dataset is S0 even if exotic; a loud crash is at most S1. When torn, escalate.

Budget: be thorough — this is a correctness audit of a data bridge for research
computing, not a smoke test. Prioritise A, B, C, D, F, J (silent-wrongness
surfaces) over I, K if time runs short, and say in the report what got cut.
