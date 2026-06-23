* ============================================================================
*  parqit — replicação MANUAL dos testes de correção + performance
*  (otimização do `parqit collect` passthrough + correção do floor de DATE)
*
*  COMO USAR: corre SECÇÃO A SECÇÃO. O estado persiste em GLOBALS ($...),
*  não em locals, por isso podes executar cada bloco isoladamente e inspecionar
*  os resultados entre blocos. Os únicos locals são iteradores de loop (corre
*  o loop todo de uma vez) e ficam escondidos no programa auxiliar `parqit_fp`.
*
*  Ordem: corre a Secção 0 primeiro (setup). Depois 1..7 por qualquer ordem.
* ============================================================================


* ===========================================================================
*  SECÇÃO 0 — setup (corre UMA VEZ, primeiro)
* ===========================================================================
clear all
set more off
set varabbrev off

* >>> ajusta estes 3 caminhos se necessário <<<
global REPO : env PARQIT_REPO
if (`"$REPO"' == "") global REPO `"`c(pwd)'"'
global REF : env PARQIT_BENCH_REF
if (`"$REF"' == "") global REF "parqit_benchmark_ref.parquet"
global DATA "$REPO/benchmarks/_out/synthetic_medium_data"

adopath ++ "$REPO/src/ado/p"
global PARQIT_PLUGIN_PATH "$REPO/build/dev/parqit.plugin"

which parqit
parqit version


* ---------------------------------------------------------------------------
*  SECÇÃO 0b — programa auxiliar de "fingerprint" (corre UMA VEZ)
*  Põe em  $FP   a lista  nome:tipo:formato  de todas as variáveis em memória,
*  e em    $SIG  a assinatura de dados (valores).  Locals ficam internos aqui.
* ---------------------------------------------------------------------------
capture program drop parqit_fp
program define parqit_fp
    global FP ""
    foreach v of varlist _all {
        local t : type `v'
        local f : format `v'
        global FP "$FP `v':`t':`f'"
    }
    quietly datasignature
    global SIG "`r(datasignature)'"
end


* ===========================================================================
*  SECÇÃO 1 — CORREÇÃO: `use` == `use`+`collect` no ficheiro ALL-NUMERIC
*  (é aqui que a otimização F2 passa a actuar no collect)
* ===========================================================================
capture confirm file "$REF"
if (_rc) di as err ">> ficheiro de referência não encontrado: $REF"

* --- 1A) leitura DIRETA: parqit use ..., clear ---
clear
capture parqit close _all
quietly parqit use using "$REF", clear
parqit_fp
global FP_USE  "$FP"
global SIG_USE "$SIG"
describe

* --- 1B) VIA VIEW: parqit use ... (lazy) + parqit collect, clear ---
clear
capture parqit close _all
quietly parqit use using "$REF"
quietly parqit collect, clear
parqit_fp
global FP_COL  "$FP"
global SIG_COL "$SIG"
describe

* --- 1C) comparar (tipos+formatos e valores têm de ser IDÊNTICOS) ---
display as txt "tipos/formatos iguais? " as res ("$FP_USE"  == "$FP_COL")
display as txt "assinatura igual?      " as res ("$SIG_USE" == "$SIG_COL")
assert "$FP_USE"  == "$FP_COL"
assert "$SIG_USE" == "$SIG_COL"
display as result "OK Secção 1: use == collect byte-idêntico (all-numeric)"


* ===========================================================================
*  SECÇÃO 2 — CORREÇÃO: `use` == `use`+`collect` num ficheiro MISTO
*  workers_perf: int64/int32/double+missings/string/DATE32 (a coluna hire_date
*  é DATE → tem de ficar `long` em AMBOS os caminhos)
* ===========================================================================
global WRK "$DATA/workers_perf.parquet"
capture confirm file "$WRK"
if (_rc) di as err ">> não encontrado: $WRK  (gera com: python3 benchmarks/make_synthetic_data.py)"

* --- 2A) direto ---
clear
capture parqit close _all
quietly parqit use using "$WRK", clear
parqit_fp
global FP_USE  "$FP"
global SIG_USE "$SIG"
global THD_USE : type hire_date
describe

* --- 2B) view + collect ---
clear
capture parqit close _all
quietly parqit use using "$WRK"
quietly parqit collect, clear
parqit_fp
global FP_COL  "$FP"
global SIG_COL "$SIG"
global THD_COL : type hire_date
describe

* --- 2C) comparar ---
display as txt "hire_date (DATE) tipo: use=" as res "$THD_USE" as txt "  collect=" as res "$THD_COL"
display as txt "tipos/formatos iguais? " as res ("$FP_USE" == "$FP_COL")
display as txt "assinatura igual?      " as res ("$SIG_USE" == "$SIG_COL")
assert "$THD_USE" == "long"
assert "$THD_COL" == "long"
assert "$FP_USE"  == "$FP_COL"
assert "$SIG_USE" == "$SIG_COL"
display as result "OK Secção 2: use == collect; DATE = long nos dois caminhos"


* ===========================================================================
*  SECÇÃO 3 — CORREÇÃO: coluna DATE NÃO transborda (oráculo independente)
*  far_dates.parquet (fixture committed): datas 1900..2099.
*  2099-12-31 = 51134 dias desde 1960-01-01 (> 32740 = max do `int` da Stata).
*  Se fosse `int`, transbordaria; tem de ser `long` com o valor exacto.
* ===========================================================================
global FAR "$REPO/tests/fixtures/far_dates.parquet"
capture confirm file "$FAR"
if (_rc) di as err ">> fixture em falta: $FAR"

* --- 3A) direto ---
clear
capture parqit close _all
quietly parqit use using "$FAR", clear
sort id
global THD_USE : type d
quietly summarize d if id == 3, meanonly
global VAL_USE = r(mean)
list, noobs
display as txt "use:     d é " as res "$THD_USE" as txt "  ; 2099-12-31 (id=3) = " as res $VAL_USE

* --- 3B) collect ---
clear
capture parqit close _all
quietly parqit use using "$FAR"
quietly parqit collect, clear
sort id
global THD_COL : type d
quietly summarize d if id == 3, meanonly
global VAL_COL = r(mean)
list, noobs
display as txt "collect: d é " as res "$THD_COL" as txt "  ; 2099-12-31 (id=3) = " as res $VAL_COL

* --- 3C) oráculo: long + valor 51134 em ambos ---
assert "$THD_USE" == "long"
assert "$THD_COL" == "long"
assert $VAL_USE == 51134
assert $VAL_COL == 51134
display as result "OK Secção 3: DATE colecta como long, valor 51134 (sem overflow)"


* ===========================================================================
*  SECÇÃO 4 — PERFORMANCE A/B no ficheiro de referência (all-numeric)
*  Mede min-de-6, mesma sessão, do tempo de:
*    A) parqit use ..., clear        (caminho direto, F2)
*    B) parqit use ... + parqit collect (passthrouth — AGORA também com F2)
*  Esperado DEPOIS da otimização: B - A ~ 0 (antes era ~ +0.24 s).
*  NOTA: corre cada loop `forvalues {...}` COMO UM BLOCO ÚNICO.
* ===========================================================================
* aquece a cache do SO uma vez
clear
capture parqit close _all
quietly parqit use using "$REF", clear

* A e B INTERCALADOS no mesmo loop (cada par vê a mesma carga da máquina —
* mais robusto ao ruído do que medir A todo e depois B todo). Corre o loop
* COMO UM BLOCO ÚNICO.
global BESTA = 1e9
global BESTB = 1e9
forvalues i = 1/6 {
    * A) parqit use, clear
    clear
    capture parqit close _all
    timer clear 1
    timer on 1
    quietly parqit use using "$REF", clear
    timer off 1
    quietly timer list 1
    global TA = r(t1)
    global BESTA = min($BESTA, $TA)

    * B) parqit use + parqit collect
    clear
    capture parqit close _all
    timer clear 2
    timer on 2
    quietly parqit use using "$REF"
    quietly parqit collect, clear
    timer off 2
    quietly timer list 2
    global TB = r(t2)
    global BESTB = min($BESTB, $TB)

    display as txt "rep `i':  A use,clear=" as res %6.3f $TA as txt "   B use+collect=" as res %6.3f $TB
}
display as result "min A(use,clear)=" %6.3f $BESTA "   min B(use+collect)=" %6.3f $BESTB ///
    "   B-A=" %6.3f ($BESTB - $BESTA) " s   (~0 depois da otimização; era ~+0.24)"


* ===========================================================================
*  SECÇÃO 5 (opcional) — PERFORMANCE A/B no workers (string-heavy)
*  Confirma que NÃO há regressão num ficheiro com strings (B ~ A).
* ===========================================================================
clear
capture parqit close _all
quietly parqit use using "$WRK", clear

* novamente intercalado (corre como um bloco)
global BESTA = 1e9
global BESTB = 1e9
forvalues i = 1/6 {
    clear
    capture parqit close _all
    timer clear 3
    timer on 3
    quietly parqit use using "$WRK", clear
    timer off 3
    quietly timer list 3
    global TA = r(t3)
    global BESTA = min($BESTA, $TA)

    clear
    capture parqit close _all
    timer clear 4
    timer on 4
    quietly parqit use using "$WRK"
    quietly parqit collect, clear
    timer off 4
    quietly timer list 4
    global TB = r(t4)
    global BESTB = min($BESTB, $TB)
}
display as txt "workers: A(use)=" as res %6.3f $BESTA as txt "  B(collect)=" as res %6.3f $BESTB ///
    as txt "  B-A=" as res %6.3f ($BESTB - $BESTA) as txt " s  (esperado ~0 — sem regressão)"


* ===========================================================================
*  SECÇÃO 6 (opcional) — GLOB multi-ficheiro: use == collect byte-idêntico
*  Gera 2 shards all-numeric com o MESMO esquema via duckdb e lê com glob.
*  (requer o CLI `duckdb` no PATH)
* ===========================================================================
global GDIR "$REPO/benchmarks/_out/_glob_demo"
shell mkdir -p "$GDIR"
shell duckdb -c "COPY (SELECT i AS id, (i*7)%1000 AS v, i::DOUBLE/3 AS x FROM range(1,500000) t(i)) TO '$GDIR/part_a.parquet' (FORMAT PARQUET)"
shell duckdb -c "COPY (SELECT i AS id, (i*3)%1000 AS v, i::DOUBLE/7 AS x FROM range(500000,1200000) t(i)) TO '$GDIR/part_b.parquet' (FORMAT PARQUET)"

clear
capture parqit close _all
quietly parqit use using "$GDIR/part_*.parquet", clear
parqit_fp
global FP_USE  "$FP"
global SIG_USE "$SIG"

clear
capture parqit close _all
quietly parqit use using "$GDIR/part_*.parquet"
quietly parqit collect, clear
parqit_fp
global FP_COL  "$FP"
global SIG_COL "$SIG"

assert "$FP_USE"  == "$FP_COL"
assert "$SIG_USE" == "$SIG_COL"
display as result "OK Secção 6: glob multi-ficheiro use == collect byte-idêntico"


* ===========================================================================
*  SECÇÃO 7 (opcional) — "engine floor": parqit já está no limite do DuckDB
*  Compara o tempo de cada workflow no DuckDB puro (via shell). Útil para ver
*  que os save-workflows não têm folga acima do motor. (requer `duckdb` no PATH)
* ===========================================================================
* Tudo numa só linha física (o `shell` da Stata não aceita continuação com `\`).
* `.timer on` persiste entre os vários `-c`, dando o tempo real por query.
shell duckdb -c ".timer on" -c "COPY (SELECT * FROM read_parquet('$DATA/workers_perf.parquet')) TO '/tmp/o_scan.parquet' (FORMAT PARQUET)" -c "COPY (SELECT *, log(wage) AS lwage FROM read_parquet('$DATA/workers_perf.parquet') WHERE year>=2020 AND wage>0) TO '/tmp/o_filter.parquet' (FORMAT PARQUET)" -c "COPY (SELECT firm_id, year, avg(wage) AS wage, stddev_samp(wage) AS sd, count(wage) AS n FROM read_parquet('$DATA/workers_perf.parquet') GROUP BY firm_id, year) TO '/tmp/o_collapse.parquet' (FORMAT PARQUET)" -c "COPY (SELECT * FROM read_parquet('$DATA/workers_perf.parquet') ORDER BY firm_id, year, wage) TO '/tmp/o_sort.parquet' (FORMAT PARQUET)"

display as result "FIM."
