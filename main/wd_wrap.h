// main/wd_wrap.h —— 中英混排折行（纯逻辑，可在宿主机编译）。
//
// 为什么不能交给 LVGL 自己折：
//   ① LVGL 按像素断行，会在任何地方断 —— 中文标点会被甩到行首（"，"、"。" 开头的行），
//      这在中文排版里是硬错误，读起来非常刺眼；
//   ② 英文会被从单词中间截断（"transpor / tation"）；
//   ③ 后果无法预期：同一段文字在不同字号下断的位置不同，版面校验（本仓库用
//      _Static_assert 钉住的那些预算）就失去了意义。
//
// 所以折行在这里算好，交给 LVGL 的每一行都是"已经决定好的、不需要再折的"文本。
//
// **宽度单位是半角单位（half-width unit）**，不是码点数，也不是像素：
//   * ASCII 与拉丁扩展、IPA 各算 1
//   * 汉字与全角标点各算 2
// 于是"一行 23 units"同时意味着「23 个英文字母」和「11~12 个汉字」——
// 而这两个数字正是 tools/wordbook/content.py 里定字段上限时用的那两个
// （CAP_SENT_EN=104 = 26×4 行，CAP_SENT_CN=39 = 13×3 行）。
// 两边用同一套算术，上限和折行结果才不会互相打架。
//
// 这是**预算**而不是精确排版：NotoSansCJKsc 的 ASCII 是比例字宽
// （i 是 0.275em，m 是 0.926em），同样 26 个字符的实际像素宽度能差一倍多。
// 精确宽度必须由持有字体的那一层来量（UI 层用 lv_txt_get_width）。这里保证的是
// **行数上限**和**标点位置**，这两件事不需要像素精度，而且必须在没有字体时也能测。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 一屏最多排 16 行。字号翻倍后每行只剩 17 个半角单位（48px 档），
// 长释义 64 个汉字（128 units）就要 8 行；例句英 + 中合计再留一倍余量。
// 正文区本身可滚动，这个上限只负责「别让一个损坏字段吃光内存」。
#define WD_WRAP_MAX_LINES 16
#define WD_WRAP_MAX_TEXT 512   // 超过这个长度按损坏处理，只折前 512 字节

typedef struct {
    uint16_t start[WD_WRAP_MAX_LINES];  // 该行在原文里的字节偏移
    uint16_t len[WD_WRAP_MAX_LINES];    // 该行的字节长度（不含换行，已去尾空格）
    unsigned count;                     // 实际行数
    bool truncated;                     // 行数超上限，尾部被丢弃 —— UI 要显示省略号
} wd_wrap_t;

// 一段文字的**总宽度**（半角单位）。用于判断"要不要折"和"能不能放下"。
unsigned wd_wrap_units(const char *text);

// 按每行最多 units_per_line 个半角单位折行。
// units_per_line == 0 时按 26（16px 正文档的宽度）处理，绝不返回 0 行。
wd_wrap_t wd_wrap_text(const char *text, unsigned units_per_line);

// 一个码点的显示宽度（半角单位）。导出是为了让宿主测试能逐条对撞 —
// 宽度表算错会让所有版式预算失效，而这一条最容易悄悄跑偏。
unsigned wd_wrap_unit_of(uint32_t codepoint);

// 该码点是否禁止出现在**行首**（中文禁则）。导出原因同上：禁则表要可测。
bool wd_wrap_no_line_start(uint32_t codepoint);

// 该码点是否禁止出现在**行尾**。
bool wd_wrap_no_line_end(uint32_t codepoint);

#ifdef __cplusplus
}  // extern "C"
#endif
