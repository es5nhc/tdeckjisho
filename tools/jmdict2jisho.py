#!/usr/bin/env python3
"""
jmdict2jisho.py – convert JMdict XML into the binary files TDeckJisho reads.

    python3 tools/jmdict2jisho.py JMdict_e.gz  sdcard/tdeckjisho/jisho
    python3 tools/jmdict2jisho.py tools/sample_jmdict.xml sdcard/tdeckjisho/jisho

Why convert at all?  The ESP32-S3 has ~300 KB of fast RAM and 8 MB PSRAM.
JMdict_e is ~60 MB of XML. Parsing XML on the device for every search would
take minutes. So we do the expensive work ONCE on the PC and write files that
the device can search with a handful of small reads:

  entries.dat  every entry as a compact text record (u16 length + UTF-8)
  kana.idx     fixed-size records (hiragana reading -> entry offset), sorted;
               followed by a sparse index (every 256th key)
  kanji.idx    the same for every written (kanji) form of a word: the
               look-up of a word you SELECT in a text
  eng.idx      the same for english words
  info.txt     build info and the JMdict licence notice

The exact byte layout is documented in docs/02-dictionary.md and in
src/dict/Dictionary.hpp. If you change anything here, change it there too.

Only the Python standard library is used.
"""

import gzip
import re
import struct
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

# ---- format constants (must match src/dict/Dictionary.cpp) ----------------
INDEX_MAGIC = b"JIDX"
INDEX_VERSION = 2
KEY_SIZE = 43                     # bytes of UTF-8 key, zero padded
RECORD_SIZE = KEY_SIZE + 1 + 4    # key + u8 score + u32 offset = 48
BLOCK_RECORDS = 256               # sparse index: one key per 256 records
SEP = "\x1f"                      # field separator inside an entry record

# Words too common to be useful search keys. Keep in sync with
# isStopWord() in src/dict/Dictionary.cpp.
STOP_WORDS = {"a", "an", "the", "to", "of", "in", "on", "at", "for", "and", "or",
              "be", "is", "by", "with", "as", "sb", "sth", "etc", "one", "s"}

XML_LANG = "{http://www.w3.org/XML/1998/namespace}lang"


def open_source(path: Path):
    """Open .gz or plain XML transparently (binary mode – the parser decodes)."""
    return gzip.open(path, "rb") if path.suffix == ".gz" else open(path, "rb")


def read_entity_map(path: Path) -> dict:
    """
    JMdict writes part-of-speech as XML entities: <pos>&v1;</pos>.
    The XML parser EXPANDS them to their long text ("Ichidan verb"), which is
    too long for a 320 px screen. The DTD at the top of the file declares
        <!ENTITY v1 "Ichidan verb">
    so we read those declarations and build the reverse map
        "Ichidan verb" -> "v1"
    to get the short codes back.
    """
    head = b""
    with open_source(path) as f:
        while b"]>" not in head and len(head) < 2_000_000:
            chunk = f.read(65536)
            if not chunk:
                break
            head += chunk
    text = head.decode("utf-8", errors="replace")
    return {desc: name for name, desc in re.findall(r'<!ENTITY\s+([\w\-]+)\s+"([^"]*)">', text)}


def priority(entry) -> int:
    """
    0..100: how common the word is, from JMdict's <ke_pri>/<re_pri> tags.
      news1/2  – frequency in Mainichi Shimbun articles
      ichi1/2  – "Ichimango goi bunruishuu" word list
      spec1/2  – hand-picked common words
      gai1/2   – common loanwords
      nfXX     – rank bucket of 500 words in the news corpus (01 = top 500)
    """
    tags = {e.text for e in entry.iter() if e.tag in ("ke_pri", "re_pri")}
    p = 0
    p += 25 if "news1" in tags else 5 if "news2" in tags else 0
    p += 25 if "ichi1" in tags else 5 if "ichi2" in tags else 0
    p += 20 if "spec1" in tags else 10 if "spec2" in tags else 0
    p += 20 if "gai1" in tags else 5 if "gai2" in tags else 0
    nfs = [int(t[2:]) for t in tags if t.startswith("nf") and t[2:].isdigit()]
    if nfs:
        p += max(0, (49 - min(nfs)) // 2)
    return min(p, 100)


def is_common(entry) -> bool:
    tags = {e.text for e in entry.iter() if e.tag in ("ke_pri", "re_pri")}
    return bool(tags & {"news1", "ichi1", "spec1", "spec2", "gai1"})


def kata_to_hira(s: str) -> str:
    """Same rule as utf8::katakanaToHiragana() on the device."""
    return "".join(chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else c for c in s)


def truncate_key(s: str) -> bytes:
    """UTF-8 encode and cut to KEY_SIZE bytes without splitting a character."""
    b = s.encode("utf-8")
    if len(b) <= KEY_SIZE:
        return b
    # Same rule as utf8::truncateBytes() in C++: look at the first byte we would
    # cut OFF. While it is a continuation byte (10xxxxxx), the cut is inside a
    # character, so move it left. Both sides MUST agree, or long keys never match.
    cut = KEY_SIZE
    while cut > 0 and (b[cut] & 0xC0) == 0x80:
        cut -= 1
    return b[:cut]


def gloss_core(g: str) -> str:
    """'to eat (food)' -> 'eat' : used to detect exact English matches."""
    g = re.sub(r"\([^)]*\)", "", g.lower()).strip()
    if g.startswith("to "):
        g = g[3:]
    return g.strip()


def clean(s):
    """Bytes below 0x20 (TAB, line break...) become spaces.

    The record format uses '\n' and 0x1F as separators, so they must never come
    from the data: a gloss with a line break in it would otherwise turn into an
    extra "sense" line. The device converter does exactly the same
    (clean() in src/dict/JmdictXml.cpp), so both outputs stay byte-identical.
    """
    return "".join(" " if ord(c) < 0x20 else c for c in s)


def convert(src: Path, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    entity_map = read_entity_map(src)
    print(f"{len(entity_map)} entity definitions found")

    kana_keys = []   # (key_bytes, score, offset)
    kanji_keys = []  # the same, for every <keb> (kanji spelling), as written
    eng_keys = {}    # (word_bytes, offset) -> best score
    n_entries = 0
    t0 = time.time()

    with open(out_dir / "entries.dat", "wb") as dat, open_source(src) as f:
        # iterparse = streaming parser. We react to each </entry> and then
        # clear it, so memory stays flat even for a 60 MB file.
        for event, elem in ET.iterparse(f, events=("end",)):
            if elem.tag != "entry":
                continue

            kanji = [clean(k.findtext("keb") or "") for k in elem.findall("k_ele")]
            readings = [clean(r.findtext("reb") or "") for r in elem.findall("r_ele")]
            pri = priority(elem)

            senses = []
            last_pos = []
            for s in elem.findall("sense"):
                pos = [clean(entity_map.get(p.text, p.text) or "") for p in s.findall("pos")]
                # JMdict rule: a sense without <pos> inherits the previous one.
                pos = pos or last_pos
                last_pos = pos
                misc = [clean(entity_map.get(m.text, m.text) or "") for m in s.findall("misc")]
                field = [clean(entity_map.get(m.text, m.text) or "") for m in s.findall("field")]
                glosses = [clean(g.text) for g in s.findall("gloss")
                           if g.text and g.get(XML_LANG, "eng") == "eng"]
                if glosses:
                    senses.append((",".join(pos + misc + field), glosses))

            if not readings or not senses:
                elem.clear()
                continue

            # ---- the entry record -------------------------------------
            lines = ["c" if is_common(elem) else "",
                     SEP.join(kanji),
                     SEP.join(readings)]
            lines += [SEP.join([tags] + gl) for tags, gl in senses]
            payload = "\n".join(lines).encode("utf-8")
            while len(payload) > 65535:          # u16 length limit: drop senses
                lines.pop()
                payload = "\n".join(lines).encode("utf-8")

            offset = dat.tell()
            dat.write(struct.pack("<H", len(payload)))   # "<H" = little-endian u16
            dat.write(payload)

            # ---- kana index: every reading, katakana folded to hiragana --
            for i, r in enumerate(readings):
                score = min(255, pri + (50 if i == 0 else 0))
                kana_keys.append((truncate_key(kata_to_hira(r)), score, offset))

            # ---- kanji index: every written form, as written (no case/kana folding) --
            # Used by the look-up of a selected word: 食べる -> this entry. The first
            # spelling is the main one, like the first reading above.
            for i, k in enumerate(kanji):
                score = min(255, pri + (50 if i == 0 else 0))
                kanji_keys.append((truncate_key(k), score, offset))

            # ---- english index: every word of every gloss -------------
            # Score = how likely this entry is what someone typing `w` wants:
            #   the gloss IS the word ("to eat" for eat)    +100
            #   the gloss starts with the word              +30
            #   found in the first / second / third sense   +60 / +30 / +15
            #   first gloss of its sense                    +10
            #   word frequency (priority 0..100)            +0..40
            # Sense position outweighs frequency on purpose: 取る (very
            # common) has "to eat" only as a minor meaning; 食べる's MAIN
            # meaning is "to eat". The first version of this formula let
            # frequency win and ranked 取る above 食べる.
            for si, (_, gl) in enumerate(senses):
                for gi, g in enumerate(gl):
                    core = gloss_core(g)
                    for w in set(re.findall(r"[a-z0-9]+", g.lower())):
                        if w in STOP_WORDS or len(w) < 2:
                            continue
                        score = pri * 40 // 100
                        if core == w:
                            score += 100
                        elif core.startswith(w + " "):
                            score += 30
                        score += (60, 30, 15)[si] if si < 3 else 0
                        if gi == 0:
                            score += 10
                        k = (truncate_key(w), offset)
                        eng_keys[k] = max(eng_keys.get(k, 0), min(score, 255))

            n_entries += 1
            if n_entries % 20000 == 0:
                print(f"  {n_entries} entries...")
            elem.clear()   # free the subtree we just processed

    eng_list = [(k, s, o) for (k, o), s in eng_keys.items()]
    write_index(out_dir / "kana.idx", kana_keys)
    write_index(out_dir / "kanji.idx", kanji_keys)
    write_index(out_dir / "eng.idx", eng_list)

    (out_dir / "info.txt").write_text(
        f"source={src.name}\n"
        f"built={time.strftime('%Y-%m-%d')}\n"
        f"entries={n_entries}\n"
        f"kana_keys={len(kana_keys)}\n"
        f"kanji_keys={len(kanji_keys)}\n"
        f"eng_keys={len(eng_list)}\n"
        "licence=JMdict (c) Electronic Dictionary Research and Development Group, "
        "CC BY-SA 4.0, https://www.edrdg.org/edrdg/licence.html\n",
        encoding="utf-8")
    print(f"done: {n_entries} entries, {len(kana_keys)} kana keys, {len(kanji_keys)} kanji keys, "
          f"{len(eng_list)} english keys in {time.time() - t0:.1f}s")


def write_index(path: Path, records) -> None:
    """
    Sort by key BYTES ascending, then score descending. The device compares
    raw bytes too (std::string::compare), so both sides agree on the order –
    which binary search depends on.

    Version 2 adds a SPARSE INDEX after the records: the key of every
    BLOCK_RECORDS-th record. The device loads only this small table into
    RAM, binary-searches it there, and then reads ONE block of records from
    the card. (Databases call this a two-level index; it's the idea behind
    B-trees and LevelDB's SSTables.)
    """
    records.sort(key=lambda r: (r[0], -r[1]))
    with open(path, "wb") as f:
        # Header: magic, version, recordSize, keySize, blockRecords, count
        f.write(struct.pack("<4sHHHHI", INDEX_MAGIC, INDEX_VERSION,
                            RECORD_SIZE, KEY_SIZE, BLOCK_RECORDS, len(records)))
        for key, score, offset in records:
            # ljust pads the key with zero bytes up to KEY_SIZE.
            f.write(key.ljust(KEY_SIZE, b"\0") + struct.pack("<BI", score, offset))
        # The sparse index: first key of each block.
        for i in range(0, len(records), BLOCK_RECORDS):
            f.write(records[i][0].ljust(KEY_SIZE, b"\0"))
    print(f"  wrote {path.name}: {len(records)} records, {path.stat().st_size / 1e6:.1f} MB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    convert(Path(sys.argv[1]), Path(sys.argv[2]))
