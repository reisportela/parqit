# Correções da migração DuckDB 1.5.6 → 2.0.0

Branch: `review/duckdb-2.0.0-lts-20261022`, a partir de
`aa9f2dbaa773ca91354f1bf5d6f89197d713ab47`.

Atualizado em 2026-10-23. **NO-GO para substituir 1.5.6.** As correções passaram
a matriz local de validação, mas subsistem perdas de desempenho não aceites e
uma decisão pendente sobre mudanças documentadas no SQL direto. Os alvos de
distribuição também não estão certificados. O [relatório inicial](AUDITORIA_MIGRACAO_DUCKDB_2_0_0_2026-10-22.md)
e os seus binários permanecem preservados. O default continua em 1.5.6.

## Discussão pedida e plano de aceitação

Foi consultado o modelo exato `claude-fable-5-1`, com `--effort max`, sem
ferramentas, acesso ao workspace ou persistência de sessão. Foram concluídas
quatro rondas técnicas, identificadas em `advisor/review_receipts.json` dentro
de `audit_repro/duckdb_200_remediation_20261022/`. As respostas finais estão em
`focused_response.txt`, `followup_response.txt`, `final_code_response.txt` e
`performance_contract_response.txt`, no mesmo subdiretório `advisor/`.
A primeira consulta, mais extensa, excedeu o limite de geração e não constitui
uma revisão concluída. A análise e a aceitação locais continuam a cargo do Codex.

O revisor exigiu a materialização das chaves voláteis, verificações das páginas
Parquet múltiplas, testes de caminhos sem glob, controlo do pool real de I/O e
desempenho só depois da correção dos resultados. Essas exigências foram
incorporadas. A sua objeção aos limites das junções confundia limites de
**número de linhas** com valores de somas: para os seus dados `+10,-9` e duas
correspondências do `+10`, o limite de quatro linhas dá o intervalo `[-36,40]`,
que contém a soma correta `11`. Mantêm-se limites conservadores e aritmética
verificada; não se desativa globalmente a otimização das somas.

Na segunda ronda o Claude retirou expressamente a objeção aos limites das
junções e confirmou a projeção das chaves voláteis. Na terceira confirmou o
tratamento das molduras e identificou a necessidade de criar uma seleção de
linhas própria, em vez de escrever numa seleção potencialmente partilhada.
Na quarta exigiu provas dos contratos das funções, regras de tipos partilhadas
com os callbacks, verificação da aridade e preservação do binder. Essas condições
foram incorporadas e testadas. Os erros possíveis de percentagens, histogramas
e fronteiras temporais mantêm a proteção do motor.

O utilizador autorizou expressamente o envio do material técnico adequado.
Foram enviados código e resultados sintéticos, sem microdados ou credenciais.
Os pareceres são consultivos; não constituem uma certificação independente dos
resultados executados localmente pelo Codex.

Plano, pela ordem das dependências:

1. Preservar o estado inicial e isolar os novos builds e os dados sintéticos.
2. Corrigir o orçamento de trabalhadores, a leitura Parquet/CSV e a proteção
   dos inteiros antes de avaliar desempenho.
3. Corrigir limites e duração das estatísticas das junções; materializar chaves
   voláteis antes das otimizações das janelas.
4. Restaurar as fronteiras de tipos, metadados, caminhos e erros numéricos.
5. Exigir testes adversariais dirigidos, CTest completo, Valgrind e suite Stata
   completa, incluindo referências nativas e oráculos independentes.
6. Medir 1.5.6/2.0.0 em blocos intercalados com resultados exatos; qualquer perda
   sem justificação documentada impede aceitar a substituição.

## Correções implementadas

| Área | Causa e correção |
|---|---|
| Inteiros exatos | O otimizador podia transformar em constante o predicado SQL de proteção. A consulta lê extremos `HUGEINT/UHUGEINT` com a barreira de leitura, e a comparação exata ocorre em C++. Testam-se rodapés falsificados, incluindo `min=max=0`, sem alterar as páginas de dados. |
| Parquet com nulos | Extremos textuais iguais perdiam a possibilidade de nulos. Nas páginas, a eliminação também emite nulos como placeholders; conserva-se validade desconhecida, mantendo o refinamento de validade dos grupos de linhas. |
| CSV | A nova conversão no leitor interpretava o texto diretamente como o tipo final. Conversões entre tipos distintos voltam a ocorrer depois da leitura do tipo detetado. |
| Caminhos | A expansão local preserva `link/..` para resolução pelo sistema operativo, incluindo caminhos sem glob. |
| Tipos nulos | A anotação Parquet `UNKNOWN` conserva o tipo físico como em 1.5.6. O adaptador continua a recusar um tipo efetivamente SQLNULL; a diferença de inferência/materialização de `NULL` no SQL direto está identificada separadamente abaixo. |
| Junções | Limites conservadores respeitam os lados preservados. Estatísticas locais sobrevivem à análise recursiva do outro lado, incluindo junções posicionais. |
| Janelas voláteis | Uma projeção anterior às otimizações calcula cada ocorrência uma vez; partição, ordenação, pares e molduras usam a mesma chave. Uma chamada independente no offset de `RANGE` permanece independente. |
| Erros numéricos | Restauram-se os erros explícitos de variância, desvio-padrão e correlação em caso de resultados/intermédios não finitos, preservando os acumuladores. |
| Metadados e Hive | Ficheiros com `parqit.schema` mantêm campos opcionais; a verificação de valores reservados de partição descodifica uma única camada percentual. |
| Literais | Inteiros literais acima de `UHUGEINT` preservam a inferência `DOUBLE` de 1.5.6; valores explicitamente `BIGNUM` mantêm esse tipo e a fronteira de suporte do Stata. |
| Trabalhadores | `async_threads` segue o orçamento de threads de consulta; testes verificam o pool real e o encerramento. |
| Buffer mínimo | O default de 2.0 passou de 1 000 000 para 10 485 760 bytes. O build embutido conserva o default/RESET de 1.5.6, preservando `stream_buffer_mb 0`. |
| Contratos das funções | A C API 2.0 marca todos os escalares como falíveis, impedindo a reutilização de dicionários. Um binder próprio recupera apenas contratos demonstrados para os tipos efetivamente ligados; os restantes continuam falíveis. O classificador de tipos e famílias é partilhado com o leitor dos callbacks. |
| Escrita particionada (2026-10-24) | PARTITION-DELAY-1: uma partição com um buffer adiado pequeno deixava de encaminhar os lotes grandes seguintes por um escritor de uma só thread, sem ORDER_BY nem rotação (`physical_copy_to_file.cpp` `b80c038f…` → `e676b2f4…`). PARALLEL-PACK-1: só no COPY particionado, cada tarefa de materialização compacta peças fixas do mesmo grupo de chave única, produzindo a mesma coleção (`hashed_sort.cpp` `acc3bc3e…` → `205a3eff…`, `hashed_sort.hpp` `74d68c56…` → `ae5affee…`). Pins inalterados: `physical_copy_to_file.hpp`, `parquet_writer.cpp`, `column_data_collection.cpp`. Evidência: `audit_repro/bigdata_500m_20261024/partition_engine_claude_01/`. |

Os patches DuckDB verificam hashes dos ficheiros originais e corrigidos,
recusam modificações desconhecidas e preservam staging já existente. Não há
desativação global de otimizadores nem relaxamento das tolerâncias numéricas.

## Evidência já obtida

O candidato congelado em `acceptance_stage_19/` passou os 188 alvos Stata,
com 232 veredictos PASS, zero FAIL e zero interrupções
(`staged_stata_final_summary.json`). CTest passou 6/6, com 247 casos C++ e
436 622 asserções, além da execução específica de encerramento sob Valgrind
(`ctest_acceptance_20.log`). O microbenchmark Arrow marcado como opcional na
suite permanece SKIP; não foi contado como teste de correção executado.

Os testes dos contratos escalares cobrem 361 pares de tipos, escalas zero e
positivas, larguras até 128 bits, signed/unsigned, BOOLEAN, extremos de HUGEINT
e DECIMAL(38), NULL, parâmetros preparados, NUL embutido e offsets extremos.
Uma função provadamente total utiliza o dicionário; uma função comum da C API
continua sujeita à política conservadora. Tipos incompatíveis e percentagens
inválidas continuam a falhar quando efetivamente avaliados.

O teste V169 passou páginas únicas/múltiplas, índice de páginas, dicionário e
ausência de estatísticas (`v169_02.log`). As versões anteriores das correções
foram rejeitadas pelos novos testes e permanecem documentadas nos logs.

A matriz independente adicional verificou 163 casos em ambos os motores. Outra
matriz passou 96 combinações de nulabilidade, páginas V1/V2, PLAIN, encodings
delta, fallback de dicionário e predicados que aceitam NULL. Os headers dos
ficheiros confirmam os encodings efetivamente escritos. Uma variante de mais
96 casos força páginas a atravessar os limites dos vetores. Os 96 resultados
sob Valgrind coincidem com os resultados nativos, sem erros de memória.
Passaram ainda 28 oráculos de campos STRUCT/LIST e filtros aninhados.

O código partilhado também passou os 188 alvos Stata/232 PASS em 1.5.6 após a
refatorização do classificador de tipos (`baseline_stata_shared_final_summary.json`),
além de CTest 6/6. Um processo Stata novo resolveu o ado e o plugin pela árvore
staged completa, sem `PARQIT_PLUGIN_PATH`, confirmou versão/source ID e executou
selftest e os dois percursos de dados (`loader_check_19b/receipt.json`).

A reprodução de partições voláteis com 10 007 linhas e 17 chaves possíveis
encontrou grupos incorretos também em 1.5.6. A correção em 2.0.0 compara os
resultados com um `GROUP BY` sobre chaves determinísticas e verifica o número
de chamadas da sequência. Trata-se de uma melhoria de correção, não de aceitar
a equivalência entre dois resultados errados.

Uma janela de 3 000 007 linhas com limite de 32 MB produziu a permutação e soma
esperadas, consumiu exatamente 3 000 007 valores da sequência e derramou
11 272 192 bytes para disco. Outra, com quatro threads, 6 000 007 linhas e
limite de 64 MB, derramou 68 386 816 bytes e conservou a soma exata e exatamente
uma chamada por linha (`window_extra_final_receipt.json`). Subconsultas
correlacionadas, QUALIFY, CTE recursiva e execução preparada também passaram.

## Desempenho: impedimento confirmado

O ensaio final usa um milhão de linhas sintéticas, cinco blocos ABBA, vinte
processos Stata novos e três repetições por fluxo em cada processo. Threads de
consulta/fill: 4/4; memória DuckDB: 1 GB; cache do SO quente. Todas as assinaturas
Stata, as asserções analíticas e a leitura independente dos ficheiros gravados
passaram. A referência é o plugin 1.5.6 congelado antes desta revisão.

O rácio abaixo é a mediana dos cinco rácios emparelhados por bloco, com médias
geométricas A e B dentro do bloco. Não é o quociente das duas medianas globais
de tempo. Valores acima de 1 representam maior tempo em 2.0.0.

| Fluxo | Mediana 1.5.6 (s) | Mediana 2.0 corrigido (s) | Rácio emparelhado B/A |
|---|---:|---:|---:|
| collect | 0,07600 | 0,10850 | **1,4558** |
| filtro | 0,02033 | 0,02783 | **1,2941** |
| strings | 0,43833 | 0,47767 | **1,0936** |
| clock | 0,15650 | 0,15350 | 0,9279 |
| collapse | 0,30167 | 0,25817 | 0,8357 |
| egen agrupados/fundidos | 0,30100 | 0,32517 | 1,1288 |
| merge | 0,19950 | 0,20750 | 1,0613 |
| rename | 0,15617 | 0,18100 | 1,1429 |
| save | 0,05867 | 0,06350 | 1,1855 |
| windows | 0,44017 | 0,46983 | 1,0417 |
| CSV | 0,59083 | 0,61117 | 1,0034 |
| CSV com conversões | 0,68617 | 0,73133 | 1,0748 |
| páginas com nulos | 0,13550 | 0,12850 | 0,9479 |

Collect, filtro e strings foram mais lentos nos cinco blocos. Os outros fluxos
têm dispersão maior e não recebem uma certificação geral de não regressão.
O pico RSS mediano do processo Stata completo foi 471 798 KiB na referência e
519 430 KiB no candidato; não é uma medição isolada da memória do DuckDB.
Resultados completos, controlos A/A e B/B e hashes:
`performance_final_19/receipt.json`, `measurements.json` e `performance_decision.json`.

A marcação incondicional dos escalares da C API como falíveis explicava a maior
regressão de strings: os callbacks deixavam de reutilizar dicionários. A correção
dos contratos reduziu o rácio de aproximadamente 1,83 para 1,09. Os rastreios
separam também o trabalho em `execute_pending`, fetch e Arrow. Variações de
read-ahead, número de trabalhadores I/O, cache local e preparação antecipada do
stream não resolveram consistentemente o problema. Não foram promovidas a
configurações do produto. A tentativa de Callgrind excedeu 180 s sem produzir
um perfil utilizável; não foi usada como prova.

**Nenhuma perda foi aceite.** Não foi estabelecida uma ligação documentada e
direta que permita classificar estas perdas como exceções expectáveis ao
critério do utilizador. O [artigo oficial sobre I/O assíncrono](https://duckdb.org/2026/07/31/asynchronous-io)
refere diferenças negligenciáveis no exemplo de leituras locais com cache
quente, não uma autorização geral para estas regressões. O
[guia de migração](https://duckdb.org/docs/2.0/operations_manual/migration)
recomenda testes numa instalação paralela e ponderar versões corretivas para
aplicações críticas; a linha 1.5 permanece suportada até março de 2027.

## SQL direto: contrato ainda por decidir

A confrontação final com o guia oficial confirmou diferenças que os testes
dos verbos Stata não cobrem como equivalência de linguagem. A pergunta ao
utilizador distingue preservar integralmente a semântica 1.5.6 no SQL direto
de adotar a semântica nativa 2.0. Sem essa decisão não se apresenta a passagem
da suite como garantia para qualquer consulta SQL.

| Reprodução | 1.5.6 | Candidato 2.0 atual |
|---|---|---|
| `'abc' ~ 'b'` | false | true |
| `2*3!` | 720 | 12 |
| `X'41'` | VARCHAR `x41` | BLOB `A` |
| `B'0101'` | VARCHAR `b0101` | BIT `0101` |
| `CREATE TABLE ... AS SELECT NULL x` | INTEGER | NULL |

Os resultados estão em `documented_sql_baseline.jsonl` e
`documented_sql_candidate.jsonl`. São mudanças documentadas, não foram
classificadas como corrupção indiscriminada. Também não foram aceites
silenciosamente como compatibilidade integral. O guia indica opções de
compatibilidade para regex e NULL e uma extensão de parser legado; não foi
descarregada nem instalada outra extensão nesta ronda.

## Identidade e custódia

O candidato tem source ID
`25fb6a618d57bad75c5aa7bcdff3030fea5c86374efbfc359d06ea0b23510302`
e o plugin congelado tem SHA256
`781b54ee534a74c565545108d3c4523a32bc54b5d4123db81d04a6af408ca71c`.
O ID foi recalculado independentemente a partir de 83 entradas de source/pins.

Um build de verificação de 1.5.6 usou inicialmente o destino ado predefinido
do repositório. O destino foi corrigido para staging isolado e esse único
plugin gerado foi preservado como prova. A árvore `ado/plus/p` foi reposta no
estado da última release verificada: os outros 18 ficheiros já coincidiam e
o plugin reposto tem SHA256
`dbb7f098b07caac3c4e4fc3da7f039ff22470a81d8cf5087ca50943594efc38c`.
Não existia um hash desse plugin no início deste turno, pelo que a prova é de
reposição da release verificada, não de identidade com um snapshot inicial
inexistente. Recibo: `custody/repo_stage_recovery.json`.

## Limites

Não foram executados Stata nativo em Windows/macOS, CI de distribuição,
publicação, instalação global, commit ou push. Os resultados locais não
certificam esses ambientes. Os exemplos e testes usam apenas dados sintéticos
ou públicos; não foram usados microdados.
