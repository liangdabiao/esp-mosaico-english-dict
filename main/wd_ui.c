// main/wd_ui.c —— 六屏的绘制实现（ESP-Mosaico 触摸版，480×480）。
//
// 分层与职责见 wd_ui.h。相对 C3 三键原版的两点实现约定：
//
// 1. 文字一律先自己折行（wd_wrap），再交给 LVGL —— 原版的三条理由不变
//    （LVGL 按像素断行会在中文标点前断、从单词中间截英文、不同字号断点不同）。
//    折行缓冲按 UTF-8 边界截短、以「…」收尾。
//
// 2. 触摸交互由本层把 LV_EVENT_CLICKED 翻译成 wd_ui_evt_t 事件上抛给 wd_app：
//    卡片网格直接可点（没有焦点游标），词条卡片点按 = 翻面，底部五个动作按钮，
//    设置值页有大号 −/＋ 按钮，非根屏状态栏有「‹返回」热区，右侧常驻「设置」。
//    回调在 LVGL 任务上下文执行 —— bsp_display_lock 是递归锁，
//    回调里再调 show_* 是安全的（qiaopi 已验证的用法）。
//
// 2026-10「全部增加 100% 大」后的版式要点：
//   * 字号 64/48/32，行高 84/63/42；所有中文用 48 或 32 档（64 档没有汉字）；
//     英文例句用 32 档（比单词和中文译文小一号）
//   * 词条正文是**滚动区**：48px 下放不下就上下拖，不再缩到 32px 硬塞
//   * 选书/进度改成 2x3 大卡片 + 底部两枚 64px 高的大按钮（8 行列表放不下了）
//   * 提示条只在设置页显示，其余屏的操作提示已经写在按钮上
#include "wd_ui.h"

#include "bsp/display.h"   // bsp_display_lock / bsp_display_unlock
#include "esp_log.h"
#include "wd_font.h"
#include "wd_wrap.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 配色：沿用基座 english-podcast 的暗色系（CO5300 深底浅字对比度实测更好）
// ---------------------------------------------------------------------------
#define WD_C_BACKGROUND 0x071017u
#define WD_C_PANEL 0x0D1B23u
#define WD_C_CHIP 0x17303Au     // 按钮/芯片比面板亮一档，大字下边界更清楚
#define WD_C_GRID 0x24404Au
#define WD_C_TEXT 0xF4F0DEu
#define WD_C_MUTED 0x849BA0u
#define WD_C_AMBER 0xFFB74Du
#define WD_C_GREEN 0x4ED39Au
#define WD_C_RED 0xFF5D62u
#define WD_C_DARK_TEXT 0x071017u

// ---------------------------------------------------------------------------
// 锁：LVGL 对象只能在持有 BSP 显示锁时碰
// ---------------------------------------------------------------------------
static bool ui_lock(void) {
    if (bsp_display_lock(500)) return true;
    ESP_LOGW("wd_ui", "拿不到显示锁（500ms）—— 这次界面更新被丢弃");
    return false;
}

static void ui_unlock(void) { bsp_display_unlock(); }

// ---------------------------------------------------------------------------
// 局部版面常量（480×480 屏，内容区 x=24..456 宽 432）
// ---------------------------------------------------------------------------
// 与区块预算（wd_ui.h 的 _Static_assert）不同：这里是一个区块**内部**怎么摆。

// 共享栏（状态栏 48px）
#define WD_BACK_CHIP_X 8
#define WD_BACK_CHIP_Y 2
#define WD_BACK_CHIP_W 100
#define WD_BACK_CHIP_H 44
#define WD_STATUS_BOOK_X_BACK 116
#define WD_STATUS_BOOK_X_ROOT 24
#define WD_STATUS_BOOK_W 66        // 两个汉字 x 32px = 64
#define WD_STATUS_COUNT_GAP 6
#define WD_STATUS_COUNT_W 162      // 「4533/4533」9 个半角 x ~17.6px ≈ 158
#define WD_WIFI_BAR_X 352
#define WD_WIFI_BAR_GAP 7
#define WD_WIFI_BAR_COUNT 3
#define WD_SET_CHIP_X 380
#define WD_SET_CHIP_Y 2
#define WD_SET_CHIP_W 92
#define WD_SET_CHIP_H 44
#define WD_STATUS_TEXT_Y 3       // 32px 行高 42 在 48px 里居中
#define WD_HINT_Y (WD_SCREEN_H - WD_ZONE_HINT_H)   // 432

// 网格卡片内部（选书/进度共用）
#define WD_CARD_NAME_Y 0
#define WD_CARD_COUNT_Y 62
#define WD_CARD_BAR_X 16
#define WD_CARD_BAR_Y 104
#define WD_CARD_BAR_W (WD_GRID_CARD_W - 2 * WD_CARD_BAR_X)   // 180
#define WD_CARD_BAR_H 6
#define WD_GRID_TOP (WD_ZONE_STATUS_H + WD_ZONE_GAP)         // 52
#define WD_GRID_FOOTER_Y (WD_GRID_TOP + WD_GRID_ROWS * WD_GRID_CARD_H + \
                          (WD_GRID_ROWS - 1) * WD_GRID_GAP + WD_GRID_GAP)   // 412

// 词条页
#define WD_WORD_TOP WD_ZONE_STATUS_H               // 48
#define WD_IPA_Y (WD_WORD_TOP + WD_ZONE_WORD_H)    // 144
#define WD_DIVIDER_Y (WD_IPA_Y + WD_ZONE_IPA_H)    // 192
#define WD_WORD_BODY_TOP (WD_DIVIDER_Y + WD_ZONE_DIVIDER_H)   // 194
#define WD_BAR_Y (WD_WORD_BODY_TOP + WD_WORD_BODY_H)          // 344
#define WD_BAR_ROW_H 64
#define WD_BAR_ROW2_Y (WD_BAR_Y + WD_BAR_ROW_H + 8)           // 416
#define WD_BAR_SMALL_W 140
#define WD_BAR_SMALL_GAP 6
#define WD_BAR_WIDE_W ((WD_BODY_W - WD_ZONE_GAP) / 2)         // 214
#define WD_SCROLLBAR_W 6

// 测验页
#define WD_QUIZ_TOP WD_ZONE_STATUS_H               // 48
#define WD_QUIZ_OPT_TOP (WD_QUIZ_TOP + WD_QUIZ_QUESTION_H + 2)   // 190
#define WD_QUIZ_OPT_X WD_SAFE_X
#define WD_QUIZ_OPT_W WD_BODY_W
#define WD_QUIZ_OPT_LABEL_PAD 24

// 设置菜单
#define WD_SET_MENU_TOP 52
#define WD_SET_MENU_ROW_H 70
#define WD_SET_MENU_ROW_GAP 4
#define WD_SETTINGS_MENU_ROWS 5u

// 设置值页
#define WD_SET_TITLE_Y 64
#define WD_SET_TITLE_H 63
#define WD_SET_VALUE_Y 136
#define WD_SET_VALUE_H 84
#define WD_SET_BAR_X 64
#define WD_SET_BAR_Y 244
#define WD_SET_BAR_W (WD_BODY_W - 2 * (WD_SET_BAR_X - WD_SAFE_X))   // 352
#define WD_SET_BAR_H 16
#define WD_SET_STEP_Y 288
#define WD_SET_STEP_W 96
#define WD_SET_STEP_H 96

// 设置配网/关于页
#define WD_SET_TEXT_TITLE_Y 64
#define WD_SET_WIFI_BODY_Y 140
#define WD_SET_WIFI_BODY_H 168
#define WD_SET_WIFI_BTN_Y 330
#define WD_SET_WIFI_BTN_H 92
#define WD_ABOUT_TITLE_Y 64
#define WD_ABOUT_NAME_Y 136
#define WD_ABOUT_SUB_Y 204
#define WD_ABOUT_ROW_TOP 256
#define WD_ABOUT_ROW_STEP 44
#define WD_ABOUT_VER_Y 388

// 启动页
#define WD_BOOT_TITLE_Y 56
#define WD_BOOT_DETAIL_Y 128
#define WD_BOOT_AP_BOX_Y 264
#define WD_BOOT_BTN_Y 428
#define WD_BOOT_BTN_X WD_SAFE_X
#define WD_BOOT_BTN_W WD_BODY_W
#define WD_BOOT_TITLE_Y_PLAIN 110
#define WD_BOOT_DETAIL_Y_PLAIN 186
#define WD_BOOT_BTN_Y_PLAIN 356
#define WD_BOOT_BTN_H_PLAIN 64

// 单行文本缓冲上限（沿用原版的容量推导）
#define WD_UI_WORD_CAP 32
#define WD_UI_IPA_CAP 32
#define WD_UI_MEANING_CAP 208
#define WD_UI_SENT_EN_CAP 112
#define WD_UI_SENT_CN_CAP 128
#define WD_UI_NOTE_CAP 24
#define WD_UI_HINT_CAP 48
#define WD_UI_TITLE_CAP 32

// 省略号必须以字面量出现在 main/ 源码里，才能被 gen_font.py 的界面文案扫描
// 扫进字库（扫描器按字符串字面量的字符迭代，"\xE2\x80\xA6" 这种转义会漏）。
static const char kEllipsis[] = "…";

// 设置菜单固定项。顺序必须与 wd_settings_page_t 一致（wd_settings_page_of_row
// 直接拿行号 + 1 当枚举值），少改一处就是"选了亮度进了配网"那种编译期拦不住的错。
// 字翻倍后一屏只放得下 5 项；「返回」由顶栏的 ‹返回 承担，不再占一行。
static const char *const kSettingsMenuItems[WD_SETTINGS_MENU_ROWS] = {
    "音量", "亮度", "息屏", "配网", "关于"};

WD_STATIC_ASSERT(WD_SETTINGS_MENU_ROWS <= WD_UI_CARDS,
                 "设置菜单项数超过网格能显示的数量");
WD_STATIC_ASSERT(WD_SETTINGS_VOLUME == 1 && WD_SETTINGS_BRIGHTNESS == 2 &&
                     WD_SETTINGS_TIMEOUT == 3 && WD_SETTINGS_WIFI == 4 &&
                     WD_SETTINGS_ABOUT == 5,
                 "菜单项顺序与 wd_settings_page_t 的顺序绑定了");

// ---------------------------------------------------------------------------
// 对象与状态
// ---------------------------------------------------------------------------

static lv_obj_t *s_root;        // 自建 screen（深色铺底）
static lv_obj_t *s_chrome;      // 共享栏容器（常驻，内容区之上）
static lv_obj_t *s_content;     // 内容区容器（切屏时清空重建）
static wd_screen_t s_current = WD_SCREEN_COUNT;

static void (*s_evt_cb)(wd_ui_evt_t evt, int arg) = NULL;

// 共享栏
static lv_obj_t *s_back_chip;    // 非根屏的「‹返回」热区
static lv_obj_t *s_settings_chip;  // 右侧「设置」芯片（启动/设置页隐藏）
static lv_obj_t *s_status_book;
static lv_obj_t *s_status_count;
static lv_obj_t *s_wifi_bar[WD_WIFI_BAR_COUNT];
static lv_obj_t *s_hint;

static char s_status_book_text[WD_BOOK_SHORT_MAX];
static uint32_t s_status_mastered;
static uint32_t s_status_total;
static bool s_status_connected;
static char s_hint_text[WD_UI_HINT_CAP];

// 启动页
static lv_obj_t *s_boot_title;
static lv_obj_t *s_boot_detail;
static lv_obj_t *s_boot_ap_ssid;
static lv_obj_t *s_boot_btn;
static char s_boot_title_text[WD_UI_TITLE_CAP];
static char s_boot_detail_text[WD_UI_MEANING_CAP];
static char s_boot_ap_text[WD_UI_WORD_CAP];
static bool s_boot_has_ap;

// 选书 / 进度页（2x3 卡片 + 底部两枚按钮）
static lv_obj_t *s_card_box[WD_UI_CARDS];
static lv_obj_t *s_card_track[WD_UI_CARDS];
static lv_obj_t *s_card_fill[WD_UI_CARDS];
static lv_obj_t *s_card_name[WD_UI_CARDS];
static lv_obj_t *s_card_count[WD_UI_CARDS];
static lv_obj_t *s_footer_btn[WD_UI_FOOTERS];
static lv_obj_t *s_footer_label[WD_UI_FOOTERS];

// 列表页的缓存（rebuild_content 用）。wd_ui_show_books/show_progress 拷贝完
// 数据后如果当前不在该屏，就只更新缓存不重建 —— 切回来时再按缓存重画。
static wd_book_t s_books_cache[WD_MAX_BOOKS];
static uint32_t s_mastered_cache[WD_MAX_BOOKS];
static unsigned s_book_count_cache;
static bool s_reset_armed_cache;

// 词条页
static lv_obj_t *s_word_label;
static lv_obj_t *s_ipa_label;
static lv_obj_t *s_note_label;
static lv_obj_t *s_card_area;      // 滚动正文区；点按 = 翻面
static lv_obj_t *s_meaning_label;
static lv_obj_t *s_sent_en_label;
static lv_obj_t *s_sent_cn_label;
static lv_obj_t *s_bar_btn[5];     // [‹上一个][发音][读例句][看例句|记住了][下一个›]
static lv_obj_t *s_bar_label[5];
static char s_word_text[WD_UI_WORD_CAP];
static char s_ipa_text[WD_UI_IPA_CAP];
static char s_meaning_text[WD_UI_MEANING_CAP];
static char s_sent_en_text[WD_UI_SENT_EN_CAP];
static char s_sent_cn_text[WD_UI_SENT_CN_CAP];
static char s_note_text[WD_UI_NOTE_CAP];
static wd_side_t s_side = WD_SIDE_MEANING;

// 测验页
static lv_obj_t *s_quiz_qbox;      // 题干/反馈区，作答后点它 = 下一题
static lv_obj_t *s_quiz_question;
static lv_obj_t *s_quiz_fb1;
static lv_obj_t *s_quiz_fb2;
static lv_obj_t *s_quiz_opt_box[WD_QUIZ_CHOICES];
static lv_obj_t *s_quiz_opt_label[WD_QUIZ_CHOICES];
static char s_quiz_meaning[WD_UI_MEANING_CAP];
static char s_quiz_answer_word[WD_UI_WORD_CAP];
static char s_quiz_answer_ipa[WD_UI_IPA_CAP];
static char s_quiz_choice[WD_QUIZ_CHOICES][WD_UI_WORD_CAP];
static wd_quiz_t s_quiz;
static bool s_quiz_has_state;      // s_quiz 是否是一道真出题（vs 清零的 IDLE）

// 设置页（四种形态：菜单 / 值页 / 配网文本页 / 关于页）
typedef enum {
    WD_SET_SHAPE_NONE = 0,
    WD_SET_SHAPE_MENU,
    WD_SET_SHAPE_VALUE,
    WD_SET_SHAPE_WIFI,
    WD_SET_SHAPE_ABOUT,
} wd_set_shape_t;

static wd_set_shape_t s_set_shape = WD_SET_SHAPE_NONE;
static wd_settings_page_t s_set_page = WD_SETTINGS_MENU;
static uint8_t s_set_volume;
static uint8_t s_set_brightness;
static uint16_t s_set_timeout;

static lv_obj_t *s_set_title;
static lv_obj_t *s_set_value;
static lv_obj_t *s_set_bar_track;
static lv_obj_t *s_set_bar_fill;
static lv_obj_t *s_set_step_minus;
static lv_obj_t *s_set_step_plus;
static lv_obj_t *s_set_wifi_btn;
static lv_obj_t *s_set_menu_row[WD_SETTINGS_MENU_ROWS];
static lv_obj_t *s_set_menu_label[WD_SETTINGS_MENU_ROWS];

// 真实行高（从字库读；预算值在 wd_layout.h，两者必须在 init 里核一次）
static uint8_t s_line_h[WD_TIER_COUNT];

// 折行缓冲（LVGL 的 set_text 会拷走内容，同屏内连续复用安全）。
// 16 行会给文本额外插 15 个 '\n'，所以比 WD_WRAP_MAX_TEXT 多留一截。
static char s_fold[WD_WRAP_MAX_TEXT + WD_WRAP_MAX_LINES + 8];

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u) --n;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static lv_obj_t *mk_box(lv_obj_t *parent, int x, int y, int w, int h,
                        uint32_t color, int radius) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    // LVGL 9 的 lv_obj 默认可点：装饰盒子若留在命中链上，会替下层按钮吃掉点击
    // （全屏容器尤其致命）。需要点击的盒子在创建后自行加回 CLICKABLE。
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

// 事件载荷打包：evt 放低 16 位，arg 以**有符号 16 位**存高 16 位 ——
// SET_STEP 的 arg 是 -1，按无符号拆出来会得到 65535。
static void set_hot_data(lv_obj_t *obj, wd_ui_evt_t evt, int arg) {
    lv_obj_set_user_data(
        obj, (void *)(uintptr_t)((unsigned)evt |
                                 ((unsigned)(int16_t)arg << 16)));
}

static void on_hot_clicked(lv_event_t *e) {
    if (!s_evt_cb) return;
    const uintptr_t packed = (uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    const wd_ui_evt_t evt = (wd_ui_evt_t)(packed & 0xFFFFu);
    const int arg = (int)(int16_t)(packed >> 16);
    // 触摸链路唯一的观测点：LVGL 无指针注入/截屏，验收只能靠这条日志。
    ESP_LOGI("wd_ui", "tap evt=%u arg=%d", (unsigned)evt, arg);
    s_evt_cb(evt, arg);
}

// 透明可点热区：不画任何东西，只把 CLICKED 事件翻译成一个 ui 事件。
static lv_obj_t *mk_hot_area(lv_obj_t *parent, int x, int y, int w, int h,
                             wd_ui_evt_t evt, int arg) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    set_hot_data(obj, evt, arg);
    lv_obj_add_event_cb(obj, on_hot_clicked, LV_EVENT_CLICKED, NULL);
    return obj;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, int x, int y,
                          int w, int h, uint32_t color, const lv_font_t *font,
                          lv_text_align_t align, lv_label_long_mode_t mode) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_label_set_text(obj, text ? text : "");
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, align, 0);
    lv_label_set_long_mode(obj, mode);
    return obj;
}

static void set_text_or_empty(lv_obj_t *obj, const char *text) {
    if (obj) lv_label_set_text(obj, text ? text : "");
}

static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (!obj) return;
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_text_color(lv_obj_t *obj, uint32_t color) {
    if (obj) lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

// 折行到不超过 max_lines 行，行间插 '\n'；被丢弃的末行以「…」收尾。
static const char *fold_text(const char *text, unsigned units_per_line,
                             unsigned max_lines) {
    s_fold[0] = '\0';
    if (!text || !*text || max_lines == 0) return s_fold;

    const wd_wrap_t wrap = wd_wrap_text(text, units_per_line);
    unsigned lines = wrap.count;
    bool cut = wrap.truncated;
    if (lines > max_lines) {
        lines = max_lines;
        cut = true;
    }

    const size_t ell = strlen(kEllipsis);
    size_t out = 0;
    for (unsigned i = 0; i < lines; ++i) {
        if (i) {
            if (out + 1 >= sizeof(s_fold)) break;
            s_fold[out++] = '\n';
        }
        const char *src = text + wrap.start[i];
        size_t len = wrap.len[i];
        const size_t reserve = (cut && i + 1 == lines) ? ell : 0;
        if (out + len + reserve + 1 > sizeof(s_fold)) {
            const size_t room = sizeof(s_fold) - 1 - out - reserve;
            len = (len > room) ? room : len;
            while (len > 0 && ((unsigned char)src[len] & 0xC0u) == 0x80u) --len;
        }
        memcpy(s_fold + out, src, len);
        out += len;
        if (reserve) {
            memcpy(s_fold + out, kEllipsis, ell);
            out += ell;
        }
    }
    s_fold[out] = '\0';
    return s_fold;
}

static unsigned text_lines(const char *text) {
    if (!text || !*text) return 0;
    unsigned lines = 1;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') ++lines;
    }
    return lines;
}

// 这段文字在**这一档**下折行后放得进 height 像素吗（用与折行同一套算术）。
static bool fold_fits(const char *text, wd_tier_t tier, unsigned height) {
    if (!text || !*text) return true;
    const uint8_t line_h = s_line_h[tier];
    if (!line_h) return false;
    const unsigned max_lines = height / line_h;
    if (!max_lines) return false;
    const wd_wrap_t wrap =
        wd_wrap_text(text, wd_layout_metrics(tier)->units_per_line);
    return !wrap.truncated && wrap.count <= max_lines;
}

// 纵向居中的 y 偏移（真实行高）。
static int centered_y(int top, int height, wd_tier_t tier) {
    const int h = (int)s_line_h[tier];
    const int off = (h < height) ? (height - h) / 2 : 0;
    return top + off;
}

// 选项/卡片这类"一行必须放下"的文字：只在 48 与 32 之间选。
// 不用 wd_layout_pick_tier：它可能返回 64px 档，而 64px 行高 84 塞不进
// 66px 的选项盒；64px 字库也没有汉字。
static wd_tier_t pick_mid_or_small(const char *text) {
    if (!text || !text[0]) return WD_TIER_MID;
    return wd_font_fits(NULL, WD_TIER_MID, text) ? WD_TIER_MID : WD_TIER_SMALL;
}

// ---------------------------------------------------------------------------
// 共享栏
// ---------------------------------------------------------------------------

// 哪些屏是"根"：根屏不显示返回热区。
static bool screen_is_root(wd_screen_t s) {
    return s == WD_SCREEN_BOOT || s == WD_SCREEN_BOOKS;
}

// 「设置」芯片出现在除启动页以外的非设置页：从任意一屏都能一步进音量。
static bool screen_shows_settings(wd_screen_t s) {
    return s == WD_SCREEN_BOOKS || s == WD_SCREEN_WORD || s == WD_SCREEN_QUIZ ||
           s == WD_SCREEN_PROGRESS;
}

static void apply_status(void) {
    if (s_status_book) lv_label_set_text(s_status_book, s_status_book_text);
    if (s_status_count) {
        char text[28];
        if (s_status_total) {
            snprintf(text, sizeof(text), "%u/%u", (unsigned)s_status_mastered,
                     (unsigned)s_status_total);
        } else {
            text[0] = '\0';
        }
        lv_label_set_text(s_status_count, text);
    }
    for (int i = 0; i < WD_WIFI_BAR_COUNT; ++i) {
        if (!s_wifi_bar[i]) continue;
        lv_obj_set_style_bg_color(s_wifi_bar[i],
                                  lv_color_hex(s_status_connected ? WD_C_GREEN : WD_C_GRID), 0);
    }
}

static void apply_hint(void) { set_text_or_empty(s_hint, s_hint_text); }

// 共享栏随屏幕变化的可见性/位置。每次 s_current 变了都要调。
static void apply_chrome(void) {
    const bool back = !screen_is_root(s_current);
    set_hidden(s_back_chip, !back);
    set_hidden(s_settings_chip, !screen_shows_settings(s_current));
    set_hidden(s_hint, s_current != WD_SCREEN_SETTINGS);

    // 有返回键时书名让开 108px；根屏从安全区直接起排。
    const int book_x = back ? WD_STATUS_BOOK_X_BACK : WD_STATUS_BOOK_X_ROOT;
    if (s_status_book) lv_obj_set_x(s_status_book, book_x);
    if (s_status_count) lv_obj_set_x(s_status_count, book_x + WD_STATUS_BOOK_W + WD_STATUS_COUNT_GAP);
}

static void build_chrome(lv_obj_t *parent) {
    lv_obj_t *status = mk_box(parent, 0, 0, WD_SCREEN_W, WD_ZONE_STATUS_H,
                              WD_C_PANEL, 0);
    s_back_chip = mk_hot_area(status, WD_BACK_CHIP_X, WD_BACK_CHIP_Y,
                              WD_BACK_CHIP_W, WD_BACK_CHIP_H, WD_UI_EVT_BACK, 0);
    lv_obj_set_style_bg_color(s_back_chip, lv_color_hex(WD_C_CHIP), 0);
    lv_obj_set_style_bg_opa(s_back_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_back_chip, 8, 0);
    mk_label(s_back_chip, "‹返回", 0, (WD_BACK_CHIP_H - s_line_h[WD_TIER_SMALL]) / 2,
             WD_BACK_CHIP_W, s_line_h[WD_TIER_SMALL], WD_C_AMBER,
             wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);

    s_status_book = mk_label(status, "", WD_STATUS_BOOK_X_ROOT, WD_STATUS_TEXT_Y,
                             WD_STATUS_BOOK_W, s_line_h[WD_TIER_SMALL], WD_C_MUTED,
                             wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_LEFT,
                             LV_LABEL_LONG_MODE_DOTS);
    s_status_count = mk_label(status, "", WD_STATUS_BOOK_X_ROOT + WD_STATUS_BOOK_W + WD_STATUS_COUNT_GAP,
                              WD_STATUS_TEXT_Y, WD_STATUS_COUNT_W,
                              s_line_h[WD_TIER_SMALL], WD_C_MUTED,
                              wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_LEFT,
                              LV_LABEL_LONG_MODE_DOTS);
    for (int i = 0; i < WD_WIFI_BAR_COUNT; ++i) {
        const int bar_h = 9 + i * 6;
        s_wifi_bar[i] = mk_box(status, WD_WIFI_BAR_X + i * WD_WIFI_BAR_GAP,
                               WD_ZONE_STATUS_H - 6 - bar_h, 5, bar_h, WD_C_GRID, 1);
    }
    s_settings_chip = mk_hot_area(status, WD_SET_CHIP_X, WD_SET_CHIP_Y,
                                  WD_SET_CHIP_W, WD_SET_CHIP_H,
                                  WD_UI_EVT_SETTINGS_OPEN, 0);
    lv_obj_set_style_bg_color(s_settings_chip, lv_color_hex(WD_C_CHIP), 0);
    lv_obj_set_style_bg_opa(s_settings_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_settings_chip, 8, 0);
    mk_label(s_settings_chip, "设置", 0,
             (WD_SET_CHIP_H - s_line_h[WD_TIER_SMALL]) / 2, WD_SET_CHIP_W,
             s_line_h[WD_TIER_SMALL], WD_C_AMBER, wd_font(WD_TIER_SMALL),
             LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);

    s_hint = mk_label(parent, "", WD_SAFE_X, WD_HINT_Y + (WD_ZONE_HINT_H - 42) / 2,
                      WD_BODY_W, 42, WD_C_MUTED, wd_font(WD_TIER_SMALL),
                      LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
    apply_status();
    apply_hint();
    apply_chrome();
}

// ---------------------------------------------------------------------------
// ① 启动 / 配网页
// ---------------------------------------------------------------------------

static void build_boot(void) {
    if (s_boot_has_ap) {
        s_boot_title = mk_label(s_content, s_boot_title_text, WD_SAFE_X,
                                WD_BOOT_TITLE_Y, WD_BODY_W, WD_BOOT_TITLE_H,
                                WD_C_AMBER, wd_font(WD_TIER_MID),
                                LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
        s_boot_detail = mk_label(s_content, s_boot_detail_text, WD_SAFE_X,
                                 WD_BOOT_DETAIL_Y, WD_BODY_W, WD_BOOT_DETAIL_H,
                                 WD_C_TEXT, wd_font(WD_TIER_SMALL),
                                 LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_t *box = mk_box(s_content, WD_SAFE_X, WD_BOOT_AP_BOX_Y, WD_BODY_W,
                               WD_BOOT_AP_H, WD_C_PANEL, 12);
        mk_label(box, "手机连接这个 Wi-Fi 热点", 0, 6, WD_BODY_W, 42, WD_C_MUTED,
                 wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_CENTER,
                 LV_LABEL_LONG_MODE_DOTS);
        s_boot_ap_ssid = mk_label(box, s_boot_ap_text, 0, 52, WD_BODY_W, 63,
                                  WD_C_AMBER, wd_font(WD_TIER_MID),
                                  LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
        // 32px 一行约 23 个半角单位：「浏览器打开」5 个汉字(10) + 空格 + IP(11)
        // = 22 单位，正好放得下；再加上「然后」就会被省略号吃掉 IP。
        mk_label(box, "浏览器打开 192.168.4.1", 0, 118, WD_BODY_W, 42,
                 WD_C_GREEN, wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_CENTER,
                 LV_LABEL_LONG_MODE_DOTS);
        s_boot_btn = mk_hot_area(s_content, WD_BOOT_BTN_X, WD_BOOT_BTN_Y,
                                 WD_BOOT_BTN_W, WD_BOOT_BUTTON_H,
                                 WD_UI_EVT_BOOT_CONFIG, 0);
        lv_obj_set_style_bg_color(s_boot_btn, lv_color_hex(WD_C_CHIP), 0);
        lv_obj_set_style_bg_opa(s_boot_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_boot_btn, 10, 0);
        mk_label(s_boot_btn, "重新配网", 0, (WD_BOOT_BUTTON_H - 42) / 2,
                 WD_BOOT_BTN_W, 42, WD_C_AMBER, wd_font(WD_TIER_SMALL),
                 LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
    } else {
        s_boot_title = mk_label(s_content, s_boot_title_text, WD_SAFE_X,
                                WD_BOOT_TITLE_Y_PLAIN, WD_BODY_W, WD_BOOT_TITLE_H,
                                WD_C_AMBER, wd_font(WD_TIER_MID),
                                LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
        s_boot_detail = mk_label(s_content, s_boot_detail_text, WD_SAFE_X,
                                 WD_BOOT_DETAIL_Y_PLAIN, WD_BODY_W, WD_BOOT_DETAIL_H,
                                 WD_C_TEXT, wd_font(WD_TIER_SMALL),
                                 LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_WRAP);
        // 提示条说「点下方按钮重新配网」，这里必须真的有这个按钮 ——
        // 词库不可用时的唯一自救入口就是它。
        s_boot_btn = mk_hot_area(s_content, WD_BOOT_BTN_X, WD_BOOT_BTN_Y_PLAIN,
                                 WD_BOOT_BTN_W, WD_BOOT_BTN_H_PLAIN,
                                 WD_UI_EVT_BOOT_CONFIG, 0);
        lv_obj_set_style_bg_color(s_boot_btn, lv_color_hex(WD_C_CHIP), 0);
        lv_obj_set_style_bg_opa(s_boot_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_boot_btn, 12, 0);
        mk_label(s_boot_btn, "重新配网", 0, (WD_BOOT_BTN_H_PLAIN - 63) / 2,
                 WD_BOOT_BTN_W, 63, WD_C_AMBER, wd_font(WD_TIER_MID),
                 LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
    }
}

static void apply_boot(void) {
    set_text_or_empty(s_boot_title, s_boot_title_text);
    set_text_or_empty(s_boot_detail, s_boot_detail_text);
    set_text_or_empty(s_boot_ap_ssid, s_boot_ap_text);
}

// ---------------------------------------------------------------------------
// ② 选书页 / ⑤ 进度页（2x3 卡片网格 + 底部两枚按钮）
// ---------------------------------------------------------------------------

static void build_grid(bool progress_page) {
    const wd_ui_evt_t tap_evt = progress_page ? WD_UI_EVT_PROGRESS_TAP
                                              : WD_UI_EVT_BOOKS_TAP;
    for (unsigned i = 0; i < WD_UI_CARDS; ++i) {
        const int col = (int)(i % WD_GRID_COLS);
        const int row = (int)(i / WD_GRID_COLS);
        const int x = WD_SAFE_X + col * (int)(WD_GRID_CARD_W + WD_GRID_GAP);
        const int y = WD_GRID_TOP + row * (int)(WD_GRID_CARD_H + WD_GRID_GAP);
        s_card_box[i] = mk_box(s_content, x, y, WD_GRID_CARD_W, WD_GRID_CARD_H,
                               WD_C_PANEL, 12);
        lv_obj_add_flag(s_card_box[i], LV_OBJ_FLAG_CLICKABLE);
        set_hot_data(s_card_box[i], tap_evt, (int)i);
        lv_obj_add_event_cb(s_card_box[i], on_hot_clicked, LV_EVENT_CLICKED, NULL);

        s_card_name[i] = mk_label(s_card_box[i], "", 0, WD_CARD_NAME_Y,
                                  WD_GRID_CARD_W, 63, WD_C_TEXT,
                                  wd_font(WD_TIER_MID), LV_TEXT_ALIGN_CENTER,
                                  LV_LABEL_LONG_MODE_DOTS);
        s_card_count[i] = mk_label(s_card_box[i], "", 0, WD_CARD_COUNT_Y,
                                   WD_GRID_CARD_W, 42, WD_C_MUTED,
                                   wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_CENTER,
                                   LV_LABEL_LONG_MODE_DOTS);
        s_card_track[i] = mk_box(s_card_box[i], WD_CARD_BAR_X, WD_CARD_BAR_Y,
                                 WD_CARD_BAR_W, WD_CARD_BAR_H, WD_C_GRID, 3);
        s_card_fill[i] = mk_box(s_card_track[i], 0, 0, WD_CARD_BAR_W,
                                WD_CARD_BAR_H, WD_C_GREEN, 3);
    }

    static const char *const kBookFooters[WD_UI_FOOTERS] = {"开始测验", "学习进度"};
    static const char *const kProgressFooters[WD_UI_FOOTERS] = {"重置本册", "设置"};
    for (unsigned i = 0; i < WD_UI_FOOTERS; ++i) {
        const int x = WD_SAFE_X + (int)i * (int)(WD_GRID_CARD_W + WD_GRID_GAP);
        s_footer_btn[i] = mk_hot_area(s_content, x, WD_GRID_FOOTER_Y,
                                      WD_GRID_CARD_W, WD_GRID_FOOTER_H, tap_evt,
                                      (int)i);
        lv_obj_set_style_bg_color(s_footer_btn[i], lv_color_hex(WD_C_CHIP), 0);
        lv_obj_set_style_bg_opa(s_footer_btn[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_footer_btn[i], 12, 0);
        // arg 要用"行号 = count / count+1"，出题与清空才分得清 ——
        // 在 apply_grid 里按当前书数重新写。
        s_footer_label[i] = mk_label(s_footer_btn[i],
                                     progress_page ? kProgressFooters[i]
                                                   : kBookFooters[i],
                                     0, (WD_GRID_FOOTER_H - 63) / 2,
                                     WD_GRID_CARD_W, 63,
                                     i == 0 ? WD_C_AMBER : WD_C_TEXT,
                                     wd_font(WD_TIER_MID), LV_TEXT_ALIGN_CENTER,
                                     LV_LABEL_LONG_MODE_DOTS);
    }
}

static void apply_grid(bool progress_page) {
    const wd_ui_evt_t tap_evt = progress_page ? WD_UI_EVT_PROGRESS_TAP
                                              : WD_UI_EVT_BOOKS_TAP;
    for (unsigned i = 0; i < WD_UI_CARDS; ++i) {
        if (i >= s_book_count_cache) {
            set_hidden(s_card_box[i], true);
            continue;
        }
        set_hidden(s_card_box[i], false);
        const wd_book_t *book = &s_books_cache[i];
        set_text_or_empty(s_card_name[i], book->short_name);
        char text[28];
        snprintf(text, sizeof(text), "%u/%u", (unsigned)s_mastered_cache[i],
                 (unsigned)book->count);
        set_text_or_empty(s_card_count[i], text);

        uint32_t filled = 0;
        if (book->count) {
            filled = (uint32_t)((uint64_t)WD_CARD_BAR_W * s_mastered_cache[i] /
                                book->count);
        }
        if (filled > WD_CARD_BAR_W) filled = WD_CARD_BAR_W;
        lv_obj_set_width(s_card_fill[i], (int)filled);
        set_hidden(s_card_fill[i], filled == 0);
    }

    for (unsigned i = 0; i < WD_UI_FOOTERS; ++i) {
        // 行号与 wd_app 的约定一致：书是 0..count-1，底部第一枚是 count，
        // 第二枚是 count+1。
        set_hot_data(s_footer_btn[i], tap_evt, (int)s_book_count_cache + (int)i);
    }

    // 「重置本册」的二次确认：第一次点后整枚按钮描红，第二次才真清。
    if (progress_page && s_reset_armed_cache && s_footer_btn[0]) {
        lv_obj_set_style_border_color(s_footer_btn[0], lv_color_hex(WD_C_RED), 0);
        lv_obj_set_style_border_width(s_footer_btn[0], 3, 0);
        lv_obj_set_style_border_opa(s_footer_btn[0], LV_OPA_COVER, 0);
        set_text_color(s_footer_label[0], WD_C_RED);
    } else if (s_footer_btn[0]) {
        lv_obj_set_style_border_width(s_footer_btn[0], 0, 0);
        set_text_color(s_footer_label[0], WD_C_AMBER);
    }
}

// ---------------------------------------------------------------------------
// ③ 词条页
// ---------------------------------------------------------------------------

static void build_word(void) {
    s_word_label = mk_label(s_content, "", WD_SAFE_X, WD_WORD_TOP, WD_BODY_W,
                            WD_ZONE_WORD_H, WD_C_TEXT, wd_font(WD_TIER_BIG),
                            LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
    s_ipa_label = mk_label(s_content, "", WD_SAFE_X, WD_IPA_Y, WD_BODY_W, 42,
                           WD_C_MUTED, wd_font(WD_TIER_SMALL),
                           LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_MODE_DOTS);
    s_note_label = mk_label(s_content, "", WD_SAFE_X, WD_IPA_Y, WD_BODY_W, 42,
                            WD_C_AMBER, wd_font(WD_TIER_SMALL),
                            LV_TEXT_ALIGN_RIGHT, LV_LABEL_LONG_MODE_DOTS);
    mk_box(s_content, WD_SAFE_X, WD_DIVIDER_Y, WD_BODY_W, WD_ZONE_DIVIDER_H,
           WD_C_GRID, 0);

    // 正文是滚动区：48px 字放不下时上下拖，不再缩字号。区域本身可点 = 翻面，
    // 拖动时 LVGL 会把它当滚动而不是点击，两者不冲突。
    s_card_area = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_card_area);
    lv_obj_add_flag(s_card_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_card_area, WD_SAFE_X, WD_WORD_BODY_TOP);
    lv_obj_set_size(s_card_area, WD_BODY_W, WD_WORD_BODY_H);
    lv_obj_set_style_bg_opa(s_card_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_card_area, 0, 0);
    lv_obj_set_style_border_width(s_card_area, 0, 0);
    lv_obj_set_scroll_dir(s_card_area, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_card_area, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(s_card_area, WD_SCROLLBAR_W, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_card_area, lv_color_hex(WD_C_GRID),
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_card_area, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_card_area, WD_SCROLLBAR_W / 2, LV_PART_SCROLLBAR);
    set_hot_data(s_card_area, WD_UI_EVT_WORD_FLIP, 0);
    lv_obj_add_event_cb(s_card_area, on_hot_clicked, LV_EVENT_CLICKED, NULL);

    s_meaning_label = mk_label(s_card_area, "", 0, 0, WD_BODY_W, 63, WD_C_TEXT,
                               wd_font(WD_TIER_MID), LV_TEXT_ALIGN_LEFT,
                               LV_LABEL_LONG_MODE_WRAP);
    // 英文例句用 32px：64px 与单词同大会喧宾夺主，48px 又和中文译文一样高；
    // 32px 一行能放 23 个半角单位，长句折行少、滚动也少。
    s_sent_en_label = mk_label(s_card_area, "", 0, 0, WD_BODY_W,
                               (int)s_line_h[WD_TIER_SMALL], WD_C_TEXT,
                               wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_LEFT,
                               LV_LABEL_LONG_MODE_WRAP);
    s_sent_cn_label = mk_label(s_card_area, "", 0, 0, WD_BODY_W, 63, WD_C_MUTED,
                               wd_font(WD_TIER_MID), LV_TEXT_ALIGN_LEFT,
                               LV_LABEL_LONG_MODE_WRAP);

    // 第一行：‹上一个 / 发音 / 读例句（30px 级按钮，等宽三枚）。
    const wd_ui_evt_t row1_evt[3] = {WD_UI_EVT_WORD_PREV, WD_UI_EVT_WORD_SPEAK,
                                     WD_UI_EVT_WORD_SPEAK_SENTENCE};
    const char *const row1_text[3] = {"‹上一个", "发音", "读例句"};
    for (int i = 0; i < 3; ++i) {
        const int x = WD_SAFE_X + i * (WD_BAR_SMALL_W + WD_BAR_SMALL_GAP);
        lv_obj_t *btn = mk_hot_area(s_content, x, WD_BAR_Y, WD_BAR_SMALL_W,
                                    WD_BAR_ROW_H, row1_evt[i], 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(WD_C_CHIP), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        s_bar_btn[i] = btn;
        s_bar_label[i] = mk_label(btn, row1_text[i], 0, (WD_BAR_ROW_H - 42) / 2,
                                  WD_BAR_SMALL_W, 42, WD_C_TEXT,
                                  wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_CENTER,
                                  LV_LABEL_LONG_MODE_DOTS);
    }

    // 第二行：A 面「看例句」/ B 面「记住了」 + 「下一个›」。
    // 事件与文案都在重建时定死，翻面必然重建（见 wd_ui_show_word），
    // 所以不存在"换了面按钮没换"的中间态。
    const bool example = (s_side == WD_SIDE_EXAMPLE);
    const wd_ui_evt_t row2_evt[2] = {example ? WD_UI_EVT_WORD_MASTER
                                             : WD_UI_EVT_WORD_FLIP,
                                     WD_UI_EVT_WORD_NEXT};
    const char *const row2_text[2] = {example ? "记住了" : "看例句", "下一个›"};
    for (int i = 0; i < 2; ++i) {
        const int x = WD_SAFE_X + i * (int)(WD_BAR_WIDE_W + WD_ZONE_GAP);
        lv_obj_t *btn = mk_hot_area(s_content, x, WD_BAR_ROW2_Y, WD_BAR_WIDE_W,
                                    WD_BAR_ROW_H, row2_evt[i], 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(WD_C_CHIP), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        s_bar_btn[3 + i] = btn;
        s_bar_label[3 + i] = mk_label(btn, row2_text[i], 0, 0, WD_BAR_WIDE_W, 63,
                                      (i == 0 && example) ? WD_C_GREEN : WD_C_AMBER,
                                      wd_font(WD_TIER_MID), LV_TEXT_ALIGN_CENTER,
                                      LV_LABEL_LONG_MODE_DOTS);
    }
}

static void apply_word(void) {
    // 单词：整行放得下就用 64px，放不下降 48 / 32。
    const wd_tier_t word_tier = wd_layout_pick_tier(s_word_text, wd_font_fits, NULL);
    lv_obj_set_style_text_font(s_word_label, wd_font(word_tier), 0);
    lv_obj_set_pos(s_word_label, WD_SAFE_X,
                   centered_y(WD_WORD_TOP, WD_ZONE_WORD_H, word_tier));
    lv_obj_set_height(s_word_label, (int)s_line_h[word_tier]);
    set_text_or_empty(s_word_label, s_word_text);

    // 音标行右侧的发音状态提示，宽度按实测像素挤占音标。
    set_text_or_empty(s_note_label, s_note_text);
    int ipa_w = WD_BODY_W;
    if (s_note_text[0]) {
        lv_point_t size = {0, 0};
        lv_text_get_size(&size, s_note_text, wd_font(WD_TIER_SMALL), 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        ipa_w = WD_BODY_W - size.x - 8;
        if (ipa_w < 120) ipa_w = 120;
    }
    lv_obj_set_width(s_ipa_label, ipa_w);
    set_hidden(s_ipa_label, s_ipa_text[0] == '\0');
    set_text_or_empty(s_ipa_label, s_ipa_text);
    set_hidden(s_note_label, s_note_text[0] == '\0');

    const bool meaning = (s_side == WD_SIDE_MEANING);
    set_hidden(s_meaning_label, !meaning);
    set_hidden(s_sent_en_label, meaning);
    set_hidden(s_sent_cn_label, meaning);

    if (meaning) {
        const unsigned units = wd_layout_metrics(WD_TIER_MID)->units_per_line;
        const int line_h = (int)s_line_h[WD_TIER_MID];
        const char *src = s_meaning_text[0] ? s_meaning_text : "（这条词没有释义）";
        const char *text = fold_text(src, units, WD_WRAP_MAX_LINES);
        lv_label_set_text(s_meaning_label, text);
        lv_obj_set_pos(s_meaning_label, 0, 0);
        lv_obj_set_height(s_meaning_label, (int)text_lines(text) * line_h);
    } else {
        // 英文例句按 32px 档折行；中文译文仍按 48px 档。
        const unsigned en_units = wd_layout_metrics(WD_TIER_SMALL)->units_per_line;
        const int en_line_h = (int)s_line_h[WD_TIER_SMALL];
        const char *src_en = s_sent_en_text[0] ? s_sent_en_text : "（这条词没有例句）";
        const char *en = fold_text(src_en, en_units, WD_WRAP_MAX_LINES);
        lv_label_set_text(s_sent_en_label, en);
        lv_obj_set_pos(s_sent_en_label, 0, 0);
        const int en_h = (int)text_lines(en) * en_line_h;
        lv_obj_set_height(s_sent_en_label, en_h);

        const unsigned cn_units = wd_layout_metrics(WD_TIER_MID)->units_per_line;
        const int cn_line_h = (int)s_line_h[WD_TIER_MID];
        const char *cn = fold_text(s_sent_cn_text, cn_units, WD_WRAP_MAX_LINES);
        lv_label_set_text(s_sent_cn_label, cn);
        lv_obj_set_pos(s_sent_cn_label, 0, en_h + 12);
        lv_obj_set_height(s_sent_cn_label, (int)text_lines(cn) * cn_line_h);
    }
    lv_obj_update_layout(s_card_area);
    lv_obj_scroll_to_y(s_card_area, 0, LV_ANIM_OFF);
}

// ---------------------------------------------------------------------------
// ④ 测验页
// ---------------------------------------------------------------------------

static void build_quiz(void) {
    // 题干/反馈区：作答前是释义，作答后换成「答对/答错」两行反馈；
    // 整区常驻可点 = 下一题（应用在未作答时忽略）。
    s_quiz_qbox = mk_hot_area(s_content, WD_SAFE_X, WD_QUIZ_TOP, WD_BODY_W,
                              WD_QUIZ_QUESTION_H, WD_UI_EVT_QUIZ_NEXT, 0);
    s_quiz_question = mk_label(s_quiz_qbox, "", 0, 0, WD_BODY_W,
                               WD_QUIZ_QUESTION_H, WD_C_TEXT,
                               wd_font(WD_TIER_MID), LV_TEXT_ALIGN_CENTER,
                               LV_LABEL_LONG_MODE_WRAP);
    s_quiz_fb1 = mk_label(s_quiz_qbox, "", 0, 16, WD_BODY_W, 63, WD_C_GREEN,
                          wd_font(WD_TIER_MID), LV_TEXT_ALIGN_CENTER,
                          LV_LABEL_LONG_MODE_DOTS);
    s_quiz_fb2 = mk_label(s_quiz_qbox, "", 0, 86, WD_BODY_W, 42, WD_C_MUTED,
                          wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_CENTER,
                          LV_LABEL_LONG_MODE_DOTS);

    for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
        const int y = WD_QUIZ_OPT_TOP + (int)i * (int)WD_QUIZ_CHOICE_STEP;
        s_quiz_opt_box[i] = mk_box(s_content, WD_QUIZ_OPT_X, y, WD_QUIZ_OPT_W,
                                   WD_QUIZ_CHOICE_H, WD_C_PANEL, 12);
        lv_obj_add_flag(s_quiz_opt_box[i], LV_OBJ_FLAG_CLICKABLE);
        set_hot_data(s_quiz_opt_box[i], WD_UI_EVT_QUIZ_PICK, (int)i);
        lv_obj_add_event_cb(s_quiz_opt_box[i], on_hot_clicked, LV_EVENT_CLICKED, NULL);
        s_quiz_opt_label[i] = mk_label(s_quiz_opt_box[i], "",
                                       WD_QUIZ_OPT_LABEL_PAD, 0,
                                       WD_QUIZ_OPT_W - 2 * WD_QUIZ_OPT_LABEL_PAD,
                                       63, WD_C_TEXT, wd_font(WD_TIER_MID),
                                       LV_TEXT_ALIGN_CENTER,
                                       LV_LABEL_LONG_MODE_DOTS);
    }
}

static void apply_quiz(void) {
    const bool usable = s_quiz_has_state &&
                        (wd_quiz_ready(&s_quiz) || wd_quiz_answered(&s_quiz));
    if (!usable) {
        lv_obj_set_style_text_font(s_quiz_question, wd_font(WD_TIER_SMALL), 0);
        set_text_or_empty(s_quiz_question, "这本书暂时出不了题");
        lv_obj_set_pos(s_quiz_question, 0, 0);
        lv_obj_set_size(s_quiz_question, WD_BODY_W, WD_QUIZ_QUESTION_H);
        set_hidden(s_quiz_fb1, true);
        set_hidden(s_quiz_fb2, true);
        for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
            set_hidden(s_quiz_opt_box[i], true);
        }
        return;
    }

    const bool answered = wd_quiz_answered(&s_quiz);

    // 选项字号：四个里最难放的那个能承担的档，四个必须一致。
    // 只在 48/32 里选（64px 行高塞不进 66px 的选项盒，且 64px 档没有汉字）。
    wd_tier_t opt_tier = WD_TIER_MID;
    for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
        if (pick_mid_or_small(s_quiz_choice[i]) == WD_TIER_SMALL) {
            opt_tier = WD_TIER_SMALL;
        }
    }

    const unsigned answer_slot = wd_quiz_slot_of_answer(&s_quiz);
    for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
        set_hidden(s_quiz_opt_box[i], false);
        lv_obj_set_style_text_font(s_quiz_opt_label[i], wd_font(opt_tier), 0);
        lv_obj_set_pos(s_quiz_opt_label[i], WD_QUIZ_OPT_LABEL_PAD,
                       (WD_QUIZ_CHOICE_H - s_line_h[opt_tier]) / 2);
        set_text_or_empty(s_quiz_opt_label[i], s_quiz_choice[i]);

        uint32_t bg = WD_C_PANEL;
        uint32_t fg = WD_C_TEXT;
        if (answered) {
            if (i == answer_slot) {
                bg = WD_C_GREEN;
                fg = WD_C_DARK_TEXT;
            } else if (i == s_quiz.picked) {
                bg = WD_C_RED;
                fg = WD_C_DARK_TEXT;
            }
        }
        lv_obj_set_style_bg_color(s_quiz_opt_box[i], lv_color_hex(bg), 0);
        set_text_color(s_quiz_opt_label[i], fg);
    }

    if (!answered) {
        set_hidden(s_quiz_fb1, true);
        set_hidden(s_quiz_fb2, true);
        set_hidden(s_quiz_question, false);

        // 题干：短释义 48px 居中，长释义 32px。
        const wd_tier_t q_tier =
            fold_fits(s_quiz_meaning, WD_TIER_MID, WD_QUIZ_QUESTION_H)
                ? WD_TIER_MID
                : WD_TIER_SMALL;
        lv_obj_set_style_text_font(s_quiz_question, wd_font(q_tier), 0);
        const unsigned lines = WD_QUIZ_QUESTION_H / s_line_h[q_tier];
        const char *text = fold_text(s_quiz_meaning,
                                     wd_layout_metrics(q_tier)->units_per_line,
                                     lines);
        lv_label_set_text(s_quiz_question, text);
        lv_obj_set_pos(s_quiz_question, 0,
                       ((int)WD_QUIZ_QUESTION_H -
                        (int)text_lines(text) * (int)s_line_h[q_tier]) /
                           2);
        lv_obj_set_size(s_quiz_question, WD_BODY_W,
                        (int)text_lines(text) * (int)s_line_h[q_tier]);
        return;
    }

    // 作答后：反馈取代题干，仍是两行（48px + 32px）。
    set_hidden(s_quiz_question, true);
    set_hidden(s_quiz_fb1, false);
    set_hidden(s_quiz_fb2, false);
    const bool correct = (s_quiz.phase == WD_QUIZ_PHASE_CORRECT);
    char line1[WD_UI_WORD_CAP + 12];
    snprintf(line1, sizeof(line1), "%s %s", correct ? "答对" : "答错",
             s_quiz_answer_word);
    set_text_or_empty(s_quiz_fb1, line1);
    set_text_color(s_quiz_fb1, correct ? WD_C_GREEN : WD_C_RED);

    char line2[WD_UI_IPA_CAP + 24];
    if (s_quiz_answer_ipa[0]) {
        snprintf(line2, sizeof(line2), "%s · 点这里继续 ›", s_quiz_answer_ipa);
    } else {
        snprintf(line2, sizeof(line2), "点这里继续 ›");
    }
    set_text_or_empty(s_quiz_fb2, line2);
}

// ---------------------------------------------------------------------------
// ⑥ 设置页
// ---------------------------------------------------------------------------

static void build_settings_menu(void) {
    for (unsigned i = 0; i < WD_SETTINGS_MENU_ROWS; ++i) {
        const int y = WD_SET_MENU_TOP +
                      (int)i * (int)(WD_SET_MENU_ROW_H + WD_SET_MENU_ROW_GAP);
        s_set_menu_row[i] = mk_box(s_content, WD_SAFE_X, y, WD_BODY_W,
                                   WD_SET_MENU_ROW_H, WD_C_CHIP, 12);
        lv_obj_add_flag(s_set_menu_row[i], LV_OBJ_FLAG_CLICKABLE);
        set_hot_data(s_set_menu_row[i], WD_UI_EVT_SET_MENU, (int)i);
        lv_obj_add_event_cb(s_set_menu_row[i], on_hot_clicked, LV_EVENT_CLICKED, NULL);
        s_set_menu_label[i] = mk_label(s_set_menu_row[i], kSettingsMenuItems[i], 0,
                                       (WD_SET_MENU_ROW_H - 63) / 2, WD_BODY_W, 63,
                                       i == 0 ? WD_C_AMBER : WD_C_TEXT,
                                       wd_font(WD_TIER_MID), LV_TEXT_ALIGN_CENTER,
                                       LV_LABEL_LONG_MODE_DOTS);
    }
}

static void build_settings_value(void) {
    s_set_title = mk_label(s_content, "", WD_SAFE_X, WD_SET_TITLE_Y, WD_BODY_W,
                           WD_SET_TITLE_H, WD_C_TEXT, wd_font(WD_TIER_MID),
                           LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
    s_set_value = mk_label(s_content, "", WD_SAFE_X, WD_SET_VALUE_Y, WD_BODY_W,
                           WD_SET_VALUE_H, WD_C_AMBER, wd_font(WD_TIER_BIG),
                           LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
    s_set_bar_track = mk_box(s_content, WD_SET_BAR_X, WD_SET_BAR_Y, WD_SET_BAR_W,
                             WD_SET_BAR_H, WD_C_GRID, WD_SET_BAR_H / 2);
    s_set_bar_fill = mk_box(s_set_bar_track, 0, 0, WD_SET_BAR_W, WD_SET_BAR_H,
                            WD_C_AMBER, WD_SET_BAR_H / 2);
    s_set_step_minus = mk_hot_area(s_content, WD_SAFE_X + 24, WD_SET_STEP_Y,
                                   WD_SET_STEP_W, WD_SET_STEP_H, WD_UI_EVT_SET_STEP, -1);
    s_set_step_plus = mk_hot_area(s_content,
                                  WD_SCREEN_W - WD_SAFE_X - 24 - WD_SET_STEP_W,
                                  WD_SET_STEP_Y, WD_SET_STEP_W, WD_SET_STEP_H,
                                  WD_UI_EVT_SET_STEP, 1);
    // 大号 ASCII "-" / "+"：64px 档只有 ASCII 与 IPA，正好够用；
    // 全角的 －/＋ 不在 64px 档的字库里，会变成方框。
    static const char *const kMinus = "-";
    static const char *const kPlus = "+";
    for (int k = 0; k < 2; ++k) {
        lv_obj_t *btn = k ? s_set_step_plus : s_set_step_minus;
        lv_obj_set_style_bg_color(btn, lv_color_hex(WD_C_CHIP), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, 16, 0);
        mk_label(btn, k ? kPlus : kMinus, 0, (WD_SET_STEP_H - 84) / 2,
                 WD_SET_STEP_W, 84, WD_C_TEXT, wd_font(WD_TIER_BIG),
                 LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
    }
}

static void build_settings_wifi(void) {
    s_set_title = mk_label(s_content, "配网", WD_SAFE_X, WD_SET_TEXT_TITLE_Y,
                           WD_BODY_W, WD_SET_TITLE_H, WD_C_TEXT,
                           wd_font(WD_TIER_MID), LV_TEXT_ALIGN_CENTER,
                           LV_LABEL_LONG_MODE_DOTS);
    // 每行都必须 <= 23 个半角单位（32px 一行），四行正好占满 168px。
    // 原来带「1. 」编号的写法每行要折成两行，正文放不下第四行。
    s_set_value = mk_label(s_content,
                           "点下面按钮开始配网\n"
                           "手机连上屏幕热点\n"
                           "浏览器打开 192.168.4.1\n"
                           "配好自动返回",
                           WD_SAFE_X, WD_SET_WIFI_BODY_Y, WD_BODY_W,
                           WD_SET_WIFI_BODY_H, WD_C_MUTED,
                           wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_LEFT,
                           LV_LABEL_LONG_MODE_WRAP);
    s_set_wifi_btn = mk_hot_area(s_content, WD_SAFE_X, WD_SET_WIFI_BTN_Y,
                                 WD_BODY_W, WD_SET_WIFI_BTN_H, WD_UI_EVT_SET_WIFI, 0);
    lv_obj_set_style_bg_color(s_set_wifi_btn, lv_color_hex(WD_C_AMBER), 0);
    lv_obj_set_style_bg_opa(s_set_wifi_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_set_wifi_btn, 14, 0);
    mk_label(s_set_wifi_btn, "开始配网", 0, (WD_SET_WIFI_BTN_H - 63) / 2,
             WD_BODY_W, 63, WD_C_DARK_TEXT, wd_font(WD_TIER_MID),
             LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
}

static void build_settings_about(void) {
    s_set_title = mk_label(s_content, "关于", WD_SAFE_X, WD_ABOUT_TITLE_Y,
                           WD_BODY_W, WD_SET_TITLE_H, WD_C_TEXT,
                           wd_font(WD_TIER_MID), LV_TEXT_ALIGN_CENTER,
                           LV_LABEL_LONG_MODE_DOTS);
    s_set_value = mk_label(s_content, "英语词典", WD_SAFE_X, WD_ABOUT_NAME_Y,
                           WD_BODY_W, 63, WD_C_AMBER, wd_font(WD_TIER_MID),
                           LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
    mk_label(s_content, "离线词库 · 在线发音", WD_SAFE_X, WD_ABOUT_SUB_Y, WD_BODY_W,
             42, WD_C_MUTED, wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_CENTER,
             LV_LABEL_LONG_MODE_DOTS);
    // 每行 <= 23 个半角单位（32px 一行），否则会在 DOTS 模式下被截断。
    static const char *const kRows[] = {
        "点书学习，点卡片翻面",
        "发音读单词与例句",
        "测验与进度在主页下方",
    };
    for (unsigned i = 0; i < sizeof(kRows) / sizeof(kRows[0]); ++i) {
        mk_label(s_content, kRows[i], WD_SAFE_X,
                 WD_ABOUT_ROW_TOP + (int)i * WD_ABOUT_ROW_STEP, WD_BODY_W, 42,
                 WD_C_TEXT, wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_CENTER,
                 LV_LABEL_LONG_MODE_DOTS);
    }
    char ver[48];
    snprintf(ver, sizeof(ver), "固件版本  v" CONFIG_APP_PROJECT_VER);
    mk_label(s_content, ver, WD_SAFE_X, WD_ABOUT_VER_Y, WD_BODY_W, 42, WD_C_MUTED,
             wd_font(WD_TIER_SMALL), LV_TEXT_ALIGN_CENTER, LV_LABEL_LONG_MODE_DOTS);
}

static wd_set_shape_t shape_of(wd_settings_page_t page) {
    switch (page) {
        case WD_SETTINGS_VOLUME:
        case WD_SETTINGS_BRIGHTNESS:
        case WD_SETTINGS_TIMEOUT:
            return WD_SET_SHAPE_VALUE;
        case WD_SETTINGS_WIFI:
            return WD_SET_SHAPE_WIFI;
        case WD_SETTINGS_ABOUT:
            return WD_SET_SHAPE_ABOUT;
        case WD_SETTINGS_MENU:
        default:
            return WD_SET_SHAPE_MENU;
    }
}

static void build_settings(wd_set_shape_t shape) {
    switch (shape) {
        case WD_SET_SHAPE_VALUE:
            build_settings_value();
            break;
        case WD_SET_SHAPE_WIFI:
            build_settings_wifi();
            break;
        case WD_SET_SHAPE_ABOUT:
            build_settings_about();
            break;
        case WD_SET_SHAPE_MENU:
        case WD_SET_SHAPE_NONE:
        default:
            build_settings_menu();
            break;
    }
}

static void apply_settings(void) {
    char value[32];
    int percent = -1;
    switch (s_set_shape) {
        case WD_SET_SHAPE_VALUE:
            switch (s_set_page) {
                case WD_SETTINGS_VOLUME:
                    copy_str(value, sizeof(value), "音量");
                    percent = (int)s_set_volume;
                    break;
                case WD_SETTINGS_BRIGHTNESS:
                    copy_str(value, sizeof(value), "亮度");
                    percent = (int)s_set_brightness;
                    break;
                case WD_SETTINGS_TIMEOUT:
                    copy_str(value, sizeof(value), "息屏");
                    break;
                default:
                    value[0] = '\0';
                    break;
            }
            {
                char num[24];
                if (s_set_page == WD_SETTINGS_TIMEOUT) {
                    if (s_set_timeout == 0) {
                        copy_str(num, sizeof(num), "不自动息屏");
                    } else if (s_set_timeout < 60) {
                        snprintf(num, sizeof(num), "%u 秒", (unsigned)s_set_timeout);
                    } else {
                        snprintf(num, sizeof(num), "%u 分钟",
                                 (unsigned)(s_set_timeout / 60));
                    }
                } else {
                    snprintf(num, sizeof(num), "%u%%",
                             (unsigned)((s_set_page == WD_SETTINGS_VOLUME)
                                            ? s_set_volume
                                            : s_set_brightness));
                }
                // 音量/亮度是 ASCII 数字，用 64px 大字；息屏含汉字，用 48px。
                if (s_set_page == WD_SETTINGS_TIMEOUT) {
                    lv_obj_set_style_text_font(s_set_value, wd_font(WD_TIER_MID), 0);
                    lv_obj_set_pos(s_set_value, WD_SAFE_X,
                                   WD_SET_VALUE_Y + (int)(WD_SET_VALUE_H - 63) / 2);
                    lv_obj_set_height(s_set_value, 63);
                } else {
                    lv_obj_set_style_text_font(s_set_value, wd_font(WD_TIER_BIG), 0);
                    lv_obj_set_pos(s_set_value, WD_SAFE_X, WD_SET_VALUE_Y);
                    lv_obj_set_height(s_set_value, 84);
                }
                set_text_or_empty(s_set_title, value);
                set_text_or_empty(s_set_value, num);
            }
            if (s_set_bar_track && s_set_bar_fill) {
                const bool show = (percent >= 0);
                set_hidden(s_set_bar_track, !show);
                if (show) {
                    if (percent > 100) percent = 100;
                    lv_obj_set_width(s_set_bar_fill, (WD_SET_BAR_W * percent) / 100);
                    set_hidden(s_set_bar_fill, percent <= 0);
                }
            }
            break;
        case WD_SET_SHAPE_WIFI:
        case WD_SET_SHAPE_ABOUT:
        case WD_SET_SHAPE_MENU:
        case WD_SET_SHAPE_NONE:
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// 切屏
// ---------------------------------------------------------------------------

static void forget_objects(void) {
    s_boot_title = NULL;
    s_boot_detail = NULL;
    s_boot_ap_ssid = NULL;
    s_boot_btn = NULL;
    for (unsigned i = 0; i < WD_UI_CARDS; ++i) {
        s_card_box[i] = NULL;
        s_card_track[i] = NULL;
        s_card_fill[i] = NULL;
        s_card_name[i] = NULL;
        s_card_count[i] = NULL;
    }
    for (unsigned i = 0; i < WD_UI_FOOTERS; ++i) {
        s_footer_btn[i] = NULL;
        s_footer_label[i] = NULL;
    }
    for (unsigned i = 0; i < WD_SETTINGS_MENU_ROWS; ++i) {
        s_set_menu_row[i] = NULL;
        s_set_menu_label[i] = NULL;
    }
    s_word_label = NULL;
    s_ipa_label = NULL;
    s_note_label = NULL;
    s_card_area = NULL;
    s_meaning_label = NULL;
    s_sent_en_label = NULL;
    s_sent_cn_label = NULL;
    for (int i = 0; i < 5; ++i) {
        s_bar_btn[i] = NULL;
        s_bar_label[i] = NULL;
    }
    s_quiz_qbox = NULL;
    s_quiz_question = NULL;
    s_quiz_fb1 = NULL;
    s_quiz_fb2 = NULL;
    for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
        s_quiz_opt_box[i] = NULL;
        s_quiz_opt_label[i] = NULL;
    }
    s_set_title = NULL;
    s_set_value = NULL;
    s_set_bar_track = NULL;
    s_set_bar_fill = NULL;
    s_set_step_minus = NULL;
    s_set_step_plus = NULL;
    s_set_wifi_btn = NULL;
}

static void rebuild_content(void) {
    lv_obj_clean(s_content);
    forget_objects();
    switch (s_current) {
        case WD_SCREEN_BOOT:
            build_boot();
            apply_boot();
            break;
        case WD_SCREEN_BOOKS:
            build_grid(false);
            apply_grid(false);
            break;
        case WD_SCREEN_WORD:
            build_word();
            apply_word();
            break;
        case WD_SCREEN_QUIZ:
            build_quiz();
            apply_quiz();
            break;
        case WD_SCREEN_PROGRESS:
            build_grid(true);
            apply_grid(true);
            break;
        case WD_SCREEN_SETTINGS:
            if (s_set_shape != WD_SET_SHAPE_NONE) {
                build_settings(s_set_shape);
                apply_settings();
            }
            break;
        default:
            break;
    }
    apply_chrome();
}

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

void wd_ui_set_evt_cb(void (*cb)(wd_ui_evt_t, int)) { s_evt_cb = cb; }

bool wd_ui_init(void) {
    if (!ui_lock()) return false;
    wd_font_init();

    for (int tier = 0; tier < WD_TIER_COUNT; ++tier) {
        const lv_font_t *font = wd_font((wd_tier_t)tier);
        s_line_h[tier] = font ? (uint8_t)font->line_height : 0;
        if (font && font->line_height > wd_tier_metrics[tier].line_height) {
            ESP_LOGW("wd_ui", "第 %d 档真实行高 %d 超过预算 %d —— 按表算的行数偏乐观",
                     tier, (int)font->line_height,
                     (int)wd_tier_metrics[tier].line_height);
        }
    }

    // 字库自检：取不到字形 = 生成物没同步，宁可报错也不满屏方框。
    lv_font_glyph_dsc_t dsc;
    for (int tier = 0; tier < WD_TIER_COUNT; ++tier) {
        const lv_font_t *font = wd_font((wd_tier_t)tier);
        if (!font || !lv_font_get_glyph_dsc(font, &dsc, 'A', 0)) {
            ESP_LOGE("wd_ui", "第 %d 档字库取不到 'A' —— 没生成或已损坏", tier);
            ui_unlock();
            return false;
        }
        if (tier != WD_TIER_BIG &&
            !lv_font_get_glyph_dsc(font, &dsc, 0x4E2Du /* 中 */, 0)) {
            ESP_LOGE("wd_ui", "第 %d 档字库取不到汉字 —— 界面文案会显示成方框", tier);
            ui_unlock();
            return false;
        }
        if (!lv_font_get_glyph_dsc(font, &dsc, 0x0259u /* ə */, 0)) {
            ESP_LOGE("wd_ui", "第 %d 档字库取不到 ə —— IPA 回退链没接上", tier);
        }
    }

    // 自建深色 screen（LVGL 默认主题是浅底，不铺底就是白底米白字）。
    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(WD_C_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);

    s_content = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_content);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_content, 0, 0);
    lv_obj_set_size(s_content, WD_SCREEN_W, WD_SCREEN_H);

    s_chrome = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_chrome);
    lv_obj_remove_flag(s_chrome, LV_OBJ_FLAG_SCROLLABLE);
    // 共享栏整屏覆盖在内容层之上。它必须不可点，否则任何点按都先命中它，
    // 下层所有卡片/按钮全部失效（只剩栏内的返回/设置芯片还能响应）。
    lv_obj_remove_flag(s_chrome, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_chrome, 0, 0);
    lv_obj_set_size(s_chrome, WD_SCREEN_W, WD_SCREEN_H);
    build_chrome(s_chrome);
    lv_obj_move_foreground(s_chrome);

    lv_screen_load(s_root);

    s_current = WD_SCREEN_COUNT;
    s_boot_has_ap = false;
    s_side = WD_SIDE_MEANING;
    s_quiz_has_state = false;
    s_set_shape = WD_SET_SHAPE_NONE;
    s_status_book_text[0] = '\0';
    s_hint_text[0] = '\0';
    s_book_count_cache = 0;
    s_reset_armed_cache = false;

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI("wd_ui", "共享栏就绪；LVGL 堆空闲 %u / 总 %u，最大可用块 %u，碎片 %u%%",
             (unsigned)mon.free_size, (unsigned)mon.total_size,
             (unsigned)mon.free_biggest_size, (unsigned)mon.frag_pct);
    ui_unlock();
    return true;
}

void wd_ui_show(wd_screen_t screen) {
    if (screen >= WD_SCREEN_COUNT || screen == s_current) return;
    if (!ui_lock()) return;
    s_current = screen;
    rebuild_content();
    apply_status();
    ui_unlock();
}

void wd_ui_set_status(const wd_ui_status_t *status) {
    if (!ui_lock()) return;
    if (status) {
        copy_str(s_status_book_text, sizeof(s_status_book_text), status->book_short);
        s_status_mastered = status->mastered;
        s_status_total = status->total;
        s_status_connected = status->connected;
    } else {
        s_status_book_text[0] = '\0';
        s_status_mastered = 0;
        s_status_total = 0;
        s_status_connected = false;
    }
    apply_status();
    ui_unlock();
}

void wd_ui_set_hint(const char *text) {
    if (!ui_lock()) return;
    copy_str(s_hint_text, sizeof(s_hint_text), text);
    apply_hint();
    ui_unlock();
}

void wd_ui_show_boot(const char *title, const char *detail, const char *ap_ssid) {
    copy_str(s_boot_title_text, sizeof(s_boot_title_text), title);
    copy_str(s_boot_detail_text, sizeof(s_boot_detail_text), detail);
    copy_str(s_boot_ap_text, sizeof(s_boot_ap_text), ap_ssid);
    const bool has_ap = (s_boot_ap_text[0] != '\0');

    if (!ui_lock()) return;
    const bool shape_ok = (s_current == WD_SCREEN_BOOT) && (has_ap == s_boot_has_ap);
    s_boot_has_ap = has_ap;
    if (s_current != WD_SCREEN_BOOT) {
        s_current = WD_SCREEN_BOOT;
        rebuild_content();
    } else if (!shape_ok) {
        rebuild_content();
    } else {
        apply_boot();
    }
    apply_chrome();
    apply_status();
    ui_unlock();
}

void wd_ui_show_books(const wd_book_t *books, const uint32_t *mastered_per_book,
                      unsigned count) {
    if (count > WD_MAX_BOOKS) count = WD_MAX_BOOKS;
    s_book_count_cache = count;
    for (unsigned i = 0; i < count; ++i) {
        s_books_cache[i] = books[i];
        s_mastered_cache[i] = mastered_per_book ? mastered_per_book[i] : 0;
    }
    if (!ui_lock()) return;
    if (s_current != WD_SCREEN_BOOKS) {
        s_current = WD_SCREEN_BOOKS;
        rebuild_content();
    } else {
        apply_grid(false);
        apply_chrome();
    }
    apply_status();
    ui_unlock();
}

void wd_ui_show_progress(const wd_book_t *books, const uint32_t *mastered,
                         unsigned count, bool reset_armed) {
    if (count > WD_MAX_BOOKS) count = WD_MAX_BOOKS;
    s_book_count_cache = count;
    s_reset_armed_cache = reset_armed;
    for (unsigned i = 0; i < count; ++i) {
        s_books_cache[i] = books[i];
        s_mastered_cache[i] = mastered ? mastered[i] : 0;
    }
    if (!ui_lock()) return;
    if (s_current != WD_SCREEN_PROGRESS) {
        s_current = WD_SCREEN_PROGRESS;
        rebuild_content();
    } else {
        apply_grid(true);
        apply_chrome();
    }
    apply_status();
    ui_unlock();
}

void wd_ui_show_word(const char *word, const char *ipa, const char *meaning,
                     const char *sentence_en, const char *sentence_cn,
                     wd_side_t side, const char *audio_note) {
    const wd_side_t prev_side = s_side;
    copy_str(s_word_text, sizeof(s_word_text), word);
    copy_str(s_ipa_text, sizeof(s_ipa_text), ipa);
    copy_str(s_meaning_text, sizeof(s_meaning_text), meaning);
    copy_str(s_sent_en_text, sizeof(s_sent_en_text), sentence_en);
    copy_str(s_sent_cn_text, sizeof(s_sent_cn_text), sentence_cn);
    copy_str(s_note_text, sizeof(s_note_text), audio_note);
    s_side = (side == WD_SIDE_EXAMPLE) ? WD_SIDE_EXAMPLE : WD_SIDE_MEANING;

    if (!ui_lock()) return;
    // 翻面会切换 A/B 两套标签的可见性并改第 4 个按钮的文字/颜色，重建比逐对象
    // 刷新更简单也更不易漏；同侧的词切换只改文字。
    const bool side_changed = (prev_side != s_side) || (s_current != WD_SCREEN_WORD);
    if (side_changed) {
        s_current = WD_SCREEN_WORD;
        rebuild_content();
    } else {
        apply_word();
    }
    apply_chrome();
    apply_status();
    ui_unlock();
}

void wd_ui_show_quiz(const char *meaning, const char *answer_word,
                     const char *answer_ipa, const char *const *choices,
                     const wd_quiz_t *quiz) {
    copy_str(s_quiz_meaning, sizeof(s_quiz_meaning), meaning);
    copy_str(s_quiz_answer_word, sizeof(s_quiz_answer_word), answer_word);
    copy_str(s_quiz_answer_ipa, sizeof(s_quiz_answer_ipa), answer_ipa);
    for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
        copy_str(s_quiz_choice[i], sizeof(s_quiz_choice[i]),
                 choices ? choices[i] : NULL);
    }
    if (quiz) {
        s_quiz = *quiz;
        s_quiz_has_state = true;
    } else {
        memset(&s_quiz, 0, sizeof(s_quiz));
        s_quiz.phase = WD_QUIZ_PHASE_IDLE;
        s_quiz_has_state = false;
    }

    if (!ui_lock()) return;
    if (s_current != WD_SCREEN_QUIZ) {
        s_current = WD_SCREEN_QUIZ;
        rebuild_content();
    } else {
        apply_quiz();
        apply_chrome();
    }
    apply_status();
    ui_unlock();
}

void wd_ui_show_settings(wd_settings_page_t page, uint8_t volume_percent,
                         uint8_t brightness_percent, uint16_t timeout_seconds) {
    if (page >= WD_SETTINGS_PAGE_COUNT) page = WD_SETTINGS_MENU;
    s_set_page = page;
    s_set_volume = volume_percent;
    s_set_brightness = brightness_percent;
    s_set_timeout = timeout_seconds;

    if (!ui_lock()) return;
    const wd_set_shape_t want = shape_of(page);
    if (s_current != WD_SCREEN_SETTINGS || s_set_shape != want) {
        s_set_shape = want;
        s_current = WD_SCREEN_SETTINGS;
        rebuild_content();
    } else {
        apply_settings();
        apply_chrome();
    }
    apply_status();
    ui_unlock();
}
