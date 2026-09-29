* V140: the release audit's R identity, units, bounds, encoding and cleanup cases.
clear all
set more off
set varabbrev off
set linesize 255
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
tempfile stem

python:
from sfi import Macro
from pathlib import Path
import hashlib, json, os, shutil, struct, subprocess
import pyarrow.parquet as pq
import pyreadstat

stem = Macro.getLocal("stem")
Path(stem + "_utf8.csv").write_text("id,place\n1,José\n2,Porto\n", encoding="utf-8")
Path(stem + "_legacy.csv").write_bytes("id,place\n1,Москва\n".encode("cp1251"))
Path(stem + "_cp864.csv").write_bytes(b"raw\n%\n%\x80\n")
shutil.copyfile(Macro.getLocal("repo") + "/tests/fixtures/encoding/cp864.sav", stem + ".sav")
rscript = shutil.which("Rscript")
if rscript:
    result = subprocess.run([rscript, "--vanilla", Macro.getLocal("repo") +
                             "/tests/fixtures/r/make_audit_fixtures.R", stem],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0 and "DONE" in result.stdout, (result.stdout, result.stderr)
    raw = Path(stem + "_compact.rds").read_bytes()
    old = struct.pack(">ddd", 3, 1, 1)
    assert raw.count(old) == 1
    for name, state in (("length", (1e100, 1, 1)), ("first", (3, 1e100, 1)),
                        ("fraction", (3.5, 1, 1))):
        Path(stem + "_bad_" + name + ".rds").write_bytes(raw.replace(old, struct.pack(">ddd", *state)))
Macro.setLocal("has_r", "1" if rscript else "0")
end

* UTF-8 stays at its original path, including on the lazy path.
parqit use `"`stem'_utf8.csv"', clear encoding(cp1252) filename(origin)
assert _N == 2 & place[1] == "José"
assert origin == `"`stem'_utf8.csv"'
parqit use `"`stem'_utf8.csv"', name(v140) encoding(cp1252) filename(origin)
assert `"`r(bridge)'"' == ""
parqit save `"`stem'_csv_view.parquet"', replace
parqit close _all

* A syntax failure must not create a decoded copy or alter memory.
clear
set obs 1
gen guard = 999
python:
scratch = Path(os.environ.get("TMPDIR", "/tmp"))
before = set(scratch.glob("_parqit_bridge_csv_*"))
end
forvalues i = 1/3 {
    capture parqit use `"`stem'_legacy.csv"', clear encoding(cp1251) csv(header(bogus))
    assert _rc == 198
    assert _N == 1 & guard == 999
}
python:
assert set(scratch.glob("_parqit_bridge_csv_*")) == before
end

* all requires the normal writer; refusal must precede output publication.
label variable guard "Ů"
label data "Ů"
parqit save `"`stem'_seed.parquet"', data replace
parqit use `"`stem'_seed.parquet"', clear
python:
Path(stem + "_protected.parquet").write_bytes(b"existing destination")
end
capture parqit save `"`stem'_protected.parquet"', data replace copysource encoding(gbk, all)
assert _rc == 198
python:
assert Path(stem + "_protected.parquet").read_bytes() == b"existing destination"
end
parqit save `"`stem'_copy.parquet"', data replace copysource encoding(gbk)
parqit save `"`stem'_all.parquet"', data replace encoding(gbk, all)

* CP864's low percent byte is Arabic percent, also in SPSS labels.
clear
set obs 2
gen str8 raw = char(37)
replace raw = char(37) + char(128) in 2
label variable raw "%"
label data "%"
parqit save `"`stem'_memory864.parquet"', data replace encoding(ibm864, all)
parqit use `"`stem'_cp864.csv"', clear encoding(ibm864, all)
parqit save `"`stem'_csv864.parquet"', data replace
parqit save `"`stem'_spss864.parquet"' using `"`stem'.sav"', replace encoding(ibm864)

if (`has_r') {
    parqit use `"`stem'_objects.rds"', clear object([[1]])
    assert _N == 1 & id == 222
    parqit use `"`stem'_objects.rds"', clear object([[1]]_1)
    assert id == 333
    parqit use `"`stem'_objects.rds"', clear object([[1]]_2)
    assert id == 111
    parqit use `"`stem'_dates.rds"', clear
    local fmt : format d
    assert "`fmt'" == "%tc"
    assert d[1] == clock("1970-01-01 00:00:00", "YMDhms")
    assert d[2] - d[1] == 43200000
    parqit save `"`stem'_dates_back.parquet"', data replace
    parqit save `"`stem'_limits.parquet"' using `"`stem'_limits.rds"', replace
    parqit use `"`stem'_limits.parquet"', clear
    assert d[1] == -2147483647 & d[3] == 2147483647
    assert i == d
    assert t[2] == 86399.9999999
    foreach name in length first fraction {
        capture parqit save `"`stem'_protected.parquet"' using `"`stem'_bad_`name'.rds"', replace
        assert _rc == 610
        capture parqit use `"`stem'_bad_`name'.rds"', clear
        assert _rc == 610
        assert _N == 3 & d[1] == -2147483647
    }
}
else di as txt "note: Rscript not found; R-specific V140 cases skipped"

python:
def metadata(name):
    return {k.decode(): json.loads(v) for k, v in
            pq.read_metadata(stem + "_" + name + ".parquet").metadata.items() if k.startswith(b"parqit.")}

tab = pq.read_table(stem + "_csv_view.parquet").to_pydict()
assert tab["place"] == ["José", "Porto"]
assert tab["origin"] == [stem + "_utf8.csv"] * 2
assert metadata("copy")["parqit.dtalabel"] == "Ů"
assert metadata("all")["parqit.dtalabel"] == "女"
assert metadata("all")["parqit.schema"]["vars"][0]["varlab"] == "女"
expected = [b"%".decode("cp864"), b"%\x80".decode("cp864")]
assert expected == ["٪", "٪°"]
assert pq.read_table(stem + "_memory864.parquet")["raw"].to_pylist() == expected
assert pq.read_table(stem + "_csv864.parquet")["raw"].to_pylist() == expected
assert metadata("memory864")["parqit.dtalabel"] == "٪"
oracle, md = pyreadstat.read_sav(stem + ".sav", encoding="CP864", output_format="dict")
assert pq.read_table(stem + "_spss864.parquet")["raw"].to_pylist() == oracle["raw"] == ["٪"]
assert metadata("spss864")["parqit.dtalabel"] == md.file_label == "٪"
assert metadata("spss864")["parqit.schema"]["vars"][0]["varlab"] == md.column_labels[0] == "٪"
if rscript:
    import datetime
    tab = pq.read_table(stem + "_limits.parquet").to_pydict()
    assert tab["d"] == tab["i"] == [-2147483647, 0, 2147483647]
    assert tab["t"] == [0, 86399.9999999, 86399.999999]
    assert pq.read_table(stem + "_dates_back.parquet")["d"].to_pylist() == [
        datetime.datetime(1970, 1, 1), datetime.datetime(1970, 1, 1, 12)]
    assert Path(stem + "_protected.parquet").read_bytes() == b"existing destination"
end
parqit close _all
di "VERDICT(V140_R_ENCODING_AUDIT): PASS"
