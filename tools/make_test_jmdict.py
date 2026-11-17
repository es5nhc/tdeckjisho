#!/usr/bin/env python3
"""
make_test_jmdict.py – generate a FAKE, full-size JMdict-format XML file.

    python3 tools/make_test_jmdict.py  /tmp/fake_jmdict.xml  [entries=210000]

Why: performance work needs realistic SIZE (≈210 000 entries, ≈270 000
readings, ≈450 000 glosses – the real JMdict_e's ballpark), but the content
can be nonsense. Random readings, random kanji, English-ish glosses with a
Zipf-like word distribution (a few words are very common, most are rare –
like real text). Deterministic (fixed random seed), so benchmarks are
repeatable.

Use the REAL JMdict for the real device; use this for measuring.
"""

import random
import sys

SEED = 20261114
KANA = ("あいうえおかきくけこがぎぐげごさしすせそざじずぜぞたちつてとだでど"
        "なにぬねのはひふへほばびぶべぼぱぴぷぺぽまみむめもやゆよらりるれろわん")
YOON = ["きゃ", "きゅ", "きょ", "しゃ", "しゅ", "しょ", "ちゃ", "ちゅ", "ちょ",
        "りゃ", "りゅ", "りょ", "じゃ", "じゅ", "じょ", "にゅ", "ひょ", "みょ"]
LONG = ["う", "い", "っ", "ん"]
COMMON = ("to be one person thing time year way day man go come see make take get give know "
          "say think water fire tree mountain river eat drink read write speak hear walk run "
          "big small long short new old good bad high low left right above below inside outside "
          "house school car train book letter paper word language country city town village "
          "money work study play rest sleep wake open close start end begin finish cold hot "
          "red blue white black green light dark sound voice music picture colour form shape "
          "morning evening night spring summer autumn winter rain snow wind cloud sky sea").split()
POS = ["n", "v1", "v5k", "v5r", "v5s", "adj-i", "adj-na", "adv", "exp", "vs", "n-adv"]
PRI = ["news1", "ichi1", "spec1", "gai1", "news2", "ichi2"]


def make_words(rng):
    # 30 000 pseudo-words: consonant-vowel syllables, like "torami", "kesuvo".
    syll = [c + v for c in "bdfgklmnprstvz" for v in "aeiou"]
    words = set(COMMON)
    while len(words) < 30000:
        words.add("".join(rng.choice(syll) for _ in range(rng.randint(2, 4))))
    words = sorted(words)
    rng.shuffle(words)
    common = [w for w in COMMON]
    return common + [w for w in words if w not in COMMON]


def zipf_pick(rng, words):
    # Lower indexes much more likely: index ≈ N * u^3
    return words[int(len(words) * rng.random() ** 3)]


def reading(rng):
    parts = []
    for _ in range(rng.randint(1, 4)):
        parts.append(rng.choice(YOON) if rng.random() < 0.15 else rng.choice(KANA))
        if rng.random() < 0.25:
            parts.append(rng.choice(LONG))
    return "".join(parts)


def kanji(rng):
    return "".join(chr(rng.randint(0x4E00, 0x9FA5)) for _ in range(rng.randint(1, 3)))


def main():
    out = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 210000
    rng = random.Random(SEED)
    words = make_words(rng)
    with open(out, "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE JMdict [\n')
        f.write('<!ELEMENT JMdict (entry*)>\n')
        for p in POS:
            f.write(f'<!ENTITY {p} "part of speech {p}">\n')
        f.write("]>\n<JMdict>\n")
        for i in range(n):
            f.write(f"<entry><ent_seq>{8000000 + i}</ent_seq>")
            pri = [rng.choice(PRI)] if rng.random() < 0.08 else []
            for _ in range(rng.choice([0, 1, 1, 1, 2])):
                f.write("<k_ele><keb>" + kanji(rng) + "</keb>"
                        + "".join(f"<ke_pri>{p}</ke_pri>" for p in pri) + "</k_ele>")
            for _ in range(rng.choice([1, 1, 1, 2])):
                f.write("<r_ele><reb>" + reading(rng) + "</reb></r_ele>")
            for s in range(rng.choice([1, 1, 2, 3])):
                f.write("<sense>")
                if s == 0:
                    f.write(f"<pos>&{rng.choice(POS)};</pos>")
                for _ in range(rng.randint(1, 3)):
                    g = " ".join(zipf_pick(rng, words) for _ in range(rng.choice([1, 1, 2, 3])))
                    if rng.random() < 0.3:
                        g = "to " + g
                    f.write(f"<gloss>{g}</gloss>")
                f.write("</sense>")
            f.write("</entry>\n")
        f.write("</JMdict>\n")
    print(f"wrote {n} fake entries to {out}")


if __name__ == "__main__":
    main()
