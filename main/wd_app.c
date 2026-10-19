// main/wd_app.c —— 应用状态机（ESP-Mosaico 触摸版）。
//
// 职责与分层见 wd_app.h。这里记三件"每次出问题都先看这里"的事：
//
// ---------------------------------------------------------------------------
// 1. 点按即一切：六屏的交互对照表
// ---------------------------------------------------------------------------
//   启动/配网页   「重新配网」按钮 → 重开热点；配好自动回学习界面
//   选书页        点书 → 进书学习；点「开始测验」→ 测验；点「学习进度」→ 进度页
//   词条页        底部 [‹上一个][发音][看例句|记住了][下一个›]；
//                 点卡片 = 翻面（A 释义 ↔ B 例句，翻面**不停**发音）
//   测验页        点选项 = 作答；作答后点结果区 = 下一题
//   进度页        点书 = 进书学习；点「重置本册」两次 = 清空当前书进度；
//                 点「设置」= 设置菜单
//   设置页        点菜单行进页；值页点 −/＋ 调节；配网页点「开始配网」
//   顶栏          非根屏左上角常驻「‹返回」—— 返回永远在同一个位置
//
// ---------------------------------------------------------------------------
// 2. 没联网不等于不能用
// ---------------------------------------------------------------------------
// 词库在 Flash 的 rodata 里（约 2.9 MB），查词、翻面、做题全都不需要网络 ——
// 只有发音需要。所以**只有配网门户开着的时候**才显示配网页；一旦词库可读，
// 主界面就该出来，状态栏的 Wi-Fi 指示条暗着表示"发音暂时用不了"。
// 反过来做（没网就停在配网页）会让一台能用的词典看起来像砖头。
//
// ---------------------------------------------------------------------------
// 3. 写盘要克制
// ---------------------------------------------------------------------------
// 每点一次都存一次断点 = 每学几个词就擦写一次 Flash。所以断点用
// "脏标记 + 定期落盘"（见 wd_app_tick），而**掌握位图**不同：它是一个词
// 一个 bit 的累积状态，丢了就是真的丢了，所以标记掌握时立刻写。
#include "wd_app.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wd_audio.h"
#include "wd_content.h"
#include "wd_content_blob.h"
#include "wd_display.h"
#include "wd_manifest.h"
#include "wd_progress.h"
#include "wd_quiz.h"
#include "wd_store.h"

#include <stdio.h>
#include <string.h>

#define TAG "wd_app"

// ---------------------------------------------------------------------------
// 0. 三个任务都会进来 —— 所以要有一把锁
// ---------------------------------------------------------------------------
// 进这一层的任务有三条：触摸事件（LVGL 任务，prio 高）、周期 tick（tick_task，
// prio 3）、Wi-Fi 事件（WifiManager 的回调任务）。三条都会改 s_mode / s_offset
// 这些全局量并调 wd_ui_*。
//
// 只靠 wd_ui 里那把显示锁是不够的：它保护的是 LVGL 的对象图，
// 而"现在该显示哪一屏"是本层自己的状态。两条任务交错时的真实后果是
// "屏幕切到了另一屏 / 一次点按丢了"，不崩、但用户看得见，而且因为可复现性差
// （要恰好撞上）会很难查。
//
// 锁是递归的：wd_app_init 会调 show_* 这些内部函数，内部函数自己也加锁。
static SemaphoreHandle_t s_lock;

static void app_lock(void) {
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void app_unlock(void) {
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

// 掌握位图的缓冲。最大的一本书是 KaoYan_2 的 4533 词 -> 567 字节；
// 给 1024 字节（够 8192 词）留 1.8 倍余量。超了就拒绝进入那本书并打日志，
// **不截断**：截断的位图会让"已掌握"数字变小，而用户会以为是进度丢了。
#define WD_BITMAP_CAP 1024

// wd_app_tick 的周期（main.cc 按这个值建定时器），以及由此推出的低频落盘间隔。
#define WD_TICK_MS 500
#define WD_RESUME_TICKS (30000 / WD_TICK_MS)   // 30 秒落一次断点

typedef enum {
    WD_MODE_BOOT = 0,
    WD_MODE_BOOKS,
    WD_MODE_WORD,
    WD_MODE_QUIZ,
    WD_MODE_PROGRESS,
    WD_MODE_SETTINGS,
} wd_mode_t;

static wd_blob_t s_blob;
static bool s_ready;            // 词库可读（不可读时只剩配网一件事）
static unsigned s_book_count;
static wd_book_t s_books[WD_MAX_BOOKS];
static uint32_t s_mastered[WD_MAX_BOOKS];
static uint8_t s_bits[WD_BITMAP_CAP];   // 只装**当前书**的位图

static unsigned s_book;
static uint32_t s_offset;       // 书内偏移
static uint32_t s_seed;         // 出题种子（进断点记录，重启后能复现同一道题）
static wd_side_t s_side;
static wd_word_t s_word;
static wd_quiz_t s_quiz;

static wd_mode_t s_mode;
// 进度页「重置本册」的二次确认。这是**破坏性**操作：第一次点把目标行标红，
// 第二次点才真清；点别处（换目标）就撤回。
static bool s_reset_armed;
static wd_settings_page_t s_set_page;
// 设置页是从哪一屏进来的。顶栏「设置」芯片在四屏常驻，返回必须回原屏。
static wd_mode_t s_settings_origin;

static bool s_connected;
static char s_ap_ssid[33];
static char s_boot_reason[96];   // 词库不可用时打在屏幕上的人话
static uint8_t s_volume;
static uint8_t s_brightness;
static uint16_t s_timeout;

static void (*s_config_ap_hook)(void);
static int s_audio_state_shown = -1;
static bool s_resume_dirty;
static uint32_t s_tick;

// ---------------------------------------------------------------------------
// 状态栏
// ---------------------------------------------------------------------------

static const wd_book_t *cur_book(void) {
    return (s_book < s_book_count) ? &s_books[s_book] : NULL;
}

static void push_status(void) {
    const wd_book_t *book = cur_book();
    // 选书页与进度页正在**浏览**：此时显示"哪一本书"的进度是错的 ——
    // s_book 是"上一次进去过的那一本"，与停在哪个条目毫无关系。那两屏的每一行
    // 本来就带着自己的进度数字，所以状态栏左槽改放应用名当标题用。
    const bool browsing = (s_mode == WD_MODE_BOOKS) || (s_mode == WD_MODE_PROGRESS);

    wd_ui_status_t status;
    memset(&status, 0, sizeof(status));
    status.book_short = browsing ? "词典" : (book ? book->short_name : "");
    status.mastered = (!browsing && book) ? s_mastered[s_book] : 0;
    status.total = (!browsing && book) ? book->count : 0;
    status.connected = s_connected;
    wd_ui_set_status(&status);
}

// ---------------------------------------------------------------------------
// 掌握位图
// ---------------------------------------------------------------------------

static bool load_bits(unsigned book) {
    if (book >= s_book_count) return false;
    const uint32_t bytes = wd_progress_bitmap_bytes(s_books[book].count);
    if (bytes > sizeof(s_bits)) {
        ESP_LOGE(TAG, "书 %s 要 %u 字节位图，超过缓冲 %u —— 拒绝进入",
                 s_books[book].key, (unsigned)bytes, (unsigned)sizeof(s_bits));
        return false;
    }
    // 先清零再读：wd_store_load_bitmap 在"没存过"时返回 false 但不保证
    // 不动缓冲，后面所有 bit 运算都假定缓冲内容是有效的。
    memset(s_bits, 0, bytes);
    if (!wd_store_load_bitmap(s_books[book].key, s_books[book].count, s_bits)) {
        // 没存过是正常路径（第一次学这本书），不是错误。
        wd_progress_clear(s_bits, s_books[book].count);
    }
    return true;
}

static void refresh_mastery_current(void) {
    if (s_book >= s_book_count) return;
    s_mastered[s_book] = wd_progress_mastered(s_bits, s_books[s_book].count);
}

// 遍历所有书。s_bits 在这里当**暂存**用（读完一本记下数字读下一本），
// 所以结束前必须把当前书重新装回来 —— 漏了这一步的后果是"标记掌握写进了
// 上一本书的位图"，而屏幕上的数字还是对的（因为刷新时又重算了一遍）。
static void refresh_masteries_all(void) {
    for (unsigned i = 0; i < s_book_count; ++i) {
        const uint32_t bytes = wd_progress_bitmap_bytes(s_books[i].count);
        if (bytes > sizeof(s_bits)) {
            s_mastered[i] = 0;
            continue;
        }
        memset(s_bits, 0, bytes);
        if (!wd_store_load_bitmap(s_books[i].key, s_books[i].count, s_bits)) {
            wd_progress_clear(s_bits, s_books[i].count);
        }
        s_mastered[i] = wd_progress_mastered(s_bits, s_books[i].count);
    }
    load_bits(s_book);
}

static void mark_mastered(uint32_t local_offset) {
    const wd_book_t *book = cur_book();
    if (!book || local_offset >= book->count) return;
    // 自己是自己的最后一关：s_bits 装得下当前书才允许写。enter_book 已经拦过
    // 一次，但"标记掌握"是会落盘的那一步，多一道判断比多一个坏位图便宜。
    if (wd_progress_bitmap_bytes(book->count) > sizeof(s_bits)) {
        ESP_LOGE(TAG, "书 %s 的位图超过缓冲 —— 拒绝标记掌握", book->key);
        return;
    }
    if (wd_progress_get(s_bits, book->count, local_offset)) return;

    wd_progress_set(s_bits, book->count, local_offset, true);
    // 立刻写：位图是累积状态，丢了就是真的丢了（断点丢了只是回到几页前）。
    if (!wd_store_save_bitmap(book->key, s_bits, book->count)) {
        ESP_LOGW(TAG, "掌握位图写入失败 —— 这次的标记重启后会消失");
    }
    refresh_mastery_current();
    push_status();
}

// 清空某一本书的掌握进度。与 mark_mastered 相反的一步，而且是**破坏性**的，
// 所以调用方必须先做一次确认（进度页「重置本册」的两次点按）。
//
// 注意：这里把 s_bits 当暂存用（和 refresh_masteries_all 一样）。如果清的不是
// 当前这本书，用完必须把当前书的位图装回来 —— 否则下一次 mark_mastered 会把
// 当前书的掌握标记写进**被清空那本书**的 NVS 键上，两本书一起坏。
static void reset_book_progress(unsigned book) {
    if (book >= s_book_count) return;
    const uint32_t bytes = wd_progress_bitmap_bytes(s_books[book].count);
    if (bytes > sizeof(s_bits)) {
        ESP_LOGE(TAG, "书 %s 的位图超过缓冲 —— 拒绝清空", s_books[book].key);
        return;
    }
    wd_progress_clear(s_bits, s_books[book].count);
    if (!wd_store_save_bitmap(s_books[book].key, s_bits, s_books[book].count)) {
        ESP_LOGE(TAG, "清空 %s 的进度写回失败 —— 这次清空没有生效", s_books[book].key);
        return;
    }
    s_mastered[book] = 0;
    if (book != s_book) load_bits(s_book);
    ESP_LOGI(TAG, "已清空 %s 的掌握进度", s_books[book].key);
}

// ---------------------------------------------------------------------------
// 断点
// ---------------------------------------------------------------------------

static void save_resume(void) {
    wd_resume_t resume;
    resume.content_version = s_blob.version;
    resume.book_index = s_book;
    resume.offset = s_offset;
    resume.seed = s_seed;
    if (!wd_store_save_resume(&resume)) {
        ESP_LOGW(TAG, "断点保存失败（进度仍在内存里，重启会回到上次成功的断点）");
        return;
    }
    s_resume_dirty = false;
}

static void mark_resume_dirty(void) { s_resume_dirty = true; }

// ---------------------------------------------------------------------------
// 各屏的显示
// ---------------------------------------------------------------------------

static const char *audio_note(void) {
    switch (wd_audio_state()) {
        case WD_AUDIO_LOADING: return "加载";
        case WD_AUDIO_PLAYING: return "播放";
        case WD_AUDIO_FAILED: return "失败";
        default: return "";
    }
}

static void show_boot(void) {
    if (s_ready) {
        // 配网门户开着。热点名与网址由 wd_ui 画在同一个区块里。
        wd_ui_show_boot("正在配网", "手机连上热点后打开下面的网址", s_ap_ssid);
        wd_ui_set_hint("配好后自动回到学习");
    } else {
        // 词库不可用。detail 里带着 wd_content_open 给出的人话，
        // 因为"刷错文件"和"生成器有 bug"指向完全不同的排查方向。
        wd_ui_show_boot("词库不可用", s_boot_reason, s_ap_ssid[0] ? s_ap_ssid : NULL);
        wd_ui_set_hint("点下方按钮重新配网");
    }
}

static void show_books(void) {
    wd_ui_show_books(s_books, s_mastered, s_book_count);
    wd_ui_set_hint("点书开始学习");
}

static void show_progress(void) {
    if (s_book_count == 0) return;
    wd_ui_show_progress(s_books, s_mastered, s_book_count, s_reset_armed);

    // 提示条说明「重置本册」确认到哪一步了。
    if (s_reset_armed) {
        // 48 = WD_UI_HINT_CAP。书名长度是内容侧的，不该在这里硬算 ——
        // 直接按提示条的容量给。
        char hint[48];
        const wd_book_t *cur = cur_book();
        snprintf(hint, sizeof(hint), "再点一次清空%s进度", cur ? cur->short_name : "");
        wd_ui_set_hint(hint);
    } else {
        wd_ui_set_hint("点书继续学 · 点设置进设置");
    }
}

// 只重画词条页。发音状态变了走这里，**不走** word_load ——
// word_load 里有 wd_audio_stop()，那样每 500ms 的 tick 都会把正在播的发音掐掉。
static void word_redraw(void) {
    if (s_mode != WD_MODE_WORD) return;
    wd_ui_show_word(wd_content_field(&s_word, WD_FIELD_WORD),
                    wd_content_field(&s_word, WD_FIELD_IPA),
                    wd_content_field(&s_word, WD_FIELD_TRANS),
                    wd_content_field(&s_word, WD_FIELD_SENT_EN),
                    wd_content_field(&s_word, WD_FIELD_SENT_CN), s_side,
                    audio_note());
    wd_ui_set_hint(s_side == WD_SIDE_MEANING ? "点卡片看例句 · 发音听读音"
                                             : "点记住了打卡 · 点卡片回释义");
}

// 按 s_offset 取词、切换面、重画。会先停掉正在播的发音 ——
// 用户已经翻到下一个词了，再放完上一个词只会让人困惑（见 wd_audio.h）。
static void word_load(wd_side_t side) {
    const wd_book_t *book = cur_book();
    if (!book || book->count == 0) return;
    if (s_offset >= book->count) s_offset = 0;
    if (!wd_content_get_in_book(&s_blob, s_book, s_offset, &s_word)) {
        ESP_LOGW(TAG, "取词失败：书 %u 偏移 %u", s_book, (unsigned)s_offset);
        return;
    }
    s_side = side;
    wd_audio_stop();
    s_audio_state_shown = (int)WD_AUDIO_IDLE;
    word_redraw();
}

// 翻面**不停发音**：用户可能边听读音边看例句。走 word_redraw 而不是 word_load。
static void word_flip(void) {
    s_side = (s_side == WD_SIDE_MEANING) ? WD_SIDE_EXAMPLE : WD_SIDE_MEANING;
    word_redraw();
}

static void show_quiz(void) {
    const bool usable = wd_quiz_ready(&s_quiz) || wd_quiz_answered(&s_quiz);

    const char *choices[WD_QUIZ_CHOICES] = {""};
    const char *meaning = "";
    const char *answer_word = "";
    const char *answer_ipa = "";
    if (usable) {
        for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
            wd_word_t choice;
            if (wd_content_get_in_book(&s_blob, s_book, s_quiz.choice[i], &choice)) {
                choices[i] = wd_content_field(&choice, WD_FIELD_WORD);
            }
        }
        wd_word_t answer;
        if (wd_content_get_in_book(&s_blob, s_book, s_quiz.answer_offset, &answer)) {
            meaning = wd_content_field(&answer, WD_FIELD_TRANS);
            answer_word = wd_content_field(&answer, WD_FIELD_WORD);
            answer_ipa = wd_content_field(&answer, WD_FIELD_IPA);
        }
    }

    // 不可用时也把 &s_quiz 传下去（它的 phase 是 IDLE），让 UI 走"题目不可用"
    // 那一支 —— 这正是 wd_quiz 里给 IDLE 一个显式状态的理由。
    wd_ui_show_quiz(meaning, answer_word, answer_ipa, choices, &s_quiz);
    wd_ui_set_hint(wd_quiz_answered(&s_quiz) ? "点下方答对结果继续" : "点选项作答");
}

static void show_settings(void) {
    wd_ui_show_settings(s_set_page, s_volume, s_brightness, s_timeout);
    switch (s_set_page) {
        case WD_SETTINGS_MENU:
            wd_ui_set_hint("点按进入");
            break;
        case WD_SETTINGS_WIFI:
            wd_ui_set_hint("点开始配网");
            break;
        case WD_SETTINGS_ABOUT:
            wd_ui_set_hint("");
            break;
        case WD_SETTINGS_VOLUME:
        case WD_SETTINGS_BRIGHTNESS:
        case WD_SETTINGS_TIMEOUT:
        default:
            // 步进按钮画的是 ASCII 的 - 与 +（64px 档没有全角符号）。
            wd_ui_set_hint("点 - + 调节");
            break;
    }
}

// ---------------------------------------------------------------------------
// 模式切换
// ---------------------------------------------------------------------------

static void enter_book(unsigned index) {
    if (index >= s_book_count) return;
    const bool switched = (index != s_book);
    if (switched) {
        // 先装位图、成功了再改 s_book。反过来写（先改 s_book 再装）在 load_bits
        // 失败时会把 s_bits 留成**上一本书**的内容，而 s_book 已是新书 ——
        // 之后 mark_mastered 就把进度写到另一本书的键上了，两本书一起坏。
        if (!load_bits(index)) return;
        s_book = index;
        // 接着上次学到的地方：第一本未掌握的词。全掌握时从 0 开始（复习）。
        const uint32_t next =
            wd_progress_next_unmastered(s_bits, s_books[s_book].count, 0);
        s_offset = (next < s_books[s_book].count) ? next : 0;
        wd_store_save_book(s_books[s_book].key);
    }
    s_mode = WD_MODE_WORD;
    word_load(WD_SIDE_MEANING);
    push_status();
    mark_resume_dirty();
}

static bool next_question(void) {
    const wd_book_t *book = cur_book();
    if (!book || book->count == 0) return false;

    // 优先考没掌握的词 —— 已经会了的词再考一遍对学习没有帮助。
    // 全都掌握了就顺着往下出题（复习）。
    uint32_t candidate =
        wd_progress_next_unmastered(s_bits, book->count, (s_offset + 1) % book->count);
    if (candidate >= book->count) candidate = (s_offset + 1) % book->count;
    s_offset = candidate;

    // 种子每出一题换一次，同时**存进断点**：重启后回到这一题要能重现同一道题
    // （包括四个选项的顺序），否则用户会以为自己记错了。
    s_seed = s_seed * 1664525u + 1013904223u;
    if (s_seed == 0) s_seed = 1;
    mark_resume_dirty();

    if (!wd_quiz_start(&s_quiz, &s_blob, s_book, candidate, s_seed)) {
        ESP_LOGW(TAG, "出题失败（%s）—— 内容侧断言过每本书至少 4 种释义，"
                      "走到这里说明词库有问题",
                 book->key);
        return false;
    }
    return true;
}

static void enter_quiz(void) {
    s_mode = WD_MODE_QUIZ;
    next_question();
    show_quiz();
    push_status();
}

static void back_to_books(void) {
    s_mode = WD_MODE_BOOKS;
    wd_audio_stop();
    show_books();
    push_status();
    save_resume();
}

static void back_to_settings_menu(void) {
    s_set_page = WD_SETTINGS_MENU;
    show_settings();
}

// 顶栏「设置」芯片与进度页底部的「设置」共用这个入口：记住来路，返回时
// 回到进来前那一屏。音量是用户最常改的一项，所以入口必须在任何一屏都一步可达。
static void open_settings(void) {
    if (!s_ready || s_mode == WD_MODE_SETTINGS) return;
    s_settings_origin = s_mode;
    s_set_page = WD_SETTINGS_MENU;
    s_mode = WD_MODE_SETTINGS;
    wd_audio_stop();
    show_settings();
    push_status();
}

static void close_settings(void) {
    s_set_page = WD_SETTINGS_MENU;
    switch (s_settings_origin) {
        case WD_MODE_WORD:
            s_mode = WD_MODE_WORD;
            s_audio_state_shown = (int)wd_audio_state();
            word_redraw();
            push_status();
            break;
        case WD_MODE_QUIZ:
            s_mode = WD_MODE_QUIZ;
            show_quiz();
            push_status();
            break;
        case WD_MODE_PROGRESS:
            s_mode = WD_MODE_PROGRESS;
            show_progress();
            push_status();
            break;
        case WD_MODE_BOOKS:
        default:
            back_to_books();
            break;
    }
}

// ---------------------------------------------------------------------------
// 触摸事件
// ---------------------------------------------------------------------------

static void on_evt_boot(void) {
    if (s_config_ap_hook) s_config_ap_hook();
}

static void on_evt_books_tap(unsigned row) {
    if (!s_ready) return;
    if (row < s_book_count) {
        enter_book(row);
    } else if (row == s_book_count) {
        enter_quiz();
    } else {
        s_mode = WD_MODE_PROGRESS;
        show_progress();
    }
}

static void on_evt_progress_tap(unsigned row) {
    if (!s_ready || s_book_count == 0) return;

    if (row < s_book_count) {          // 某本书 → 继续学
        s_reset_armed = false;
        enter_book(row);
        return;
    }
    if (row == s_book_count) {         // 「重置本册」= 清空**当前在学**那本
        if (!s_reset_armed) {
            s_reset_armed = true;
            show_progress();           // 提示条会换成"再点一次清空 XX"
            return;
        }
        s_reset_armed = false;
        reset_book_progress(s_book);
        // 位图清了，"学到哪了"也就失去意义 —— 顺手归零。不归零的话从进度页
        // 再次进入这本书会停在清空之前的位置，看起来像"没清干净"。
        s_offset = 0;
        s_side = WD_SIDE_MEANING;
        mark_resume_dirty();
        show_progress();
        return;
    }
    s_reset_armed = false;             // 「设置」
    open_settings();
}

static void word_step(int delta) {
    const wd_book_t *book = cur_book();
    if (!book || book->count == 0) return;
    // 换词时一律回到 A 面：留在 B 面会让"这一面的例句属于哪个词"
    // 变成一个要靠记忆的问题。
    const uint32_t step = (delta < 0) ? (uint32_t)(-delta) : (uint32_t)delta;
    s_offset = (s_offset + book->count + (delta < 0 ? book->count - step : step)) %
               book->count;
    word_load(WD_SIDE_MEANING);
    mark_resume_dirty();
}

static void on_evt_word(wd_ui_evt_t evt) {
    const wd_book_t *book = cur_book();
    if (!book || book->count == 0) return;

    switch (evt) {
        case WD_UI_EVT_WORD_PREV:
            word_step(-1);
            break;
        case WD_UI_EVT_WORD_NEXT:
            word_step(+1);
            break;
        case WD_UI_EVT_WORD_SPEAK:
            // 只重播**当前词**的发音，不动位置也不翻面。
            wd_audio_play_word(wd_content_field(&s_word, WD_FIELD_WORD));
            break;
        case WD_UI_EVT_WORD_SPEAK_SENTENCE: {
            const char *sentence = wd_content_field(&s_word, WD_FIELD_SENT_EN);
            // 485 个词没有例句，按钮照样可点 —— 走到这里给一行日志，比
            // "点了没反应"更容易排查。
            if (!sentence || !sentence[0]) {
                ESP_LOGI(TAG, "当前词没有英文例句");
                break;
            }
            wd_audio_play_sentence(sentence);
            break;
        }
        case WD_UI_EVT_WORD_FLIP:
            word_flip();
            break;
        case WD_UI_EVT_WORD_MASTER: {
            // B 面「记住了」：标记已掌握并跳到下一个未掌握的词。这是本应用的
            // 主循环：看释义 → 看例句 → 我记住了 → 下一个。
            mark_mastered(s_offset);
            uint32_t next =
                wd_progress_next_unmastered(s_bits, book->count,
                                            (s_offset + 1) % book->count);
            if (next >= book->count) {
                ESP_LOGI(TAG, "%s 全部掌握", book->key);
                next = s_offset;   // 没有下一个未掌握的词，停在原地
            }
            s_offset = next;
            word_load(WD_SIDE_MEANING);
            mark_resume_dirty();
            break;
        }
        default:
            break;
    }
}

static void on_evt_quiz(wd_ui_evt_t evt, int arg) {
    if (evt == WD_UI_EVT_QUIZ_PICK) {
        if (!wd_quiz_ready(&s_quiz)) return;   // 已作答后再点选项 = 无效点按
        if (arg < 0 || arg >= (int)WD_QUIZ_CHOICES) return;
        const bool correct = wd_quiz_answer(&s_quiz, (unsigned)arg);
        if (correct) mark_mastered(s_quiz.answer_offset);
        show_quiz();
        return;
    }
    // QUIZ_NEXT：作答后点结果区才有效；没作答时 UI 也发得出这个事件
    // （结果区常驻可点），这里挡住。
    if (!wd_quiz_answered(&s_quiz)) return;
    next_question();
    show_quiz();
}

static void on_evt_settings(wd_ui_evt_t evt, int arg) {
    if (evt == WD_UI_EVT_SET_MENU) {
        if (arg < 0) return;
        const wd_settings_page_t page = wd_settings_page_of_row((unsigned)arg);
        // 菜单只有 5 项（音量/亮度/息屏/配网/关于），返回固定走顶栏「‹返回」。
        // 越界点按直接忽略 —— 这里不能再"当作返回"，否则误触会把用户弹出去。
        if (page >= WD_SETTINGS_PAGE_COUNT) return;
        s_set_page = page;
        show_settings();
        return;
    }
    if (evt == WD_UI_EVT_SET_WIFI) {
        if (s_config_ap_hook) s_config_ap_hook();
        return;
    }
    if (evt == WD_UI_EVT_SET_STEP) {
        const int delta = (arg < 0) ? -1 : 1;
        switch (s_set_page) {
            case WD_SETTINGS_VOLUME: {
                // 音量是 0..100 的连续量，步进 5 最顺手。夹紧而不是取模 ——
                // 音量从 0 直接跳到 100 会吓人一跳。
                int volume = (int)s_volume + delta * 5;
                if (volume < 0) volume = 0;
                if (volume > 100) volume = 100;
                s_volume = (uint8_t)volume;
                wd_audio_set_volume(s_volume);
                wd_store_save_volume(s_volume);
                break;
            }
            case WD_SETTINGS_BRIGHTNESS: {
                const size_t tiers = wd_display_brightness_tier_count();
                if (tiers) {
                    const size_t cur = wd_display_brightness_selection();
                    const size_t next =
                        (delta > 0) ? (cur + 1) % tiers : (cur + tiers - 1) % tiers;
                    wd_display_set_brightness_selection(next);
                    s_brightness = wd_display_brightness();
                }
                break;
            }
            case WD_SETTINGS_TIMEOUT: {
                const size_t tiers = wd_display_timeout_tier_count();
                if (tiers) {
                    const size_t cur = wd_display_timeout_selection();
                    const size_t next =
                        (delta > 0) ? (cur + 1) % tiers : (cur + tiers - 1) % tiers;
                    wd_display_set_timeout_selection(next);
                    s_timeout = wd_display_timeout_seconds();
                }
                break;
            }
            default:
                return;
        }
        show_settings();
    }
}

static void on_evt_back(void) {
    switch (s_mode) {
        case WD_MODE_WORD:
        case WD_MODE_QUIZ:
            back_to_books();
            break;
        case WD_MODE_PROGRESS:
            s_reset_armed = false;
            back_to_books();
            break;
        case WD_MODE_SETTINGS:
            if (s_set_page == WD_SETTINGS_MENU) {
                close_settings();
            } else {
                back_to_settings_menu();
            }
            break;
        case WD_MODE_BOOT:
        case WD_MODE_BOOKS:
        default:
            break;   // 根屏没有返回
    }
}

void wd_app_on_ui_evt(wd_ui_evt_t evt, int arg) {
    app_lock();
    switch (evt) {
        case WD_UI_EVT_BOOT_CONFIG:
            if (s_mode == WD_MODE_BOOT) on_evt_boot();
            break;
        case WD_UI_EVT_BOOKS_TAP:
            if (s_mode == WD_MODE_BOOKS) on_evt_books_tap((unsigned)arg);
            break;
        case WD_UI_EVT_PROGRESS_TAP:
            if (s_mode == WD_MODE_PROGRESS) on_evt_progress_tap((unsigned)arg);
            break;
        case WD_UI_EVT_WORD_PREV:
        case WD_UI_EVT_WORD_SPEAK:
        case WD_UI_EVT_WORD_SPEAK_SENTENCE:
        case WD_UI_EVT_WORD_FLIP:
        case WD_UI_EVT_WORD_MASTER:
        case WD_UI_EVT_WORD_NEXT:
            if (s_mode == WD_MODE_WORD) on_evt_word(evt);
            break;
        case WD_UI_EVT_QUIZ_PICK:
        case WD_UI_EVT_QUIZ_NEXT:
            if (s_mode == WD_MODE_QUIZ) on_evt_quiz(evt, arg);
            break;
        case WD_UI_EVT_SET_MENU:
        case WD_UI_EVT_SET_STEP:
        case WD_UI_EVT_SET_WIFI:
            if (s_mode == WD_MODE_SETTINGS) on_evt_settings(evt, arg);
            break;
        case WD_UI_EVT_SETTINGS_OPEN:
            open_settings();
            break;
        case WD_UI_EVT_BACK:
            on_evt_back();
            break;
        case WD_UI_EVT_NONE:
        default:
            break;
    }
    app_unlock();
}

// ---------------------------------------------------------------------------
// 周期任务
// ---------------------------------------------------------------------------

void wd_app_tick(void) {
    app_lock();
    ++s_tick;
    if (s_tick == 0) s_tick = 1;   // 回绕到 0 会让下面所有取模判断同时命中

    // 发音状态变了才重画。不判这一下的话，词条页每 500ms 就被重画一次 ——
    // 没有任何视觉变化，却每次都走一遍折行与文本重排。
    if (s_mode == WD_MODE_WORD) {
        const int state = (int)wd_audio_state();
        if (state != s_audio_state_shown) {
            s_audio_state_shown = state;
            word_redraw();
        }
    }

    // 断点定期落盘。点按时只置脏标记：每点一次就擦写一次 Flash，
    // 学一小时就是几千次写。
    if (s_resume_dirty && s_tick % WD_RESUME_TICKS == 0) save_resume();
    app_unlock();
}

// ---------------------------------------------------------------------------
// 注入与初始化
// ---------------------------------------------------------------------------

void wd_app_set_network(bool connected) {
    app_lock();
    if (s_connected == connected) {
        app_unlock();
        return;
    }
    s_connected = connected;
    wd_audio_set_network(connected);
    // 联网状态只在状态栏上体现（Wi-Fi 指示条），不切屏：词库在本地，
    // 没网也能查词，切到配网页反而是错的。
    if (!connected) wd_audio_stop();
    push_status();
    app_unlock();
}

void wd_app_set_ap_ssid(const char *ssid) {
    app_lock();
    const bool had = (s_ap_ssid[0] != '\0');
    if (ssid) {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", ssid);
    } else {
        s_ap_ssid[0] = '\0';
    }
    const bool has = (s_ap_ssid[0] != '\0');
    // 热点名没变就不动屏幕：ConfigModeEnter 与 ConfigModeExit 之外，
    // Wi-Fi 侧还可能重复上报同一个值。
    if (has == had) {
        app_unlock();
        return;
    }

    if (has) {
        // 门户开着就进配网页。**不看 s_ready** —— 词库坏掉时更需要把热点名与
        // 网址显示出来（那时屏幕上只有一句"词库不可用"，用户无从下手）。
        if (s_mode != WD_MODE_BOOT) {
            wd_audio_stop();
            s_mode = WD_MODE_BOOT;
        }
        show_boot();
        app_unlock();
        return;
    }

    // 门户关了：词库可读就回主界面；不可读时停在错误页（那才是要给用户看的）。
    if (s_ready && s_mode == WD_MODE_BOOT) back_to_books();
    app_unlock();
}

void wd_app_set_config_ap_hook(void (*hook)(void)) { s_config_ap_hook = hook; }

void wd_app_init(void) {
    // 锁要在任何一条任务起来之前建好（main.cc 是"先 wd_app_init 再建任务"）。
    // 建不出来也继续跑：单任务时不会出问题，只是之后跨任务就有风险 —— 而
    // "没有界面"比"理论上的竞态"严重得多。
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (!s_lock) ESP_LOGE(TAG, "状态锁创建失败 —— 跨任务的屏幕切换可能错乱");
    }
    app_lock();

    memset(s_books, 0, sizeof(s_books));
    memset(s_mastered, 0, sizeof(s_mastered));
    memset(s_bits, 0, sizeof(s_bits));
    s_book = 0;
    s_offset = 0;
    s_seed = 1;
    s_ready = false;
    s_mode = WD_MODE_BOOT;
    s_reset_armed = false;      // 热重启不能带着上一次的"待确认"状态
    s_set_page = WD_SETTINGS_MENU;
    s_settings_origin = WD_MODE_BOOKS;
    s_connected = false;
    s_resume_dirty = false;
    s_tick = 0;
    s_audio_state_shown = -1;
    // 注意：**不清空 s_ap_ssid**。main.cc 有可能在 wd_app_init 之前就收到了
    // ConfigModeEnter（配网门户先起来、界面后初始化），那时注入进来的 SSID
    // 必须留到下面决定首屏用哪一屏。

    if (!wd_ui_init()) {
        // 字库不可用时继续跑：停机的设备用户无从下手，而屏幕上的方框
        // 配上串口里这行原因，至少是"看得见的坏"。
        ESP_LOGE(TAG, "界面初始化失败（字库不完整）—— 继续启动，屏幕可能显示方框");
    }
    wd_ui_set_evt_cb(wd_app_on_ui_evt);

    char reason[96];
    reason[0] = '\0';
    if (!wd_content_open(&s_blob, reason, sizeof(reason))) {
        ESP_LOGE(TAG, "词库不可用：%s", reason);
        snprintf(s_boot_reason, sizeof(s_boot_reason), "%s", reason);
        show_boot();
        push_status();
        app_unlock();
        return;
    }
    ESP_LOGI(TAG, "词库就绪：%u 本 / %u 词，内容版本 %08x",
             (unsigned)s_blob.book_count, (unsigned)s_blob.word_count,
             (unsigned)s_blob.version);

    wd_store_init(s_blob.version);

    for (unsigned i = 0; i < s_blob.book_count && i < WD_MAX_BOOKS; ++i) {
        if (wd_manifest_book_at(&s_blob, i, &s_books[s_book_count])) ++s_book_count;
    }
    if (s_book_count == 0) {
        // 走到这里说明 blob 的头部自洽（wd_content_open 过了）但书表是空的。
        // 这一支也要给人话，否则屏幕上只有标题、没有原因 —— 用户看到的是
        // 一个"什么都没有"的错误页。
        ESP_LOGE(TAG, "书表为空 —— 词库文件形状不对");
        snprintf(s_boot_reason, sizeof(s_boot_reason), "%s", "词库文件里一本书都没有");
        show_boot();
        push_status();
        app_unlock();
        return;
    }

    // 先恢复"上次在学哪本书"，再恢复断点 —— 断点里的 book_index 更权威
    // （它和 offset/seed 是同一次写入的），两处不一致时以断点为准。
    char book_key[WD_BOOK_KEY_MAX];
    book_key[0] = '\0';
    if (wd_store_load_book(book_key, sizeof(book_key)) && book_key[0]) {
        const int index = wd_manifest_find_book(&s_blob, book_key);
        if (index >= 0 && (unsigned)index < s_book_count) s_book = (unsigned)index;
    }

    wd_resume_t resume;
    const wd_resume_err_t err = wd_store_load_resume(&s_blob, &resume);
    if (err == WD_RESUME_OK && resume.book_index < s_book_count) {
        s_book = resume.book_index;
        s_offset = resume.offset;
        s_seed = resume.seed ? resume.seed : 1;
    } else if (err == WD_RESUME_ERR_VERSION) {
        // 正常路径：词库换过了，旧位置指向的是另一个词。从头开始。
        ESP_LOGI(TAG, "词库版本变了，从第一本开始");
    } else if (err != WD_RESUME_ERR_MAGIC) {
        // MAGIC 不符 = 从没存过（NVS 里是空的），不必报。
        ESP_LOGW(TAG, "断点不可用：%s", wd_resume_err_text(err));
    }

    s_volume = wd_store_load_volume();
    wd_audio_set_volume(s_volume);
    s_brightness = wd_display_brightness();
    s_timeout = wd_display_timeout_seconds();

    s_ready = true;
    refresh_masteries_all();
    wd_store_save_book(s_books[s_book].key);

    s_mode = s_ap_ssid[0] ? WD_MODE_BOOT : WD_MODE_BOOKS;
    if (s_mode == WD_MODE_BOOT) {
        show_boot();
    } else {
        show_books();
    }
    push_status();
    ESP_LOGI(TAG, "就绪：书 %s，书内 %u/%u，音量 %u",
             s_books[s_book].key, (unsigned)(s_offset + 1),
             (unsigned)s_books[s_book].count, (unsigned)s_volume);
    app_unlock();
}
