<p align="right">
  <a href="resumable-mastery-progress-in-nvs.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Keeping mastery progress resumable in NVS

Recorded while building the [English Dict app](english-dict/README.md). The device has to
remember, per book, which words the reader has already learned — across a power cycle, and
inside a flash partition measured in kilobytes. This entry is the one-bit-per-word choice,
the two ways an off-by-one in it silently corrupts data, and how a reset is made hard to
trigger by accident.

## One bit per word, not a counter

The tempting representation is an integer per book: "451 of 1803 learned". It is smaller and
simpler, and it is wrong for this application, because the quiz needs to know **which**
words are already learned, not how many. To prefer unseen words it has to test membership
per word, and a counter cannot answer that.

So progress is a bitmap, one bit per entry, and the size follows from the book:

```
bitmap_bytes(n) = (n + 7) / 8
```

The largest shipped book is 4,533 entries, which is 567 bytes. All six books together are
still under 2 KB — small enough that the whole structure fits comfortably in one NVS
namespace, keyed per book, with no streaming or paging machinery.

## Two silent corruption paths, one of them caught by inspection

A bitmap makes two off-by-one mistakes possible, and neither shows up as a crash:

1. **Writing past the end of the buffer.** The bitmap for a book is sized from that book's
   entry count, but the buffer it is read into is a fixed-size member. A book larger than
   the buffer — a new word book, or a corrupted header — would overflow it by
   `bitmap_bytes(count) - sizeof(buffer)` bytes. The guard is an explicit check before the
   read: if the count needs more bytes than the buffer holds, log and refuse, rather than
   proceed and scribble over neighbouring state.

2. **Loading the wrong book's bitmap into the right buffer.** This one is subtler and was
   caught in review rather than on the device. The natural order is "set the current book
   index, then load its bitmap", but if the load fails, the index has already moved: the
   previous book's bitmap is now sitting in the buffer under the new book's key, and the
   next save writes it out under the new book. **Both books end up corrupt**, and the
   symptom is progress that jumps between books. The fix is to load first and only advance
   the index on success, so a failed load leaves the state it found.

Both defects share a property worth naming: a bitmap has no self-describing length, so
"how many bits are valid" is knowledge that lives only in the header, and every place that
treats the buffer as a run of bytes has to be told it.

## Reset has to be deliberate

`wd_progress_clear()` wipes one book, and a reader who has spent a week on a book must not
lose it to a stray keypress. The reset lives one row past the end of the book list, and it
takes **two presses of OK**: the first arms it and changes the hint line to name the book
and ask again, the second commits. Moving the selection disarms it. The confirmation names
the book, because "reset this book" on a screen that is showing six of them is ambiguous —
the reader has to be able to see *which* one they are about to clear.

The cost of getting the naming wrong is itself instructive: the buffer that holds the hint
text had to grow from 24 to 48 bytes. The string is "re-press OK to clear" plus a book name,
and six Chinese characters plus the fixed wording is 24 bytes before the terminator — so the
original buffer would have truncated the last character into a mojibake tail on the device,
which is exactly the kind of defect that only appears with the longest book name.

## Why it is worth the bytes

The bitmap is what makes the study loop feel continuous: open the device, pick the book you
were reading, and continue from where you stopped, with the quiz biased toward the words you
have not yet marked. A counter would have been half the size and would have removed the
feature the book exists for.

## Related

- [English Dict app](english-dict/README.md) — the application that owns this state.
- [Turning a word book into a flash blob](word-book-to-flash-blob.md) — the read-only side
  of the same data, and the field caps the bitmap's book sizes come from.
- [Letting measured line height, not font size, decide the layout](measured-line-height-over-font-size.md)
  — the other entry where an off-by-one is a correctness question, not a cosmetic one.
