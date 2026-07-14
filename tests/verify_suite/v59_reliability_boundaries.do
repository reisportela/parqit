* v59 — GO-GO data-integrity boundaries from the 2026-07-14 adversarial audit.
* Independent oracles inspect the filesystem and physical Parquet payloads;
* expected refusals must be loud, atomic, and leave no published target.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

tempfile stem
local base `"`stem'"'

* ================= REL-001: package never deletes predictable siblings =====
clear
set obs 4
gen long id = _n
gen byte g = mod(_n, 2)

local flat `"`base'_owned.parquet"'
local flat_tmp `"`flat'.parqit_tmp"'
local flat_old `"`flat'.parqit_old"'
tempname fh
file open `fh' using `"`flat_tmp'"', write text replace
file write `fh' "USER_TMP_SENTINEL"
file close `fh'
file open `fh' using `"`flat_old'"', write text replace
file write `fh' "USER_OLD_SENTINEL"
file close `fh'
parqit save `"`flat'"', replace data
confirm file `"`flat_tmp'"'
confirm file `"`flat_old'"'

local tree `"`base'_partitioned"'
local tree_tmp `"`tree'.parqit_tmp"'
local tree_old `"`tree'.parqit_old"'
mkdir `"`tree_tmp'"'
mkdir `"`tree_old'"'
file open `fh' using `"`tree_tmp'/USER_DATA.txt"', write text replace
file write `fh' "USER_TREE_TMP"
file close `fh'
file open `fh' using `"`tree_old'/USER_DATA.txt"', write text replace
file write `fh' "USER_TREE_OLD"
file close `fh'
parqit save `"`tree'"', replace data partition_by(g)
confirm file `"`tree_tmp'/USER_DATA.txt"'
confirm file `"`tree_old'/USER_DATA.txt"'

local locked `"`base'_locked.parquet"'
mkdir `"`locked'.parqit_lock"'
file open `fh' using `"`locked'.parqit_lock/USER_DATA.txt"', write text replace
file write `fh' "USER_LOCK_SENTINEL"
file close `fh'
capture noisily parqit save `"`locked'"', replace data
local lockrc = _rc
assert `lockrc' != 0
confirm file `"`locked'.parqit_lock/USER_DATA.txt"'

python:
import glob, os
from sfi import Macro

base = Macro.getLocal("base")
flat = Macro.getLocal("flat")
tree = Macro.getLocal("tree")
locked = Macro.getLocal("locked")
assert open(flat + ".parqit_tmp", encoding="utf-8").read() == "USER_TMP_SENTINEL"
assert open(flat + ".parqit_old", encoding="utf-8").read() == "USER_OLD_SENTINEL"
assert open(tree + ".parqit_tmp/USER_DATA.txt", encoding="utf-8").read() == "USER_TREE_TMP"
assert open(tree + ".parqit_old/USER_DATA.txt", encoding="utf-8").read() == "USER_TREE_OLD"
assert open(locked + ".parqit_lock/USER_DATA.txt", encoding="utf-8").read() == "USER_LOCK_SENTINEL"
assert not os.path.exists(locked)
assert glob.glob(flat + ".parqit_txn_*") == []
assert glob.glob(tree + ".parqit_txn_*") == []
end

* Publication failure after an existing partition tree was set aside must
* restore it. If that rollback itself fails, the prior bytes must remain in a
* named recovery root rather than being deleted by automatic cleanup.
replace id = id + 100
python:
import os
os.environ["PARQIT_TEST_FAIL_OUTPUT_PUBLISH"] = "1"
end
capture noisily parqit save `"`tree'"', replace data partition_by(g)
local publishrc = _rc
assert `publishrc' != 0
python:
import glob, os
import pyarrow.parquet as pq
from sfi import Macro

os.environ.pop("PARQIT_TEST_FAIL_OUTPUT_PUBLISH", None)
tree = Macro.getLocal("tree")
assert sorted(pq.read_table(tree)["id"].to_pylist()) == [1, 2, 3, 4]
assert glob.glob(tree + ".parqit_txn_*") == []
end

python:
import os
os.environ["PARQIT_TEST_FAIL_OUTPUT_PUBLISH"] = "1"
os.environ["PARQIT_TEST_FAIL_OUTPUT_ROLLBACK"] = "1"
end
capture noisily parqit save `"`tree'"', replace data partition_by(g)
local rollbackrc = _rc
assert `rollbackrc' != 0
python:
import glob, os
import pyarrow.parquet as pq
from sfi import Macro

os.environ.pop("PARQIT_TEST_FAIL_OUTPUT_PUBLISH", None)
os.environ.pop("PARQIT_TEST_FAIL_OUTPUT_ROLLBACK", None)
tree = Macro.getLocal("tree")
roots = glob.glob(tree + ".parqit_txn_*")
assert not os.path.exists(tree)
assert len(roots) == 1, roots
recovery = os.path.join(roots[0], "old")
assert sorted(pq.read_table(recovery)["id"].to_pylist()) == [1, 2, 3, 4]
# Complete the recovery so this regression leaves no intentional debris.
os.rename(recovery, tree)
os.rmdir(roots[0])
assert sorted(pq.read_table(tree)["id"].to_pylist()) == [1, 2, 3, 4]
end

* ================= DATA-002/004/005: hostile physical fixtures =============
local u64in `"`base'_u64.parquet"'
local u64out `"`base'_u64_grouped.parquet"'
local decin `"`base'_decimal.parquet"'
local decout `"`base'_decimal_grouped.parquet"'
local nulin `"`base'_strl_nul.parquet"'
local nulfast `"`base'_strl_nul_fast.parquet"'
local nulgen `"`base'_strl_nul_general.parquet"'
local nullazy `"`base'_strl_nul_lazy.parquet"'
local nulpart `"`base'_strl_nul_partitioned"'
local tsin `"`base'_timestamp_extreme.parquet"'
local tsout `"`base'_timestamp_extreme_out.parquet"'
local tsnegin `"`base'_timestamp_extreme_negative.parquet"'
local tsnegout `"`base'_timestamp_extreme_negative_out.parquet"'

python:
from decimal import Decimal
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro

pq.write_table(pa.table({"key": pa.array([2**53, 2**53 + 1], type=pa.uint64())}),
               Macro.getLocal("u64in"))
pq.write_table(pa.table({"key": pa.array([Decimal(2**53), Decimal(2**53 + 1)],
                                          type=pa.decimal128(20, 0))}),
               Macro.getLocal("decin"))
payload = "a" * 2100 + "\x00" + "TAIL"
pq.write_table(pa.table({"id": pa.array([1], type=pa.int32()),
                         "payload": pa.array([payload])}),
               Macro.getLocal("nulin"))
# raw us chosen so epoch-shifted Stata ms is exactly 2^53+1
raw_us = 9006883635540993000
ts = pa.array([raw_us], type=pa.int64()).cast(pa.timestamp("us"))
pq.write_table(pa.table({"event": ts}), Macro.getLocal("tsin"))
raw_us_negative = -9007514873940993000
ts_negative = pa.array([raw_us_negative], type=pa.int64()).cast(pa.timestamp("us"))
pq.write_table(pa.table({"event": ts_negative}), Macro.getLocal("tsnegin"))
end

clear
parqit use using `"`u64in'"'
parqit contract key, freq(freq)
parqit save `"`u64out'"', replace
parqit close _all

clear
parqit use using `"`decin'"'
parqit contract key, freq(freq)
parqit save `"`decout'"', replace
parqit close _all

python:
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro

u = pq.read_table(Macro.getLocal("u64out"))
assert u.num_rows == 2
assert sorted(u["key"].to_pylist()) == [2**53, 2**53 + 1]
assert sorted(u["freq"].to_pylist()) == [1, 1]
assert pa.types.is_uint64(u.schema.field("key").type)
d = pq.read_table(Macro.getLocal("decout"))
assert d.num_rows == 2
assert [int(x) for x in sorted(d["key"].to_pylist())] == [2**53, 2**53 + 1]
assert sorted(d["freq"].to_pylist()) == [1, 1]
assert pa.types.is_decimal(d.schema.field("key").type)
end

* Binary strL: both direct writers refuse without publishing; an untouched
* lazy Parquet-to-Parquet save remains lossless because it need not enter Stata.
clear
parqit use using `"`nulin'"', clear
local payload_type : type payload
assert `"`payload_type'"' == "strL"
capture noisily parqit save `"`nulfast'"', replace data
local nulfastrc = _rc
assert `nulfastrc' == 198
replace id = 2
capture noisily parqit save `"`nulgen'"', replace data
local nulgenrc = _rc
assert `nulgenrc' == 198
capture noisily parqit save `"`nulpart'"', replace data partition_by(id)
local nulpartrc = _rc
assert `nulpartrc' == 198
clear
parqit use using `"`nulin'"'
parqit save `"`nullazy'"', replace
parqit close _all

python:
import os
import pyarrow.parquet as pq
from sfi import Macro

for name in ("nulfast", "nulgen", "nulpart"):
    assert not os.path.exists(Macro.getLocal(name)), name
v = pq.read_table(Macro.getLocal("nullazy"))["payload"].to_pylist()[0]
assert len(v) == 2105 and v[2100] == "\x00" and v.endswith("TAIL")
end

* An exact millisecond that binary64 cannot represent is refused on eager,
* collect, and lazy save. Every failed materializer preserves prior state.
clear
set obs 1
gen long sentinel = 777
capture noisily parqit use using `"`tsin'"', clear
local tserc = _rc
assert `tserc' != 0
assert _N == 1 & sentinel[1] == 777
parqit use using `"`tsin'"'
capture noisily parqit save `"`tsout'"', replace
local tssaverc = _rc
assert `tssaverc' != 0
capture noisily parqit collect, clear
local tscollectrc = _rc
assert `tscollectrc' != 0
assert _N == 1 & sentinel[1] == 777
parqit close _all
python:
import os
from sfi import Macro
assert not os.path.exists(Macro.getLocal("tsout"))
end

capture noisily parqit use using `"`tsnegin'"', clear
local tsnegerc = _rc
assert `tsnegerc' != 0
assert _N == 1 & sentinel[1] == 777
parqit use using `"`tsnegin'"'
capture noisily parqit save `"`tsnegout'"', replace
local tsnegsaverc = _rc
assert `tsnegsaverc' != 0
capture noisily parqit collect, clear
local tsnegcollectrc = _rc
assert `tsnegcollectrc' != 0
assert _N == 1 & sentinel[1] == 777
parqit close _all
python:
import os
from sfi import Macro
assert not os.path.exists(Macro.getLocal("tsnegout"))
end

* ================= DATA-003 / TYPE-007/008: path and physical type parity ===
local typesrc `"`base'_types_source.parquet"'
local typelazy `"`base'_types_lazy.parquet"'
local typedirect `"`base'_types_direct.parquet"'
clear
set obs 1
gen long id = 1
gen double x = 0
parqit save `"`typesrc'"', replace data
parqit use using `"`typesrc'"'
parqit gen byte vb = 42
parqit gen int vi = 42
parqit gen long vl = 42
parqit gen float vf = 42
parqit gen double vd = 42
parqit gen vu = 42
parqit replace x = 9007199254740993
parqit save `"`typelazy'"', replace
parqit collect, clear
assert x[1] == 9007199254740992
assert `"`: type vb'"' == "byte"
assert `"`: type vi'"' == "int"
assert `"`: type vl'"' == "long"
assert `"`: type vf'"' == "float"
assert `"`: type vd'"' == "double"
assert `"`: type vu'"' == "double"
assert `"`: type x'"' == "double"
parqit save `"`typedirect'"', replace data
parqit close _all

python:
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro

lazy = pq.read_table(Macro.getLocal("typelazy"))
direct = pq.read_table(Macro.getLocal("typedirect"))
want = {"vb": pa.int8(), "vi": pa.int16(), "vl": pa.int32(),
        "vf": pa.float32(), "vd": pa.float64(), "vu": pa.float64(),
        "x": pa.float64()}
for name, typ in want.items():
    assert lazy.schema.field(name).type == typ, (name, lazy.schema.field(name).type, typ)
    assert direct.schema.field(name).type == typ, (name, direct.schema.field(name).type, typ)
assert lazy["x"].to_pylist() == [float(2**53)]
assert direct["x"].to_pylist() == [float(2**53)]
end

* Replace promotion must remain non-lossy and its saved physical type must be
* reloadable without stale narrow metadata forcing truncation.
local promsrc `"`base'_promotion_source.parquet"'
local promout `"`base'_promotion_out.parquet"'
clear
set obs 1
gen byte b = 1
gen int i = 1
gen float f = 1
gen double d = 1
parqit save `"`promsrc'"', replace data
parqit use using `"`promsrc'"'
parqit replace b = 200
parqit replace i = 40000
parqit replace f = 16777217
parqit gen double f_after = f
parqit replace d = 4000000000
parqit gen double d_square = d*d
parqit save `"`promout'"', replace
parqit use using `"`promout'"', clear
assert b[1] == 200 & i[1] == 40000
assert f[1] == 16777216 & f_after[1] == 16777216
assert d[1] == 4000000000 & d_square[1] == 1.6e19
assert `"`: type b'"' == "int"
assert `"`: type i'"' == "long"
assert `"`: type f'"' == "float"
assert `"`: type d'"' == "double"
parqit close _all
python:
import pyarrow as pa
import pyarrow.parquet as pq
from sfi import Macro
t = pq.read_table(Macro.getLocal("promout"))
assert t.schema.field("f").type == pa.float32()
assert t.schema.field("d").type == pa.float64()
assert t["f"].to_pylist() == [16777216.0]
assert t["f_after"].to_pylist() == [16777216.0]
assert t["d_square"].to_pylist() == [1.6e19]
end

* DATE-009: native-invalid literals fail at the command and do not add columns.
clear
parqit use using `"`typesrc'"'
capture noisily parqit gen double badtc = tc(01jan2020 00:00:59.9999)
assert _rc == 198
capture noisily parqit gen double badd = td(01jan0099)
assert _rc == 198
parqit collect, clear
capture confirm variable badtc
assert _rc == 111
capture confirm variable badd
assert _rc == 111
parqit close _all

di "VERDICT(V59_RELIABILITY_BOUNDARIES): PASS - ownership/recovery, exact keys, NUL, timestamp refusal, materializer parity, types and date bounds"
