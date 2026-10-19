// tests/wd_blob_fixture.h —— **合成**的迷你词库 blob（2 本书 / 3 条词，约 300 字节）。
//
// 为什么需要它，而不是所有测试都用 assets/content/wd_content.bin：
//
//   1. **拒绝路径测不了真实文件**。要验证"书表区间不连续就拒绝打开"，
//      必须造一个区间不连续的文件 —— 真实产物永远是对的。
//      而这类损坏正是设备上最难查的故障（刷机中断、分区不够、生成器改坏布局），
//      恰恰最需要测试覆盖。
//   2. 真实 blob 2.9 MB，每次跑测试都要读盘；合成的在栈上，零 IO。
//   3. 合成 blob 里每条记录都是手写的确定值，断言可以写死 —— 而真实 blob 的
//      固定向量由生成器产出（tests/wd_content_fixture.h），那是另一条路径。
//
// 它校验的是**解析器**，不是生成器。生成器由 tools/wordbook/verify_blob.py
// 用另一套独立实现校验。三者互不重叠。
//
// 布局与 tools/wordbook/gen_content.py 完全一致（这是刻意的：合成 blob 若用了
// 另一套布局，测出来的"通过"毫无意义）。池的写入顺序也与生成器相同：
// 先所有书的 name/short/key，再按书顺序写每条词的 5 个字段。
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define WD_MINI_VERSION 0x12345678u
#define WD_MINI_BOOK_COUNT 2u
#define WD_MINI_WORD_COUNT 3u

#define WD_MINI_HEADER_OFF 0u
#define WD_MINI_BOOKS_OFF 32u
#define WD_MINI_BOOK_ENTRY 20u
#define WD_MINI_WORDS_OFF (WD_MINI_BOOKS_OFF + WD_MINI_BOOK_COUNT * WD_MINI_BOOK_ENTRY) /* 72 */
#define WD_MINI_POOL_OFF (WD_MINI_WORDS_OFF + (WD_MINI_WORD_COUNT + 1) * 4)             /* 88 */

static inline void wd_patch8(unsigned char *buf, size_t off, unsigned value) {
    buf[off] = (unsigned char)(value & 0xffu);
}

static inline void wd_patch16(unsigned char *buf, size_t off, unsigned value) {
    buf[off + 0] = (unsigned char)(value & 0xffu);
    buf[off + 1] = (unsigned char)((value >> 8) & 0xffu);
}

static inline void wd_patch32(unsigned char *buf, size_t off, unsigned value) {
    buf[off + 0] = (unsigned char)(value & 0xffu);
    buf[off + 1] = (unsigned char)((value >> 8) & 0xffu);
    buf[off + 2] = (unsigned char)((value >> 16) & 0xffu);
    buf[off + 3] = (unsigned char)((value >> 24) & 0xffu);
}

// 把一条字符串写进池，返回写到的池内偏移，并把 pool_len 推进（含结尾 NUL）。
static inline size_t wd_mini_push(unsigned char *pool, size_t *pool_len,
                                 const char *text) {
    const size_t at = *pool_len;
    const size_t n = strlen(text);
    memcpy(pool + at, text, n);
    pool[at + n] = '\0';
    *pool_len = at + n + 1;
    return at;
}

// 构造迷你 blob，返回总字节数；cap 不够返回 0。
static inline size_t wd_build_mini_blob(unsigned char *buf, size_t cap) {
    static const char *kNames[WD_MINI_BOOK_COUNT] = {"甲书", "乙书"};
    static const char *kShorts[WD_MINI_BOOK_COUNT] = {"甲", "乙"};
    static const char *kKeys[WD_MINI_BOOK_COUNT] = {"BookA", "BookB"};
    static const unsigned kCounts[WD_MINI_BOOK_COUNT] = {2, 1};
    static const char *kWords[WD_MINI_WORD_COUNT][5] = {
        {"apple", "\u02c8\u00e6pl", "n 苹果", "An apple.", "一个苹果。"},
        {"banana", "b\u0259\u02c8n\u0251\u02d0n\u0259", "n 香蕉", "A banana.", "一个香蕉。"},
        {"cat", "k\u00e6t", "n 猫", "A cat.", "一只猫。"},
    };

    unsigned char pool[768];
    size_t pool_len = 0;

    size_t name_at[WD_MINI_BOOK_COUNT];
    size_t short_at[WD_MINI_BOOK_COUNT];
    size_t key_at[WD_MINI_BOOK_COUNT];
    for (unsigned b = 0; b < WD_MINI_BOOK_COUNT; ++b) {
        name_at[b] = wd_mini_push(pool, &pool_len, kNames[b]);
        short_at[b] = wd_mini_push(pool, &pool_len, kShorts[b]);
        key_at[b] = wd_mini_push(pool, &pool_len, kKeys[b]);
    }

    size_t word_at[WD_MINI_WORD_COUNT + 1];
    unsigned wi = 0;
    for (unsigned b = 0; b < WD_MINI_BOOK_COUNT; ++b) {
        for (unsigned k = 0; k < kCounts[b]; ++k) {
            word_at[wi] = pool_len;
            for (unsigned f = 0; f < 5; ++f) {
                wd_mini_push(pool, &pool_len, kWords[wi][f]);
            }
            ++wi;
        }
    }
    word_at[WD_MINI_WORD_COUNT] = pool_len;

    const size_t total = WD_MINI_POOL_OFF + pool_len;
    if (cap < total) return 0;

    memcpy(buf, "WDB1", 4);
    wd_patch32(buf, 4, WD_MINI_VERSION);
    wd_patch16(buf, 8, WD_MINI_BOOK_COUNT);
    wd_patch16(buf, 10, 0);                 // reserved
    wd_patch32(buf, 12, WD_MINI_WORD_COUNT);
    wd_patch32(buf, 16, WD_MINI_BOOKS_OFF);
    wd_patch32(buf, 20, WD_MINI_WORDS_OFF);
    wd_patch32(buf, 24, WD_MINI_POOL_OFF);
    wd_patch32(buf, 28, (unsigned)total);

    unsigned first = 0;
    for (unsigned b = 0; b < WD_MINI_BOOK_COUNT; ++b) {
        unsigned char *entry = buf + WD_MINI_BOOKS_OFF + b * WD_MINI_BOOK_ENTRY;
        wd_patch32(entry, 0, (unsigned)name_at[b]);
        wd_patch32(entry, 4, (unsigned)short_at[b]);
        wd_patch32(entry, 8, (unsigned)key_at[b]);
        wd_patch32(entry, 12, first);
        wd_patch32(entry, 16, kCounts[b]);
        first += kCounts[b];
    }

    for (unsigned i = 0; i <= WD_MINI_WORD_COUNT; ++i) {
        wd_patch32(buf, WD_MINI_WORDS_OFF + i * 4, (unsigned)word_at[i]);
    }

    memcpy(buf + WD_MINI_POOL_OFF, pool, pool_len);
    return total;
}
