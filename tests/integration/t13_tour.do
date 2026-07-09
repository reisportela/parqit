* The public feature tour is executable documentation: run it from the runner's
* isolated temp directory and require its native-oracle verdict.
clear all
set more off
args repo plugin

do `"`repo'/examples/parqit_tour.do"' `"`repo'"' `"`plugin'"'

* Exercise the two auxiliary dispatcher branches that cannot be meaningfully
* driven in batch.  The dialog helper is deliberately defensive with no
* arguments; `menu` either succeeds under GUI Stata or refuses loudly in batch.
capture noisily parqit _dlgvars
assert _rc == 0
capture noisily parqit menu
assert inlist(_rc, 0, 199)

di as result "VERDICT(T13_AUXILIARY_COMMANDS): PASS — dialog helper is defensive; menu obeys the GUI/batch contract"
