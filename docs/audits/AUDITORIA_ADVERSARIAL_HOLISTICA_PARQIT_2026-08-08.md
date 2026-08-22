# Auditoria adversarial holística do parqit — 2026-08-08 (PT)

| | |
|---|---|
| **Alvo** | `parqit` v0.1.23, commit `e9815a4` (branch `main`, árvore limpa) |
| **Data** | 2026-08-08 |
| **Auditor** | Claude (Fable 5), sessão interativa na máquina de desenvolvimento |
| **Ambiente** | Linux EL9 x86_64 · StataNow MP (`/usr/local/stata/stata-mp`) · DuckDB embebido 1.5.3 (pinned) · build `dev` (RelWithDebInfo) |
| **Método** | Leitura integral do código (≈14 000 linhas: `src/engine`, `src/plugin`, `src/ado` + Mata, CMake, runner, lint) · execução das três suites oficiais · sondas ao motor DuckDB · **verificação oracle-first**: cada alegação de paridade foi executada contra o Stata nativo nesta máquina antes de ser classificada (lição PQ-AUD-003: nunca confiar em claims "verificados" sem os re-executar) |

## 1. Sumário executivo

O parqit está num estado de engenharia **excecional para um package Stata**: o
disciplinamento de erros (rc ≠ 0 + mensagem em todos os caminhos), a troca
atómica validate-then-mutate, a escrita Parquet verificada e transacional, o
protocolo hex anti-injeção, o manifesto de colunas nome-a-nome e a suite de
verificação (≈80 veredictos, todos PASS neste commit) são raros mesmo em
software comercial. A baseline completa — `ctest` (3/3), `release_lint`,
`tests/run_stata.sh` integral incluindo os testes de concorrência x01/x02 —
está **verde** neste ambiente.

Ainda assim, a auditoria encontrou **3 defeitos confirmados por reprodução**
(nenhum coberto pela suite atual), 2 lacunas de paridade/documentação e 1
risco residual documentável. O mais grave (F2) produz **valores errados em
silêncio** — a classe de falha que a carta de correção do projeto declara
inadmissível — e nasceu precisamente de uma alegação "verificada nativamente"
que era falsa e foi fixada por um teste unitário no sentido errado. Nenhum dos
achados compromete a integridade dos dados em uso típico; todos têm correção
local e barata.

**Veredicto: GO condicionado** — aplicar F1–F3 (e decidir F4) antes do próximo
release; F5–F6 são melhorias.

## 2. Baseline verificada (nada assumido)

| Verificação | Resultado |
|---|---|
| `cmake --preset dev && cmake --build build/dev -j` | OK (incremental) |
| `ctest --preset dev` (unit, runner_no_match, unit_concurrent) | 3/3 PASS |
| `bash tests/release_lint.sh` | OK — v0.1.23, datas coerentes, manifesto .pkg coerente com o workflow |
| `bash tests/run_stata.sh` (integração + verify v02–v60 + roundtrip + x01/x02 concorrentes) | **exit 0, ~80 veredictos PASS, 0 FAIL** |

## 3. Achados

Severidades: **A**lta (valor errado silencioso ou perda de dados), **M**édia
(comportamento errado ruidoso, crash, divergência de paridade), **B**aixa
(usabilidade/documentação), **I**nfo (risco residual a documentar).

### F2 [A] `strpos(s, "")` devolve 0; o Stata nativo devolve 1 — valores errados em silêncio

- **Evidência nativa** (executada nesta máquina): `strpos("ab","")` → **1**;
  `strpos("","")` → **0**. O DuckDB devolve 1 em ambos.
- **parqit**: devolve **0 sempre** que a agulha é vazia — o guard
  `STRPOS-EMPTY-1` em `src/engine/exprtrans.cpp` (≈ linha 1111) força 0 com
  base no comentário "Stata strpos(s,'') == 0", que é **falso**. O erro está
  fixado no sentido errado em `tests/unit/test_exprtrans.cpp:105` e registado
  em `ASSUMPTIONS.md:625`.
- **Impacto**: `parqit gen hit = strpos(var, "…")` com agulha vazia — cenário
  realista quando a agulha vem de um macro que expande vazio num loop —
  devolve 0 onde o nativo devolve 1, **sem qualquer erro ou aviso** (rc 0).
  É a classe de defeito que o brief §5 declara inadmissível.
- **Reprodução**: `parqit gen double sp = strpos(s, "")` + `collect` → sp = 0;
  nativo `gen sp = strpos(s, "")` → 1 para s ≠ "".
- **Correção**: agulha vazia → `CASE WHEN coalesce(s,'') = '' THEN 0 ELSE 1 END`;
  corrigir o teste unitário, a entrada em ASSUMPTIONS e acrescentar teste de
  paridade nativa na verify suite.
- **Meta-lição** (raiz do defeito): uma claim nativa errada entrou no código e
  o teste unitário passou a protegê-la. A prevenção estrutural é um teste
  oracle table-driven (ver §5.1).

### F1 [M] `parqit list if …` aborta com erro Mata 3301 quando a expressão contém " in " sem range válido

- **Causa raiz**: em `src/ado/p/parqit.ado`, `_parqit_split_in()`:
  ```
  if (cols(t) == 2 & strtoreal(t[1]) != . & strtoreal(t[2]) != .) {
  ```
  Em Mata, `&` **não faz curto-circuito**: com `cols(t) == 1`, `t[2]` é
  avaliado na mesma → 3301 "subscript invalid" e o comando aborta com rc
  errado e sem mensagem útil.
- **Reprodução** (confirmada): `parqit list if s == "a in 3"` →
  `_parqit_split_in(): 3301 subscript invalid`, rc 3301. Qualquer filtro cujo
  texto contenha " in " sem cauda `f/l` numérica dispara — p.ex.
  `parqit list if strpos(nota, " in ") > 0`.
- **Impacto**: crash ruidoso num comando de exploração legítimo; rc espúrio.
- **Correção**: aninhar as condições (testar `cols(t)` antes de indexar) e
  proteger também a cauda vazia; repro mínimo para `audit_repro/`.

### F3 [M] `gen float x = <valor fora da gama float>` aborta na materialização com erro cru do motor; o nativo guarda missing em silêncio

- **Evidência nativa**: `gen float f = 1e300` e `gen float f = exp(700)` →
  rc 0, `f = .` (missing silencioso).
- **parqit**: o verbo `gen` aceita (rc 0 — o bind probe LIMIT 0 não avalia),
  e o `collect`/`save` aborta com rc 920 e a mensagem crua
  `Conversion Error: Type DOUBLE with value 1e+300 can't be cast … FLOAT`
  incluindo o SQL interno — longe do comando que causou o problema.
- **Causa raiz**: em `src/engine/view.cpp`, `coerce_storage()` trata os alvos
  inteiros com guard de gama (`trunc(v) < lo … THEN NULL`, EXPR-1) mas o alvo
  `float` usa um `CAST(v AS FLOAT)` nu — e o DuckDB, ao contrário do IEEE,
  **erra** em vez de saturar num double→float fora de gama. O `replace` numa
  coluna float está protegido (o bind probe promove a intenção para double,
  TYPE-007); apenas `gen`/`egen` com tipo `float` explícito estão expostos.
- **Impacto**: pipeline que no nativo corre (com missing) aborta no parqit,
  com erro deslocado e não-Stata; incoerência interna com o contrato EXPR-1
  (byte/int/long → missing; float → aborto).
- **Correção**: guardar o ramo float como os inteiros:
  `CASE WHEN v IS NULL THEN NULL WHEN abs(v) > 1.70141173319e38 THEN NULL ELSE CAST(v AS FLOAT) END`
  (limite = `kStataFloatMax` já existente em `typemap.hpp`).

### F4 [B/M] Literais de missing estendido em expressões: `x == .a` ≡ `x == .` — divergência não documentada

- **Evidência**: `parqit count if x == .a` sobre uma vista com um missing
  devolve **1**; o nativo sobre os mesmos dados devolve **0** (`.` ≠ `.a`).
- **Estado**: o tradutor colapsa `.a`–`.z` para NULL (comentário "documented"),
  mas a ajuda documenta apenas o colapso **no armazenamento** e os idiomas com
  `.`; o significado dos literais `.a`–`.z` em expressões não está documentado
  em lado nenhum, e o resultado divergente é silencioso.
- **Recomendação** (coerente com a carta "loud, never guess"): como uma vista
  nunca contém `.a`–`.z` (o Parquet tem um único conceito de missing), a
  comparação com um literal estendido é **irrespondível** — rejeitar com erro
  claro ("extended-missing categories do not exist in a lazy view; test
  `missing(x)` or `x == .`") é mais honesto do que igualar a `.`
  silenciosamente. Alternativa mínima: manter ≡ `.` e documentar na ajuda.
  Decisão a registar em `ASSUMPTIONS.md`.

### F5 [B] Wildcards de varlist não aceites no `use` eager/lazy nem em `mergein keepusing()`/`appendin keep()`

- **Evidência**: `parqit use id x* using f.parquet[, clear]` e
  `parqit mergein 1:1 id using f.parquet, keepusing(x*)` → rc 101 com a
  mensagem crua `using not allowed` (o `syntax [namelist]` rejeita `*` e o
  fallback re-parseia mal). O nativo aceita wildcards em ambos os contextos.
  Nota: o merge **lazy** já suporta wildcards em `keepusing()` (via
  `glob_match`), e os verbos `keep`/`drop`/`order` também — a lacuna é só na
  seleção de colunas do caminho eager e no que dele deriva (`mergein`/`appendin`).
- **Impacto**: falha ruidosa mas críptica; assimetria com o nativo e com o
  próprio parqit lazy.
- **Correção**: aceitar os tokens no ado (`anything` em vez de `namelist`) e
  expandir os padrões contra os nomes Stata no `plan_columns()` (reutilizar o
  `glob_match` já existente), com erro claro para padrão sem correspondência.

### F6 [I] Determinismo de fatias com chaves de ordenação empatadas, entre materializações repetidas

- Um `parqit sort chave` com empates seguido de `keep in 1/5` (ou `head`,
  `list in`) compila para `ORDER BY … LIMIT`; a ordem **entre empates** é
  não-contratual no motor, e cada `collect` re-executa o plano — duas
  materializações do mesmo view podem, em teoria, devolver linhas diferentes
  na fronteira da fatia (no nativo o `sort` reordena uma vez e o resultado
  fica fixo). Os pontos já endurecidos (COLLAPSE-3, TT-A1, m:m) mostram o
  padrão de solução: desempate total por todas as colunas, NULLS LAST.
  A sonda desta auditoria não observou instabilidade em prática (mesmo plano,
  mesma sessão), mas nada a garante entre sessões/versões/threads.
- **Recomendação**: aplicar o desempate total no `ORDER BY` interno do
  `keep_in` e da fatia de preview do `collect_prepare` (não no `order_by_sql()`
  global, para não perturbar `_n`/merge), ou documentar explicitamente o
  não-determinismo entre empates como limitação.

## 4. Alegações verificadas como corretas (amostra do que foi ativamente atacado sem achado)

Cada linha foi confirmada por execução nesta sessão, contra o nativo e/ou o
motor:

- `mod(7,0)` = `.` e `mod(7,-3)` = `.` no nativo — o guard `b <= 0 → NULL`
  do parqit **está certo** (o comentário no código que contradiz o manual da
  Stata é correto; o manual está desatualizado).
- `round(7,-2)` = 6 no nativo — a fórmula `floor(x/u + 0.5)*u` do parqit
  reproduz exatamente o caso de unidade negativa; `round(2.5,0)` = 2.5
  (pass-through) idem; paridade confirmada ponta-a-ponta (probe I).
- `2^3^2` = 64 e `-2^2` = -4 — associatividade esquerda do `^` e precedência
  do menos unário corretas no parser.
- `least/greatest` do DuckDB ignoram NULL — `min(1,.)` = 1, `max(.,.)` = `.`
  em paridade com o nativo.
- `statamissing on`: `count if x > 5` com missing e `gen y = x > 5`
  reproduzem o nativo (probe D).
- `%tC` round-trip (BIGINT em disco, formato preservado, diferenças de 1000ms
  exatas) — probe G.
- `summarize, detail` com n=7: p25/p50/p75/p90, skewness e kurtosis
  byte-iguais ao nativo (probe H) — a regra exata de percentis e os momentos
  centrais em duas passagens estão certos.
- `subinstr(s,"","X",.)` → s inalterado, como o nativo (SUBINSTR-NULL-1 ok).
- `substr` com comprimento negativo/posição negativa; truncagem byte a byte
  com U+FFFD; `strpos` com needle multibyte (offset em bytes) — ok.
- `gen byte x = 3.9/-2.5/200` → 3/-2/missing; `egen byte t = total(…)` fora
  de gama → missing — EXPR-1/EGEN-STORAGE-1 em paridade exata.
- `collapse (sum)` de grupo todo-missing → 0, como o nativo.
- Suite completa verde, incluindo: injeção hostil (v16), 2500 vars (v18),
  fronteira strL 2045/2046 e strL de 1MB (v19), fill paralelo vs oracle
  pyarrow (v20), NaN/±Inf/limites float32 (v15), globs de esquema misto
  recusados (v48), estatísticas de footer forjadas recusadas (v49), NUL em
  nomes de coluna recusado (v50), UTF-8 inválido recusado nos dois caminhos
  de escrita (v32/v52), atomicidade do `collect` (v09) e do publish/rollback
  de outputs (v59, x02), bridges entre processos concorrentes (x01).
- Higiene de release: versões/datas sincronizadas em 7 superfícies e
  validadas pelo lint; manifesto `.pkg` coerente com os binários que o CI
  constrói; caminho de leak de paths pessoais bloqueado.

## 5. Recomendações de melhoria (além das correções)

1. **Oráculo de paridade de funções, table-driven** (prevenção estrutural do
   padrão F2): um verify-teste que percorre uma tabela de expressões-limite
   (`strpos` com agulha vazia, `mod` 0/negativo, `round` unidade negativa,
   `cond` 4-arg, `inlist`/`inrange` com missing, potências, datas fracionárias,
   …), calcula cada uma via `parqit gen` e via `gen` nativo sobre os mesmos
   dados e exige igualdade campo a campo. Qualquer futura "correção de
   paridade" passa a ter de vencer o oráculo vivo, não um comentário.
2. **Determinismo de fatias** (F6) — desempate total nos `ORDER BY` internos
   de `keep_in`/preview, ou documentação explícita.
3. **Mensagem/gestão de memória em HPC partilhado**: o DuckDB usa por omissão
   ~80% da RAM da máquina como `memory_limit`; num nó de login partilhado
   isso é agressivo. Documentar na ajuda (secção performance) a recomendação
   `parqit set memory_limit` para ambientes partilhados.
4. **`ustrpos`/variantes Unicode**: continuam por implementar (erro alto e
   claro — correto); listar explicitamente na ajuda ao lado de
   `strpos`/`substr` byte-based, para que a assimetria com `ustrlen` (que
   existe) seja visível.
5. Manter a disciplina de `audit_repro/`: cada achado desta auditoria deve
   ganhar o seu repro mínimo e o seu teste pinado (v61+), como as auditorias
   anteriores.

## 6. Riscos residuais (fora do âmbito desta sessão)

- **Plataformas**: tudo o que aqui foi executado correu em Linux x86_64; os
  binários macOS/Windows são cobertos pelo CI de build mas as suites Stata só
  correm em máquinas licenciadas — o risco residual por plataforma mantém-se
  o documentado na certificação v0.1.22.
- **Fill paralelo via SPI**: a reentrância de `SF_vstore`/`SF_sstore` para
  células disjuntas apoia-se no precedente de produção do `pq`; o escape
  `PARQIT_FILL_THREADS=0` existe e está documentado. Sem alteração.
- **`parqit sql`** executa SQL arbitrário no motor embebido (incluindo
  `COPY TO`) — é o escape hatch documentado e é poder local do utilizador,
  não uma superfície de ataque remota. Sem alteração.

## 7. Conclusão

Três defeitos reais e reproduzíveis (F1–F3) — um deles da classe mais séria
(valor errado silencioso, F2) — num package cuja infraestrutura de correção
está claramente acima da norma. As correções são locais, com testes de
regressão óbvios, e estão especificadas em
[PROMPT_CORRECAO_MELHORIA_PARQIT_2026-08-08.md](PROMPT_CORRECAO_MELHORIA_PARQIT_2026-08-08.md).
Depois de aplicadas e com a suite verde, o parqit fica, no julgamento deste
auditor, em condições de manter o estatuto GO da certificação de 2026-07-14.

---
*Evidência bruta: logs `runadv01.log`/`runadv02.log`/`native_probe.log`/`np2.log`
(scratch da sessão), resumo de veredictos da suite oficial (exit 0), sondas
`duckdb` CLI. Reproduções mínimas especificadas no prompt de correção.*
