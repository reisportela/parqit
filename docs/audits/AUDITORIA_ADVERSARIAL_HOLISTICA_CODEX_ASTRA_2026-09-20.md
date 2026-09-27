# Auditoria adversarial e holística do parqit — 2026-09-20

Foram reproduzidos seis achados, incluindo perda de colunas e de categorias de
omissos. A suite existente passa, mas não cobre as combinações que os expõem.
Não recomendo declarar concluída a integridade das funcionalidades novas de
`xmissing` enquanto CA-02, CA-03 e CA-04 permanecerem abertos.

Esta é uma auditoria do binário local e da árvore com trabalho por commitar.
Não é uma certificação da versão publicada nem uma implementação das correções.
As propostas ficam em patches separados, não aplicados. Codex executou a
auditoria; não foram usados agentes adicionais nem revisores externos.

## Evidência e método

Base observada: branch `perf/streaming-fill-and-audit-2026-09-20`, HEAD
`25fe286372ff6f25b73f1dee791596be7578f6ea`, banner parqit 0.1.37 e DuckDB
incorporado 1.5.3. A versão do banner não identifica, por si só, estas alterações.

| Artefacto | SHA256 |
|---|---|
| `ado/plus/p/parqit.plugin`, efetivamente usado | `d90bcece990031917958a23be0970a8eba9a5aedc2caf64c0bcd8a087d704ad4` |
| `build/dev/parqit.plugin` | `0b452e15a5ec9392580f37ebff269dd754d6663cbecaf1e1cf345201002725a0` |
| `build/dev/parqit_tests` | `c981a95b38e1fed6bfbe502c5d452aa87bceeb8d778eae7eaa0587334f930bbd` |

Os dois plugins têm GNU build ID `b37a29abdeb19b42d37325e9337999eac82caf34`;
a instalação contém o plugin stripped. Todos os ficheiros de `src/ado/p/`
coincidem byte a byte com os correspondentes de `ado/plus/p/`. Os objetos das
seis fontes C++ diretamente alteradas são posteriores às respetivas fontes.
Não houve recompilação: a referência executada é o build existente identificado
acima, como determina o roteiro.

Ambiente: Linux x86_64, StataNow MP 19.5, 48 CPUs disponíveis ao processo,
Python 3.12.12, PyArrow 24.0.0 e NumPy 2.2.6 no Python do Stata.
As sondas novas usam exclusivamente dados sintéticos, Stata nativo, PyArrow e,
nas somas extremas, `Fraction` sobre os valores binários exatos. A suite
preexistente usa também o dataset didático `auto` instalado com o Stata em
t15/v89/v92/v106/v107; a restrição a sintéticos aplica-se integralmente aos
ataques novos, mas não a essas cinco fixtures existentes. Nenhum dado de
investigação do utilizador foi usado. As versões do
Python/PyArrow/NumPy ficam no log da sonda `a11_wide_cell_trigger.do`.

Foram estudados o brief, as instruções locais, README, Unreleased, ASSUMPTIONS
#138–151, o índice de auditorias, os relatórios de 05/09 e 19/09, a carta externa
dos 14 riscos e o estilo local de xhdfe. A leitura dos registos numéricos
intermédios foi dirigida aos achados e remediações; não se afirma uma releitura
integral de todos os documentos históricos indexados. Não se alterou qualquer
destes materiais.

O [kit](../../audit_repro/codex_astra_20260920/README.md) contém o runner,
sondas, logs, inventários, matriz e propostas. O runner existente foi chamado
sequencialmente para cada um dos 124 testes de integration, verify e roundtrip.
Os cinco comandos definidos no CTest foram executados por CTest num diretório
novo, preservando `build/dev/Testing` e os seus logs anteriores.

| Gate | Início | Fim |
|---|---|---|
| Stata, 124 testes sequenciais | 152 PASS / 0 FAIL | 152 PASS / 0 FAIL |
| CTest, incluindo Valgrind de shutdown | 5/5 PASS | 5/5 PASS |
| Release/dialog/help lint | OK | OK |
| Sondas adversariais novas | 10 famílias: 35 PASS / 10 FAIL, sem abortos na execução concluída | 11 famílias: 37 PASS / 10 FAIL, sem abortos |

O roteiro refere 158 PASS na execução integral anterior. Não se tentou reproduzir
esse número omitindo diferenças de âmbito: `x01_bridge_xproc` e
`x02_output_xproc` exigem Stata concorrente e ficaram excluídos pela restrição
do §9. Não há resultado novo para esses dois testes. A enumeração de todos os
testes e veredictos executados está nos `results.json` das duas fases.

Os logs iniciais com problemas do próprio arnês foram conservados em
`probes_mjf6unti/`; a execução corrigida e repetida está em
`probes_c3_3vumm/`; a última execução, incluindo o trigger por células, está
em `probes_nuqn6lmi/`. A suite existente do início e do fim tem os mesmos nomes
de testes, os mesmos veredictos e todos os códigos de saída a zero.
Só linhas efetivamente emitidas como `VERDICT` contam:
linhas de código ecoadas pelo Stata e PASS anteriores a um aborto não contam
como conclusão de um teste.

## Achados

Todos os seis são CONFIRMADOS por execução. Os números dos achados não são os
números dos ficheiros de sonda: a tabela explicita a correspondência.

| ID | Área e severidade | Resultado reproduzido | Sonda |
|---|---|---|---|
| CA-01 | Definição de sessão; orçamento de memória não respeitado como documentado | `stream_buffer_mb 0` conserva o buffer da leitura anterior | `a01_stream_buffer_reset.do` |
| CA-02 | Integridade; perda de colunas com rc 0 | Relações inválidas em `parqit.xmissing` escondem primárias com valores | `a02_xmissing_graph.do` |
| CA-03 | Integridade e mensagem | `int64(string)` desativa a validação dos códigos; uma `.z` válida vira texto vazio apesar da nota de restauro | `a03_xmissing_interactions.do` |
| CA-04 | Integridade; ficheiros válidos | Um glob `relaxed` com companheiras diferentes perde `.a` e expõe a coluna auxiliar | `a08_xmissing_globs.do` |
| CA-05 | Documentação operacional | `discard` não repõe definições nem elimina a view no runtime ensaiado | `a09_session_discard.do` |
| CA-06 | Mensagem de erro | Conflito `delim()`/`nullstr()` expõe Binder Error e SQL gerado | `a10_csv_error_contract.do` |

### CA-01 — o valor zero não repõe o buffer

Sequência: ler a definição efetiva com `current_setting`, carregar um ficheiro
de 100 linhas com `stream_buffer_mb 8`, escolher `0`, carregar novamente e
consultar a definição através de `levelsof`, sem um novo fetch de preview.

```text
initial="976.5 KiB"; capped="7.6 MiB"; after_zero="7.6 MiB"
VERDICT(CA01_ZERO_RESETS_BUFFER): FAIL
VERDICT(CA01_DATA_PRESERVED): PASS
```

O motor apresenta em unidades binárias os 1 000 000 e 8 000 000 bytes
configurados. Isto é uma medição da configuração, não do RSS. O risco abrange
quem tenta reduzir o buffer após uma leitura que o tenha aumentado, incluindo
uma leitura com dimensionamento automático.

Causa: `src/plugin/plugin_io.cpp:3616` devolve `-1` para o zero; em `:3873`
o ramo `if (cap >= 0)` simplesmente não muda a configuração anterior.
`Session::set_streaming_buffer_bytes`, em `src/engine/session.cpp:432`, configura
um valor persistente na ligação. A fonte DuckDB confirma um reset próprio em
`src/main/settings/custom_settings.cpp:1605` e o cálculo do default em
`src/main/client_config.cpp:28`.

Proposta: executar `RESET streaming_buffer_size` antes do fetch quando o modo
é zero; manter o erro explícito se o reset falhar. `CA-01.patch` acompanha o
teste proposto `v110_stream_buffer_reset.do`. Esforço estimado: pequeno.

### CA-02 — uma companheira pode esconder a própria coluna de dados

PyArrow escreve `id=[1,2], x=[10,20]` e o mapa `{"x":"x"}`.
O leitor devolve rc 0, duas linhas e apenas `id`. A nota diz incorretamente que
`x` não existe no ficheiro. Com `x=[10,20], y=[30,40]` e
`{"x":"y","y":"x"}`, desaparecem ambas e é impressa uma nota de restauro.
As duas propriedades `NO_COLUMN_LOSS` falham.

Alcance: metadados inválidos produzidos ou alterados por terceiros. Não se
observou o escritor do parqit produzir estes mapas. Ainda assim, o leitor não
deve converter metadados contraditórios em eliminação bem-sucedida de dados.
O payload original foi lido independentemente com PyArrow antes da chamada.

Causa: `src/plugin/plugin_io.cpp:1701` acrescenta a companheira a `hidden`
antes de validar a relação; o caso autorreferente é tratado como primária
ausente em `:1704`. Não existe guarda para uma coluna declarada simultaneamente
primária e companheira, e as colunas escondidas são retiradas em `:1727`.

Proposta: recusar esta sobreposição antes de esconder qualquer coluna.
Não é necessário recusar a companheira órfã normal, que tem uma política
documentada distinta e passou a sua sonda. `CA-02.patch` inclui
`v111_xmissing_graph.do`. Esforço estimado: pequeno.

### CA-03 — converter BIGINT para texto contorna a validação de omissos

Fixture: `x=[9007199254740993,NULL]`, companheira `int8=[0,27]` e mapa válido.
`parqit use ..., clear int64(string)` devolve rc 0, os dígitos exatos na
primeira linha e texto vazio na segunda. Também aceita o mapa `[1,0]`, que
atribui um omisso à célula não nula. Os quatro testes, com 1 e 2 workers,
falham a propriedade de recusa. Não são truncamentos do payload BIGINT:
`9007199254740993` chega corretamente.

No controlo válido `[0,26]`, a `.z` também vira `""`, mas a mensagem continua
a afirmar que as companheiras são restauradas. Este controlo é registado como
observação, não como uma exigência retroativa de um formato textual ainda não
explicitado no manual.

Causa: `plan_big53_as_text`, em `src/engine/typemap.cpp:370`, muda o transfer
para Utf8. A guarda em `src/plugin/plugin_io.cpp:3265` exclui Utf8 de toda a
validação e restauração. A nota continua a usar a lista construída antes dessa
conversão (`:1725` e `:2167`).

Proposta: validar os códigos independentemente do transfer final. O patch
propõe adicionalmente preservar `.a`–`.z` como texto literal e acrescenta essa
regra ao help, mantendo o omisso ordinário como texto vazio. Esta é uma escolha
explícita de contrato da correção candidata, não comportamento já validado.
`CA-03.patch` inclui `v112_xmissing_text_integrity.do`. Esforço: pequeno a médio,
pela interação entre representação, nota e documentação.

### CA-04 — glob de ficheiros válidos perde a categoria do omisso

Dois ficheiros escritos pelo próprio parqit, ambos com `save, data xmissing`:
o primeiro contém `(id,x)=(1,.a),(2,10)`; o segundo contém `(3,.),(4,20)`.
O segundo não necessita de companheira. O PyArrow confirma os códigos e os
tipos antes da leitura conjunta.

```text
parqit use "union_xm_*.parquet", clear relaxed
note: parqit metadata differs across matched files; labels/formats not restored
id    x    _parqit_xm_x
 1    .          1
 2   10          0
 3    .          .
 4   20          .
VERDICT(CA08_MIXED_COMPANIONS_SAFE): FAIL
```

Alcance: globs de exportações válidas quando a ocorrência de omissos estendidos
varia entre ficheiros. A mudança de `x=.a` para `x=.` pode alterar chaves de
operações posteriores. Os bytes da companheira ainda existem, mas a variável
primária carregada está errada e a mensagem anuncia apenas perda de apresentação.

Causa: `src/plugin/plugin_io.cpp:912` exige igualdade de todos os `parqit.*`.
Ao encontrar diferença, devolve o objeto vazio, descartando também o canal que
define valores omissos, e não apenas labels/formatos. ASSUMPTIONS #148 e ToDo
já reconhecem o mecanismo na atualização parcial de partições; a sonda demonstra
o seu alcance no glob público `relaxed`, que não está recusado.

Proposta imediata: recusa explícita quando essa queda global de metadados
afetaria companheiras, com instrução para ler e fazer `appendin` separadamente.
Uma união por ficheiro dos mapas é uma implementação posterior, com validação
de identidade e códigos, não uma simples remoção da guarda existente.
`CA-04.patch` inclui `v113_xmissing_mixed_files.do`. Esforço: pequeno para a
recusa; médio para reconciliação integral. A recusa deve ser anunciada como
alteração de comportamento quando implementada.

### CA-05 — `discard` conserva o estado do plugin observado

Depois de configurar `fill_threads=1`, `threads=1`, `stream_buffer_mb=8` e
abrir uma view, executou-se o comando nativo `discard`. `parqit version`
continuou a mostrar 1/1/8, e `parqit count` continuou a devolver uma linha
da view anterior. O processo tinha começado com 48 threads e definições auto.

O help afirma o contrário em `src/ado/p/parqit.sthlp:1182` e
`parqit_technical.sthlp:851`; `CLAUDE.md:65` aconselha `discard` como alternativa
a reiniciar depois de um rebuild. O código de carregamento volta a testar o
plugin já registado em `src/ado/p/parqit.ado:36`, e os estados de sessão vivem
em objetos C++ persistentes. O help local do Stata para `discard` descreve a
remoção de programas carregados automaticamente, não promete um reset deste
estado C++.

Proposta: corrigir a documentação e instruir o reinício do Stata após um rebuild;
usar `parqit close _all` para fechar explicitamente as views. Não acrescentar
um reset implícito com semântica nova. `CA-05.patch` inclui
`v114_discard_plugin_persistence.do`, que fixa a persistência observada e deve
ser qualificado nas outras plataformas. Esforço: pequeno.

### CA-06 — erro de CSV mostra o SQL interno

Num CSV benigno com separador `;`, chamar
`csv(delim(";") nullstr("a;b"))` devolve rc 920 e mostra:

```text
parqit use: Binder Error: DELIMITER must not appear in the NULL specification and vice versa
LINE 1: SELECT * FROM read_csv_auto([...], ...)
VERDICT(CA10_OWN_ERROR_MESSAGE): FAIL
```

A sentinela fica intacta: a atomicidade passou. Não se observou execução de SQL
injetado. O defeito é de tradução e atribuição da mensagem, contra o contrato
de v69 e a promessa de erros próprios para as opções públicas.

Causa: `csv_options_from_request`, `src/plugin/plugin_io.cpp:543`, valida as
opções individualmente, mas não esta combinação; o erro chega pela sondagem
de schema do motor. `CA-06.patch` propõe uma validação prévia específica e
inclui `v115_csv_option_error_contract.do`. Não cobre todos os erros possíveis
do leitor CSV. Esforço: pequeno para este caso; médio para a revisão global.

## Falsos positivos e dúvidas resolvidas

- As primeiras versões de a04/a06 usavam blocos Python multilinha dentro de
  `foreach` e terminavam com `r(1)` no parsing do arnês. Foram corrigidas para
  chamadas Python numa linha. Não constituem falhas do plugin; os logs originais
  foram preservados e as sondas concluídas passaram.
- A primeira tentativa de `nullstr` continha também o próprio delimitador.
  A recusa não demonstrava injeção. Separaram-se o teste de literal e o conflito
  benigno de opções; só a exposição da mensagem foi classificada como CA-06.
- O argumento da companheira está presente no caminho numérico dos workers.
  A sua ausência no ramo strL não demonstra perda de omissos numéricos; a04
  confirmou o round trip dos seis numéricos e do texto longo em ambos os modos.
- Não se exigiu igualdade ao Stata onde a acumulação nativa pode perder
  precisão. A soma de `[maxdouble,-maxdouble,1,0]` foi adjudicada por `Fraction`:
  parqit devolve exatamente 1 e mantém os dois extremos como valores.
- A ausência de `int64`, `fill_threads` e `stream_buffer_mb` no diálogo de
  definições já consta de ASSUMPTIONS #149. Foi reconfirmada, não apresentada
  como uma descoberta nova.

## Performance

Não há proposta de aceleração nesta auditoria. Não foram feitos benchmarks
antes/depois nem medições de pico RSS; os tempos dos runners incluem a criação
de fixtures e validações, pelo que não são tempos de leitura comparáveis.

Os dois writers e os fetches streamed/materialized foram comparados por
integridade, não para atribuir speedups. Não se reabrem a alocação em lote,
o cap fixo de streaming, o limite arbitrário de workers nem a redução de precisão.
Corrigir CA-01 pode aumentar o tempo do modo zero porque passa a respeitar o
limite de buffer escolhido; essa troca deve ser medida e explicada na implementação.

Os seis patches passaram apenas `git apply --check`. Não foram aplicados,
compilados ou medidos: não existe GO funcional ou de desempenho para as soluções.

## Matriz da superfície revista

O inventário executável `surface_audit.py` encontrou 53 comandos públicos,
53 programas ado e 53 entradas na sintaxe do help. A matriz individual está
em `surface_9kel2a56/matrix.json`. Presença textual não prova todas as opções
nem todas as suas combinações.

Revisão da qualidade dos testes: v108 chama "wide" a uma fixture de 90 000
linhas, que já satisfaz o limiar por linhas; isso não isola o novo limiar por
células. A sonda a11 usa 20 000 × 110, provoca uma falha na criação do primeiro
worker apenas no modo auto e verifica todas as células nos modos serial/auto.
O teste passou; é uma lacuna de isolamento do teste existente, sem defeito
funcional reproduzido nessa regra.

| Família | Comandos | Evidência e limites |
|---|---|---|
| Fontes | use, open, sql | Integration, tipos, bridges, v104–v107; novas sondas de metadados/CSV |
| Linhas | keep, drop, sample, duplicates | Suite existente, intervalos e oráculos nativos; amostragem v95 |
| Colunas | gen, egen, replace, rename, order | Pipeline, v61/v68/v93 e testes de precisão |
| Ordenação | sort, gsort | Pipeline, chaves de merge, sortedby e comparação completa a04 |
| Agregar/reestruturar | collapse, contract, reshape, pivot | Suite existente, percentis, a05 com chaves IEEE e soma exata |
| Combinar | merge, append, joinby, mergein, appendin | v14/v25/v71/v75/v88/v96/v102/v103; CA-04 afeta a preparação dos dados |
| Materializar | collect, save | Tipos, metadata, ambos os writers, streamed/materialized, corrupção e atomicidade |
| Exploração | describe, glimpse, ds, lookfor, codebook, count, head, list, levelsof, distinct, misstable | Integration e verify; recusas/limites v66–v69/v86 |
| Estatística | summarize, tabstat, tabulate, correlate, pwcorr, histogram | Oráculos nativos/exatos existentes e a05; sem pesos novos |
| Sessão | view, views, close, show, explain, query, set, path, version, selftest, menu | Integration, lint, taskset v108, a01/a09; CA-01 e CA-05 abertos |

O diálogo de leitura tem controlos e emissão de `int64`, `binary`, `filename`
e `csv`; o de escrita emite `xmissing`. O diálogo de definições só tem quatro
das sete opções. Também não há controlo `int64()` em collect no diálogo de
escrita nem em mergein/appendin no diálogo de combinação. São lacunas de
cobertura da interface, não novas provas de dados incorretos.

Não se executaram cliques novos nem se certificou a aparência do Viewer.
O harness GUI foi lido, mas contém caminhos temporários antigos e gestão de
Xvfb/PIDs específica de outra execução. Usaram-se o lint dos dez diálogos e
os testes batch de formas/contexto; as sessões gráficas existentes ficaram intactas.

## Não achado: ataques executados sem defeito

- a04: 60 001 linhas × 9 variáveis, byte/int/long/float/double, `%tm`, `.a/.z/.m`,
  UTF-8/emoji, strL acima de 2045 bytes, labels, notes, characteristics e sortedby.
  PyArrow confirmou payload, tipos e todos os metadados iguais entre os dois
  writers; Stata nativo confirmou todas as células e a datasignature em seis
  combinações de fetch × workers. Nomes `x`/`X` com códigos distintos passaram.
- a05: NULL, NaN, ±Inf e 1e308 em chaves estrangeiras dão os mesmos grupos que
  a representação explícita de missing no Stata. Collapse e contract passaram;
  a soma exata e os dois extremos de `maxdouble()` também.
- a06: corrupção de 32 bytes numa página gzip do último de 20 row groups,
  rejeitada independentemente por PyArrow; quatro combinações de fetch × workers
  recusam e preservam a sentinela. A leitura seguinte verifica as 200 000 linhas.
- a07: texto `0001`, `1e5`, `TRUE` preservado com allvarchar; apóstrofos e
  tokens SQL em nullstr ficam literais. Sondas de delim/quote/escape/dateformat/
  timestampformat e um types inválido não executaram a instrução adicional.
- a11: 20 000 linhas × 110 colunas, abaixo do limiar por linhas e acima de
  dois milhões de células; disparo efetivo dos workers e preservação integral
  dos valores e da datasignature nos modos serial e automático.
- a02: código 256 recusado sem alterar a sentinela; companheira órfã não aplicada.
  Um mapa que aponta para uma companheira inexistente foi observado a carregar
  missing simples sem nota específica; ficou registado, sem inferir um payload
  original que o ficheiro já não permite recuperar.
- Suites existentes: fronteiras de tipos, datas, nomes hostis, metadata, ranges,
  globs/Hive, partições, bridges, estatísticas e falhas de transferência. São
  execuções novas dos testes existentes, não novos ataques inventados para este relatório.

A carta dos 14 riscos está representada por testes atuais: posição/nome e
round trip em t01/t11/v02/v10/v12/v18/v19; datas em v03/v05/v22/v54/v74;
partições em v76/v78/v85; inteiros/tipos em v06/v11/v15/v41/v104/v105;
labels/metadados em v07/v39/v51; erros/atomicidade em v08/v09/v49/v52/v73;
intervalos em v13/v62/v86. A correspondência é por invariante aplicável ao
parqit, não por opções exclusivas do outro pacote.

Limites de cobertura: sem runtime macOS/Windows/Stata 16, sem nova CI, sem
cliques GUI, sem dois Stata concorrentes, sem kill durante publicação, sem
falha física de disco, sem benchmark de 50M/100M linhas. Não se provou
formalmente a reentrância do SPI proprietário. O header vendorizado expõe
callbacks (`vendor/stata/stplugin.h:220`), não a implementação de SF_vstore;
a evidência disponível é empírica nos testes executados.

Na API do motor, a leitura direta da fonte confirmou: erro e EOF partilham
o NULL em `src/main/capi/stream-c.cpp:17`; o erro é consultado antes do destroy
em `src/plugin/plugin_io.cpp:4209`; o cleanup explícito está em `:4227`.
A conversão Arrow é por chamada/chunk em `src/main/capi/arrow-c.cpp:48` e
`src/common/arrow/arrow_converter.cpp:19`. As referências sem prefixo completo
deste parágrafo são internas a `build/dev/_deps/duckdb-src/`. Estas observações
não substituem testes de todas as falhas de alocação e de todos os sistemas.

## Plano de remediação

1. CA-04 e CA-02: proteger a identidade e os valores antes de permitir novos
   resultados bem-sucedidos. CA-04 é atingível só com ficheiros válidos do próprio
   escritor; CA-02 endurece a fronteira contra metadata contraditória.
2. CA-03: retirar o bypass de validação e decidir/documentar a representação
   textual dos códigos. Executar o teste com ambos os fetches e workers, e
   verificar o Parquet resultante se for acrescentado um novo round trip.
3. CA-01: repor efetivamente o buffer; testar transições auto→0, número→0,
   ambiente→opção e falha do reset, medindo o efeito em memória e tempo.
4. CA-05/CA-06: alinhar instruções de sessão e erros de CSV; só depois completar
   as opções ausentes nos diálogos com um clique real no comando emitido.
5. Compilar a composição dos patches, correr cada regressão antes/depois e a
   suite integral na instalação preparada; declarar mudanças públicas no
   changelog. Só então avaliar desempenho ou preparar uma release.

Não recomendo otimização do fill durante esta correção, substituir a aritmética
exata pelo resultado nativo menos preciso, aceitar genericamente metadata
divergente, nem implementar a fase 2 dos omissos lazy como um remendo incidental.

## Custódia

O inventário inicial contém 2 590 ficheiros versionados ou não ignorados
preexistentes, incluindo as alterações locais. Os ficheiros ignorados fora
da instalação/build identificados não foram sujeitos a um inventário integral.

SHA256 do `git diff --binary` inicial:
`a05cd6c9027d0c800c9c2f09afb6afe32aeee72ff4de0f8c0a5f1f516f2816e6`.
O índice inicial está vazio: hash
`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`.
A comparação final está no
[recibo](../../audit_repro/codex_astra_20260920/receipt_95uc5518/receipt.json),
com hashes finais, e no
[inventário de adições](../../audit_repro/codex_astra_20260920/receipt_95uc5518/created_files.txt).
O diff final tem o mesmo SHA256 inicial indicado acima. O diff, índice, HEAD, estado
Git e binários mantêm-se iguais, mas existe uma exceção ao inventário integral:
`examples/logs/Examples_Parqit_BigData_read_V01.txt` cresceu de 2 574 para
2 949 bytes. O log estava aberto desde as 14:29:51 e o seu último mtime,
15:15:07, antecede o primeiro teste Stata da auditoria, lançado depois do CTest
terminar às 15:15:10. A evidência aponta para uma sessão preexistente; não se
atribuiu um PID histórico. Ver
[custody_note.md](../../audit_repro/codex_astra_20260920/custody_note.md).
O log foi preservado. As restantes 2 589 entradas comparadas coincidem.

As únicas adições desta auditoria à árvore são este relatório e
`audit_repro/codex_astra_20260920/`. Os testes usam diretórios próprios sob
`/tmp`; os runners existentes removem apenas o seu scratch por teste.
Não houve eliminação de materiais preexistentes, alteração das fontes,
instalação global, configuração pessoal, commit, push, PR ou publicação.

Reverter as adições da auditoria não exige reverter código: basta arquivar os
dois alvos novos, com autorização para essa operação. A atualização do log
preexistente deve permanecer fora dessa operação. Os patches de correção
permanecem propostas para revisão.
