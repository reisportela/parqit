# Relatorio para corrigir `parqit`

Data: 2026-06-24
Repositorio-alvo: `reisportela/parqit`
Commit inspecionado localmente: `722a2c83e809aca4422b0f020ab595ada55f62a4` (== HEAD)
Versao instalada testada: `parqit 0.1.11`
Ambiente de reproducao: StataNow 19.5 MP, Windows 11

Este relatorio e o briefing de correcao para o Claude. A correcao deve ser feita
no repositorio upstream do `parqit`, nao no test-pack. Todas as reproducoes abaixo
foram confirmadas ao vivo contra `parqit 0.1.11` (nao sao apenas leitura de log).

## Objetivo

Corrigir um crash (`st_global(): 3300 argument out of range`) na restauracao de
characteristics/notas que ocorre sempre que o resultado materializado nao contem
uma variavel que carregava uma `char` ou uma nota. Isto inclui o caminho mais
comum de todos: ler um subconjunto de colunas com `parqit use <varlist> using
f.parquet, clear`.

## Resumo do bug (confirmado empiricamente)

- Mecanismo: ao decorar o resultado, o ado executa
  `st_global(tgt + "[" + cname + "]", val)` para cada characteristic guardada no
  ficheiro. Se `tgt` for um nome Stata legal mas nao existir no resultado,
  `st_global` aborta com rc 3300. Notas de Stata sao characteristics
  (`var[note0]`, `var[note1]`, `_dta[note#]`), por isso seguem o mesmo caminho.
- Falha ruidosa, nao corrupcao silenciosa: rc != 0 com mensagem, e o estado em
  memoria fica intacto (o `use, clear` falhado deixa a memoria como estava). Nao
  ha perda silenciosa de dados. Classe: robustez/abort, nao "rc 0 com dados
  stale".
- Severidade: ALTA por alcance (basta um `use` de subconjunto de colunas sobre
  qualquer ficheiro com char ou nota numa coluna nao pedida), mas e um abort
  recuperavel, nao corrupcao.

Casos confirmados ao vivo (parqit 0.1.11):

| Caso                                                              | rc      |
|-------------------------------------------------------------------|---------|
| `parqit use <subset> using f, clear` exclui coluna com char       | 3300    |
| `parqit use <subset> using f, clear` exclui coluna com nota       | 3300    |
| `contract` remove coluna com char, depois `collect`               | 3300    |
| `collapse` remove coluna com char, depois `collect`               | 3300    |
| `contract` remove coluna com nota, depois `collect`               | 3300    |
| `rename` de coluna com char, depois `collect`  (NAO e bug)        | 0 (ok)  |
| `use` integral (todas as colunas), char aplicada a coluna viva    | 0 (ok)  |

Importante: `rename` NAO esta afetado. A view remapeia a char para o novo nome
(`src/engine/view.cpp:401-404`), por isso `parqit rename` preserva a char
corretamente. Nao incluir `rename` na lista de casos a corrigir; incluir antes
um teste que garante que a correcao NAO regride o rename.

## Erro confirmado: PARQIT-CHAR-01

### Contrato / documentacao

- `parqit use [varlist] using ...` e `parqit contract varlist, freq(newvar)` sao
  documentados em `src/ado/p/parqit.sthlp:59` e seguintes.
- O help promete que labels, value labels, notas, formatos e characteristics sao
  guardados e restaurados: `src/ado/p/parqit.sthlp:129-131` e `README.md:361-362`.

### Reproducao minima A (caminho mais comum: use de subconjunto)

```stata
clear
set obs 4
gen x = _n
gen z = _n*10
char x[source] "synthetic"

tempfile f
parqit save "`f'.parquet", replace data

clear
parqit use z using "`f'.parquet", clear
* OBSERVADO: st_global(): 3300 ; _parqit_resp_decorate(): function returned error
* ESPERADO apos correcao: rc 0, z carregado, char de x descartada (sem alvo no
* resultado)
```

### Reproducao minima B (contract + collect)

```stata
clear
set obs 3
gen x = _n
gen g = mod(_n, 2)
char x[source] "synthetic"

tempfile f
parqit save "`f'.parquet", replace data

clear
parqit use using "`f'.parquet"
parqit contract g, freq(freq)
parqit collect, clear
* OBSERVADO: rc 3300 ; ESPERADO: rc 0, tabela com g e freq (2 obs)
```

`collapse` reproduz o mesmo (remove a coluna com char antes do `collect`); uma
nota de variavel numa coluna removida reproduz o mesmo (notas sao chars).

### Evidencia no test-pack externo

- `tests/run_all_parqit_tests.do:139` cria `char wage[source] "synthetic"`.
- `tests/run_all_parqit_tests.do:458-461` corre `parqit contract region sector,
  freq(freq)` seguido de `collect`.
- `tests/outputs/parqit_adversarial_tests.log:1512-1520` mostra o rc 3300.

## Root cause

### Proximal (ado)

`_parqit_resp_decorate()` em `src/ado/p/parqit.ado:2539-2549`. O ramo `char`
valida apenas LEGALIDADE de nome, nunca EXISTENCIA da variavel-alvo:

- guard atual: `if (st_isname(cname) & (tgt == "_dta" | st_isname(tgt)))`
  (linha 2544)
- chamada vulneravel: `st_global(tgt + "[" + cname + "]", ...)` (linha 2545)

Os ramos `var` (2504-2518) e formatos (`_parqit_resp_create`, 2459-2462) aplicam
metadados a variaveis RECEM-CRIADAS e sao seguros; `vlab` (2520-2537, 2563-2568)
aplica a um nome de label, nao a uma variavel, por isso no pior caso deixa um
label orfao, mas nao aborta. O ramo `char` e o UNICO sitio da decoracao que
aplica metadados a um alvo que pode nao existir no resultado.

### Raiz a montante (plugin) -- recomendado tratar tambem aqui

A assimetria que origina o bug esta no emitter da resposta:

- registos `var` sao podados as colunas sobreviventes (iteram `ctx.active`):
  `src/plugin/plugin_io.cpp:488-499`.
- registos `char` sao despejados em bruto do metadata do ficheiro, SEM filtro:
  `src/plugin/plugin_io.cpp:519-527`.
- o caminho de `save` ja filtra as chars as colunas vivas:
  `src/plugin/plugin_view.cpp:311-318`.
- o caminho de `collect` copia o mapa inteiro sem filtro:
  `src/plugin/plugin_view.cpp:923`.
- o mapa de chars da view so e podado em `rename`
  (`src/engine/view.cpp:401-404`), nunca em drop/keep/collapse/contract/varlist.

Ou seja: o `save` esta correto, o `collect`/`use` esta a replicar chars de
colunas que ja nao existem no resultado.

## Correcao recomendada

### 1. Guard na ado (P0, sem rebuild, corrige tambem ficheiros ja gravados)

AVISO CRITICO sobre o primitivo: NAO usar `st_varindex()`. Verificado ao vivo,
`st_varindex("nome_inexistente")` ABORTA com rc 3500 ("invalid Stata variable
name"); usar o ingles equivalente trocaria o rc 3300 pelo rc 3500. Usar
`_st_varindex()` (variante com underscore, nao-abortante), que devolve `.` para
variavel ausente e NAO faz abreviacao. Alem disso, `_st_varindex("_dta")` e `.`,
por isso o caso `tgt == "_dta"` TEM de continuar a ser tratado explicitamente,
senao perde-se todas as chars/notas de dataset.

Substituir o ramo `char` em `src/ado/p/parqit.ado:2539-2549` por:

```mata
else if (f[1] == "char") {
    tgt   = _parqit_unhex(f[2])
    cname = _parqit_unhex(f[3])
    /* gate de legalidade (inalterado): um nome ilegal abortaria st_global */
    if (st_isname(cname) & (tgt == "_dta" | st_isname(tgt))) {
        /* gate de existencia (novo): uma projecao (use<varlist>/contract/
         * collapse/keep/drop/reshape/...) pode remover a variavel que tinha a
         * char ou a nota. Aplicar so a _dta ou a uma variavel que sobreviva no
         * resultado encenado. _st_varindex devolve . quando ausente;
         * st_varindex ABORTA rc 3500 num nome ausente, e st_varindex("_dta")
         * e . -- por isso o ramo _dta tem de ficar. */
        if (tgt == "_dta" | _st_varindex(tgt) < .)
            st_global(tgt + "[" + cname + "]", _parqit_unhex(f[4]))
        else
            printf("note: dropping characteristic %s[%s] (variable not in result)\n",
                   tgt, cname)
    }
    else
        printf("note: skipping characteristic %s[%s] (invalid name)\n", tgt, cname)
}
```

Notas:

- `_st_varindex` so e chamado quando `st_isname(tgt)` e verdadeiro, por isso
  nunca recebe um nome invalido.
- emite NOTA (nao silencio), em linha com os ramos irmaos em 2513/2530/2532/2547.
- a decoracao corre dentro da staged frame
  (`src/ado/p/parqit.ado:306-313` e analogos), por isso `_st_varindex` ve as
  colunas do resultado -- contexto correto.

### 2. Filtro no plugin (P1, raiz, espelha o save)

Em `src/plugin/plugin_io.cpp:519-527` (ou ao montar `ctx.meta.chars` em
`src/plugin/plugin_view.cpp:920-923`), emitir um registo `char` apenas quando
`tgt == "_dta"` ou `tgt` pertence as colunas vivas, exatamente como o filtro
`live` do save em `src/plugin/plugin_view.cpp:311-318`:

```cpp
if (ctx.meta.present && ctx.meta.chars.is_object()) {
    std::set<std::string> live;                 // colunas sobreviventes
    for (const auto &p : ctx.active) live.insert(p.stata_name);
    for (const auto &tgt : ctx.meta.chars.items()) {
        if (tgt.key() != "_dta" && !live.count(tgt.key())) continue;  // novo filtro
        if (!tgt.value().is_object()) continue;
        for (const auto &c : tgt.value().items())
            if (c.value().is_string())
                w.rec("char", {}, {tgt.key(), c.key(), c.value().get<std::string>()});
    }
}
```

Fazer 1 + 2 (guard na ado + filtro no plugin) e defesa em profundidade: a ado
protege ficheiros ja gravados e ficheiros estrangeiros; o plugin deixa de emitir
registos orfaos. Opcionalmente, podar tambem `chars_` na view quando colunas saem
(`src/engine/view.cpp`, junto a drop/keep/collapse/contract), nao so em rename.

## Testes de regressao a adicionar ao upstream

Adicionar um teste no `tests/verify_suite/` que use uma fixture que RETEM uma char
e uma nota numa coluna que sera removida (as fixtures `*_no_var_chars` atuais
mascaram o bug). Sugestao de ficheiro: `tests/verify_suite/v_char_projection.do`.

```stata
clear all
set more off
set varabbrev off
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'

local fails 0
tempfile f

clear
set obs 6
gen keepme = _n
gen gone   = _n * 10
gen grp    = mod(_n, 2)
gen noted  = _n
char keepme[source] "survivor"
char gone[source]   "to-be-dropped"
note noted: a variable note
note: a dataset note
parqit save `"`f'.parquet"', replace data

* Caso 1 (headline): use de subconjunto exclui a coluna com char e a com nota
clear
capture noisily parqit use keepme grp using `"`f'.parquet"', clear
if (_rc) {
    di as err "FAIL c1: subset use aborted (rc=`=_rc')"
    local ++fails
}
else {
    capture assert `"`: char keepme[source]'"' == "survivor"
    if (_rc) { di as err "FAIL c1: survivor char lost"; local ++fails }
}

* Caso 2: contract remove a coluna com char
clear
parqit use using `"`f'.parquet"'
parqit contract grp, freq(freq)
capture noisily parqit collect, clear
if (_rc) { di as err "FAIL c2: contract+collect aborted (rc=`=_rc')"; local ++fails }
else {
    capture assert _N == 2 & c(k) == 2
    if (_rc) { di as err "FAIL c2: wrong shape"; local ++fails }
}
capture parqit close _all

* Caso 3: collapse remove a coluna com char
clear
parqit use using `"`f'.parquet"'
parqit collapse (mean) mk=keepme, by(grp)
capture noisily parqit collect, clear
if (_rc) { di as err "FAIL c3: collapse+collect aborted (rc=`=_rc')"; local ++fails }
capture parqit close _all

* Caso 4: notas de variavel e de dataset ainda fazem round-trip no use integral
clear
parqit use using `"`f'.parquet"', clear
capture assert `"`: char _dta[note1]'"' != ""
if (_rc) { di as err "FAIL c4: _dta note lost"; local ++fails }
capture assert `"`: char noted[note1]'"' != ""
if (_rc) { di as err "FAIL c4: variable note lost"; local ++fails }

* Caso 5: rename NAO deve regredir (char segue a coluna renomeada)
clear
parqit use using `"`f'.parquet"'
parqit rename keepme kept
capture noisily parqit collect, clear
if (_rc) { di as err "FAIL c5: rename+collect aborted (rc=`=_rc')"; local ++fails }
else {
    capture assert `"`: char kept[source]'"' == "survivor"
    if (_rc) { di as err "FAIL c5: char did not follow rename"; local ++fails }
}
capture parqit close _all

if (`fails' == 0) di as result "VERDICT(v_char_projection): PASS"
else              di as error  "VERDICT(v_char_projection): FAIL (`fails')"
exit cond(`fails' == 0, 0, 459)
```

Se o runner exigir enumeracao manual da suite, registar o novo teste na lista.

## Validacao esperada

```bash
cmake --build build/dev --target parqit_plugin parqit_tests -j
ctest --preset dev --output-on-failure
STATA=stata-mp BUILD_DIR="$PWD/build/dev" bash tests/run_stata.sh v_char_projection
STATA=stata-mp BUILD_DIR="$PWD/build/dev" bash tests/run_stata.sh
```

No Windows, validar tambem com o test-pack externo (depois de corrigido o
problema de portabilidade do `set tempdir`, ver o relatorio do test-pack):

```powershell
& "C:\Program Files\StataNow19\StataMP-64.exe" /e do `
  "C:\Users\mangelo\Dropbox\1.miguel\Build_Ados\PARQIT\tests\run_all_parqit_tests.do" `
  "C:\Users\mangelo\Dropbox\1.miguel\Build_Ados\PARQIT"
```

Resultado esperado: o test-pack passa sem a falha `contract drops char-bearing
variables without internal metadata error`.

## NAO confundir com o caso `partition_by` (Windows)

Houve uma falha anterior ao escrever `partition_by()` numa pasta Dropbox/Windows
(`could not move temporary partition tree ... Access is denied`). O test-pack
contornou-a escrevendo para um `tempfile`. CORRECAO DE ENQUADRAMENTO face a versao
anterior deste relatorio:

- NAO e o caso ingenuo "rename sobre diretorio existente". O codigo move a arvore
  antiga para o lado (`dest -> dest.parqit_old`) ANTES do rename final
  (`src/plugin/plugin_io.cpp:751-778`, aside na 757, swap na 767), por isso no
  momento da falha o destino ja nao existe.
- A falha e o hazard de "handle aberto na arvore acabada de escrever": DuckDB,
  antivirus on-access, Windows Search Indexer ou cloud-sync com handles abertos
  nos ficheiros novos. E uma fragilidade REAL de Windows, reproduzivel sem
  Dropbox, porque `fs::rename(tmpdest, dest)` e feito uma unica vez, sem retry
  (`src/plugin/plugin_io.cpp:686-709` no caminho de ficheiro unico,
  `:751-778` no particionado).

Recomendacao: tratar como item separado, prioridade P2, nao como o bug principal.
Mitigacao sugerida: retry com backoff a volta dos `fs::rename`, e/ou fallback
`fs::copy` recursivo + `remove_all` no ramo `_WIN32`. A nota do help
"renamed atomically" (`src/ado/p/parqit.sthlp:240-243`) e aspiracional em Windows.

## Caso pre-existente separado (documentar, P2)

Chars/notas de uma coluna cujo NOME DE ORIGEM e ilegal (ex.: `"my var"` saneado
para `my_var`) sao hoje descartadas (`st_isname(tgt)` falso), porque a char fica
chaveada pelo nome ilegal. Isto NAO e causado pela correcao acima e nao e
agravado por ela, mas contraria parcialmente a promessa de round-trip. Decidir:
migrar a char para o `stata_name` saneado, ou registar a limitacao em
`ASSUMPTIONS.md` para o help nao sobre-prometer.

## Entregaveis esperados do Claude

1. Patch minimo em `src/ado/p/parqit.ado` (ramo `char`, com `_st_varindex` e o
   caso `_dta` preservado).
2. Filtro de chars no emitter do plugin (`src/plugin/plugin_io.cpp` e/ou
   `src/plugin/plugin_view.cpp`), espelhando o filtro do save.
3. Teste novo `tests/verify_suite/v_char_projection.do` (5 casos acima) e
   atualizacao do runner se necessario.
4. Validacao com a suite Stata relevante e, se possivel, suite completa + ctest.
5. Nota no `CHANGELOG.md`. Confirmar que NAO e duplicado: META-1 (0.1.4) foi um
   rc 3300 por truncagem de char longa; META-2 (0.1.5) foi chars a seguir
   `rename`; nenhum cobre char/nota de uma variavel removida por projecao.
6. (Opcional, P2) item separado para o retry de rename em Windows e para o caso
   sanitize-rename.
