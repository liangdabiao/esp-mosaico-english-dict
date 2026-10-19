// main/wd_quiz.c —— 四选一状态机（纯逻辑，可在宿主机编译）。
#include "wd_quiz.h"

#include <string.h>

// xorshift32。选它而不是 LCG 的理由：LCG 的低位周期很短，而这里要做
// `next % seen`（seen 是逐次递增的候选计数，从 1 到几千），低位偏斜会直接
// 变成"前几个词被选中的概率明显更高"—— 也就是干扰项总从书的开头取。
// xorshift32 三行移位异或，同样便宜，但每个比特都参与。
static inline uint32_t rng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// 在书里挑一个干扰项。
//
// 算法：**蓄水池抽样**（reservoir sampling），一遍扫描、零内存分配。
// 为什么不"先收集候选到数组再随机挑"：最大的一本书 4533 词，若把候选
// （下标 + 长度 + 释义指针）收集起来就是几十 KB —— 设备端总共 400 KB SRAM，
// 还要跟 LVGL 和音频抢，为一道四选一题占用几十 KB 不划算。
// 蓄水池只保留"当前选中的那一个"，见一个换一个的概率是 1/seen，等价于均匀随机。
//
// 长度容差**逐级放宽**（±2 → ±4 → 不限）。这跟基座里环形缓冲 16/8/4 KB 的
// 退让是同一个思路：先用最贴合的条件，实在凑不齐再退，而不是一上来就用最宽的
// 条件把出题质量拉平。实际数据里第一级几乎总能凑齐。
static bool pick_distractor(wd_quiz_t *quiz, const wd_blob_t *blob, unsigned book,
                            uint32_t answer_offset, const char *answer_trans,
                            uint32_t answer_len, const uint32_t *taken,
                            unsigned taken_count, uint32_t *out) {
    static const uint32_t kMaxDelta[] = {2u, 4u, 0xFFFFFFFFu};

    wd_book_t info;
    if (!wd_manifest_book_at(blob, book, &info)) return false;

    for (unsigned level = 0; level < 3; ++level) {
        const uint32_t limit = kMaxDelta[level];
        uint32_t chosen = 0;
        uint32_t seen = 0;
        bool found = false;

        for (uint32_t i = 0; i < info.count; ++i) {
            if (i == answer_offset) continue;

            bool duplicated = false;
            for (unsigned k = 0; k < taken_count; ++k) {
                if (taken[k] == i) { duplicated = true; break; }
            }
            if (duplicated) continue;

            wd_word_t word;
            if (!wd_content_get_in_book(blob, book, i, &word)) continue;

            // 释义与答案相同的一律排除。
            // 否则会出现两个"看起来都对"的选项，而用户选了那个被判错 ——
            // 这是自测最不能出的错，比选项少一个严重得多。
            if (strcmp(wd_content_field(&word, WD_FIELD_TRANS), answer_trans) == 0) continue;

            const uint32_t len = (uint32_t)strlen(wd_content_field(&word, WD_FIELD_WORD));
            const uint32_t delta = (len > answer_len) ? (len - answer_len)
                                                      : (answer_len - len);
            if (delta > limit) continue;

            ++seen;
            if (rng_next(&quiz->rng) % seen == 0) {
                chosen = i;
                found = true;
            }
        }

        if (found) {
            // 再确认一次它不是答案也不是已选项 —— 蓄水池的边界（seen 从 1 开始）
            // 已经在上面保证了，这里是廉价的断言，防止日后重构时条件写反。
            if (chosen == answer_offset) continue;
            *out = chosen;
            return true;
        }
    }
    return false;
}

bool wd_quiz_start(wd_quiz_t *quiz, const wd_blob_t *blob, unsigned book,
                   uint32_t answer_offset, uint32_t seed) {
    if (!quiz || !blob) return false;
    memset(quiz, 0, sizeof(*quiz));
    quiz->picked = WD_QUIZ_CHOICES;
    quiz->phase = WD_QUIZ_PHASE_IDLE;   // 任何失败路径都停在这里，不会是"像是出了一题"

    wd_book_t info;
    if (!wd_manifest_book_at(blob, book, &info)) return false;
    if (answer_offset >= info.count) return false;

    // xorshift 的状态不能为 0（0 是不动点，之后每一"随机"数都是 0）。
    // 种子由上层给（通常是内容版本 xor 词序），这里兜底成 1。
    quiz->rng = seed ? seed : 1u;
    quiz->book = book;
    quiz->answer_offset = answer_offset;

    wd_word_t answer;
    if (!wd_content_get_in_book(blob, book, answer_offset, &answer)) return false;
    const char *answer_trans = wd_content_field(&answer, WD_FIELD_TRANS);
    const uint32_t answer_len = (uint32_t)strlen(wd_content_field(&answer, WD_FIELD_WORD));

    uint32_t distractors[WD_QUIZ_CHOICES - 1];
    uint32_t taken[WD_QUIZ_CHOICES - 1];
    unsigned taken_count = 0;
    for (unsigned i = 0; i < WD_QUIZ_CHOICES - 1; ++i) {
        if (!pick_distractor(quiz, blob, book, answer_offset, answer_trans, answer_len,
                             taken, taken_count, &distractors[i])) {
            return false;  // 凑不齐 —— 见头文件里的说明，这意味着内容有问题
        }
        taken[taken_count++] = distractors[i];
    }

    // 四个值放进槽位后洗牌。洗牌必须用同一个 rng 流，且顺序固定 ——
    // 否则"同一 seed 同一题"这条约定就断了，宿主测试的固定向量会随机失败。
    uint32_t values[WD_QUIZ_CHOICES];
    values[0] = answer_offset;
    values[1] = distractors[0];
    values[2] = distractors[1];
    values[3] = distractors[2];
    for (unsigned i = WD_QUIZ_CHOICES - 1; i > 0; --i) {
        const unsigned j = rng_next(&quiz->rng) % (i + 1);
        const uint32_t tmp = values[i];
        values[i] = values[j];
        values[j] = tmp;
    }

    quiz->answer_slot = 0;
    for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
        quiz->choice[i] = values[i];
        if (values[i] == answer_offset) quiz->answer_slot = i;
    }
    quiz->phase = WD_QUIZ_PHASE_ASK;    // 只有走完全部步骤才进入"等作答"
    return true;
}

bool wd_quiz_answer(wd_quiz_t *quiz, unsigned slot) {
    if (!quiz) return false;

    // 参数校验先于状态判断：越界的槽位是调用方的 bug，任何时候都该诚实地报 false，
    // 不能因为"这题已经答过了"就把参数错误吞掉。且无论如何都不改变状态。
    if (slot >= WD_QUIZ_CHOICES) return false;

    if (quiz->phase == WD_QUIZ_PHASE_IDLE) return false;   // 根本没出题

    if (quiz->phase != WD_QUIZ_PHASE_ASK) {
        // 已经答过：返回原结果，不推进状态。连点四下不会把错的变成对的。
        return quiz->phase == WD_QUIZ_PHASE_CORRECT;
    }

    quiz->picked = slot;
    const bool correct = (slot == quiz->answer_slot);
    quiz->phase = correct ? WD_QUIZ_PHASE_CORRECT : WD_QUIZ_PHASE_WRONG;
    return correct;
}
