#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/wordbook/gen_font.py —— 生成三档主字体 + 每档一个 IPA 补充字体。

    python gen_font.py            # 生成
    python gen_font.py --check    # 只校验现有字库是否覆盖当前字符集

三档的职责是**刻意分开的**，这是本项目字库能塞进 Flash 的关键：

    wd_font_64   64px  只渲染单词与音标 —— 字符集 = ASCII + IPA + 界面标点，
                       **不含任何汉字**（若照抄「大字档也用全字符集」的写法，
                       这一档就要带上 3700 多个汉字，多花好几 MB）
    wd_font_48   48px  释义、中文例句、书名、设置标题 —— 全字符集
    wd_font_32   32px  音标、状态栏、按钮、提示条 —— 全字符集

补充字体解决本项目的**唯一新技术风险**：主字体 NotoSansCJKsc 在真实字符集里
缺 18 个码点（ŋ ɒ ɔ ə ɚ ɛ ɜ ɝ ɪ ʃ ʊ ʌ ʒ ʳ ˈ ˌ ː ˚），而几乎每个词都含 ə/ɪ/ɛ。
照抄家族现有管线的话，音标会**满屏方框**。补充字体由 Noto Sans（SIL OFL 1.1，
无 Reserved Font Name，可随仓库分发）生成，只含主字体缺的那些码点。

两套字体**不做 fontTools merge**：merge 出来的字体会有两份互相打架的 advance 表，
而且以后升级母字体时没人愿意再 merge 一次。这里用 LVGL 自己的
`lv_font_t.fallback` 回退链，在 wd_font.c 里串起来。

--check 用的是**硬判据**（不是只比字符集清单，也不是只看退出码）：
解析生成出来的 .c 里的 `cmaps[]`，把两条 cmap（连续区间那条 `unicode_list = NULL`、
稀疏那条按**它自己的** range_start 加表内偏移）都算上，得到「设备真能渲染的码点集」，
再与当前推导出的清单求差集。差集非空 = 屏幕上会出现方框。
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import content  # noqa: E402

MAIN_FONT = os.environ.get("WD_MAIN_FONT", r"D:/esp/fontsrc/NotoSansCJKsc-Regular.otf")
IPA_FONT = os.environ.get("WD_IPA_FONT", r"D:/esp/fontsrc/ipa/NotoSans-VF.ttf")
LV_FONT_CONV_DIR = os.environ.get("LV_FONT_CONV_DIR",
                                  "D:/esp/fonttools/node_modules/lv_font_conv")
NODE = os.environ.get("NODE_EXE", r"C:/Program Files/nodejs/node.exe")
DRIVER = os.path.join(HERE, "font_conv_driver.mjs")

FONTS_DIR = os.path.join(content.REPO, "assets", "fonts")
BPP = 4

# 生成字库放在 assets/fonts/，文件名决定 C 里的字体符号名。
TIERS = (
    ("wd_font_64", 64, "head"),
    ("wd_font_48", 48, "full"),
    ("wd_font_32", 32, "full"),
)

# main/ 下参与「界面文案字符集」扫描的文件后缀（跳过生成物本身）
SCAN_SUFFIX = (".c", ".cc", ".h")
SKIP_NAMES = {"wd_font_64.c", "wd_font_48.c", "wd_font_32.c",
              "wd_ipa_64.c", "wd_ipa_48.c", "wd_ipa_32.c"}

_STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
_BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
_LINE_COMMENT_RE = re.compile(r"//[^\n]*")


# ---------------------------------------------------------------------------
# 字符集推导
# ---------------------------------------------------------------------------

def scrape_ui_chars() -> set[str]:
    """扫 main/ 下源码里的字符串字面量，取字符集。

    只扫 main/，不扫生成物；注释先剥掉（写注释时很容易顺手把中文抄进去，
    扫进来是无害的，但会掩盖「界面文案缺字」这类真问题）。
    """
    main_dir = os.path.join(content.REPO, "main")
    chars: set[str] = set()
    if not os.path.isdir(main_dir):
        return chars
    for name in sorted(os.listdir(main_dir)):
        if name in SKIP_NAMES or not name.endswith(SCAN_SUFFIX):
            continue
        path = os.path.join(main_dir, name)
        with io.open(path, encoding="utf-8", errors="replace", newline="") as handle:
            text = handle.read()
        text = _BLOCK_COMMENT_RE.sub(" ", text)
        text = _LINE_COMMENT_RE.sub(" ", text)
        for literal in _STRING_RE.findall(text):
            for ch in literal:
                if ch == "\\" or ch < " ":
                    continue
                chars.add(ch)
    return chars


def expected_charsets() -> dict[str, set[str]]:
    records = content.load_records()
    full = set(content.charset_full(records))
    head = set(content.charset_head(records))
    ui = scrape_ui_chars()

    # 32px 档保持「不含汉字」的纪律：界面文案里落在这档上的一定要是 ASCII。
    # 这里不做静默过滤 —— 若 UI 文案把汉字带进来，说明版式设计违背了分档约定，
    # 应该去改界面，而不是偷偷把它塞进大字档。
    head_chars = head | {c for c in ui if ord(c) < 128}
    full_chars = full | ui

    print(f"  内容字符集  全档 {len(full):>5} 码点 / 大字档 {len(head):>4} 码点")
    print(f"  界面文案字符（扫 main/ 字符串字面量）{len(ui):>5} 码点")
    return {
        "full": full_chars,
        "head": head_chars,
    }


def font_codepoints(path: str) -> set[int]:
    from fontTools.ttLib import TTFont
    font = TTFont(path, fontNumber=0)
    return set((font.getBestCmap() or {}).keys())


# ---------------------------------------------------------------------------
# 调用 lv_font_conv
# ---------------------------------------------------------------------------

def run_conv(source_font: str, size: int, chars: set[str], output: str) -> None:
    if not os.path.exists(DRIVER):
        raise SystemExit(f"缺少转换器驱动：{DRIVER}")
    if not os.path.isdir(LV_FONT_CONV_DIR):
        raise SystemExit(f"找不到 lv_font_conv 包目录（设 LV_FONT_CONV_DIR）：{LV_FONT_CONV_DIR}")

    argv = [
        "--font", source_font,
        "--symbols", "".join(sorted(chars)),
        "--size", str(size),
        "--bpp", str(BPP),
        "--format", "lvgl",
        "--no-compress",
        # ⚠ 这一行是必须的，缺了它整个固件编不过。
        #
        # 不传 --lv-include 时 lv_font_conv 会写死 `#include "lvgl/lvgl.h"`，
        # 而组件管理器把 LVGL 的目录改名成了 `managed_components/lvgl__lvgl`
        # —— 于是 include 路径解析不到，报 `fatal error: lvgl/lvgl.h: No such
        # file or directory`。**生成出来的 .c 看着完全正常**，只有真去编译
        # （尤其是冷编）才会暴露，而那时已经烧掉十几分钟。
        #
        # 传 lvgl.h 之后产出 `#include "lvgl.h"`，与家族里其它仓库
        # （ancient-poets / ai-passport / art-magazine）完全一致 ——
        # 它们的生成器都带这个参数，本仓库第一版漏了。
        "--lv-include", "lvgl.h",
        "--output", output,
    ]
    handle = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False,
                                         encoding="utf-8")
    try:
        json.dump({"argv": argv}, handle)
        handle.close()
        env = dict(os.environ)
        env["LV_FONT_CONV_DIR"] = LV_FONT_CONV_DIR
        result = subprocess.run([NODE, DRIVER, handle.name],
                                capture_output=True, text=True, env=env)
        if result.returncode != 0:
            print(result.stdout[-2000:], file=sys.stderr)
            print(result.stderr[-2000:], file=sys.stderr)
            raise SystemExit(f"lv_font_conv 失败（{os.path.basename(output)}，"
                             f"字号 {size}，{len(chars)} 码点）")
    finally:
        os.unlink(handle.name)


# ---------------------------------------------------------------------------
# 硬判据：解析生成出来的 .c，还原「设备真能渲染的码点集」
# ---------------------------------------------------------------------------

_INT_LIST_RE = re.compile(
    r"static const uint(?:8|16)_t (\w+)\[\]\s*=\s*\{(.*?)\};", re.S)
_NUM_RE = re.compile(r"0x[0-9A-Fa-f]+|\d+")


def _parse_int_list(body: str) -> list[int]:
    return [int(v, 0) for v in _NUM_RE.findall(body)]


def codepoints_in_font_c(path: str) -> set[int]:
    """从 lv_font_conv 生成的 .c 里还原「这个字体真能画出来的码点集」。

    `cmaps[]` 有**三种**形态，漏掉任何一种都会得出一份骗人的清单：

      1. 连续区间 + `unicode_list = NULL` + `glyph_id_ofs_list = NULL`
         （`CMAP_FORMAT0_TINY`）⇒ 整段 range_start .. range_start+len-1 都有字形。

      2. 连续区间 + `unicode_list = NULL` + `glyph_id_ofs_list = glyph_id_ofs_list_N`
         （`CMAP_FORMAT0_FULL`）⇒ 区间里**有的码点没有字形**，靠偏移表里该位是否为 0
         表示。**第一版就是漏了这一条**：32px 档报了「96 个码点」，而实际上
         32..95 这段里有十几个是空的（主字体没有的 IPA 码点就在其中），
         于是「主字体单独就覆盖全部需求」这个错误结论差点让回退链形同虚设。
         屏幕上会出现的方框，恰恰就是从这一类漏检里漏出去的。

      3. `unicode_list = unicode_list_N`（`CMAP_SPARSE_TINY`）⇒ 码点 =
         **这条 cmap 自己的** range_start + 表内值。注意不能用别条 cmap 的 range_start
         （同一个文件里常常 ASCII 那条排在前面，取错会把全部偏移算到错误的码点上，
         结果是「清单里每个字都缺失」—— 满屏失败一定是解析错了，不是字库错了）。
    """
    with io.open(path, encoding="utf-8", errors="replace", newline="") as handle:
        text = handle.read()

    lists: dict[str, list[int]] = {}
    for name, body in _INT_LIST_RE.findall(text):
        lists[name] = _parse_int_list(body)

    start = text.find("cmaps[]")
    if start < 0:
        raise SystemExit(f"{path} 里找不到 cmaps[]，生成物格式变了？")
    end = text.find("};", start)
    body = text[start:end]

    found: set[int] = set()
    for block in re.findall(r"\{(.*?)\}", body, re.S):
        m_start = re.search(r"\.range_start\s*=\s*(\d+)", block)
        m_len = re.search(r"\.range_length\s*=\s*(\d+)", block)
        m_list = re.search(r"\.unicode_list\s*=\s*(\w+)", block)
        m_ofs = re.search(r"\.glyph_id_ofs_list\s*=\s*(\w+)", block)
        if not (m_start and m_len and m_list):
            continue
        range_start = int(m_start.group(1))
        range_length = int(m_len.group(1))
        list_name = m_list.group(1)
        ofs_name = m_ofs.group(1) if m_ofs else "NULL"

        if list_name != "NULL":
            found.update(range_start + v for v in lists.get(list_name, []))
        elif ofs_name == "NULL":
            found.update(range(range_start, range_start + range_length))
        else:
            ofs = lists.get(ofs_name, [])
            for i in range(range_length):
                # 偏移 0 表示「这个码点没有字形」—— **但区间首位例外**。
                # LVGL 自己的注释写得很清楚（lv_font_fmt_txt.c）：
                #   "The first character is always valid and should have offset = 0
                #    However if a character is missing it also has offset=0.
                #    So if there is a 0 not on the first position then it's a missing
                #    character"
                # 漏掉这条例外会误报首字缺字；把这条例外用过头（首位之外也放行）
                # 就会漏掉真正的缺字 —— 两边都会直接变成屏幕上的方框。
                if i == 0 or (i < len(ofs) and ofs[i] != 0):
                    found.add(range_start + i)
    return found


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def check(sets: dict[str, set[str]]) -> int:
    """硬判据：**主字体 ∪ 该档的 IPA 补充字体** 必须覆盖推导出的字符集。

    这里不能拿主字体单独比 —— 主字体本来就不含那 18 个 IPA 码点，它们靠
    `lv_font_t.fallback` 回退链补上。判据是「设备真能渲染的码点集」，
    也就是两套字体取并集。（第一版就是拿主字体单独比的，于是每次生成完
    都误报一大片 STALE，等于把一个真判据变成了噪声。）
    """
    problems = 0
    for name, size, tier in TIERS:
        path = os.path.join(FONTS_DIR, name + ".c")
        ipa_path = os.path.join(FONTS_DIR, name.replace("wd_font_", "wd_ipa_") + ".c")

        if not os.path.exists(path) or not os.path.exists(ipa_path):
            missing_files = [os.path.basename(p) for p in (path, ipa_path)
                             if not os.path.exists(p)]
            print(f"STALE  {' / '.join(missing_files)} 不存在", file=sys.stderr)
            problems += 1
            continue

        want = {ord(c) for c in sets[tier]}
        got = codepoints_in_font_c(path) | codepoints_in_font_c(ipa_path)
        missing = want - got
        if missing:
            preview = "".join(chr(c) for c in sorted(missing)[:40])
            print(f"STALE  {name}.c + {os.path.basename(ipa_path)}"
                  f" 合计仍缺 {len(missing)} 个码点：{preview!r}"
                  f"  ← 屏幕上会出现方框", file=sys.stderr)
            problems += 1
        else:
            print(f"  OK   {name}.c + {os.path.basename(ipa_path)}  {size}px"
                  f"  主字体 {len(codepoints_in_font_c(path))} 码点"
                  f" + 补充 {len(codepoints_in_font_c(ipa_path))} 码点"
                  f" ⇒ 覆盖 {len(want)} 码点")
    return problems


def generate(sets: dict[str, set[str]], only: str | None) -> int:
    os.makedirs(FONTS_DIR, exist_ok=True)

    main_cmap = font_codepoints(MAIN_FONT)
    ipa_cmap = font_codepoints(IPA_FONT)

    need = sets["full"] | sets["head"]
    missing = {c for c in need if ord(c) not in main_cmap}
    uncovered = {c for c in missing if ord(c) not in ipa_cmap}
    if uncovered:
        raise SystemExit(
            "补充字体也不覆盖这些码点，必须先解决再生成："
            + " ".join(f"{c}(U+{ord(c):04X})" for c in sorted(uncovered)))
    print(f"  主字体缺 {len(missing)} 个码点，全部由 IPA 补充字体覆盖")
    print(f"    {''.join(sorted(missing))}")

    for name, size, tier in TIERS:
        if only and only not in name:
            continue
        chars = sets[tier]
        out = os.path.join(FONTS_DIR, name + ".c")
        print(f"  生成 {name}.c  {size}px  {len(chars)} 码点 …", flush=True)
        run_conv(MAIN_FONT, size, chars, out)

        ipa_name = name.replace("wd_font_", "wd_ipa_")
        ipa_out = os.path.join(FONTS_DIR, ipa_name + ".c")
        print(f"  生成 {ipa_name}.c  {size}px  {len(missing)} 补充码点 …", flush=True)
        run_conv(IPA_FONT, size, missing, ipa_out)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="只校验现有字库覆盖当前字符集，不重新生成")
    parser.add_argument("--only", default=None, help="只生成名字里含该串的那一档")
    args = parser.parse_args()

    print("推导字符集：")
    sets = expected_charsets()
    if args.check:
        return 1 if check(sets) else 0
    generate(sets, args.only)
    print("校验生成结果：")
    return 1 if check(sets) else 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
