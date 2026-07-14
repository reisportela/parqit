# Parecer técnico GO-GO de fiabilidade dos dados — parqit v0.1.22

- Data: 14 de julho de 2026
- Versão candidata: `v0.1.22`
- Baseline adversarial auditada: `v0.1.21` (`ffb9c70770330137b89c5024a986da97ef7905d1`)
- Implementação corrigida: `d0a535440b4ace3f0a84b617837a825a985153de`
- CI multiplataforma da implementação: [GitHub Actions 29365971883](https://github.com/reisportela/parqit/actions/runs/29365971883)
- Ambiente runtime integral: Linux x86_64, StataNow MP 19.5, DuckDB 1.5.3

## 1. Decisão executiva

**GO-GO operacional, dentro do contrato e dos limites explicitamente testados.**

- **GO-1 — correção e fiabilidade dos dados: GO.** Os 20 findings confirmados
  na auditoria adversarial de `v0.1.21`, incluindo os cinco S0 e os oito S1,
  foram corrigidos, convertidos em regressões ou gates estruturais e voltaram a
  ser testados. Não permanece aberto nenhum S0/S1 identificado nessa auditoria.
- **GO-2 — engenharia de release: GO.** A versão, documentação, pacote e código
  estão coerentes; o candidato passou uma instalação staged integral e a matriz
  Linux x86_64, macOS ARM64 e Windows x86_64 compilou, executou os testes C++ e
  verificou o binário exato recolhido para distribuição.

Este resultado sustenta o uso institucional controlado de `parqit v0.1.22` nas
superfícies cobertas. Não é uma garantia matemática de ausência de qualquer bug,
uma certificação legal/acreditada, nem uma validação runtime de Stata em todos os
sistemas operativos. Os limites residuais e as condições de utilização estão nas
secções 7 e 8. Qualquer falha no pipeline da tag ou na validação do artefacto
Linux efetivamente publicado revoga automaticamente este GO-GO.

## 2. Âmbito efetivamente assegurado

| Superfície | Evidência | Decisão |
|---|---|---|
| Código C++/ado e engine embebido | build limpo, 69 casos C++, 1 027 assertions, CTest e revisão adversarial | **GO** |
| Runtime Stata em Linux x86_64 | duas suites integrais, incluindo instalação staged; PyArrow, DuckDB e Stata nativo como oracles | **GO** |
| Build macOS ARM64 | build, testes C++, formato Mach-O e exatamente `_pginit`/`_stata_call` | **GO estrutural** |
| Build Windows x86_64 | build, testes C++, formato PE/COFF e exatamente `pginit`/`stata_call` | **GO estrutural** |
| macOS/Windows dentro de Stata | não existia runtime Stata disponível nesses runners | **não certificado em runtime** |
| macOS Intel | não é compilado nem publicado pela matriz atual | **fora do âmbito** |
| Filesystems e falhas físicas extremas | não houve power loss, `SIGKILL`, disco cheio real, NFS/SMB ou interferência de antivírus | **fora do âmbito testado** |

## 3. Fecho dos gates G0-G9

| Gate | Estado em v0.1.22 | Fundamentação principal |
|---|---|---|
| G0 — snapshot/proveniência/versão | **PASS forte** | versão `0.1.22` coerente em todas as superfícies; lint de release; commits e CI identificados |
| G1 — paridade Stata | **PASS** | `v59`, `v60` e 30 pipelines diferenciais seeded contra Stata nativo |
| G2 — valores/tipos/precisão | **PASS no contrato definido** | wide integers/decimals preservados até à fronteira; conversões não representáveis recusam ou avisam; literais binary64 e temporais testados |
| G3 — metadata round-trip | **PASS** | globs completos, JSON estrito, provenance posicional, labels/chars/formats e `sortedby` cobertos |
| G4 — atomicidade/erro | **PASS** | transações de output owned, rollback, views validate-then-commit e falhas injetadas preservam o estado anterior |
| G5 — lazy/bridges/ownership/concorrência | **PASS** | tipos físicos lazy, sort state, lifecycle e dois processos no mesmo destino cobertos |
| G6 — escala/limites/out-of-core | **PASS condicionado** | 1,5 M linhas, 2 500 variáveis, `strL` de 1 MB, partições e limites SPI simulados; não é uma prova de escala ilimitada |
| G7 — harness resistente | **PASS** | cada finding tem regressão/gate; mutation de assertion faz o runner falhar e o controlo restaurado passa |
| G8 — staged/publicável | **PASS do candidato** | instalação staged integral e binários CI exatos verificados nos três sistemas; a publicação final permanece gate terminal |
| G9 — documentação fiel | **PASS** | help, README, CHANGELOG, pacote e limitações foram alinhados com o comportamento executado |

## 4. Matriz de remediação dos 20 findings

| Finding | Severidade original | Fecho implementado | Prova de regressão/gate |
|---|---:|---|---|
| PARQIT-REL-001 | S0 | lock criado atomicamente e staging aleatório, owned e same-filesystem; cleanup nunca remove paths previsíveis/alheios | `v59`, `x02` |
| PARQIT-DATA-002 | S0 | `UBIGINT`, `HUGEINT`, `UHUGEINT` e `DECIMAL` deixam de ser colapsados prematuramente na fronteira lazy | `v59`, unit |
| PARQIT-DATA-003 | S0 | literais numéricos são canonicalizados pela mesma representação binary64 de Stata | `v59`, unit |
| PARQIT-DATA-004 | S0 | o fast direct path pré-valida `strL` binário e recusa NUL antes de criar output; lazy Parquet-to-Parquet preserva-o | `v59` |
| PARQIT-DATA-005 | S0 | timestamps exigem round-trip exato de milissegundos, incluindo extremos positivos e negativos | `v59`, unit |
| PARQIT-SEM-006 | S1 | `replace` e `append` invalidam/materializam corretamente estado de ordenação diferida | `v60`, unit |
| PARQIT-TYPE-007 | S1 | o tipo físico lazy passa a cumprir o tipo explícito e a metadata; coerção float/double entra no plano | `v59`, unit |
| PARQIT-TYPE-008 | S1 | `gen` sem tipo usa `double`, de forma coerente com o contrato público | `v59`, unit, help |
| PARQIT-DATE-009 | S1 | literais temporais inválidos deixam de ser normalizados silenciosamente e falham antes de commit | `v59`, unit |
| PARQIT-META-010 | S1 | metadata é reconciliada sobre a enumeração completa dos ficheiros do glob | `v60` |
| PARQIT-META-011 | S1 | JSON `parqit.*` malformado ou com top-level inválido falha alto | `v60` |
| PARQIT-META-012 | S1 | provenance de colunas duplicadas é transportada por posição, não apenas por nome | `v60` |
| PARQIT-META-013 | S1 | `sortedby` é persistido e restaurado quando a ordem continua válida | `v60` |
| PARQIT-ATOM-014 | S2 | group rename, incluindo ciclos, valida numa view candidata e só depois faz commit | `v60`, unit |
| PARQIT-ATOM-015 | S2 | `sql ..., clear` materializa numa candidata e preserva a view anterior em falha | `v60` |
| PARQIT-ATOM-016 | S2 | funções string e todos os view ops passam bind-probe antes de substituir o plano corrente | `v60` |
| PARQIT-PORT-017 | S2 | sanitização Unicode usa a classificação UTF-8 da própria DuckDB/utf8proc | `v60`, unit |
| PARQIT-LIFE-018 | S2 condicional | criação parcial do worker pool é apanhada, abortada, notificada e inteiramente joined | `v60`, fault injection |
| PARQIT-PKG-019 | S3 | DuckDB é embebida em static mode no Windows e `plugin.def` define a única ABI pública | CI: export table exata |
| PARQIT-DOC-020 | S3 | help limita `_n`/`_N` aos caminhos efetivamente suportados | help, `v60` |

Durante a revisão pós-correção foram ainda endurecidos três pontos que não foram
aceites apenas por a suite ficar verde:

1. Se publicação e rollback falharem simultaneamente, o payload anterior fica
   retido num path de recuperação explicitamente reportado; o destrutor não o
   apaga.
2. `replace` com storage `float`/`double` grava a coerção no plano lazy antes de
   verbos posteriores, evitando que valores intermédios usem precisão diferente.
3. O gate de exports verifica o ficheiro já recolhido e, no macOS, distingue
   imports indefinidos de exports definidos; isto foi confirmado numa nova
   matriz verde, não presumido por leitura de código.

## 5. Evidência executada

### 5.1 Build e testes locais

```text
cmake --preset dev                                                   PASS
cmake --build build/dev --target parqit_plugin parqit_tests -j       PASS
./build/dev/parqit_tests                                             69/69 casos
                                                                    1027/1027 assertions
                                                                    1 skipped
ctest --preset dev --output-on-failure                               3/3 PASS
bash tests/release_lint.sh                                           PASS v0.1.22
bash -n tests/*.sh tests/concurrent/*.sh                             PASS
git diff --check                                                     PASS
```

### 5.2 Stata sobre instalação staged fresca

- superfície: `/tmp/parqit_staged_v0122_final.Hrr54d`;
- plugin testado:
  `/tmp/parqit_staged_v0122_final.Hrr54d/src/ado/p/parqit.plugin`;
- SHA-256:
  `cb68af9b619ce9fb4d2d3702add19867af3366d62865527f9f2ac726c50d2427`;
- resultado: **74 entradas de teste concluídas; 79 linhas `VERDICT(...): PASS`,
  zero non-PASS**;
- logs: `/tmp/parqit_tests.UnDg8s`;
- inclui `v59`, `v60`, concorrência cross-process `x01`/`x02`, round-trip,
  integração, escala e todos os regressions anteriores.

Foi também executada uma suite integral final sobre o checkout antes do stage,
com o mesmo resultado terminal. O stage evita que um ficheiro omitido, uma
resolução acidental do source tree ou um plugin antigo transforme um PASS local
num falso PASS de distribuição.

### 5.3 Oracles e sensibilidade do harness

- 30 pipelines determinísticos seeded de `keep if -> gen double -> replace ->
  collapse -> sort -> collect` coincidiram célula a célula com Stata nativo;
  log: `/tmp/parqit_diff_v0122.Eqy2Ju/parqit.log`.
- PyArrow e DuckDB foram usados para schema físico, valores raw, metadata,
  timestamps, globs e bytes Parquet independentes do plugin.
- Uma mutation controlada alterou uma expectativa de `v59`; o script deixou de
  terminar, o runner saiu com código 1 e registou `SCRIPT DID NOT FINISH` em
  `/tmp/parqit_tests.4LoREc`. O ficheiro original foi restaurado.

### 5.4 CI multiplataforma

A execução [29365971883](https://github.com/reisportela/parqit/actions/runs/29365971883),
no commit `d0a535440b4ace3f0a84b617837a825a985153de`, terminou com:

| Job | Build | C++ | Binário recolhido | ABI/estrutura |
|---|---:|---:|---:|---:|
| release-lint | — | — | — | **PASS** |
| Linux x86_64 / AlmaLinux 8 | **PASS** | **PASS** | **PASS** | ELF 64-bit, stripped, sem libstdc++/libgcc dinâmicas e dois exports: **PASS** |
| macOS ARM64 | **PASS** | **PASS** | **PASS** | Mach-O 64-bit e dois exports definidos: **PASS** |
| Windows x86_64 | **PASS** | **PASS** | **PASS** | PE/COFF 64-bit e dois exports: **PASS** |

## 6. Política de fronteiras deliberada

Alguns domínios Parquet são mais ricos do que os tipos que a memória de Stata
consegue representar. A política de `v0.1.22` é fail-loud/preserve, nunca
aproximação silenciosa:

- wide integers e decimals mantêm o tipo exato enquanto o pipeline permanece
  lazy e podem ser gravados Parquet-to-Parquet; ao `collect`, a fronteira
  binary64 é verificada e avisada/recusada segundo o caso;
- timestamps cuja contagem de milissegundos não faça round-trip exato para
  Stata são recusados em vez de arredondados silenciosamente;
- `strL` com NUL é preservado no caminho lazy Parquet-to-Parquet, mas o fast
  direct path desde memória Stata recusa-o antes de publicar qualquer output;
- metadata malformada ou inconsistente deixa de ser descartada silenciosamente
  e passa a produzir erro acionável.

## 7. Risco residual e exclusões

Permanecem fora da prova executada:

- Stata runtime em macOS ARM64 e Windows x86_64; a CI prova build, unit tests,
  formato e ABI, não que uma instalação local específica de Stata carrega o
  plugin;
- macOS Intel, que não tem runner nem asset nesta release;
- power loss, `SIGKILL`, disco cheio real, falhas de rename depois de sync,
  NFS/SMB, mounts com semântica não POSIX e locks/interferência de antivírus;
- campanhas prolongadas de fuzz, milhões de ficheiros num glob e limites físicos
  extremos de storage/memória;
- recuperação automática de locks deixados por crash: um
  `<dest>.parqit_lock` preexistente bloqueia de propósito e exige inspeção humana,
  porque removê-lo automaticamente enfraqueceria ownership;
- defeitos desconhecidos fora do espaço coberto pelos testes e oracles.

Estas exclusões impedem uma promessa absoluta de “zero erros”. Não invalidam o
GO-GO operacional no âmbito testado; definem quando uma entidade deve executar
validação adicional antes de produção.

## 8. Condições recomendadas para uso institucional

1. Fixar a versão/tag e arquivar SHA-256 dos assets usados; não instalar de uma
   branch móvel em produção.
2. Executar `parqit version` e `parqit selftest` após cada instalação. Em macOS
   ou Windows, fazer pelo menos um smoke e um round-trip local antes de produção.
3. Preservar inputs imutáveis/backups e logs do pipeline, como se faria com
   qualquer motor de transformação usado em estatística oficial.
4. Tratar qualquer erro ou aviso de precisão/metadata como bloqueador de output;
   não o suprimir para obter um resultado.
5. Pilotar separadamente workloads em NFS/SMB, volumes sincronizados ou paths
   sujeitos a antivírus, porque a atomicidade real depende do filesystem.
6. Para identificadores `UINT64`, `HUGEINT` ou `DECIMAL` fora de binary64,
   preferir transformação e `parqit save` lazy; não exigir `collect` para Stata
   se a representação exata não existir.
7. Para estatísticas oficiais de elevado impacto, manter reconciliações
   independentes de totais, chaves e schema nos outputs finais. Este relatório
   reduz risco de software; não substitui controlo estatístico do processo.

## 9. Regra terminal da release

A tag `v0.1.22` só deve ser criada sobre o commit que contém este parecer depois
de a sua própria matriz `main` ficar verde. Depois da tag, o workflow deve:

1. repetir lint, build, unit tests e inspeção do plugin nos três sistemas;
2. criar os ZIPs e ficheiros loose sem substituir os binários verificados;
3. publicar uma release não-draft e não-prerelease;
4. permitir descarregar o asset Linux publicado, verificar-lhe o hash/ABI e
   executar novamente a suite Stata staged sobre esse binário exato.

Se qualquer passo falhar, a decisão regressa a **NO-GO** até correção e nova
evidência. Se todos passarem, o GO-GO deste parecer fica fechado também sobre a
superfície efetivamente publicada.
