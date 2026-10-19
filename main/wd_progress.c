// main/wd_progress.c —— 学习进度的序列化（纯逻辑，可在宿主机编译）。
#include "wd_progress.h"

#include <string.h>

// ---------------------------------------------------------------------------
// A. 掌握位图
// ---------------------------------------------------------------------------

void wd_progress_clear(uint8_t *bits, uint32_t word_count) {
    if (!bits) return;
    memset(bits, 0, wd_progress_bitmap_bytes(word_count));
}

bool wd_progress_get(const uint8_t *bits, uint32_t word_count, uint32_t index) {
    if (!bits || index >= word_count) return false;
    return (bits[index >> 3] >> (index & 7u)) & 1u;
}

void wd_progress_set(uint8_t *bits, uint32_t word_count, uint32_t index, bool mastered) {
    if (!bits || index >= word_count) return;
    const uint8_t mask = (uint8_t)(1u << (index & 7u));
    if (mastered) {
        bits[index >> 3] |= mask;
    } else {
        bits[index >> 3] &= (uint8_t)~mask;
    }
}

uint32_t wd_progress_mastered(const uint8_t *bits, uint32_t word_count) {
    if (!bits) return 0;

    uint32_t total = 0;
    const uint32_t full_bytes = word_count / 8u;

    // 整字节部分：一次数 8 个，避免 19513 次移位。
    for (uint32_t i = 0; i < full_bytes; ++i) {
        uint8_t byte = bits[i];
        while (byte) {
            byte &= (uint8_t)(byte - 1u);  // 抹掉最低的 1
            ++total;
        }
    }

    // 尾部不足 8 位的那几个 bit：**只数 word_count 范围内的**。
    // 若这里不看边界，位图里残留的多余位会把"已掌握"数撑到比词数还大。
    const uint32_t tail_start = full_bytes * 8u;
    for (uint32_t index = tail_start; index < word_count; ++index) {
        if (wd_progress_get(bits, word_count, index)) ++total;
    }
    return total;
}

uint32_t wd_progress_next_unmastered(const uint8_t *bits, uint32_t word_count,
                                     uint32_t from) {
    if (!bits || word_count == 0) return word_count;

    // 从 from 往后，再绕回 [0, from)。
    // 两段都不含 word_count 本身 —— 返回它表示"整本都掌握了"。
    for (uint32_t index = (from < word_count ? from : 0); index < word_count; ++index) {
        if (!wd_progress_get(bits, word_count, index)) return index;
    }
    for (uint32_t index = 0; index < from && index < word_count; ++index) {
        if (!wd_progress_get(bits, word_count, index)) return index;
    }
    return word_count;
}

// ---------------------------------------------------------------------------
// B. 断点位置
// ---------------------------------------------------------------------------

// Fletcher-16（mod 255）。
//
// 选它而不是 CRC32 的理由：这条记录正文只有 20 字节，CRC32 的查表或位运算
// 开销没有意义；而 Fletcher 只要两行加法，可以在宿主测试里**手算一遍**对撞 ——
// 校验算法本身也要能被验证，否则"校验和永远相等"这种 bug 会一直绿着。
//
// 它要挡的是**半写**：NVS 提交前掉电时，理论上有机会留下一条长度正确、
// 内容却是上一次的记录。魔数和格式字段在开头，可能恰好对上，于是坏数据被
// 当成好数据用 —— 表现为"开机后跳到某个莫名其妙的位置"。
static uint16_t checksum16(const uint8_t *data, size_t len) {
    uint32_t low = 0;
    uint32_t high = 0;
    for (size_t i = 0; i < len; ++i) {
        low = (low + data[i]) % 255u;
        high = (high + low) % 255u;
    }
    return (uint16_t)((high << 8) | low);
}

const char *wd_resume_err_text(wd_resume_err_t err) {
    switch (err) {
        case WD_RESUME_OK:             return "正常";
        case WD_RESUME_ERR_NULL:       return "进度指针为空";
        case WD_RESUME_ERR_SHORT:      return "进度记录长度不足";
        case WD_RESUME_ERR_MAGIC:      return "进度记录魔数不符";
        case WD_RESUME_ERR_FORMAT:     return "进度记录版本不符";
        case WD_RESUME_ERR_CHECKSUM:   return "进度记录校验和不符";
        case WD_RESUME_ERR_VERSION:    return "词库已更新，进度重新开始";
        case WD_RESUME_ERR_RANGE:      return "进度位置越界";
    }
    return "未知错误";
}

size_t wd_resume_encode(const wd_resume_t *resume, uint8_t *out, size_t cap) {
    if (!resume || !out) return 0;
    if (cap < WD_RESUME_BYTES) return 0;

    out[0] = (uint8_t)(WD_RESUME_MAGIC & 0xffu);
    out[1] = (uint8_t)((WD_RESUME_MAGIC >> 8) & 0xffu);
    out[2] = (uint8_t)WD_RESUME_FORMAT;
    out[3] = 0;  // 保留

    const uint32_t values[4] = {
        resume->content_version, resume->book_index, resume->offset, resume->seed,
    };
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t value = values[i];
        out[4 + i * 4 + 0] = (uint8_t)(value & 0xffu);
        out[4 + i * 4 + 1] = (uint8_t)((value >> 8) & 0xffu);
        out[4 + i * 4 + 2] = (uint8_t)((value >> 16) & 0xffu);
        out[4 + i * 4 + 3] = (uint8_t)((value >> 24) & 0xffu);
    }

    const uint16_t sum = checksum16(out, 20);
    out[20] = (uint8_t)(sum & 0xffu);
    out[21] = (uint8_t)((sum >> 8) & 0xffu);
    return WD_RESUME_BYTES;
}

wd_resume_err_t wd_resume_decode(const uint8_t *data, size_t len,
                                 const wd_blob_t *blob, wd_resume_t *out) {
    if (!data || !blob || !out) return WD_RESUME_ERR_NULL;
    memset(out, 0, sizeof(*out));

    // 长度先判：后面所有读取都假定至少有 22 字节。
    if (len < WD_RESUME_BYTES) return WD_RESUME_ERR_SHORT;

    if (wd_rd16(data) != WD_RESUME_MAGIC) return WD_RESUME_ERR_MAGIC;
    if (data[2] != (uint8_t)WD_RESUME_FORMAT) return WD_RESUME_ERR_FORMAT;

    const uint16_t want = checksum16(data, 20);
    if (wd_rd16(data + 20) != want) return WD_RESUME_ERR_CHECKSUM;

    wd_resume_t parsed;
    parsed.content_version = wd_rd32(data + 4);
    parsed.book_index = wd_rd32(data + 8);
    parsed.offset = wd_rd32(data + 12);
    parsed.seed = wd_rd32(data + 16);

    // 内容版本不符 = 词库换了。位置本身没问题，只是对不上新词库 ——
    // 这是**正常路径**，调用方据此从头开始即可，不该在屏幕上报错。
    if (parsed.content_version != blob->version) return WD_RESUME_ERR_VERSION;

    if (parsed.book_index >= blob->book_count) return WD_RESUME_ERR_RANGE;

    wd_book_t book;
    if (!wd_manifest_book_at(blob, parsed.book_index, &book)) return WD_RESUME_ERR_RANGE;
    if (parsed.offset >= book.count) return WD_RESUME_ERR_RANGE;

    *out = parsed;
    return WD_RESUME_OK;
}
