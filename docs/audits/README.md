# Audit trail — the correctness evidence chain for parqit

parqit's correctness case is adversarial: independent audits (commissioned
from different AI agents, judged against independent oracles) attack the
package, every confirmed finding becomes a minimal repro and a pinned
regression test, and a release ships only when the whole chain is green.
This folder preserves that evidence verbatim — historical documents are not
edited after the fact, and some are in Portuguese (marked PT).

How findings become permanent: each confirmed defect gets a minimal repro in
[`audit_repro/`](../../audit_repro) and a dedicated invariant test in
[`tests/verify_suite/`](../../tests/verify_suite) (v27–v32 pin the 2026-06/07
adversarial passes); `tests/run_stata.sh` runs the full suite and
`tests/release_lint.sh` gates the release surfaces.

## Certification

| Date | Document |
|---|---|
| 2026-07-14 | [CERTIFICACAO_GO_GO_FIABILIDADE_DADOS_PARQIT_2026-07-14.md](CERTIFICACAO_GO_GO_FIABILIDADE_DADOS_PARQIT_2026-07-14.md) (PT) — the v0.1.22 GO-GO data-reliability certification linked from the README: scoped evidence, closed findings, residual risks, institutional-use conditions |
| 2026-08-23 | [2026-08-22/CERTIFICATION.md](2026-08-22/CERTIFICATION.md) (EN) — certification of the v0.1.28 release: the 2026-08-22 six-auditor adversarial audit (≈1,300 checks), 38 + 8 remediated items, independent adversarial verification, full Stata suite 97/97, unit 3,653/3,653, performance vs v0.1.27 within noise, outputs byte-identical |

## Adversarial audits and remediation

| Date | Document |
|---|---|
| 2026-06-12 | [PARQIT_AUDIT_REPORT.md](PARQIT_AUDIT_REPORT.md) — independent audit; regressions pinned in `tests/integration/t10_audit_fixes.do` |
| — | [parqit_adversarial_audit.md](parqit_adversarial_audit.md) — adversarial audit report |
| 2026-06-14 | [PARQIT_ADVERSARIAL_AUDIT_2026-06-14_f386b5b.md](PARQIT_ADVERSARIAL_AUDIT_2026-06-14_f386b5b.md) — adversarial audit at commit f386b5b |
| 2026-06-14 | [PARQIT_ADVERSARIAL_AUDIT_FOR_CLAUDE_20260614.md](PARQIT_ADVERSARIAL_AUDIT_FOR_CLAUDE_20260614.md) — findings handoff for Claude |
| 2026-06-14 | [PARQIT_ADVERSARIAL_AUDIT_FOR_CODEX_20260614.md](PARQIT_ADVERSARIAL_AUDIT_FOR_CODEX_20260614.md) — findings handoff for Codex |
| 2026-06-16 | [PARQIT_ADVERSARIAL_AUDIT_2026-06-16.md](PARQIT_ADVERSARIAL_AUDIT_2026-06-16.md) — adversarial audit |
| 2026-06-23 | [PARQIT_ADVERSARIAL_AUDIT_2026-06-23_CODEX.md](PARQIT_ADVERSARIAL_AUDIT_2026-06-23_CODEX.md) — Codex adversarial audit |
| 2026-07-03 | [AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-07-03.md](AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-07-03.md) (PT) — holistic adversarial audit |
| 2026-07-09 | [PARQIT_CODEX_HOLISTIC_AUDIT_2026-07-09.md](PARQIT_CODEX_HOLISTIC_AUDIT_2026-07-09.md) — Codex holistic audit |
| 2026-07-14 | [AUDITORIA_ADVERSARIAL_FIABILIDADE_DADOS_PARQIT_2026-07-14.md](AUDITORIA_ADVERSARIAL_FIABILIDADE_DADOS_PARQIT_2026-07-14.md) (PT) — data-reliability adversarial audit behind the v0.1.22 blockers |
| 2026-07-14 | [PARQIT_AUDIT_REMEDIATION_2026-07-14.md](PARQIT_AUDIT_REMEDIATION_2026-07-14.md) — remediation record for the 2026-07-14 audit |
| 2026-08-08 | [AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-08.md](AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-08.md) (PT) — holistic adversarial audit of v0.1.23: 3 confirmed defects (F1–F3), oracle-first native verification; fixes specified in the matching prompt below |
| 2026-08-08 | [AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md](AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md) (PT) — adversarial audit of the public help against the executed v0.1.24: PASS with residual risks, 6 documental corrections applied (F1–F7) and 5 runtime message defects recorded (D1–D5) |
| 2026-08-08 | [PARQIT_HELP_AUDIT_REMEDIATION_2026-08-08.md](PARQIT_HELP_AUDIT_REMEDIATION_2026-08-08.md) (PT) — remediation record for D1–D6: refusals, message attribution, bridge quietness; pinned by `v67` |
| 2026-08-09 | [AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-09.md](AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-09.md) (PT) — post-remediation holistic audit of the v0.1.24 worktree: GO; D1–D7 verified in code and by execution; ~110 fresh native-differential probes (expressions, reshape leading zeros, hostile column names, caps); 2 minor documentation findings (A1–A2) |
| 2026-08-09 | [PARQIT_AUDIT_REMEDIATION_2026-08-09.md](PARQIT_AUDIT_REMEDIATION_2026-08-09.md) (PT) — implementation of the revised docs/test prompt plus independent final audit: precise lazy/help contract, v68/v69, reshape name preflight, native join-key rc 106, and 89/89 local Stata verdicts |
| 2026-08-22 | [2026-08-22/TRIAGE.md](2026-08-22/TRIAGE.md) (EN) — consolidated findings and fix specifications of the six-auditor adversarial audit of v0.1.27 + same-day ENC-2/NAME-CASE-1 (S0: merge using-only rows, automatic source-copy save, %tc µs precision, real()/date domains, partition-key types, name recovery, aliased-column propagation, silent .dta bridges) and the verifier's round-2 residuals |
| 2026-08-22 | [2026-08-22/A1_types_values.md](2026-08-22/A1_types_values.md) · [A2_names_metadata.md](2026-08-22/A2_names_metadata.md) · [A3_verb_semantics.md](2026-08-22/A3_verb_semantics.md) · [A4_atomicity_io.md](2026-08-22/A4_atomicity_io.md) · [A5_docs_contract.md](2026-08-22/A5_docs_contract.md) · [A6_performance.md](2026-08-22/A6_performance.md) (EN) — the six parallel auditor reports (types/values, names/metadata, verb semantics vs native, atomicity/IO/concurrency, docs contract, performance vs v0.1.27), each with its PASS/FAIL coverage table |
| 2026-08-22 | [2026-08-22/IMPLEMENTATION_LOG.md](2026-08-22/IMPLEMENTATION_LOG.md) (EN) — remediation record (38 items + round 2): copysource opt-in, merge fill, %tc integer µs, real()/date guards, partition keys, name recovery replicas of DuckDB's rules, ViewCol.stata invariant, torn-read guard, loud bridges; tests v71–v77; ASSUMPTIONS #96–#99 |
| 2026-08-22 | [2026-08-22/VERIFY_REPORT.md](2026-08-22/VERIFY_REPORT.md) (EN) — independent adversarial verification of the remediation: every S0 closed, ~600 new checks, concurrent writer/reader harnesses, residuals handed to round 2 |
| 2026-08-22 | [2026-08-22/IMPLEMENTER_BRIEF.md](2026-08-22/IMPLEMENTER_BRIEF.md) (EN) — the brief given to the remediation agent (ground rules, priority groups, definition of done) |
| 2026-09-01 | [AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-09-01.md](AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-09-01.md) (PT) — holistic adversarial audit of v0.1.29 (full code read, baseline 98/98 unit + 98/98 Stata, ≈480 new oracle-checked probes): 2 × S1 (string `partition_by` key `NULL` read as missing; float-column comparisons with decimal literals evaluated in single precision), 2 × S2 (`describe` type shift on Hive trees; CSV duplicate/case-clashing headers renamed silently), S3/S4 parity and usability items, plus the implementer plan (T1–T9) for v0.1.30; part C records the same-day implementation of T1–T4 (float literals compared in double + `float()`, string partition keys `NULL`/`__HIVE_DEFAULT_PARTITION__` refused with a read-side note, `describe` types keyed by name, CSV header-name recovery), pinned by `tests/verify_suite/v78`–`v81` and `audit_repro/`; part D records T5–T9 (`mod`/`%d`/`(count)` format, `drop in`/`tabulate` labels/`duplicates list`, dialect notes, harness, out-of-core `collapse`/`tabstat` percentiles pinned by `v82`–`v83`), the packaging of the two crash-course do-files and the v0.1.30 release, full suite 104/104 |

## Parity and performance versus pq

| Date | Document |
|---|---|
| 2026-06-15 | [RELATORIO_parqit_vs_pq_2026-06-15.md](RELATORIO_parqit_vs_pq_2026-06-15.md) (PT) — parqit vs pq comparison report |
| — | [PARITY_parqit_vs_pq_claude.md](PARITY_parqit_vs_pq_claude.md) — feature/result parity study vs pq |
| — | [PARQIT_PERFORMANCE_AUDIT_claude.md](PARQIT_PERFORMANCE_AUDIT_claude.md) — performance audit |

The runnable side of this comparison lives in
[`examples/pq_to_parqit_common_workflows.do`](../../examples/pq_to_parqit_common_workflows.do)
(command-by-command parity, requires `pq`) and the harnesses under
[`benchmarks/`](../../benchmarks).

## Audit prompts (methodology)

The commissioning prompts handed to the auditing agents, kept so each audit
is reproducible as an experiment:

- [CHATGPT_AUDIT_PROMPT.md](CHATGPT_AUDIT_PROMPT.md)
- [codex_audit_prompt.md](codex_audit_prompt.md)
- [PROMPT_AUDITORIA_ADVERSARIAL_FIABILIDADE_DADOS_PARQIT_2026-07-14.md](PROMPT_AUDITORIA_ADVERSARIAL_FIABILIDADE_DADOS_PARQIT_2026-07-14.md) (PT)
- [PROMPT_CORRECAO_MELHORIA_PARQIT_2026-08-08.md](PROMPT_CORRECAO_MELHORIA_PARQIT_2026-08-08.md) (PT) — implementation prompt for the 2026-08-08 audit findings (F1–F6 + parity oracle)
- [PROMPT_AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md](PROMPT_AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md) (PT) — commissioning prompt for the adversarial audit of the public help
- [PROMPT_CORRECAO_RUNTIME_PARQIT_2026-08-08.md](PROMPT_CORRECAO_RUNTIME_PARQIT_2026-08-08.md) (PT) — implementation prompt for the runtime message defects (D1–D6) the help audit recorded
- [PROMPT_CORRECAO_DOCS_PARQIT_2026-08-09.md](PROMPT_CORRECAO_DOCS_PARQIT_2026-08-09.md) (PT) — complete implementation prompt for the 2026-08-09 audit: documentation findings A1–A2, the missing-mode qualifier sentence, the `v68` second native oracle, and the `v69` raw-engine-error message sweep, with fixed decisions and a decided-not-to-fix record

## Verification kit

[`verifications/parqit_audit_package/`](verifications/parqit_audit_package)
— the self-contained repro/test pack that accompanied the 2026-06 external
audit (its `tests/**/outputs/` scratch is git-ignored).
