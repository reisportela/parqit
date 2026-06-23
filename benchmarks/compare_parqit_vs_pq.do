* ============================================================================
*  parqit  vs  pq (stata_parquet_io) — comparação de PROPRIEDADES, FIABILIDADE
*  e TEMPO DE COMPUTAÇÃO das funções comuns aos dois: use, save, describe,
*  merge, append.
*
*  Diferença de arquitetura a ter em mente:
*    - pq   : ponte de I/O. Lê o Parquet para a MEMÓRIA da Stata e depois usa
*             comandos nativos (ex.: `pq merge` lê o using para uma frame e
*             corre o `merge` nativo). O master tem de estar em memória.
*    - parqit : camada de manipulação OUT-OF-CORE. Os verbos (merge/append/…)
*             constroem um plano lazy compilado para SQL no DuckDB; só o
*             resultado é materializado (`collect` p/ memória, `save` p/ disco).
*
*  Para uma comparação "maçã com maçã" cada operação termina com o resultado
*  EM MEMÓRIA nos dois casos (no parqit via `collect`), e compara-se a igualdade
*  dos dados com uma ASSINATURA independente do tipo de armazenamento.
*
*  COMO USAR: corre SECÇÃO A SECÇÃO. O estado persiste em GLOBALS ($...), não em
*  locals, para poderes executar e inspecionar bloco a bloco.
* ============================================================================


* ===========================================================================
*  SECÇÃO 0 — setup + helpers (corre UMA VEZ, primeiro)
* ===========================================================================
clear all
set more off
set varabbrev off

* >>> ajusta se necessário <<<
global REPO : env PARQIT_REPO
if (`"$REPO"' == "") global REPO `"`c(pwd)'"'
global DATA "$REPO/benchmarks/_out/synthetic_medium_data"
global OUT  "/tmp/parqit_pq_cmp"        // saídas temporárias
global REPS 3                          // repetições por tempo (min-de-N)

adopath ++ "$REPO/src/ado/p"
global PARQIT_PLUGIN_PATH "$REPO/build/dev/parqit.plugin"
shell mkdir -p "$OUT"

* ficheiros usados (muda aqui para escalar)
global F_USE   "$DATA/workers_perf.parquet"          // use / save (10M x 13)
global M_MAST  "$DATA/workers_perf.parquet"          // merge: master
global M_USING "$DATA/firms_perf.parquet"            // merge: using (500k)
global M_KEY   "firm_id"
global M_KEEP  "tfp capital industry"
global A_FILE  "$DATA/patents_perf.parquet"          // append: A e B (self, 1M->2M)

which parqit
which pq
parqit version

* --- helper: assinatura canónica e independente do tipo de armazenamento ---
*  recast de todas as numéricas p/ double (neutraliza int vs long vs double),
*  ordena variáveis e observações de forma determinística, e assina os valores.
*  Põe $SIG (assinatura), $NOBS, $NVARS. (Locals ficam internos ao programa.)
capture program drop _canonsig
program define _canonsig
    quietly ds, has(type numeric)
    if "`r(varlist)'" != "" quietly recast double `r(varlist)'
    capture order _all, alphabetic
    sort _all
    quietly datasignature
    global SIG   = "`r(datasignature)'"
    global NOBS  = _N
    global NVARS = c(k)
end

* --- helper: imprime a linha de comparação de uma operação ---
capture program drop _report
program define _report
    args op
    local eq = cond("$SIG_PQ" == "$SIG_PARQIT", "IGUAIS  OK", "DIFEREM  <<< VER")
    di as txt "{hline 78}"
    di as txt "`op'"
    di as txt "  pq   : N=" as res "$N_PQ"   as txt " k=" as res "$K_PQ" ///
        as txt "  tempo(min)=" as res %7.3f $T_PQ   as txt " s"
    di as txt "  parqit : N=" as res "$N_PARQIT" as txt " k=" as res "$K_PARQIT" ///
        as txt "  tempo(min)=" as res %7.3f $T_PARQIT as txt " s"
    di as txt "  resultado (valores): " as res "`eq'"
    if ($T_PARQIT > 0) di as txt "  rácio de tempo pq/parqit = " as res %5.2f ($T_PQ/$T_PARQIT) ///
        as txt "  (>1 => parqit mais rápido)"
end


* ===========================================================================
*  SECÇÃO 1 — USE: ler Parquet para a memória da Stata
*    pq use using F, clear     vs     parqit use using F, clear
* ===========================================================================
global T_PQ = 1e9
global T_PARQIT = 1e9
forvalues i = 1/$REPS {
    clear
    timer clear 1
    timer on 1
    quietly pq use using "$F_USE", clear
    timer off 1
    quietly timer list 1
    global T_PQ = min($T_PQ, r(t1))
    if (`i' == $REPS) {
        _canonsig
        global SIG_PQ "$SIG"
        global N_PQ $NOBS
        global K_PQ $NVARS
    }

    clear
    capture parqit close _all
    timer clear 2
    timer on 2
    quietly parqit use using "$F_USE", clear
    timer off 2
    quietly timer list 2
    global T_PARQIT = min($T_PARQIT, r(t2))
    if (`i' == $REPS) {
        _canonsig
        global SIG_PARQIT "$SIG"
        global N_PARQIT $NOBS
        global K_PARQIT $NVARS
    }
}
_report "USE  (ler para memória)  —  $F_USE"


* ===========================================================================
*  SECÇÃO 2 — SAVE: escrever a memória para Parquet
*    carrega os dados UMA vez; depois compara a escrita:
*    pq save using out, replace   vs   parqit save out, replace data
*    (compara os dois ficheiros relendo-os)
* ===========================================================================
clear
capture parqit close _all
quietly parqit use using "$F_USE", clear        // dados em memória (nenhum save os altera)

global T_PQ = 1e9
global T_PARQIT = 1e9
forvalues i = 1/$REPS {
    timer clear 1
    timer on 1
    quietly pq save using "$OUT/save_pq.parquet", replace
    timer off 1
    quietly timer list 1
    global T_PQ = min($T_PQ, r(t1))

    timer clear 2
    timer on 2
    quietly parqit save "$OUT/save_parqit.parquet", replace data
    timer off 2
    quietly timer list 2
    global T_PARQIT = min($T_PARQIT, r(t2))
}
* fiabilidade: reler os dois ficheiros e comparar valores
capture parqit close _all
quietly parqit use using "$OUT/save_pq.parquet", clear
_canonsig
global SIG_PQ "$SIG"
global N_PQ $NOBS
global K_PQ $NVARS
capture parqit close _all
quietly parqit use using "$OUT/save_parqit.parquet", clear
_canonsig
global SIG_PARQIT "$SIG"
global N_PARQIT $NOBS
global K_PARQIT $NVARS
_report "SAVE (escrever memória->Parquet, relido p/ comparar)"


* ===========================================================================
*  SECÇÃO 3 — DESCRIBE: metadados (linhas/colunas), sem ler dados
*    pq describe using F     vs     parqit describe F
* ===========================================================================
global T_PQ = 1e9
global T_PARQIT = 1e9
forvalues i = 1/$REPS {
    timer clear 1
    timer on 1
    quietly pq describe using "$M_USING"
    timer off 1
    quietly timer list 1
    global T_PQ = min($T_PQ, r(t1))

    timer clear 2
    timer on 2
    quietly parqit describe "$M_USING"
    timer off 2
    quietly timer list 2
    global T_PARQIT = min($T_PARQIT, r(t2))
    if (`i' == $REPS) {
        global N_PARQIT = r(n_rows)
        global K_PARQIT = r(n_cols)
    }
}
* pq describe imprime as dimensões; parqit expõe r(n_rows)/r(n_cols)
di as txt "{hline 78}"
di as txt "DESCRIBE  —  $M_USING"
di as txt "  pq   : tempo(min)=" as res %7.3f $T_PQ   as txt " s   (ver dimensões impressas acima)"
di as txt "  parqit : N=" as res "$N_PARQIT" as txt " k=" as res "$K_PARQIT" ///
    as txt "  tempo(min)=" as res %7.3f $T_PARQIT as txt " s"
* mostra o describe do pq uma vez, visível:
di as txt "  --- pq describe (saída) ---"
pq describe using "$M_USING"


* ===========================================================================
*  SECÇÃO 4 — MERGE m:1  (a operação que pediste)
*    pq  : pq use master,clear ; pq merge m:1 KEY using USING, keepusing() nogen
*    parqit: parqit use master ; parqit merge m:1 KEY using USING, keepusing() nogen ;
*          parqit collect, clear
*    Resultado em memória nos dois; compara-se igualdade de valores.
*    Propriedade-chave: o pq carrega o MASTER (10M) inteiro para memória; o
*    parqit nunca o materializa até ao collect (e nem isso é preciso se gravares
*    com `parqit save`, ver a linha de "propriedade" no fim).
* ===========================================================================
global T_PQ = 1e9
global T_PARQIT = 1e9
forvalues i = 1/$REPS {
    * --- pq ---
    clear
    timer clear 1
    timer on 1
    quietly pq use using "$M_MAST", clear
    quietly pq merge m:1 $M_KEY using "$M_USING", keepusing($M_KEEP) nogenerate
    timer off 1
    quietly timer list 1
    global T_PQ = min($T_PQ, r(t1))
    if (`i' == $REPS) {
        _canonsig
        global SIG_PQ "$SIG"
        global N_PQ $NOBS
        global K_PQ $NVARS
    }

    * --- parqit ---
    clear
    capture parqit close _all
    timer clear 2
    timer on 2
    quietly parqit use using "$M_MAST"
    quietly parqit merge m:1 $M_KEY using "$M_USING", keepusing($M_KEEP) nogenerate
    quietly parqit collect, clear
    timer off 2
    quietly timer list 2
    global T_PARQIT = min($T_PARQIT, r(t2))
    if (`i' == $REPS) {
        _canonsig
        global SIG_PARQIT "$SIG"
        global N_PARQIT $NOBS
        global K_PARQIT $NVARS
    }
}
_report "MERGE m:1 $M_KEY  (master=$M_MAST  using=$M_USING)"

* propriedade out-of-core: o parqit pode gravar o merge SEM carregar o master
* para a memória da Stata (o pq não — precisa do master em memória).
capture parqit close _all
timer clear 3
timer on 3
quietly parqit use using "$M_MAST"
quietly parqit merge m:1 $M_KEY using "$M_USING", keepusing($M_KEEP) nogenerate
quietly parqit save "$OUT/merge_parqit.parquet", replace
timer off 3
quietly timer list 3
di as txt "  propriedade: parqit merge -> save OUT-OF-CORE (sem tocar na memória) = " ///
    as res %7.3f r(t3) as txt " s   (o pq teria de carregar o master de $M_MAST)"
capture parqit close _all


* ===========================================================================
*  SECÇÃO 5 — APPEND
*    pq  : pq use A,clear ; pq append using B
*    parqit: parqit use A ; parqit append using B ; parqit collect, clear
* ===========================================================================
global T_PQ = 1e9
global T_PARQIT = 1e9
forvalues i = 1/$REPS {
    * --- pq ---
    clear
    timer clear 1
    timer on 1
    quietly pq use using "$A_FILE", clear
    quietly pq append using "$A_FILE"
    timer off 1
    quietly timer list 1
    global T_PQ = min($T_PQ, r(t1))
    if (`i' == $REPS) {
        _canonsig
        global SIG_PQ "$SIG"
        global N_PQ $NOBS
        global K_PQ $NVARS
    }

    * --- parqit ---
    clear
    capture parqit close _all
    timer clear 2
    timer on 2
    quietly parqit use using "$A_FILE"
    quietly parqit append using "$A_FILE"
    quietly parqit collect, clear
    timer off 2
    quietly timer list 2
    global T_PARQIT = min($T_PARQIT, r(t2))
    if (`i' == $REPS) {
        _canonsig
        global SIG_PARQIT "$SIG"
        global N_PARQIT $NOBS
        global K_PARQIT $NVARS
    }
}
_report "APPEND  (A=B=$A_FILE, self-append)"
capture parqit close _all


* ===========================================================================
*  SECÇÃO 6 — nota final
* ===========================================================================
di as txt "{hline 78}"
di as txt "Leitura: 'resultado: IGUAIS OK' confirma que parqit e pq produzem os"
di as txt "MESMOS valores (assinatura independente do tipo de armazenamento)."
di as txt "Se algum disser 'DIFEREM', inspeciona com describe/list os dois lados."
di as txt "Os tempos são min-de-$REPS execuções intercaladas (mesma carga)."
di as result "FIM."
