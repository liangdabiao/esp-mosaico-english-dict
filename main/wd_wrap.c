// main/wd_wrap.c —— 中英混排折行（纯逻辑，可在宿主机编译）。
#include "wd_wrap.h"

#include <string.h>

// ---------------------------------------------------------------------------
// UTF-8 解码
//
// 对非法序列一律"按单字节推进"而不是报错：折行是个纯显示功能，遇到一个坏字节
// 就整段不显示是最糟的选择。按单字节推进保证指针永远前进 —— 任何"可能不前进"
// 的路径在折行里都会变成死循环，而这是在设备上表现为"按一下键就卡死"的那类 bug。
// ---------------------------------------------------------------------------
static unsigned utf8_next(const char *s, uint32_t *out) {
    const uint8_t c = (uint8_t)s[0];
    if (c < 0x80u) {
        *out = c;
        return 1;
    }

    unsigned need;
    uint32_t cp;
    if ((c & 0xE0u) == 0xC0u) {
        need = 1;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0u) == 0xE0u) {
        need = 2;
        cp = c & 0x0Fu;
    } else if ((c & 0xF8u) == 0xF0u) {
        need = 3;
        cp = c & 0x07u;
    } else {
        *out = c;  // 非法起始字节
        return 1;
    }

    for (unsigned i = 1; i <= need; ++i) {
        const uint8_t cont = (uint8_t)s[i];
        if ((cont & 0xC0u) != 0x80u) {
            // 续字节不合法（含撞上字符串结尾的情况：'\0' 同样不满足 10xxxxxx）
            *out = c;
            return 1;
        }
        cp = (cp << 6) | (cont & 0x3Fu);
    }
    *out = cp;
    return need + 1;
}

// 返回 at 之前那个码点的起始偏移（至少回退 1 字节）。
static size_t step_back(const char *text, size_t begin, size_t at) {
    if (at <= begin) return begin;
    size_t p = at - 1;
    while (p > begin && ((uint8_t)text[p] & 0xC0u) == 0x80u) --p;
    return p;
}

// 去掉 [begin, end) 末尾的空格。
static size_t trim_trailing_spaces(const char *text, size_t begin, size_t end) {
    while (end > begin && text[end - 1] == ' ') --end;
    return end;
}

// ---------------------------------------------------------------------------
// 宽度
// ---------------------------------------------------------------------------
unsigned wd_wrap_unit_of(uint32_t cp) {
    // 半角：ASCII、拉丁扩展（é ç）、IPA（ə ɪ ŋ ʃ）。字库里所有的非 ASCII
    // 音标字符都在这一档 —— 它们的 advance 与 ASCII 同量级（0.2~0.6 em）。
    if (cp < 0x1100u) return 1;

    // 以下按"东亚宽度 = 宽"（East Asian Wide / Fullwidth）：
    if (cp >= 0x2E80u && cp <= 0xA4CFu) return 2;   // CJK 部首/假名/统一表意/注音
    if (cp >= 0xAC00u && cp <= 0xD7A3u) return 2;   // 谚文音节
    if (cp >= 0xF900u && cp <= 0xFAFFu) return 2;   // CJK 兼容表意
    if (cp >= 0xFE30u && cp <= 0xFE6Fu) return 2;   // CJK 兼容形式
    if (cp >= 0xFF00u && cp <= 0xFF60u) return 2;   // 全角 ASCII 变体
    if (cp >= 0xFFE0u && cp <= 0xFFE6u) return 2;   // 全角符号（￥ ￠ 等）
    if (cp >= 0x20000u) return 2;                   // 扩展 B 及以后
    return 1;
}

// ---------------------------------------------------------------------------
// 中文禁则
//
// 只列**实际会出现在本词库文本里**的那些字符，再加上常见的中文标点。
// 不是越全越好：每加一个字符都要有"它真的会出现"的理由，否则这张表会变成
// 一个没人敢改的、从别处抄来的大清单。
//
// 参考：源数据里的标点实际只有 ，。、；：！？（）·…“”‘’ 这十几种。
// ---------------------------------------------------------------------------
bool wd_wrap_no_line_start(uint32_t cp) {
    switch (cp) {
        // 半角
        case ',': case '.': case ';': case ':': case '!': case '?':
        case ')': case ']': case '}': case '>': case '%':
            return true;
        // 全角与中文标点
        case 0x3001:  // 、
        case 0x3002:  // 。
        case 0xFF0C:  // ，
        case 0xFF1B:  // ；
        case 0xFF1A:  // ：
        case 0xFF01:  // ！
        case 0xFF1F:  // ？
        case 0xFF09:  // ）
        case 0x3015:  // 〕
        case 0xFF3D:  // ］
        case 0xFF5D:  // ｝
        case 0x300B:  // 》
        case 0x300D:  // 」
        case 0x300F:  // 』
        case 0x3009:  // 〉
        case 0x3011:  // 】
        case 0x201D:  // ”
        case 0x2019:  // ’
        case 0x00B7:  // ·
        case 0x2026:  // …
        case 0x2027:  // ‧
        case 0x2014:  // —
        case 0x2030:  // ‰
        case 0x2103:  // ℃
            return true;
        default:
            return false;
    }
}

bool wd_wrap_no_line_end(uint32_t cp) {
    switch (cp) {
        case '(': case '[': case '{': case '<':
            return true;
        case 0x3008:  // 〈
        case 0x300A:  // 《
        case 0x300C:  // 「
        case 0x300E:  // 『
        case 0x3010:  // 【
        case 0xFF08:  // （
        case 0x3014:  // 〔
        case 0xFF3B:  // ［
        case 0xFF5B:  // ｛
        case 0x201C:  // “
        case 0x2018:  // ‘
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// 折行
// ---------------------------------------------------------------------------

unsigned wd_wrap_units(const char *text) {
    if (!text) return 0;
    unsigned units = 0;
    const size_t total = strlen(text);
    for (size_t p = 0; p < total;) {
        uint32_t cp = 0;
        p += utf8_next(text + p, &cp);
        units += wd_wrap_unit_of(cp);
    }
    return units;
}

// p 处能否断行（"下一行从 p 开始"）？返回下一行的起点，0 表示不能在这里断。
static size_t break_candidate(const char *text, size_t total, size_t begin,
                              size_t p, uint32_t prev_cp) {
    // 空格之后可以断：跳过随后所有空格，下一行从第一个非空格起
    if (text[p - 1] == ' ') {
        size_t q = p;
        while (q < total && text[q] == ' ') ++q;
        return (q > begin && q < total) ? q : 0;
    }

    uint32_t cp = 0;
    utf8_next(text + p, &cp);

    // 宽字符之前可以断 —— 中文没有词边界，字与字之间天然可断
    if (wd_wrap_unit_of(cp) == 2) return p;

    // 半角字符之前，若**前一个字符是宽字符**，也可以断。
    // 这是中英交界处的断点来源（"n 恶意" 的 n|恶、"abc中文" 的 c|中）；
    // 纯英文（前后都是半角）则不断，交给空格处的词边界。
    if (prev_cp && wd_wrap_unit_of(prev_cp) == 2) return p;

    return 0;
}

// 算出从 pos 开始的一行：[out_line_end) 是本行的显示区间（已去尾空格），
// out_next 是下一行的起点。**保证 out_next > pos**。
static void wrap_one(const char *text, size_t total, size_t pos, unsigned width,
                     size_t *out_line_end, size_t *out_next) {
    size_t p = pos;
    unsigned units = 0;
    size_t best = 0;             // 最靠右的可用断点（下一行的起点）
    uint32_t prev_cp = 0;

    while (p < total && text[p]) {
        uint32_t cp = 0;
        const unsigned bytes = utf8_next(text + p, &cp);

        if (p > pos) {
            const size_t candidate = break_candidate(text, total, pos, p, prev_cp);
            if (candidate > best) best = candidate;
        }

        const unsigned w = wd_wrap_unit_of(cp);
        // `units > 0` 是防死循环的关键：单字符就比整行宽时也必须收下它，
        // 否则这一行的长度会是 0，调用方永远前进不了。
        if (units + w > width && units > 0) break;

        units += w;
        prev_cp = cp;
        p += bytes;
    }

    // 没超宽 = 这一段就是最后一行
    size_t next = (p >= total) ? total : (best > pos ? best : p);
    if (next > total) next = total;

    // ---- 禁则一：下一行不能以标点开头 ----
    // 做法不是把标点推到下一行，而是**把标点拉回本行**（悬挂标点）：
    // 本行因此略微超出 26 units，但这是有意的取舍 —— 中文排版里
    // "标点掉到行首"让读者停顿，"行尾多出半个字"几乎看不出来。
    //
    // ⚠ 必须先跳过空白再判。源数据里大量出现
    //   `(= become an actor ) .` 这种"右括号 + 空格 + 句号"的写法
    //   （英文例句里的括号注释），断点会落在那个空格上，
    //   于是下一行的第一个**非空字符**才是句号 —— 只看 text[next]
    //   会得到"是个空格，可以断"，句号照样掉到行首。
    for (unsigned guard = 0; guard < 4 && next < total; ++guard) {
        size_t q = next;
        while (q < total && text[q] == ' ') ++q;
        if (q >= total) break;
        uint32_t cp = 0;
        const unsigned bytes = utf8_next(text + q, &cp);
        if (!wd_wrap_no_line_start(cp)) break;
        next = q + bytes;   // 空白与标点一起拉回本行
    }
    if (next > total) next = total;

    // ---- 禁则二：本行末尾不能是开括号 ----
    // 否则配对的右括号会孤零零地出现在下一行开头。
    // 与禁则一相反，这里是把开括号**推到下一行**（本行变短）。
    for (unsigned guard = 0; guard < 4; ++guard) {
        size_t last = next;
        while (last > pos && text[last - 1] == ' ') --last;
        if (last <= pos) break;
        const size_t start = step_back(text, pos, last);
        if (start <= pos) break;   // 本行只剩这一个字符，不再推
        uint32_t cp = 0;
        utf8_next(text + start, &cp);
        if (!wd_wrap_no_line_end(cp)) break;
        next = start;
    }

    // 兜底：任何修正都不能让 next 退回 pos（那会让调用方原地打转）。
    if (next <= pos) {
        uint32_t cp = 0;
        next = pos + utf8_next(text + pos, &cp);
        if (next > total) next = total;
    }

    *out_line_end = trim_trailing_spaces(text, pos, next);
    *out_next = next;
}

wd_wrap_t wd_wrap_text(const char *text, unsigned units_per_line) {
    wd_wrap_t result;
    memset(&result, 0, sizeof(result));
    if (!text || !text[0]) return result;
    if (units_per_line == 0) units_per_line = 23;   // 32px 正文档的宽度

    size_t total = strlen(text);
    if (total > WD_WRAP_MAX_TEXT) total = WD_WRAP_MAX_TEXT;

    size_t pos = 0;
    while (pos < total) {
        // 下一行不以空格开头（断点已经跳过空格，这里兜住硬断等路径）
        while (pos < total && text[pos] == ' ') ++pos;
        if (pos >= total) break;

        if (result.count >= WD_WRAP_MAX_LINES) {
            result.truncated = true;   // 尾部放不下：UI 显示省略号，不要静默丢字
            break;
        }

        size_t line_end = pos;
        size_t next = pos;
        wrap_one(text, total, pos, units_per_line, &line_end, &next);
        if (next <= pos) next = pos + 1;   // 不可能发生，但保住"一定前进"

        result.start[result.count] = (uint16_t)pos;
        result.len[result.count] = (uint16_t)(line_end - pos);
        ++result.count;
        pos = next;
    }
    return result;
}
