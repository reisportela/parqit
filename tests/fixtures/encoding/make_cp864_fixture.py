"""Write v140's synthetic SPSS input; run explicitly with pandas + pyreadstat.

The fixture is UTF-8/ASCII; v140 deliberately overrides its code page to CP864
and compares every percent sign, including labels, with ReadStat's decoding.
"""
from pathlib import Path
import pandas as pd
import pyreadstat

pyreadstat.write_sav(pd.DataFrame({"raw": ["%"]}),
                    str(Path(__file__).with_name("cp864.sav")),
                    file_label="%", column_labels={"raw": "%"})
