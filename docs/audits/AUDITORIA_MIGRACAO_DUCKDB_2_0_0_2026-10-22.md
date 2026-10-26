# Revisão adversarial da migração DuckDB 1.5.6 → 2.0.0 LTS

**Data:** 22 de outubro de 2026. **Decisão: NO-GO.**
**Branch local:** `review/duckdb-2.0.0-lts-20261022`.
**Base:** `aa9f2dbaa7` / parqit 0.5.1. **Motor por omissão: 1.5.6.**

O candidato 2.0.0 compila, mas não preserva o contrato do parqit. Foram
reproduzidas perdas de proteção numérica, alteração de linhas e valores em
Parquet e incompatibilidades funcionais. Os resultados positivos de grande
parte da suite não autorizam a substituição. Não foi feita instalação global,
publicação, commit ou push. A evidência persistente está no arquivo local
`audit_repro/duckdb_200_review_20261022/`, não distribuído com a release.

## Identificação, custódia e método

O estado inicial tinha dois ficheiros versionados alterados, `.gitignore` e
`docs/audits/README.md`, e muitos ficheiros não versionados. Foram guardados
447 hashes de ficheiros versionados, o inventário de 10 855 ficheiros não
versionados e cópias dos dois ficheiros alterados. Estes dois ficheiros não
fazem parte das alterações da revisão. O manifesto é
`custody/initial_manifest.json`; a verificação final fica em
`custody/final_custody_check.json`.

Foram usados o código oficial final e o mecanismo SHA256 do CMake, não o CLI
alpha anteriormente instalado neste servidor. A identidade oficial é
[tag v2.0.0](https://github.com/duckdb/duckdb/releases/tag/v2.0.0), commit
`d54cbbe3df17e8e5ce68312f483b4155836112aa`. O SHA256 do arquivo é
`7e89053255046bfb26afbd7f524a2fb84df7dbef496aa911ea35e4a1182fdc00`.
O [guia oficial](https://duckdb.org/docs/2.0/operations_manual/migration)
descreve alterações de sintaxe, configuração, API e formato de base de dados;
estas não constituem autorização para perder precisão no parqit.

Os dois builds usam o mesmo host Linux e `RelWithDebInfo`. A referência é
`build/duck156`; o candidato é `build/duck200_review_20261022`, com árvore ado
própria. As suites recebem explicitamente o caminho do plugin. Os logs do
runner foram copiados de `/tmp` para a pasta persistente da revisão. Todos os
novos dados são sintéticos. Não foram usados microdados nem subagentes.

| Binário | SHA256 |
|---|---|
| Referência, plugin usado pela suite completa | `eb3cc104962576a6905bb6cec16ea050a95ef5778917c24f6cb55bfcb6db2627` |
| Referência, cópia stripped preservada | `e71de3d7e4bb0ce2bb0e9d63291b68c767b8f610e6d0c208ae1215aa3020896e` |
| Candidato, plugin usado pela suite completa | `1464d7b04bff2718f133f45647cb32bbf981bbe090c247ea2f64feeb0ec6fd9c` |
| Candidato, superfície ado stripped | `62b393478b6102d03a6936f7a8c281d8b6ba0b65cb7b77118f66b2e40a148ff4` |

O source ID do candidato é
`fd2c8be321ecc2f3f3cf2529d159005dab07090388797006371b7136bc84479c`.
`binary_identities.json` distingue os binários raw e stripped; estes não são
intercambiados nas afirmações sobre os testes.

Em processos Stata novos, a resolução normal pela árvore ado staged, sem
`PARQIT_PLUGIN_PATH`, carregou o motor esperado e passou `SELECT 1` nos dois
casos. O selftest passou na referência e devolveu o `r(920)` já identificado
no candidato. Evidência: `loader_baseline/run.log` e `loader_candidate/run.log`.

## Adaptações necessárias para tornar o candidato executável

- Seleção explícita de apenas dois motores com arquivos fixos; 1.5.6 continua
  por omissão. O identificador 2.0.0 e o commit do motor são fornecidos segundo
  o novo mecanismo de versão, evitando que o motor herde o Git do parqit.
- Registo explícito de `core_functions` e `parquet` através do helper 2.0.
  A mudança da ligação estática também está descrita na
  [issue 27644](https://github.com/duckdb/duckdb/issues/27644).
- Desativação de autoload/autoinstall na configuração antes de abrir o motor:
  os antigos switches de compilação deixaram de ter efeito no 2.0.
- Preservação da sintaxe antiga de lambdas no SQL do utilizador e da regra
  anterior de divisão inteira/módulo por zero. O SQL gerado e o selftest
  passam a usar `lambda x : ...`, também aceite no 1.5.6.
- Retenção das correções de amostragem, destruição do alocador e duplicação
  de subárvores agregadas na transformação de janelas. As outras correções
  anteriores têm implementações a montante verificadas pelo código e hashes.
  Oito ficheiros estão fixados em `cmake/DuckDB200Sources.cmake`.

Os 21 cenários do novo teste dos patches passaram: originais, reaplicação,
LF/CRLF, oito alterações desconhecidas, oito misturas de finais de linha e
três ficheiros de staging preexistentes. Esta verificação prova a disciplina
dos patches, não a correção de todo o motor. Não foi adotada a nova C API v2;
a compatibilidade C API v1 e o handle C++ dos callbacks temporais foram
inspecionados no código final do 2.0.0.

## Resultados executados

| Verificação | Referência 1.5.6 | Candidato 2.0.0 |
|---|---|---|
| Compilação do plugin e testes | Build existente identificado | PASS, compilado de fonte |
| CTest completo | 6/6 PASS | 3/6 PASS; runtime e unit FAIL; encerramento TIMEOUT |
| C++ | 239 casos, 435 455 asserções PASS | 234/239 casos PASS; cinco casos falham, 23 asserções falham |
| Suite Stata completa | 186 alvos, 230 veredictos PASS | 187 alvos, 215 veredictos PASS, cinco FAIL e 11 marcadores de aborto/inconclusão |
| V168, compatibilidade SQL/configuração/row functions | PASS, execução adicional | PASS, incluído nos 187 alvos |
| V169, strings PLAIN e missing | PASS, execução adicional | FAIL, execução adicional |
| Lint de distribuição e dez diálogos | PASS | Superfície de código/ados PASS; não certifica o binário |
| Matriz comparativa de desempenho | NOT_RUN | NOT_RUN: verificação de correção reprovada |
| Runtime Stata Windows/macOS e CPUs antigas | NOT_RUN | NOT_RUN |

Os veredictos não são contagens de alvos: alguns testes têm vários veredictos,
e um PASS anterior pode ser seguido de aborto. Os 11 marcadores não são
somados aos cinco FAIL como se fossem 16 defeitos independentes. A suite
completa do candidato tem 15 alvos com falha/aborto; o V169 acrescenta um
alvo reprovado. Os dois novos testes foram executados também na referência.

No Valgrind, as seis asserções do teste de encerramento chegaram a PASS,
mas o processo não terminou no limite explícito de 180 segundos. O resultado
é TIMEOUT, não PASS nem prova de fuga de memória. Na referência o processo
terminou e passou em 44,29 segundos. Logs: `baseline_ctest.log`,
`baseline_ctest_detailed.log`, `candidate_ctest_01.log`, `baseline_stata/`,
`candidate_stata/`, `baseline_migration/`, `baseline_plain_null/` e
`candidate_plain_null/`.

## Falhas de dados e precisão que impedem a substituição

### D200-01 — Proteção `int64()` anulada pela otimização de estatísticas

**FAIL, bloqueante.** V124 reproduz 24 falhas nas combinações eager/lazy e
`int64(refuse|string|round)`, com BIGINT positivo/negativo, UBIGINT e DECIMAL.
Com rodapés que anunciam falsamente extremos dentro de 2^53, o candidato
aceita a leitura com `rc=0` em casos que deviam ser recusados ou convertidos
para texto exato. A opção `round` também deixa de anunciar a conversão.

O teste C++ mostra a distinção: a agregação lê o máximo real, mas o predicado
de proteção devolve falso. A presença de `first(1)`, que protegia a consulta
no 1.5.6, já não protege a comparação contra a propagação de estatísticas.
Não é aceitável retirar este controlo ou passar a confiar no rodapé.
Evidência: `candidate_stata/v124_footer_precision.log` e teste C++
`precision extrema fence reads payload despite dishonest exact footer statistics`.

### D200-02 — `missing()` altera a amostra e o conteúdo de strings PLAIN

**FAIL, bloqueante.** Um Parquet válido contém três identificadores e uma
string com dois NULL e um valor não vazio. `parqit keep if missing(c)` deve
reter duas linhas. O 1.5.6 retém duas; o 2.0.0 conta, recolhe e grava três.
A string não vazia da linha indevidamente retida é substituída por missing.
O SQL direto `WHERE c IS NULL` reproduz a mesma falha.

Os controlos com dictionary encoding e sem estatísticas passam nos dois
motores. PyArrow verifica os ficheiros de origem e os resultados gravados;
a soma dos identificadores passa indevidamente de 3 para 6 no candidato.
As flags de exatidão do rodapé são verdadeiras e foram verificadas por
descodificação compact-Thrift, sem alterar as páginas de dados.
Evidência: `upstream_baseline/`, `upstream_candidate/`,
`plain_null_saved_oracle.json`, V169. A
[issue 27456](https://github.com/duckdb/duckdb/issues/27456) descreve o mesmo
problema; a conclusão aqui assenta na reprodução com o plugin final local.

### D200-03 — CSV booleano deixa de carregar

**FAIL, bloqueante funcional.** V107 falha num CSV com `TRUE`: o candidato
tenta ler o campo como TINYINT e devolve `r(920)`. A referência carrega o
mesmo fixture. Isto afeta a interação entre inferência, conversão e scan;
não se resolve exigindo ao utilizador opções diferentes para o mesmo ficheiro.
Evidência: `candidate_stata/v107_csv_options.log`.

### D200-04 — Resolução de caminhos pode selecionar outro ficheiro

**FAIL, bloqueante de identidade da origem.** V137 usa um caminho com um link
seguido de `..`. O 1.5.6 segue a resolução do sistema; o 2.0.0 lê outro ramo.
No fixture sintético, espera-se Москва e obtém-se Омск, tanto no caminho
UTF-8 como na leitura com transcodificação. Não é uma mera diferença na
forma de apresentar o caminho. Evidência: `candidate_stata/v137_encoding_csv.log`, F18.

### D200-05 — Coluna inteiramente nula desaparece

**FAIL funcional.** V11 deixa de encontrar `nul`: o 2.0.0 expõe a coluna
Parquet como tipo NULL e o adaptador elimina-a com aviso. No 1.5.6 o mesmo
ficheiro mantém a variável byte toda missing. Não houve perda silenciosa de
valores não nulos, mas há perda de uma variável que o contrato preservava.
Uma correção tem de distinguir este caso de um resultado SQL genuinamente
sem tipo; não basta alterar indiscriminadamente a política de tipos.

### D200-06 — O selftest e SQL com partições voláteis deixam de funcionar

**FAIL funcional.** O binder 2.0.0 recusa expressões voláteis em `PARTITION BY`.
O selftest numérico recusa consequentemente o candidato (`r(920)`), afetando
M0, tours, cursos, um comando do diálogo, abreviaturas e V163. V161 também
reproduz a recusa no SQL direto. Não foram retiradas as asserções, convertidos
estes casos em PASS ou declarado que o contrato numérico 4 está satisfeito
apenas porque o plugin ainda anuncia esse número.

### D200-07 — Resultados errados adicionais do núcleo SQL embutido

**FAIL numérico do motor; exposição distinta dos verbos estatísticos próprios.**
O programa `capi_diagnostics.cpp` liga as mesmas bibliotecas usadas pelos
testes de cada build. Em tabelas sintéticas num motor em memória:

- `sum(v)` é 1, mas `sum(v)>0` devolve falso no 2.0.0 após um LEFT JOIN com
  lado direito vazio; no 1.5.6 devolve verdadeiro.
- A soma de dois máximos BIGINT devolve `-2` no 2.0.0, em vez de
  `18446744073709551614`. A
  [issue 27641](https://github.com/duckdb/duckdb/issues/27641) é corroborada.
- Para quatro valores proporcionais a 1e200, `stddev_samp` devolve infinito
  e `corr` devolve 0. A referência recusava o cálculo com erro. O oráculo
  Decimal com 120 dígitos confirma desvio padrão finito
  `1.2909944487358056e200` e correlação arredondada a binary64 igual a 1.

Isto não demonstra que `parqit correlate` devolva 0: os seus agregadores
próprios passaram os testes correspondentes. Os dois casos de soma, quando
repetidos apenas com dois Parquets no diagnóstico Stata, também passaram.
Os resultados C API são registados como falhas do núcleo/SQL, sem atribuir
indiscriminadamente a mesma falha a todos os verbos.
Evidência: `capi_baseline_diagnostics.log`, `capi_candidate_diagnostics.log`,
`decimal_oracle.json`, `upstream_probes.do`.

## Diferenças que não foram confundidas com corrupção

- **Schema dos dois escritores — REVIEW.** V109 reprova a igualdade global
  Arrow/staged. Uma reprodução separada confirma que todos os payloads,
  tipos físicos, códigos `.a`–`.z` e metadados globais são iguais. A diferença
  está na nulabilidade de sete campos do schema. Não foi classificada como
  perda de códigos ou valores. Evidência: `staged_schema_comparison.json`
  e os dois diretórios `staged_baseline/` e `staged_candidate/`.
- **Planos EXPLAIN — REVIEW da instrumentação.** O 2.0.0 usa nomes como
  `Hash Join`/`Window`, onde os testes procuram `JOIN`/`WINDOW`. Isto explica
  algumas asserções C++ de apresentação. Não explica D200-01, os resultados
  errados nem a recusa de partições voláteis. Um teste futuro pode adaptar
  o leitor do plano, mantendo as verificações da transformação efetiva.
- **Chave Hive reservada — REVIEW do contrato.** V78 passa as verificações
  de valores especiais anteriores, mas o candidato aceita
  `__HIVE_DEFAULT_PARTITION__` onde o contrato publicado exigia recusa.
  Não foi inferida corrupção desse valor só pelo código de retorno diferente.
- **Literal inteiro enorme — REVIEW de tipos/compatibilidade.** V155 passa
  os casos anteriores e para no literal de 40 dígitos: o novo tipo do motor
  não é carregável pelo adaptador. A recusa é explícita, não truncagem
  silenciosa. É necessária uma decisão de representação antes de afirmar
  equivalência de comportamento.

## Cobertura positiva e limites

A suite percorreu leitura direta, planos lazy, collect/save, tipos, datas,
strings, metadados, labels, formatos, características, merge/append/joinby,
reshape/pivot/collapse, exploração estatística, by/subscritos/time-series,
amostragem, R, SPSS, encodings, erros, atomicidade e concorrência. Muitos
casos destas famílias passam: os 215 PASS do candidato são evidência útil,
não um certificado de todas as opções e combinações possíveis.

O inventário a montante guardou 47 issues abertas que mencionavam 2.0.0.
Foram distinguidos problemas antigos, novas regressões e casos sem reprodução
local. Por exemplo, o problema DELTA_BINARY_PACKED da issue 27454 também é
reportado para 1.5.6 e requer Parquet V2; o writer normal permanece em V1.
Não foi usado como prova de uma regressão introduzida nesta migração.

O formato de base de dados DuckDB 2.0 não é o formato Parquet: o fluxo normal
do package abre um motor em memória e grava Parquet. A nova versão de storage
não foi apresentada como explicação para as falhas de ficheiros acima.
Windows, macOS, Stata noutras versões e CPUs antigas continuam NOT_RUN;
não foram criados artefactos de release nem acionado CI remoto.

## Desempenho e condições para reabrir a migração

**NOT_RUN. Nenhuma perda de desempenho foi aceite.** A condição de correção
falhou antes da campanha. O harness preparado usa um milhão de linhas
sintéticas, cinco blocos ABBA, três repetições por medição, quatro threads,
dez workflows e verificações analíticas/independentes dos resultados. O
próprio runner recusa começar perante os resultados atuais das suites.
Não existem rácios, medições de memória ou alegações de velocidade aprovados.

As issues a montante sobre estimativas de joins, filtros VARCHAR e NOT EXISTS
foram inventariadas, mas não são uma justificação genérica para qualquer
regressão. Uma eventual perda só pode ser aceite depois de preservar o
resultado, reproduzir o workload, ligar a causa à alteração documentada do
motor e medir repetições intercaladas com controlos A/A e B/B.

Para reabrir a substituição: corrigir e reproduzir a vermelho/verde cada
falha bloqueante; manter precisão e funcionalidades; esclarecer os casos
REVIEW sem enfraquecer os oráculos; passar CTest e a suite Stata completa
numa instalação staged identificada; concluir a campanha de desempenho; e
validar os alvos de distribuição. Não foi recomendado desativar globalmente
otimizadores ou remover testes para obter uma suite verde.

## Reprodutibilidade e recuperação

Os comandos executados e identidades constam dos logs e do recibo
`review_receipt.json`. Para repetir o novo teste, da raiz do repositório:

```bash
STATA=stata-mp BUILD_DIR="$PWD/build/duck200_review_20261022" \
  bash tests/run_stata.sh v169
```

O FAIL é o resultado esperado no candidato atual, não um teste a ignorar.
A referência em `build/duck156` passa o mesmo teste. Não se deve confundir
um código de saída 0 do processo Stata com uma asserção satisfeita.

O patch das alterações versionadas da revisão fica em
`audit_repro/duckdb_200_review_20261022/review_tracked.patch`; exclui os dois
ficheiros que já estavam alterados. O inverso desse patch permite recuperar
apenas estas alterações, após confirmar que não houve edições posteriores.
Os ficheiros novos têm um manifesto próprio. Não foi efetuada limpeza ou
remoção dos ficheiros anteriores do utilizador.
