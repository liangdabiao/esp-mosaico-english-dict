// main/wd_progress.h —— 学习进度的**序列化**（纯逻辑，可在宿主机编译）。
//
// 这个模块只做一件事：把进度变成字节、把字节变回进度，并且**严格拒绝任何
// 读不通的输入**。它不碰 NVS —— NVS 的读写（键名组装、命名空间、掉电重试）
// 全在 main/wd_store.c。分开的理由：NVS 在宿主机上跑不了，而这个模块的
// 每一条拒绝路径都必须能被测试覆盖 —— "进度读坏了"这类 bug 只在真机上
// 偶发，靠手工复现几乎不可能。
//
// 两份数据：
//
//   A. **掌握位图**（每本书一份，存 NVS 键 `wdp.<book_key>`）
//      19513 个词一个 bit = 2440 字节；但按书拆分后单份最大只有 567 字节
//      （KaoYan_2 的 4533 词）。按书拆不是为了省空间，是为了**写入粒度**：
//      NVS 每次写是整条记录的读-改-写，一份 2.4 KB 的位图意味着每标记一个词
//      就要重写 2.4 KB 到 Flash —— 而 Flash 擦写寿命是有限的，一页学 20 个词
//      就是 480 KB 的写入量。拆成 567 字节后同样的操作少写 4 倍以上。
//
//   B. **断点位置**（一条记录，NVS 键 `wds`）
//      下次开机接着上次那一页往下学。存的是"哪本书 + 书内第几个词 + 出题种子"，
//      外加**内容版本号**。
//
// 内容版本号是这里最关键的一个字段：词库换了之后，书内偏移 137 指向的
// 完全是另一个词。此时若继续沿用旧位置，用户会突然跳到一本陌生的书中间，
// 而他完全不知道发生了什么。所以版本不符时**必须**把位置作废，从头开始 ——
// 这是正常路径，不是错误（见 WD_RESUME_ERR_VERSION 的注释）。
#pragma once

#include "wd_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// A. 掌握位图
// ---------------------------------------------------------------------------

// 装 word_count 个 bit 需要多少字节。
static inline uint32_t wd_progress_bitmap_bytes(uint32_t word_count) {
    return (word_count + 7u) / 8u;
}

// 位图第一个字节里、"最后一个 bit 之后"的多余位。这些位必须是 0 ——
// 否则 wd_progress_mastered 会数出比实际词数更多的"已掌握"，屏幕上就会出现
// 「已掌握 1802 / 1801」这种不可能的进度。
void wd_progress_clear(uint8_t *bits, uint32_t word_count);

bool wd_progress_get(const uint8_t *bits, uint32_t word_count, uint32_t index);
void wd_progress_set(uint8_t *bits, uint32_t word_count, uint32_t index, bool mastered);

// 已掌握的词数。**按 word_count 封顶**，不去数尾部那些本不该存在的位。
uint32_t wd_progress_mastered(const uint8_t *bits, uint32_t word_count);

// 从 from（含）开始往后找第一个未掌握的词；绕回开头再找一遍。
// 全部掌握时返回 word_count —— 这个值不是一个合法下标，调用方必须判它。
uint32_t wd_progress_next_unmastered(const uint8_t *bits, uint32_t word_count,
                                     uint32_t from);

// ---------------------------------------------------------------------------
// B. 断点位置
// ---------------------------------------------------------------------------

#define WD_RESUME_MAGIC 0x4457u   // 'WD'，小端存下来是 57 44
#define WD_RESUME_FORMAT 1u       // 布局版本；改字段顺序时必须 +1
#define WD_RESUME_BYTES 22u       // 20 字节正文 + 2 字节校验和

typedef struct {
    uint32_t content_version;  // = wd_blob_t.version；不符则位置作废
    uint32_t book_index;
    uint32_t offset;           // 书内偏移
    uint32_t seed;             // 出题种子，用于复现"当前这一题"
} wd_resume_t;

typedef enum {
    WD_RESUME_OK = 0,
    WD_RESUME_ERR_NULL,
    WD_RESUME_ERR_SHORT,     // 长度不足
    WD_RESUME_ERR_MAGIC,     // 不是本模块写的数据
    WD_RESUME_ERR_FORMAT,    // 布局版本不符（保存方是新版固件）
    WD_RESUME_ERR_CHECKSUM,  // 校验和不符 —— 数据被破坏
    WD_RESUME_ERR_VERSION,   // 内容换了。**正常路径**：调用方据此从头开始，不必报错
    WD_RESUME_ERR_RANGE,     // 书序号或书内偏移越界
} wd_resume_err_t;

const char *wd_resume_err_text(wd_resume_err_t err);

// 序列化。返回写入的字节数；cap 不足返回 0（**绝不写半个记录**：
// 半个记录读回来时长度看着是对的，是最难查的一类进度 bug）。
size_t wd_resume_encode(const wd_resume_t *resume, uint8_t *out, size_t cap);

// 反序列化 + 校验。blob 用来判定 content_version 与下标范围。
//
// 校验顺序是有讲究的：先长度、再魔数、再布局版本、再校验和、**最后**才是
// 内容版本与范围。理由：前面几项说明"这份数据坏了"，最后一项说明"这份数据是好的、
// 只是过时了"。两者对用户的含义完全不同 —— 前者该报错，后者该静默重置。
// 顺序反了的话，坏数据会被当成"过时"而静默丢掉，真正的损坏就永远看不见。
wd_resume_err_t wd_resume_decode(const uint8_t *data, size_t len,
                                 const wd_blob_t *blob, wd_resume_t *out);

#ifdef __cplusplus
}  // extern "C"
#endif
