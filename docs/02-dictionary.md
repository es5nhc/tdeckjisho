# 02 – The dictionary

JMdict is about 60 MB of XML with 215,000 entries. The ESP32 can't parse that for every
keystroke, so the expensive work happens once. A converter turns the XML into a handful
of binary files on the SD card, built so that a search needs about ten small reads.

There are two converters with the same output, byte for byte:

- `tools/jmdict2jisho.py` on the PC (about 12 s, Python standard library only)
- `src/dict/DictBuilder.cpp` and its helpers, on the device: Home → Update dictionary

## The files

```
/tdeckjisho/jisho/
    entries.dat   every entry, one after the other
    kana.idx      reading (hiragana)  -> entry     sorted
    kanji.idx     written form 食べる -> entry     sorted (optional)
    eng.idx       English word        -> entry     sorted
    info.txt      build date, counts, licence; shown on the About screen
```

### entries.dat

Each entry is a 2-byte length (little-endian `u16`) followed by that many bytes of
UTF-8 text. The text is lines separated by `\n`, and fields within a line are separated
by the byte `0x1F` (shown as `|` here):

```
c                                   "c" if the word is common, else empty
食べる|喰べる                        written forms (may be empty)
たべる                               readings
v1,vt|to eat                         one line per sense: tags, then the glosses
v1,vt|to live on (e.g. a salary)|to subsist on
```

An entry's **offset** (where its length bytes start) is its identity. The indexes point
to it, the search results carry it, and `Dictionary::loadEntry(offset)` reads it.

Both converters turn every byte below `0x20` in the data into a space, so a stray tab
or line break in JMdict can never break this format.

### The index files (`*.idx`, version 2)

All integers are little-endian.

| Offset | Type | Field |
|---|---|---|
| 0 | `char[4]` | magic `JIDX` |
| 4 | `u16` | version, 2 |
| 6 | `u16` | record size, 48 |
| 8 | `u16` | key size, 43 |
| 10 | `u16` | block size, 256 records |
| 12 | `u32` | record count |
| 16 | records | each: `key[43]` (UTF-8, zero-padded), `u8 score`, `u32 offset` |
| after the records | sparse index | `key[43]` of record 0, 256, 512, … |

The records are sorted by key **bytes**, then by score (highest first), then by
offset. Keys longer than 43 bytes are cut without splitting a UTF-8 character. The
search cuts the query the same way, or long keys would never match.

Why read the integers byte by byte instead of `memcpy`ing them into a struct? A struct's
layout (padding, alignment, byte order) belongs to the compiler and the CPU, and a file
format must not depend on either. `le16()` and `le32()` in `Dictionary.cpp` spell the
format out. In Python this is `struct.unpack_from("<H", ...)`.

## How a search works

### Finding a key: a two-level index

An SD card charges per **operation** (about 1 ms for every seek and read), not per byte.
Reading 12 KB once is far cheaper than reading 48 bytes twenty times. The index is
built around that:

1. `DictIndex::open()` reads the sparse index (every 256th key) into RAM once. That's
   50–150 KB per file, in PSRAM.
2. `lowerBound()` binary-searches the sparse index **in RAM** to find which block of 256
   records can hold the key.
3. It reads that **one** block (256 × 48 = 12 KB) and searches inside it.

That's one SD read to find any key among 880,000. It's the same idea as a B-tree, or
LevelDB's SSTables. `DictIndex` also keeps the last block in a one-block cache, so
reading the following records costs nothing until the next block.

Binary search only works if both converters and the search agree on the order. Python
sorts `bytes`, and C++ compares with `std::string_view::compare`, which also compares
bytes as unsigned values.

### From keystroke to result list

`src/screens/SearchScreen.cpp`:

1. You type. After 150 ms without a key (`kDebounceMs`) the search runs. Not every
   keystroke starts one.
2. **Romaji to kana.** `romaji::toHiragana("taberu")` gives `たべる`
   (`src/core/Romaji.cpp`). It accepts Hepburn, Kunrei and wāpuro spellings, mixed.
   Long vowels must be typed as they are written in kana: `toukyou` finds 東京, `tokyo`
   doesn't.
3. **Modes** (← →): 自動 searches both as a reading and as English, 和英 only as a
   reading, 英和 only as English.
4. `searchKana()` / `searchEnglish()` collect up to 40 hits (`kMaxResults`). English
   with several words ("ice cream") narrows the result to entries that have all of them.
5. **Ranking** (`Dictionary::rank`): exact matches first, then by score, then shorter
   keys.
6. **Lazy loading.** A hit is only an offset at first. `describe()` reads the headword,
   reading and first gloss of just the visible rows, and more when you scroll. It reads
   them in **file order**, because on FAT a forward seek follows the cluster chain,
   while a backward one may start again from the beginning of the file.

### Scores

The converter gives every index record a score of 0–255, so the most likely entry for a
key comes first.

- **Kana and kanji keys:** the word's priority (0–100, from JMdict's `news1`, `ichi1`,
  `spec1`, `gai1` and `nfXX` tags) + 50 for the entry's *first* reading or spelling.
- **English keys:** for each word of each gloss:

  | | |
  |---|---|
  | the gloss *is* the word ("to eat" for `eat`) | +100 |
  | the gloss starts with the word | +30 |
  | in the 1st / 2nd / 3rd sense | +60 / +30 / +15 |
  | the first gloss of its sense | +10 |
  | the word's priority | +0 … 40 |

  Sense position deliberately outweighs frequency: 取る is more common than 食べる, but
  "to eat" is only a minor meaning of 取る.

Very common English words ("the", "to", "of", "sth" …) aren't indexed at all. The list
exists twice: `STOP_WORDS` in the converter and `isStopWord()` in `Dictionary.cpp`.

## Looking up a word in a text

In the reader, `c` (or a tap) puts a cursor in the text, and Enter looks up the word
under it (`Dictionary::lookupAt`). Japanese has no spaces, so nobody knows where the
word ends. The look-up tries the **longest match** first:

1. Take up to 12 characters after the cursor (`kMaxLookupChars`), stopping at
   punctuation, digits or Latin letters.
2. For 12, 11, 10 … 1 characters, look the text up as it is **and** in every dictionary
   form that de-inflection suggests.
3. The longest length that finds something comes first. Shorter matches follow it, up
   to the popup's 4 rows (日本語 first, then 日本).

**De-inflection** (`src/core/Deinflect.cpp`) is conjugation backwards: a table of
(ending → dictionary ending) pairs. `食べました` minus `ました` plus `る` gives `食べる`.
The table over-generates on purpose. A candidate that isn't a real word is simply not
found in the dictionary, which is the real test.

Words written with kanji are found through `kanji.idx`. A dictionary from an older
converter doesn't have that file. Everything else still works, but kanji words can't be
looked up.

Latin text works differently: the word under the cursor is searched in English.

### Katakana and mixed spellings

News writes many words in katakana that the dictionary lists with kanji, above all the
names of animals and plants: クマ for 熊, シカ for 鹿. A katakana word is found through
`kana.idx` like any reading. Four rules make the right entry come first:

- **The katakana spelling decides between homophones.** For words usually written in
  katakana, JMdict lists that spelling as a reading of its own: 鹿 has しか *and* シカ,
  while 歯科 (dentistry) has only しか. When the text is katakana, entries that list exactly
  that spelling come first (`exactForm`), even though 歯科 is the more common word.
- **Half kanji, half katakana.** JMdict has 子熊 but not 子グマ. For a form made of one kanji
  run and one katakana run (`mixedForm`), the look-up reads the kanji part on its own
  (子 → こ), adds the katakana as hiragana (こ + ぐま = こぐま), and looks that reading up. It
  keeps only entries that also have a kanji form starting with 子, and shows that form
  (子熊). This also finds ガン細胞 (癌細胞) with the katakana first.
- **Rendaku.** In a compound, the second word's first sound is often voiced: クマ becomes
  グマ in 子グマ, シカ becomes ジカ in 雄ジカ. If nothing else is found (親グマ, which JMdict
  doesn't have, or グマ when the cursor sits on its グ), the katakana is tried un-voiced
  (`katakanaNoun`, `unvoiceFirst`) and the noun is offered (熊). Only entries that list the
  katakana spelling count, and only the best one is offered, because this is a guess.
- **The character before the cursor.** The reader also passes the text just before the
  cursor (`rt::plainTextBefore`). After a kanji, a voiced katakana word is tried un-voiced
  even when it is a word itself: 雄ジカ gives 鹿 before 時価. The spelling as written stays
  first when JMdict lists it: 窓ガラス is glass (硝子), not a crow (烏).

### Counters

After a number (0–9, full-width digits, 〇一二…十百千万億兆, and 何 / 数 / 幾 as in 何匹), the
word under the cursor is most likely a counter. Among matches of the same length, entries
with a counter sense (part of speech `ctr`) come first, then suffixes (`suf`, `n-suf`),
then the rest, and the row shows the counter sense's gloss. So 3人 shows にん "counter
for people" instead of ひと "person", and ３本 shows "counter for long, thin objects"
instead of "book".

A longer match still comes first, with one exception: a match that ends in hiragana and
is neither a counter nor a suffix moves below the shorter ones. ３本の is 本 + の, not
本の ("mere"). Kanji compounds keep their place, because JMdict tags 時間 only as a noun:
3時間 is 時間, and 2年生 is 年生. The cost is 3人目, which shows 人目 ("public gaze") first
and the counter 人 second.

## Building the dictionary on the device

Home → Update dictionary (`u`) downloads `JMdict_e.gz` (or uses one already in
`/tdeckjisho/jmdict/`) and builds the same files as the Python converter, on a machine
with 8 MB of RAM, while the UI keeps working. While no dictionary is installed, the same
menu entry reads **Download dictionary**.

```
SD card ─► FileSource ─► GzipSource ─► XmlPull ─► JmdictReader ─► encodeEntry
           raw bytes      inflated      tags/text   one entry       record + keys
```

| File | Job |
|---|---|
| `dict/BuildIo.*` | Reading in pieces (`ByteSource`), gzip decoding (`GzipSource`, using the miniz copy inside LovyanGFX), buffered writing |
| `dict/JmdictXml.*` | A small hand-written XML "pull" parser that knows only what JMdict uses |
| `dict/JmdictEncode.*` | One entry → its `entries.dat` record and index keys: a line-by-line port of the Python converter |
| `dict/ExternalSort.*` | Sorting more records than fit in RAM |
| `dict/DictBuilder.*` | The phases, progress, timing, crash safety |
| `screens/UpdateDictScreen.*` | Download, source choice, the progress bar |

### Phases

```
Parse       JMdict(.gz) -> entries.dat + kana/kanji/eng runs   one pass over the XML
MergeKana   kana runs  -> kana.idx
MergeKanji  kanji runs -> kanji.idx
MergeEng    eng runs   -> eng.idx
Finish      info.txt, then swap the new folder in
```

### External merge sort

`eng.idx` has about 880,000 records × 48 bytes = 42 MB, and the device has 8 MB. Python
just calls `records.sort()`. The device uses the classic method from the days of tape
drives:

1. **Runs.** Fill a buffer (4 MB for all three indexes together), sort it, write it out.
   Repeat. Each run is sorted, but the whole isn't.
2. **Merge.** Open all runs and keep taking the smallest front record. A min-heap finds
   it in log₂(k) steps for k runs.

All runs of one index go into **one** file, read through one handle. The SD driver
allows only 10 open files at once (`max_files` in `hal/tdeck/Storage.cpp`).

Sorting a full buffer takes about a second, and the UI freezes for that moment. That's
the one place where a second core would help (see "Long jobs" in chapter 01).

### Crash safety

Everything is written into `/tdeckjisho/jisho.new/`. `info.txt` is written **last**, as
the "commit marker". Only then are the folders swapped:

```
jisho -> jisho.old,   jisho.new -> jisho,   delete jisho.old
```

`DictBuilder::recoverSwap()` runs at every boot, before the dictionary is opened. If the
power went out in the middle of a swap, it finishes or undoes it. The old dictionary
works until the new one is complete.

The downloaded file is checked as well. A gzip file ends with the CRC-32 and the length
of its contents, and `GzipSource::finish()` compares them with what was actually
decoded. A cut-off download or a damaged card is caught there.

## Keeping the converters identical

The same rules exist in three places:

| Rule | Python | C++ |
|---|---|---|
| index layout | `tools/jmdict2jisho.py` (top) | `dict/JmdictEncode.hpp`, `dict/Dictionary.cpp` |
| record, keys, scores | `convert()` | `dict/JmdictEncode.cpp` |
| sort order | `write_index()` | `recordLess()` in `dict/ExternalSort.cpp` |
| stop words | `STOP_WORDS` | `isStopWord()` in `dict/Dictionary.cpp` |
| key truncation | `truncate_key()` | `utf8::truncateBytes()` |

Python does some clever things quietly: Unicode `lower()` turns `K` (the Kelvin sign)
into `k`, `strip()` removes Unicode spaces, and the `(...)` regex doesn't nest. The C++
port imitates each of them (`pyLower()`, `pyStrip()`, `removeParens()`). The comment
at the top of `JmdictEncode.hpp` lists them.

After changing any of these, run

```sh
tests/build_compare.sh                # the 52-word sample: seconds
tests/build_compare.sh JMdict_e.gz    # the real thing: a few minutes
```

It builds the dictionary with both converters and compares every file with `cmp`.
**Changing the format** (key size, record layout) also means raising the index version
in all three places. `Dictionary::open()` refuses files whose header doesn't match
exactly.
