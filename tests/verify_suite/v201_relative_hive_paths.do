* Local Parquet paths must expose Hive keys independently of cwd spelling.
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
global V201_fails 0
program define _v201_check
    args ok message
    if (`ok') di as txt "PASS `message'"
    else {
        di as err "FAIL `message'"
        global V201_fails = $V201_fails + 1
    }
end
program define _v201_read
    args path expected n k message
    foreach mode in eager lazy {
        if ("`mode'"=="eager") parqit use "`path'", clear
        else {
            parqit use using "`path'"
            parqit collect, clear
        }
        capture {
            assert _N==`n' & c(k)==`k'
            cf _all using "`expected'"
        }
        _v201_check `=_rc==0' "`mode': `message'"
        parqit close _all
    }
end
tempfile stem all_expected first_expected second_expected alias_expected physical_expected saved
local original_pwd `"`c(pwd)'"'
local dir `"`stem'_paths"'
python:
from pathlib import Path
import csv, hashlib, os
import pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
base = Path(Macro.getLocal('dir'))
base.mkdir()
root = base / 'tree'
odd = base / ('literal[1] space' if os.name == 'nt' else 'literal*?[1] space')
for parent in [root, odd]:
    for year, month, ids in [(2020, 1, [1, 2, 3]), (2021, 2, [4, 5, 6])]:
        path = parent / ('year=' + str(year)) / ('month=' + str(month)) / 'part-000.parquet'
        path.parent.mkdir(parents=True)
        pq.write_table(pa.table({'id': pa.array(ids, pa.int32()),
                                 'val': pa.array([n / 4 for n in ids], pa.float64())}), path)
rows = [(n, n / 4, 1 if n <= 3 else 2, 2020 if n <= 3 else 2021) for n in range(1, 7)]
with open(base / 'expected.csv', 'w', newline='') as f:
    writer = csv.writer(f)
    writer.writerow(['id', 'val', 'month', 'year'])
    writer.writerows(rows)
aliases = base / 'aliases'
(aliases / 'year=2030').mkdir(parents=True)
physical = base / 'physical'
(physical / 'inner').mkdir(parents=True)
pq.write_table(pa.table({'id': [90], 'val': [9.0]}), physical / 'parent.parquet')
pq.write_table(pa.table({'id': [99], 'val': [99.0]}), base / 'parent.parquet')
os.symlink(physical / 'inner', base / 'link', target_is_directory=True)
os.symlink(root / 'year=2020/month=1', aliases / 'year=2030/month=3', target_is_directory=True)
clash = base / 'clash/year=2020'
clash.mkdir(parents=True)
pq.write_table(pa.table({'YEAR': [999], 'id': [1]}), clash / 'part-000.parquet')
(base / 'control.csv').write_text('id,val\n1,0.25\n')
source_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in base.rglob('*.parquet')}
Macro.setLocal('root', str(root))
Macro.setLocal('odd', str(odd))
Macro.setLocal('aliases', str(aliases))
end
import delimited using "`dir'/expected.csv", clear varnames(1) asdouble
save "`all_expected'"
keep if id<=3
save "`first_expected'"
replace year=2030
replace month=3
save "`alias_expected'"
use "`all_expected'", clear
keep if id>=4
save "`second_expected'"
clear
set obs 1
gen double id=90
gen double val=9
save "`physical_expected'"

* The reported 0.8.1 failure: the file contains only id/val, keys live in cwd.
cd "`root'/year=2020/month=1"
parqit use "part-000.parquet", clear
capture confirm variable year month
if (_rc) {
    di as err "relative Parquet basename lost its Hive partition keys"
    di as txt "VERDICT(V201_RELATIVE_HIVE_PATHS): FAIL"
    exit 9
}
_v201_read "part-000.parquet" "`first_expected'" 3 4 "basename in partition"
_v201_read "./part-000.parquet" "`first_expected'" 3 4 "leading dot"
_v201_read "../month=1/part-000.parquet" "`first_expected'" 3 4 "leading parent"
_v201_read "*.parquet" "`first_expected'" 3 4 "relative glob in partition"
_v201_read "." "`first_expected'" 3 4 "relative partition directory"
_v201_read "`root'/year=2020/month=1/part-000.parquet" "`first_expected'" 3 4 "absolute leaf"
cd "`root'/year=2020"
_v201_read "month=1/part-000.parquet" "`first_expected'" 3 4 "relative child path"
_v201_read "month=1/*.parquet" "`first_expected'" 3 4 "relative child glob"
_v201_read "month=1" "`first_expected'" 3 4 "relative child directory"
cd "`root'"
_v201_read "." "`all_expected'" 6 4 "root directory"
_v201_read "**/*.parquet" "`all_expected'" 6 4 "recursive relative glob"
cd "`root'/year=2021/month=2"
_v201_read "part-000.parquet" "`second_expected'" 3 4 "different cwd and partition"

* Home paths retain DuckDB's native home_directory expansion, not cwd.
parqit set duckdb home_directory "`root'"
_v201_read "~/year=2020/month=1/part-000.parquet" "`first_expected'" 3 4 "native home-directory expansion"
parqit set duckdb reset home_directory
_v201_read "file:`root'/year=2020/month=1/part-000.parquet" "`first_expected'" 3 4 "native single-slash file URI"

* A relative source is fixed at lazy open, not re-resolved by collect/save.
cd "`root'/year=2020/month=1"
parqit use using "part-000.parquet"
cd "`root'/year=2021/month=2"
parqit collect, clear
capture cf _all using "`first_expected'"
_v201_check `=_rc==0' "lazy source remains fixed after cwd changes"
parqit summarize id
_v201_check `=r(mean)==2' "lazy analysis still reads original partition"
parqit save "`saved'.parquet"
python:
table = pq.read_table(Macro.getLocal('saved') + '.parquet')
ok = (table.num_rows == 3 and set(table.column_names) == {'id', 'val', 'month', 'year'}
      and table['id'].to_pylist() == [1, 2, 3]
      and table['val'].to_pylist() == [0.25, 0.5, 0.75]
      and table['year'].to_pylist() == [2020] * 3
      and table['month'].to_pylist() == [1] * 3)
Macro.setLocal('oracle', str(int(ok)))
end
_v201_check `oracle' "view-save matches independent PyArrow values after cwd change"
parqit close _all

* Known cwd bytes are literal, even when the caller supplies a live glob.
python: os.chdir(Path(Macro.getLocal('odd')) / 'year=2020/month=1')
_v201_read "*.parquet" "`first_expected'" 3 4 "literal wildcard bytes in cwd prefix"
_v201_read "." "`first_expected'" 3 4 "literal wildcard bytes in directory path"

* Symlink spelling carries Hive keys; link/.. must keep physical OS resolution.
cd "`aliases'"
_v201_read "year=2030/month=3/part-000.parquet" "`alias_expected'" 3 4 "logical Hive path through symlink"
cd "`dir'"
_v201_read "link/../parent.parquet" "`physical_expected'" 1 2 "physical symlink/parent traversal"

* filename() is absolute for local Parquet inputs; CSV retains its spelling.
cd "`root'/year=2020/month=1"
parqit use "part-000.parquet", clear filename(srcfile)
capture assert c(k)==5 & _N==3 & srcfile==`"`root'/year=2020/month=1/part-000.parquet"'
_v201_check `=_rc==0' "relative Parquet provenance is absolute"
cd "`dir'"
parqit use "control.csv", clear filename(srcfile)
capture assert _N==1 & c(k)==3 & srcfile=="control.csv" & id==1 & val==0.25
_v201_check `=_rc==0' "CSV provenance and payload retain existing behaviour"

* Exposing the keys must retain the existing refusal of case-clashing data.
clear
set obs 1
gen byte sentinel=7
cd "`dir'/clash/year=2020"
capture noisily parqit use "part-000.parquet", clear
local rc=_rc
capture assert `rc'==198 & _N==1 & c(k)==1 & sentinel[1]==7
_v201_check `=_rc==0' "relative Hive/file case clash refuses atomically"
python:
ok = all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == h for p, h in source_hashes.items())
Macro.setLocal('unchanged', str(int(ok)))
end
_v201_check `unchanged' "all source Parquet bytes remain unchanged"
cd `"`original_pwd'"'
if ($V201_fails) {
    di as txt "VERDICT(V201_RELATIVE_HIVE_PATHS): FAIL"
    exit 9
}
di as txt "VERDICT(V201_RELATIVE_HIVE_PATHS): PASS"
