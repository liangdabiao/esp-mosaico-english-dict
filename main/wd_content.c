// main/wd_content.c —— 词条访问层（纯逻辑，可在宿主机编译）。
#include "wd_content.h"

#include <string.h>

const char *wd_content_err_text(wd_content_err_t err) {
    switch (err) {
        case WD_CONTENT_OK:              return "正常";
        case WD_CONTENT_ERR_NULL:        return "内容指针为空";
        case WD_CONTENT_ERR_OFFSETS:     return "词条偏移表不单调或长度不符";
        case WD_CONTENT_ERR_FIELDS:      return "词条字段数与偏移表对不上";
        case WD_CONTENT_ERR_NOT_FOUND:   return "词条序号越界";
        case WD_CONTENT_ERR_BOOK:        return "书目序号越界";
    }
    return "未知错误";
}

// 池的边界。偏移表里的值都是**池内偏移**，所以绝对地址 = data + pool_off + 值。
static inline const char *pool_begin(const wd_blob_t *blob) {
    return (const char *)(blob->data + blob->pool_off);
}

static inline uint32_t pool_len(const wd_blob_t *blob) {
    return blob->total_size - blob->pool_off;
}

static inline uint32_t word_offset_at(const wd_blob_t *blob, uint32_t index) {
    return wd_rd32(blob->data + blob->words_off + (size_t)index * 4u);
}

wd_content_err_t wd_content_verify(const wd_blob_t *blob, uint32_t *bad_index) {
    if (bad_index) *bad_index = 0;
    if (!blob || !blob->data) return WD_CONTENT_ERR_NULL;

    const uint32_t total_pool = pool_len(blob);

    for (uint32_t i = 0; i < blob->word_count; ++i) {
        const uint32_t at = word_offset_at(blob, i);
        const uint32_t next = word_offset_at(blob, i + 1);

        // 偏移必须严格前进（相等 = 空词条），且不能越过池尾。
        if (at >= next || next > total_pool) {
            if (bad_index) *bad_index = i;
            return WD_CONTENT_ERR_OFFSETS;
        }

        // 这一段里必须**正好**是 5 个 NUL 结尾的串。多一个字节少一个字节都拒 ——
        // 这正是"写侧与读侧布局跑偏"会留下的痕迹，而它恰恰是一切乱码的根源。
        const char *p = pool_begin(blob) + at;
        const char *stop = pool_begin(blob) + next;
        for (unsigned k = 0; k < WD_FIELD_COUNT; ++k) {
            while (p < stop && *p) ++p;
            if (p >= stop) {
                if (bad_index) *bad_index = i;
                return WD_CONTENT_ERR_FIELDS;
            }
            ++p;  // 跳过 NUL
        }
        if (p != stop) {
            if (bad_index) *bad_index = i;
            return WD_CONTENT_ERR_FIELDS;
        }
    }

    // 末项是"哨兵"，必须正好等于池长度 —— 否则最后一个词的边界就没有依据。
    if (word_offset_at(blob, blob->word_count) != total_pool) {
        if (bad_index) *bad_index = blob->word_count;
        return WD_CONTENT_ERR_OFFSETS;
    }
    return WD_CONTENT_OK;
}

bool wd_content_get(const wd_blob_t *blob, uint32_t index, wd_word_t *out) {
    if (!blob || !out) return false;
    if (index >= blob->word_count) return false;
    out->blob = blob;
    out->index = index;
    return true;
}

bool wd_content_get_in_book(const wd_blob_t *blob, unsigned book_index,
                            uint32_t offset, wd_word_t *out) {
    if (!blob || !out) return false;
    if (book_index >= blob->book_count) return false;

    const uint8_t *entry = blob->data + blob->books_off +
                           (size_t)book_index * WD_BOOK_ENTRY_SIZE;
    const uint32_t first = wd_rd32(entry + WD_BOOK_FIELD_FIRST);
    const uint32_t count = wd_rd32(entry + WD_BOOK_FIELD_COUNT);
    if (offset >= count) return false;

    out->blob = blob;
    out->index = first + offset;
    return true;
}

const char *wd_content_field(const wd_word_t *word, unsigned field) {
    // 空串而不是 NULL：调用方拿到它可以直接交给绘制函数，不必在每个调用点判空。
    static const char kEmpty[] = "";

    if (!word || !word->blob) return kEmpty;
    const wd_blob_t *blob = word->blob;
    if (word->index >= blob->word_count) return kEmpty;
    if (field >= WD_FIELD_COUNT) return kEmpty;

    const char *p = pool_begin(blob) + word_offset_at(blob, word->index);
    const char *limit = pool_begin(blob) + pool_len(blob);
    if (p >= limit) return kEmpty;

    // 跳到第 field 个字段：往前跳 field 个 NUL。
    // 正常情况下这一段已经过 verify 校验，跳不到界；这里的边界判断是给
    // "忘了调 verify" 的调用方兜底 —— 宁可显示空行，也不能读到池外。
    for (unsigned k = 0; k < field; ++k) {
        while (p < limit && *p) ++p;
        if (p >= limit) return kEmpty;
        ++p;
    }
    if (p >= limit) return kEmpty;
    return p;
}

int wd_content_book_of(const wd_blob_t *blob, uint32_t index) {
    if (!blob || index >= blob->word_count) return -1;
    for (unsigned i = 0; i < blob->book_count; ++i) {
        const uint8_t *entry = blob->data + blob->books_off +
                               (size_t)i * WD_BOOK_ENTRY_SIZE;
        const uint32_t first = wd_rd32(entry + WD_BOOK_FIELD_FIRST);
        const uint32_t count = wd_rd32(entry + WD_BOOK_FIELD_COUNT);
        if (index >= first && index < first + count) return (int)i;
    }
    return -1;
}

bool wd_content_local_of(const wd_blob_t *blob, uint32_t index, uint32_t *out_offset) {
    if (out_offset) *out_offset = 0;
    const int book = wd_content_book_of(blob, index);
    if (book < 0) return false;
    const uint8_t *entry = blob->data + blob->books_off +
                           (size_t)book * WD_BOOK_ENTRY_SIZE;
    if (out_offset) *out_offset = index - wd_rd32(entry + WD_BOOK_FIELD_FIRST);
    return true;
}
