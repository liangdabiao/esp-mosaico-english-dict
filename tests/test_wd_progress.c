// tests/test_wd_progress.c —— 学习进度序列化的宿主测试。
//
// 这个模块的每条拒绝路径都必须在真机上"偶发过一次"才会有人发现，
// 所以它们全部要在这里被主动制造出来：
//   * 长度不足 / 魔数不符 / 布局版本不符 / 校验和不符 / 位置越界
//   * **校验顺序**：坏数据 + 版本不符时，必须报"损坏"而不是"过时"——
//     两者对用户的含义完全不同（该报错 vs 该静默重置）。顺序写反的话，
//     真正的损坏会被当成"过时"静默丢掉，永远查不出来。
//
// 位图部分要钉住一个容易忽略的边界：word_count 不是 8 的整数倍时，
// 最后一个字节里有几个"本不该存在的位"。它们若是 1，已掌握数会超过总词数。

#include "wd_progress.h"
#include "wd_blob_fixture.h"
#include "wd_test.h"

#define MINI_CAP 1024

// ---------------------------------------------------------------------------
// A. 掌握位图
// ---------------------------------------------------------------------------

static void test_bitmap_shape(void) {
    WD_EXPECT(wd_progress_bitmap_bytes(0) == 0, "0 词应当 0 字节");
    WD_EXPECT(wd_progress_bitmap_bytes(1) == 1, "1 词应当 1 字节");
    WD_EXPECT(wd_progress_bitmap_bytes(8) == 1, "8 词应当 1 字节");
    WD_EXPECT(wd_progress_bitmap_bytes(9) == 2, "9 词应当 2 字节");
    // 最大的一本书 4533 词 -> 567 字节（这正是不按书拆分会让 NVS 每次重写
    // 2.4 KB 的原因）
    WD_EXPECT(wd_progress_bitmap_bytes(4533) == 567, "4533 词应当 567 字节，实得 %u",
              wd_progress_bitmap_bytes(4533));
    WD_EXPECT(wd_progress_bitmap_bytes(19513) == 2440, "19513 词应当 2440 字节，实得 %u",
              wd_progress_bitmap_bytes(19513));
}

static void test_bitmap_bits(void) {
    uint8_t bits[8];
    const uint32_t count = 30;

    wd_progress_clear(bits, count);
    for (uint32_t i = 0; i < count; ++i) {
        WD_EXPECT(!wd_progress_get(bits, count, i), "清零后第 %u 位应为 0", i);
    }
    WD_EXPECT(wd_progress_mastered(bits, count) == 0, "清零后已掌握应为 0");

    // 设奇数位
    for (uint32_t i = 1; i < count; i += 2) {
        wd_progress_set(bits, count, i, true);
    }
    for (uint32_t i = 0; i < count; ++i) {
        const bool want = (i % 2) == 1;
        WD_EXPECT(wd_progress_get(bits, count, i) == want, "第 %u 位应为 %d", i,
                  (int)want);
    }
    WD_EXPECT(wd_progress_mastered(bits, count) == 15, "应有 15 位已掌握，实得 %u",
              wd_progress_mastered(bits, count));

    // 清掉一位
    wd_progress_set(bits, count, 3, false);
    WD_EXPECT(!wd_progress_get(bits, count, 3), "第 3 位应被清掉");
    WD_EXPECT(wd_progress_mastered(bits, count) == 14, "应剩 14 位");

    // 越界：静默无操作，绝不能越写
    const uint8_t before = bits[3];
    wd_progress_set(bits, count, count, true);
    wd_progress_set(bits, count, 9999, true);
    WD_EXPECT(bits[3] == before, "越界 set 不该改动任何字节");
    WD_EXPECT(!wd_progress_get(bits, count, count), "越界 get 应返回 false");
    WD_EXPECT(!wd_progress_get(0, count, 0), "空位图 get 应返回 false");
    WD_EXPECT(wd_progress_mastered(0, count) == 0, "空位图应为 0");
}

// 尾部多余位：word_count 不是 8 的倍数时，最后一个字节的高位不属于任何词。
// 若把它们算进"已掌握"，屏幕上就会出现「已掌握 12 / 10」这种不可能的进度。
static void test_bitmap_tail_bits(void) {
    uint8_t bits[2];
    const uint32_t count = 10;   // 占 2 字节，第二字节只有 bit 0、1 有效

    wd_progress_clear(bits, count);
    // 把 10 个词全设为掌握
    for (uint32_t i = 0; i < count; ++i) wd_progress_set(bits, count, i, true);
    WD_EXPECT(wd_progress_mastered(bits, count) == 10, "应为 10，实得 %u",
              wd_progress_mastered(bits, count));

    // 手工把第二字节的 bit 2..7（对应下标 10..15）点亮 —— 这些位不属于任何词
    bits[1] |= 0xFC;
    WD_EXPECT(wd_progress_mastered(bits, count) == 10,
              "尾部多余位不该被计入，实得 %u", wd_progress_mastered(bits, count));

    // 但只点亮 bit 0（下标 8）时，计数必须变化
    wd_progress_clear(bits, count);
    bits[0] = 0xFF;   // 下标 0..7
    bits[1] = 0x01;   // 下标 8
    WD_EXPECT(wd_progress_mastered(bits, count) == 9, "应为 9，实得 %u",
              wd_progress_mastered(bits, count));
}

static void test_bitmap_next(void) {
    uint8_t bits[4];
    const uint32_t count = 20;

    wd_progress_clear(bits, count);
    WD_EXPECT(wd_progress_next_unmastered(bits, count, 0) == 0, "全未掌握应从 0 开始");
    WD_EXPECT(wd_progress_next_unmastered(bits, count, 7) == 7, "全未掌握应从 7 开始");

    // 掌握 0..4，从 0 找 -> 5
    for (uint32_t i = 0; i <= 4; ++i) wd_progress_set(bits, count, i, true);
    WD_EXPECT(wd_progress_next_unmastered(bits, count, 0) == 5, "应得 5，实得 %u",
              wd_progress_next_unmastered(bits, count, 0));
    WD_EXPECT(wd_progress_next_unmastered(bits, count, 3) == 5, "应得 5");
    WD_EXPECT(wd_progress_next_unmastered(bits, count, 5) == 5, "应得 5");

    // 只剩 12 未掌握，从 13 找 -> 绕回 12
    for (uint32_t i = 0; i < count; ++i) wd_progress_set(bits, count, i, true);
    wd_progress_set(bits, count, 12, false);
    WD_EXPECT(wd_progress_next_unmastered(bits, count, 13) == 12,
              "应绕回 12，实得 %u", wd_progress_next_unmastered(bits, count, 13));
    WD_EXPECT(wd_progress_next_unmastered(bits, count, 0) == 12, "应得 12");

    // 全部掌握 -> 返回 count（不是一个合法下标，调用方必须判它）
    wd_progress_set(bits, count, 12, true);
    const uint32_t none = wd_progress_next_unmastered(bits, count, 0);
    WD_EXPECT(none == count, "全掌握应返回 %u，实得 %u", count, none);

    // 越界的 from 应当从 0 开始找，而不是崩
    wd_progress_set(bits, count, 7, false);
    WD_EXPECT(wd_progress_next_unmastered(bits, count, 999) == 7,
              "越界 from 应从 0 找起");

    WD_EXPECT(wd_progress_next_unmastered(bits, 0, 0) == 0, "0 词应返回 0");
    WD_EXPECT(wd_progress_next_unmastered(0, count, 0) == count, "空位图应返回 count");
}

// ---------------------------------------------------------------------------
// B. 断点位置
// ---------------------------------------------------------------------------

// 独立实现的 Fletcher-16，用来对撞 encode 写出来的两个字节。
// 它验证的是"算的是前 20 字节还是前 22 字节""小端序对不对""写在第几个偏移"，
// 而不是算法本身（算法只有一种写法，没有第二种可选）。
static uint16_t ref_checksum(const uint8_t *data, size_t len) {
    unsigned s1 = 0;
    unsigned s2 = 0;
    for (size_t i = 0; i < len; ++i) {
        s1 = (s1 + data[i]) % 255u;
        s2 = (s2 + s1) % 255u;
    }
    return (uint16_t)((s2 << 8) | s1);
}

static void test_resume_roundtrip(void) {
    unsigned char buf[MINI_CAP];
    const size_t total = wd_build_mini_blob(buf, sizeof(buf));
    wd_blob_t blob;
    WD_EXPECT(wd_manifest_open(buf, total, &blob) == WD_MANIFEST_OK, "打开失败");

    wd_resume_t in;
    in.content_version = WD_MINI_VERSION;
    in.book_index = 1;
    in.offset = 0;
    in.seed = 0xDEADBEEFu;

    uint8_t encoded[WD_RESUME_BYTES];
    const size_t written = wd_resume_encode(&in, encoded, sizeof(encoded));
    WD_EXPECT(written == WD_RESUME_BYTES, "应写 %u 字节，实得 %u", WD_RESUME_BYTES,
              (unsigned)written);

    // 结构字段的位置与字节序
    WD_EXPECT(encoded[0] == 0x57 && encoded[1] == 0x44, "魔数小端应为 57 44，实得 %02x %02x",
              encoded[0], encoded[1]);
    WD_EXPECT(encoded[2] == WD_RESUME_FORMAT, "布局版本应在偏移 2，实得 %u", encoded[2]);
    WD_EXPECT(encoded[3] == 0, "保留字节应为 0");

    // 校验和：独立算一遍，比对写在第 20、21 字节的小端值
    const uint16_t want = ref_checksum(encoded, 20);
    const uint16_t got = (uint16_t)(encoded[20] | (encoded[21] << 8));
    WD_EXPECT(got == want, "校验和应为 %04x（写在偏移 20 小端），实得 %04x", want, got);

    // 往返
    wd_resume_t out;
    const wd_resume_err_t err = wd_resume_decode(encoded, sizeof(encoded), &blob, &out);
    WD_EXPECT(err == WD_RESUME_OK, "往返解码失败：%s", wd_resume_err_text(err));
    WD_EXPECT(out.content_version == in.content_version, "版本不符");
    WD_EXPECT(out.book_index == in.book_index, "书序号不符");
    WD_EXPECT(out.offset == in.offset, "偏移不符");
    WD_EXPECT(out.seed == in.seed, "种子不符");

    // cap 不足时返回 0，且**一个字节都不写**
    uint8_t small[WD_RESUME_BYTES];
    memset(small, 0xAA, sizeof(small));
    uint8_t guard[WD_RESUME_BYTES];
    memcpy(guard, small, sizeof(guard));
    WD_EXPECT(wd_resume_encode(&in, small, WD_RESUME_BYTES - 1) == 0,
              "cap 不足应返回 0");
    WD_EXPECT(memcmp(small, guard, sizeof(guard)) == 0, "cap 不足时不该写任何字节");
    WD_EXPECT(wd_resume_encode(0, small, sizeof(small)) == 0, "空 resume 应返回 0");
    WD_EXPECT(wd_resume_encode(&in, 0, sizeof(small)) == 0, "空 out 应返回 0");
}

static void test_resume_rejections(void) {
    unsigned char blob_buf[MINI_CAP];
    const size_t total = wd_build_mini_blob(blob_buf, sizeof(blob_buf));
    wd_blob_t blob;
    wd_manifest_open(blob_buf, total, &blob);

    wd_resume_t in;
    in.content_version = WD_MINI_VERSION;
    in.book_index = 1;
    in.offset = 0;
    in.seed = 7;

    uint8_t good[WD_RESUME_BYTES];
    wd_resume_encode(&in, good, sizeof(good));
    wd_resume_t out;

    // 长度不足
    WD_EXPECT(wd_resume_decode(good, WD_RESUME_BYTES - 1, &blob, &out) ==
                  WD_RESUME_ERR_SHORT, "长度不足应报 SHORT");
    WD_EXPECT(wd_resume_decode(good, 0, &blob, &out) == WD_RESUME_ERR_SHORT,
              "长度 0 应报 SHORT");

    // 空指针
    WD_EXPECT(wd_resume_decode(0, WD_RESUME_BYTES, &blob, &out) == WD_RESUME_ERR_NULL,
              "空数据应报 NULL");
    WD_EXPECT(wd_resume_decode(good, WD_RESUME_BYTES, 0, &out) == WD_RESUME_ERR_NULL,
              "空 blob 应报 NULL");
    WD_EXPECT(wd_resume_decode(good, WD_RESUME_BYTES, &blob, 0) == WD_RESUME_ERR_NULL,
              "空 out 应报 NULL");

    // 魔数不符
    {
        uint8_t bad[WD_RESUME_BYTES];
        memcpy(bad, good, sizeof(bad));
        bad[0] ^= 0xFF;
        WD_EXPECT(wd_resume_decode(bad, sizeof(bad), &blob, &out) ==
                      WD_RESUME_ERR_MAGIC, "魔数被改应报 MAGIC");
    }
    // 布局版本不符
    {
        uint8_t bad[WD_RESUME_BYTES];
        memcpy(bad, good, sizeof(bad));
        bad[2] = 99;
        WD_EXPECT(wd_resume_decode(bad, sizeof(bad), &blob, &out) ==
                      WD_RESUME_ERR_FORMAT, "布局版本不符应报 FORMAT");
    }
    // 校验和不符：改一个数据字节但不改校验和
    {
        uint8_t bad[WD_RESUME_BYTES];
        memcpy(bad, good, sizeof(bad));
        bad[16] ^= 0x01;   // seed 的一个字节
        WD_EXPECT(wd_resume_decode(bad, sizeof(bad), &blob, &out) ==
                      WD_RESUME_ERR_CHECKSUM, "校验和不符应报 CHECKSUM");
    }
    // 内容版本不符：**这是正常路径**，不是损坏
    {
        wd_resume_t older = in;
        older.content_version = WD_MINI_VERSION ^ 0xABCDu;
        uint8_t encoded[WD_RESUME_BYTES];
        wd_resume_encode(&older, encoded, sizeof(encoded));
        WD_EXPECT(wd_resume_decode(encoded, sizeof(encoded), &blob, &out) ==
                      WD_RESUME_ERR_VERSION, "内容版本不符应报 VERSION");
    }
    // 书序号越界（mini blob 只有 2 本书）
    {
        wd_resume_t bad_in = in;
        bad_in.book_index = 2;
        uint8_t encoded[WD_RESUME_BYTES];
        wd_resume_encode(&bad_in, encoded, sizeof(encoded));
        WD_EXPECT(wd_resume_decode(encoded, sizeof(encoded), &blob, &out) ==
                      WD_RESUME_ERR_RANGE, "书序号越界应报 RANGE");
    }
    // 书内偏移越界（BookB 只有 1 条词）
    {
        wd_resume_t bad_in = in;
        bad_in.offset = 1;
        uint8_t encoded[WD_RESUME_BYTES];
        wd_resume_encode(&bad_in, encoded, sizeof(encoded));
        WD_EXPECT(wd_resume_decode(encoded, sizeof(encoded), &blob, &out) ==
                      WD_RESUME_ERR_RANGE, "书内偏移越界应报 RANGE");
    }

    // ---- 校验顺序 ----
    //
    // 一份**同时**损坏（校验和不对）且版本不符的数据，必须报 CHECKSUM。
    // 若顺序写反（先判版本），损坏会被当成"词库更新了"而静默重置 ——
    // 用户看到的是"进度突然没了"，而真正的原因（Flash 数据坏了）永远查不出来。
    {
        wd_resume_t older = in;
        older.content_version = WD_MINI_VERSION ^ 0x1234u;
        uint8_t encoded[WD_RESUME_BYTES];
        wd_resume_encode(&older, encoded, sizeof(encoded));
        encoded[8] ^= 0x08;   // 破坏 book_index，校验和随之失效
        WD_EXPECT(wd_resume_decode(encoded, sizeof(encoded), &blob, &out) ==
                      WD_RESUME_ERR_CHECKSUM,
                  "既损坏又过时的数据必须报 CHECKSUM（损坏优先于过时）");
    }
    // 同理：既损坏又越界，也应报 CHECKSUM
    {
        wd_resume_t bad_in = in;
        bad_in.book_index = 99;
        uint8_t encoded[WD_RESUME_BYTES];
        wd_resume_encode(&bad_in, encoded, sizeof(encoded));
        encoded[4] ^= 0xFF;
        WD_EXPECT(wd_resume_decode(encoded, sizeof(encoded), &blob, &out) ==
                      WD_RESUME_ERR_CHECKSUM, "既损坏又越界应报 CHECKSUM");
    }

    // 解码失败时不该留下半份结果
    WD_EXPECT(wd_resume_decode(good, 3, &blob, &out) == WD_RESUME_ERR_SHORT,
              "长度不足");
    WD_EXPECT(out.book_index == 0 && out.offset == 0 && out.seed == 0,
              "解码失败后 out 应被清空");
}

int main(void) {
    test_bitmap_shape();
    test_bitmap_bits();
    test_bitmap_tail_bits();
    test_bitmap_next();
    test_resume_roundtrip();
    test_resume_rejections();

    WD_SUMMARY("test_wd_progress");
}
