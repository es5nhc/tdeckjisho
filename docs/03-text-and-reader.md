# 03 – Text and the reader

Dictionary entries, books (`.txt`, `.md`, `.html`), web pages, RSS articles and the
About page are all shown by **one** screen, `DocumentScreen`. That works because they
are all converted into one document format first:

```
.md   ─ parseMarkdown ┐
.txt  ─ parsePlain    ├─► rt::Doc ─► layout() ─► lines ─► RichView draws them
.html ─ parseHtml     │   (one       (where to    (byte    (fonts, colours,
feed  ─ parseFeed ────┤    string)    break)       ranges)  furigana, pictures)
entry ─ entryToDoc ───┘
```

The hard part, breaking mixed Japanese and Latin text into lines with several fonts,
is written once.

## The document format: one string with control bytes

`rt::Doc` (`src/text/RichText.hpp`) is a title, a list of links, a list of pictures, and
**one UTF-8 string** in which a few control bytes carry the formatting, much like ANSI
colour codes in a terminal:

```
"\x11Title\n" "Some \x01bold\x01 text and \x03a link\x04.\n"
```

| Byte | Name | Kind | Meaning |
|---|---|---|---|
| `\n` | | | ends a paragraph and switches every inline style off |
| `0x01` | `BOLD` | inline toggle | bold on/off |
| `0x02` | `RUBY` | inline toggle | furigana (see below) |
| `0x03` / `0x04` | `LINK` / `ENDLINK` | inline | link; the n-th `LINK` uses `doc.links[n]` |
| `0x05` | `ACCENT` | inline toggle | highlight colour |
| `0x06` | `DIM` | inline toggle | grey |
| `0x07` | `BASE` | marker | where a furigana base starts |
| `0x11` / `0x12` | `H1` / `H2` | block | headings |
| `0x13` | `BULLET` | block | list item |
| `0x14` | `QUOTE` | block | quotation |
| `0x15` | `RULE` | block | horizontal line, no text |
| `0x16` | `PRE` | block | preformatted, monospaced, spaces kept |
| `0x17` | `SMALL` | block | small print |
| `0x18` | `IMAGE` | block | a picture; the n-th one is `doc.images[n]`, its text is the caption |

A **block** byte can only be the first byte of a paragraph. An **inline** byte switches
a style on or off anywhere.

Why not a tree of paragraph and span objects? A 2 MB book would be tens of thousands of
small allocations, which fragment the ESP32's small internal RAM. One string is one
allocation, and it goes to PSRAM. It's also easy to debug: print it.

The price is that control bytes from the *source* must never reach the document. A book
containing a `0x01` byte would turn bold. So parsers never append to `doc.text`
themselves. They use `rt::Builder`, which turns control bytes and line breaks into
spaces, collapses whitespace, and counts links. It's the same idea as escaping user
input before putting it into SQL.

## The parsers

All of them are plain functions, `(source text) → Doc`, in `src/text/`:

| Function | File | Understands |
|---|---|---|
| `parsePlain` | `Plain.cpp` | one paragraph per line; Aozora Bunko furigana `漢字《かんじ》` and `｜base《ruby》`; `［＃…］` notes shown dimmed |
| `parseMarkdown` | `Markdown.cpp` | `#` headings, lists, `>` quotes, `**bold**`, `*em*`, `` `code` ``, `[links](url)`, `---`, fenced code blocks, pictures, and `{漢字\|かんじ}` furigana (the DenDen Markdown syntax) |
| `parseHtml` | `Html.cpp` | readable text, headings, lists, links, `<pre>`, `<ruby>`, entities; pictures only in local files |
| `parseFeed` | `Feed.cpp` | RSS 2.0 and Atom: the list of articles. An article's summary goes through `parseHtml` |
| `entryToDoc` | `screens/Actions.cpp` | a dictionary entry, built directly with `rt::Builder` |

Which parser runs is decided in `actions::openFile()` (by file extension) and
`actions::openUrl()` (feed? `text/plain`? otherwise HTML).

`parseHtml` takes a `Source` argument, `Local` or `Web`, and the caller has to say which
one it is. Only a `Local` file may show pictures from the SD card. A web page gets
`[alt text]` instead, so a page from the internet can't make the device read files off
the card.

### Input rules

- Everything must be **UTF-8**. A book that isn't is refused with a message. For a web
  page that isn't, the text is treated as Latin-1 (`ensureUtf8()` in `Actions.cpp`).
  Japanese books from Aozora Bunko are Shift_JIS, so convert them once on the PC:
  `iconv -f SHIFT_JIS -t UTF-8 in.txt > out.txt`.
- **Size limits:** a book up to 4 MB (`kMaxBookBytes`), a web page or feed up to
  1.5 MB (`kMaxWebBytes`). Both are in `Actions.cpp`.
- Every parser sees bytes from outside, so they are written defensively: no recursion
  that a deeply nested file could overflow, and no loops that become quadratic on
  repetitive input. `docs/security/` tests exactly that (chapter 07).

## Line breaking

`rt::layout(doc, width, metrics)` (`src/text/Layout.cpp`) splits the document into
lines. A `Line` holds no text, only the byte range `[start, end)` in `doc.text`, the
style that is active at `start`, the block type and the height. Drawing a line means
walking that range again. A 1 MB book becomes about 25,000 lines × 16 bytes = 400 KB,
in one allocation.

Rules of thumb it follows:

- Latin text breaks at spaces. A word wider than the line is cut anyway.
- Japanese can break between almost any two characters. A line just must not *start*
  with closing punctuation (`、。」）` …) or small kana (`っゃ` …), and must not *end*
  with an opening bracket (`「（` …). This is a simple form of the Japanese
  line-breaking rules (禁則処理, *kinsoku shori*): `noBreakBefore()` and
  `noBreakAfter()` in `Layout.cpp`.
- A furigana group is never broken.

`layout()` asks a `Metrics` object how wide each character is. On the device and in the
simulator that's `FontMetrics` (`src/ui/RichView.cpp`), which asks LovyanGFX and caches
the answers in a 1024-slot table. In the unit tests it's a fake that says "8 px". Because
of that interface, the line breaking is tested without any graphics library.

The reading position is stored as a **byte offset** into the text, not a line number,
so it survives a change of font size. `DocumentScreen` saves it in
`/tdeckjisho/config/positions.txt` (one `path=offset` line per book) when you leave a
book, and restores it when you open the book again.

## Fonts

The UI must show Estonian (õ ä ö ü š ž) and Japanese, in a sans-serif font. None of
LovyanGFX's built-in fonts has everything:

- `lgfxJapanGothicP` (sans-serif, proportional): Japanese and õäöü, but no š, ž or „
- `efontJA`: has š and ž, but its Latin letters look like a typewriter

So the code uses **both**. Gothic draws everything, and `efontJA` stands in for single
characters Gothic doesn't have. `draw::resolve()` (`src/ui/Draw.cpp`) picks the font
**per character**:

1. the requested font has it → use it
2. its fallback font has it → use that, for this character only
3. neither → a plain stand-in (`„` becomes `"`)

All text in the app goes through `draw::text()` / `draw::run()`, so this applies
everywhere. Browsers handle text the same way, walking a list of fonts per character.

Bold is "fake bold": Gothic has no bold variant, so the text is drawn twice, 1 px apart.

**A trap in the Japanese font:** it maps byte `0x7E` to an overline (‾) and `0x5C` to a
yen sign (¥), following the old JIS X 0201 standard. A `~` or `\` in a Japanese UI text
therefore looks wrong. `Strings.def` avoids both characters in the Japanese column.

Which font is used where is all in `src/ui/Theme.hpp`: `fontUi()`, `fontSmall()`,
`fontBody(size)`, `fontMono()`, `fontH1()`, `fontRuby()`, `fallback()`. They are
bitmap fonts compiled into flash, and every size costs a few hundred KB. Chapter 06
covers changing them.

## Furigana

Furigana is written as a group of three codes:

```
BASE 漢字 RUBY かんじ RUBY        drawn as      かんじ
                                                  漢字
```

The reading is drawn small (`fontRuby()`, efontJA 10 px), centred above the base. The
group is as wide as the wider of the two and is never broken across lines. A document
with furigana gets a band of extra space above every line. `r` hides the furigana, and
then the band disappears too.

A `RUBY` pair *without* a `BASE` in front is drawn inline, small, after the text. That's
the fallback when a source gives a reading but the base can't be found.

The reader shows only the furigana that the text contains. It never derives readings
itself.

## Pictures

`.md` and local `.html` files can contain pictures (JPEG, PNG, BMP, QOI):

1. **While opening** (`rt::resolveImages`, `src/text/Images.cpp`), each picture's path
   is resolved relative to the document, and only its **header** is read. Every format
   stores the size near the start of the file. For JPEG that means walking the segments
   and *seeking over* the 20–60 KB of EXIF data many photos carry. Layout needs the
   height before anything is drawn, and decoding a JPEG just to learn its size would
   take ~100 ms.
2. **Layout** shrinks a picture (never enlarges it) to the text width and at most
   150 px tall (`kMaxImageH`), so the picture and its caption always fit on one screen.
3. **Drawing** (`src/ui/ImageCache.cpp`) decodes each picture once, at display size,
   into a sprite. Up to 1 MB of decoded pictures are kept, and the least recently used
   one is thrown out first. Decoding costs 50–150 ms on the device, and copying a cached
   picture about 1 ms.

The decoders are LovyanGFX's. They can't do **progressive JPEG**: such a picture shows
its caption and the reason instead. Save it as baseline JPEG, and shrink big photos on
the PC. A 4000 px photo can be decoded, but it takes seconds.

A picture file chosen in the library (`.jpg`, `.jpeg`, `.png`, `.bmp`, `.qoi`) opens the
same way: `actions::openFile` builds a document that holds just that picture, with the
file name as its caption, so the steps above apply unchanged.

## The reader's keys

`DocumentScreen` (`src/screens/DocumentScreen.*`):

| Key | Action |
|---|---|
| ↑ ↓ | one line |
| ← → , `b` / space | one page |
| `g` / `G` | start / end |
| `f` | font size 12 → 16 → 20 (saved as `font`) |
| `r` | furigana on/off (only in texts that have furigana; saved as `ruby`) |
| `l`, Enter, trackball press | list of links |
| `c`, or a tap on a word | cursor: move with the arrows, Enter looks the word up |
| `d` | the dictionary |
| Backspace | back |

The cursor works on **units**: a character, or a whole furigana group (the cursor
stands on the kanji, because the reading isn't what you look up). `src/text/Cursor.cpp`
converts between pixels and characters, using the same `Metrics` as the layout and the
drawing, so all three agree on where each character is. The look-up itself is in
chapter 02.
