<p align="right">
  <strong>简体中文</strong> · <a href="word-book-to-flash-blob.md">English</a>
</p>

# 把词库变成 Flash 里的 blob

记录于开发[英语词典应用](english-dict/README.zh_CN.md)期间。设备自带六本书、19,513 条词条 ——
2,899,123 字节内容，装进 8 MB Flash、没有 PSRAM —— 而且读取时从不把一条记录拷进 RAM。
这篇写文件格式、为什么是 blob 而不是生成出来的 C 数组、以及那套让读写两头保持一致的
跨语言固定向量。

## 一个事实源，两份投影

内容住在 `tools/wordbook/words.txt`：每行一条、制表符分隔，五个字段顺序固定
（单词、音标、中文释义、英文例句、中文例句）。`tools/wordbook/books.json` 列出六本书及其顺序。
其余全部由 `gen_content.py` 从这两个文件投影出来，而投影是**被校验**的，不是被信任的：

- `assets/content/wd_content.bin` —— 固件链接进去的那个 blob；
- `tests/wd_content_fixture.h` —— 若干真实记录按逐字段期望值渲染出来，字段顺序或拼接方式一变，
  宿主测试立刻失败。

字段上限在生成期就强制，不只靠设备：`CAP_WORD = 24`、`CAP_IPA = 28`、`CAP_TRANS = 64`、
`CAP_SENT_EN = 104`、`CAP_SENT_CN = 39`。实际分发的词最长 18 字符、最长音标 21 字符，
所以这些上限留的是真余量 —— 它们同时是版式**被允许假设**的数字，这正是值得为一条超限词条
拒绝入库的理由。

## 布局

```
偏移 0      文件头，32 字节
               magic[4]="WDB1"  version u32  book_count u16  reserved u16
               word_count u32    books_off u32  words_off u32  pool_off u32
               total_size u32
偏移 32     书表，book_count x 20 字节
               name_off u32  short_off u32  key_off u32  first u32  count u32
...         词偏移表，(word_count + 1) x u32
               每个词第一条字段在池内的偏移；末项 = 池长度
...         字符串池，UTF-8，字段之间 NUL 分隔
               同一个词的 5 个字段连续排列：
               word\0ipa\0trans\0sent_en\0sent_cn\0
```

真正省事的设计决定是最后一行。因为一条词条的五个字段是连续的，**一个偏移就能定位整条词**，
偏移表是 `N + 1` 项而不是 `5N` 项。读取方用一次表查询、再加走最多五个 NUL 结尾的字符串，
就拿到「第 `i` 条、第 `k` 个字段」，没有任何交叉引用要解析。

每个字段都是小端，而且每个字段都**逐字节读**，绝不通过把指针强转成 `uint32_t *` 去读。
blob 被链接进 Flash，不保证 4 字节对齐；RISC-V 上非对齐读取会直接异常，而失败的表现会是
「某一条词条上随机崩溃」，而不是一个格式 bug。

## 为什么是 blob 而不是 C 数组

显然的替代方案是把 `const char content[] = {0x57, 0x44, ...}` 生成进一个 `.c` 文件，
这块板上以前试过。两项代价都是真的：

- 2.9 MB 的数组是被**编译**的、不是被 include 的，会明显拖慢构建，还会把每一处引用它的
  `compile_commands.json` 条目撑爆；
- 生成出来的源文件无法审阅。一次容量变更会以两百万行的 diff 出现，于是没人去看它。

改用 `main/CMakeLists.txt` 的 `target_add_binary_data` 嵌入 blob，`main/wd_content.h` 声明
链接器按文件名生成的 `_binary_..._start` / `_end` 符号。有一条很容易反向踩错的规则：
blob **不要**再出现在 `SRCS` 里，否则会被链接两次。

## 读取方被允许信任什么

因为生成器强制了上限、固定向量钉住了字段顺序，读取方可以保持很小：它知道一条词条就是
五个按固定顺序排列的 NUL 结尾字符串，运行期不必再验一遍格式。它**唯一**保留的结构性检查是
文件头 —— `magic`、`version`、`total_size` —— 于是一个过期或被截断的 blob 会在
`wd_content_init()` 处带着一条原因串失败，让启动页能显示出来，而不是被当作合法数据往下走。

这个分工才是重点：约束在生成期被证明，那里失败是笔记本上的一道红门禁；设备端只需要识别
「这个文件不是我构建时配套的那个」。

## 相关

- [英语词典应用](english-dict/README.zh_CN.md) —— 消费这个 blob 的应用。
- [用一条回退链让每个音标都不画成方框](ipa-fallback-font-chain.zh_CN.md) ——
  同一条「推导清单、而非维护清单」的规则用在字体上。
- [让掌握进度在 NVS 里可续](resumable-mastery-progress-in-nvs.zh_CN.md) ——
  写入侧：设备关于每本书真正存下来的那点状态。
