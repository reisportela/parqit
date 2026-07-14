# Auditoria adversarial ao `parqit` — 2026-06-16

**Alvo:** working tree em `main` (HEAD `a9197cb` + alterações não-commitadas em
`src/engine/exprtrans.cpp`, `src/engine/session.cpp`, `src/plugin/plugin_view.cpp`).
**Plugin testado:** `build/dev/parqit.plugin` (compilado 16-jun 01:26, posterior às
fontes — inclui as alterações pendentes de `string()`/`substr()`/`query`).
**Oráculos independentes:** Stata 19.5 nativo, `pyarrow`, `duckdb` CLI.
**Natureza:** adversarial mas **não-destrutiva** — apenas leituras e execuções;
nenhum ficheiro do repositório foi alterado. Todas as sondas (`.do`) foram criadas
em `/tmp/parqit_adv/` para manter o repo intacto. Este relatório é o único ficheiro
novo no repo.

O método foi adaptativo (como pedido): cada área foi atacada com casos-limite,
comparando o `parqit` célula-a-célula com a verdade de referência do Stata nativo
(usando o **mesmo texto de expressão** nos dois lados para eliminar divergências de
transcrição); à medida que uma área se revelava sólida, o esforço migrava para a
seguinte de maior risco. A pista forte — *"problemas com strings ou missings ainda
não detetados"* — guiou o foco.

---

## 1. Sumário executivo

Foi encontrada **uma (1) falha substantiva**, de severidade **alta**, com
corrupção silenciosa de dados e violação direta da carta de correção (§6: erros
altos, fidelidade de round-trip, nunca rc 0 com ficheiro mau):

> **F-1 — `parqit save` corrompe silenciosamente strings com UTF-8 inválido
> (dados Latin-1 / legados, comuns no domínio do mantenedor).**

Todo o resto do que foi sondado **resistiu** — incluindo áreas onde auditorias a
pontes Stata↔colunar costumam falhar. O `parqit` mostra-se notavelmente defensivo:
o tradutor de expressões, a semântica de missing, o `collapse`, a ordenação, o
`merge`/`append` e a fidelidade numérica estão todos corretos e, quando perdem
informação, **avisam explicitamente**. Ver a matriz da §4.

| ID | Área | Severidade | Estado |
|----|------|-----------|--------|
| **F-1** | `save` de string com UTF-8 inválido | **Alta** | **Bug — corrupção silenciosa** |
| O-1 | `parqit save` grava a view aberta (não a memória) | Baixa | Por design, **com aviso** — não é bug |
| O-2 | Missings estendidos `.a–.z` colapsam para `.` no save | Baixa | Documentado + avisado |
| O-3 | Semântica SQL de missing em filtros difere do Stata por defeito | Baixa | Por design; `statamissing on` reproduz o Stata (verificado) |

---

## 2. F-1 — `parqit save` corrompe silenciosamente strings com UTF-8 inválido

### Severidade: **ALTA** (corrupção silenciosa de dados; gera ficheiros ilegíveis)

> **Estado: CORRIGIDO (2026-06-16).** Os dois caminhos de escrita validam agora
> cada célula com `parqit_is_valid_utf8` (`src/plugin/plugin_io.cpp`) e erram alto
> em `var[obs]` apontando para `unicode translate` — nunca rc 0 com ficheiro mau,
> e um save falhado não destrói o ficheiro pré-existente. Bloqueado pelo verify
> test `tests/verify_suite/v32_invalid_utf8_save.do`; suite completa (42/42) +
> unit tests (49/49) verdes. Detalhes em `CHANGELOG` e `ASSUMPTIONS` #49. A
> descrição abaixo documenta o estado original e a evidência.

### Resumo
Quando uma variável string do Stata contém bytes que **não são UTF-8 válido**
(p.ex. `é` em Latin-1 = `0xE9`, ou `0xFF`), `parqit save` **sucede com `rc 0` e sem
qualquer aviso**, mas produz um Parquet inválido. O comportamento depende do
caminho de escrita e **é corrupção silenciosa nos dois casos**:

* **Caminho por defeito (Arrow-scan, v0.1.6):** grava os bytes crus numa coluna
  cujo tipo lógico Parquet é `string` (UTF-8). O ficheiro resultante **não é
  legível por nenhuma ferramenta** — nem pelo próprio `parqit`.
* **Caminho fallback (`PARQIT_SAVE_NOARROW=1`, temp-table):** converte
  **silenciosamente** cada célula afetada para `NULL` — ficheiro legível, mas com
  **perda total** do valor.

Os dois caminhos produzem resultados **diferentes** para a mesma entrada, o que
contradiz a afirmação do `CHANGELOG` (`[0.1.6]`, linha 31): *"Output is
byte-identical … under both paths"*. A alegação só é verdadeira para a suíte de
verificação atual, que **nunca alimenta UTF-8 inválido**.

### Causa-raiz
O caminho de escrita em memória lê os bytes da string do Stata via `SF_sdata` /
`SF_strldata` e insere-os **diretamente** no buffer Arrow `utf8`, **sem validar
nem sanitizar UTF-8** (`src/plugin/plugin_io.cpp:1787`):

```cpp
c.bytes.insert(c.bytes.end(), strbuf.data(), strbuf.data() + len);
```

Existe imediatamente acima (`:1767`) uma guarda para strLs binárias
(`SF_var_is_binary` → erro alto `kRcUsage`), mas **não** há guarda equivalente
para UTF-8 inválido num `str#`/`strL` de texto. A sanitização `utf8_lossy()`
(→ U+FFFD) que o `parqit` já implementa em `src/engine/session.cpp:106` é usada
**apenas** no scalar `substr` (leitura), nunca no caminho de escrita.

### Reprodução (`/tmp/parqit_adv/utf8_save_probe.do`)
```stata
clear
set obs 3
gen strL s = ""
replace s = char(233) in 1                 // 0xE9  (Latin-1 'é', UTF-8 inválido isolado)
replace s = "ok" + char(195) + "x" in 2    // 0xC3  (lead byte sem continuação)
replace s = char(255) + char(254) in 3     // 0xFF 0xFE
parqit save "badutf.parquet", replace        // -> rc 0, "3 obs, 3 vars written"  (SEM AVISO)
parqit use using "badutf.parquet"            // -> rc 0
parqit collect, clear                        // -> rc 920: value "\xE9" is not valid UTF8!
```

### Evidência dos oráculos independentes
**Caminho Arrow (defeito):**
```
$ python3 -c "import pyarrow.parquet as pq; pq.read_table('badutf_arrow.parquet').column('s').to_pylist()"
UnicodeDecodeError: 'utf-8' codec can't decode byte 0xe9 in position 0
   (o esquema declara a coluna 's' como logical type = string/UTF-8)

$ duckdb -c "SELECT s FROM read_parquet('badutf_arrow.parquet')"
Invalid Input Error: Invalid string encoding found in Parquet file: value "ok\xC3x" is not valid UTF8!
```
**Caminho temp-table (`PARQIT_SAVE_NOARROW=1`):**
```
$ python3 -c "...read_table('badutf_noarrow.parquet').column('s').to_pylist()"
['None', 'None', 'None']        # as 3 células viraram NULL — perda total e silenciosa
```

### Porque importa (e porque não é um caso de laboratório)
O Stata 14+ é UTF-8 internamente, mas dados reais **frequentemente** contêm bytes
Latin-1/legados: importação de CSV/colunar sem `unicode translate`, `.dta`
pré-14, dados administrativos, ou construção via `char()`. O próprio Stata fornece
`unicode translate` precisamente porque isto é comum. No domínio do mantenedor
(dados portugueses/BPLIM) acentos em Latin-1 são um caso corrente. O efeito
prático: **`parqit save` de um dataset Stata legítimo pode produzir, em silêncio, um
Parquet que ninguém — nem o `parqit` — consegue reler.**

Isto viola três invariantes da carta de correção (§5–§6):
* **Erros altos:** rc 0 com um ficheiro que não pode ser lido (o oposto do
  contrato "nunca rc 0 com ficheiro stale/inválido").
* **Round-trip de tipos:** a propriedade de round-trip exige que "UTF-8/emoji"
  faça round-trip; uma string Latin-1 realista faz round-trip para um ficheiro
  partido (Arrow) ou para `NULL` (temp-table).
* **Nunca coluna silenciosamente toda-missing:** o caminho fallback transforma
  cada célula afetada em `NULL` sem aviso.

Não está documentado: `ASSUMPTIONS.md`, `README.md` e `parqit.sthlp` só referem
UTF-8 no contexto de *leitura* (a política U+FFFD do `substr`), nunca uma exigência
ou comportamento de UTF-8 válido na *escrita*.

### Correção sugerida (barata, padrão já existente no ficheiro)
No mesmo laço de `plugin_io.cpp` (~1766–1790), espelhar a guarda binária:
1. **Preferível — erro alto por célula:** validar UTF-8 ao ler cada string; se
   inválida, devolver `kRcUsage` com `"parqit save: <var>[<obs>] contains invalid
   UTF-8 (Parquet/Arrow strings must be valid UTF-8; run -unicode translate-)"`.
   Determinístico, sem perda silenciosa, alinhado com a guarda de strL binária.
2. **Alternativa — sanear com aviso:** passar cada string por `utf8_lossy()`
   (→ U+FFFD) e emitir um `note:` único por variável afetada (consistente com a
   política do `substr` e com os outros avisos lossy do save). Mais permissivo,
   mas perde os bytes originais.

Qualquer das opções torna os dois caminhos (Arrow / temp-table) coerentes outra
vez e restabelece a alegação "byte-identical under both paths".

---

## 3. Observações menores (não são bugs)

* **O-1 — `parqit save <ficheiro>` com uma view aberta grava a view, não a
  memória.** Confirmado, mas **por design e com aviso explícito**
  (`src/ado/p/parqit.ado:1237`: *"materialising view … — the dataset in memory is
  untouched; use the data option to export memory instead"*) e documentado
  (cabeçalho `:1204`). Não é silencioso. (Apanhou-me duas vezes durante a
  auditoria por eu não fechar a view entre secções — útil como lembrete de UX,
  não como defeito.)

* **O-2 — missings estendidos `.a–.z` colapsam para `.` simples no save.**
  Esperado (o Parquet tem um único conceito de missing); o ado **avisa**
  (`:1217`, *"were written as nulls"*). Confirmado empiricamente: `.a`/`.b` →
  `.` após `save→use→collect`, com `missing()` verdadeiro. Perda documentada.

* **O-3 — por defeito os filtros/comparações usam semântica de missing do SQL,
  que difere do Stata.** Ex.: `keep if x > 15` com `x` missing — o Stata mantém a
  linha (missing > 15 é verdadeiro), o `parqit` por defeito descarta-a. É **por
  design** (a carta manda "default SQL missing semantics"); `parqit set
  statamissing on` reproduz o Stata e foi **verificado correto** (ver §4). Vale
  como nota de portabilidade para quem traz código Stata.

---

## 4. O que resistiu (matriz de robustez)

Tudo abaixo foi testado adversarialmente e **bateu com o Stata nativo** (ou com o
oráculo apropriado). Esta secção documenta a amplitude coberta.

### Strings (`/tmp/parqit_adv/strings_probe.do`) — 35 casos, **0 bugs**
| Domínio | Casos | Resultado |
|---|---|---|
| `substr` posicional | `p=0`, `p` negativo, `p` além do fim, `n` negativo, `n=0`, `n` a transbordar | exato (incl. `substr(s,0,n)=""`, idêntico ao Stata) |
| `substr` por byte | `substr("café",4,2)="é"`; corte a meio de codepoint → **U+FFFD** (documentado) | exato |
| `strlen` vs `ustrlen` | byte vs caractere em `"café"` (5/4) e `"a😀b"` (6/3) | exato |
| `strpos` | agulha vazia (`=1`), sem match (`0`), offset multibyte, agulha > palheiro | exato |
| `upper`/`lower`/`ustrupper` | dobra só-ASCII vs Unicode em `"café"` | exato (`upper("café")="CAFé"`) |
| `trim`/`ltrim`/`rtrim`, `subinstr` (replace-all), `subinstr` com `from` vazio | — | exato |
| `string()`/`strofreal()` | `1e100`→`1.0e+100`, `1/3`→`.3333333`, `123456.789`→`123456.8` | exato (paridade `%9.0g`) |
| `real()`, `regexm`, concatenação `+` | incl. `real("12abc")=.` | exato |

### Missings e ordenação (`/tmp/parqit_adv/missings_probe.do`)
* **`sort` e `gsort -x`** — colocação de missing: `[4,5]` (último) em ambas as
  direções, **igual ao Stata** (confirma que o `NULLS LAST` incondicional de
  `view.cpp:61` está correto — o `gsort -x` do Stata também põe missing por
  último).
* **`statamissing on`** reproduz o Stata em `keep if x>15` (4 linhas vs 2 no
  defeito SQL) e em `gen f=(x>15)` célula-a-célula.
* **Limpeza atómica** mantida: o `collect` que falhou (F-1) **preservou** os dados
  em memória — invariante "validate-then-mutate" verificada.

### `collapse` (`/tmp/parqit_adv/collapse_probe.do`) — **0 bugs**
Grupo com **primeira linha missing** + grupo **todo-missing**, em numérico e
string, todos a bater com o Stata:
* `(first)`/`(last)` **incluem** missing (truque `arg_min({'v':x}, rn)['v']`
  funciona — não salta o NULL), distintos de `(firstnm)`/`(lastnm)`.
* grupo todo-missing: `sum=0` (convenção Stata via `coalesce`), `mean/sd/min/max/
  median/first=.`, `count=0` — exato.
* strings: `(first)=""`, `(firstnm)="b"`, `(last)="c"` com primeira célula vazia.

### Fidelidade numérica (`/tmp/parqit_adv/edge_num_probe.do`) — **0 bugs**, avisos corretos
* doubles não-representáveis (`±1e308`, `1.5e308`), `NaN`, `±Inf` → `.` missing
  (sem lixo, sem colisão com código de missing estendido errado), **com aviso**:
  `note: d: 5 value(s) outside Stata's storable range stored as missing`.
* `8e307` e `2^53` preservados exatamente.
* `int32` max/min (`±2147483648`) → `double` exato (evita colisão com os códigos de
  missing do `long`; ASSUMPTIONS #5 a funcionar).
* `int64` > 2^53 → `double`, **com aviso**: `note: i64: values beyond 2^53 rounded
  to nearest double`.

### Metadata, ordenação de strings, two-table (`/tmp/parqit_adv/meta_order_probe.do`, `append_probe.do`)
* **Value/var labels com acentos** (`café`, `naïve`, `preço médio`) → round-trip
  exato. Label com UTF-8 **inválido** não corrompe o pipeline (save/read rc 0) —
  o caminho de metadata é mais robusto que o de dados de coluna.
* **Ordenação/collation de strings** = ordem de byte do Stata (`sort s` e
  `keep if s < "a"` coincidem).
* **`merge 1:1` com chave de tipo incompatível** (int vs string) → **erro alto
  rc 198**, view sobrevive. Sem join-errado silencioso.
* **`append` numérico-vs-string** → **erro alto rc 198** (`plugin_io`/`view.cpp:916`
  guardam o conflito de `kind`); **`append` byte→double** → coerção numérica limpa.

---

## 5. Recomendações

1. **Corrigir F-1** antes de qualquer release SSC: adicionar validação de UTF-8 no
   laço de escrita de strings (`plugin_io.cpp` ~1766–1790), preferencialmente como
   **erro alto por célula** (espelhando a guarda de strL binária). Acrescentar um
   teste `verify_suite` que (a) tente `parqit save` de `char(233)`/`char(0xFF)` e
   exija rc≠0 + mensagem, ou rc 0 com U+FFFD confirmado por oráculo pyarrow, e
   (b) verifique que os dois caminhos (Arrow / `PARQIT_SAVE_NOARROW=1`) concordam.
2. **Ajustar a alegação do `CHANGELOG`** ("byte-identical under both paths") ou
   garanti-la cobrindo o caso UTF-8 inválido no teste de capacidade já existente.
3. **Documentar** a política de UTF-8 na escrita em `ASSUMPTIONS.md` (hoje só há a
   nota de leitura no `parqit.sthlp:412`).

## 6. Artefactos de reprodução
Todas as sondas estão em `/tmp/parqit_adv/` (fora do repo):
`strings_probe.do`, `missings_probe.do`, `collapse_probe.do`, `utf8_save_probe.do`,
`meta_order_probe.do`, `edge_num_probe.do`, `append_probe.do`, mais os Parquet
`badutf_*.parquet`, `edge_dbl.parquet`, `edge_int.parquet`. Cada `.do` recebe
`repo` e `plugin` como argumentos e imprime linhas `VERDICT(...)`.
