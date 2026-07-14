# Relatório comparativo — `parqit` vs `pq` para ficheiros Parquet em Stata

**Precisão · robustez (erros possíveis) · tempo de computação**

**Autor:** Claude (Opus 4.8), a pedido de Miguel Portela · **Data:** 2026-06-15
**Objeto:** comparação empírica e independente entre os dois pacotes Stata que
leem/escrevem Parquet — `parqit` (motor DuckDB embebido) e `pq`
(`stata_parquet_io`, motor Rust/Polars).

> **Como ler este relatório.** Todos os números de tempo e todas as afirmações
> de (in)correção foram **medidos** nesta máquina hoje; nenhum é estimado. A
> verificação de valores usa sempre um **oráculo independente** (a biblioteca
> Python `pyarrow`, alheia aos dois pacotes), nunca um round-trip do próprio
> pacote. Os ficheiros `.do` e `.py` que reproduzem cada resultado estão em
> [benchmarks/_out/precision/](benchmarks/_out/precision/) e listados no Anexo A.

---

## 0. Ambiente e versões

| Componente | Versão |
|---|---|
| `parqit` | 0.1.5 (motor: DuckDB v1.5.3, Stata Plugin Interface 3.0) |
| `pq` (`stata_parquet_io`) | 3.0.7 |
| Stata | StataNow 19.5 MP (licença 16-core) |
| Oráculo independente | Python 3 + `pyarrow` |
| Máquina | AlmaLinux 9.7 · 48 cores · ~1 TiB RAM |
| Dados de tempo | sintéticos `benchmarks/_out/synthetic_medium_data/` (trabalhadores 10M×13, firmas 500k, patentes 1M) |

---

## 1. Sumário executivo

1. **Onde os dois produzem o mesmo resultado, produzem valores idênticos.** Em
   `use`/`save`/`merge`/`append`/`describe` sobre os mesmos dados, a assinatura
   de valores (independente do tipo de armazenamento) é **IGUAL** nos dois.

2. **`parqit` é 4–6× mais rápido** a ler (`use`), a gravar Parquet→Parquet
   (`save` em passagem), e em `merge` out-of-core; **empata** no `append` e no
   `describe`. A **única** operação em que `parqit` é mais lento é o `save` de
   dados **alterados em memória** (8,1 s vs 6,1 s) — explicado e justificado na
   §2.

3. **`parqit` é substancialmente mais robusto na fidelidade de tipos.** Confirmei
   **ao vivo, hoje, em `pq` 3.0.7**, com oráculo independente, **quatro
   corrupções silenciosas** (rc 0, dados errados no disco) que `pq` comete e
   `parqit` não (§3): datas de período `%tm/%tq/…`, inteiros `uint32 ≥ 2³¹`,
   colunas `decimal`, e formatos de hora `hh:mm`. `pq` também **perde as
   etiquetas de valor**; `parqit` preserva-as.

4. A auditoria de correção de 2026-06-11 (sobre `pq` 3.0.0) documenta **14
   perigos**, quase todos silenciosos; **cada um** tem um teste de invariante
   correspondente no `parqit` que prova que `parqit` é imune por desenho (§3.3).

5. **Honestidade sobre o `parqit`:** tem limitações próprias bem definidas
   (§4) — sobretudo o `save` de dados em memória ser mais lento, e os *missing*
   estendidos `.a–.z` colapsarem para um único *missing* (mas com **aviso**).

> **Resumo de uma linha:** para o trabalho típico em Parquet, `parqit` é mais
> rápido **e** mais seguro; a exceção de velocidade é gravar para Parquet dados
> que foram editados dentro da Stata, onde `pq` é ~25 % mais rápido.

---

## 2. Tempo de computação

10 milhões de observações × 13 variáveis, mínimo de 3 execuções intercaladas
(mesma carga), resultado **em memória** nos dois casos para ser «maçã com maçã».

| Operação | `pq` 3.0.7 | `parqit` 0.1.5 | rácio pq/parqit | valores |
|---|---:|---:|---:|:--:|
| **`use`** (ler Parquet → memória) | 6,569 s | **1,334 s** | **4,9×** | IGUAIS |
| **`save`** — dados **não alterados** após `use` | 6,193 s | **1,100 s** | **5,6×** | IGUAIS |
| **`save`** — dados **alterados** em memória | 6,070 s | 8,126 s | 0,75× | IGUAIS |
| **`describe`** (só metadados) | 0,017 s | **0,009 s** | ~1,9× | — |
| **`merge m:1`** (+ `collect`) | 9,753 s | **2,280 s** | **4,3×** | IGUAIS |
| **`append`** | 0,314 s | **0,266 s** | 1,2× | IGUAIS |

**As duas vias do `save` no `parqit`.** O `parqit save … , data` tem duas vias:

- **Via rápida (passagem):** se os dados em memória ainda são o resultado
  *intacto* de um `parqit use … , clear` de um único Parquet, `parqit` grava
  copiando diretamente do ficheiro de origem pelo motor (DuckDB `COPY`), sem
  reler célula a célula. → **1,1 s (5,6× mais rápido que `pq`).** É o caso de
  conversão/reescrita de ficheiros.
- **Via geral:** se os dados foram **modificados** na Stata, `parqit` tem de os
  reler da memória pela Stata Plugin Interface (SPI), célula a célula. → **8,1 s
  (mais lento que os 6,1 s do `pq`).**

**Porque é que a via geral é mais lenta — e porque não a aceleramos.**
Investigámos a fundo. A leitura célula a célula pela SPI é o chão de custo: a
Stata MP **serializa internamente** o acesso a dados pela SPI (`SF_vdata`), pelo
que paralelizar não ajuda (~15 % a 16 *threads*); e os acessores de *string* da
SPI **não são reentrantes** — chamá-los de várias *threads* corrompe a *heap*
(confirmado por *crash*). Tentou-se ainda uma ponte de extração em bloco via
Mata para `/dev/shm` (tmpfs); é **byte-a-byte correta** mas medida **mais
lenta** em todas as variantes (a sobrecarga da transferência excede o que poupa,
porque `SF_vdata` é, afinal, rápido, ~15 ns/célula). Conclusão: a via geral está
no chão da SPI; a vantagem do `pq` aqui vem de um acesso à memória da Stata que
não é exposto pela interface pública de *plugins*.

**Recomendação prática.** Para gravar resultados de manipulação, prefira manter
os dados como *view* do `parqit` e usar os verbos *lazy* (`parqit open _data` →
`keep`/`gen`/`collapse`/… → `parqit save`): nesse caminho a gravação é
**out-of-core e não relê a memória da Stata** (rápida). A via lenta só é atingida
quando se edita com comandos nativos da Stata e depois se grava.

---

## 3. Precisão e fidelidade de tipos

### 3.1 Igualdade de valores

Nas operações comuns (§2), a **assinatura de valores** (todas as numéricas
recodificadas para `double`, ordenação determinística, `datasignature`) é
**idêntica** entre `parqit` e `pq`. Onde ambos funcionam, ambos dão o mesmo.

### 3.2 Onde divergem — quatro corrupções silenciosas do `pq` (confirmadas ao vivo em 3.0.7)

Estas foram **medidas hoje** com `pq` 3.0.7 e verificadas com `pyarrow` a ler o
ficheiro no disco. Em todos os casos `pq` devolve **rc 0** — o utilizador não
recebe erro; os dados ficam silenciosamente errados.

#### (a) Datas de período `%tm`/`%tq`/`%tw`/`%th`/`%ty` → datas de calendário erradas

Variável mensal `month = 2018m1` (`%tm`), trimestral `quarter = 2010q1` (`%tq`):

| | no disco (oráculo `pyarrow`) | round-trip de volta à Stata |
|---|---|---|
| **`parqit`** | `month` = `int32` **696**, `quarter` = `int32` **200** (a contagem de períodos correta) | **`2018m1` (`%tm`)**, **`2010q1` (`%tq`)** — exato |
| **`pq`** | `month` = `date32` **1961-11-27**, `quarter` = `date32` **1960-07-19** | **`27nov1961` (`%td`)**, **`19jul1960` (`%td`)** — **corrompido** |

`pq` aplica o desfasamento de **dias** a uma contagem de **meses/trimestres**.
O dado fica errado para qualquer leitor (pandas/R/Spark), e **nem o próprio `pq`
o recupera** — o round-trip `pq`→`pq` devolve a data errada, não `2018m1`. Como
variáveis mensais/trimestrais são omnipresentes em dados de painel, é o achado
mais consequente. `parqit` mantém a contagem inteira no disco e o formato `%t*` nos
metadados `parqit.*`, com round-trip perfeito.

#### (b) `uint32 ≥ 2³¹` → *missing*

Coluna `uint32` com valores `[1, 2147483648, 4294967295, 0, 100]`:

| | valores lidos |
|---|---|
| **`parqit`** | `1, 2147483648, 4294967295, 0, 100` — **corretos** (em `double`) |
| **`pq`** | `1, ., ., 0, 100` — **os valores ≥ 2³¹ tornam-se *missing*** |

#### (c) Colunas `decimal` → tudo *missing* / ilegível

Coluna `decimal(18,3)` = `[1.5, 2.25, -3.75, 0, 1000000.125]`:

| | resultado |
|---|---|
| **`parqit`** | `1.500, 2.250, -3.750, 0.000, 1000000.125` — **preservados** (convertidos para `double`, **com aviso**: «decimal converted to double») |
| **`pq`** | `Undefined parquet type: decimal[18,3]`; a coluna fica **toda *missing*** |

#### (d) Formato de hora `hh:mm` → coluna inteiramente nula

Variável `%tcHH:MM:SS` com 3 horas válidas (`08:30`, `09:30`, `10:30`):

| | no disco (oráculo) |
|---|---|
| **`parqit`** | `2026-01-01 08:30`, `09:30`, `10:30` — **timestamps corretos** |
| **`pq`** | `None, None, None` — **coluna totalmente perdida** |

#### (e) Etiquetas de valor e *missing* estendidos

- **Etiquetas de valor:** `parqit` **preserva-as** (ex.: `sex` mantém o rótulo
  `lblsex` ao reler); `pq` **perde-as** (lê `sex` como 0/1 sem rótulo).
- **Missing estendidos `.a`–`.z`:** ambos colapsam para um único *missing* (o
  Parquet só tem um conceito de ausência), mas **`parqit` emite um aviso alto** —
  *«note: extended missing values (.a-.z) in `wage` were written as nulls»* —
  enquanto `pq` é silencioso. As **definições** das etiquetas de *missing*
  sobrevivem nos metadados `parqit.*`.
- **Datas `%td`, `float`/`double`, `strings`/unicode (`São_β`):** corretos nos
  dois.

### 3.3 Mapa sistemático dos 14 perigos do `pq` (auditoria 2026-06-11) e o guarda do `parqit`

A auditoria de correção `pq_audit_2026-06-11` (sobre `pq` **3.0.0**) reproduziu
14 achados; cada um é um perigo *independente do motor* (vale para qualquer ponte
Stata↔colunar). A coluna **«ao vivo 3.0.7»** marca os que **eu re-confirmei
hoje** na versão instalada. Os restantes estão documentados para 3.0.0 — convém
re-verificar antes de citar como atuais.

| # | Perigo no `pq` | Silencioso? | ao vivo 3.0.7 | Guarda no `parqit` (teste de invariante) |
|---|---|:--:|:--:|---|
| 1 | `save` com varlist reordenado/subconjunto, ou varlist+`if()`/`partition_by()`/`format()` → troca/baralha colunas; varlist+`if()` grava ficheiro de 0 linhas | sim | **não** (reordenação simples **correta** em 3.0.7) | manifesto de colunas indexado por **nome de origem**, nunca por posição (ASSUMPTIONS #11) |
| 2 | Coluna com nome a sanitizar (`if`, `1x`, >32 car.) carrega **toda *missing*** | sim | (não testado ao vivo) | `v02_renamed_columns` — valores corretos sob renomeação documentada |
| **3** | **`%tm/%tq/…` gravados como datas de calendário erradas** | **sim** | **SIM** (§3.2a) | `v03_period_dates` — contagem inteira no disco, formato nos metadados |
| 4 | `chunk()`+`partition_by()` guarda só o último *chunk* | sim | (não testado) | `parqit` grava ficheiro único atómico / árvore final direta; `v08` |
| **5** | **Formato `hh:mm` → coluna inteiramente NULL** | **sim** | **SIM** (§3.2d) | `v05_hhmm_datetime` — classe vem do **prefixo** do formato |
| **6** | **`uint32 ≥ 2³¹` → *missing*** | **sim** | **SIM** (§3.2b) | `v06_uint32_overflow` — `uint32` chega como número |
| 7 | `save, label` grava `""` para valores sem rótulo; `.a "rótulo"` vira a *string* `"rótulo"` | sim | (não testado) | `v07_label_fidelity` — valores numéricos no disco, rótulos em metadados |
| 8 | Falhas de `save` no *plugin* devolvem **rc 0** | semi | (não testado) | `v08_save_errors_loud` — falha = rc≠0 **e** mensagem |
| 9 | `use, clear` **destrói** os dados em memória se a leitura falhar | rc alto, mas dados já perdidos | (não testado) | `v09_atomic_clear` — *stage*-and-swap; dados sobrevivem a falhas |
| 10 | Nomes de coluna duplicados: todas menos a última são descartadas | sim | (não testado) | `v10_duplicate_columns` — desambiguadas com aviso |
| **11** | **Tipos não suportados (decimal, list, struct) → tudo *missing*** | **sim** | **SIM** (§3.2c) | `v11_unsupported_types` — decimal→double **com valores**; list/struct caem **com erro** |
| 12 | Coluna do utilizador `_pq_strl_key` silenciosamente descartada | sim | (não testado) | `v12_internal_names` — auxiliares são *tempnames* verificados contra o esquema |
| 13 | `in()` que a Stata rejeitaria → 0 linhas (ou **todas**) em silêncio | sim | parcial (parqit valida: rc≠0) | `v13_in_ranges` — intervalos inválidos são erro explícito |
| 14 | Um **espaço** num nome de coluna torna o ficheiro **ilegível** (rc 198) | alto | (não testado) | `v02`/`v16` — nomes em listas compound-quoted, hex-encoded |

Além destes, o `parqit` traz testes adversariais **sem equivalente no `pq`**:
`v15_float_extremes` (NaN/±Inf/alargamento de `float32`), `v16_injection_hostile`
(nomes em forma de injeção SQL, *bytes* NUL), `v17_locale_dp_comma` (vírgula
decimal), `v18_wide_2500_vars`, `v19_strl_boundary`, e `v20`–`v30`.

---

## 4. Limitações do `parqit` (lista honesta)

Para uma comparação justa, eis o que o `parqit` **não** faz ou faz de forma
deliberadamente diferente (documentado em `README.md`, `ASSUMPTIONS.md`,
`parqit.sthlp`):

- **`save` de dados editados em memória é mais lento** que o `pq` (§2). É o
  chão da SPI; a alternativa rápida é manipular por *views*/verbos.
- **Missing estendidos `.a`–`.z` colapsam para um único `.`** no Parquet (com
  **aviso**). A identidade `.a` vs `.b` por célula **não** sobrevive ao
  round-trip; só as definições de etiqueta sobrevivem (contrato v1 — ASSUMPTIONS #13).
- **`strL` binários (BLOB) são recusados na gravação** (erro alto). `strL` de
  texto faz round-trip; não há via BLOB na v1 (ASSUMPTIONS #12).
- **NULL ≡ `""` para *strings*** — a Stata não tem *string missing*; na
  gravação emite-se `""`, nunca NULL (assimetria documentada).
- **Semântica de *missing* em expressões é a do SQL por omissão** (NULL); para
  emular a ordenação da Stata («missing é maior que tudo») há `parqit set
  statamissing on`.
- **`merge m:m`** reproduz o emparelhamento sequencial da Stata — quase nunca é
  o que se quer (igual à Stata).
- **`decimal` → `double`** (com aviso): preserva o valor, mas não a aritmética
  decimal exata.
- **Precisão temporal:** `%tc` é inteiro de ms; `TIMESTAMP(us/ns)` é truncado de
  forma determinística; inteiros > 2⁵³ arredondam para `double` (com aviso).
- **`save` particionado** escreve direto na árvore final (não temp-then-rename):
  uma falha a meio é **alta** (rc≠0) mas pode deixar uma árvore parcial a
  remover. O `save` de ficheiro único é totalmente atómico.
- **Fontes não-Parquet:** lê CSV out-of-core (sem metadados de rótulos) e faz
  ponte de `.dta`/`.xls(x)` pequenos via Parquet temporário; **SAS/SPSS estão
  fora de âmbito** (o `pq` lê-os).

---

## 5. Conclusão e recomendações

- **Para ler, converter e manipular Parquet out-of-core:** o `parqit` é a escolha
  mais rápida (4–6×) **e** mais segura (evita, por construção, as corrupções
  silenciosas do `pq`). É especialmente relevante para **dados de painel**
  (datas `%tm/%tq`) e **identificadores grandes** (`uint32`), onde o `pq` 3.0.7
  corrompe em silêncio.
- **Para gravar dados editados dentro da Stata:** se a velocidade for crítica e
  os dados forem numéricos, o `pq` é ~25 % mais rápido nesse passo específico;
  caso contrário, manipular por *views* do `parqit` e gravar é mais rápido e
  preserva tudo.
- **Para ler SAS/SPSS** ou escrever SAS/SPSS/CSV: o `pq` cobre isso; o `parqit`
  é Parquet-only.

A regra de segurança: **um pacote que devolve rc 0 mas grava dados errados é
mais perigoso do que um que é lento.** O `parqit` foi desenhado para que cada
perda de informação seja **impossível em silêncio** — ou é exata, ou é um erro
alto, ou é um aviso explícito.

---

## Anexo A — Reprodução

Todos os ficheiros estão em [benchmarks/_out/precision/](benchmarks/_out/precision/)
e [benchmarks/](benchmarks/):

- **Tempo:** [benchmarks/compare_parqit_vs_pq.do](benchmarks/compare_parqit_vs_pq.do)
  (correr secção a secção); para o `save` geral, editar para invalidar a via
  rápida com `char _dta[_parqit_fast_source_nonce] ""`.
- **Precisão de escrita** (§3.2a, e): `prec_write.do` (grava por `parqit` e `pq`)
  + oráculo `pyarrow` que inspeciona os dois ficheiros no disco; `prec_read.do`
  (round-trip de volta pelos dois leitores).
- **Precisão de leitura** (§3.2b, c): `edge_read.parquet` (criado por `pyarrow`
  com `uint32`, `int64`, `date32`, `decimal`) lido por `parqit` e `pq`.
- **Perigos do `pq`** (§3.2a–d, confirmados ao vivo): comandos mínimos por achado
  na auditoria `/home/mangelo/Documents/BPLIM_GitHub/pq_audit_2026-06-11/issues/`.

**Método do oráculo:** nunca se confia no round-trip do próprio pacote (mascara
erros simétricos, como em §3.2a); lê-se sempre o ficheiro no disco com `pyarrow`,
que é independente do `parqit` e do `pq`.

---

*Documento gerado a partir de medições reproduzíveis na máquina de
desenvolvimento (AlmaLinux 9.7, 48 cores). Nenhum número foi estimado. As
afirmações sobre `pq` 3.0.0 provêm da auditoria de 2026-06-11; as marcadas «ao
vivo 3.0.7» foram re-medidas hoje na versão instalada.*
