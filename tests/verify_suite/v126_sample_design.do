* V126 — SAMPLE-DESIGN-1: parqit sample # [if] [, by() cluster() any all
* generate()|keep()], the philosophy of sample2 (STB-37 dm46). Oracles: native
* sample and sample2 on the same data (frames, per-stratum counts, errors) and
* an independent Python implementation of the documented cluster key.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem

* sample2 is the oracle for frames and counts; without it those checks are skipped
capture which sample2
local have_sample2 = (_rc == 0)
if (!`have_sample2') di as txt "note: sample2 not installed; its comparisons are skipped"

program define _v126_grep, rclass
    version 16.0
    args logfile needle
    tempname fh
    local found 0
    local cur ""
    file open `fh' using `"`logfile'"', read text
    file read `fh' line
    while (!r(eof)) {
        if (substr(`"`macval(line)'"', 1, 2) == "> ") {
            local cur `"`macval(cur)'`=substr(`"`macval(line)'"', 3, .)'"'
        }
        else {
            if (strpos(`"`macval(cur)'"', `"`needle'"')) local found 1
            local cur `"`macval(line)'"'
        }
        file read `fh' line
    }
    if (strpos(`"`macval(cur)'"', `"`needle'"')) local found 1
    file close `fh'
    return scalar found = `found'
end

* whole clusters: every non-missing hh in memory has all of its 3 rows
program define _v126_whole
    version 16.0
    args cvar
    tempvar n
    quietly bysort `cvar': gen long `n' = _N
    capture assert `n' == 3 if !missing(`cvar')
    local ok = !_rc
    drop `n'
    assert `ok'
end

* 200 households of 3 people, region constant within hh; the first household
* has no id (hh missing, hhs empty), so its rows are outside every frame
clear
set seed 126
set obs 600
gen long hh = ceil(_n / 3)
gen byte region = 1 + mod(hh, 4)
gen int age = 20 + floor(runiform() * 60)
gen double inc = round(runiform() * 1000, .01)
gen str8 hhs = "h" + string(hh)
replace hh = . in 1/3
replace hhs = "" in 1/3
replace region = 9 in 1/3
save `"`stem'_native.dta"'
parqit save `"`stem'_src.parquet"', data

* households per region and the 25 percent quota (a whole-number percentage, so
* parqit's exact rule and sample2's int(n*#/100+.5) agree)
keep if !missing(hh)
bysort hh: keep if _n == 1
contract region, freq(n)
gen long k = int(n * 25 / 100 + .5)
quietly summarize k
local total_k = r(sum)
save `"`stem'_hquota.dta"'

program define _v126_quota
    version 16.0
    args quota
    preserve
    keep if !missing(hh)
    bysort hh: keep if _n == 1
    contract region, freq(got)
    merge 1:1 region using `"`quota'"', nogenerate
    replace got = 0 if missing(got)
    assert got == k
    restore
end

* ---- 1. whole clusters within strata; missing clusters kept, with a note ----
tempfile lg
quietly log using `"`lg'"', text name(v126) replace
parqit use using `"`stem'_src.parquet"', name(S)
parqit sample 25, cluster(hh) by(region) seed(7)
quietly log close v126
_v126_grep `"`lg'"' "note: 3 observation(s) with a missing hh are outside the sampling frame and kept"
assert r(found)
parqit collect, clear
_v126_whole hh
assert _N == 3 * `total_k' + 3
assert missing(hh) if region == 9
count if missing(hh)
assert r(N) == 3
_v126_quota `"`stem'_hquota.dta"'
keep hh region
drop if missing(hh)
bysort hh: keep if _n == 1
save `"`stem'_drawn_hh.dta"'
parqit save `"`stem'_s1.parquet"'
if (`have_sample2') {
    use `"`stem'_native.dta"', clear
    sample2 25 if !missing(hh), cluster(hh) by(region)
    _v126_quota `"`stem'_hquota.dta"'
    count if missing(hh)
    assert r(N) == 3
}

* ---- 2. an independent Python implementation of the key picks the same hh ---
python:
import struct
from fractions import Fraction
import math
import pyarrow.parquet as pq
from sfi import Macro
M = (1 << 64) - 1
def mix(z):
    z = (z + 0x9e3779b97f4a7c15) & M
    z = ((z ^ (z >> 30)) * 0xbf58476d1ce4e5b9) & M
    z = ((z ^ (z >> 27)) * 0x94d049bb133111eb) & M
    return z ^ (z >> 31)
def key_number(seed, x):
    x = 0.0 if x == 0 else float(x)
    return mix(struct.unpack('<Q', struct.pack('<d', x))[0] ^ mix(seed))
def key_text(seed, s):
    h = 0xcbf29ce484222325
    for c in s.encode('utf-8'):
        h = ((h ^ c) * 0x100000001b3) & M
    return mix(h ^ mix(seed ^ 0x5bd1e9955bd1e995))
def quota(n, pct):
    q = Fraction(n) * Fraction(pct) / 100
    f = math.floor(q)
    return f + (1 if (q - f) * 2 >= 1 else 0)
def expected(path, cvar, strata, pct, seed, text):
    t = pq.read_table(path).to_pydict()
    units = {}
    for i, c in enumerate(t[cvar]):
        if c is None or c == '':
            continue
        units[c] = tuple(t[s][i] for s in strata)
    groups = {}
    for c, s in units.items():
        groups.setdefault(s, []).append(c)
    drawn = set()
    for s, cs in groups.items():
        key = (lambda c: key_text(seed, c)) if text else (lambda c: key_number(seed, c))
        cs.sort(key=lambda c: (key(c), c))
        drawn.update(cs[:quota(len(cs), pct)])
    return drawn
stem = Macro.getLocal('stem')
got = {c for c in pq.read_table(stem + '_s1.parquet').to_pydict()['hh'] if c is not None}
want = expected(stem + '_src.parquet', 'hh', ['region'], 25.0, 7, False)
assert got == want, (sorted(got ^ want))[:10]
end

* ---- 3. the draw is invariant to row order, file splits and threads ---------
use `"`stem'_native.dta"', clear
gen double shuffle = runiform()
sort shuffle
drop shuffle
parqit save `"`stem'_shuffled.parquet"', data
preserve
keep in 1/250
parqit save `"`stem'_part_a.parquet"', data
restore
keep in 251/l
parqit save `"`stem'_part_b.parquet"', data
foreach src in "shuffled" "part_*" {
    parqit use using `"`stem'_`src'.parquet"', name(I)
    parqit sample 25, cluster(hh) by(region) seed(7)
    parqit collect, clear
    keep hh region
    drop if missing(hh)
    bysort hh: keep if _n == 1
    cf _all using `"`stem'_drawn_hh.dta"'
}
parqit set threads 1
parqit use using `"`stem'_src.parquet"', name(I)
parqit sample 25, cluster(hh) by(region) seed(7)
parqit collect, clear
keep hh region
drop if missing(hh)
bysort hh: keep if _n == 1
cf _all using `"`stem'_drawn_hh.dta"'
parqit set threads 4

* ---- 4. a text cluster: the empty string is missing, keys hash the bytes ----
parqit use using `"`stem'_src.parquet"', name(T)
parqit sample 40, cluster(hhs) seed(11)
parqit save `"`stem'_s4.parquet"'
parqit collect, clear
_v126_whole hhs
count if hhs == ""
assert r(N) == 3
python:
got = {c for c in pq.read_table(stem + '_s4.parquet').to_pydict()['hhs'] if c}
want = expected(stem + '_src.parquet', 'hhs', [], 40.0, 11, True)
assert got == want, sorted(got ^ want)[:10]
end

* ---- 5. if frames: strict refuses split clusters; any and all resolve them --
use `"`stem'_native.dta"', clear
bysort hh: egen byte old_any = max(age > 60)
bysort hh: egen byte old_all = min(age > 60)
keep if !missing(hh)
bysort hh: keep if _n == 1
count if old_any
local n_any = r(N)
count if old_all
local n_all = r(N)
save `"`stem'_frames.dta"'

parqit use using `"`stem'_src.parquet"', name(F)
quietly parqit describe
local steps = r(n_steps)
capture noisily parqit sample 50 if age > 60, cluster(hh)
assert _rc == 198
quietly parqit describe
assert r(n_steps) == `steps'
foreach rule in any all {
    parqit use using `"`stem'_src.parquet"', name(F)
    parqit sample 50 if age > 60, cluster(hh) `rule' seed(3)
    parqit collect, clear
    _v126_whole hh
    merge m:1 hh using `"`stem'_frames.dta"', keepusing(old_`rule') keep(master match) nogenerate
    * every household outside the frame stays whole; half of the frame is drawn
    quietly levelsof hh if !old_`rule' & !missing(hh)
    assert `: word count `r(levels)'' == 200 - 1 - `n_`rule''
    quietly levelsof hh if old_`rule' == 1
    local k = `: word count `r(levels)''
    assert `k' == int(`n_`rule'' * 50 / 100 + .5)
    * a frame cluster that is not drawn loses every row, even rows outside the if
    count if old_`rule' == 1
    assert r(N) == 3 * `k'
    if (`have_sample2') {
        use `"`stem'_native.dta"', clear
        sample2 50 if age > 60 & !missing(hh), cluster(hh) `rule'
        merge m:1 hh using `"`stem'_frames.dta"', keepusing(old_`rule') keep(master match) nogenerate
        quietly levelsof hh if !old_`rule' & !missing(hh)
        assert `: word count `r(levels)'' == 200 - 1 - `n_`rule''
        quietly levelsof hh if old_`rule' == 1
        assert `: word count `r(levels)'' == `k'
    }
}

* ---- 6. by() must be constant within clusters, as in sample2 -----------------
parqit use using `"`stem'_src.parquet"', name(B)
capture noisily parqit sample 25, cluster(hh) by(age)
assert _rc == 198
if (`have_sample2') {
    use `"`stem'_native.dta"', clear
    capture sample2 25, cluster(hh) by(age)
    assert _rc == 198
}

* ---- 7. generate()/keep() flag the same draw without dropping rows ----------
parqit use using `"`stem'_src.parquet"', name(G)
parqit sample 25, cluster(hh) by(region) seed(7) generate(pick)
parqit sample 25, cluster(hh) by(region) seed(7) keep(pick2)
capture noisily parqit sample 25, cluster(hh) generate(pick)
assert _rc == 198
capture noisily parqit sample 25, cluster(hh) generate(a1) keep(a2)
assert _rc == 198
parqit collect, clear
assert _N == 600
assert pick == pick2
assert pick == 1 if missing(hh)
bysort hh: assert pick == pick[1]
keep if pick == 1 & !missing(hh)
keep hh region
bysort hh: keep if _n == 1
cf _all using `"`stem'_drawn_hh.dta"'

* ---- 8. rows: by() strata and if frames with sample's counts ----------------
use `"`stem'_native.dta"', clear
contract region, freq(n)
gen long k = int(n * 10 / 100 + .5)
save `"`stem'_rquota.dta"'
parqit use using `"`stem'_src.parquet"', name(O)
parqit sample 10, by(region) seed(5)
parqit collect, clear
contract region, freq(got)
merge 1:1 region using `"`stem'_rquota.dta"', nogenerate
replace got = 0 if missing(got)
assert got == k
* native sample draws the same number of rows per stratum
use `"`stem'_native.dta"', clear
sample 10, by(region)
contract region, freq(got)
merge 1:1 region using `"`stem'_rquota.dta"', nogenerate
replace got = 0 if missing(got)
assert got == k
parqit use using `"`stem'_src.parquet"', name(O2)
parqit sample 10 if age > 60, seed(5)
parqit collect, clear
use `"`stem'_native.dta"', clear
count if age > 60
local nframe = r(N)
local nout = _N - `nframe'
parqit view O2
parqit collect, clear
count if age <= 60
assert r(N) == `nout'
count if age > 60
assert r(N) == int(`nframe' * 10 / 100 + .5)

* generate() alone flags exactly the rows the plain percentage form keeps
parqit use using `"`stem'_src.parquet"', name(P1)
parqit sample 20, seed(9)
parqit collect, clear
sort hh hhs region age inc
save `"`stem'_plain.dta"'
parqit use using `"`stem'_src.parquet"', name(P2)
parqit sample 20, seed(9) generate(g)
parqit collect, clear
keep if g == 1
drop g
sort hh hhs region age inc
cf _all using `"`stem'_plain.dta"'

* ---- 9. counts per stratum, 100 percent and count 0 ---------------------------
parqit use using `"`stem'_src.parquet"', name(C)
parqit sample 5, count cluster(hh) by(region) seed(2)
parqit collect, clear
forvalues r = 1/4 {
    quietly levelsof hh if region == `r'
    assert `: word count `r(levels)'' == 5
}
parqit use using `"`stem'_src.parquet"', name(C)
parqit sample 100, cluster(hh) generate(all_in)
parqit collect, clear
assert all_in == 1
parqit use using `"`stem'_src.parquet"', name(C)
parqit sample 0, count cluster(hh)
parqit collect, clear
assert _N == 3 & missing(hh)

* ---- 10. the existing forms keep their plans ----------------------------------
* long SQL lines wrap in the log; Python rejoins the "> " continuations
python:
def logged(path, needle):
    with open(path, encoding='utf-8', errors='replace') as fh:
        return needle in fh.read().replace('\n> ', '')
end
parqit use using `"`stem'_src.parquet"', name(X)
parqit sample 10, seed(3)
tempfile lx
quietly log using `"`lx'"', text name(v126x) replace
parqit show
quietly log close v126x
python: Macro.setLocal('found', str(int(logged(Macro.getLocal('lx'), 'ORDER BY hash(hash('))))
assert `found'
python: Macro.setLocal('found', str(int(logged(Macro.getLocal('lx'), '(design'))))
assert !`found'
parqit use using `"`stem'_src.parquet"', name(X)
parqit sample 50, count seed(3)
quietly log using `"`lx'"', text name(v126x) replace
parqit show
quietly log close v126x
python: Macro.setLocal('found', str(int(logged(Macro.getLocal('lx'), 'USING SAMPLE reservoir(50 ROWS) REPEATABLE (3)'))))
assert `found'

* any/all without cluster() are ignored with a note; both at once are refused
parqit use using `"`stem'_src.parquet"', name(N)
capture noisily parqit sample 10, by(region) any all
assert _rc == 198
capture noisily parqit sample 10, cluster(hh region)
assert _rc == 198

* ---- 11. the frame follows the session's missing semantics ------------------
* SQL mode: a missing age is outside the frame; statamissing on: age > 60 holds
* for a missing age, as in sample2. A comma inside a compound-quoted string of
* the if expression does not end it.
use `"`stem'_native.dta"', clear
replace age = . in 10/30
parqit save `"`stem'_miss.parquet"', data
count if age > 60 & !missing(age)
local n_sql = r(N)
count if age > 60
local n_stata = r(N)
foreach mode in off on {
    parqit set statamissing `mode'
    parqit use using `"`stem'_miss.parquet"', name(M)
    parqit sample 50 if age > 60, seed(4)
    parqit collect, clear
    local n = cond("`mode'" == "on", `n_stata', `n_sql')
    assert _N == 600 - `n' + int(`n' * 50 / 100 + .5)
}
parqit set statamissing off
parqit use using `"`stem'_src.parquet"', name(Q)
parqit sample 50 if hhs != `"h1, "x""' | age > 60, seed(4) cluster(hh) any
* the options after the quoted comma were all parsed
tempfile lq
quietly log using `"`lq'"', text name(v126q) replace
parqit show
quietly log close v126q
python: Macro.setLocal('found', str(int(logged(Macro.getLocal('lq'), 'cluster any, seed 4)'))))
assert `found'

* ---- 12. names and syntax the design refuses, with the view unchanged --------
parqit use using `"`stem'_src.parquet"', name(R)
quietly parqit describe
local steps = r(n_steps)
foreach bad in "generate(_n)" "generate(_N)" "keep(_n)" {
    capture noisily parqit sample 10, cluster(hh) `bad'
    assert _rc == 7
}
capture noisily parqit sample 10 if _n < 5, cluster(hh)
assert _rc == 198
tempfile ls
quietly log using `"`ls'"', text name(v126s) replace
capture noisily parqit sample 10 in 1/5
local rc_in = _rc
capture noisily parqit sample 10 if
local rc_if = _rc
capture noisily parqit sample if age > 60
local rc_amount = _rc
parqit sample 10, any seed(1)
quietly log close v126s
assert `rc_in' == 198 & `rc_if' == 198 & `rc_amount' == 198
python: Macro.setLocal('found', str(int(logged(Macro.getLocal('ls'), 'in is not supported on a lazy view'))))
assert `found'
python: Macro.setLocal('found', str(int(logged(Macro.getLocal('ls'), 'if requires an expression'))))
assert `found'
python: Macro.setLocal('found', str(int(logged(Macro.getLocal('ls'), 'note: option any ignored (no cluster())'))))
assert `found'
quietly parqit describe
assert r(n_steps) == `steps' + 1

* ---- 13. a strata value missing on one row of a cluster is refused ------------
use `"`stem'_native.dta"', clear
gen byte strat = region
replace strat = . in 10
parqit save `"`stem'_strat.parquet"', data
parqit use using `"`stem'_strat.parquet"', name(ST)
capture noisily parqit sample 25, cluster(hh) by(strat)
assert _rc == 198
if (`have_sample2') {
    capture sample2 25, cluster(hh) by(strat)
    assert _rc == 198
}

* ---- 14. double clusters that are not integers, against the Python key -------
use `"`stem'_native.dta"', clear
gen double hd = hh + 0.25
parqit save `"`stem'_hd.parquet"', data
parqit use using `"`stem'_hd.parquet"', name(HD)
parqit sample 30, cluster(hd) by(region) seed(13)
parqit save `"`stem'_s14.parquet"'
parqit collect, clear
_v126_whole hd
python:
got = {c for c in pq.read_table(stem + '_s14.parquet').to_pydict()['hd'] if c is not None}
want = expected(stem + '_hd.parquet', 'hd', ['region'], 30.0, 13, False)
assert got == want, sorted(got ^ want)[:10]
end

* ---- 15. generate() on rows with by(): the quota per stratum, nothing dropped -
parqit use using `"`stem'_src.parquet"', name(GR)
parqit sample 10, by(region) seed(5) generate(g10)
parqit collect, clear
assert _N == 600
keep if g10 == 1
contract region, freq(got)
merge 1:1 region using `"`stem'_rquota.dta"', nogenerate
replace got = 0 if missing(got)
assert got == k

* ---- 16. pending keep in: the cluster check catches it, rows wait for collect -
parqit use using `"`stem'_src.parquet"', name(KI)
parqit keep in 1/100000
quietly parqit describe
local steps = r(n_steps)
capture noisily parqit sample 25, cluster(hh)
assert _rc == 198
quietly parqit describe
assert r(n_steps) == `steps'
parqit sample 25, seed(2) generate(gk)
capture noisily parqit collect, clear
assert _rc == 198

* ---- 17. a design on a copied view leaves the source view alone -------------
parqit use using `"`stem'_src.parquet"', name(SRC)
parqit use using view:SRC, name(CPY)
parqit sample 25, cluster(hh) by(region) seed(7)
parqit view SRC
parqit collect, clear
sort hh age inc hhs region
tempfile back
save `back'
use `"`stem'_native.dta"', clear
sort hh age inc hhs region
cf _all using `back'
parqit view CPY
parqit collect, clear
keep hh region
drop if missing(hh)
bysort hh: keep if _n == 1
cf _all using `"`stem'_drawn_hh.dta"'
parqit close _all

di "VERDICT(V126_SAMPLE_DESIGN): PASS"
