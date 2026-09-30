* V142 — a tie in a declared sort follows the source's physical row order
*   (ORDER-CARRIER-1, 2026-09-29). auto, saved sorted by foreign alone,
*   reopens with that sort declared: within a tie, _n, keep in, list in,
*   collect, save and the Obs of duplicates list follow the rows' order in the
*   file — native Stata's order after use — at every evaluation of the plan.
* Oracles: native Stata (use, keep in, list, sort ..., stable, duplicates
*   list) and pyarrow (the saved file: no hidden column, rows in order).
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem nat nlog plog

* 52 domestic and 22 foreign cars tie on the declared sort
sysuse auto, clear
gen int id = _n
save `"`stem'_auto.dta"', replace
parqit save `"`stem'_auto.parquet"', data replace

* _n in the plan, the collected order and the file order agree
parqit use using `"`stem'_auto.parquet"', name(t)
parqit gen long n = _n
parqit collect, clear
assert n == _n & id == _n

* keep in takes the rows native keep in takes, across the tie boundary
parqit use using `"`stem'_auto.parquet"', name(t)
parqit keep in 50/54
parqit collect, clear
assert _N == 5 & id == 49 + _n

* replacing the sort key ends the declared sort; the order it had stays
parqit use using `"`stem'_auto.parquet"', name(t)
parqit replace foreign = 1 - foreign
parqit gen long n = _n
parqit collect, clear
assert n == _n & id == _n

* a new sort breaks its ties in file order: native sort ..., stable
use `"`stem'_auto.dta"', clear
sort rep78, stable
gen int nat = _n
keep id nat
save `"`nat'"', replace
parqit use using `"`stem'_auto.parquet"', name(t)
parqit sort rep78
parqit gen long n = _n
parqit collect, clear
assert n == _n
merge 1:1 id using `"`nat'"', assert(match) nogenerate
assert n == nat

* descending as well
parqit use using `"`stem'_auto.parquet"', name(t)
parqit gsort -rep78
parqit gen long n = _n
parqit collect, clear
assert n == _n
assert id > id[_n - 1] if _n > 1 & rep78 == rep78[_n - 1]

* a dropped sort key keeps deciding the ties it decided, as in native Stata
use `"`stem'_auto.dta"', clear
sort rep78 price, stable
gen int nat = _n
keep id nat
save `"`nat'"', replace
parqit use using `"`stem'_auto.parquet"', name(t)
parqit sort rep78 price
parqit drop price
parqit gen long n = _n
parqit collect, clear
assert n == _n
merge 1:1 id using `"`nat'"', assert(match) nogenerate
assert n == nat

* a saved view holds its own columns only, rows in the plan's order
parqit use using `"`stem'_auto.parquet"', name(t)
parqit keep id foreign
parqit save `"`stem'_out.parquet"', replace
python:
from sfi import Macro
import pyarrow.parquet as pq
t = pq.read_table(Macro.getLocal("stem") + "_out.parquet")
assert t.column_names == ["id", "foreign"], t.column_names
assert t.column("id").to_pylist() == list(range(1, 75)), t.column("id").to_pylist()
end

* two files, each sorted by foreign and holding both groups: a tie follows the
* file, then the row — in a plain collect, an eager use, _n and keep in alike
use `"`stem'_auto.dta"', clear
preserve
keep if id <= 30 | inrange(id, 53, 62)
parqit save `"`stem'_part_a.parquet"', data replace
restore
keep if inrange(id, 31, 52) | id >= 63
parqit save `"`stem'_part_b.parquet"', data replace
parqit use using `"`stem'_part_*.parquet"', name(t)
parqit collect, clear
assert _N == 74 & id == _n
parqit use `"`stem'_part_*.parquet"', clear
assert _N == 74 & id == _n
parqit use using `"`stem'_part_*.parquet"', name(t)
parqit gen long n = _n
parqit collect, clear
assert n == _n & id == _n
parqit use using `"`stem'_part_*.parquet"', name(t)
parqit keep in 28/34
parqit collect, clear
assert _N == 7 & id == 27 + _n

* list in and duplicates list print the rows and Obs native Stata prints
use `"`stem'_auto.dta"', clear
log using `"`nlog'"', text replace name(v142n)
list id make foreign in 50/54
duplicates list rep78 foreign
log close v142n
parqit use using `"`stem'_auto.parquet"', name(t)
log using `"`plog'"', text replace name(v142p)
parqit list id make foreign in 50/54
parqit duplicates list rep78 foreign, limit(1000)
log close v142p
python:
import re
from sfi import Macro
def lines(path):
    rows, table = [], []
    for s in open(path, encoding="utf-8"):
        s = s.rstrip("\n")
        m = re.match(r"^\s*\d+\. (\|.*)$", s)
        if m and len(rows) < 5:
            rows.append(m.group(1))          # list: row labels differ by design
        elif s.startswith("  +") or s.startswith("  |") or s.startswith("Duplicates in terms of"):
            table.append(s)
    return rows, table
nat, pq_ = lines(Macro.getLocal("nlog")), lines(Macro.getLocal("plog"))
assert len(nat[0]) == 5 and nat[0] == pq_[0], (nat[0], pq_[0])
assert len(nat[1]) > 20 and nat[1] == pq_[1], \
    "\n".join(a + "  <>  " + b for a, b in zip(nat[1], pq_[1]) if a != b)
end

* a source column named like the engine's row positions: same values, and a
* note that the order within ties is the engine's
sysuse auto, clear
gen long file_row_number = 1000 + 7 * _n
save `"`stem'_clash.dta"', replace
parqit save `"`stem'_clash.parquet"', data replace
log using `"`plog'"', text replace name(v142c)
parqit use using `"`stem'_clash.parquet"', name(c)
log close v142c
python:
from sfi import Macro
text = open(Macro.getLocal("plog"), encoding="utf-8").read().replace("\n> ", "")
assert 'column "file_row_number" hides the engine\'s physical row positions' in text, text
end
parqit collect, clear
sort file_row_number
cf _all using `"`stem'_clash.dta"'

* a single file needs only the row position, so file_index is an ordinary
* name there; over several files it hides the file position, with the note
sysuse auto, clear
gen int id = _n
gen long file_index = -_n
save `"`stem'_fi.dta"', replace
parqit save `"`stem'_fi.parquet"', data replace
log using `"`plog'"', text replace name(v142f)
parqit use using `"`stem'_fi.parquet"', name(c)
log close v142f
parqit gen long n = _n
parqit collect, clear
assert n == _n & id == _n
drop n
cf _all using `"`stem'_fi.dta"'
keep in 1/40
parqit save `"`stem'_fi_a.parquet"', data replace
use in 41/74 using `"`stem'_fi.dta"', clear
parqit save `"`stem'_fi_b.parquet"', data replace
log using `"`plog'"', text append name(v142g)
parqit use using `"`stem'_fi_*.parquet"', name(c)
log close v142g
parqit collect, clear
sort id
cf _all using `"`stem'_fi.dta"'
python:
from sfi import Macro
text = open(Macro.getLocal("plog"), encoding="utf-8").read().replace("\n> ", "")
assert text.count("hides the engine's physical row positions") == 1, text
assert 'column "file_index" hides' in text, text
end
parqit close _all

di "VERDICT(V142_TIED_SORT_ORDER): PASS - within a tie of the declared sort, _n, keep in, list in, collect, save and duplicates list Obs follow the file's row order as native Stata does; new sorts break ties in file order (= sort, stable), a dropped key keeps its ties, multi-file sources follow file then row; a source column named file_row_number (or file_index over several files) keeps its values and gets a note"
