* A6 (audit of 25 Oct 2026): partitions(append|replace) over partitions of
* several files. The engine can write a partition in more than one file (under
* file eviction, upstream as with the 2.0 corrections), and parqit never assumes
* one file per partition: an append adds its files beside the existing ones, a
* replace swaps the whole leaf, a publish failure restores every touched file,
* and a staged partition that cannot be listed fails the append loudly
* (PART-LIST-1, PARQIT_TEST_FAIL_PARTITION_LIST) instead of counting it as
* extended with its files left behind. Since the audit of the execution (G6,
* PART-SCHEMA-ALL-1): incoming partitions of several files, failures midway
* (after the first file moved, an unreadable entry, unlistable staged
* partitions), exact footers, and siblings that disagree with the result in
* columns or parqit.* metadata, refused wherever they lie. Oracles: pyarrow
* (exact multisets per partition, partition membership by directory, every
* footer), SHA-256 of every file untouched.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile stem
local dir `"`stem'_d"'
mkdir `"`dir'"'
local tree `"`dir'/tree"'

* ---------- a tree of 3 months; month 1 then holds two files ----------------
clear
set obs 300
gen int year = 2024
gen byte month = 1 + mod(_n - 1, 3)
gen long id = _n
gen double x = _n / 7
label variable x "The x"
parqit save `"`tree'"', replace data partition_by(year month)
python:
from sfi import Macro, Scalar
import os, glob, hashlib, shutil
import pyarrow.parquet as pq
tree = Macro.getLocal("tree")
leaf1 = os.path.join(tree, "year=2024", "month=1")
files = sorted(glob.glob(os.path.join(leaf1, "*.parquet")))
assert len(files) == 1
shutil.copyfile(files[0], os.path.join(leaf1, "second.parquet"))
def shamap():
    out = {}
    for p in glob.glob(os.path.join(tree, "**", "*.parquet"), recursive=True):
        out[os.path.relpath(p, tree)] = hashlib.sha256(open(p, "rb").read()).hexdigest()
    return out
def leaf_rows(month):
    """the (id, x) multiset of a partition's files; None when a file's own key
    disagrees with its directory"""
    rows = []
    for p in sorted(glob.glob(os.path.join(tree, "year=2024", "month=%d" % month, "*.parquet"))):
        t = pq.read_table(p, partitioning=None)
        if "month" in t.column_names and set(t.column("month").to_pylist()) - {month}:
            return None
        rows += list(zip(t.column("id").to_pylist(), t.column("x").to_pylist()))
    return sorted(rows)
def footers_ok():
    for p in glob.glob(os.path.join(tree, "**", "*.parquet"), recursive=True):
        md = pq.ParquetFile(p).schema_arrow.metadata or {}
        if not any(k.startswith(b"parqit.") for k in md):
            return False
    return True
before = shamap()
base1 = leaf_rows(1)
Scalar.setValue("base_ok", int(len(before) == 4 and len(base1) == 200))
end
if (scalar(base_ok) != 1) {
    di as err "FAIL: the starting tree is not 3 partitions with a two-file month 1"
    local ++fails
}

* ---------- the result: month 1 (existing, two files) and month 4 (new) ------
clear
set obs 40
gen int year = 2024
gen byte month = cond(_n <= 20, 1, 4)
gen long id = 1000 + _n
gen double x = _n / 7
label variable x "The x"
tempfile result
save `"`result'"'

* a staged partition that cannot be listed: loud, the tree untouched
python: import os; os.environ["PARQIT_TEST_FAIL_PARTITION_LIST"] = "1"
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_list = _rc
python: import os; os.environ.pop("PARQIT_TEST_FAIL_PARTITION_LIST", None)
* a publish failure midway: every touched file restored
python: import os; os.environ["PARQIT_TEST_FAIL_OUTPUT_PUBLISH"] = "1"
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_pub = _rc
python: import os; os.environ.pop("PARQIT_TEST_FAIL_OUTPUT_PUBLISH", None)
python:
from sfi import Scalar
Scalar.setValue("same_after_failures", int(shamap() == before))
end
if (`rc_list' == 0 | `rc_pub' == 0 | scalar(same_after_failures) != 1) {
    di as err "FAIL: a failed append (listing rc `rc_list', publish rc `rc_pub') changed the tree or succeeded"
    local ++fails
}

* append: the new files beside the two old ones; months 2 and 3 untouched
qui parqit save `"`tree'"', data partition_by(year month) partitions(append)
python:
from sfi import Scalar
after = shamap()
new1 = [(1000 + n, n / 7) for n in range(1, 21)]
new4 = [(1000 + n, n / 7) for n in range(21, 41)]
ok = int(leaf_rows(1) == sorted(base1 + new1) and leaf_rows(4) == sorted(new4)
         and all(after.get(k) == v for k, v in before.items())
         and len(glob.glob(os.path.join(leaf1, "*.parquet"))) == 3 and footers_ok())
Scalar.setValue("append_ok", ok)
before_replace = after
end
if (scalar(append_ok) != 1) {
    di as err "FAIL: append into a two-file partition lost or changed rows or files"
    local ++fails
}

* replace: month 1's whole leaf swapped (its three files go), others untouched
use `"`result'"', clear
qui parqit save `"`tree'"', data partition_by(year month) partitions(replace)
python:
from sfi import Scalar
after = shamap()
untouched = [k for k in before_replace if not k.startswith(os.path.join("year=2024", "month=1")) and
             not k.startswith(os.path.join("year=2024", "month=4"))]
ok = int(leaf_rows(1) == sorted(new1) and leaf_rows(4) == sorted(new4)
         and all(after.get(k) == before_replace[k] for k in untouched)
         and len(glob.glob(os.path.join(leaf1, "*.parquet"))) == 1 and footers_ok())
Scalar.setValue("replace_ok", ok)
end
if (scalar(replace_ok) != 1) {
    di as err "FAIL: replace of a multi-file partition left old files or touched others"
    local ++fails
}

* ---------- G6 and PART-SCHEMA-ALL-1 (audit of the execution, 25 Oct 2026) -----
* A new tree of 3 months, and a result of 6,000 rows over months 1 and 4 that
* the engine writes in several files per partition (PARQIT_TEST_PARTITION_EVICT:
* one open file, flushed every 1,024 rows). A listing that fails after the first
* incoming file moved, an entry that cannot be read and staged partitions that
* cannot be listed all fail loudly with the tree as it was; the append then adds
* every file with exact footers; a sibling file whose columns or parqit.*
* metadata differ from the result, anywhere in the tree, is refused.
local tree2 `"`dir'/tree2"'
clear
set obs 300
gen int year = 2024
gen byte month = 1 + mod(_n - 1, 3)
gen long id = _n
gen double x = _n / 7
label variable x "The x"
parqit save `"`tree2'"', replace data partition_by(year month)
clear
set obs 6000
gen int year = 2024
gen byte month = cond(mod(_n, 2), 1, 4)
gen long id = 10000 + _n
gen double x = _n / 7
label variable x "The x"
python:
from sfi import Macro
import os, glob, hashlib
import pyarrow.parquet as pq
tree2 = Macro.getLocal("tree2")
def shamap2():
    return {os.path.relpath(p, tree2): hashlib.sha256(open(p, "rb").read()).hexdigest()
            for p in glob.glob(os.path.join(tree2, "**", "*.parquet"), recursive=True)}
def rows2(month):
    out = []
    for p in sorted(glob.glob(os.path.join(tree2, "year=2024", "month=%d" % month, "*.parquet"))):
        t = pq.read_table(p, partitioning=None)
        out += list(zip(t.column("id").to_pylist(), t.column("x").to_pylist()))
    return sorted(out)
ref = pq.ParquetFile(glob.glob(os.path.join(tree2, "year=2024", "month=2", "*.parquet"))[0])
ref_schema = ref.schema_arrow.remove_metadata()
ref_meta = {k: v for k, v in (ref.schema_arrow.metadata or {}).items() if k.startswith(b"parqit.")}
before2 = shamap2()
base2 = {m: rows2(m) for m in (1, 2, 3)}
os.environ["PARQIT_TEST_PARTITION_EVICT"] = "1"
# the forced suite's save hooks change the batches the engine partitions, and with them whether it
# writes several files: this section tests the publication, so it writes with the default save
saved_env = {k: os.environ.pop(k) for k in ("PARQIT_TEST_SAVE_BLOCK_ROWS", "PARQIT_FILL_THREADS") if k in os.environ}
end

python: import os; os.environ["PARQIT_TEST_FAIL_PARTITION_LIST"] = "2"
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_mid = _rc
python: import os; os.environ.pop("PARQIT_TEST_FAIL_PARTITION_LIST", None)
python: import os; os.environ["PARQIT_TEST_FAIL_PARTITION_STAT"] = "1"
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_stat = _rc
python: import os; os.environ.pop("PARQIT_TEST_FAIL_PARTITION_STAT", None)
python: import os; os.environ["PARQIT_TEST_FAIL_PARTITION_LEAVES"] = "1"
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_leaves = _rc
python: import os; os.environ.pop("PARQIT_TEST_FAIL_PARTITION_LEAVES", None)
python:
from sfi import Scalar
Scalar.setValue("same2", int(shamap2() == before2))
end
if (`rc_mid' == 0 | `rc_stat' == 0 | `rc_leaves' == 0 | scalar(same2) != 1) {
    di as err "FAIL: a failure midway through the append (rc `rc_mid', `rc_stat', `rc_leaves') changed the tree or succeeded"
    local ++fails
}

* on one thread the engine meets the partitions in the same order every time
* and evicts them alternately, so each gets several files; on many threads a
* thread given one vector holds 1,024 rows per partition, flushes none early
* and may write one file per partition (25 Oct 2026)
parqit set threads 1
qui parqit save `"`tree2'"', data partition_by(year month) partitions(append)
parqit set threads 4
python:
from sfi import Scalar
after2 = shamap2()
added1 = [k for k in after2 if k.startswith(os.path.join("year=2024", "month=1")) and k not in before2]
added4 = [k for k in after2 if k.startswith(os.path.join("year=2024", "month=4"))]
new1 = [(10000 + n, n / 7) for n in range(1, 6001) if n % 2]
new4 = [(10000 + n, n / 7) for n in range(1, 6001) if not n % 2]
def footer_exact(p):
    s = pq.ParquetFile(p).schema_arrow
    meta = {k: v for k, v in (s.metadata or {}).items() if k.startswith(b"parqit.")}
    return s.remove_metadata().equals(ref_schema) and meta == ref_meta
parts = {"several files in month 1": len(added1) > 1, "several files in month 4": len(added4) > 1,
         "rows of months 1 and 4": rows2(1) == sorted(base2[1] + new1) and rows2(4) == sorted(new4),
         "rows of months 2 and 3": rows2(2) == base2[2] and rows2(3) == base2[3],
         "old files unchanged": all(after2.get(k) == v for k, v in before2.items()),
         "footers": all(footer_exact(os.path.join(tree2, k)) for k in after2)}
ok = all(parts.values())
if not ok:
    print("v186 append section:", parts, "files added to months 1 and 4:", len(added1), len(added4))
Scalar.setValue("append2_ok", int(ok))
end
if (scalar(append2_ok) != 1) {
    di as err "FAIL: an append of several files per partition lost rows, changed old files or wrote other footers"
    local ++fails
}

* a sibling with other columns, then one with other parqit.* metadata, in month 3
python:
import json
import pyarrow as pa
leaf3 = os.path.join(tree2, "year=2024", "month=3")
pq.write_table(pa.table({"id": pa.array([1], pa.int32()), "x": pa.array([0.5], pa.float32())}),
               os.path.join(leaf3, "odd_columns.parquet"))
before_odd = shamap2()
end
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_cols = _rc
python:
from sfi import Scalar
same_cols = shamap2() == before_odd
os.remove(os.path.join(leaf3, "odd_columns.parquet"))
src = glob.glob(os.path.join(leaf3, "*.parquet"))[0]
t = pq.read_table(src, partitioning=None)
md = dict(t.schema.metadata)
schema = json.loads(md[b"parqit.schema"])
for v in schema["vars"]:
    if v.get("name") == "x":
        v["varlab"] = "Another label"
md[b"parqit.schema"] = json.dumps(schema).encode()
pq.write_table(t.replace_schema_metadata(md), os.path.join(leaf3, "odd_labels.parquet"))
before_odd = shamap2()
Scalar.setValue("same_cols", int(same_cols))
end
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_meta = _rc
python:
from sfi import Scalar
Scalar.setValue("same_meta", int(shamap2() == before_odd))
os.remove(os.path.join(leaf3, "odd_labels.parquet"))
os.environ.pop("PARQIT_TEST_PARTITION_EVICT", None)
os.environ.update(saved_env)
end
if (`rc_cols' == 0 | `rc_meta' == 0 | scalar(same_cols) != 1 | scalar(same_meta) != 1) {
    di as err "FAIL: an append into a tree with a disagreeing sibling (rc `rc_cols', `rc_meta') succeeded or changed it"
    local ++fails
}

* Codex audit (finding 4): the reader takes every *.parquet below the tree,
* hidden ones and those in hidden directories included, so the check takes them
* too, a file named ".parquet" among them; what the reader does not take
* (.DS_Store, a hidden directory without Parquet files) changes nothing
python:
import hashlib
def allmap():
    out = {}
    for top, dirs, files in os.walk(tree2):
        for f in files:
            p = os.path.join(top, f)
            out[os.path.relpath(p, tree2)] = hashlib.sha256(open(p, "rb").read()).hexdigest()
    return out
pq.write_table(t.replace_schema_metadata(md), os.path.join(leaf3, ".hidden_labels.parquet"))
before_hidden = allmap()
end
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_hidden = _rc
python:
from sfi import Scalar
Scalar.setValue("same_hidden", int(allmap() == before_hidden))
os.remove(os.path.join(leaf3, ".hidden_labels.parquet"))
pq.write_table(t.replace_schema_metadata(md), os.path.join(leaf3, ".parquet"))
before_dot = allmap()
end
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_dot = _rc
python:
from sfi import Scalar
Scalar.setValue("same_dot", int(allmap() == before_dot))
os.remove(os.path.join(leaf3, ".parquet"))
open(os.path.join(tree2, ".DS_Store"), "wb").write(b"\0\0\0\1Bud1")
os.makedirs(os.path.join(tree2, "year=2024", ".ipynb_checkpoints"), exist_ok=True)
open(os.path.join(tree2, "year=2024", ".ipynb_checkpoints", "notes.txt"), "w").write("not data")
end
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_benign = _rc
if (`rc_hidden' == 0 | scalar(same_hidden) != 1 | `rc_dot' == 0 | scalar(same_dot) != 1 | `rc_benign' != 0) {
    di as err "FAIL: a hidden Parquet file that disagrees was not refused (rc `rc_hidden', `rc_dot'), or a hidden entry the reader does not take was (rc `rc_benign')"
    local ++fails
}

* Codex audit, round 2: the first file that cannot be read or differs, in
* file order, is named whatever the chunk of footers read at once (1, 2, 64)
tempfile k1 k2 k64
python:
pq.write_table(t.replace_schema_metadata(md), os.path.join(leaf3, "odd_labels.parquet"))
open(os.path.join(tree2, "year=2024", "month=2", "cut.parquet"), "wb").write(b"PAR1\0\0\0\0")
saved_chunk = os.environ.get("PARQIT_TEST_TREE_CHUNK_FILES")
os.environ["PARQIT_TEST_TREE_CHUNK_FILES"] = "1"
end
log using `"`k1'.log"', text replace name(_v186k)
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_k1 = _rc
log close _v186k
python: import os; os.environ["PARQIT_TEST_TREE_CHUNK_FILES"] = "2"
log using `"`k2'.log"', text replace name(_v186k)
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_k2 = _rc
log close _v186k
python: import os; os.environ["PARQIT_TEST_TREE_CHUNK_FILES"] = "64"
log using `"`k64'.log"', text replace name(_v186k)
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_k64 = _rc
log close _v186k
python:
from sfi import Scalar
def named(p):
    return [l.strip() for l in open(p, errors="replace") if "cut.parquet" in l or "odd_labels.parquet" in l]
m = [named(r"`k1'.log"), named(r"`k2'.log"), named(r"`k64'.log")]
Scalar.setValue("same_named", int(m[0] != [] and m[0] == m[1] == m[2]))
os.remove(os.path.join(leaf3, "odd_labels.parquet"))
os.remove(os.path.join(tree2, "year=2024", "month=2", "cut.parquet"))
if saved_chunk is None:
    os.environ.pop("PARQIT_TEST_TREE_CHUNK_FILES", None)
else:
    os.environ["PARQIT_TEST_TREE_CHUNK_FILES"] = saved_chunk
end
if (`rc_k1' == 0 | `rc_k1' != `rc_k2' | `rc_k1' != `rc_k64' | scalar(same_named) != 1) {
    di as err "FAIL: a tree with a disagreeing file and a truncated one was refused differently by chunk (rc `rc_k1' `rc_k2' `rc_k64')"
    local ++fails
}

* Codex audit, round 2, finding 3: as the reader (it never follows a link to
* a directory, reads a link to a regular file, skips special files): a
* partition that is a link to a directory is refused, rows added below it
* would not be read; a link to a disagreeing file is refused; a hidden link
* to a directory and a FIFO named *.parquet are passed over
python:
import tempfile
side = tempfile.mkdtemp(dir=os.path.dirname(tree2))
pq.write_table(t.replace_schema_metadata(md), os.path.join(side, "odd_labels.parquet"))
os.symlink(side, os.path.join(tree2, "year=2024", "month=9"))
before_link = allmap()
end
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_dlink = _rc
python:
from sfi import Scalar
Scalar.setValue("same_dlink", int(allmap() == before_link))
os.remove(os.path.join(tree2, "year=2024", "month=9"))
os.symlink(side, os.path.join(tree2, ".archive"))
os.symlink(os.path.join(side, "odd_labels.parquet"), os.path.join(leaf3, "link_labels.parquet"))
before_flink = allmap()
end
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_flink = _rc
python:
from sfi import Scalar
Scalar.setValue("same_flink", int(allmap() == before_flink))
os.remove(os.path.join(leaf3, "link_labels.parquet"))
os.mkfifo(os.path.join(leaf3, "pipe.parquet"))
end
capture noisily parqit save `"`tree2'"', data partition_by(year month) partitions(append)
local rc_fifo = _rc
python:
os.remove(os.path.join(leaf3, "pipe.parquet"))
os.remove(os.path.join(tree2, ".archive"))
end
if (`rc_dlink' == 0 | scalar(same_dlink) != 1 | `rc_flink' == 0 | scalar(same_flink) != 1 | `rc_fifo' != 0) {
    di as err "FAIL: a partition linked to a directory (rc `rc_dlink') or a link to a disagreeing file (rc `rc_flink') was not refused, or a FIFO with a hidden link to a directory was not passed over (rc `rc_fifo')"
    local ++fails
}

if (`fails' == 0) di as result "VERDICT(v186_partition_multifile): PASS"
else di as err "VERDICT(v186_partition_multifile): FAIL (`fails')"
