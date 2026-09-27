# Auditoria adversarial independente — VIEW-COPY-1 (`parqit use [varlist] using view:<origem>, name(<nova>)`)

> Cópia arquivada do relatório do auditor. A pasta `$FA` era temporária: as sondas e os logs estão
> em `audit_repro/fable_view_copy_20260925/`. A resolução de cada achado está no fim.

Auditor: Fable (Claude), 25 de setembro de 2026.
Objeto: ramo `feat/view-copy`, alterações por gravar sobre `3c1ca16` (v0.2.3);
plugin `build/viewcopy/parqit.plugin` (compilado 09:58, posterior à última
alteração de `plugin_view.cpp` às 09:55); `build/viewcopy/parqit_tests`
(09:50, posterior a `view.hpp` e `test_view.cpp`).
Modo: só leitura no repositório; sem compilação; sondas com dados pequenos
(≤ 40 linhas) em batch (`stata-mp -b`), cada uma com `TMPDIR` privado para
contar resíduos de pontes sem interferência de outras sessões.

Todos os ficheiros desta auditoria estão em
`/tmp/claude-1003/-home-mangelo-Documents-GitHub-parqit/8015a030-5853-46da-88c4-095f0cdae4f1/scratchpad/fable_audit/`
(abaixo, `$FA`):

| Sonda | Ficheiros | Cobre |
|---|---|---|
| P1 | `$FA/p1/p1_bridges.do`, `.log` | pontes: cadeias A→B→C nas três ordens de fecho, substituição da cópia e da origem, cópia de volta sobre o nome da origem, `view:` embebido por `merge`, cópia como lado `using`, `close _all`, resíduos, recusas |
| P2 | `$FA/p2/p2_states.do`, `.log` | estados do plano: leitura direta (#38), Hive, glob, CSV, `sql, name()`, `int64()`, `filename()`, xmissing, NAME-CASE-1, `sample` (contagem e percentagem), collapse/reshape, recusa de `save` sobre a própria fonte, `keep in` pendente + chave de sort removida, wildcards/duplicados, contabilidade |
| P3 | `$FA/p3/p3_ado.do`, `.log` | superfície ado: 28 recusas com atomicidade por recusa (n.º de vistas, vista corrente, `show` da origem), formas de sintaxe, prefixo `parqit view X:`, `mergein`/`appendin`/`describe` com `view:`, sem vistas abertas |
| P3-old | `$FA/p3/p3_old.do`, `.log` (ado de `3c1ca16` extraído para `$FA/old/`) | comportamento anterior de `using view:X` |
| P4 | `$FA/p4/p4_edges.do`, `.log` | nome-alvo embebido no plano da origem, origem embebida substituída, nome de origem Unicode, glob `relaxed`, `query`, listagem com caminho curto |

Também corridos: `build/viewcopy/parqit_tests --test-case='*copied*'` (1 caso,
22/22 asserções), `ctest --test-dir build/viewcopy` (5/5),
`BUILD_DIR=build/viewcopy bash tests/run_stata.sh v125`
(`VERDICT(V125_VIEW_COPY): PASS`, log em `/tmp/parqit_tests.rZzFBg/`) e
`bash tests/release_lint.sh` (`dialog-lint OK`, `release-lint OK: v0.2.3`).

---

## Sumário

A funcionalidade está correta no que importa ao maintainer: **não encontrei
nenhum caminho que corrompa dados, altere estatísticas, apague uma ponte cedo
demais, deixe resíduos ou mude em silêncio a semântica de um comando que já
funcionava**. A cópia é uma cópia por valor do `View`, validada antes de
qualquer mutação; a contagem de referências das pontes aguenta cadeias,
substituições, fontes embebidas e `close _all` em todas as ordens que
experimentei; as recolhas da cópia batem com a origem, com `parqit keep` e com
o gémeo nativo em todos os estados de plano testados (leitura direta, Hive,
glob, CSV, `sql`, `int64()`, `filename()`, xmissing, NAME-CASE-1, `sample`,
collapse/reshape, `keep in` pendente, `query`, `relaxed`); as 28 recusas
testadas deixam o n.º de vistas, a vista corrente e o `show` da origem
exatamente como estavam.

O que há a corrigir é periférico:

| ID | Sev. | Estado | Problema |
|---|---|---|---|
| VC-1 | baixo | CONFIRMADO | `parqit mergein`/`appendin ... using view:X` (e `parqit use using view:X, clear`) passam a falhar com a mensagem "`clear` describes how to read a file …" — uma opção que o utilizador de `mergein` não escreveu; rc 601 → 198 |
| VC-2 | médio | CONFIRMADO | `v125` não testa o que a especificação (§1.4) exige: a secção 2 não exercita a leitura direta da #38 (fixture gravado ordenado ⇒ `sort_keys()` não vazio ⇒ `direct_read` falso), as recusas não verificam vista corrente nem `show` por recusa, `owned` falta, e cadeias/ordens de fecho/substituição da origem/fontes embebidas/`close _all` não estão cobertas (verifiquei-as todas a verde nas sondas) |
| VC-3 | baixo | CONFIRMADO (pré-existente) | `name(_all)` é aceite (também em `use` de ficheiro, `sql`, `open _data`); uma vista chamada `_all` não pode ser fechada isoladamente |
| VC-4 | nota | CONFIRMADO (pré-existente) | Sob o prefixo, `parqit view A: use using view:A, name(P)` só deixa `P` corrente se `A` já era a corrente; o help diz que a cópia "makes it current" sem esta ressalva |
| VC-5 | nota | CONFIRMADO | `parqit views` corta a origem a 40 caracteres, pelo que a linha de uma cópia mostra `view:P (/tmp/…` e esconde justamente o ficheiro; `describe` mostra tudo |
| VC-6 | nota | CONFIRMADO | Documentação: a segunda forma `parqit use view:X, name(Y)` funciona mas não aparece no bloco de sintaxe nem no README; `r(source_view)` não consta dos "Stored results"; `VIEW:` em maiúsculas cai na leitura de ficheiro (rc 601), como nos `view:` de `merge`/`append` |
| VC-7 | nota | SUSPEITA | "both views draw the same sample" depende, na forma `sample N, count`, da repetibilidade do `USING SAMPLE reservoir(N ROWS) REPEATABLE(seed)` do DuckDB; verificado aqui (30 linhas, threads por omissão e 1) mas não num ficheiro com vários row groups |
| VC-8 | nota | CONFIRMADO | A cópia de uma vista sobre um ficheiro `xmissing` dobra `.a–.z` para `.` como a origem, mas não repete a nota XMISS-1 que `parqit use <ficheiro>` imprime |
| VC-9 | nota | SUSPEITA (estático) | Diálogo: `ed_value` (nome da nova vista) não tem `max(32)` ao contrário de `ed_name`; a implementação reutiliza `ed_cmd`/`ed_value` em vez dos `ed_newname`/`ed_vars` da especificação (funcional; não abri a GUI) |

Nenhum achado é crítico ou alto. VC-1 e VC-3 são mensagens/recusas em caminhos
já ruidosos; VC-2 é a lacuna que mais vale fechar porque as garantias que a
documentação promete (pontes partilhadas em cadeia, leitura direta, recusas
atómicas) só ficam fixadas na suíte depois de o v125 as verificar de facto.

---

## Achados

### VC-1 — `mergein`/`appendin` com `using view:X` recebem a recusa de `clear` do `use` (baixo, CONFIRMADO)

**Evidência.** `$FA/p3/p3_ado.log`, secção "4. mergein/appendin":

```
. capture noisily parqit mergein m:1 id using view:A
parqit use: clear describes how to read a file; view:A copies that view's plan
> as it is; load it with parqit view A: collect, clear
rc(mergein using view:A) = 198
. capture noisily parqit appendin using view:A
parqit use: clear describes how to read a file; view:A copies that view's plan
> as it is; load it with parqit view A: collect, clear
rc(appendin using view:A) = 198
```

Com o ado de `3c1ca16` (`$FA/p3/p3_old.log`):

```
parqit use: file not found: no file matches "view:A" (nothing to read); check t
> he path or pattern
OLD rc(mergein using view:A) = 601
OLD rc(appendin using view:A) = 601
```

**Causa.** `_parqit_mergein` e `_parqit_appendin` (`src/ado/p/parqit.ado`,
`qui parqit use `keys' `keepusing' using `"`using'"', clear `i64'`) delegam a
leitura do lado de disco em `_parqit_use ..., clear`. O novo ramo `view:` corre
antes de `_parqit_resolve_source` e recusa `clear` com uma mensagem escrita
para o utilizador de `parqit use`. O caminho continua ruidoso (rc 198), mas a
mensagem atribui ao utilizador uma opção que ele não deu e sugere um remédio
(`parqit view A: collect, clear`) que não é o que `mergein` precisa. O mesmo
texto aparece em `parqit use using view:A, clear`, onde é adequado.

**Correção.** Em `_parqit_mergein` e `_parqit_appendin`, logo após o `syntax`,
antes do `parqit use` interno:

```stata
if (substr(`"`using'"', 1, 5) == "view:") {
    local vsrc = substr(`"`using'"', 6, .)
    di as err "parqit mergein: the using side must be a file on disk, not an open view;" ///
        " write the view first (parqit view `vsrc': save <tmp.parquet>) and mergein that file," ///
        " or keep the join out of core: parqit open _data, then parqit merge ... using view:`vsrc'," ///
        " then parqit collect, clear"
    exit 198
}
```

(análogo para `appendin`, apontando `parqit append using view:X`). O remédio
não pode ser `parqit view X: collect, clear`: isso substituiria em memória o
master que `mergein`/`appendin` existem para preservar.

**Teste.** Em `v125_view_copy.do`, secção 7: `capture noisily parqit mergein m:1 id using view:R` e
`parqit appendin using view:R` com `assert _rc == 198` e um `log`-grep da
frase própria de `mergein`/`appendin` (padrão de v67/v70).

### VC-2 — `v125` não verifica o que a especificação §1.4 exige (médio, CONFIRMADO)

Três lacunas, todas com o comportamento real verificado a verde nas minhas
sondas — o problema é a suíte não o fixar.

**(a) A secção 2 não exercita a leitura direta da #38.** O fixture do v125 é
gravado depois de `sort id year` (linhas 55–57 do teste), logo o ficheiro traz
`sortedby` e `cmd_view_open` restaura-o em `sort_`. `direct_read` em
`cmd_view_collect_prepare` (`plugin_view.cpp` ~1610) exige
`sort_keys().empty()`, pelo que P e Q recolhem pelo caminho
materializa-e-dimensiona, não pelo direto. Evidência (`$FA/p2/p2_states.log`,
vista `PS` sobre um fixture gravado ordenado):

```
SELECT * FROM __parqit_s0 ORDER BY "id" NULLS LAST, "year" NULLS LAST
```

e, para o fixture não ordenado (`P`/`Q`), `parqit show` termina em
`SELECT * FROM __parqit_s0` sem `ORDER BY`, `describe` dá `pipeline steps: 0`
em ambas, e a assinatura de tipos/formatos/labels + `cf _all` da cópia é igual
à da origem (`ok A: untouched copy collects identically (sig + cf)`).

**(b) Recusas.** A especificação pede, por recusa, que a vista corrente não
mude e que o `parqit show` da origem seja igual antes e depois; o v125 só
compara `r(n_views)` uma vez no fim e `count` como proxy, e a lista de opções
não inclui `owned`. Verifiquei 28 recusas em P3 com um instantâneo por recusa
(`_vc_state`: `n_views`, vista corrente via `_dlgcontext`, `show` de A): nenhum
`FAIL`; todas rc 198 (ou rc 100 quando é o próprio `syntax` a recusar:
`name(a b)`, `name(1bad)`, nome com 33 caracteres, `if`, `in`).

**(c) Pontes.** Cadeia de cópias, ordens de fecho, substituição da origem
enquanto a cópia vive, `view:` embebido na origem, cópia como lado `using`,
nome-alvo embebido na origem e `close _all` sem resíduos não estão no v125
(a cadeia é a "lacuna conhecida"). Todos verdes em P1/P4, por exemplo:

```
ok   chain-M N O after close M: exists=1
ok   chain-M N O after close N: exists=1
ok   chain-M N O: last view O collects correctly with two closed
ok   chain-M N O after close O: exists=0
bridges after chain M N O: 0
...
ok   replace source: copy still returns the OLD snapshot
ok   embedded target replaced: bridge gone after close B: exists=0
bridges after close _all: 0
```

**Correção (tests/verify_suite/v125_view_copy.do).**
- Secção 2: gerar um segundo fixture sem `sort` antes do `parqit save ..., data`
  (ou `sort` só depois de gravar) e afirmar `parqit describe` → `r(n_steps)==0`
  e que o log de `parqit show` não contém `ORDER BY`, antes do `cf _all`.
- Secção 7: um `foreach` que, por recusa, guarda `parqit views` `r(n_views)`,
  o `r(view)` de `parqit _dlgcontext parqit_views, report` e o `show` da origem
  (via `log using`), e compara; juntar `owned` à lista.
- Novas secções, com o padrão `_v125_exists`: cadeia M→N→O fechada nas ordens
  (M,N,O), (O,N,M), (N,M,O); `parqit open _data, name(M)` repetido com a cópia
  aberta; origem com `merge ... using view:M` copiada e M/origem fechadas antes
  da cópia; cópia usada como `using view:` de outra vista; `close _all` seguido
  de contagem de `_parqit_bridge_*` em `c(tmpdir)` igual a zero (o v58 já usa
  este padrão).

### VC-3 — `name(_all)` é aceite (baixo, CONFIRMADO, pré-existente)

**Evidência** (`$FA/p3/p3_ado.log`):

```
. capture noisily parqit use using view:A, name(_all)
(lazy view _all copied from view A: 3 columns; plan copied, no rows read — ...)
rc(name(_all)) = 0
  * _all                        3        1   view:A (/tmp/claude-1003/-home-man
. capture noisily parqit close _all
(views closed)
```

**Causa.** `Name(name)` no `syntax` aceita `_all` (é um nome válido), e nem
`_parqit_use` (ambos os ramos), nem `_parqit_sql`, nem `_parqit_open` o
recusam; `_parqit_close` trata `_all` como palavra reservada. A vista fica
impossível de fechar isoladamente. Não é específico da cópia — `parqit use
<ficheiro>, name(_all)` tem o mesmo problema — mas a nova forma torna `name()`
obrigatório e é onde o utilizador mais escreverá nomes.

**Correção.** Um pequeno validador partilhado (por exemplo
`_parqit_check_viewname`) chamado em `_parqit_use` (antes do ramo `view:`, já
que serve ambos), `_parqit_sql` e `_parqit_open`:
`if ("`name'" == "_all") { di as err "parqit: _all is reserved by parqit close; choose another name()"; exit 198 }`.

**Teste.** Uma recusa no v125 §7 e no v-teste de `open _data`/`sql`.

### VC-4 — sob o prefixo, a cópia só fica corrente quando o prefixo já era a corrente (nota, CONFIRMADO, pré-existente)

**Evidência** (`$FA/p3/p3_ado.log`, secção 3):

```
before prefix: current = Z
after 'parqit view A: use using view:A, name(P1)' with prev=Z: current = Z
after 'parqit view A: use using view:A, name(P2)' with prev=A: current = P2
```

**Causa.** `_parqit_view` só restaura a vista anterior quando
`"`prev'" != "`name'"`; assume que o comando envolvido não muda a vista
corrente, o que `use ..., name()` (ficheiro ou `view:`) nunca respeitou. O
help da cópia ("opens newview ... and makes it current") não menciona o caso.

**Correção.** Ou (a) em `_parqit_view`, depois do comando, restaurar `prev`
sempre que a vista corrente reportada por `view_alive` difira de `prev`
(inclui `prev == name`), ou (b) uma frase no parágrafo `{marker copy}` do help:
"under `parqit view name: ...` the previous current view is restored, as for
any prefixed command". (a) é mais coerente com "run one command, then
restore".

**Teste.** v125: as duas invocações acima com `assert` sobre
`r(view)` de `parqit _dlgcontext parqit_views, report`.

### VC-5 — `parqit views` esconde a origem real de uma cópia (nota, CONFIRMADO)

**Evidência** (`$FA/p2/p2_states.log`, secção B, e `$FA/p4/p4_edges.log`, secção 6):

```
    Q                           7        0   view:P (/tmp/claude-1003/-home-man
> gelo-~
  * Q2                          7        0   view:Q (view:P (/tmp/claude-1003/-
> home-~
```
com um caminho curto: `t   2   0   view:s (base.parquet)`.

**Causa.** `_parqit_print_views` aplica `_parqit_clip(..., 40)` à origem; o
prefixo `view:P (` consome 8 dos 40 caracteres e o resto do caminho é o que se
perde. `parqit describe` e `parqit show` mostram o texto completo
(`lazy view over view:P (/…/unsorted.parquet)`; `-- source: view:P (…)`), como
o help diz.

**Correção.** Em `_parqit_print_views`, cortar pela cauda (mostrar o fim do
caminho) ou alargar para 60 quando o texto começa por `view:`; ou, em
`cmd_view_copy`, usar só `view:<origem>` em `source_desc_` para a listagem e
manter a forma aninhada em `describe`. Qualquer das opções é só apresentação.

**Teste.** log-grep do `parqit views` no v125 com o nome do ficheiro.

### VC-6 — pequenas lacunas de documentação (nota, CONFIRMADO)

- A segunda forma `parqit use view:X, name(Y)` (e as variantes com aspas
  `parqit use "view:A", name(B3)` / `using "view:A"`) funciona
  (`$FA/p3/p3_ado.log`, secção 2) mas o bloco de sintaxe do help só mostra a
  forma com `using`, e o README idem; a especificação §1.5 propunha a linha
  `{cmd:parqit use view:}{it:viewname}{cmd:,} {opt n:ame(newview)}`.
- `_parqit_use` devolve `r(source_view)` na cópia; o parágrafo "Stored
  results" do help (`parqit.sthlp` ~1568) só lista `r(k)` e `r(view)` para o
  `use` lazy.
- `VIEW:A` (maiúsculas) cai na leitura de ficheiro:
  `parqit use: file not found: no file matches "VIEW:A"`, rc 601 — coerente
  com `merge`/`append` (`rfind("view:", 0)` é sensível a maiúsculas). Pode
  valer uma meia frase, ou nada.
- O restante da documentação bate com o comportamento observado: "later verbs
  on either view do not reach the other" (v125 §1, P2), "closing either leaves
  the other usable" (P1, nas duas ordens), "both read the same files", "a
  pending keep in range is checked when the copy is collected or saved" (v125
  §6), "`name()` is required and must differ", "options that describe how to
  read a file are refused" (P3), "the copy shares the source's temporary
  bridges until the last view using them closes" (P1/P4), "`views` and `show`
  name the source as `view:viewname` followed by that view's own source" (P2,
  com a ressalva de VC-5), ASSUMPTIONS #164 na íntegra.

**Correção.** Acrescentar a linha de sintaxe e `r(source_view)` ao help; uma
menção no README.

### VC-7 — a promessa "same sample" na forma `count` assenta na repetibilidade do reservoir do DuckDB (nota, SUSPEITA)

**Evidência** (`$FA/p2/p2_states.log`, secção K): `parqit sample 10, count`
na origem, cópia, e recolhas repetidas com threads por omissão e
`parqit set threads 1`:

```
rc(source sample-count collected twice) = 0
rc(copy vs source sample-count) = 0
rc(source sample-count, threads 1, vs first draw) = 0
rc(copy sample-count, threads 1, vs first draw) = 0
rc(copy vs source sample-percent, threads 1) = 0
```

**Causa/limite.** A forma percentual é determinística por construção
(`ORDER BY hash(hash(row, seed)), row` sobre `row_number()`; `View::sample`).
A forma `count` compila para `USING SAMPLE reservoir(N ROWS) REPEATABLE (seed)`.
No DuckDB 1.5.3 (`build/viewcopy/_deps/duckdb-src/src/execution/operator/helper/physical_reservoir_sample.cpp`,
lido, não compilado) o operador tem um único `SampleGlobalSinkState` com um
`ReservoirSample` semeado por `options.seed`, e `Sink` insere cada chunk nesse
reservatório sob `lock_guard<mutex> glock(global_state.lock)`; com vários
threads a ler vários row groups, a ordem de chegada dos chunks é a que os
threads produzirem. Se essa ordem varia entre execuções, o reservatório
final pode variar — não verifiquei (só testei um ficheiro de um row group).
A cópia não acrescenta risco nenhum — a seed já está escrita na etapa e é
copiada tal e qual — mas a frase do help/README/#164 promete igualdade entre
as duas vistas, que só é verdadeira se a própria origem for repetível.

**Correção.** Ou qualificar a frase ("keeps its seed, so the two views run the
same sampling step"), ou fixar com um fixture de vários row groups
(`parqit save ..., chunk()` pequeno) e `parqit set threads` > 1 no v125 §5, na
forma `count`, e só então manter a frase forte.

### VC-8 — a cópia não repete a nota XMISS-1 (nota, CONFIRMADO)

**Evidência** (`$FA/p2/p2_states.log`, secção I): `parqit use using xm.parquet,
name(X)` imprime a nota sobre `.a–.z`; `parqit use using view:X, name(Y)`
imprime só a mensagem de cópia; `collect` de Y dá `wage[1]==. & wage[2]==.`
(dobrado como na origem) e `parqit save` de Y não escreve colunas
`_parqit_xm_*` (PyArrow: `['id','year','wage','city','grp','f','day']`,
`wage[0:3] = [None, None, None]`). Comportamento igual ao da origem; só a
nota se perde para quem herda a vista.

**Correção (opcional).** Guardar no `View` a lista `xm_folded` (um
`std::vector<std::string>`, copiado com o resto) e repeti-la em
`cmd_view_copy` como `note:`.

### VC-9 — diálogo (nota, SUSPEITA — só análise estática)

`parqit_views.dlg`: `op_copy` reaproveita `ed_name` (origem), `ed_cmd`
(varlist) e `ed_value` (nome novo); `ed_name` tem `max(32)`, `ed_value` não, e
`require main.ed_name`/`require main.ed_value` estão certos. Um nome novo com
mais de 32 caracteres só falha em runtime (rc 100/198, ruidoso). A
especificação previa `ed_newname`/`ed_vars`; a reutilização é funcional. Não
abri a GUI (restrição); `dialog-lint` passa.

**Correção.** `max(32)` em `ed_value` não serve aos outros `set` (memory_limit,
tempdir), portanto ou um `EDIT ed_newname` próprio com `max(32)`, ou deixar
como está.

---

## O que verifiquei e está correto

**Plugin (`cmd_view_copy`) e engine.**
- Cópia por valor: todos os membros de `View` são valores (`std::string`,
  `std::vector`, `nlohmann::json`, `long long`, `bool`); não há ponteiros nem
  handles partilhados. Doctest "a copied view is an independent plan": 22/22
  asserções, com contagens reais sobre o fixture (`year==2020` ordenado por
  `id`, `keep in 1/2` → ids 1 e 2; `id==1` → 1) e a verificação de que
  `keep_vars` preserva `ranges_`.
- Atomicidade: validação (`load_req`, `req_text`, `target` vazio/igual, origem
  viva, `keep_vars`) toda antes de `drop_owned`/atribuição/`add_bridge_refs`/
  `g_current`; a única mutação possível após a validação é a inserção no
  `std::map` (só falharia por `bad_alloc`, apanhado pelo catch-all de
  `stata_call`, que converte qualquer exceção em rc + `SF_error`). Nenhuma
  chamada ao DuckDB.
- Protocolo: `source`, `name` e cada elemento de `varlist` seguem em hex
  (`_parqit_jtext`/`_parqit_jlist` → `_parqit_jstr`); nomes Unicode no alvo
  (v125 §8) e na origem (`view:médias` → `cópia_ç`, P4 §3) atravessam intactos.
- Recusas rc 198 com mensagem própria: sem `name()`, `name()==origem`
  (inclusive `view:Z, name(Z)` numa cópia), origem inexistente/fechada/`1x`/
  `a-b`, `view:` vazio ou com duas palavras, `clear`/`relaxed`/`encoding()`/
  `int64()` (inclusive `int64(nonsense)`, recusado antes de `_parqit_typeopts`,
  como a especificação pede)/`binary()`/`filename()`/`csv()`/`owned`, opção
  desconhecida, varlist com nome inexistente, sem vistas abertas, placeholder
  `default` morto. Em nenhuma mudou o n.º de vistas, a corrente ou o `show`.

**Pontes (P1, P4; contagem de `_parqit_bridge_*` num `TMPDIR` privado).**
- Cadeia M→N→O sobre `open _data`: a ponte sobrevive até à última das três em
  cada ordem de fecho; a última recolhe `cf _all` igual ao original; zero
  resíduos.
- Substituir a cópia por outra cópia da mesma origem, ou por uma vista de
  ficheiro, larga só as referências dela; substituir a origem
  (`open _data, name(M)` de novo) mantém a ponte antiga viva enquanto a cópia
  existir e a cópia continua a devolver o snapshot antigo; copiar a cópia de
  volta sobre o nome da origem; nome-alvo que a origem embebe por `merge`;
  origem embebida substituída; a cópia como lado `using view:` de outra vista
  (que herda as pontes); `close _all` com cópias de `open _data` e de um
  adaptador `.dta` → zero diretórios.
- Recusas não tocam nas pontes (`bridges before refusals == after`).

**Estados do plano (P2, P4), sempre com `cf _all` e/ou assinatura de
tipos/formatos/labels/`sortedby` contra a origem, `parqit keep` ou o nativo.**
- Leitura direta (#38): cópia sem varlist de vista intocada sobre ficheiro não
  ordenado → `pipeline steps: 0`, sem `ORDER BY`, recolha byte-idêntica.
- Hive (`partition_by`), glob, CSV, `parqit sql ..., name()`, glob `relaxed`,
  `query` (fragmento cru), collapse e reshape wide no plano.
- `int64(string)` herdado (a cópia recolhe `big` como texto exato
  `9007199254740993`); cópia de vista `refuse` recusa no `collect` e aceita
  `collect, int64(round)`, como a mensagem da recusa promete.
- `filename(src)`: cópia sem varlist mantém `src` (leitura direta com
  `filename_column`); varlist que a omite descarta-a ("not forced"); varlist
  que a nomeia mantém a ordem do utilizador; igual a cópia + `parqit keep`.
- xmissing: dobragem igual à origem; `save` da cópia sem companheiras.
- NAME-CASE-1: varlist com o nome exposto (`NUEMP`) ou o alias do engine
  (`nuemp`) resolve como `parqit keep`; ordem `id NUEMP nuemp` preservada.
- `sample 30` (v125 §5) e `sample 10, count` (P2 §K): mesma realização na
  cópia (ver VC-7 para o limite).
- `keep in 5/10` após `sort year id` + varlist que remove `year`: igual a
  `parqit keep id wage` e ao nativo (`sortedby` esvazia nos dois, como no
  Stata nativo); `keep in 1/100000` copiado falha no `collect` com o mesmo rc
  da origem e sem tocar na memória (v125 §6).
- Wildcards `i* d?y` → `id day`; duplicados `id id wage` → `id wage`, como
  `parqit keep`.
- `parqit save` a partir da cópia recusa (rc 198, "overlaps the open view's
  own source") escrever sobre o ficheiro fonte, sobre um caminho que casa com
  o glob fonte e dentro do diretório Hive fonte; o ficheiro fonte fica intacto
  (PyArrow: 30 linhas, 7 colunas).
- `count`, `describe`, `show`, `views`, `_dlgcontext` (vista corrente = cópia)
  sobre a cópia; `r(n_views)` sobe e desce corretamente.

**Documentação e gates.** `release_lint.sh` OK (dialog-lint e version/date);
as linhas novas do help têm no máximo 115 bytes; `{marker copy}` existe e é
referido por `{help parqit##copy:...}`; CHANGELOG `[Unreleased] ### Added`;
ASSUMPTIONS #164 descreve exatamente o que o código faz; README linha da tabela
coerente.

## Limites da auditoria

- Sem compilação: auditei o plugin `build/viewcopy` tal como estava (mais
  recente que as fontes alteradas, confirmado por `stat`).
- Sem GUI: o diálogo foi lido, não exercitado; a verificação sob Xvfb é do
  implementador.
- Dados pequenos (≤ 40 linhas, um row group); não testei VC-7 com vários row
  groups nem pressão de memória; não corri a suíte completa de 176 testes
  (máquina partilhada) — só v125, os 5 `ctest` e o lint.
- Não testei concorrência entre processos Stata (x01) com cópias; a cópia não
  cria pontes, pelo que o contrato BRIDGE-XPROC-1 não é tocado.
- Não exercitei `pivot` no plano copiado (collapse, reshape, merge, query e
  sample foram exercitados; `pivot` é mais uma etapa de valor no mesmo
  `stages_`, mas fica por confirmar empiricamente).
- A afirmação de que `direct_read` é realmente o caminho executado na cópia
  assenta em código lido (`n_stages()==0 && sort_keys().empty() &&
  pending_ranges().empty()` sobre campos copiados) mais os sinais observáveis
  (`describe` 0 passos, `show` sem `ORDER BY`, recolha idêntica); o plugin não
  expõe uma flag "direct".

---

## Resolução (25 de setembro de 2026, ramo `feat/view-copy`, sem commit)

Cada achado foi primeiro confirmado com uma sonda própria
(`audit_repro/fable_view_copy_20260925/verificacao/`) e só depois corrigido.

| ID | Resolução |
|---|---|
| VC-1 | Implementado. `_parqit_no_view_using`, chamado em `_parqit_mergein` e `_parqit_appendin` logo após o `syntax` (antes do `parqit use` interno e de `_parqit_typeopts`), recusa `view:` com mensagem própria e aponta a alternativa out-of-core (`open _data` + `merge`/`append ... using view:` + `collect`). O v125 §7 fixa o rc 198, a memória intacta (`datasignature`) e as duas mensagens. |
| VC-2 | Implementado; o v125 foi reescrito. §2 usa uma fixture não ordenada e verifica `describe` → `r(n_steps)==0` e `show` sem `ORDER BY`, antes da assinatura e do `cf`. §7 usa `_v125_refuse`, que compara n.º de vistas, vista corrente (`_dlgcontext`), `show` da corrente e n.º de pontes antes e depois de cada recusa, já com `owned`. §4 cobre a cadeia M→N→O nas três ordens de fecho, a substituição da origem, a fonte embebida por `merge` e a cópia como `using`. §14 cobre `close _all` sem resíduos. |
| VC-3 | Implementado. `_parqit_check_viewname` é chamado em `_parqit_use` (nos dois ramos), em `_parqit_sql` e em `_parqit_open` (antes de criar a ponte). Fixado no v125 §7; CHANGELOG `### Fixed`; ASSUMPTIONS #165 (VIEWNAME-ALL-1). |
| VC-4 | Implementado pela opção (a). `_parqit_view` repõe a vista anterior sempre que o comando deixou outra corrente, salvo quando fechou a própria vista do prefixo e essa era a anterior; o help foi atualizado. Fixado no v125 §9; CHANGELOG `### Fixed`; #165 (PREFIX-RESTORE-1). |
| VC-5 | Implementado. A listagem usa `_parqit_clip_mid`: os primeiros 13 caracteres, `~` e os últimos 26. Fixado no v125 §10; CHANGELOG `### Changed`. |
| VC-6 | Implementado: linha de sintaxe da segunda forma, `r(source_view)` nos Stored results, nota sobre `view:` em minúsculas, e o README. |
| VC-7 | Verificado e **não confirmado**. No DuckDB 1.5.3, `PhysicalReservoirSample::ParallelSink()` devolve `!repeatable`, e o parqit passa sempre `REPEATABLE`, pelo que o reservatório corre em série. Quatro repetições com 40 row groups e 8 threads deram a mesma amostra. A frase da documentação mantém-se e fica fixada no v125 §15 (30 row groups, 4 threads, origem repetida e cópia); ASSUMPTIONS #165. |
| VC-8 | Implementado. `View::set_xmissing_folded` é registado em `cmd_view_open`, e `cmd_view_copy` repete a nota XMISS-1. Fixado no v125 §11. |
| VC-9 | Implementado. O diálogo tem um `EDIT ed_newname` próprio, com `max(32)`. Verificado sob Xvfb: o Submit gera `parqit use make price using view:panel, name(copyv)`, e o dialog-lint passa. |

Verificação depois das correções, no build isolado `build/viewcopy`:

- `ctest` 5/5;
- `run_stata.sh` completo, **176/176 PASS** (logs em `/tmp/parqit_tests.3noicy`), incluindo o v125 reforçado;
- `release_lint.sh` OK.
