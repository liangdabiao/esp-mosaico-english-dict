// tests/test_wd_wrap.c —— 中英混排折行的宿主测试。
//
// 折行看起来是个"显示细节"，但在这个应用里它是**版式的承重墙**：
// 正文区高度、字段上限（content.py 里的 CAP_SENT_EN / CAP_TRANS）、
// 字号分档（wd_layout.h）三者全靠"一段文字折成几行"来对齐。
// 折行算错，前面所有预算都跟着失效，而且症状只是"最后一行被裁掉半截"。
//
// 四组：
//   1. 宽度表：半角 1 / 汉字 2 —— 这张表错一位，所有预算都是错的
//   2. 禁则表：中文标点不能落在行首，开括号不能落在行尾
//   3. 折行的结构性质：拼接还原（不丢字符）、必然前进（不死循环）、行数上限
//   4. **真实数据全量**：19513 条释义 + 两万条例句，逐条折行，检查行数预算与行首禁则

#include "wd_wrap.h"
#include "wd_content.h"
#include "wd_test.h"

// 把折行结果拼回来（跳过空格），应与原文去掉所有空格后逐字节相等。
// 这是"折行不丢字符"的唯一硬判据 —— 只检查行数或每行长度是看不出丢字的。
static void check_reassembly(const char *text, const wd_wrap_t *wrapped,
                             const char *label) {
    if (wrapped->truncated) return;   // 截断时本来就会丢尾部

    char joined[WD_WRAP_MAX_TEXT * 2];
    size_t n = 0;
    for (unsigned i = 0; i < wrapped->count; ++i) {
        for (unsigned k = 0; k < wrapped->len[i]; ++k) {
            joined[n++] = text[wrapped->start[i] + k];
        }
    }
    joined[n] = '\0';

    char want[WD_WRAP_MAX_TEXT * 2];
    size_t m = 0;
    for (const char *p = text; *p; ++p) {
        if (*p != ' ') want[m++] = *p;
    }
    want[m] = '\0';

    char got[WD_WRAP_MAX_TEXT * 2];
    size_t g = 0;
    for (size_t i = 0; i < n; ++i) {
        if (joined[i] != ' ') got[g++] = joined[i];
    }
    got[g] = '\0';

    if (strcmp(got, want) != 0) {
        WD_EXPECT(false, "[%s] 拼接还原不一致\n  原文 \"%s\"\n  拼回 \"%s\"", label,
                  text, got);
    } else {
        WD_EXPECT(true, "%s", "");
    }
}

// 每一行的第一个字符不能是禁则字符（除非它就是整段文字的起点）。
static void check_line_starts(const char *text, const wd_wrap_t *wrapped,
                             const char *label) {
    for (unsigned i = 0; i < wrapped->count; ++i) {
        const size_t at = wrapped->start[i];
        if (at == 0 || wrapped->len[i] == 0) continue;
        const unsigned char c = (unsigned char)text[at];
        uint32_t cp = c;
        if (c >= 0xF0) {
            cp = (uint32_t)(c & 0x07) << 18;
            cp |= (uint32_t)((unsigned char)text[at + 1] & 0x3F) << 12;
            cp |= (uint32_t)((unsigned char)text[at + 2] & 0x3F) << 6;
            cp |= (uint32_t)((unsigned char)text[at + 3] & 0x3F);
        } else if (c >= 0xE0) {
            cp = (uint32_t)(c & 0x0F) << 12;
            cp |= (uint32_t)((unsigned char)text[at + 1] & 0x3F) << 6;
            cp |= (uint32_t)((unsigned char)text[at + 2] & 0x3F);
        } else if (c >= 0xC0) {
            cp = (uint32_t)(c & 0x1F) << 6;
            cp |= (uint32_t)((unsigned char)text[at + 1] & 0x3F);
        }
        if (wd_wrap_no_line_start(cp)) {
            WD_EXPECT(false, "[%s] 第 %u 行以禁则字符 U+%04X 开头（原文 \"%s\"）",
                      label, i, cp, text);
        } else {
            WD_EXPECT(true, "%s", "");
        }
    }
}

static void test_units(void) {
    // ASCII
    WD_EXPECT(wd_wrap_unit_of('a') == 1, "小写字母应 1 单位");
    WD_EXPECT(wd_wrap_unit_of('Z') == 1, "大写字母应 1 单位");
    WD_EXPECT(wd_wrap_unit_of(' ') == 1, "空格应 1 单位");
    WD_EXPECT(wd_wrap_unit_of(',') == 1, "半角逗号应 1 单位");
    // 拉丁扩展与 IPA —— 字库里所有非 ASCII 音标字符都在这一档
    WD_EXPECT(wd_wrap_unit_of(0x00E6) == 1, "æ 应 1 单位");
    WD_EXPECT(wd_wrap_unit_of(0x0259) == 1, "ə 应 1 单位");
    WD_EXPECT(wd_wrap_unit_of(0x026A) == 1, "ɪ 应 1 单位");
    WD_EXPECT(wd_wrap_unit_of(0x014B) == 1, "ŋ 应 1 单位");
    WD_EXPECT(wd_wrap_unit_of(0x02C8) == 1, "ˈ 应 1 单位");
    WD_EXPECT(wd_wrap_unit_of(0x02D0) == 1, "ː 应 1 单位");
    // 汉字与全角标点
    WD_EXPECT(wd_wrap_unit_of(0x4E2D) == 2, "中 应 2 单位");
    WD_EXPECT(wd_wrap_unit_of(0x9FFF) == 2, "U+9FFF 应 2 单位");
    WD_EXPECT(wd_wrap_unit_of(0xFF0C) == 2, "全角逗号应 2 单位");
    WD_EXPECT(wd_wrap_unit_of(0x3002) == 2, "。应 2 单位");
    WD_EXPECT(wd_wrap_unit_of(0xFF08) == 2, "（应 2 单位");

    // wd_wrap_units 与逐字符累加一致
    WD_EXPECT(wd_wrap_units("abc") == 3, "abc 应 3 units");
    WD_EXPECT(wd_wrap_units("中文") == 4, "「中文」应 4 units");
    WD_EXPECT(wd_wrap_units("a中") == 3, "「a中」应 3 units");
    WD_EXPECT(wd_wrap_units("") == 0, "空串应 0 units");
    WD_EXPECT(wd_wrap_units(0) == 0, "空指针应 0 units");
}

static void test_forbidden(void) {
    // 行首禁则
    WD_EXPECT(wd_wrap_no_line_start(0xFF0C), "，不能在行首");
    WD_EXPECT(wd_wrap_no_line_start(0x3002), "。不能在行首");
    WD_EXPECT(wd_wrap_no_line_start(0xFF09), "）不能在行首");
    WD_EXPECT(wd_wrap_no_line_start(0x300D), "」不能在行首");
    WD_EXPECT(wd_wrap_no_line_start(0xFF1F), "？不能在行首");
    WD_EXPECT(wd_wrap_no_line_start(0x2026), "…不能在行首");
    WD_EXPECT(wd_wrap_no_line_start('.'), ". 不能在行首");
    WD_EXPECT(!wd_wrap_no_line_start(0x4E2D), "汉字可以在行首");
    WD_EXPECT(!wd_wrap_no_line_start('a'), "字母可以在行首");

    // 行尾禁则
    WD_EXPECT(wd_wrap_no_line_end(0xFF08), "（不能在行尾");
    WD_EXPECT(wd_wrap_no_line_end(0x300A), "《不能在行尾");
    WD_EXPECT(wd_wrap_no_line_end(0x201C), "“不能在行尾");
    WD_EXPECT(wd_wrap_no_line_end('('), "( 不能在行尾");
    WD_EXPECT(!wd_wrap_no_line_end(0x4E2D), "汉字可以在行尾");
    WD_EXPECT(!wd_wrap_no_line_end(0xFF0C), "，可以在行尾");
}

static void test_basic(void) {
    // 空与极短
    {
        wd_wrap_t w = wd_wrap_text("", 26);
        WD_EXPECT(w.count == 0, "空串应 0 行，实得 %u", w.count);
        WD_EXPECT(!w.truncated, "空串不该标截断");

        w = wd_wrap_text(0, 26);
        WD_EXPECT(w.count == 0, "空指针应 0 行");

        w = wd_wrap_text("hi", 26);
        WD_EXPECT(w.count == 1, "「hi」应 1 行，实得 %u", w.count);
        WD_EXPECT(w.len[0] == 2 && w.start[0] == 0, "「hi」应占 2 字节");

        // units_per_line == 0 时按 26 处理，不能返回 0 行
        w = wd_wrap_text("hello world", 0);
        WD_EXPECT(w.count >= 1, "width=0 时应当有兜底宽度，不该 0 行");
    }

    // 英文按词断：整句 5 个单词，每行放得下 2 个
    {
        const char *text = "one two three four five";
        wd_wrap_t w = wd_wrap_text(text, 8);
        WD_EXPECT(w.count >= 2, "应按词断成多行，实得 %u 行", w.count);
        check_reassembly(text, &w, "英文按词");
        check_line_starts(text, &w, "英文按词");
        // 每一行都不该以空格开头
        for (unsigned i = 0; i < w.count; ++i) {
            if (w.len[i] == 0) continue;
            WD_EXPECT(text[w.start[i]] != ' ', "第 %u 行不该以空格开头", i);
        }
    }

    // 中文按字断：20 个汉字，每行 5 个（10 units）
    {
        const char *text = "一二三四五六七八九十一二三四五六七八九十";
        wd_wrap_t w = wd_wrap_text(text, 10);
        WD_EXPECT(w.count == 4, "20 个汉字按 10 units 应 4 行，实得 %u", w.count);
        for (unsigned i = 0; i < w.count; ++i) {
            WD_EXPECT(w.len[i] == 15, "每行应 5 个汉字 = 15 字节，第 %u 行实得 %u", i,
                      w.len[i]);
        }
        check_reassembly(text, &w, "中文按字");
    }

    // 行首禁则：把逗号正好顶到行首的位置
    {
        // 「一二三四五，六七八九十」每行 5 个汉字 —— 逗号正好落在第 2 行行首。
        // 正确行为是把它**拉回上一行**（悬挂标点），于是第 1 行变成 6 个字符宽。
        const char *text = "一二三四五，六七八九十";
        wd_wrap_t w = wd_wrap_text(text, 10);
        check_line_starts(text, &w, "悬挂标点");
        check_reassembly(text, &w, "悬挂标点");
        // 逗号必须和它前面的字在同一行
        for (unsigned i = 0; i < w.count; ++i) {
            for (unsigned k = 0; k + 2 < w.len[i]; ++k) {
                if ((unsigned char)text[w.start[i] + k] == 0xEF &&
                    (unsigned char)text[w.start[i] + k + 1] == 0xBC &&
                    (unsigned char)text[w.start[i] + k + 2] == 0x8C) {
                    WD_EXPECT(k > 0, "全角逗号不该出现在行首位置 0");
                }
            }
        }
        (void)text;
    }

    // 长单词硬断：32 个字母没有空格，行宽 10 —— 必须切成 4 行，不能死循环
    {
        const char *text = "abcdefghijklmnopqrstuvwxyzabcdef";
        wd_wrap_t w = wd_wrap_text(text, 10);
        WD_EXPECT(w.count == 4, "32 字母按 10 units 应 4 行，实得 %u", w.count);
        check_reassembly(text, &w, "硬断");
    }

    // 行数上限：500 个字符、行宽 2 —— 必然超过 WD_WRAP_MAX_LINES
    {
        char text[WD_WRAP_MAX_TEXT + 8];
        for (unsigned i = 0; i < sizeof(text) - 1; ++i) text[i] = 'a';
        text[sizeof(text) - 1] = '\0';
        wd_wrap_t w = wd_wrap_text(text, 2);
        WD_EXPECT(w.count == WD_WRAP_MAX_LINES, "应填满 %u 行，实得 %u",
                  WD_WRAP_MAX_LINES, w.count);
        WD_EXPECT(w.truncated, "超过行数上限必须标 truncated（UI 好显示省略号）");
    }

    // 非法 UTF-8：按单字节推进，绝不卡死
    {
        const char bad[] = "\xE4\xB8\x2C\x41\x42\x43\x44\x45\x46\x47\x48";
        wd_wrap_t w = wd_wrap_text(bad, 3);
        WD_EXPECT(w.count >= 1, "非法 UTF-8 也应当折出行来");
        WD_EXPECT(w.count <= WD_WRAP_MAX_LINES, "行数仍受上限约束");
        // 截断的 3 字节序列之后还要能继续 —— 检查最后一行确实推进到了字符串尾
        size_t last = w.start[w.count - 1] + w.len[w.count - 1];
        WD_EXPECT(last <= strlen(bad), "行的范围不该越过字符串结尾");
    }

    // 空白文本：全是空格
    {
        wd_wrap_t w = wd_wrap_text("        ", 4);
        WD_EXPECT(w.count == 0, "纯空格不该产生行，实得 %u", w.count);
    }
}

// 真实数据：全量折行，检查行数预算与行首禁则。
static void test_real(const char *path) {
    size_t size = 0;
    unsigned char *data = wd_read_file(path, &size);
    if (!data) {
        printf("（跳过真实数据组：读不到 %s）\n", path);
        return;
    }

    wd_blob_t blob;
    const wd_manifest_err_t merr = wd_manifest_open(data, size, &blob);
    if (merr != WD_MANIFEST_OK) {
        WD_EXPECT(false, "blob 打开失败：%s", wd_manifest_err_text(merr));
        free(data);
        return;
    }

    // 折行只吃文本，不关心书与序号 —— 遍历全部词条即可。
    unsigned trans_over_6 = 0, trans_total = 0;
    unsigned en_over_5 = 0, en_total = 0;
    unsigned cn_over_4 = 0, cn_over_4_total = 0;
    unsigned max_trans_lines = 0, max_en_lines = 0, max_cn_lines = 0;
    unsigned trans_with_cjk_punct = 0;

    for (uint32_t i = 0; i < blob.word_count; ++i) {
        wd_word_t word;
        if (!wd_content_get(&blob, i, &word)) continue;

        const char *trans = wd_content_field(&word, WD_FIELD_TRANS);
        const char *en = wd_content_field(&word, WD_FIELD_SENT_EN);
        const char *cn = wd_content_field(&word, WD_FIELD_SENT_CN);

        const wd_wrap_t wt = wd_wrap_text(trans, 17);   // 24px 档
        const wd_wrap_t we = wd_wrap_text(en, 25);      // 16px 档
        const wd_wrap_t wc = wd_wrap_text(cn, 25);

        if (wt.count > max_trans_lines) max_trans_lines = wt.count;
        if (we.count > max_en_lines) max_en_lines = we.count;
        if (wc.count > max_cn_lines) max_cn_lines = wc.count;
        if (wt.count > 6) ++trans_over_6;
        if (we.count > 5) ++en_over_5;
        if (wc.count > 4) ++cn_over_4;
        ++trans_total;
        if (en[0]) ++en_total;
        if (cn[0]) { ++cn_over_4_total; }

        // 数据里确实存在中文标点 —— 证明下面的行首检查不是空转
        for (const char *p = trans; *p; ++p) {
            const unsigned char c = (unsigned char)*p;
            if (c == 0xEF || c == 0xE3 || c == 0xE2 || c == 0xE2) {
                // 粗判：UTF-8 三字节首字节，够用来确认"有全角标点"
                trans_with_cjk_punct += 1;
                break;
            }
        }

        // 硬上限：wd_wrap 自己最多 8 行，超过就会被标 truncated
        WD_EXPECT(!wt.truncated, "第 %u 条释义折行超过 %u 行的硬上限", i,
                  WD_WRAP_MAX_LINES);

        check_line_starts(trans, &wt, "释义");
        check_line_starts(en, &we, "例句英");
        check_line_starts(cn, &wc, "例句中");
        check_reassembly(trans, &wt, "释义");
        check_reassembly(en, &we, "例句英");
        check_reassembly(cn, &wc, "例句中");
    }

    printf("ok  真实数据折行：%u 条释义 / %u 条英文例句 / %u 条中文例句\n", trans_total,
           en_total, cn_over_4_total);
    printf("      最长行数  释义 %u / 6   例句英 %u / 5   例句中 %u / 4\n",
           max_trans_lines, max_en_lines, max_cn_lines);
    printf("      超预算     释义 %u (%.2f%%)  例句英 %u (%.2f%%)  例句中 %u (%.2f%%)\n",
           trans_over_6, 100.0 * trans_over_6 / (trans_total ? trans_total : 1),
           en_over_5, 100.0 * en_over_5 / (en_total ? en_total : 1),
           cn_over_4, 100.0 * cn_over_4 / (cn_over_4_total ? cn_over_4_total : 1));
    printf("      含全角标点的释义 %u 条（行首禁则检查确实扫到了标点）\n",
           trans_with_cjk_punct);

    // 超预算的比例要有上界：超过就说明字段上限或字号分档该调整了。
    // 少量溢出是可接受的（UI 会截断并显示省略号），但比例必须有据可查。
    if (trans_total) {
        WD_EXPECT(100.0 * trans_over_6 / trans_total < 1.0,
                  "释义超 6 行的比例 %.2f%% 过高（预期 < 1%%）",
                  100.0 * trans_over_6 / trans_total);
    }
    if (en_total) {
        WD_EXPECT(100.0 * en_over_5 / en_total < 1.0,
                  "英文例句超 5 行的比例 %.2f%% 过高（预期 < 1%%）",
                  100.0 * en_over_5 / en_total);
    }
    if (cn_over_4_total) {
        WD_EXPECT(100.0 * cn_over_4 / cn_over_4_total < 1.0,
                  "中文例句超 4 行的比例 %.2f%% 过高（预期 < 1%%）",
                  100.0 * cn_over_4 / cn_over_4_total);
    }

    free(data);
}

int main(int argc, char **argv) {
    const char *blob_path = (argc > 1) ? argv[1] : WD_BLOB_DEFAULT;

    test_units();
    test_forbidden();
    test_basic();
    test_real(blob_path);

    WD_SUMMARY("test_wd_wrap");
}
