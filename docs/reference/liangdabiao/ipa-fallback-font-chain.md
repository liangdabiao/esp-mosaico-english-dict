<p align="right">
  <a href="ipa-fallback-font-chain.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Chaining two fonts so no pronunciation renders as a box

Recorded while building the [English Dict app](english-dict/README.md). The word bank
carries an IPA line for almost every entry, and the font family every application on this
board is built on does not contain all the symbols IPA needs. This entry is what the gap
was, why merging the two faces was the wrong fix, and how the run-time fallback chain is
wired instead.

## The gap: 18 code points, and a large share of the bank needs three of them

The interface draws three text sizes (16, 24 and 32 px), and a headword may be set at any
of them. The main face is Noto Sans CJK SC. Measured against the inventory the application
actually draws, it is missing exactly **18** code points, every one of them IPA:

```
ŋ ɒ ɔ ə ɚ ɛ ɜ ɝ ɪ ʃ ʊ ʌ ʒ ʳ ˈ ˌ ː ˚
```

Those are not exotic. `ə`, `ɪ` and `ɛ` appear in the great majority of the 19,513
entries, and `ˈ` (primary stress) is in nearly all of them, so "occasionally missing" is
not a description that survives contact with the content — a single missing glyph turns
the whole pronunciation line into boxes, which looks worse than showing nothing.

## Merging the two faces is the wrong fix, and the reason is subtle

The obvious approach is to build one font from two sources: subset the IPA letters out of
Noto Sans, subset the ideographs out of Noto Sans CJK, and merge the glyph tables. It does
not work here, and the failure is not one of coverage:

- The two faces give **different advance widths to the same code point**. `i` is a
  proportional glyph in one and a fixed-width one in the other. A merge has to pick one
  number per code point; whichever it picks is wrong for the text that was authored
  against the other.
- The merge also has to choose **one `line_height` and one `base_line`** for the result,
  and those differ between the sources as well. The layout budget was computed from the
  main face's metrics, so the merged face would silently invalidate it.

Cherry-picking per code point is possible in principle and unmaintainable in practice:
every addition re-opens the "which width won" question for all 18 symbols.

## The fix: a supplement that contains only what is missing, chained at run time

`gen_font.py` emits **two** files per size:

| File | Contents |
| --- | --- |
| `wd_font_<n>.c` | The full inventory minus the 18 missing code points, drawn from the main face. |
| `wd_ipa_<n>.c` | Exactly those 18 code points, drawn from Noto Sans. |

The supplement is deliberately tiny — 18 code points at 16, 24 and 32 px come to 8,704,
12,509 and 18,508 bytes — because it is only ever asked for characters the main table
cannot answer. `wd_font.c` links each pair at init:

```c
s_font[tier] = wd_font_16;          /* the main table */
s_font[tier]->fallback = &wd_ipa_16; /* consulted only for a glyph the main table lacks */
```

**LVGL resolves a fallback per glyph, not per string.** `lv_font_get_glyph_dsc` on the
main face returns nothing for `ə`, and only then is the fallback asked. There is no
per-string decision, no "switch to the IPA font" mode, and no need for the caller to know
which of the 18 symbols a pronunciation contains. It also means the fallback is free for
a word whose pronunciation happens to use none of them.

Why this beats merging, concretely: nothing is ever duplicated, so there is no width to
disagree about. Each glyph keeps the metrics of the face it came from, and the main face's
`line_height` — the number the whole layout budget was derived from — is untouched.

## Derive the missing set; never maintain it

The list of 18 is computed at generation time, not typed in:

```
missing = (inventory the app draws) - (code points the main face has)
```

`gen_font.py` asks the source font's `cmap` for its coverage and subtracts, so adding a
word with a symbol the main face happens to lack widens the supplement automatically, and
swapping the main face to one that already has all 18 shrinks the supplement to nothing
without a code change. The inventory itself comes from the same content module the C table
generator uses, so the fonts and the word bank cannot disagree about what has to be drawn.

## What the gate checks

Three checks, all cheap:

1. `gen_font.py --check` recomputes the wanted set from the current sources and compares
   it against the code points actually present in the shipped `.c` files. `want - got` is
   the failure signal, so a new Chinese interface string that introduced a code point the
   subset lacks fails the gate rather than reaching the device as a box.
2. `wd_ui_init()` returns `false` if any tier cannot produce an ASCII glyph **or if the
   fallback chain is not linked**. That is the one UI-level failure allowed to stop the
   app: a screen full of boxes is worse than not starting.
3. Every glyph the application can draw was checked against the source `cmap` before
   conversion, so the converter is never asked to invent a glyph.

## Related

- [English Dict app](english-dict/README.md) — the application this chain is built for.
- [Turning a word book into a flash blob](word-book-to-flash-blob.md) — the content side
  of the same "derive it, do not maintain it" rule.
- [Letting measured line height, not font size, decide the layout](measured-line-height-over-font-size.md)
  — why the main face's `line_height` had to stay authoritative.
