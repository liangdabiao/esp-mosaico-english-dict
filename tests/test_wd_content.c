// tests/test_wd_content.c —— 词条访问层的宿主测试。
//
// 三组：
//   1. 合成 blob 的正常路径：取词、跳字段、书内/全局序号的换算
//   2. 合成 blob 的**拒绝路径**：偏移表不单调、末项与池长不符、字段数不对、
//      某段里多出字节 —— 这些都是"生成器改了布局而解析器没跟上"的痕迹
//   3. **真实 blob 与固定向量逐字段对撞**（tests/wd_content_fixture.h）
//
// 第三组是本文件的核心。fixture 里写的是生成器认为每条记录应该被解析成什么；
// 设备侧必须逐字段相等。字段顺序、拼接方式、偏移基准任何一处跑偏，
// 都会在这里红 —— 而这三样在编译期完全看不出来，在屏幕上只表现为乱码。

#include "wd_content.h"
#include "wd_blob_fixture.h"
#include "wd_test.h"

#include "wd_content_fixture.h"

#define MINI_CAP 1024

static void test_mini_fields(void) {
    unsigned char buf[MINI_CAP];
    const size_t total = wd_build_mini_blob(buf, sizeof(buf));
    WD_EXPECT(total > 0, "迷你 blob 构造失败");
    if (!total) return;

    wd_blob_t blob;
    WD_EXPECT(wd_manifest_open(buf, total, &blob) == WD_MANIFEST_OK, "打开失败");

    uint32_t bad = 0;
    const wd_content_err_t verr = wd_content_verify(&blob, &bad);
    WD_EXPECT(verr == WD_CONTENT_OK, "全量校验失败：%s（第 %u 条）",
              wd_content_err_text(verr), bad);

    // 三条词的 5 个字段，逐字段写死。
    static const char *kExpect[WD_MINI_WORD_COUNT][WD_FIELD_COUNT] = {
        {"apple", "\u02c8\u00e6pl", "n 苹果", "An apple.", "一个苹果。"},
        {"banana", "b\u0259\u02c8n\u0251\u02d0n\u0259", "n 香蕉", "A banana.", "一个香蕉。"},
        {"cat", "k\u00e6t", "n 猫", "A cat.", "一只猫。"},
    };

    for (uint32_t i = 0; i < WD_MINI_WORD_COUNT; ++i) {
        wd_word_t word;
        WD_EXPECT(wd_content_get(&blob, i, &word), "取第 %u 条失败", i);
        for (unsigned f = 0; f < WD_FIELD_COUNT; ++f) {
            const char *got = wd_content_field(&word, f);
            WD_EXPECT(strcmp(got, kExpect[i][f]) == 0,
                      "第 %u 条第 %u 字段 = \"%s\"，期望 \"%s\"", i, f, got,
                      kExpect[i][f]);
        }
    }

    // 书内偏移寻址：第 0 本 2 条（apple/banana），第 1 本 1 条（cat）。
    wd_word_t word;
    WD_EXPECT(wd_content_get_in_book(&blob, 0, 0, &word), "第 0 本第 0 条取不到");
    WD_EXPECT(word.index == 0, "第 0 本第 0 条的全局序号应为 0，实得 %u", word.index);
    WD_EXPECT(strcmp(wd_content_field(&word, WD_FIELD_WORD), "apple") == 0,
              "第 0 本第 0 条应是 apple");
    WD_EXPECT(wd_content_get_in_book(&blob, 0, 1, &word), "第 0 本第 1 条取不到");
    WD_EXPECT(strcmp(wd_content_field(&word, WD_FIELD_WORD), "banana") == 0,
              "第 0 本第 1 条应是 banana");
    WD_EXPECT(wd_content_get_in_book(&blob, 1, 0, &word), "第 1 本第 0 条取不到");
    WD_EXPECT(word.index == 2, "第 1 本第 0 条的全局序号应为 2，实得 %u", word.index);
    WD_EXPECT(strcmp(wd_content_field(&word, WD_FIELD_WORD), "cat") == 0,
              "第 1 本第 0 条应是 cat");

    // 越界一律拒绝
    WD_EXPECT(!wd_content_get(&blob, WD_MINI_WORD_COUNT, &word), "全局序号越界应拒绝");
    WD_EXPECT(!wd_content_get_in_book(&blob, 0, 2, &word), "第 0 本第 2 条越界应拒绝");
    WD_EXPECT(!wd_content_get_in_book(&blob, 2, 0, &word), "第 2 本不存在应拒绝");

    // 字段号越界返回空串，不是 NULL
    WD_EXPECT(wd_content_field(&word, WD_FIELD_COUNT)[0] == '\0',
              "越界字段号应返回空串");
    WD_EXPECT(wd_content_field(&word, 99)[0] == '\0', "越界字段号应返回空串");

    // 全局 <-> 书内 的换算
    for (uint32_t i = 0; i < WD_MINI_WORD_COUNT; ++i) {
        const int book = wd_content_book_of(&blob, i);
        WD_EXPECT(book >= 0, "第 %u 条找不到所属书", i);
        uint32_t local = 999;
        WD_EXPECT(wd_content_local_of(&blob, i, &local), "第 %u 条取书内偏移失败", i);
        WD_EXPECT(wd_content_get_in_book(&blob, (unsigned)book, local, &word),
                  "第 %u 条往返失败", i);
        WD_EXPECT(word.index == i, "第 %u 条往返后序号变了：%u", i, word.index);
    }
    WD_EXPECT(wd_content_book_of(&blob, WD_MINI_WORD_COUNT) == -1, "越界应返回 -1");

    // has 的语义：音标缺失是常态，UI 据此整行隐藏
    WD_EXPECT(wd_content_has(&word, WD_FIELD_WORD), "cat 有单词");
    WD_EXPECT(wd_content_has(&word, WD_FIELD_SENT_CN), "cat 有中文例句");
}

static void test_mini_verify_rejections(void) {
    // (a) 偏移表不单调：把第 0 条的偏移改成和第 1 条一样
    {
        unsigned char buf[MINI_CAP];
        const size_t total = wd_build_mini_blob(buf, sizeof(buf));
        wd_blob_t blob;
        wd_manifest_open(buf, total, &blob);
        wd_patch32(buf, WD_MINI_WORDS_OFF + 0, 999999u);   // 越过池尾
        uint32_t bad = 0;
        const wd_content_err_t err = wd_content_verify(&blob, &bad);
        WD_EXPECT(err == WD_CONTENT_ERR_OFFSETS, "偏移越界应报 OFFSETS，实得 %s",
                  wd_content_err_text(err));
        WD_EXPECT(bad == 0, "应指出第 0 条有问题，实得 %u", bad);
    }
    // (b) 哨兵（末项）被改**大**到越过池尾 —— 最后一条词的边界就没了依据
    {
        unsigned char buf[MINI_CAP];
        const size_t total = wd_build_mini_blob(buf, sizeof(buf));
        wd_blob_t blob;
        wd_manifest_open(buf, total, &blob);
        unsigned sentinel = 0;
        memcpy(&sentinel, buf + WD_MINI_WORDS_OFF + WD_MINI_WORD_COUNT * 4, 4);
        wd_patch32(buf, WD_MINI_WORDS_OFF + WD_MINI_WORD_COUNT * 4, sentinel + 3);
        uint32_t bad = 0;
        const wd_content_err_t err = wd_content_verify(&blob, &bad);
        WD_EXPECT(err == WD_CONTENT_ERR_OFFSETS, "哨兵越过池尾应报 OFFSETS，实得 %s",
                  wd_content_err_text(err));
    }
    // (b2) 哨兵被改**小** —— 最后一条词的段变短，于是字段数不足。
    //      这里报 FIELDS 而不是 OFFSETS 是正确的：哨兵本身就是最后一条的右边界，
    //      改动它首先表现为"那一段里装不下 5 个字段"。
    {
        unsigned char buf[MINI_CAP];
        const size_t total = wd_build_mini_blob(buf, sizeof(buf));
        wd_blob_t blob;
        wd_manifest_open(buf, total, &blob);
        unsigned sentinel = 0;
        memcpy(&sentinel, buf + WD_MINI_WORDS_OFF + WD_MINI_WORD_COUNT * 4, 4);
        wd_patch32(buf, WD_MINI_WORDS_OFF + WD_MINI_WORD_COUNT * 4, sentinel - 3);
        uint32_t bad = 0;
        const wd_content_err_t err = wd_content_verify(&blob, &bad);
        WD_EXPECT(err == WD_CONTENT_ERR_FIELDS, "哨兵被改小应报 FIELDS，实得 %s",
                  wd_content_err_text(err));
    }
    // (c) 某一段里字段数不够：把第 0 条的下一段起点提前（吃掉一个字段）
    {
        unsigned char buf[MINI_CAP];
        const size_t total = wd_build_mini_blob(buf, sizeof(buf));
        wd_blob_t blob;
        wd_manifest_open(buf, total, &blob);
        unsigned next = 0;
        memcpy(&next, buf + WD_MINI_WORDS_OFF + 1 * 4, 4);
        wd_patch32(buf, WD_MINI_WORDS_OFF + 1 * 4, next - 4);   // 提前 4 字节
        uint32_t bad = 0;
        const wd_content_err_t err = wd_content_verify(&blob, &bad);
        WD_EXPECT(err == WD_CONTENT_ERR_FIELDS, "字段数不足应报 FIELDS，实得 %s",
                  wd_content_err_text(err));
        WD_EXPECT(bad == 0, "应指出第 0 条有问题，实得 %u", bad);
    }
    // (d) 某一段里多出字节：把下一段起点推后
    {
        unsigned char buf[MINI_CAP];
        const size_t total = wd_build_mini_blob(buf, sizeof(buf));
        wd_blob_t blob;
        wd_manifest_open(buf, total, &blob);
        unsigned next = 0;
        memcpy(&next, buf + WD_MINI_WORDS_OFF + 1 * 4, 4);
        wd_patch32(buf, WD_MINI_WORDS_OFF + 1 * 4, next + 3);
        uint32_t bad = 0;
        const wd_content_err_t err = wd_content_verify(&blob, &bad);
        WD_EXPECT(err == WD_CONTENT_ERR_FIELDS, "多出字节应报 FIELDS，实得 %s",
                  wd_content_err_text(err));
    }
    // (e) 空指针
    {
        uint32_t bad = 0;
        WD_EXPECT(wd_content_verify(0, &bad) == WD_CONTENT_ERR_NULL, "空 blob 应被拒绝");
    }
}

static void test_real_fixture(const char *path) {
    size_t size = 0;
    unsigned char *data = wd_read_file(path, &size);
    if (!data) {
        printf("（跳过真实 blob 组：读不到 %s）\n", path);
        return;
    }

    wd_blob_t blob;
    const wd_manifest_err_t merr = wd_manifest_open(data, size, &blob);
    WD_EXPECT(merr == WD_MANIFEST_OK, "真实 blob 打开失败：%s",
              wd_manifest_err_text(merr));
    if (merr != WD_MANIFEST_OK) {
        free(data);
        return;
    }

    uint32_t bad = 0;
    const wd_content_err_t verr = wd_content_verify(&blob, &bad);
    WD_EXPECT(verr == WD_CONTENT_OK, "真实 blob 全量校验失败：%s（第 %u 条）",
              wd_content_err_text(verr), bad);
    if (verr != WD_CONTENT_OK) {
        free(data);
        return;
    }

    // fixture 的每条记录给的是 (书 key, 单词, 音标, 释义, 例句英, 例句中)。
    // 设备侧只能按"书 + 单词"去找 —— 这正是要验证的：**从 key 到书内偏移**
    // 这条链路能不能走通。走不通就说明书表或偏移表有问题。
    unsigned matched = 0;
    for (unsigned i = 0; i < kWdFixtureCaseCount; ++i) {
        const wd_fixture_case_t *want = &kWdFixtureCases[i];
        const int book_index = wd_manifest_find_book(&blob, want->book);
        if (book_index < 0) {
            WD_EXPECT(false, "fixture 第 %u 条的书 %s 找不到", i, want->book);
            continue;
        }

        wd_book_t book;
        wd_manifest_book_at(&blob, (unsigned)book_index, &book);

        // 在书内线性找 —— 词不是按字典序排的，没法二分。
        // 19 条 fixture × 最多 4533 词，可接受。
        bool found = false;
        for (uint32_t offset = 0; offset < book.count; ++offset) {
            wd_word_t word;
            if (!wd_content_get_in_book(&blob, (unsigned)book_index, offset, &word)) break;
            if (strcmp(wd_content_field(&word, WD_FIELD_WORD), want->word) != 0) continue;

            found = true;
            ++matched;
            const struct {
                unsigned field;
                const char *name;
                const char *value;
            } checks[] = {
                {WD_FIELD_WORD, "单词", want->word},
                {WD_FIELD_IPA, "音标", want->ipa},
                {WD_FIELD_TRANS, "释义", want->trans},
                {WD_FIELD_SENT_EN, "例句英", want->sent_en},
                {WD_FIELD_SENT_CN, "例句中", want->sent_cn},
            };
            for (unsigned c = 0; c < sizeof(checks) / sizeof(checks[0]); ++c) {
                const char *got = wd_content_field(&word, checks[c].field);
                if (strcmp(got, checks[c].value) != 0) {
                    WD_EXPECT(false,
                              "fixture 第 %u 条 %s/%s 的%s不一致\n"
                              "      期望: \"%s\"\n      实得: \"%s\"",
                              i, want->book, want->word, checks[c].name,
                              checks[c].value, got);
                } else {
                    WD_EXPECT(true, "%s", "");
                }
            }
            break;
        }
        if (!found) {
            WD_EXPECT(false, "fixture 第 %u 条的词 %s 在书 %s 里找不到", i, want->word,
                      want->book);
        }
    }

    printf("ok  真实 blob：%u 条固定向量逐字段相等（共 %u 条）\n", matched,
           kWdFixtureCaseCount);
    free(data);
}

int main(int argc, char **argv) {
    const char *blob_path = (argc > 1) ? argv[1] : WD_BLOB_DEFAULT;

    test_mini_fields();
    test_mini_verify_rejections();
    test_real_fixture(blob_path);

    WD_SUMMARY("test_wd_content");
}
