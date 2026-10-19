<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# English Dict — full application record

The [English Dict app](../../../../README.md) is an offline vocabulary trainer for the
FoloToy AI Passport: **six word books, 19,513 entries**, on a 240 x 320 screen with three
buttons. Nothing is fetched except the pronunciation. This page is the record behind it —
the six screens, the content pipeline, the font and audio decisions, the layout budget and
the verification log.

**Hardware contract.** ESP32-C3, 8 MB flash, no PSRAM, ~400 KB SRAM. 240 x 320 ST7789P3
panel. ES8311 codec. Three keys — up, down and OK — ADC-divided onto a single GPIO, so the
firmware sees bounce and a double-tap as the same kind of event. A separate LVGL recursive
mutex serialises the three tasks that touch the UI.

## The six screens

| # | Screen | What is on it | The three keys |
| --- | --- | --- | --- |
| 1 | Boot / provisioning | A 24 px title ("connecting" or "word bank unavailable"), a reason line, and — when an access point is up — the SSID, the password and `192.168.4.1` | Long press OK starts the access point |
| 2 | Books (home) | Six books plus a Quiz row and a Progress row, exactly the eight the list can show | UP/DOWN to move, OK to enter, long press OK for Settings |
| 3 | Word | Two sides sharing one headword area: **A** the word, its IPA and the Chinese gloss; **B** the English example and its translation | UP/DOWN turns the card over, OK advances to the next word, double-tap OK replays the pronunciation |
| 4 | Quiz | A Chinese gloss as the prompt and four English words below it, then a two-line feedback band | UP/DOWN between the four words, OK to answer, then OK for the next question |
| 5 | Progress | One row per book with a mastery bar, then "Reset this book" and "Settings" | UP/DOWN to move, OK to act; reset takes two presses of OK |
| 6 | Settings | Volume, brightness, screen timeout, Wi-Fi and About, each on its own page | UP/DOWN to adjust or move, OK to confirm, long press to go back |

Every screen writes in its bottom bar what the three keys do right now. The full key
reference lives on the About page, because the hint bar fits two instructions and no more.

## Content

- **19,513 entries in six books**, ordered easiest first: junior high 1,801; senior high
  3,668; CET-4 3,739; CET-6 2,345; postgraduate 4,533; IELTS 3,427.
- Each entry carries five fields: the word, its IPA, a Chinese gloss, an English example
  and that example's translation. The IPA is present for nearly every entry; a small number
  of headwords have no example, and both are treated as normal rather than as errors — the
  line is simply hidden.
- The whole bank is a **2,899,123-byte flash blob**, linked in from
  `assets/content/wd_content.bin` and read in place. See
  [Turning a word book into a flash blob](../word-book-to-flash-blob.md) for the format and
  the caps.
- The quiz always asks in one direction: a Chinese gloss is the prompt and four English
  words are the candidates. Distractors are drawn near the answer's word length, and any
  entry whose gloss equals the answer's is excluded — both rules only make sense with the
  prompt as the gloss and the candidates as words.

## Fonts

Three sizes (16, 24 and 32 px) of one main face, plus a **18-code-point IPA supplement**
chained to each size at run time, because the main face does not contain `ə`, `ɪ`, `ɛ` and
fifteen other symbols that most pronunciations need. The whole font set is **1,428,705
bytes** of bitmap. The 32 px size is deliberately cut to 93 code points with no ideograph
at all, so a Chinese string is never set at 32 px. The full reasoning is in
[Chaining two fonts so no pronunciation renders as a box](../ipa-fallback-font-chain.md).

## Pronunciation

Online, one clip at a time, from Youdao's public endpoint: a 1–2 second, ~10 KB MP3,
decoded in software and written to I2S. Storing the audio locally would take about 94 MB at
8 kHz ADPCM — twelve times the whole content partition — so online is the only option. The
player keeps four mechanisms from the sibling radio application (the decode chain, the
completion check, an output watchdog and "a failed write must log") and adds three the
key-driven device needs (request debounce, cancellation, ID3v2 stripping). See
[One clip at a time](../single-clip-online-pronunciation.md).

## Progress

One bit per entry, per book, in NVS — 567 bytes for the largest book. A bitmap rather than
a counter, because the quiz has to know which words are learned, not how many. Reset takes
two presses of OK and names the book. See
[Keeping mastery progress resumable in NVS](../resumable-mastery-progress-in-nvs.md).

## Layout budget

Everything is sized from two sets of numbers: a budget line height per tier (16 px -> 21,
24 px -> 31, 32 px -> 42) and the measured `line_height` read from the generated fonts
(20 / 29 / 36). The budget is what the constants are written against; the measured value is
smaller in every tier, which is why sizing from round numbers is safe. Each screen asserts
its own vertical budget at compile time. See
[Letting measured line height, not font size, decide the layout](../measured-line-height-over-font-size.md).

## Flash budget

| Part | Bytes | Note |
| --- | --- | --- |
| Application image | 6,121,344 | `0x5d6780` |
| — of which code | 1,246,212 | `.flash.text` |
| — of which fonts | 1,428,705 | the six bitmap subsets |
| — of which content | 2,899,123 | the word-bank blob |
| `factory` partition | 8,323,072 | 8128 K, **26% free** after linking |

Command-line checks stay under two minutes; the full cold build takes about ten.

## Build and verify

```sh
./tools/validate.sh --static      # repository checks and host tests
./tools/validate.sh --firmware    # cold build, merged image, verification, archive
```

The firmware gate produces `build/FoloToy-AI-Passport-full.bin`, a merged image flashed
from offset `0x0`.

**Verification log (2026-10-02).**

| Check | Result |
| --- | --- |
| Host tests | 6 of 6 suites pass |
| `gen_content.py --check` / `gen_font.py --check` | Consistent with the sources |
| Application image | 6,121,344 bytes, first byte `0xE9`, inside `factory` at `0x10000` |
| Merged image | 6,186,880 bytes |
| `verify_firmware.py` | PASS (app and merged layout both) |
| Merged image SHA-256 | `2c1f29a92514647f03f14f574d90dc18c4a19350a5787dd88dd4598a19dd70b0` |

## How this differs from the web original

The design was ported from a build-free single-page web vocabulary app, and three things
changed on purpose:

- **No on-screen keyboard.** Provisioning is done from a phone browser at `192.168.4.1`; the
  boot screen's job is to make the SSID and the address readable, nothing more.
- **A mastery bitmap instead of a score.** The web version is a free-form drill; the device
  keeps a durable per-book record so a session can resume, which is what the bitmap exists
  to serve.
- **One pronunciation direction only.** The quiz asks gloss-to-word, never word-to-gloss, for
  the distractor rules given above.

## Related

- [Chaining two fonts so no pronunciation renders as a box](../ipa-fallback-font-chain.md)
- [Turning a word book into a flash blob](../word-book-to-flash-blob.md)
- [Letting measured line height, not font size, decide the layout](../measured-line-height-over-font-size.md)
- [Keeping mastery progress resumable in NVS](../resumable-mastery-progress-in-nvs.md)
- [One clip at a time](../single-clip-online-pronunciation.md)
