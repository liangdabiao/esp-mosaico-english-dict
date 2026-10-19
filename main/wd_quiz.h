// main/wd_quiz.h —— 四选一自测的纯状态机（不依赖 ESP-IDF / LVGL / 随机数库）。
//
// 为什么干扰项要"长度相近的异词"：
//   如果干扰项是从整本书里随机抓的，选项长度会一眼看出差别 —— 长选项几乎总是
//   那个生僻词，短选项是常用词。玩两页就能靠长度猜对，自测就失去意义了。
//   把候选限制在「与答案词长差 ≤ 2」的范围内，四个选项看上去才像同类词。
//
// 为什么**不用 rand()**：题目必须可复现。
//   ① 宿主测试要固定向量 —— 同一 (书, 词序, 种子) 必须每次产出同一道题，
//      否则断言无从下手；
//   ② 设备上"回到上一题"或"重听这一题"时，重新出题如果变了，用户会以为
//      自己记错了；
//   ③ newlib 的 rand() 在 ESP-IDF 上还要考虑线程安全与是否被别处 srand 过。
//   自己带一个 32 位 xorshift 状态，种子存进进度里，就能精确复现任何一题。
#pragma once

#include "wd_content.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WD_QUIZ_CHOICES 4

typedef enum {
    // IDLE 必须存在，而且必须是 0。
    //
    // 出题失败时（书太小、释义种类不足）状态会被清零，若没有 IDLE，
    // 清零后的状态恰好等于"已出好题且答对会落在槽位 0"—— 于是 UI 只要漏检
    // wd_quiz_start 的返回值，用户点第一个选项就会被判成对的。
    // 这是一个"错的教学"级别的 bug，所以给它一个显式状态而不是靠调用方小心。
    WD_QUIZ_PHASE_IDLE = 0,
    WD_QUIZ_PHASE_ASK,       // 已出题，等作答
    WD_QUIZ_PHASE_CORRECT,
    WD_QUIZ_PHASE_WRONG,
} wd_quiz_phase_t;

typedef struct {
    unsigned book;                          // 书序号
    uint32_t answer_offset;                 // 正确答案的**书内**偏移
    uint32_t choice[WD_QUIZ_CHOICES];       // 四个选项的书内偏移
    unsigned answer_slot;                   // 正确项落在哪个槽位（0..3）
    unsigned picked;                        // 用户选的槽位；未作答时 = WD_QUIZ_CHOICES
    wd_quiz_phase_t phase;
    uint32_t rng;                           // xorshift 状态，非 0
} wd_quiz_t;

// 出题。seed 相同 + 同一个词 ⇒ 题目（含选项顺序）完全相同。
//
// 返回 false 表示这本书凑不出四个不同的选项（词太少，或者全书的释义只有
// 一两种写法）。这**不该发生**：tools/wordbook/content.py 的 validate() 已经
// 断言每本书至少有 4 种不同释义。所以 false 意味着内容文件损坏或校验被绕过了，
// 调用方应当把这一题跳过而不是显示一道只有两个选项的题。
bool wd_quiz_start(wd_quiz_t *quiz, const wd_blob_t *blob, unsigned book,
                   uint32_t answer_offset, uint32_t seed);

// 作答。
//
// 三种返回 false 的情形，语义不同但都不改变状态：
//   * slot 越界（调用方的 bug，**优先于**下面的"重复作答"判断 —— 参数校验
//     永远先做，这样已答对的题传一个越界槽位也会诚实地报 false）
//   * 还没出题（phase == IDLE）
//   * 已经答过且上次答错
//
// 反复点击不会把"答错"刷成"答对"，也不会把"答对"刷成"答错"。
bool wd_quiz_answer(wd_quiz_t *quiz, unsigned slot);

// 正确项落在哪个槽位（UI 要标绿/标红，需要知道位置）。
static inline unsigned wd_quiz_slot_of_answer(const wd_quiz_t *quiz) {
    return quiz ? quiz->answer_slot : 0;
}

// 引导当前这一题的答题进度（0..3 里第几个）：从选定候选进入下一题用。
static inline bool wd_quiz_answered(const wd_quiz_t *quiz) {
    return quiz && (quiz->phase == WD_QUIZ_PHASE_CORRECT ||
                    quiz->phase == WD_QUIZ_PHASE_WRONG);
}

static inline bool wd_quiz_ready(const wd_quiz_t *quiz) {
    return quiz && quiz->phase == WD_QUIZ_PHASE_ASK;
}

#ifdef __cplusplus
}  // extern "C"
#endif
