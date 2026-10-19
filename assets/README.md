<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores the fonts, the generated word-book content and the images this
application ships, organised by asset type. Keep each asset in the matching subdirectory
and document its destination, naming, integration method and source/licence. Do not mix
binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size and format when relevant.
- Document the source, license, character range, conversion command and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

### English Dict app

The application ships **six** uncompressed LVGL bitmap subsets — three sizes of the main
face, and a small IPA supplement beside each of them:

| File | Format | Use |
| --- | --- | --- |
| [`fonts/wd_font_16.c`](fonts/wd_font_16.c) | 16 px, 4 bpp, uncompressed LVGL | Status bar, hint bar, list rows, example sentences and the quiz feedback line. |
| [`fonts/wd_font_24.c`](fonts/wd_font_24.c) | 24 px, 4 bpp, uncompressed LVGL | Words that no longer fit one line at 32 px, Chinese glosses, quiz candidates, settings values. |
| [`fonts/wd_font_32.c`](fonts/wd_font_32.c) | 32 px, 4 bpp, uncompressed LVGL | The headword itself, when the whole word fits on one line. |
| [`fonts/wd_ipa_16.c`](fonts/wd_ipa_16.c) | 16 px, 4 bpp, uncompressed LVGL | The 18 IPA code points the main face is missing, at 16 px. |
| [`fonts/wd_ipa_24.c`](fonts/wd_ipa_24.c) | 24 px, 4 bpp, uncompressed LVGL | The same 18 code points at 24 px. |
| [`fonts/wd_ipa_32.c`](fonts/wd_ipa_32.c) | 32 px, 4 bpp, uncompressed LVGL | The same 18 code points at 32 px. |

**Why two faces.** The main face does not contain 18 of the IPA code points the word
books use — `ŋ ɒ ɔ ə ɚ ɛ ɜ ɝ ɪ ʃ ʊ ʌ ʒ ʳ ˈ ˌ ː ˚` — and almost every entry carries `ə`, `ɪ`
or `ɛ`. Without a supplement, every pronunciation line on screen would be a row of blank
boxes. The supplement contains **only** those 18 code points, so the two sets never
overlap; LVGL resolves a missing glyph through `lv_font_t.fallback` at run time, keeping
each glyph in the table it was drawn for.

- **Source (main face):** Noto Sans CJK SC Regular, SIL Open Font License 1.1.
- **Source (IPA supplement):** Noto Sans, SIL Open Font License 1.1.
- **Character range:** 3,768 code points per size for the main face — 3,632 CJK
  ideographs, plus printable ASCII, the IPA letters and the CJK punctuation the interface
  draws. The 32 px size is deliberately cut to **93 code points**: ASCII, IPA and
  punctuation only, **not one ideograph** — which is what keeps the whole font set under
  1.6 MB, and why the interface never sets an ideograph in 32 px.
- **Converter:** `lv_font_conv` 1.5.3 with `--bpp 4 --no-compress --format lvgl`.
- **Regenerate (from the repository root):** `python3 tools/wordbook/gen_font.py`, then
  `python3 tools/wordbook/gen_font.py --check` to confirm the shipped files still match
  the sources. Both masters are located through `WD_MAIN_FONT` / `WD_IPA_FONT`; the
  defaults point at `D:/esp/fontsrc/`.

## Content

Store generated word-book data in `content/`. The device reads exactly one file.

| File | Format | Use |
| --- | --- | --- |
| [`content/wd_content.bin`](content/wd_content.bin) | binary blob, 2,899,123 bytes | The whole word bank: 6 books, 19,513 entries, each carrying a word, its IPA, a Chinese gloss and a bilingual example. |

- **Source:** `tools/wordbook/words.txt` is the single source of truth — one entry per
  line, tab-separated. `tools/wordbook/books.json` lists the six books and their order.
- **Generator:** `python3 tools/wordbook/gen_content.py`, with
  `python3 tools/wordbook/gen_content.py --check` for the gate.
- **Layout:** a 32-byte header, a 20-byte record per book, a `(word_count + 1)` offset
  table, then a NUL-separated string pool in which the five fields of one entry sit next
  to each other, so one offset locates a whole entry. Everything is little-endian and read
  byte by byte, because the blob is linked into Flash and is not guaranteed to be 4-byte
  aligned. The full layout is documented at the top of `tools/wordbook/gen_content.py`.
- **Why a blob and not a C array:** a 2.9 MB array slows the compile and link noticeably
  and the generated source cannot be reviewed. `main/CMakeLists.txt` embeds it with
  `target_add_binary_data`, and `main/wd_content.h` declares the symbols the linker
  generates from the file name. Do not also list the blob in `SRCS`.
- **A second output:** the same generator writes `tests/wd_content_fixture.h`, a set of
  real records with per-field expected values, so a host test catches any change to the
  field order or to the join.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 x 2160, JPEG | Product hero image for the project README. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 x 724, PNG RGBA | Technical infographic of the hardware contract, kept as a reference asset. Generated for this repository family on 2026-09-17. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 x 336, PNG RGBA | Transparent wordmark for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 x 336, PNG RGBA | White wordmark, used by the README when GitHub is in dark mode. |
| [`images/logo.png`](images/logo.png) | 2048 x 1024, PNG RGBA | Original square logo the two wordmarks were extracted from. |

- These five files are shared, unchanged, with the sibling repositories (`ai-passport`,
  `daodejing-daily`, `qiaopi-quiz`). They are documentation only, never flashed, and cost
  no Flash.
- **The six device screens are not shipped as images in this revision.** Their geometry is
  the layout budget in `main/wd_ui.h`, which is asserted at compile time; keep any screen
  capture at 240 x 320 native, or at an integer multiple of it.
- Never commit device QR secrets, credentials or personal data in an image.
