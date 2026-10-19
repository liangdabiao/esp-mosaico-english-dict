<p align="right">
  <a href="measured-line-height-over-font-size.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Letting measured line height, not font size, decide the layout

Recorded while building the [English Dict app](english-dict/README.md). Every layout
defect in it traced back to one habit: choosing a block height from the font size, then
discovering that the face draws taller than that. This entry is the two sets of numbers
that replaced the guessing, and the arithmetic that follows from them.

## Line height is not the font size, and the budget is not the measurement

Two different numbers exist for each of the three sizes, and mixing them up is the bug:

| Tier | Budget (`size x 1.3`) | Measured `line_height` |
| --- | --- | --- |
| 16 px | 21 | **20** |
| 24 px | 31 | **29** |
| 32 px | 42 | **36** |

The budget column is what the layout constants are written against; the measured column
is read straight out of the generated `assets/fonts/wd_font_*.c` (`font->line_height`), and
it is what LVGL actually advances by. The measured value is **smaller in every tier**, so
a block sized from the budget is conservative — that is the whole reason it is safe to
write the constants from round numbers rather than from a device measurement.

The important consequence runs the other way. A block must be sized from the **budget**,
never from the observed value, because the assertion that proves it fits has to stay true
without anyone re-reading the font files. `WD_QUIZ_RESULT_H` is `2 x 21 = 42 + 2 = 44`,
even though two real lines only need `2 x 20 = 40`; the two spare pixels are slack, not
waste.

## "Off by one pixel" is off by one whole line

The quiz page once reserved **104** px for the question block, with a comment that read
"5 lines x 21 = 105, so 104 is only one line short" — and that sentence is wrong twice
over. `104 // 21 = 4`: the block held four lines, not five, and the fifth line was simply
clipped. On the panel this shows up as text that stops mid-sentence, which reads like a
content problem rather than a layout one.

The number was corrected to **105**, and the reasoning moved into the constant. The check
that would have caught it is arithmetic, not a device test:

```
lines_that_fit(h) = h // budget_line_height(tier)
```

Any place where a block's height is meant to hold `n` lines has to be `>= n x budget`, and
"almost n x budget" is `n - 1`.

## The same trap hides in a `snprintf` buffer

A near-miss on a buffer has the same shape. The quiz feedback line renders "wrong" plus the
word plus its IPA, and the longest word in the bank is 18 characters while the longest IPA
is 21. Two Chinese characters plus a space is 5 more. That is 45 half-width units against a
line that holds 25, so the line **cannot** fit one row:

- the feedback block grew from 28 px (one row) to **44 px** (two rows), and the 16 px came
  out of the four quiz choice boxes, which went 34 px to 30 px each;
- the two rows are written into the string **explicitly**, with a `\n`, rather than left to
  the label's own wrapping — an explicit break is the only way the layout can be checked
  ahead of time;
- the buffer is sized from the worst caller, `WD_UI_WORD_CAP + WD_UI_IPA_CAP + 8`, not from
  the value in front of you.

Without both halves, the defect is intermittent in the worst way: a short word fits on one
row and a long one silently drops its whole IPA line, which looks like a random bug rather
than a fixed capacity.

## Bind every budget to the layout at compile time

The budgets live in `main/wd_ui.h` as named constants with the arithmetic written next to
them, and `wd_ui.c` asserts the relationships so a later edit fails the build instead of
the device:

```c
_Static_assert(WD_QUIZ_CHOICE_H >= 30, "a choice row this short hides the selection bar");
_Static_assert(WD_ZONE_STATUS_H + WD_QUIZ_QUESTION_H +
                   WD_QUIZ_CHOICE_H * WD_QUIZ_CHOICES + WD_QUIZ_RESULT_H +
                   WD_ZONE_HINT_H <= WD_SCREEN_H,
               "quiz page does not fit (22 + 105 + 4x30 + 44 + 28 = 319)");
```

The list pages carry the same shape of assertion — `WD_ZONE_STATUS_H + 8 x WD_LIST_ROW_H +
WD_ZONE_HINT_H <= WD_SCREEN_H`, which comes to `22 + 8 x 31 + 28 = 298` out of 320, leaving
22 px of genuine slack. A host test cannot cover any of this: the constants pull in
`lvgl.h`. The compile-time assertion is the only check that stays in the loop for free.

## Why the 32 px tier never carries an ideograph

One more consequence of measuring rather than assuming. The 32 px subset holds **93** code
points and **not one ideograph** (see
[the font chain entry](ipa-fallback-font-chain.md)). So a title that is a Chinese string
cannot be set at 32 px at all — it would be a row of boxes. `WD_BOOT_TITLE_H` is therefore
**31** (the 24 px budget), not 42, and the comment says so, because "the big tier must fit"
is exactly the assumption that would otherwise slip back in.

## Related

- [English Dict app](english-dict/README.md) — the pages these constants govern.
- [Chaining two fonts so no pronunciation renders as a box](ipa-fallback-font-chain.md) —
  where the measured metrics come from.
- [Keeping mastery progress resumable in NVS](resumable-mastery-progress-in-nvs.md) — the
  other place where an off-by-one is a silent data question, not a visual one.
