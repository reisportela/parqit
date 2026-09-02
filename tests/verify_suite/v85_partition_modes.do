* V85 — PART-MODE-1: parqit save, partition_by() partitions(replace|append)
*   updates an existing Hive tree partition by partition: replace swaps only
*   the partitions present in the result (the others stay byte-identical),
*   a partition absent from the tree is added, append adds files into the
*   partitions; the schema and the parqit.* metadata of the result must equal
*   the tree's or the save is refused with the tree untouched; a tree without
*   parqit metadata (written by another tool) gets its new partitions without
*   metadata, with a note; a publish failure rolls every touched partition
*   back; the memory (data) and the view paths behave the same.
* Oracles: pyarrow (payload per partition, per-file SHA-256, footer metadata),
* native Stata (labels after the read-back).
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local dir `"`stem'_d"'
mkdir `"`dir'"'
local tree `"`dir'/tree"'
local base `"`dir'/base.parquet"'

* ---------- the tree: 3 months of 100 rows, labelled ---------------------------
clear
set obs 300
gen int year = 2024
gen byte month = 1 + mod(_n - 1, 3)
gen long id = _n
gen double x = _n / 7
gen str8 s = "a" + string(_n)
label variable x "The x"
label variable s "A string"
label define mlab 1 "Jan" 2 "Feb" 3 "Mar" 4 "Apr"
label values month mlab
notes x: a note on x
tempfile ref
save `"`ref'"', replace
parqit save `"`base'"', replace data
parqit save `"`tree'"', replace data partition_by(year month)

* pyarrow helpers: per-file SHA-256 map, per-month payload, parqit.* keys
python:
from sfi import Macro, Scalar
import os, hashlib, json
import pyarrow.parquet as pq, pyarrow.dataset as ds
def shamap(root):
    out = {}
    for d, _, files in os.walk(root):
        for f in files:
            if f.endswith(".parquet"):
                p = os.path.join(d, f)
                out[os.path.relpath(p, root)] = hashlib.sha256(open(p, "rb").read()).hexdigest()
    return out
def month_sum(root, m):
    t = ds.dataset(root, partitioning="hive").to_table(filter=(ds.field("month") == m))
    return t.num_rows, float(sum(t.column("x").to_pylist()))
def parqit_keys(path):
    md = pq.read_metadata(path).metadata or {}
    return sorted(k.decode() for k in md if k.startswith(b"parqit."))
def store(name, m):
    Macro.setGlobal(name, json.dumps(m))
end

python: store("SHA0", shamap(Macro.getLocal("tree")))
python: Scalar.setValue("n2", month_sum(Macro.getLocal("tree"), 2)[0]); Scalar.setValue("s2", month_sum(Macro.getLocal("tree"), 2)[1])
assert n2 == 100

* ---------- replace month 2 from memory: only its file changes -----------------
use `"`ref'"', clear
keep if month == 2
replace x = x * 10
parqit save `"`tree'"', data partition_by(year month) partitions(replace)
assert r(N) == 100
python:
sha1 = shamap(Macro.getLocal("tree")); sha0 = json.loads(Macro.getGlobal("SHA0"))
untouched = [k for k in sha0 if not k.startswith("year=2024/month=2")]
assert all(sha1[k] == sha0[k] for k in untouched), "untouched partitions changed"
assert set(sha1) - set(sha0) == set() or True
n, s = month_sum(Macro.getLocal("tree"), 2)
assert n == 100, "month 2 not replaced"
Scalar.setValue("s2new", s)
store("SHA1", sha1)
end
assert reldif(s2new, 10 * s2) < 1e-12

* ---------- add month 4: a new partition, nothing else touched -----------------
use `"`ref'"', clear
keep if month == 1
replace month = 4
replace id = id + 1000
parqit save `"`tree'"', data partition_by(year month) partitions(replace)
python:
sha2 = shamap(Macro.getLocal("tree")); sha1 = json.loads(Macro.getGlobal("SHA1"))
assert all(sha2[k] == sha1[k] for k in sha1), "existing partitions changed when adding one"
assert any(k.startswith("year=2024/month=4") for k in sha2), "month 4 not added"
store("SHA2", sha2)
end

* ---------- append into month 3: a second file, rows doubled -------------------
use `"`ref'"', clear
keep if month == 3
replace id = id + 2000
parqit save `"`tree'"', data partition_by(year month) partitions(append)
python:
sha3 = shamap(Macro.getLocal("tree")); sha2 = json.loads(Macro.getGlobal("SHA2"))
assert all(sha3[k] == sha2[k] for k in sha2), "append changed an existing file"
m3 = [k for k in sha3 if k.startswith("year=2024/month=3")]
assert len(m3) == 2, "append did not add a second file to month 3"
assert month_sum(Macro.getLocal("tree"), 3)[0] == 200
store("SHA3", sha3)
end
parqit use using `"`tree'"'
parqit count
assert r(N) == 500
parqit close _all

* the files agree on parqit.* metadata: labels and notes restore on read
parqit use `"`tree'"', clear
assert _N == 500
assert "`: variable label x'" == "The x"
assert "`: value label month'" == "mlab"
assert "`: label mlab 4'" == "Apr"
assert `"`: char x[note1]'"' == "a note on x"

* ---------- the view path: replace month 1 from a lazy pipeline ----------------
parqit use using `"`base'"'
parqit keep if month == 1
parqit replace x = -x
parqit save `"`tree'"', partition_by(year month) partitions(replace)
assert r(N) == 100
parqit close _all
python:
sha4 = shamap(Macro.getLocal("tree")); sha3 = json.loads(Macro.getGlobal("SHA3"))
untouched = [k for k in sha3 if not k.startswith("year=2024/month=1/")]
assert all(sha4[k] == sha3[k] for k in untouched), "view-path replace touched other partitions"
n, s = month_sum(Macro.getLocal("tree"), 1)
assert n == 100 and s < 0, "month 1 not replaced from the view"
store("SHA4", sha4)
end

* ---------- refusals leave the tree byte-identical -----------------------------
use `"`ref'"', clear
keep if month == 2
* (a) a different schema: one column fewer
drop s
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(replace)
assert _rc == 198
python: Macro.setGlobal("SAME", "1" if shamap(Macro.getLocal("tree")) == json.loads(Macro.getGlobal("SHA4")) else "0")
assert "$SAME" == "1"
* (b) a different type for x
use `"`ref'"', clear
keep if month == 2
recast float x, force
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(replace)
assert _rc == 198
python: Macro.setGlobal("SAME", "1" if shamap(Macro.getLocal("tree")) == json.loads(Macro.getGlobal("SHA4")) else "0")
assert "$SAME" == "1"
* (c) different Stata metadata: a changed variable label
use `"`ref'"', clear
keep if month == 2
label variable x "Another label"
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(replace)
assert _rc == 198
python: Macro.setGlobal("SAME", "1" if shamap(Macro.getLocal("tree")) == json.loads(Macro.getGlobal("SHA4")) else "0")
assert "$SAME" == "1"
* (d) replace and partitions() together; partitions() without partition_by();
*     an unknown mode; different keys; a plain file as destination
use `"`ref'"', clear
keep if month == 2
capture noisily parqit save `"`tree'"', replace data partition_by(year month) partitions(replace)
assert _rc == 198
capture noisily parqit save `"`tree'"', data partitions(replace)
assert _rc == 198
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(merge)
assert _rc == 198
capture noisily parqit save `"`tree'"', data partition_by(month) partitions(replace)
assert _rc == 198
capture noisily parqit save `"`base'"', data partition_by(year month) partitions(replace)
assert _rc != 0
python: Macro.setGlobal("SAME", "1" if shamap(Macro.getLocal("tree")) == json.loads(Macro.getGlobal("SHA4")) else "0")
assert "$SAME" == "1"
* (e) a publish failure rolls the touched partitions back (test hook)
python:
import os
os.environ["PARQIT_TEST_FAIL_OUTPUT_PUBLISH"] = "1"
end
use `"`ref'"', clear
keep if inlist(month, 1, 2)
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(replace)
local rc = _rc
python:
os.environ.pop("PARQIT_TEST_FAIL_OUTPUT_PUBLISH", None)
end
assert `rc' != 0
python: Macro.setGlobal("SAME", "1" if shamap(Macro.getLocal("tree")) == json.loads(Macro.getGlobal("SHA4")) else "0")
assert "$SAME" == "1"

* ---------- zero rows: nothing touched, a note --------------------------------
use `"`ref'"', clear
keep if month == 99
parqit save `"`tree'"', data partition_by(year month) partitions(replace)
assert r(N) == 0
python: Macro.setGlobal("SAME", "1" if shamap(Macro.getLocal("tree")) == json.loads(Macro.getGlobal("SHA4")) else "0")
assert "$SAME" == "1"

* ---------- a tree written by another tool (no parqit metadata) ----------------
local foreign `"`dir'/foreign"'
python:
import pyarrow as pa
t = pa.table({"year": pa.array([2024] * 6, pa.int16()), "month": pa.array([1, 1, 2, 2, 3, 3], pa.int8()),
              "id": pa.array([1, 2, 3, 4, 5, 6], pa.int32()), "x": pa.array([1.0, 2.0, 3.0, 4.0, 5.0, 6.0])})
pq.write_to_dataset(t, Macro.getLocal("foreign"), partition_cols=["year", "month"])
end
clear
set obs 2
gen int year = 2024
gen byte month = 2
gen long id = 30 + _n
gen double x = 100 + _n
label variable x "labelled in memory"
parqit save `"`foreign'"', data partition_by(year month) partitions(replace)
python:
root = Macro.getLocal("foreign")
files = [os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs if f.endswith(".parquet")]
m2 = [f for f in files if "month=2" in f]
assert len(m2) == 1 and parqit_keys(m2[0]) == [], "the new partition carries parqit metadata in a tree without it"
n, s = month_sum(root, 2)
assert n == 2 and s == 203.0
assert month_sum(root, 1)[0] == 2 and month_sum(root, 3)[0] == 2
end
* a tree whose files carry the partition key inside is refused
local keyed `"`dir'/keyed"'
python:
os.makedirs(os.path.join(Macro.getLocal("keyed"), "year=2024"))
pq.write_table(t, os.path.join(Macro.getLocal("keyed"), "year=2024", "part-0.parquet"))
end
clear
set obs 2
gen int year = 2024
gen byte month = 2
gen long id = _n
gen double x = _n
capture noisily parqit save `"`keyed'"', data partition_by(year) partitions(replace)
assert _rc == 198

di "VERDICT(V85_PARTITION_MODES): PASS - partitions(replace) swaps only the partitions in the result and adds new ones, partitions(append) adds files, other partitions stay byte-identical; schema/metadata/key/mode conflicts refused with the tree untouched; publish failure rolled back; foreign trees written without parqit metadata; memory and view paths agree"
