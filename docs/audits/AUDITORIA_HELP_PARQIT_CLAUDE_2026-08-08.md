# Auditoria adversarial do help público do `parqit` — 2026-08-08

**Objeto:** `src/ado/p/parqit.sthlp` (v0.1.24, 8aug2026), confrontado com o
dispatcher `src/ado/p/parqit.ado`, o plugin/engine em `src/plugin/` e
`src/engine/`, os diálogos `.dlg`, os testes existentes e o comportamento
observado em `stata-mp` 19.5 com o plugin `build/dev/parqit.plugin`
(DuckDB v1.5.3).

**Executor:** Claude (Opus 5), sessão isolada, sem rede.
**Âmbito de escrita autorizado e efetivamente usado:** `src/ado/p/parqit.sthlp`,
`tests/release_lint.sh`, um teste novo dedicado ao contrato do help
(`tests/verify_suite/v66_help_contract.do`) e este relatório. Nenhum ficheiro de
código funcional (C++/ado), versão, data, changelog ou empacotamento foi tocado.
Não houve commit, push, PR, tag nem release.

---

## 1. Veredito executivo

> **PASS WITH RESIDUAL RISKS** — para a **qualidade do help**, não para a release.

O help já estava substancialmente correto: a passagem anterior de auditoria (as
alterações locais não commitadas em `parqit.sthlp`) tinha eliminado as
formulações absolutas mais perigosas ("nothing is read", "sem uma segunda cópia"
em `open _data`) e alinhado a maior parte do contrato. Esta auditoria confirmou
essa correção por execução e encontrou **seis defeitos documentais residuais**,
todos agora corrigidos, dos quais dois são materiais (uma promessa de opções que
o comando rejeita, e um `r()` prometido como sempre presente que só existe
condicionalmente).

Ficam registados **quatro defeitos de implementação** encontrados durante a
falsificação, todos fora do âmbito de escrita desta tarefa e nenhum deles com
risco de corrupção de dados: falham alto, mas com mensagens que expõem
internals ou nomes errados. Estão descritos em §5 como trabalho separado.

O risco residual do help é baixo e está enumerado em §8.

---

## 2. Matriz de rastreabilidade

Coluna "Evidência de execução": `P#`/`Q#`/`R#`/`S#`/`T#` remetem para os
probes descritos em §6.2 (logs em scratchpad da sessão; todos reprodutíveis com
o do-file indicado).

| Superfície / afirmação | Evidência de código | Evidência de teste/execução | Local no help | Veredito |
|---|---|---|---|---|
| 53 subcomandos públicos do dispatcher | `parqit.ado:14-19` (`local cmds`) | `release_lint.sh` (check mecânico) | secção Syntax | OK |
| Opções de cada subcomando | `syntax` de cada `_parqit_*` | P1, P3, Q13–Q15, S3 | Syntax + secções próprias | 2 correções (§3 F1, F3) |
| Abreviaturas das opções | maiúsculas em cada `syntax` | leitura sistemática | Syntax | OK (todas conferem) |
| `use` lazy vs eager; `using` opcional | `parqit.ado:301-405` | P2, R9, T2 | Syntax | 1 correção (§3 F4) |
| `use, clear` não fecha views | `_parqit_use` (ramos disjuntos) | Q10 (`r(n_views)`=1) | Syntax, Materialisers | OK |
| Lazy merge: opções aceites | `parqit.ado:1730` | P1 (rc 198 × 4), v66 (× 7) | Syntax, Verbs | **corrigido** (§3 F1) |
| `mergein` = `merge` nativo | `parqit.ado:1899-1957` | Q16 | Syntax, Perf tips | 1 precisão (§3 F6) |
| `appendin keep()` nomeia vars do ficheiro | `parqit.ado:1971` | Q16 (N=304, k=4) | Syntax | OK |
| `correlate` sem opções | `parqit.ado:1246` | P3 (rc 101), v66 | Syntax | **corrigido** (§3 F3) |
| `describe <source>` = só Parquet | `plugin_io.cpp:2159-2167` (`source_for` sem `csv`) | P6 (`.csv`/`.dta` → rc 920), R7 | Input formats, Limitations | OK |
| Glob de esquema misto recusado | `strict_schema_gate` | R7 (rc 198 estrito / rc 0 relaxed) | Input formats | OK |
| CSV/`.tab` principal = scan; `using` = bridge | `parqit.ado:262-295` | P8, T1, T2, T3 | Input formats | OK |
| `open _data` escreve snapshot | `parqit.ado:1527-1576` | P16 (`r(bridge)`) | Input formats, diagnostics | OK |
| Chaves de metadata (4) + `sortedby` | `plugin_io.cpp:551-592`, `plugin_view.cpp:471` | oráculo pyarrow (§6.3) | Stata metadata in Parquet | 1 omissão corrigida (§3 F5) |
| Round-trip de metadata (auto.dta) | — | Q12 (tipos, formatos, labels, notas, `Sorted by`) | Examples | OK |
| Estatísticas de `collapse` | `view.cpp:675-728` | leitura + S5 | Verbs | OK |
| Funções de `egen` | `view.cpp:908-916` | S3 | Verbs | OK |
| `duplicates drop` (com/sem varlist) | `plugin_view.cpp` + `_parqit_duplicates` | Q15 (4 casos) | Verbs | OK |
| `sample` (%, `count`, seed) | `parqit.ado:973-985` | P13 (3 casos) | Verbs | OK |
| `keep in` (#, negativo, invertido) | `parqit.ado:547-576` | P12 | Verbs | OK |
| Wildcards: `keep/drop/order/by()/keepusing()` | `view.cpp:202-218` | S3 | Verbs | OK |
| `sort`/`gsort` só nomes explícitos | `view.cpp:588-599` | S4 | Verbs | OK (ver §5 D4) |
| Caps de exploração (10 000 / 30 / 200 / 14 / 100 / 1 000 / 5 000) | `plugin_view.cpp:2830,2911,2967,3178,3194,3250,3414` | leitura + Q3 | Exploring a view | OK |
| `list` 20 / `list if` 200 | `parqit.ado:1090,1133` | Q4 | Exploring a view | OK |
| `head` positivo | `parqit.ado:1052` | Q11 (rc 198 × 2) | Exploring a view | OK |
| Lista completa de funções de expressão (57) | `exprtrans.cpp` (`fname ==`) | `release_lint.sh` (agora nos dois sentidos) | Expressions | OK + check reforçado |
| Literais de data (formatos aceites) | `exprtrans.cpp:747-810` | leitura | Expressions | 1 melhoria (§3 F5b) |
| `_n`/`_N`: onde funcionam | `view.cpp:380,428-434`; `plugin_view` stats | Q5, S1, S2, v66 | Expressions, Limitations | **corrigido** (§3 F2) |
| `statamissing` aplica-se na tradução | `_parqit_set` + `translate_*` | P14 | Expressions | OK |
| Codecs aceites / rejeitados | `plugin_io.cpp:1057-1063` | Q14 (rc 198) | Materialisers | OK |
| `chunk()` arredonda a múltiplos de 2048 | `plugin_io.cpp:1095` + DuckDB | Q13 + oráculo pyarrow (§6.3) | Materialisers | OK (confirmado) |
| `compression(snappy)` por omissão | `plugin_io.cpp:1092` | oráculo pyarrow: `SNAPPY` | Materialisers | OK |
| Recusa de destino sobreposto à fonte | `plugin_view.cpp:1385-1500` | v45 (suite existente) | Materialisers | OK |
| Lock `.parqit_lock` fail-closed | `plugin_io.cpp:110-200` | x02 (suite existente) | Materialisers | OK |
| Teto 2^31-1 obs (rc 901) | `parqit.ado:416-425`, `plugin_io.cpp:69` | leitura | Materialisers, Limitations | OK |
| `collect` sem `clear` → erro 4 | `parqit.ado:994-996` | Q9 (rc 4) | Materialisers | OK |
| Mapa de tipos DuckDB→Stata | `typemap.cpp:84-280` | leitura exaustiva | Type mapping | OK (lista de não suportados bate certo) |
| Saneamento de nomes (32 cp, reservados, `v#`, sufixos) | `sanitize.cpp:96-156` | leitura | Type mapping | OK |
| `r()` de cada forma de comando | `return` de cada `_parqit_*` | P5, P7–P11, P15–P17, Q1–Q3, T1, T3 | Stored results | **corrigido** (§3 F2b) |
| `parqit set` (4 nomes, ranges, aviso tempdir) | `plugin_view.cpp:1707-1747` | R8 (rc 0 + aviso) | Settings | OK |
| `menu` recusa batch | `_parqit_menu` | R8 (rc 199) | Settings | OK |
| Diálogos: comandos que constroem | `*.dlg` (grep) | leitura dos 10 `.dlg` | Point and click | OK |
| Exemplos do help, literais | — | Q12, R1, R2, R3, R4 | Examples | OK (todos correm) |
| Links SMCL internos | — | `release_lint.sh` (agora inclui links inline) | todo o ficheiro | OK + check reforçado |
| Renderização SMCL | — | `translate ... smcl2txt` (rc 0, 1191 linhas, zero markup por interpretar) | todo o ficheiro | OK |

---

## 3. Achados documentais (corrigidos nesta tarefa)

### F1 — MODERADO: o help prometia a `parqit merge` opções que o comando rejeita

**Afirmação anterior.** A linha de sintaxe do `merge` lazy terminava em
`[, merge_options]`, e o único sítio onde `merge_options` era definido (o
parágrafo de `mergein`) dizia: "*{it:merge_options} are the native ones
(keepusing(), keep(), generate(), nogenerate, update, replace, assert(), force,
nolabel, nonotes, noreport)*". Um leitor conclui que `parqit merge 1:1 id using
X, force` é válido.

**Evidência de código.** `src/ado/p/parqit.ado:1730`:
`syntax using/ [, keep(string) KEEPUSing(string) GENerate(name) NOGENerate]`.

**Evidência de execução (P1).** Com uma view aberta sobre `master.parquet`:

| comando | `_rc` |
|---|---|
| `parqit merge 1:1 id using lookup.parquet, force` | 198 |
| `... , update` | 198 |
| `... , assert(match)` | 198 |
| `... , nolabel` | 198 |

**Correção.** A linha de sintaxe do `merge` lazy passa a listar as suas quatro
opções; o parágrafo de `mergein` passa a dizer que `merge_options` pertence a
`mergein` e acrescenta que o `merge` lazy não é um wrapper do `merge` nativo e
rejeita qualquer outra opção nativa.

### F2 — MODERADO: `_n`/`_N` — âmbito real das restrições

**Afirmação anterior.** "*They are not yet supported by `replace` or by a
`gen ... if` qualifier*" e, na secção de exploração, "*`parqit count if exp`
(any parqit expression, including missing(a,b,c))*".

Duas imprecisões distintas:

1. "por um qualificador `gen ... if`" lê-se como "`gen` com `if` não aceita
   `_n`", o que é falso: só o **interior do qualificador** é recusado.
2. `count if`/`list if` **não implementam** `_n`/`_N`, ao contrário do que
   "qualquer expressão parqit" sugere.

**Evidência de código.** `src/engine/view.cpp:380` recusa apenas
`uses_rowctx(c.sql)` (a condição do `if`), e o caminho de estatísticas
(`view_stats`) não faz o `rowctx_wrap`.

**Evidência de execução (Q5, S1, S2).**

| comando | `_rc` |
|---|---|
| `parqit gen double rn = _n` | 0 |
| `parqit gen double rn2 = _n if id > 5` | 0 |
| `parqit gen double b = wage if _n > 5` | 198 (*"_n/_N are not supported in the if qualifier of gen (yet)"*) |
| `parqit replace wage = _n` | 198 |
| `parqit drop if _n > 25` | 0 |
| `parqit count if _n <= 5` | **920** (erro do motor, ver §5 D1) |
| `parqit list if _N > 1` | **920** |

**Correção.** O parágrafo de `_n`/`_N` enumera agora exatamente os quatro
contextos vedados; a linha de `count if` na secção de exploração exclui
explicitamente `_n`/`_N` e liga para *Expressions*; o bullet de *Limitations*
foi alinhado.

### F2b — MODERADO: `r(ext_missing)`/`r(frac_dates)` não são devolvidos "sempre"

**Afirmação anterior.** "*`parqit save` **always** returns scalars r(N) and r(k),
local r(filename), and locals r(ext_missing) and r(frac_dates) … (**empty if
none**)*".

**Evidência de execução (Q1/Q2).** Com perda:

```
r(frac_dates) : "dfrac"
r(ext_missing) : "x"
```

Sem perda, `return list` mostra apenas `r(N)`, `r(k)`, `r(filename)` — os dois
macros **não existem** (é o comportamento normal do Stata: `return local x ""`
não cria o macro).

**Correção.** A secção *Stored results* passa a dizer que os dois macros são
guardados apenas quando houve perda, que estão ausentes de `return list` quando
não houve, e que ambas as referências expandem para vazio — que é a forma
correta de os testar.

### F3 — MENOR: `parqit correlate` não aceita `obs`/`sig`

A linha `parqit correlate varlist | parqit pwcorr varlist [, obs sig]` fazia
crer que as opções servem os dois comandos. `parqit.ado:1246` é
`syntax anything(name=vars)`, sem opções; **P3**: `parqit correlate id wage, obs`
→ `_rc = 101` ("options not allowed"). As duas formas passam a ter linhas de
sintaxe separadas, com a nota "(listwise; takes no options)".

### F4 — MENOR: a forma sem `using` estava documentada como exclusiva de `clear`

O help mostrava só `parqit use {it:filename}, clear`. Na realidade
(`parqit.ado:306-311`, `capture syntax ... using/` com fallback) o `using` pode
ser omitido sempre que não há varlist, e a forma sem `clear` abre uma view lazy.
**P2/R9**: `parqit use "master.parquet"` → rc 0, view `default` aberta;
`parqit use "panel.parquet", name(nm)` → rc 0; `parqit use "u*.parquet", relaxed`
→ rc 0. A segunda linha de sintaxe passa a mostrar as três opções e uma frase
explica que as duas formas são a mesma.

### F5 — MENOR: metadata e literais de data incompletos

* `parqit.schema` também transporta o **marcador de ordenação** (`sortedby`):
  `plugin_view.cpp:471` e `plugin_io.cpp:2245-2254` escrevem-no,
  `plugin_io.cpp:580-592` e `parqit.ado:449-455` restauram-no. Confirmado pelo
  oráculo pyarrow (`schema top-level keys: ['sortedby','vars','version']`,
  `sortedby: ['foreign']`) e por `Sorted by: foreign` depois de
  `sysuse auto` → `save` → `use` (**Q12**). A descrição de `parqit.schema` no
  help passa a incluí-lo.
* **F5b:** a lista de funções mencionava `td() tc() tC() tm() tq() th() tw()
  ty()` sem dizer que ortografia aceitam. `exprtrans.cpp:747-810` impõe
  `td(ddmonyyyy)`, `ty(yyyy)`, `tm(yyyym#)`, `tq(yyyyq#)`, `th(yyyyh#)`,
  `tw(yyyyw#)`, `tc()`/`tC(ddmonyyyy hh:mm[:ss[.fff]])`, e **`tC()` devolve a
  mesma contagem que `tc()`** (parqit não soma leap seconds — decisão explícita
  no código). Está agora num parágrafo próprio.

### F6 — MENOR: absolutos ainda por delimitar

* *Performance tips* dizia que `mergein`/`appendin` correm "*no round-trip
  through DuckDB*" sem sujeito: o lado em disco **é** lido pelo motor
  (`_parqit_mergein` faz `parqit use … , clear` numa frame). Passa a dizer que
  os dados em memória é que nunca atravessam o DuckDB.
* "*`parqit save` runs the pipeline and writes Parquet directly, never touching
  Stata's memory*" passa a começar por "With a view open,", já que sem view (ou
  com `data`) o `save` lê precisamente a memória.
* `tabulate`: `{opt row}/{opt col}` são aceites e **ignorados** na forma
  one-way (**P4/S6**, rc 0). A nota da linha de sintaxe diz agora isso em vez
  de "two-way only".

### F7 — MENOR (navegação)

O índice `viewerjumpto` rotulava "Options" uma secção intitulada `parqit set`
que documenta `set`, SQL cru e diagnósticos. Secção e rótulo passam a
"Settings, raw SQL and diagnostics" (o marcador `options` mantém-se, para não
quebrar ligações), e o índice ganha entradas para *Author* e *Acknowledgements*,
que existiam como marcadores sem entrada.

---

## 4. Falsificações tentadas e resultado

As doze perguntas obrigatórias do prompt, com o resultado:

| # | Hipótese a falsificar | Resultado |
|---|---|---|
| 1 | O help promete ausência total de I/O em operações que fazem scan de schema | **Não confirmada.** O texto diz "no result rows are loaded into Stata", "Opening probes source schema and metadata". *Mas* a mensagem em runtime ainda diz "nothing read" → §5 D3 |
| 2 | Confunde lazy com ausência de validação imediata | **Não confirmada.** A *Description* descreve bind-validation e queries de validação de contrato; Q7 mostra `query` inválida recusada com a view intacta |
| 3 | Afirma que `open _data` não cria segunda cópia sem prova | **Não confirmada.** O texto já diz que escreve um snapshot Parquet temporário; P16 devolve `r(bridge)` |
| 4 | Atribui a `describe` suporte a adaptadores | **Não confirmada.** É explicitamente Parquet-only; P6 confirma rc 920 em `.csv` e `.dta` |
| 5 | Mistura o caminho CSV principal com o `using`/bridge | **Não confirmada.** P8/T2: CSV e `.tab` principais não criam bridge; T1: CSV no `using` cria (`r(bridge)`) |
| 6 | Descreve mal views vivas/embebidas e a restauração do estado | **Não confirmada.** R5: `parqit view a: keep if …` muta a view `a` (20) e deixa `b` intacta (60), exatamente como o help afirma; R1 embebe `view:stats` num merge |
| 7 | Não distingue truncagem, amostragem e erro nos caps | **Não confirmada.** Q3/Q4: `list` trunca com nota, `tabulate`/`tabstat`/`levelsof` erram, `histogram` limita bins — tudo como documentado |
| 8 | Ambíguo sobre perdas de precisão/datas/strings/metadata | **Não confirmada** (uma omissão menor corrigida: `sortedby`, F5) |
| 9 | Lista `r()` errados por forma | **CONFIRMADA** para `save` (F2b). Todas as outras formas verificadas batem certo |
| 10 | Contradições internas | **CONFIRMADA** (F1: `merge_options`; F3: `correlate`) |
| 11 | Exemplos falham copiados literalmente | **Não confirmada.** Q12, R1, R2, R3, R4 correm todos com rc 0 e produzem o resultado descrito (incluindo os nomes `wage2018 n2018 …` do `pivot`) |
| 12 | Links SMCL para âncoras inexistentes / markup a atravessar linhas | **Não confirmada** hoje, **mas o check era incompleto**: o lint só validava alvos entre aspas (`viewerjumpto`), não os links inline `{help parqit##x:…}`. Reforçado (§7) |

---

## 5. Defeitos de implementação encontrados (FORA do âmbito — trabalho separado)

Nenhum destes foi mascarado no help: onde o help e o comportamento divergiam, o
help foi ajustado ao contrato pretendido e o defeito fica aqui registado. Nenhum
corrompe dados; todos falham alto.

### D1 — MODERADO: `count if`/`list if` com `_n`/`_N` expõem um nome interno do motor

**Repro:**

```stata
parqit use using p.parquet
parqit sort id
parqit count if _n <= 5
```

**Observado:**

```
parqit count: Binder Error: Referenced column "__PARQIT_ROW__" not found in FROM clause!
Candidate bindings: "firmid"
LINE 1: ...quet(['panel.parquet'])) SELECT * FROM __parqit_s0) WHERE (__PARQIT_ROW__ <= 5)
```

`_rc = 920`. Idem `parqit count if _N > 1` (`__PARQIT_NROWS__`).

**Porque importa.** O caminho `view_stats` traduz a expressão mas não aplica o
`rowctx_wrap` que `View::gen`/`keep if` aplicam, e o *placeholder* interno
chega ao binder. Isto viola o espírito da charter §5/§6.12 (nenhum nome interno
deve ser visível) e da §6.8 (erros altos **com mensagem útil**). O comportamento
correto seria a mesma recusa explícita que `gen` já dá: *"_n/_N are not
supported in count if"*, com rc 198.

**Sugestão.** Detetar `uses_rowctx()` no ramo `countif`/preview de
`plugin_view.cpp` e devolver `kRcUsage` com mensagem própria — ou implementar o
`rowctx_wrap` também aí, que é a correção que preserva funcionalidade.
`tests/verify_suite/v66_help_contract.do` assere apenas `_rc != 0` neste ponto,
precisamente para não bloquear essa melhoria.

### D2 — MENOR: `parqit list` reporta erros como `parqit collect`

`parqit list if _n <= 5` falha com o prefixo `parqit collect: …` (o preview
partilha a entrada `view_collect_prepare`). O utilizador vê o nome de um comando
que não escreveu.

### D3 — MENOR: a mensagem de `parqit use` lazy ainda diz "nothing read"

`parqit.ado:348-350` imprime
`(lazy view default opened over X: 4 columns; nothing read — use parqit collect or parqit save)`.
O help foi deliberadamente reescrito (na passagem anterior) para não usar essa
formulação, porque a abertura **lê** o schema e a metadata do ficheiro. A
mensagem em runtime devia acompanhar (por exemplo: "schema probed; no rows
loaded"). Não é alterável nesta tarefa (é código ado).

### D4 — MENOR: `parqit sort` aceita um wildcard que casa exatamente uma coluna

`View::sort` (`view.cpp:591-594`) chama `expand_patterns` e só rejeita quando
`names.size() != keys.size()`. **S4**: `parqit sort w*` com uma única coluna a
casar → rc 0; `parqit sort *` com várias → rc 198 *"wildcards are not allowed in
parqit sort"*. O contrato documentado ("explicit names only") é o pretendido; o
help **não** foi alterado para abençoar a exceção.

### D5 — COSMÉTICO: ruído dos bridges e nome de view candidata

* Importar um lado `using` não-Parquet imprime as mensagens internas do
  `import delimited`/`use` e do `parqit save` do bridge, incluindo o caminho
  temporário (**T1**, **T3**). `_parqit_import_to_bridge` não usa `quietly`.
* `parqit sql "…", clear` imprime `(… collected; view __000000 remains open)` —
  o tempname da view candidata — enquanto devolve `r(view) = "default"` (**Q8**).
* `parqit summarize x, detail` imprime uma célula `%` vazia na última linha da
  tabela (número ímpar de percentis).

---

## 6. Validação executada nesta sessão

### 6.1 Bateria obrigatória

| Comando | rc | Resultado observado |
|---|---|---|
| `git diff --check` | 0 | sem whitespace errors |
| `bash -n tests/release_lint.sh` | 0 | — |
| `bash tests/release_lint.sh` | 0 | `release-lint OK: v0.1.24 (8aug2026 / pkg 20260808); CHANGELOG top [0.1.24]` |
| `cmake --build build/dev --target parqit_ado_sync -j` | 0 | `ado/plus/p <- parqit.sthlp` |
| `cmp -s src/ado/p/parqit.sthlp ado/plus/p/parqit.sthlp` | 0 | ficheiros idênticos |
| `ctest --preset dev` | 0 | 3/3 (`unit`, `runner_no_match`, `unit_concurrent`) |
| `STATA=stata-mp BUILD_DIR=$PWD/build/dev bash tests/run_stata.sh m0_smoke` | 0 | `VERDICT(M0_SMOKE): PASS` |
| `… bash tests/run_stata.sh v6` | 0 | `V60…V66: PASS` (7/7, inclui o teste novo) |

Checks mecânicos exigidos, todos verdes via `release_lint.sh`:

* os 53 subcomandos públicos do dispatcher aparecem na secção Syntax;
* as 57 funções de `exprtrans.cpp` aparecem na lista de funções do help **e**
  nenhum nome da lista está por implementar (check novo, nos dois sentidos);
* todos os alvos `parqit##…` (índice **e** links inline) têm `{marker}`;
* nenhuma diretiva SMCL inline atravessa linhas físicas.

Cada um destes quatro checks foi **testado negativamente** numa cópia sandbox da
árvore (remover `regexm` da lista; acrescentar `ustrregexm`; apontar um link para
`parqit##inputformats`; acrescentar um subcomando `frobnicate` ao dispatcher):
os quatro falharam com a mensagem certa e voltaram a passar depois de reposta a
cópia. O `parqit.sthlp` do repositório nunca foi mutado nestes testes.

### 6.2 Probes adversariais em Stata

Cinco do-files, corridos em `stata-mp -b` com `adopath ++ <repo>/src/ado/p` e
`$PARQIT_PLUGIN_PATH` apontado ao plugin local, com dados sintéticos criados
pelo próprio probe em `c(tmpdir)`:

* **probe1 (P1–P17)** — opções rejeitadas do merge lazy, forma sem `using`,
  `correlate` com opções, one-way `tabulate` com `row`, `r()` de 17 formas de
  comando, `describe` de `.csv`/`.dta`, bridges, prefixo `view name:`,
  `keep in`, `sample`, `save` de view vs memória, `open _data`, `path`,
  `version`.
* **probe2 (Q1–Q16)** — `r(ext_missing)`/`r(frac_dates)` com e sem perda, `r()`
  de toda a família de exploração, caps de `list`, matriz `_n`/`_N`, literais
  `.a`/`.A`, `&&`, `usubstr`, função inexistente, `query` inválida seguida de
  `count`, `sql` nas duas formas, erro 4, `use, clear` com view aberta, `head`
  com 0/−1, round-trip do `auto.dta`, `chunk()`, codec desconhecido,
  `duplicates drop` nas quatro combinações, `appendin keep()`.
* **probe3 (R1–R10)** — os exemplos do help executados literalmente (views
  nomeadas + `view:` como fonte, `query`/`explain`/`sql`, `pivot`, `reshape
  long`), mutação da view alvo pelo prefixo, `misstable summarize`,
  `duplicates list`, `tabulate a b, row col`, união `relaxed`, `set tempdir`
  inexistente, `menu` em batch, `selftest`, `order`/`rename` com troca,
  `contract`.
* **probe4 (S1–S6)** — matriz fina de `_n`/`_N`, wildcards em `egen by()` e
  `merge keepusing()`, wildcard em `sort`, `collapse` sem `by()` sobre view
  vazia.
* **probe5 (T1–T3)** — bridge do lado `using` em CSV, `.tab` como fonte
  principal, `append` com fonte `.dta`.

### 6.3 Oráculos independentes

`pyarrow` 24.0.0 sobre ficheiros escritos pelo parqit (o parqit nunca foi usado
para se validar a si próprio):

```
chunk(1000) row_groups= 5 sizes= [2048, 2048, 2048, 2048, 1808]
chunk(3000) row_groups= 3 sizes= [4096, 4096, 1808]
keys: ['parqit.chars', 'parqit.dtalabel', 'parqit.schema', 'parqit.vallabs']
schema top-level keys: ['sortedby', 'vars', 'version']
sortedby: ['foreign']
compression: SNAPPY
```

Isto confirma, independentemente: (a) o arredondamento de `chunk()` a múltiplos
de 2048 e o mínimo efetivo de 2048 que o help afirma; (b) que as chaves de
metadata são exatamente as quatro documentadas; (c) que `snappy` é mesmo o
codec por omissão; (d) que o marcador de ordenação viaja em `parqit.schema`.

### 6.4 Renderização SMCL

`help parqit` **não pode ser aberto** numa sessão não interativa desta máquina:
o Stata em batch (e também com stdin em pipe) responde `request ignored because
of batch mode` e devolve `_rc = 0`, pelo que esse rc não prova nada. Em
substituição, a renderização foi verificada com o próprio renderer do Stata:

```stata
translate "<repo>/src/ado/p/parqit.sthlp" "help_render.txt", translator(smcl2txt) replace
```

→ `_rc = 0`, 1191 linhas de texto renderizado, **zero** ocorrências de
`{cmd:`, `{bf:`, `{it:`, `{opt`, `{p_end}`, `{marker`, `{help`, `{p 8` … no
output (nenhum markup por interpretar). As secções alteradas foram inspecionadas
no output renderizado (bloco de funções, literais de data, linhas de sintaxe de
`merge`/`correlate`/`use`, secção de settings) e apresentam-se corretamente.

---

## 7. Alterações aplicadas

### 7.1 `src/ado/p/parqit.sthlp` (14 hunks)

| # | Alteração | Justificação |
|---|---|---|
| 1 | Índice: "Options" → "Settings, raw SQL and diagnostics"; entradas novas para *Author* e *Acknowledgements* | F7 |
| 2 | Título da secção `{marker options}` idem + frase de abertura "Engine settings." | F7 |
| 3 | Segunda forma de `parqit use` com as três opções + parágrafo a explicar a omissão de `using` | F4 (P2, R9) |
| 4 | Linha de sintaxe do `merge` lazy com as suas quatro opções | F1 (P1, v66) |
| 5 | `merge_options` atribuído a `mergein`; frase a dizer que o `merge` lazy rejeita o resto | F1 |
| 6 | Nota de `row`/`col` no `tabulate` | F6 (P4, S6) |
| 7 | `correlate` e `pwcorr` em linhas separadas | F3 (P3) |
| 8 | `parqit.schema` inclui o marcador `sortedby` | F5 (oráculo pyarrow, Q12) |
| 9 | Perf tips: `mergein`/`appendin` — o lado em disco é lido pelo motor | F6 |
| 10 | Perf tips: "Write without loading" passa a "With a view open, …" | F6 |
| 11 | `count if` exclui `_n`/`_N` e liga para *Expressions* | F2 (S1) |
| 12 | Lista de funções delimitada por comentários `{* parqit-lint: … }` + parágrafo novo sobre a ortografia dos literais de data e `tC()`≡`tc()` | F5b + suporte ao check bidirecional |
| 13 | Parágrafo de `_n`/`_N` reescrito com os quatro contextos vedados | F2 (Q5, S1, S2) |
| 14 | *Limitations*: bullet de `_n`/`_N` alinhado; *Stored results*: `r(ext_missing)`/`r(frac_dates)` condicionais | F2, F2b (Q1, Q2) |

Nada foi removido do contrato público: todas as alterações restringem promessas
excessivas ou acrescentam factos verificados.

### 7.2 `tests/release_lint.sh`

* A verificação de funções de expressão passa a ler o **bloco delimitado** do
  help (`{* parqit-lint: expression-function-list begin/end }`) e a comparar nos
  **dois sentidos**: função implementada ausente do help **e** função anunciada
  no help que `exprtrans.cpp` não implementa. O parser só olha para o conteúdo
  dos grupos `{cmd:…}`, pelo que a prosa entre eles não é lida como nomes.
* A verificação de âncoras passa a extrair **todos** os `parqit##…` do ficheiro
  (índice `viewerjumpto` **e** links inline `{help parqit##x:…}`), não apenas os
  que estão entre aspas. Antes, um link inline para um marcador inexistente
  passava despercebido.
* Cabeçalho de documentação do script atualizado.

Não foram acrescentados checks baseados em prosa incidental.

### 7.3 `tests/verify_suite/v66_help_contract.do` (novo)

Justificação da exceção prevista no prompt: `release_lint.sh` é um lint de
texto e **não pode** verificar comportamento. Os contratos corrigidos em F1,
F2, F2b, F3, F4 e F6 são comportamentais; sem um teste, voltam a divergir em
silêncio. O ficheiro pina, com um `assert` por frase do help:

* o `merge` lazy recusa sete opções nativas (`_rc == 198`) e aceita as quatro
  documentadas, deixando a view utilizável;
* `correlate` recusa `obs`/`sig` (`_rc == 101`), `pwcorr` aceita-os;
* `save` não define `r(ext_missing)`/`r(frac_dates)` sem perda e define-os
  (com os nomes certos) com perda;
* `_n`/`_N`: aceites em `keep if`/`drop if` e na expressão principal de `gen`
  (mesmo com `if`), recusados no qualificador `if` de `gen` e em `replace`
  (`_rc == 198`), e falham alto em `count if`/`list if` (`_rc != 0`,
  deliberadamente sem fixar o código, para não bloquear a correção D1);
* `parqit use <file>` sem `using` funciona lazy, com `name()` e com `clear`;
* `tabulate` one-way aceita e ignora `row`/`col`.

Estado: **PASS**. Duas mutações de controlo (inverter `assert _rc == 198` e
`assert r(ext_missing) == ""`) fazem o ficheiro abortar com `r(9)` e sem linha
`VERDICT`, o que o runner conta como falha — os asserts não passam por vacuidade.

---

## 8. Limitações da validação e risco residual

1. **`help parqit` interativo não foi aberto.** Nesta máquina o Stata só aceita
   `help` numa sessão verdadeiramente interativa; em batch e com stdin em pipe
   a chamada é ignorada. A renderização foi validada com `translate …
   smcl2txt` (§6.4), que usa o mesmo renderer, mas não exercita o Viewer nem os
   links clicáveis. **Recomenda-se abrir `help parqit` uma vez no Stata GUI
   antes da release** e clicar nas entradas do índice.
2. **A suite Stata completa não foi corrida.** Correram `m0_smoke`, a família
   `v6*` (7 ficheiros, inclui o novo) e os testes C++. Como só mudaram o
   `.sthlp`, o lint e um teste novo, nenhum caminho de código foi afetado; ainda
   assim, isto **não** é uma validação de release.
3. **Os diálogos foram auditados por leitura**, não abertos (`db parqit_*`
   requer GUI). As afirmações do help sobre eles foram confrontadas com os
   comandos que cada `.dlg` constrói.
4. **Os quatro defeitos de implementação de §5 continuam abertos.** O mais
   relevante (D1) faz o help ser correto por descrição de uma limitação em vez
   de por descrição de uma funcionalidade: se D1 for corrigido implementando
   `_n`/`_N` em `count if`/`list if`, o help terá de ser revisto (o teste v66
   foi escrito para não o impedir).
5. **`describe`/`glimpse` de uma fonte não-Parquet devolve rc 920** com a
   mensagem do motor. O help diz que é Parquet-only, o que é verdade, mas a
   mensagem de erro podia ser mais didática ("use `parqit use` para CSV/dta").
   Não alterado (código).
6. **A cópia instalada** `ado/plus/p/parqit.sthlp` foi sincronizada pelo alvo
   `parqit_ado_sync` e é byte-idêntica à fonte. Não foi editada à mão.
7. **O índice `docs/audits/README.md` não foi atualizado** com a entrada deste
   relatório: o prompt proíbe explicitamente alterá-lo. Fica para o mantenedor.

---

## 9. Ficheiros locais preexistentes preservados sem alteração

Verificado por `git status --short` no início e no fim, e por checksum:

| Ficheiro | Estado | Observações |
|---|---|---|
| `docs/audits/README.md` | ` M` (modificado pelo utilizador) | **não tocado** por esta tarefa |
| `docs/audits/AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-08.md` | `??` | md5 `ecd7e2576a7c633fcd40729f9c1a8094` — inalterado |
| `docs/audits/PROMPT_AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md` | `??` | md5 `f49bb0e5bfc46416010c6f2edf46f050` — inalterado |
| `docs/audits/PROMPT_CORRECAO_MELHORIA_PARQIT_2026-08-08.md` | `??` | md5 `32dacf1d659e245200ea1bff990ae497` — inalterado |
| `src/ado/p/parqit.sthlp` | ` M` (auditoria anterior) | alterações anteriores **preservadas na íntegra**; esta auditoria acrescentou 14 hunks por cima |
| `tests/release_lint.sh` | ` M` (auditoria anterior) | checks anteriores preservados; dois foram substituídos por versões mais fortes (bidirecional; links inline) |

Estado final do worktree:

```
 M docs/audits/README.md
 M src/ado/p/parqit.sthlp
 M tests/release_lint.sh
?? docs/audits/AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-08.md
?? docs/audits/AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md
?? docs/audits/PROMPT_AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md
?? docs/audits/PROMPT_CORRECAO_MELHORIA_PARQIT_2026-08-08.md
?? tests/verify_suite/v66_help_contract.do
```

Sem `git reset`, `git restore`, `git checkout --`, `git clean`, reformatadores
globais ou substituições em massa. Sem commit, push, PR, tag ou release.

---

## 10. Nota final

Um help aprovado não é uma release aprovada. Esta auditoria cobre apenas a
fidelidade do `parqit.sthlp` ao comportamento observado da v0.1.24 nesta
máquina. A decisão de release continua a depender da suite Stata completa a
partir da árvore instalada, do CI verde nas três plataformas e do tratamento
dos defeitos de §5.
