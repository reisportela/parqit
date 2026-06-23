# parqit vs pq — paridade de funcionalidades para ficheiros Parquet

**Autor:** Claude (Opus 4.8) · **Data:** 2026-06-13
**Objeto:** garantir que o `parqit` cobre **todas** as funcionalidades do
`pq` (`stata_parquet_io`) **no que diz respeito a ficheiros Parquet**.
**Método:** extração exaustiva da superfície de comandos/opções de cada um
(ado + help), mapeamento capacidade-a-capacidade, e verificação empírica de
cada lacuna e respetiva solução. Comparação de fiabilidade e tempo em
[`benchmarks/compare_parqit_vs_pq.do`](benchmarks/compare_parqit_vs_pq.do).

## Veredicto

**O `parqit` é funcionalmente completo para Parquet face ao `pq`.** Das 42
capacidades Parquet do `pq`, todas são alcançáveis no `parqit`:

| Categoria | N.º | Significado |
|---|---:|---|
| **Mesma sintaxe** | 14 | igual nos dois |
| **Sintaxe diferente** | 16 | mesmo resultado pela gramática de verbos *lazy* do `parqit` |
| **Lacuna fechada nesta ronda** | 1 | `relaxed` — implementado (ver §3) |
| **Conveniência com workaround** | 4 | uma-opção no `pq`; no `parqit` em 1–2 passos (testado, §4) |
| **Fora de âmbito (por desenho)** | 6 | leitores/escritores SAS/SPSS/CSV e *knobs* internos do motor do `pq` |

Na manipulação o `parqit` é um **superconjunto** do `pq`: junta a outra *view*
aberta, pré-transforma o lado *using* de um `merge` com todos os verbos,
`collapse`/`reshape`/`joinby`/`gsort`/exploração out-of-core — nada disto
existe no `pq`. A comparação direta (mesmos dados) confirma **valores
idênticos** em `use`/`save`/`merge`/`append`/`describe`.

---

## 1. Mesma sintaxe (14)

`pq` e `parqit` fazem-no da mesma forma:

- `use [varlist] using <f>` (seleção de colunas = *projection pushdown*)
- `use … , clear` (ler para memória)
- `merge <tipo> <chaves> using <f>` (1:1 / m:1 / 1:m / m:m)
- `merge … , keepusing(varlist)` · `keep(master using match)` · `generate(name)` / `nogenerate`
- `save [varlist] using <f>` · `replace` · `partition_by(varlist)` · `compression(codec)` · `compression_level(#)`
- `describe using <f>` · `path <f>`

## 2. Sintaxe diferente — mesma capacidade (16)

O `pq` empilha tudo em opções de `use`; o `parqit` usa verbos *lazy* sobre uma
*view* e materializa com `collect`/`save`. O resultado é o mesmo:

| pq (uma opção) | parqit (verbos) |
|---|---|
| `use … , in(a/b)` | `parqit use <f>` → `parqit keep in a/b` → `parqit collect, clear` |
| `use … , if(exp)` | `parqit use <f>` → `parqit keep if exp` → `parqit collect, clear` |
| `use … , sort(vars)` | `parqit use <f>` → `parqit sort vars` (ou `gsort` p/ direções mistas) |
| `use … , random_n(N)` | `parqit use <f>` → `parqit sample N, count seed(s)` |
| `use … , random_share(p)` | `parqit use <f>` → `parqit sample p seed(s)` |
| `use … , drop(vars)` | `parqit drop vars` (aceita `*` e `?`) |
| `use … , drop_strl` | `parqit drop <cols strL>` (após `parqit describe`/`codebook`) |
| `use … , compress` | **automático** — o `parqit` dimensiona cada coluna ao menor tipo Stata exato |
| `use … , compress_string_to_numeric` | `parqit gen <tipo> v = real(s)` (por coluna) ou `parqit sql … TRY_CAST` |
| `append … , <read opts>` | `parqit append using <f> [<f> …]` (UNION ALL BY NAME); pré-filtra abrindo o using como *view* |
| `merge … , assert(res)` | `parqit merge … , generate(_m)` → `parqit tabulate _m` / `parqit count if _m==…` |
| `merge … , <read opts no using>` | abre o using como *view*, aplica `keep`/`sample`/`sort`/`drop`, e faz `merge … using <view>` |
| `save … , if(exp)` | `parqit keep if exp` → `parqit save` |
| `save … , chunk(N)` | `parqit save … , chunk(#)` (linhas por *row group*) |
| `save … , stream/consolidate` | `parqit save` já escreve **um ficheiro atómico out-of-core** por omissão |
| `describe … , detailed` | `parqit codebook` (kind/min/max/distinct por variável) |

---

## 3. Lacuna fechada nesta ronda — `relaxed` (implementado)

**Antes:** `parqit use data_*.parquet` sobre ficheiros com **esquemas
diferentes** dava erro (rc 920, *schema mismatch in glob*). O `pq` tem
`relaxed` para isto. (A auditoria automática assumiu, erradamente, que o
`parqit` já o fazia de forma nativa — o teste empírico provou que **não**.)

**Agora:** adicionei a opção **`parqit use … , relaxed`** (compila para
`read_parquet(…, union_by_name = true)`). Une as colunas por nome; uma
coluna ausente num ficheiro chega como *missing* — exatamente como o `pq`
`relaxed` e como o `parqit append` (que já unia por nome). Sem a opção, o
desencontro de esquemas continua a ser **erro alto** (nunca silencioso).
A precisão não muda: uma coluna ausente nalguns ficheiros chega como *NULL*
sob `union_by_name`, e um NULL **não alarga** o intervalo min/max — por isso o
dimensionamento por estatísticas do *footer* permanece correto mesmo quando a
coluna só existe nalguns ficheiros (não é, como uma versão anterior desta nota
afirmava, porque `count(stats) < count(*)` força um scan — `parquet_metadata`
só emite linhas para as colunas presentes em cada ficheiro). O caso de um
*row-group* inteiramente nulo continua a recuar para um scan exato. Teste:
`tests/verify_suite/v23_relaxed_union_by_name.do`.

```stata
parqit use using /data/anos/data_*.parquet, clear relaxed    // esquemas diferentes → união por nome
```

---

## 4. Conveniências do pq — workaround testado no parqit (4)

São opções de uma-tecla no `pq`; no `parqit` fazem-se em 1–2 passos. Todas
**verificadas a funcionar**. (Documentadas como diferenças conscientes; cada
uma é additiva e pode ser promovida a opção do `parqit` se valer a pena.)

### 4.1 `merge … , update [replace]` — preencher o master a partir do using
Stata: preenche os *missing* do master (e com `replace`, sobrepõe) a partir
das colunas homónimas do using. No `parqit` uma coluna **não-chave homónima** do
using é descartada no merge (fica a do master, com aviso) e não há rename-no-merge,
por isso traz-se a coluna do using **com outro nome** primeiro — um passo a mais:
```stata
* 1) abre o using, renomeia a coluna em conflito e grava um using temporário
tempfile utmp
parqit use using using.parquet
parqit rename x x_u
parqit save `"`utmp'.parquet"', replace
* 2) agora o merge traz x_u sem colidir com o x do master
parqit use using master.parquet
parqit merge 1:1 id using `"`utmp'.parquet"', keepusing(x_u) nogenerate
parqit replace x = x_u if missing(x)   // == update   (replace: x = x_u if !missing(x_u))
parqit drop x_u
parqit collect, clear
```
Resultado: master `x={., 5}` + using `x={99,88}` → `x={99, 5}` (id=1
preenchido, id=2 mantém o master) — semântica de `merge, update`. (A nota
anterior mostrava `keepusing(x_u)` diretamente sobre `using.parquet`, o que
**falha** porque a coluna lá se chama `x`, não `x_u`; daí o passo de rename.)

### 4.2 `use … , asterisk_to_variable(name)` — coluna a partir do nome do ficheiro
Para conjuntos com a chave só no nome (`data_2002.parquet …`):
```stata
parqit sql `"SELECT *, regexp_extract(filename, 'data_([0-9]+)', 1) AS year "' ///
          `"FROM read_parquet('/data/data_*.parquet', filename = true)"', clear
```
Verificado: cria `year = {2002, 2003}` a partir dos nomes. (DuckDB
`filename=true` é mais geral que o `pq`: dá o caminho completo, do qual
extrais o pedaço que quiseres.)

### 4.3 `save … , noautorename` — gravar os nomes Stata sanitizados
O `parqit` restaura os nomes Parquet originais (pré-sanitização) ao gravar.
Para gravar antes os nomes Stata limpos:
```stata
parqit rename <nome_original> <nome_desejado>    // por coluna afetada
parqit save out.parquet, replace
```

### 4.4 `save … , label` — gravar as etiquetas (strings) em vez dos códigos
Para um Parquet auto-descritivo (pandas/R/Spark, sem conceito de *value labels*):
```stata
decode varlab, generate(varlab_str)      // decode nativo da Stata
drop varlab
parqit open _data
parqit save out.parquet, replace data
```
(ou um `CASE WHEN` em `parqit sql` a mapear código→etiqueta.)

---

## 5. Fora de âmbito por desenho (6)

Intencionalmente **não** no `parqit` (o brief define-o como Parquet-only com
um motor único, DuckDB):

- **Ler SAS/SPSS/CSV** — `use_sas`/`use_spss`/`use_csv`, `merge_*`, `describe_*`.
- **Escrever SAS/SPSS/CSV** — `save_spss`/`save_csv`.
- **`format(sas|spss|csv)`** em todos os comandos — o `parqit` não precisa de `format()`, Parquet é o único.
- **`batch_size` / `infer_schema_length` / `parse_dates` / `fast` / `max_obs_per_batch` / `preserve_order`** — *knobs* internos Polars/CSV/SAS do `pq`; o `parqit` afina o motor com `parqit set threads/memory_limit/tempdir`.
- **`merge … force/sorted/nolabels/nonotes/noreport`** — contabilidade do `merge` de `.dta` em memória, sem análogo num JOIN SQL; o `parqit` junta por nome independentemente da ordem física.
- **`save … , do_not_reload`** — o `parqit save` (via *view*) nunca toca na memória da Stata, logo "não recarregar" já é o estado por omissão.
- **`compression(lzo|lto)`** — codecs específicos do Polars/`pq` que o escritor Parquet do DuckDB não expõe; o `parqit` rejeita codecs desconhecidos de forma alta (nunca substitui em silêncio).

---

## 6. Verificação

- **C++ unit:** 46/46 casos, 577 asserções.
- **Suites Stata:** 32/32 PASS (inclui o novo `v23_relaxed_union_by_name`);
  nenhuma invariante preexistente regrediu.
- **Comparação direta `parqit` vs `pq`** (`benchmarks/compare_parqit_vs_pq.do`,
  10M linhas): `use`/`save`/`merge`/`append`/`describe` dão **valores
  idênticos** (assinatura independente do tipo de armazenamento).

*Diffs desta ronda: `src/plugin/plugin_io.{hpp,cpp}` + `src/plugin/plugin_view.cpp`
+ `src/ado/p/parqit.ado` (opção `relaxed`); `src/ado/p/parqit.sthlp` e `README.md`
(doc); `ASSUMPTIONS.md` #40; `CHANGELOG.md`. Novo teste
`tests/verify_suite/v23_relaxed_union_by_name.do`.*
