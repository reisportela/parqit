# Auditoria adversarial de SAMPLE-DESIGN-1 (`parqit sample` com desenhos à maneira do `sample2`)

> Cópia arquivada do relatório do auditor. As sondas e os logs estão em
> `audit_repro/fable_sample_design_20260925/` (a pasta temporária citada abaixo já não existe).
> A resolução de cada achado está no fim.

- **Data:** 2026-09-25
- **Auditor:** Fable (independente; só leitura no repositório, sem compilação)
- **Alvo:** working tree de `feat/view-copy` sobre `3c1ca16` (v0.2.3), apenas a parte SAMPLE-DESIGN-1 (#166); plugin `build/viewcopy/parqit.plugin` (DuckDB 1.5.3 embutido); testes C++ `build/viewcopy/parqit_tests`.
- **Oráculos:** Stata nativo (`sample`), `sample2` 1.2.0 (Weesie), reimplementação Python da chave documentada (pyarrow), CLI `duckdb` (só para hipóteses; o que conta é o plugin).
- **Sondas e logs:** `/tmp/claude-1003/-home-mangelo-Documents-GitHub-parqit/8015a030-5853-46da-88c4-095f0cdae4f1/scratchpad/fable_audit2/` (`make_fixtures.py`, `p1_encodings.do`…`p7_reserved.do` e os `.log` respetivos; `v126run/parqit.log` é a reexecução do v126 a partir do scratchpad).

## Sumário

A funcionalidade está, no essencial, correta e coerente com a semântica declarada em #166. Verifiquei empiricamente, com este plugin, que: os clusters saem inteiros sem perda nem duplicação de linhas em todas as codificações (INTEGER, BIGINT, DOUBLE com `-0`/`+0` misturados, DECIMAL, DATE, TIMESTAMP, texto UTF-8); o sorteio é idêntico entre codificações e bate com uma reimplementação independente da chave; NaN, ±inf, ≥2^1023, NULL, `''` e `.a`/`.b` (vista xmissing) ficam fora da população e são mantidos com a nota certa; `if` + `strict`/`any`/`all` comportam-se como o `sample2` nos dois modos de `statamissing`, incluindo o caso em que a condição é NULL; um estrato NULL dentro de um cluster é recusado; as contagens por estrato seguem a regra #134 (e a divergência documentada de 0,3 % de 500 confirma-se: parqit 1, `sample`/`sample2` 2); `generate()` sozinho marca exatamente as linhas da forma percentual simples (sem sort, com sort, 1 vs 4 threads, glob de 2 ficheiros); o desenho numa vista copiada não toca na origem; o indicador grava como `int8`/`byte`. O v126 passa com este plugin e os 84 asserts C++ relevantes passam.

Encontrei **um achado de severidade média** (SD-1): `generate()`/`keep()` aceitam os nomes reservados `_n`/`_N`, que o Stata e os outros verbos lazy recusam; a coluna criada é depois inalcançável nas expressões (o tradutor lê `_n`/`_N` como contexto de linha), pelo que `sample 50, generate(_n)` seguido de `keep if _n == 1` devolve **1 linha** em vez da amostra, em silêncio. Os restantes achados são baixos ou notas (precisão de documentação, assimetria de validação, mensagens genéricas, lacunas de teste). Não encontrei nenhuma via de corrupção de dados nem de amostra errada fora de SD-1.

## Achados

### SD-1 — `generate()`/`keep()` aceitam `_n` e `_N`; a coluna fica inalcançável e um `keep if _n == 1` dá uma amostra errada em silêncio

- **Severidade:** médio
- **Estado:** CONFIRMADO
- **Evidência:**
  - `p5_names_ado.log` l. 262–264: `parqit sample 50, cluster(cid_i) seed(1) generate(_n)` → rc 0 (nota da população impressa, etapa adicionada). l. 314–324: `generate(_N)` → rc 0; `parqit collect` → rc 0 com `note: column "_N" loaded as __N` (renomeação com nota). l. 301–308: `parqit gen byte _N = 1` → `_N invalid name (reserved word)` rc 7; `parqit rename region _n` → idem. l. 333–335: nativo `gen byte _n = 1` → `_n invalid varname` rc 198.
  - `p7_reserved.log` l. 61–89 (sequência `_n`): `parqit ds` lista `i s _n` (l. 66); `parqit keep if _n == 1` → rc 0; `parqit count` → **1** (l. 79); `parqit gen double z = _n` → rc 0; `collect` → `(4 vars, 1 obs collected)` com a coluna `__n` (l. 89). l. 116–131 (sequência `_N`): `parqit sort _N` rc 0, `parqit keep if _N == 1` rc 0, `collect` → `(3 vars, 0 obs collected)` (l. 131).
- **Porquê médio e não alto/baixo:** o gatilho exige que o utilizador escolha um nome que o próprio Stata recusa em `gen`, logo é improvável na prática; a gravidade vem de o resultado ser uma amostra errada em silêncio (rc 0, sem nota até ao `collect`), o que a regra do maintainer não tolera — daí médio, não baixo.
- **Causa:** `_parqit_sample` (src/ado/p/parqit.ado) valida o nome só com `syntax [, … GENerate(name) KEEP(name)]`, que aceita qualquer nome sintaticamente válido, incluindo palavras reservadas; `_parqit_gen` e `_parqit_rename` fazem `confirm name` (que recusa `_n`, `_N`, `_all`, …). No engine, `View::sample_design` só verifica `col_index`/`ci_guard`; o tradutor (`exprtrans.cpp`) resolve `_n`/`_N` como contexto de linha antes de olhar para o esquema, logo a coluna criada nunca é lida por `keep if`, `gen`, `sort`.
- **Correção:** em `_parqit_sample`, logo depois de `if ("`keep'" != "") local generate `keep'`, acrescentar `if ("`generate'" != "") confirm name `generate'` (mesmo rc 7 e mensagem do Stata que `gen`/`rename` já dão). Defensivamente, em `View::sample_design` recusar `generate` igual a `_n`/`_N` (ou qualquer nome que `translate_filter` trate como contexto), para que o caminho do diálogo e futuras chamadas não dependam do ado.
- **Teste:** em v126, `capture noisily parqit sample 10, generate(_n)` → `assert _rc == 7`; idem `generate(_N)` e `keep(_n)`; e `assert r(n_steps)` inalterado.

### SD-2 — Inteiros acima de 2^53 colapsam na mesma chave; a ordem entre eles é por valor, não aleatória; o comentário do header é impreciso

- **Severidade:** baixo
- **Estado:** CONFIRMADO
- **Evidência:** `p1_encodings.log` l. 537: `9007199254740992` (2^53) e `9007199254740993` (2^53+1) têm a mesma chave `2919751125690537255` com `seed(7)`; l. 542 `ORACLE big : MATCH` (o oráculo Python com `float(x)` reproduz o sorteio, logo `__parqit_double` arredonda ao par como o Python). Nenhum erro, clusters inteiros (l. 393 `CHECK(big_rc): PASS`, l. 425–429 `N_big`, `big_whole`, `big_noNULLflag`: PASS).
- **Causa:** `src/engine/sample_key.hpp` `of_double` recebe o binary64 (`__parqit_double(c)` em `view.cpp` ~l. 1493), pelo que BIGINT/HUGEINT/DECIMAL acima de 2^53 perdem bits antes do hash. O comentário do header ("mix is a bijection, so distinct numbers never share a key") é verdadeiro para binary64 distintos, não para números distintos; o help técnico ("a number hashes its binary64 value") está certo. O desempate por `c` mantém o determinismo, mas entre clusters colididos a ordem é crescente por valor — dentro de um par colidido, o "menor" entra sempre primeiro.
- **Correção:** documentar em `parqit_technical.sthlp` e no header: "inteiros de magnitude ≥ 2^53 são ordenados pela sua aproximação binary64; os que coincidem desempatam pelo valor". Alternativa mais forte (opcional): uma variante `of_int64` para colunas inteiras usada só quando o valor não é exatamente representável em double — quebraria a invariância INTEGER↔DOUBLE apenas nesses valores; não a recomendo sem decisão do maintainer.
- **Teste:** doctest em `test_statistics.cpp`: `CHECK(of_double(0, 9007199254740993.0) == of_double(0, 9007199254740992.0))` com o comentário explicativo; e um caso v126/unit com dois clusters BIGINT colididos a verificar que o desempate é pelo valor.

### SD-3 — No caminho por linhas, um `keep in` pendente fora do intervalo não é detetado na altura do verbo (só o caminho com clusters valida)

- **Severidade:** nota
- **Estado:** CONFIRMADO
- **Evidência:** `p4_verb_time.log` l. 72–97: com `parqit keep in 1/1000` pendente numa vista de 135 linhas, `sample 30, cluster(cid_i)` → `parqit: in 1/1000 is out of range: only 135 observations at that step` (rc 198, plano inalterado); `sample 30, seed(7) generate(g)` → rc 0, `n_steps` passa de 1 para 2; `collect` → mesmo erro, rc 198.
- **Causa:** `sample_design_checks` (plugin_view.cpp) chama `validate_ranges` só quando há `cluster()`; o caminho por linhas não corre sonda nenhuma.
- **Correção:** ou chamar `validate_ranges` para qualquer desenho (uma contagem por intervalo pendente, barata) em `cmd_view_op` no ramo `op == "sample"`, ou deixar como está e dizer no help técnico que só o desenho com clusters valida os intervalos pendentes na altura do verbo. É coerente com os restantes verbos (validação na materialização), pelo que não é um defeito de integridade.
- **Teste:** v126: `keep in 1/100000` pendente + desenho por linhas → assert do rc escolhido.

### SD-4 — Compound quotes aninhadas no `if` falham no tradutor (pré-existente); a frase de #166 sobre o splitter é enganadora ponta-a-ponta

- **Severidade:** nota
- **Estado:** CONFIRMADO (pré-existente; não é de SAMPLE-DESIGN-1)
- **Evidência:** `p6_misc.log` l. 84–86: `parqit keep if txt == `"h1 `"a, b"' c"'` → `unexpected trailing input (at position 24 …)` rc 198; l. 97–99: o mesmo `if` em `sample` → mesma mensagem. Um nível de compound quote funciona nos dois (l. 76–81, 89–94), e a vírgula dentro de `` `"h1, "x""' `` não corta as opções (`seed(1) generate(g2)` foram parseadas como opções).
- **Causa:** `_parqit_split_opts` (Mata) conta o aninhamento corretamente, mas `translate_filter` (exprtrans.cpp) só aceita um nível de compound quote; a expressão chega inteira ao tradutor e é ele que recusa.
- **Correção:** reformular #166: "o splitter respeita parênteses, aspas e compound quotes aninhadas; o tradutor de expressões aceita um nível de compound quote, como em `keep if`". Se se quiser suportar aninhamento, é no tradutor (fora deste âmbito).
- **Teste:** unit test do tradutor com `` x == `"a `"b"' c"' `` documentando a decisão (aceitar ou recusar).

### SD-5 — `any`/`all` sem `cluster()` são ignorados com nota; o help não o diz e o v126 não o testa apesar do comentário

- **Severidade:** nota
- **Estado:** CONFIRMADO
- **Evidência:** `p5_names_ado.log` l. 413–420: `parqit sample 10, any` → `note: option any ignored (no cluster())`, rc 0; idem `all`. O help (`{marker sampledesign}`) só descreve `any`/`all` com clusters. `tests/verify_suite/v126_sample_design.do` §10 diz "any/all without cluster() are ignored with a note" mas só testa `any all` juntos (rc 198) e `cluster(hh region)`.
- **Causa:** comportamento herdado do `sample2` (`option -any- ignored`), implementado em `_parqit_sample`, não documentado.
- **Correção:** uma frase no help ("without `cluster()`, `any` and `all` are ignored with a note, as in `sample2`") e uma asserção no v126 via `_v126_grep` do log.
- **Teste:** o próprio v126 §10.

### SD-6 — Mensagens genéricas do parser do Stata para `in`, `if` vazio e montante ausente

- **Severidade:** nota
- **Estado:** CONFIRMADO
- **Evidência:** `p5_names_ado.log` l. 348–370: `parqit sample 10 if` → `'if' found where nothing expected` r(7); `parqit sample 10 in 1/5` → `'in' found where nothing expected` r(7); `parqit sample if age > 60` → `'' found where number expected` r(7); l. 343–345: `parqit sample 10, by(region) if age > 60` → `option if not allowed` r(198).
- **Causa:** `_parqit_split_if` só corta em `if ` com espaço a seguir; o resto cai em `confirm number`. O help diz que `in` não é suportado, mas a mensagem não o diz.
- **Correção:** em `_parqit_sample`, antes de `confirm number`, detetar `in` no head (`parqit sample: in is not supported on a lazy view; use keep in first`) e um `if` sem expressão (`parqit sample: if requires an expression`); r(198). Ruidosos já são; é só clareza.
- **Teste:** v126: dois `capture noisily` com grep da mensagem.

### SD-7 — O nome de `generate()` não entra no conjunto `taken` de `fresh_helper`

- **Severidade:** nota
- **Estado:** SUSPEITA (não reproduzido; exige adivinhar o contador de helpers)
- **Evidência:** leitura de `View::sample_design` (view.cpp): todos os `fresh_helper(...)` são chamados antes de `cols_.push_back(nc)` e sem `taken = {generate}`. `p5_names_ado.log` l. 272–274 mostra que `generate(__parqit_sample_cluster_1)` é aceite (o contador já ia adiante, sem colisão). Analisei o SQL gerado: as referências a helpers dentro das CTEs são qualificadas (`flags.c`) e o alias de saída não colide com colunas de `prev`, pelo que não encontrei uma forma de produzir resultados errados; ficaria em todo o caso um SQL confuso.
- **Correção:** passar `{generate}` como `taken` em cada `fresh_helper` de `sample_design` (uma linha por chamada) — ou recusar nomes com prefixo `__parqit_` em `generate()`, como acontece noutros verbos se for esse o padrão da casa.
- **Teste:** unit test com `generate("__parqit_sample_cluster_N")` para o N que o contador vai usar (obtido por dois `fresh_helper` consecutivos no teste).

### SD-8 — `seed()` limitado a 0…2^31−1 pelo `syntax seed(integer)`; o help fala só em "nonnegative seed(#)"

- **Severidade:** nota
- **Estado:** CONFIRMADO (pré-existente)
- **Evidência:** `p5_names_ado.log` l. 474–476: `seed(4294967296)` → `option seed() incorrectly specified` r(198); `p6_misc.log` l. 255–262: `seed(0)` e `seed(2147483647)` aceites. `seed(-7)` → sorteio aleatório (documentado no help do `sample`).
- **Causa:** tipo `integer` do `syntax`.
- **Correção:** só documentação ("an integer between 0 and 2^31−1"), ou `seed(numlist max=1 integer >=0)` se se quiser mais amplitude (o engine já lida com UBIGINT). Sem impacto na integridade.

## O que verifiquei e está correto (com reprodução)

Todos os `CHECK(...)` abaixo estão nos logs indicados com `PASS`, salvo indicação.

**Sorteio de clusters e codificações (`p1_encodings.log`)**
- INTEGER, BIGINT e DOUBLE com o mesmo valor sorteiam o mesmo conjunto (`bigint_eq_int`, `double_eq_int`); DECIMAL(9,2) = DOUBLE (`decimal_eq_double`, ambos `1 4 5 6 12 14 19 28 32 33 37 38` = oráculo); DATE = dias Stata (`date_eq_days`); TIMESTAMP(µs) = ms Stata (`ts_eq_ms`). Em todos: `_N == 135` com `generate()`, indicador constante dentro do cluster, nenhum indicador NULL, 15 linhas de cluster missing mantidas.
- `-0.0` e `0.0` no mesmo cluster (cluster 0 de `cid_d`): um só cluster, sem duplicação de linhas pelo LEFT JOIN (`N_cid_d`, `cid_d_whole`; sem `generate()`, o cluster 0 sai inteiro: 0 linhas, l. 170–173). O GROUP BY do DuckDB 1.5.3 agrupa ±0 juntos, como o CLI 1.5.0-dev tinha sugerido.
- Especiais numa coluna DOUBLE (NaN, +inf, 1e308, −1e308, NULL — 15 linhas): fora da população, nota "15 observation(s) with a missing special …", mantidas com indicador 1; o sorteio dos clusters válidos é o mesmo de `cid_d` (`special_eq_double`).
- Texto UTF-8 (`Évora`, `Ëvora`, `日本`, `h7 ` com espaço final) e `''`: `ORACLE txt : MATCH` (l. 544) com FNV-1a sobre bytes UTF-8; 15 linhas `''` mantidas.
- Oráculo Python da chave documentada: `oracle_cid_i` (l. 559–562: `2 3 5 6 7 14 16 19 20 34 37 38` nos dois lados), `big` e `txt` MATCH. Os 7 vetores fixos do doctest recomputados em Python coincidem (`mix` é bijeção mod 2^64: soma de constante e xorshift-multiply por ímpares).

**Populações elegíveis (`p2_frames.log`)**
- SQL mode: condição NULL deixa a linha fora; `strict` recusa com `the if condition selects only part of 6 cluster(s) of cid_i; specify any … or all …` (l. 75), rc 198 e plano inalterado (`n_steps` 0); `all` → 2 dos 4 clusters inteiramente elegíveis, os partidos e os de fora ficam inteiros (`sql_all_*`); `any` → 5 de 10 (`sql_any_*`), clusters inteiros.
- `statamissing on`: `age > 60` vale para `age` missing; só os 2 clusters partidos por valor são recusados (l. 169); `all` → 4 de 8; `any` → 5 de 10; a nota conta as 15 linhas de cluster missing quando a condição as abrange (`stata_note_missing_in_frame_kept`).
- Paridade com `sample2` nos mesmos dados (semântica Stata): `strict` rc 198; `all` 4 de 8 (l. 273 mostra 6 = 4 sorteados + 2 partidos mantidos), `any` 5 de 10 (l. 275); um cluster elegível não sorteado perde todas as linhas, incluindo as fora do `if`, nos dois (`p6_misc.log` l. 139–168, cluster 19).
- Divergência documentada em #166 (2): com `by(cid_i) cluster(cid_i)` a 30 %, parqit mantém as 15 linhas de cluster missing (`by_eq_cluster_none_drawn`), `sample2` apaga as 135 (o missing é um cluster para ele) — l. 349–357.
- Estratos: `strat_null` NULL numa só linha do cluster 6 → `by() must be constant within clusters: 1 value(s) of cid_i span several strata` rc 198 (l. 287); cluster 5 todo NULL forma estrato próprio e é sorteado (50 % de 1 → 1); `sample2` também recusa. `count(DISTINCT row(NULL))` conta o NULL como valor, confirmado no plugin.
- Quotas #134 por estrato com clusters: 12,5 % de 10 → 1; 35 % de 10 → 4; `count` 50 ≥ n → todos; `count` 0 → nenhum elegível, missing mantidos.

**Caminho por linhas (`p3_rows.log`)**
- Forma simples estável: duas execuções iguais a 4 threads (`plain_twice_same_4threads`), 1 thread = 4 threads, glob de 2 ficheiros = ficheiro único (l. 223, rc 0) — `preserve_insertion_order` fica no default do DuckDB (session.cpp não o altera).
- `generate()` sozinho = forma simples (sem sort e com `parqit sort y`), 4000 de 20000 linhas; a vista ordenada sorteia outro conjunto, como esperado (row_number sobre a ordem).
- Clusters no glob e a 1 thread = ficheiro único a 4 threads (`cluster_glob_eq_single`, `cluster_1thread_eq_4threads`).
- 0,3 % de 500: parqit 1 (simples e desenho), `sample` 2, `sample2` 2 — exatamente o que o help afirma.
- `by(s)` + `if i > 100`: 100 linhas de fora mantidas, 20 por estrato = `int(n*10/100+.5)`; `sample` nativo dá os mesmos totais.
- `count` com desenho: 7 por estrato; sem `by()`, 7 linhas mas conjunto diferente do reservoir da forma simples (documentado); `count` com `if`: 5 das 10 elegíveis + 490; `count` > população → tudo mantido.
- `_n`/`_N` no `if` recusados com `_n and _N are not supported in the if of sample` (linhas e clusters); 100 % com desenho marca tudo; `gsort -i` + `by(s)`: primeira linha `i == 500`, quotas 63 por estrato.

**Verificações na altura do verbo (`p4_verb_time.log`)**
- `keep in` válido + desenho sobre a fatia (60 linhas, 3 de 5 clusters por região); desenho depois de `collapse` (41 linhas, 3 de 10 por região, missing mantido); depois de `merge m:1 … using view:` com estrato do lado using (5 de 10); um `sample 50` sem seed antes do desenho tem o seed cozido no SQL (`309705268`) e dois `collect` dão o mesmo `_N`.
- Vista copiada (`use using view:S, name(C)`): o desenho acrescenta uma etapa só à cópia (`copy_has_stage`, `source_unchanged`), a origem não tem `g`, e uma cópia da cópia sorteia o mesmo (`copy_same_draw`).

**Nomes, ado, materialização (`p5_names_ado.log`, `p6_misc.log`)**
- `cluster(hh_id)` (alias de `"hh id"`), `cluster(Hh)` e `cluster(hh)` (case-distintos; `hh` resolve para o alias `hh_1` por NAME-CASE-1), `by(região)`, `by(reg*)`: rc 0 e clusters inteiros em cada chave; `cluster(h*)` que expande a várias → `cluster() takes one variable`; abreviatura `cluster(regi)` → `variable regi not found in the view` (coerente com os outros verbos lazy).
- `generate(region)` → `already defined`; `generate(REGION)`/`generate(PICK)` → mensagem NAME-CASE-1 (`differs only by case …`), igual à do `gen`; `generate(1x)` → `invalid name`; `generate()`+`keep()` → recusa; `any all` → recusa; `cluster(a b)` → recusa; `-5`, `101`, `0` → `sample percentage out of range`; `2.5, count`, `-1, count` → `needs a nonnegative integer below 2^63`; `if` com vírgula em string, com `"if x"` dentro de string, com `inlist(region, 1, 2)` → rc 0 e opções parseadas; `age == "x"`, `age >` → erros do tradutor com posição; `10 20` → r(7); opção mal escrita → `option clusters() not allowed`.
- `save` depois de `generate(pick)`: `pick: int8` no Parquet (l. 637), `byte` no `collect` (l. 664), valores {0,1}; `describe`/`count`/`show` (`(design, if, by, cluster any, generate pick, seed 7)`); `keep if pick == 1` + `collect` = forma que apaga (`flag_then_keep_eq_drop`, 63 linhas).
- Vista xmissing com `.a`/`.b` no cluster: dobradas para `.` com a nota XMISS-1, nota do desenho conta 21 (15 + 3 + 3), todas mantidas (`xm_missing_kept_21`).
- Estratos de texto no caminho por linhas (`''` é estrato próprio: 8 de 15) e com clusters; `keep()` sinónimo de `generate()` (3 de 10 por região); `seed(0)` e `seed(2147483647)`; `if cid_date >= td(15jan2020)` com `cluster(cid_date)` → 13 de 26 clusters datados, os anteriores mantidos.

**Testes e documentação**
- `parqit_tests --test-case='*sample*,*cluster*'`: 4 casos, 84 asserts, SUCCESS. Os doctests testam o que dizem: o fixture (ids 1–3, 2 linhas cada) dá 1 cluster a 34 %, 2 a 50 % (1,5 → 2), 3 linhas de 2019 + 1 de 2020; a igualdade `generate()`/forma simples é verificada nos dois sentidos (EXCEPT vazio e `sum(g) == count`); a sonda devolve `0|0|0`, `3|0|0`, `0|3|0`.
- v126 reexecutado com este plugin a partir do scratchpad: `VERDICT(V126_SAMPLE_DESIGN): PASS` (`v126run/parqit.log` l. 866).
- Afirmações do help (`{marker sampledesign}`), do help técnico, do README, do CHANGELOG e de #166 conferidas uma a uma contra o comportamento: corretas, com as ressalvas SD-2 (header), SD-4 (#166), SD-5 (help) e SD-8 (help). O diálogo `parqit_filter.dlg` é coerente: `ex_if` ativo em `sample`, `by`/`cluster`/`generate` como `optionarg`, `any`/`all` só emitidos com cluster e regra ≠ `strict` (`.equals` é usado pelos diálogos base do Stata), `ed_gen` com `max(32)`; o comando gerado é parseado pelo `_parqit_split_opts` (parênteses e vírgulas no `if`).

**Lacunas do v126 (não invalidam o PASS; a fixar)**
- Não testa `generate()` no caminho por linhas com `by()`; nenhum cluster DOUBLE/DECIMAL/DATE/±0; nenhum estrato NULL dentro de um cluster; nem `_n`/`_N` no `if` nem em `generate()` (SD-1); nem `keep in` pendente (SD-3) nem vista copiada; §11 só verifica que o `if` com vírgula não dá erro, não onde cortou; §10 não testa a nota de `any` sem cluster (SD-5); não fixa que um cluster elegível não sorteado perde as linhas fora do `if` (só as contagens).

## Limites da auditoria

- Sem compilação nem alteração do repositório; o plugin auditado é o `build/viewcopy` fornecido (assumo que corresponde ao working tree; `parqit show` confirma o SQL do desenho, `version`/notas confirmam o resto).
- A invariância à versão do DuckDB só se sustenta por construção (chave própria; desempate por valor); não foi testada com outra versão do engine.
- Divergência entre a verificação na altura do verbo e a execução só é possível se os ficheiros de origem mudarem entretanho (documentado em #166 como "sources must stay stable"); não a testei.
- Dados pequenos (135–20 000 linhas); nenhuma medição de custo além de constatar que a sonda corre o plano inteiro uma vez (documentado).
- Não abri o diálogo (análise estática apenas).

---

## Resolução (25 de setembro de 2026, ramo `feat/view-copy`, sem commit)

| ID | Resolução |
|---|---|
| SD-1 | Implementado. `_parqit_sample` corre `confirm name` sobre o nome de `generate()`/`keep()` (rc 7, "invalid name (reserved word)"), e `View::sample_design` recusa `_n`/`_N`, para não depender do ado. Fixado no v126 §12 (`generate(_n)`, `generate(_N)`, `keep(_n)` → rc 7; `_n` no `if` → rc 198) e no doctest. |
| SD-2 | Documentado no header de `sample_key.hpp` e no help técnico: os inteiros acima de 2^53 ordenam-se pela aproximação binary64, e os empates desfazem-se pelo valor. Há um doctest com dois BIGINT, 2^53 e 2^53+1: com 50 % sai o menor. |
| SD-3 | Documentado (opção do auditor): só as verificações dos clusters validam um `keep in` pendente na altura do verbo; os desenhos por linhas deixam-no para a materialização, como os outros verbos. Fixado no v126 §16. |
| SD-4 | #166 reformulado: o separador respeita compound quotes aninhadas, e o tradutor aceita um nível, como em `keep if`. |
| SD-5 | Uma frase no help, e o v126 §12 verifica a nota "option any ignored (no cluster())". |
| SD-6 | Implementado: mensagens do parqit (rc 198) para `in`, para `if` sem expressão e para o montante em falta. Fixado no v126 §12. |
| SD-7 | Implementado: o nome do indicador entra no `taken` de todos os `fresh_helper` do `sample_design`. Doctest com `generate(__parqit_sample_source_1)`. |
| SD-8 | Documentado no help: `seed()` vai de 0 a 2 147 483 647. |
| Lacunas do v126 | Todas cobertas: §13 (estrato missing numa linha de um cluster, também no `sample2`), §14 (clusters `double` não inteiros, contra o oráculo Python), §15 (`generate()` por linhas com `by()`), §16 (`keep in` pendente), §17 (desenho numa vista copiada), §11 (onde a vírgula cortou) e §5 (um cluster elegível não sorteado perde todas as linhas). |

Verificação depois das correções, no build isolado `build/viewcopy`:

- `ctest` 5/5;
- v126 PASS, com 17 secções;
- `run_stata.sh` completo, **177/177 PASS** (logs em `/tmp/parqit_tests.S8xu48`);
- `release_lint.sh` OK.
