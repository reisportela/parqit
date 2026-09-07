* V88: refuse ambiguous cross-source aliases before any plan/metadata mutation.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* An exposed name stays occupied when its case sibling is projected away.
parqit sql "SELECT 1 AS a, 2 AS A"
parqit drop a
capture noisily parqit gen A = 99
assert _rc == 198
parqit collect, clear
assert _N == 1 & A == 2
confirm new variable a

parqit sql "SELECT 2 AS id, 22 AS A", name(u)
parqit sql "SELECT 1 AS id, 11 AS a", name(m)
capture noisily parqit append using view:u
assert _rc == 198
parqit collect, clear
assert _N == 1 & id == 1 & a == 11

* An alias A_1 for A must never match an unrelated original A_1.
parqit sql "SELECT 1 AS id, 44 AS A_1", name(u)
foreach cmd in "append using view:u" "merge 1:1 id using view:u" "joinby id using view:u" {
    parqit sql "SELECT 1 AS id, 11 AS a, 33 AS A", name(m)
    capture noisily parqit `cmd'
    assert _rc == 198
    parqit collect, clear
    assert _N == 1 & id == 1 & a == 11 & A == 33
    confirm new variable A_1
}
capture noisily parqit merge 1:1 A_1 using view:u
assert _rc == 111

* A column excluded by keepusing() must not prevent a valid merge.
parqit merge 1:1 id using view:u, keepusing(id)
parqit collect, clear
assert _N == 1 & a == 11 & A == 33 & _merge == 3

* Two using sources must also be checked against one another.
parqit sql "SELECT 3 AS id, 33 AS A", name(v)
parqit sql "SELECT 2 AS id, 22 AS a", name(u)
parqit sql "SELECT 1 AS id", name(m)
capture noisily parqit append using view:u view:v
assert _rc == 198
parqit collect, clear
assert _N == 1 & id == 1
confirm new variable a

* A using view with a compatible superset already has a safe alias for A.
parqit sql "SELECT 2 AS id, 22 AS a, 44 AS A", name(u)
parqit sql "SELECT 1 AS id, 11 AS a", name(m)
parqit append using view:u
parqit collect, clear
assert _N == 2 & a == 11*id
assert missing(A) if id == 1
assert A == 44 if id == 2

* File inputs are aligned before combination; preserve that existing route.
tempfile casefile aliasfile
python:
from sfi import Macro
import pyarrow as pa, pyarrow.parquet as pq
pq.write_table(pa.table({'id':[1], 'A':[22]}), Macro.getLocal('casefile'))
pq.write_table(pa.table({'id':[1], 'A_1':[44]}), Macro.getLocal('aliasfile'))
end
parqit sql "SELECT 1 AS id, 11 AS a", name(m)
parqit append using `casefile'
parqit collect, clear
assert _N == 2 & a[1] == 11 & missing(a[2])
assert missing(A[1]) & A[2] == 22
foreach cmd in "merge 1:1 id" "joinby id" {
    parqit sql "SELECT 1 AS id, 11 AS a", name(m)
    parqit `cmd' using `casefile'
    parqit collect, clear
    assert _N == 1 & a == 11 & A == 22
    parqit sql "SELECT 1 AS id, 11 AS a, 33 AS A", name(m)
    parqit `cmd' using `aliasfile'
    parqit collect, clear
    assert _N == 1 & a == 11 & A == 33 & A_1 == 44
}

* Already aligned case-distinct manifests remain supported, on both paths.
parqit sql "SELECT 2 AS id, 22 AS a, 44 AS A", name(u)
parqit sql "SELECT 1 AS id, 11 AS a, 33 AS A", name(m)
parqit append using view:u
parqit sort id
tempfile out
parqit save `out'
parqit collect, clear
assert _N == 2 & a == 11*id & A == 22+11*id
python:
from sfi import Macro
import pyarrow.parquet as pq
assert pq.read_table(Macro.getLocal('out')).to_pydict() == {
    'id': [1, 2], 'a': [11, 22], 'A': [33, 44]}
end
parqit close _all
di "VERDICT(V88_TWO_TABLE_NAME_IDENTITY): PASS"
