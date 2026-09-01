# Auditoria adversarial holística do parqit — 2026-09-01

Auditor: Claude (Fable 5.1), sessão autónoma de 2026-09-01.
Objeto: `parqit` v0.1.29 (commit `422d18e`, working tree com as alterações
não commitadas de 2026-08-28: exemplos SSC, README, help, t13/t14).
Plugin auditado: `build/dev/parqit.plugin` recompilado nesta sessão a partir da
árvore actual (preset `dev`, DuckDB 1.5.3 embebido). Stata: StataNow 19.5 MP,
Linux x86_64. Oráculos independentes: Stata nativo, pyarrow 24.0, DuckDB CLI.

Este documento tem três partes: (A) o relatório da auditoria, com evidência
executada e causa raiz no código; (B) o plano de execução para o agente
implementador, com especificação por tarefa e definição de concluído; (C) o
registo da implementação de T1–T4 feita na mesma sessão.

---

## A. Relatório

### A.1 Veredito

**GO condicional.** O núcleo de integridade de dados (tipos, metadados,
atomicidade, nomes, concorrência) confirmou-se sólido: 98/98 testes Stata,
98 casos / 3.653 asserções C++, e ≈480 verificações novas contra oráculos
independentes sem qualquer perda silenciosa nos caminhos que as auditorias
anteriores já pinavam. Foram, porém, encontrados **dois defeitos S1 de
semântica silenciosa** que devem ser corrigidos antes da próxima etiqueta
(v0.1.30) e um S2 já conhecido (o `describe` de árvores Hive) que continua por
corrigir:

| ID | Sev. | Achado (resumo) | Estado |
|---|---|---|---|
| F1 | S1 | `partition_by()` sobre chave string: o valor literal `"NULL"` é lido de volta como missing (`""`), sem aviso; um valor `"NULL"`/`__HIVE_DEFAULT_PARTITION__` numa árvore estrangeira também colapsa para `""`. | novo, reproduzido; **corrigido** (parte C, T2) |
| F2 | S1 | Comparações de uma coluna `float` com literal decimal não representável (`x == 0.1`, `x > 0.1`, `inrange`, `inlist`, `cond`) avaliam em precisão simples (o motor converte o literal para FLOAT); o Stata nativo avalia em double. 9 de 22 filtros divergem. | novo, reproduzido; **corrigido** (parte C, T1) |
| F3 | S2 | `parqit describe <dir Hive>` e `describe <glob sobre árvore>`: coluna "parquet type" e `r(type_i)` emparelhadas por posição com nomes na ordem do manifesto — tipos deslocados a partir da coluna de partição. | conhecido (2026-08-28), confirmado; **corrigido** (parte C, T3) |
| F4 | S2 | Fonte CSV com cabeçalhos duplicados ou que diferem só por maiúsculas (`a,a,b,A`) carrega `a a_1 b A_2` sem nota nem `src_name`. Charter §6.10. | novo; **corrigido** (parte C, T4) |
| F5 | S3 | `collapse (count) n = s` (fonte string) herda o formato `%9s` do source para a coluna numérica; todo o `collect` imprime "skipping display format %9s". | novo |
| F6 | S3 | Formato de data antigo `%d`/`%-d` (sinónimo documentado de `%td`) não é classificado como data: escrito como INT32 cru, não `DATE` (interop com terceiros). Round-trip parqit→parqit intacto. | novo |
| F7 | S3 | `mod(x,y)` com módulo não inteiro (`mod(7, 0.00001)`) devolve `-8.9e-16` onde o nativo devolve `9.99999999911e-06` (fórmula `x - y*floor(x/y)` vs `trunc` com ajuste de sinal). | novo |
| F8 | S3 | `ustrupper`/`ustrlower` usam o case mapping simples do DuckDB: `ustrupper("straße")` = `STRAẞE` (nativo `STRASSE`), `ustrlower("İ")` = `i` (nativo `i̇`). | novo (doc/impl.) |
| F9 | S3 | `regexm()` com newline embebido: `regexm(s,"^line1$")` e `regexm(s,"1.l")` são 1 no Stata e 0 no RE2 (sem modo multilinha; `.` não casa `\n`). Não coberto pela nota de dialecto da ajuda. | novo (doc) |
| F10 | S3 | `parqit drop in f/l` não existe; a mensagem é "variable in not found in the view". | novo (usabilidade) |
| F11 | S3 | `float()` não está implementada — é o idioma nativo para comparar floats (`x == float(0.1)`) e torna-se necessária com F2. | novo; **implementada** (parte C, T1) |
| F12 | S3 | `parqit tabulate` não mostra value labels (o `tabulate` nativo mostra); sem opção `nolabel`. | novo (feature gap) |
| F13 | S4 | `duplicates list`: uma célula string com TAB desalinha as colunas (TAB é o separador interno da linha). | novo (cosmético) |
| F14 | S4 | `+x` (mais unário) é aceite; o Stata rejeita (r(198)). `-1e308` (magnitude negativa acima de `mindouble`) fica missing no parqit; o Stata guarda-o. | novo (nota) |
| F15 | S4 | Harness: `v67`/`v70` fazem `grep` a frases inteiras no log; com um `TMPDIR` muito longo (>150 bytes) a mensagem ultrapassa `linesize 255`, quebra de linha e o teste "não termina". Reexecutados com `/tmp` curto: PASS. | novo (teste) |
| F16 | S3 | `collapse (median/p##)` e `tabstat` percentis materializam `list_sort(list(x))` por grupo — limitado pela RAM em grupos enormes; `summarize, detail` já usa o caminho CTAS/ordem. | oportunidade (perf) |

Nenhum achado é S0 (corrupção silenciosa num caminho principal). F1 e F2 são
S1 porque produzem resultados errados **sem aviso** em situações plausíveis
(uma chave string com o texto `NULL` em dados administrativos; qualquer
variável `float` — o tipo numérico por omissão do Stata — filtrada por um
literal decimal como `0.1`).

### A.2 Método

1. **Leitura integral do código** (≈20 k linhas): `src/engine/*` (exprtrans,
   view, typemap, sanitize, session, parquet_footer, legacy_encoding,
   request/hexcodec), `src/plugin/*` (parqit_plugin, plugin_io, plugin_view),
   `src/ado/p/parqit.ado` (dispatch, Mata wire, decoradores), `parqit.sthlp`
   (secções de expressões, materialisers, limitações), `parqit.pkg`,
   `stata.toc`, `.github/workflows/build.yml`, `tests/run_stata.sh`,
   `release_lint.sh`, e os relatórios/triagem da auditoria de 2026-08-22 para
   não repetir cobertura.
2. **Baseline executada nesta sessão:** `cmake --build build/dev` (0 avisos),
   `parqit_tests` 98/98 (3.653 asserções), `bash tests/run_stata.sh` completa:
   96 PASS + 2 "DID NOT FINISH" (`v67`, `v70`) que reexecutados com `TMPDIR`
   curto dão PASS (F15). `bash tests/release_lint.sh`: OK.
3. **Sondagens adversariais novas** (14 do-files, ≈480 verificações), cada uma
   com oráculo independente (Stata nativo por `cf`/`assert`/contagens,
   pyarrow para o ficheiro escrito, DuckDB CLI para hipóteses). Ficheiros e
   logs em `local/audit_2026-09-01/probes/` (git-ignored):

| Sondagem | Área | Resultado |
|---|---|---|
| p1/p1b_expr | 355 expressões (numéricas, string, datas, literais) vs `gen` nativo com `statamissing on` | 207 OK; divergências explicadas (F7, F8, F9, F14) ou artefactos documentados (arredondamento de `%td` fraccionário no save, `.z_` acima de maxdouble, `substr` em byte de multibyte → U+FFFD, `ty()` extensão) |
| p2_partition | `partition_by` com chave string (`""`, `"NULL"`, `a=b`, `a/b`, `x y`, `é`), `%td`, `%tm`, `byte`, `long`, `double` com missings; eager, lazy, describe | numéricas/data exactas (`cf`); string: F1; describe: F3 |
| p3_describe | describe de ficheiro plano, árvore Hive, glob sobre árvore, forma view | F3 confirmado nas formas dir e glob; view OK |
| p4_sample | reprodutibilidade de `sample, seed()` com 8 e 1 threads; tamanhos vs nativo | OK (mesma assinatura em 3 corridas e entre threads; 50.000 = nativo; `count` exacto) |
| p5_csv | cabeçalhos duplicados/case, coluna vazia, tokens NA/NULL, newline entre aspas | F4; restantes OK (NA/NULL ficam strings, nunca nulls silenciosos) |
| p7_misc | count em string, egen total tipo, `%d`/`%tg`/`%9,2f`/`%-td…`, rename só por caixa, vista vazia, ordenação Unicode e `gsort` com missing vs nativo, tamanho de `sample` em N=45, tabulate com labels, TAB em strings, comparação float, nomes reservados `_n if strL in` em Parquet estrangeiro | F5, F6, F12, F13, F2(pista); resto OK (ordens iguais ao nativo, nomes reservados → `__n _if _strL _in` com `src_name`) |
| p8_frames | `preserve`/`restore` em volta de `use, clear` e `collect, clear`; frame corrente não-default; frlink de entrada | OK |
| p9_meta_verbs | labels com aspas/`$`/backtick, notas com `|`, chars, value labels com chaves `-1 .a .b`, label órfão, dataset label com emoji, `sortedby` após view save; merge m:1 (chaves string com `""`, keepusing wildcard, keep(match master), variável comum) vs nativo; append (ordem de colunas diferente, colunas ausentes, larguras, conflito de value label); reshape long/wide com j string; pivot | tudo `cf` exacto |
| p10_perf | 3 M linhas × 7: save 0,84 s, use 0,32 s, collect 0,32 s, collapse mean/sd/count por firma×ano 0,26 s, mediana/p90 0,35 s, summarize detail 0,33 s, sort+_n+filtro+save 0,31 s, merge m:1 0,35 s, tabulate/codebook/duplicates 0,28 s, save partition_by 1,4 s | sem comportamento patológico |
| p11_float | 22 filtros sobre coluna float vs `count if` nativo | F2 (9 divergem) + F11 |
| p12_hive_null | árvore escrita por pyarrow com `None`, `"NULL"`, `""` | F1 lado da leitura |
| p13_usability | `drop in`, `summarize if`, `egen … if`, `sample … if`, `tabulate, sort`, `collapse, cw` | F10; os restantes são recusas claras (r(101)/r(198)) |

### A.3 Achados em detalhe

#### F1 — S1 — chave de partição string: `"NULL"` lê-se como missing

**Evidência** (`p2_partition`, `p12_hive_null`):

```
. parqit save by_k, replace partition_by(k)     // k contém "a" "" "a=b" "a/b" "é" "x y" "A" "NULL"
. parqit use by_k, clear
. cf _all using ref
               k: 1 mismatch
                  obs 9.  in master; NULL in using
```

Directórios escritos: `k=a k= k=a%3Db k=a%2Fb k=%C3%A9 k=x%20y k=A k=NULL`.
Os valores com `=`, `/`, espaço e Unicode voltam intactos (URL-encoding do
motor) e a string vazia também (`k=`). O literal `NULL` não: o leitor Hive do
DuckDB mapeia o directório `k=NULL` para SQL NULL, que a fronteira do parqit
normaliza para `""`. Numa árvore escrita por pyarrow (`p12`), `None`,
`"NULL"` e `""` chegam os três como `""`. Para chaves numéricas o mesmo
directório `n=NULL` significa missing e o round-trip é exacto (p2: `d`, `m`,
`b`, `n`, `f` com `cf` rc 0).

**Causa raiz.** `copy_out_parquet` (`src/plugin/plugin_io.cpp:1867-1914`)
delega a escrita da árvore ao `COPY … PARTITION_BY` do motor e verifica apenas
a contagem de linhas (`verify`), nunca o conjunto de valores das chaves;
`cmd_save_data` (`:3649-3665`) e `cmd_view_save`
(`src/plugin/plugin_view.cpp:1568-1592`) validam só que a chave existe e que
sobra uma coluna. No lado da leitura `hive_boundary_override`
(`plugin_io.cpp:1520-1550`) só restaura tipos numéricos.

**Impacto.** Perda silenciosa de um valor legítimo de uma variável de
partição (`"NULL"` é comum em exportações de bases de dados); a ajuda não
documenta a restrição.

**Correcção proposta.** (a) No ramo particionado de `copy_out_parquet`, após
o COPY e antes de publicar, comparar o conjunto de valores distintos de cada
chave de partição na origem (`SELECT DISTINCT k FROM (<query>)`, com `''`
e NULL separados) com o lido de volta da árvore encenada
(`read_parquet(<staged>/**/*.parquet, hive_partitioning=true)`); qualquer
diferença (valor ausente, colapsado ou alterado) descarta a encenação e
falha alto com a mensagem "partition value X of k cannot be written as a
directory name; write a single file or recode it". Isto cobre `NULL`,
`__HIVE_DEFAULT_PARTITION__` e qualquer futura assimetria de codificação do
motor, sem enumerar tokens. (b) Na leitura de árvores estrangeiras, quando a
coluna de partição é VARCHAR e algum directório é `k=NULL` ou
`k=__HIVE_DEFAULT_PARTITION__`, imprimir `note:` a dizer que esse valor foi
lido como missing (o parqit não pode distinguir). (c) Ajuda + README
(Limitations, `partition_by()`), CHANGELOG, ASSUMPTIONS.

**Teste.** `tests/verify_suite/v78_partition_string_keys.do`: chave string com
`""`, `"NULL"`, `"__HIVE_DEFAULT_PARTITION__"`, `a=b`, `a/b`, `x y`, `é`,
`"A"`/`"a"`; save eager e view; esperar rc≠0 com a mensagem nova para os dois
tokens e round-trip `cf` exacto para os restantes; árvore pyarrow com `None`
→ nota presente; oráculo pyarrow para a lista de directórios.

#### F2 — S1 — comparações float × literal decimal em precisão simples

**Evidência** (`p11_float`, coluna `float x` = 0.1, 0.3, 1.1, 2.5; `d` = double
com os mesmos valores):

| filtro | nativo `count if` | parqit `count if` |
|---|---|---|
| `x == 0.1` | 0 | 1 |
| `x > 0.1` | 4 | 3 |
| `x <= 0.3` | 1 | 2 |
| `x != 1.1` | 4 | 3 |
| `x > 1.1` | 2 | 1 |
| `x == 0.3` | 0 | 1 |
| `inrange(x, 0.1, 0.3)` | 1 | 2 |
| `inlist(x, 0.1, 1.1)` | 0 | 2 |
| `cond(x > 0.1, 1, 0)` (soma) | 4 | 3 |
| `d == 0.1`, `d > 0.1`, `x*1 == 0.1`, `x+0 > 0.1`, `x == 2.5`, `x < 1.1` | iguais | iguais |

`p13`: `parqit gen double xd = x` seguido de `count if xd == 0.3` dá 0 (correcto),
`count if x == 0.3` dá 1.

**Causa raiz.** O tradutor emite o literal como texto decimal
(`exprtrans.cpp:480-501`, `dtoa`, DATA-003) e a comparação sem cast
(`relational()`, `:393-472`; `inrange` `:1003-1022`); ao comparar uma coluna
FLOAT com um literal DECIMAL/DOUBLE o binder do DuckDB converte o literal para
FLOAT, i.e. compara em precisão simples. Os operadores aritméticos já fazem
`CAST(... AS DOUBLE)` (INF-1), por isso `x*1 == 0.1` está certo. A ajuda promete
"Expressions compute in double precision, exactly like Stata's expression
evaluator".

**Impacto.** `keep if`, `drop if`, `count if`, `gen`, `replace … if` e `egen`
sobre qualquer variável `float` (o tipo por omissão do `gen` nativo) com
literais como `0.1`, `0.3`, `1.1` seleccionam linhas diferentes das do Stata,
sem aviso; `merge`/`collapse` não são afectados (não há literal).

**Correcção proposta** (contida, sem perder pushdown): em `relational()` e
no caminho numérico de `inrange()`, quando um operando é um literal `Tok::Num`
(ou, mais simples e uniforme, sempre que ambos são numéricos), envolver o
literal em `CAST(<lit> AS DOUBLE)`; o motor passa então a promover o FLOAT para
DOUBLE (regra de alargamento), que é exactamente a semântica do Stata. Não
forçar DOUBLE nos literais de `gen`/`replace` fora de comparações (DATA-003
depende do tipo inteiro do literal para manter byte/int/long). Alternativa mais
radical — `CAST(ref AS DOUBLE)` para colunas FLOAT em `boundary_for`
(`plugin_view.cpp:284`) — muda o tipo materializado de ficheiros estrangeiros
e obriga a marcar `meta_type="float"`; só se a primeira não bastar.
Implementar também F11 (`float()`).

**Teste.** Unit em `test_exprtrans.cpp` (SQL gerado contém o cast); verify
`v79_float_literal_compare.do` com os 22 filtros de `p11` e oráculo nativo,
mais `gen`/`replace … if` e `egen … , by()`; correr `v42`/`v61`/`v68`/`v75`
para garantir não-regressão.

#### F3 — S2 — `describe` de árvores Hive/globs: tipos deslocados

**Evidência** (`p3_describe`, ficheiro plano vs árvore `partition_by(year)`):

```
FLAT: id INTEGER/long  year SMALLINT/int  age FLOAT/float  wage DOUBLE/double  name VARCHAR/str8  flag TINYINT/byte
TREE: id INTEGER/long  year FLOAT/int     age DOUBLE/float wage VARCHAR/double name TINYINT/str8  flag BIGINT/byte
GLOB (tree/**/*.parquet): idem TREE
```

A forma view (`parqit use using tree` + `parqit describe`) está correcta.

**Causa raiz.** `cmd_describe` (`plugin_io.cpp:2953-2964`) emite os registos
`dtype` pela ordem do scan (chave de partição em último) e
`_parqit_resp_describe` (`parqit.ado:3339-3385`) emparelha `dtypes[i]` por
posição com `snames[i]`, que vêm na ordem do manifesto (COLORDER-1 repõe a
chave na posição original). O `dtype` já transporta o nome; o zip é
posicional — exactamente a classe de risco que o charter proíbe.

**Correcção.** Em `_parqit_resp_describe` construir um mapa nome→tipo a
partir dos registos `dtype` e procurar pelo nome de origem do registo `var`
(campo 4, o nome Parquet; para colunas saneadas o nome do scan), caindo em
posição só quando o nome não existe. Em `cmd_describe`, se necessário,
acrescentar ao registo `var` o nome do scan. Corrigir também `r(type_i)`.

**Teste.** `v80_describe_alignment.do`: árvore com chave em posição não final,
glob sobre a árvore, glob de vários ficheiros planos, `relaxed`, ficheiro com
nomes saneados; assert `r(type_i)` == tipo pyarrow por nome.

#### F4 — S2 — CSV: nomes duplicados/só-por-caixa renomeados em silêncio

**Evidência** (`p5_csv`, cabeçalho `a,a,b,A`): `describe` mostra `a a_1 b A_2`,
sem nota; `char list` sem `src_name`; forma lazy idem. Para Parquet o mesmo
caso produz nota, alias e `src_name` (v70).

**Causa raiz.** `plan_columns` (`plugin_io.cpp:791`) só recupera nomes quando
`paths_sql != "[]"`; `source_for` (`:438-444`) marca CSV com `"[]"`, e
`read_csv_auto` já entrega os nomes desduplicados pelo leitor.

**Correcção.** Para fontes CSV obter os nomes verdadeiros do cabeçalho
(`sniff_csv(<file>)` → `Columns`, ou `read_csv(<file>, header=false,
all_varchar=true) LIMIT 1` com o mesmo delimitador detectado) e aplicar a
mesma recuperação (`parquet_names`, notas, `src_name`, alias NAME-CASE-1 na
vista). Se a recuperação não for contida, no mínimo uma nota por coluna
renomeada. Documentar na ajuda ("Input formats").

**Teste.** `v81_csv_header_names.do`: `a,a,b,A`, cabeçalho com espaços e
acentos, cabeçalho vazio; eager+lazy; notas presentes; `src_name` correcto.

#### F5 — S3 — `(count)` de string herda `%9s`

`view.cpp:861-862` copia `scol.fmt` para todos os targets; para `count` de uma
fonte string a coluna numérica recebe `%9s` e cada `collect` imprime
"note: ns: skipping display format %9s". Correcção: `count` → formato do nativo
(`%8.0g`; o nativo recusa strings, o parqit estende — manter a extensão e
documentá-la na ajuda). Teste em `v75`.

#### F6 — S3 — `%d`/`%-d` não classificados como data

`typemap.cpp:40-60` exige `%t?`; o Stata documenta `%d` como sinónimo de
`%td`. `p7`: `long dd %d` é escrito como INT32 21916 (pyarrow), `%tdCCYY-NN-DD`
como `date32`. Round-trip parqit intacto; terceiros vêem inteiros. Correcção:
classificar `%d…`/`%-d…` como `Td` (unit test) e mencionar na tabela de tipos.

#### F7 — S3 — `mod()` com módulo não inteiro

`p1b`: `mod(7, 0.00001)` nativo `9.99999999911182e-06`, parqit `-8.882e-16`
(`exprtrans.cpp:944-958`, `a - b*floor(a/b)` em double). Verificado nesta
sessão que o nativo se comporta como `r = a - b*trunc(a/b)` seguido de
`r + b` quando `r < 0` (o próprio `di 7 - 0.00001*floor(7/0.00001)` no Stata dá
`-8.88e-16`, e `-8.88e-16 + 1e-5` é exactamente o valor que `mod()` devolve);
`(0.3,0.1)`, `(1,0.1)` e `(-5.5,2)` coincidem com essa fórmula. Atenção: o
`fmod()` do DuckDB é `a - b*floor(a/b)` (dá `0.5` para `(-5.5,2)` e
`-8.88e-16` para `(7,1e-5)`), pelo que não serve. Correcção: emitir
`r = a - b*trunc(a/b)` e `CASE WHEN r < 0 THEN r + b ELSE r END`, mantendo
`y <= 0 → .`; oráculo nativo para `(7,1e-5) (1,0.1) (0.3,0.1) (5.5,2) (-5.5,2)
(1e15+0.5,1) (x,2.5) (x,0.3)`.

#### F8 — S3 — case mapping Unicode

`ustrupper("straße")` → `STRAẞE` (nativo `STRASSE`); `ustrlower("İ")` → `i`
(nativo `i̇`). O DuckDB só tem mapeamento simples. Decisão recomendada:
documentar na ajuda (secção de strings) como diferença de dialecto; opcional:
tratar `ß→SS` explicitamente via `replace()` antes do `upper()`.

#### F9 — S3 — `regexm()` e newlines

RE2 sem multilinha: `^`/`$` só nos extremos da string e `.` não casa `\n`; o
`regexm` nativo casa `^line1$` e `1.l` em `"line1\nline2"`. Acrescentar à nota
de dialecto da ajuda (que hoje só fala de `\d \w {n,m}` e não-guloso).

#### F10 — S3 — `parqit drop in`

`_parqit_drop` (`parqit.ado:657-666`) não tem ramo `in`; mensagem enganadora.
Implementar `drop in f/l` (complemento de `keep in` sobre a mesma ordem; via
`row_number()` + `NOT BETWEEN`, validado como `keep in`) ou recusar com
"drop in is not implemented; use keep in". Ajuda e dialog `parqit_filter`.

#### F11 — S3 — `float()`

Acrescentar `float(x)` = `CAST(CAST(x AS FLOAT) AS DOUBLE)` (missing → NULL;
fora de ±1.70e38 → NULL) à lista de funções da ajuda (lint da lista).

#### F12 — S3 — `tabulate` sem value labels

`_parqit_print_tabulate`/`_parqit_print_tab2` (`parqit.ado:3620`, `:3793`) só
formatam números. A vista transporta `vallabs_`; o plugin pode enviar o mapa
valor→texto da variável no cabeçalho `tabh`/`t2h`, e o ado aplica-o (com opção
`nolabel`, como o nativo). Idem `levelsof` mantém valores (nativo também).

#### F13 — S4 — `duplicates list` e TAB

`plugin_view.cpp:3622-3639` separa células com `\t` dentro de um campo hex;
uma célula com TAB parte a linha. Usar `\x1f` como separador ou hex por célula.

#### F14 — S4 — notas de paridade

`+x` aceite (nativo r(198)); `-1e308` (abaixo de `mindouble`, que o Stata ainda
guarda) fica missing pela regra |x| ≥ 2^1023 (INF-1). Documentar; nenhuma acção
de código necessária.

#### F15 — S4 — harness sensível ao comprimento do caminho

`v67` (linha 152) e `v70` (linha 295) procuram frases inteiras no log; com
`TMPDIR` de ~150 bytes a mensagem excede `linesize 255` e quebra. Correcção
barata: `run_stata.sh` cria o `RUNDIR` sob `/tmp` (já o faz por omissão) e
documenta que `TMPDIR` deve ser curto; ou os testes normalizam o log
(`subinstr(txt, char(10)+"> ", "", .)`) antes do `grep`.

#### F16 — S3 (perf) — percentis por `list_sort(list(x))`

`stata_pctile_sql` (`view.cpp:721-733`) materializa a lista completa do grupo
(três vezes no texto SQL) — para `collapse (median)` sem `by()` sobre 10^9
linhas é uma lista de 8 GB em memória. `summarize, detail` já usa CTAS
ordenada + `rowid`. Oportunidade: `quantile_disc` com a regra de rank exacta
já documentada em `plugin_view.cpp` (PERF-DET-1), validada contra o nativo
em `v44`/`v75`. Sem urgência: 3 M linhas × 50 k grupos em 0,35 s.

### A.4 O que se confirmou correcto (para registo)

- **Expressões:** 207 formas numéricas/string/data iguais ao nativo (`round`
  1–2 args, `mod`, potências e precedência `-x^2`/`!x`, `inrange` com limites
  missing, `inlist`, `cond` 3/4 args, `min/max` com missing, `string()` em 36
  valores incl. 1e23/2^53/denormais, `real()` em 20 grafias incl. `1d3`,
  `1_000`, `0x10`, `+5`, `5.`, `-.5`, tab, `strpos` byte-based, `substr`
  posições 0/negativas/fraccionárias, `subinstr`, `regexm` com POSIX classes,
  concatenação, funções de data no domínio e nos limites 01jan0100/31dec9999,
  literais `td tc tm tq th tw`, rejeições `tq(2020q0)`, `th(2020h3)`,
  `tc(…59.9999)`).
- **Partições numéricas/data/byte com missings:** exactas em eager, lazy e
  describe (tipo restaurado do manifesto).
- **`sample`:** reprodutível com `seed()` entre corridas e entre 1/8 threads;
  N = nativo (percentagem e `count`).
- **Metadados:** labels com aspas/`$`/backtick, notas com `|`, chars, value
  labels `-1/.a/.b`, label órfão, dataset label com emoji, `sortedby` — exactos
  em eager e após view save com rename/gen.
- **Verbos de duas tabelas e reshape/pivot:** `cf` exacto vs nativo.
- **Ordenação:** strings Unicode/vazias e `gsort -x` com missing iguais ao
  nativo; `_n` coerente.
- **Nomes reservados** (`_n`, `if`, `in`, `strL`) num Parquet estrangeiro:
  saneados com nota e `src_name` em ambos os caminhos.
- **`preserve`/`restore`, frames não-default, frlink de entrada:** o swap
  atómico não interfere.
- **CSV:** `NA`/`NULL` ficam strings (nunca nulls silenciosos); newline entre
  aspas preservado.
- **Performance:** sem regressão perceptível (tabela em A.2).
- **Empacotamento:** `parqit.pkg` mapeia os quatro binários (incl. aliases
  `OSX.X8664`/`OSX.ARM64`), `Distribution-Date` coerente, `release_lint` OK,
  CI com AlmaLinux 8 e libstdc++ estática.

### A.5 Observações não bloqueantes

- `plugin_io.cpp` (4.4 k linhas) concentra I/O, planeamento, escrita Arrow e
  copysource; uma separação (`plan.cpp`, `writer.cpp`) facilitaria auditorias
  futuras. Sem impacto funcional.
- O caminho eager reordena com `sort …, stable` no frame encenado sempre que
  o manifesto tem `sortedby`, mesmo com os dados a chegar já ordenados;
  medir antes de optimizar.
- `c(pid)` vem vazio nesta build do StataNow: o nonce de `copysource` fica
  `_<seq>`; continua único por sessão, mas convém registar em ASSUMPTIONS.

---

## B. Plano de execução para o agente implementador

### B.1 Regras de base (obrigatórias)

1. Ler primeiro `CLAUDE.md`, `AGENTS.md` e este relatório. O brief
   `parqit_build_prompt.md` é autoritativo; a regra de não-regressão de
   `AGENTS.md` aplica-se a cada tarefa.
2. Correcção primeiro, performance depois. Nenhuma tarefa pode remover uma
   funcionalidade, reduzir precisão ou tornar um erro silencioso.
3. Cada tarefa produz: código + teste pinado (verify `v78+` ou unit) + entradas
   em `CHANGELOG.md` (secção `[Unreleased]`), `ASSUMPTIONS.md` (nova decisão
   numerada, continuar após #102) e `parqit.sthlp`/`README.md` quando muda o
   contrato visível. Um só bump de versão no fim (v0.1.30, data do dia;
   `release_lint.sh` verifica todas as superfícies).
4. Não tocar em `ssc_submission/`, `local/`, `Conference_Presentation/`, nem
   nos ficheiros já modificados no working tree por outra razão
   (exemplos SSC, t13/t14), excepto onde a tarefa o exija explicitamente.
5. Validar sempre com: `cmake --build build/dev -j` → `./build/dev/parqit_tests`
   → `bash tests/run_stata.sh <novo teste>` → `bash tests/run_stata.sh`
   completo (com `TMPDIR` curto, ver F15) → `bash tests/release_lint.sh`.
   Reportar o output real; um FAIL não se mascara.
6. Repros mínimos: os do-files em `local/audit_2026-09-01/probes/` reproduzem
   cada achado (correr com `do <ficheiro> <repo> <plugin>`); um repro
   reduzido de cada defeito corrigido vai para `audit_repro/`.
7. Commits atómicos por tarefa, Conventional Commits, sem commit enquanto a
   build ou a suite estiverem vermelhas.

### B.2 Tarefas, por prioridade

#### T1 (S1) — Comparações float em double (F2) + `float()` (F11)

- Ficheiros: `src/engine/exprtrans.cpp` (`relational()`, `inrange` numérico,
  `call()`), `tests/unit/test_exprtrans.cpp`, `tests/verify_suite/v79_float_literal_compare.do`,
  `parqit.sthlp` (lista de funções + parágrafo "Expressions compute in double
  precision"), `README.md` (tabela de funções se existir), CHANGELOG, ASSUMPTIONS.
- Especificação: em `relational()`, quando ambos os lados são numéricos,
  envolver cada operando que seja literal (`Tok::Num`, incl. o literal
  canónico `dtoa`) em `CAST(<lit> AS DOUBLE)`; se um teste unitário mostrar
  que o motor ainda compara em FLOAT quando o outro lado é coluna FLOAT e o
  literal é inteiro (`x == 16777217`), aplicar o cast a todos os literais
  numéricos em contexto relacional. Fazer o mesmo no caminho numérico de
  `inrange()`; `inlist` e `cond` passam por `relational()`. Não alterar a
  emissão de literais em atribuições (`gen`/`replace`) fora de comparações
  (DATA-003). Acrescentar `float(x)`: um argumento numérico, SQL
  `CASE WHEN abs(CAST(x AS DOUBLE)) > 1.7014117331926443e38 THEN NULL ELSE CAST(CAST(x AS FLOAT) AS DOUBLE) END`
  (NULL passa), e o nome à lista sincronizada da ajuda (o lint verifica).
- Testes: unit (SQL gerado; `float()`); `v79` reproduz `p11_float.do`
  (os 22 filtros, `gen double y = x > 0.1`, `replace … if x == 0.1`,
  `egen … if`-equivalente por `keep if`) com oráculo nativo `count if`; correr
  `v42 v61 v68 v75 t13 t14`.
- Concluído quando: 22/22 iguais ao nativo; `x == float(0.1)` = 1; suites
  verdes; ajuda actualizada.

#### T2 (S1) — `partition_by` com chaves string (F1)

- Ficheiros: `src/plugin/plugin_io.cpp` (`copy_out_parquet`, ramo
  particionado; `cmd_save_data`; `cmd_save_data_direct`),
  `src/plugin/plugin_view.cpp` (`cmd_view_save`, leitura de árvores em
  `cmd_view_open`/`prepare_using` via `plan_columns`), `plugin_io.cpp`
  (`plan_columns`: nota de leitura), `v78_partition_string_keys.do`, ajuda
  (Materialisers/`partition_by()`, Limitations), README (Limitations),
  CHANGELOG, ASSUMPTIONS.
- Especificação: (a) verificação de round-trip das chaves antes de publicar:
  para cada chave de partição, `SELECT DISTINCT <k> FROM (<query>)` na origem
  e `SELECT DISTINCT <k> FROM read_parquet(<staged glob>, hive_partitioning=true)`
  no destino encenado; comparar como multiconjuntos de valores com NULL e
  `''` distintos (para chaves numéricas comparar após cast ao tipo de origem);
  diferença → descartar encenação, rc 198 e mensagem que nomeia a chave e o
  primeiro valor não reproduzível; (b) na leitura (`plan_columns`, quando
  `ctx->hive_columns` contém uma coluna VARCHAR), detectar directórios
  `k=NULL` e `k=__HIVE_DEFAULT_PARTITION__` (via `parquet_file_metadata`
  file_name) e emitir `note:` "partition value NULL of k read as missing";
  (c) documentar: um valor de chave string igual a `NULL` ou
  `__HIVE_DEFAULT_PARTITION__` não pode ser escrito como partição (recusa) e
  numa árvore estrangeira lê-se como missing (nota).
- Testes: `v78` conforme A.3/F1; pyarrow como oráculo da lista de
  directórios e do conteúdo; correr `v74`, `v76`, `t11`, `t14`.
- Concluído quando: tokens recusados com a mensagem, restantes valores
  `cf` exactos eager/lazy/view-save, nota presente na árvore pyarrow.

#### T3 (S2) — `describe` alinhado por nome (F3)

- Ficheiros: `src/ado/p/parqit.ado` (`_parqit_resp_describe`),
  `src/plugin/plugin_io.cpp` (`cmd_describe`, `write_var_records` se for
  preciso transportar o nome do scan), `v80_describe_alignment.do`,
  `audit_repro/repro_describe_hive_type_shift.do`, CHANGELOG.
- Especificação: registos `dtype` → mapa nome→tipo; para cada registo `var`
  procurar por (i) nome de origem (campo 4), (ii) nome do scan (novo campo ou
  `parquet_names` invertido), (iii) posição só se único; `r(type_i)` e a
  tabela impressa usam o resultado. Manter a saída para ficheiros planos
  byte-igual (t15 e dialogs usam `describe`).
- Concluído quando: `v80` iguala `r(type_i)` ao tipo pyarrow por nome em
  árvore, glob sobre árvore, glob plano, `relaxed`, nomes saneados.

#### T4 (S2) — Nomes de cabeçalho CSV (F4)

- Ficheiros: `src/plugin/plugin_io.cpp` (`source_for` para guardar o caminho
  CSV; `plan_columns` ramo CSV), `src/plugin/plugin_view.cpp`
  (`cmd_view_open` já usa `plan_columns`), `v81_csv_header_names.do`, ajuda
  ("Input formats"), CHANGELOG, ASSUMPTIONS.
- Especificação: `sniff_csv(<path>)` devolve o delimitador, a aspa e
  `HasHeader`, mas o seu campo `Columns` já vem desduplicado (`a, a_1, b, A_2`
  — verificado), pelo que os nomes brutos têm de vir da primeira linha lida
  como dados: `SELECT * FROM read_csv(<path>, header=false, all_varchar=true,
  delim=<Delimiter>, quote=<Quote>) LIMIT 1` (verificado: devolve `a a b A`).
  Alinhar posicionalmente com os nomes do scan (mesma contagem; só quando
  `HasHeader`) e reutilizar `recover_at`/`is_dedup_of` para preencher
  `ctx->parquet_names`; a partir daí a saneação, o alias NAME-CASE-1, as
  notas e `src_name` seguem o caminho Parquet. Se a leitura do cabeçalho
  falhar ou a contagem não bater, fallback: nota genérica listando as colunas
  cujo nome tem o formato `<outro>_<n>`.
- Concluído quando: `a,a,b,A` carrega com notas e `src_name`, eager e lazy;
  `v24`, `v52`, `t02` continuam verdes.

#### T5 (S3) — Paridade de funções: `mod` (F7), `%d` (F6), `(count)` formato (F5)

- `mod`: `exprtrans.cpp` → `r = a - b*trunc(a/b)`, depois `r + b` se `r < 0`
  (não usar o `fmod()` do DuckDB, que é a versão `floor`); unit + linhas de
  oráculo nativo em `v68` (ou `v79`).
- `%d`: `typemap.cpp::classify_format` aceita `%d`, `%-d`, `%dX…` como `Td`
  (atenção: não confundir com `%dc`, inexistente); unit em `test_typemap.cpp`;
  round-trip em `v41` ou `v54`; tabela de tipos da ajuda/README.
- `(count)`: `view.cpp::collapse` → formato `%8.0g` para `count`
  (independentemente do tipo da fonte); pinar em `v75`; ajuda documenta a
  extensão `(count)` sobre strings.

#### T6 (S3) — Usabilidade e cobertura: `drop in` (F10), `tabulate` labels (F12), `duplicates list` (F13)

- `drop in`: `_parqit_drop` + novo `op` no plugin ou reutilização de
  `keep_in` com máscara invertida (validação de intervalo igual à de `keep in`,
  com `f`/`l`/negativos); ajuda, dialog `parqit_filter.dlg` (se expuser
  `keep in`, expor `drop in`), `t15` shapes, `v13`.
- `tabulate`: o plugin envia, no cabeçalho `tabh`/`t2h`, o nome do value
  label da variável; o ado aplica `vallabs_` (que já chegam ao `collect`) —
  o mais simples é o plugin emitir registos `vlab` para as variáveis
  tabuladas; opção `nolabel`; manter `r()` inalterado. Pinar em `t13`/`v66`.
- `duplicates list`: separador `\x1f` (ou hex por célula) em
  `plugin_view.cpp` e `_parqit_print_duplist`.

#### T7 (S3/doc) — Notas de dialecto e documentação (F8, F9, F14, A.5)

- Ajuda (secção Expressions): case mapping simples de `ustrupper/ustrlower`
  (`ß`, `İ`); `regexm` sem multilinha (`^`/`$` só nos extremos, `.` não casa
  `\n`); mais unário aceite; valores abaixo de `mindouble` ficam missing.
- ASSUMPTIONS: as decisões acima + `c(pid)` vazio no nonce.
- README: `partition_by` (T2), tipos `%d` (T5), `float()` (T1).

#### T8 (S4) — Harness (F15)

- `tests/run_stata.sh`: avisar (ou recusar) quando `RUNDIR` ultrapassa ~100
  bytes; `v67`/`v70`: normalizar o log antes do `grep` (remover `\n> `).
- Concluído quando: a suite passa com `TMPDIR` de 160 bytes.

#### T9 (opcional, perf) — Percentis sem `list_sort` (F16)

- Só depois de T1–T8 verdes. `stata_pctile_sql` via `quantile_disc` com os
  ranks exactos da regra do Stata (k, k+1 quando np é inteiro); validar
  byte-a-byte contra `v44`/`v75` e medir em 10^7 linhas × 1 grupo e ×10^5
  grupos; manter a versão antiga se houver qualquer diferença numérica.

### B.3 Ordem e critérios de fecho

1. T1 → T2 → T3 → T4 (correcção de dados/contrato), depois T5 → T6 → T7 →
   T8, e T9 só se sobrar tempo.
2. Fecho da release: `parqit_tests` 100 %, `run_stata.sh` completo
   100 % (com os novos `v78`–`v81`), `release_lint` OK, exemplos
   `examples/parqit_basics.do` e `parqit_tour.do` executam sem erro contra
   o novo plugin, `docs/audits/README.md` ganha a linha desta auditoria e da
   remediação, versão v0.1.30 em todas as superfícies (CMake, ado, sthlp,
   dialogs, pkg `Distribution-Date`, README, CITATION, CHANGELOG).
3. Entregar um `IMPLEMENTATION_LOG` curto (o que mudou, o que ficou por
   fazer e porquê, output real das suites), à semelhança de
   `docs/audits/2026-08-22/IMPLEMENTATION_LOG.md`.

### B.4 Fora de âmbito (decidido não fazer agora)

- Modo multilinha em `regexm` (o Stata não o tem; só documentar).
- Full case mapping ICU (o motor não oferece; documentar).
- `summarize/egen/sample … if` (recusas claras já existem; feature futura).
- Mudar a política de arredondamento de `%td` fraccionários no save
  (documentada, com nota).

---

## C. Implementação de T1–T4 (2026-09-01, mesma sessão)

Executadas as tarefas T1 a T4 do plano (F2/F11, F1, F3, F4). Nada foi
commitado; a etiqueta v0.1.30 (bump de versão em todas as superfícies) fica
para o fecho da release, depois de T5–T8.

| Tarefa | Achado | Alteração | Teste |
|---|---|---|---|
| T1 | F2 + F11 | `exprtrans.cpp`: um literal numérico não inteiro que o float32 não representa é emitido como `CAST(<lit> AS DOUBLE)` em `relational()`, `inrange()` e nos operandos de `round()` (o motor passa a alargar a coluna FLOAT, como o Stata); `float(x)` implementada. | unit `FLOAT-LIT-1`/`FLOAT-FN-1` (test_exprtrans.cpp, coluna FLOAT na fixture); `v79_float_literal_compare.do` (64 filtros × 2 modos vs `count if` nativo, `gen`/`replace ... if` por `cf`, valores de `float()`) |
| T2 | F1 | `plugin_io.cpp`: `partition_string_keys_check()` percorre a árvore encenada antes de a publicar e recusa (rc 198, nada publicado) um directório `<chave>=NULL` ou `<chave>=__HIVE_DEFAULT_PARTITION__` sob uma chave VARCHAR; nota de leitura em `plan_columns` para árvores estrangeiras com esses directórios; `cmd_view_open` passa a imprimir as notas estruturais do planeador (antes só o caminho eager as mostrava). | `v78_partition_string_keys.do` (12 valores de string incl. `""`, `=`, `/`, espaço, `%`, `.`, Unicode, caixa, `01` — exactos em eager/lazy/view save; recusas; árvore pyarrow com nota nos dois caminhos); `audit_repro/repro_partition_string_null.do` |
| T3 | F3 | `cmd_describe` emite os registos `dtype` na ordem do manifesto, com o nome Stata; `_parqit_resp_describe` procura o tipo pelo nome (fallback posicional só se o nome não existir). | `v80_describe_alignment.do` (ficheiro plano, árvore, glob sobre árvore, glob plano, ficheiro estrangeiro saneado/deduplicado/alias de caixa; oráculo pyarrow por nome); `audit_repro/repro_describe_hive_type_shift.do` |
| T4 | F4 | `Source.csv_first_sql`; em `plan_columns` o cabeçalho bruto do CSV é lido com o dialecto do `sniff_csv` (`read_csv(header=false, all_varchar=true, …)`) e alinhado com o scan, reutilizando `recover_at`; `recover_at` reconhece `column<idx>` para uma célula de cabeçalho vazia. | `v81_csv_header_names.do` (`a,a,b,A`, TSV, cabeçalho vazio, nome com espaço, sem cabeçalho; notas no log); `audit_repro/repro_csv_duplicate_headers.do` |

Desvios em relação ao plano, com a razão:

- T2: o plano propunha comparar os valores distintos de cada chave na origem
  e na árvore; isso re-executa todo o pipeline num view save. A verificação
  passou a ser a inspecção dos nomes dos directórios encenados (exacta, porque
  o parqit nunca escreve uma string NULL); ASSUMPTIONS #104. Verificou-se ainda
  que o DuckDB 1.5.3 escreve a chave numérica missing como
  `__HIVE_DEFAULT_PARTITION__` (a CLI de desenvolvimento escreve `NULL`).
- T4: `sniff_csv()` devolve nomes já desduplicados e reporta uma aspa/escape
  não definidos como o texto literal `(empty)`; o cabeçalho é lido como dados
  e esses valores são mapeados para vazio antes de chamar `read_csv`;
  ASSUMPTIONS #106.
- T1: os literais inteiros ficam sem cast (mesmo acima de 2^24) para não
  alterar o plano de nenhum filtro por chave inteira; o residual está
  documentado na ajuda e em ASSUMPTIONS #103. Durante a validação surgiu um
  caso irmão — `round(x, 0.1)` numa coluna float calculava em precisão simples
  — corrigido no mesmo passo (operandos em DOUBLE).

Validação final (output real):

- `cmake --build build/dev`: 0 avisos; `parqit_tests`: 100 casos / 3.756
  asserções PASS.
- `bash tests/run_stata.sh` completa, `TMPDIR` por omissão: **102 VERDICT
  PASS / 0 FAIL** (98 anteriores + v78–v81), `SUITE_RC=0`.
- Repros `audit_repro/repro_{partition_string_null,float_literal_compare,
  describe_hive_type_shift,csv_duplicate_headers}.do`: PASS.
- `examples/parqit_basics.do` e `examples/parqit_tour.do` contra o plugin
  novo: sem erros.
- `bash tests/release_lint.sh`: OK (a lista de funções da ajuda inclui
  `float`).
- Documentação: CHANGELOG `[Unreleased]` (Fixed ×5, Added ×1), ASSUMPTIONS
  #103–#106, `parqit.sthlp` (lista de funções, parágrafo de expressões,
  `partition_by()`, Input formats), README (Limitations).

Por fazer (plano, parte B): T5 (`mod`, `%d`, formato de `(count)`), T6
(`drop in`, `tabulate` com value labels, `duplicates list`), T7 (notas de
dialecto na ajuda), T8 (harness), T9 (percentis), bump v0.1.30 e commit —
feito na parte D.

## D. Implementação de T5–T9 e fecho da release v0.1.30 (2026-09-01, mesma sessão)

| Tarefa | Achado | Alteração | Teste |
|---|---|---|---|
| T5 | F7, F6, F5 | `exprtrans.cpp`: `mod()` é o resto truncado, deslocado por `+y` quando negativo, com operandos em DOUBLE (MOD-TRUNC-1); `typemap.cpp`: `%d`, `%-d` e `%d<tokens>` classificados como data (DFMT-1); `view.cpp`: o alvo `(count)` de uma string leva `%8.0g` (COUNT-FMT-1). | unit `MOD-TRUNC-1` (test_exprtrans), `DFMT-1` (test_typemap); `v82_audit_fixes_20260901.do` (14 casos de `mod` vs nativo; `%d` → `date32` no pyarrow e restauro exacto; formato do count sem nota) |
| T6 | F10, F12, F13 | `view.cpp`/`plugin_view.cpp`/`parqit.ado`: `drop in f/l` (complemento de `keep in`, mesma gramática e validação; também no diálogo de filtro); `tabulate` mostra os value labels (registos `tvl`) e `nolabel` mostra os códigos; `duplicates list` junta as células com `\x1f`. | `v82` (`drop in` vs nativo em 6 intervalos, recusas e composição; `tabulate` com e sem labels no log; um TAB dentro de uma célula); `t15_dialog_shapes.do` (+ forma `drop in`) |
| T7 | F8, F9, F14, A.5 | `parqit.sthlp`: `ustrupper`/`ustrlower` (mapeamento simples), `regexm` sem modo multilinha, `mod` não inteiro, mais unário aceite, valores abaixo de `mindouble` missing; README (Limitations). | `bash tests/release_lint.sh` (lista de funções) |
| T8 | F15 | `tests/run_stata.sh` avisa quando o directório temporário é longo o suficiente para o Stata quebrar os caminhos citados; `v67`/`v70` comparam sem depender da quebra de linha. | suite completa com `TMPDIR` por omissão |
| T9 | F16 | `view.cpp`: `pct_window_sql` — a regra de percentis do Stata sobre `row_number()`/`count()` por grupo (uma janela que faz spill) em vez de `list_sort(list(x))`, em `collapse` e em `tabstat` (`plugin_view.cpp`); `stata_pctile_sql` removida; `View::fresh_helper` tornada pública. | `v83_collapse_percentiles.do` (collapse vs `collapse` nativo por `cf`: grupos ímpares/pares/de uma linha/todos missing, empates, fontes int e float, p1..p99, com e sem `by()`, ao lado de `first`/`last`; `tabstat` vs `tabstat, save` nativo: 800 células); `t03` (p25) e `t13` (p50) mantêm-se |

Medições de T9 (duckdb CLI, `memory_limit='1GB'`, `temp_directory` definido;
ficheiro de 200 M linhas com 200 000 grupos): a formulação antiga falha com
`Out of Memory Error` com e sem `by()`; a nova completa em 35 s (`by()`) e
63 s (um único grupo). No probe de 20 M linhas dentro do Stata: 24,1 s →
20,1 s de relógio; RSS máximo sem limite de memória 6,0 GB → 8,7 GB
(materialização da janela; limitado por `memory_limit` e com spill —
ASSUMPTIONS #114). O `EXPLAIN` confirma que o scan lê apenas as colunas
usadas (`g`, `x`, `f` no probe).

Empacotamento (pendente desde 2026-08-28, ASSUMPTIONS #102): `parqit.pkg`
passa a listar `f parqit_basics.do` e `f parqit_tour.do`; `build.yml` copia
os dois ficheiros para todos os zips e para os assets soltos da release;
`release_lint.sh` verifica que cada ficheiro `f` existe (`src/ado/p/` ou
`examples/`) e é copiado pelo workflow.

Revisão integral da ajuda (`parqit.sthlp`): corrigidas a linha de sintaxe de
`tabulate` (faltava `nolabel`), a nota sobre o formato do `(count)` de uma
string, `%d` como sinónimo de `%td` na secção de tipos e a nota de percentis
out-of-core em `collapse`; banner 0.1.30. O resto do texto foi confirmado
contra o comportamento final.

Validação final (output real):

- `cmake --build build/dev`: 0 avisos; `parqit_tests`: 102 casos / 3.802
  asserções PASS.
- `bash tests/run_stata.sh` completa, `TMPDIR` por omissão: **104 VERDICT
  PASS / 0 FAIL** (102 anteriores + v82 + v83), `SUITE_RC=0`.
- `bash tests/release_lint.sh`: OK (v0.1.30, 01sep2026 / pkg 20260901,
  CHANGELOG `[0.1.30]`).
- `examples/parqit_basics.do` e `examples/parqit_tour.do` contra o plugin
  0.1.30 (`parqit version` → `0.1.30`, DuckDB v1.5.3): sem erros.
- Documentação: CHANGELOG `[0.1.30]` (Changed ×2, Fixed ×10, Added ×5),
  ASSUMPTIONS #103–#114, `parqit.sthlp`, README, CLAUDE.md.
