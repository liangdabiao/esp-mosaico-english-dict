// main/wd_layout.c —— 版式度量表与字号自适应（纯逻辑，可在宿主机编译）。
#include "wd_layout.h"

// 这张表就是 main/wd_layout.h 头注释里那份实测结果的落点。
// units_per_line 沿自 210px 宽实测（P5），按 432/210 等比放大再向下取整
// （见 wd_layout.h 的推算）。改任何一个数之前，先跑
// tools/wordbook/measure_layout.py 复核。
const wd_tier_metrics_t wd_tier_metrics[WD_TIER_COUNT] = {
    // size, line_height (size x 1.3), units_per_line（字号翻倍后单位数折半）
    {64, 84, 12},   // WD_TIER_BIG   —— 单词；432px 下约 12 个半角字符
    {48, 63, 17},   // WD_TIER_MID   —— 释义/例句；17 units = 17 个汉字/行
    {32, 42, 23},   // WD_TIER_SMALL —— 音标、按钮、状态栏；23 units
};

wd_tier_t wd_layout_pick_tier(const char *text, wd_fit_probe_t probe, void *ctx) {
    if (!text || !text[0]) return WD_TIER_BIG;
    if (!probe) return WD_TIER_SMALL;   // 没有度量手段时按最小档处理，绝不冒险放大

    // 从大到小试。顺序就是"尽量用大字"这个产品意图本身 ——
    // 这是一个学习工具，字越大越好认，只是放不下才退。
    for (unsigned i = 0; i < WD_TIER_COUNT; ++i) {
        const wd_tier_t tier = (wd_tier_t)i;
        if (probe(ctx, tier, text)) return tier;
    }

    // 三个档都放不下。返回最小档而不是"最后一个试过的档"：
    // 最小档的溢出量最小，配合 UI 层的硬断/省略号，读起来最接近完整。
    return WD_TIER_SMALL;
}
