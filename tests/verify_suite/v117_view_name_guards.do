* V117 — helper names and reshape/merge output identities never alias user data.
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem

* Native reshape permits case-distinct x/X; the lazy engine must refuse a
* collision before committing a plan that would bind x to X.
input id X x1 x2
1 999 10 20
end
label variable x1 "Wave one"
parqit save `"`stem'_reshape.parquet"', data
preserve
reshape long x, i(id) j(j)
assert X == 999 & x == 10*j
restore
parqit use using `"`stem'_reshape.parquet"'
capture noisily parqit reshape long x, i(id) j(j)
assert _rc == 198
parqit collect, clear
assert _N == 1 & id == 1 & X == 999 & x1 == 10 & x2 == 20
assert `"`: variable label x1'"' == "Wave one"
parqit close _all

* Existing case aliases remain valid when their engine names do not collide.
clear
input id x X x1 x2
1 888 999 10 20
end
parqit save `"`stem'_alias.parquet"', data
parqit use using `"`stem'_alias.parquet"'
parqit drop x
parqit reshape long x, i(id) j(j)
parqit gen double got = x
parqit collect, clear
assert _N == 2 & X == 999 & x == 10*j & got == x
parqit close _all

* Exact exposed names also remain unique after source aliases are assigned.
clear
input id X x x1 x2
1 999 888 10 20
end
parqit save `"`stem'_exposed.parquet"', data
parqit use using `"`stem'_exposed.parquet"'
capture noisily parqit reshape long x, i(id) j(j)
assert _rc == 198
parqit collect, clear
assert _N == 1 & X == 999 & x == 888 & x1 == 10 & x2 == 20
parqit close _all

* Helpers must avoid the engine's ASCII case folding, including after rename.
clear
input double marker x
999 10
888 20
end
gen double native_seq = _n
gen double native_total = _N
parqit save `"`stem'_helper.parquet"', data
parqit use using `"`stem'_helper.parquet"'
parqit rename marker __PARQIT_RN_1
parqit keep __PARQIT_RN_1 x native_seq native_total
parqit gen double seq = _n
parqit collect, clear
assert seq == native_seq & __PARQIT_RN_1 == 999 - 111*(_n-1)
parqit close _all
parqit use using `"`stem'_helper.parquet"'
parqit rename marker __PARQIT_NN_1
parqit gen double total = _N
parqit collect, clear
assert total == native_total & __PARQIT_NN_1 == 999 - 111*(_n-1)
parqit close _all

* Using-side helper collisions must not turn a match into master-only.
clear
input id payload
1 10
end
parqit save `"`stem'_master.parquet"', data
clear
input id double __PARQIT_UM_2
1 .
end
parqit save `"`stem'_using_helper.parquet"', data
parqit use using `"`stem'_master.parquet"'
parqit merge 1:1 id using `"`stem'_using_helper.parquet"'
parqit collect, clear
assert _N == 1 & _merge == 3 & payload == 10 & missing(__PARQIT_UM_2)
parqit close _all

* Native refuses an existing merge marker on the using side (r(110));
* parqit keeps its existing name-error code (r(198)) and its old view.
clear
input id byte _merge
1 99
end
save `"`stem'_using_marker.dta"'
parqit save `"`stem'_using_marker.parquet"', data
clear
input id payload
1 10
end
capture noisily merge 1:1 id using `"`stem'_using_marker.dta"'
assert _rc == 110
assert _N == 1 & payload == 10
parqit use using `"`stem'_master.parquet"'
capture noisily parqit merge 1:1 id using `"`stem'_using_marker.parquet"'
assert _rc == 198
parqit collect, clear
assert _N == 1 & id == 1 & payload == 10
capture confirm variable _merge
assert _rc == 111
parqit merge 1:1 id using `"`stem'_using_marker.parquet"', gen(matched)
parqit collect, clear
assert _N == 1 & _merge == 99 & matched == 3 & payload == 10
parqit close _all

* A using marker excluded by keepusing() does not collide with the output.
parqit use using `"`stem'_master.parquet"'
parqit merge 1:1 id using `"`stem'_using_marker.parquet"', keepusing(id)
parqit collect, clear
assert _N == 1 & _merge == 3 & payload == 10
parqit close _all

* Alias identity matters even when the SQL names differ.
clear
input id byte _MERGE _merge
1 98 99
end
parqit save `"`stem'_marker_alias.parquet"', data
parqit use using `"`stem'_marker_alias.parquet"', name(markers)
parqit drop _MERGE
parqit use using `"`stem'_master.parquet"', name(master)
capture noisily parqit merge 1:1 id using view:markers
assert _rc == 198
parqit collect, clear
assert _N == 1 & id == 1 & payload == 10
parqit merge 1:1 id using view:markers, gen(matched)
parqit collect, clear
assert _N == 1 & _merge == 99 & matched == 3 & payload == 10
parqit close _all

* Extend parqit's existing append-marker refusal to exposed aliases as well.
clear
input id X x
1 999 888
end
label variable x "Using payload"
parqit save `"`stem'_append_alias.parquet"', data
parqit use using `"`stem'_append_alias.parquet"', name(append_alias)
parqit drop X
parqit use using `"`stem'_master.parquet"', name(master)
capture noisily parqit append using view:append_alias, generate(x)
assert _rc == 198
parqit collect, clear
assert _N == 1 & id == 1 & payload == 10
parqit append using view:append_alias, generate(source)
parqit collect, clear
assert _N == 2
assert payload == 10 & missing(x) if source == 0
assert x == 888 & missing(payload) if source == 1
assert `"`: variable label x'"' == "Using payload"
parqit close _all

di "VERDICT(V117_VIEW_NAME_GUARDS): PASS"
