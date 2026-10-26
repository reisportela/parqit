* Codex final audit: link dependencies, real rollback failures, both walks,
* publication before/after faults and reference filenames. Synthetic only.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
local fails 0
tempfile base trace
local template `"`base'_template"'
local tree `"`base'_tree"'
set obs 6
gen int year = 1
gen byte month = 1
gen double v = _n
label variable v "value"
parqit save `"`template'"', data partition_by(year month) replace

python:
import os, stat, shutil, hashlib, json, re
from pathlib import Path
import pyarrow.parquet as pq
import pyarrow.dataset as ds
from sfi import Macro, Scalar
template = Path(Macro.getLocal("template"))
base = Macro.getLocal("base")
side = Path(base + "_external")
side.mkdir()
src = next(template.rglob("*.parquet"))
external = side / "x.parquet"
shutil.copyfile(src, external)
table = pq.read_table(src, partitioning=None)
md = dict(table.schema.metadata)
schema = json.loads(md[b"parqit.schema"])
for column in schema["vars"]:
    if column.get("name") == "v": column["varlab"] = "OTHER"
md[b"parqit.schema"] = json.dumps(schema).encode()
other = side / "other.parquet"
pq.write_table(table.replace_schema_metadata(md), other)
def inventory(root):
    out = {}
    for top, dirs, files in os.walk(root):
        for name in dirs + files:
            p = Path(top) / name
            mode = p.lstat().st_mode
            out[str(p.relative_to(root))] = ("link:" + os.readlink(p) if stat.S_ISLNK(mode) else
                "directory" if stat.S_ISDIR(mode) else hashlib.sha256(p.read_bytes()).hexdigest())
    return out
def fresh(name):
    root = Path(base + "_" + name)
    shutil.copytree(template, root)
    Macro.setLocal("tree", str(root))
    return root
def message():
    raw = Path(Macro.getLocal("trace") + ".log").read_text(errors="replace")
    return " ".join(re.sub(r"\n> ", "", raw).split())
def report(name, good):
    print("CHECK v192 " + name + (": PASS" if good else ": FAIL"))
    Scalar.setValue("check", int(good))
def prepare_link(scenario):
    global root, before, kind
    kind = scenario
    root = fresh(kind)
    bridge = root / "year=1/month=1/bridge.parquet"
    os.symlink(external, bridge)
    alias = root / "year=2/month=1/alias.parquet"
    alias.parent.mkdir(parents=True)
    if kind == "chain":
        os.symlink("../../year=1/month=1/bridge.parquet", alias)
    elif kind == "directory":
        portal = side / "portal"
        os.symlink(bridge.parent, portal)
        os.symlink(portal / bridge.name, alias)
    elif kind in ("real_directory", "real_directory_absolute"):
        passage = root / "year=1/month=1/.pass"
        passage.mkdir()
        if kind == "real_directory":
            target = "../../year=1/month=1/.pass/../../../../" + side.name + "/x.parquet"
        else:
            target = str(passage / ("../../../../" + side.name + "/x.parquet"))
        os.symlink(target, alias)
    else:
        target = external
        for i in range(41):
            link = side / ("hop_%02d" % i)
            os.symlink(target, link)
            target = link
        os.symlink(target, alias)
    before = inventory(root)
def prepare_external(scenario):
    global root, alias, kind
    kind = scenario
    root = fresh(kind)
    alias = root / "year=2/month=1/alias.parquet"
    alias.parent.mkdir(parents=True)
    if kind == "direct":
        os.symlink(external, alias)
    elif kind == "external_chain":
        link = side / "bridge.parquet"
        os.symlink("x.parquet", link)
        os.symlink(link, alias)
    elif kind == "external_relative":
        os.symlink("../../../" + side.name + "/x.parquet", alias)
    elif kind == "external_relative_portal":
        real = side / "outside_real"
        real.mkdir()
        shutil.copyfile(external, real / "x.parquet")
        os.symlink("outside_real", side / "outside_portal")
        os.symlink("../../../" + side.name + "/outside_portal/x.parquet", alias)
    else:
        current = side / "nested"
        current.mkdir()
        route = current
        for i in range(6):
            real = current / ("d%d" % i)
            real.mkdir()
            os.symlink(real.name, current / ("p%d" % i))
            current = real
            route = route / ("p%d" % i)
        shutil.copyfile(external, current / "x.parquet")
        os.symlink(route / "x.parquet", alias)
    os.environ["PARQIT_TEST_TRACE_PARTITIONS"] = "1"
def verify_external():
    global steps
    os.environ.pop("PARQIT_TEST_TRACE_PARTITIONS", None)
    good = int(Macro.getLocal("rc")) == 0
    if good:
        result = ds.dataset(root, format="parquet", partitioning="hive").to_table()
        kept = result.filter(__import__("pyarrow").compute.equal(result["year"], 2))
        updated = result.filter(__import__("pyarrow").compute.equal(result["year"], 1))
        good = (kept.num_rows == 6 and sorted(kept["v"].to_pylist()) == list(range(1, 7)) and
            updated.num_rows == 6 and sorted(updated["v"].to_pylist()) == list(range(101, 107)) and
            pq.ParquetFile(alias).schema_arrow.metadata == table.schema.metadata)
    steps = re.findall(r"PARQIT_PARTITIONS_TRACE (first_walk|COPY|second_walk|publication)", message())
    report(kind, good)
def prepare_reference(scenario):
    global root, reference, before, kind
    kind = scenario
    root = fresh("reference_" + kind)
    reference = str(next(root.rglob("*.parquet")))
    before = inventory(root)
end

* Relative file hop through another partition, and an external directory hop
* back through the tree: the external final target does not make them safe.
foreach kind in chain directory real_directory real_directory_absolute long_chain {
    python: prepare_link(Macro.getLocal("kind"))
    capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(replace)
    local rc = _rc
    python: report(kind, int(Macro.getLocal("rc")) == 198 and inventory(root) == before)
    if scalar(check) != 1 local ++fails
}

* External-only two-hop and direct links are accepted. PyArrow checks every
* retained cell and every updated cell, together with the original metadata.
replace v = v + 100
foreach kind in direct external_chain external_relative external_relative_portal external_nested {
    python: prepare_external(Macro.getLocal("kind"))
    log using `"`trace'.log"', text replace name(_v192)
    capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(replace)
    local rc = _rc
    log close _v192
    python: verify_external()
    if scalar(check) != 1 local ++fails
    python: report("trace_" + kind, steps == ["first_walk", "COPY", "second_walk", "publication"])
    if scalar(check) != 1 local ++fails
}
replace v = v - 100

* Failure before the leaf rename removes even newly created parent directories.
python:
root = fresh("parents")
before = inventory(root)
os.environ["PARQIT_TEST_FAULT"] = "publish_before_leaf:0"
end
replace year = 3
replace month = 7
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc = _rc
python:
os.environ.pop("PARQIT_TEST_FAULT", None)
report("parents_before_mutation", int(Macro.getLocal("rc")) != 0 and inventory(root) == before)
end
if scalar(check) != 1 local ++fails
replace year = 1
replace month = 1

* An exception removing an appended file leaves that file, retains every old
* byte, and never invents a recovery copy of a previous partition.
python:
root = fresh("remove_exception")
before = inventory(root)
os.environ["PARQIT_TEST_FAULT"] = "publish_file:0,rollback_remove:0"
end
log using `"`trace'.log"', text replace name(_v192)
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc = _rc
log close _v192
python:
os.environ.pop("PARQIT_TEST_FAULT", None)
after = inventory(root)
good = (int(Macro.getLocal("rc")) != 0 and
    any(p.endswith(".parquet") and p not in before for p in after) and
    all(after.get(p) == value for p, value in before.items()) and
    "is NOT as it was" in message() and "retained for recovery" not in message())
report("remove_exception", good)
end
if scalar(check) != 1 local ++fails

* One restoration throws; the next still restores its original bytes. Only
* the one real recovery copy is named, and the transaction root is retained.
python:
root = fresh("restore_exception")
shutil.copytree(root / "year=1", root / "year=2")
before = inventory(root)
os.environ["PARQIT_TEST_FAULT"] = "publish_step:3,rollback_restore:0"
end
replace year = cond(_n <= 3, 1, 2)
replace v = v + 100
log using `"`trace'.log"', text replace name(_v192)
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(replace)
local rc = _rc
log close _v192
replace year = 1
replace v = v - 100
python:
os.environ.pop("PARQIT_TEST_FAULT", None)
copies = list(root.parent.glob(root.name + ".parqit_txn_*/aside/year=*/month=1"))
good = int(Macro.getLocal("rc")) != 0 and len(copies) == 1
if good:
    aside = copies[0]
    old_year = aside.parent.name
    restored_year = "year=2" if old_year == "year=1" else "year=1"
    after = inventory(root)
    old_files = {p: v for p, v in before.items() if p.startswith(restored_year + "/")}
    good = (inventory(aside) == inventory(template / "year=1/month=1") and
        all(after.get(p) == v for p, v in old_files.items()) and
        not (root / old_year / "month=1").exists() and
        str(aside) in re.sub(r"\s+", "", message()) and
        message().count("retained for recovery at") == 1 and "is NOT as it was" in message())
report("restore_exception", good)
end
if scalar(check) != 1 local ++fails

* A tree populated after COPY is compared by the second walk; only the
* planted file remains, and its exact reference filename is reported.
python:
root = Path(base + "_populated")
root.mkdir()
Macro.setLocal("tree", str(root))
relative = "year=9/month=1/planted.parquet"
os.environ["PARQIT_TEST_TRACE_PARTITIONS"] = "1"
os.environ["PARQIT_TEST_PARTITIONS_PLANT_SOURCE"] = str(other)
os.environ["PARQIT_TEST_PARTITIONS_PLANT_TARGET"] = relative
end
log using `"`trace'.log"', text replace name(_v192)
capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
local rc = _rc
log close _v192
python:
for key in ("PARQIT_TEST_TRACE_PARTITIONS", "PARQIT_TEST_PARTITIONS_PLANT_SOURCE", "PARQIT_TEST_PARTITIONS_PLANT_TARGET"):
    os.environ.pop(key, None)
steps = re.findall(r"PARQIT_PARTITIONS_TRACE (first_walk|COPY|second_walk|publication)", message())
planted = root / relative
good = (int(Macro.getLocal("rc")) == 198 and steps == ["first_walk", "COPY", "second_walk"] and
    planted.is_file() and planted.read_bytes() == other.read_bytes() and
    list(root.rglob("*.parquet")) == [planted] and relative in message())
report("second_walk_populated", good)
end
if scalar(check) != 1 local ++fails

* Both first-file refusals name their reference, not only "the existing tree".
foreach kind in metadata schema {
    python: prepare_reference(Macro.getLocal("kind"))
    if "`kind'" == "metadata" label variable v "another label"
    if "`kind'" == "schema" rename v w
    log using `"`trace'.log"', text replace name(_v192)
    capture noisily parqit save `"`tree'"', data partition_by(year month) partitions(append)
    local rc = _rc
    log close _v192
    if "`kind'" == "metadata" label variable v "value"
    if "`kind'" == "schema" rename w v
    python: report("reference_" + kind, int(Macro.getLocal("rc")) == 198 and reference in re.sub(r"\s+", "", message()) and inventory(root) == before)
    if scalar(check) != 1 local ++fails
}

if `fails' == 0 di as result "VERDICT(v192_codex_final_partitions): PASS"
else di as err "VERDICT(v192_codex_final_partitions): FAIL (`fails')"
