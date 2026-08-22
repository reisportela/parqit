# PROMPT — Correções da auditoria holística de 2026-08-09 (docs + testes + varredura de mensagens)

> Prompt de implementação para um agente de código de IA (Claude Code / Codex).
> Copiar integralmente como instrução da tarefa. Fonte dos achados:
> [AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-09.md](AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-09.md)
> — achados A1–A2 (§4), sugestão editorial de §5, recomendações de §7 e o
> risco residual herdado de §8. Este prompt é **completo**: cobre tudo o que a
> auditoria julgou dever ser corrigido, fixa as decisões (nada fica "opcional")
> e regista o que foi deliberadamente decidido NÃO corrigir, para que não seja
> re-litigado. As Tarefas 1–3 são documentais; as Tarefas 4–5 acrescentam
> testes e podem tocar runtime **apenas** no padrão de mensagem já estabelecido
> (D6/D7); nenhuma tarefa toca plano de dados, tipos, metadata ou
> materializadores.

---

És um engenheiro de correção a trabalhar no repositório `parqit`
(`~/Documents/GitHub/parqit`). Lê primeiro `CLAUDE.md` e
`parqit_build_prompt.md` e obedece-lhes; em particular:

- **Regra de não-regressão**: nunca remover funcionalidade, reduzir precisão,
  corromper metadados, enfraquecer um caminho de erro, nem mudar semântica
  pública em silêncio. A Tarefa 2 tem a decisão de produto fixada — documenta,
  não remove. A Tarefa 5 só pode tornar mensagens *melhores* (rc próprio +
  texto útil), nunca mais silenciosas.
- Antes de escrever, `git status --short` e `git diff`: o worktree contém a
  remediação não commitada de 2026-08-08 (D1–D7 + auditoria do help, já
  auditada e aprovada) e os relatórios/prompts em `docs/audits/`. **Preserva
  tudo integralmente** — esta tarefa constrói por cima, nunca descarta. Nada
  de `git reset/restore/checkout --/clean`.
- Um branch de feature (p.ex. `docs/audit-2026-08-09`), Conventional Commits,
  nunca cometer estado que parta o build. Sem push, tag ou release sem
  instrução explícita do mantenedor.
- **Não alterar** versões, datas, `parqit.pkg`, diálogos, `CMakeLists.txt`,
  `CITATION.cff`. `tests/release_lint.sh` é o juiz das superfícies (nota: o
  CHANGELOG exige headings `###` únicos por secção).
- Fluxo de validação em cada tarefa e no fecho:
  `bash tests/release_lint.sh` → (se o `.sthlp`/`.ado` mudou)
  `cmake --build build/dev --target parqit_ado_sync -j` +
  `cmp -s src/ado/p/parqit.sthlp ado/plus/p/parqit.sthlp` +
  `translate "src/ado/p/parqit.sthlp" render.txt, translator(smcl2txt) replace`
  (rc 0, zero markup por interpretar) → (se testes/runtime mudaram)
  `cmake --build build/dev -j` → `ctest --preset dev` →
  `STATA=stata-mp BUILD_DIR=$PWD/build/dev bash tests/run_stata.sh <filtro>` →
  **integral** no fecho. Cada build refresca `ado/plus/p/`; depois de
  recompilar, `discard` numa sessão Stata viva. Nada se declara "feito" sem
  output real.

Âmbito de escrita autorizado: `README.md`, `src/ado/p/parqit.sthlp`,
`src/ado/p/parqit.ado` (o comentário da Tarefa 1.3 e, se a Tarefa 5 o exigir,
guards de mensagem no padrão DESCRIBE-EXT-1), `src/plugin/*.cpp` e
`src/engine/*.{cpp,hpp}` (apenas se a Tarefa 5 encontrar ofensores — padrão
JOINKEY-1), `ASSUMPTIONS.md`, `CHANGELOG.md`,
`tests/verify_suite/v68_expr_native_oracle2.do` (novo),
`tests/verify_suite/v69_raw_error_sweep.do` (novo),
`tests/unit/` (se a Tarefa 5 tocar o engine), `audit_repro/` (repros de
ofensores da Tarefa 5), `docs/audits/README.md` (só se faltarem entradas — o
auditor já acrescentou as de 2026-08-09; verifica, não dupliques) e o registo
de remediação novo `docs/audits/PARQIT_AUDIT_REMEDIATION_2026-08-09.md`.

Implementa as tarefas pela ordem dada.

---

## Tarefa 1 (A1) — o README deixa de prometer "Nothing is read"

O help (§Description/§lazy) e a mensagem de runtime dizem, desde a ronda
2026-08-08, que abrir uma view **sonda** schema e metadata sem materializar
observações. O README ficou como a única superfície pública com a promessa
absoluta antiga; a varredura da remediação D3 usou `grep 'nothing read'` e
falhou a variante com "is".

1. `README.md:52-53` — "*a plan of work, like a do-file you are still
   writing. Nothing is read, nothing is computed, and whatever dataset …*"
   → reescreve para o contrato exato do help, por exemplo:
   "*a plan of work, like a do-file you are still writing. Opening probes the
   file's schema and metadata, but no observations are read, nothing is
   computed, and whatever dataset you have in Stata's memory is not touched.*"
2. `README.md:226` — "*(nothing is read yet)*" → "*(schema probed, no rows
   loaded)*", igual à mensagem de runtime.
3. `src/ado/p/parqit.ado:336` — comentário interno "*open (or replace) the
   named lazy view — nothing is read*" → "*… — schema probed, no rows
   loaded*". Só o comentário; nenhuma linha executável.
4. Varredura de fecho, com ambas as grafias e case-insensitive:
   `grep -rniE 'nothing (is )?read' README.md src/ examples/ tests/ benchmarks/ docs/`
   — as únicas ocorrências aceitáveis são históricas: secções já lançadas do
   `CHANGELOG.md`, `docs/audits/*` (registos verbatim) e logs git-ignored em
   `benchmarks/_out`. Ajusta qualquer outra que a varredura encontre.
5. `CHANGELOG.md` `[Unreleased]` → `### Changed`: uma linha — README (e
   comentário do ado) alinhados com o contrato "schema probed" do help.

**Aceitação:** grep limpo fora do histórico; `release_lint` verde; nenhum
outro ficheiro tocado por esta tarefa.

## Tarefa 2 (A2) — `ty()` documentado como extensão do parqit

Facto verificado pela auditoria (probe_parity): `ty(2026)` é **r(133)
"unknown function" no Stata nativo 19.5**; o parqit aceita-o (valor = o
próprio ano, correto para %ty). O help apresenta os oito literais como
"*constants spelt in Stata's own notation*" e promete noutro parágrafo que a
sintaxe que o nativo rejeita é rejeitada também.

**Decisão fixada: manter e documentar.** Remover `ty()` seria uma regressão
de superfície (não-regressão) e não há risco de valor errado. A alternativa
estrita — recusar `ty()` apontando para o ano nu — fica registada como
rejeitada; só se o mantenedor a pedir por escrito, e nesse caso exige
`exprtrans.cpp` + teste unitário + help + CHANGELOG, fora deste prompt.

1. `src/ado/p/parqit.sthlp`, parágrafo dos literais de data (começa "*The
   date literals are constants spelt in Stata's own notation*"): apara a
   afirmação e acrescenta a exceção, por exemplo, no fim do parágrafo:
   "*`ty(`yyyy`)` is a parqit extension accepted for symmetry: native Stata
   has no `ty()` function (a yearly %ty value is written as the bare year,
   e.g. `2026`); the other seven literals match native Stata's own
   notation.*"
   **Atenção ao lint:** `ty` tem de continuar dentro do bloco delimitado
   `{* parqit-lint: expression-function-list begin/end }` — está implementado
   e o check é bidirecional; a nota vai na prosa, o bloco não muda.
2. Pondera meia frase no parágrafo "*syntax native Stata rejects (`||`,
   `&&`, `=`) is rejected here too*" se, depois da nota do ponto 1, ainda a
   julgares contraditória; caso contrário deixa-o (enumera operadores
   concretos, não universalidade).
3. `ASSUMPTIONS.md`: entrada nova (número a seguir à última) com o facto
   nativo (r(133) em StataNow 19.5), a decisão manter-e-documentar e a
   alternativa rejeitada.
4. `CHANGELOG.md` `[Unreleased]` → `### Changed`: uma linha (help passa a
   marcar `ty()` como extensão parqit).

**Aceitação:** `release_lint` verde (lista de funções continua a bater nos
dois sentidos); render SMCL rc 0; `cmp` da cópia instalada OK.

## Tarefa 3 (§5) — o parágrafo do modo de missing nomeia o qualificador `if`

A auditoria confirmou por execução (`gen y = _n if x > 0` com `x` missing:
nativo atribui, parqit em modo SQL deixa missing) que o qualificador `if` de
`gen`/`replace` segue a semântica do modo de missing, como um filtro — o help
documenta o efeito em filtros e atribuições mas não nomeia o qualificador, e
o leitor tem de o inferir.

Em `src/ado/p/parqit.sthlp` §Expressions, no parágrafo do modo SQL-missing
("*it differs for the upper tail and inequality …*"), acrescenta uma frase,
por exemplo: "*The `if` qualifier of `gen` and `replace` is a filter and
follows the same missing-value mode: under the default SQL semantics a
missing comparison excludes the row, under `statamissing on` it reproduces
native Stata.*" Mantém o resto do parágrafo intacto.

**Aceitação:** render SMCL rc 0; `v66` continua verde (não muda nenhum
contrato, só nomeia um caso do contrato existente).

## Tarefa 4 (§7.3) — `v68_expr_native_oracle2.do`: pinar o terreno verificado à mão

A auditoria verificou ~80 comparações nativo-vs-parqit; o `v61` pina 31.
Sem um teste, o terreno extra volta a depender de comentários. Um do-file
novo `tests/verify_suite/v68_expr_native_oracle2.do`, no padrão da suite:
recebe `args repo plugin`, faz `adopath ++ "`repo'/ado/plus/p"` e
`global PARQIT_PLUGIN_PATH "`plugin'"`, gera os seus dados sintéticos,
compara **no mesmo processo** o resultado nativo e o parqit (gen nativo em
memória → `parqit save …, data` → gens lazy sobre o mesmo Parquet →
`collect` → `merge 1:1` → asserts), e termina com uma única linha
`VERDICT(V68_EXPR_NATIVE_ORACLE2): PASS` ou `FAIL` — o runner julga só por
essa linha e um FAIL anterior nunca é mascarado por um PASS posterior.

Casos mínimos a pinar (da auditoria; um assert por caso):

- `round(-2.5)`/`round(-0.5)`/`round(2.5)`/`round(x,.)` — meias para
  +infinito, unidade missing;
- `mdy(2.5,10,2020)`, `mdy(0,1,2020)`, `mdy(2,30,2020)`, `dofm(1.5)`,
  `dofm(-0.5)`, `mofd(59.9)`, `dow(-0.5)`, `day(-0.5)` — truncagem
  fracionária e datas impossíveis row-local;
- matriz `inrange` com missings (x, lo, hi e todos), `cond` 3-arg e 4-arg
  com condição missing, `min`/`max` com literais `.`, `inlist(m, ., 3)`;
- `string(x)`/%9.0g num espetro de magnitudes (±, 1e20, 1.2345e-5, inteiro
  de 9 dígitos) e `real(".")`/`real("1e400")`/`real("abc")`/`real("1.5e3")`;
- `upper`/`strupper`/`ustrupper`/`lower` sobre "café"/"Ærø" (paridade byte a
  byte da dobra ASCII-only) e `strpos` com agulha multibyte;
- `1 < x < 10` e `gen y = _n if x > 0` com `x` missing: pina a divergência
  **documentada** em modo SQL como desigualdade esperada (nativo 1/atribuído,
  parqit missing) **e** a igualdade sob `statamissing on` — assim uma mudança
  de qualquer dos lados acorda o teste;
- `substr` que parte um codepoint: pina o valor documentado do parqit
  (U+FFFD + resto), não a igualdade com o nativo;
- `ty(2026)`: nativo `capture` → assert `_rc == 133`; parqit → rc 0 e valor
  2026 (pina o facto e a extensão documentada na Tarefa 2);
- `reshape long` com zeros à esquerda, variantes (`inc01`,`inc2`) e
  (`inc01`,`inc1`,`inc2`): rc e valores iguais aos nativos;
- Parquet hostil com colunas `__PARQIT_ROW__` e `__parqit_rn_1`, gerado no
  próprio teste pelo oráculo pyarrow (python3 -c … como os v-testes fazem;
  nada committed em `tests/fixtures/`): `parqit sort id` + `keep if _n <= 2`
  + `collect` com valores intactos; expressão que nomeie `__PARQIT_ROW__`
  recusa alto; `gen z = _n` coexiste com a coluna.

Correr `bash tests/run_stata.sh v68` e colar o VERDICT no registo de
remediação. `CHANGELOG.md` `[Unreleased]` → `### Added`: uma linha.

**Aceitação:** v68 PASS; uma mutação de controlo (inverter um assert) fá-lo
FAIL — prova que os asserts não passam por vacuidade; suite `v6` inteira
verde.

## Tarefa 5 (§8, risco residual 2) — varredura "nome de utilizador inexistente → erro cru do motor"

O registo de remediação de 2026-08-08 deixou explícito: D7 fechou o caso
concreto (chave de join inexistente), mas "*uma passagem dedicada — para cada
query gerada, o que acontece se um nome do utilizador não existir? — continua
por fazer*". Esta tarefa fá-la, test-first.

1. Escreve `tests/verify_suite/v69_raw_error_sweep.do`: sobre uma view
   pequena (2 colunas conhecidas), invoca **cada** superfície pública que
   aceita nomes de variáveis com um nome inexistente (`nosuchvar`) e assere,
   para cada uma: `_rc != 0`, mensagem **sem** `Binder Error`, sem
   `__parqit_s` e sem SQL gerado (usa a técnica de log-capture do `v67`:
   `qui log using …, name(probe)` → comando → `qui log close probe` → grep
   das needles no log; reaproveita o helper `_v67_grep` se fizer sentido
   copiá-lo). Superfícies mínimas: `keep`/`drop`/`order`/`sort`/`gsort`,
   `rename` (os dois lados), `gen`/`replace`/`egen … by()`, `keep if`/`count
   if`/`list` (varlist e if), `collapse` (fonte e by), `contract`,
   `duplicates report/list/drop`, `reshape long/wide` (`i()`, `j()`, stubs),
   `pivot` (`rows()`/`cols()`/spec), `merge`/`joinby` (chave — já pinada por
   D7, mantém como controlo — e `keepusing()`), `append generate()`
   (colisão), `summarize`/`tabulate`/`tabstat`/`levelsof`/`misstable`/
   `codebook`/`distinct`/`correlate`/`pwcorr`/`histogram`, `mergein`/
   `appendin` (`keepusing()`/`keep()`), `save partition_by()`.
2. Para cada ofensor que a varredura encontre (rc 920 com texto do binder ou
   nome interno): corrige no padrão estabelecido — valida o nome contra o
   manifesto **antes** de qualquer query, com a mensagem do próprio parqit e
   o rc que o caso nativo usa (`kRcVarNotFound`/111 para variável inexistente,
   `kRcUsage`/198 para uso inválido), no sítio mais a montante que já conheça
   o manifesto (ado se o nome nunca deve chegar ao wire; plugin/engine caso
   contrário — precedentes: DESCRIBE-EXT-1, JOINKEY-1, ROWCTX-1). Cada
   correção leva o seu repro mínimo em `audit_repro/` e, se tocar o engine,
   um caso unitário.
3. Se uma superfície já recusar bem, o v69 pina-a na mesma (é o contrato).
   Se alguma recusa exigir mudança de semântica pública para ser corrigida,
   **não** a mudes: regista em `ASSUMPTIONS.md` e deixa o assert do v69
   frouxo (`_rc != 0` apenas) com comentário a citar a entrada.
4. `CHANGELOG.md`: `### Added` (v69) e, se houver correções, `### Fixed`
   (uma linha por ofensor corrigido).

**Aceitação:** v69 PASS com zero needles proibidas em todas as superfícies;
ctest verde se o engine mudou; nenhuma falha ficou mais silenciosa (todo o
caminho tocado continua rc ≠ 0 + mensagem, provado pelo próprio v69).

## Decidido NÃO corrigir (registo para não re-litigar)

- **`sortedby_names()` faz unquote ingénuo** (`view.cpp:152-161`) de nomes
  com aspas: inatingível por construção — o manifesto lazy só contém nomes
  sanitizados (aspas nunca sobrevivem a `sanitize_stata_name`) e os verbos
  validam nomes novos. Sem correção; se um dia o manifesto aceitar nomes
  crus, o invariante cai e isto tem de ser revisto.
- **Overflow de agregados dentro do pipeline** (`collapse (sum)` pode
  transportar um double ≥ sentinela até à fronteira): sem divergência
  observável — o próprio Stata não armazena >8.99e307, `parqit_finite` usa a
  sentinela do Stata em todos os produtores aritméticos, `missing()` inclui o
  teste de gama e as fronteiras aplicam o guard. Custo/benefício de guardar
  os agregados não compensa; fica documentado aqui.
- **Divergências do modo SQL-missing e do `substr`/U+FFFD**: comportamento
  documentado no help e agora pinado pelo v68 (Tarefa 4) — não são defeitos.
- **Remover `ty()`**: rejeitado (Tarefa 2).

## Tarefa 6 — housekeeping e fecho

1. Escreve `docs/audits/PARQIT_AUDIT_REMEDIATION_2026-08-09.md` (português
   europeu, padrão de `PARQIT_HELP_AUDIT_REMEDIATION_2026-08-08.md`): por
   tarefa — o quê, ficheiros/linhas, prova (teste + rc), decisões tomadas; e
   o inventário de validação do ponto 3. Acrescenta a linha dele ao índice
   `docs/audits/README.md` (as entradas do relatório e deste prompt já lá
   estão — verifica, não dupliques).
2. `ASSUMPTIONS.md`: além da entrada de `ty()` (Tarefa 2), regista qualquer
   facto novo que as Tarefas 4–5 encontrem (p.ex. um comportamento nativo
   inesperado num caso do v68).
3. Correr TUDO e colar os resumos no registo de remediação:
   `cmake --build build/dev -j`, `ctest --preset dev`,
   `STATA=stata-mp BUILD_DIR=$PWD/build/dev bash tests/run_stata.sh`
   **integral** (inclui v66–v69), `bash tests/release_lint.sh`,
   `cmp -s src/ado/p/parqit.sthlp ado/plus/p/parqit.sthlp`,
   `git diff --check`.
4. `git status --short` final: só o âmbito autorizado; a remediação de
   2026-08-08 intacta (confere por diff). Commits atómicos por tarefa
   (`docs: align README with the schema-probed lazy contract`,
   `docs: document ty() as a parqit extension`,
   `docs: name the gen/replace if qualifier in the missing-mode paragraph`,
   `test: add v68 second native expression oracle`,
   `fix/test: refuse unknown user names before the engine (v69 sweep)`).
   Sem push, tag ou release; reporta com precisão o que mudou, o que não
   mudou e como reverter.

## Critérios globais de aceitação

- Tarefas 1–3: nenhuma superfície de código executável alterada além do
  comentário de 1.3; Tarefa 4 só acrescenta testes; Tarefa 5 só endurece
  mensagens/recusas no padrão existente, com teste e repro por correção.
- `release_lint` verde, render SMCL limpa, cópia instalada sincronizada,
  ctest verde, suite Stata integral verde (incluindo v68 e v69 novos).
- Nenhum nome interno (`__PARQIT_*`, `__parqit_s*`, tempnames, caminhos de
  bridge) visível em nenhuma mensagem dos casos cobertos; nenhuma falha mais
  silenciosa do que estava.
- Qualquer desvio deste prompt (facto divergente, custo inaceitável, decisão
  de design) fica registado em `ASSUMPTIONS.md` — nunca resolvido em
  silêncio.
