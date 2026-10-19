// tests/wd_test.h —— 宿主测试的公共设施。
//
// 与基座 tests/test_pdq_manifest.c 的差别：那边用 assert，第一个失败就中止。
// 这里改成**记录失败但继续跑**。理由：本轮要覆盖的是"拒绝路径"——
// 十几条互相独立的损坏输入，每一条各自断言一次。用 assert 的话，
// 修好第一条才会看见第二条，改一轮测试要来回十几次；收集起来一次看全。
//
// 用法：
//     #include "wd_test.h"
//     int main(void) {
//         WD_EXPECT(x == 1, "x 应该是 1，实得 %d", x);
//         WD_SUMMARY("test_xxx");
//     }
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int wd_checks = 0;
static int wd_failures = 0;

// GCC 16 起，fprintf(stderr, "") 这种零长格式串会报
// -Werror=format-zero-length。WD_EXPECT(cond) 不带详情是合法用法（只想计数），
// 所以用 __VA_OPT__ 把"空可变参"整体剥掉，而不是传一个空串进去。
#define WD_EXPECT(cond, ...)                                        \
    do {                                                            \
        ++wd_checks;                                                \
        if (!(cond)) {                                              \
            ++wd_failures;                                          \
            fprintf(stderr, "FAIL  %s:%d  ", __FILE__, __LINE__);   \
            __VA_OPT__(fprintf(stderr, __VA_ARGS__);)               \
            fputc('\n', stderr);                                    \
            fflush(stderr);                                         \
        }                                                           \
    } while (0)

// 只想报告一次计数，不想在成功路径上刷屏。
#define WD_SUMMARY(name)                                                       \
    do {                                                                       \
        if (wd_failures) {                                                     \
            printf("%-28s %d 项断言，%d 项失败\n", name, wd_checks, wd_failures); \
            return 1;                                                          \
        }                                                                      \
        printf("%-28s %d 项断言全部通过\n", name, wd_checks);                   \
        return 0;                                                              \
    } while (0)

// 把整个文件读进内存。用 malloc，返回值由调用方 free。
// 读不到就返回 NULL（调用方据此跳过依赖真实 blob 的测试组，而不是崩掉）。
// 标 inline：不是每个测试都要读文件，不用它的编译单元不该报警告。
static inline unsigned char *wd_read_file(const char *path, size_t *out_size) {
    if (out_size) *out_size = 0;
    FILE *handle = fopen(path, "rb");
    if (!handle) return NULL;
    if (fseek(handle, 0, SEEK_END) != 0) { fclose(handle); return NULL; }
    const long length = ftell(handle);
    if (length <= 0) { fclose(handle); return NULL; }
    rewind(handle);

    unsigned char *buffer = (unsigned char *)malloc((size_t)length);
    if (!buffer) { fclose(handle); return NULL; }
    const size_t got = fread(buffer, 1, (size_t)length, handle);
    fclose(handle);
    if (got != (size_t)length) { free(buffer); return NULL; }

    if (out_size) *out_size = got;
    return buffer;
}

// 真实 blob 的默认位置（相对仓库根）。测试可以接受 argv[1] 覆盖。
#define WD_BLOB_DEFAULT "assets/content/wd_content.bin"
