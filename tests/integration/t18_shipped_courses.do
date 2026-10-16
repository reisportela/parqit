* Execute the two shipped courses in runner-owned scratch, using the exact
* ado/plugin surface selected by the test runner. Both use synthetic data.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
do `"`repo'/examples/parqit_basics.do"'
do `"`repo'/examples/parqit_tour.do"'
parqit close _all
di "VERDICT(T18_SHIPPED_COURSES): PASS"
