// tests/test_wd_quiz.c —— 四选一状态机的宿主测试。
//
// 这里钉住的是三条**硬不变量**，任何一条破了都会让自测变成错的教学：
//
//   1. 四个选项**互不相同**，且 correct 一定在里面
//   2. 每个干扰项的**释义与正确答案不同** —— 否则会出现两个"看起来都对"的选项，
//      用户选了那个被判错。这是四选一最不能犯的错。
//   3. 同一 (书, 词序, 种子) 必须产出**完全相同**的题（含选项顺序）。
//      否则"回到上一题"会变成一道新题，用户会以为自己记错了。
//
// 外加两条质量指标（不是断言，是统计）：
//   * 干扰项与答案的**词长差**应集中在 ±2 内（这是出题策略的意图）
//   * 每一道题都能凑齐四个选项（对应 content.py 里"每本书 ≥4 种释义"的断言）

#include "wd_quiz.h"
#include "wd_blob_fixture.h"
#include "wd_test.h"

#define MINI_CAP 1024

// 检查一道题的全部硬不变量。返回 false 表示这道题有问题（已记入断言）。
static void check_question(const wd_blob_t *blob, const wd_quiz_t *quiz,
                           const char *label) {
    const unsigned book = quiz->book;

    wd_word_t answer;
    if (!wd_content_get_in_book(blob, book, quiz->answer_offset, &answer)) {
        WD_EXPECT(false, "[%s] 答案 %u 取不到", label, quiz->answer_offset);
        return;
    }
    const char *answer_trans = wd_content_field(&answer, WD_FIELD_TRANS);
    const uint32_t answer_len =
        (uint32_t)strlen(wd_content_field(&answer, WD_FIELD_WORD));

    // 1. 正确项在四个选项里，且位置与 answer_slot 一致
    WD_EXPECT(quiz->answer_slot < WD_QUIZ_CHOICES, "[%s] answer_slot=%u 越界", label,
              quiz->answer_slot);
    WD_EXPECT(quiz->choice[quiz->answer_slot] == quiz->answer_offset,
              "[%s] answer_slot=%u 指向 %u，应为 %u", label, quiz->answer_slot,
              quiz->choice[quiz->answer_slot], quiz->answer_offset);

    // 2. 四个选项两两不同
    for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
        WD_EXPECT(quiz->choice[i] < 1000000u, "[%s] 选项 %u 越界", label, i);
        for (unsigned j = i + 1; j < WD_QUIZ_CHOICES; ++j) {
            WD_EXPECT(quiz->choice[i] != quiz->choice[j],
                      "[%s] 选项 %u 与 %u 相同（都是 %u）", label, i, j, quiz->choice[i]);
        }
    }

    // 3. 每个干扰项的释义与答案不同
    unsigned max_delta = 0;
    for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
        if (i == quiz->answer_slot) continue;
        wd_word_t other;
        if (!wd_content_get_in_book(blob, book, quiz->choice[i], &other)) {
            WD_EXPECT(false, "[%s] 选项 %u 取不到", label, i);
            continue;
        }
        const char *other_trans = wd_content_field(&other, WD_FIELD_TRANS);
        WD_EXPECT(strcmp(other_trans, answer_trans) != 0,
                  "[%s] 选项 %u 的释义与答案相同：\"%s\"", label, i, other_trans);

        const uint32_t len = (uint32_t)strlen(wd_content_field(&other, WD_FIELD_WORD));
        const uint32_t delta = (len > answer_len) ? (len - answer_len) : (answer_len - len);
        if (delta > max_delta) max_delta = delta;
    }

    // 把长度差也带出来，供调用方统计（不在这里断言，因为第三级退让允许任意长度）
    (void)max_delta;
}

static void test_mini(void) {
    unsigned char buf[MINI_CAP];
    const size_t total = wd_build_mini_blob(buf, sizeof(buf));
    wd_blob_t blob;
    wd_manifest_open(buf, total, &blob);

    // 迷你 blob 的 BookA 只有 2 条词，凑不出四个选项 —— 这正是"内容不够"的
    // 情形，必须**明确失败**而不是给出一道只有两个选项的题。
    wd_quiz_t quiz;
    WD_EXPECT(!wd_quiz_start(&quiz, &blob, 0, 0, 1),
              "只有 2 条词的书不该能出四选一题");
    // 失败后必须停在 IDLE。若停在 ASK，answer_slot 与 choice[0] 都是 0，
    // 调用方漏检返回值时用户点第一个选项就会被判对 —— 一个"错的教学"级别的 bug。
    WD_EXPECT(quiz.phase == WD_QUIZ_PHASE_IDLE, "出题失败后应停在 IDLE，实得 %d",
              (int)quiz.phase);
    WD_EXPECT(!wd_quiz_ready(&quiz), "出题失败后不该处于 ready");
    WD_EXPECT(!wd_quiz_answered(&quiz), "出题失败后不该算已作答");

    // 参数不合法
    WD_EXPECT(!wd_quiz_start(0, &blob, 0, 0, 1), "空 quiz 应被拒绝");
    WD_EXPECT(!wd_quiz_start(&quiz, 0, 0, 0, 1), "空 blob 应被拒绝");
    WD_EXPECT(!wd_quiz_start(&quiz, &blob, 5, 0, 1), "书序号越界应被拒绝");
    WD_EXPECT(!wd_quiz_start(&quiz, &blob, 0, 99, 1), "词序越界应被拒绝");
    WD_EXPECT(!wd_quiz_answer(&quiz, 0), "没出题就作答应返回 false");
    WD_EXPECT(!wd_quiz_answer(0, 0), "空 quiz 作答应返回 false");
}

static void test_real(const char *path) {
    size_t size = 0;
    unsigned char *data = wd_read_file(path, &size);
    if (!data) {
        printf("（跳过真实 blob 组：读不到 %s）\n", path);
        return;
    }

    wd_blob_t blob;
    const wd_manifest_err_t merr = wd_manifest_open(data, size, &blob);
    if (merr != WD_MANIFEST_OK) {
        WD_EXPECT(false, "真实 blob 打开失败：%s", wd_manifest_err_text(merr));
        free(data);
        return;
    }

    // ---- 1. 确定性与不变量：每本书抽样一批词 ----
    //
    // 全量 19513 词 × 每词 3 遍扫书（最长 4533）= 2.6 亿次字段读，太慢。
    // 每本书取 120 个均匀分布的位置，已经足以覆盖各种词长与释义分布。
    const unsigned kSamplesPerBook = 120;
    unsigned total_questions = 0;
    unsigned delta_hist[9] = {0};   // 词长差 0..8+ 的分布
    unsigned long long delta_sum = 0;

    for (unsigned b = 0; b < blob.book_count; ++b) {
        wd_book_t book;
        wd_manifest_book_at(&blob, b, &book);
        const uint32_t step = book.count / kSamplesPerBook;
        const uint32_t stride = step ? step : 1;

        for (uint32_t offset = 0; offset < book.count; offset += stride) {
            wd_quiz_t q1, q2;
            const uint32_t seed = 0x9e3779b9u ^ (offset * 2654435761u);

            if (!wd_quiz_start(&q1, &blob, b, offset, seed)) {
                WD_EXPECT(false, "书 %u 第 %u 条出不了题（释义种类不足？）", b, offset);
                continue;
            }
            ++total_questions;

            // 同一 seed 必须重现同一道题
            WD_EXPECT(wd_quiz_start(&q2, &blob, b, offset, seed), "重复出题失败");
            WD_EXPECT(q1.answer_slot == q2.answer_slot,
                      "同一 seed 的 answer_slot 不同：%u vs %u", q1.answer_slot,
                      q2.answer_slot);
            bool same = true;
            for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
                if (q1.choice[i] != q2.choice[i]) same = false;
            }
            WD_EXPECT(same, "同一 seed 的选项不同");

            char label[64];
            snprintf(label, sizeof(label), "book%u/off%u", b, offset);
            check_question(&blob, &q1, label);

            // 收集长度差分布
            wd_word_t answer;
            wd_content_get_in_book(&blob, b, offset, &answer);
            const uint32_t answer_len =
                (uint32_t)strlen(wd_content_field(&answer, WD_FIELD_WORD));
            for (unsigned i = 0; i < WD_QUIZ_CHOICES; ++i) {
                if (i == q1.answer_slot) continue;
                wd_word_t other;
                if (!wd_content_get_in_book(&blob, b, q1.choice[i], &other)) continue;
                const uint32_t len =
                    (uint32_t)strlen(wd_content_field(&other, WD_FIELD_WORD));
                const uint32_t delta =
                    (len > answer_len) ? (len - answer_len) : (answer_len - len);
                delta_sum += delta;
                delta_hist[delta > 8 ? 8 : delta]++;
            }

            // ---- 作答语义 ----
            wd_quiz_t q3;
            wd_quiz_start(&q3, &blob, b, offset, seed);
            const unsigned correct_slot = q3.answer_slot;
            const unsigned wrong_slot = (correct_slot + 1) % WD_QUIZ_CHOICES;

            WD_EXPECT(wd_quiz_answer(&q3, correct_slot), "选正确答案应返回 true");
            WD_EXPECT(q3.phase == WD_QUIZ_PHASE_CORRECT, "答对后 phase 应为 CORRECT");
            WD_EXPECT(q3.picked == correct_slot, "picked 应为 %u", correct_slot);
            // 答过之后再点别的，结果不变
            WD_EXPECT(wd_quiz_answer(&q3, wrong_slot), "答对后重复作答应保持 true");
            WD_EXPECT(q3.phase == WD_QUIZ_PHASE_CORRECT, "重复作答不应改变 phase");
            WD_EXPECT(!wd_quiz_answer(&q3, WD_QUIZ_CHOICES), "越界槽位应返回 false");
            WD_EXPECT(q3.phase == WD_QUIZ_PHASE_CORRECT, "越界槽位不应改变 phase");

            wd_quiz_t q4;
            wd_quiz_start(&q4, &blob, b, offset, seed);
            WD_EXPECT(!wd_quiz_answer(&q4, wrong_slot), "选错误答案应返回 false");
            WD_EXPECT(q4.phase == WD_QUIZ_PHASE_WRONG, "答错后 phase 应为 WRONG");
            // 答错后再点正确的，也不能"刷"成对
            WD_EXPECT(!wd_quiz_answer(&q4, q4.answer_slot), "答错后重复作答应保持 false");
            WD_EXPECT(q4.phase == WD_QUIZ_PHASE_WRONG, "答错后 phase 不应改变");
        }
    }

    // ---- 2. 不同 seed 应该给出不同的题（统计，不是断言）----
    unsigned varied = 0;
    const unsigned kVariety = 60;
    for (unsigned i = 0; i < kVariety; ++i) {
        wd_quiz_t a, b2;
        wd_quiz_start(&a, &blob, 0, 10, 1000u + i);
        wd_quiz_start(&b2, &blob, 0, 10, 2000u + i);
        bool same = (a.answer_slot == b2.answer_slot);
        for (unsigned k = 0; k < WD_QUIZ_CHOICES && same; ++k) {
            if (a.choice[k] != b2.choice[k]) same = false;
        }
        if (!same) ++varied;
    }

    // ---- 3. 种子为 0 也应可复现（内部兜底成 1）----
    {
        wd_quiz_t a, b2;
        WD_EXPECT(wd_quiz_start(&a, &blob, 0, 3, 0), "seed=0 出题失败");
        WD_EXPECT(wd_quiz_start(&b2, &blob, 0, 3, 0), "seed=0 重复出题失败");
        bool same = (a.answer_slot == b2.answer_slot);
        for (unsigned k = 0; k < WD_QUIZ_CHOICES && same; ++k) {
            if (a.choice[k] != b2.choice[k]) same = false;
        }
        WD_EXPECT(same, "seed=0 两次出题结果不同（兜底值没生效）");
    }

    const unsigned options = total_questions * (WD_QUIZ_CHOICES - 1);
    printf("ok  真实 blob：%u 道题全部满足不变量\n", total_questions);
    printf("      不同 seed 产生不同题：%u/%u\n", varied, kVariety);
    if (options) {
        printf("      干扰项词长差 均值 %.2f  分布(0..8+) ", (double)delta_sum / options);
        for (unsigned i = 0; i <= 8; ++i) {
            printf("%u%s", delta_hist[i], i < 8 ? "/" : "\n");
        }
        const unsigned within4 = delta_hist[0] + delta_hist[1] + delta_hist[2] +
                                 delta_hist[3] + delta_hist[4];
        printf("      词长差 <= 4 的占比 %.1f%%\n", 100.0 * within4 / options);
    }
    free(data);
}

int main(int argc, char **argv) {
    const char *blob_path = (argc > 1) ? argv[1] : WD_BLOB_DEFAULT;

    test_mini();
    test_real(blob_path);

    WD_SUMMARY("test_wd_quiz");
}
