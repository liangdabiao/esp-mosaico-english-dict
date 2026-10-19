#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/wordbook/content.py —— 词库内容的唯一真相源。

本文件承载三件事，生成器与字库生成器都从这里 import，任何一处都不许另写一份：

  1. 事实源 words.txt 的解析
  2. 校验：分隔符安全性、字段上限、字符集、每本书的可用性
  3. 字号分档的字符集 —— 字库清单从它推导

三条设计要点，每条都有理由，不要随手改：

  * **上限写在这里，不是写在文档里。** 超限内容当场报错退出，不让它进固件。
    文档里的上限没人守，生成器里的上限才是门。

  * **32px 档只渲染单词与音标**，所以那一档的字符集是 ASCII + IPA 共 86 个码点，
    而不是 3910 个 —— 汉字只出现在 16px 与 24px 两档。这一条把大字档从上千字形
    压到几十个，是本项目字库能塞进 Flash 的关键。

  * **音标清洗在 extract.py 做**（首个变体、去括号注解），这里只校验清洗后的结果。
    源里 `subordinate` 的音标是 `səˈbɔːdɪnɪt (for n. &adj.)`——26 个字符，
    不清洗会撑破 16px 的一行。
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SOURCE = os.path.join(HERE, "words.txt")
BOOKS_JSON = os.path.join(HERE, "books.json")

SEP = "|"

# ---------------------------------------------------------------------------
# 字段上限 —— 全部从版式反解，不是拍的
#
#   版式前提：正文区 BODY_W = 210px。
#     16px 档：英文约 0.5em/字符 ≈ 8px ⇒ 26 字符/行；中文 advance = 字号 ⇒ 13 字/行
#     24px 档：中文 advance = 字号 ⇒ 8 字/行
#
#   CAP_WORD    实测最长单词 18（含 ç 的少数词），留余量；32px 档另按 advance 表
#               自适应降档 32→24→16
#   CAP_IPA     实测清洗后最长 21，留余量。26 字符的原始音标在 16px 是 187px，放得下
#   CAP_TRANS   实测 P50=14 P90=33 P95=41 P99=57 max=124；定 64 只截断 97 词（0.5%）
#   CAP_SENT_EN 16px 英文 26 字符/行 × 4 行
#   CAP_SENT_CN 16px 中文 13 字/行 × 3 行
# ---------------------------------------------------------------------------
CAP_WORD = 24
CAP_IPA = 28
CAP_TRANS = 64
CAP_SENT_EN = 104
CAP_SENT_CN = 39

ELLIPSIS = "…"

# 单词允许的字符：ASCII 字母 + 拉丁扩展（é ç 之类）+ 撇号与连字符。
# 实测 19515 个词里没有一个含空格，也没有词组，所以不用管空格。
WORD_RE = re.compile(r"^[A-Za-z\u00c0-\u024f'\u2019\-]+$")

# 不可见字符：源里真实存在（U+200B 零宽空格、U+2009 窄空格各若干处）。
# 留着有三重害处：混进字库清单、让折行算错、让「同一个词」比对失败。
INVISIBLE = "\u200b\u200c\u200d\ufeff\u200e\u200f\u2060"
THIN_SPACES = "\u2009\u200a\u202f\u00a0"


# ---------------------------------------------------------------------------
# 解析
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Word:
    """事实源里的一条词条。字段名与 words.txt 的列一一对应。"""
    book: str
    word: str
    ipa: str
    trans: str
    sent_en: str
    sent_cn: str
    index: int = 0   # 在自己那本书里的下标，进度位图按它寻址

    def fields(self) -> tuple[str, str, str, str, str]:
        return (self.word, self.ipa, self.trans, self.sent_en, self.sent_cn)


def clean_text(text: str) -> str:
    """清掉不可见字符、把窄空格与制表符归一成普通空格、压掉重复空格。"""
    out = []
    for ch in text:
        if ch in INVISIBLE:
            continue
        if ch in THIN_SPACES or ch == "\t":
            ch = " "
        elif ch in "\r\n":
            ch = " "
        out.append(ch)
    return re.sub(r" {2,}", " ", "".join(out)).strip()


def load_books() -> list[dict]:
    with open(BOOKS_JSON, encoding="utf-8") as handle:
        return json.load(handle)["books"]


def book_order() -> list[str]:
    return [b["key"] for b in load_books()]


def split_record(line: str) -> list[str] | None:
    """把一行事实源切成 6 个字段。形状不对返回 None。

    注释行、空白行、字段数不对一律拒绝 —— 绝不「尽力解析」出半条记录。
    """
    if not line or line.startswith("#"):
        return None
    if line.count(SEP) != 5:
        return None
    return line.split(SEP)


def load_records(path: str = SOURCE) -> list[Word]:
    """读事实源并按书分组，同时赋 index。"""
    raw: dict[str, list[tuple[str, str, str, str, str]]] = {}
    with open(path, encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            parts = split_record(line.rstrip("\n"))
            if parts is None:
                continue
            book, word, ipa, trans, sent_en, sent_cn = parts
            if not word or not trans:
                raise SystemExit(f"{path}:{line_no} 单词或释义为空")
            raw.setdefault(book, []).append((word, ipa, trans, sent_en, sent_cn))

    records: list[Word] = []
    for key in book_order():
        for i, (word, ipa, trans, sent_en, sent_cn) in enumerate(raw.get(key, [])):
            records.append(Word(key, word, ipa, trans, sent_en, sent_cn, i))
    return records


def index_records(records: list[Word]) -> dict[str, list[Word]]:
    by_book: dict[str, list[Word]] = {key: [] for key in book_order()}
    for record in records:
        by_book.setdefault(record.book, []).append(record)
    return by_book


# ---------------------------------------------------------------------------
# 校验
# ---------------------------------------------------------------------------

def validate(records: list[Word], verbose: bool = True) -> dict[str, dict]:
    """校验全部上限与结构约束；返回每本书的统计。任何一处不合格直接退出。

    「玩家分不出差别」的内容也要有结构断言 —— 自测页要出 4 个候选，
    若某本书里不同释义的字符串少于 4 种，那个书就没法出题。
    """
    books = load_books()
    declared = {b["key"]: b for b in books}
    by_book = index_records(records)

    problems: list[str] = []
    stats: dict[str, dict] = {}

    for key, items in by_book.items():
        if key not in declared:
            problems.append(f"事实源出现了 books.json 未声明的书：{key}")
            continue
        if not items:
            problems.append(f"{key} 一条词都没有")
            continue

        expect = declared[key]["expect"]
        if len(items) < int(expect * 0.9):
            problems.append(
                f"{key} 只有 {len(items)} 条，远低于预期 {expect} —— 多半下错了 zip")

        distinct_trans = {w.trans for w in items}
        if len(distinct_trans) < 4:
            problems.append(f"{key} 只有 {len(distinct_trans)} 种不同释义，出不了四选一")

        for w in items:
            where = f"{key}/{w.word}"
            if SEP in "".join(w.fields()):
                problems.append(f"{where} 字段里含分隔符 {SEP}")
            if not WORD_RE.match(w.word):
                problems.append(f"{where} 不是合法的单词形状")
            if len(w.word) > CAP_WORD:
                problems.append(f"{where} 单词 {len(w.word)} 字符，超上限 {CAP_WORD}")
            if len(w.ipa) > CAP_IPA:
                problems.append(f"{where} 音标 {len(w.ipa)} 字符，超上限 {CAP_IPA}")
            if len(w.trans) > CAP_TRANS:
                problems.append(f"{where} 释义 {len(w.trans)} 字符，超上限 {CAP_TRANS}")
            if len(w.sent_en) > CAP_SENT_EN:
                problems.append(f"{where} 例句英 {len(w.sent_en)} 字符，超上限 {CAP_SENT_EN}")
            if len(w.sent_cn) > CAP_SENT_CN:
                problems.append(f"{where} 例句中 {len(w.sent_cn)} 字符，超上限 {CAP_SENT_CN}")
            if bool(w.sent_en) != bool(w.sent_cn):
                # 例句只留半句没意义：要么两面都有，要么整条不要
                problems.append(f"{where} 例句只有一侧（英 {len(w.sent_en)} / 中 {len(w.sent_cn)}）")

        stats[key] = {
            "count": len(items),
            "expect": expect,
            "distinct_trans": len(distinct_trans),
            "with_sentence": sum(1 for w in items if w.sent_en),
            "max_word": max(len(w.word) for w in items),
            "max_ipa": max(len(w.ipa) for w in items),
            "max_trans": max(len(w.trans) for w in items),
            "max_sent_en": max((len(w.sent_en) for w in items), default=0),
            "max_sent_cn": max((len(w.sent_cn) for w in items), default=0),
            "no_ipa": sum(1 for w in items if not w.ipa),
        }

    if problems:
        for line in problems[:40]:
            print(f"  FAIL  {line}", file=__import__("sys").stderr)
        if len(problems) > 40:
            print(f"  ... 另有 {len(problems) - 40} 条", file=__import__("sys").stderr)
        raise SystemExit(f"内容校验未通过：{len(problems)} 处问题")

    if verbose:
        total = sum(s["count"] for s in stats.values())
        print(f"校验通过：{len(stats)} 本书 / {total} 条词条")
        for key, s in stats.items():
            print(f"  {key:<12} {s['count']:>5} 词  有例句 {s['with_sentence']:>5}"
                  f"  释义种类 {s['distinct_trans']:>5}"
                  f"  最长 词{s['max_word']} 音{s['max_ipa']} 义{s['max_trans']}"
                  f" 例句{s['max_sent_en']}/{s['max_sent_cn']}")
    return stats


# ---------------------------------------------------------------------------
# 字号分档的字符集（字库清单的唯一来源）
# ---------------------------------------------------------------------------

def charset_full(records: list[Word]) -> str:
    """16px 与 24px 两档：单词 + 音标 + 释义 + 例句（英中）的全部字符。"""
    chars: set[str] = set()
    for w in records:
        chars |= set(w.word) | set(w.ipa) | set(w.trans) | set(w.sent_en) | set(w.sent_cn)
    chars.add(" ")
    chars.discard("\n")
    return "".join(sorted(chars))


def charset_head(records: list[Word]) -> str:
    """32px 档：只渲染单词与音标 —— 汉字一个都不要。"""
    chars: set[str] = set()
    for w in records:
        chars |= set(w.word) | set(w.ipa)
    chars.add(" ")
    return "".join(sorted(chars))


# ---------------------------------------------------------------------------
# 自测用：一本书里的「释义池」
# ---------------------------------------------------------------------------

def has_usable_sentence(w: Word) -> bool:
    return bool(w.sent_en) and bool(w.sent_cn)


if __name__ == "__main__":
    import sys

    sys.stdout.reconfigure(encoding="utf-8")
    if not os.path.exists(SOURCE):
        raise SystemExit(f"事实源不存在：{SOURCE}（先跑 extract.py）")
    _records = load_records()
    _stats = validate(_records)
    print()
    print(f"16/24px 档字符集 {len(charset_full(_records))} 码点")
    print(f"32px  档字符集 {len(charset_head(_records))} 码点")
