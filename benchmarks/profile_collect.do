* Profile the sub-components of parqit collect
clear all
set more off
set varabbrev off

local repo : env PARQIT_REPO
if (`"`repo'"' == "") local repo `"`c(pwd)'"'
local plugin `"`repo'/build/dev/parqit.plugin"'
local parquet : env PARQIT_BENCH_REF
if (`"`parquet'"' == "") local parquet "parqit_benchmark_ref.parquet"

adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

* Force compilation of Mata functions in parqit.ado by running it directly
run `"`repo'/src/ado/p/parqit.ado"'

* Redefine the core loader with timers
program define profile_load_core
    syntax, resp(string) strl(string) tag(string) n(integer) [names(string)]

    tempname stage
    local curframe = c(frame)

    timer clear 11
    timer clear 12
    timer clear 13
    timer clear 14
    timer clear 15
    timer clear 16
    timer clear 17

    timer on 11
    frame create `stage'
    timer off 11

    frame `stage' {
        timer on 12
        mata: _parqit_resp_create(`"`resp'"', `n')
        timer off 12

        if (`n' > 0) {
            timer on 13
            plugin call parqit_plugin `names' in 1/`n', use_fetch `tag'
            timer off 13
        }

        timer on 14
        mata: _parqit_apply_strl(`"`strl'"')
        timer off 14

        timer on 15
        mata: _parqit_resp_decorate(`"`resp'"')
        timer off 15

        if (`"`parqit_dtalabel'"' != "") {
            mata: st_local("dl", _parqit_unhex(st_local("parqit_dtalabel")))
            label data `"`dl'"'
        }
    }

    timer on 16
    frame copy `stage' `curframe', replace
    timer off 16

    timer on 17
    frame drop `stage'
    timer off 17

    quietly timer list
    di as txt "Timers for load_core:"
    di as txt "  11 (frame create):    " as res %8.3f r(t11) " sec"
    di as txt "  12 (resp_create):     " as res %8.3f r(t12) " sec (allocating variables)"
    di as txt "  13 (plugin fetch):    " as res %8.3f r(t13) " sec (C++ reading & filling)"
    di as txt "  14 (apply_strl):      " as res %8.3f r(t14) " sec (strL sidecar)"
    di as txt "  15 (resp_decorate):   " as res %8.3f r(t15) " sec (labels, formats)"
    di as txt "  16 (frame copy):      " as res %8.3f r(t16) " sec (atomic swap)"
    di as txt "  17 (frame drop):      " as res %8.3f r(t17) " sec"
end

* Run the benchmark
di as txt "=== profiling parqit use ==="
timer clear 1
timer on 1
parqit use using `"`parquet'"'
timer off 1
quietly timer list 1
di as txt "parqit use took " as res %8.3f r(t1) " sec"

di as txt "=== profiling parqit collect ==="
timer clear 2
timer on 2

* Manually recreate parqit collect but using our profile_load_core
tempfile req resp strl
local _sq_limit -1
mata: _parqit_write_collect_request("`req'", "`resp'", "`strl'")

timer clear 3
timer on 3
capture noisily plugin call parqit_plugin, view_collect_prepare `reqhex'
timer off 3
quietly timer list 3
di as txt "view_collect_prepare took " as res %8.3f r(t3) " sec"

if (_rc == 0) {
    profile_load_core, resp(`"`resp'"') strl(`"`strl'"') tag("`parqit_tag'") ///
        n(`parqit_n') names("`parqit_names'")
}

timer off 2
quietly timer list 2
di as txt "Total collect time (excluding preparation): " as res %8.3f (r(t2) - r(t3)) " sec"

capture parqit close _all
clear
