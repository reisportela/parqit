* V146 — the ado-files refuse a plugin of another release (PLUGIN-VERSION-1,
*   2026-10-01). A running Stata keeps the plugin it loaded first, so parqit
*   updated while Stata is open (or a plugin net install could not replace)
*   paired new ado-files with an old plugin: parqit save of auto then stopped
*   with "option revalidvars() not allowed" (BPLIM). Every command now
*   compares the two releases first and names both.
* The ado-files of another release are simulated by a copy whose required
*   plugin release is 9.9.9; the real plugin must be refused with r(498),
*   also when it is already loaded (the case of an update while Stata runs).
clear all
set more off
args repo plugin
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem
local dir `"`stem'_ado"'
mkdir `"`dir'"'
tempfile plog

python:
import re
from sfi import Macro
src = open(Macro.getLocal("repo") + "/src/ado/p/parqit.ado", encoding="utf-8").read()
new, n = re.subn(r'local ado_version "[0-9]+\.[0-9]+\.[0-9]+"', 'local ado_version "9.9.9"', src)
assert n == 1, n
open(Macro.getLocal("dir") + "/parqit.ado", "w", encoding="utf-8").write(new)
end

* the matching ado-files load the plugin and accept it
adopath ++ `"`repo'/src/ado/p"'
parqit version
* ado-files of another release, the plugin still loaded: refused all the same
discard
adopath ++ `"`dir'"'
sysuse auto, clear
log using `"`plog'"', text replace name(v146)
capture noisily parqit save `"`stem'_auto.parquet"', replace
local rc = _rc
log close v146
assert `rc' == 498
capture confirm file `"`stem'_auto.parquet"'
assert _rc == 601

python:
from sfi import Macro
text = open(Macro.getLocal("plog"), encoding="utf-8").read().replace("\n> ", "")
assert "but the ado-files are version 9.9.9" in text, text
assert "the plugin in use is version " in text, text
assert "close Stata and start it again" in text, text
end

di "VERDICT(V146_PLUGIN_VERSION_CHECK): PASS - ado-files of another release refuse the loaded plugin before any work (r(498), nothing written), naming both releases and telling the user to restart Stata"
