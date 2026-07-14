# Auditoria adversarial ao `parqit` — 2026-06-23

**Auditor:** Codex  
**Modo:** auditoria com correcoes aplicadas  
**Alvo:** working tree local em `/home/mangelo/Documents/GitHub/parqit`, v0.1.8  
**Nota de higiene:** `CLAUDE.md` ja estava modificado antes desta auditoria e nao
foi alterado por este ciclo.

## Linha de base

Antes de alterar codigo, a build dev, os unit tests C++ e a suite Stata completa
passavam:

```bash
cmake --preset dev
cmake --build build/dev --target parqit_plugin parqit_tests -j
ctest --preset dev --output-on-failure
STATA=stata-mp BUILD_DIR="$PWD/build/dev" bash tests/run_stata.sh
```

## Finding confirmado

### RZ-1 — `reshape long` usava sufixos numericos com zeros a esquerda como valores

**Severidade:** alta, silent wrong result.  
**Superficie:** `parqit reshape long`.

Caso adversarial:

```stata
clear
input id inc1 inc01
1 10 100
2 20 200
end
reshape long inc, i(id) j(year)
```

O Stata nativo usa apenas `inc1 -> inc`, cria `year=1`, e carrega `inc01` como
variavel ordinaria. O `parqit` anterior tratava `inc01` como outro xij para
`j=1`, fabricando quatro linhas em vez de duas e colocando `100/200` em `inc`.

Sondas adicionais mostraram a regra nativa:

- `inc1 inc01`: `inc01` e carregada; `inc` vem de `inc1`.
- `inc01` sem `inc1`: o Stata cria `j=1`, `inc` missing, e carrega `inc01`.
- `inc2 inc02 inc10`: `inc02` e carregada; os valores long vem de `inc2` e
  `inc10`.

## Correcoes aplicadas

- `src/engine/view.cpp`: `reshape_long()` agora canonicaliza sufixos numericos
  (`01 -> 1`, `00 -> 0`) para decidir o valor `j`, mas so usa a coluna canonica
  (`inc1`, `inc0`) como xij. A coluna com zero a esquerda continua carregada.
  Quando so existe a coluna nao-canonica, o stub long e materializado como
  `NULL`, como missing no Stata.
- `tests/verify_suite/v34_reshape_leading_zero_suffix.do`: novo teste
  end-to-end contra Stata nativo para os tres casos acima.
- `tests/release_lint.sh`: o lint de release agora rejeita headings `###`
  duplicados dentro de qualquer secao do `CHANGELOG.md`, nao apenas em
  `[Unreleased]`.
- `CHANGELOG.md`: consolidado o bloco `0.1.3` que ainda tinha dois
  `### Added`; adicionada entrada `[Unreleased]` para esta correcao.

## Validacao apos as alteracoes

Comandos executados depois da correcao:

```bash
cmake --build build/dev --target parqit_plugin parqit_tests -j
ctest --preset dev --output-on-failure
bash tests/release_lint.sh
STATA=stata-mp BUILD_DIR="$PWD/build/dev" bash tests/run_stata.sh v34
STATA=stata-mp BUILD_DIR="$PWD/build/dev" bash tests/run_stata.sh t05_power
STATA=stata-mp BUILD_DIR="$PWD/build/dev" bash tests/run_stata.sh
```

Resultado: todos passaram. A suite Stata completa inclui agora `v34` e terminou
com PASS em integracao, `verify_suite` e roundtrip.

## Risco residual

Nao foi executado benchmark de performance dedicado. A alteracao funcional e
limitada a `reshape_long()` e acrescenta apenas trabalho sobre o manifesto de
colunas no momento de compilar o plano; nao muda materializacao, I/O, tipagem,
metadata, joins, collect/save, nem caminhos quentes de leitura/escrita.
