#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/wordbook/verify_blob.py —— 独立读回校验。

**故意不复用 gen_content.py 的任何代码**：它的写和这里的读是两条独立路径，
只有这样才能证明 blob 的字段布局真的对得上 —— 写和读共用一份实现的话，
同一个错误会在两边同时出现，测试永远绿。

校验四件事：
  1. 魔数 / 版本 / 计数自洽，各段偏移不越界、不重叠
  2. 从池里按「一个词一条偏移、5 个字段连续 NUL 分隔」的约定解出来的
     每一条，与事实源 words.txt 逐字段相等
  3. 每本书的 first/count 覆盖全部词条且不重不漏
  4. 字符集与 content.py 推导出的清单一致（字库按它生成，对不上就会出方框）

用法：
    python verify_blob.py            # 校验默认产物
    python verify_blob.py <blob>     # 校验指定文件

退出码 0 = 通过；非 0 = 失败并打印第一处不一致。
"""

from __future__ import annotations

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_BLOB = os.path.join(REPO, "assets", "content", "wd_content.bin")

MAGIC = b"WDB1"
HEADER_SIZE = 32
BOOK_ENTRY_SIZE = 20


def u16(data: bytes, at: int) -> int:
    return struct.unpack_from("<H", data, at)[0]


def u32(data: bytes, at: int) -> int:
    return struct.unpack_from("<I", data, at)[0]


def cstr(data: bytes, at: int, limit: int) -> str:
    end = data.find(b"\x00", at, limit)
    if end < 0:
        raise SystemExit(f"FAIL  偏移 {at} 起没有找到 NUL 结尾（上限 {limit}）")
    return data[at:end].decode("utf-8")


def parse_words_txt() -> list[tuple[str, str, str, str, str, str]]:
    path = os.path.join(HERE, "words.txt")
    out = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("|")
            if len(parts) != 6:
                raise SystemExit(f"FAIL  words.txt 这一行不是 6 个字段：{line[:80]!r}")
            out.append(tuple(parts))
    return out


def main() -> int:
    blob_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BLOB
    with open(blob_path, "rb") as handle:
        data = handle.read()

    if data[:4] != MAGIC:
        raise SystemExit(f"FAIL  魔数不对：{data[:4]!r}")
    version = u32(data, 4)
    book_count = u16(data, 8)
    reserved = u16(data, 10)
    word_count = u32(data, 12)
    books_off = u32(data, 16)
    words_off = u32(data, 20)
    pool_off = u32(data, 24)
    total = u32(data, 28)

    if reserved != 0:
        raise SystemExit("FAIL  保留字段非 0")
    if total != len(data):
        raise SystemExit(f"FAIL  header 里的 total={total}，实际文件 {len(data)}")
    if books_off != HEADER_SIZE:
        raise SystemExit(f"FAIL  书表偏移 {books_off} != {HEADER_SIZE}")
    if words_off != books_off + book_count * BOOK_ENTRY_SIZE:
        raise SystemExit("FAIL  词偏移表的偏移与书表长度对不上")
    if pool_off != words_off + (word_count + 1) * 4:
        raise SystemExit("FAIL  字符串池偏移与词偏移表长度对不上")
    if not (HEADER_SIZE <= books_off < words_off < pool_off <= total):
        raise SystemExit("FAIL  各段偏移顺序不合法")

    books = []
    for i in range(book_count):
        base = books_off + i * BOOK_ENTRY_SIZE
        # 书表里的三个字符串字段是**池内偏移**，不是文件偏移 —— 加池基址。
        # （第一版这里漏了 pool_off，于是把文件头的 "WDB1" 当成了书名；
        #  这正是独立读回校验存在的意义：写和读共用一份实现的话，这类错永远抓不到。）
        books.append({
            "name": cstr(data, pool_off + u32(data, base), total),
            "short": cstr(data, pool_off + u32(data, base + 4), total),
            "key": cstr(data, pool_off + u32(data, base + 8), total),
            "first": u32(data, base + 12),
            "count": u32(data, base + 16),
        })

    offsets = [u32(data, words_off + i * 4) for i in range(word_count + 1)]
    if offsets[-1] != total - pool_off:
        raise SystemExit(f"FAIL  末条偏移 {offsets[-1]} != 池长 {total - pool_off}")
    for i in range(word_count):
        if offsets[i] >= offsets[i + 1]:
            raise SystemExit(f"FAIL  第 {i} 条的偏移没有前进：{offsets[i]} -> {offsets[i+1]}")

    decoded: list[tuple[str, str, str, str, str]] = []
    for i in range(word_count):
        at = pool_off + offsets[i]
        stop = pool_off + offsets[i + 1]
        fields = []
        for _ in range(5):
            value = cstr(data, at, stop)
            fields.append(value)
            at += len(value.encode("utf-8")) + 1
        decoded.append(tuple(fields))

    expect = parse_words_txt()
    if len(expect) != word_count:
        raise SystemExit(f"FAIL  词条数不一致：blob {word_count}，事实源 {len(expect)}")
    for i, (want, got) in enumerate(zip(expect, decoded)):
        want_fields = want[1:]
        if want_fields != got:
            raise SystemExit(f"FAIL  第 {i} 条不一致\n  事实源 {want_fields}\n  blob   {got}")

    covered = 0
    for i, book in enumerate(books):
        expected_first = covered
        if book["first"] != expected_first:
            raise SystemExit(
                f"FAIL  第 {i} 本书（{book['name']}）first={book['first']}，应为 {expected_first}")
        covered += book["count"]
    if covered != word_count:
        raise SystemExit(f"FAIL  书表覆盖 {covered} 条，词条总数 {word_count}")

    sys.path.insert(0, HERE)
    import content as content_module  # noqa: E402

    # 书表的 key / 中文名 / 顺序必须与 books.json 逐项一致。
    # 顺序错了不会有任何编译期或运行期报错 —— 只会让「选书」页的难度阶梯乱掉，
    # 而且进度位图按 key 分组，顺序变化会让已有进度看起来"跳到别的书上"。
    declared = content_module.load_books()
    if len(declared) != book_count:
        raise SystemExit(f"FAIL  books.json 声明 {len(declared)} 本书，blob 里 {book_count} 本")
    for i, (want, got) in enumerate(zip(declared, books)):
        for field in ("key", "name", "short"):
            if want[field] != got[field]:
                raise SystemExit(
                    f"FAIL  第 {i} 本书的 {field} 不一致：books.json={want[field]!r}，blob={got[field]!r}")

    records = content_module.load_records()
    want_chars = set(content_module.charset_full(records))
    got_chars = set()
    for fields in decoded:
        for field in fields:
            got_chars |= set(field)
    got_chars.add(" ")
    if want_chars != got_chars:
        missing = "".join(sorted(want_chars - got_chars))
        extra = "".join(sorted(got_chars - want_chars))
        raise SystemExit(f"FAIL  字符集不一致 缺={missing!r} 多={extra!r}")

    print(f"PASS  {blob_path}")
    print(f"      版本 {version} / {book_count} 本书 / {word_count} 条词条 / {total} 字节")
    print(f"      逐字段对撞全部一致；字符集 {len(want_chars)} 码点一致；书表与 books.json 一致")
    for book in books:
        print(f"      {book['key']:<12} {book['short']:<4} {book['name']:<8} {book['count']:>5} 词"
              f"（起始下标 {book['first']}）")
    longest = max(decoded, key=lambda f: len(f[0]))
    print(f"      最长单词 {len(longest[0])} 字符：{longest[0]}")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
