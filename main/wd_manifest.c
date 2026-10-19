// main/wd_manifest.c —— 词库 blob 的清单层（纯逻辑，可在宿主机编译）。
#include "wd_manifest.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 池内字符串读取
//
// blob 里的三个字符串字段（key / name / short）存的都是**池内偏移**。
// 这个 helper 是唯一把「池内偏移」变成「可读 C 字符串」的地方 —— 转换只写一遍，
// 就不会出现某一条路径忘了加 pool_off 的情况。
//
// 必须带上限扫描：blob 损坏时池里可能根本没有 NUL，一路读下去会走出附件边界
// （Flash 里跨段读取不会崩，但会读到别的数据，表现得像"书名变成了随机字节"）。
// ---------------------------------------------------------------------------
static bool pool_str(const wd_blob_t *blob, uint32_t off, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';

    const uint32_t pool_len = blob->total_size - blob->pool_off;
    if (off >= pool_len) return false;   // 偏移落在池外

    const char *begin = (const char *)(blob->data + blob->pool_off + off);
    const char *limit = (const char *)(blob->data + blob->total_size);

    size_t n = 0;
    while (begin + n < limit && begin[n] != '\0') {
        if (n + 1 >= cap) return false;  // 装不下（含结尾 NUL）
        out[n] = begin[n];
        ++n;
    }
    if (begin + n >= limit) return false;  // 扫到池尾都没见到 NUL
    out[n] = '\0';
    return true;
}

// key 要进 NVS 键名，因此必须是**可见 ASCII 且无空格**。
// 允许 0x21..0x7e：'.' 和 '=' 本来就不合法（NVS 键名禁用），但这里先只卡
// "ASCII 可见"，具体字符集由 wd_store 决定 —— 本函数只负责挡住 UTF-8 和空格。
static bool is_ascii_key(const char *text) {
    if (!text[0]) return false;
    for (const char *p = text; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}

const char *wd_manifest_err_text(wd_manifest_err_t err) {
    switch (err) {
        case WD_MANIFEST_OK:                return "正常";
        case WD_MANIFEST_ERR_NULL:          return "内容指针为空";
        case WD_MANIFEST_ERR_TOO_SMALL:     return "内容文件过小";
        case WD_MANIFEST_ERR_MAGIC:         return "内容不是词库（魔数不符）";
        case WD_MANIFEST_ERR_RESERVED:      return "内容布局版本不符";
        case WD_MANIFEST_ERR_TOTAL:         return "内容长度与文件头不符（截断或粘连）";
        case WD_MANIFEST_ERR_COUNTS:        return "书数或词数不合法";
        case WD_MANIFEST_ERR_LAYOUT:        return "内容分段偏移不合法";
        case WD_MANIFEST_ERR_BOOK_RANGE:    return "书目覆盖的区间不连续";
        case WD_MANIFEST_ERR_TEXT:          return "书目文字损坏或超长";
    }
    return "未知错误";
}

wd_manifest_err_t wd_manifest_open(const uint8_t *data, size_t size, wd_blob_t *out) {
    if (!data || !out) return WD_MANIFEST_ERR_NULL;
    memset(out, 0, sizeof(*out));

    if (size < WD_BLOB_HEADER_SIZE) return WD_MANIFEST_ERR_TOO_SMALL;
    if (memcmp(data, WD_BLOB_MAGIC, 4) != 0) return WD_MANIFEST_ERR_MAGIC;

    const uint32_t version    = wd_rd32(data + 4);
    const uint16_t book_count = wd_rd16(data + 8);
    const uint16_t reserved   = wd_rd16(data + 10);
    const uint32_t word_count = wd_rd32(data + 12);
    const uint32_t books_off  = wd_rd32(data + 16);
    const uint32_t words_off  = wd_rd32(data + 20);
    const uint32_t pool_off   = wd_rd32(data + 24);
    const uint32_t total      = wd_rd32(data + 28);

    if (reserved != 0) return WD_MANIFEST_ERR_RESERVED;
    // version 只是指纹，不参与判定：换了版本号但布局没变（比如只改了一个释义）
    // 时 blob 仍然完全可用。真正决定可用性的是下面这些结构约束。
    (void)version;

    // 长度必须**恰好**等于文件头里写的。少一个字节说明附件被截断（Flash 分区
    // 不够、下载中断），多一个字节说明有东西被追加到了附件后面 —— 两种都不能用。
    if (total != size) return WD_MANIFEST_ERR_TOTAL;
    if (book_count == 0 || book_count > WD_MAX_BOOKS) return WD_MANIFEST_ERR_COUNTS;
    if (word_count == 0 || word_count > WD_MAX_WORDS) return WD_MANIFEST_ERR_COUNTS;

    // 三段必须首尾相接、不留缝、不重叠。溢出不是问题：book_count ≤ 32、
    // word_count ≤ 2^20，下面的乘法离 2^32 很远。
    if (books_off != WD_BLOB_HEADER_SIZE) return WD_MANIFEST_ERR_LAYOUT;
    if (words_off != books_off + (uint32_t)book_count * WD_BOOK_ENTRY_SIZE)
        return WD_MANIFEST_ERR_LAYOUT;
    if (pool_off != words_off + (word_count + 1u) * 4u) return WD_MANIFEST_ERR_LAYOUT;
    if (!(books_off < words_off && words_off < pool_off && pool_off <= total))
        return WD_MANIFEST_ERR_LAYOUT;

    out->data = data;
    out->size = size;
    out->version = version;
    out->book_count = book_count;
    out->word_count = word_count;
    out->books_off = books_off;
    out->words_off = words_off;
    out->pool_off = pool_off;
    out->total_size = total;

    // 书表：first 必须**正好**接在上一本的末尾，且最后一本正好收在 word_count。
    // 只检查 first+count <= word_count 是不够的：那样"中间空出一段词"和
    // "两本书指向同一批词"都能通过，而症状分别是某些词永远学不到、
    // 以及两本书的进度互相污染 —— 都很难从屏幕上看出来。
    uint32_t expected_first = 0;
    for (unsigned i = 0; i < book_count; ++i) {
        const uint8_t *entry = data + books_off + (size_t)i * WD_BOOK_ENTRY_SIZE;
        const uint32_t name_off  = wd_rd32(entry + WD_BOOK_FIELD_NAME);
        const uint32_t short_off = wd_rd32(entry + WD_BOOK_FIELD_SHORT);
        const uint32_t key_off   = wd_rd32(entry + WD_BOOK_FIELD_KEY);
        const uint32_t first     = wd_rd32(entry + WD_BOOK_FIELD_FIRST);
        const uint32_t count     = wd_rd32(entry + WD_BOOK_FIELD_COUNT);

        if (count == 0) return WD_MANIFEST_ERR_BOOK_RANGE;
        if (first != expected_first) return WD_MANIFEST_ERR_BOOK_RANGE;
        if (first + count > word_count) return WD_MANIFEST_ERR_BOOK_RANGE;
        expected_first = first + count;

        wd_book_t book;
        memset(&book, 0, sizeof(book));
        if (!pool_str(out, key_off, book.key, sizeof(book.key))) return WD_MANIFEST_ERR_TEXT;
        if (!pool_str(out, name_off, book.name, sizeof(book.name))) return WD_MANIFEST_ERR_TEXT;
        if (!pool_str(out, short_off, book.short_name, sizeof(book.short_name)))
            return WD_MANIFEST_ERR_TEXT;
        if (!is_ascii_key(book.key)) return WD_MANIFEST_ERR_TEXT;
    }
    if (expected_first != word_count) return WD_MANIFEST_ERR_BOOK_RANGE;

    return WD_MANIFEST_OK;
}

bool wd_manifest_book_at(const wd_blob_t *blob, unsigned index, wd_book_t *out) {
    if (!blob || !out) return false;
    memset(out, 0, sizeof(*out));
    if (index >= blob->book_count) return false;

    const uint8_t *entry = blob->data + blob->books_off + (size_t)index * WD_BOOK_ENTRY_SIZE;
    // open() 已经把这三个字符串校验过一遍，这里不会失败；但返回值仍然检查 ——
    // 依赖"上游一定校验过"的代码在重构时会变成静默的乱码。
    const bool ok =
        pool_str(blob, wd_rd32(entry + WD_BOOK_FIELD_NAME), out->name, sizeof(out->name)) &&
        pool_str(blob, wd_rd32(entry + WD_BOOK_FIELD_SHORT), out->short_name,
                 sizeof(out->short_name)) &&
        pool_str(blob, wd_rd32(entry + WD_BOOK_FIELD_KEY), out->key, sizeof(out->key));
    if (!ok) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->first = wd_rd32(entry + WD_BOOK_FIELD_FIRST);
    out->count = wd_rd32(entry + WD_BOOK_FIELD_COUNT);
    return true;
}

int wd_manifest_find_book(const wd_blob_t *blob, const char *key) {
    if (!blob || !key || !key[0]) return -1;
    for (unsigned i = 0; i < blob->book_count; ++i) {
        wd_book_t book;
        if (!wd_manifest_book_at(blob, i, &book)) continue;
        if (strcmp(book.key, key) == 0) return (int)i;
    }
    return -1;
}
