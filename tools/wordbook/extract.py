#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/wordbook/extract.py —— 一次性：从 kajweb/dict 抽出事实来源 words.txt。

    python extract.py            # 缺 zip 就下载，然后重建 words.txt
    python extract.py --fetch    # 只下载
    python extract.py --report   # 只打印统计，不写文件

规范化做四件事，每一件都是实测逼出来的：

  1. **音标取首个变体并去掉括号注解。** 源里用 ';' 与 ',' 分隔多读音，
     还有 `səˈbɔːdɪnɪt (for n. &adj.)` 这种带注解的形式（26 字符，会撑破 16px 一行）。
  2. **清掉不可见字符。** 源里真实存在 U+200B 零宽空格与 U+2009 窄空格；
     留着会让字库清单多出永远看不见的码点，也会让折行算错。
  3. **释义保留词性前缀**（`n. 苹果`），用全角分号 `；` 连接多个义项。
  4. **在全部候选例句里挑最优的一条，而不是固定取第一条。**
     实测 96.2% 的词能挑到「英 ≤104 字符 / 中 ≤39 字符」两行都放得下的例句；
     固定取第一条的话这个比例会低不少，等于白扔内容。

为什么下载走 curl 而不是 urllib：本机 Python 的 urllib 会撞沙箱代理返回
502 Bad Gateway；curl 正常。同理 `cdn.jsdelivr.net` 在本机时通时不通，
**一律走 `fastly.jsdelivr.net`**。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import content  # noqa: E402  （同目录模块，必须在 sys.path 调整之后 import）

CACHE = os.path.join(HERE, "cache")

HEADER = """# WDB1 词库事实来源
# 由 tools/wordbook/extract.py 从 kajweb/dict 生成。**不要手改** —— 重跑生成器即可。
# 字段：book|word|ipa|trans|sent_en|sent_cn
# 分隔符 '|' 已在全部内容里核过出现 0 次（content.py 每次运行都会复验；
# 换词书之前必须重新量一遍，不能沿用结论）。
"""


def fetch(books: list[dict], cdn: str, force: bool = False) -> dict[str, str]:
    """下载并解压六本词书，返回 {key: zip 路径}。"""
    os.makedirs(CACHE, exist_ok=True)
    paths: dict[str, str] = {}
    for book in books:
        key, source = book["key"], book["source"]
        dst = os.path.join(CACHE, f"{key}.zip")
        paths[key] = dst
        if force or not os.path.exists(dst) or os.path.getsize(dst) < 10000:
            url = cdn + source
            print(f"  下载 {key} <- {url}")
            result = subprocess.run(
                ["curl", "-sSL", "-m", "180", "-o", dst, url],
                capture_output=True, text=True,
            )
            if result.returncode != 0:
                raise SystemExit(f"下载失败 {key}: {result.stderr.strip()}")
        size = os.path.getsize(dst) if os.path.exists(dst) else 0
        if size < 10000:
            raise SystemExit(
                f"{key} 的 zip 只有 {size} 字节 —— 多半是 404 或 CDN 抽风，"
                f"内容：{open(dst, encoding='utf-8', errors='replace').read()[:200]!r}")
        print(f"  {key:<12} {size:>9} B")
    return paths


def rows_of(zip_path: str) -> list[dict]:
    with zipfile.ZipFile(zip_path) as archive:
        inner = [n for n in archive.namelist() if n.lower().endswith(".json")]
        if not inner:
            raise SystemExit(f"{zip_path} 里没有 .json")
        data = archive.read(inner[0]).decode("utf-8", "replace")
    out = []
    for line in data.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return out


def clean_ipa(raw: str) -> str:
    """首个变体 + 去括号注解 + 统一重音符号。

    源数据里有三处不规范，全部要在这里收干净（都是实测发现的，不是防御性编程）：

      * **多读音用 `;` 或全角 `；` 分隔。** 只切 ASCII 分号会漏掉 19 条，
        例如 `laɪv；lɪv`、`ˈɪnvəlɪd；ɪnˈvælɪd` —— 整串留在音标里又长又读不出来。
      * **带语法注解**，如 `səˈbɔːdɪnɪt (for n. &adj.)`（26 字符，会撑破一行）。
      * **重音符号混用三种写法**：ASCII 撇号 `'`（绝大多数，如 `'kænsl`）、
        IPA 的 `ˈ`、以及排版引号 `‘`（如 `'defect' → ‘dɪfɛkt`）。
        统一成 IPA 的 `ˈ`（U+02C8）—— 音标字段里的 `'` 就是重音，字典里不该出现
        撇号当重音用。这一条同时让「音标里还有哪些非 ASCII 码点」变得可数。
    """
    text = content.clean_text(raw or "")
    if not text:
        return ""
    text = re.split(r"[;；]", text)[0]
    text = re.split(r"[(,，]", text)[0]
    text = text.replace("\u2018", "\u02c8").replace("\u2019", "\u02c8")
    text = text.replace("'", "\u02c8")
    return text.strip().strip("/").strip()


def build_trans(entries: list[dict]) -> str:
    parts = []
    for entry in entries or []:
        pos = content.clean_text(entry.get("pos") or "")
        cn = content.clean_text(entry.get("tranCn") or "")
        if not cn:
            continue
        parts.append(f"{pos} {cn}".strip() if pos else cn)
    return tidy_cjk_punct(content.clean_text("；".join(parts)))


# 源数据在中文标点后带一个空格（`v 取消， 废除； 抵消`）。中文排版里标点两侧
# 不该有空格，留着既白吃 210px 的版宽、又让「取消， 废除」看起来是断裂的。
# 只动标点**旁边**的空格，不动英文内部的空格（例句里 `, ` 必须保留）。
_TIGHTEN = (
    (re.compile(r"([，。；：、？！）》」』】])[ \t]+"), r"\1"),
    (re.compile(r"[ \t]+([，。；：、？！）《》「」『』【])"), r"\1"),
    (re.compile(r"([（《「『【])[ \t]+"), r"\1"),
)


def tidy_cjk_punct(text: str) -> str:
    out = text
    for pattern, repl in _TIGHTEN:
        out = pattern.sub(repl, out)
    return out.strip()


def cap(text: str, limit: int, break_on: str | None = None) -> str:
    """截断到 limit 字符，末尾加省略号。

    中文优先在 `；` 处断，这样「n. 苹果；v. 放弃」不会断成半条义项。
    """
    if len(text) <= limit:
        return text
    if break_on:
        window = text[: limit - 1]
        cut = window.rfind(break_on)
        if cut >= limit // 2:
            return window[:cut] + content.ELLIPSIS
    return text[: limit - 1] + content.ELLIPSIS


def cap_sentence_en(text: str) -> str:
    """英文例句按词边界截断 —— 截在半个单词上比截短更难读。"""
    if len(text) <= content.CAP_SENT_EN:
        return text
    window = text[: content.CAP_SENT_EN - 1]
    cut = window.rfind(" ")
    if cut >= content.CAP_SENT_EN // 2:
        return window[:cut].rstrip(" ,;:") + content.ELLIPSIS
    return window + content.ELLIPSIS


def pick_sentence(entries: list[dict]) -> tuple[str, str]:
    """在全部候选例句里挑最优的一条。

    优先级：
      1. 英中**都能放得下**的，取源顺序里第一条（最典型）
      2. 英文放得下、中文放不下的，截中文
      3. 都放不下，取「相对预算最不超标」的一条，英中各自截断
    一侧为空的一律丢弃 —— 只剩半句的例句没有学习价值。
    """
    candidates: list[tuple[str, str]] = []
    for entry in entries or []:
        en = content.clean_text(entry.get("sContent") or "")
        cn = tidy_cjk_punct(content.clean_text(entry.get("sCn") or ""))
        if not en or not cn:
            continue
        candidates.append((en, cn))
    if not candidates:
        return "", ""

    fits_both = [p for p in candidates
                 if len(p[0]) <= content.CAP_SENT_EN and len(p[1]) <= content.CAP_SENT_CN]
    if fits_both:
        return fits_both[0]

    fits_en = [p for p in candidates if len(p[0]) <= content.CAP_SENT_EN]
    if fits_en:
        en, cn = fits_en[0]
        return en, cap(cn, content.CAP_SENT_CN)

    def pressure(pair: tuple[str, str]) -> float:
        return max(len(pair[0]) / content.CAP_SENT_EN, len(pair[1]) / content.CAP_SENT_CN)

    en, cn = min(candidates, key=pressure)
    return cap_sentence_en(en), cap(cn, content.CAP_SENT_CN)


def build_word(row: dict) -> tuple[str, str, str, str, str] | None:
    word = content.clean_text(row.get("headWord") or "")
    if not word or len(word) > content.CAP_WORD or not content.WORD_RE.match(word):
        return None

    inner = ((row.get("content") or {}).get("word") or {}).get("content") or {}
    ipa = clean_ipa(inner.get("usphone") or inner.get("ukphone") or inner.get("phone") or "")
    if len(ipa) > content.CAP_IPA:
        return None

    trans = build_trans(inner.get("trans"))
    if not trans:
        return None
    trans = cap(trans, content.CAP_TRANS, break_on="；")

    sent_en, sent_cn = pick_sentence((inner.get("sentence") or {}).get("sentences"))
    return word, ipa, trans, sent_en, sent_cn


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fetch", action="store_true", help="只下载，不写 words.txt")
    parser.add_argument("--force", action="store_true", help="强制重新下载")
    parser.add_argument("--report", action="store_true", help="只打印统计，不写文件")
    args = parser.parse_args()

    with open(os.path.join(HERE, "books.json"), encoding="utf-8") as handle:
        config = json.load(handle)
    books = config["books"]

    print("下载词书：")
    paths = fetch(books, config["cdn"], force=args.force)
    if args.fetch:
        return 0

    lines: list[str] = []
    kept_total = 0
    dropped_total = 0
    for book in books:
        key = book["key"]
        seen: set[str] = set()
        kept = 0
        dropped = 0
        dup = 0
        for row in rows_of(paths[key]):
            built = build_word(row)
            if built is None:
                dropped += 1
                continue
            word = built[0]
            if word in seen:
                dup += 1
                continue
            seen.add(word)
            lines.append(content.SEP.join((key,) + built))
            kept += 1
        kept_total += kept
        dropped_total += dropped
        print(f"  {key:<12} 保留 {kept:>5}  丢弃 {dropped:>4}  同书重复 {dup:>4}"
              f"  （预期 {book['expect']}）")

    print(f"合计保留 {kept_total} 条，丢弃 {dropped_total} 条")

    if args.report:
        return 0

    target = os.path.join(HERE, "words.txt")
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(HEADER)
        handle.write("\n".join(lines))
        handle.write("\n")
    print(f"已写出 {target}（{os.path.getsize(target) / 1024 / 1024:.2f} MB）")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
