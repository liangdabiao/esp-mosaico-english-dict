// tests/test_wd_layout.c —— 版式度量与字号自适应的宿主测试。
//
// 两组：
//   1. **度量表必须与预算一致**。2026-10 的「全部增加 100% 大」之后三档是
//      64/48/32px（行高 84/63/42），units_per_line 按字号翻倍折半为 12/17/23。
//      这里把数字写死对撞 —— 以后有人"顺手调一下"就是一条失败断言，
//      而不是等到屏幕上出现裁掉半截的行才发现。
//   2. **字号自适应阶梯**：64 -> 48 -> 32 的顺序、提前停止、兜底行为。
//
// 另外一件在这里"看不见"但确实生效的事：main/wd_layout.h 与 main/wd_ui.h
// 里的 _Static_assert 会在**编译期**验证纵向预算。这个文件能编译通过，
// 本身就说明词条页 48 + 96 + 48 + 2 + 136 + 150 == 480 成立。

#include "wd_layout.h"
#include "wd_content.h"
#include "wd_test.h"

#include <string.h>

// 一个用"字符数 x 每字符 em 数 x 字号"估算宽度的探针。
//
// 真实 UI 层用 lv_txt_get_width 量（那里有字体对象）。这里用近似值是为了
// 在没有字体的宿主环境里验证**阶梯逻辑**本身 —— 阶梯的正确性与宽度算法无关。
// 0.55 em/字符取自 tools/wordbook/measure_layout.py 的实测（ASCII 平均
// advance 落在 0.47~0.52，长单词偏大，取 0.55 略保守）。
// 432px 正文宽下三个档的分界线（0.55 em）：
//   64px: len*35.2 <= 432 → len <= 12
//   48px: len*26.4 <= 432 → len <= 16
//   32px: len*17.6 <= 432 → len <= 24
typedef struct {
    unsigned max_calls;      // 最多允许被调用几次 —— 用来验证"提前停止"
    unsigned calls;
    wd_tier_t order[WD_TIER_COUNT + 2];
    unsigned order_len;
    double em_per_char;
} probe_ctx_t;

static bool width_probe(void *ctx, wd_tier_t tier, const char *text) {
    probe_ctx_t *c = (probe_ctx_t *)ctx;
    if (c->order_len < sizeof(c->order) / sizeof(c->order[0])) {
        c->order[c->order_len] = tier;
    }
    ++c->order_len;
    ++c->calls;

    const double width = (double)strlen(text) * c->em_per_char *
                         (double)wd_layout_metrics(tier)->size;
    return width <= (double)WD_BODY_W;
}

static void test_metrics(void) {
    // 屏与安全区
    WD_EXPECT(WD_SCREEN_W == 480 && WD_SCREEN_H == 480, "屏尺寸应为 480x480");
    WD_EXPECT(WD_SAFE_X == 24 && WD_BODY_W == 432, "安全区 24 / 正文宽 432");

    // 字号 / 行高 / 每行 units —— 与字号翻倍后的预算逐项对撞
    const struct {
        wd_tier_t tier;
        unsigned size;
        unsigned line_h;
        unsigned units;
        const char *name;
    } want[] = {
        {WD_TIER_BIG, 64, 84, 12, "64px"},
        {WD_TIER_MID, 48, 63, 17, "48px"},
        {WD_TIER_SMALL, 32, 42, 23, "32px"},
    };
    for (unsigned i = 0; i < 3; ++i) {
        const wd_tier_metrics_t *m = wd_layout_metrics(want[i].tier);
        WD_EXPECT(m->size == want[i].size, "%s 字号应 %u，实得 %u", want[i].name,
                  want[i].size, m->size);
        WD_EXPECT(m->line_height == want[i].line_h, "%s 行高应 %u，实得 %u",
                  want[i].name, want[i].line_h, m->line_height);
        WD_EXPECT(m->units_per_line == want[i].units,
                  "%s 每行 units 应 %u（字号翻倍后折半），实得 %u",
                  want[i].name, want[i].units, m->units_per_line);
    }

    // 越界档位退回最小档，不越界读表
    WD_EXPECT(wd_layout_metrics(WD_TIER_COUNT)->size == 32, "越界档位应退回 32px");
    WD_EXPECT(wd_layout_metrics((wd_tier_t)99)->size == 32, "越界档位应退回 32px");

    // 行数计算（按真实行高：32px 42、48px 63）
    WD_EXPECT(wd_layout_lines_in(WD_TIER_SMALL, 0) == 0, "0 高度应 0 行");
    WD_EXPECT(wd_layout_lines_in(WD_TIER_SMALL, 42) == 1, "42px 应 1 行");
    WD_EXPECT(wd_layout_lines_in(WD_TIER_SMALL, 41) == 0, "41px 不满 1 行");
    WD_EXPECT(wd_layout_lines_in(WD_TIER_MID, WD_WORD_BODY_H) == 2,
              "词条正文 %upx 应容 2 行 48px，实得 %u", WD_WORD_BODY_H,
              wd_layout_lines_in(WD_TIER_MID, WD_WORD_BODY_H));
    WD_EXPECT(wd_layout_lines_in(WD_TIER_SMALL, WD_WORD_BODY_H) == 3,
              "词条正文 %upx 应容 3 行 32px，实得 %u", WD_WORD_BODY_H,
              wd_layout_lines_in(WD_TIER_SMALL, WD_WORD_BODY_H));
    WD_EXPECT(wd_layout_lines_in(WD_TIER_MID, WD_QUIZ_QUESTION_H) == 2,
              "题干区 %upx 应容 2 行 48px，实得 %u", WD_QUIZ_QUESTION_H,
              wd_layout_lines_in(WD_TIER_MID, WD_QUIZ_QUESTION_H));
    WD_EXPECT(wd_layout_lines_in(WD_TIER_SMALL, WD_QUIZ_QUESTION_H) == 3,
              "题干区 %upx 应容 3 行 32px，实得 %u", WD_QUIZ_QUESTION_H,
              wd_layout_lines_in(WD_TIER_SMALL, WD_QUIZ_QUESTION_H));

    // 纵向预算加总：词条页没有提示条，正文一直铺到动作条上沿。
    const unsigned word_sum = WD_ZONE_STATUS_H + WD_ZONE_WORD_H + WD_ZONE_IPA_H +
                              WD_ZONE_DIVIDER_H + WD_WORD_BODY_H + WD_WORD_BAR_H;
    WD_EXPECT(word_sum == WD_SCREEN_H, "词条页各区块加总应等于屏高 %u，实得 %u",
              WD_SCREEN_H, word_sum);

    // 2x3 卡片网格 + 底部两枚按钮
    WD_EXPECT(WD_GRID_CARD_W == 212, "卡片宽应 212，实得 %u", WD_GRID_CARD_W);
    const unsigned grid_h = WD_ZONE_STATUS_H + WD_ZONE_GAP +
                            WD_GRID_ROWS * WD_GRID_CARD_H +
                            (WD_GRID_ROWS - 1) * WD_GRID_GAP + WD_ZONE_GAP +
                            WD_GRID_FOOTER_H;
    WD_EXPECT(grid_h <= WD_SCREEN_H, "网格 + 底栏高 %u 超过 %u", grid_h, WD_SCREEN_H);
    WD_EXPECT(WD_GRID_FOOTER_H >= 44,
              "底栏按钮高 %u 应 >= 44（触摸点按不误触）", WD_GRID_FOOTER_H);

    // 测验页：题干 + 四个选项
    const unsigned quiz_h = WD_ZONE_STATUS_H + WD_QUIZ_QUESTION_H + 2 +
                            WD_QUIZ_CHOICE_STEP * (WD_QUIZ_CHOICES - 1) +
                            WD_QUIZ_CHOICE_H;
    WD_EXPECT(quiz_h <= WD_SCREEN_H, "测验页高 %u 超过 %u", quiz_h, WD_SCREEN_H);
    WD_EXPECT(WD_QUIZ_CHOICE_H >= 63,
              "选项盒高 %u 要放得下一行 48px（行高 63）", WD_QUIZ_CHOICE_H);

    printf("ok  版式：屏 %ux%u / 正文 %upx / 三档 %u,%u,%u px / 每行 %u,%u,%u units\n",
           WD_SCREEN_W, WD_SCREEN_H, WD_BODY_W, wd_tier_metrics[0].size,
           wd_tier_metrics[1].size, wd_tier_metrics[2].size,
           wd_tier_metrics[0].units_per_line, wd_tier_metrics[1].units_per_line,
           wd_tier_metrics[2].units_per_line);
    printf("      词条页正文 %upx（48px 2 行 / 32px 3 行）；网格 %u + 底栏 %u = %u\n",
           WD_WORD_BODY_H, WD_GRID_ROWS * WD_GRID_CARD_H, WD_GRID_FOOTER_H, grid_h);
}

static void test_pick_tier(void) {
    // 空文本：不显示，档位无所谓 —— 约定给最大档
    {
        probe_ctx_t ctx = {0, 0, {0, 0, 0}, 0, 0.55};
        WD_EXPECT(wd_layout_pick_tier("", width_probe, &ctx) == WD_TIER_BIG,
                  "空文本应返回 BIG");
        WD_EXPECT(wd_layout_pick_tier(0, width_probe, &ctx) == WD_TIER_BIG,
                  "空指针应返回 BIG");
        WD_EXPECT(ctx.calls == 0, "空文本不该调用探针");
    }

    // 没有探针时按最小档处理 —— 宁可小，绝不冒险放大
    WD_EXPECT(wd_layout_pick_tier("hello", 0, 0) == WD_TIER_SMALL,
              "没有探针应返回 SMALL");

    // 12 字符：64px 下 12*35.2 = 422.4 <= 432，应停在 BIG
    {
        probe_ctx_t ctx = {0, 0, {0, 0, 0}, 0, 0.55};
        const wd_tier_t tier = wd_layout_pick_tier("abcdefghijkl", width_probe, &ctx);
        WD_EXPECT(tier == WD_TIER_BIG, "12 字符应在 64px 放下，实得档 %d", (int)tier);
        WD_EXPECT(ctx.calls == 1, "应当在第一档就停下，实得调用 %u 次", ctx.calls);
        WD_EXPECT(ctx.order_len == 1 && ctx.order[0] == WD_TIER_BIG,
                  "调用顺序应为 BIG");
    }

    // 13 字符：64px 下 457.6 > 432，48px 下 343.2 <= 432，应停在 MID
    {
        probe_ctx_t ctx = {0, 0, {0, 0, 0}, 0, 0.55};
        const wd_tier_t tier =
            wd_layout_pick_tier("abcdefghijklm", width_probe, &ctx);
        WD_EXPECT(tier == WD_TIER_MID, "13 字符应降到 48px，实得档 %d", (int)tier);
        WD_EXPECT(ctx.order_len == 2, "应调用两次，实得 %u", ctx.order_len);
        WD_EXPECT(ctx.order[0] == WD_TIER_BIG && ctx.order[1] == WD_TIER_MID,
                  "调用顺序应为 BIG -> MID");
    }

    // 17 字符：64px/48px 都超（598.4 / 448.8 > 432），32px（299.2）放得下
    {
        probe_ctx_t ctx = {0, 0, {0, 0, 0}, 0, 0.55};
        const wd_tier_t tier =
            wd_layout_pick_tier("abcdefghijklmnopq", width_probe, &ctx);
        WD_EXPECT(tier == WD_TIER_SMALL, "17 字符应降到 32px，实得档 %d", (int)tier);
        WD_EXPECT(ctx.order_len == 3, "应调用三次，实得 %u", ctx.order_len);
        WD_EXPECT(ctx.order[0] == WD_TIER_BIG && ctx.order[1] == WD_TIER_MID &&
                      ctx.order[2] == WD_TIER_SMALL,
                  "调用顺序应为 BIG -> MID -> SMALL");
    }

    // 三档都放不下：返回 SMALL（最接近能放下的那一档）
    {
        probe_ctx_t ctx = {0, 0, {0, 0, 0}, 0, 9.0};   // 无论多大都说放不下
        const wd_tier_t tier = wd_layout_pick_tier("abcdefghij", width_probe, &ctx);
        WD_EXPECT(tier == WD_TIER_SMALL, "都放不下时应返回 SMALL，实得 %d", (int)tier);
        WD_EXPECT(ctx.order_len == 3, "三档都应试过，实得 %u 次", ctx.order_len);
    }

    // 25 字符：48px 下 660 > 432，32px 下 440 > 432 —— 也走"都放不下"，
    // 但 24 字符（422.4）应当停在 SMALL。这一对钉住 32px 的边界。
    {
        probe_ctx_t ctx = {0, 0, {0, 0, 0}, 0, 0.55};
        WD_EXPECT(wd_layout_pick_tier("abcdefghijklmnopqrstuvwx", width_probe,
                                      &ctx) == WD_TIER_SMALL,
                  "24 字符应在 32px 放下");
        probe_ctx_t ctx2 = {0, 0, {0, 0, 0}, 0, 0.55};
        WD_EXPECT(wd_layout_pick_tier("abcdefghijklmnopqrstuvwxy", width_probe,
                                      &ctx2) == WD_TIER_SMALL,
                  "25 字符三档都放不下，兜底也应返回 SMALL");
    }
}

// 真实数据：用近似探针跑一遍全部单词的降档分布。
// 这不是精确值（没有字体），而是用来确认两件结构性事实：
//   a) 64px 大字档确实是**常态**而不是例外；
//   b) 最长词（18 字符）在 64px 下放不下、在 32px 下放得下 —— 阶梯真的在工作。
static void test_real_downgrade(const char *path) {
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

    unsigned big = 0, mid = 0, small = 0, longest_len = 0;
    wd_tier_t longest_tier = WD_TIER_BIG;

    for (uint32_t i = 0; i < blob.word_count; ++i) {
        wd_word_t word;
        if (!wd_content_get(&blob, i, &word)) continue;
        const char *text = wd_content_field(&word, WD_FIELD_WORD);
        if (!text[0]) continue;

        probe_ctx_t ctx = {0, 0, {0, 0, 0}, 0, 0.55};
        const wd_tier_t tier = wd_layout_pick_tier(text, width_probe, &ctx);
        switch (tier) {
            case WD_TIER_BIG: ++big; break;
            case WD_TIER_MID: ++mid; break;
            default: ++small; break;
        }

        const size_t len = strlen(text);
        if (len > longest_len) {
            longest_len = len;
            longest_tier = tier;
        }
    }

    const unsigned total = big + mid + small;
    printf("ok  真实单词的降档分布（近似探针）：64px %u (%.1f%%) / 48px %u (%.1f%%) / "
           "32px %u (%.1f%%)\n",
           big, 100.0 * big / total, mid, 100.0 * mid / total, small,
           100.0 * small / total);
    printf("      最长单词 %u 字符，落在 %d px 档\n", longest_len,
           wd_layout_metrics(longest_tier)->size);

    WD_EXPECT(total > 0, "应当有词条");
    // 三档在真实数据里都要出现 —— 任何一档为 0 都说明阈值或探针算错了。
    WD_EXPECT(big > 0 && mid > 0 && small > 0,
              "真实数据应覆盖三档，实得 %u/%u/%u", big, mid, small);
    WD_EXPECT(longest_len >= 14, "最长单词应当 >= 14 字符，实得 %u", longest_len);
    WD_EXPECT(longest_tier == WD_TIER_SMALL,
              "最长单词 %u 字符应降到 32px，实得 %d px", longest_len,
              wd_layout_metrics(longest_tier)->size);

    free(data);
}

int main(int argc, char **argv) {
    const char *blob_path = (argc > 1) ? argv[1] : WD_BLOB_DEFAULT;

    test_metrics();
    test_pick_tier();
    test_real_downgrade(blob_path);

    WD_SUMMARY("test_wd_layout");
}
