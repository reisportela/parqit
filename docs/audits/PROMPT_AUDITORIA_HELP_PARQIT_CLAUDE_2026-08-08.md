# Prompt para Claude — auditoria adversarial e correção do help do `parqit`

Trabalha no repositório:

`/home/mangelo/Documents/GitHub/parqit`

## Missão

Faz uma auditoria independente, adversarial, holística e minuciosa do help público do `parqit`, cuja fonte canónica é `src/ado/p/parqit.sthlp`. Confronta cada afirmação do help com a implementação efetiva, os testes e, quando necessário, o comportamento observado em Stata. Depois, aplica diretamente as correções documentais e os reforços de teste que a evidência justificar.

Não assumes que o help atual, o README, os comentários, os testes existentes ou as alterações locais estão corretos. Tenta falsificar as afirmações importantes antes de as aceitar. O objetivo não é tornar o texto mais longo: é torná-lo completo, exato, navegável e útil, sem promessas que o programa não cumpra nem funcionalidades públicas omitidas.

## Instruções obrigatórias antes de agir

1. Lê integralmente, nesta ordem:
   - `AGENTS.md`;
   - `README.md`;
   - `parqit_build_prompt.md`.
2. Trata como decisões fixas todas as instruções marcadas como “must” ou “do not” no build brief.
3. Inspeciona `git status --short`, `git diff`, `git diff --cached` e os ficheiros não rastreados relevantes antes de escrever.
4. Preserva integralmente o worktree existente. Não uses `git reset`, `git restore`, `git checkout --`, `git clean`, reformatadores globais nem substituições em massa.
5. Há trabalho local que deve ser revisto, mas não descartado:
   - `src/ado/p/parqit.sthlp` contém alterações não commitadas de uma auditoria anterior;
   - `tests/release_lint.sh` contém verificações novas relacionadas com o help;
   - `docs/audits/README.md`, `docs/audits/AUDITORIA_ADVERSARIAL_HOLISTICA_PARQIT_2026-08-08.md` e `docs/audits/PROMPT_CORRECAO_MELHORIA_PARQIT_2026-08-08.md` são trabalho preexistente do utilizador e não podem ser alterados, substituídos ou incorporados no commit por acidente.
6. Confirma o estado real no momento da execução; a lista anterior não autoriza tocar noutros ficheiros modificados ou não rastreados.

## Âmbito de escrita autorizado

Podes:

- corrigir `src/ado/p/parqit.sthlp` com patches pequenos e focados;
- corrigir `tests/release_lint.sh` e, apenas se houver uma lacuna substantiva que esse ficheiro não possa cobrir com clareza, acrescentar um teste estritamente dedicado ao contrato do help;
- sincronizar a cópia instalada `ado/plus/p/parqit.sthlp` através do alvo de build já previsto pelo projeto, sem a editar manualmente;
- criar o relatório `docs/audits/AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md` com achados, provas, alterações e validação final.

Não alteres código funcional C++/ado, versões, datas de release, changelog, empacotamento, tags ou releases nesta tarefa. Se descobrires um defeito real de implementação — e não apenas documentação incorreta — não o escondas mudando o help para normalizar comportamento errado: reproduz o defeito, classifica a gravidade e regista-o claramente no relatório como trabalho separado. Não faças commit, push, PR, tag ou release.

## Hierarquia de evidência

Usa esta ordem para decidir o que o help deve afirmar:

1. comportamento reproduzido da versão local que está efetivamente a ser testada;
2. dispatcher e implementação executável atuais;
3. testes que exercem realmente a afirmação;
4. decisões normativas de `parqit_build_prompt.md`;
5. README, changelog, comentários e documentos de auditoria como pistas a verificar, nunca como prova autónoma.

Se código, testes e documentação divergirem, torna a divergência explícita. Distingue sempre:

- contrato público intencional;
- comportamento implementado e comprovado;
- comportamento apenas inferido;
- limitação conhecida;
- bug ou incerteza ainda não resolvida.

## Superfícies que tens de inspecionar

Não limites a auditoria a uma leitura editorial do `.sthlp`. Examina, no mínimo:

- o dispatcher e todos os handlers públicos em `src/ado/p/parqit.ado` e restantes ficheiros `.ado` relevantes;
- a gramática, opções, abreviações, defaults, validações, mensagens de erro e valores devolvidos por cada comando;
- `src/plugin/` e `src/engine/`, incluindo as superfícies de views, I/O, expressões, tipos, metadata e sanitização; presta atenção particular aos ficheiros equivalentes a `plugin_view.cpp`, `plugin_io.cpp`, `view.cpp`, `exprtrans.cpp`, `typemap.cpp` e `sanitize.cpp` que existam no estado atual;
- `src/ado/p/parqit.pkg`, diálogos e outros artefactos distribuídos que definam a superfície pública;
- testes unitários, de integração, round-trip e `verify_suite`, incluindo fixtures e expectativas negativas;
- `README.md`, `BUILDING.md`, `CHANGELOG.md`, `CITATION.cff` e documentação adjacente apenas para detetar inconsistências que devam ser resolvidas no help ou registadas no relatório;
- o diff local completo dos dois ficheiros já alterados, avaliando cada adição anterior como uma hipótese a testar.

Usa `rg`/`rg --files` para construir inventários reproduzíveis. Não determines a cobertura por memória nem por simples pesquisa de nomes: confirma que a semântica descrita corresponde ao ramo de código executado.

## Matriz de auditoria obrigatória

Constrói primeiro uma matriz de rastreabilidade, ainda que compacta, com pelo menos estas colunas:

| Superfície ou afirmação | Evidência de código | Evidência de teste/execução | Local no help | Veredito/ação |
|---|---|---|---|---|

A matriz tem de cobrir, sem omissões deliberadas:

### 1. Inventário integral da interface pública

- Todos os subcomandos aceites pelo dispatcher, excluindo apenas comandos inequivocamente internos.
- Sintaxe posicional, opções, abreviações permitidas, opções mutuamente exclusivas e defaults.
- Diferenças entre formas do mesmo comando: memória versus ficheiro, `using` versus fonte principal, nome de view versus origem externa, etc.
- Comandos ou variantes presentes no código mas ausentes do help, e texto no help sem comando correspondente.
- Comandos acessíveis por diálogos ou menus e cobertura efetiva desses diálogos.

### 2. Modelo de execução

- Formação do plano lazy, momento de execução, validações precoces e operações que podem consultar schema, metadata ou amostras antes da materialização.
- Diferença rigorosa entre “não materializa a tabela em Stata”, “não executa o plano completo” e “não faz qualquer I/O”. Não uses estas frases como sinónimos.
- Estado e ciclo de vida das views: substituição, cópia, dependências, embedding de views, mutações da fonte, invalidação, `clear`, recuperação do prefixo e possíveis conflitos.
- O que é eager, lazy, metadata-only ou materializador para cada comando relevante.

### 3. Origens, formatos e adaptadores

- Parquet, CSV, TSV, TXT/tab-delimited, DTA, XLS e XLSX, mas apenas nas formas realmente suportadas.
- Distinção entre leitores nativos e adaptadores/bridges via Stata; custos, limites e requisitos dessa distinção.
- Diferença entre fontes aceites diretamente e ficheiros aceites apenas depois de importados/convertidos.
- Globs, diretórios, listas de ficheiros, `relaxed`, schema reconciliation, ordem/união de colunas e erros de fontes incompatíveis.
- Regras de caminhos, quoting, extensões, compressão, partitions e opções específicas por formato.
- Não atribuas a `describe` ou a outro comando suporte de adaptadores se a forma de ficheiro implementada for apenas Parquet.

### 4. Verbos de transformação

- `select`, `drop`, `rename`, `filter`, `generate`, `replace`, `egen`, `sort`, `gsort`, `by`, `bysort`, agregações e qualquer outro verbo público encontrado.
- Semântica exata de ordem, grupos, missing values, ties, type promotion, nomes de colunas e substituição de expressões.
- `merge`, `append`, `joinby`, `mergein` e `appendin`: chaves, cardinalidade, proveniência, colisões de nomes, variáveis indicadoras, unmatched, schema/type reconciliation e diferença entre operar sobre views ou fontes externas.
- `reshape`, `pivot`, `duplicates`, `sample`, `contract`, `collapse` e variantes: pré-condições, defaults, determinismo, seeds, nomes produzidos e limitações.
- Qualquer comando que altere uma view deve ser descrito como tal, incluindo atomicidade e o estado preservado em caso de erro.

### 5. Expressões

- Todos os operadores e todas as funções aceites pelo tradutor de expressões, obtidos da implementação e não de uma lista histórica.
- Aridades, aliases, argumentos opcionais, funções agregadas versus escalares e contextos onde cada função é válida.
- Semântica de `_n`, `_N`, subscripting/lag/lead, strings, Unicode versus bytes, regex, datas, missing values e coerções.
- `statamissing` e quaisquer modos que alterem a tradução: esclarece se a semântica fica fixada quando a expressão entra no plano ou se é consultada apenas na execução.
- Quoting, identificadores, literais, precedence, unsupported syntax e mensagens de erro relevantes.
- Confirma automaticamente que todas as funções implementadas aparecem numa secção apropriada do help e que o help não anuncia funções inexistentes.

### 6. Materialização, persistência e segurança de escrita

- `collect` e `save`: limites de linhas/colunas, memória, chunking, tipos, labels, metadata, locks, ficheiros temporários, renames atómicos e comportamento após erro.
- Diferença entre trazer dados para Stata e escrever Parquet sem os materializar em memória Stata.
- Colisão entre origem e destino, overwrite/replace, destinos particionados, diretórios existentes, codecs e comportamento multi-ficheiro.
- Qualquer afirmação de atomicidade deve identificar o âmbito real: ficheiro único, conjunto particionado, view ou estado em memória.
- Verifica cuidadosamente afirmações sobre `open _data`: não digas que evita uma segunda cópia se a implementação efetivamente a cria ou se a prova não sustenta essa formulação.

### 7. Tipos, missing values e metadata

- Mapeamento completo DuckDB/Arrow/Parquet ↔ Stata e tratamento de inteiros, reais, doubles, decimals, booleans, strings, binary, dates, datetimes e tipos não suportados.
- Precisão numérica, overflow, NaN/Inf, `-0`, inteiros fora da representação exata, NUL em strings e perdas ou rejeições deliberadas.
- Missing básico e extended missings, incluindo a diferença entre dados Stata, representação Arrow e round-trip Parquet.
- Variable labels, value labels, formats, display formats, characteristics/metadata, sort state e quaisquer sidecars ou chaves embebidas.
- Sanitização, deduplicação e truncagem de nomes; deixa claro quando o nome original é preservado em metadata e quando não é.
- Semântica e limitações de datas e tempos, incluindo timezone quando aplicável.

### 8. Exploração, SQL e configuração

- `describe`, `count`, `summarize`, `tabulate`, `list`, `browse`, `codebook`, `inspect` e outros comandos de exploração encontrados.
- Caps/defaults de linhas, níveis, colunas ou combinações; quando há erro em vez de truncagem e como aumentar explicitamente um limite.
- Formas que retornam resultados sem alterar dados, formas que abrem UI e formas que materializam temporariamente.
- `sql`/`query`: quoting, comandos permitidos, resultados, mutação ou não do catálogo, relação com views e limites de segurança.
- Settings, opções persistentes e variáveis de ambiente: nomes exatos, precedence, defaults, alcance temporal e momento em que são lidos.

### 9. Resultados devolvidos e mensagens

- Para cada forma de cada comando, confirma `return list`/`r()` contra o código e uma execução real quando viável.
- Não generalizes resultados de uma forma para outra: por exemplo, memória versus ficheiro ou sucesso versus erro.
- Distingue resultados garantidos de resultados incidentais. Não prometas `r()` que não esteja intencionalmente definido.
- Confirma códigos de retorno e mensagens quando sejam parte útil do contrato público.

### 10. Limitações e dependências externas

- Expõe limitações materiais sem as exagerar nem as esconder.
- Distingue limitações do `parqit`, DuckDB, Stata, formatos de ficheiro e bibliotecas externas.
- Não uses formulações absolutas como “zero I/O”, “lossless” ou “atomic” sem delimitar condições e exceções.
- Verifica se “lossless metadata round-trip” é descrito exatamente para as superfícies em que é comprovado; qualquer exceção deve ser explícita.

## Perguntas de falsificação obrigatórias

Procura evidência contrária, no mínimo, para estas hipóteses:

1. O help promete ausência total de leitura/I/O em operações que fazem scans de schema, metadata ou validação?
2. Confunde lazy evaluation com ausência de validação imediata?
3. Afirma que `open _data` não cria uma segunda cópia sem prova suficiente?
4. Atribui a `describe` suporte a adaptadores que só existe noutras superfícies?
5. Mistura o caminho CSV principal com o caminho `using`/bridge?
6. Descreve corretamente mutações da fonte, views vivas, views embebidas e a restauração do estado?
7. Torna explícitos caps de exploração e distingue truncagem, amostragem e erro?
8. É suficientemente claro sobre perdas ou rejeições de storage, precisão numérica, datas, strings e metadata?
9. Lista exatamente os `r()` de cada forma de comando?
10. Existem contradições internas no help ou entre o help e o contrato normativo do projeto?
11. Algum exemplo do help falha quando copiado literalmente para Stata?
12. Algum link SMCL interno aponta para uma âncora inexistente ou algum markup se estende indevidamente entre linhas físicas?

## Método de trabalho

1. Faz o inventário mecânico das superfícies públicas e das funções de expressão.
2. Mapeia cada elemento para o help atual e assinala ausências, excessos e ambiguidades.
3. Para cada problema potencial, reproduz antes de corrigir:
   - lê o ramo de implementação pertinente;
   - identifica ou acrescenta uma prova automatizada;
   - quando a semântica continuar ambígua, executa um `.do` mínimo com `stata-mp`, usando apenas o `adopath` e o plugin deste repositório;
   - usa dados temporários apenas dentro do repositório ou de `/tmp` e não toca no ado global do utilizador.
4. Se PyArrow ou DuckDB Python já estiverem disponíveis, podes usá-los como oráculo independente para casos de ficheiro/metadata. Não instales pacotes, não descarregues binários e não uses a rede.
5. Só depois altera o help. Prefere formulações verificáveis e exemplos curtos que exponham a distinção importante.
6. Reforça `tests/release_lint.sh` apenas com invariantes estáveis e semanticamente justificadas; evita checks frágeis baseados em prose incidental.
7. Revê todo o documento depois dos patches, incluindo coerência entre secções, referências cruzadas, índices, exemplos e SMCL.

## Qualidade editorial exigida

- Mantém o estilo de um help Stata profissional e idiomático.
- Escreve o help público em inglês, salvo se o estilo atual do ficheiro determinar inequivocamente outra coisa; escreve o relatório em português europeu.
- Usa termos de modo consistente: view, source, plan, materialization, adapter, bridge, metadata e missing value não devem mudar de significado entre secções.
- Organiza por tarefas e decisões do utilizador, mas preserva referências suficientemente precisas para todas as opções.
- Evita duplicação extensa. Usa links SMCL internos quando melhorarem a navegação e valida todos os destinos.
- Exemplos têm de ser sintaticamente válidos, representar workflows suportados e não depender de paths pessoais.
- Não escondas caveats críticos em notas vagas nem sobrecarregues a primeira leitura com detalhes internos sem valor operacional.

## Validação mínima obrigatória

Executa, ou explica com prova concreta por que não foi possível executar, todos os checks aplicáveis:

```bash
git diff --check
bash -n tests/release_lint.sh
bash tests/release_lint.sh
cmake --build build/dev --target parqit_ado_sync -j
cmp -s src/ado/p/parqit.sthlp ado/plus/p/parqit.sthlp
ctest --preset dev
STATA=stata-mp BUILD_DIR="$PWD/build/dev" bash tests/run_stata.sh m0_smoke
```

Além disso:

- abre `help parqit` numa execução real de `stata-mp` com a árvore local no início do `adopath` e confirma `_rc == 0`;
- executa testes Stata mínimos para todas as afirmações alteradas que não estejam já cobertas de forma inequívoca;
- confirma mecanicamente que todos os subcomandos públicos do dispatcher aparecem no help;
- confirma mecanicamente que todas as funções públicas do tradutor de expressões aparecem no help;
- confirma que todos os destinos de links internos do help existem;
- confirma que a fonte canónica e a cópia instalada têm conteúdo idêntico depois da sincronização;
- inspeciona o `git diff` final e o `git status --short` para garantir que só os ficheiros autorizados foram alterados.

Não declares que a suite completa passou se apenas executaste testes estreitos. Não reutilizes resultados antigos como se fossem validação fresca. Se a build existente estiver ausente ou exigir downloads, não uses rede: valida o que for possível e regista a limitação com precisão.

## Critérios de aceitação

A tarefa só está concluída quando:

- cada comando público implementado está representado de forma encontrável no help;
- cada opção pública e cada variante materialmente diferente tem contrato correto;
- todas as funções de expressão implementadas e públicas estão documentadas com aridade/contexto suficiente;
- não restam afirmações absolutas falsas ou não demonstradas sobre lazy execution, I/O, cópias, atomicidade ou fidelidade;
- formatos, adaptadores e caminhos de dados estão claramente separados;
- tipos, metadata, missing values e perdas possíveis estão descritos sem ambiguidade perigosa;
- resultados `r()` documentados coincidem com execução/código;
- exemplos e links SMCL passam os checks relevantes;
- regressões documentais importantes ficam protegidas por checks estáveis;
- a cópia distribuída do help está sincronizada com a fonte;
- o relatório distingue factos provados, alterações feitas, problemas fora de âmbito e risco residual.

## Entrega

No ficheiro `docs/audits/AUDITORIA_HELP_PARQIT_CLAUDE_2026-08-08.md`, inclui:

1. veredito executivo (`PASS`, `PASS WITH RESIDUAL RISKS` ou `NO-GO` para a qualidade do help, não para a release inteira);
2. matriz de rastreabilidade ou uma versão resumida com ligação clara às provas;
3. achados por gravidade, incluindo falsificações tentadas e resultado;
4. lista exata das alterações aplicadas e respetiva justificação;
5. comandos de validação, códigos de retorno e resultados observados nesta execução;
6. limitações de validação e riscos residuais;
7. inventário dos ficheiros locais preexistentes que foram preservados sem alteração.

Na resposta final ao utilizador, começa pelo resultado. Indica concisamente:

- o que estava errado ou em falta;
- os ficheiros alterados;
- o que foi validado de novo e o que não foi possível validar;
- quaisquer bugs funcionais encontrados fora do âmbito;
- o veredito sobre o help.

Não confundas um help aprovado com uma release aprovada.
