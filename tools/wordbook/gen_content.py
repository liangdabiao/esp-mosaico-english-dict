#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/wordbook/gen_content.py —— 把事实源投影成设备端的内容 blob。

    python gen_content.py            # 生成
    python gen_content.py --check    # 只校验「现有产物是否与事实源一致」，不写

产物两个：
  * `assets/content/wd_content.bin`  —— 经 main/CMakeLists.txt 的 target_add_binary_data
    编进固件。**不做成巨大的 .c 数组**：2.7 MB 的数组会让编译链接明显变慢，
    生成出来的源文件也没法审阅（这条是继承自家族侨批/拼豆两轮的经验）。
  * `tests/wd_content_fixture.h`     —— 跨语言固定向量：给宿主测试几条真实记录
    的**逐字段期望值**，字段顺序 / 拼接方式任何一处跑偏都会被当场抓住。

blob 布局（全部 little-endian；读取一律**逐字节拼**，因为附件在 Flash 里
不保证 4 字节对齐，RISC-V 上的未对齐访问会直接异常）：

    偏移  0  header 32 字节
              magic[4] "WDB1"   version u32   book_count u16   reserved u16
              word_count u32    books_off u32   words_off u32  pool_off u32
              total_size u32
    偏移 32  书表    book_count x 20 字节
              **name_off u32   short_off u32   key_off u32**   first u32   count u32
    ...      词偏移表 (word_count + 1) x 4 字节
              每个词的第一条字段在池内的偏移；末项 = 池长度
    ...      字符串池  UTF-8，字段之间 NUL 分隔
              **同一个词的 5 个字段连续排列**（word\\0ipa\\0trans\\0sent_en\\0sent_cn\\0），
              所以一个词只需要一个偏移就能定位，偏移表的规模从 5N 降到 N+1

⚠ 书表里三个字符串字段的顺序是 **name / short / key**，不是 key 在前。
  设备侧 main/wd_manifest.c 曾经按 key/name/short 读，于是打开真实 blob 时
  报「书目文字损坏或超长」—— 因为 key 位置读到的是中文名。改这里的顺序，
  必须同时改 main/wd_manifest.h 里的 WD_BOOK_FIELD_* 常量。

为什么书表里必须同时有 key 和中文名（20 字节而不是 12、16）：
  * `key`（ChuZhong_3）是**进度的分组键** —— 掌握位图按它分组，改了等于用户丢进度。
    它也是 tests/wd_content_fixture.h 里 book 一列的值；设备端若读不出 key，
    那条固定向量就没法在设备侧验证，「书顺序错位」这类最隐蔽的错误会溜过去。
  * `name`/`short` 是给屏幕看的中文名。只有中文名读不出 key，只有 key 屏幕上是英文。
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import content  # noqa: E402

REPO = content.REPO
BLOB = os.path.join(REPO, "assets", "content", "wd_content.bin")
FIXTURE = os.path.join(REPO, "tests", "wd_content_fixture.h")

MAGIC = b"WDB1"
HEADER_SIZE = 32
BOOK_ENTRY_SIZE = 20
RESERVED = 0


def build_blob() -> tuple[bytes, dict]:
    records = content.load_records()
    stats = content.validate(records, verbose=False)
    books = content.load_books()
    by_book = content.index_records(records)

    pool = bytearray()
    name_offsets: dict[str, tuple[int, int, int]] = {}
    for book in books:
        key = book["key"]
        name_offsets[key] = (len(pool), 0, 0)
        pool += book["name"].encode("utf-8") + b"\x00"
        name_offsets[key] = (name_offsets[key][0], len(pool), 0)
        pool += book["short"].encode("utf-8") + b"\x00"
        name_offsets[key] = (name_offsets[key][0], name_offsets[key][1], len(pool))
        pool += key.encode("ascii") + b"\x00"

    offsets: list[int] = []
    for book in books:
        for word in by_book[book["key"]]:
            offsets.append(len(pool))
            for field in word.fields():
                pool += field.encode("utf-8") + b"\x00"
    offsets.append(len(pool))

    book_count = len(books)
    word_count = len(offsets) - 1
    books_off = HEADER_SIZE
    words_off = books_off + book_count * BOOK_ENTRY_SIZE
    pool_off = words_off + (word_count + 1) * 4
    total = pool_off + len(pool)

    # 版本号 = 内容指纹。设备端把它读出来打日志，一眼就能看出刷的是哪一版内容。
    digest = hashlib.sha256()
    digest.update(MAGIC)
    digest.update(struct.pack("<H", book_count))
    digest.update(struct.pack("<I", word_count))
    for book in books:
        digest.update(book["key"].encode())
    digest.update(bytes(pool))
    version = int.from_bytes(digest.digest()[:4], "little")

    out = bytearray()
    out += MAGIC
    out += struct.pack("<IHH", version, book_count, RESERVED)
    out += struct.pack("<IIIII", word_count, books_off, words_off, pool_off, total)

    first = 0
    for book in books:
        key = book["key"]
        count = len(by_book[key])
        name_at, short_at, key_at = name_offsets[key]
        out += struct.pack("<IIIII", name_at, short_at, key_at, first, count)
        first += count

    for value in offsets:
        out += struct.pack("<I", value)

    out += pool
    assert len(out) == total, (len(out), total)

    meta = {
        "version": version,
        "book_count": book_count,
        "word_count": word_count,
        "pool_bytes": len(pool),
        "total": total,
        "stats": stats,
        "books": books,
        "records": records,
        "by_book": by_book,
    }
    return bytes(out), meta


def write_fixture(meta: dict) -> None:
    """跨语言固定向量：每条给「期望被解析成什么」。

    字段顺序与拼接方式在编译期完全看不出来，只有这条能防两端悄悄跑偏。
    """
    records = meta["records"]
    by_book = meta["by_book"]

    samples: list[tuple[str, content.Word]] = []
    for book in meta["books"]:
        key = book["key"]
        items = by_book[key]
        samples.append((key, items[0]))                                   # 每本书第一条
        samples.append((key, max(items, key=lambda w: len(w.trans))))      # 释义最长
        samples.append((key, max(items, key=lambda w: len(w.word))))       # 单词最长
    longest_sentence = max(records, key=lambda w: len(w.sent_en))
    samples.append((longest_sentence.book, longest_sentence))

    def esc(text: str) -> str:
        return (text.replace("\\", "\\\\").replace('"', '\\"'))

    lines = [
        "// tests/wd_content_fixture.h —— 由 tools/wordbook/gen_content.py 生成，不要手改。",
        "//",
        "// 跨语言固定向量：这里写的是**服务端（生成器）认为**每条记录应该被解析成什么。",
        "// 设备侧的 wd_content 必须逐字段对上。字段顺序、拼接方式、偏移基准任何一处",
        "// 跑偏，宿主测试当场红 —— 这三样在编译期都看不出来。",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define WD_FIX_BOOK_COUNT {meta['book_count']}",
        f"#define WD_FIX_WORD_COUNT {meta['word_count']}",
        f"#define WD_FIX_VERSION {meta['version']}u",
        "",
        "typedef struct {",
        "    const char *book;",
        "    const char *word;",
        "    const char *ipa;",
        "    const char *trans;",
        "    const char *sent_en;",
        "    const char *sent_cn;",
        "} wd_fixture_case_t;",
        "",
        "static const wd_fixture_case_t kWdFixtureCases[] = {",
    ]
    for key, word in samples:
        lines.append(
            f'    {{ "{esc(key)}", "{esc(word.word)}", "{esc(word.ipa)}",'
            f' "{esc(word.trans)}", "{esc(word.sent_en)}", "{esc(word.sent_cn)}" }},')
    lines += [
        "};",
        "",
        "static const unsigned kWdFixtureCaseCount =",
        "    (unsigned)(sizeof(kWdFixtureCases) / sizeof(kWdFixtureCases[0]));",
        "",
    ]
    with open(FIXTURE, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    blob, meta = build_blob()
    print(f"书 {meta['book_count']} 本 / 词条 {meta['word_count']} 条"
          f" / 池 {meta['pool_bytes'] / 1024 / 1024:.2f} MB"
          f" / blob {meta['total'] / 1024 / 1024:.2f} MB"
          f" / version {meta['version']}")

    if args.check:
        if not os.path.exists(BLOB):
            print("STALE: assets/content/wd_content.bin 不存在", file=sys.stderr)
            return 1
        with open(BLOB, "rb") as handle:
            existing = handle.read()
        if existing != blob:
            print(f"STALE: wd_content.bin 与事实源不一致"
                  f"（现有 {len(existing)} B，应为 {len(blob)} B）—— 重跑 gen_content.py",
                  file=sys.stderr)
            return 1
        print("wd_content.bin 与事实源一致")
    else:
        os.makedirs(os.path.dirname(BLOB), exist_ok=True)
        with open(BLOB, "wb") as handle:
            handle.write(blob)
        print(f"已写出 {BLOB}")

    write_fixture(meta)
    print(f"已写出 {FIXTURE}")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
