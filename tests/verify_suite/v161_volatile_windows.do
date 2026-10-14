* V161: an engine window rewrite must not duplicate volatile SQL inputs.
clear all
set more off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 4
forvalues batch=1/2 {
    parqit sql "SELECT i%2 AS g, random() AS x FROM range(1000) t(i)"
    parqit egen double m = mean(x), by(g)
    if `batch' == 2 parqit egen double s = total(x), by(g)
    parqit collect, clear
    assert _N == 1000
    bysort g: egen double expected = mean(x)
    assert abs(m-expected) <= 1e-12
    if `batch' == 2 {
        bysort g: egen double expected_sum = total(x)
        assert abs(s-expected_sum) <= 1e-10
    }
    parqit close _all
}
* Disjoint intervals guarantee distinct keys, despite the volatile component.
* A duplicated evaluation would instead lose rows at the self-join.
parqit sql "SELECT i, avg(i) OVER (PARTITION BY i*2+random()) AS m FROM range(1000) t(i)"
parqit collect, clear
assert _N == 1000 & m == i
parqit close _all
* The volatile body is held in lambda bind data, not ordinary children.
foreach argument in 1 i {
    parqit sql "SELECT i%2 AS g,list_transform([`argument'],y -> random())[1] AS x FROM range(1000) t(i)"
    parqit egen double m = mean(x), by(g)
    parqit collect, clear
    assert _N == 1000
    bysort g: egen double expected = mean(x)
    assert abs(m-expected) <= 1e-12
    parqit close _all
}
* Order-dependent aggregates are not marked volatile. Their result must not
* be computed twice for the outer window either (Opus follow-up reproducer).
parqit set threads 8
parqit set memory_limit 512MB
foreach f in first any_value {
    parqit sql "SELECT k%7 AS g,`f'(i) AS x FROM (SELECT i%100000 AS k,i FROM range(4000000) t(i)) GROUP BY k"
    parqit egen double m = mean(x), by(g)
    parqit collect, clear
    assert _N == 100000
    bysort g: egen double expected = mean(x)
    assert m == expected
    parqit close _all
}
display "VERDICT(V161_VOLATILE_WINDOWS): PASS - volatile inputs, lambda bodies, partition keys and order-dependent aggregates preserve rows and native results"
