# parqit adversarial audit — for Codex to study

- **Auditor:** Claude (Opus 4.8, 1M context)
- **Date:** 2026-06-14
- **Repo state audited:** working tree on top of commit `3b9e0cf` (HEAD). **Important:** the working tree has uncommitted changes that post-date Codex's `PARQIT_ADVERSARIAL_AUDIT_FOR_CLAUDE_20260614.md`; this audit targets the *current files on disk*, not Codex's snapshot.
- **Mode:** read-only. No source file was modified. Only this report was written.
- **Method:** a 109-agent adversarial workflow — 12 independent dimension finders + a Codex cross-examiner, then **two diverse-lens skeptics per finding** (code-level + contract-level) each prompted to *refute*. Every headline claim was proved with an **independent oracle** (real Stata 19.5 via `stata-mp`, the `duckdb` CLI, and/or pyarrow), never a parqit-only round-trip. The synthesising auditor independently re-verified the S1/S2 silent-wrong-result findings at file:line.
- **Result:** 48 findings filed → **42 confirmed, 5 disputed, 1 refuted**. 2 are S1, ~13 are S2; 23 are silent-wrong-results (rc 0, wrong data).

> **Nota (PT):** Este relatório é uma auditoria adversarial independente, escrita para o Codex estudar (espelha o relatório que o Codex escreveu para o Claude). O corpo está em inglês para precisão técnica e para casar com o código e com o relatório anterior do Codex. As quatro findings do Codex (C01–C04) já foram tratadas no *working tree*; este documento confirma isso e apresenta **defeitos novos e mais profundos** que o Codex não viu — em particular dois resultados silenciosamente errados de severidade alta (`merge` com chaves em falta; `reshape long` com colisão de prefixo) e um conjunto de divergências semânticas do tradutor de expressões.

---

## 1. Executive summary

The current tree is materially stronger than any prior snapshot, and the maintainer has clearly worked through Codex's audit: **C02, C03 and Codex's test-runner gap are fixed; C01 is resolved by an explicit contract clarification; C04 is mitigated with a documented opt-out** (see §2). I confirmed all of this against the code.

But this audit is genuinely adversarial, and parqit is *not* defect-free. The headline is a cluster of **silent wrong results** — rc 0 with incorrect data — that none of the existing tests and none of Codex's findings touch:

1. **`merge`/`joinby` on a missing/empty/NaN join key silently mismatches** (TT-1, S1/S2). The out-of-core JOIN compares raw Parquet values with `IS NOT DISTINCT FROM` and never normalises them to Stata's missing/empty equivalence — while parqit's *own read path* does. So an out-of-core `parqit merge` disagrees with native Stata, with parqit `mergein`, and with parqit `collect`+merge on the same data whenever one side encodes a missing key as NULL/`''`/NaN and the other as parqit's form. **This is exactly the pq finding-11 / brief-§6 missing-key hazard, made cross-tool.** Bites hard on pandas/pyarrow Parquet (missing floats = NaN).
2. **`reshape long` with a stub that is a prefix of another column fabricates rows and corrupts the result** (RESHAPE-1, S1). Stub `inc` silently absorbs `income` as suffix `ome`; 6 correct rows become 9 wrong ones.
3. **The expression translator diverges from real Stata in ~9 distinct, oracle-proven ways** (XLAT-1…9, mostly S2): `string()`/`strofreal()` emits `CAST … AS VARCHAR` not `%9.0g`; `substr()`/`strpos()` are *character*-based while Stata is *byte*-based; `^` is right-associative; `cond()`/`&`/`|`/`!`/`inrange()`/`mod()` mishandle missing. Several unit tests *codify the wrong answer*.
4. **A finite Parquet `DOUBLE` ≥ Stata's missing sentinel (8.99e+307) is silently read as missing** (TYPE-1, S2). Astronomically rare trigger, but a contract violation ("never silently nulls a value").
5. **`collapse (count)`/`firstnm`/`lastnm` on string columns count empty strings as present** (COLLAPSE-1 / PARITY-1, S2), where Stata errors `r(109)` or skips them.
6. **The eager `parqit use` path crashes on UUID columns** (`strlen(UUID)` binder error) while the lazy collect path succeeds — the two documented materialisers disagree on the same file (STR-1, S2).

Release/process hygiene also has two concrete, verified problems: **v21–v26 hardcode `/home/mangelo` paths** (so the newest correctness tests run nowhere but the maintainer's box) **and commit a private data path with a person's name** (REL-2, S2); and **CMake says `0.1.3` but no `v0.1.3` git tag exists** (latest is `v0.1.2`) with no tag↔version guard in the release job (REL-3, S3).

**Top of the action list (silent-wrong-result first):** TT-1, RESHAPE-1, the XLAT cluster (esp. XLAT-2 byte-vs-char and XLAT-1 `string()`), COLLAPSE-1/PARITY-1, STR-1, TYPE-1; then REL-2/REL-3 before any tag.

What is **provably solid** is documented in §6 — the type map's headline charter hazards, the atomicity/loud-error machinery, the parallel-fill race-safety, the hex-codec request channel, and the metadata round-trip for parqit-written files are all genuinely well built. Spend review effort on the boundaries (foreign Parquet, multibyte text, missing-value semantics, cross-tool merges), not on those cores.

---

## 2. Status of Codex's prior findings in the current working tree

I re-examined every Codex finding and "important non-finding" against the code on disk.

| Codex id | Codex severity | Verdict now | Evidence |
|---|---|---|---|
| **C01** extended missings not lossless | S2 | **Confirmed but S3 (doc tension), not S2** | [plugin_io.cpp:1583-1592](src/plugin/plugin_io.cpp#L1583-L1592) writes every Stata missing (incl. `.a`–`.z`, detected `d > SV_missval`) as a plain NULL, and the loss *is* warned. [ASSUMPTIONS.md #13](ASSUMPTIONS.md) now states per-cell identity is the deliberate v1 contract (brief only requires metadata survival). The residual defect is purely documentation: [README.md:41](README.md#L41) "**Lossless Stata round-trips**" and [README.md:303](README.md#L303) "`parqit → parqit` is lossless" are unqualified, contradicting the honest Limitations note three lines later ([README.md:317](README.md#L317)). **Fix: qualify the two headline "lossless" claims.** |
| **C02** Parquet NULL → all-missing byte | S2/S3 | **Outdated / resolved** | The `DUCKDB_TYPE_SQLNULL → byte` case was removed; SQLNULL now drops loudly via the default branch ([typemap.cpp:234-259](src/engine/typemap.cpp#L234-L259)); `Transfer::Null` deleted from `fill_column`; [plugin_view.cpp:163](src/plugin/plugin_view.cpp#L163) mirrors it; `test_typemap` flipped to assert `dropped==true`. **But see META-3 (§5): the *symptom* survives via a different mechanism** — an all-null *typed* column loads as a silent all-missing `byte`, and the SQLNULL guard is dead code for real Parquet. |
| **C03** version/date drift | S3 | **Outdated / resolved** | CMake/ado/sthlp/README/pkg all read `0.1.3` / `14jun2026` / `20260614`; CHANGELOG has exactly one `[Unreleased]`; new [tests/release_lint.sh](tests/release_lint.sh) enforces it and is wired as a CI release gate (`needs: [lint, build]`). **But the lint has blind spots — see REL-3 (tag axis) and REL-4 (changelog body).** |
| **C04** parallel fill portability | S3 | **Agree (still real)** | Default-on for ≥50k rows still stands; the only change is a documented opt-out `PARQIT_FILL_THREADS=0` ([README "Tuning the read"](README.md)). CI still cannot run Stata, so SPI-store reentrancy is unproven off Linux. **THREAD-2 deepens this: the parqit-owned queue/abort logic has no race-detector coverage on any OS.** |

**Codex's "important non-findings" all still hold** — I re-verified each in the current code: the per-promotion `_data` bridge (`PARQIT_OPENDATA_SEQ`), the `reshape long` duplicate-`i()` guard ([plugin_view.cpp:1257-1277](src/plugin/plugin_view.cpp#L1257-L1277)), `misstable` `r(n_complete)`, `tabulate row col`, `save, chunk()` → `ROW_GROUP_SIZE`, and the `stata_call` exception boundary. I additionally confirmed `merge` enforces its 1:1/m:1/1:m uniqueness contract up front ([plugin_view.cpp:1092-1112](src/plugin/plugin_view.cpp#L1092-L1112)) and uses NULL-safe `IS NOT DISTINCT FROM` joins — which is correct *as far as it goes* but is exactly why TT-1 (§4) is subtle.

---

## 3. Findings table (47 actionable; 1 refuted)

Severity is the post-verification consensus of the two skeptics (original→corrected noted in text where they split). `swr` = silent wrong result (rc 0, wrong data).

| ID | Sev | swr | Area | Title |
|---|---|---|---|---|
| **TT-1** | **S1/S2** | ✔ | merge | Out-of-core merge/joinby silently mismatch NULL/`''`/NaN keys (no Stata-missing normalisation) |
| **RESHAPE-1** | **S1** | ✔ | reshape | `reshape long` stub-prefix collision fabricates rows / wrong j |
| TYPE-1 | S2 | ✔ | typemap | Finite `DOUBLE` ≥ 8.99e+307 silently read as Stata missing |
| XLAT-1 / PARITY-2 | S2 | ✔ | expr | `string()`/`strofreal()` = `CAST AS VARCHAR`, not `%9.0g` (test codifies wrong "2.0") |
| XLAT-2 | S2 | ✔ | expr | `substr()`/`strpos()` are char-based; Stata is byte-based (corrupts multibyte; negative-start can return empty) |
| XLAT-3 | S2 | ✔ | expr | `^` is right-associative; Stata is left-associative (`2^3^2` → 512 vs 64) |
| XLAT-4 | S2 | ✔ | expr | `cond(c,a,b)` returns NULL on missing `c`; Stata returns the TRUE branch (test codifies wrong result) |
| XLAT-5 | S2 | ✔ | expr | `&` `|` `!` yield NULL on missing operand; Stata treats missing as TRUE — not fixable via `statamissing` |
| COLLAPSE-1 / PARITY-1 | S2 | ✔ | collapse | `(count)`/`firstnm`/`lastnm` on strings count `''` as present; Stata errors `r(109)`/skips |
| RESHAPE-2 | S2 | ✔ | reshape | An `i()` var whose name is a stub prefix is consumed as a stub and dropped; breaks final ORDER BY |
| STR-1 | S2 | – | strings | UUID column crashes eager `parqit use` (`strlen(UUID)`); lazy collect succeeds → two paths disagree |
| TT-2 | S2 | ✔ | merge | m:m using-side `row_number()` has no `ORDER BY` → engine-defined pairing, breaks "reproduces Stata exactly" |
| REL-2 | S2 | – | release | v21–v26 hardcode `/home/mangelo`, ignore runner args; commit a private data path (`…/BigData/Paulo/…`) |
| REL-1 | S2/S3 (disputed) | – | release | Released Linux `.so` can dynamically link libstdc++/libgcc; no `ldd`/`readelf` gate; build stays rc 0 |
| TYPE-2 | S3 | ✔ | typemap | Save narrows `%tm`/`%td`-formatted values via unchecked `static_cast<int32_t>` → silent wrap |
| XLAT-6 | S3 | ✔ | expr | `mod(x,y<0)` returns a value; Stata returns missing |
| XLAT-7 | S3 | ✔ | expr | `inrange(x,.,hi)` returns FALSE; Stata treats missing lower as −∞ (excludes every row) |
| XLAT-9 | S3 | ✔ | expr | `gen d=(x==k)` yields NULL for missing x **even in `statamissing`** — partially false README promise |
| INJ-1 | S3 | ✔ | protocol | Untrusted value-label **value** is the one non-hex response field; a `|`/newline in foreign `parqit.vallabs` corrupts metadata, rc 0 |
| META-1 | S3 | – | metadata | Foreign `parqit.*` char/value-label name that is not a legal Stata identifier aborts the whole load (`r(3300)`) |
| META-3 | S3 | ✔ | metadata | All-null *typed* column silently loads as all-missing `byte`; SQLNULL drop guard is dead code (the live C02 mechanism) |
| LOC-1 | S3 (latent) | – | locale | Numeric→SQL literals use the global C locale; comma-decimal locale breaks `collapse (median)`/`sample` (loud) |
| REL-3 | S3 | – | release | Release job never checks git-tag == project version; CMake `0.1.3` but newest tag is `v0.1.2` |
| THREAD-1 | S3/S4 | ✔ | strL | strL sidecar header obs field is 10 digits → corrupts strL cells at ≥10^10 rows (serial+parallel) |
| XLAT-8 | S4 | ✔ | expr | `_n`/`_N` markers substring-matched → a string literal containing `__PARQIT_ROW__` is corrupted |
| ATOM-1 | S4 (disputed) | – | atomicity | Partitioned save isn't atomic; a mid-write failure leaves a partial tree and then blocks all retries |
| ATOM-2 | S4 | – | atomicity | `view_save` / `parqit open _data` path drops the lossy-conversion notes that in-memory `parqit save` emits |
| ATOM-3 | S4 | – | atomicity | Orphaned DuckDB temp table + stale prepared-read state when a collect aborts before `use_fetch` |
| THREAD-2 | S4 | – | threading | Parallel queue/abort/reduce logic has no C++ unit/TSan coverage; only a Linux-only Stata test |
| THREAD-3 | S4 | ✔ | threading | `SF_vstore`/`SF_sstore` return codes ignored → a per-cell store failure is undetectable |
| META-2 | S4 | – | metadata | Char target not remapped through the sanitiser → chars on a renamed foreign column silently dropped on re-save |
| META-4 | S4 | ✔ | metadata | Foreign value-label keys silently coerced via `strtoreal` (`1.5→1` collision, `'abc'→.`, `'1e3'→1000`) |
| STR-2 | S4 | ✔ | strings | Arrow string walker assumes int32 offsets + contiguous layout, unchecked; also a per-chunk 2 GB ceiling |
| SORT-1 | S4 | – | sort | `sort`/`gsort` rely on DuckDB's *default* NULL ordering, not explicit `NULLS LAST` |
| RESHAPE-3 | S4 | – | reshape | `reshape wide` builds raw `stub+jvalue` names; negative/non-name j-values → invalid names Stata refuses |
| RESHAPE-4 | S4 | – | reshape | reshape-long stub metadata taken from the lexicographically-smallest suffix, not data order |
| LOC-2 | S4 | ✔ | locale | `sample <share>` literal loses precision via `%.6f` (`0.0001234` path); histogram fixed, this not |
| LOC-3 | S4 (disputed) | – | locale | `memory_limit`/`tempdir` cache the value before the SET succeeds; a rejected value re-applies on reopen |
| REL-4 | S4 | – | release | `release_lint` misses duplicated `### Added` block in the 0.1.3 section; only counts headings |
| TT-3 | S4 | – | merge | Generated `_merge` lacks Stata's value labels (1 master / 2 using / 3 matched) |
| PARITY-3 | S3/S4 | – | parity | PARITY doc's "verified" `merge, update/replace` emulation is non-functional as written |
| PARITY-4 | S4 | – | parity | `relaxed` metadata fast-path's documented safety invariant is false (correct only by NULL accident) |
| PARITY-5 | S4 | – | parity | `parqit describe` returns `r(n_cols)`, not pq's `r(n_columns)` — drop-in r() surface differs |
| PARITY-6 | S4 | ✔ | parity | `real('inf')`/`real('nan')` keep inf/nan; Stata `real()` returns missing |
| INJ-2 | S4 (disputed) | – | sql | reshape-wide interpolates data-derived numeric j-values raw (robustness gap, not injection) |
| **META-5** | **refuted** | – | metadata | (View-collect metadata overlay by position) — guarded by a size-equality check; not a defect |

---

## 4. High-severity findings (S1/S2) — detail

### TT-1 — `merge`/`joinby` silently mismatch missing/empty/NaN keys (S1/S2, swr)

**Where:** [view.cpp:727](src/engine/view.cpp#L727), [:769-771](src/engine/view.cpp#L769-L771), [:924](src/engine/view.cpp#L924) (the `IS NOT DISTINCT FROM` join conditions); [plugin_view.cpp:157](src/plugin/plugin_view.cpp#L157) (`boundary_for` reads VARCHAR/numeric raw); [v14_join_missing_keys.do:60](tests/verify_suite/v14_join_missing_keys.do#L60).

**What.** The merge/joinby/m:m join compares **raw** boundary-cast key columns: `__m.key IS NOT DISTINCT FROM __u.key`. That correctly matches NULL↔NULL — but it never normalises keys to Stata's missing/empty equivalence. Stata has no string NULL and no NaN: parqit's own read path turns a Parquet NULL string into `""` and a NaN into `.`. So when a key is encoded as **NULL/`''`/NaN on one side and parqit's form on the other**, `'' IS NOT DISTINCT FROM NULL` = FALSE and `NaN IS NOT DISTINCT FROM NULL` = FALSE → no match, even though native Stata, parqit `mergein`, and parqit `collect`+merge all treat them as equal.

**Oracle proof (real Stata + parqit + pyarrow).** master = pyarrow file, key `g` NULL on one row; using = pyarrow file with `g=''`. Native `merge m:1 g`: 3 obs, the empty-key row **matched** (`_merge==3`). parqit out-of-core `parqit merge m:1 g`: 4 obs, the rows are split `_merge==1` and `_merge==2` — **never matched**. rc 0 both times. The numeric NaN-vs-NULL case reproduces identically at the SQL level.

**Why it matters.** Wrong `_merge` codes and wrong matched/unmatched counts on real data. **The common trigger is pandas/pyarrow Parquet, where missing floats are stored as NaN** — merging such a file against a parqit-written (NULL) file silently loses every missing-key match. `v14` passes only because it round-trips *both* sides through parqit, so both are `''`/NULL — it never exercises the cross-tool asymmetry it claims to cover. This is precisely pq finding 11 / brief §6's "Stata matches equal missing keys", reintroduced at the Parquet manipulation layer.

**Severity.** Skeptics split S1 vs S2. I rate it **S2 (high), trending S1 for pandas-origin data.** swr, oracle-proven.

**Fix.** Normalise keys inside the join comparisons, symmetrically on `__m` and `__u` (and the m:m spine UNION): `nullif(k,'')` for strings, `CASE WHEN isnan(k) THEN NULL ELSE k END` for floats. Extend v14 with a pyarrow-written side carrying genuine NULL strings and NaN numerics.

### RESHAPE-1 — `reshape long` stub-prefix collision fabricates rows (S1, swr)

**Where:** [view.cpp:966-983](src/engine/view.cpp#L966-L983), [:1012-1024](src/engine/view.cpp#L1012-L1024); [plugin_view.cpp:1278](src/plugin/plugin_view.cpp#L1278).

**What.** `reshape_long` discovers suffixes by a pure prefix test (`c.name.compare(0, st.size(), st) == 0`). With stub `inc` and columns `inc1 inc2 income`, `income` is read as stub `inc` + suffix `ome`. The balance check passes (every stub has every suffix), `jnum` flips false (suffix `ome` is non-numeric) so `j` becomes a **string** column, and the `i()`-uniqueness pre-check still passes. **Oracle (duckdb replaying parqit's exact UNION-ALL):** parqit yields **9 rows** including a bogus `year='ome'` group; real `stata-mp reshape long inc, i(id) j(year)` yields **6 rows** with `j={1,2}` and `income` carried correctly. rc 0 in both — data differs.

**Why it matters.** A user reshaping a file with a stub that is a prefix of an unrelated variable gets corrupt long data (fabricated j-group, duplicated values, string j) with no error, contradicting README's positioning of `parqit reshape` as a drop-in for Stata's `reshape`.

**Fix.** Match Stata: a stub member's "suffix" must be a valid j-value shared by *all* stubs; require numeric-only suffixes when j is numeric; treat non-conforming prefix matches as carried constants. Exclude `i()`/`j()` vars from stub discovery (also fixes RESHAPE-2).

### XLAT cluster — the expression translator (S2–S3, swr)

The translator is a real recursive-descent parser and is sound on several hard points (see §6). But it diverges from real Stata in nine oracle-proven ways. **Two unit tests actively codify wrong answers** (`test_exprtrans.cpp:102` asserts `string(x)=="2.0"`; `:123-125` asserts `cond(x>1,7,8,9)=="9"` where Stata gives 7) — so the suite *masks* these rather than catching them.

- **XLAT-1 / PARITY-2 (S2):** `string()`/`strofreal()` → `(CASE WHEN x IS NULL THEN '.' ELSE CAST(x AS VARCHAR) END)` ([exprtrans.cpp:873-881](src/engine/exprtrans.cpp#L873-L881)). Stata's default is `%9.0g`. `string(42)`→`"42.0"` not `"42"`; `string(1e7)`→`"10000000.0"` not `"1.00e+07"`; `string(1/3)`→`"0.3333333333333333"` not `".3333333"`. **Breaks string keys built from numbers → silently wrong merges/joins.** (Bites only DOUBLE/FLOAT-typed columns — integers `CAST` cleanly — but Stata's default numeric is double.) Fix: implement a `%9.0g`-equivalent formatter; correct the test.
- **XLAT-2 (S2):** `substr()`/`strpos()` map to DuckDB's **character**-based functions ([exprtrans.cpp:842-862](src/engine/exprtrans.cpp#L842-L862)); Stata is **byte**-based (only `usubstr`/`ustrpos` are char-based, and parqit implements neither). `strpos("héllo","l")` = 3 (parqit) vs 4 (Stata); `substr("héllo",2,2)` = `"él"` vs `"é"`. Worse, the negative-start branch feeds byte-based `strlen()` into char-based `substr()`, so `substr("héllo",-1,1)` returns `''` (parqit) vs `"o"` (Stata). Internally inconsistent (parqit's `strlen` *is* byte-based) and contradicts the code's own "lengths are BYTES, like Stata" comment. Fix: byte-accurate `substr`/`strpos`; reserve char semantics for `usubstr`/`ustrpos`.
- **XLAT-3 (S2):** `^` is right-associative ([exprtrans.cpp:487-502](src/engine/exprtrans.cpp#L487-L502) recurses into `unary()` for the exponent); Stata is left-associative. `2^3^2` → 512 (parqit) vs 64 (Stata). The help explicitly promises "arithmetic with Stata precedence". Fix: loop on `Caret` in `power()`.
- **XLAT-4 (S2):** 3-arg `cond(c,a,b)` returns NULL when `c` is missing ([exprtrans.cpp:764-780](src/engine/exprtrans.cpp#L764-L780)); Stata returns the TRUE branch (missing is nonzero). The 4-arg form also diverges for a *comparison* condition that evaluates to SQL-NULL. Not fixable via `statamissing` (root cause is `as_bool`). Fix: treat a missing condition as TRUE in the 3-arg form; correct the test.
- **XLAT-5 (S2):** `&`/`|`/`!` propagate NULL on a missing operand ([exprtrans.cpp:320-325](src/engine/exprtrans.cpp#L320-L325) `as_bool` = `((v)<>0)`); Stata treats missing as TRUE: `(.&1)=1`, `(.|0)=1`, `(!.)=0`. So `keep if (a|b)` silently drops rows Stata keeps. `as_bool` ignores the `statamissing` flag, so **there is no user remedy**. Fix: coerce `as_bool(v)` = `COALESCE((v)<>0, TRUE)` (record the decision in ASSUMPTIONS, since it bends the literal `&`→`AND` mapping).
- **XLAT-6 (S3):** `mod(7,-3)` → −2 (parqit) vs missing (Stata); `mod(7,0)` → NaN vs missing. Sibling `sqrt`/`round` already have such guards; `mod` lacks one. Fix: `CASE WHEN (b)<=0 THEN NULL ELSE … END`.
- **XLAT-7 (S3):** `inrange(x,.,hi)` (open lower interval, a documented idiom) returns FALSE for every non-missing x ([exprtrans.cpp:781-789](src/engine/exprtrans.cpp#L781-L789) reuses the literal-missing relational path); the upper-bound case is correct by accident. Fix: special-case missing bounds → drop the constraint.
- **XLAT-9 (S3):** `gen d=(x==k)` yields NULL for missing x **even in `statamissing` mode** ([exprtrans.cpp:380-401](src/engine/exprtrans.cpp#L380-L401), value-context wrap at [:982-997](src/engine/exprtrans.cpp#L982-L997)); Stata always yields 0/1. Ordering ops *are* fixed by `statamissing`, so the gap is specifically `==`/`!=` in value context — contradicting [README:313-316](README.md#L313-L316)'s "every comparison where it matters". Fix: collapse NULL→0/1 in value context under `statamissing`; tighten the README wording.
- **XLAT-8 (S4):** `_n`/`_N` are carried as the sentinel strings `__PARQIT_ROW__`/`__PARQIT_NROWS__` and substituted by an unguarded, non-literal-aware `find()`/replace ([view.cpp:147-158](src/engine/view.cpp#L147-L158)). A string literal containing that text is corrupted and spuriously activates the row-context machinery. Fix: carry `_n`/`_N` as an AST flag, or substitute only outside string literals.

### TYPE-1 — finite `DOUBLE` ≥ missing sentinel silently read as missing (S2, swr)

**Where:** [plugin_io.cpp:743-753](src/plugin/plugin_io.cpp#L743-L753) (and the `Float32` twin at 737). The `Float64` fill is `SF_vstore(…, (isnan||isinf) ? SV_missval : d)` — the **only** guards are NaN/Inf. `SV_missval` = 8.98846567431158e+307 (Stata's `.`); the `.a`–`.z` band occupies the doubles above it to `DBL_MAX`. A finite Parquet double in `[8.99e+307, 1.797e+308]` is stored verbatim and Stata reads it as missing (`.`/`.z`). The save path *does* guard this band ([plugin_io.cpp:1584](src/plugin/plugin_io.cpp#L1584)); the read path doesn't — a real read/write asymmetry. v15 deliberately tests `8.9e307` (just *under* the sentinel) and never above. Reachable only via a genuine DOUBLE source (FLOAT/DECIMAL/UBIGINT/HUGEINT all sit below the sentinel).

**Why it matters.** Contradicts README's "never silently nulls a value" and its closed list ("NaN→missing, ±Inf→missing *with a note*") — this case gets no note. Trigger is astronomically rare in real data (hence S2, not S1), but it is a genuine silent corruption the charter says must be impossible. Fix: on the read path, treat finite `d ≥ SV_missval` as out-of-range — store `SV_missval` with a loud per-column note (like Inf) or refuse; add an above-sentinel test.

### COLLAPSE-1 / PARITY-1 — `(count)`/`firstnm`/`lastnm` on strings (S2, swr)

**Where:** [view.cpp:410](src/engine/view.cpp#L410), [:424-435](src/engine/view.cpp#L424-L435). `collapse` explicitly exempts `count` (and allows `first/last/firstnm/lastnm`) from the numeric-required check, compiling to `count(strvar)` / `arg_min`/`arg_max`. DuckDB counts non-NULL, but parqit's own string contract is `NULL == ''` ([plugin_view.cpp:1535](src/plugin/plugin_view.cpp#L1535)), so empty strings are counted as present. **Oracle:** `['a','','b',NULL,'']` → parqit `count`=4; Stata-nonmissing=2, and real `collapse (count) cs=s` **errors `r(109)`** (string not allowed). `firstnm`/`lastnm` can return `''` where Stata's `*nm` skip missing. Fix: treat `''` as missing for string count/firstnm/lastnm (`count(*) FILTER (WHERE coalesce(ref,'')<>'')`), or reject and document the extension.

### RESHAPE-2 — `i()` var consumed by a stub (S2, swr)

**Where:** [view.cpp:1004-1007](src/engine/view.cpp#L1004-L1007), [:1045-1047](src/engine/view.cpp#L1045-L1047). With stub `x` and i-var `x2`, `x2` matches the prefix test, lands in `stubcols`, is dropped from output, and the materialisation `ORDER BY "x2"` then references a missing column. Fix: error (or exclude) when an `i()`/`j()` var collides with stub discovery — same root cause as RESHAPE-1.

### STR-1 — UUID crashes eager `parqit use`; lazy path succeeds (S2)

**Where:** [plugin_io.cpp:350](src/plugin/plugin_io.cpp#L350), [:387-388](src/plugin/plugin_io.cpp#L387-L388). The observed-range pass emits `max(strlen(ref))` on the **raw** source column, never `p.cast_sql`. [typemap.cpp:228-233](src/engine/typemap.cpp#L228-L233) maps UUID to a string with `cast_sql = CAST(ref AS VARCHAR)` and `needs_strlen=true`, but `strlen(UUID)` is a DuckDB **binder error**. Proven in Stata 19.5: `parqit use using uuid.parquet, clear` → rc 920; the lazy `parqit use` + `parqit collect, clear` on the **same file succeeds** (loads `str36`), because `boundary_for` casts first. The two documented materialisers disagree — violating the v21 byte-identical invariant. Fix: size strings over `p.cast_sql.empty() ? ref : p.cast_sql`.

### TT-2 — m:m using-side pairing is engine-defined (S2, swr)

**Where:** [view.cpp:713-719](src/engine/view.cpp#L713-L719). The master row_number appends `order_by_sql()`; the **using** row_number has **no `ORDER BY`**. README:321 promises "merge m:m reproduces Stata's sequential pairing exactly". Currently latent (DuckDB happened to preserve scan order for small inputs) but not guaranteed across versions/parallelism. Fix: append a deterministic `ORDER BY` to the using-side window (or document m:m needs both sides pre-sorted).

### REL-2 — newest tests are non-portable + a private path is committed (S2)

**Where:** [v21_collect_passthrough_sizing.do:18](tests/verify_suite/v21_collect_passthrough_sizing.do#L18) & [:43](tests/verify_suite/v21_collect_passthrough_sizing.do#L43), v22–v26, [run_stata.sh:44](tests/run_stata.sh#L44). I verified: v01–v20 declare `args repo plugin` and honour the runner-passed paths; **v21–v26 declare none** and hardcode `local repo "/home/mangelo/Documents/GitHub/parqit"` + a `build/dev` plugin path. On any other machine they can't even load parqit, so the newest correctness tests (passthrough sizing, date-overflow floor, relaxed union, multiformat, mergein/appendin, perf tips) silently don't run — and CI never runs Stata, so nothing catches it. Separately, [v21:43](tests/verify_suite/v21_collect_passthrough_sizing.do#L43) commits `/home/mangelo/Documents/BigData/Paulo/main_95_21_ready.parquet` (a private dataset path including a person's name; `capture confirm file` only makes it *skip*). Fix: give v21–v26 `args repo plugin`; replace the BigData reference with a generated fixture.

### REL-1 — released Linux plugin may not be self-contained (S2/S3, disputed)

**Where:** [CMakeLists.txt:132-146](CMakeLists.txt#L132-L146), [build.yml:46-49](.github/workflows/build.yml#L46-L49). `-static-libstdc++ -static-libgcc` is applied only if a `check_cxx_source_compiles` probe succeeds; otherwise CMake prints a *warning* and links dynamically. The AlmaLinux 8 CI step installs `gcc-toolset-12` but **not** the static archive subpackage, and there is **no `ldd`/`readelf`/`otool` assertion** that the shipped `parqit.plugin` has no libstdc++/libgcc_s `NEEDED` entry. So a green release can ship a `.so` that fails to `dlopen` on the EL-family HPC nodes the container exists to support — the headline portability promise ([CMakeLists.txt:133](CMakeLists.txt#L133)) silently violated. Skeptics split S2/S4 (build-config, not data). Fix: install the static archive, make the missing-static case a hard error for release builds, add a `readelf` gate in CI.

---

## 5. S3/S4 findings — grouped, with file:line and one-line fix

**Type / save path**
- **TYPE-2 (S3, swr):** save picks on-disk width from the *display format* not storage type ([plugin_io.cpp:1503-1518](src/plugin/plugin_io.cpp#L1503-L1518)) and narrows with an unchecked `static_cast<int32_t>(nearbyint(d))` ([:1621-1630](src/plugin/plugin_io.cpp#L1621-L1630)); only fractional parts are flagged, not out-of-range integers. A `double = 5e9` formatted `%tm` is written wrapped (−2147483648), rc 0 — proved end-to-end with a pyarrow oracle. parqit can't manufacture this itself (loaded period columns are in-range), so the trigger is a hand-attached format; still a silent on-disk corruption. Fix: bound-check before the cast; widen to BIGINT when needed.

**Protocol / injection / metadata boundary**
- **INJ-1 (S3, swr):** the hex-codec promises "no quoting bug can exist", but the value-label **value** (`e[0]`) is emitted as the lone **plain** (non-hex) response field ([plugin_io.cpp:466](src/plugin/plugin_io.cpp#L466)), read straight from untrusted `parqit.vallabs` with no numeric validation. A `|`/newline in a foreign file's metadata silently corrupts the field-delimited protocol → wrong/lost value labels, rc 0 ([parqit.ado:2295](src/ado/p/parqit.ado#L2295)). Fix: hex-encode it like every other field, or validate it is a finite numeric and drop-with-message otherwise. (Sibling `stat`/`det`/`ts` records are the same class but currently emit only numeric stats, so not hostile-reachable yet.)
- **META-1 (S3):** foreign `parqit.*` char target/name or value-label name that isn't a legal Stata identifier (space, >32 chars) aborts the *entire* `parqit use` with opaque `r(3300)` ([parqit.ado:2301-2304](src/ado/p/parqit.ado#L2301-L2304)); the data itself loads fine, only the decorate step throws. Load is atomic (live data preserved). Fix: `capture` each metadata-restore and warn-and-skip; remap char targets through the sanitiser.
- **META-3 (S3, swr):** an all-null *typed* column (DuckDB surfaces a pyarrow `null` column as **INTEGER**, never SQLNULL) loads as an all-missing `byte` with **no note** ([typemap.cpp:264-272](src/engine/typemap.cpp#L264-L272)); an all-null int64 likewise collapses to `byte`, losing its type. **This is the live mechanism behind Codex C02 — the SQLNULL drop guard is dead code for real Parquet.** Fix: detect all-null in `refine_plan` and drop-with-message (or note); preserve the source type when known; reconcile the dead SQLNULL branch with a real test.
- **META-2 (S4):** char target never remapped through the sanitiser → chars/notes on a renamed foreign column silently dropped on a view re-save ([plugin_view.cpp:253-259](src/plugin/plugin_view.cpp#L253-L259)).
- **META-4 (S4, swr):** foreign value-label keys coerced via `strtoreal` (`1.5→1` collision, `'abc'→.`, `'1e3'→1000`) with no warning ([parqit.ado:2295](src/ado/p/parqit.ado#L2295)). parqit→parqit unaffected.

**Strings / threading**
- **THREAD-1 (S3/S4, swr):** the strL sidecar header obs field is 10 digits (`%010lld`) ([plugin_io.cpp:794-797](src/plugin/plugin_io.cpp#L794-L797)); the Mata reader parses by fixed offsets ([parqit.ado:2258-2264](src/ado/p/parqit.ado#L2258-L2264)). At ≥10^10 rows the field overflows → misparsed length/row, garbled strL cells. Stata-MP supports ~1.1 trillion obs. Extreme-scale, serial+parallel. Fix: widen to 13+ digits or length-prefix the record; guard loudly on overflow.
- **STR-2 (S4, swr):** the Arrow string walker hard-codes int32 offsets + `buffers[1]=offsets/buffers[2]=data` without inspecting the schema format ([plugin_io.cpp:780-811](src/plugin/plugin_io.cpp#L780-L811)). Correct only because DuckDB 1.5.3 defaults to `arrow_offset_size=REGULAR` and `produce_arrow_string_view=false`; a future pin or a per-chunk >2 GB string buffer silently corrupts every string. Fix: import the schema, assert format `'u'` and `n_buffers==3`, or pin the arrow options explicitly.
- **THREAD-2 (S4):** the parqit-owned queue/abort/reduce state machine has no C++ unit test and no TSan/ASan build anywhere ([CMakeLists.txt], `grep fsanitize` = 0); only the Linux-only V20 exercises it, and CI can't run Stata. Fix: factor the loop to be testable without Stata; add a TSan ctest target + a platform smoke step running V20 on release binaries.
- **THREAD-3 (S4, swr):** `SF_vstore`/`SF_sstore` return codes (an `ST_int` error code) are discarded in every `fill_column` branch ([plugin_io.cpp:709-811](src/plugin/plugin_io.cpp#L709-L811)); a per-cell store rejection (e.g. under `SD_SAFEMODE`) would leave the cell at its pre-created value with rc 0. Latent (upstream invariant checks make it unlikely). Fix: check the return and funnel into the existing abort path.

**Atomicity / resources**
- **ATOM-1 (S4, disputed):** the temp-then-atomic-rename discipline is single-file only ([plugin_io.cpp:588-606](src/plugin/plugin_io.cpp#L588-L606)); the partitioned branch ([:607-619](src/plugin/plugin_io.cpp#L607-L619)) writes directly to the final tree, so a mid-write failure leaves a partial tree that a reader treats as complete — and the next attempt is *refused* by the "never delete partition trees" guard. Loud at failure, but the on-disk artefact is partial and self-blocking. Fix: stage the partitioned output in a sibling temp dir and rename atomically.
- **ATOM-2 (S4):** `view_save` / `parqit open _data` performs the same lossy conversions (extended-missing→null, fractional date rounding) but emits **neither warning note** that the in-memory `save_data` path emits ([plugin_view.cpp:811-869](src/plugin/plugin_view.cpp#L811-L869)) — proven: identical payload, missing warnings. The view path is the product's headline workflow. Fix: carry the flags through and emit the same notes.
- **ATOM-3 (S4):** a collect that prepares a spillable `_parqit_collect_N` temp table but aborts before `use_fetch` orphans it for the DuckDB session's life ([plugin_view.cpp:724-731](src/plugin/plugin_view.cpp#L724-L731)). Bounded resource leak, loud failure. Fix: drop the prior prepared source in `set_prepared_read`, or add a `view_collect_abort`.

**Sort / locale**
- **LOC-1 (S3, latent):** numeric→SQL literals use the process C locale, not a forced C locale ([view.cpp:563](src/engine/view.cpp#L563) `std::to_string(amount)`, [view.cpp:355](src/engine/view.cpp#L355) `std::to_string(p)`, [exprtrans.cpp:268](src/engine/exprtrans.cpp#L268) `atof`); no `setlocale(LC_NUMERIC,"C")` anywhere. Under a comma-decimal locale (e.g. the maintainer's pt_PT), `collapse (median)`/percentiles and `sample <share>` emit comma literals that DuckDB **loudly** mis-parses (`Binder Error`/`Parser Error`) — proven with a compiled C++ probe + duckdb CLI. Latent because Stata pins `LC_NUMERIC=C`; v17 passes because it tests Stata's `set dp comma` display setting, not the OS locale. Fix: a locale-independent formatter (`std::to_chars`/C-locale `snprintf`).
- **LOC-2 (S4, swr):** `sample <share>` formats the fraction with `%.6f` ([view.cpp:563](src/engine/view.cpp#L563)) → `0.0001234567` becomes `0.000000` → 0 rows, rc 0. The histogram path was already fixed with 17-digit precision ([plugin_view.cpp:2231](src/plugin/plugin_view.cpp#L2231)); this one wasn't. Fix: use the same 17-digit/`to_chars` formatting.
- **LOC-3 (S4, disputed):** `set_memory_limit`/`set_temp_directory` cache the member before the `SET` succeeds ([session.cpp:78](src/engine/session.cpp#L78), [:83](src/engine/session.cpp#L83)), so a rejected value re-applies on the next reopen. Minor.
- **SORT-1 (S4):** `sort`/`gsort` emit no explicit `NULLS LAST` ([view.cpp:333-347](src/engine/view.cpp#L333-L347)); correctness rides on DuckDB's current default (which happens to match Stata's missing-last in every direction). Fix: emit explicit `NULLS LAST` on every key.

**Reshape (cosmetic) & release**
- **RESHAPE-3 (S4):** `reshape wide` builds raw `stub+jvalue` names; `j=-1` → `inc-1` (invalid identifier) where Stata refuses (`r(198)`); parqit relies on the downstream sanitiser to silently rename ([view.cpp:1098-1101](src/engine/view.cpp#L1098-L1101)). Fix: validate generated names, error like Stata.
- **RESHAPE-4 (S4):** reshape-long stub metadata (format/value-label) comes from the lexicographically-smallest suffix, not data order ([view.cpp:1036-1041](src/engine/view.cpp#L1036-L1041)). Cosmetic only (storage type is protected by `refine_plan`).
- **REL-3 (S3):** the release job fires on any `v*` tag and never checks tag == `project(parqit VERSION)` ([build.yml:76-114](.github/workflows/build.yml#L76-L114)); `release_lint` only checks *internal* coherence. Verified the repo is *in* this risk state: CMake says `0.1.3`, newest tag is `v0.1.2`. Fix: add a tag↔version guard in the release job.
- **REL-4 (S4):** `release_lint` misses the duplicated `### Added` block inside the 0.1.3 CHANGELOG section ([release_lint.sh:76-83](tests/release_lint.sh#L76-L83)); it only counts headings. Fix: reject duplicate `### <Type>` within one version section.
- **TT-3 (S4):** generated `_merge` has no value labels ([view.cpp:810-817](src/engine/view.cpp#L810-L817)); numeric values are correct, only `tabulate _merge` labels differ from native. Fix: attach the standard `_merge` label.

**pq / jrothbaum parity overclaims** (output is about *parqit*, validated against the rival source)
- **PARITY-3 (S3/S4):** [PARITY_parqit_vs_pq_claude.md §4.1](PARITY_parqit_vs_pq_claude.md#L97-L108) presents a "verified" `merge, update/replace` emulation, but `keepusing` matches using columns **by literal name** and parqit unconditionally drops an overlapping non-key using column (keeping master) with only a warning, and has no rename-on-merge facility ([view.cpp:662-688](src/engine/view.cpp#L662-L688)) — so the documented `keepusing(x_u)` step errors. Fix: add a real merge suffix/rename, or correct the doc to the actual multi-step path and drop "verified".
- **PARITY-4 (S4):** the `relaxed` metadata fast-path comment + [PARITY §3](PARITY_parqit_vs_pq_claude.md#L80-L82) justify trusting footer min/max by claiming a partially-present column has `count(stats) < count(*)` and falls back to a scan; **`parquet_metadata` only emits rows for columns that exist in each file**, so the guard passes for an absent column and the fallback never fires ([plugin_io.cpp:311-333](src/plugin/plugin_io.cpp#L311-L333)). Sizing stays correct only because NULLs can't widen the range — not the documented reason. No reproduced corruption today. Fix: state the true reason; compare against the *total* row-group count.
- **PARITY-5 (S4):** `parqit describe` returns `r(n_cols)`; pq returns `r(n_columns)` ([parqit.ado:1104-1105](src/ado/p/parqit.ado#L1104-L1105)). A pq-era script reading `r(n_columns)` silently gets empty. Fix: alias `r(n_columns)`.
- **PARITY-6 (S4, swr):** `real(s)` → `TRY_CAST(s AS DOUBLE)` ([exprtrans.cpp:885](src/engine/exprtrans.cpp#L885)) keeps `real('inf')`=inf, `real('nan')`=nan; Stata returns missing. Fix: map non-finite to NULL.

---

## 6. What is provably solid (where *not* to spend review effort)

A balanced adversarial audit must say what survived attack. The finders + skeptics confirmed these with oracles and could **not** break them:

- **Type map — headline charter hazards genuinely closed by design.** Period formats `%tm/%tq/%th/%ty/%tw/%tb/%tC` stay INTEGER/BIGINT period counts both directions (`2026m1`→792 as int32, oracle-checked); uint32 routes via BIGINT, uint64/hugeint/decimal via DOUBLE so values ≥2^31 never null; >2^53 integers carry a loud note; float32 finite values above ±1.70e38 widen to double with a note; TIME-of-day never arrives all-null; TIMESTAMP_TZ→UTC instant is correct (vendored build never loads ICU). The only adjacent holes are TYPE-1 (double sentinel) and TYPE-2 (save narrowing).
- **Atomicity & loud errors.** Top-level `try/catch` → rc 920 + `SF_error` ([parqit_plugin.cpp:141-186](src/plugin/parqit_plugin.cpp#L141-L186)); single-file saves are temp-then-atomic-rename with an independent row-count verify before rename (no `.parqit_tmp` leftover after a rejected save; unwritable-dir/dir-target/no-replace/unknown-codec all return nonzero rc with the target untouched); collect/use,clear stage into a temp frame and swap only on success (live data survives every failed load). A hypothesised round-half-even vs round-half-away divergence between the two save paths was **falsified** (both round identically).
- **Parallel fill is race-free by construction** on every path exercised: `duckdb_data_chunk_to_arrow` deep-copies into a self-contained `ArrowArray`; the result is read only by the single producer; row-range partitioning is disjoint and gap-free (8-worker fill straddling 2048-row chunk boundaries matched a pyarrow oracle cell-for-cell); no exception escapes a `std::thread`; thread count is bounds-checked (`strtol`, reject ≤0, clamp [1,1024]); the strL FILE is serialised by a mutex. **No silent-wrong-result in the threading itself** (the defects are THREAD-1's extreme-scale header and THREAD-2's test gap).
- **The hex codec closes the request direction.** Every user-originated string crosses the Stata↔plugin boundary as hex ([hexcodec.cpp] + the Mata twin), so no quoting bug can exist *into* the engine; `quote_ident`/`quote_literal` ([session.cpp:120-142](src/engine/session.cpp#L120-L142)) are textbook-correct (a column named `x"; DROP TABLE t; --` is read as data, oracle-checked); the expr translator resolves identifiers against the view schema and routes string literals through `quote_literal`, so `gen`/`replace`/`keep if` cannot inject SQL. The single response-direction hole is INJ-1 (the plain vlab value field).
- **Metadata round-trip is lossless for parqit-written files.** Variable/value labels (incl. labels on `.a`, keys at int32 extremes), notes, characteristics, dataset label, and `%fmt` formats all survive with quotes/pipes/UTF-8/newlines intact (every text field hex-encoded); the written file is standard Parquet readable by pyarrow/duckdb; the manifest is keyed by source name and applied by stata_name; `apply_meta_type` only ever *widens* storage. Defects are all on the **foreign-file** boundary (META-1…4).
- **String sizing is byte-correct.** The 2045/2046 `str#`/strL boundary is exact (oracle-checked: 2045B→`str2045`, 2046B→strL, an all-emoji 2044B value→`str2044` with the trailing 4-byte emoji intact, a 5 MB strL round-trips); multibyte values are never sized by char count nor split at a `str#` capacity; `NULL≡''` preserved; embedded-NUL truncation is *loud*; binary strLs refused loudly on save. (The expr-level byte/char bug XLAT-2 is separate from storage sizing.)
- **collapse/sort/sample core.** `mean`, `sd` (sample N−1), `count` (numeric), `min/max/sum/median/p##` match Stata exactly (duckdb oracle replayed against `stata-mp`, grouped, with missings); `gsort` missing-last matches Stata in every direction; `sample` is reproducible across runs and thread counts; `parqit sql` rebuilds the manifest from the actual result columns. (Defects: COLLAPSE-1 strings, SORT-1 latent NULL ordering.)
- **SSC packaging mechanics are correct.** `parqit.pkg` lists `parqit.ado`/`parqit.sthlp`, four per-OS plugin mappings matching the CI rename step, `h parqit.plugin`, and correctly omits `f parqit.pkg`; `plugin.map`/macOS symbol flags restrict exports to `stata_call`/`pginit` and hide DuckDB; `VERSIONS.md` pins match CMake (DuckDB 1.5.3 + SHA256). (Defects: REL-1 static-link gate, REL-2 test paths, REL-3 tag guard, REL-4 changelog.)

---

## 7. pq audit hazard-class coverage (the 14-finding charter)

parqit is, in most classes, **safer than pq** — but two pq classes are reintroduced at the cross-tool boundary and one has an adjacent hole.

| pq finding | parqit status |
|---|---|
| 01 save varlist family | Guarded (manifest by source name). |
| 02 renamed columns load all-missing | Guarded ([sanitize.cpp], v02). |
| 03 `%tm…` date corruption | **Fixed** on read; new adjacent **TYPE-2** on save (unchecked narrowing). |
| 04 chunk/partition data loss | Guarded; partitioned-save atomicity gap **ATOM-1**. |
| 05 hh:mm all-null time | Guarded (TIME never all-null). |
| 06 uint32 → missing | **Fixed**; adjacent **TYPE-1** (DOUBLE sentinel band) via a different mechanism. |
| 07 label option blanks values | Better than pq (numeric payload + labels preserved). |
| 08 save errors report success | Guarded (loud rc + verify; v08). |
| 09 `use,clear` destroys data on error | Guarded (atomic stage/swap; v09). |
| 10 duplicate column names | Guarded (`sanitize_unique`; v10). |
| 11 unsupported types load all-missing | Mostly fixed (LIST/STRUCT/… drop loudly); **META-3**: an all-null *typed* column still loads as a silent all-missing `byte`. |
| 12 internal-name clobber | Guarded (tempnames; v12). The **XLAT-8** marker leak is the lone adjacent hole. |
| 13 invalid `in()` | Guarded; note the **unsorted-view `keep in` determinism** caveat (§8). |
| 14 space in column name | Guarded (sanitiser + hex codec). |
| **(missing-key match)** | **TT-1 reintroduces this** for cross-tool merges (NULL/`''`/NaN vs parqit form) — the single most important new finding. |

---

## 8. Promise / claim audit

| Claim | Source | Verdict |
|---|---|---|
| "Lossless Stata round-trips" / "`parqit → parqit` is lossless" | [README:41](README.md#L41), [:303](README.md#L303) | **Overbroad** — extended-missing cell identity and (TYPE-2) over-range period values are not lossless; honestly disclosed in Limitations but the headline isn't qualified. |
| "never silently nulls a value on overflow" | README:278/284 | **False** for a finite DOUBLE ≥ sentinel (TYPE-1). |
| "arithmetic with Stata precedence (`^` is power)" | [sthlp:391-392](src/ado/p/parqit.sthlp#L391-L392) | **False** — `^` is right-associative (XLAT-3). |
| `statamissing` "emulates Stata's ordering in every comparison where it matters" | [README:313-316](README.md#L313-L316) | **Partially false** — `==`/`!=` in value context still yield NULL (XLAT-9); `&`/`|`/`!` never fixed (XLAT-5). |
| Stata functions incl. `substr`/`strpos` supported, "lengths are BYTES" | [sthlp:399](src/ado/p/parqit.sthlp#L399) | **Inconsistent** — `substr`/`strpos` are char-based (XLAT-2). |
| "merge m:m reproduces Stata's sequential pairing exactly" | README:321 | **Not guaranteed** — using-side pairing is engine-defined (TT-2). |
| "Stata-compatible `_merge`" | README:187 | True numerically; missing value labels (TT-3). |
| Missing keys match missing keys (Stata-faithful merge) | sthlp:219 | True for NULL↔NULL; **false** for cross-tool `''`/NaN (TT-1). |
| PARITY: `merge, update/replace` "Verificado" 2-step | PARITY §4.1 | **Non-functional as written** (PARITY-3). |
| PARITY: `relaxed` fast-path safety reasoning | PARITY §3 | **False reasoning** (correct by NULL accident) (PARITY-4). |
| `describe` is "Mesma sintaxe" as pq | PARITY §1 | `r(n_columns)` differs (PARITY-5). |
| Single self-contained `.so` for HPC | CMakeLists:133 | **Best-effort, unverified** in CI (REL-1). |
| pq hazard classes ported to verify tests | brief §9 | Mostly true; v14 gives **false coverage** of the missing-key contract (TT-1); v21–v26 don't run off the maintainer's box (REL-2). |

**Additional semantic caveat (not a formal finding):** `keep in N/M` on a lazy view with **no explicit `parqit sort`** uses `LIMIT/OFFSET` over an `ORDER BY`-less query ([view.cpp:540](src/engine/view.cpp#L540)), so "first N rows" is engine/parallelism-dependent — undefined row identity, unlike native Stata's current-order semantics. Inherent to lazy/columnar engines, but undocumented for `keep in`.

---

## 9. Test-suite gaps & recommended new tests

1. **`merge` cross-tool missing keys (TT-1):** a pyarrow-written using/master with genuine NULL strings and NaN numerics vs parqit's form; assert `_merge`/values match native Stata. (Today's v14 round-trips both sides through parqit and gives false coverage.)
2. **`reshape long` stub-prefix collision (RESHAPE-1)** and **i-var-as-stub (RESHAPE-2)** vs `stata-mp`.
3. **Expression translator vs real Stata** for `string()`/`strofreal()`, byte-based `substr`/`strpos` on multibyte text, chained `^`, `cond`/`&`/`|`/`!`/`inrange`/`mod` on missing — and **fix the two unit tests that codify wrong answers** (`test_exprtrans.cpp:102`, `:123-125`).
4. **TYPE-1:** a v15 case with a finite DOUBLE *above* the sentinel + an independent oracle. **TYPE-2:** a double-storage `%tm`/`%td` value out of int32 range.
5. **`collapse (count)`/`firstnm`/`lastnm` on strings** vs Stata (COLLAPSE-1).
6. **UUID via the eager `use` path** (STR-1) — assert `use` == `use`+`collect`.
7. **Foreign `parqit.*` metadata** with illegal char/label names (META-1), all-null typed column (META-3), pipe/newline in a value-label value (INJ-1), non-integer vlab keys (META-4).
8. **Make v21–v26 portable** (`args repo plugin`) and remove the private BigData path (REL-2); add a **tag↔version** guard in the release job (REL-3) and a **`readelf`/`ldd` self-containment** assertion (REL-1).
9. **TSan ctest target** for the parallel queue/abort/reduce loop + a **platform smoke** step running V20 on release binaries (THREAD-2, also Codex C04).
10. **Comma-decimal locale** test that runs the plugin under `LC_NUMERIC=pt_PT.UTF-8` and exercises `collapse (median)` + `sample <share>` (LOC-1/LOC-2).

---

## 10. Methodology & confidence

- **Fan-out:** 12 independent dimension finders (type map, expr translator, sanitiser/injection, atomicity, threading, two-table, manifest/metadata, strings, locale/session, collapse/reshape/sql/sample, release/packaging, jrothbaum comparison) + a dedicated Codex cross-examiner.
- **Verification:** every finding refuted by two diverse-lens skeptics (code-level + contract/semantics), each defaulting to "refuted". 42/48 survived both; 5 were downgraded/disputed; 1 (META-5) was refuted and excluded. Severities above are the post-verification consensus, with original→corrected noted where the two skeptics split (notably TT-1 S1↔S2, REL-1 S2↔S4, TT-2 upgraded to S2).
- **Oracles:** real Stata 19.5 (`stata-mp -b`), the `duckdb` CLI, pyarrow — used on every silent-wrong-result claim. The vendored DuckDB 1.5.3 + Arrow C-data headers were read for the threading and string-layout claims (not assumed from memory). The synthesising auditor independently re-confirmed TT-1, TYPE-1, XLAT-1, XLAT-2 at file:line, and verified REL-2/REL-3 directly (`git tag`, grep of v21–v26).
- **Not done:** no release artefacts were built in the AlmaLinux 8 container (REL-1 is reasoned from CMake/CI, not from a built binary); no Stata run on macOS/Windows (C04/THREAD-2 portability remains inherent); no destructive disk-full/permission/kill tests beyond the live probes noted; the translator was probed broadly but not exhaustively across every Stata function.
- **Refuted / non-defects:** META-5 (view-collect metadata overlay is guarded by a size-equality check); the save-path rounding-divergence hypothesis (falsified — both paths round identically); and all of Codex's "fixed" non-findings still hold (§2).

---

*This report is read-only analysis; no parqit source file was modified. Companion docs: Codex's `PARQIT_ADVERSARIAL_AUDIT_FOR_CLAUDE_20260614.md` (the prior audit re-examined in §2), `PARITY_parqit_vs_pq_claude.md` (its §3/§4.1 claims tested in PARITY-3/PARITY-4), and the pq correctness charter at `/home/mangelo/Documents/BPLIM_GitHub/pq_audit_2026-06-11` (mapped in §7).*
