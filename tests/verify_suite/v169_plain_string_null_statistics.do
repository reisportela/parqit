* A true constant min/max must retain the NULL bitmap of a PLAIN string page.
* Independent PyArrow fixtures and saved-payload checks: DuckDB issue #27456.
clear all
set more off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile fixture output
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
base = Macro.getLocal("fixture")
table = pa.table({"rid": pa.array([1, 2, 3], pa.int32()),
                  "c": pa.array([None, None, "abcdefgh"], pa.string())})
for name, dictionary, statistics in [("plain", False, True),
                                      ("dictionary", True, True),
                                      ("nostats", False, False)]:
    pq.write_table(table, base + "_" + name + ".parquet",
                   use_dictionary=dictionary, write_statistics=statistics)
    assert pq.read_table(base + "_" + name + ".parquet").equals(table)
multi = pa.table({"rid": pa.array(range(1, 4097), pa.int32()),
                  "c": pa.array([None] * 256 + ["abcdefgh"] * 3840, pa.string())})
for name, dictionary, statistics, index in [("multipage", False, True, False),
                                           ("pageindex", False, True, True),
                                           ("multidict", True, True, False),
                                           ("multinostats", False, False, False)]:
    pq.write_table(multi, base + "_" + name + ".parquet", compression="NONE",
                   use_dictionary=dictionary, write_statistics=statistics,
                   write_page_index=index, data_page_size=256, write_batch_size=64)
    assert pq.read_table(base + "_" + name + ".parquet").equals(multi)
end
capture noisily {
    foreach variant in plain dictionary nostats {
        clear
        set obs 1
        gen long sentinel = 12345
        parqit use using `"`fixture'_`variant'.parquet"'
        parqit keep if missing(c)
        parqit count
        assert r(N) == 2
        parqit save `"`output'_`variant'.parquet"', view
        assert _N == 1 & sentinel == 12345
        parqit collect, clear
        assert _N == 2 & missing(c)
        sort rid
        assert rid == _n
        parqit close _all
    }
    foreach variant in multipage pageindex multidict multinostats {
        parqit use using `"`fixture'_`variant'.parquet"'
        parqit keep if missing(c)
        parqit collect, clear
        assert _N == 256 & missing(c)
        sort rid
        assert rid == _n
        parqit close _all
        parqit use using `"`fixture'_`variant'.parquet"'
        parqit keep if !missing(c)
        parqit collect, clear
        assert _N == 3840 & c == "abcdefgh"
        sort rid
        assert rid == _n + 256
        parqit close _all
    }
}
local rc = _rc
if `rc' {
    di "VERDICT(V169_PLAIN_NULL_STATS): FAIL - rc=`rc'; PLAIN string NULL filtering changed rows"
    exit `rc'
}
python:
from sfi import Macro
import pyarrow.parquet as pq
for name in ("plain", "dictionary", "nostats"):
    saved = pq.read_table(Macro.getLocal("output") + "_" + name + ".parquet")
    assert saved.num_rows == 2
    assert saved["rid"].to_pylist() == [1, 2]
    assert saved["c"].to_pylist() in ([None, None], ["", ""])
end
di "VERDICT(V169_PLAIN_NULL_STATS): PASS - single/multipage, page-index, dictionary and absent-statistics filters preserve rows and payload"
