# PROMPT — Correção dos defeitos de runtime encontrados pela auditoria do help (2026-08-08)

> Prompt de implementação para um agente de código de IA (Claude Code / Codex).
> Copiar integralmente como instrução da tarefa. Fonte dos achados:
> [AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md](AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md),
> secções §5 (defeitos D1–D5) e §8 (riscos residuais). Não confundir com
> [PROMPT_CORRECAO_MELHORIA_PARQIT_2026-08-08.md](PROMPT_CORRECAO_MELHORIA_PARQIT_2026-08-08.md)
> (F1–F6), que **já foi executado** — os testes v61–v65 existem na árvore.

---

És um engenheiro de correção a trabalhar no repositório `parqit`
(`~/Documents/GitHub/parqit`). Lê primeiro `CLAUDE.md` e
`parqit_build_prompt.md` e obedece-lhes; em particular:

- **Regra de não-regressão**: nunca remover funcionalidade, reduzir precisão,
  corromper metadados, enfraquecer um caminho de erro, nem mudar semântica
  pública em silêncio. Todos os defeitos desta ronda são de *mensagem/caminho
  de erro*; nenhum toca no plano de dados — mantém isso assim.
- **Erros altos com mensagem útil** (charter §6.8) e **nenhum nome interno
  visível** (charter §5/§6.12): é exatamente o que D1 e D2 violam hoje.
- Antes de escrever, `git status --short` e `git diff`: o worktree pode conter
  trabalho não commitado da auditoria do help
  (`src/ado/p/parqit.sthlp`, `tests/release_lint.sh`,
  `tests/verify_suite/v66_help_contract.do`, relatórios em `docs/audits/`).
  **Preserva-o integralmente** — esta tarefa constrói por cima dele, nunca o
  descarta. Nada de `git reset/restore/checkout --/clean`.
- Fluxo de validação em cada tarefa e no fim:
  `cmake --build build/dev -j` → `ctest --preset dev` →
  `bash tests/run_stata.sh <filtro>` (e **integral** no fim) →
  `bash tests/release_lint.sh`. Cada build refresca `ado/plus/p/`; depois de
  recompilar, `discard` numa sessão Stata viva. Nada se declara "feito" sem
  output real.
- Um branch de feature (p.ex. `fix/help-audit-runtime-2026-08-08`),
  Conventional Commits, nunca cometer estado que parta o build. Não fazer
  push, tag nem release sem instrução explícita do maintainer.
- **Não alterar** versões, datas, `parqit.pkg`, diálogos, nem — salvo o ponto
  explícito da Tarefa 8 — `parqit.sthlp`: o help acabado de auditar já
  descreve o contrato correto; estas correções alinham o *runtime* com ele.

Âmbito de escrita autorizado: `src/ado/p/parqit.ado`,
`src/plugin/plugin_view.cpp`, `src/engine/exprtrans.{cpp,hpp}`,
`src/engine/view.cpp`, `benchmarks/profile_parqit.ado`,
`tests/verify_suite/v66_help_contract.do` (ajuste pontual),
`tests/verify_suite/v67_runtime_message_contract.do` (novo),
`audit_repro/`, `CHANGELOG.md`, `ASSUMPTIONS.md`, `docs/audits/README.md`
(índice) e o registo de remediação
`docs/audits/PARQIT_HELP_AUDIT_REMEDIATION_2026-08-08.md`.

Implementa as tarefas pela ordem dada (da mais grave para a menos). Os números
D# referem-se ao relatório da auditoria do help.

---

## Tarefa 1 (D1, severidade Moderada) — `_n`/`_N` em `count if`/`list if`: recusa limpa, não Binder Error

**Hoje:** `parqit count if _n <= 5` e `parqit list if _N > 1` falham com
rc 920 e expõem o placeholder interno:

```
parqit count: Binder Error: Referenced column "__PARQIT_ROW__" not found in FROM clause!
```

**Contrato documentado** (parqit.sthlp, secções *Expressions* e *Exploring a
view*, já corrigidas): os filtros read-only "do not implement them at all" e
falham alto sem mudar a view. O runtime tem de honrar isso com uma recusa
própria, como `gen`/`replace` já fazem.

**Decisão fixa desta tarefa: recusar, não implementar.** Implementar
`_n`/`_N` nos caminhos de stats/preview (via `rowctx_wrap`) é uma melhoria
legítima mas exige rever o help, o v66 e a matriz de contextos — fica
registada em `ASSUMPTIONS.md` como enhancement futuro, **não** se faz aqui.

1. Em `src/engine/exprtrans.hpp`, acrescenta a `ExprResult` o campo
   `bool uses_rowctx = false;`. Em `src/engine/exprtrans.cpp`, marca-o quando
   o parser consome um token `SysN`/`SysBigN` (≈ linhas 144–145 do lexer e o
   `case Tok::SysN:` ≈ linha 522 — o sítio certo é onde o placeholder
   `__PARQIT_ROW__`/`__PARQIT_NROWS__` entra no SQL). Garante que o flag
   sobrevive a `translate_filter` (que embrulha `translate_expression`).
   **Não** alteres o `uses_rowctx()` estático de `view.cpp` nem os caminhos
   `keep if`/`gen` que hoje funcionam — o flag novo é informação adicional,
   os consumidores existentes ficam como estão.
2. `src/plugin/plugin_view.cpp`, ramo `countif` (≈ linha 2981): depois de
   `translate_filter` devolver `ok`, se `tr.uses_rowctx`, `cry` com
   `"parqit count: _n/_N are not supported in count if; apply parqit keep if
   first, or collect and count natively"` e devolve `kRcUsage` (198).
3. Idem no caminho de preview partilhado `cmd_view_collect_prepare`
   (`pfilter`, ≈ linha 1186): mensagem
   `"parqit <label>: _n/_N are not supported in the if filter of <label>; use
   parqit keep if instead"` — o `<label>` vem da Tarefa 2; até lá usa
   `"parqit list"` (o único chamador que passa `filter`; confirma-o com
   `grep _sq_pfilter src/ado/p/parqit.ado`).
4. Aperta `tests/verify_suite/v66_help_contract.do`: os dois
   `assert _rc != 0` do bloco "read-only stats filters" passam a
   `assert _rc == 198`, e o comentário que justificava não fixar o código é
   substituído por um que cite esta correção (a recusa é agora contratual).
5. Repro mínimo `audit_repro/repro_countif_rowctx.do` (antes: rc 920 +
   `__PARQIT_ROW__` no output; depois: rc 198 + mensagem limpa).
6. Unit test em `tests/unit/` (ficheiro de exprtrans): `translate_filter`
   de `_n <= 5` devolve `ok` com `uses_rowctx == true`; de `x <= 5`,
   `uses_rowctx == false`.

**Aceitação:** os dois comandos devolvem rc 198 com mensagem que nomeia o
comando e nunca contém `__PARQIT_`; `parqit count` seguinte devolve o N
inalterado; `keep if _n`, `drop if _n` e `gen … = _n` continuam a funcionar
(v66 verde); ctest verde.

## Tarefa 2 (D2, severidade Menor) — erros do preview atribuídos ao comando certo

**Hoje:** `head`/`list` partilham `view_collect_prepare` e os erros de motor
saem com o prefixo `parqit collect:` (cinco `cry` em
`src/plugin/plugin_view.cpp` ≈ linhas 1129, 1150, 1240, 1251, 1327; o erro de
*tradução* do filtro em 1191 já diz `parqit list:`).

1. `_parqit_wr_collect_request` (`src/ado/p/parqit.ado` ≈ linha 2400): emite
   um campo novo `label` com o valor do local `_sq_cmdlabel`; por omissão
   (local vazio) emite `collect`, para o wire ficar estável.
2. Os três chamadores definem o local antes do `mata`:
   `_parqit_collect` → `collect`; `_parqit_head` → `head`;
   `_parqit_list` → `list`.
3. No plugin, lê `label` (com default `"collect"` — `req.value`), valida-o
   contra o conjunto `{collect, head, list}` (outro valor → usa `collect`;
   nunca ecoes texto arbitrário do wire numa mensagem) e usa
   `"parqit " + label + ": "` nos cinco `cry` e na mensagem da Tarefa 1.3.
4. Confirma com `grep -n '"parqit collect' src/plugin/plugin_view.cpp` que
   não sobra nenhum prefixo fixo nesse caminho.

**Aceitação:** um erro reproduzível do caminho partilhado — p.ex.
`parqit list in 50/60` sobre uma view de 20 linhas (a validação de gama vive
num dos `cry` listados) — reporta `parqit list: …`; o mesmo erro provocado
por `head`/`collect` reporta o nome respetivo. Nenhuma mensagem perde
informação.

## Tarefa 3 (D3, severidade Menor) — a mensagem do `use` lazy deixa de dizer "nothing read"

O help foi deliberadamente reescrito para "Opening probes source schema and
metadata but does not materialise observations"; a mensagem de runtime ainda
diz o contrário.

1. `src/ado/p/parqit.ado` ≈ linha 350: troca
   `… columns; nothing read — use {bf:parqit collect} or {bf:parqit save})`
   por
   `… columns; schema probed, no rows loaded — use {bf:parqit collect} or {bf:parqit save})`.
2. A mesma frase existe copiada em `benchmarks/profile_parqit.ado:178` —
   atualiza-a igual (harness não distribuído, mas não deve divergir).
3. `grep -rn 'nothing read' tests/ examples/ src/ README.md` e ajusta
   qualquer teste/exemplo que dependa do texto antigo. **Não** toques em
   `CHANGELOG.md:1554` nem em `docs/audits/*` — são registos históricos.

**Aceitação:** grep sem ocorrências fora de histórico; suites que capturam
mensagens (se alguma) verdes.

## Tarefa 4 (D4, severidade Menor) — `parqit sort` rejeita wildcards mesmo com um só match

**Hoje:** `View::sort` (`src/engine/view.cpp` ≈ 588–599) expande padrões e só
recusa quando o nº de nomes difere do nº de chaves — `parqit sort w*` com uma
única coluna a casar passa em silêncio, contra o contrato "explicit names
only".

1. Em `View::sort`, antes de `expand_patterns`: se alguma chave contém `*` ou
   `?`, devolve logo `"wildcards are not allowed in parqit sort"`. Remove o
   check posterior por contagem (fica morto) ou mantém-no como cinto — mas a
   deteção primária é pelo padrão, não pelo resultado.
2. `gsort` passa pelo mesmo `View::sort` via `op_sort` — confirma que herda a
   recusa; `reshape i()` já usa `col_index` direto (verificado na auditoria:
   sem expansão), não precisa de alteração.
3. Pin no v67 (Tarefa 7): `parqit sort w*` → rc 198 mesmo quando `w*` casa
   exatamente uma coluna; `parqit gsort -w*` idem; e um caso positivo
   `parqit sort wage` continua rc 0.

**Aceitação:** ambos os negativos rc 198 com a mensagem existente; nenhum
teste atual regride (corre a família `v6` e `t0` inteira).

## Tarefa 5 (D5a, severidade Cosmética) — silenciar o ruído dos bridges sem calar falhas

**Hoje:** importar um lado `using` não-Parquet imprime o chatter interno do
`use`/`import` e o `(N obs, k vars written to …/bridge.parquet)` do save do
bridge, expondo caminhos temporários (probes T1/T3 da auditoria).

1. Em `_parqit_import_to_bridge` (`src/ado/p/parqit.ado` ≈ 230–260),
   prefixa com `quietly` os comandos interiores (`use`, `import excel`,
   `import delimited`, `parqit save … , replace data`). Nota de facto: em
   Stata, `quietly` **não** suprime mensagens de erro nem output `as error`
   ([P] quietly) — mas o texto de erro do plugin chega via `SF_error`, cujo
   comportamento sob `quietly` tens de **verificar, não assumir**.
2. Verificação obrigatória antes de dar a tarefa por feita: com um `.dta`
   corrompido (escreve lixo num ficheiro com extensão `.dta`) e com um save
   de bridge forçado a falhar, o rc continua ≠ 0 **e a mensagem de erro
   continua visível**. Se o `quietly` engolir a mensagem do plugin, usa em
   alternativa o padrão de `_parqit_open` (`capture noisily { quietly … }`)
   ou gate só o `di` de sucesso de `_parqit_save` através de um local
   interno — e regista a escolha no registo de remediação.
3. Pin no v67: um `merge` com `using` CSV bem-sucedido não imprime nenhum
   caminho `bridge.parquet` (log-capture, ver Tarefa 7); um `using` `.dta`
   corrompido falha com rc ≠ 0 e mensagem não vazia.

**Aceitação:** T1/T3 da auditoria re-executados à mão mostram output limpo;
falhas continuam altas; `v24`/`v25` (multiformat, mergein/appendin) verdes.

## Tarefa 6 (D5b + D5c, severidade Cosmética) — mensagens de `sql, clear` e do `summarize, detail`

**D5b.** `parqit sql "…", clear` imprime
`(… collected; view __000000 remains open)` — o tempname da view candidata —
embora devolva `r(view) = "default"`.

1. Em `_parqit_sql` (`src/ado/p/parqit.ado` ≈ 2056–2075): passa o collect
   interno a `capture noisily { quietly _parqit_collect, clear }` (erros
   continuam visíveis; chatter não), preservando o tratamento de rc e o
   rollback existentes. Depois do `view_commit` bem-sucedido, imprime a
   mensagem final com o nome **comprometido**:
   `(`r(k)' vars, `r(N)' obs collected; view `sql_target' remains open)` —
   `r(N)`/`r(k)` sobrevivem do collect até ali (o `plugin call` não limpa
   `r()`); o `return local view`/`return add` existentes mantêm-se.
2. Pin no v67 via log-capture: o output de `sql …, clear` contém
   `view default remains open` e não contém `__00`.

**D5c.** `parqit summarize x, detail` imprime uma célula `%` órfã na última
linha (9 percentis, loop de pares em `_parqit_print_detail`,
`src/ado/p/parqit.ado` ≈ 3347–3350).

3. No ramo `i == 9` do loop, emite só o par esquerdo
   (`printf("  %11s%% %-14s\n", pl[9], …)`) em vez de imprimir a metade
   direita vazia com `%`. Mantém larguras e alinhamento das linhas pares.
4. Pin no v67 via log-capture: o output de `summarize …, detail` não contém
   nenhuma linha cujo último carácter não-branco seja `%`.

## Tarefa 7 — teste novo `tests/verify_suite/v67_runtime_message_contract.do`

Um único do-file, padrão da suite (gera os seus dados, `args repo plugin`,
`VERDICT(V67_RUNTIME_MESSAGE_CONTRACT): PASS/FAIL`), cobrindo o contrato de
*mensagens* que o lint de texto não consegue ver. Técnica de captura: um log
nomeado secundário à volta do comando —

```stata
capture program drop _v67_grep
program define _v67_grep, rclass
    args logfile needle
    tempname fh
    local found 0
    file open `fh' using `"`logfile'"', read text
    file read `fh' line
    while (!r(eof)) {
        if (strpos(`"`macval(line)'"', `"`needle'"')) local found 1
        file read `fh' line
    }
    file close `fh'
    return scalar found = `found'
end
* uso:  qui log using "`lg'", text name(probe)  →  <comando>  →
*       qui log close probe  →  _v67_grep "`lg'" "needle"
```

Casos mínimos (cada um com o seu assert):

1. D1: `count if _n <= 5` → rc 198; log sem `__PARQIT_`; `count` seguinte
   devolve o N previsto. Idem `list if _N > 1`.
2. D2: um erro de motor em `parqit list` reporta prefixo `parqit list:`;
   em `parqit head`, `parqit head:`.
3. D3: o log do `parqit use using …` lazy contém `no rows loaded` e não
   contém `nothing read`.
4. D4: `sort w*` (um match) e `gsort -w*` → rc 198.
5. D5a: merge com `using` CSV → log sem `bridge.parquet`; `.dta` corrompido →
   rc ≠ 0 e mensagem presente no log.
6. D5b: `sql …, clear` → log com `view default remains open`, sem `__00`.
7. D5c: `summarize …, detail` → nenhuma célula `%` órfã.
8. D6 (Tarefa 8): `describe` de `.csv` e de `.dta` → rc 198 e mensagem que
   aponta para `parqit use`.

Correr `bash tests/run_stata.sh v67` e colar o VERDICT no relatório.

## Tarefa 8 (D6 / §8.5, severidade Menor) — `describe` de fonte não-Parquet com erro didático

**Hoje:** `parqit describe f.csv` → rc 920 com o erro cru do motor. O help
já diz que `describe <source>` é Parquet-only; a mensagem deve dizê-lo
também.

1. Em `_parqit_describe` (`src/ado/p/parqit.ado` ≈ 1368, antes do
   `plugin call … describe`): calcula a extensão final do alvo com a mesma
   regra de `_parqit_resolve_source` (basename, última `.`,
   case-insensitive); se for `csv|tsv|txt|tab|dta|xls|xlsx`, erro rc 198:
   `parqit describe: reads Parquet footers only (file, glob or Hive
   directory); open delimited text, .dta or Excel with parqit use and
   describe the view`. Diretórios, globs e restantes extensões seguem para o
   plugin como hoje.
2. `glimpse` delega em `_parqit_describe` — herda o guard; confirma.
3. O help **não** muda (já afirma Parquet-only). Se decidires reformular a
   frase da mensagem, mantém-na consistente com o texto do help.
4. Pin no v67 (caso 8).

## Tarefa 9 — housekeeping e registo

1. `CHANGELOG.md`, secção `[Unreleased]` (sem bump de versão/datas; o
   `release_lint.sh` é o juiz e exige headings `###` únicos por secção):
   - **Fixed:** D1 (recusa limpa de `_n`/`_N` em `count if`/`list if`, antes
     Binder Error com nome interno), D2 (prefixo do comando certo nos erros
     de `head`/`list`), D4 (`sort`/`gsort` recusam wildcard com um só match),
     D5b (mensagem de `sql, clear` com o nome comprometido), D5c (célula `%`
     órfã no `summarize, detail`).
   - **Changed:** D3 (mensagem do `use` lazy alinhada com o help), D5a
     (import de bridges silencioso no sucesso), D6 (`describe` não-Parquet
     com erro didático).
   - **Added:** v67, flag `ExprResult::uses_rowctx`.
2. `ASSUMPTIONS.md`: entrada única a registar (a) a decisão de recusar em vez
   de implementar `_n`/`_N` nos filtros read-only, com o enhancement futuro
   apontado (rowctx_wrap no caminho de stats/preview exigiria rever
   parqit.sthlp §Expressions, v66 e v67), e (b) o resultado da verificação
   `quietly`×`SF_error` da Tarefa 5.
3. `docs/audits/README.md` (autorizado nesta tarefa): na tabela de
   relatórios, linha para
   `AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md` (auditoria do help, 6
   correções documentais + D1–D5 registados) e para o registo de remediação
   novo; na lista de prompts, linha para este ficheiro. Verifica antes que o
   maintainer não as acrescentou já — não duplicar.
4. Escreve `docs/audits/PARQIT_HELP_AUDIT_REMEDIATION_2026-08-08.md`
   (português europeu, padrão de
   `PARQIT_AUDIT_REMEDIATION_2026-07-14.md`): por defeito — sintoma, causa,
   correção aplicada com ficheiros/linhas, prova (teste + rc), e o inventário
   de validação da Tarefa 10.

## Tarefa 10 — fecho

1. Correr TUDO e colar os resumos no registo de remediação:
   `cmake --build build/dev -j` (recompila o plugin alterado),
   `ctest --preset dev`,
   `STATA=stata-mp BUILD_DIR=$PWD/build/dev bash tests/run_stata.sh`
   **integral** (o runner exige ≥1 PASS e 0 FAIL por log — inclui v66
   apertado e v67 novo),
   `bash tests/release_lint.sh`,
   `cmp -s src/ado/p/parqit.sthlp ado/plus/p/parqit.sthlp`.
2. `git status --short` final: só os ficheiros do âmbito autorizado; o
   trabalho preexistente da auditoria do help intacto (verifica por diff que
   os hunks dela não foram tocados).
3. Commits atómicos por tarefa
   (`fix: refuse _n/_N cleanly in count if/list if (D1)`, …). Sem push, tag
   ou release; reportar com precisão o que mudou, o que não mudou e como
   reverter.

## Critérios globais de aceitação

- Suite Stata integral verde (incluindo v66 apertado e v67 novo), ctest
  verde, release_lint verde, cópia instalada sincronizada.
- Nenhum nome interno (`__PARQIT_*`, tempnames `__00…`, caminhos de bridge)
  visível em nenhuma mensagem de sucesso ou erro dos casos cobertos.
- Nenhuma falha ficou mais silenciosa do que estava: todos os caminhos de
  erro tocados continuam a produzir rc ≠ 0 **e** mensagem visível — provado
  por teste, não por leitura.
- `parqit.sthlp` continua verdadeiro sem alterações (a única exceção
  admissível é a frase da mensagem da Tarefa 8, se optares por citá-la).
- Qualquer desvio deste prompt (facto divergente, custo inaceitável, decisão
  de design) fica registado em `ASSUMPTIONS.md` — nunca resolvido em
  silêncio.
