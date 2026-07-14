# Prompt — Auditoria adversarial da fiabilidade dos dados no parqit

Quero que atues como uma equipa independente de auditores sénior de software de
dados, especializada em Stata, C++, DuckDB, Parquet, Arrow, sistemas
lazy/out-of-core, testes diferenciais e engenharia de releases.

Repositório:
`/home/mangelo/Documents/GitHub/parqit`

## Objetivo

Realiza uma auditoria adversarial, agressiva, holística e incisiva ao
`parqit`, com prioridade absoluta à fiabilidade dos dados.

Quero alcançar um grau elevadíssimo de confiança de que o `parqit`:

1. não altera silenciosamente valores, tipos, missing values ou metadados;
2. não produz resultados diferentes de Stata sem documentar ou recusar
   explicitamente a operação;
3. não perde observações, variáveis, precisão, ordem relevante, labels, formats,
   notes ou characteristics;
4. não deixa resultados parciais, views corrompidos ou datasets em memória
   destruídos após erros;
5. não publica artefactos diferentes daqueles que foram testados;
6. se comporta corretamente em todos os caminhos eager, lazy, collect, save,
   adapters e operações entre duas tabelas.

Não procures confirmar que o programa está correto. Tenta ativamente falsificar
essa hipótese.

## Modo não destrutivo

Esta fase é exclusivamente de auditoria:

- Não alteres ficheiros versionados.
- Não corrijas código.
- Não faças commit, push, tag, release ou PR.
- Não alteres o ado tree global, `profile.do`, instalação ou licença do
  Stata.
- Não apagues nem reformates ficheiros do utilizador.
- Não confies em builds ou logs antigos como prova atual.
- Para builds, mutations, fuzzing e repros, cria uma cópia isolada do snapshot
  auditado sob `/tmp`.
- Podes criar apenas um novo relatório Markdown no repositório:
  `AUDITORIA_ADVERSARIAL_FIABILIDADE_DADOS_PARQIT_<DATA>.md`.
- Conserva scripts, fixtures e logs detalhados sob `/tmp` e indica os
  caminhos no relatório.

Se uma verificação não puder ser executada, não a marques como PASS:
classifica-a como “não verificada” e explica exatamente porquê.

## Preparação obrigatória

Começa por ler integralmente:

- `AGENTS.md`
- `README.md`
- `parqit_build_prompt.md`
- `ASSUMPTIONS.md`
- `CHANGELOG.md`
- documentação de build e release;
- código Stata, C++, testes, workflows e manifestos de distribuição.

Regista antes de testar:

- commit, branch, dirty state e tag;
- versão em todas as superfícies;
- compiladores, CMake, Stata, Python e bibliotecas;
- plugin efetivamente carregado por Stata;
- release pública mais recente, workflow e hashes dos assets.

Faz primeiro uma leitura independente. Só depois consulta auditorias anteriores,
usando-as como hipóteses a desafiar e não como conclusões.

## Trabalho em equipa

Lança agentes independentes com responsabilidades separadas:

1. Semântica Stata e comandos públicos.
2. Engine C++/DuckDB, planos lazy, ownership, concorrência e atomicidade.
3. Fidelidade física Parquet/Arrow, tipos, valores e metadados.
4. Test harness, build, packaging, CI, portabilidade e artefactos publicados.

Um agente deve atuar explicitamente como red team e tentar invalidar os PASS dos
restantes. Consolida os resultados, elimina duplicados e resolve divergências
com evidência executável.

## Modelo de confiança

Não uses apenas os testes existentes. Combina:

- inspeção estática;
- testes existentes executados de novo;
- testes diferenciais contra Stata nativo;
- oráculos independentes com pyarrow e DuckDB;
- testes metamórficos;
- property-based tests ou fuzzing focado;
- mutation testing do test harness numa cópia scratch;
- inspeção dos ficheiros Parquet físicos;
- testes sobre os binários realmente publicados.

Um PASS forte exige, sempre que aplicável:

1. resultado correto contra um oráculo independente;
2. cobertura de casos limite e adversariais;
3. confirmação nos diferentes caminhos de materialização;
4. ausência de erro silencioso;
5. execução sobre a superfície staged ou publicada.

Não uses outro pacote relacionado como único oráculo independente.

## Mapa end-to-end

Reconstrói o ciclo de vida dos dados e identifica invariantes em cada transição:

```text
Stata memory
→ direct save
→ Parquet físico
→ lazy view
→ transformação do plano
→ collect
→ Stata memory
```

e:

```text
Parquet/CSV/DTA/Excel
→ adapter ou scan
→ lazy plan
→ save
→ novo Parquet
```

Para cada transição verifica valores, tipos, missingness, nomes, ordem relevante,
metadados, número de linhas e comportamento em erro.

## Matriz mínima de auditoria

### A. Tipos e valores

- byte, int, long, float e double;
- limites mínimos/máximos, overflow e valores acima de 2^31;
- precisão float/double, ties de arredondamento e valores negativos;
- signed/unsigned, decimal e all-null;
- NaN, +Inf, -Inf, `.`, `.a`–`.z`;
- str#, limites strL, texto longo e strL binário;
- UTF-8 válido/inválido, Unicode, NUL e strings vazias;
- colunas duplicadas, nomes reservados, hostis, longos ou sanitizados.

### B. Datas e tempos

- `%td`, `%tc`, `%tC`, `%tw`,
  `%tm`, `%tq`, `%th`, `%ty` e
  `%tb`;
- valores negativos, half ties, frações, extremos e overflow;
- paridade entre direct save, lazy save e collect;
- payload físico e formatos Stata recuperados.

### C. Metadados

- variable labels e value labels;
- labels ligados a missings;
- formats, notes, characteristics e sort information;
- nomes originais e `src_name`;
- rename, keep/drop/order, collapse, reshape e save de views;
- metadados KV Parquet, incluindo round-trips repetidos.

### D. Semântica dos comandos

Testa contra Stata nativo, quando exista equivalente:

- use/read, save/write, open, collect e close;
- keep, drop, generate, replace, rename, order e sort;
- expressions, missings, booleans, strings, regex e datas;
- collapse, summarize, tabulate, tabstat, correlate;
- codebook, distinct, duplicates e levelsof;
- reshape long/wide e pivot;
- merge 1:1, m:1 e 1:m;
- recusa correta do lazy `merge m:m`;
- `mergein m:m` contra o oracle nativo;
- joinby, append, appendin e múltiplas using sources;
- named views, view como using e referências partilhadas;
- sql/query e estado do plano.

### E. Inputs e caminhos

- Parquet único, glob, pasta e Hive partitions;
- schemas iguais, incompatíveis e `relaxed`;
- CSV, DTA, XLS/XLSX e adapters;
- paths absolutos/relativos, espaços, Unicode, quotes e wildcards;
- tempdirs partilhados por vários processos;
- ficheiros inexistentes, sem permissões, truncados ou corruptos;
- output que coincide ou se sobrepõe com o input.

### F. Atomicidade e erros

Para todas as operações relevantes, força erros antes, durante e depois da
preparação:

- o dataset Stata existente deve permanecer intacto;
- o view anterior deve continuar válido;
- outputs parciais não podem parecer válidos;
- replace deve ser atómico;
- bridges e temporários devem ter ownership correto;
- close/replace/close _all devem respeitar referências;
- nenhum cleanup pode apagar ficheiros do utilizador;
- o return code e a mensagem devem preservar a causa original;
- nenhuma exceção pode ser convertida em missing ou PASS silencioso.

### G. Concorrência e lifecycle

- duas ou mais sessões Stata com o mesmo TMPDIR;
- bridges simultâneos e nomes colidentes;
- views dependentes fechadas em ordens diferentes;
- operações falhadas parcialmente;
- repetição intensiva de open/close/replace;
- isolamento entre processos;
- comportamento após terminação anormal, distinguindo o que é recuperável do
  que depende do SO.

### H. Escala e estrutura

- zero, uma e muitas observações;
- datasets muito largos, incluindo milhares de variáveis;
- strings muito grandes;
- row counts acima do limite SPI, mesmo que simulados via metadata;
- partições, globs extensos e schemas mistos;
- execução out-of-core sem materialização oculta;
- memória e tempo apenas depois de a correção estar demonstrada.

### I. Segurança semântica

- nomes e expressões hostis;
- escaping SQL/JSON/hex;
- quotes, comentários, delimitadores e Unicode;
- injection através de nomes, paths, labels, formats ou expressions;
- pedidos internos adulterados;
- tentativa de declarar como “owned” um ficheiro não criado pelo pacote.

### J. Test harness

Audita os próprios testes como possível fonte de falsos verdes:

- `capture assert` sem consumo de `_rc`;
- Stata batch devolver zero após abort;
- PASS impresso antes de erro posterior;
- filtros que selecionam zero testes;
- logs em falta ou antigos;
- testes que usam o mesmo temporary path;
- oráculos que reproduzem a mesma implementação;
- asserts tautológicos ou demasiado fracos.

Faz mutations controladas apenas numa cópia scratch para demonstrar que testes
críticos ficam realmente vermelhos quando a implementação ou a asserção é
corrompida.

### K. Build e release

- Executa configure, build, CTest, testes unitários e suite Stata integral.
- Testa a superfície staged, não apenas os ficheiros fonte.
- Inspeciona o workflow da tag e o manifest `parqit.pkg`.
- Descarrega a release pública e verifica hashes, ZIPs e ficheiros loose.
- Confirma que o plugin testado é o mesmo que foi publicado.
- Verifica formato, exports, stripping e dependências runtime.
- Linux deve ser self-contained e compatível com o baseline glibc declarado.
- Verifica estruturalmente macOS ARM64 e Windows.
- Quando não houver runtime Stata noutra plataforma, diz explicitamente que a
  inspeção estrutural não substitui esse runtime.
- Faz um smoke test Stata isolado sobre o ZIP Linux efetivamente descarregado.

## Gates de fiabilidade

Classifica explicitamente:

- G0 — snapshot, proveniência e versão;
- G1 — paridade semântica Stata;
- G2 — valores, tipos e precisão;
- G3 — metadata round-trip;
- G4 — atomicidade e comportamento em erro;
- G5 — lazy plans, bridges, ownership e concorrência;
- G6 — escala, limites e out-of-core;
- G7 — test harness resistente a falsos verdes;
- G8 — staged install e artefactos publicados;
- G9 — documentação fiel ao comportamento real.

Cada gate deve terminar como:

- PASS forte;
- PASS condicionado;
- FAIL;
- NÃO VERIFICADO.

## Severidade dos achados

- S0: corrupção, perda ou alteração silenciosa de dados.
- S1: resultado semanticamente incorreto ou metadata relevante perdida.
- S2: atomicidade, erro, lifecycle ou portabilidade que pode afetar resultados.
- S3: teste, documentação, UX ou packaging que pode esconder um problema.
- S4: melhoria sem impacto demonstrado na fiabilidade.

Para cada finding apresenta:

- ID estável;
- severidade;
- grau de certeza;
- impacto sobre investigadores/dados;
- ficheiros e linhas;
- caminho de execução;
- repro mínimo;
- resultado observado;
- resultado esperado;
- oracle utilizado;
- explicação da causa-raiz;
- razão pela qual os testes anteriores não o detetaram.

## Regras de conclusão

- Não pares porque as suites existentes passaram.
- Não confundas ausência de finding com prova de ausência de bugs.
- Não declares “seguro” se existir algum S0/S1 ou gate essencial não verificado.
- Um finding S0/S1 deve ser reproduzido por duas vias independentes sempre que
  possível.
- Se suspeitares de um bug mas não o conseguires provar, mantém-no separado como
  hipótese.
- Distingue sempre comportamento do source, build local, staged install e
  release pública.
- Indica claramente a cobertura que falta e o risco residual.

## Entrega

Produz o relatório:

`AUDITORIA_ADVERSARIAL_FIABILIDADE_DADOS_PARQIT_<DATA>.md`

Estrutura mínima:

1. Veredito executivo.
2. Snapshot e ambiente.
3. Mapa do fluxo de dados e invariantes.
4. Matriz de cobertura.
5. Gates G0–G9.
6. Findings ordenados por severidade.
7. Repros e evidência.
8. Resultados dos testes frescos.
9. Verificação dos artefactos publicados.
10. Limitações e risco residual.
11. Avaliação final de confiança.
12. Plano de remediação priorizado, sem implementar alterações.

A avaliação final deve usar linguagem rigorosa, por exemplo:

- “Não foram encontrados defeitos conhecidos que comprometam a fiabilidade
  dentro da cobertura executada”; ou
- “Não é possível dar essa garantia devido aos seguintes gates/falhas”.

Não atribuas uma percentagem de confiança arbitrária. Fundamenta a confiança na
cobertura, independência dos oráculos, mutations, plataformas e caminhos
efetivamente testados.

No final, resume separadamente:

- o que está demonstrado;
- o que está apenas sugerido;
- o que continua por verificar;
- quais os três riscos residuais mais importantes.

Começa agora e continua autonomamente até esgotar todos os eixos da auditoria.
Não implementes correções nesta fase.
