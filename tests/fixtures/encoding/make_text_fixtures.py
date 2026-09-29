"""Delimited text in many encodings for tests/verify_suite/v137 (CSV-ENC-1).

    python3 make_text_fixtures.py <stem>

writes <stem>_<name>.csv for each fixture below and <stem>_truth.json: for
each fixture, the Unicode text of every row — the ground truth the decoded
files must equal. The bytes are produced from that text by Python's encoders;
the decoding under test is parqit's own (Stata's ICU checks its tables, v136).
"""
import json
import os
import sys

HEADER = ["id", "name", "place"]

# name: (encoding to write with, rows of (name, place))
TEXTS = {
    "pl_1250": ("cp1250", [("Zażółć gęślą jaźń", "Łódź"), ("Świętosław", "Kraków")]),
    "ru_1251": ("cp1251", [("Ёлки-палки", "Москва"), ("Щука", "Санкт-Петербург")]),
    "el_1253": ("cp1253", [("Ψυχή", "Αθήνα"), ("Ωμέγα", "Θεσσαλονίκη")]),
    "tr_1254": ("cp1254", [("İğdır ılık", "İstanbul"), ("Şanlı çğ", "Şanlıurfa")]),
    "he_1255": ("cp1255", [("שלום", "ירושלים"), ("תודה", "תל אביב")]),
    "ar_1256": ("cp1256", [("مرحبا", "القاهرة"), ("شكرا", "دمشق")]),
    "lt_1257": ("cp1257", [("Ąžuolas", "Klaipėda"), ("Žemaitė", "Šiauliai")]),
    # Vietnamese in 1258 is written with combining tone marks; the text is
    # that sequence, not the precomposed letters
    "vi_1258": ("cp1258", [("Việt", "Hà Nội"), ("Trần", "Huế")]),
    "th_874": ("cp874", [("สวัสดี", "กรุงเทพมหานคร"), ("ขอบคุณ", "เชียงใหม่")]),
    "uk_koi8u": ("koi8_u", [("Ґанок", "Київ"), ("Їжак", "Львів")]),
    "ru_866": ("cp866", [("Ёжик", "Новосибирск"), ("Щи", "Омск")]),
    # 表 and ソ end in 0x5C (ASCII backslash) in Shift_JIS; a quoted field holds the delimiter
    "ja_sjis": ("cp932", [("ソフト表示", "東京"), ("能力", "大阪, 日本")]),
    "ja_eucjp": ("euc_jp", [("日本語", "東京"), ("ひらがな", "京都")]),
    "zh_gbk": ("gbk", [("女人", "北京"), ("你好", "上海")]),
    "zh_gb18030": ("gb18030", [("\U00020000\U0002A6D6", "€ 北京"), ("中文", "深圳")]),
    "tw_big5": ("big5", [("繁體", "台北"), ("高雄市", "臺南")]),
    "ko_949": ("cp949", [("똠방각하", "서울"), ("안녕", "부산")]),
    "u16le_bom": ("utf-16", [("😀 Ωmega 中文", "Zürich"), ("👩‍👩‍👧 ok", "𠀀 北京")]),
    "u16be_bom": ("utf-16-be+bom", [("Αθήνα 東京", "São Paulo"), ("😀", "𠀀")]),
    "u16le_nobom": ("utf-16-le", [("Lisboa", "Porto"), ("José", "Braga")]),
    "u8_bom": ("utf-8-sig", [("Москва", "東京"), ("😀", "é")]),
}


def csv_text(rows):
    def field(v):
        return '"' + v.replace('"', '""') + '"' if ("," in v or '"' in v) else v
    lines = [",".join(HEADER)]
    for i, (name, place) in enumerate(rows, 1):
        lines.append(",".join([str(i), field(name), field(place)]))
    return "\r\n".join(lines) + "\r\n"


def main(stem):
    truth = {}
    for name, (enc, rows) in TEXTS.items():
        text = csv_text(rows)
        if enc == "utf-16-be+bom":
            data = b"\xfe\xff" + text.encode("utf-16-be")
        else:
            data = text.encode(enc)  # utf-16 writes a (little-endian) byte-order mark
        with open(f"{stem}_{name}.csv", "wb") as f:
            f.write(data)
        truth[name] = [[i, n, p] for i, (n, p) in enumerate(rows, 1)]

    # a GBK file whose only text is valid UTF-8 by accident (女 = C5 AE = Ů)
    with open(f"{stem}_zh_gbk_valid.csv", "wb") as f:
        f.write("id,name,place\n1,女,女\n".encode("gbk"))
    truth["zh_gbk_valid"] = [[1, "女", "女"]]
    truth["zh_gbk_valid_as_utf8"] = [[1, "Ů", "Ů"]]

    # UTF-8 with one invalid byte (read with encoding(utf-8): one U+FFFD)
    with open(f"{stem}_u8_bad.csv", "wb") as f:
        f.write(b"id,name,place\n1,Jos\xc3\xa9,ok\n2,Jos\xe9,ok\n")
    truth["u8_bad"] = [[1, "José", "ok"], [2, "Jos�", "ok"]]

    # a lookup in windows-1251 and one in windows-1252, for the merges
    with open(f"{stem}_lk_1251.csv", "wb") as f:
        f.write("id,place\n1,Москва\n2,Омск\n".encode("cp1251"))
    with open(f"{stem}_lk_1252.csv", "wb") as f:
        f.write("id,place\n1,São João\n2,Açores\n".encode("cp1252"))

    # UTF-32 with its byte-order mark: refused
    with open(f"{stem}_u32.csv", "wb") as f:
        f.write("id,name\n1,x\n".encode("utf-32"))

    # large files: windows-1251 across several 4 MiB chunks, and UTF-16LE with a
    # surrogate pair split exactly by the first chunk boundary
    rows = []
    n = 0
    size = 0
    while size < 9 * 1024 * 1024:
        n += 1
        name = "Щёлково-" + str(n) + "-" + "ж" * (n % 97)
        rows.append((name, "Мытищи"))
        size += len(name) + 12
    text = csv_text(rows)
    with open(f"{stem}_big_1251.csv", "wb") as f:
        f.write(text.encode("cp1251"))
    truth["big_1251"] = {"n": n, "chars": sum(len(a) + len(b) for a, b in rows),
                         "last": [n, rows[-1][0], rows[-1][1]]}

    units_before = (4 * 1024 * 1024) // 2 - 1  # UTF-16 units after the mark, before the pair
    rows = []
    parts = ["id,name,place\r\n"]
    units = len(parts[0])  # ASCII so far: one unit per character
    k = 0
    while units < units_before - 200:
        k += 1
        line = f"{k},row-{k},x\r\n"
        parts.append(line)
        units += len(line)
        rows.append((f"row-{k}", "x"))
    k += 1
    name = "a" * (units_before - units - len(f"{k},"))
    parts.append(f"{k},{name}\U0001F600,x\r\n")
    rows.append((name + "\U0001F600", "x"))
    for j in range(k + 1, k + 50001):
        parts.append(f"{j},\U00020000{j},\U0001F600\r\n")
        rows.append((f"\U00020000{j}", "\U0001F600"))
    data = b"\xff\xfe" + "".join(parts).encode("utf-16-le")
    hi = 2 + 2 * units_before
    assert data[hi:hi + 2] == "\U0001F600".encode("utf-16-le")[:2], "the pair does not straddle the boundary"
    with open(f"{stem}_big_u16.csv", "wb") as f:
        f.write(data)
    truth["big_u16"] = {"n": len(rows), "straddle": [k, name + "\U0001F600", "x"],
                        "chars": sum(len(a) + len(b) for a, b in rows)}

    # --- the audit's cases (docs/audits/AUDITORIA_FABLE_RELEASE_R_ENCODINGS_2026-09-28.md)
    # F1: Hive partitions in the path of a decoded glob (and of a single file)
    for year, city in (("2020", "Москва"), ("2021", "Омск")):
        d = f"{stem}_hive/year={year}"
        os.makedirs(d, exist_ok=True)
        with open(f"{d}/part.csv", "wb") as f:
            f.write(f"id,place\n{year[-1]},{city}\n".encode("cp1251"))
    # F2: UTF-8 with NUL-padded fields (a fixed-width export), and UTF-32 without a mark
    with open(f"{stem}_nul_padded.csv", "wb") as f:
        f.write(b"id,name\n1,abc\x00\x00\x00\n2,de\x00\x00\x00\x00\n3,Z\xc3\xbcrich\n")
    with open(f"{stem}_u32_nobom.csv", "wb") as f:
        f.write("id,name\n1,x\n".encode("utf-32-le"))
    # F4: a glob of one windows-1251 file and one UTF-16LE file with its mark
    os.makedirs(f"{stem}_mixed", exist_ok=True)
    with open(f"{stem}_mixed/a.csv", "wb") as f:
        f.write("id,place\n1,Москва\n".encode("cp1251"))
    with open(f"{stem}_mixed/b.csv", "wb") as f:
        f.write(b"\xff\xfe" + "id,place\n2,東京\n".encode("utf-16-le"))
    # F5: UTF-8 and GBK lines in one file declared GBK: the UTF-8 line is decoded too
    with open(f"{stem}_mixed_gbk.csv", "wb") as f:
        f.write("id,place\n1,".encode() + "北京".encode("gbk") + "\n2,".encode() + "Ů".encode("utf-8") + b"\n")
    truth["mixed_gbk"] = [[1, "北京"], [2, "女"]]
    # F6: classic Mac line ends (CR only)
    with open(f"{stem}_cr_1251.csv", "wb") as f:
        f.write("id,place\r1,Москва\r2,Омск\r".encode("cp1251"))
    # F7: UTF-16LE Chinese text without a mark, NUL bytes too few for the density rule
    rows7 = [("北京" * 60, "上海" * 60), ("深圳" * 60, "广州" * 60)]
    with open(f"{stem}_u16le_cjk_nobom.csv", "wb") as f:
        f.write(("id,a,b\r\n" + "".join(f"{i},{a},{b}\r\n" for i, (a, b) in enumerate(rows7, 1))).encode("utf-16-le"))
    truth["u16le_cjk_nobom"] = [[i, a, b] for i, (a, b) in enumerate(rows7, 1)]

    # --- the re-audit's cases
    # F14: a glob as the disk side of mergein/appendin
    os.makedirs(f"{stem}_lk_glob", exist_ok=True)
    with open(f"{stem}_lk_glob/a.csv", "wb") as f:
        f.write("id,place\n1,Москва\n".encode("cp1251"))
    with open(f"{stem}_lk_glob/b.csv", "wb") as f:
        f.write("id,place\n2,Омск\n".encode("cp1251"))
    # F18: a link followed by .. in a glob: the system resolves it physically
    for d in ("sym/hv2", "sym/hv2u", "elsewhere/x", "elsewhere/hv2", "elsewhere/hv2u"):
        os.makedirs(f"{stem}_symtest/{d}", exist_ok=True)
    with open(f"{stem}_symtest/elsewhere/hv2/a.csv", "wb") as f:
        f.write("id,place\n1,Москва\n".encode("cp1251"))
    with open(f"{stem}_symtest/sym/hv2/a.csv", "wb") as f:
        f.write("id,place\n2,Омск\n".encode("cp1251"))
    with open(f"{stem}_symtest/elsewhere/hv2u/a.csv", "wb") as f:
        f.write("id,place\n1,Москва\n".encode("utf-8"))
    with open(f"{stem}_symtest/sym/hv2u/a.csv", "wb") as f:
        f.write("id,place\n2,Омск\n".encode("utf-8"))
    try:
        os.symlink(os.path.abspath(f"{stem}_symtest/elsewhere/x"), f"{stem}_symtest/sym/lnk")
        truth["symlink"] = 1
    except OSError:
        truth["symlink"] = 0
    # F22: a UTF-8 file read with a single-byte code page
    with open(f"{stem}_u8_plain.csv", "wb") as f:
        f.write("id,name\n1,Zürich\n".encode("utf-8"))
    # F15: random bytes named .csv, with two UTF-16LE line feeds at even offsets
    x = 598
    noise = bytearray()
    while len(noise) < 65536:
        x = (x * 1103515245 + 12345) % 2**32
        c = (x >> 16) & 0xFF
        if c not in (0, 0x0A):
            noise.append(c)
    noise[100:102] = b"\x0a\x00"
    noise[200:202] = b"\x0a\x00"
    with open(f"{stem}_random_bin.csv", "wb") as f:
        f.write(bytes(noise))

    with open(f"{stem}_truth.json", "w", encoding="utf-8") as f:
        json.dump(truth, f, ensure_ascii=False)
    print("FIXTURES_DONE")


if __name__ == "__main__":
    main(sys.argv[1])
