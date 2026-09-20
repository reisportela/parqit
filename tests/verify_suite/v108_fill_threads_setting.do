* FILL-THREADS-SET-1 / ARROW-IN-WORKERS / CPUS-1 (2026-09-20): `parqit set
* fill_threads` chooses the fill-worker count for the session (auto, or a
* number up to the CPUs available to this process — a larger number is clamped
* and said, never refused), outranks the PARQIT_FILL_THREADS environment
* variable, is reported by `parqit version` r(fill_threads), refuses bad values
* loudly; `parqit set threads` follows the same CPU bound and the engine's
* default is those CPUs (not hardware_concurrency, which ignores an affinity
* mask); `parqit set stream_buffer_mb` caps the streaming buffer in-session
* and every cap loads byte-identical data; and — the part that matters —
* every worker count loads byte-identical data (datasignature) that matches a
* pyarrow oracle, on a tall file and on a wide-but-short file (the cell
* trigger), with the Arrow conversion now happening on the workers.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0

program define check, rclass
    args cond what
    capture assert `cond'
    if (_rc) di as err "  [FAIL] `what'"
    else di as txt "  [ok] `what'"
    return scalar bad = (_rc != 0)
end

capture program drop _v108_loghas
program define _v108_loghas, rclass
    version 16.0
    gettoken lf 0 : 0
    local pat = strtrim(`"`0'"')
    tempname fh
    local found 0
    file open `fh' using `"`lf'"', read text
    file read `fh' line
    while (r(eof)==0) {
        if (strpos(`"`line'"', `"`pat'"')) local found 1
        file read `fh' line
    }
    file close `fh'
    return scalar found = `found'
end

tempfile base
local tall `"`base'_tall.parquet"'
local wide `"`base'_wide.parquet"'
python:
import numpy as np, pyarrow as pa, pyarrow.parquet as pq
from sfi import Macro
rng = np.random.default_rng(108)
n = 120000
x = rng.normal(size=n)
x[::7] = np.nan
tbl = pa.table({
    "id": pa.array(np.arange(n, dtype=np.int32)),
    "x": pa.array(x),
    "s": pa.array([f"r{i}é" if i % 3 else "" for i in range(n)]),
    "d": pa.array(np.arange(n) % 20000, pa.int32()).cast(pa.date32()),
})
pq.write_table(tbl, Macro.getLocal("tall"), row_group_size=20000, compression="zstd")
Macro.setLocal("o_tall_n", str(n))
Macro.setLocal("o_tall_sumid", str(int(np.arange(n, dtype=np.int64).sum())))
Macro.setLocal("o_tall_nx", str(int(np.count_nonzero(~np.isnan(x)))))
Macro.setLocal("o_tall_sumx", repr(float(np.nansum(x))))
m = 90000
cols = {f"v{j}": pa.array(rng.integers(-1000, 1000, size=m).astype(np.int32)) for j in range(30)}
wt = pa.table(cols)
pq.write_table(wt, Macro.getLocal("wide"), row_group_size=30000, compression="zstd")
Macro.setLocal("o_wide_n", str(m))
Macro.setLocal("o_wide_sum", str(int(sum(int(np.asarray(c).sum()) for c in cols.values()))))
end

* --- setting surface: report, accept, refuse ---
parqit version
check `"r(fill_threads) == "auto""' "version reports auto by default (got `r(fill_threads)')"
local fails = `fails' + r(bad)
parqit set fill_threads 3
parqit version
check `"r(fill_threads) == "3""' "set 3 is reported"
local fails = `fails' + r(bad)
parqit set fill_threads 1
parqit version
check `"r(fill_threads) == "1""' "set 1 is reported"
local fails = `fails' + r(bad)
parqit set fill_threads auto
parqit version
check `"r(fill_threads) == "auto""' "set auto restores auto"
local fails = `fails' + r(bad)
parqit set fill_threads 0
parqit version
check `"r(fill_threads) == "auto""' "set 0 also means auto"
local fails = `fails' + r(bad)
foreach bad in abc -1 2.5 {
    capture parqit set fill_threads `bad'
    local rcb = _rc
    check "`rcb' == 198" "bad value `bad' refused with 198 (rc `rcb')"
    local fails = `fails' + r(bad)
}
capture parqit set fill_threads
local rcb = _rc
check "`rcb' == 198" "empty value refused with 198 (rc `rcb')"
local fails = `fails' + r(bad)
parqit version
check `"r(fill_threads) == "auto""' "refused values leave the setting untouched"
local fails = `fails' + r(bad)

* --- CPUS-1: bounded by the CPUs available to this process, never refused ---
parqit version
local cpus = r(cpus)
local eng = r(threads)
check "`cpus' >= 1 & `eng' == `cpus'" "version reports r(cpus)=`cpus' and the engine threads default to them (r(threads)=`eng')"
local fails = `fails' + r(bad)
local over = `cpus' + 1
log using `"`base'_clamp.log"', replace text name(v108c)
parqit set fill_threads `over'
parqit set threads `over'
log close v108c
parqit version
local ft_got "`r(fill_threads)'"
local th_got = r(threads)
check `""`ft_got'" == "`cpus'""' "fill_threads `over' is clamped to `cpus' (r(fill_threads)=`ft_got')"
local fails = `fails' + r(bad)
check "`th_got' == `cpus'" "threads `over' is clamped to `cpus' (r(threads)=`th_got')"
local fails = `fails' + r(bad)
_v108_loghas `"`base'_clamp.log"' fill_threads `over' exceeds the `cpus' CPUs available
check "r(found) == 1" "... the fill_threads clamp is said"
local fails = `fails' + r(bad)
_v108_loghas `"`base'_clamp.log"' threads `over' exceeds the `cpus' CPUs available
check "r(found) == 1" "... and the threads clamp is said"
local fails = `fails' + r(bad)
parqit sql "SELECT current_setting('threads') AS t"
parqit collect, clear
capture quietly parqit close _all
check "t[1] == `cpus'" "... and the engine runs with `cpus' threads (current_setting)"
local fails = `fails' + r(bad)
parqit set fill_threads `cpus'
parqit version
check `"r(fill_threads) == "`cpus'""' "fill_threads `cpus' (every available CPU) is accepted as is"
local fails = `fails' + r(bad)
parqit set fill_threads auto
* the affinity path: a child Stata under taskset must see only those CPUs.
* The two CPU ids come from THIS process's own mask (a cluster allocation
* need not include CPUs 0-1, and Stata's shell cannot report taskset's exit
* status); the executable is found by probing the flavours under
* c(sysdir_stata) (c(flavor) does not name the binary on every installation).
local aff ""
capture python:
import os
from sfi import Macro
try:
    ids = sorted(os.sched_getaffinity(0))
    Macro.setLocal("aff", ",".join(str(i) for i in ids[:2]) if len(ids) >= 2 else "")
except Exception:
    Macro.setLocal("aff", "")
end
if ("`c(os)'" == "Unix" & "`aff'" != "") {
    local exe ""
    foreach cand in stata-mp stata-se stata {
        capture confirm file `"`c(sysdir_stata)'`cand'"'
        if (_rc == 0 & `"`exe'"' == "") local exe `"`c(sysdir_stata)'`cand'"'
    }
    if (`"`exe'"' != "") {
        tempname fh
        file open `fh' using `"`base'_child.do"', write text replace
        file write `fh' `"adopath ++ "`repo'/src/ado/p""' _n
        file write `fh' `"global PARQIT_PLUGIN_PATH "`plugin'""' _n
        file write `fh' "parqit version" _n
        file write `fh' `"file open out using "`base'_child.txt", write text replace"' _n
        file write `fh' `"file write out (string(r(cpus)) + " " + string(r(threads)))"' _n
        file write `fh' "file close out" _n
        file close `fh'
        shell cd "`c(tmpdir)'" && taskset -c `aff' "`exe'" -b do "`base'_child.do"
        tempname fr
        local got ""
        capture {
            file open `fr' using `"`base'_child.txt"', read text
            file read `fr' got
            file close `fr'
        }
        check `""`got'" == "2 2""' "a child Stata under taskset -c `aff' sees 2 CPUs and 2 engine threads (got '`got'')"
        local fails = `fails' + r(bad)
        capture erase `"`base'_child.do"'
        capture erase `"`base'_child.txt"'
        capture erase `"`base'_child.log"'
    }
    else di as txt "  [skip] no Stata executable found under c(sysdir_stata); affinity case not run"
}
else di as txt "  [skip] not Unix, or fewer than 2 CPUs in this process's mask; affinity case not run"

* --- data identity across worker counts, against the oracle ---
program define load_sig, rclass
    args file
    parqit use `"`file'"', clear
    quietly datasignature
    return local sig "`r(datasignature)'"
    return scalar N = _N
end
foreach f in tall wide {
    local sigs
    foreach t in 1 2 4 8 {
        parqit set fill_threads `t'
        load_sig `"``f''"'
        local sig_`t' "`r(sig)'"
        local n_`t' = r(N)
        local sigs "`sigs' `sig_`t''"
    }
    check `"("`sig_1'" == "`sig_2'") & ("`sig_2'" == "`sig_4'") & ("`sig_4'" == "`sig_8'") & ("`sig_1'" != "")"' "`f': datasignature identical for 1/2/4/8 workers (`sig_1')"
    local fails = `fails' + r(bad)
    check "`n_1' == `o_`f'_n' & `n_8' == `o_`f'_n'" "`f': N matches the oracle"
    local fails = `fails' + r(bad)
    if ("`f'" == "tall") local sig_tall_ref "`sig_1'"
    if ("`f'" == "wide") local sig_wide_ref "`sig_1'"
}
* the tall file's values against the oracle (last load was 8 workers)
parqit set fill_threads 8
parqit use `"`tall'"', clear
quietly summarize id
check "r(sum) == `o_tall_sumid' & r(N) == `o_tall_n'" "tall: sum(id) exact"
local fails = `fails' + r(bad)
quietly summarize x
check "r(N) == `o_tall_nx' & reldif(r(sum), `o_tall_sumx') < 1e-12" "tall: NULL count and sum(x) match pyarrow"
local fails = `fails' + r(bad)
* date32 day 1 is 1970-01-02, i.e. Stata %td 3654 (1960-01-01 + 3653 days = 1970-01-01)
check `"s[2] == "r1é" & s[1] == "" & d[2] == 3654 & d[1] == 3653"' "tall: string, empty string and date cells exact"
local fails = `fails' + r(bad)
parqit use `"`wide'"', clear
local tot = 0
foreach v of varlist v0-v29 {
    quietly summarize `v'
    local tot = `tot' + r(sum)
}
check "`tot' == `o_wide_sum' & _N == `o_wide_n' & c(k) == 30" "wide: 30 x 90k cells summed exactly (cell-triggered parallel path)"
local fails = `fails' + r(bad)

* --- precedence over the environment variable ---
python:
import os
os.environ["PARQIT_FILL_THREADS"] = "1"
end
parqit set fill_threads 4
parqit version
check `"r(fill_threads) == "4""' "session setting outranks PARQIT_FILL_THREADS"
local fails = `fails' + r(bad)
load_sig `"`tall'"'
local sig_p "`r(sig)'"
check `"("`sig_p'" == "`sig_tall_ref'") & ("`sig_p'" != "")"' "load under the precedence case is identical to the tall file's signature"
local fails = `fails' + r(bad)
python:
import os
os.environ.pop("PARQIT_FILL_THREADS", None)
end
parqit set fill_threads auto
* CPUS-1: the variable is clamped to the available CPUs too, said once
python:
import os
from sfi import Macro
os.environ["PARQIT_FILL_THREADS"] = str(int(Macro.getLocal("cpus")) + 1)
end
log using `"`base'_env.log"', replace text name(v108e)
load_sig `"`tall'"'
local sig_env "`r(sig)'"
log close v108e
python:
import os
os.environ.pop("PARQIT_FILL_THREADS", None)
end
_v108_loghas `"`base'_env.log"' PARQIT_FILL_THREADS=`over' exceeds the `cpus' CPUs available
check "r(found) == 1" "PARQIT_FILL_THREADS=`over' is clamped to `cpus' and said"
local fails = `fails' + r(bad)
check `"("`sig_env'" == "`sig_tall_ref'") & ("`sig_env'" != "")"' "... and that load is byte-identical"
local fails = `fails' + r(bad)

* --- STREAM-BUFFER-SET-1: the in-session cap on the streaming buffer ---
parqit version
check `"r(stream_buffer_mb) == "auto""' "stream_buffer_mb reports auto by default"
local fails = `fails' + r(bad)
foreach v in 8 0 {
    parqit set stream_buffer_mb `v'
    parqit version
    check `"r(stream_buffer_mb) == "`v'""' "set stream_buffer_mb `v' is reported"
    local fails = `fails' + r(bad)
    foreach f in tall wide {
        load_sig `"``f''"'
        check `"("`r(sig)'" == "`sig_`f'_ref'") & ("`r(sig)'" != "")"' "stream_buffer_mb `v': `f' loads byte-identical (`r(sig)')"
        local fails = `fails' + r(bad)
    }
}
foreach bad in abc -1 2.5 {
    capture parqit set stream_buffer_mb `bad'
    local rcb = _rc
    check "`rcb' == 198" "stream_buffer_mb bad value `bad' refused with 198 (rc `rcb')"
    local fails = `fails' + r(bad)
}
parqit set stream_buffer_mb auto
parqit version
check `"r(stream_buffer_mb) == "auto""' "stream_buffer_mb auto restores auto"
local fails = `fails' + r(bad)

if (`fails' == 0) di "VERDICT(V108_FILL_THREADS_SETTING): PASS - fill_threads/threads bounded by the available CPUs (clamped and said, affinity honoured), stream_buffer_mb caps load byte-identical, 1/2/4/8 workers match pyarrow on tall and wide files"
else di "VERDICT(V108_FILL_THREADS_SETTING): FAIL - `fails' check(s)"
