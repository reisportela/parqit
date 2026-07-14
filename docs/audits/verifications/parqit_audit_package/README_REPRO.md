# parqit audit package - reproduction kit

Self-contained kit to reproduce the audit of `parqit` on the machine where you
build/develop parqit. Audited upstream commit `722a2c83e809aca4422b0f020ab595ada55f62a4`,
parqit 0.1.11.

## Contents

```
README_REPRO.md                              this file
AGENTS.md                                    test-pack guardrails / how the pack is meant to run
audit/
  RELATORIO_PARA_CLAUDE_CORRIGIR_PARQIT.md   fix brief for the upstream dev (the patch + tests)
  parqit_audit_report.md                     test-pack audit report (finding + harness defects)
repro/
  repro_char_projection.do                   minimal, portable reproducer of PARQIT-CHAR-01
  verify_mata_primitives.do                  proof the fix must use _st_varindex, not st_varindex
tests/
  run_all_parqit_tests.do                    the full adversarial test pack (Windows-oriented)
reference/
  parqit_adversarial_tests.log               the Windows reference run (shows the rc 3300 FAIL)
```

## Prerequisites

- Stata 16 or newer (`stata-mp`, `stata-se`, or `stata`).
- parqit available to Stata: either built and on the adopath, or `net install`ed.
  On a dev tree you typically prepend the repo's ado dir and point the plugin var.

## 1. Fast path: reproduce the bug (recommended on the dev machine)

The two `repro/` do-files are fully portable (Stata tempfiles only, no
machine-specific paths) and are the reliable way to reproduce on macOS/Linux.

parqit already on the adopath:

```bash
stata-mp -b do repro/repro_char_projection.do
stata-mp -b do repro/verify_mata_primitives.do
```

Point at a build tree (upstream verify-suite style; second arg optional):

```bash
stata-mp -b do repro/repro_char_projection.do "/path/to/parqit" "/path/to/build/parqit_plugin.plugin"
```

Windows:

```powershell
& "C:\Program Files\StataNow19\StataMP-64.exe" /e do repro\repro_char_projection.do
& "C:\Program Files\StataNow19\StataMP-64.exe" /e do repro\verify_mata_primitives.do
```

Expected with the CURRENT (unfixed) parqit:

- `repro_char_projection.do`: CASE1/CASE2/CASE3 report `rc = 3300` (CASE1 is a plain
  `parqit use <subset> using f, clear`; CASE2 contract+collect; CASE3 collapse+collect).
  CASE5 (rename) is `rc 0` already. Final line: `VERDICT(...): FAIL (...) -> PARQIT-CHAR-01 reproduced`.
- `verify_mata_primitives.do`: shows `st_global on ABSENT var -> rc 3300`,
  `st_varindex(absent) -> rc 3500 ABORT`, `_st_varindex(absent) -> rc 0, value .`.

Expected AFTER applying the fix in `audit/RELATORIO_...md`:

- `repro_char_projection.do`: all cases `rc 0`, `VERDICT(...): PASS (bug fixed)`.

Batch logs land next to the do-file as `repro_char_projection.log` /
`verify_mata_primitives.log` (Stata batch auto-log).

## 2. Full audit suite

`tests/run_all_parqit_tests.do` is the complete adversarial pack (synthetic data,
bounded public downloads, ~70 assertions). It writes its own log under
`tests/outputs/parqit_adversarial_tests.log`.

```bash
# from the extracted package root
stata-mp -b do tests/run_all_parqit_tests.do "$(pwd)"
```

```powershell
& "C:\Program Files\StataNow19\StataMP-64.exe" /e do `
  tests\run_all_parqit_tests.do "C:\path\to\parqit_audit_package"
```

It exits 0 on PASS and 459 on FAIL; the final line is
`VERDICT(PARQIT_ADVERSARIAL_TEST_PACK): PASS|FAIL`. The reference run that
captured the bug is in `reference/parqit_adversarial_tests.log` (see lines
1512-1520 for the rc 3300).

### Known caveats of the full suite (see parqit_audit_report.md)

- macOS/Linux: the suite has a P0 portability bug - it converts `/` to `\` for the
  `parqit set tempdir` test (lines 40-43, used at 670/673), which fails on
  non-Windows. Until that is patched, either delete lines 40-43 (Stata accepts
  forward slashes everywhere) or expect that single section to FAIL. The `repro/`
  files above have no such issue.
- Offline/firewalled: the net install and two public downloads are scored as FAIL
  rather than SKIP, so the suite needs internet (raw.githubusercontent must be
  reachable).

## Where to start reading

1. `audit/RELATORIO_PARA_CLAUDE_CORRIGIR_PARQIT.md` - the actionable fix brief
   (root cause, the ado + plugin patch, the regression test, validation).
2. `audit/parqit_audit_report.md` - the finding, severity, and the test-pack's own
   defects to fix before sharing.
