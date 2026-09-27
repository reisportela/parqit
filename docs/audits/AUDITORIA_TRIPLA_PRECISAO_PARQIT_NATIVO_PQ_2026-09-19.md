# Auditoria tripla de precisão: parqit vs Stata nativo vs pq — 2026-09-19

**Resultado.** Nas operações que os três sabem fazer — `merge` (1:1, m:1, 1:m),
`append`, leitura e escrita — o `parqit` reproduz o Stata nativo **célula a
célula**, incluindo o tipo, os códigos e o rótulo de `_merge`. Em três pontos o
`parqit` é **mais exato do que o Stata nativo**, verificado contra aritmética de
precisão arbitrária e não contra outra implementação. Em seis pontos o `pq`
perde ou corrompe dados que o `parqit` preserva: um devolve uma coluna inteira a
omissos com `rc=0`, e outro escreve no ficheiro uma contagem de meses como se
fosse uma data, o que faz qualquer leitor que não seja o `pq` ler **1962 em vez
de 2026**. Ficam identificados **dois riscos reais do
`parqit`**, ambos anunciados pelo próprio programa mas com consequências que a
documentação não segue até ao fim, e um ponto em que o `pq` é mais protetor.

Nenhum ficheiro do produto foi alterado. Esta auditoria é de leitura e execução;
as correções propostas ficam na §7 para decisão do maintainer.

## 1. Base

| | |
|---|---|
| Commit | `40dd997` (árvore com alterações locais não commitadas) |
| parqit | v0.1.37 — `parqit.ado` SHA256 `771ec610…`, plugin `57f462f1…` (43 898 416 bytes) |
| pq | v4.0.2 — `pq.ado` SHA256 `8559c913…`, plugin `b54ae198…` |
| Stata | StataNow MP 19.5, Linux, licença 16 núcleos |
| Oráculos | pyarrow 24.0.0, duckdb CLI v1.5.0-dev3845, Python `Fraction`/`Decimal` (120 dígitos) |

## 2. Método

**Três pernas, e a decomposição que as torna informativas.** O `pq merge` lê o
*using* para uma frame e corre o `merge` **nativo**. Logo:

- `pq` vs nativo isola a **camada de leitura** do `pq`;
- `parqit` vs nativo isola a **semântica de junção** do `parqit` e o `collect`.

O Stata nativo sobre os mesmos dados em `.dta` é a referência semântica dos
verbos; para as estatísticas a referência é a aritmética exata, que **prevalece
sobre o nativo**.

**O oráculo em disco.** Os mesmos dados em memória são escritos pelos dois
(`parqit save` e `pq save`, com e sem `statametadata`) e os ficheiros são lidos
pelo **pyarrow**, que não é nenhum dos dois. Confirma o tipo físico de cada
coluna, o payload célula a célula e os metadados chave-valor
(`oracle_disk.txt`). É o que separa «o valor volta bem ao Stata» de «o ficheiro
diz a verdade a terceiros» — e foi aí que apareceu T20b. Cada leitor lê também o
ficheiro do outro, o que testa a interoperabilidade sem confiar em nenhum
round-trip de uma só implementação.

**O comparador.** Nunca se compara um resumo (média, `datasignature`) quando se
pode comparar a célula. Os numéricos vão a `double` (float→double é exato) e
comparam-se com `==`, que em Stata é bit-exato e distingue `.` de `.a`; as
strings comparam-se byte a byte. Cada comparação produz duas passagens — **na
ordem produzida** e **como multiconjunto** — o que separa «dados diferentes» de
«mesmos dados noutra ordem». O tipo de armazenamento é reportado à parte do
valor: o `parqit` dimensiona ao menor tipo exato e o `pq` faz `compress`, pelo
que um tipo diferente não é, por si, um erro; um valor diferente é.

**Dados.** Fixtures sintéticos pequenos (≤ 20 linhas), todas as linhas
inspecionáveis, mais sete ficheiros Parquet **estrangeiros** escritos por
pyarrow com tipos e magnitudes que o Stata não representa: `int64` acima de
2^53, `uint64` acima de 2^63, `decimal(38,10)`, extremos IEEE, texto UTF-8 na
fronteira `str#`/`strL`, nulos em todas as colunas, e nomes que diferem só por
caixa.

## 3. Evidência

Tudo é reexecutável em [`audit_repro/triple_precision_20260919/`](../../audit_repro/triple_precision_20260919/):

| Ficheiro | O que faz |
|---|---|
| `_cmp.do` | o comparador exato descrito acima |
| `a01_merge_core.do` | merge m:1, 1:1, 1:m, `keep()`/`keepusing()`, `_merge`, chave duplicada, m:m, using vazio |
| `a02_order.do` | ordem das linhas, determinismo, marcador `sortedby`, empates, instabilidade do nativo |
| `a03_types.do` | 4 combinações escritor × leitor + os sete ficheiros hostis |
| `a04_probe.do` | adjudicação das divergências de `a03` contra os limites do próprio Stata |
| `a05_keys.do` | chaves omissas, omissos estendidos, texto, BIGINT |
| `a06_keys2.do` | teste simétrico do colapso `.a`→`.` e junção lazy sobre BIGINT |
| `a07_append.do` | alargamento de tipos, coluna ausente, guarda de opções |
| `a08_stats.do` | `collapse` e `summarize, detail` contra o nativo |
| `a09_metadata.do` | rótulos, formatos, notas, características e interoperabilidade cruzada |
| `a10_oracle.do` | verifica o rótulo de `_merge` e `string()` no nativo; escreve os pares de ficheiros de `oracle/` |
| `mk_hostile.py` | gera os ficheiros estrangeiros (pyarrow) |
| `mk_oracle.py` → `oracle_disk.txt` | o pyarrow lê `oracle/*.parquet`: tipo físico, valores e metadados chave-valor |
| `run_audit.sh` | corre cada família num processo Stata limpo; logs em `logs/` |
| `baseline.json` | identidade de código, plugins, Stata, oráculos e SHA256 de cada ficheiro |
| `oracle_stats.txt` | a aritmética exata que adjudica a §5 |

Execução completa: 10 famílias, 39 veredictos, 33 `PASS` e 6 `FAIL`. **Os seis `FAIL` estão
todos adjudicados abaixo**: quatro são a perda de omissos estendidos partilhada
pelas duas implementações (T13), um é o risco T06, e um — `collapse` — é o
Stata nativo a errar (T08). O comparador está certo em assinalá-los; a
adjudicação é que decide de que lado está o erro.

## 4. Onde o parqit reproduz o nativo exatamente

**T01 — `merge` + `collect` = `merge` nativo.** Em `m:1`, `1:1`, `1:m`, com
`keep(match)`, com `keepusing()` e com um *using* de zero linhas, os valores são
idênticos célula a célula. O `_merge` volta como `byte`, com os mesmos códigos
(`n1=2`, `n3=10`) e o mesmo rótulo de valor (`Master only (1)`, `Matched (3)`).
A variável não-chave presente nos dois lados segue a regra nativa. O `pq`
também, como esperado de quem delega no `merge` nativo.

**T02 — chaves omissas seguem o Stata, não o SQL.** Em SQL `NULL = NULL` é
falso; em Stata omisso casa com omisso. Com chaves `{1, ., 2}` e `{1, ., 3}` as
três pernas dão `matched=2`. Chaves de texto vazias, com espaço final e
acentuadas: `matched=3` nas três.

**T03 — `append`.** Alargamento de tipos (`byte`+`double`→`double`,
`int`+`long`→`long`, `str5`+`str20`→`str20`) e coluna presente só num dos lados
(preenchida com omissos): os três idênticos ao nativo, valores bit-exatos. Uma
opção inexistente (`parqit append …, relaxed`) falha alto com `rc=198` — não é
ignorada em silêncio.

**T04 — `m:m`.** O `parqit` **recusa** o `m:m` lazy (`rc=198`), como está
documentado: um plano lazy não retém a ordem física dentro da chave de que a
regra sequencial do Stata depende. O `parqit mergein m:m` — que corre o `merge`
nativo — reproduz o nativo célula a célula **e na mesma ordem**. O `pq`, que
delega, também.

**T05 — `collect` puro preserva a ordem do ficheiro.** Sem verbos, o `collect`
devolve o ficheiro linha a linha, na ordem física (`ord EQUAL`). O `pq` também.

## 5. Onde o parqit é mais exato do que o Stata nativo

Estes três resultados foram adjudicados contra `Fraction`/`Decimal` com 120
dígitos, não contra o nativo. Em dados ordinários as duas implementações
coincidem bit a bit; a divergência só aparece onde a acumulação em `double`
quebra.

**T06 — soma com cancelamento catastrófico.** Para o grupo
`{1e16, 1, -1e16, 1}` a soma exata é **2**.

| | soma | média |
|---|---|---|
| exato | `2` | `0.5` |
| **parqit** | `+1.0000000000000X+001` = **2** ✔ | `+1.0000000000000X-001` = **0.5** ✔ |
| Stata nativo | `+1.0000000000000X+000` = 1 ✘ | `+1.0000000000000X-002` = 0.25 ✘ |

O nativo acumula da esquerda para a direita: `1e16 + 1` perde o `1` (o *ulp* de
`1e16` é 2), e um dos dois uns desaparece.

**T07 — desvio-padrão cuja variância não cabe num `double`.** Para
`{1e300, -1e300, 2.5, -2.5}` a variância exata é ≈ `6.67e599` — não
representável — mas o desvio-padrão, `8.16496580927726033e+299`, é. O nativo
devolve **omisso**; o `parqit` devolve `+1.381e2dab5cdc2X+3e4`, cuja mantissa é
**idêntica** à do valor exato corretamente arredondado.

**T08 — `summarize, detail`.** Sobre as mesmas 14 observações:

| momento | valor exato | parqit | Stata nativo |
|---|---|---|---|
| desvio-padrão | `3.92232270276368064e+299` (mantissa `1.2bdf87a584ee6`) | `+1.2bdf87a584ee6X+3e3` ✔ | omisso ✘ |
| assimetria | `-6.40649781750640148e-300` (mantissa `1.1295af0a04445`) | `-1.1295af0a04445X-3e2` ✔ | omisso ✘ |
| curtose | exatamente `7` | `+1.c000000000000X+002` = 7 ✔ | omisso ✘ |

> **Aviso ao leitor e a quem mantiver o código:** quem comparar o `parqit` com o
> `collapse`/`summarize` nativos nestes dados vai ver uma diferença. A diferença
> é o nativo a transbordar. **Não «corrigir» o `parqit` na direção do nativo.**

**T09 — a junção lazy é exata sobre `BIGINT`.** Com chaves `2^53` e `2^53+1` —
indistinguíveis num `double` — o `parqit merge 1:1` junta corretamente `A↔10` e
`B↔20`, porque a junção corre **no motor, antes do `collect`**. O mesmo ficheiro
lido para memória colapsa as duas chaves numa só (`duplicates report` passa de 2
para 1 valor distinto). O `pq` recusa-se a ler o ficheiro. **Nenhum caminho que
passe pela memória — nativo ou `pq` — consegue esta junção.** É a vantagem
arquitetural do desenho *lazy*, demonstrada e não afirmada.

## 6. Divergências do nativo por desenho, e dois riscos reais

**T10 — ordem das linhas depois de um `merge` (divergência não documentada).**
O `parqit` devolve o resultado agrupado pela chave e declara `sortedby(firm)`;
verificou-se independentemente que a declaração é **verdadeira**. O nativo
devolve uma ordem intercalada e não declara nada. A consequência a jusante é
real: a soma das posições físicas da primeira linha de cada `firm` é 10 no
nativo e 16 no `parqit`, pelo que `by firm: gen j = _n` ou `_n` dão resultados
diferentes.

Mas a ordem do nativo **não é um contrato do Stata**. Com os mesmos dados,
mudando apenas a ordem física das linhas do *using*, o `merge m:1` nativo
devolve o master em ordens diferentes dentro da chave (`2 1 3 4 6 5 8 7` contra
`1 2 3 4 5 6 8 7`). O `parqit`, pelo contrário, é **determinista**: duas
execuções do mesmo plano deram resultados idênticos na ordem produzida.

Conclusão: a igualdade exigível é a de multiconjunto, e o `parqit` cumpre-a.
Falta **documentar** que a ordem é agrupada pela chave, que o marcador
`sortedby` é verdadeiro, e que não coincide com a do nativo.

**T11 — RISCO: o colapso `.a`→`.` cria um emparelhamento FALSO numa chave.**
Master com chave `.a`, *using* com chave `.`:

| | emparelhados | N do resultado |
|---|---|---|
| nativo (`.dta`, sem ponte) | 1 | 3 |
| parqit (`save` + `merge`) | **2** | 2 |
| pq (`save` + `merge`, os dois lados) | **2** | 2 |

O teste simétrico em `a06` mostra que o `pq` faz **exatamente o mesmo** quando
os dois lados atravessam a ponte: é um risco **genérico de qualquer ponte
Stata↔colunar**, não um defeito do `parqit`. A perda `.a`–`.z`→`.` está
documentada e o `save` avisa. O que **não** está documentado é a consequência:
numa *chave*, a perda deixa de ser cosmética e passa a juntar linhas erradas,
em silêncio, depois do aviso da escrita.

**T12 — RISCO: chaves inteiras acima de 2^53.** Um ficheiro com seis valores
`int64` distintos passa a **cinco** depois de lido (`2^53` e `2^53+1` colidem);
o motor, esse, distingue os seis (`GROUP BY` devolve 6 grupos). O `parqit`
anuncia (`note: i64: values beyond 2^53 rounded to nearest double`) e prossegue.
O `pq` **recusa** (`rc=198`) com uma mensagem que nomeia a coluna e oferece
`safe_int64`, que carrega a coluna como `str19` **sem perda** — aqui o `pq` é
mais protetor.

O `parqit` tem um caminho sem perda, mas não é uma opção de `use`:
`parqit sql "SELECT id, CAST(i64 AS VARCHAR) AS s64 FROM read_parquet('…')"`
devolveu `9007199254740993` e `9223372036854775807` exatos. Note-se que
`parqit gen str20 s = string(i64)` devolve `9.01e+15` — o que é o comportamento
**nativo** de `string()` sem formato, e não um defeito, mas é uma armadilha
previsível.

**T13 — perda de omissos estendidos (partilhada, documentada).** `.a`, `.b` e
`.z` voltam como `.` nas quatro combinações escritor × leitor, `pq`→`pq`
inclusive. O valor `1.7976931348623157e+308`, que o Stata guarda num padrão
reservado, também volta como `.`. É a perda que o README documenta; o `pq`
partilha-a sem a documentar.

**T14 — `count if v > 0`: 11 ou 17?** Em Stata o omisso é maior que tudo, e o
nativo conta **17**. O `parqit` por omissão usa semântica SQL e conta **11**;
com `parqit set statamissing on` conta **17**, igual ao nativo. Está
documentado e é reversível, mas é a divergência que mais facilmente muda um
resultado sem que ninguém repare.

**T15 — `rc` diferente na chave duplicada de um `1:1`.** As três falham alto,
como devem: nativo `r(459)`, `pq` `r(459)`, `parqit` `r(198)`. Quem tenha
`capture … if _rc == 459` não apanha o `parqit`.

## 7. Onde o pq perde dados que o parqit preserva

**T16 — `decimal(38,10)`: coluna inteira a omissos, em silêncio.** O `pq`
imprime `Undefined parquet type: decimal[38,10]`, devolve **`rc=0`** e a coluna
toda a `.` (os quatro valores vêm `+1.0000000000000X+3ff`). O `parqit` converte
para `double` com `note: dec: decimal converted to double…` e devolve os quatro
valores corretos. Esta é exatamente a classe de falha que a carta de correção do
repositório proíbe: *coluna toda omissa em silêncio*.

**T17 — valores fora do intervalo do `double` do Stata.** O próprio Stata dá
`c(mindouble)` = `-8.9884656743115785e+307` e `c(maxdouble)` =
`+8.9884656743115785e+307` (simétrico — confirmado, não assumido). O que o
Stata faz com valores fora desse intervalo **depende do sinal**, e foi
verificado atribuindo-os diretamente a um `double` nativo:

| valor | Stata nativo (atribuição) | parqit | pq |
|---|---|---|---|
| `1e308` | `+1.1ccf385ebc8a0X+3ff`, mostrado **`.z_`**, `missing()==1` | `.`, com `note:` | `+1.1ccf385ebc8a0X+3ff` = **`.z_`**, em silêncio |
| `-1.7976931348623157e+308` | `.`, `missing()==1` | `.`, com `note:` | valor **vivo**, `missing()==0`, abaixo de `c(mindouble)` |

No caso positivo o `pq` faz o mesmo que o nativo faz numa atribuição: guarda os
bits em bruto, que o Stata mostra como `.z_`. **Aqui o `pq` não é o único
culpado** — mas `.z_` não é um omisso estendido legítimo, e o `parqit` é o
único dos três a devolver um omisso que o Stata reconhece e a dizê-lo. Note-se
que, nesse ponto, o `parqit` também **diverge do nativo** (`.` contra `.z_`);
é uma divergência benigna, mas esta auditoria diz o que encontra.

O defeito exclusivo do `pq` é o **caso negativo**: guarda
`-1.7976931348623157e+308` como um valor vivo, fora do intervalo que o Stata
declara poder armazenar, e sem aviso. O nativo e o `parqit` devolvem omisso.

**T18 — `float32` fora do intervalo do `float` do Stata.** O `parqit` alarga a
coluna a `double` e preserva `3.40282346638528860e+38` e o denormal
`1.40129846432481707e-45`, anunciando (`note: f: float32 values beyond Stata's
float range; stored as double`). O `pq` mantém `float` e devolve, em silêncio,
`.` para o primeiro e `0` para o segundo. São duas filosofias — fidelidade ao
ficheiro contra fidelidade ao tipo Stata — mas só uma não perde nada e só uma
o diz.

**T19 — nomes de coluna hostis.** O `parqit` lê `nuemp` e `NUEMP` como colunas
distintas com os valores certos (1 e 10), renomeia `com espaco` para
`com_espaco` guardando o nome original em `char varname[src_name]`, e mantém
`acentuação`. O `pq` falha com `rc=198` e a mensagem `>2045 invalid name`, que
além de fatal é enganadora.

**T20 — metadados.** Com as opções certas dos dois lados:

| | rótulo do dataset | rótulos de variável | formatos | rótulos de valor | notas | características |
|---|---|---|---|---|---|---|
| **parqit** (por omissão) | ✔ | ✔ | ✔ `%9.3f` `%tm` `%tc` | ✔ | ✔ 2 | ✔ variável **e** `_dta` |
| **pq**, `statametadata` | ✔ | ✔ | ✔ | ✔ | ✔ 2 | ✘ perdidas |
| **pq**, por omissão | ✘ | ✘ | ✘ `%tm`→**`%td`** | ✘ | ✘ | ✘ |

Nenhum dos dois lê os metadados do outro; não há corrupção, apenas ausência de
interoperabilidade, o que é o comportamento seguro. O `parqit` guarda tudo sob
`parqit.*` (`parqit.schema`, `parqit.vallabs`, `parqit.chars`,
`parqit.dtalabel`) e o `pq`, com `statametadata`, sob
`org.stata.pq.labels.v1`; o pyarrow lê os dois ficheiros sem dificuldade, pelo
que ambos continuam Parquet padrão.

**T20b — DEFEITO DO pq: uma contagem de períodos `%tm` é escrita no ficheiro
como uma data de calendário.** Este é o achado mais grave da auditoria, e não
se vê a partir do Stata: é preciso ler o ficheiro com um terceiro. Para
`dm = ym(2026,1)+1`, formato `%tm`, valor interno 793:

| escritor | tipo físico no Parquet | o que o pyarrow lê |
|---|---|---|
| **parqit** | `INT32` / `Int(bitWidth=32, isSigned=true)` | `793` — a contagem de meses, com `"fmt":"%tm"` em `parqit.schema` |
| **pq** (por omissão **e** com `statametadata`) | `INT32` / **`Date`** (`date32[day]`) | **`datetime.date(1962, 3, 4)`** |

O `pq` declara no ficheiro que 793 é um *dia* desde 1970. Qualquer consumidor
que não seja o `pq` — pandas, Spark, DuckDB, R/arrow — lê **março de 1962** em
vez de fevereiro de 2026, um erro de 64 anos, sem nada que o assinale. Os
metadados do próprio `pq` são coerentes com o erro (`"stata_type":"date"` para
uma variável mensal), e é por isso que na volta ao Stata o formato aparece como
`%td`. O `parqit` escreve um inteiro e guarda o formato à parte, que é o que o
contrato de tipos do repositório exige — `%tm/%tq/%th/%ty/%tw` permanecem
contagens de período e nunca são reescalados para datas de calendário.

Verificado por leitura direta em `oracle_disk.txt`; o `%tc` está correto nos
dois (`timestamp`, mesmo instante; o `parqit` em microssegundos, o `pq` em
milissegundos).

**T21 — tipos de armazenamento na leitura.** O `pq` estreita (`int`→`byte`,
`str6`→`str5`, `str4`→`str1`); o `parqit` preserva o tipo declarado. A
consequência não é cosmética: numa variável que o `pq` estreitou para `str5`, um
`replace s = "seischar"` posterior trunca.

**T22 — `parqit save` com uma view aberta.** Anuncia em voz alta
(`materialising view default — the dataset in memory is untouched; use the data
option to export memory instead`). Apanhou um erro no arnês desta auditoria
antes de ele produzir um resultado errado: é a mensagem a funcionar.

## 8. Resumo

| Tema | parqit vs nativo | parqit vs pq |
|---|---|---|
| `merge` 1:1 / m:1 / 1:m, `keep()`, `keepusing()` | idêntico | idêntico |
| `_merge` (tipo, códigos, rótulos) | idêntico | idêntico |
| `append` (tipos, coluna ausente) | idêntico | idêntico |
| chaves omissas e de texto | idêntico | idêntico |
| `m:m` | recusado lazy; `mergein` idêntico | pq delega no nativo |
| ordem das linhas pós-merge | difere (nativo não é reprodutível) | — |
| somas com cancelamento | **parqit exato, nativo erra** | pq = nativo |
| `sd`/assimetria/curtose com transbordo | **parqit exato, nativo omisso** | pq não tem |
| junção sobre `BIGINT` > 2^53 | **só o parqit acerta** | pq não lê |
| leitura de `BIGINT` > 2^53 | perda anunciada | **pq mais protetor** (`safe_int64`) |
| `decimal(38,10)` | — | **pq devolve tudo omisso, `rc=0`** |
| extremos do `double` | único omisso legítimo dos três | **pq guarda valor vivo fora de `c(mindouble)`** |
| `%tm` no ficheiro | inteiro + formato em metadados (contrato cumprido) | **pq escreve `date32`: terceiros leem 1962** |
| `float32` extremo | preserva, anunciando | **pq perde em silêncio** |
| nomes distintos só por caixa | lê | **pq falha** |
| metadados | tudo, por omissão | **pq perde características; `%tm`→`%td`** |
| omissos estendidos | perda documentada | perda igual, não documentada |
| `count if v > 0` | difere por omissão; `statamissing on` iguala | — |

## 9. Recomendações

Nenhuma foi implementada. Por ordem de importância:

1. **T11** — teste `v100` que fixe o falso emparelhamento por colapso `.a`→`.`
   numa chave, e um parágrafo no README/help: numa *chave*, a perda documentada
   junta linhas erradas. Ponderar um aviso do `save` quando a coluna colapsada
   tenha sido usada como chave de um `merge`/`joinby` no plano.
2. **T12** — opção de leitura sem perda para `int64`/`uint64` (equivalente ao
   `safe_int64` do `pq`) ou, no mínimo, documentar a receita
   `parqit sql … CAST(col AS VARCHAR)`, que já funciona e é exata.
3. **T10** — documentar a ordem pós-`merge`, incluindo a demonstração de que a
   ordem do nativo não é reprodutível; é o que justifica a escolha.
4. **T15** — ponderar `rc=459` na chave duplicada de um `1:1`, por compatibilidade.
5. **T06–T09, T16–T21** — material verificável para o README e para o artigo:
   são vantagens demonstradas, com oráculo independente, e não afirmações. A
   mais forte é **T20b**, porque não é uma questão de conveniência para o
   utilizador de Stata mas de o ficheiro dizer a verdade a quem o ler de fora.
6. **T20b** — comunicar ao autor do `pq`. É um defeito de interoperabilidade
   reproduzível em quatro linhas e independente do parqit; vale mais corrigido
   do que usado como argumento.

## 10. Limitações

Dados sintéticos e pequenos, uma máquina, uma versão do `pq` (4.0.2), Linux
apenas. Duas afirmações que poderiam ter ficado por verificar foram executadas
no Stata nativo e não assumidas: o rótulo de valor de `_merge`
(`Matched (3)`, igual nos dois) e `string(9007199254740992)` = `9.01e+15`, que
é o comportamento nativo de `string()` sem formato. Não foram medidos tempos nem uso de memória. A superfície coberta é a
comum às três implementações mais os tipos hostis; `reshape`, `pivot`,
`joinby`, partições Hive e a leitura de CSV/SAS/SPSS ficaram de fora. As
estatísticas foram auditadas com uma família adversarial compacta, apoiada nas
auditorias numéricas de 2026-09-06 e 2026-09-07, que trataram o tema em
profundidade. Um `PASS` aqui vale para o contraexemplo executado, não é
certificação universal. Nenhum ficheiro do produto foi alterado, nada foi
commitado, e a instalação global de Stata não foi tocada.
