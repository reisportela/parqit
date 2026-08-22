* V76 — I/O edges of the 2026-08-22 audit (A4/A5):
*   A4-4 valid destination names up to NAME_MAX (255 bytes) are accepted — the
*        lock/staging siblings fall back to short digest-keyed names; a name past
*        NAME_MAX fails loudly with nothing left behind;
*   A4-5 a save INTO the open view's directory source is refused (the view would
*        read its own output back);
*   A4-6 a symlink destination is written THROUGH (the target is replaced, the
*        link stays a link); a read-only destination refuses replace (r(608));
*   A4-7 a foreign Hive tree with '=' inside a partition value fails with a
*        parqit message naming the cause and the remedy;
*   A4-8 the PARQIT_TEST_FAIL_OUTPUT_PUBLISH hook covers the POSIX flat-file
*        commit: the old file is byte-identical, nothing leaks;
*   A5-1 encoding(bogus) on a lazy view save is refused before anything is
*        written; a valid name is accepted.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local dir `"`stem'_d"'
mkdir `"`dir'"'
clear
set obs 5
gen long id = _n
gen str4 s = "s" + string(_n)
tempfile base
parqit save `"`base'.parquet"', replace data

* ---------- A4-4 long destination names ----------------------------------------
local long240 = substr(200 * "a", 1, 232) + ".parquet"           // 240 bytes
parqit save `"`dir'/`long240'"', replace data
assert r(N) == 5
parqit use using `"`dir'/`long240'"', clear
assert _N == 5
parqit save `"`dir'/`long240'"', replace data                     // replace over it too
local long256 = substr(300 * "b", 1, 248) + ".parquet"           // 256 bytes > NAME_MAX
capture noisily parqit save `"`dir'/`long256'"', replace data
assert _rc != 0
python:
from sfi import Macro
import os
d = Macro.getLocal("dir")
left = [f for f in os.listdir(d) if ".parqit_" in f]
Macro.setLocal("leak", "1" if left else "0")
end
assert "`leak'" == "0"

* ---------- A4-5 destination inside the view's directory source ------------------
local src `"`dir'/dsrc"'
mkdir `"`src'"'
clear
set obs 10
gen long id = _n
parqit save `"`src'/p1.parquet"', replace data
replace id = id + 10
parqit save `"`src'/p2.parquet"', replace data
parqit use using `"`src'"'
parqit keep if id <= 10
capture noisily parqit save `"`src'/subset.parquet"', replace
assert _rc == 198
capture noisily parqit save `"`src'/tree"', replace partition_by(id)
assert _rc == 198
capture noisily parqit save `"`src'"', replace partition_by(id)
assert _rc == 198
parqit count
assert r(N) == 10
* a sibling outside the directory is fine
parqit save `"`dir'/outside.parquet"', replace
parqit count
assert r(N) == 10
parqit close _all

* ---------- A4-6 symlink and read-only destinations -----------------------------
python:
from sfi import Macro
import os
d = Macro.getLocal("dir")
open(os.path.join(d, "target.parquet"), "wb").write(b"stale")
os.symlink(os.path.join(d, "target.parquet"), os.path.join(d, "link.parquet"))
end
clear
set obs 3
gen long id = _n
parqit save `"`dir'/link.parquet"', replace data
python:
from sfi import Macro
import os, pyarrow.parquet as pq
d = Macro.getLocal("dir")
ok = os.path.islink(os.path.join(d, "link.parquet"))
ok = ok and pq.read_table(os.path.join(d, "target.parquet")).num_rows == 3     # written THROUGH the link
Macro.setLocal("oklink", "1" if ok else "0")
end
assert "`oklink'" == "1"
* read-only destination: replace refused, file untouched
parqit save `"`dir'/ro.parquet"', replace data
python:
from sfi import Macro
import os, hashlib
p = os.path.join(Macro.getLocal("dir"), "ro.parquet")
os.chmod(p, 0o444)
Macro.setLocal("md5ro", hashlib.md5(open(p, "rb").read()).hexdigest())
end
capture noisily parqit save `"`dir'/ro.parquet"', replace data
assert _rc == 608
python:
from sfi import Macro
import os, hashlib
p = os.path.join(Macro.getLocal("dir"), "ro.parquet")
Macro.setLocal("md5ro2", hashlib.md5(open(p, "rb").read()).hexdigest())
os.chmod(p, 0o644)
end
assert "`md5ro'" == "`md5ro2'"

* ---------- A4-7 foreign Hive tree with '=' in a partition value ----------------
python:
from sfi import Macro
import os, pyarrow as pa, pyarrow.parquet as pq
d = os.path.join(Macro.getLocal("dir"), "hive_eq", "city=a=b")
os.makedirs(d)
pq.write_table(pa.table({"x": pa.array([1, 2], pa.int64())}), os.path.join(d, "part.parquet"))
end
* the pinned DuckDB 1.5.3 reads a partition value containing '=' as-is
* (city=a=b -> "a=b"), so this tree no longer raises the raw Binder Error the
* 2026-08-22 audit saw; parqit's friendly_engine_error wrapper remains as a
* defensive net for other malformed Hive trees. Confirm the tree reads.
parqit use using `"`dir'/hive_eq"', clear
assert _N == 2
parqit use using `"`dir'/hive_eq/**/*.parquet"', clear
assert _N == 2

* ---------- A4-8 publish hook on the POSIX flat path ---------------------------
clear
set obs 4
gen long id = _n
parqit save `"`dir'/pub.parquet"', replace data
python:
from sfi import Macro
import os, hashlib
p = os.path.join(Macro.getLocal("dir"), "pub.parquet")
Macro.setLocal("md5pub", hashlib.md5(open(p, "rb").read()).hexdigest())
os.environ["PARQIT_TEST_FAIL_OUTPUT_PUBLISH"] = "1"
end
replace id = id * 10
capture noisily parqit save `"`dir'/pub.parquet"', replace data
local prc = _rc
python:
from sfi import Macro
import os, hashlib
os.environ.pop("PARQIT_TEST_FAIL_OUTPUT_PUBLISH", None)
p = os.path.join(Macro.getLocal("dir"), "pub.parquet")
Macro.setLocal("md5pub2", hashlib.md5(open(p, "rb").read()).hexdigest())
d = Macro.getLocal("dir")
left = [f for f in os.listdir(d) if f.startswith("pub.parquet.parqit_")]
Macro.setLocal("publeak", "1" if left else "0")
end
assert `prc' != 0
assert "`md5pub'" == "`md5pub2'"
assert "`publeak'" == "0"
parqit save `"`dir'/pub.parquet"', replace data       // works again without the hook
assert r(N) == 4

* ---------- A5-1 encoding() validated on a lazy view save ---------------------
parqit use using `"`base'.parquet"'
capture noisily parqit save `"`dir'/enc_bogus.parquet"', replace encoding(bogus)
assert _rc == 198
capture confirm file `"`dir'/enc_bogus.parquet"'
assert _rc != 0
parqit save `"`dir'/enc_ok.parquet"', replace encoding(latin1)
assert r(N) == 5
parqit close _all

* ---------- V2.7 parqit messages: a missing file/pattern (r(601), no raw IO
* Error or SQL snippet); partition_by() naming every variable ----------------
capture log close v76m
log using `"`dir'/v76m.log"', replace name(v76m) text
capture noisily parqit use using `"`dir'/does_not_exist.parquet"', clear
local rc1 = _rc
capture noisily parqit use using `"`dir'/does_not_exist.parquet"'
local rc2 = _rc
capture noisily parqit use using `"`dir'/nomatch_*.parquet"', clear
local rc3 = _rc
capture noisily parqit describe `"`dir'/does_not_exist.parquet"'
local rc4 = _rc
log close v76m
assert `rc1' == 601 & `rc2' == 601 & `rc3' == 601 & `rc4' == 601
python:
from sfi import Macro
import os
txt = open(os.path.join(Macro.getLocal("dir"), "v76m.log"), encoding="utf-8", errors="replace").read()
ok = txt.count("file not found: no file matches") >= 4 and "IO Error" not in txt and "LINE 1:" not in txt
Macro.setLocal("msg_ok", "1" if ok else "0")
end
assert "`msg_ok'" == "1"
clear
set obs 2
gen byte a = _n
gen byte b = _n
capture noisily parqit save `"`dir'/pt_all"', replace data partition_by(a b)
assert _rc == 198
capture confirm file `"`dir'/pt_all"'
assert _rc != 0
parqit use using `"`base'.parquet"'
capture noisily parqit save `"`dir'/pt_all2"', replace partition_by(id s)
assert _rc == 198
capture confirm file `"`dir'/pt_all2"'
assert _rc != 0
parqit close _all

di "VERDICT(V76_IO_EDGES): PASS - 240-byte names accepted and >NAME_MAX refused cleanly, save into a directory source refused, symlink written through, read-only replace refused r(608), hive '=' value read (DuckDB 1.5.3), POSIX publish hook keeps the old file, encoding() validated on view save, missing file and all-column partition_by() give parqit messages"
