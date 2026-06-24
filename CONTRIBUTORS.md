# Contributors

`parqit` is authored and maintained by **Miguel Portela** (NIPE / Universidade do
Minho and BPLIM / Banco de Portugal), who designed the package — taking
[`pq`](https://github.com/jrothbaum/stata_parquet_io) by **Jon Rothbaum** as the
starting point and re-basing the manipulation layer on an embedded DuckDB engine
through a C++ plugin.

## Implementation (AI coding agents)

The C++/DuckDB plugin and the ado layer were programmed, adversarially
cross-audited and prepared for cross-platform release by two AI coding agents
working in tandem, under the author's direction and review. Both contributed
substantially to the making of `parqit`:

- **OpenAI Codex**
- **Anthropic Claude** (via Claude Code)

They are credited here, and in the [README](README.md#acknowledgements), as
development tools / contributors — **not** as citation authors (see
[`CITATION.cff`](CITATION.cff)).

## With thanks

- **Jon Rothbaum** — [`pq`](https://github.com/jrothbaum/stata_parquet_io), the work
  from which the `parqit` solution was designed.
- The **BPLIM** team at **Banco de Portugal** (https://bplim.bportugal.pt/), whose
  interaction throughout greatly benefited the development of `parqit`.

See [README — Acknowledgements](README.md#acknowledgements) for the full credits.
