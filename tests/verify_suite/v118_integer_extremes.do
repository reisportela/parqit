* Signed/unsigned 128-bit extremes must respect every int64 mode.
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
global V118_fails 0
program define _v118_check
    args ok message
    if (`ok') di as txt "PASS `message'"
    else {
        di as err "FAIL `message'"
        global V118_fails = $V118_fails + 1
    }
end
program define _v118_sentinel
    clear
    set obs 2
    gen byte sentinel = _n
end
local signed_sql "SELECT x, i FROM (VALUES(CAST('-170141183460469231731687303715884105728' AS HUGEINT),1),(-9007199254740993::HUGEINT,2),(-9007199254740992::HUGEINT,3),(-9007199254740991::HUGEINT,4),(0::HUGEINT,5),(9007199254740991::HUGEINT,6),(9007199254740992::HUGEINT,7),(9007199254740993::HUGEINT,8),(CAST('170141183460469231731687303715884105727' AS HUGEINT),9),(NULL::HUGEINT,10))t(x,i) ORDER BY i"
local unsigned_sql "SELECT x, i FROM (VALUES(0::UHUGEINT,1),(9007199254740991::UHUGEINT,2),(9007199254740992::UHUGEINT,3),(9007199254740993::UHUGEINT,4),(CAST('170141183460469231731687303715884105728' AS UHUGEINT),5),(CAST('340282366920938463463374607431768211455' AS UHUGEINT),6),(NULL::UHUGEINT,7))t(x,i) ORDER BY i"
foreach kind in signed unsigned {
    _v118_sentinel
    parqit sql "``kind'_sql'"
    capture noisily parqit collect, clear int64(refuse)
    local rc = _rc
    capture assert `rc' == 198 & _N == 2 & sentinel[1] == 1 & sentinel[2] == 2
    _v118_check `=_rc == 0' "`kind' protective refusal and atomicity"
    parqit close _all
}
parqit sql "`signed_sql'"
capture noisily parqit collect, clear int64(string)
local rc = _rc
python:
from sfi import Data, Macro
want = [-(2**127),-(2**53+1),-(2**53),-(2**53-1),0,2**53-1,2**53,2**53+1,2**127-1,None]
expected = ['' if v is None else str(v) for v in want]
got = None
try:
    got = Data.get('x')
    ok = got == expected
except Exception as exc:
    print(f'ORACLE EXCEPTION {type(exc).__name__}: {exc}')
    ok = False
if not ok:
    print(f'ORACLE expected={expected!r}; actual={got!r}')
Macro.setLocal('oracle',str(int(ok)))
end
_v118_check `=`rc' == 0 & `oracle'' "HUGEINT exact strings and null"
parqit close _all
parqit sql "`unsigned_sql'"
capture noisily parqit collect, clear int64(string)
local rc = _rc
python:
from sfi import Data, Macro
want = [0,2**53-1,2**53,2**53+1,2**127,2**128-1,None]
expected = ['' if v is None else str(v) for v in want]
got = None
try:
    got = Data.get('x')
    ok = got == expected
except Exception as exc:
    print(f'ORACLE EXCEPTION {type(exc).__name__}: {exc}')
    ok = False
if not ok:
    print(f'ORACLE expected={expected!r}; actual={got!r}')
Macro.setLocal('oracle',str(int(ok)))
end
_v118_check `=`rc' == 0 & `oracle'' "UHUGEINT exact strings and null"
parqit close _all
parqit sql "`signed_sql'"
capture noisily parqit collect, clear int64(round)
local rc = _rc
python:
from sfi import Data, Macro, Missing
want = [-(2**127),-(2**53+1),-(2**53),-(2**53-1),0,2**53-1,2**53,2**53+1,2**127-1,None]
expected = [None if v is None else float(v) for v in want]
got = None
try:
    got = Data.get('x')
    ok = len(got) == len(expected) and all(Missing.isMissing(g) if w is None else g == w for g,w in zip(got,expected))
except Exception as exc:
    print(f'ORACLE EXCEPTION {type(exc).__name__}: {exc}')
    ok = False
if not ok:
    print(f'ORACLE expected={expected!r}; actual={got!r}')
Macro.setLocal('oracle',str(int(ok)))
end
_v118_check `=`rc' == 0 & `oracle'' "HUGEINT rounded values match Python binary64"
parqit close _all
parqit sql "`unsigned_sql'"
capture noisily parqit collect, clear int64(round)
local rc = _rc
python:
from sfi import Data, Macro, Missing
want = [0,2**53-1,2**53,2**53+1,2**127,2**128-1,None]
expected = [None if v is None else float(v) for v in want]
got = None
try:
    got = Data.get('x')
    ok = len(got) == len(expected) and all(Missing.isMissing(g) if w is None else g == w for g,w in zip(got,expected))
except Exception as exc:
    print(f'ORACLE EXCEPTION {type(exc).__name__}: {exc}')
    ok = False
if not ok:
    print(f'ORACLE expected={expected!r}; actual={got!r}')
Macro.setLocal('oracle',str(int(ok)))
end
_v118_check `=`rc' == 0 & `oracle'' "UHUGEINT rounded values match Python binary64"
parqit close _all
foreach kind in HUGEINT UHUGEINT {
    foreach mode in refuse round string {
        parqit sql "SELECT * FROM (VALUES(0::`kind'),(9007199254740991::`kind'),(9007199254740992::`kind'),(NULL::`kind'))t(x)"
        capture noisily parqit collect, clear int64(`mode')
        local rc = _rc
        capture assert `rc' == 0 & _N == 4 & x[1] == 0 & x[2] == 9007199254740991 & x[3] == 9007199254740992 & missing(x[4])
        _v118_check `=_rc == 0' "`kind' `mode' exact boundary stays numeric"
        parqit close _all
        parqit sql "SELECT NULL::`kind' AS x"
        capture noisily parqit collect, clear int64(`mode')
        local rc = _rc
        capture assert `rc' == 0 & _N == 1 & missing(x[1])
        _v118_check `=_rc == 0' "`kind' `mode' all null"
        parqit close _all
        parqit sql "SELECT NULL::`kind' AS x WHERE false"
        capture noisily parqit collect, clear int64(`mode')
        local rc = _rc
        capture assert `rc' == 0 & _N == 0 & c(k) == 1
        _v118_check `=_rc == 0' "`kind' `mode' empty"
        parqit close _all
    }
}
* A mixed signed/unsigned comparison must not round the threshold + 1.
foreach mode in refuse string round {
    _v118_sentinel
    parqit sql "SELECT CAST('9007199254740993' AS UHUGEINT) AS x"
    capture noisily parqit collect, clear int64(`mode')
    local rc = _rc
    if ("`mode'" == "refuse") {
        capture assert `rc' == 198 & _N == 2 & sentinel[1] == 1
    }
    else if ("`mode'" == "string") {
        capture assert `rc' == 0 & _N == 1 & x[1] == "9007199254740993"
    }
    else {
        capture assert `rc' == 0 & _N == 1 & x[1] == 9007199254740992
    }
    _v118_check `=_rc == 0' "UHUGEINT singleton threshold + 1, `mode'"
    parqit close _all
}
* Preserve the existing DECIMAL-to-HUGEINT trigger: ties round away from zero.
foreach value in 9007199254740992.4 -9007199254740992.4 {
    foreach mode in refuse string round {
        parqit sql "SELECT CAST('`value'' AS DECIMAL(38,1)) AS x"
        capture noisily parqit collect, clear int64(`mode')
        local rc = _rc
        capture assert `rc' == 0 & _N == 1 & x[1] == real("`value'")
        _v118_check `=_rc == 0' "DECIMAL `value' below trigger, `mode'"
        parqit close _all
    }
}
foreach value in 9007199254740992.5 -9007199254740992.5 {
    foreach mode in refuse string round {
        _v118_sentinel
        parqit sql "SELECT CAST('`value'' AS DECIMAL(38,1)) AS x"
        capture noisily parqit collect, clear int64(`mode')
        local rc = _rc
        if ("`mode'" == "refuse") {
            capture assert `rc' == 198 & _N == 2 & sentinel[1] == 1
        }
        else if ("`mode'" == "string") {
            capture assert `rc' == 0 & _N == 1 & x[1] == "`value'"
        }
        else {
            capture assert `rc' == 0 & _N == 1 & x[1] == real("`value'")
        }
        _v118_check `=_rc == 0' "DECIMAL `value' at trigger, `mode'"
        parqit close _all
    }
}
if ($V118_fails) di as err "VERDICT(V118_INTEGER_EXTREMES): FAIL - $V118_fails checks"
else di as txt "VERDICT(V118_INTEGER_EXTREMES): PASS"
macro drop V118_fails
