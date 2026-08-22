# PROMPT — Correção e melhoria do parqit (achados da auditoria de 2026-08-08)

> Prompt de implementação para um agente de código de IA (Claude Code / Codex).
> Copiar integralmente como instrução da tarefa. Fonte dos achados:
> [AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-08.md](AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-08.md).

---

És um engenheiro de correção a trabalhar no repositório `parqit`
(`~/Documents/GitHub/parqit`). Lê primeiro `CLAUDE.md` e
`parqit_build_prompt.md` e obedece-lhes; em particular:

- **Regra de não-regressão**: nunca remover funcionalidade, reduzir precisão,
  corromper metadados, enfraquecer um caminho de erro, nem mudar semântica
  pública em silêncio. Correção primeiro; desempenho só depois de os testes
  de fidelidade passarem.
- **Toda a alegação de paridade com o Stata é verificada executando o Stata
  nativo** (`stata-mp` local) antes de entrar em código, comentário, teste ou
  ASSUMPTIONS — o achado F2 desta ronda nasceu de uma claim "verificada" que
  era falsa e ficou protegida por um teste unitário no sentido errado. Se o
  nativo contradisser este prompt, o nativo ganha; regista a discrepância em
  `ASSUMPTIONS.md`.
- Cada defeito corrigido ganha: (a) um repro mínimo em `audit_repro/`,
  (b) um teste pinado na `tests/verify_suite/` com `VERDICT(...): PASS/FAIL`
  e oráculo independente (nativo e/ou pyarorw/duckdb CLI), (c) entrada no
  `CHANGELOG.md` (secção `[Unreleased]` → release), e quando aplicável
  (d) atualização de `ASSUMPTIONS.md` e da ajuda `parqit.sthlp`.
- Fluxo de validação obrigatório no fim de cada tarefa e no fim de tudo:
  `cmake --build build/dev -j` → `ctest --preset dev` →
  `bash tests/run_stata.sh <filtro>` (e integral no fim) →
  `bash tests/release_lint.sh`. Nada se declara "feito" sem output real.
- Um branch de feature (p.ex. `fix/audit-2026-08-08`), Conventional Commits,
  nunca cometer estado que parta o build. Não fazer push nem abrir PR sem
  instrução explícita do maintainer.

Implementa as tarefas pela ordem dada (da mais grave para a menos). Os números
F# referem-se ao relatório da auditoria.

---

## Tarefa 1 (F2, severidade Alta) — `strpos(s, "")` deve devolver 1 (0 só com haystack vazio)

**Facto nativo verificado em 2026-08-08 (StataNow MP):**
`strpos("ab","")` → **1**; `strpos("","")` → **0**. O DuckDB dá 1 em ambos.
O parqit devolve hoje 0 sempre — valores errados com rc 0.

1. Em `src/engine/exprtrans.cpp`, no handler de `strpos` (guard comentado
   `STRPOS-EMPTY-1`, ≈ linha 1111): substitui o ramo da agulha vazia
   `THEN 0` por `THEN (CASE WHEN <s> = '' THEN 0 ELSE 1 END)`, onde `<s>` é a
   expressão já coalescida do haystack (`coalesce(arg0, '')`). Mantém intacto
   o resto (agulha não encontrada → 0; conversão para offset em bytes).
   Reescreve o comentário com o facto nativo correto e a data da verificação.
2. `tests/unit/test_exprtrans.cpp:105` — corrige o CHECK para o comportamento
   nativo (linha 2 do dataset de teste tem `s = "b"`, logo esperar `"1"`) e
   acrescenta um caso com haystack vazio → `"0"`.
3. `ASSUMPTIONS.md` (entrada ≈ linha 625 "strpos(s,\"\") -> 0"): corrige o
   texto, marca a claim antiga como errada e data a re-verificação.
4. Novo teste Stata `tests/verify_suite/v61_expr_native_oracle.do` — ver
   Tarefa 7 (o oráculo table-driven cobre este caso; garante que inclui
   `strpos(s,"")` com haystack vazio e não-vazio).
5. Repro mínimo `audit_repro/repro_strpos_empty_needle.do` (gen via parqit vs
   gen nativo, assert de igualdade).

**Aceitação:** `parqit gen double h = strpos(s, "")` seguido de `collect`
iguala o `gen` nativo em todas as linhas (1 quando `s != ""`, 0 quando
`s == ""`); unit tests e suite verdes.

## Tarefa 2 (F1, severidade Média) — crash Mata 3301 em `parqit list if` com " in " na expressão

**Causa:** em `src/ado/p/parqit.ado`, `_parqit_split_in()` usa
`if (cols(t) == 2 & strtoreal(t[1]) != . & strtoreal(t[2]) != .)`; em Mata o
`&` **não** curto-circuita, logo com `cols(t) == 1` o `t[2]` aborta com 3301.
Repro: `parqit list if s == "a in 3"`.

1. Reestrutura a função para nunca indexar além de `cols(t)`:
   ```
   if (cols(t) == 2) {
       if (strtoreal(t[1]) != . & strtoreal(t[2]) != .) { ...split f/l... }
   }
   else if (cols(t) == 1) {
       if (strtoreal(t[1]) != .) { ...split f/f... }
   }
   ```
   e protege o caso de cauda vazia (`tailpart == ""`) antes do `ustrsplit`.
2. Enquanto lá estás, verifica que um `" in "` dentro de um literal seguido de
   range válido continua a comportar-se de forma correta e documentada: para
   `s == "a in 3/4"` o splitter atual não separa (cauda `4"` não numérica) —
   mantém esse comportamento e acrescenta-o ao teste como caso de guarda.
3. Repro `audit_repro/repro_list_in_literal.do`; teste
   `tests/verify_suite/v62_list_if_in_literal.do`: (a)
   `parqit list if s == "a in 3"` devolve exatamente 1 linha, rc 0;
   (b) `parqit list if strpos(s, " in ") > 0` rc 0; (c)
   `parqit list if x == 5 in 1/2` continua a fatiar como antes.

**Aceitação:** os três casos acima com rc 0 e resultados iguais ao nativo
(`list` sobre os dados colecionados); suite verde.

## Tarefa 3 (F3, severidade Média) — `gen`/`egen` com tipo `float` explícito e valor fora da gama float: missing, não rc 920

**Facto nativo verificado:** `gen float f = 1e300` e `= exp(700)` → rc 0,
`f = .`. No parqit hoje o `collect`/`save` aborta com
`Conversion Error … can't be cast … FLOAT` (rc 920), porque
`coerce_storage()` (em `src/engine/view.cpp`) emite `CAST(v AS FLOAT)` nu e o
DuckDB erra num double→float fora de gama em vez de saturar.

1. Em `coerce_storage()`, ramo `StType::Float`: emite um guard de gama no
   estilo do ramo inteiro, usando o limite já existente `kStataFloatMax`
   (`typemap.hpp`):
   ```
   (CASE WHEN (v) IS NULL THEN NULL
         WHEN abs(CAST((v) AS DOUBLE)) > <kStataFloatMax via dtoa> THEN NULL
         ELSE CAST((v) AS FLOAT) END)
   ```
   Nota: `v` repete-se — aceitável a um nível (o ramo inteiro já repete
   `trunc(v)` três vezes); NÃO uses o idioma CASE para operandos aninhados de
   aritmética (regra do `parqit_finite`).
2. Confirma que o mesmo caminho serve o `egen <fcn>, …` com tipo `float`
   (passa por `coerce_storage`) e que o `replace` de coluna float continua
   protegido pela promoção TYPE-007 (não mexer).
3. Verifica um efeito lateral desejado: com o guard, o bind probe deixa de
   poder rebentar e o erro desaparece também do `parqit save` lazy.
4. Repro `audit_repro/repro_gen_float_overflow.do`; teste
   `tests/verify_suite/v63_gen_float_range.do`: `gen float` de `1e300`,
   `exp(700)`, `-1e39`, e um valor DENTRO da gama (`1e38`) — os três primeiros
   colecionam como missing (igual ao nativo), o último preserva o valor com
   arredondamento float32 (compara com o nativo via `assert`), e o tipo
   colecionado é `float`.

**Aceitação:** paridade exata com o nativo nos quatro casos, em `collect` e em
`save`+`use`; suite verde.

## Tarefa 4 (F4) — literais `.a`–`.z` em expressões: tornar o comportamento honesto e documentado

Hoje `x == .a` traduz para `x IS NULL` (≡ `x == .`): numa vista com um
missing, `count if x == .a` dá 1 onde o nativo dá 0. Nada disto está na ajuda.

**Decisão recomendada (implementa-a, salvo contraordem do maintainer):**
rejeitar com erro claro qualquer literal de missing estendido usado em
comparação/expressão — uma vista lazy nunca contém `.a`–`.z` (Parquet tem um
único missing), logo a pergunta é irrespondível e a resposta silenciosa é
sempre errada para alguém:

1. Em `src/engine/exprtrans.cpp`, no lexer/parser do `MissingDot`: mantém `.`
   como hoje; para `.a`–`.z` devolve erro anchored:
   `"extended-missing literals (.a-.z) cannot be tested in a lazy view: the
   Parquet boundary collapses all extended missings to a single missing; test
   missing(x) or compare with . instead"`.
2. Atualiza `parqit.sthlp` (secção *Missing-value semantics*) e o parágrafo de
   limitações do README para dizer explicitamente o que acontece a `.a`–`.z`
   em expressões; regista a decisão em `ASSUMPTIONS.md`.
3. Teste `tests/verify_suite/v64_extended_missing_literals.do`: `keep if
   x == .a` e `gen y = (x != .b)` falham com a mensagem nova e rc ≠ 0; os
   idiomas com `.` continuam intactos (`x == .`, `x < .`, `x >= .`).
4. Confirma que nenhum teste existente usa `.a` em expressões parqit (usa
   `grep` sobre `tests/` e `examples/`); se usar, ajusta com a alternativa
   documentada.

Se o maintainer preferir a alternativa mínima (manter ≡ `.` e apenas
documentar), implementa só os passos 2–3 adaptados e regista a escolha.

## Tarefa 5 (F5) — wildcards de varlist no caminho eager (`use`, `mergein keepusing()`, `appendin keep()`)

Hoje `parqit use id x* using f.parquet[, clear]` e
`parqit mergein …, keepusing(x*)` falham com rc 101 `using not allowed`
(o `syntax [namelist]` rejeita `*` e o fallback re-parseia mal). O nativo
aceita; o caminho lazy do parqit também (keep/drop/order e o `keepusing()` do
merge lazy já expandem wildcards).

1. Em `_parqit_use` (`src/ado/p/parqit.ado`): troca `syntax [namelist] using/`
   por uma forma que aceite tokens com `*`/`?` (p.ex.
   `syntax [anything(name=vlist)] using/ …`), preservando o fallback do
   filename atual. Passa os tokens tal-e-qual no request.
2. Em `plan_columns()` (`src/plugin/plugin_io.cpp`), na seleção por varlist:
   quando um token contém `*`/`?`, expande-o contra os `stata_name` dos planos
   pela ordem do padrão, deduplicado — reutiliza o `glob_match` de
   `view.cpp` (move-o para um header partilhado do engine, p.ex.
   `sanitize.hpp`, em vez de o duplicar). Padrão sem correspondência → erro
   claro (`variable x* not found in the file(s)`, rc 111), como o nativo.
3. `_parqit_mergein`/`_parqit_appendin` beneficiam automaticamente (delegam
   no `use` eager) — confirma e testa; a mensagem do passo 2 tem de citar o
   padrão original.
4. Teste `tests/verify_suite/v65_eager_varlist_wildcards.do`: eager `use` com
   `x*`, lazy `use using` com `x*` (já deve passar após o passo 1 — a
   `keep_vars` expande), `mergein keepusing(x*)`, `appendin keep(x*)`, mais o
   caso negativo `parqit use z* using …` → rc 111 com mensagem citando `z*`.

**Aceitação:** os quatro positivos igualam o nativo (`use`/`merge`/`append`
sobre um `.dta` twin); o negativo é ruidoso e claro; suite verde.

## Tarefa 6 (F6) — determinismo de fatias `keep in`/preview com empates no sort

Fatias (`keep in`, `head`, `list in`) sobre um `sort` com chaves empatadas
compilam para `ORDER BY <chaves> LIMIT/OFFSET`; a ordem entre empates é
não-contratual e cada materialização re-executa o plano.

1. Em `View::keep_in()` (`src/engine/view.cpp`): no `ORDER BY` **interno** da
   fatia (o subquery com LIMIT/OFFSET), acrescenta como desempate as
   restantes colunas do manifesto, `NULLS LAST`, à imagem de TT-A1/COLLAPSE-3.
   **Não** alteres `order_by_sql()` global (não perturbar `_n`, merge spine,
   nem a ordem final de materialização).
2. Faz o mesmo na fatia de preview de `cmd_view_collect_prepare`
   (`plugin_view.cpp`, ramo `pf/pl`) — o `LIMIT/OFFSET` ali aplicado sobre
   `compile(true)` precisa do mesmo desempate; implementa-o compondo o SQL da
   fatia com o `ORDER BY` estendido em vez do `compile(true)` simples.
3. Documenta no `parqit.sthlp` (secção do `sort`/`in`) que fatias com empates
   são desempatadas de forma reproduzível por todas as colunas.
4. Teste `tests/verify_suite/v66_slice_tie_determinism.do`: ficheiro com 10k
   linhas, chave com muitos empates; `sort chave` + `keep in 1/100` +
   `collect` duas vezes na mesma sessão e uma vez após `discard`+reload —
   payloads byte-iguais (compara via `cf` com um save intermédio); e o mesmo
   para `list in`.

Se o custo do desempate total se revelar mensurável em ficheiros largos,
regista os números e discute antes de otimizar; correção primeiro.

## Tarefa 7 — oráculo de paridade de funções, table-driven (prevenção estrutural)

Motivação: F2 só existiu porque uma claim nativa errada ficou protegida por um
teste unitário. A partir de agora a paridade escalar tem um oráculo vivo.

1. Cria `tests/verify_suite/v61_expr_native_oracle.do`: gera um dataset
   pequeno (numéricos com missing/negativos/fracionários; strings com "",
   multibyte, espaços), grava via `parqit save`, abre a vista e, para uma
   TABELA de expressões (uma linha = uma expressão), faz `parqit gen r# =
   <expr>` + `collect` e compara com `gen` nativo campo a campo
   (`assert r# == n#` com tolerância 0; para strings, igualdade exata).
2. Popular a tabela com, no mínimo: `strpos(s,"")` (haystack vazio e não),
   `strpos`/`substr` multibyte, `subinstr(s,"","X",.)`, `mod(x,0)`,
   `mod(x,-3)`, `round(x)`, `round(x,-2)`, `round(x,0)`, `2^3^2`, `-x^2`,
   `x/0`, `exp(800)`, `ln(0)`, `sqrt(-1)`, `min(x,.)`, `max(.,.)`,
   `inrange(.,1,5)`, `inlist(x,1,.)`, `cond(x,1,2)` e `cond(x,1,2,9)` com x
   missing, `int(-2.7)`, `floor/ceil` negativos, `real("inf")`, `string(x)`
   para inteiro/fração/1e-5/missing, `td(29feb2020)` e `mdy(2,30,2020)`,
   `dow`/`doy`/`mofd` com dia fracionário. Corre a tabela nos DOIS modos
   (`statamissing off` e `on`) onde a semântica documentada coincidir; onde
   divergir por contrato (comparações em modo SQL), marca a linha como
   only-statamissing.
3. O teste imprime cada expressão falhada com os dois valores; VERDICT único.
4. Referencia este teste nos comentários de `exprtrans.cpp` como o local
   canónico para acrescentar casos quando se tocar num handler de função.

## Tarefa 8 — melhorias de documentação e acabamento

1. `parqit.sthlp`, secção performance: nota sobre `memory_limit` (por omissão
   o DuckDB reserva ~80% da RAM; em nós HPC partilhados recomendar
   `parqit set memory_limit` explícito) e sobre o desempate de fatias
   (Tarefa 6.3).
2. `parqit.sthlp`, funções de string: listar explicitamente que
   `ustrpos`/`usubstr` não são suportadas (erro claro) e que
   `strpos`/`substr` são byte-based como as nativas homónimas.
3. `docs/audits/README.md`: acrescenta as linhas do relatório
   2026-08-08 e deste prompt nas tabelas respetivas (se o auditor ainda não o
   tiver feito — verifica antes de duplicar).
4. `CHANGELOG.md`: entradas Fixed (F1, F2, F3), Changed (F4 se rejeição,
   F6), Added (v61–v66, wildcards F5) na secção `[Unreleased]`; ao cortar o
   release, mover para `[0.1.24]` com data.

## Tarefa 9 — fecho e release

1. Correr TUDO: `cmake --build build/dev -j`, `ctest --preset dev`,
   `bash tests/run_stata.sh` integral, `bash tests/release_lint.sh` — colar o
   resumo de veredictos no output final da tarefa.
2. Bump para `0.1.24` em todas as superfícies sincronizadas (CMakeLists,
   banner do ado, sthlp, dialogs, README (Status + exemplo net install),
   CLAUDE.md, parqit.pkg Distribution-Date, CITATION.cff) — o
   `release_lint.sh` é o juiz.
3. NÃO fazer `git push`, tag ou release sem instrução explícita do
   maintainer; deixar o branch local pronto com commits atómicos por tarefa
   (`fix: strpos empty needle parity (F2)`, etc.) e reportar com precisão o
   que mudou, o que não mudou e como reverter.

## Critérios globais de aceitação

- Suite Stata integral verde (incluindo v61–v66 novos), ctest verde,
  release_lint verde.
- Nenhuma regressão de semântica pública fora das corrigidas e documentadas.
- Cada F# encerrado tem repro em `audit_repro/`, teste pinado, CHANGELOG e,
  quando toca em paridade, evidência de execução nativa no próprio teste.
- Qualquer desvio deste prompt (facto nativo divergente, custo inaceitável,
  decisão de design) fica registado em `ASSUMPTIONS.md` — nunca resolvido em
  silêncio.
