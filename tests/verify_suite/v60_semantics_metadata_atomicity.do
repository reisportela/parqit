* v60 — GO-GO semantic, metadata, atomicity, Unicode and lifecycle regressions
* from the 2026-07-14 adversarial reliability audit.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local base `"`stem'"'

* ================= SEM-006: deferred order is consumed at the right stage ===
local sortsrc `"`base'_sort_source.parquet"'
clear
input long id long x
1 2
2 1
3 3
end
parqit save `"`sortsrc'"', replace data
parqit use using `"`sortsrc'"'
parqit sort x
parqit replace x = -x
parqit gen double seq = _n
parqit collect, clear
sort id
assert seq[1] == 2 & seq[2] == 1 & seq[3] == 3
assert x[1] == -2 & x[2] == -1 & x[3] == -3
parqit close _all

local master `"`base'_append_master.parquet"'
local using `"`base'_append_using.parquet"'
clear
input long id long x
101 2
102 1
end
parqit save `"`master'"', replace data
clear
input long id long x
200 0
201 3
end
parqit save `"`using'"', replace data
parqit use using `"`master'"'
parqit sort x
parqit append using `"`using'"'
parqit keep in 1/2
parqit collect, clear
assert _N == 2 & id[1] == 102 & id[2] == 101
parqit close _all

* ================= ATOM-014/015/016: failed commands preserve prior view ====
local atomsrc `"`base'_atomic_source.parquet"'
clear
set obs 2
gen long a = 10 * _n
gen long b = 100 * _n
gen long id = _n
parqit save `"`atomsrc'"', replace data

parqit use using `"`atomsrc'"'
capture noisily parqit rename (a b) (c c)
local duprc = _rc
assert `duprc' == 198
parqit collect, clear
confirm variable a
confirm variable b
capture confirm variable c
assert _rc == 111
assert a[1] == 10 & b[2] == 200
parqit close _all

parqit use using `"`atomsrc'"'
parqit rename (a b) (b a)
parqit collect, clear
assert b[1] == 10 & a[2] == 200
parqit close _all

parqit use using `"`atomsrc'"'
capture noisily parqit gen z = trim(id)
local trimrc = _rc
assert `trimrc' == 198
capture noisily parqit gen z = subinstr(id, "1", "x", .)
local subrc = _rc
assert `subrc' == 198
capture noisily parqit replace id = _n
assert _rc == 198
capture noisily parqit gen first = 1 if _n == 1
assert _rc == 198
parqit count
assert r(N) == 2
parqit collect, clear
assert _N == 2 & id[1] == 1 & id[2] == 2
capture confirm variable z
assert _rc == 111
parqit close _all

parqit use using `"`atomsrc'"'
capture noisily parqit sql `"SELECT CAST(s AS INTEGER) AS y FROM (VALUES ('bad')) t(s)"', clear
local sqlrc = _rc
assert `sqlrc' != 0
* The original default view must still be current and executable.
parqit count
assert r(N) == 2
parqit collect, clear
assert _N == 2 & id[1] == 1 & id[2] == 2
confirm variable a
capture confirm variable y
assert _rc == 111
parqit close _all

* ================= META-010/011/012/013: honest metadata =====================
local mixdir `"`base'_mixed_metadata"'
local mixglob `"`mixdir'/*.parquet"'
local malformed `"`base'_malformed_metadata.parquet"'
local duplicate `"`base'_duplicate_names.parquet"'
python:
import json, os
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro

mixdir = Macro.getLocal("mixdir")
os.mkdir(mixdir)
schema = {"version": 1,
          "vars": [{"name": "m", "src": "m", "type": "int",
                    "fmt": "%tm", "varlab": "FROM_A_ONLY", "vallab": ""}],
          "sortedby": ["m"]}
meta = {b"parqit.schema": json.dumps(schema).encode(),
        b"parqit.vallabs": b"{}", b"parqit.chars": b"{}",
        b"parqit.dtalabel": json.dumps("A ONLY").encode()}
a = pa.table({"m": pa.array([1, 2], type=pa.int32())}).replace_schema_metadata(meta)
b = pa.table({"m": pa.array([3, 4], type=pa.int32())})
pq.write_table(a, os.path.join(mixdir, "a.parquet"))
pq.write_table(b, os.path.join(mixdir, "b.parquet"))

badmeta = {b"parqit.schema": b"{this is not json",
           b"parqit.vallabs": b"{}", b"parqit.chars": b"{}",
           b"parqit.dtalabel": json.dumps("MUST NOT APPLY").encode()}
bad = pa.table({"m": pa.array([5], type=pa.int32())}).replace_schema_metadata(badmeta)
pq.write_table(bad, Macro.getLocal("malformed"))

dup = pa.Table.from_arrays([pa.array([1, 2], type=pa.int32()),
                            pa.array([10, 20], type=pa.int32())],
                           names=["dup", "dup"])
pq.write_table(dup, Macro.getLocal("duplicate"))
end

clear
parqit use using `"`mixglob'"', clear
assert _N == 4
assert `"`: format m'"' != "%tm"
assert `"`: variable label m'"' == ""
assert `"`: data label'"' == ""
local mixsorted : sortedby
assert `"`mixsorted'"' == ""

clear
parqit use using `"`malformed'"', clear
assert _N == 1 & m[1] == 5
assert `"`: variable label m'"' == ""
assert `"`: data label'"' == ""
assert `"`: format m'"' != "%tm"

clear
parqit use using `"`duplicate'"'
parqit keep dup dup_1
parqit collect, clear
assert dup[1] == 1 & dup_1[2] == 20
local dupsrc : char dup_1[src_name]
assert `"`dupsrc'"' == "dup"
parqit close _all

* sortedby survives direct load, lazy collect and lazy save/reload; `by:' can
* use the restored marker without an explicit new sort.
local sortmeta `"`base'_sorted_metadata.parquet"'
local sortmeta2 `"`base'_sorted_metadata_lazy.parquet"'
clear
input byte g long id
2 4
1 2
2 3
1 1
end
sort g id
parqit save `"`sortmeta'"', replace data
clear
parqit use using `"`sortmeta'"', clear
local sb1 : sortedby
assert `"`sb1'"' == "g id"
capture by g: gen byte by_marker = 1
assert _rc == 0
drop by_marker

clear
parqit use using `"`sortmeta'"'
parqit collect, clear
local sb2 : sortedby
assert `"`sb2'"' == "g id"
capture by g: gen byte first_in_g = (_n == 1)
assert _rc == 0
drop first_in_g
parqit close _all

clear
parqit use using `"`sortmeta'"'
parqit save `"`sortmeta2'"', replace
parqit close _all
parqit use using `"`sortmeta2'"', clear
local sb3 : sortedby
assert `"`sb3'"' == "g id"
capture by g: gen byte first_again = (_n == 1)
assert _rc == 0

* ================= PORT-017: sanitizer always emits Stata-valid names =======
local unicode `"`base'_unicode_names.parquet"'
python:
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro

cjk = "資料" * 10                 # 20 valid Unicode letters, below 32 chars
t = pa.Table.from_arrays([pa.array([1]), pa.array([2]), pa.array([3])],
                         names=["😀bad", "\u0301accent", cjk])
pq.write_table(t, Macro.getLocal("unicode"))
Macro.setLocal("cjk_name", cjk)
end

clear
parqit use using `"`unicode'"'
parqit collect, clear
confirm variable _bad
confirm variable _accent
confirm variable `cjk_name'
local emoji_src : char _bad[src_name]
local combining_src : char _accent[src_name]
assert `"`emoji_src'"' == "😀bad"
assert `"`combining_src'"' == "́accent"
assert ustrlen(`"`cjk_name'"') == 20
parqit close _all

* ================= LIFE-018: partial thread creation returns, never aborts ===
local threadsrc `"`base'_thread_source.parquet"'
clear
set obs 100
gen long id = _n
gen double x = sqrt(_n)
parqit save `"`threadsrc'"', replace data
clear
set obs 1
gen long sentinel = 909
python:
import os
os.environ["PARQIT_FILL_THREADS"] = "4"
os.environ["PARQIT_TEST_FAIL_THREAD_AT"] = "1"
end
capture noisily parqit use using `"`threadsrc'"', clear
local threadrc = _rc
assert `threadrc' != 0
assert _N == 1 & sentinel[1] == 909
python:
import os
os.environ.pop("PARQIT_TEST_FAIL_THREAD_AT", None)
end
parqit use using `"`threadsrc'"', clear
assert _N == 100 & id[1] == 1 & id[100] == 100
python:
import os
os.environ.pop("PARQIT_FILL_THREADS", None)
end

di "VERDICT(V60_SEMANTICS_METADATA_ATOMICITY): PASS - order, rollback, metadata, sortedby, Unicode and worker lifecycle"
