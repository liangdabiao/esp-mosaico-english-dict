// main/wd_content_blob.h —— 词库字节从哪来（只有一个答案）。
//
// 词库 2.75 MB 不走"生成一个巨大的 .c 数组"这条路：那会让编译与链接明显变慢，
// 生成出来的源文件也没法审阅。改成二进制附加（main/CMakeLists.txt 的
// target_add_binary_data）—— 字节仍然落在 Flash 的 rodata 段，读它就是一次
// 内存映射访问，与数组没有性能差别。
//
// 代价是取字节靠的是**链接器符号名**（由 ESP-IDF 按文件名生成），
// 而符号名写错只会在链接期炸、报的还是"找不到某个符号"这种看不出根因的话。
// 所以声明全部收在 wd_content_blob.c 这一个文件里，别处不得再写 extern。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wd_content.h"
#include "wd_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

const uint8_t *wd_content_blob(void);
uint32_t wd_content_blob_size(void);

// 打开并**两层都验**：先验清单层（头 + 书表），再验词条层（偏移表 + 字符串池）。
//
// 返回 false 表示词库不可用。reason 里带一句人话（"不是 WDB1 开头" /
// "词条偏移表不单调"…），设备上直接打在屏幕上 —— 「刷错文件」与
// 「生成器有 bug」指向完全不同的排查方向，只显示"加载失败"等于没显示。
//
// 启动时调一次。两层全量校验约 19513 条，在 ESP32-C3 上是毫秒级
// （只做整数比较，不碰字符串内容）。
bool wd_content_open(wd_blob_t *out, char *reason, size_t reason_capacity);

#ifdef __cplusplus
}  // extern "C"
#endif
