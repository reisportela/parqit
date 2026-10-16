"""Independent Parquet type, value and display-format oracle (synthetic data)."""
import datetime as dt
import json
from pathlib import Path
import sys

import pyarrow as pa
import pyarrow.parquet as pq

table = pq.read_table(sys.argv[1])
assert table.num_rows == 6
assert pa.types.is_date32(table.schema.field("daily").type)
assert pa.types.is_timestamp(table.schema.field("clocktime").type)
assert table["daily"].to_pylist() == [dt.date(2026, 10, 15) + dt.timedelta(days=i) for i in range(6)]
assert table["clocktime"].to_pylist() == [dt.datetime(2026, 10, 15, 12, 34, 56) + dt.timedelta(milliseconds=i*1234) for i in range(1, 7)]
starts = {"monthly": 720, "quarterly": 240, "halfyear": 120, "weekly": 3120, "yearly": 2020}
for name, first in starts.items():
    assert pa.types.is_integer(table.schema.field(name).type), (name, table.schema)
    assert table[name].to_pylist() == [first+i for i in range(1, 7)], name
assert table["amount"].to_pylist() == [i/8 for i in range(1, 7)]
assert table["caption"].to_pylist() == [f"record {i}" for i in range(1, 7)]
schema = json.loads(table.schema.metadata[b"parqit.schema"])
columns = schema["vars"] if isinstance(schema, dict) else schema
by_name = {col["name"]: col for col in columns}
for name, fmt in {"daily": "%tdCCYY-NN-DD", "clocktime": "%tcCCYY-NN-DD_HH:MM:SS.sss", "monthly": "%tm", "quarterly": "%tq", "halfyear": "%th", "weekly": "%tw", "yearly": "%ty", "amount": "%12,3f", "caption": "%-12s"}.items():
    assert by_name[name]["fmt"] == fmt, (name, by_name[name])
# A separate synthetic fixture exercises repair of malformed display metadata.
by_name["daily"]["fmt"] = "invalid-format"
metadata = dict(table.schema.metadata)
metadata[b"parqit.schema"] = json.dumps(schema).encode()
pq.write_table(table.replace_schema_metadata(metadata), sys.argv[3])
Path(sys.argv[2]).write_text("PASS: independent types, values and formats\n")
