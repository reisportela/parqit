# Auditoria adversarial holistica ao parqit

Data: 2026-07-03  
Modo: auditoria read-only ao codigo e aos artefactos existentes; nao foram
alterados ficheiros de codigo, testes, build, ado, dlg ou documentacao existente.
Este ficheiro e apenas o relatorio pedido para triagem posterior pelo Claude.

## Escopo e metodo

Leitura obrigatoria feita primeiro: `README.md` e `parqit_build_prompt.md`.
Usei tambem `ASSUMPTIONS.md`, `CHANGELOG.md`, `src/ado/p/parqit.ado`,
os dialogos `src/ado/p/parqit_*.dlg`, o plugin C++ em `src/plugin/` e
`src/engine/`, o manifesto `src/ado/p/parqit.pkg`, o workflow
`.github/workflows/build.yml`, suites em `tests/`, logs existentes de Stata em
`/tmp/parqit_tests.AtJP5a`, e o log local de `ctest` em
`build/dev/Testing/Temporary/LastTest.log`.

Nao rerodei a suite completa porque `tests/run_stata.sh` cria novos diretorios e
logs. Em vez disso, usei evidencias ja existentes:

- `bash tests/release_lint.sh` foi executado nesta auditoria e passou:
  `release-lint OK: v0.1.16 (02jul2026 / pkg 20260702); CHANGELOG top [0.1.16]`.
- O log existente de `ctest` de 2026-07-02 mostra `63/63` testes unitarios
  passados, `970/970` asserts.
- O resumo existente `/tmp/parqit_tests.AtJP5a/VERDICTS_SUMMARY.txt`, datado de
  2026-07-02 18:25, mostra PASS na suite Stata completa: integracao, roundtrip e
  `verify_suite` ate `v45`.

Conclusao de base: o nucleo do parqit esta muito melhor coberto do que um pacote
Stata normal. Nao encontrei uma nova corrupcao de valores reproduzida nesta
passagem. Os riscos mais importantes estao agora na camada DLG/menu, na
consistencia de release/instalacao, e num ponto de metadados/proveniencia no
caminho lazy `view -> save`.

## Tese do produto que deve guiar as correcoes

O contrato central e: `parqit use` abre uma view lazy por defeito; verbos Stata
constroem um plano; so `parqit collect` ou `parqit save` materializam. O produto
nao e mais um leitor Parquet. A UX, os dialogos, os defaults e a documentacao
devem empurrar o utilizador para esta arquitetura, nao para o padrao
`read -> collect -> memory`.

## Resumo executivo dos principais riscos

1. **Alto: o dialogo de leitura contraria a tese lazy.** `parqit_read.dlg`
   vem com `clear` activo por defeito e emite `parqit use ..., clear`. Isto
   transforma o fluxo de entrada num leitor eager por defeito.
2. **Alto: o dialogo de materializacao faz `collect, clear` por defeito.** Isto
   ignora a protecao normal de `parqit collect` sem `clear` contra substituir
   dados em memoria.
3. **Alto/UX: os dialogos nao sao view-aware para variaveis.** A maioria usa
   `EDIT` livre; `EXP` usa o expression builder de Stata, que conhece a base em
   memoria, nao necessariamente a view lazy aberta.
4. **Medio/alto: o motor ja aceita diretorios Parquet, mas o DLG usa `FILE`
   picker.** O utilizador deve poder seleccionar uma pasta/Hive tree, nao so um
   ficheiro.
5. **Alto/release: `parqit.pkg` anuncia `MACINTEL64`, mas o CI/release nao
   constroi nem empacota `parqit_macintel64.plugin`.** A README diz que macOS
   Intel nao esta neste release; o manifesto promete o contrario.
6. **Medio/alto: `view_save` parece perder o nome original de colunas
   estrangeiras sanitizadas.** O caminho eager preserva `char var[src_name]`;
   o caminho lazy guarda na metadata da view `src = c.name`, ou seja, o nome
   Stata sanitizado.
7. **Medio: ha um `src/ado/p/parqit.plugin` local, ignorado pelo git, grande e
   antigo, que pode mascarar o build actual quando se usa `adopath ++ src/ado/p`
   sem `PARQIT_PLUGIN_PATH`.**
8. **Medio: os `.dlg` estao quase todos com cabecalho `0.1.15` enquanto o pacote
   esta em `0.1.16`, e o `release_lint` nao verifica dialogos.**

## Achados detalhados

### F1. DLG de leitura materializa por defeito

Evidencia:

- `src/ado/p/parqit_read.dlg:56-60` define `ck_clear` com `default(1)` e label
  "Read into memory now".
- `src/ado/p/parqit_read.dlg:76-91` emite `parqit use ...` e passa a opcao
  `clear` se a checkbox estiver marcada.
- O proprio `parqit.ado` documenta no codigo que `parqit use` e lazy por
  defeito: `src/ado/p/parqit.ado:239-281`.

Risco: a primeira experiencia grafica rebaixa o produto para "leitor Parquet
para Stata", precisamente o que a tese do projeto diz que nao e. Em dados grandes
isto tambem pode criar a primeira falha do utilizador: tentar meter em memoria
aquilo que deveria ficar out-of-core.

Recomendacao:

- Inverter o default: checkbox desmarcada, label primaria "Open lazy view
  (recommended)".
- Manter "Read into memory now" como opcao secundaria explicita.
- Activar `name()` quando lazy esta seleccionado.
- O comando default do dialogo deve ser `parqit use using "..."`, sem `clear`.
- Adicionar lint/teste estatico que falha se `parqit_read.dlg` voltar a ter
  `option(clear) default(1)`.

### F2. DLG de collect/save tem `collect, clear` por defeito

Evidencia:

- `src/ado/p/parqit_write.dlg:17-23` chama `script collect_on` no `PREINIT`.
- `src/ado/p/parqit_write.dlg:47-55` poe `Collect` como primeiro radio.
- `src/ado/p/parqit_write.dlg:98-102` emite sempre `parqit collect, clear`.
- O comando publico protege o utilizador quando `clear` nao e dado:
  `src/ado/p/parqit.ado:892-897`.

Risco: no CLI, `parqit collect` sem `clear` recusa substituir dados alterados em
memoria; no DLG, o default contorna essa protecao. Isto e perigoso para um menu
apontado a utilizadores menos tecnicos.

Recomendacao:

- Separar "Collect" de "replace current Stata data".
- Por defeito emitir `parqit collect`, nao `parqit collect, clear`.
- Adicionar uma checkbox explicita "Replace current data in memory" que so entao
  adiciona `clear`.
- Considerar um primeiro modo seguro: `parqit show` ou `parqit describe` da view,
  com `collect/save` como accao deliberada.

### F3. Escolha de variaveis por dropdown/lista ainda nao existe

Evidencia:

- `parqit_vars.dlg` usa `EDIT ed_vars`, `EDIT ed_old`, `EDIT ed_new`
  (`src/ado/p/parqit_vars.dlg:56-71`).
- `parqit_pivot.dlg` usa `EDIT` para `rows`, `cols`, `values`
  (`src/ado/p/parqit_pivot.dlg:26-59`).
- `parqit_stats.dlg` usa `EDIT` para varlists (`src/ado/p/parqit_stats.dlg:114-175`).
- `parqit_combine.dlg` usa `EDIT` para keys e `keepusing`
  (`src/ado/p/parqit_combine.dlg:86-94`).
- `parqit_filter.dlg` e `parqit_gen.dlg` usam `EXP`, que abre o expression builder
  de Stata (`src/ado/p/parqit_filter.dlg:49-52`,
  `src/ado/p/parqit_gen.dlg:55-63`).
- Ja existe uma rota leve para listar colunas da view: `parqit ds` chama
  `view_info describe` e devolve `r(varlist)` (`src/ado/p/parqit.ado:1068-1079`;
  `src/plugin/plugin_view.cpp:753-759`).

Risco: se a view lazy nao coincide com a base em memoria, o expression builder e
os campos livres induzem erros. O utilizador visual nao sabe que variaveis estao
na view; tambem nao tem descoberta de tipos/labels.

Recomendacao:

- Criar uma camada pequena de suporte a DLG, por exemplo `parqit _dlg_schema`
  ou `parqit schema, frame(_parqit_schema)` que exponha `name`, `kind`, `format`,
  `label`, `source`.
- Nos dialogos, adicionar "Refresh from current view" e uma lista/dropdown
  view-aware. Se o DLG nativo nao permitir lista dinamica directa, usar uma
  frame/dataset de metadados como proxy para controlos standard de Stata.
- Para expressoes, nao confiar no `EXP` builder como unica UX: adicionar botoes
  para inserir variavel, operadores frequentes, `missing()`, `inrange()`, `td()`,
  `tm()`, etc., baseados no esquema da view.

### F4. O DLG nao oferece seleccao de pasta embora o motor aceite diretorios

Evidencia:

- O motor detecta diretorios e transforma em `dir/**/*.parquet` com
  `hive_partitioning = true`: `src/plugin/plugin_io.cpp:121-166`.
- O dialogo de leitura usa `FILE fi_using` com filtro de ficheiros:
  `src/ado/p/parqit_read.dlg:20-25` e `:44-49`.
- O dialogo de combine tambem usa `FILE fi_using`:
  `src/ado/p/parqit_combine.dlg:73-78`.
- O proprio label diz "Hive directory", mas o controlo e um file picker.

Risco: a funcionalidade core existe, mas a UI torna-a dificil ou invisivel.
Isto e especialmente mau para Parquet particionado, que e uma das formas naturais
de usar o DuckDB out-of-core.

Recomendacao:

- Adicionar modo "File / glob / folder".
- Se Stata DLG nao tiver picker de diretorio portavel, usar um `EDIT` de caminho
  com um botao/ajuda especifico "Paste folder path" e exemplos de `*.parquet`.
- No write DLG, tratar `partition_by()` como escrita para diretorio e ajustar o
  controlo "Save as..." para nao sugerir apenas ficheiro `.parquet`.

### F5. Dialogos fora dos gates de release

Evidencia:

- Sete dialogos ainda declaram `VERSION 0.1.15 02jul2026`; so
  `parqit_pivot.dlg` esta em `0.1.16`.
- `tests/release_lint.sh` verifica `CMakeLists.txt`, `parqit.ado`,
  `parqit.sthlp`, README, `parqit.pkg` e CHANGELOG, mas nao verifica `.dlg`
  (`tests/release_lint.sh:24-75`).
- Ha descricao em `CHANGELOG.md` de que os dialogos entraram no release, mas nao
  encontrei teste versionado que exercite ou linterize os comandos gerados pelos
  `.dlg`. O unico `examples/parqit_dlg.do` e nao versionado e tem caminhos
  absolutos locais.

Risco: o DLG pode quebrar silenciosamente a superficie publicitada. O historico
ja teve bug de release em que os `.dlg` nao entravam nos assets; agora entram,
mas nao ha gate suficiente sobre versao/defaults/comandos gerados.

Recomendacao:

- Estender `release_lint.sh` para exigir que todos os `parqit_*.dlg` tenham a
  mesma versao/data do `.ado`.
- Adicionar lint de pacote: todo `f` em `parqit.pkg` deve existir; todo `.dlg`
  listado deve ser incluido no workflow de release.
- Adicionar um teste estatico simples para os defaults perigosos: read nao pode
  defaultar `clear`; write nao pode hard-codear `collect, clear` sem checkbox.

### F6. Manifesto promete macOS Intel mas o release nao o constroi

Evidencia:

- O workflow comenta/remocao temporaria de `macos-x86_64`:
  `.github/workflows/build.yml:30-37`.
- O assembly de zips so trata `linux_x86_64`, `macos_arm64`, `windows_x86_64`:
  `.github/workflows/build.yml:125-152`.
- O release upload so inclui `parqit_macarm64.plugin`, `parqit_linux64.plugin`,
  `parqit_win64.plugin`: `.github/workflows/build.yml:165-174`.
- `src/ado/p/parqit.pkg:40` ainda declara
  `g MACINTEL64 parqit_macintel64.plugin parqit.plugin`.
- A README avisa que macOS Intel nao esta neste release:
  `README.md:141-144`.

Risco: um utilizador em Stata macOS Intel pode receber uma falha de instalacao
por ficheiro inexistente, apesar de o manifesto prometer a plataforma. Isto e
mais grave do que "nao suportado": e um contrato de package incoerente.

Recomendacao:

- Ou reactivar build/release de macOS Intel;
- Ou remover temporariamente a linha `g MACINTEL64 ...` do `.pkg` e documentar
  claramente que nao ha binario net-install para Intel.
- Adicionar uma verificacao de release que compara as linhas `g` do `.pkg` com
  os artefactos realmente gerados.

### F7. Exemplo recomendado de install aponta para versao antiga

Evidencia:

- README status: `v0.1.16`.
- Exemplo recomendado usa `v0.1.13`: `README.md:84-92`.

Risco: um utilizador que copia o comando "recommended" instala uma versao antiga
e depois reporta bugs ja corrigidos. Isto e especialmente provavel porque a
README tambem menciona `latest`, mas apenas como nota secundaria.

Recomendacao:

- Trocar o exemplo principal para
  `https://github.com/reisportela/parqit/releases/latest/download`.
- Se for preferivel versao fixa, manter o exemplo em `v0.1.16` e adicionar uma
  regra no release lint que detecta drift do tag no README.

### F8. Possivel perda de proveniencia de nomes originais no caminho lazy save

Evidencia estatica:

- O caminho eager planeia colunas com `source_name` e `stata_name`, e o response
  record inclui o original: `src/plugin/plugin_io.cpp:524-532`. A ado depois
  grava `char var[src_name]` quando nome original difere do nome Stata.
- `view_open` sanitiza nomes e guarda apenas `ViewCol.name = stata_names[c]`;
  quando ha rename, emite so warning: `src/plugin/plugin_view.cpp:503-533`.
- `view_kv_fragment()` grava `jv["src"] = c.name`, isto e, o nome actual da view,
  nao o nome Parquet original: `src/plugin/plugin_view.cpp:291-305`.
- Os testes existentes cobrem bem o caminho eager (`v02`, `v16`) e a
  persistencia eager `load -> save, data -> load` (`v36`), mas nao vi teste
  especifico para `foreign hostile names -> parqit use` lazy -> `parqit save`
  -> reload preservando `src_name`.

Risco: em `parqit use using foreign.parquet` seguido de `parqit save out.parquet`,
uma coluna estrangeira `"unit cost"` pode sair como `unit_cost` sem metadata
recuperavel do nome original. Valores continuam correctos, mas isto fere a
promessa de esquema reversivel/metadados e afecta workflows que dependem de
nomes originais em ficheiros de terceiros.

Recomendacao:

- Adicionar `source_name`/`origin_name` a `ViewCol`.
- Em `view_open`, preencher com o nome Parquet original e, quando sanitizado,
  inserir/transportar `src_name` nas chars da view.
- Em transformacoes que criam colunas novas (`gen`, `collapse`, `pivot`,
  `reshape`), definir `source_name` como o nome novo, porque ai ja nao ha
  origem Parquet 1:1.
- Alterar `view_kv_fragment()` para gravar `src = origin_name`.
- Criar `v46_lazy_srcname_viewsave.do`: pyarrow cria `"raw name"`; `parqit use`
  lazy; `parqit save`; `parqit use ..., clear`; assert `char raw_name[src_name]`
  e payload.

### F9. Artefacto local ignorado pode mascarar o plugin actual

Evidencia:

- Existe `src/ado/p/parqit.plugin`, ignorado por `.gitignore`, com 917 MB e data
  2026-06-23.
- O build actual esta em `build/dev/parqit.plugin`, 920 MB, data 2026-07-02.
- O install repo-local esta em `ado/plus/p/parqit.plugin`, 40 MB, data 2026-07-02.
- `_parqit_ensure_plugin` usa `findfile parqit.plugin` se `PARQIT_PLUGIN_PATH`
  nao estiver definido (`src/ado/p/parqit.ado:38-72`).

Risco: um teste manual com `adopath ++ src/ado/p` e sem `PARQIT_PLUGIN_PATH`
pode carregar o plugin antigo de `src/ado/p` enquanto o `.ado` e `0.1.16`. Isto
gera falsos bugs ou falsos PASS.

Recomendacao:

- Apagar o artefacto local ignorado fora de uma mudanca de codigo, ou mover
  plugins locais sempre para `ado/plus/p`.
- Documentar no workflow de desenvolvimento: usar `ado/plus/p` ou definir
  `PARQIT_PLUGIN_PATH="$PWD/build/dev/parqit.plugin"`.
- Considerar um check de desenvolvimento que avise se existe `src/ado/p/*.plugin`.

### F10. Script de benchmark/profile esta stale depois dos short names

Evidencia:

- Codigo activo usa helpers Mata curtos `_parqit_wr_*`; nenhum nome `_parqit_*`
  activo em `src/ado/p/parqit.ado` excede 32 caracteres.
- Nao ha residuos versionados de `slab`/`_slab` na superficie publica.
- `benchmarks/profile_collect.do:97` ainda chama
  `_parqit_write_collect_request`, que ja nao existe no `.ado` actual.

Risco: benchmark/profiling manual falha antes de medir, e alguem pode confundir
isso com regressao do collect.

Recomendacao:

- Actualizar ou remover `benchmarks/profile_collect.do`.
- Adicionar uma lint pequena para detectar chamadas a `_parqit_write_collect_request`
  e outros nomes de helpers removidos.
- Manter a convencao actual: prefixo curto `_parqit_wr_*` para request writers;
  nao encurtar nomes publicos.

### F11. Falta uma camada DLG de gestao de views

O menu tem leitura, explore, stats, filter, vars, gen, pivot, combine, write e
help (`src/ado/p/parqit.ado:120-153`). Dentro da logica do produto, falta uma
area visual para:

- `parqit views`
- mudar `parqit view <name>`
- fechar uma view ou `_all`
- `parqit show` e `parqit explain`
- `parqit set statamissing`, `threads`, `memory_limit`, `tempdir`

Isto e aditivo e encaixa bem no modelo actual: tudo deve emitir comandos
`parqit` normais e reproduziveis na Review window.

### F12. DLG de combine nao cobre todo o `merge`

Evidencia:

- O comando publico suporta `merge ..., generate(name) nogenerate`
  (`src/ado/p/parqit.sthlp:303-307`; parser em `src/ado/p/parqit.ado:1620-1677`).
- O dialogo de combine tem campo "Source marker variable" para append e desactiva
  `ed_gen` no modo merge (`src/ado/p/parqit_combine.dlg:30-40`,
  `:108-111`, `:122-156`).

Risco: usuarios do menu nao conseguem escolher o nome da variavel `_merge`, so
podem aceitar `_merge` ou `nogenerate`.

Recomendacao:

- Separar "merge result variable" de "append source marker".
- No modo merge, permitir `generate(name)` e `nogenerate` como opcoes
  mutuamente claras.

### F13. Pequena deriva em ASSUMPTIONS.md

`ASSUMPTIONS.md:80-83` descreve atomic collect como tempframe -> save tempfile
-> use. O codigo actual faz frame staging e swap por `frame drop/rename` sob
`nobreak` (`src/ado/p/parqit.ado:333-385`). A semantica parece correcta e ate
melhor, mas a nota esta stale.

Risco baixo, mas este ficheiro e usado como memoria de decisoes tecnicas; quando
stale, induz fixes errados.

Recomendacao: alinhar a descricao quando houver uma proxima ronda de doc-only.

## Avaliacao especifica dos short names

Estado geral: bom.

- A migracao publica `slab -> parqit` parece completa na superficie versionada:
  `git grep` nao encontrou `slab`, `_slab`, `SLAB` ou `miparqiteled` em
  `README.md`, `CHANGELOG.md`, `src`, `tests`, `examples`, `benchmarks`,
  `CMakeLists.txt` e `.github`.
- A solucao `_parqit_wr_*` para writers Mata e correcta: evita o problema de
  limites de nomes de Stata sem encurtar o comando publico `parqit`.
- O unico ponto fraco que encontrei e periferico: `benchmarks/profile_collect.do`
  ainda usa o nome antigo `_parqit_write_collect_request`.

Recomendacao para Claude: nao redesenhar a convencao. Apenas limpar o benchmark
stale e adicionar lint para impedir que nomes longos/antigos voltem.

## Melhorias aditivas alinhadas com a logica existente

Prioridade 1: DLG como front-end lazy, nao reader.

- `parqit_read`: default lazy, view name visivel, pasta/glob bem suportados.
- `parqit_write`: collect/save so como materializadores explicitos; collect sem
  `clear` por defeito.
- `parqit_vars/stats/pivot/combine`: var pickers a partir do esquema da view.
- `parqit_filter/gen`: expression helper view-aware.

Prioridade 2: introspeccao visual.

- Dialogo "Views / SQL": listar views, mudar current, show, explain, close.
- Dialogo "Settings": statamissing, threads, memory_limit, tempdir.

Prioridade 3: release gates.

- `.dlg` version/date lint.
- `.pkg` platform/assets consistency.
- README install URL lint.
- detectar `src/ado/p/*.plugin` em dev/local lint.

Prioridade 4: metadados/proveniencia.

- `ViewCol` deve carregar nome original quando a coluna veio 1:1 de fonte
  Parquet/CSV.
- `view_save` deve preservar `src_name` para passthrough/projection/rename
  quando a origem ainda e recuperavel.

Prioridade 5: features futuras, sem mexer no contrato actual.

- `collapse` com pesos, hoje rejeitado de forma segura.
- opcao futura para preservar identidade de `.a`-`.z` por sidecar metadata/RLE.
- modo regex mais Stata-like ou rejeicao opcional de constructs RE2 fora do
  subconjunto comum.
- selector de pasta/Hive tree e construtor de glob no DLG.

## Plano de verificacao recomendado para a proxima ronda

Narrow tests antes de full suite:

1. `bash tests/release_lint.sh`
2. novo lint DLG/package:
   - todos `parqit_*.dlg` com versao/data iguais ao `.ado`;
   - `parqit_read.dlg` nao defaulta `clear`;
   - `parqit_write.dlg` nao emite `collect, clear` sem checkbox;
   - linhas `g` do `.pkg` batem com assets do workflow.
3. novo `v46_lazy_srcname_viewsave.do`.
4. teste pequeno de comando gerado pelos DLGs, se for viavel em batch; caso nao,
   linter estatico dos `PROGRAM command`.
5. `ctest --preset dev`
6. `STATA=stata-mp BUILD_DIR="$PWD/build/dev" bash tests/run_stata.sh v46`
7. full suite local antes de release:
   `STATA=stata-mp BUILD_DIR="$PWD/build/dev" bash tests/run_stata.sh`

## Nota final para Claude

Nao comecar por reescrever o nucleo C++: a evidencia actual favorece fixes
cirurgicos. A prioridade deve ser alinhar o menu/DLG com a tese lazy, endurecer
release gates, e fechar o ponto de proveniencia `view_save`. O nucleo de tipos,
atomicidade, ranges, metadata eager, hostile names, joins, reshape/pivot e IO
ja tem cobertura forte e PASS recente; qualquer mudanca ai deve ser justificada
por teste adversarial novo.
