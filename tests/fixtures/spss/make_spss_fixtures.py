"""Regenerate the SPSS fixtures of tests/verify_suite/v127-v130 (SPSS-READ-1).

    python make_spss_fixtures.py        (needs pandas + pyreadstat)

Writes one synthetic survey in the three layouts of the SPSS system-file
format — uncompressed (survey.sav), bytecode-compressed (survey_bc.sav) and
ZLIB-compressed (survey.zsav) — with pyreadstat, i.e. ReadStat, a writer
independent of parqit's reader. The content is deterministic (fixed seed);
only the creation stamp in the file header changes between runs. It carries
what the reader must preserve: user-missing values (discrete, a range plus a
discrete value, open LO/HI ranges), value labels on numbers (integer and
non-integer keys) and on strings, a very long string, UTF-8 text with emoji,
dates, date-times, times, a duration, SPSS display formats, variable labels
(one longer than Stata's 80 characters), measurement levels, display widths,
documents, a file label, a name longer than 32 characters and one with a
period.
"""
import datetime as dt
import os

import numpy as np
import pandas as pd
import pyreadstat

HERE = os.path.dirname(os.path.abspath(__file__))
N = 60
rng = np.random.default_rng(20260926)


def with_sysmis(values, every):
    out = np.array(values, dtype=float)
    out[::every] = np.nan
    return out


base = dt.datetime(2020, 1, 1)
df = pd.DataFrame({
    "id": np.arange(1, N + 1, dtype=float),
    "q1": rng.choice([1, 2, 3, 4, 5, 97, 98, 99], N).astype(float),
    "q2": rng.choice([-9, -8, -2, -1, 1, 2, 3, 99], N).astype(float),
    "income": with_sysmis(np.round(rng.normal(1500, 900, N), 2), 11),
    "hours": rng.choice([5.0, 20.0, 40.0, 200.0, 999.0], N),
    "score": rng.choice([0.5, 1.0, 1.5, 2.0], N),
    "sex": rng.choice(["M", "F", "X", ""], N),
    "city": rng.choice(["Braga", "Porto", "Lisboa", "Guimarães", "São João 🦆", ""], N),
    "comment": ["resposta %d: " % i + "texto longo " * (i % 40) for i in range(N)],
    "bday": [dt.date(1940, 1, 1) + dt.timedelta(days=int(d)) for d in rng.integers(0, 25000, N)],
    "visit": [dt.date(2021, 6, 1) + dt.timedelta(days=int(d)) for d in rng.integers(0, 400, N)],
    "stamp": [base + dt.timedelta(seconds=int(s)) for s in rng.integers(0, 10 ** 8, N)],
    "clock": [dt.time(int(h), int(m), int(s)) for h, m, s in
              zip(rng.integers(0, 24, N), rng.integers(0, 60, N), rng.integers(0, 60, N))],
    "Satisfaction_with_public_services_overall": rng.integers(1, 6, N).astype(float),
    "q.3": with_sysmis(rng.integers(0, 3, N), 7),
})
df.loc[df.index[::13], "bday"] = None
df.loc[df.index[::17], "stamp"] = None

labels = {
    "id": "Respondent identifier",
    "q1": "Satisfaction with the service",
    "q2": "Trust in institutions (negative codes are missing)",
    "income": "Monthly income (EUR)",
    "hours": "Hours worked last week",
    "score": "Composite score",
    "sex": "Sex of respondent",
    "city": "City of residence",
    "comment": "Open answer",
    "bday": "Date of birth",
    "visit": "Date of the visit",
    "stamp": "Interview start",
    "clock": "Time of the interview",
    "Satisfaction_with_public_services_overall":
        "Overall, how satisfied are you with the public services available in the "
        "municipality where you currently live?",
    "q.3": "Question with a period in its name",
}
value_labels = {
    "q1": {1: "Very poor", 2: "Poor", 3: "Fair", 4: "Good", 5: "Very good",
           97: "Not applicable", 98: "Refused", 99: "Don't know"},
    "q2": {-9: "Refused", -8: "Don't know", -2: "Not asked", 1: "Low", 2: "Medium", 3: "High"},
    "score": {0.5: "half", 1: "one", 1.5: "one and a half"},
    "sex": {"M": "Male", "F": "Female", "X": "Not stated"},
    "Satisfaction_with_public_services_overall": {1: "Not at all", 5: "Completely"},
}
missing = {
    "q1": [97, 98, 99],
    "q2": [{"lo": -9, "hi": -1}, 99],
    "income": [{"lo": float("-inf"), "hi": 0}],
    "hours": [{"lo": 100, "hi": float("inf")}],
    "sex": ["X"],
}
formats = {
    "id": "F4.0", "q1": "F2.0", "q2": "F3.0", "income": "COMMA12.2", "hours": "F5.1",
    "score": "F4.1", "bday": "DATE11", "visit": "ADATE10", "stamp": "DATETIME20",
    "clock": "TIME8",
}
measure = {"id": "nominal", "q1": "ordinal", "q2": "ordinal", "income": "scale",
           "hours": "scale", "sex": "nominal", "city": "nominal"}
widths = {"id": 6, "income": 12, "comment": 40}
common = dict(file_label="parqit SPSS fixture: synthetic survey",
              column_labels=labels, note=["Synthetic survey for the parqit tests.",
                                          "Second document line: São João 🦆."],
              variable_value_labels=value_labels, missing_ranges=missing,
              variable_display_width=widths, variable_measure=measure,
              variable_format=formats)
pyreadstat.write_sav(df, os.path.join(HERE, "survey.sav"), **common)
pyreadstat.write_sav(df, os.path.join(HERE, "survey_bc.sav"), row_compress=True, **common)
pyreadstat.write_sav(df, os.path.join(HERE, "survey.zsav"), compress=True, **common)
print("wrote survey.sav, survey_bc.sav, survey.zsav in", HERE)
