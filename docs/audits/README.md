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

## Verification kit

[`verifications/parqit_audit_package/`](verifications/parqit_audit_package)
— the self-contained repro/test pack that accompanied the 2026-06 external
audit (its `tests/**/outputs/` scratch is git-ignored).
