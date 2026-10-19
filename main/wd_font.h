// main/wd_font.h —— 三档字体 + IPA 补充字体的回退链。
//
// ---------------------------------------------------------------------------
// 为什么是"两套字体 + 回退链"而不是一套
// ---------------------------------------------------------------------------
// 主字体是 NotoSansCJKsc（SIL OFL）。实测它在应用真实字符集里
// **缺 18 个**，全是音标：ŋ ɒ ɔ ə ɚ ɛ ɜ ɝ ɪ ʃ ʊ ʌ ʒ ʳ ˈ ˌ ː ˚
// （`tools/wordbook/gen_font.py` 每次生成都会把这行数字重新算一遍并打印）。
// 而几乎每个词的音标都含 ə / ɪ / ɛ —— 只有一套字体的话，音标行会满屏方框，
// 而音标是这本词典的第二重要信息。
//
// 补充字体用 Noto Sans（同为 SIL OFL，可随仓库分发），只含主字体缺的那些码点，
// 每档几十个字形。两套字体在运行期由 LVGL 的 `lv_font_t.fallback` 串起来。
//
// **不用 fontTools merge**：merge 出来的字体两份 advance 表会打架
// （同一个码点在两份源里宽度不同，merge 只能取一个，取哪个都是错的）。
// LVGL 的 fallback 是运行期逐字形解析（lv_font.c:123，注释写明"Resolved
// recursively"），每个字形始终用它自己的那张表，不存在这个问题。
//
// ---------------------------------------------------------------------------
// 为什么要拷一份到 RAM（这是本文件唯一的一处"绕"）
// ---------------------------------------------------------------------------
// 生成的字库是 `const lv_font_t wd_font_32 = {...}` —— 落在 Flash 的 rodata 段，
// 写它必崩。而挂回退链就是要写 `font->fallback`。
// 所以启动时把三个结构体**拷到 RAM**（每个几十字节），在副本上挂 fallback。
//
// 拷外层结构体是安全的：LVGL 9 的 lv_font_fmt_txt_dsc_t 里已经没有字形缓存
// （生成的 .c 里那个 cache 只在 `#if LVGL_VERSION_MAJOR == 8` 分支里存在），
// 所以副本与原件共享全部只读数据，且不共享任何可变状态。
// 三档各用各的副本，也不存在同一份字体被两个档位复用的问题。
//
// 界面汉字的界面文案用字哪来的：主字体覆盖，这条链只补音标，不补汉字。
// 界面文案若出现主字体没有的字，是这个链解决不了的 —— 那属于字库生成阶段
// 该报错的事（见 tools/wordbook/gen_font.py 的覆盖检查）。
#pragma once

#include "lvgl.h"
#include "wd_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动时调一次（在 lv_init / bsp_lvgl_init 之后，任何绘制之前）。幂等。
void wd_font_init(void);

// 取某一档的字体对象，可直接交给 lv_obj_set_style_text_font()。
// tier 越界时按最小档返回 —— 与 wd_layout_metrics 的行为一致，不返回 NULL。
const lv_font_t *wd_font(wd_tier_t tier);

// 给 wd_layout_pick_tier 用的宽度探针：这个字符串在**这一档**下放得下一行吗。
//
// 为什么探针放在这里而不是 wd_layout 里：只有持有字体对象的那一层才知道
// 每个字形的真实 advance（NotoSansCJKsc 的 ASCII 是比例字宽 —— i 是 0.275em、
// m 是 0.926em，差 3 倍多）。wd_layout 表里的 units_per_line 是**折行**用的
// 保守预算（实测 P5），探针是**选字号**用的当下真实宽度，两者是独立的两个来源：
// 选档以探针为准，折行以表为准。
//
// ctx 不用传（传 NULL）。函数指针类型见 wd_fit_probe_t。
bool wd_font_fits(void *ctx, wd_tier_t tier, const char *text);

#ifdef __cplusplus
}  // extern "C"
#endif
