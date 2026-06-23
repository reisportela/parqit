* M0 smoke: the parqit plugin loads in this Stata, reports versions, and its
* embedded engine passes the in-process selftest (DuckDB opens, writes and
* reads Parquet with parqit KV metadata). Also locks the Mata hex codec to the
* canonical vectors shared with tests/unit/test_hexcodec.cpp.
*
* Usage:  stata-mp -b do m0_smoke.do <repo_root> <path_to_parqit.plugin>
clear all
set more off
args repo plugin

capture noisily {
    assert `"`repo'"' != "" & `"`plugin'"' != ""
    adopath ++ `"`repo'/src/ado/p"'
    global PARQIT_PLUGIN_PATH `"`plugin'"'

    parqit version
    assert `"`r(parqit_version)'"' != ""
    assert `"`r(duckdb_version)'"' != ""
    local dver `"`r(duckdb_version)'"'

    * selftest covers: Mata hex codec vectors, ado↔plugin codec agreement
    * (plugin echo round-trip), and the embedded engine end-to-end
    parqit selftest
    assert `"`r(selftest)'"' == "ok"

    * unknown subcommand must be loud (rc 198), not silent
    capture parqit frobnicate
    assert _rc == 198

    * stubs announce themselves rather than half-working
    capture parqit collapse (mean) x, by(g)
    assert _rc == 198
}
local rc = _rc

if (`rc' == 0) {
    di "VERDICT(M0_SMOKE): PASS - plugin loaded, DuckDB `dver' selftest ok, codec locked"
}
else {
    di "VERDICT(M0_SMOKE): FAIL - rc=`rc' (see log above)"
    exit `rc'
}
