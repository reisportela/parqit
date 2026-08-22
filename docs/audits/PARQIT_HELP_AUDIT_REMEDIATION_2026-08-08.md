# Remediação dos defeitos de runtime — auditoria do help, 2026-08-08

Registo de execução de
[PROMPT_CORRECAO_RUNTIME_PARQIT_2026-08-08.md](PROMPT_CORRECAO_RUNTIME_PARQIT_2026-08-08.md),
que implementa os defeitos D1–D6 registados na §5 e §8.5 de
[AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md](AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md),
mais o D7 que o maintainer mandou corrigir depois de a verificação desta ronda
o expor.

## Resultado

Os sete defeitos foram corrigidos: os seis do prompt (D1–D6) e o D7, que
apareceu durante a verificação do D5a e que o maintainer mandou corrigir na
mesma ronda. **Todos eram defeitos de mensagem ou de caminho de erro**: nenhum
tocava no plano de dados, nos tipos, na metadata ou num materializador, e
nenhum ficou mais silencioso do que estava. O help
auditado descrevia já o contrato correto; estas correções alinham o *runtime*
com ele — `parqit.sthlp` não foi alterado.

Estado final dos gates: **PASS local**. Bateria C++ com 3/3 testes CTest,
72/72 casos e 1063/1063 asserções doctest. Bateria Stata integral com 87
verdicts PASS, zero FAIL, zero log em falta e zero abort depois do verdict.
`release_lint` verde e cópia instalada sincronizada.

## Snapshot e ambiente

- Base Git: `e0a79f5` (`main`), mais o trabalho local não commitado da
  auditoria do help (`src/ado/p/parqit.sthlp`, `tests/release_lint.sh`,
  `tests/verify_suite/v66_help_contract.do`, relatórios em `docs/audits/`),
  **preservado integralmente**.
- Host: GCC/CMake 3.26 locais, StataNow MP 19.5, DuckDB v1.5.3 (build `dev`).
- Oráculos disponíveis e usados na auditoria de origem: pyarrow 24.0.0,
  duckdb CLI.

## Defeitos e correções

### D1 — `_n`/`_N` em `count if`/`list if` expunham `__PARQIT_ROW__`

**Sintoma.** `parqit count if _n <= 5` e `parqit list if _N > 1` falhavam com
rc 920 e a mensagem crua do motor:

```
parqit count: Binder Error: Referenced column "__PARQIT_ROW__" not found in FROM clause!
```

**Causa.** O tradutor emite os *placeholders* `__PARQIT_ROW__`/
`__PARQIT_NROWS__`, resolvidos apenas pelo compilador da view no ponto em que
uma *stage* do plano é acrescentada (`keep if`/`drop if`, `View::gen`). Os
filtros read-only aplicam a condição a um `SELECT` **já compilado**, pelo que o
token chegava ao binder do DuckDB — um nome interno numa mensagem pública
(charter §5/§6.12), para uma limitação que o help já documenta.

**Correção.**

- `src/engine/exprtrans.hpp` — campo novo `ExprResult::uses_rowctx`.
- `src/engine/exprtrans.cpp` — `Parser::used_rowctx` marcado nos ramos
  `Tok::SysN`/`Tok::SysBigN` e propagado por `translate_expression` e
  `translate_filter`. Os consumidores existentes (`view.cpp`) não foram
  tocados: o flag é informação adicional.
- `src/plugin/plugin_view.cpp` — ramo `countif` e o filtro de preview de
  `cmd_view_collect_prepare` recusam com mensagem própria e `kRcUsage` (198).

**Decisão registada.** Recusar, não implementar. Implementar `_n`/`_N` nesses
caminhos alargaria um contrato público que `parqit.sthlp` §Expressions, `v66` e
`v67` fixam como indisponível; fica em `ASSUMPTIONS.md` §88 como *enhancement*
futuro, com as três superfícies que teriam de mudar em conjunto.

**Prova.** rc 198 nos dois comandos, mensagem sem `__PARQIT_` e sem
`Binder Error`, view inalterada a seguir (`v67`, `v66` apertado de
`_rc != 0` para `_rc == 198`, `audit_repro/repro_countif_rowctx.do`, e um caso
unitário novo em `tests/unit/test_exprtrans.cpp` com 15 asserções: o flag liga
para `_n`, `_N`, `mod(_n,2)==0 & x>1` e em modo `statamissing`, não liga para
`x <= 5` nem `x + 1`, e não pode ser forjado a partir de um literal de string).

### D2 — erros de `head`/`list` atribuídos a `parqit collect`

**Sintoma.** `parqit list in 50/60` fora de gama, uma variável de preview
inexistente e "no lazy view is open" vindo de `parqit head` reportavam-se todos
como `parqit collect: …`.

**Causa.** `collect`, `head` e `list` partilham `view_collect_prepare`, cujos
prefixos estavam escritos à mão.

**Correção.** Campo `label` no request (`_parqit_wr_collect_request`), definido
por `_parqit_collect`/`_parqit_head`/`_parqit_list`; o plugin lê-o, **valida-o
contra `{collect, head, list}`** (nunca ecoa texto do wire) e usa-o nos oito
`cry` da função. O `load_req` passou a correr antes do `require_view` para que
a própria mensagem de "no view" já saiba o nome do comando.

**Armadilha encontrada e corrigida durante a execução.** A primeira versão lia
`req.value("label", …)` e a validação caía sempre no default: **todo o texto
originado pelo utilizador neste wire é hex** (CLAUDE.md). A leitura passou a
`parqit::req_text(req, "label", …, /*required=*/false)`. Sem o teste de
mensagem do `v67` isto teria passado como "feito" com o defeito intacto.

**Prova.** `v67` assere `parqit list:` e `parqit head:` nas mensagens
respetivas e a **ausência** de `parqit collect:` nos dois blocos.

### D3 — a mensagem do `use` lazy dizia "nothing read"

**Sintoma.** Contradizia o help, que descreve a abertura como "probes source
schema and metadata".

**Correção.** `src/ado/p/parqit.ado` e a cópia em
`benchmarks/profile_parqit.ado`: `nothing read` → `schema probed, no rows
loaded`. Nenhum teste ou exemplo dependia do texto antigo (`grep` sobre
`tests/`, `examples/`, `src/`, `benchmarks/`, `README.md`); as únicas
ocorrências restantes são logs históricos em `benchmarks/_out` (git-ignored) e
`CHANGELOG.md`, deliberadamente intocados.

**Prova.** `v67` exige `no rows loaded` e proíbe `nothing read` no log real.

### D4 — `sort`/`gsort` aceitavam um wildcard com um único match

**Sintoma.** `parqit sort w*` era aceite quando `w*` casava exatamente uma
coluna, contra o contrato documentado "explicit names only".

**Causa.** `View::sort` comparava o número de nomes expandidos com o número de
chaves; um padrão 1-para-1 não mudava a contagem.

**Correção.** `src/engine/view.cpp` — deteção pelo **padrão**, antes da
expansão. O check por contagem fica como cinto para duplicados.

**Prova.** `v67`: `sort w*`, `gsort -w*`, `gsort +w*` e `sort ye?r` → rc 198;
`sort wage` e `gsort -wage id` continuam rc 0.

### D5a — ruído e caminho do bridge no `using` não-Parquet

**Sintoma.** Um lado `using` em CSV/`.dta`/Excel imprimia o chatter do
`import`/`use` e o `(N obs, k vars written to …/bridge.parquet)` do snapshot,
expondo um caminho temporário que o utilizador nunca nomeou.

**Correção.** `_parqit_import_to_bridge` envolve o import e o snapshot em
`capture noisily frame … { quietly { … } }`.

**Verificação obrigatória do prompt (facto, não suposição).** Medido em
StataNow MP 19.5, antes de aceitar o `quietly`:

| caso | resultado |
|---|---|
| `capture noisily quietly use <inexistente>.dta` | `file … not found` visível, rc 601 |
| `capture noisily quietly parqit merge 1:1 <chave inválida>` | texto `SF_error` do plugin visível, rc 920 |
| o mesmo dentro de `capture noisily { quietly { … } }` | texto visível, rc 920 |
| `.dta` corrompido no lado `using` (caminho real do bridge) | `file … not Stata format` visível, rc 610 |
| bridge CSV bem-sucedido | **nenhum** output |

`quietly` não suprime nem o texto de erro nativo nem o `SF_error` do plugin.
Registado em `ASSUMPTIONS.md` §89.

**Prova.** `v67` assere, no mesmo teste, a ausência de `bridge.parquet` e de
`_parqit_bridge_import` no sucesso **e** a presença de `not Stata format` com
rc ≠ 0 na falha — mais um `parqit count` a seguir a cada um, para que "calado"
nunca possa passar por "saltado".

### D5b — `sql , clear` imprimia o tempname da view candidata

**Sintoma.** `(2 vars, 3 obs collected; view __000000 remains open)` enquanto
`r(view)` devolvia `default`.

**Correção.** `_parqit_sql` passa o collect interno a
`capture noisily quietly` (falhas continuam visíveis) e reimprime a linha com o
nome **comprometido** depois do `view_commit`, a partir de `r(N)`/`r(k)`
capturados antes do `plugin call`.

**Prova.** `v67`: log com `view default remains open`, sem `__00`, e
`r(view) == "default"`.

### D5c — célula `%` órfã no `summarize, detail`

**Sintoma.** Nove percentis em duas colunas deixavam a última linha com um `%`
sem valor.

**Correção.** `_parqit_print_detail` emite a linha final só com o par
esquerdo, mantendo larguras.

**Prova.** `v67` reconstrói as linhas lógicas do log e assere que nenhuma
termina em `%`, e que `99%` continua a ser impresso com `r(p99)` correto.

### D6 — `describe` de fonte não-Parquet com erro cru

**Sintoma.** `parqit describe f.csv` → rc 920 com `Invalid Input Error` do
motor. O help já dizia "Parquet-only"; a mensagem não.

**Correção.** `_parqit_describe` classifica a extensão final do alvo com a
mesma regra de `_parqit_resolve_source` (basename, último ponto,
case-insensitive) e, para `csv|tsv|txt|tab|dta|xls|xlsx`, para com rc 198 e
aponta para `parqit use`. Globs e outras extensões seguem para o plugin como
antes; `glimpse` herda o guard por delegação. **Um diretório é sempre isento**
(`direxists`): uma árvore Hive pode legitimamente chamar-se `algo.csv`, e
recusá-la seria uma regressão — verificado com um diretório `hive.csv/` que
continua a ser descrito normalmente (rc 0), enquanto o ficheiro `plain.csv` é
recusado (rc 198).

**Prova.** `v67`: `.csv` e `.dta` → rc 198 com `reads Parquet footers only` e
`parqit use using`, sem `Invalid Input Error`; `describe` de um `.parquet`
continua a devolver `r(n_rows)`/`r(n_cols)`.

### D7 — chave de join inexistente chegava ao binder do DuckDB

*Fora da lista original; encontrado durante a verificação do `quietly` (D5a) e
corrigido por instrução explícita do maintainer.*

**Sintoma.** `parqit merge 1:1 nosuchkey using ...` devolvia rc 920 com:

```
parqit merge: Binder Error: Referenced column "nosuchkey" not found in FROM clause!
Candidate bindings: "id"
LINE 1: ...])) SELECT * FROM __parqit_s0) GROUP BY (CASE WHEN isnan(CAST("nosuchkey" ...
```

**Causa.** Ordem, não lógica em falta: `View::merge_with` **já tinha** a
mensagem certa ("key X not found in the master view"), mas os contratos de
unicidade correm antes da mutação do plano — corretamente — e agrupam pelas
chaves, pelo que uma chave inexistente era vista primeiro pelo binder. O
`joinby`, sem contrato de unicidade, chegava à mensagem certa mas devolvia
rc 198 (usage) em vez de 111.

**Correção.** `View::join_keys_error(op, keys, using)` — uma única
implementação da regra "a chave existe dos dois lados com o mesmo kind" —
chamada por `merge_with`, por `joinby_with` **e** por `cmd_view_twotable` antes
de qualquer query. Ambos os verbos param agora com rc 111 (o código que o Stata
nativo dá para uma variável inexistente) e a mensagem do motor.

**Efeito lateral corrigido no mesmo sítio.** Os três verbos de duas tabelas
prefixam a sua própria mensagem com o nome do verbo e o chamador acrescentava
outro: `parqit joinby: joinby: key ... not found`. O chamador passou a usar
`cry("parqit " + e)`.

**Prova.** `v67` (bloco D7) e `audit_repro/repro_merge_key_not_found.do`:
chave ausente nos três kinds lazy, presente só no master, só no using, dentro
de uma lista multi-chave, e com kinds diferentes — todos rc 111, log sem
`Binder Error` e sem `__parqit_s`, view intacta a seguir a cada recusa, e um
merge válido logo depois na mesma view. `keepusing(nosuch)` e a colisão de
`generate()` do `append` ficam pinados como controlos de prefixo único.

## Ficheiros alterados

### Runtime

- `src/engine/exprtrans.hpp`, `src/engine/exprtrans.cpp` (ROWCTX-1)
- `src/engine/view.cpp`, `src/engine/view.hpp` (SORT-WILD-1, JOINKEY-1)
- `src/plugin/plugin_view.cpp` (ROWCTX-1, MSG-LABEL-1, JOINKEY-1)
- `src/ado/p/parqit.ado` (MSG-LABEL-1, BRIDGE-QUIET-1, SQL-CANDNAME-1,
  DETAIL-ODDCELL-1, DESCRIBE-EXT-1, mensagem do `use` lazy)
- `benchmarks/profile_parqit.ado` (a mesma linha, para não divergir)

### Regressões

- `tests/verify_suite/v67_runtime_message_contract.do` (novo)
- `tests/verify_suite/v66_help_contract.do` (dois asserts apertados para 198)
- `tests/unit/test_exprtrans.cpp` (caso `ROWCTX-1`)
- `audit_repro/repro_countif_rowctx.do`, `audit_repro/repro_merge_key_not_found.do` (novos)

### Documentação

- `CHANGELOG.md` (`[Unreleased]`: Added/Fixed/Changed)
- `ASSUMPTIONS.md` (§88 recusar-vs-implementar; §89 `quietly` × `SF_error`)
- `docs/audits/README.md` (índice: relatório da auditoria do help, este
  registo, e os dois prompts)

**Não alterados**, como o prompt exige: `src/ado/p/parqit.sthlp` (o help
auditado continua verdadeiro sem uma única alteração), versões, datas,
`parqit.pkg`, diálogos, `CMakeLists.txt`, `CITATION.cff`.

## Validação (execução desta remediação)

| Comando | rc | Resultado |
|---|---|---|
| `cmake --build build/dev -j` | 0 | plugin + testes reconstruídos; `ado/plus/p` refrescado |
| `ctest --preset dev` | 0 | 3/3 (`unit`, `runner_no_match`, `unit_concurrent`) |
| `./build/dev/parqit_tests` | 0 | 72/72 casos, 1063/1063 asserções |
| `bash tests/run_stata.sh` (integral) | 0 | **87 verdicts PASS, 0 FAIL**, 0 abort pós-verdict |
| `bash tests/run_stata.sh v67` | 0 | `VERDICT(V67_RUNTIME_MESSAGE_CONTRACT): PASS` |
| `bash tests/release_lint.sh` | 0 | `v0.1.24 (8aug2026 / pkg 20260808); CHANGELOG top [0.1.24]` |
| `cmp -s src/ado/p/parqit.sthlp ado/plus/p/parqit.sthlp` | 0 | idênticos |
| `cmp -s src/ado/p/parqit.ado ado/plus/p/parqit.ado` | 0 | idênticos |
| `git diff --check` | 0 | sem whitespace errors |

## Risco residual

1. **`_n`/`_N` continuam indisponíveis** em `count if`/`list if`. É agora uma
   recusa limpa e documentada, não um erro do motor — mas continua a ser uma
   limitação, não uma funcionalidade (`ASSUMPTIONS.md` §88).
2. **Outros caminhos podem ainda devolver erros crus do motor.** O caso
   concreto encontrado nesta ronda (chave de merge inexistente) foi corrigido
   como D7, mas a auditoria não varreu sistematicamente todas as entradas do
   plugin à procura do mesmo padrão. Uma passagem dedicada — "para cada query
   gerada, o que acontece se um nome do utilizador não existir?" — continua
   por fazer.
3. **O `label` do wire tem três valores válidos.** Se um caminho novo passar a
   usar `view_collect_prepare`, tem de acrescentar o seu label à lista do
   plugin, ou a mensagem volta a dizer `collect`.
4. **`help parqit` continua sem ser aberto num Viewer real** (o Stata ignora
   `help` fora de uma sessão interativa nesta máquina). A renderização foi
   validada na auditoria com `translate … , translator(smcl2txt)`.
