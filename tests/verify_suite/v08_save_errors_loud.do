* CHARTER 8 (pq finding 8): every save failure is a nonzero rc PLUS a
* message — never rc 0 with a missing/stale file. pq's signature: writes
* into nonexistent dirs printed an error yet returned 0.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

set obs 3
gen x = _n
gen y = 10 * _n

* nonexistent directory → loud failure
capture parqit save "/nonexistent_dir_parqit/x.parquet", replace
assert _rc != 0
local rc1 = _rc

* overwrite without replace → rc 602 and target byte-identical
tempfile obase
local out `"`obase'.parquet"'
parqit save `"`out'"', replace
python:
from sfi import Macro
import hashlib
Macro.setLocal("md5_before", hashlib.md5(open(Macro.getLocal("out"), "rb").read()).hexdigest())
end
capture parqit save `"`out'"'
assert _rc == 602
python:
from sfi import Macro
import hashlib
Macro.setLocal("md5_after", hashlib.md5(open(Macro.getLocal("out"), "rb").read()).hexdigest())
end
assert "`md5_before'" == "`md5_after'"

* unknown compression codec → loud rejection BEFORE writing (pq S4-4 wrote
* zstd while claiming lzo)
capture parqit save `"`out'2.parquet"', compression(lzo)
assert _rc != 0
capture confirm file `"`out'2.parquet"'
assert _rc != 0   // nothing was written

* partitioned target that exists → refused (parqit never deletes trees)
capture parqit save `"`out'"', partition_by(x)
assert _rc != 0

* partitioned success publishes only the final tree, never a stale staging dir
tempfile pbase
local pdir `"`pbase'_parts"'
parqit save `"`pdir'"', replace partition_by(x)
python:
from sfi import Macro
import os
import pyarrow.dataset as ds
pdir = Macro.getLocal("pdir")
assert os.path.isdir(pdir), pdir
assert not os.path.exists(pdir + ".parqit_tmp"), pdir + ".parqit_tmp"
assert ds.dataset(pdir, format="parquet", partitioning="hive").count_rows() == 3
end

di "VERDICT(V08_SAVE_ERRORS): PASS - failures return rc!=0 (`rc1', 602, codec, partition) and never touch targets; partition staging publishes cleanly"
