// main/wd_manifest.h —— 词库 blob 的**清单层**（文件头 + 书表）的纯解析。
//
// 刻意不依赖 ESP-IDF / LVGL：与基座 english-podcast 的 pdq_manifest 同一形制，
// 能被宿主机测试直接编译运行 —— 改一行解析就能秒级验证，不必先烧进设备看屏幕。
//
// 与 pdq_manifest 的差别只有一处：那边解析的是**文本行**（每节课一行，运行期还要
// 反复 strchr 找 '|' 和 '\n'），这边解析的是**二进制 blob**（由
// tools/wordbook/gen_content.py 产出，2.76 MB / 19513 条词）。二进制不是炫技：
// 一次内存映射就能按偏移直达任意一条，体积也比文本小得多，代价是需要一份
// 精确的布局约定 —— 而布局约定恰恰是最容易两端跑偏的东西，所以它有一组
// 跨语言固定向量（tests/wd_content_fixture.h）和一条独立的读回校验
// （tools/wordbook/verify_blob.py，故意不复用生成器的任何代码）。
//
// 布局（全部 little-endian）：
//
//   偏移  0  header 32 字节
//             magic[4]="WDB1"  version u32   book_count u16   reserved u16
//             word_count u32    books_off u32   words_off u32  pool_off u32
//             total_size u32
//   偏移 32  书表   book_count x 20 字节
//             name_off u32  short_off u32  key_off u32  first u32  count u32
//   ...      词偏移表 (word_count+1) x 4 字节（池内偏移；末项 = 池长度）
//   ...      字符串池 UTF-8，NUL 分隔；一个词的 5 个字段连续排列
//
// 三条硬约束，每条都有理由：
//
//  1. **所有多字节读取逐字节拼**，绝不用 `*(const uint32_t *)p`。
//     blob 作为二进制附件落在 Flash 的 rodata 段，**不保证 4 字节对齐**；
//     RISC-V 上的未对齐字访问会直接异常 —— 不是读错值，是崩。
//
//  2. **不做「尽力解析」**。魔数不对、段偏移不单调、书表覆盖不上词条 —— 一律
//     打开失败并给出具体原因。半个词库比没有词库更糟：屏幕会显示乱码而不是报错。
//
//  3. **池内字符串的偏移 = blob 里存的值 + pool_off**。blob 里存的是**池内偏移**，
//     不是文件偏移。这一条曾经栽过：第一版忘了加 pool_off，于是把文件头的
//     "WDB1" 当成了书名解析出来（详见 tools/wordbook/verify_blob.py 里的注释）。
//
//  4. **书表里三个字符串字段的顺序是 name / short / key**。这一条也栽过：
//     最初按 key / name / short 解析，于是真实 blob 打开时把中文名当成了 key，
//     报「书目文字损坏或超长」—— 合成 blob 的宿主测试当场抓住了它。
//     下面那组 WD_BOOK_FIELD_* 常量就是为这件事立的规矩：**不许再写裸偏移**。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wd_assert.h"

// 这个模块是 .c（C 编译），但被 .cc（C++）调用。
// 少了 extern "C" 两边就会各自按自己的规则修饰符号名，编译能过、链接期才报
// undefined reference —— 而且报的是"函数不存在"，看上去像漏了源文件。
#ifdef __cplusplus
extern "C" {
#endif

#define WD_BLOB_MAGIC "WDB1"
#define WD_BLOB_HEADER_SIZE 32u
#define WD_BOOK_ENTRY_SIZE 20u

// 书表里每个字段的字节偏移。
//
// **不许在读取处写裸数字**。这三个值必须与 tools/wordbook/gen_content.py 的写入
// 顺序一致（name / short / key）—— 曾经按 key / name / short 读过一次，
// 真实 blob 打开时报「书目文字损坏或超长」（key 位置读到的是中文名，不是可见
// ASCII）。给偏移起名之后，读取处一眼就能看出读的是哪个字段。
#define WD_BOOK_FIELD_NAME 0u
#define WD_BOOK_FIELD_SHORT 4u
#define WD_BOOK_FIELD_KEY 8u
#define WD_BOOK_FIELD_FIRST 12u
#define WD_BOOK_FIELD_COUNT 16u

// 上限（不是拍的：book_count 取 6 本书的 5 倍余量；word_count 取 19513 的 53 倍）。
// 上限的作用是**尽早拒绝一个形状完全不对的 blob**（比如把字体文件当词库传进来），
// 而不是限制未来加书。
#define WD_MAX_BOOKS 32u
#define WD_MAX_WORDS (1u << 20)

// key 会进 NVS 键名（`wdp.<key>`，见 main/wd_store.c）。NVS 键名上限 15 字符
// （不含 NUL），前缀占 4，所以 key 最多 11 —— 这是硬约束：超了写进度会
// 静默失败，表现成「学完一页翻回来进度没了」，极难排查。
// 现有最长 key 是 ChuZhong_3（10 字符），留 1 个余量。
//
// WD_BOOK_KEY_MAX 是**缓冲区大小**（11 字符 + NUL = 12）。
// 下面这条断言把上面那段说明从"算出来的话"变成"编译期事实" ——
// 算出来的东西不钉住，改前缀或者改上限时就会漂。
// 15 与 4 写字面量是因为纯逻辑层不能 include nvs.h
// （它必须能被宿主 gcc 直接编，见 tools/run_host_tests.sh）。
#define WD_BOOK_KEY_MAX 12
WD_STATIC_ASSERT(4 + (WD_BOOK_KEY_MAX - 1) <= 15,
                 "wdp.<key> 超过 NVS 键名上限 —— 进度会静默写失败");
#define WD_BOOK_NAME_MAX 24    // 最长中文名「初中核心词汇」UTF-8 占 18 B
#define WD_BOOK_SHORT_MAX 12   // 最长简称「初中」占 6 B

typedef struct {
    const uint8_t *data;   // 不拥有；由调用方保证生命周期（设备端是 rodata，永久）
    size_t size;
    uint32_t version;      // 内容指纹，由生成器算；只用于日志与进度失效判定
    uint16_t book_count;
    uint32_t word_count;
    uint32_t books_off;
    uint32_t words_off;
    uint32_t pool_off;
    uint32_t total_size;
} wd_blob_t;

typedef struct {
    char key[WD_BOOK_KEY_MAX];          // ChuZhong_3 —— 进度的分组键、日志里的标识
    char name[WD_BOOK_NAME_MAX];        // 初中核心词汇 —— 屏幕上显示的
    char short_name[WD_BOOK_SHORT_MAX]; // 初中 —— 选书页那一列窄标签
    uint32_t first;                     // 该书第一条词的**全局**词序
    uint32_t count;
} wd_book_t;

// 打开失败的原因。
//
// 为什么返回枚举而不是 bool：设备上刷错内容时，屏幕上只能显示一行字，而
// 「magic 不对」和「offset 对不上」指向完全不同的排查方向（刷错文件 / 生成器有 bug）。
// 把原因带出来，一线就能定位。
typedef enum {
    WD_MANIFEST_OK = 0,
    WD_MANIFEST_ERR_NULL,      // 空指针
    WD_MANIFEST_ERR_TOO_SMALL, // 连 32 字节的头都不够
    WD_MANIFEST_ERR_MAGIC,     // 不是 WDB1 开头 —— 多半刷错了文件
    WD_MANIFEST_ERR_RESERVED,  // 保留字段非 0 —— 布局版本对不上
    WD_MANIFEST_ERR_TOTAL,     // header 里的 total_size 与实际长度不符（截断/粘连）
    WD_MANIFEST_ERR_COUNTS,    // 书数或词数为 0 / 超过上限
    WD_MANIFEST_ERR_LAYOUT,    // 各段偏移不单调、长度对不上、越界
    WD_MANIFEST_ERR_BOOK_RANGE,// 书表的 first/count 不连续或不覆盖全部词条
    WD_MANIFEST_ERR_TEXT,      // 池里的字符串没有 NUL 结尾 / 超长 / key 不是可见 ASCII
} wd_manifest_err_t;

// 稳定的中文诊断文案（屏幕上与日志里都用它，保证两处说法一致）。
const char *wd_manifest_err_text(wd_manifest_err_t err);

// 解析文件头并**全量校验书表**（含池里的 18 个字符串）。
//
// 校验在这里做一次，之后的 book_at/find_book 就只是取字段 —— 访问期不再防御，
// 也不再有"这个字段可能没校验过"的模糊地带。代价是启动时一次几百微秒的扫描，
// 换来的是整个运行期不需要在每个调用点写 if。
wd_manifest_err_t wd_manifest_open(const uint8_t *data, size_t size, wd_blob_t *out);

// 取第 index 本书。越界返回 false 并清空 out。
bool wd_manifest_book_at(const wd_blob_t *blob, unsigned index, wd_book_t *out);

// 按 key 找书的序号；找不到返回 -1。
//
// 为什么需要它：进度按 key 存。用户换到另一本书再换回来，或者以后书单顺序
// 调整（books.json 里插一本），都要靠 key 而不是序号才能找回进度。
int wd_manifest_find_book(const wd_blob_t *blob, const char *key);

// ---------------------------------------------------------------------------
// blob 里所有多字节整数都是 little-endian，且**必须逐字节拼**（见文件头第 1 条）。
// 这两个 inline 是 wd_content.c 与 wd_manifest.c 共同的唯一读法。
// ---------------------------------------------------------------------------
static inline uint16_t wd_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t wd_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifdef __cplusplus
}  // extern "C"
#endif
