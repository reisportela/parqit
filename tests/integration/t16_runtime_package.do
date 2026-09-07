* net install delivers the platform plugin and compiler-runtime notices, without a DLL.
clear all
set more off
args repo plugin
tempfile stem
local fixture `"`stem'_runtime"'
shell python3 "`repo'/tests/package_runtime_fixture.py" prepare "`repo'" "`fixture'"
sysdir set PLUS `"`fixture'/plus"'
net set ado PLUS
net set other `"`fixture'/other"'
net install parqit_runtime_probe, from(`"`fixture'/source"')
shell python3 "`repo'/tests/package_runtime_fixture.py" check "`repo'" "`fixture'"
confirm file `"`fixture'/runtime_install.ok"'
di "VERDICT(T16_RUNTIME_PACKAGE): PASS"
