# 絵本 Pictures in the reader

Markdown pictures are written `![caption](file)`. The path is relative to this
file, so these live in `books/img/`. The caption below each picture is the alt text.

![Sakura in Tartu, spring 2026](img/sakura.jpg)

A picture may also sit in the middle of a sentence: here comes that famous sentence ![This is a pen, in PNG](img/korewapendesu.png) and the sentence continues below it.

## Every format the decoders know

![これはペンです - in BMP)](img/korewapendesu.bmp)

![Same, but (QOI, "Quite OK Image")](img/korewapendesu.qoi)

## And one they don't

The next file is a *progressive* JPEG. TJpgDec, the JPEG decoder inside
LovyanGFX, reads only *baseline* JPEG, so you get a small icon and the caption:

![おっと！ progressive JPEGが読めないよ！](img/sakura-progressive.jpg)

Fix it on the PC: `convert in.jpg -interlace none out.jpg`

A missing file looks the same: ![this file does not exist](img/nothing-here.png)

---

Pictures are shrunk to the screen width and at most 150 px tall, decoded once
and kept in PSRAM while this document is open, so scrolling stays fast.
