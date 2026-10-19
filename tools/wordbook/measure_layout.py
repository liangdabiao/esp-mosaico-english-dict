#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/wordbook/measure_layout.py —— 量出各档字号的真实排版容量。

它存在的唯一理由：**wd_layout.c 里的 constants 不能拍脑袋**。
`content.py` 定字段上限时用的是「16px 约 26 字符/行」这个粗估，
而字体实测下来 ASCII 平均 advance 是 0.47~0.52 em —— 粗估与实测谁对谁错，
必须量出来才知道，否则「上限 104 字符的例句放得下 4 行」这句话就没有依据。

量三件事：

  1. 每个字段在 16/24/32px 下的**像素宽度分位数**
  2. **单位模型的偏差**：wd_wrap 用「半角单位」折行（ASCII=1、汉字=2），
     真实排版用像素。两者行数一致的边界在哪里 —— 决定了 units_per_line 取多少
  3. **各档能承担哪些字段**：给定正文区高度与行高，每个字段有多少条会排不下

用法：
    python measure_layout.py

字体：主字体 NotoSansCJKsc（汉字 advance 恰好 1.000 em），
      IPA 码点回退到 NotoSans-VF（与设备端的 lv_font fallback 链一致）。
"""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import content  # noqa: E402

MAIN_FONT = os.environ.get("WD_MAIN_FONT", r"D:/esp/fontsrc/NotoSansCJKsc-Regular.otf")
IPA_FONT = os.environ.get("WD_IPA_FONT", r"D:/esp/fontsrc/ipa/NotoSans-VF.ttf")

# 版式前提：屏 240 宽，左右各留 15px 安全边
BODY_W = 210


def load_advances(path: str) -> dict[int, float]:
    """码点 -> advance（em）。取不到的字形直接跳过。"""
    from fontTools.ttLib import TTFont

    font = TTFont(path, lazy=True)
    upem = font["head"].unitsPerEm
    cmap = font.getBestCmap()
    hmtx = font["hmtx"]
    table: dict[int, float] = {}
    for cp, gname in cmap.items():
        try:
            table[cp] = hmtx[gname][0] / upem
        except KeyError:
            pass
    font.close()
    return table


# 与 main/wd_wrap.c 的 wd_wrap_unit_of 必须逐条一致。
def unit_width(cp: int) -> int:
    if cp < 0x1100:
        return 1
    if 0x2E80 <= cp <= 0xA4CF:
        return 2
    if 0xAC00 <= cp <= 0xD7A3:
        return 2
    if 0xF900 <= cp <= 0xFAFF:
        return 2
    if 0xFE30 <= cp <= 0xFE6F:
        return 2
    if 0xFF00 <= cp <= 0xFF60:
        return 2
    if 0xFFE0 <= cp <= 0xFFE6:
        return 2
    if cp >= 0x20000:
        return 2
    return 1


class Measurer:
    def __init__(self) -> None:
        self.main = load_advances(MAIN_FONT)
        self.ipa = load_advances(IPA_FONT)
        self.missing: set[int] = set()

    def advance(self, cp: int) -> float:
        a = self.main.get(cp)
        if a is not None:
            return a
        a = self.ipa.get(cp)
        if a is not None:
            return a
        self.missing.add(cp)
        return 0.5   # 兜底：两套字体都没有（不该发生），按 0.5 em 估

    def px(self, text: str, size: int) -> float:
        return sum(self.advance(ord(ch)) for ch in text) * size

    def units(self, text: str) -> int:
        return sum(unit_width(ord(ch)) for ch in text)


def wrap_by(text: str, capacity: float, per_char) -> int:
    """贪心折行，返回行数。capacity 与 per_char 用同一套单位。"""
    if not text:
        return 0
    lines = 1
    cur = 0.0
    for ch in text:
        w = per_char(ch)
        if cur + w > capacity and cur > 0:
            lines += 1
            cur = 0.0
        cur += w
    return lines


def quantile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, int(q * (len(ordered) - 1) + 0.5))
    return ordered[idx]


def main() -> int:
    records = content.load_records()
    m = Measurer()

    print(f"主字体 {os.path.basename(MAIN_FONT)}  {len(m.main)} 个字形")
    print(f"补充字体 {os.path.basename(IPA_FONT)}  {len(m.ipa)} 个字形")
    print(f"正文宽 {BODY_W}px")
    print()

    fields = [
        ("单词   word", lambda w: w.word),
        ("音标   ipa", lambda w: w.ipa),
        ("释义   trans", lambda w: w.trans),
        ("例句英 en", lambda w: w.sent_en),
        ("例句中 cn", lambda w: w.sent_cn),
    ]

    # ---- 1. 像素宽度分位数 ----
    print("== 1. 各字段像素宽度（只统计非空） ==")
    header = f"{'字段':<14}{'条数':>7}{'最长units':>10}"
    for size in (16, 24, 32):
        header += f"{'P100@%d' % size:>10}{'P95':>8}"
    print(header)
    for label, getter in fields:
        texts = [getter(w) for w in records]
        texts = [t for t in texts if t]
        if not texts:
            continue
        line = f"{label:<14}{len(texts):>7}{max(m.units(t) for t in texts):>10}"
        for size in (16, 24, 32):
            px = [m.px(t, size) for t in texts]
            line += f"{quantile(px, 1.0):>10.1f}{quantile(px, 0.95):>8.1f}"
        print(line)
    print()

    # ---- 2. 单位模型 vs 像素模型 ----
    #
    # 对每段文本，算出「像素刚好放满一行」时对应的 units_per_line：
    #     upl_equiv = BODY_W * units / px
    # 若把 upl 定在它之下，用 units 折行就不会比真实像素乐观（行数只会更多）。
    # 取全体文本的这个量的 P5（保守端），就是该档可用的 units_per_line 上限。
    print("== 2. 单位模型反解（units_per_line 上限，取 P5 保守） ==")
    print(f"{'字号':<8}{'P5':>8}{'P25':>8}{'P50':>8}{'上限(向下取整)':>16}")
    upl_of_size: dict[int, int] = {}
    for size in (16, 24, 32):
        equivs: list[float] = []
        for _, getter in fields:
            for w in records:
                text = getter(w)
                if not text:
                    continue
                u = m.units(text)
                if u < 20:      # 太短的文本折算不稳定，不参与
                    continue
                px = m.px(text, size)
                if px <= 0:
                    continue
                equivs.append(BODY_W * u / px)
        p5, p25, p50 = quantile(equivs, 0.05), quantile(equivs, 0.25), quantile(equivs, 0.50)
        upl = int(p5)
        upl_of_size[size] = upl
        print(f"{size}px{chr(9)}".ljust(8) + f"{p5:>8.1f}{p25:>8.1f}{p50:>8.1f}{upl:>16}")
    print()

    # ---- 3. 单位模型的行数是否真的不乐观 ----
    print("== 3. 行数一致性（units 模型行数 vs 像素模型行数） ==")
    print(f"{'字号':<8}{'upl':>5}{'样本':>8}{'units更少':>11}{'相等':>8}{'units更多':>11}")
    for size in (16, 24, 32):
        upl = upl_of_size[size]
        fewer = equal = more = 0
        for _, getter in fields:
            for w in records:
                text = getter(w)
                if not text:
                    continue
                lu = wrap_by(text, upl, lambda ch: unit_width(ord(ch)))
                lp = wrap_by(text, BODY_W, lambda ch: m.advance(ord(ch)) * size)
                if lu < lp:
                    fewer += 1
                elif lu == lp:
                    equal += 1
                else:
                    more += 1
        total = fewer + equal + more
        print(f"{size}px{chr(9)}".ljust(8) + f"{upl:>5}{total:>8}{fewer:>11}{equal:>8}{more:>11}"
              f"   {'OK 不乐观' if fewer == 0 else '⚠ units 模型偏乐观'}")
    print()

    # ---- 4. 各档能承担哪些字段 ----
    #
    # 每档给一个正文区高度与行高。汉字 advance = 1.000 em，所以行高取字号的 1.3 倍
    # 留出行距（24px -> 32px 行高，16px -> 21px 行高）。
    print("== 4. 字段在预算行数内放得下的比例 ==")
    print("（正文区高度 178px；行高 = 字号 x 1.3）")
    zone_h = 178.0
    for size in (16, 24, 32):
        line_h = size * 1.3
        max_lines = int(zone_h // line_h)
        print(f"-- {size}px  行高 {line_h:.1f}px  最多 {max_lines} 行 --")
        for label, getter in fields:
            texts = [getter(w) for w in records]
            texts = [t for t in texts if t]
            if not texts:
                continue
            upl = upl_of_size[size]
            overflow = sum(
                1 for t in texts
                if wrap_by(t, upl, lambda ch: unit_width(ord(ch))) > max_lines)
            print(f"   {label:<14} 排不下 {overflow:>5} / {len(texts):>5}"
                  f"  ({overflow / len(texts) * 100:.1f}%)")
    print()

    if m.missing:
        chars = "".join(sorted(chr(cp) for cp in m.missing))
        print(f"⚠ 两套字体都缺的码点 {len(m.missing)} 个：{chars!r}", file=sys.stderr)
        return 1
    print("两套字体合起来覆盖了全部实际用到的码点。")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
