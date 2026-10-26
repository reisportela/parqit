* Fable final audit (25 Oct 2026): partitions(replace|append) checks the
* whole existing tree whatever its first branch holds, and publishes so that a
* failure leaves the tree as it was.
*   A0/A1: a first-listed branch whose leaf is empty, holds only a hidden file
*   that disagrees, is a link to an empty directory, or holds only a FIFO and
*   a broken link named *.parquet, no longer turns the check off (or hangs on
*   the FIFO); a link to a file of the tree itself is refused.
*   Publication: a failure after a partition was swapped in, or after a file
*   was added, is rolled back; an appended name already taken is refused; a
*   rollback that cannot remove what it added says the tree is not as it was.
* Every refused or failed update leaves the tree byte-identical (pyarrow and
* hashlib, files not followed through links). Needs pyarrow from Stata's Python.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile base log1
local tree `"`base'_tree"'

* the tree: one real branch, year=2024 month=1, v labelled "value"
set obs 30
gen int year = 2024
gen byte month = 1
gen double v = _n
label variable v "value"
parqit save `"`tree'"', data partition_by(year month) replace

python:
import os, stat, shutil, hashlib, tempfile, json
import pyarrow.parquet as pq
from sfi import Macro, Scalar
tree = Macro.getLocal("tree")
real = os.path.join(tree, "year=2024")
def allmap():
    out = {}
    for top, dirs, files in os.walk(tree):
        for f in files + dirs:
            p = os.path.join(top, f)
            st = os.lstat(p)
            if stat.S_ISREG(st.st_mode):
                out[os.path.relpath(p, tree)] = hashlib.sha256(open(p, "rb").read()).hexdigest()
            elif stat.S_ISDIR(st.st_mode):
                out[os.path.relpath(p, tree)] = "directory"
            else:
                out[os.path.relpath(p, tree)] = "link:" + os.readlink(p) if stat.S_ISLNK(st.st_mode) else "special"
    return out
def first_branch(make):
    # a new branch listed before the real one where the directory keeps
    # insertion order: set the real branch aside, make the new one, put it back
    for y in (2023, 2022, 2025, 2021):
        d = os.path.join(tree, "year=%d" % y)
        os.rename(real, real + ".aside")
        make(d)
        os.rename(real + ".aside", real)
        if os.listdir(tree)[0] == "year=%d" % y:
            return d
        shutil.rmtree(d) if not os.path.islink(d) else os.remove(d)
    d = os.path.join(tree, "year=2023")
    make(d)
    print("NOTE v191: could not obtain first-branch enumeration order; order-independent assertions still run")
    return d
src = os.path.join(real, "month=1", [f for f in os.listdir(os.path.join(real, "month=1")) if f.endswith(".parquet")][0])
t = pq.read_table(src, partitioning=None)
md = dict(t.schema.metadata)
schema = json.loads(md[b"parqit.schema"])
for v in schema["vars"]:
    if v.get("name") == "v":
        v["varlab"] = "OTHER"
md[b"parqit.schema"] = json.dumps(schema).encode()
other = t.replace_schema_metadata(md)
end

* the data to append: month 2 of 2024, v labelled "value" (as the tree)
replace month = 2

* A0: an empty first-listed leaf, then a disagreeing append
python:
odd = first_branch(lambda d: os.makedirs(os.path.join(d, "month=1")))
before = allmap()
end
label variable v "another label"
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_empty = _rc
label variable v "value"
python:
Scalar.setValue("same_empty", int(allmap() == before))
shutil.rmtree(odd)
end

* A0: a first-listed leaf holding only a hidden file that disagrees
python:
def hidden(d):
    os.makedirs(os.path.join(d, "month=1"))
    pq.write_table(other, os.path.join(d, "month=1", ".old.parquet"))
odd = first_branch(hidden)
before = allmap()
end
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_hidden = _rc
python:
Scalar.setValue("same_hidden", int(allmap() == before))
shutil.rmtree(odd)
end

* A0: a partition that is a link to an empty directory
python:
side = tempfile.mkdtemp(dir=os.path.dirname(tree))
def linked(d):
    os.makedirs(d)
    os.symlink(side, os.path.join(d, "month=1"))
odd = first_branch(linked)
before = allmap()
os.environ["PARQIT_TEST_TRACE_PARTITIONS"] = "1"
end
replace year = 2023
replace month = 1
log using `"`log1'.log"', text replace name(_v191)
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_link = _rc
log close _v191
replace year = 2024
replace month = 2
python:
Scalar.setValue("same_link", int(allmap() == before and os.listdir(side) == []))
trace = open(Macro.getLocal("log1") + ".log", errors="replace").read()
Scalar.setValue("before_copy", int("PARQIT_PARTITIONS_TRACE first_walk" in trace and "PARQIT_PARTITIONS_TRACE COPY" not in trace))
os.environ.pop("PARQIT_TEST_TRACE_PARTITIONS", None)
shutil.rmtree(odd)
end

* A1: a first-listed leaf holding only a FIFO and a broken link named
* *.parquet (the reader reads neither), then a disagreeing append: compared
* with the real file, refused, and never opening the FIFO
python:
def special(d):
    os.makedirs(os.path.join(d, "month=1"))
    os.mkfifo(os.path.join(d, "month=1", "pipe.parquet"))
    os.symlink(os.path.join(side, "gone.parquet"), os.path.join(d, "month=1", "gone.parquet"))
odd = first_branch(special)
before = allmap()
end
label variable v "another label"
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_fifo = _rc
label variable v "value"
python:
Scalar.setValue("same_fifo", int(allmap() == before))
shutil.rmtree(odd)
end

* a link to a file of the tree itself: the reader reads the file twice
python:
alias = os.path.join(real, "month=1", "alias.parquet")
os.symlink(src, alias)
before = allmap()
end
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_alias = _rc
python:
Scalar.setValue("same_alias", int(allmap() == before))
os.remove(alias)
end

if (`rc_empty' == 0 | scalar(same_empty) != 1 | `rc_hidden' == 0 | scalar(same_hidden) != 1 | ///
    `rc_link' == 0 | scalar(same_link) != 1 | `rc_fifo' == 0 | scalar(same_fifo) != 1 | ///
    `rc_alias' == 0 | scalar(same_alias) != 1 | scalar(before_copy) != 1) {
    di as err "FAIL: an update the first branch hid from the check was not refused, or changed the tree (rc `rc_empty' `rc_hidden' `rc_link' `rc_fifo' `rc_alias')"
    local ++fails
}

* publication: a failure after the first partition was swapped in (replace of
* month 1 and a new month 3), and after the first file was added (append into
* month 1), is rolled back
replace month = cond(_n <= 15, 1, 3)
python:
before = allmap()
os.environ["PARQIT_TEST_FAULT"] = "publish_step:1"
end
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(replace)
local rc_step = _rc
python:
Scalar.setValue("same_step", int(allmap() == before))
os.environ["PARQIT_TEST_FAULT"] = "publish_file:0"
end
replace month = 1
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_file = _rc
python:
Scalar.setValue("same_file", int(allmap() == before))
os.environ.pop("PARQIT_TEST_FAULT", None)
end

* an appended name already taken is refused, the file there untouched
python: import os; os.environ["PARQIT_TEST_APPEND_TOKEN"] = "00000000000000aa"
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_first = _rc
python:
Scalar.setValue("taken_ok", int(any(f.endswith("_00000000000000aa.parquet") for f in os.listdir(os.path.join(real, "month=1")))))
before = allmap()
end
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_taken = _rc
python:
Scalar.setValue("same_taken", int(allmap() == before))
os.environ.pop("PARQIT_TEST_APPEND_TOKEN", None)
end

* a rollback that cannot remove what it published says the tree is not as it
* was: the added file really remains, and all original files stay identical
replace month = cond(_n <= 15, 1, 4)
python:
before = allmap()
os.environ["PARQIT_TEST_FAIL_OUTPUT_PUBLISH"] = "1"
os.environ["PARQIT_TEST_FAIL_ROLLBACK_REMOVE"] = "1"
end
log using `"`log1'.log"', text replace name(_v191)
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc_remove = _rc
log close _v191
python:
os.environ.pop("PARQIT_TEST_FAIL_OUTPUT_PUBLISH", None)
os.environ.pop("PARQIT_TEST_FAIL_ROLLBACK_REMOVE", None)
message = " ".join(open(Macro.getLocal("log1") + ".log", errors="replace").read().split())
Scalar.setValue("said_not", int("is NOT as it was" in message and "retained for recovery" not in message))
after = allmap()
Scalar.setValue("leftover", int(any(p not in before and p.endswith(".parquet") for p in after)))
Scalar.setValue("old_identical", int(all(after.get(p) == v for p, v in before.items())))
end

if (`rc_step' == 0 | scalar(same_step) != 1 | `rc_file' == 0 | scalar(same_file) != 1 | ///
    `rc_first' != 0 | scalar(taken_ok) != 1 | `rc_taken' == 0 | scalar(same_taken) != 1 | ///
    `rc_remove' == 0 | scalar(said_not) != 1 | scalar(leftover) != 1 | scalar(old_identical) != 1) {
    di as err "FAIL: a publication failure was not rolled back, a taken name was not refused, or a failed rollback did not say so (rc `rc_step' `rc_file' `rc_first' `rc_taken' `rc_remove')"
    local ++fails
}

if (`fails' == 0) di as result "VERDICT(v191_partition_tree_gate): PASS"
else di as err "VERDICT(v191_partition_tree_gate): FAIL (`fails')"
