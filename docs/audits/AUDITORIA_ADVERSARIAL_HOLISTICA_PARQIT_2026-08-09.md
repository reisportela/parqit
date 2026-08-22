# Auditoria adversarial holística do parqit — 2026-08-09 (PT)

| | |
|---|---|
| **Alvo** | `parqit` v0.1.24 — commit `e0a79f5` (`main`) **mais** a remediação não commitada da ronda de 2026-08-08 (D1–D7 + auditoria do help), isto é, o estado que se pretende commitar |
| **Data** | 2026-08-08 → 2026-08-09 (sessão atravessou a meia-noite) |
| **Auditor** | Claude (Fable 5), sessão interativa na máquina de desenvolvimento |
| **Ambiente** | Linux EL9 x86_64 · StataNow MP 19.5 (`stata-mp`) · DuckDB embebido 1.5.3 (pinned) · build `dev` (RelWithDebInfo) · pyarrow disponível |
| **Método** | (1) leitura integral do diff não commitado (1 737 linhas) e auditoria da remediação D1–D7; (2) releitura dirigida do engine (`view.cpp` e `exprtrans.cpp` na íntegra; `plugin_view.cpp`/`plugin_io.cpp`/`sanitize.cpp`/ado por alvo); (3) re-execução própria de todos os gates; (4) **sondas diferenciais novas contra o Stata nativo no mesmo processo** — ~110 verificações executadas em três do-files próprios (`probe_parity`, `probe_semantics`, `probe_edge`), incluindo ficheiros Parquet hostis gerados por pyarrow. Lição PQ-AUD-003 aplicada: nenhuma claim de paridade foi aceite sem re-execução nativa |

## 1. Sumário executivo

Esta é uma auditoria *pós-remediação*: a árvore auditada já contém as correções
das duas auditorias de 2026-08-08 (F1–F6 do runtime; F1–F7 documentais do help;
D1–D7 de mensagens). O veredito é o mais limpo da cadeia de evidência até à
data:

- **A remediação D1–D7 está corretamente implementada** — verificada no código,
  nos testes novos (`v66` apertado, `v67` novo, caso unitário ROWCTX-1,
  `audit_repro/`) e por execução. Um ponto que o registo de remediação afirmava
  sem prova exaustiva — todas as mensagens de `merge_with`/`append_with`/
  `joinby_with` chegarem prefixadas com o verbo ao novo `cry("parqit " + e)` —
  foi conferido *string a string* no engine: está completo.
- **Zero defeitos de severidade Alta ou Média.** As sondas diferenciais novas
  (80 comparações expressão-a-expressão nativo-vs-parqit, reshape com zeros à
  esquerda, collapse, merge, nomes hostis, caps) não encontraram um único valor
  errado silencioso.
- **2 achados documentais Baixos** (§4): o README ainda promete "Nothing is
  read", formulação que o help e a mensagem de runtime abandonaram
  deliberadamente nesta ronda; e `ty()` é aceite pelo tradutor mas **não existe
  no Stata nativo** — o help apresenta-o como notação nativa.
- As 4 divergências que as sondas detetaram são **todas comportamento
  documentado** no help (modo SQL-missing; `substr` que parte um codepoint),
  confirmadas frase a frase (§5).

**Veredicto: GO.** A árvore está em condições de commit e de manter o estatuto
da certificação de 2026-07-14. Tudo o que esta auditoria julga dever ser
corrigido ou pinado — os dois achados de §4, a frase de §5, o oráculo `v68` de
§7.3 e a varredura de mensagens herdada de §8 — está especificado, com
decisões fixadas, em
[PROMPT_CORRECAO_DOCS_PARQIT_2026-08-09.md](PROMPT_CORRECAO_DOCS_PARQIT_2026-08-09.md);
nada bloqueia dados.

## 2. Baseline verificada (re-executada nesta sessão, nada herdado)

| Verificação | Resultado |
|---|---|
| `cmake --build build/dev -j` | OK (incremental; `ado/plus/p` refrescado) |
| `ctest --preset dev` (`unit`, `runner_no_match`, `unit_concurrent`) | 3/3 PASS |
| `bash tests/release_lint.sh` (inclui os checks novos: dispatcher↔help, lista de funções bidirecional, âncoras inline, SMCL) | OK — `v0.1.24 (8aug2026 / pkg 20260808)` |
| `bash tests/run_stata.sh` **integral** | exit 0; todos os verdicts PASS (o runner falha com qualquer FAIL), incluindo `v61`–`v65`, `v66` apertado, `v67` novo, `t01`, `x01`/`x02` |
| `translate parqit.sthlp → smcl2txt` | rc 0, 1 191 linhas, zero markup por interpretar |

## 3. Verificação da remediação de 2026-08-08 (D1–D7)

Cada correção foi lida no diff e confrontada com o teste que a pina; nenhuma
introduziu regressão detetável:

- **D1/ROWCTX-1** — o flag `ExprResult::uses_rowctx` é atribuído nos dois
  pontos de consumo (`SysN`/`SysBigN`) e propagado por ambos os entry points; as
  recusas em `countif` e no filtro de preview usam `kRcUsage` e mensagem
  própria. O caso unitário cobre aninhamento, `statamissing` e o anti-spoof
  (literal com o marcador continua rejeitado — XLAT-8).
- **D2/MSG-LABEL-1** — o `label` viaja hex (`req_text`), é validado contra
  `{collect, head, list}` e nunca ecoa texto do wire; o `load_req` corre antes
  do `require_view` para a própria mensagem "no view" nomear o comando certo.
- **D3** — mensagem do `use` lazy alinhada; ver, porém, o achado A1 (§4): a
  varredura `grep 'nothing read'` não apanhou a variante "Nothing **is** read"
  do README.
- **D4/SORT-WILD-1** — deteção pelo padrão antes da expansão; o check por
  contagem fica como cinto. Nomes Stata não contêm `*`/`?`, logo sem falsos
  positivos.
- **D5a/b/c** — `quietly` verificado contra `SF_error` (ASSUMPTIONS §89), linha
  do `sql, clear` reimpressa com o nome comprometido, célula `%` órfã removida.
- **D6/DESCRIBE-EXT-1** — classificação por extensão com isenção de diretórios
  (`direxists`); `glimpse` herda por delegação.
- **D7/JOINKEY-1** — `join_keys_error` é a única implementação e é chamada
  pelos dois verbos e pelo plugin antes dos contratos de unicidade; rc 111.
  Conferi que **todas** as strings de erro devolvidas por `merge_with`
  (`"merge: …"` ×5), `append_with` (`"append: …"` ×4) e `joinby_with`
  (`"joinby: …"` ×2) trazem o prefixo do verbo que o novo
  `cry("parqit " + e)` pressupõe — a suposição é válida em todos os caminhos.

## 4. Achados novos

Severidades como nas auditorias anteriores: **A**lta, **M**édia, **B**aixa,
**I**nfo.

### A1 [B] O README ainda promete "Nothing is read" — contradiz o contrato corrigido do help e a nova mensagem de runtime

- **Evidência.** `README.md:52-53`: "*a plan of work, like a do-file you are
  still writing. Nothing is read, nothing is computed*"; `README.md:226`:
  "*Open a lazy view over one or many Parquet files (nothing is read yet)*".
  Cosmético: o comentário em `src/ado/p/parqit.ado:336` ("`nothing is read`")
  é interno mas repete a mesma formulação.
- **Porque importa.** A auditoria do help de 2026-08-08 classificou esta
  formulação absoluta como enganosa — abrir uma view **lê** o schema e a
  metadata, pode amostrar um CSV para inferência de tipos e pode criar um
  bridge — e reescreveu o help para "*Opening probes source schema and
  metadata but does not materialise observations*"; a remediação D3 alinhou a
  mensagem de runtime ("`schema probed, no rows loaded`"). O README, que o
  projeto define como superfície pública estável, ficou como a única superfície
  com a promessa antiga: as três superfícies públicas dizem agora coisas
  diferentes.
- **Causa raiz.** A varredura da Tarefa 3 do prompt de remediação usou
  `grep -rn 'nothing read'` — sem a variante "nothing **is** read".
- **Correção.** Precisão de texto no README (duas ocorrências), opcionalmente o
  comentário do ado; especificada no prompt (T1).

### A2 [B] `ty()` é aceite pelo parqit mas não existe no Stata nativo — o help apresenta-o como notação nativa

- **Evidência executada** (probe_parity): `gen double n41 = ty(2026)` no Stata
  nativo 19.5 → **r(133) "unknown function ty()"**; `parqit gen double p41 =
  ty(2026)` → rc 0 (valor 2026, identidade — um valor %ty é o próprio ano).
  Os restantes sete literais (`td tc tC tm tq th tw`) existem no nativo e
  passaram a paridade de valor exata.
- **Porque importa.** O help afirma que os literais de data são "*constants
  spelt in Stata's own notation*" e lista `ty(yyyy)` entre eles
  (`parqit.sthlp` ~741 e o parágrafo dos literais); noutra secção promete que
  "*syntax native Stata rejects (`||`, `&&`, `=`) is rejected here too*". Um
  do-file com `ty()` corre no parqit e quebra ao migrar para o nativo — o
  inverso da promessa. Sem risco de dados (a semântica do valor é trivial e
  correta).
- **Correção recomendada** (não-regressão: nunca remover funcionalidade em
  silêncio): manter `ty()` e **documentá-lo como extensão do parqit** no
  parágrafo dos literais de data (nativamente, um valor %ty escreve-se como o
  próprio ano), com registo em `ASSUMPTIONS.md`. Alternativa mais estrita —
  recusar `ty()` com mensagem que aponte para o ano nu — só se o mantenedor
  preferir paridade estrita a superfície; é uma remoção de funcionalidade e
  exigiria CHANGELOG.

## 5. Divergências detetadas e confirmadas como documentadas (não são defeitos)

As sondas diferenciais compararam 80 expressões/strings nativo-vs-parqit sobre
os mesmos dados. 76 bateram certo; as 4 divergências são exatamente o
comportamento que o help documenta, confirmado frase a frase:

| Sonda | Divergência observada | Onde está documentada |
|---|---|---|
| N44 | `gen y = 1 < x < 10` com `x` missing: nativo 1, parqit `.` (modo SQL) | §Expressions: "*gen y = x > c yields system missing … for rows where x is missing*"; cadeia relacional associativa à esquerda documentada no mesmo parágrafo |
| N91 | `gen y = _n if x > 0` com `x` missing: nativo atribui (missing conta como verdadeiro), parqit deixa `.` (modo SQL) | §Expressions, parágrafo do modo de missing (o qualificador é um filtro); sob `statamissing on` reproduz o nativo — verificado (N80–N82 passam) |
| S69/S70 | `substr()` que corta um codepoint UTF-8 a meio: nativo devolve o fragmento de bytes cru, parqit U+FFFD | §Expressions: "*if a substr() slice splits a UTF-8 codepoint, parqit returns the replacement character*" |

Sugestão editorial (não bloqueante, T3 do prompt): o parágrafo do modo
SQL-missing podia nomear explicitamente o qualificador `if` de `gen`/`replace`
entre os contextos afetados — hoje o leitor tem de inferir que o qualificador é
um filtro.

## 6. Ataques executados sem achado (amostra do que foi ativamente tentado)

Cada linha corresponde a execução real nesta sessão, com o nativo e/ou oráculos
independentes como juiz:

- **Paridade de expressões (76/80 exatas, resto em §5)** — incluindo os pontos
  que esta auditoria escolheu por nunca terem sido pinados: `round(-2.5)` = −2
  (a claim NUM-2 do código **confirma-se** no nativo), `round(x,.)`,
  `mdy(2.5,10,2020)` e `dofm(1.5)`/`mofd(59.9)` (o nativo **também trunca**
  componentes fracionários — DATE-2 confirmado), `dow(-0.5)`/`day(-0.5)`,
  matriz completa de `inrange` com missings, `cond` 3/4 argumentos com condição
  missing, `min`/`max`/`inlist` com literais missing, `x/0`, `0/0`,
  `sqrt(-1)`, `ln(0)`, `exp(710)`, `log10(-1)`, `strpos` com agulha vazia
  (STRPOS-EMPTY-2 correto) e multibyte, `string()`/`%9.0g` num espetro de
  magnitudes (o scalar interno `parqit_stata_string` reproduz o nativo),
  `real()` de texto inválido/`"1e400"`/`"."`, `regexm` com `\d` e classes,
  `upper`/`strupper` **ASCII-only sobre "café"/"Ærø"** (a implementação via
  `translate()` reproduz o nativo byte a byte; `ustrupper` idem em Unicode),
  `trim`/`ltrim`/`rtrim`, concatenação, literais `td/tc/tC/tm/tq/th/tw`,
  precedência (`-2^2`, associatividade de `^`), e os três idiomas sob
  `statamissing on`.
- **`reshape long` com zeros à esquerda** (`inc01`+`inc2`, e `inc01`+`inc1`+
  `inc2`): o nativo faz exatamente o que o help do parqit descreve — carrega
  `inc01` como coluna ordinária, `j=1` existe e o valor vem de `inc1` ou fica
  missing — e o parqit reproduz valores e rc em ambas as variantes. A claim
  mais intrincada do help sobreviveu à falsificação.
- **`collapse`** (mean/sum/count, `by()`): valores byte-iguais ao nativo em
  tolerância dupla; tipo do resultado igual (double) neste ambiente.
- **Nomes hostis**: Parquet gerado por pyarrow com colunas `__PARQIT_ROW__`,
  `__parqit_rn_1` e `he"llo`. Abrir, `sort`, `keep if _n <= 2`, `collect` —
  valores intactos ao lado da maquinaria de row-context; expressão que nomeie
  `__PARQIT_ROW__` recusa alto (INJID-1); `gen z2 = _n` coexiste com a coluna
  reservada; `he"llo` chega como `he_llo` com `src_name` preservado. O
  desenho (substituição só no SQL do filtro; manifesto sanitizado no open;
  `fresh_helper` contra o manifesto vivo) fecha as colisões por construção.
- **Merge lazy**: `keep(match match 3)` (tokens repetidos) aceite com o
  significado documentado; resultado ordenado pelas chaves; marcador `_merge`
  com o value label standard.
- **Caps e validação**: tabulate one-way >10 000 níveis recusa; `histogram
  bins(2000)` → 1 000; `levelsof` acima do limite recusa; `tabstat by()` 201
  grupos recusa / 3 passa; `query` inválido recusa e a view fica utilizável
  (count = 10 001); `sql` cujo resultado só tem colunas LIST recusa;
  `list in 3` bare funciona; `set threads 0` recusa / 2 aceita; `head 0`
  recusa.
- **Overflow de agregados dentro do pipeline**: tentei fabricar um double
  finito ≥ sentinela de missing do Stata dentro do plano (via `collapse (sum)`)
  para divergir de filtros subsequentes — sem sucesso observável: o próprio
  Stata não armazena >8.99e307 (o input já nasce missing), `parqit_finite`
  usa a sentinela do Stata (não o infinito IEEE) em todos os produtores
  aritméticos, `missing()` inclui o teste de gama, e as fronteiras
  (collect/save) aplicam o guard. Paridade de contagem confirmada com o twin
  nativo.
- **Higiene**: `sortedby_names()` faz um unquote ingénuo que corromperia um
  nome com aspas — mas verifiquei que é inatingível: o manifesto lazy só
  contém nomes já sanitizados (aspas nunca sobrevivem à sanitização) e os
  verbos validam nomes novos. Descartado como não-achado; fica o registo.

## 7. Recomendações

1. **Commitar a remediação pendente** (está verde em todos os gates; o worktree
   é o estado auditado) segundo o plano de commits do prompt de remediação.
2. Aplicar o prompt de correção
   ([PROMPT_CORRECAO_DOCS_PARQIT_2026-08-09.md](PROMPT_CORRECAO_DOCS_PARQIT_2026-08-09.md)),
   que fixa integralmente: A1 (varredura "nothing is read" no README +
   comentário do ado), A2 (`ty()` mantido e documentado como extensão), a
   frase de §5 (qualificador `gen … if` nomeado no parágrafo do modo de
   missing), o `v68_expr_native_oracle2` que pina o terreno verificado à mão
   nesta auditoria (`mdy`/`dofm`/`mofd` fracionários, matriz
   `inrange`/`cond`/`inlist` com missings, `string()`-%9.0g, `upper`-ASCII,
   reshape com zeros à esquerda, ficheiro hostil `__PARQIT_ROW__` — o v61
   cobre 31 expressões; estas ~80 sobrepõem-no e estendem-no), e a varredura
   test-first `v69` do risco residual "nome inexistente → erro cru do motor"
   (§8). O prompt regista também o que foi decidido **não** corrigir.
3. Manter a disciplina: qualquer futura "correção de paridade" tem de vencer o
   oráculo vivo, não um comentário (a lição F2/STRPOS continua a ser a certa).

## 8. Riscos residuais (sem alteração face a 2026-08-08)

Os riscos residuais da auditoria holística e da auditoria do help de
2026-08-08 mantêm-se tal como lá enumerados: plataformas não-Linux cobertas só
pelo CI de build; `help parqit` nunca aberto num Viewer interativo desta
máquina (renderização validada por `smcl2txt`); fill paralelo apoiado no
precedente do `pq` com escape documentado; `parqit sql` como poder local
documentado; `_n`/`_N` indisponíveis nos filtros read-only (agora recusa
limpa, ASSUMPTIONS §88); a varredura sistemática "que outros caminhos podem
devolver erros crus do motor" (registo de remediação, risco 2) continua por
fazer — D7 fechou o caso concreto conhecido.

## 9. Conclusão

Depois de três rondas adversariais em dois dias, esta passagem — desenhada para
*falsificar* tanto a remediação como as claims mais recentes do help — não
encontrou nenhum defeito de dados, de tipos, de metadata ou de mensagens: os
únicos achados são duas imprecisões documentais baixas, ambas com correção de
minutos. No julgamento deste auditor, o parqit v0.1.24 (árvore com a
remediação) está em condições de commit e release, condicionado apenas aos
gates habituais de release (CI multi-plataforma, suite integral a partir da
árvore instalada).

---
*Evidência bruta desta sessão (scratchpad): `probe_parity.captured.log` (80
comparações; 4 divergências, todas documentadas; TY_ASYMMETRY nativo rc 133 vs
parqit rc 0), `probe_semantics.captured.log` (23 verificações, todas PASS),
`probe_edge.captured.log` (overflow/limites; o único FAIL é artefacto da
própria sonda — `gen double x = 1e308` já é missing no Stata nativo, e a soma
de grupo todo-missing = 0 é a regra nativa pinada), `make_hostile.py` +
`hostile.parquet`, `help_render.txt` (1 191 linhas). Suites oficiais: exit 0
integral re-executado nesta sessão.*
