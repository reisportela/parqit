* V30 — residual robustness fixes from the Codex audit (2026-06-14, third round):
*   META-2  characteristics/notes on a renamed column survive a view re-save
*   IO-3    save … , replace partition_by(...) is re-runnable over an existing tree
* (ATOM-3 — an aborted collect no longer orphans its spill temp table — is a
*  defensive cleanup in set_prepared_read; the abort is not deterministically
*  triggerable from Stata, so it is covered by the suite's many collects, not here.
*  ATOM-1's partitioned-save atomicity was already in place: the partitioned branch
*  stages in dest.parqit_tmp, verifies, then atomically renames.)
* Each on-disk check uses an independent pyarrow oracle, never parqit alone.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile t

* ---- META-2 : a char/note on a renamed column survives the re-save ---------
clear
set obs 3
gen wage = _n
char wage[source] "survey2020"
note wage: a wage note
parqit save `"`t'_A.parquet"', replace data
parqit use using `"`t'_A.parquet"'
parqit rename wage pay
parqit save `"`t'_B.parquet"', replace
python:
import pyarrow.parquet as pq, json
from sfi import Macro
md = pq.read_metadata(Macro.getLocal("t")+"_B.parquet").metadata
ch = json.loads(md[b"parqit.chars"].decode())
ok = ("pay" in ch) and ("wage" not in ch) and ("source" in ch.get("pay", {}))
Macro.setLocal("m2_ok", "1" if ok else "0")
Macro.setLocal("m2_keys", ",".join(ch.keys()))
end
if ("`m2_ok'"!="1") di as err "FAIL META-2: chars not remapped to 'pay' (keys=[`m2_keys'])"
local fails = `fails' + ("`m2_ok'"!="1")

* ---- IO-3 : replace partition_by(...) is re-runnable over an existing tree --
local pdir `"`t'_parts"'
clear
set obs 6
gen g = mod(_n, 3)
gen v = _n
parqit save `"`pdir'"', replace partition_by(g) data
clear
set obs 6
gen g = mod(_n, 3)
gen v = 100 + _n
capture noisily parqit save `"`pdir'"', replace partition_by(g) data
if (_rc) di as err "FAIL IO-3: replace partition_by over existing tree failed rc=`=_rc'"
local fails = `fails' + (_rc!=0)
python:
import pyarrow.dataset as ds
from sfi import Macro
n = ds.dataset(Macro.getLocal("pdir"), format="parquet", partitioning="hive").count_rows()
Macro.setLocal("io3_n", str(n))
end
if ("`io3_n'"!="6") di as err "FAIL IO-3: re-run tree has `io3_n' rows (want 6 — no stale/duplication)"
local fails = `fails' + ("`io3_n'"!="6")
* without replace over an existing tree → still refused loudly
capture parqit save `"`pdir'"', partition_by(g) data
if (_rc==0) di as err "FAIL IO-3: partition_by without replace over an existing tree was not refused"
local fails = `fails' + (_rc==0)

di as txt "VERDICT(V30_RESIDUAL_FIXES): " cond(`fails'==0, "PASS", "FAIL — `fails' failures") ///
    " - char-remap-on-rename / partition replace re-runnable"
