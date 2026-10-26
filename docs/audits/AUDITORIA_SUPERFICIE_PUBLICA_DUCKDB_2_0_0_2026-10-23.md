# Auditoria da superfície pública: parqit com DuckDB 2.0.0

Branch: `review/duckdb-2.0.0-lts-20261022`.

**Estado:** revisão funcional Linux aprovada na matriz executada; desempenho
recuperado nos percursos correntes, com uma ressalva explícita de memória/tempo
na ordenação limitada a 128 MB. Não se declara aceleração universal nem se
promove o candidato a release.

## Referência, identidade e âmbito

A referência é o **binário efetivamente publicado**, não uma recompilação local
do motor antigo. A consulta à [release pública v0.5.1](https://github.com/reisportela/parqit/releases/tag/v0.5.1)
identificou o commit `2de6756070c17f5d7e88fad8581fac9733a48d05` e confirmou
os digests dos 28 assets já disponíveis localmente. A referência Linux tem SHA256
`dbb7f098b07caac3c4e4fc3da7f039ff22470a81d8cf5087ca50943594efc38c`.

O candidato final desta etapa é `candidate_complete`, SHA256
`f092df42a588bf9b43943006d2a77b4e0e30ef9fc3d1384aeaaceecd9fe3d4f1`,
source ID `4d644f551ec9587db8f66bedbf23c9279a876d542141a58d02faa8803d44b2c7`,
contrato numérico 4. O ado mantém o SHA256 público
`e88702154846b7a2d4bd90790a815b64fe3504dc2a469ae28c8c7b87b216a80d`.
As diferenças medidas não resultam de uma reescrita do frontend Stata.

O candidato foi compilado em Release com GCC 12.2.1, como a release Linux.
O ambiente de compilação continua a diferir: AlmaLinux 9.7 neste host versus
AlmaLinux 8 na release. Compilador e opções comparáveis não tornam idênticos
todos os componentes do toolchain. Ambos os binários executam no mesmo host,
com 48 CPUs disponíveis. O build local 1.5.6 serve apenas para validar o código
partilhado; não substitui a referência pública nos benchmarks.

Toda a evidência desta etapa está no arquivo local
`audit_repro/public_surface_20261023/`, não distribuído com a release.
Os dados são sintéticos ou públicos. Não houve commit, push, publicação,
instalação global ou alteração de dados de investigação. O motor por omissão
do projeto continua a ser 1.5.6; 2.0.0 é o candidato de revisão isolado.

## Contrato SQL escolhido pelo mantenedor

Foi expressamente escolhido **SQL nativo do DuckDB 2.0 para `sql/query`**, com
as incompatibilidades documentadas. Isto não altera as expressões Stata de
`gen`, `egen`, `replace` e qualificadores. A ajuda, changelog, pressupostos
e testes de migração foram atualizados em conjunto.

| Expressão SQL direta | Público 1.5.6 | Nativo 2.0 | Forma explícita/migração |
|---|---|---|---|
| `2*3!` | 720 | 12 | `2*(3!)` para 12; `(2*3)!` para 720 |
| `'abc' ~ 'b'` | falso | verdadeiro | `regexp_full_match()` quando se pretende correspondência integral |
| `X'41'`, `B'0101'` | texto | literais binário/bit | Usar texto entre aspas quando o objetivo é texto |
| `NULL` sem tipo | coluna missing | tipo SQLNULL sem representação direta | `NULL::DOUBLE`, por exemplo |
| Inteiro além de UHUGEINT | inferência DOUBLE | BIGNUM exato no motor | Conversão explícita para texto quando se pretende carregar os dígitos em Stata |
| Lambda `x -> x+1` | aceite | recusada por omissão | `lambda x : x+1` |
| Divisão inteira/modulo por zero | NULL | erro | `TRY(...)` quando NULL é deliberadamente pretendido |

Um tipo sem representação é retirado **com uma mensagem**; um resultado sem
qualquer coluna carregável é recusado. O log confirma o aviso para a coluna
SQLNULL. Uma recusa não apaga o dataset atual. O teste inclui `query`, a
precedência com parênteses, a sintaxe moderna de lambda e a manutenção de
missing na aritmética dos comandos Stata.

## Percurso funcional e oráculos

O inventário da distribuição pública contém 57 comandos, 98 funções/literais
de expressão e 39 nomes egen. A presença de uma palavra num teste foi usada
apenas para descoberta de cobertura, nunca como prova de execução.
O mapa de cobertura local, `coverage_ledger.md`, nesse arquivo
liga famílias de comandos, opções e contratos aos testes executados.

O candidato final passou **190 alvos Stata, com 236 verificações PASS**, e
**CTest 6/6**. A unidade C++ passou 254 casos e 468 755 asserções; o caso de
concorrência separado passou mais sete asserções. O código partilhado também
passou CTest 6/6 com 1.5.6. O código de saída do Stata nunca foi usado como
certificação: os runners exigem os marcadores finais e analisam os abortos.
O reforço final do teste de binding, incluindo bins fracionários, passou
separadamente 56 asserções; a recusa de `0.4` sem conversão explícita é uma
regra de tipos do próprio motor, não uma falha do plugin.

Foram acrescentados testes permanentes para cinco aliases de expressão,
todos os 99 percentis de grupo e todos os 99 percentis por linha, e o alias
egen `sum()`. Os casos incluem domínios inválidos, valores em falta, empates,
grupos inteiramente missing e fronteiras entre blocos. O Stata nativo é o
oráculo onde a sua aritmética é adequada; Fraction/Decimal e PyArrow verificam
os casos em que uma simples concordância entre dois percursos seria insuficiente.

As verificações adicionais de ordem repetem três vezes, em ambos os binários,
recolha de dez milhões de linhas, filtros posicionais, intervalos, append,
globs, pontes de memória, sequências e CSV, sem ordenar posteriormente para
esconder uma permutação. `duplicates drop` com payload diferente exige uma
ordem declarada: o teste confirma a recusa e declara a ordem antes da operação.

Com um limite deliberado de 8 MB, as falhas reais de memória preservaram tanto
o dataset sentinela como o hash de um ficheiro já existente. A sessão recuperou
e executou uma consulta seguinte. Os testes de concorrência preservam também
a propriedade exclusiva dos outputs e a separação das pontes temporárias.

## Defeitos públicos confirmados e corrigidos

A versão pública passou 231 das primeiras 232 verificações; a falha inicial
foi o novo caso de chaves Hive textuais `null` com variantes de maiúsculas.
A reprodução independente mostrou escrita com rc 0 seguida de leitura como
missing. O candidato recusa essa escrita antes de perder a informação. Os
restantes símbolos, separadores, percent escapes, Unicode e missing legítimos
têm testes próprios de round-trip.

A exploração dos casos degenerados encontrou mais dois erros no cálculo
estatístico público:

- `sebinomial` verificava a média, embora todos os valores não missing devam
  ser 0 ou 1. Por exemplo, `[-1,0,1]` produzia zero em vez de missing.
- `sepoisson` não recusava todos os valores negativos e omitia o arredondamento
  do total. Para `[0.5,0.5,0.5]`, produzia aproximadamente 0,408248; o cálculo
  com o total arredondado é `sqrt(2)/3`, aproximadamente 0,471405.

O [manual do Stata](https://www.stata.com/manuals/dcollapse.pdf) apresenta as
fórmulas gerais. As condições de domínio e os casos de arredondamento foram
estabelecidos por execução nativa, preservada nos probes e em V171.

Os novos acumuladores verificam o domínio por grupo e amostra. O total de
Poisson é arredondado na sua representação inteira exata; a raiz racional
final usa o mesmo núcleo certificado dos momentos. Não existe passagem do
total por DOUBLE antes do arredondamento. Os estados e custos dos restantes
totais/momentos mantêm-se.

Aplica-se a política numérica já autorizada em ASSUMPTIONS 134: não reproduzir
erros aritméticos do próprio Stata. Assim, um valor exatamente abaixo de 0,5
não se transforma num empate falso, e um erro-padrão finito não passa a missing
por overflow de uma soma intermédia. V171 certifica cada valor recolhido e
gravado por comparação de frações com os pontos médios dos doubles vizinhos,
incluindo a regra de empate. A compatibilidade nativa ordinária e estas
diferenças de maior precisão são verificadas separadamente.

## Alterações da API que explicavam custos evitáveis

O [estudo anterior](ESTUDO_DUCKDB_1_5_6_2_0_0_2026-10-23.md) e a
[correção de execução preparada](IMPLEMENTACAO_PARALELISMO_DUCKDB_2_0_0_2026-10-23.md)
reproduziram o plano `EXECUTE` que ocultava a análise de ordem/paralelismo.
Essa correção mantém a propriedade do plano original e usa o percurso streaming
real; o teste distingue produtores simultâneos de uma tarefa que apenas muda
de thread. `ORDER BY` explícito continua obrigatório mesmo quando a preservação
de ordem de inserção está desligada.

Nesta etapa, a medição de toda a superfície revelou dois custos adicionais:

1. **Histograma.** O registo C API 2.0 marcava o callback como suscetível de erro,
   impedindo a reutilização dos valores do dicionário. A experiência controlada
   passou de 10 milhões de valores processados para 1 100 960, com resultados
   iguais. A prova de produção exige famílias/escalas compatíveis, limites
   literais válidos e contagem admissível. Só avalia constantes e conversões
   primitivas nativas sem estado, cuja implementação é conferida. Limites
   dinâmicos, parâmetros, conversões inválidas e expressões falíveis mantêm
   o tratamento de erros. Cancelamentos e erros internos propagam-se.
   Os 40 intervalos foram verificados independentemente; o controlo PLAIN,
   sem dicionários, não reduz o número de valores processados.
2. **Cardinalidade por linha.** Em 1.5.6, `DataChunk::size()` devolvia um campo;
   em 2.0 pode derivar o tamanho dos vetores. Oito loops de callbacks repetiam
   essa chamada em cada linha. A cópia experimental que a faz uma vez por
   bloco imutável manteve os resultados e reduziu a mediana de uma consulta
   de correlação de 0,436382 para 0,388472 s. A correção conserva exatamente
   a gama de iteração e a aritmética.

A hipótese de cópia nos acessos mutáveis aos vetores foi explicitamente
refutada por inspeção e reprodução de identidade de ponteiros; não motivou
uma alteração no plugin. Continua a ser usada a C API v1, com as adaptações
concretas acima. Há agora 40 ficheiros upstream verificados: 21 sem alteração,
18 correções locais e uma reposição da inferência literal nativa 2.0.

## Comparação final de desempenho

Foram concluídos **78 cenários em 112 processos Stata novos**, com 2 576 000 000
células gravadas verificadas independentemente. A matriz cobre manipulação e
estatísticas a um/dez milhões de linhas, formatos externos a 100 mil linhas,
configurações por omissão e ordenação com uso efetivo de disco temporário.
Os recibos PASS certificam execução e resultados; não significam, por si só,
ausência de custos de desempenho.

Os processos são intercalados em blocos ABBA/BAAB, com duas execuções de cada
versão por bloco. Há cinco blocos nas comparações principais e três nos
restantes grupos; os scripts conservam as repetições internas e os controlos
da mesma versão. As medições decorreram sem outras compilações/testes nossos.
O host é partilhado e não foram alterados governor ou caches globais.
Os inputs estavam quentes em cache; não se apresenta isto como um ensaio de
disco frio. Foram usados quatro threads/1 GB, salvo os perfis explicitamente
identificados como configuração por omissão, 48 threads ou 128 MB.

As colunas A/B abaixo são medianas por versão. Delta e percentagem são medianas
emparelhadas por bloco, pelo que não têm de coincidir com a diferença/quociente
das medianas A/B. Valores negativos indicam um candidato mais rápido.

| Cenário | Público A (s) | Candidato B (s) | Delta emparelhado | Variação emparelhada |
|---|---:|---:|---:|---:|
| collect, 10M, 4 threads | 0,31625 | 0,26750 | −53,25 ms | −16,55% |
| collapse, 10M, 4 threads | 1,99975 | 1,39800 | −589,00 ms | −29,47% |
| strings + sort + posição, 10M, 4 threads | 3,61300 | 3,76225 | +111,50 ms | +3,05% |
| collect, 10M, por omissão | 0,13150 | 0,10875 | −28,25 ms | −21,47% |
| collapse, 10M, por omissão | 0,99075 | 0,60350 | −355,00 ms | −36,78% |
| save, 10M, por omissão | 0,21575 | 0,16200 | −49,75 ms | −22,54% |
| strings + sort + posição, 10M, por omissão | 1,01200 | 1,03250 | +18,25 ms | +1,80% |
| histograma, 10M, 4 threads | 0,21350 | 0,17017 | −40,50 ms | −19,27% |
| correlate, 10M, 4 threads | 0,91700 | 0,86350 | −54,83 ms | −6,00% |
| pwcorr, 10M, 4 threads | 0,87167 | 0,84883 | −21,17 ms | −2,42% |
| histograma, 10M, 48 threads | 0,11200 | 0,08917 | −21,33 ms | −19,10% |
| correlate, 10M, 48 threads | 0,26717 | 0,25950 | −5,83 ms | −2,20% |
| pwcorr, 10M, 48 threads | 0,25317 | 0,25200 | +0,17 ms | +0,05% |
| tabstat, 1M, 4 threads | 0,81200 | 0,61125 | −209,25 ms | −24,76% |
| escrita particionada, 1M, 4 threads | 0,31825 | 0,20175 | −115,00 ms | −35,67% |

As perdas anteriores de histogramas e correlações foram removidas na escala
maior. Pequenos deltas, como os 0,17 ms de pwcorr, não são transformados em
reprovação por uma percentagem isolada. Também se conservam os custos pequenos
de alguns casos de um milhão de linhas e a ressalva de strings; não se usa
uma média global para os esconder.

A tabela integral local `performance_complete.md`,
o CSV `performance_complete.csv` e o
resumo estruturado `performance_complete_summary.json`, nesse arquivo,
incluem todos os casos, amplitudes entre blocos, controlos e memória do processo
inteiro. Esta memória não deve ser interpretada como pico de um único operador.
Também não diminui em todos os perfis: a mediana RSS dos processos principais
de 10M/4 threads caiu de 2 109,4 para 1 629,6 MiB, mas a das sessões com formatos
externos de 100 mil linhas subiu de 312,7 para 335,3 MiB. Esse acréscimo de
22,6 MiB está registado; a matriz não demonstra uma garantia universal de menor
RAM, nem separa nesse total bibliotecas, Stata e estruturas de cada conversor.

A figura das diferenças absolutas de tempo está no recibo local
`audit_repro/public_surface_20261023/performance_absolute.png`.

### Ressalva: igual limite nominal não implica igual consumo real de RAM

Com quatro threads e **128 MB em ambos**, a ordenação/gravação de dez milhões
de linhas passou de 1,8195 para 2,1780 s: delta emparelhado +0,351 s (+19,87%).
Isto é um custo persistente neste perfil, não é apagado como ruído. Ao mesmo
tempo, o RSS mediano caiu de aproximadamente 320,0 para 259,8 MiB e o pico
observado de ficheiros temporários subiu de 213,3 para 265,8 MiB.

A experiência seguinte retirou completamente o Stata e o plugin, usando
motores recompilados com o mesmo GCC 11/RelWithDebInfo. É uma experiência
causal, **não** uma substituição da referência pública na tabela principal.

| Motor nativo e configuração | Mediana (s) | RSS mediano (MiB) | Escrita reportada pelo SO (bytes) |
|---|---:|---:|---:|
| 1.5.6, 128 MB | 1,45344 | 280,11 | 180 490 240 |
| 2.0 corrigido, 128 MB | 1,74629 | 215,31 | 281 317 376 |
| 2.0 upstream, 128 MB | 1,70433 | 215,93 | 244 568 064 |
| 2.0 upstream, I/O assíncrono desligado | 1,78394 | 197,58 | 295 129 088 |
| 2.0 upstream, read-ahead desligado | 1,79832 | 206,20 | 290 639 872 |

O custo reproduz-se no 2.0 upstream e não desaparece ao desligar os dois
mecanismos assíncronos. Os contadores foram recolhidos em `/proc/<pid>/io`
enquanto cada processo permanecia vivo após a consulta, e os outputs completos
foram verificados. Não houve leitura física reportada neste ensaio de cache
quente. A quantidade de escrita e o consumo de RAM diferem efetivamente;
estatísticas internas de versões diferentes não são assumidas equivalentes.

Na calibração de RAM, 2.0 upstream com 192 MB fez 1,3717 s/279,08 MiB de RSS,
contra 1.5.6 com 128 MB a 1,5445 s/285,24 MiB. A confirmação com os **plugins
exatos** voltou a intercalar três blocos e verificou mais 480 milhões de células:

| parqit | Limite configurado | Mediana (s) | RSS mediano (MiB) |
|---|---:|---:|---:|
| Público v0.5.1 | 128 MB | 1,8660 | 328,62 |
| Candidato 2.0 | 192 MB | 1,7365 | 322,57 |

O delta emparelhado foi −0,1285 s (−6,83%). **Esta última comparação usa limites
configurados diferentes**, escolhidos para aproximar a RAM efetivamente usada;
não invalida o resultado mais lento a 128/128 MB. Também não é uma regra geral
de conversão de limites. O plugin não aumenta silenciosamente `memory_limit`.
Um workflow com orçamento muito pequeno deve calibrar explicitamente esse
limite e o uso de disco no seu host, ou aceitar o custo medido com menos RAM.

Recibos: `spill_benchmarks_02`, `native_spill_attribution_02`,
`native_spill_memory_match_01` e `spill_matched_memory_01`, todos no diretório
de evidência. O ficheiro inicial da atribuição nativa preserva uma recusa de
configuração do profiler, corrigida no harness antes das medições aceites.

## Debate adversarial e limites

O Claude Fable 5.1 max recebeu código e resultados sintéticos sob autorização
expressa, em modo sem ferramentas nem escrita. O debate identificou o risco
de duplo arredondamento e o catch demasiado amplo, ambos resolvidos. As
objeções baseadas em hipóteses incorretas sobre `Number`, casts ou o algoritmo
do histograma foram confrontadas com código e reproduções. O
registo de adjudicação local `advisor_resolution.md`, nesse arquivo,
separa sugestões, defeitos confirmados, hipóteses refutadas e aceitação pelo root.

Não se infere ausência universal de erros ou aceleração de todas as combinações
possíveis. A validação nativa Windows/macOS e novos cliques em GUI são NOT_RUN;
os diálogos têm validação estática e de comandos. A investigação DRD anterior
permanece REVIEW, e ASan/TSan não são aqui declarados aprovados. Não foram
alteradas caches globais, definições de terceiros ou a instalação habitual.
As cópias anteriores e os manifests permitem recuperar os ficheiros alterados
individualmente, sem reset ou limpeza global da árvore de trabalho.
