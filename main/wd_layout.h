// main/wd_layout.h —— 版式预算与字号自适应（纯逻辑，可在宿主机编译）。
//
// 2026-10 的「全部增加 100% 大」把三档字号整体翻倍：64 / 48 / 32px。
// 横向单位数沿用 210px 实测结论（P5 保守值）按 432/210 等比放大后**再按
// 字号翻倍折半**——字号与每行字符数成反比，翻倍字号就是一半单位：
//
//   正文宽 432px（左右各留 24px）：
//     32px -> 23 units（旧 16px 档放大后的等价档）
//     48px -> 17 units
//     64px -> 12 units
//
//   最长单词 telecommunications（18 字符）：
//     64px 约 630px、48px 约 472px，都超过 432px；32px 约 315px 放得下。
//     所以长词会落到 32px 档 —— 这是屏幕物理宽度决定的，不是预算写错。
//     wd_layout_pick_tier 的降档阶梯因此比之前更常用。
//
// 字号分档的职责划分（也是字库能塞进 Flash 的原因）：
//   64px  只渲染**单词**（英文）—— 字库只需 ASCII + IPA
//   48px  释义、中文例句、书名、设置标题 —— 全字符集
//   32px  英文例句、音标、状态栏、按钮、提示条 —— 全字符集
//
// 纵向预算用 _Static_assert 钉死在编译期。理由：这些常量之间是加法关系，
// 改任何一个（比如把状态栏加高 4px）都会挤掉正文区的行数，而症状是
// **屏幕上最后一行被裁掉半截** —— 只有把 19513 个词的释义都翻一遍才会发现。
// 编译期断言把这个错误变成一条编译错误。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "wd_assert.h"
#include "wd_quiz.h"   // WD_QUIZ_CHOICES —— 测验页的纵向预算按它算

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 屏幕与安全区
// ---------------------------------------------------------------------------
#define WD_SCREEN_W 480
#define WD_SCREEN_H 480
#define WD_SAFE_X 24                       // 左右留白：CO5300 是方屏无圆角裁剪，
                                           // 24px 让文字不贴边、按钮有足够的点按边距
#define WD_BODY_W (WD_SCREEN_W - 2 * WD_SAFE_X)   // 432

WD_STATIC_ASSERT(WD_BODY_W == 432, "正文宽必须是 432px —— 所有 units 预算都按它算的");

// ---------------------------------------------------------------------------
// 字号档位
// ---------------------------------------------------------------------------
typedef enum {
    WD_TIER_BIG = 0,     // 64px —— 单词大字
    WD_TIER_MID = 1,     // 48px —— 释义、例句、书名
    WD_TIER_SMALL = 2,   // 32px —— 状态栏、提示条、按钮与长词兜底
    WD_TIER_COUNT = 3,
} wd_tier_t;

typedef struct {
    uint8_t size;            // 字号 px
    uint8_t line_height;     // 行高 px（字号 x 1.3，留出行距）
    uint8_t units_per_line;  // 一行放得下的半角单位数（210px 实测 P5，等比放大取整）
} wd_tier_metrics_t;

extern const wd_tier_metrics_t wd_tier_metrics[WD_TIER_COUNT];

static inline const wd_tier_metrics_t *wd_layout_metrics(wd_tier_t tier) {
    if (tier >= WD_TIER_COUNT) tier = WD_TIER_SMALL;
    return &wd_tier_metrics[tier];
}

// 这一档在 height 像素高的区域里能排满几行（向下去整）。
static inline unsigned wd_layout_lines_in(wd_tier_t tier, unsigned height) {
    const unsigned line_h = wd_layout_metrics(tier)->line_height;
    return line_h ? height / line_h : 0;
}

// ---------------------------------------------------------------------------
// 单词字号自适应
//
// 从 64px 往下试，返回**第一个**能放下的档。
//
// 宽度由调用方探（probe）而不是在这里算：只有持有字体对象的那一层才知道
// 每个字形的真实 advance（NotoSansCJKsc 的 ASCII 是比例字宽，i 是 0.275em、
// m 是 0.926em，差 3 倍多）。在这里再维护一份 advance 表等于把同一个事实
// 存两遍 —— 而 UI 层的 LVGL 本来就有 lv_txt_get_width。
//
// 这个分层的好处是可测：宿主测试用一个"按字符数算宽度"的假 probe 就能验证
// 阶梯逻辑（64 -> 48 -> 32 -> 兜底），不需要字体、不需要屏幕。
// ---------------------------------------------------------------------------
typedef bool (*wd_fit_probe_t)(void *ctx, wd_tier_t tier, const char *text);

// 全部档都放不下时返回 WD_TIER_SMALL —— 这是"最接近能放下"的选择。
// 调用方拿到 SMALL 后仍要再确认一次：更长词组在最坏情况下 16px 也只是勉强，
// 必要时交给 wd_wrap 硬断。
// text 为空时返回 WD_TIER_BIG（不显示，档位无所谓）。
wd_tier_t wd_layout_pick_tier(const char *text, wd_fit_probe_t probe, void *ctx);

// ---------------------------------------------------------------------------
// 纵向预算（A 面 = 释义面 / B 面 = 例句面，共用状态栏与提示条）
// ---------------------------------------------------------------------------
#define WD_ZONE_STATUS_H 48u    // 顶部状态栏：返回 + 书名 + 进度 + 设置
#define WD_ZONE_WORD_H 96u      // 单词区（64px 行高 84 + 上下留白）
#define WD_ZONE_IPA_H 48u       // 音标行（32px，行高 42 + 上下留白）
#define WD_ZONE_DIVIDER_H 2u    // 分隔线
#define WD_ZONE_HINT_H 48u      // 底部提示条（只在启动/设置页显示）

#define WD_ZONE_A_BODY_H \
    (WD_SCREEN_H - (WD_ZONE_STATUS_H + WD_ZONE_WORD_H + WD_ZONE_IPA_H + \
                    WD_ZONE_DIVIDER_H + WD_ZONE_HINT_H))   // = 238

WD_STATIC_ASSERT(WD_ZONE_A_BODY_H == 238, "A 面正文区高度变了 —— 行数预算要重算");

// 词条页没有提示条，正文区一直延伸到动作条上沿。
WD_STATIC_ASSERT(WD_ZONE_A_BODY_H >= 3 * 63,
                 "正文区放不下 3 行 48px —— 版式预算要重算");
WD_STATIC_ASSERT(WD_ZONE_A_BODY_H >= 5 * 42,
                 "正文区放不下 5 行 32px —— 版式预算要重算");

#define WD_ZONE_GAP 4u   // 区块之间的呼吸间隔

// ③ 词条页：状态 48 / 单词 96 / 音标 48 / 分隔线 2 / 正文 150 / 动作条 136。
// 正文 150px 在 48px 下容 2 行、32px 下容 3 行；**正文是滚动区**，长释义与
// 长例句不再靠缩小字号硬塞，48px 看不完就上下滑。
#define WD_WORD_BAR_H 136u
#define WD_WORD_BODY_H (WD_SCREEN_H - (WD_ZONE_STATUS_H + WD_ZONE_WORD_H + \
                                       WD_ZONE_IPA_H + WD_ZONE_DIVIDER_H + \
                                       WD_WORD_BAR_H))   // 150
WD_STATIC_ASSERT(WD_WORD_BODY_H == 150, "词条页正文高度变了 —— 滚动区预算要重算");
WD_STATIC_ASSERT(2 * 63 <= WD_WORD_BODY_H, "正文区放不下 2 行 48px");
WD_STATIC_ASSERT(3 * 42 <= WD_WORD_BODY_H, "正文区放不下 3 行 32px");

// ④ 测验页：题干/反馈区 + 4 个 66px 选项（步进 70）。
// 题干是中文释义，48px 两行、32px 三行；作答后同一区域换成两行反馈。
// 48 + 140 + 2 + 3*70 + 66 = 466 ≤ 480。
#define WD_QUIZ_QUESTION_H 140u
#define WD_QUIZ_CHOICE_H 66u
#define WD_QUIZ_CHOICE_STEP 70u
WD_STATIC_ASSERT(WD_ZONE_STATUS_H + WD_QUIZ_QUESTION_H + 2 +
                         WD_QUIZ_CHOICE_STEP * (WD_QUIZ_CHOICES - 1) +
                         WD_QUIZ_CHOICE_H <= WD_SCREEN_H,
                 "测验页放不下 —— 题干 + 四个选项超了 480px");

// ---------------------------------------------------------------------------
// 选书 / 进度页：2 x 3 卡片网格 + 底部两枚按钮。
//
// 网格区：3 x 112 + 2 x 8 = 352，从 y=52 到 404；
// 底部按钮 64px（容 48px 字），y=412..476；状态栏占 0..48。
// ---------------------------------------------------------------------------
#define WD_GRID_COLS 2u
#define WD_GRID_ROWS 3u
#define WD_GRID_GAP 8u
#define WD_GRID_CARD_H 112u
#define WD_GRID_CARD_W ((WD_BODY_W - WD_GRID_GAP) / WD_GRID_COLS)   // 212
#define WD_GRID_FOOTER_H 64u

WD_STATIC_ASSERT(WD_GRID_CARD_W == 212, "网格卡片宽度算错了");
WD_STATIC_ASSERT(WD_ZONE_STATUS_H + 4 + WD_GRID_ROWS * WD_GRID_CARD_H +
                         (WD_GRID_ROWS - 1) * WD_GRID_GAP + WD_GRID_GAP +
                         WD_GRID_FOOTER_H <= WD_SCREEN_H,
                 "2x3 网格 + 底部按钮放不下 480px");

#ifdef __cplusplus
}  // extern "C"
#endif
