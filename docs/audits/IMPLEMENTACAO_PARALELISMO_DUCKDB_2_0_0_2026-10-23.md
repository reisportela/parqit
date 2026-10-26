# Implementação decorrente do estudo DuckDB 1.5.6 → 2.0.0

Branch: `review/duckdb-2.0.0-lts-20261022`.
Estudo: [contratos, experiências e debate com Claude](ESTUDO_DUCKDB_1_5_6_2_0_0_2026-10-23.md).
Evidência: `audit_repro/duckdb_200_deep_perf_20261023/`.

## Alteração aplicada

O parqit usa o C API preparado para recolher dados em streaming. No DuckDB 2.0,
esse percurso acrescenta um PhysicalExecute que oculta o plano real à análise
de fontes/ordem. O resultado podia ser uma recolha serial apesar de threads=4;
com preservação de insertion order desligada, podia também perder ORDER BY.

`cmake/DuckDB200Prepared.cmake` corrige apenas a seleção da recolha. Retira os
invólucros EXECUTE transparentes para analisar ordem e suporte de índices de
blocos. O sink continua construído com o plano e os dados originais. Não altera
a travessia global, a propriedade dos chunks, as funções numéricas ou a gramática.

O ficheiro corrigido e oito dependências da prova são protegidos por hashes.
A lista completa de 2.0 passa a 33 fontes: 19 com correções e 14 verificadas sem
edição. Mantêm-se os guards de LF/CRLF, recusa de alterações desconhecidas e
preservação de staging preexistente.

O código de transporte e a API v1 foram conservados. Os comentários da sessão
passaram a distinguir os lifetimes e o momento de execução de 1.5.6 e 2.0.
O default continua 1.5.6; não houve instalação global, commit, push ou publicação.

## Teste discriminante, antes e depois

O novo teste de regressão lê um milhão de linhas Parquet com vários grupos,
conta callbacks simultâneos e verifica todos os valores. IDs de threads distintos
não bastam: uma única tarefa serial pode migrar entre threads depois de esperar
por espaço no buffer. O teste mede o pico de callbacks ativos, respeitando o
orçamento configurado.

Também executa três vezes ORDER BY descendente com insertion order desligada.
As verificações de ordem comparam todos os valores com a sequência esperada.

- Antes da correção: quatro falhas — pico de um único produtor e três resultados
  fora da ordem pedida. `negative_gate_test_02.log`.
- Depois: 33 verificações passaram. `prepared_fix_targeted_01.log`.

## Validação integrada

Cada motor foi reconstruído e congelado numa árvore separada. O runner de Stata
usa TMPDIR/STATATMP próprios de cada teste, através de um wrapper local que não
altera a instalação Stata. Em ambos os motores passaram CTest 6/6 e a suite
completa de 188 alvos Stata, com 232 veredictos PASS e zero falhas.

| Motor | Source ID | SHA-256 do plugin congelado |
|---|---|---|
| 1.5.6 | `d202e34d818050343c2796470d3e9d082ce2f557faca6aec87ba0d62783662b2` | `07dc565f30e1e1bc6162e786d7cb851cc25deccc9c29f523c42efb55d03cf5c1` |
| 2.0.0 corrigido | `79573e544ee276e1f791daa19e1164b161d2a2c599715725b15e2021c92347a4` | `a5c844fd1bcae892f974e66d709c1c1b4f2b131ac2b19cb2f926ac44a993ee54` |

O plugin continua a exportar apenas `pginit` e `stata_call`.
Recibos, manifestos e logs completos: `implementation_validation_01/`.

O estudo precedente conserva evidência adicional de contratos, propriedade e
memória, incluindo o controlo independente do motor original. Memcheck passou;
DRD permanece REVIEW e não é apresentado como um teste limpo de corridas. ASan,
TSan do motor completo e execução nativa Windows/macOS não foram realizados.

## Medição do parqit completo

Foi executada uma comparação intercalada com três plugins congelados:
L=1.5.6, P=2.0 antes desta correção e F=2.0 corrigido. A matriz menor cobre os
13 fluxos anteriores com um milhão de linhas; a maior cobre collect, filtro,
strings, collapse e save com dez milhões de linhas.

Passaram as assinaturas Stata, asserções analíticas e os Parquet gravados em todos
os braços. PyArrow verificou independentemente 828 milhões de células gravadas.
São 36 processos Stata novos, 18 por escala, com três blocos palindrómicos
equilibrados. Cada processo repete cada fluxo três vezes na escala menor e duas
na maior. Query/fill/Stata processors=4/4/4, memória DuckDB=1 GB e uma leitura
completa de aquecimento por processo. Não correram compilações ou outras suites
durante estas medições.

Os tempos nas tabelas são medianas por braço, em segundos. O delta emparelhado
é a mediana das diferenças entre médias aritméticas dos dois processos de cada
braço em cada bloco. O rácio emparelhado usa médias geométricas por bloco e a
mediana desses rácios. Por isso, o rácio/delta emparelhado não tem de coincidir
com o quociente/diferença das medianas das primeiras colunas.

### Um milhão de linhas

| Fluxo | L: 1.5.6 (s) | P: 2.0 antes (s) | F: 2.0 corrigido (s) | Delta F−L emparelhado |
|---|---:|---:|---:|---:|
| collect | 0,08117 | 0,11217 | 0,08433 | +3,17 ms |
| filtro | 0,02200 | 0,02933 | 0,02900 | +5,50 ms |
| strings | 0,46317 | 0,49283 | 0,46133 | −0,33 ms |
| clock | 0,19450 | 0,16283 | 0,14300 | −50,17 ms |
| collapse | 0,30350 | 0,26467 | 0,26433 | −37,50 ms |
| egen fundidos | 0,33133 | 0,35500 | 0,32667 | −7,00 ms |
| merge | 0,21300 | 0,22567 | 0,20167 | −8,17 ms |
| rename | 0,18383 | 0,19933 | 0,17083 | −11,17 ms |
| save | 0,06733 | 0,06983 | 0,06583 | +1,33 ms |
| windows | 0,47267 | 0,48817 | 0,46733 | −1,83 ms |
| CSV | 0,59733 | 0,61583 | 0,56817 | −39,00 ms |
| CSV com conversões | 0,70300 | 0,72550 | 0,63917 | −52,67 ms |
| páginas com nulos | 0,11667 | 0,14267 | 0,12633 | +6,00 ms |

A perda anterior de aproximadamente 31 ms em collect caiu para uma diferença
pequena relativamente à referência. O custo fixo do filtro permanece; não se
usa a sua percentagem isolada como reprovação. Strings ficou praticamente
equivalente nesta escala, e outros fluxos ganharam tempo.

### Dez milhões de linhas

| Fluxo | L: 1.5.6 (s) | P: 2.0 antes (s) | F: 2.0 corrigido (s) | Delta F−L emparelhado | Rácio F/L emparelhado |
|---|---:|---:|---:|---:|---:|
| collect | 0,32875 | 0,42850 | 0,26625 | −76,75 ms | 0,7645 |
| filtro de 1% | 0,04675 | 0,04575 | 0,04025 | −11,25 ms | 0,7805 |
| strings + sort + posição | 3,61025 | 4,16150 | 3,75775 | +150,00 ms | 1,0416 |
| collapse com cinco estatísticas | 1,93225 | 1,48775 | 1,48975 | −448,00 ms | 0,7677 |
| save de metade das linhas | 0,26900 | 0,25325 | 0,26625 | −2,50 ms | 0,9904 |

Collect e collapse foram mais rápidos que 1.5.6 nos três blocos. Os rácios por
bloco de collect foram 0,7645/0,8003/0,7504; os de collapse,
0,7677/0,7618/0,7732. O ganho de collapse já estava presente no motor 2.0 antes
desta correção e não lhe é atribuído. No collect, F/P é 0,6016 emparelhado:
a correção remove uma parte substancial do custo do percurso preparado.

Strings é a ressalva material desta matriz: o delta F−L foi +0,150/+0,252/−0,046 s
nos três blocos. A correção melhora consistentemente o 2.0 anterior, mas não
estabelece uma vantagem sobre 1.5.6 nesse fluxo. Não se apaga essa observação
numa média de todos os comandos, nem se afirma que todas as funções aceleram.

O filtro e save variam bastante nos controlos da mesma versão; os seus rácios
devem ser lidos juntamente com os poucos milissegundos em causa. Os controlos
e todas as medições individuais estão no recibo, sem seleção de mínimos.

### Memória e alcance da conclusão

Medianas do pico RSS do processo Stata completo, em KiB:

| Escala | L: 1.5.6 | P: 2.0 antes | F: 2.0 corrigido |
|---|---:|---:|---:|
| 1 milhão | 473 648 | 513 556 | 471 180 |
| 10 milhões | 2 198 090 | 1 908 318 | 1 686 818 |

Na escala maior, F usa aproximadamente 499 MiB menos que L neste conjunto de
fluxos. É uma medição do processo inteiro e do pico de uma sessão com vários
comandos, não um valor isolado por operador. O novo modo de recolha também muda
a disponibilização dos blocos: o buffer batch-ordered pode entregar uma unidade
observável sem esperar pela saturação do buffer simples. Isso é compatível com
mais sobreposição e menor retenção; não se atribui todo o RSS a um único efeito.

A carga da máquina passou de 4,82/5,73/5,53 para 10,35/8,52/6,95. Trata-se de
um servidor partilhado; não foi mudado o governor ou a configuração de terceiros.
Os resultados sustentam ganhos concretos nesta matriz e identificam os custos
residuais. Não são uma garantia universal sobre volume, largura, cardinalidades,
strings, discos ou hardware diferentes.

Recibos: `package_benchmarks_01/receipt.json`, `measurements.json`, comandos,
logs de cada processo e ficheiros gravados para o oráculo independente.

O critério do utilizador admite perdas muito pequenas em troca de ganhos
substanciais. Uma percentagem isolada sobre poucos milissegundos não será usada
como reprovação da migração; também não será usada para inflacionar um ganho.

## Limites e recuperação

O resultado não muda a política de SQL direto nem constitui uma release.
Compatibilidade de gramática/tipos e validação nativa noutras plataformas são
decisões/gates separados da correção de paralelismo.

As cópias anteriores estão em `before_implementation/`. O patch isolado é
`implementation.patch`; a versão do estudo fechada antes da implementação está
em `study_report_at_gate.md`, com hash em `study_gate_receipt.json`. Os plugins
anteriores e os relatórios históricos permanecem preservados.
