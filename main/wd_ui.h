// main/wd_ui.h —— 六屏的绘制层（触摸版）。
//
// ---------------------------------------------------------------------------
// 分层：这一层只管画，不管"该显示什么"
// ---------------------------------------------------------------------------
// wd_ui 收到的是**已经准备好的字符串与数值**，不做任何查找、不做状态机、
// 不碰词库与进度。所有"下一步该显示哪一屏"的判断都在 wd_app.c。
//
// 为什么这么切：LVGL 的那套对象生命周期只能在设备上验，而"该显示什么"是
// 纯逻辑，能在宿主机上跑断言。把两者混在一起，等于让最容易错的那部分
// 只能靠真机试 —— 这是本仓库前面六个模块一直避免的事。
//
// ---------------------------------------------------------------------------
// 触摸交互模型（与 ESP32-C3 三键版的差别）
// ---------------------------------------------------------------------------
// C3 版是"上/下/确定/双击/长按"五手势的焦点模型；这块屏是 480×480 触摸，
// 一切操作收敛为**点按**：
//   * 列表行直接点 —— 没有高亮光标，点哪行就是哪行
//   * 词条页底部常驻五个动作按钮（上一个/单词发音/读例句/翻面或记住了/
//     下一个），分两行摆；点卡片本身也翻面 —— 用户在别家项目上骂过
//     "连返回都没有"，这里每个动作都必须看得见
//   * 顶栏左侧常驻「‹返回」键（根页没有），右侧常驻「设置」芯片 ——
//     音量这类最常调的东西不该埋进二级页面
//   * 选书/进度页是 2x3 大卡片网格 + 底部两枚大按钮（字大了，列表行放不下）
//   * 设置值页有 −/＋ 步进按钮；配网页有「开始配网」按钮
//   *  destructive 操作（清空进度）保持两次点按确认
//
// UI → 应用的唯一出口是点按事件回调（wd_ui_set_evt_cb 注册），
// 应用 → UI 的入口是下面的 show_* / set_* 系列。事件在 LVGL 任务上下文
// 派发（BSP indev 自动分发），回调里调 show_* 是安全的（同线程递归锁）。
//
// ---------------------------------------------------------------------------
// 六屏（与 wd_app.c 的状态机一一对应）
// ---------------------------------------------------------------------------
//   ① 启动 / 配网页 —— 未联网、正在连接、词库坏了都走这一屏
//   ② 选书页（主页）—— 2x3 书卡 + 底部「开始测验」「学习进度」
//   ③ 词条页       —— 双面：A 释义 / B 例句，共用单词区与状态栏
//   ④ 测验页       —— 四选一
//   ⑤ 进度页       —— 2x3 书卡 + 底部「重置本册」「设置」
//   ⑥ 设置页       —— 音量 / 亮度 / 息屏 / 配网 / 关于
//
// ---------------------------------------------------------------------------
// 共享栏常驻，内容区每次切屏重建
// ---------------------------------------------------------------------------
// LVGL 用 clib malloc（sdkconfig CONFIG_LV_USE_CLIB_MALLOC），但六屏对象加起来
// 上百个，全建出来常驻仍会白占：标签文本每次 set_text 都可能 realloc。
// 所以分两层：状态栏（含返回键）与提示条在 wd_ui_init 里建一次，此后只改文字；
// 每屏内容区切屏时 lv_obj_clean 后重建 —— 常驻最多一屏的量。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "wd_layout.h"
#include "wd_manifest.h"
#include "wd_quiz.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 屏幕与页面
// ---------------------------------------------------------------------------

typedef enum {
    WD_SCREEN_BOOT = 0,   // ① 启动 / 配网 / 致命错误
    WD_SCREEN_BOOKS,      // ② 选书（主页）
    WD_SCREEN_WORD,       // ③ 词条（A/B 双面）
    WD_SCREEN_QUIZ,       // ④ 四选一
    WD_SCREEN_PROGRESS,   // ⑤ 进度
    WD_SCREEN_SETTINGS,   // ⑥ 设置
    WD_SCREEN_COUNT,
} wd_screen_t;

typedef enum {
    WD_SIDE_MEANING = 0,  // A 面：释义
    WD_SIDE_EXAMPLE,      // B 面：例句
    WD_SIDE_COUNT,
} wd_side_t;

// ---------------------------------------------------------------------------
// 触摸事件（UI → 应用）。arg 的含义按事件标注。
// ---------------------------------------------------------------------------
typedef enum {
    WD_UI_EVT_NONE = 0,
    // ① 启动 / 配网页：「重新配网」
    WD_UI_EVT_BOOT_CONFIG,
    // ② 选书页：arg = 行号 [0..count+1]（书 / 测验 / 进度 由应用分辨）
    WD_UI_EVT_BOOKS_TAP,
    // ③ 词条页：底部按钮与卡片翻面
    WD_UI_EVT_WORD_PREV,    // 上一个词
    WD_UI_EVT_WORD_SPEAK,   // 播放当前词发音
    WD_UI_EVT_WORD_SPEAK_SENTENCE,  // 朗读当前词的例句
    WD_UI_EVT_WORD_FLIP,    // 翻面（A↔B，卡片点按同此事件）
    WD_UI_EVT_WORD_MASTER,  // B 面「记住了」：标记掌握并跳到下一个未掌握
    WD_UI_EVT_WORD_NEXT,    // 下一个词
    // ④ 测验页：arg = 槽位 0..3；作答后点结果区要下一题
    WD_UI_EVT_QUIZ_PICK,
    WD_UI_EVT_QUIZ_NEXT,
    // ⑤ 进度页：arg = 行号 [0..count+1]（书 / 重置本册 / 设置）
    WD_UI_EVT_PROGRESS_TAP,
    // ⑥ 设置页
    WD_UI_EVT_SET_MENU,     // arg = 菜单行 0..4（音量/亮度/息屏/配网/关于）
    WD_UI_EVT_SET_STEP,     // arg = -1 / +1（音量 ±5；亮度/息屏 = 档位 ±1）
    WD_UI_EVT_SET_WIFI,     // 配网页「开始配网」
    // 顶栏右侧常驻「设置」芯片：从任意非根屏直达设置
    WD_UI_EVT_SETTINGS_OPEN,
    // 顶栏「‹返回」：回到哪一级由应用按当前模式分辨
    WD_UI_EVT_BACK,
} wd_ui_evt_t;

// 注册事件回调（通常在 wd_app_init 里把 wd_app 的入口接上来）。
// 回调在 LVGL 任务上下文执行；cb 为 NULL 时事件丢弃（初始化前点按）。
void wd_ui_set_evt_cb(void (*cb)(wd_ui_evt_t evt, int arg));

// ---------------------------------------------------------------------------
// 纵向预算 —— 每一屏都要能被这些常量证明放得下
// ---------------------------------------------------------------------------
// 屏幕 480×480。2026-10 的「全部增加 100% 大」之后，三档字号是 64/48/32，
// 行高预算 84/63/42。状态栏 48px 六屏共用；提示条只在**设置页**显示
// （启动页有自己的说明行，其余屏的按钮文字已经说明操作，不再占 48px 去重复）。
// 这些断言的存在意义：改任何一屏的分区高度，编译期就会告诉你别处还放不放得下。
// 行数一律按**预算行高**算，不是按字号 x 1.3 的浮点值。

// ③ 词条页与 ④ 测验页的纵向预算在 wd_layout.h（纯逻辑，宿主测试也要用）：
// WD_ZONE_GAP / WD_WORD_BAR_H / WD_WORD_BODY_H / WD_QUIZ_QUESTION_H /
// WD_QUIZ_CHOICE_H / WD_QUIZ_CHOICE_STEP。

// ① 启动 / 配网页
//
// 标题**只能**用 48px 档（MID），不能用 64px 档：wd_font_64 的字符集是
// ASCII + IPA + 界面标点，**一个汉字都没有** —— 拿它画「正在配网」只会满屏方框。
#define WD_BOOT_TITLE_H 63u     // 1 行 48px
#define WD_BOOT_DETAIL_H 126u   // 3 行 32px
#define WD_BOOT_AP_H 160u
#define WD_BOOT_BUTTON_H 48u
WD_STATIC_ASSERT(WD_ZONE_STATUS_H + WD_ZONE_GAP + WD_BOOT_TITLE_H + WD_ZONE_GAP +
                         WD_BOOT_DETAIL_H + WD_ZONE_GAP + WD_BOOT_AP_H +
                         WD_ZONE_GAP + WD_BOOT_BUTTON_H <= WD_SCREEN_H,
                 "启动页放不下 —— 48 + 4 + 63 + 4 + 126 + 4 + 160 + 4 + 48 = 461 ≤ 480");

// ② / ⑤ 选书与进度页：2 x 3 卡片网格 + 底部两枚大按钮（常量在 wd_layout.h）。
// 卡片里只放**简称**（两字，48px 宽 96px，放得进 212px 卡宽）；
// 全名六个汉字在 48px 下是 288px，塞不进半屏卡片 —— 简称本来就是为这一屏准备的。
#define WD_UI_CARDS (WD_GRID_COLS * WD_GRID_ROWS)   // 6
#define WD_UI_FOOTERS 2u
WD_STATIC_ASSERT(WD_ZONE_STATUS_H + WD_ZONE_GAP + WD_GRID_ROWS * WD_GRID_CARD_H +
                         (WD_GRID_ROWS - 1) * WD_GRID_GAP + WD_GRID_GAP +
                         WD_GRID_FOOTER_H <= WD_SCREEN_H,
                 "2x3 网格 + 底部按钮放不下 480px");

// ④ 测验页：题干/反馈区 + 4 个 66px 选项（步进 70）的预算同样在 wd_layout.h。

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

// 初始化三档字体与共享栏（状态栏 + 提示条）。在 bsp_display_start() 之后调一次。
//
// 返回 false 表示**字库不可用**：某个档连 ASCII 都取不到字形，或者 IPA 的
// 回退链没接上。前者意味着屏幕上会满屏方框，那比不启动更糟 —— 这是唯一
// 一个"宁可停下来报错"的 UI 级失败。
bool wd_ui_init(void);

// 切屏。同屏重复调用是廉价的（只改可见性）。返回键在需要它的屏上自动出现
// （BOOKS / BOOT 是根页，没有返回）。
//
// 注意：切回来时内容区是**重建**的，重建后会按上一次 show_* 存下的内容重画，
// 所以调用方不必在切屏后补一次 show_*。
void wd_ui_show(wd_screen_t screen);

// ---------------------------------------------------------------------------
// 状态栏与提示条（六屏共用，各自独立于 show_*）
// ---------------------------------------------------------------------------
// 两者都存在 wd_ui 里，切屏重建内容区时会自动重新应用 —— 调用方只管在
// 数值变化时调一次，不必关心当前在哪一屏。

typedef struct {
    const char *book_short;    // 「初中」；空串则不显示书名
    uint32_t mastered;         // 当前书已掌握词数
    uint32_t total;            // 当前书总词数
    bool connected;            // 是否已联网（Wi-Fi 指示条点亮）
} wd_ui_status_t;

void wd_ui_set_status(const wd_ui_status_t *status);

// 底部提示条。传 NULL 或空串则清空。**只有设置页显示** —— 字翻倍之后
// 480px 高度很紧，其余屏的操作提示已经写在按钮上，不再用 48px 去重复。
// 提示条宽 432px、32px 下一行约 13 个汉字。
void wd_ui_set_hint(const char *text);

// ---------------------------------------------------------------------------
// ① 启动 / 配网页
// ---------------------------------------------------------------------------
// title 大字（「正在配网」/「词库不可用」）
// detail 小字（「手机连上热点后打开下面的网址」/ 词库错误原因）
// ap_ssid 为空则不显示配网区块；非空时显示该 SSID 与 192.168.4.1。
// 「重新配网」按钮常驻本屏（ap_ssid 非空时点它会重开热点，等价于重配）。
//
// 配网区块的存在与否会改变整页的垂直分配（有 AP 信息时四块、没有时三块），
// 所以 ap_ssid 的有无发生变化时内容区会重建一次。正常路径上它只变一次。
void wd_ui_show_boot(const char *title, const char *detail, const char *ap_ssid);

// ---------------------------------------------------------------------------
// ② 选书页（主页）
// ---------------------------------------------------------------------------
// books/mastered_per_book 至少 count 项。2x3 卡片可点，点哪张发
// WD_UI_EVT_BOOKS_TAP（arg = 行号）：0 .. count-1 是书；底部两枚按钮发
// count（「开始测验」）与 count + 1（「学习进度」）。行号含义与 wd_app.c 对齐。
void wd_ui_show_books(const wd_book_t *books, const uint32_t *mastered_per_book,
                      unsigned count);

// ---------------------------------------------------------------------------
// ③ 词条页
// ---------------------------------------------------------------------------
// 所有字符串都可为 NULL / 空串：
//   * ipa 缺失是**常态**（1000+ 条词源里就没有音标）—— 空则整行隐藏
//   * sentence_* 缺失同理（485 个词没有例句）
// audio_note 非空时显示在音标行右侧（「加载」「播放」「失败」）。
//
// 单词字号是现场择优的：64px 起，整行放不下才降 48 / 32（wd_layout_pick_tier）。
// 释义与例句**固定 48px**，正文区可上下拖动滚动 —— 字翻倍之后，
// 再靠缩到 32px 硬塞等于把这次改动又退回去。
//
// 底部动作条五个按钮（两行）：
//   第一行：‹上一个 / 发音 / 读例句
//   第二行：（A 面「看例句」、B 面「记住了」）/ 下一个›
// 中央大按钮随 side 换文案与事件（FLIP ↔ MASTER）；卡片本身点按也发 FLIP。
void wd_ui_show_word(const char *word, const char *ipa, const char *meaning,
                     const char *sentence_en, const char *sentence_cn,
                     wd_side_t side, const char *audio_note);

// ---------------------------------------------------------------------------
// ④ 测验页
// ---------------------------------------------------------------------------
// **题干是中文释义，四个选项是英文单词** —— 这个方向不是审美选择，是
// wd_quiz.c 的实现决定的（干扰项容差按英文词长 ±2、排除"释义与答案相同"的词；
// 两条规则都只在"释义当题干、单词当选项"下有意义）。
//
// answer_word / answer_ipa 只在**作答之后**显示在反馈区（两行），UI 层靠
// quiz->phase / quiz->picked 判断，调用方不需要自己清空它们。
//
// 未作答时点选项发 WD_UI_EVT_QUIZ_PICK（arg = 槽位）；已作答后点反馈区发
// WD_UI_EVT_QUIZ_NEXT。quiz 为 NULL 或处于 IDLE 时按"题目不可用"渲染
// （选项灰掉、不可点）—— 这是防"漏检出题失败"的最后一关。
void wd_ui_show_quiz(const char *meaning, const char *answer_word,
                     const char *answer_ipa, const char *const *choices,
                     const wd_quiz_t *quiz);

// ---------------------------------------------------------------------------
// ⑤ 进度页
// ---------------------------------------------------------------------------
// 与选书页共用行高，每行多一条掌握度进度条。每行可点，发
// WD_UI_EVT_PROGRESS_TAP（arg = 行号）：0 .. count-1 进书学习，
// count 是「重置本册」（清当前在学的书，**点两次确认**，第一次点后 UI 把这行
// 标红等待第二次；armed 由调用方传进来），count + 1 是「设置」。
void wd_ui_show_progress(const wd_book_t *books, const uint32_t *mastered,
                         unsigned count, bool reset_armed);

// ---------------------------------------------------------------------------
// ⑥ 设置页
// ---------------------------------------------------------------------------

typedef enum {
    WD_SETTINGS_MENU = 0,
    WD_SETTINGS_VOLUME,
    WD_SETTINGS_BRIGHTNESS,
    WD_SETTINGS_TIMEOUT,
    WD_SETTINGS_WIFI,
    WD_SETTINGS_ABOUT,
    WD_SETTINGS_PAGE_COUNT,
} wd_settings_page_t;

// 设置菜单第 row 行对应哪一页（row 从 0 开始）。返回 WD_SETTINGS_PAGE_COUNT
// 表示那一行是「返回」。菜单项的文字与顺序都在 wd_ui.c 里（那是界面文案），
// "点第几行该做什么"是应用逻辑（在 wd_app.c）。两者靠**枚举顺序**对齐，所以这里
// 给一条唯一的换算函数，并在 wd_ui.c 里用 _Static_assert 把枚举顺序钉死。
static inline wd_settings_page_t wd_settings_page_of_row(unsigned row) {
    return (wd_settings_page_t)(row + 1u);
}

// 传**已解析的值**而不是档位下标：这一层是 .c（C 编译），而档位表的唯一来源在
// wd_display.cc。传值之后各处职责唯一：wd_display 持有档位表，wd_app 读数，
// wd_ui 只管把数字排成字（「息屏 0 -> 关闭 / 30 -> 30 秒」属于排版，留在这一层）。
//
// volume_percent / brightness_percent: 0..100；timeout_seconds: 0 = 不自动息屏。
// 值页（音量/亮度/息屏）画 −/＋ 两个步进按钮发 WD_UI_EVT_SET_STEP；
// 配网页画「开始配网」按钮发 WD_UI_EVT_SET_WIFI；返回一律走顶栏「‹返回」。
void wd_ui_show_settings(wd_settings_page_t page, uint8_t volume_percent,
                         uint8_t brightness_percent, uint16_t timeout_seconds);

#ifdef __cplusplus
}  // extern "C"
#endif
