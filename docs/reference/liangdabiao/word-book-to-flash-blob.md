<p align="right">
  <a href="word-book-to-flash-blob.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Turning a word book into a flash blob

Recorded while building the [English Dict app](english-dict/README.md). The device ships
19,513 entries across six books — 2,899,123 bytes of content in an 8 MB flash with no
PSRAM — and reads them without ever copying a record into RAM. This entry is the file
format, why it is a blob and not a generated C array, and the cross-language fixture that
keeps the reader and the writer honest.

## One source of truth, two projections

The content lives in `tools/wordbook/words.txt`: one entry per line, tab-separated, the
five fields in a fixed order (word, IPA, Chinese gloss, English example, Chinese example).
`tools/wordbook/books.json` lists the six books and their order. Everything else is
projected from those two files by `gen_content.py`, and the projection is checked rather
than trusted:

- `assets/content/wd_content.bin` — the blob the firmware links in;
- `tests/wd_content_fixture.h` — a handful of real records rendered as per-field expected
  values, so a host test fails the moment the field order or the join changes.

Field caps are enforced at generation time, not only on the device: `CAP_WORD = 24`,
`CAP_IPA = 28`, `CAP_TRANS = 64`, `CAP_SENT_EN = 104`, `CAP_SENT_CN = 39`. The longest
word actually shipped is 18 characters and the longest IPA is 21, so the caps carry real
slack — they are the number the layout is *also* allowed to assume, which is why they are
worth rejecting an entry over.

## The layout

```
offset 0     header, 32 bytes
               magic[4]="WDB1"  version u32  book_count u16  reserved u16
               word_count u32    books_off u32  words_off u32  pool_off u32
               total_size u32
offset 32    book table, book_count x 20 bytes
               name_off u32  short_off u32  key_off u32  first u32  count u32
...          word offset table, (word_count + 1) x u32
               the pool offset of each entry's first field; the last item = pool length
...          string pool, UTF-8, fields NUL-separated
               the five fields of one entry sit next to each other:
               word\0ipa\0trans\0sent_en\0sent_cn\0
```

The design decision that pays off is the last line. Because an entry's five fields are
contiguous, **one offset locates a whole entry**, and the offset table is `N + 1` entries
instead of `5N`. The reader gets "entry `i`, field `k`" from a single table lookup and a
short walk of at most five NUL-terminated strings, with no cross-references to resolve.

Every field is little-endian and every field is read **byte by byte**, never through a
cast to a `uint32_t *`. The blob is linked into flash and is not guaranteed to be 4-byte
aligned; RISC-V raises an exception on an unaligned load, and the failure would look like
a random crash on one entry rather than a format bug.

## Why a blob and not a C array

The obvious alternative is `const char content[] = {0x57, 0x44, ...}` generated into a `.c`
file, and it was tried on this board before. Both costs are real:

- A 2.9 MB array is compiled, not included, and it measurably slows the build; it also
  blows up every `compile_commands.json` entry that touches it.
- The generated source cannot be reviewed. A capacity change shows up as a two-million-line
  diff, so nobody checks it.

Instead `main/CMakeLists.txt` embeds the blob with `target_add_binary_data`, and
`main/wd_content.h` declares the `_binary_..._start` / `_end` symbols the linker generates
from the file name. One rule that is easy to get wrong in the other direction: the blob
must **not** also appear in `SRCS`, or it gets linked twice.

## What the reader is allowed to trust

Because the generator enforces the caps and the fixture pins the field order, the reader
can stay small: it knows the entry is five NUL-terminated strings in a fixed order, and it
never has to re-validate the format at run time. The one structural check it *does* keep is
the header — `magic`, `version` and `total_size` — so a stale or truncated blob fails at
`wd_content_init()` with a reason string the boot page can show, instead of being walked
as if it were valid.

That split is the point: the invariants are proven at generation time, where a failure is
a red gate on a laptop, and the device only has to detect a file that is not the one it was
built for.

## Related

- [English Dict app](english-dict/README.md) — the application that consumes the blob.
- [Chaining two fonts so no pronunciation renders as a box](ipa-fallback-font-chain.md) —
  the same "derive the inventory, do not maintain it" rule applied to fonts.
- [Keeping mastery progress resumable in NVS](resumable-mastery-progress-in-nvs.md) — the
  write side: the little state the device does store about each book.
