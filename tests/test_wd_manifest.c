// tests/test_wd_manifest.c —— 词库 blob 清单层的宿主测试。
//
// 两组：
//   1. **合成 blob**（tests/wd_blob_fixture.h）—— 正常路径 + 十七条拒绝路径。
//      拒绝路径是本文件的重点：真机上"刷了半个文件""分区不够""生成器改坏布局"
//      都会落到这些分支上，而它们的表现是屏幕显示乱码而不是崩溃，
//      只能靠这里一条条钉住。
//   2. **真实 blob**（assets/content/wd_content.bin）—— 与生成器产出的
//      tests/wd_content_fixture.h 对撞：书数、词数、版本号、书的 key 与顺序。
//      这是唯一能防"生成器与解析器悄悄跑偏"的设备侧手段。
//
// 编译（宿主机）：直接跑 tools/run_host_tests.sh。
//
// 那个 MSYS2_ARG_CONV_EXCL 不是可选项：不加的话 MSYS 会把 `/d/esp-mo/...`
// 当成路径转换成一个反斜杠形式，cc 收到的是一个不存在的目录。

#include "wd_manifest.h"
#include "wd_blob_fixture.h"
#include "wd_test.h"

#include "wd_content_fixture.h"

// 用一个能装下迷你 blob 的缓冲区（约 300 字节，留 4 倍余量）。
#define MINI_CAP 1024

static void test_mini_ok(void) {
    unsigned char buf[MINI_CAP];
    const size_t total = wd_build_mini_blob(buf, sizeof(buf));
    WD_EXPECT(total > 0, "迷你 blob 构造失败");
    if (!total) return;

    wd_blob_t blob;
    const wd_manifest_err_t err = wd_manifest_open(buf, total, &blob);
    WD_EXPECT(err == WD_MANIFEST_OK, "迷你 blob 应当打开成功，实得 %s",
              wd_manifest_err_text(err));
    if (err != WD_MANIFEST_OK) return;

    WD_EXPECT(blob.version == WD_MINI_VERSION, "版本号不符：%u", blob.version);
    WD_EXPECT(blob.book_count == WD_MINI_BOOK_COUNT, "书数不符：%u", blob.book_count);
    WD_EXPECT(blob.word_count == WD_MINI_WORD_COUNT, "词数不符：%u", blob.word_count);
    WD_EXPECT(blob.total_size == total, "总长不符：%u vs %u", blob.total_size,
              (unsigned)total);

    wd_book_t book;
    WD_EXPECT(wd_manifest_book_at(&blob, 0, &book), "取第 0 本书失败");
    WD_EXPECT(strcmp(book.key, "BookA") == 0, "第 0 本书 key=%s", book.key);
    WD_EXPECT(strcmp(book.name, "甲书") == 0, "第 0 本书 name=%s", book.name);
    WD_EXPECT(strcmp(book.short_name, "甲") == 0, "第 0 本书 short=%s", book.short_name);
    WD_EXPECT(book.first == 0 && book.count == 2, "第 0 本区间 %u/%u", book.first,
              book.count);

    WD_EXPECT(wd_manifest_book_at(&blob, 1, &book), "取第 1 本书失败");
    WD_EXPECT(strcmp(book.key, "BookB") == 0, "第 1 本书 key=%s", book.key);
    WD_EXPECT(book.first == 2 && book.count == 1, "第 1 本区间 %u/%u", book.first,
              book.count);

    // 越界要拒绝，且要把 out 清空 —— 否则调用方会拿到上一本书的残留数据。
    WD_EXPECT(!wd_manifest_book_at(&blob, 2, &book), "第 2 本书不存在，应当拒绝");
    WD_EXPECT(book.key[0] == '\0', "越界后 out 没有被清空：%s", book.key);

    WD_EXPECT(wd_manifest_find_book(&blob, "BookB") == 1, "find BookB 应得 1");
    WD_EXPECT(wd_manifest_find_book(&blob, "BookA") == 0, "find BookA 应得 0");
    WD_EXPECT(wd_manifest_find_book(&blob, "BookC") == -1, "find BookC 应得 -1");
    WD_EXPECT(wd_manifest_find_book(&blob, "") == -1, "空 key 应得 -1");
}

// 一条损坏 → 一个期望的错误码。写成表，加一条就是加一行。
typedef struct {
    const char *what;
    void (*damage)(unsigned char *buf, size_t total);
    wd_manifest_err_t want;
} damage_case_t;

static void dmg_magic(unsigned char *b, size_t t) { (void)t; b[0] = 'X'; }
static void dmg_reserved(unsigned char *b, size_t t) { (void)t; wd_patch16(b, 10, 1); }
static void dmg_total(unsigned char *b, size_t t) { wd_patch32(b, 28, (unsigned)t - 1); }
static void dmg_books_zero(unsigned char *b, size_t t) { (void)t; wd_patch16(b, 8, 0); }
static void dmg_books_huge(unsigned char *b, size_t t) { (void)t; wd_patch16(b, 8, 33); }
static void dmg_words_zero(unsigned char *b, size_t t) { (void)t; wd_patch32(b, 12, 0); }
static void dmg_books_off(unsigned char *b, size_t t) { (void)t; wd_patch32(b, 16, 64); }
static void dmg_words_off(unsigned char *b, size_t t) { (void)t; wd_patch32(b, 20, 96); }
static void dmg_pool_off(unsigned char *b, size_t t) { (void)t; wd_patch32(b, 24, 200); }

// 第 1 本书的 first 改成 1（与第 0 本的 first+count=2 对不上）
static void dmg_first_gap(unsigned char *b, size_t t) {
    (void)t;
    wd_patch32(b, WD_MINI_BOOKS_OFF + 1 * WD_MINI_BOOK_ENTRY + 12, 1);
}
// 第 0 本书的 count 改成 1 ⇒ 累计 1+1=2 != word_count 3
static void dmg_count_short(unsigned char *b, size_t t) {
    (void)t;
    wd_patch32(b, WD_MINI_BOOKS_OFF + 0 * WD_MINI_BOOK_ENTRY + 16, 1);
}
// 第 0 本书的 count 改成 0
static void dmg_count_zero(unsigned char *b, size_t t) {
    (void)t;
    wd_patch32(b, WD_MINI_BOOKS_OFF + 0 * WD_MINI_BOOK_ENTRY + 16, 0);
}
// 第 0 本书的 key 偏移指向 name（"甲书"）⇒ 不是可见 ASCII
static void dmg_key_not_ascii(unsigned char *b, size_t t) {
    (void)t;
    wd_patch32(b, WD_MINI_BOOKS_OFF + 0 * WD_MINI_BOOK_ENTRY + 8, 0);
}
// 第 0 本书的 name 偏移远超池长
static void dmg_name_out_of_pool(unsigned char *b, size_t t) {
    (void)t;
    wd_patch32(b, WD_MINI_BOOKS_OFF + 0 * WD_MINI_BOOK_ENTRY + 0, 100000u);
}
// 池里一个 NUL 都没有 ⇒ 从任何偏移都会扫到池尾
static void dmg_pool_no_nul(unsigned char *b, size_t t) {
    memset(b + WD_MINI_POOL_OFF, 'X', t - WD_MINI_POOL_OFF);
}

static void test_rejections(void) {
    static const damage_case_t kCases[] = {
        {"魔数被改",              dmg_magic,            WD_MANIFEST_ERR_MAGIC},
        {"保留字段非 0",          dmg_reserved,         WD_MANIFEST_ERR_RESERVED},
        {"总长与文件不符",        dmg_total,            WD_MANIFEST_ERR_TOTAL},
        {"书数为 0",              dmg_books_zero,       WD_MANIFEST_ERR_COUNTS},
        {"书数超上限",            dmg_books_huge,       WD_MANIFEST_ERR_COUNTS},
        {"词数为 0",              dmg_words_zero,       WD_MANIFEST_ERR_COUNTS},
        {"书表偏移不对",          dmg_books_off,        WD_MANIFEST_ERR_LAYOUT},
        {"词表偏移不对",          dmg_words_off,        WD_MANIFEST_ERR_LAYOUT},
        {"池偏移不对",            dmg_pool_off,         WD_MANIFEST_ERR_LAYOUT},
        {"书目区间有空洞",        dmg_first_gap,        WD_MANIFEST_ERR_BOOK_RANGE},
        {"书目覆盖不全",          dmg_count_short,      WD_MANIFEST_ERR_BOOK_RANGE},
        {"某本书词数为 0",        dmg_count_zero,       WD_MANIFEST_ERR_BOOK_RANGE},
        {"key 不是可见 ASCII",    dmg_key_not_ascii,    WD_MANIFEST_ERR_TEXT},
        {"名字偏移落在池外",      dmg_name_out_of_pool, WD_MANIFEST_ERR_TEXT},
        {"池里没有 NUL 结尾",     dmg_pool_no_nul,      WD_MANIFEST_ERR_TEXT},
    };

    for (unsigned i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
        const damage_case_t *c = &kCases[i];
        unsigned char buf[MINI_CAP];
        const size_t total = wd_build_mini_blob(buf, sizeof(buf));
        WD_EXPECT(total > 0, "迷你 blob 构造失败");
        if (!total) continue;

        c->damage(buf, total);

        wd_blob_t blob;
        const wd_manifest_err_t err = wd_manifest_open(buf, total, &blob);
        WD_EXPECT(err == c->want, "[%s] 期望 %s，实得 %s", c->what,
                  wd_manifest_err_text(c->want), wd_manifest_err_text(err));
    }

    // 参数本身不合法
    {
        unsigned char buf[MINI_CAP];
        const size_t total = wd_build_mini_blob(buf, sizeof(buf));
        wd_blob_t blob;
        WD_EXPECT(wd_manifest_open(0, total, &blob) == WD_MANIFEST_ERR_NULL,
                  "空指针应当被拒绝");
        WD_EXPECT(wd_manifest_open(buf, 0, 0) == WD_MANIFEST_ERR_NULL,
                  "空 out 应当被拒绝");
        WD_EXPECT(wd_manifest_open(buf, 8, &blob) == WD_MANIFEST_ERR_TOO_SMALL,
                  "长度不足应当被拒绝");
        WD_EXPECT(wd_manifest_open(buf, 0, &blob) == WD_MANIFEST_ERR_TOO_SMALL,
                  "长度 0 应当被拒绝");
    }
}

// 真实 blob：与生成器产出的固定向量对撞。
static void test_real_blob(const char *path) {
    size_t size = 0;
    unsigned char *data = wd_read_file(path, &size);
    if (!data) {
        printf("（跳过真实 blob 组：读不到 %s）\n", path);
        return;
    }

    wd_blob_t blob;
    const wd_manifest_err_t err = wd_manifest_open(data, size, &blob);
    WD_EXPECT(err == WD_MANIFEST_OK, "真实 blob 打开失败：%s",
              wd_manifest_err_text(err));
    if (err != WD_MANIFEST_OK) {
        free(data);
        return;
    }

    WD_EXPECT(blob.book_count == WD_FIX_BOOK_COUNT, "书数 %u != fixture %u",
              blob.book_count, WD_FIX_BOOK_COUNT);
    WD_EXPECT(blob.word_count == WD_FIX_WORD_COUNT, "词数 %u != fixture %u",
              blob.word_count, WD_FIX_WORD_COUNT);
    WD_EXPECT(blob.version == WD_FIX_VERSION, "版本 %u != fixture %u",
              blob.version, WD_FIX_VERSION);

    // fixture 里每条记录的 book 列都是一个 key。它必须能被 find_book 找到，
    // 且找到的那本书的 key 与它逐字节相等 —— 这是"书顺序错位"的唯一防线：
    // 顺序错了不会有任何报错，只会让所有人学到别的书的词。
    for (unsigned i = 0; i < kWdFixtureCaseCount; ++i) {
        const char *key = kWdFixtureCases[i].book;
        const int index = wd_manifest_find_book(&blob, key);
        WD_EXPECT(index >= 0, "fixture 第 %u 条的 book=%s 在 blob 里找不到", i, key);
        if (index < 0) continue;

        wd_book_t book;
        WD_EXPECT(wd_manifest_book_at(&blob, (unsigned)index, &book),
                  "取书 %d 失败", index);
        WD_EXPECT(strcmp(book.key, key) == 0, "书 %d 的 key=%s，期望 %s", index,
                  book.key, key);
    }

    // 六本书的 key 顺序必须与 fixture 里第一次出现的顺序一致。
    static const char *kExpectedOrder[] = {"ChuZhong_3", "GaoZhong_2", "CET4_2",
                                           "CET6_3", "KaoYan_2", "IELTS_2"};
    for (unsigned i = 0; i < 6; ++i) {
        wd_book_t book;
        WD_EXPECT(wd_manifest_book_at(&blob, i, &book), "取第 %u 本书失败", i);
        WD_EXPECT(strcmp(book.key, kExpectedOrder[i]) == 0,
                  "第 %u 本书 key=%s，期望 %s（书顺序变了会让所有人的进度错位）",
                  i, book.key, kExpectedOrder[i]);
        WD_EXPECT(book.count > 0, "第 %u 本书词数为 0", i);
        // 中文名必须非空且不是乱码（长度合理、UTF-8 首字节是 3 字节形式）
        WD_EXPECT(book.name[0] != '\0', "第 %u 本书中文名为空", i);
        WD_EXPECT((unsigned char)book.name[0] >= 0xE0,
                  "第 %u 本书中文名首字节 0x%02x 不像 UTF-8 汉字（可能读错了池偏移）",
                  i, (unsigned char)book.name[0]);
    }

    printf("ok  真实 blob：%u 本书 / %u 条词 / 版本 %u\n", blob.book_count,
           blob.word_count, blob.version);
    free(data);
}

int main(int argc, char **argv) {
    const char *blob_path = (argc > 1) ? argv[1] : WD_BLOB_DEFAULT;

    test_mini_ok();
    test_rejections();
    test_real_blob(blob_path);

    WD_SUMMARY("test_wd_manifest");
}
