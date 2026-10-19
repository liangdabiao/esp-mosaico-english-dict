// main/wd_font.c —— 见 wd_font.h。
//
// 本文件必须编译成 C（不是 C++）：生成的字库把 `const lv_font_t wd_font_64`
// 定义在文件作用域，在 C 里这是**外部链接**的符号，在 C++ 里 const 默认是
// 内部链接 —— 同一个声明在 .cc 里就会变成另一个（不存在的）符号，
// 链接期报 undefined reference，看不出根因。
#include "wd_font.h"

#include "esp_log.h"

#define TAG "wd_font"

// 生成的字库符号。由 main/CMakeLists.txt 的 target_sources 直接编入
// （assets/fonts/wd_font_*.c / wd_ipa_*.c），且是**唯一**的编入路径 ——
// 不要再把同一个 .c 加进 SRCS，也不要被别的 .c 直接 include。
extern const lv_font_t wd_font_64;
extern const lv_font_t wd_font_48;
extern const lv_font_t wd_font_32;
extern const lv_font_t wd_ipa_64;
extern const lv_font_t wd_ipa_48;
extern const lv_font_t wd_ipa_32;

// RAM 副本。见头文件里"为什么要拷一份到 RAM"。
static lv_font_t s_font[WD_TIER_COUNT];
static bool s_ready;

void wd_font_init(void) {
    if (s_ready) return;

    // 主字体 —— 64px 档只含 ASCII + IPA，不含任何汉字（这是字库能小下来的
    // 关键：大字档从上万字形压到几十个）。它只渲染单词。
    s_font[WD_TIER_BIG] = wd_font_64;
    s_font[WD_TIER_MID] = wd_font_48;
    s_font[WD_TIER_SMALL] = wd_font_32;

    // 回退链：主字体找不到的字形交给补充字体。顺序不能反 ——
    // 补充字体只有几十个音标字形，当成主字体的话汉字会全部落空。
    s_font[WD_TIER_BIG].fallback = &wd_ipa_64;
    s_font[WD_TIER_MID].fallback = &wd_ipa_48;
    s_font[WD_TIER_SMALL].fallback = &wd_ipa_32;

    // 打一行日志把三档的真实度量写出来。这些数字只能在设备上看到，
    // 而 wd_layout 表里的都是**预算值**（行高按字号 x1.3 取整，比真实行高大）。
    // 两者的差值就是版式余量；这行日志是以后调版式时唯一的实测依据。
    for (unsigned i = 0; i < WD_TIER_COUNT; ++i) {
        const wd_tier_metrics_t *budget = wd_layout_metrics((wd_tier_t)i);
        ESP_LOGI(TAG, "档 %u：字号 %u，预算行高 %u，字体真实行高 %ld，基线 %ld",
                 i, budget->size, budget->line_height,
                 (long)s_font[i].line_height, (long)s_font[i].base_line);
    }

    s_ready = true;
}

const lv_font_t *wd_font(wd_tier_t tier) {
    // 与 wd_layout_metrics 同样的兜底：越界按最小档，绝不返回 NULL。
    // 绘制路径上一个 NULL 字体是崩溃，而一个"字小一点"只是不好看。
    if (tier >= WD_TIER_COUNT) tier = WD_TIER_SMALL;
    if (!s_ready) wd_font_init();   // 忘了调 init 也要能用（幂等，代价一次分支）
    return &s_font[tier];
}

bool wd_font_fits(void *ctx, wd_tier_t tier, const char *text) {
    (void)ctx;
    if (!text || !text[0]) return true;

    const lv_font_t *font = wd_font(tier);

    // max_width 传 LV_COORD_MAX（= 不折行），量出来的 size.x 就是这一行
    // 不折行时的真实宽度。为什么不量"在 210px 里要几行"再跟行高比：
    // 那样得到的结论会混入"折行算法在哪儿断"，而这里要回答的只是
    // "这一档放不放得下"。宽度直接比，语义最清楚，也不受 LV_TXT_BREAK_CHARS
    // 配置的影响。
    lv_point_t size = {0, 0};
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    // 留 0 余量不现实：任何一个"刚好放下"的判定都会被亚像素的舍入推翻。
    // 所以探针要求宽度 <= 正文宽 —— 不是数学上的等于，而是 LVGL 量出来的
    // 整数像素值，与真实渲染用的是同一个函数，不存在第二套算法。
    return size.x <= (int32_t)WD_BODY_W;
}
