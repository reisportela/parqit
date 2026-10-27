# Native menus: explicit view targets and compact windows

This note records the implementation checks completed before the 0.6.1 version
bump. It is not a claim about a subsequently built or published release binary.
Release assets require the four-platform CI and separate package verification.

All ten native dialogs offer a view selector where an operation uses an existing
view. The initial choice is the session's current view; variable pickers follow
the chosen source. Ordinary operations emit `parqit view name:` and restore which
view is current after execution. Transformations still change the selected
view's plan. Explicit switching, closing and source-opening operations retain
their session effects. Selector-driven copies preserve the previous current view.

File, memory, SQL-source, session and all-view modes disable the selector.
Combine distinguishes master and using views, including their separate schemas;
the native memory routes continue to use file inputs. Read offers explicit plan
copying with a required new name. Save/collect refuse a closed target rather than
redirecting to the dataset in memory. Registry/schema callbacks preserve `r()`.
Numeric GUI flags use class macro expansion, including the read-source callback.

The dialogs retain all preceding controls, options and substantive defaults.
Extra inputs use labelled tabs and the content height is capped at 395 dialog
units. Measured client windows fit a private 1280×768 Linux display, with heights
from 619 to 693 pixels. Source examples and helpers were not installed globally.

## Pre-bump evidence

| Check | Result |
|---|---|
| Existing controls/defaults retained in all ten dialogs | PASS |
| Real GUI command/mode/schema and native state checks | 44 PASS, 2 EXPECTED_REFUSAL across 46 scenarios |
| Separate follow-up showing master and using targets in the context line | PASS |
| Existing T15 command-shape and T17 current-option integration | PASS |
| Existing V92 context/source and V193 Explore compatibility checks | PASS |
| V194 selected targets, native assertions and full independent PyArrow reads | PASS |
| User/technical help tab mapping, anchors, syntax/function coverage and SMCL format | PASS |
| Native rendering of both complete helps and existing V66 help contract | PASS |
| Native Windows/macOS GUI runtime | NOT_RUN |

Fixtures were synthetic: a seven-row alpha view, a three-row beta view with a
different schema, and a separate nine-row dataset in Stata memory. SPSS tests
used the repository's synthetic survey fixture. Checks covered save/collect,
by/bysort, source-view copying, raw query and time declarations, combinations
with independent master/using views, memory/file exceptions, and explicit
switch/close/source-opening actions. The no-view memory case verified that
changing the partition update mode preserves a valid `partition_by()` choice.
The two expected refusals closed the selected target before save or collect;
the destination/memory and the prior current view were protected.

The local pre-bump runtime plugin was the unchanged DuckDB 2.0.0 / contract-4
build already present in the checkout. Its SHA-256 was
`50292aacaee8030ace2db98c815a230bd819d8684d25273f25744c812583c717`.
These local checks do not substitute for the 0.6.1 CI binaries or staged/public
installation checks. Local raw receipts remain in the maintainer's
`audit_repro/all_view_menus_20261027/` and `audit_repro/help_sync_20261027/`;
they include intermediate failed attempts as well as the accepted results.
