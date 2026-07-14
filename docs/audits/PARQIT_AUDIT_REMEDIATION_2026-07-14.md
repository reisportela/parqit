# Remediação da auditoria `parqit` — 2026-07-14

## Resultado

Os cinco itens pedidos foram remediados sobre o snapshot
`f6995b440ff1ce498dda8ae6912561808b072200` (`f6995b4`) e integrados no
candidato a release `0.1.21`, de 14jul2026. A publicação remota é feita pelo
workflow da tag `v0.1.21`, somente depois de todos os jobs obrigatórios passarem.

Estado final dos gates: **PASS local**. A bateria C++ terminou com 3/3 testes
CTest, 63/63 casos e 973/973 asserções doctest; a bateria Stata terminou com 71
logs selecionados, 74 verdicts PASS, zero FAIL, zero log em falta e zero abort
depois do verdict. O binário Linux staged, construído no ambiente AlmaLinux 8
de release, passou o gate de stripping, exports e self-containment.

## Snapshot e ambiente testados

- Base Git: `f6995b440ff1ce498dda8ae6912561808b072200`.
- Estado testado: esse commit mais as alterações locais enumeradas abaixo.
- Último gate integral: 2026-07-14, Europe/Lisbon.
- Host: GCC 11.5.0, CMake 3.26.5, StataNow MP 19.5.
- Release Linux: AlmaLinux 8, GCC Toolset 12.2.1; o probe
  `PARQIT_HAVE_STATIC_LIBSTDCXX` passou.
- O baseline, antes dos novos repros, estava verde: configure/build, 3/3 CTest,
  63/63 casos e 973/973 asserções, suite Stata e release lint.

## Ficheiros da remediação

### Runtime e contrato público

- `src/plugin/parqit_plugin.cpp`
- `src/plugin/plugin_view.cpp`
- `src/plugin/plugin_view.hpp`
- `src/ado/p/parqit.ado`
- `benchmarks/profile_parqit.ado`
- `src/ado/p/parqit_*.dlg` (contrato do combine e banners da release)

### Release engineering

- `.github/workflows/build.yml`
- `tests/verify_collected_plugin.sh` (novo)
- `CMakeLists.txt`
- `CITATION.cff`
- `CLAUDE.md`
- `src/ado/p/parqit.pkg`

### Regressões

- `tests/run_stata.sh`
- `tests/concurrent/x01_bridge_xproc.sh` (novo)
- `tests/concurrent/x01_bridge_session_a.do` (novo)
- `tests/concurrent/x01_bridge_session_b.do` (novo)
- `tests/verify_suite/v58_bridge_lifecycle_mm.do` (novo)
- `tests/integration/t04_two_table.do`
- `tests/integration/t10_audit_fixes.do`
- `tests/verify_suite/v33_audit_fixes_20260623.do`
- `tests/verify_suite/v35_audit_fixes_20260623b.do`
- `tests/verify_suite/v44_summarize_detail_native.do`
- `tests/unit/test_view.cpp`
- `audit_repro/repro_merge_mm_physical_order.do`

### Documentação

- `README.md`
- `src/ado/p/parqit.sthlp`
- `BUILDING.md`
- `ASSUMPTIONS.md`
- `CHANGELOG.md` (`[0.1.21]`)
- `PARQIT_AUDIT_REMEDIATION_2026-07-14.md` (este relatório; novo)

## BRIDGE-XPROC-1

### Antes

Com `c(pid)` e `c(processid)` vazios, duas sessões com um `TMPDIR` comum
escolhiam `_parqit_opendata__1.parquet`. O repro pré-fix em
`/tmp/parqit_tests.AQpZFw/x01_bridge_xproc.log` mostrou a sessão A a falhar a
asserção do seu próprio `id=111`, depois de recolher o conteúdo de B, e a sessão
B a expirar à espera do marker de A.

### Depois

O plugin reserva atomicamente um diretório privado por bridge. O nome combina o
PID real do SO, um contador da operação e 128 bits obtidos por
`std::random_device`; `create_directory` é o árbitro atómico final. O caminho é
transportado em hex, preservando espaços e Unicode. Já não há `capture erase`
de um nome previsível.

Uma execução pós-fix, com o mesmo `TMPDIR` contendo `shared tmp ü`, produziu:

```text
open A:    .../_parqit_bridge_opendata_551_1_0c7c.../bridge.parquet
open B:    .../_parqit_bridge_opendata_552_1_2b14.../bridge.parquet
adapter A: .../_parqit_bridge_import_551_2_322a.../bridge.parquet
adapter B: .../_parqit_bridge_import_552_2_2c54.../bridge.parquet
VERDICT(X01_BRIDGE_XPROC): PASS
```

O wrapper usa markers, polling de 50 ms com limites, inspeciona os dois logs
incluindo aborts posteriores ao verdict e remove apenas o seu scratch. Passou
quatro vezes consecutivas na verificação focada e voltou a passar na suite
integral.

## BRIDGE-LIFETIME-1

### Antes

O repro pré-fix (`/tmp/parqit_tests.8JRUEq/v58_bridge_lifecycle_mm.log`) deixou
`_parqit_imp__1.parquet` depois de um eager use falhado, acumulou bridges após
um merge falhado, manteve o bridge de um merge bem-sucedido depois de
`parqit close`, e terminou com `No files found` quando o view de origem foi
fechado antes de um plano derivado. Replace e dois views distintos também não
tinham ownership observável/correto.

### Depois

O plugin mantém um registry de bridges criados pelo próprio pacote, com estado
`pending` e referências por view. Um pedido só pode reclamar um path se ele:

1. existir no registry;
2. continuar pending e sem referências;
3. aparecer entre os inputs da operação.

Assim, um pedido interno adulterado não pode transformar um ficheiro arbitrário
do utilizador em candidato a remoção. Em sucesso, merge/append/joinby transferem
explicitamente os paths criados pela operação. Dependências `using view:name`
partilham referências, portanto fechar o view fonte não invalida o plano
derivado. Replace só troca o view depois de o candidato validar. Em erro, o ado
descarta todos os bridges pending sem substituir o rc original; em close/replace
o último owner remove o seu diretório; `close _all` é o sweep final apenas do
registry do pacote.

`V58_BRIDGE_LIFETIME_MM` cobre e passou:

- recusa de um `owned_files` não registado sem apagar o input;
- eager `.dta` falhado e bem-sucedido;
- merge lazy falhado e merge lazy bem-sucedido + close;
- append bem-sucedido com dois adapters e joinby bem-sucedido;
- replace falhado atómico e replace bem-sucedido;
- dois views independentes, fechados um a um;
- referência partilhada por um plano derivado;
- append multi-source parcialmente importado e depois falhado;
- `close _all` sem resíduos;
- paths com espaços/Unicode através do gate multi-processo.

## MM-ORDER-1

### Antes

O repro auditado terminava com:

```text
REPRODUCED: parqit m:m paired payloads in a different within-key order
VERDICT(REPRO_MERGE_MM_PHYSICAL_ORDER): FAIL
```

A ordenação determinística do plano lazy não é a ordem física dentro da chave
que o `merge m:m` nativo usa para o emparelhamento sequencial.

### Depois

O `parqit merge m:m` lazy é reconhecido apenas para devolver rc 198, antes de
resolver/importar o using ou de alterar o view. A mensagem recomenda `joinby`
para o produto cartesiano ou `parqit mergein m:m` para o comportamento
sequencial nativo. Há defesa equivalente no plugin. A implementação inferior
do spine determinístico foi mantida como helper interno para não remover código
sem necessidade, mas deixou de ser contrato público.

O repro atualizado passou: a recusa deixou o master intacto e
`parqit mergein m:m` foi byte-comparado com o `merge m:m` nativo. `t04`, `v33`,
`v35` e `v58` fixam a mesma política. README, help, dialog, assumptions e
`[0.1.21]` foram alinhados com a nova versão/data publicada.

## TEST-V44-CAPTURE-1

### Antes

O `capture assert` final não consumia `_rc`; uma mutation para `capture assert
0` ainda imprimia PASS na auditoria original.

### Depois

A asserção exige `r(N) == 5` e incrementa `$DFAILS` quando falha. A mutation
scratch forçada produziu:

```text
DIFF all-missing-first multi-variable summarize: r(N)=5 expected 5
VERDICT(V44_SUMMARIZE_DETAIL_NATIVE): FAIL - 1 diffs
```

Restaurada a asserção real, o teste focado e a suite integral deram PASS. Foram
revistos os 109 sites `capture assert` em `tests/*.do`; em todos os restantes o
primeiro comando lógico consome `_rc`, pelo que não houve alterações
heurísticas adicionais.

## DIST-STRIP-1

### Antes

O workflow validava um `find build/.../parqit.plugin`, mas recolhia o target
bruto. O asset auditado tinha 53 197 640 bytes e não estava stripped.

### Depois

O workflow recolhe `ado/plus/p/parqit.plugin`, a superfície staged criada pelo
CMake, e verifica exatamente `out/parqit.plugin` antes do upload. O ensaio local
do job revelou que a imagem EL8 ainda não instalava `file`; essa dependência foi
adicionada ao workflow.

Medição no mesmo build Release AlmaLinux 8:

| Artefacto | Bytes | Resultado do verificador |
|---|---:|---|
| `build/release-audit-linux/parqit.plugin` (bruto) | 53 226 104 | FAIL: `.symtab` presente |
| `build/release-audit-linux/collected/parqit.plugin` (staged) | 43 304 432 | PASS |

Redução: 9 921 672 bytes (18,64%). O staged é ELF64, não contém `.symtab` nem
secções debug, conserva `stata_call` e `pginit`, e não tem entradas NEEDED para
`libstdc++` ou `libgcc_s`. O mesmo script passou dentro de AlmaLinux 8. A cópia
dev foi deliberadamente recusada pela dependência dinâmica, provando também a
sensibilidade do gate de self-containment. O artefacto final `0.1.21` tem
SHA-256 `0d8f2f67c67a58fe4e3be6ee103567d7e336dee1f9b60ea27b9253d69740be82`.

No macOS, o gate reconhece Mach-O e os exports depois de `strip -x`; no Windows,
reconhece PE/COFF e os exports do MSVC Release. Não se aplica a regra ELF de
`.symtab` fora de Linux.

## Gates executados

### Focados

| Gate | Resultado |
|---|---|
| `x01_bridge_xproc` | 4 execuções consecutivas PASS; 3 verdicts por execução |
| `v58_bridge_lifecycle_mm` | PASS |
| `v44_summarize_detail_native` | PASS; mutation scratch FAIL com 1 diferença |
| `t04_two_table` + repro `m:m` | PASS; lazy rc 198 e `mergein` == nativo |
| `t10_audit_fixes` | PASS; duas promoções e cleanup |
| `v24_multiformat_sources` | PASS; `.dta/.xlsx/.csv` |
| `v40_set_tempdir_paths` | PASS; espaços/Unicode |
| `v33`–`v37` afetados | 5/5 PASS |
| profiling copy | `VERDICT(PROFILE_BRIDGE_PROTOCOL): PASS` |
| raw/staged Linux verifier | rc 1 esperado / rc 0 |

### Integrais

```text
cmake --preset dev                                             PASS
cmake --build --preset dev -j4                                PASS
ctest --preset dev --output-on-failure -j3                    3/3 PASS
./build/dev/parqit_tests                                      63/63 casos
                                                               973/973 assertions
                                                               1 skipped
suite Stata integral sobre a superfície staged dev
                                                               71/71 logs
                                                               74 PASS, 0 FAIL
suite Stata integral sobre o plugin staged Release AlmaLinux 8
                                                               71/71 logs
                                                               74 PASS, 0 FAIL
bash tests/release_lint.sh                                    PASS v0.1.21
bash -n (runner, wrapper e verificador)                       PASS
git diff --check                                              PASS
```

O CTest inclui explicitamente `runner_no_match` e `unit_concurrent`; ambos
passaram, além do unit test normal.

## Limitações e não verificado

- Os gates locais não substituem o workflow remoto da tag. O build e o gate
  Linux foram reproduzidos localmente em AlmaLinux 8; os ramos macOS arm64 e
  Windows x86_64 são executados pelo GitHub Actions, não localmente.
- Não houve runtime Stata em macOS/Windows. Os checks de formato/export do CI
  não são apresentados como substitutos dessa cobertura.
- macOS Intel continua deliberadamente fora da matrix e do manifest; nada nesta
  remediação reintroduz esse artefacto.
- O plugin dev local liga `libstdc++`/`libgcc_s` dinamicamente, como documentado;
  esse ficheiro não é o artefacto validado para release.
- Um término externo não recuperável do processo (por exemplo, `SIGKILL`) fica
  sujeito à limpeza do temp root pelo Stata/SO; os caminhos normais, falhas
  capturadas, replace, close e `close _all` estão cobertos.
- Os itens remediados foram transferidos de `[Unreleased]` para a secção
  `[0.1.21]`; o workflow recusa uma tag cuja versão não coincida com CMake.

## Worktree final e preservação de ficheiros do utilizador

Ficheiros **pré-existentes e não rastreados**, preservados sem edição:

```text
?? AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-07-14.md
?? CODEX_HOLISTIC_AUDIT_PROMPT_2026-07-09.md
?? PROMPT_CODEX_IMPLEMENTAR_AUDITORIA_PARQIT_2026-07-14.md
?? examples/parqit_dlg.do
?? examples/pq_to_parqit_common_workflows.do
?? scratch_inj/
```

Alterações da remediação (`git status --short`; `tests/concurrent/` contém os
três ficheiros novos enumerados acima):

```text
 M .github/workflows/build.yml
 M ASSUMPTIONS.md
 M BUILDING.md
 M CHANGELOG.md
 M CITATION.cff
 M CLAUDE.md
 M CMakeLists.txt
 M README.md
 M audit_repro/repro_merge_mm_physical_order.do
 M benchmarks/profile_parqit.ado
 M src/ado/p/parqit.ado
 M src/ado/p/parqit.pkg
 M src/ado/p/parqit.sthlp
 M src/ado/p/parqit_combine.dlg
 M src/ado/p/parqit_explore.dlg
 M src/ado/p/parqit_filter.dlg
 M src/ado/p/parqit_gen.dlg
 M src/ado/p/parqit_pivot.dlg
 M src/ado/p/parqit_read.dlg
 M src/ado/p/parqit_stats.dlg
 M src/ado/p/parqit_vars.dlg
 M src/ado/p/parqit_views.dlg
 M src/ado/p/parqit_write.dlg
 M src/plugin/parqit_plugin.cpp
 M src/plugin/plugin_view.cpp
 M src/plugin/plugin_view.hpp
 M tests/integration/t04_two_table.do
 M tests/integration/t10_audit_fixes.do
 M tests/run_stata.sh
 M tests/unit/test_view.cpp
 M tests/verify_suite/v33_audit_fixes_20260623.do
 M tests/verify_suite/v35_audit_fixes_20260623b.do
 M tests/verify_suite/v44_summarize_detail_native.do
?? PARQIT_AUDIT_REMEDIATION_2026-07-14.md
?? tests/concurrent/
?? tests/verify_collected_plugin.sh
?? tests/verify_suite/v58_bridge_lifecycle_mm.do
```

O relatório original da auditoria, os dois prompts locais, os exemplos
untracked e `scratch_inj/` permaneceram fora do âmbito de escrita. Não foi
executado nenhum comando destrutivo sobre trabalho preexistente.
