# Auditoria adversarial da fiabilidade dos dados no `parqit` — 14 de julho de 2026

## 1. Veredito executivo

**NO-GO. Não é possível certificar a release `v0.1.21` como fiável para os
dados dentro do padrão de confiança pedido.** Foram confirmadas perdas ou
alterações silenciosas de payload, divergências entre materializadores,
resultados semanticamente diferentes do Stata e perdas/contaminações de
metadados. Pelo critério do prompt, a existência de qualquer S0/S1 impede a
declaração de segurança.

Contagem consolidada desta auditoria:

| Severidade | Confirmados | Interpretação |
|---|---:|---|
| S0 | 5 | perda/alteração silenciosa de dados |
| S1 | 8 | resultado semântico ou metadado relevante incorreto |
| S2 | 5 | atomicidade, lifecycle ou portabilidade |
| S3 | 2 | documentação, teste ou packaging |

Os bloqueadores mais importantes são:

1. um save bem-sucedido apaga sem aviso um ficheiro ou uma árvore pertencente
   ao utilizador se ocupar o nome previsível `<dest>.parqit_tmp`;
2. duas chaves `UINT64` adjacentes acima de `2^53` tornam-se uma só chave antes
   de `contract`, e uma expressão `2^53+1` escreve valores diferentes conforme
   termina em `collect`/save direto ou save lazy;
3. o fast `save, data` trunca silenciosamente um `strL` no primeiro NUL;
4. estado de sort obsoleto depois de `replace`/`append` associa `_n` ou
   `keep in` às observações erradas;
5. o writer lazy pode escrever um tipo físico diferente do tipo explícito e do
   próprio KV `parqit.schema`.

Isto não significa que todos os caminhos estejam defeituosos. A compilação
limpa, CTest, 71 testes Stata, smoke da release pública, 30 casos diferenciais
aleatórios e vários controlos físicos passaram. Esses PASS são reais mas
condicionados à cobertura: os contraexemplos confirmados ficaram fora da suite
oficial, que continuou integralmente verde.

Esta fase foi não destrutiva relativamente ao produto. Não foram alterados
ficheiros versionados, código, versão, tag, release, instalação global do Stata
ou configuração do utilizador. O único write no checkout é este relatório; os
repros, fixtures, builds e logs vivem sob `/tmp`.

## 2. Snapshot, proveniência e ambiente

### 2.1 Fonte auditada

| Item | Valor |
|---|---|
| Checkout | `/home/mangelo/Documents/GitHub/parqit` |
| Branch | `main` |
| Commit | `ffb9c7071cc091d7a451f8e5d942a623af857b97` |
| Tag local | `v0.1.21`, anotada, peeled para o mesmo commit |
| Remote `main` | o mesmo commit, confirmado antes dos testes |
| Remote `v0.1.21^{}` | o mesmo commit |
| Versão/data | `0.1.21`, `14jul2026`, coerentes em CMake, ado, pkg e CHANGELOG |
| Estado inicial tracked | limpo |
| Estado inicial untracked | preservado, enumerado na secção 13 |

O snapshot exato foi extraído para:

```text
/tmp/parqit_audit_20260714_ffb9c707_GXyk8M
```

O build dev fresco desse snapshot tem SHA-256:

```text
51a748a65f309fc3835ce884046adcf47fb2deb6a2ce16f125eedd98e3ef574a  build/dev/parqit.plugin
```

O runner Stata recebeu explicitamente esse path; não dependeu do ado/plugin
global. O plugin dev não é apresentado como binário de distribuição: é ELF
com debug e dependências de desenvolvimento. A release pública foi testada
separadamente, pelo seu próprio hash.

### 2.2 Ferramentas

| Componente | Versão/superfície |
|---|---|
| SO de runtime | Linux x86-64 |
| CMake | 3.26.5 |
| GCC/G++ | 11.5.0 |
| DuckDB C++ embebido | 1.5.3, archive pinado |
| Stata | StataNow 19.5 MP, 16 cores, Unix x86-64 |
| Python | 3.12.9 |
| PyArrow | 24.0.0 |
| DuckDB Python | 1.4.4 |

O archive DuckDB usado na configuração fresca foi o cache do mecanismo pinado,
com SHA-256 esperado:

```text
f22a7cfb3e72be3010f4a7f2fbdd8de7d62fa036b838543acb663a722a7a71df
```

### 2.3 Release pública

- Release: `v0.1.21`, publicada, não draft e não prerelease.
- Página: <https://github.com/reisportela/parqit/releases/tag/v0.1.21>
- Workflow da tag: <https://github.com/reisportela/parqit/actions/runs/29339898404>
- Todos os jobs reportados passaram: release lint, Linux x86-64, macOS arm64,
  Windows x86-64 e montagem dos ZIPs.
- Os digests publicados pelo GitHub coincidiram com os hashes calculados aos
  downloads locais.

## 3. Fluxo de dados e invariantes exigidos

```text
Stata memory
  -> direct save / fast direct save
  -> Parquet físico
  -> lazy view e normalização de fronteira
  -> transformação do plano
  -> collect -> Stata memory
  -> ou lazy save -> novo Parquet
```

```text
Parquet / CSV / DTA / Excel
  -> scan ou adapter/bridge owned
  -> lazy plan
  -> collect ou save
  -> output publicado atomicamente
```

| Transição | Invariante de fiabilidade | Resultado |
|---|---|---|
| memória -> direct save | valor, tipo, missing, string e metadata mantêm-se; erro não publica | FAIL: NUL em `strL`; staging alheio apagado |
| Parquet -> eager | perdas inevitáveis são recusadas ou anunciadas por coluna | FAIL: TIMESTAMP acima de `2^53`; metadata/sort |
| Parquet -> lazy boundary | conversão não pode coalescer chaves antes de verbos sem recusa/opt-in | FAIL: `UINT64` adjacentes colapsam |
| lazy stages | ordem e semântica Stata são aplicadas no ponto correto | FAIL: sort obsoleto; literais inválidos; tipos de expressão |
| lazy -> collect | valor/tipo/metadata iguais ao contrato e materialização atómica | FAIL em casos adversariais; controlos normais passam |
| lazy -> save | mesmo valor lógico do collect e schema físico coerente com KV | FAIL: `2^53+1` e tipo físico |
| adapters/bridges | paths são únicos, owned e removidos só pelo owner | PASS nos regressions de bridges; ownership do staging final falha |
| erro/concorrência | estado anterior e dados alheios sobrevivem; sem resultado parcial válido | FAIL: rename, SQL, staging; race de mesmo destino não fechada |
| tag -> asset | asset testado é o publicado e satisfaz contrato estrutural/runtime | identidade forte; FAIL funcional e exports Windows |

## 4. Matriz A–K

| Área | Cobertura fresca | Veredito |
|---|---|---|
| A. Tipos e valores | limites, unsigned/decimal, NaN/Inf, signed zero, all-null, NUL, strL, duplicados; PyArrow/DuckDB | **FAIL** — três famílias S0 e tipos divergentes |
| B. Datas e tempos | `%td/%tc/%tC` e períodos na suite; half ties, us/ns, extremos e literais nativos | **FAIL** — perda de 1 ms e literais inválidos aceites |
| C. Metadados | labels, formats, chars, KV, globs, duplicados, sort marker, JSON malformado | **FAIL** — contaminação e perdas silenciosas |
| D. Semântica pública | suite integral; twins Stata para filtros, gen/replace, collapse, sort/append, rename e SQL | **FAIL** — ordem, tipos e datas |
| E. Inputs/caminhos | ficheiro, glob, relaxed, adapters, espaços/Unicode, overlap, release ZIP | **FAIL** — metadata em glob e staging previsível; caminhos normais passam |
| F. Atomicidade/erros | regressions oficiais e repros antes/depois de estado | **FAIL** — cleanup alheio, rename e SQL não atómicos |
| G. Concorrência/lifecycle | duas sessões com TMPDIR comum; bridges/refcounts; inspeção de staging e threads | **FAIL** — staging partilhado/race residual e risco de worker creation |
| H. Escala/estrutura | 1,5 M linhas, 2 500 vars, strL 1 MB, limite >2^31 simulado, partições | **PASS condicionado** — sem campanha destrutiva de escala/NFS/disco cheio |
| I. Segurança semântica | nomes/paths/expressões hostis, SQL/JSON, ownership | **FAIL** — um nome alheio é tratado como staging owned |
| J. Test harness | no-match, abort-after-verdict, concorrência, duas mutations, suite integral | **FAIL de cobertura** — runner reage corretamente, mas S0/S1 não têm regressions |
| K. Build/release | build limpo, CTest, assets, hashes, ZIP, full suite pública, formatos/exports/deps | **FAIL** — binário público reproduz falhas e Windows sobre-exporta |

## 5. Gates G0–G9

| Gate | Estado | Fundamentação |
|---|---|---|
| G0 — snapshot/proveniência/versão | **PASS forte** | commit/tag/remote coerentes; release lint; hashes de source/build/assets |
| G1 — paridade Stata | **FAIL** | sort+replace/append, tipos untyped, datas e rename divergem |
| G2 — valores/tipos/precisão | **FAIL** | cinco S0, tipo físico lazy e precision boundaries |
| G3 — metadata round-trip | **FAIL** | glob parcial, JSON inválido, provenance duplicada e `sortedby` |
| G4 — atomicidade/erro | **FAIL** | staging alheio apagado; rename/SQL/expressões quebram estado |
| G5 — lazy/bridges/ownership/concorrência | **FAIL** | materializadores divergem; sort stale; staging sem ownership |
| G6 — escala/limites/out-of-core | **PASS condicionado** | controlos significativos passaram, mas faltam escalas/filesystems extremos |
| G7 — harness resistente | **FAIL** | mutations ficam vermelhas, mas faltam tests para os bloqueadores encontrados |
| G8 — staged/publicado | **FAIL** | proveniência e smoke passam; o asset Linux público reproduz S0 e PE tem 3518 exports |
| G9 — documentação fiel | **FAIL** | promessas de double, metadata, binary strL, `_n/_N` e exports são contrariadas |

## 6. Findings confirmados

### PARQIT-REL-001 — S0 — cleanup apaga staging pertencente ao utilizador

- **Certeza:** confirmada em runtime na release Linux pública, em duas formas,
  e por inspeção direta.
- **Impacto:** um `parqit save` com `rc 0` pode apagar silenciosamente bytes que
  não pertencem ao output pedido. No modo partitioned pode remover uma árvore
  inteira.
- **Código:** `src/plugin/plugin_io.cpp:881-890` usa
  `remove(dest + ".parqit_tmp")`; `:938-944` usa `remove_all`. O mesmo desenho
  aparece em `.parqit_old` em `:904-906` e `:965-969`.
- **Caminho:** memória Stata -> `parqit save <dest>, data` ->
  `copy_out_parquet` -> limpeza do sibling previsível antes do `COPY`.
- **Repro mínimo:** criar antes do save o ficheiro
  `<dest>.parqit_tmp` com `USER_OWNED_SENTINEL`; no caso partitioned, criar a
  pasta homónima com `USER_DATA.txt`; guardar para `<dest>` sem `replace`.
- **Observado:** o save termina com sucesso, o target novo existe e o sentinel
  dá `confirm file` rc601. A variante partitioned elimina recursivamente a
  pasta e o ficheiro.
- **Esperado:** usar staging único e criado exclusivamente, com marker/registry
  de ownership; nunca remover um objeto que o processo não provou ter criado.
- **Oracle:** existência e conteúdo de ficheiro antes/depois, independentes do
  parqit, mais as chamadas `std::filesystem` correspondentes.
- **Causa-raiz:** namespace determinístico tratado implicitamente como owned,
  sem criação exclusiva, nonce ou verificação de identidade.
- **Porque escapou:** os testes criam outputs em tempdirs privados e não
  preposicionam um objeto alheio nos suffixes internos.
- **Evidência:**
  `public_release_v0.1.21/stata_install/repro_owned_staging_delete.log` e
  `repro_owned_partition_tree_delete.log` sob o snapshot `/tmp`.

Flat e partitioned são manifestações da mesma causa, não dois findings. A
eliminação de `.parqit_old` e a corrida entre dois processos no mesmo destino
ficam como extensões estáticas ainda não reproduzidas.

### PARQIT-DATA-002 — S0 — fronteira lazy não injetiva coalesce chaves `UINT64`

- **Certeza:** confirmada no build fresco e repetida no binário Linux público,
  com Stata log e PyArrow.
- **Impacto:** identificadores distintos podem tornar-se uma só chave antes de
  `contract`, `collapse`, `duplicates`, join/merge ou save; frequências e
  agregados ficam errados com `rc 0`.
- **Código:** `src/plugin/plugin_view.cpp:289-297` converte `UBIGINT/HUGEINT` e
  `DECIMAL` para `DOUBLE`; `:1153-1158` só injeta a nota no collect;
  `:1393-1417` não comunica a perda preexistente no lazy save.
- **Caminho:** foreign Parquet -> lazy boundary -> cast `UINT64` para double ->
  verbo agrupador -> lazy save.
- **Repro mínimo:** Parquet com chaves
  `[9007199254740992, 9007199254740993]`; executar
  `parqit contract key, freq(freq)` e guardar a view.
- **Observado:** output tem uma linha
  `{key=9007199254740992.0, freq=2}`; zero warning no log.
- **Esperado:** recusar/forçar opt-in antes de uma conversão não injetiva poder
  afetar verbos, ou representar as chaves de forma exata.
- **Oracle:** PyArrow conserva e lê os dois inteiros de input; PyArrow lê uma
  única linha no output; DuckDB Python confirma os tipos físicos.
- **Causa-raiz:** normalização para o universo numérico do Stata ocorre como
  cast silencioso no início do plano, e o canal de notas não acompanha o save.
- **Porque escapou:** `v06` e `v51` testam leitura/nota, não duas chaves
  adjacentes acima de `2^53` seguidas de um agrupamento lazy.
- **Evidência:** `parquet_metadata/u64_grouping.log`,
  `fixtures/u64_key_collision.parquet`, `u64_grouped.parquet` e
  `compare_resaves.py`; repetição em `parquet_metadata_public/`.

A campanha da mesma fronteira mostrou ainda `Inf`, extremos Stata, DECIMAL e
timestamps us/ns alterados no lazy save sem resumo de perdas. O caso `UINT64`
é o contraexemplo mínimo que demonstra efeito analítico, não apenas drift de
representação.

### PARQIT-DATA-003 — S0 — `2^53+1` depende do materializador

- **Certeza:** confirmada com Stata nativo, collect/direct e PyArrow físico.
- **Impacto:** a mesma expressão Stata-flavoured pode produzir valores
  diferentes para o utilizador Stata e para consumidores do Parquet. IDs ou
  contagens podem divergir uma unidade.
- **Código:** `src/engine/exprtrans.cpp:467-475` testa o literal com `strtod`,
  mas envia o token decimal original ao SQL; `src/plugin/plugin_view.cpp:367-427`
  não normaliza pelo tipo Stata no save.
- **Caminho:** coluna `double` -> `parqit replace x=9007199254740993` ->
  collect/save direto versus lazy save.
- **Repro mínimo:** `/tmp/audit_num53_path_parity.do`; há twin `%tC` em
  `/tmp/audit_tc53_path_parity.do`.
- **Observado:** Stata, collect e save direto dão `9007199254740992`; o lazy
  save escreve `9007199254740993` físico `int64`, todos com `rc 0`.
- **Esperado:** avaliar conforme binary64 do Stata e produzir o mesmo valor em
  todos os materializadores, ou recusar antes de materializar.
- **Oracle:** Stata nativo para a expressão; PyArrow para valor e tipo no disco.
- **Causa-raiz:** o parser preserva precisão decimal que a linguagem anfitriã
  não possui e o writer lazy deriva o valor/tipo do SQL.
- **Porque escapou:** não há literal inteiro vizinho de `2^53` na suite; `v54`
  usa valores temporais pequenos.
- **Evidência:** `/tmp/audit_num53_path_parity.log` e
  `/tmp/audit_tc53_path_parity.log`.

Este finding não é duplicado do anterior: DATA-002 nasce na leitura de um tipo
foreign e altera chaves antes de verbos; DATA-003 nasce no parser de expressões
e diverge entre materializadores.

### PARQIT-DATA-004 — S0 — fast `save, data` trunca `strL` no NUL

- **Certeza:** confirmada por Stata nativo, três caminhos do parqit e PyArrow;
  repetida com o plugin público.
- **Impacto:** perde silenciosamente todo o suffix a partir do NUL, embora o
  dataset Stata em memória ainda tenha os bytes.
- **Código:** eager conserva bytes no sidecar em
  `src/plugin/plugin_io.cpp:1275-1300`; o writer geral recusa binary strL em
  `:2205-2216`; o fast path trata `Str` e `StrL` igual e corta em `:2857-2861`,
  terminando com listas de perda vazias em `:2905-2910`.
- **Caminho:** Parquet string longa com NUL -> eager `use, clear` -> dataset
  unchanged -> fast `parqit save ..., data`.
- **Repro mínimo:** valor `"a"*2100 + NUL + "TAIL"` mais uma missing.
- **Observado:** eager tem `strL`, comprimento 2105 e suffix `TAIL`; fast save
  dá rc0 e output com 2100 `a`; lazy save conserva os 2105 bytes; uma mudança
  que desative o fast path leva o writer geral a recusar rc198.
- **Esperado:** fast path equivalente ao writer geral: preservação suportada
  ou recusa loud sem publicar; nunca truncagem silenciosa.
- **Oracle:** `st_sstore/st_sdata` nativos provam que o Stata conserva NUL em
  strL; PyArrow mede comprimento, posição do NUL e suffix.
- **Causa-raiz:** otimização fast reutiliza uma regra C-string de `str#` para
  `strL`, ignorando `SF_var_is_binary` e sem emitir loss note.
- **Porque escapou:** `v19` cobre o limite strL e 1 MB, mas não NUL embebido nem
  paridade fast/general/lazy.
- **Evidência:** `parquet_metadata/strl_nul.log`, `strl_nul_paths.log`,
  `probe_strl_nul.log`, `compare_resaves.py`; repetição pública em
  `parquet_metadata_public/`.

### PARQIT-DATA-005 — S0 — TIMESTAMP(us) acima de `2^53` perde 1 ms

- **Certeza:** confirmada por aritmética inteira e PyArrow, com runtime Stata.
- **Impacto:** um instante Parquet válido muda silenciosamente. A incidência
  prática é baixa porque o valor temporal é extremo, mas a perda é exata e não
  documentada para esse caso.
- **Código:** `src/engine/typemap.cpp:174-178` ativa apenas a nota sub-ms;
  `src/plugin/plugin_io.cpp:1234-1246` converte `int64 us -> int64 ms -> double`
  sem testar exatidão acima de `2^53`.
- **Caminho:** foreign `TIMESTAMP(us)` -> eager fill Stata -> direct resave.
- **Repro mínimo:** raw `9006883635540993000 us`, que após epoch shift é
  exatamente `9007199254740993` Stata-ms.
- **Observado:** materialização arredonda para `9007199254740992`; resave fica
  `9006883635540992000 us`, perda de 1000 us, sem nota específica.
- **Esperado:** recusa ou nota de precisão >`2^53`, antes de apresentar a
  materialização como fiel.
- **Oracle:** aritmética int64 e payload raw PyArrow antes/depois.
- **Causa-raiz:** só se deteta resto sub-milisegundo; não se valida se o
  milissegundo inteiro continua representável em binary64.
- **Porque escapou:** os oracles temporais existentes usam datas
  contemporâneas e não cruzam o limiar binary64 após epoch/unidade.
- **Evidência:** `parquet_metadata/eager.log`, `eager_resave.parquet`,
  `oracle_fixtures.log` e `compare_resaves.py`.

### PARQIT-SEM-006 — S1 — sort diferido fica obsoleto após `replace` e `append`

- **Certeza:** confirmada por dois twins Stata e inspeção do plano.
- **Impacto:** `_n`, rankings, lags/leads e `keep in` podem ser associados a
  sujeitos errados ou selecionar as observações erradas.
- **Código:** `View::replace` em `src/engine/view.cpp:382-421` cria um stage sem
  bake/invalidação de `sort_`; `append_with` em `:1140-1214` preserva o sort do
  master depois do `UNION`; a ordem pendente é aplicada no fim.
- **Caminho A:** `sort x; replace x=-x; gen seq=_n`.
- **Caminho B:** master sorted -> append -> `keep in 1/2`.
- **Observado:** por `id`, o twin replace dá Stata `seq=2,1,3` e parqit
  `2,3,1`; no append, o Stata retém ids `102,101` e parqit `200,102`.
- **Esperado:** materializar a ordem no ponto semântico e truncar/limpar sort
  state quando uma key muda; append concatena master já ordenado + using e
  invalida sortedby.
- **Oracle:** comandos nativos sobre os mesmos dados, mais plano estático.
- **Causa-raiz:** sort é estado diferido e sobrevive a operações que mudam a
  key ou a composição das linhas.
- **Porque escapou:** unit tests e t04 verificam valores/contagens sem sort
  anterior seguido de operação order-sensitive.
- **Evidência:** `/tmp/audit_order_data_effect.do` e `.log`.

Replace e append são uma família de causa, embora necessitem regressions
separados porque exercem `_n` e seleção por posição.

### PARQIT-TYPE-007 — S1 — tipo físico lazy ignora tipo explícito e contradiz KV

- **Certeza:** confirmada com collect, save direto, PyArrow e footer KV.
- **Impacto:** Python/R/Spark recebem schema diferente do pedido; o ficheiro
  pode anunciar `double` em `parqit.schema` e conter fisicamente `int32`.
- **Código:** `src/engine/view.cpp:30-49` deixa `double` sem cast;
  `src/plugin/plugin_view.cpp:367-427` não recasta por `ViewCol.meta_type`, mas
  `:430-445` serializa esse tipo no KV.
- **Caminho:** `parqit gen double z=42` ou replace de coluna double por `42` ->
  collect/direct/lazy save.
- **Observado:** collect e direct Parquet dizem `double`; lazy Parquet diz
  `int32`, enquanto o KV continua `type=double`.
- **Esperado:** tipo explícito determina o schema físico de todas as vias.
- **Oracle:** schema PyArrow e `: type` Stata.
- **Causa-raiz:** storage intent vive apenas em metadata; `COPY` usa a inferência
  DuckDB da expressão.
- **Porque escapou:** typed-gen é testado no collect, não no schema físico do
  lazy save; v41 foca colunas fonte.
- **Evidência:** `/tmp/audit_gen_double_physical_type.do/.log` e
  `/tmp/audit_replace_physical_type.do/.log`.

### PARQIT-TYPE-008 — S1 — `gen` untyped não cumpre nem Stata nem o help

- **Certeza:** confirmada em runtime.
- **Impacto:** storage type, range e interoperabilidade diferem do contrato;
  futuras substituições podem comportar-se de forma diferente devido à
  compressão não pedida.
- **Código/contrato:** o tipo resulta da inferência DuckDB; o help
  `src/ado/p/parqit.sthlp:531-537` e `ASSUMPTIONS.md:709-721` dizem que untyped
  é `double`; o Stata nativo usa `float`.
- **Caminho:** `gen z=42` ou `gen flag=id>0` sem tipo explícito -> collect.
- **Observado:** parqit cria `byte`; Stata cria `float`; docs prometem `double`.
- **Esperado:** implementar consistentemente o contrato escolhido ou alterar
  explicitamente documentação e testes antes da release.
- **Oracle:** Stata `describe/: type`, parqit collect e help versionado.
- **Causa-raiz:** literais inteiros/CASE booleano mantêm o tipo DuckDB; só
  algumas expressões aritméticas forçam double.
- **Porque escapou:** a suite testa valores ou typed gen, não todas as classes
  de expressão untyped.
- **Evidência:** `/tmp/audit_untyped_constant_storage.do/.log`.

### PARQIT-DATE-009 — S1 — literais temporais inválidos são aceites/rolados

- **Certeza:** confirmada contra Stata nativo.
- **Impacto:** um erro de data que o Stata recusaria pode entrar como instante
  diferente, inclusive no minuto seguinte, com `rc 0`.
- **Código:** `src/engine/exprtrans.cpp:222-254` não exige ano >=0100;
  `:283-320` aceita casas fracionárias arbitrárias e arredonda `sec*1000`.
- **Caminho:** `td(01jan0099)` e `tc(01jan2020 00:00:59.9999)` em expressão.
- **Observado:** Stata rc198; parqit rc0. O segundo caso arredonda exatamente
  para o minuto seguinte.
- **Esperado:** os mesmos limites sintáticos/temporais do Stata e erro no
  comando originador.
- **Oracle:** Stata nativo e valor numérico recolhido.
- **Causa-raiz:** validação de calendário e second range é incompleta quanto ao
  ano e precisão decimal.
- **Porque escapou:** unit tests cobrem datas impossíveis, segundo 60 e `59.5`,
  não ano 0099 nem mais de três casas.
- **Evidência:** `/tmp/audit_stata_semantics_runtime.do/.log`,
  `/tmp/parqit_date_bounds_native.log` e `/tmp/parqit_time_frac_native.log`.

### PARQIT-META-010 — S1 — metadata de um ficheiro contamina glob parcial

- **Certeza:** confirmada no build e no plugin público com footer DuckDB e
  runtime Stata.
- **Impacto:** formatos, labels e value labels podem ser aplicados a linhas de
  ficheiros que nunca os declararam, mudando a interpretação do conjunto.
- **Código:** `src/plugin/plugin_io.cpp:325-379` consulta apenas rows com key
  `parqit.%`; `per_file` não inclui ficheiros sem essas keys, logo o teste de
  igualdade nunca os vê.
- **Caminho:** glob com A contendo KV `parqit.*` e B com schema igual sem KV.
- **Observado:** o conjunto recebe `%tm`, varlabel
  `METADATA_FROM_A_ONLY`, vallab `only_a` e data label `A only`, sem aviso.
- **Esperado:** restaurar metadata apenas se todos os ficheiros matched têm o
  mesmo conjunto de KV; caso contrário, avisar e não aplicar.
- **Oracle:** `parquet_file_metadata`/`parquet_kv_metadata` e Stata.
- **Causa-raiz:** universo de ficheiros é derivado da query já filtrada pelas
  keys que se pretende validar.
- **Porque escapou:** v24/v48 cobrem schema físico misto, não ausência
  assimétrica de KV.
- **Evidência:** `parquet_metadata/mixed_metadata.log`,
  `fixtures/mixed_metadata/`; repetição em `parquet_metadata_public/`.

### PARQIT-META-011 — S1 — JSON `parqit.*` malformado perde metadata silenciosamente

- **Certeza:** confirmada por bytes do footer e Stata.
- **Impacto:** corrupção do footer apaga labels/formats/chars sem indicação;
  vallabs inválidos podem deixar uma variável ligada a um label inexistente.
- **Código:** `src/plugin/plugin_io.cpp:367-378` usa
  `json::parse(..., false)` e transforma `discarded` em JSON vazio sem warning.
- **Caminho:** schema ou vallabs sintaticamente inválidos no KV.
- **Observado:** load rc0 e zero nota; no caso schema, metadata desaparece; no
  caso vallab, a variável aponta para `only_a`, mas `label list only_a` dá rc111.
- **Esperado:** warn-and-skip identificando key/ficheiro, ou erro configurável
  quando o ficheiro declara metadata do próprio parqit.
- **Oracle:** PyArrow lê os bytes exatos do footer; Stata inspeciona a metadata.
- **Causa-raiz:** postura best-effort engole erro sintático sem canal de
  diagnóstico.
- **Porque escapou:** v51 cobre campos/formats internos inválidos, não JSON KV
  sintaticamente inválido.
- **Evidência:** `parquet_metadata/malformed_schema.log`,
  `malformed_vallabs.log` e fixtures correspondentes.

### PARQIT-META-012 — S1 — provenance de nomes duplicados perde-se após stage lazy

- **Certeza:** confirmada por collect, view save/reload e KV PyArrow.
- **Impacto:** o nome físico duplicado original deixa de ser recuperável;
  downstream toma `dup_1` por nome real, quebrando reversibilidade.
- **Código:** `src/plugin/plugin_view.cpp:680-760` usa nomes já deduplicados pelo
  DuckDB e só grava `src_name` quando o sanitiser local marca `renamed`; não usa
  o mapa posicional `meta_ctx.parquet_names` do eager planner.
- **Caminho:** Parquet físico `dup, dup` -> lazy open -> qualquer stage (`keep`)
  -> collect ou save/reload.
- **Observado:** valores 1/10 sobrevivem, mas `char dup_1[src_name]` é vazio; o
  save passa a ter físicos `dup,dup_1` e `parqit.chars={}`.
- **Esperado:** transportar o mapa posicional e warning para o view antes do
  primeiro stage.
- **Oracle:** schema/metadata PyArrow e characteristics Stata.
- **Causa-raiz:** a correção lazy observa nomes pós-DuckDB, quando a duplicação
  original já desapareceu.
- **Porque escapou:** v47 usa nome sanitizado com espaço; v52 cobre duplicado
  apenas no eager path.
- **Evidência:** `parquet_metadata/lazy_duplicate.log`,
  `duplicate_save.log`, `dup_lazy_save.parquet`.

### PARQIT-META-013 — S1 — informação `sortedby` não round-tripa

- **Certeza:** confirmada por Stata nativo, direct reload e lazy collect.
- **Impacto:** a ordem física continua correta, mas `by id:` falha rc5; scripts
  válidos no dataset original deixam de correr ou exigem novo sort.
- **Código:** o request em `src/ado/p/parqit.ado:2558-2644` serializa vars,
  labels, chars e dtalabel, não sortedby; `view_kv_fragment` em
  `src/plugin/plugin_view.cpp:431-464` também não o guarda.
- **Caminho:** dataset nativo `sort id` -> save/use ou `parqit sort id` -> collect.
- **Observado:** nativo `sortedby=[id]`; eager e lazy `sortedby=[]`; ordem
  1,2,3 mantém-se, mas `by id:` dá rc5.
- **Esperado:** persistir/restaurar keys quando a ordem materializada as
  satisfaz, ou retirar a promessa de metadata lossless e documentar a lacuna.
- **Oracle:** macro Stata `sortedby` e execução nativa `by:`.
- **Causa-raiz:** o modelo de metadata não tem campo de sort marker.
- **Porque escapou:** testes de sort comparam ordem/valores, não marker nem um
  `by:` sem re-sort.
- **Evidência:** `parquet_metadata/sort_state_repro.log`,
  `sort_metadata.log` e `probe_sort.log`.

META-013 não duplica SEM-006: aqui a ordem física está certa e perde-se um
metadado; em SEM-006 o estado obsoleto altera valores/linhas.

### PARQIT-ATOM-014 — S2 — group rename falhado deixa mutação parcial

- **Certeza:** confirmada contra Stata.
- **Impacto:** um erro capturado deixa a view com schema diferente, podendo
  afetar comandos seguintes.
- **Código:** `_parqit_rename` em `src/ado/p/parqit.ado:689-728` executa os pares
  sequencialmente sem validação global/candidate view.
- **Caminho:** `parqit rename (a b) (c c)`.
- **Observado:** Stata rc198 e mantém `a b`; parqit rc198 mas collect mostra
  `c b`. O ciclo nativo válido `(a b)->(b a)` também é recusado.
- **Esperado:** validate-then-commit atómico, incluindo ciclos, ou rollback
  integral em qualquer erro.
- **Oracle:** twin Stata e schema recolhido depois do erro.
- **Causa-raiz:** cada `_rename_one` commita imediatamente.
- **Porque escapou:** v28 testa apenas group rename válido sem colisão tardia.
- **Evidência:** `/tmp/audit_stata_semantics_runtime.do/.log`.

### PARQIT-ATOM-015 — S2 — `sql ..., clear` falhado substitui a view anterior

- **Certeza:** confirmada; memória Stata fica intacta, mas a view não.
- **Impacto:** um erro de execução converte uma view utilizável num plano que
  continua a falhar.
- **Código:** `cmd_view_sql` em `src/plugin/plugin_view.cpp:2192-2263` instala o
  candidate depois de um probe `LIMIT 0`; o ado só depois chama collect em
  `src/ado/p/parqit.ado:2039-2047`.
- **Caminho:** view válida -> SQL cujo schema bind é válido mas a execução
  `CAST('bad' AS INTEGER)` falha -> opção `clear`.
- **Observado:** comando rc920; `parqit show` já aponta para o SQL defeituoso e
  novo collect volta a falhar; a view anterior não é restaurada.
- **Esperado:** candidate isolado até collect completo, ou rollback da view em
  qualquer falha do comando público.
- **Oracle:** capacidade de materializar a view antes/depois e estado mostrado.
- **Causa-raiz:** commit depois de validação de schema, antes de validação de
  execução/materialização.
- **Porque escapou:** t05 testa SQL clear bem-sucedido; v31 testa rollback de
  fragmento que nem compila, não erro runtime após o probe.
- **Evidência:** `/tmp/audit_stata_semantics_runtime.do/.log`.

### PARQIT-ATOM-016 — S2 — funções string inválidas instalam plano quebrado

- **Certeza:** confirmada para `trim`, `ltrim`, `rtrim` e `subinstr`.
- **Impacto:** o comando originador devolve rc0, mas collect/count falham e a
  view fica inutilizável até ser fechada/reaberta.
- **Código:** `src/engine/exprtrans.cpp:1054-1064` não valida o tipo do argumento
  de trim; `:1096-1108` não valida os três primeiros argumentos de subinstr.
- **Caminho:** por exemplo `parqit gen z=trim(id)` numa coluna numérica.
- **Observado:** Stata falha imediatamente rc109 sem mutar; parqit gen rc0 e
  collect Binder Error rc920.
- **Esperado:** type mismatch no comando originador, preservando a view.
- **Oracle:** Stata nativo e materialização da view antes/depois.
- **Causa-raiz:** tradutor constrói SQL tipado apenas no bind tardio; stage é
  aceite sem probe/rollback.
- **Porque escapou:** testes de funções string usam argumentos válidos.
- **Evidência:** `/tmp/audit_expr_type_family.do/.log`.

### PARQIT-PORT-017 — S2 — sanitiser Unicode não modela nomes aceites pelo Stata

- **Certeza:** confirmada por `st_isname()/st_addvar()` e eager/lazy runtime.
- **Impacto:** Parquet legível pode abrir lazy com rc0 e só falhar no collect;
  nomes CJK válidos são truncados desnecessariamente.
- **Código:** `src/engine/sanitize.cpp:29-51` aceita qualquer byte >=0x80 e
  trunca a 32 bytes; colisões em `:54-75` também usam bytes.
- **Caminho:** nome inicial emoji/combining ou nome CJK multibyte longo.
- **Observado:** emoji/combining: eager e collect rc3300; CJK de 20 caracteres,
  que o Stata aceita, é reduzido a 10. A falha eager/collect preserva o sentinel,
  pelo que a atomicidade de memória passou.
- **Esperado:** gerar sempre nome aceite pelo Stata e aplicar limite de 32
  caracteres Unicode, preservando `src_name`.
- **Oracle:** funções SPI/Stata nativas e nomes observados no collect.
- **Causa-raiz:** validação byte-level aproximada da gramática Unicode do Stata.
- **Porque escapou:** unit tests fixam 32 bytes e só usam Unicode benigno.
- **Evidência:** `parquet_metadata/probe_names.log`, `unicode_names.log` e
  `long_cjk.log`.

### PARQIT-LIFE-018 — S2 condicional — criação parcial de threads pode terminar Stata

- **Certeza:** mecanismo C++ confirmado estaticamente; não induzido dentro do
  plugin Stata.
- **Impacto:** sob exaustão de recursos, criação do segundo/seguinte worker pode
  terminar o processo em vez de devolver erro loud/atómico.
- **Código:** `src/plugin/plugin_io.cpp:1617-1624` cria threads antes do `try`
  que começa em `:1632`; destruição de `std::thread` joinable durante unwind
  chama `std::terminate`.
- **Caminho:** parallel fill com pelo menos um worker criado e uma
  `std::system_error` num `emplace_back` seguinte.
- **Observado:** probe C++ mínimo com a mesma regra terminou pelo terminate
  handler com código 86. Não houve fault injection no plugin.
- **Esperado:** criação coberta por RAII/catch que sinaliza abort e junta todos
  os workers já criados antes de retornar erro.
- **Oracle:** regra standard de `std::thread` e probe executável.
- **Causa-raiz:** janela de criação fica fora do scope de cleanup.
- **Porque escapou:** testes exercitam workers já criados; não injetam falha
  parcial de criação. `PARQIT_FILL_THREADS` pode ampliar a contagem.
- **Evidência:** `/tmp/parqit_thread_unwind_probe.cpp` e binário homónimo.

### PARQIT-PKG-019 — S3 — plugin Windows exporta 3518 símbolos

- **Certeza:** contagem binária exata; impacto runtime Windows não demonstrado.
- **Impacto:** viola o contrato de packaging, aumenta superfície ABI e revela
  que o gate pode ficar verde apesar de over-export. Não foi provada corrupção
  ou falha de carga, por isso não se eleva a S2.
- **Código:** CMake diz “Only stata_call/pginit are exported” em
  `CMakeLists.txt:106-110`, mas limita exports apenas Apple/UNIX em `:138-145`;
  `tests/verify_collected_plugin.sh:43-53` verifica presença, não exclusividade.
- **Caminho:** MSVC Release -> staged PE -> upload -> verifier Windows.
- **Observado:** `llvm-readobj --coff-exports` conta 3518 nomes, incluindo
  `pginit`, `stata_call`, centenas de `duckdb_*` e símbolos C++ decorados; o
  verifier oficial retorna OK.
- **Esperado:** export table restrita aos dois entry points e gate de igualdade
  exata.
- **Oracle:** PE export directory via LLVM; contraste com Mach-O público, que
  expõe exatamente os dois símbolos.
- **Causa-raiz:** ausência de `.def`/equivalente Windows e assertion negativa.
- **Porque escapou:** gate só faz `grep` dos dois símbolos obrigatórios.
- **Evidência:**
  `public_release_v0.1.21/parqit_win64.plugin`, SHA-256 na secção 9.

### PARQIT-DOC-020 — S3 — `_n/_N` documentados para caminhos recusados

- **Certeza:** confirmada; falha loud e não destrutiva.
- **Impacto:** scripts compatíveis com o help falham inesperadamente.
- **Código/contrato:** `src/ado/p/parqit.sthlp:508-514` inclui `_n/_N` em gen e
  replace; `src/engine/view.cpp:398-403` recusa replace e `:354-360` recusa
  qualifier de gen.
- **Caminho:** `replace x=_n` e `gen first=1 if _n==1`.
- **Observado:** Stata rc0; parqit rc198 “not supported ... yet”.
- **Esperado:** implementar o alcance documentado ou restringir o help com
  precisão.
- **Oracle:** Stata nativo, runtime parqit e texto publicado.
- **Causa-raiz:** documentação generaliza suporte parcial de row context.
- **Porque escapou:** não há teste docs-to-runtime para essas duas formas.
- **Evidência:** `/tmp/audit_rowctx_documented.do/.log`.

## 7. Repros, oracles e disciplina de evidência

### 7.1 Diretórios principais

| Conteúdo | Path |
|---|---|
| Snapshot/build/evidência comum | `/tmp/parqit_audit_20260714_ffb9c707_GXyk8M` |
| Auditoria tipos/metadata no build | `.../parquet_metadata/` |
| Repetição no plugin Linux público | `.../parquet_metadata_public/` |
| Downloads e extrações da release | `.../public_release_v0.1.21/` |
| Full suite build fresco | `/tmp/parqit_tests.gzV7Yk/` |
| Full suite plugin público | `/tmp/parqit_tests.4t9sDz/` |
| Fuzz/diferencial seeded | `.../fuzz/differential_random.do` e `parqit.log` |
| Mutation de assertion | `/tmp/parqit_tests.5YRm7O/` |
| Restauro/controle v54 | `/tmp/parqit_tests.Y4HXhZ/` |
| Handoff metadata | `.../parquet_metadata/FINDINGS.md` |
| Handoff semântica | `/tmp/stata_semantics_audit_fragment.md` |
| Red-team final | `/tmp/parqit_red_team_final.md` |

### 7.2 Oracles independentes usados

- Stata nativo sobre os mesmos datasets para semântica, tipos, datas, ordem,
  rename, `by:` e expressões;
- PyArrow para schema, valores raw, bits de signed zero, timestamps, strings e
  KV Parquet;
- DuckDB Python e funções `parquet_*_metadata` para tipos/footers e globs;
- filesystem direto para existência/remoção de sentinels;
- LLVM/binutils/file/readelf para formato, exports, stripping, dependências e
  baseline GLIBC;
- inspeção estática independente para ligar cada repro à causa executada.

Nenhum S0 foi aceite apenas por suspeita de código. Cada S0 tem runtime e um
segundo oracle/inspeção. Achados apenas estáticos foram separados: threads é S2
condicional; `.parqit_old` e corrida same-destination ficam em risco residual.

### 7.3 Controlos positivos/rejeições de hipóteses

- signed zero foi preservado bit a bit em eager e lazy save;
- TIMESTAMPTZ preservou o instante UTC no build sem ICU no caso testado;
- falhas por nome Unicode inválido preservaram o dataset sentinel;
- lazy save preservou o NUL do `strL`, isolando o problema no fast direct path;
- eager emitiu as notas esperadas para Inf/extremos, UINT64, DECIMAL, sub-ms,
  TIME, all-null e NUL em `str#` nos casos cobertos;
- bridges entre processos, refcounts e `close _all` passaram os regressions da
  remediação v0.1.21;
- os assets textuais da release coincidem byte a byte com a tag;
- os ZIPs não estavam corruptos e o Linux público carregou no Stata isolado.

## 8. Testes frescos e mutations

### 8.1 Build e C++

```text
cmake --preset dev -DPARQIT_DUCKDB_ARCHIVE=<archive pinado>          PASS
cmake --build build/dev --target parqit_plugin parqit_tests -j 8    PASS
ctest --preset dev --output-on-failure                              3/3 PASS
./build/dev/parqit_tests                                            63/63 casos
                                                                  973/973 asserts
                                                                  1 skipped
bash tests/release_lint.sh                                          PASS 0.1.21
```

CTest cobriu `unit`, `runner_no_match` e `unit_concurrent`.

### 8.2 Suite Stata integral

| Superfície | Logs | Verdicts | Resultado |
|---|---:|---:|---|
| build dev fresco, plugin SHA `51a748...` | 71 | 74 PASS, 0 FAIL | PASS |
| plugin Linux público, SHA `0d8f2f...` | 71 | 74 PASS, 0 FAIL | PASS |

O segundo run usou o binário instalado em
`public_release_v0.1.21/stata_install/plus/p/parqit.plugin`; os ado/help usados
pelo runner são byte-idênticos aos assets públicos. Portanto o plugin
integralmente testado no segundo run é o plugin efetivamente publicado, não uma
rebuild aproximada.

Casos de escala incluídos na suite: 1,5 M linhas no parallel fill contra
PyArrow, 2 500 variáveis, `strL` de 1 MB, limites de strings, partições/globs,
schemas mistos e row count >`2^31` simulado por metadata. Isto suporta G6
condicionado, não uma prova universal de escala.

### 8.3 Diferencial aleatório focado

Foram gerados 30 casos determinísticos seeded, entre 17 e 99 observações, com
grupos incluindo missing, valores positivos/negativos/missing e strings
vazias. Em cada caso, o pipeline:

```text
keep if -> gen double -> replace condicional -> collapse sum/mean/count by(g)
-> sort -> collect
```

foi comparado célula a célula com Stata nativo. Resultado:

```text
VERDICT(AUDIT_DIFFERENTIAL_RANDOM): PASS - 30 seeded cases matched native Stata
```

Este PASS cobre o subespaço gerado; não cobre limites `2^53`, NUL ou mutações
de sort key, que foram testados separadamente e falharam.

### 8.4 Mutation testing no snapshot scratch

Foram executadas duas mutations e ambas foram revertidas por patch inverso:

1. **Implementação:** limite do sanitiser alterado de 32 para 31 bytes.
   CTest ficou vermelho: 2 casos/2 assertions falharam. Depois do restauro,
   unit voltou a 63/63 e 973/973.
2. **Assertion Stata:** expected `%tC` em v54 alterado de 101 para 102. O script
   abortou sem verdict e `tests/run_stata.sh` saiu com código 1. Depois do
   restauro, v54 voltou a PASS.

`git diff --exit-code` confirmou que os dois ficheiros scratch ficaram iguais
ao snapshot. Isto valida os mecanismos principais do runner, mas não corrige a
ausência de regressions para os findings; daí G7 continuar FAIL.

## 9. Artefactos publicados

### 9.1 Hashes calculados aos downloads

| Asset | SHA-256 |
|---|---|
| `parqit_all_platforms.zip` | `9c3f4b2372ff78e002b49fe0f840aa8e5a7b2d5e6e545e31fe3ad55ffc7cf65c` |
| `parqit_linux_x86_64.zip` | `1061828aa57399a4380afd78cd0f3017bddc60e96d4f9036ab0fe8e352453381` |
| `parqit_macos_arm64.zip` | `c57c30f0a5e2a5f71c1d683c15e2ded49613feff3be3f0e458456848a644daba` |
| `parqit_windows_x86_64.zip` | `1da6cd461d53e87f08159baa83ef376aff9d575b9115701f2fb856f8a12b1d45` |
| `parqit_linux64.plugin` | `0d8f2f67c67a58fe4e3be6ee103567d7e336dee1f9b60ea27b9253d69740be82` |
| `parqit_macarm64.plugin` | `dc8d22331e57c9a38f111afc161cd71dd9e7c5ebaff6a2e8074a0ac41df22acf` |
| `parqit_win64.plugin` | `dcdafbfda3f3735ef574e2425e67494ab6238ba35a4a1d12e5b97b2b429aaaee` |

Os quatro ZIPs passaram `unzip -t`. A extração all-platforms contém os mesmos
ado, help, pkg, toc e dialogs do commit; as únicas adições esperadas são os
três plugins.

### 9.2 Linux x86-64

- ELF64 stripped;
- exports requeridos presentes;
- NEEDED apenas `libdl`, `libpthread`, `libm`, `libc` e loader;
- sem `libstdc++`/`libgcc_s` dinâmicos;
- maior versão de símbolo observada: `GLIBC_2.25`, compatível com o baseline
  declarado;
- `tests/verify_collected_plugin.sh ... linux`: PASS;
- net install isolado, `which`, version e selftest: PASS;
- full suite 71 logs/74 verdicts: PASS;
- repros REL-001 e campanha metadata/data: **BUG CONFIRMED**.

O smoke reportou `parqit 0.1.21`, DuckDB `v1.5.3` e carregou o plugin do PLUS
temporário, não o ado tree global.

### 9.3 macOS arm64

- Mach-O 64-bit arm64;
- exports globais definidos: apenas `_pginit` e `_stata_call`;
- liga `libc++` e `libSystem` como esperado;
- ZIP/manifest estruturalmente coerentes.

Não houve runtime Stata macOS. Esta inspeção não substitui load e testes num
Mac real.

### 9.4 Windows x86-64

- PE32+ DLL x86-64;
- `pginit` e `stata_call` presentes;
- gate estrutural atual passa;
- export directory contém 3518 nomes, contrariando o contrato de dois exports.

Não houve runtime Stata Windows. Não se inferiu falha funcional apenas da
over-export table; o problema foi proporcionalmente classificado S3.

## 10. Auditorias anteriores: hipóteses desafiadas

A leitura das auditorias anteriores foi feita depois da passagem independente.

- A auditoria de 9 de julho marcou tipos/metadata e classes de NUL/nomes como
  profundamente cobertos. Os novos contraexemplos mostram extrapolação entre
  eager, lazy collect, lazy save, fast direct e general direct.
- A auditoria de 14 de julho sobre `v0.1.20` encontrou bridge concurrency,
  lifecycle, m:m, v44 e stripping. A remediação `v0.1.21` para esses itens é
  real: os novos regressions e assets passaram nesta auditoria.
- Os findings atuais são predominantemente falsos negativos históricos de
  composição/cross-path, não regressões introduzidas pela remediação 0.1.21.
- O caso `src_name` é particularmente instrutivo: a correção existente cobre
  nomes sanitizados simples, mas observa duplicados só depois de DuckDB já os
  ter desambiguado.

Conclusão metodológica: um PASS num caminho não deve ser transferido para outro
materializador sem o mesmo fixture e oracle. Cada payload crítico precisa de
uma matriz eager/direct-fast/direct-general/lazy-collect/lazy-save e, quando é
key, de pelo menos um verbo que agrupe ou junte antes de materializar.

## 11. Limitações e risco residual

### 11.1 Não verificado

- runtime Stata em Windows e macOS;
- macOS Intel, que não é publicado;
- power loss, `SIGKILL`, disco realmente cheio, NFS/SMB e Windows AV locks nos
  pontos de rename;
- race live de dois processos a guardar simultaneamente no mesmo destino;
- ownership dinâmico de `<dest>.parqit_old`;
- fault injection real no segundo/terceiro `std::thread` dentro do plugin;
- matriz exaustiva de codecs, chunks, partition_by e todos os tipos em cada
  materializador;
- bytes arbitrários/UTF-8 inválido e múltiplos NUL em `strL` nos três OSes;
- globs com milhões de ficheiros, volumes próximos do limite real do SPI e
  campanha prolongada de fuzz/sanitizers;
- crash consistency sob falha de filesystem após set-aside e antes/depois do
  rename final;
- named views e bridges owned nas variantes de rename/SQL/plan-binder failure.

### 11.2 Hipóteses sugeridas, não promovidas a finding confirmado

1. Dois processos usam o mesmo `.parqit_tmp`; um pode apagar/verificar/publicar
   o staging do outro. A verificação atual só compara row count
   (`plugin_io.cpp:866-878`), portanto payload diferente com igual N é um risco.
2. `.parqit_old` pode apagar ficheiro/árvore alheia pelo mesmo defeito de
   ownership de REL-001; falta repro runtime específico.
3. SPI calls em workers paralelos podem ter restrições adicionais por OS; não
   foi encontrada prova suficiente para declarar um finding separado.

### 11.3 Três riscos residuais mais importantes

1. **Atomicidade/ownership em concorrência de output:** é a extensão natural do
   S0 confirmado e pode misturar ou eliminar outputs entre processos.
2. **Matriz numérica/key incompleta:** negativos vizinhos de `2^53`, int64
   extremos, DECIMAL, `%tC`, TIMESTAMP(ns), timezones e joins podem revelar
   outras colisões não injetivas.
3. **Cobertura cross-materializer incompleta:** o padrão fast/general/lazy que
   expôs `strL` e tipos pode repetir-se em temporais, reshape, collapse,
   partitioned save e metadata complexa.

## 12. Avaliação final de confiança

A evidência é forte quanto à existência dos bloqueadores: snapshot e plugin
estão identificados por hash; os S0 foram reproduzidos com oracles independentes;
o red-team tentou falsificá-los e não encontrou explicação alternativa; a
release pública Linux foi testada diretamente. Não se atribui percentagem
arbitrária.

A evidência também é forte quanto a várias zonas que funcionam: builds e suites
frescos, regressions de bridges, formatos correntes, signed zero, instalações e
assets Linux, e um subespaço diferencial aleatório. Contudo, essa cobertura não
permite uma garantia global porque:

- há S0/S1 confirmados;
- G1–G5 e G7–G9 falham;
- faltam runtimes Windows/macOS e cenários destrutivos de filesystem;
- a suite oficial fica toda verde na presença dos contraexemplos.

Formulação final rigorosa:

> Não é possível garantir a fiabilidade dos dados do `parqit v0.1.21`. Há
> defeitos confirmados que alteram/perdem dados silenciosamente e resultados ou
> metadados relevantes, incluindo no binário Linux efetivamente publicado.

## 13. Plano de remediação priorizado — sem implementação

### P0 — fechar todos os S0 antes de nova release

1. Substituir `.parqit_tmp/.parqit_old` determinísticos por staging exclusivo,
   criado atomicamente e registado como owned; verificar conteúdo/schema/hash,
   não apenas row count. Acrescentar two-process same-destination e sentinels
   flat/tree/file/dir nos tests.
2. Definir uma política única para tipos foreign não representáveis: recusa
   antes de verbos sensíveis ou opt-in explícito. Testar chaves adjacentes acima
   de `2^53` em contract/collapse/duplicates/merge/join/save.
3. Normalizar a avaliação de literais ao contrato binary64 antes de construir o
   SQL e recastar lazy outputs pelo `meta_type`; exigir paridade de valor e tipo
   em collect/direct/lazy.
4. Eliminar o fast-path special case de NUL em strL ou fazê-lo chamar a mesma
   validação do writer geral. Testar NUL em começo/meio/fim, múltiplos NUL,
   changed/unchanged e partitioned.
5. Validar exatidão de timestamp ms após epoch shift; recusar/anotar >`2^53` e
   testar vizinhos positivos/negativos em us/ns/TZ.

### P1 — corrigir S1 e atomicidade

6. Fazer bake/invalidação formal de sort em cada verbo; regressions com `_n`,
   `keep in`, first/last e múltiplas keys.
7. Usar `meta_type` para schema físico e criar matriz de todos os tipos por
   gen/replace/collapse/reshape/direct/lazy.
8. Decidir e cumprir a regra de untyped gen; alinhar Stata/help/ASSUMPTIONS.
9. Completar parser temporal com os limites nativos.
10. Construir o universo de ficheiros antes de ler KV; validar presença e JSON
    por ficheiro, com warnings acionáveis.
11. Transportar provenance posicional de duplicados e persistir/restaurar
    `sortedby` quando válido.
12. Tornar group rename, SQL clear e instalação de expression stages
    validate-then-commit com rollback testado.

### P2 — lifecycle, portabilidade e harness

13. Cobrir a criação do worker pool com RAII/catch/join e adicionar fault
    injection determinística.
14. Gerar export list Windows e exigir exatamente dois exports no gate.
15. Transformar cada finding confirmado num regression adversarial em todos os
    materializadores e no plugin staged/publicável.
16. Adicionar uma campanha property/fuzz de limites numéricos, nomes, strings e
    sequências de verbos, preservando os casos minimizados como fixtures.

### Critério mínimo de aceitação

- todos os S0/S1 corrigidos e os repros antigos ficam vermelhos pré-fix e verdes
  pós-fix;
- nenhuma limpeza remove sentinels não owned, mesmo com dois processos;
- matriz de valor/tipo/metadata passa em eager, direct fast/general, collect e
  lazy save com PyArrow/DuckDB/Stata;
- CTest, full suite Stata e mutations continuam verdes/vermelhas como esperado;
- full suite roda sobre o binário exato que será publicado;
- Linux, macOS e Windows passam gates estruturais, e pelo menos smoke Stata em
  cada plataforma distribuída antes de uma afirmação cross-platform forte;
- documentação e CHANGELOG descrevem precisamente limitações inevitáveis.

## 14. Resumo pedido

### Demonstrado

- cinco S0 e treze S1/S2 confirmados no snapshot `v0.1.21`;
- pelo menos REL-001, DATA-002, DATA-004 e a família metadata reproduzem-se no
  plugin Linux público;
- build, CTest, duas full suites, smoke, fuzz focado e release provenance passam;
- duas mutations controladas ficam corretamente vermelhas;
- os PASS oficiais não cobrem os contraexemplos.

### Apenas sugerido

- race/mistura same-destination;
- eliminação dinâmica de `.parqit_old` alheio;
- terminate real dentro do plugin por falha parcial de thread creation;
- efeitos runtime dos 3518 exports Windows.

### Continua por verificar

- runtime macOS/Windows, falhas físicas de filesystem, escala extrema e matriz
  completa cross-path/cross-type.

### Decisão

**Não publicar uma nova alegação de fiabilidade nem recomendar `v0.1.21` para
workflows em que estas fronteiras possam ocorrer antes de corrigir e fechar P0
e P1 com regressions independentes.**

## 15. Preservação do checkout

Estado inicial untracked, preservado:

```text
?? AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-07-14.md
?? CODEX_HOLISTIC_AUDIT_PROMPT_2026-07-09.md
?? PROMPT_AUDITORIA_ADVERSARIAL_FIABILIDADE_DADOS_PARQIT_2026-07-14.md
?? PROMPT_CODEX_IMPLEMENTAR_AUDITORIA_PARQIT_2026-07-14.md
?? examples/parqit_dlg.do
?? examples/pq_to_parqit_common_workflows.do
?? scratch_inj/
```

Este relatório é o único path novo autorizado pela auditoria. Não houve
commit, push, tag, release, PR ou edição de ficheiros versionados.
