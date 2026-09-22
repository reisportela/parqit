* Footer extrema must not bypass the payload-based integer precision policy.
clear all
set more off
set varabbrev off
set linesize 244
args repo plugin
adopath ++ `"`repo'/src/ado/p"'
global PARQIT_PLUGIN_PATH `"`plugin'"'
parqit set threads 1
parqit set fill_threads 1
parqit set memory_limit 128MB
global V124_fails 0
program define _v124_check
    args ok message
    if (`ok') di as txt "PASS `message'"
    else {
        di as err "FAIL `message'"
        global V124_fails = $V124_fails + 1
    }
end
tempfile stem

python:
from decimal import Decimal
from pathlib import Path
import struct
import duckdb
import pyarrow.parquet as pq
from sfi import Data, Macro

cases = {
    'bigint_pos': ('BIGINT', [2147483648, 9007199254740993], 1, 9007199254740992, '<q', 1),
    'bigint_neg': ('BIGINT', [-9007199254740993, -2147483648], 0, -9007199254740992, '<q', 1),
    'ubigint': ('UBIGINT', [2147483648, 9007199254740993], 1, 9007199254740992, '<Q', 1),
    'decimal': ('DECIMAL(18,2)', [Decimal('2147483648.25'), Decimal('9007199254740993.25')],
                1, Decimal('9007199254740992.00'), '<q', 100),
}
connection = duckdb.connect(config={'threads':'1', 'memory_limit':'128MB'})
for family, (dtype, payload, extreme, replacement, packing, scale) in cases.items():
    honest = Path(Macro.getLocal('stem')+'_'+family+'_honest.parquet')
    forged = Path(Macro.getLocal('stem')+'_'+family+'_forged.parquet')
    values = ','.join("(CAST('"+str(v)+"' AS "+dtype+"))" for v in payload)
    destination = "'"+str(honest).replace("'", "''")+"'"
    connection.execute('COPY (SELECT * FROM (VALUES '+values+') t(v)) TO '+destination+
                       ' (FORMAT PARQUET, COMPRESSION UNCOMPRESSED)')
    original = honest.read_bytes()
    assert original[:4] == b'PAR1' and original[-4:] == b'PAR1'
    footer_size = struct.unpack('<I', original[-8:-4])[0]
    footer_start = len(original)-8-footer_size
    footer = original[footer_start:-8]
    before = struct.pack(packing, int(payload[extreme]*scale))
    after = struct.pack(packing, int(replacement*scale))
    assert footer.count(before) >= 1, (family, 'expected footer statistic absent')
    altered = original[:footer_start]+footer.replace(before,after)+original[-8:]
    forged.write_bytes(altered)
    assert altered[:footer_start] == original[:footer_start]
    for kind, path in [('honest',honest),('forged',forged)]:
        assert pq.read_table(path)['v'].to_pylist() == payload, (family,kind,'payload changed')
        observed = connection.execute(
            'SELECT stats_min_value,stats_max_value,min_is_exact,max_is_exact FROM parquet_metadata(?)',
            [str(path)]).fetchall()
        expected = [str(min(payload)),str(max(payload)),True,True]
        if kind == 'forged': expected[extreme] = str(replacement)
        assert observed == [tuple(expected)], (family,kind,observed,expected)
connection.close()

def _v124_validate():
    payload = cases[Macro.getLocal('family')][1]
    mode = Macro.getLocal('mode')
    expected = [str(v) for v in payload] if mode == 'string' else [float(v) for v in payload]
    try:
        actual = Data.get('v')
        ok = int(Macro.getLocal('readrc')) == 0 and actual == expected
    except Exception as exc:
        actual = None
        ok = False
    if not ok:
        print('ORACLE expected=',expected,'actual=',actual)
    if mode == 'round':
        messages = Path(Macro.getLocal('readlog')).read_text(errors='replace')
        note = 'beyond 2^53 rounded to nearest double' in messages
        if not note: print('ORACLE missing explicit wide-integer rounding note')
        ok = ok and note
    Macro.setLocal('ok',str(int(ok)))
end

foreach family in bigint_pos bigint_neg ubigint decimal {
    foreach kind in honest forged {
        foreach route in eager lazy {
            foreach mode in refuse string round {
                clear
                set obs 1
                gen byte sentinel = 99
                parqit close _all
                tempfile readlog
                log using `"`readlog'"', text name(v124read)
                if ("`route'" == "eager") {
                    capture noisily parqit use using `"`stem'_`family'_`kind'.parquet"', clear int64(`mode')
                    local readrc = _rc
                }
                else {
                    parqit use using `"`stem'_`family'_`kind'.parquet"'
                    capture noisily parqit collect, clear int64(`mode')
                    local readrc = _rc
                }
                log close v124read
                if ("`mode'" == "refuse") {
                    capture assert `readrc' == 198 & _N == 1 & c(k) == 1 & sentinel[1] == 99
                    local ok = _rc == 0
                }
                else {
                    python: _v124_validate()
                }
                _v124_check `ok' "`family' `kind' `route' int64(`mode'), rc=`readrc'"
            }
        }
    }
}
parqit close _all
if ($V124_fails) di as err "VERDICT(V124_FOOTER_PRECISION): FAIL - $V124_fails checks"
else di as txt "VERDICT(V124_FOOTER_PRECISION): PASS"
macro drop V124_fails
