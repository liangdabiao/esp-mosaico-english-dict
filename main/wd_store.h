// main/wd_store.h —— 学习进度与设置的持久化（NVS）。
//
// 存四样东西，都是"重启后必须还在"的：
//   * 每本书的掌握位图   —— 键 `wdp.<书的 key>`，见下面的键名预算说明
//   * 断点（学到哪本书哪个词、上一题的种子）—— 键 `resume`
//   * 音量               —— 键 `volume`
//   * 上次在学哪本书     —— 键 `book`
//
// 还有一个 `cver`：**内容版本**。它不是业务数据，是给上面那些数据打的标签 ——
// 词库换了（重新生成过 blob）之后，旧位图画在新词表上会指到别的词上，
// 位图本身没错、错的是它与新词表的对应关系。所以 wd_store_init 只要发现
// cver 与当前内容不符，就把整个命名空间清空，而不是留着一些"看起来还在、
// 其实对不上"的进度。这是**唯一**允许丢失用户数据的地方，且理由必须是
// "数据已经不可解释了"，不能是"懒得迁移"。
//
// 关于键名上限（这条踩过）：NVS_KEY_NAME_MAX_SIZE 是 16，含结尾 NUL，
// 所以键名最长 15 字符。`wdp.` 占 4，书的 key 最多 11 —— wd_manifest.h 里
// 有一条编译期断言钉着这个预算。超了的后果是**写进度静默失败**，
// 表现成「学完一页翻回来进度没了」，极难排查。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wd_progress.h"   // wd_resume_t —— 本模块唯一的依赖，只要这一个类型

#ifdef __cplusplus
extern "C" {
#endif

// 打开命名空间并按内容版本决定是否清空。
//
// 由 app_main 在 nvs_flash_init() 之后、任何读写之前调一次。
// 返回 false 说明 NVS 用不了（分区满 / 权限）—— 此时应用仍可运行，
// 只是学过的词不会记住。**不要**因此拒绝启动：一个能查、能听、能做题
// 但不记进度的词典，比一个开不了机的词典有用得多。
bool wd_store_init(uint32_t content_version);

// 掌握位图。word_count 用来算期望的字节数；存进去的大小与它不符即判为
// 不可用（返回 false，调用方按"全部未掌握"处理）。
bool wd_store_load_bitmap(const char *book_key, uint32_t word_count, uint8_t *bits);
bool wd_store_save_bitmap(const char *book_key, const uint8_t *bits, uint32_t word_count);

// 断点。返回值就是 wd_resume_err_t —— **不压成 bool**。
//
// 为什么保留错误码：WD_RESUME_ERR_VERSION（词库换了，从头开始即可，正常路径）
// 与 WD_RESUME_ERR_CHECKSUM（数据被写坏了，该报错）对用户的含义完全不同。
// 压成 false 就再也分不出来了，而这正是"进度条忽然回到开头"这类故障
// 最难查的原因 —— 界面上两种原因长得一模一样。
//
// blob 必须传：解码要拿它的 version / book_count / 每本书的 count 判越界。
wd_resume_err_t wd_store_load_resume(const wd_blob_t *blob, wd_resume_t *out);
bool wd_store_save_resume(const wd_resume_t *resume);

// 音量 0..100。没存过返回 80（与 wd_audio.h 的默认值一致）。
uint8_t wd_store_load_volume(void);
void wd_store_save_volume(uint8_t volume);

// 上次在学哪本书（书的 key）。没存过返回 false，out 保持空串。
bool wd_store_load_book(char *out, size_t capacity);
void wd_store_save_book(const char *book_key);

#ifdef __cplusplus
}  // extern "C"
#endif
