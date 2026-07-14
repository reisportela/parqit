* The basics guide (examples/parqit_basics.do) is executable documentation:
* run it from the runner's isolated temp directory and require its
* native-oracle verdict (it prints VERDICT(PARQIT_BASICS): PASS itself).
clear all
set more off
args repo plugin

do `"`repo'/examples/parqit_basics.do"' `"`repo'"' `"`plugin'"'
