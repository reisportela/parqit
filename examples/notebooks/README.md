# parqit notebooks

Two Jupyter notebooks that teach parqit step by step. The Stata output of every cell
is stored in the notebooks, so they can be read on GitHub without running anything.

| Notebook | Contents |
|---|---|
| [`parqit_1_data_manipulation.ipynb`](parqit_1_data_manipulation.ipynb) | The lazy view (a plan, not data): writing and reading Parquet, exploring a file without loading it, building a plan with Stata verbs, `parqit show`, `collect` versus `save`, aggregating on disk, the same code on a file 1,000 times larger, `merge` and `append` (lazy, and with the data in memory), many files as one table, partitioned folders. |
| [`parqit_2_descriptive_statistics.ipynb`](parqit_2_descriptive_statistics.ipynb) | Statistics computed on disk with Stata's definitions: `codebook`, `misstable`, `duplicates`, `distinct`, `summarize`, `tabulate`, `tabstat`, `correlate`/`pwcorr`, `histogram`, subgroups through named views, `collapse`/`pivot`/`contract`, the missing-value rule, and a check against native `summarize`. |

Both use `nlsw88`, the 1988 extract of the U.S. National Longitudinal Survey of Young
Women (2,246 women) that ships with Stata, so there is nothing to download. They follow
the do-file courses `parqit_basics.do` and `parqit_tour.do` that ship with the package.
The stored outputs were produced with parqit 0.2.1.

## Running the notebooks

1. Stata 16 or newer, with parqit 0.2.1 or later from the GitHub releases:

       net install parqit, from("https://github.com/reisportela/parqit/releases/latest/download") replace

2. Jupyter with the [nbstata](https://github.com/hugetim/nbstata) kernel:

       pip install nbstata
       python -m nbstata.install

3. Open a notebook, choose the kernel **Stata (nbstata)** and run the cells in order.

The notebooks write their files to a `parqit_notebooks` folder in Stata's temporary
directory (`c(tmpdir)`); the largest, the 2.2-million-row file of part 1, takes about
1.4 MB.

## Refreshing the stored outputs

After a release, rerun both notebooks and save their outputs in place:

    python run_notebooks.py                   # parqit as installed in Stata
    python run_notebooks.py --adopath DIR     # parqit.ado and parqit.plugin taken from DIR

The script runs each notebook in a fresh Stata session and stops at the first Stata
error, leaving that notebook unchanged. With `--adopath`, the cell that puts `DIR` first
on the ado-path is removed before saving, so no local path is written into the
notebooks.
