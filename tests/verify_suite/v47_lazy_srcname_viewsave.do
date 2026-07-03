* v47 — F8: original-name provenance must survive the LAZY path.
*
* The eager loader records a sanitised foreign column's original file name in
* `char var[src_name]`. The lazy path lost it twice over: `parqit use` (lazy)
* + `parqit collect` set no char at all, and `parqit use` + `parqit save`
* wrote a parqit.chars without it, so the reloaded file had an empty char —
* the original name "unit cost" was unrecoverable. Now view_open records the
* provenance into the view's chars (the channel that already round-trips to
* collect and into a view save's parqit.chars), so lazy behaves exactly like
* eager. Oracle: pyarrow reads the written parqit.chars directly.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* foreign file: two names that need sanitising, one that does not
tempfile fbase
local foreign `"`fbase'.parquet"'
python:
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
t = pa.table({'raw name': pa.array([10.0, 20.0]),
              'unit cost': pa.array([1.5, 2.5]),
              'ok': pa.array([1, 2], pa.int32())})
pq.write_table(t, Macro.getLocal("foreign"))
end

* ---- lazy use + collect now carries src_name, like the eager loader ----
clear
parqit use using `"`foreign'"'
parqit collect, clear
assert `"`: char raw_name[src_name]'"' == "raw name"
assert `"`: char unit_cost[src_name]'"' == "unit cost"
assert `"`: char ok[src_name]'"' == ""
assert raw_name[2] == 20 & unit_cost[1] == 1.5 & ok[2] == 2

* ---- lazy use + disk save writes the provenance into parqit.chars ----
clear
parqit use using `"`foreign'"'
tempfile obase
local out `"`obase'.parquet"'
parqit save `"`out'"', replace
parqit close _all

* independent oracle: the chars are on disk, third-party readable
python:
import json, pyarrow.parquet as pq
from sfi import Macro, SFIToolkit
md = pq.read_metadata(Macro.getLocal("out")).metadata
chars = json.loads(md[b'parqit.chars'].decode())
ok = (chars.get('raw_name', {}).get('src_name') == 'raw name'
      and chars.get('unit_cost', {}).get('src_name') == 'unit cost'
      and 'src_name' not in chars.get('ok', {}))
SFIToolkit.stata('local disk_oracle = ' + ('1' if ok else '0'))
end
assert `disk_oracle' == 1

* reloading the saved file restores the chars (and the payload)
parqit use using `"`out'"', clear
assert `"`: char raw_name[src_name]'"' == "raw name"
assert `"`: char unit_cost[src_name]'"' == "unit cost"
assert raw_name[1] == 10 & unit_cost[2] == 2.5 & ok[1] == 1

* ---- provenance follows a view rename (chars re-key, META-2) ----
clear
parqit use using `"`foreign'"'
parqit rename raw_name better
tempfile o2base
local out2 `"`o2base'.parquet"'
parqit save `"`out2'"', replace
parqit close _all
parqit use using `"`out2'"', clear
assert `"`: char better[src_name]'"' == "raw name"
assert better[2] == 20

* ---- derived columns carry no provenance char ----
clear
parqit use using `"`foreign'"'
parqit gen double twice = 2 * unit_cost
tempfile o3base
local out3 `"`o3base'.parquet"'
parqit save `"`out3'"', replace
parqit close _all
parqit use using `"`out3'"', clear
assert `"`: char twice[src_name]'"' == ""
assert `"`: char unit_cost[src_name]'"' == "unit cost"
assert twice[1] == 3

di "VERDICT(V47_LAZY_SRCNAME): PASS - lazy collect and view-save carry char[src_name] like the eager path; renames re-key it; derived columns stay clean"
