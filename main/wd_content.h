// main/wd_content.h —— 词条访问层（纯逻辑，可在宿主机编译）。
//
// 分层与基座一致：wd_manifest 管**清单**（文件头 + 书表），wd_content 管**条目**
// （词偏移表 + 字符串池），正对应 english-podcast 的 pdq_manifest / pdq_catalog。
// 分开的理由不是"看起来整齐"：清单层是 6 条记录、启动时全量校验一次就再也不变；
// 条目层是 19513 条、每次翻面都要随机访问。两者的校验成本与调用频率差三个数量级，
// 混在一起就会逼出一个"要么启动慢、要么访问期没校验"的两难。
//
// 字段以**只读指针**返回，不拷贝。
// 理由：一个词 5 个字段最长 24+28+64+104+39 = 259 字节，若拷贝到调用方栈上，
// 一个"当前词"结构就占 260 字节，而屏上同时存在的词不止一个（出题时 4 个选项
// 都要显示单词）。设备端总共只有 400 KB SRAM 且要跟 LVGL 和音频缓冲抢，
// 而 blob 本身就在 Flash 的 rodata 里、可以直接读 —— 拷贝没有任何收益。
// 代价是调用方必须知道"这个指针的生命周期 = blob 的生命周期"，写在这里。
#pragma once

#include "wd_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WD_FIELD_COUNT 5
#define WD_FIELD_WORD 0     // cancel
#define WD_FIELD_IPA 1      // ˈkænsl（可能为空串：有 1000+ 条词源里就没音标）
#define WD_FIELD_TRANS 2    // v 取消，废除；抵消；删去，划掉
#define WD_FIELD_SENT_EN 3  // Our flight was cancelled.
#define WD_FIELD_SENT_CN 4  // 我们的航班取消了。

typedef struct {
    const wd_blob_t *blob;  // 不拥有
    uint32_t index;         // **全局**词序（0 .. word_count-1）
} wd_word_t;

// 条目层的全量校验：词偏移表单调递增、每个词恰好 5 个字段且都在池内以 NUL 收尾。
//
// 为什么单独一个函数而不是塞进 wd_manifest_open：
//   * 清单层的调用方（比如只显示书单的页面）不该为 19513 条的扫描买单；
//   * 但设备启动时**必须**调它一次 —— 池里少一个 NUL 会让某个词的最后一个字段
//     一路读到池尾，屏幕上表现为"例句里混进了后面几百个词的文字"。
//     这类损坏不会崩，只会显示错，所以只能靠显式校验拦住。
//
// 返回第一个出问题的**全局词序**（可用诊断），全部正常返回 WD_CONTENT_OK。
typedef enum {
    WD_CONTENT_OK = 0,
    WD_CONTENT_ERR_NULL,
    WD_CONTENT_ERR_OFFSETS,   // 偏移表不单调 / 末项与池长不符
    WD_CONTENT_ERR_FIELDS,    // 字段数不对（没能在池内读到 5 个 NUL 结尾的串）
    WD_CONTENT_ERR_NOT_FOUND, // get 的序号越界
    WD_CONTENT_ERR_BOOK,      // 书序号越界
} wd_content_err_t;

const char *wd_content_err_text(wd_content_err_t err);

// 全量校验（启动时调一次）。bad_index 可为 NULL。
wd_content_err_t wd_content_verify(const wd_blob_t *blob, uint32_t *bad_index);

// 按全局词序取词。越界返回 false。
bool wd_content_get(const wd_blob_t *blob, uint32_t index, wd_word_t *out);

// 按「第 book_index 本，书内第 offset 条」取词。
// 书内偏移是进度的寻址单位（位图按它排），所以这个入口用得非常频繁。
bool wd_content_get_in_book(const wd_blob_t *blob, unsigned book_index,
                            uint32_t offset, wd_word_t *out);

// 读字段，返回指向 blob 内部的只读指针。字段号越界或对象无效时返回空串
// （**不返回 NULL**）：调用方拿到空串直接画空行即可，不必到处判空。
const char *wd_content_field(const wd_word_t *word, unsigned field);

// 该字段是否有内容。音标缺失是**常态**（1000+ 条），UI 要据此整行隐藏，
// 而不是画一个空行留出一段莫名的空白。
static inline bool wd_content_has(const wd_word_t *word, unsigned field) {
    return wd_content_field(word, field)[0] != '\0';
}

// 全局词序 -> 书序号；越界或找不到返回 -1。
int wd_content_book_of(const wd_blob_t *blob, uint32_t index);

// 全局词序 -> 书内偏移。越界返回 false。
bool wd_content_local_of(const wd_blob_t *blob, uint32_t index, uint32_t *out_offset);

#ifdef __cplusplus
}  // extern "C"
#endif
