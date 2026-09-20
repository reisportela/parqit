* V106 — FILENAME-1: `parqit use ..., filename(newvar)` adds a string column
*   holding the path each row was read from. The hazard is structural, not
*   cosmetic: DuckDB appends that column AFTER the files' own leaves and
*   BEFORE the Hive partition keys, so a planner that counts scan columns
*   against leaf names would silently lose exact-name recovery on a flat glob
*   and, over a Hive tree, would tag the provenance column as a partition key
*   (and then retype it from the manifest). This pins: the values against the
*   duckdb CLI and against the files the test itself wrote; the eager and the
*   lazy form; a Hive tree keeping BOTH the key and the provenance column with
*   the right types; a CSV glob; a varlist still getting the column; a name
*   that clashes with a source column or a partition key refused with the
*   dataset in memory untouched; an illegal name refused; and a save+re-read
*   round-tripping the column as ordinary Parquet text (pyarrow oracle).
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
local fails 0

python:
from sfi import Macro
import os
import pyarrow as pa, pyarrow.parquet as pq
d = Macro.getLocal("dir")
# three flat files with DIFFERENT row counts: a per-file count is a real check
for i, n in enumerate([2, 3, 4], start=1):
    pq.write_table(pa.table({"id": pa.array(list(range(n)), pa.int32()),
                             "val": pa.array([float(i)] * n, pa.float64())}),
                   os.path.join(d, "part_%d.parquet" % i))
# a Hive tree: year= directories, and a payload column of its own
for y in (2020, 2021):
    os.makedirs(os.path.join(d, "hive", "year=%d" % y))
    pq.write_table(pa.table({"id": pa.array([y, y + 1], pa.int32()),
                             "g": pa.array(["a", "b"])}),
                   os.path.join(d, "hive", "year=%d" % y, "f.parquet"))
# a two-file delimited-text glob
open(os.path.join(d, "t_1.csv"), "w").write("k,v\n1,a\n2,b\n")
open(os.path.join(d, "t_2.csv"), "w").write("k,v\n3,c\n")
# a column whose name only becomes a legal Stata name after sanitising
pq.write_table(pa.table({"my file": pa.array([1, 2], pa.int32())}),
               os.path.join(d, "spaced.parquet"))
open(os.path.join(d, "spaced.csv"), "w").write("my col\n1\n2\n")
# a relaxed union: the second file has a column the first does not
pq.write_table(pa.table({"a": pa.array([1], pa.int32())}),
               os.path.join(d, "u_1.parquet"))
pq.write_table(pa.table({"a": pa.array([2, 3], pa.int32()),
                         "b": pa.array([9.5, 8.5], pa.float64())}),
               os.path.join(d, "u_2.parquet"))
end

* ---------- 1. eager glob: exact paths and per-file counts ------------------
* Oracle A: the duckdb CLI's own filename column (the engine's ground truth).
tempfile ora
shell duckdb -csv -c "SELECT filename, count(*) AS n FROM read_parquet('`dir'/part_*.parquet', filename = true) GROUP BY 1 ORDER BY 1" > "`ora'"

parqit use `"`dir'/part_*.parquet"', clear filename(srcfile)
assert _N == 9
qui ds
assert "`r(varlist)'" == "id val srcfile"
assert substr("`: type srcfile'", 1, 3) == "str"
* Oracle B: the paths the test itself wrote (the CLI and parqit must agree
* with the file system, not merely with each other)
forvalues i = 1/3 {
    qui count if srcfile == `"`dir'/part_`i'.parquet"'
    local seen`i' = r(N)
    assert r(N) == `i' + 1
}
qui count if !inlist(srcfile, `"`dir'/part_1.parquet"', ///
    `"`dir'/part_2.parquet"', `"`dir'/part_3.parquet"')
assert r(N) == 0
* the provenance column carries a note and no source-name characteristic
assert `"`: char srcfile[note1]'"' == "source file of each row (filename())"
assert "`: char srcfile[src_name]'" == ""
* every payload value still lands on the row of its own file
assert val == 1 if srcfile == `"`dir'/part_1.parquet"'
assert val == 3 if srcfile == `"`dir'/part_3.parquet"'

python:
from sfi import Macro, SFIToolkit
import csv, os
rows = {}
with open(Macro.getLocal("ora"), newline="", encoding="utf-8") as fh:
    for r in csv.DictReader(fh):
        rows[r["filename"]] = int(r["n"])
d = Macro.getLocal("dir")
want = {os.path.join(d, "part_%d.parquet" % i): i + 1 for i in (1, 2, 3)}
Macro.setLocal("cli_ok", "1" if rows == want else "0")
if rows != want:
    SFIToolkit.displayln("duckdb CLI oracle: %s" % rows)
end
if ("`cli_ok'" != "1") {
    di as err "FAIL: the duckdb CLI oracle disagrees with the files written"
    local ++fails
}
if (`seen1' != 2 | `seen2' != 3 | `seen3' != 4) {
    di as err "FAIL: per-file row counts `seen1'/`seen2'/`seen3' != 2/3/4"
    local ++fails
}

* ---------- 2. lazy form: a first-class view column -------------------------
parqit use using `"`dir'/part_*.parquet"', filename(srcfile)
assert r(k) == 3
parqit describe
parqit keep if strpos(srcfile, "part_2.parquet") > 0
parqit collect, clear
assert _N == 3
qui ds
assert "`r(varlist)'" == "id val srcfile"
assert srcfile[1] == `"`dir'/part_2.parquet"'
assert `"`: char srcfile[note1]'"' == "source file of each row (filename())"
parqit close _all

* ---------- 3. a Hive tree keeps BOTH the key and the provenance column -----
parqit use `"`dir'/hive"', clear filename(srcfile)
assert _N == 4
qui ds
* the engine's bind order: the files' leaves, then filename(), then the keys
assert "`r(varlist)'" == "id g srcfile year"
assert "`: type year'" == "int"
assert substr("`: type srcfile'", 1, 3) == "str"
assert "`: type g'" == "str1"
qui count if year == 2020 & srcfile == `"`dir'/hive/year=2020/f.parquet"'
assert r(N) == 2
qui count if year == 2021 & srcfile == `"`dir'/hive/year=2021/f.parquet"'
assert r(N) == 2
* the key must not have been read as text, nor the path as a partition value
qui count if missing(year)
assert r(N) == 0
* and the same through the lazy path (a direct-read collect replans the scan)
parqit use using `"`dir'/hive"', filename(srcfile)
parqit collect, clear
qui ds
assert "`r(varlist)'" == "id g srcfile year"
assert "`: type year'" == "int"
qui count if year == 2021 & srcfile == `"`dir'/hive/year=2021/f.parquet"'
assert r(N) == 2
parqit close _all

* ---------- 4. a delimited-text glob ---------------------------------------
parqit use `"`dir'/t_*.csv"', clear filename(srcfile)
assert _N == 3
qui ds
assert "`r(varlist)'" == "k v srcfile"
qui count if srcfile == `"`dir'/t_1.csv"'
assert r(N) == 2
qui count if srcfile == `"`dir'/t_2.csv"'
assert r(N) == 1
assert v == "c" if k == 3

* ---------- 5. a varlist still gets the column ------------------------------
parqit use id using `"`dir'/part_*.parquet"', clear filename(srcfile)
qui ds
assert "`r(varlist)'" == "id srcfile"
parqit use srcfile id using `"`dir'/part_*.parquet"', clear filename(srcfile)
qui ds
assert "`r(varlist)'" == "srcfile id"

* ---------- 6. a clash refuses, the dataset in memory untouched -------------
sysuse auto, clear
local nbefore = _N
qui ds
local vbefore "`r(varlist)'"
capture noisily parqit use `"`dir'/part_*.parquet"', clear filename(val)
if (_rc != 198) {
    di as err "FAIL: a clash with a file column returned rc " _rc ", expected 198"
    local ++fails
}
capture noisily parqit use `"`dir'/hive"', clear filename(YEAR)
if (_rc != 198) {
    di as err "FAIL: a case clash with a Hive key returned rc " _rc ", expected 198"
    local ++fails
}
capture noisily parqit use using `"`dir'/part_*.parquet"', filename(id)
if (_rc != 198) {
    di as err "FAIL: a lazy clash returned rc " _rc ", expected 198"
    local ++fails
}
capture noisily parqit use `"`dir'/part_*.parquet"', clear filename(2bad)
if (_rc == 0) {
    di as err "FAIL: an illegal variable name was accepted"
    local ++fails
}
* a name the SANITISER would hand to a source column: "my file" loads as
* my_file, so filename(my_file) must refuse rather than let one of the two be
* quietly suffixed. Parquet and CSV (the header-recovery path) alike.
capture noisily parqit use `"`dir'/spaced.parquet"', clear filename(my_file)
if (_rc != 198) {
    di as err "FAIL: a sanitised-name clash returned rc " _rc ", expected 198"
    local ++fails
}
capture noisily parqit use using `"`dir'/spaced.parquet"', filename(my_file)
if (_rc != 198) {
    di as err "FAIL: a lazy sanitised-name clash returned rc " _rc ", expected 198"
    local ++fails
}
capture noisily parqit use `"`dir'/spaced.csv"', clear filename(my_col)
if (_rc != 198) {
    di as err "FAIL: a sanitised CSV header clash returned rc " _rc ", expected 198"
    local ++fails
}
* a .dta source is read through a package-owned bridge: refuse, do not report
* the bridge's path
qui save `"`dir'/auto.dta"', replace
capture noisily parqit use `"`dir'/auto.dta"', clear filename(srcfile)
if (_rc != 198) {
    di as err "FAIL: filename() on a .dta source returned rc " _rc ", expected 198"
    local ++fails
}
qui ds
if (_N != `nbefore' | "`r(varlist)'" != "`vbefore'") {
    di as err "FAIL: a refused filename() disturbed the dataset in memory"
    local ++fails
}
assert make[1] != ""
* the sanitised name still belongs to the source when filename() is absent
parqit use `"`dir'/spaced.parquet"', clear
qui ds
assert "`r(varlist)'" == "my_file"
assert "`: char my_file[src_name]'" == "my file"

* ---------- 6b. a relaxed union carries the column too ----------------------
parqit use `"`dir'/u_*.parquet"', clear relaxed filename(srcfile)
qui ds
assert "`r(varlist)'" == "a b srcfile"
assert _N == 3
qui count if srcfile == `"`dir'/u_1.parquet"'
assert r(N) == 1
qui count if srcfile == `"`dir'/u_2.parquet"'
assert r(N) == 2
assert missing(b) if srcfile == `"`dir'/u_1.parquet"'
assert b == 9.5 if a == 2

* ---------- 7. save and re-read: ordinary Parquet text ----------------------
parqit use `"`dir'/part_*.parquet"', clear filename(srcfile)
parqit save `"`dir'/out.parquet"', replace
parqit use `"`dir'/out.parquet"', clear
assert _N == 9
qui ds
assert "`r(varlist)'" == "id val srcfile"
assert substr("`: type srcfile'", 1, 3) == "str"
qui count if srcfile == `"`dir'/part_3.parquet"'
assert r(N) == 4
assert `"`: char srcfile[note1]'"' == "source file of each row (filename())"

python:
from sfi import Macro, SFIToolkit
import os, collections
import pyarrow.parquet as pq
d = Macro.getLocal("dir")
t = pq.read_table(os.path.join(d, "out.parquet"))
ok = str(t.schema.field("srcfile").type) == "string"
counts = collections.Counter(t.column("srcfile").to_pylist())
want = {os.path.join(d, "part_%d.parquet" % i): i + 1 for i in (1, 2, 3)}
ok = ok and dict(counts) == want
# the file must stay standard Parquet: no parqit-only trick to carry the paths
Macro.setLocal("save_ok", "1" if ok else "0")
if not ok:
    SFIToolkit.displayln("pyarrow oracle: %s / %s" % (t.schema.field("srcfile").type, dict(counts)))
end
if ("`save_ok'" != "1") {
    di as err "FAIL: the saved provenance column is not plain Parquet text"
    local ++fails
}
* the lazy path writes it without collecting, and a later use reads it back
parqit use using `"`dir'/part_*.parquet"', filename(srcfile)
parqit save `"`dir'/out_lazy.parquet"', replace
parqit close _all
parqit use `"`dir'/out_lazy.parquet"', clear
assert _N == 9
qui ds
assert "`r(varlist)'" == "id val srcfile"
qui count if srcfile == `"`dir'/part_2.parquet"'
assert r(N) == 3
assert `"`: char srcfile[note1]'"' == "source file of each row (filename())"

* ---------- 8. without filename(), nothing changes --------------------------
parqit use `"`dir'/hive"', clear
qui ds
assert "`r(varlist)'" == "id g year"
assert "`: type year'" == "int"
parqit use `"`dir'/part_*.parquet"', clear
qui ds
assert "`r(varlist)'" == "id val"
assert _N == 9

if (`fails' == 0) {
    di as result "VERDICT(V106_FILENAME_PROVENANCE): PASS - filename() adds the engine's provenance column as a first-class Stata variable (eager, lazy, Hive tree, CSV glob, varlist, save round-trip) with the duckdb CLI and pyarrow as oracles; clashes and illegal names refuse with the dataset untouched"
}
else {
    di as err "VERDICT(V106_FILENAME_PROVENANCE): FAIL - `fails' check(s)"
}
