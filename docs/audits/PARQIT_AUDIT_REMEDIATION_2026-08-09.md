# Remediação do prompt documental e auditoria final do parqit — 2026-08-09

## Veredicto

**GO local para integração.** As tarefas adequadas do prompt revisto pelo
Claude foram implementadas, o resultado foi novamente auditado de forma
independente, adversarial e transversal, e todos os gates locais ficaram
verdes. Não ficou nenhum defeito confirmado por resolver no âmbito examinado.

Este é um GO do worktree Linux e não uma certificação de release
multiplataforma: não houve neste turno execução de Stata em Windows ou macOS,
nem execução dos jobs remotos do GitHub Actions. O brief continua a exigir CI
verde nos três sistemas antes de uma nova tag.

## Identidade e âmbito

- Base pública: `v0.1.24`.
- Especificação estudada:
  `PROMPT_CORRECAO_DOCS_PARQIT_2026-08-09.md`, 298 linhas,
  SHA-256
  `10a50f9b147dd9355fa1b28b5489eac5f8ea77db6f14357506df7d535fc1f134`.
- Fontes de verdade reconciliadas: `README.md`, `parqit_build_prompt.md`,
  dispatcher e parsers em `src/ado/p/parqit.ado`, help público, engine/plugin,
  testes unitários e suites Stata.
- O worktree já continha a remediação não commitada de 2026-08-08 e os
  relatórios do Claude. Foi preservado: não houve reset, restore, clean,
  reformat global, alteração de versão ou publicação.

## Implementação das tarefas do prompt

### T1 — contrato lazy sem absolutos falsos

O README, a mensagem/comentário ado, o diálogo de leitura e o comentário do
profiling passaram a distinguir quatro factos:

1. abrir uma view sonda schema e metadata;
2. CSV pode ser amostrado para inferência e `.dta`/Excel podem ser bridged;
3. nenhum resultado é carregado no dataset corrente durante a abertura;
4. verbos de contrato podem executar validações antes da materialização final.

O `release_lint.sh` ganhou uma barreira contra o regresso das formulações
absolutas conhecidas, incluindo “nothing is read”, “nothing executes until” e
equivalentes sobre memória Stata.

### T2 — `ty()` é uma extensão declarada

O help identifica agora `ty(yyyy)` como extensão do parqit. A decisão foi
baseada num oráculo vivo StataNow MP 19.5:

- nativo: `ty(2026)` devolve rc 133, `unknown function ty()`;
- parqit: aceita a expressão e devolve o period count 2026;
- nativo escreve um valor `%ty` como o ano nu, por exemplo `2026`.

Retirar `ty()` reduziria uma superfície publicada sem corrigir um valor. A
decisão manter-e-documentar e a alternativa rejeitada ficaram registadas em
`ASSUMPTIONS.md` (TY-EXT-1). A lista help/translator continua verificada nos
dois sentidos pelo release lint.

### T3 — o qualificador `if` segue o modo de missing

O help diz agora explicitamente que o `if` de `gen` e `replace` funciona como
filtro: em modo SQL, uma comparação missing exclui a linha; com
`statamissing on`, reproduz a ordenação do Stata. O teste `v66` mantém este
contrato e a delimitação de `_n`/`_N`.

### T4 — segundo oráculo nativo de expressões (`v68`)

Foi acrescentado `tests/verify_suite/v68_expr_native_oracle2.do`. O teste cria
os dados, calcula os resultados nativos e lazy no mesmo processo Stata e cobre:

- arredondamento e datas com argumentos fracionários ou impossíveis;
- `inrange`, `cond`, `min`, `max` e `inlist` com missings;
- `string()`/`real()` em extremos e notação científica;
- case folding ASCII-only, UTF-8 e `strpos` multibyte;
- cadeias relacionais e as duas modalidades de missing;
- a divergência documentada de `substr` quando corta um codepoint;
- a extensão `ty()`;
- `reshape long` com zeros à esquerda;
- colunas hostis chamadas `__PARQIT_ROW__` e `__parqit_rn_1`.

Resultado final: `VERDICT(V68_EXPR_NATIVE_ORACLE2): PASS`. Uma cópia temporária
com o esperado `ty()` deliberadamente alterado de 2026 para 2027 produziu
`FAIL TY-PARQIT` e veredicto FAIL, confirmando que o gate não é vacuoso.

### T5 — varredura de nomes desconhecidos (`v69`)

O teste novo `v69_raw_error_sweep.do` percorre 51 casos públicos. Cada caso
exige rc não zero, mensagem própria, recusa atómica e ausência de:

- `Binder Error`, `Parser Error`, `Catalog Error` ou `Invalid Input Error`;
- SQL gerado ou `LINE 1`;
- nomes `__parqit_s*`, `__PARQIT_*` ou outros identificadores internos.

O primeiro run encontrou uma raiz com quatro manifestações — `reshape long`
com primeiro ou segundo `i()` desconhecido e `reshape wide` com `i()` ou `j()`
desconhecido — que originaram dez violações dos checks de mensagem. A
preflight foi movida para antes das queries de unicidade/missingness.

Uma comparação nativa final encontrou ainda que stubs long/wide inexistentes
devem devolver rc 111. O plugin passou a validar também esses stubs antes de
qualquer scan. `audit_repro/repro_reshape_name_not_found.do` reproduz os seis
caminhos e `ASSUMPTIONS.md` regista RESHAPE-NAME-1.

Resultado final: `VERDICT(V69_RAW_ERROR_SWEEP): PASS - 51 unknown-name cases
refuse atomically with no engine SQL or internal names`.

## Achados adicionais da auditoria independente

### A3 — precisão do modelo de execução e dos adapters

O help e o README foram verificados contra os caminhos reais do plugin. Foram
eliminadas ou afinadas as seguintes simplificações:

- previews podem materializar um número limitado de linhas numa scratch frame,
  mas nunca substituem o dataset corrente;
- Parquet suporta projection e row-group pruning; CSV é processado como stream
  out-of-core, sem row groups Parquet;
- `collect` usa direct-read numa view fonte pura e uma tabela temporária
  spillable num resultado transformado;
- `describe source` é uma inspeção de footer exclusivamente Parquet; os outros
  exploradores podem ler os dados relevantes;
- `.tab` foi incluído entre os formatos delimitados;
- `open _data` é snapshot temporário Parquet + scan, não scan direto da memória;
- apenas `collect`/`save` materializam o resultado completo; os comandos de
  exploração executam queries limitadas, agregadas ou de metadata;
- a limitação de binary `strL` foi restringida ao caso real: NUL numa escrita
  direta a partir da memória; Parquet-to-Parquet lazy preserva os bytes;
- `%tC` e `%tb` passaram a constar explicitamente do resumo de tipos do README.

### A4 — rc nativo de incompatibilidade de tipos nas chaves

O gate anterior tinha melhorado a mensagem de uma chave `merge`/`joinby`, mas
tratava nome ausente e tipo incompatível como rc 111. Um probe vivo mostrou:

```text
RC_NATIVE_MERGE=106 RC_NATIVE_JOINBY=106
RC_PARQIT_MERGE=106 RC_PARQIT_JOINBY=106
```

Foi corrigida a classificação sem duplicar a validação:

- `View::join_keys_error()` continua a ser a única origem do diagnóstico e
  expõe opcionalmente a classe type-mismatch;
- o plugin devolve rc 106 para numeric/string e rc 111 só para nome ausente;
- `test_view.cpp` separa as duas classes;
- `v67` verifica rc e mensagens de `merge` e `joinby`;
- `repro_merge_key_not_found.do` cobre os dois verbos e o caminho válido;
- JOINKEY-RC-1 ficou registado em `ASSUMPTIONS.md`.

O repro termina em PASS, e o estado da view permanece atómico em todas as
recusas.

## Auditoria específica do help

O help final tem 1 302 linhas SMCL e cobre todos os subcomandos públicos do
dispatcher, exceto o helper interno `_dlgvars`, deliberadamente não público.
A auditoria reconciliou:

- formas de sintaxe e opções;
- verbos lazy, adapters, named views e bridges;
- materializadores e família de exploração;
- SQL, settings, diagnostics e diálogos;
- lista completa de funções de expressão;
- tipos, datas, metadata e valores não representáveis;
- limites, atomicidade e stored results `r()`.

As garantias executáveis são:

- `release_lint.sh`: dispatcher → syntax, translator ↔ lista de funções,
  links → markers, SMCL inline e vocabulário lazy;
- `v66_help_contract.do`: opções, stored results, missing mode, `_n`/`_N` e
  formas de `use`;
- `v67_runtime_message_contract.do`: texto e autoria das mensagens;
- `v68`/`v69`: semântica e fronteira de erros descritas pelo help.

Em Stata batch, `help parqit` devolveu rc 0 (o viewer é naturalmente ignorado
em batch). A tradução independente `smcl2txt` devolveu rc 0, produziu 1 203
linhas e não deixou tokens SMCL por interpretar. A cópia em `ado/plus/p` ficou
idêntica à fonte.

## Decisões deliberadas de não alteração

Mantiveram-se as decisões justificadas no prompt:

- não alterar o unquote interno de `sortedby_names()`, inalcançável enquanto o
  manifesto aceitar apenas nomes Stata sanitizados;
- não acrescentar guards caros a agregados sem divergência observável;
- manter as divergências documentadas do modo SQL-missing e do `substr` que
  substitui um corte UTF-8 inválido por U+FFFD;
- manter `ty()` como extensão documentada;
- não impor um sort implícito de todas as colunas para fabricar ordem dentro de
  empates sem um total order declarado.

## Evidência final

Executada sobre o estado final deste registo:

| Gate | Resultado |
|---|---|
| `cmake --build build/dev -j` | PASS; plugin, testes e árvore ado local construídos |
| `ctest --preset dev` | 3/3 PASS |
| `./build/dev/parqit_tests` | 73/73 casos PASS; 1 071/1 071 assertions; 1 caso skipped |
| `tests/run_stata.sh` integral | 89/89 veredictos PASS; 0 FAIL/abort/log ausente; raiz `/tmp/parqit_tests.74ygCr` |
| `tests/run_stata.sh v6` | v60–v69 PASS |
| `v68` com mutação deliberada | FAIL, como exigido |
| repro reshape | PASS |
| repro merge/joinby | PASS |
| probe nativo de rc de chave | nativo/parqit `merge=106`, `joinby=106` |
| `bash tests/release_lint.sh` | PASS (`v0.1.24`, datas e superfícies coerentes) |
| help + `translate ..., smcl2txt` | rc 0; 1 203 linhas; zero markup residual |
| fonte vs `ado/plus/p` | `parqit.ado` e `parqit.sthlp` idênticos |
| `git diff --check` | PASS |

## Riscos residuais e fronteira do veredicto

- A execução runtime deste turno foi Linux x86_64 com StataNow MP 19.5.
- Não foram executados os plugins Windows x86_64 ou macOS arm64; essa prova
  pertence ao CI/release multiplataforma.
- Os diálogos foram auditados como superfícies de comandos e o contrato batch
  passou, mas não foi feita uma sessão GUI manual neste turno.
- Não houve benchmark novo; nenhuma alegação quantitativa de desempenho foi
  acrescentada.
- Não foi feito commit, push, tag ou release.

Dentro destes limites, o help reflete a superfície implementada e os caminhos
alterados estão cobertos por oráculos independentes, regressões focadas e a
suite integral.
