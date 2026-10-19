<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资产（Assets）

本目录按资产类型存放本应用随固件分发的字体、生成的词库内容与图片。每类资产放在对应子目录，
并记录其去向、命名、集成方式与来源/许可。不要把二进制资产与 Markdown 文档混放。

## 字体（Fonts）

可复用字体文件与生成出来的字库源码放在 `fonts/`。

- 命名要能看出字族、字重、字号与格式。
- 记录来源、许可、字符范围、转换命令与预期去向。
- 加字体前先算 Flash 与内部 RAM 的代价；ESP32-C3 没有 PSRAM。
- 许可不允许再分发的字体不要提交。

### 英语词典应用

本应用随固件分发**六份**未压缩的 LVGL 点阵字库 —— 主字体的三档，以及每档配套的
一份 IPA 补充字库：

| 文件 | 格式 | 用途 |
| --- | --- | --- |
| [`fonts/wd_font_16.c`](fonts/wd_font_16.c) | 16 px，4 bpp，未压缩 LVGL | 状态栏、提示条、列表行、例句、测验反馈行。 |
| [`fonts/wd_font_24.c`](fonts/wd_font_24.c) | 24 px，4 bpp，未压缩 LVGL | 32px 一行放不下的单词、中文释义、测验选项、设置项取值。 |
| [`fonts/wd_font_32.c`](fonts/wd_font_32.c) | 32 px，4 bpp，未压缩 LVGL | 整行放得下时的单词本身。 |
| [`fonts/wd_ipa_16.c`](fonts/wd_ipa_16.c) | 16 px，4 bpp，未压缩 LVGL | 主字体缺的那 18 个音标码点，16px 档。 |
| [`fonts/wd_ipa_24.c`](fonts/wd_ipa_24.c) | 24 px，4 bpp，未压缩 LVGL | 同一批 18 个码点，24px 档。 |
| [`fonts/wd_ipa_32.c`](fonts/wd_ipa_32.c) | 32 px，4 bpp，未压缩 LVGL | 同一批 18 个码点，32px 档。 |

**为什么要两套字体。** 主字体里没有词库用到的这 18 个音标码点 ——
`ŋ ɒ ɔ ə ɚ ɛ ɜ ɝ ɪ ʃ ʊ ʌ ʒ ʳ ˈ ˌ ː ˚` —— 而几乎每个词条都含 `ə`、`ɪ` 或 `ɛ`。
没有补充字库，屏幕上每一条音标都会是一排方框。补充字库**只**含这 18 个码点，两套
字符集不重叠；LVGL 在运行期通过 `lv_font_t.fallback` 解析缺字形，保证每个字形始终
留在它被绘制时所属的那张表里。

- **来源（主字体）：** Noto Sans CJK SC Regular，SIL Open Font License 1.1。
- **来源（IPA 补充）：** Noto Sans，SIL Open Font License 1.1。
- **字符范围：** 主字体每档 3,768 个码点 —— 3,632 个 CJK 汉字，加可打印 ASCII、
  释义里用到的音标字母、界面自己绘制的 CJK 标点。32px 档**故意**压到 **93 个码点**：
  只有 ASCII、IPA 与标点，**一个汉字都没有** —— 这正是整套字库能压在 1.6 MB 以内
  的原因，也是界面从不用 32px 画汉字的原因。
- **转换器：** `lv_font_conv` 1.5.3，参数 `--bpp 4 --no-compress --format lvgl`。
- **重新生成（在仓库根目录执行）：** `python3 tools/wordbook/gen_font.py`，随后
  `python3 tools/wordbook/gen_font.py --check` 确认随仓库分发的文件仍与来源一致。
  两份母字体由 `WD_MAIN_FONT` / `WD_IPA_FONT` 定位，默认指向 `D:/esp/fontsrc/`。

## 内容（Content）

生成出来的词库数据放在 `content/`。设备只读一个文件。

| 文件 | 格式 | 用途 |
| --- | --- | --- |
| [`content/wd_content.bin`](content/wd_content.bin) | 二进制 blob，2,899,123 字节 | 整个词库：6 本书、19,513 个词条，每条含单词、音标、中文释义与中英例句。 |

- **来源：** `tools/wordbook/words.txt` 是唯一事实源 —— 每行一条、制表符分隔。
  `tools/wordbook/books.json` 列出六本书及其顺序。
- **生成器：** `python3 tools/wordbook/gen_content.py`；门禁用
  `python3 tools/wordbook/gen_content.py --check`。
- **布局：** 32 字节文件头、每本书 20 字节的记录、`(word_count + 1)` 项的偏移表，
  之后是一段 NUL 分隔的字符串池 —— 同一个词的 5 个字段在池里相邻排列，所以一个偏移
  就能定位整条词。全部小端，且**逐字节读取**：blob 被链接进 Flash，不保证 4 字节对齐。
  完整布局写在 `tools/wordbook/gen_content.py` 头部。
- **为什么用 blob 而不是 C 数组：** 2.9 MB 的数组会明显拖慢编译与链接，生成出来的
  源文件也无法审阅。`main/CMakeLists.txt` 用 `target_add_binary_data` 嵌入它，
  `main/wd_content.h` 声明链接器按文件名生成的符号。**不要**再把 blob 列进 `SRCS`。
- **第二个产物：** 同一个生成器还会写出 `tests/wd_content_fixture.h`，给出若干真实
  记录的逐字段期望值，字段顺序或拼接方式一变，宿主测试当场就会失败。

## 图片（Images）

可复用源图与生成出来的展示图放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 x 2160，JPEG | 项目 README 的产品主图。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 x 724，PNG RGBA | 硬件规格的技术信息图，作为参考资产保留。2026-09-17 为本仓库家族生成。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 x 336，PNG RGBA | 用于浅色背景的透明字标。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 x 336，PNG RGBA | 白色字标，供 README 在 GitHub 深色模式下使用。 |
| [`images/logo.png`](images/logo.png) | 2048 x 1024，PNG RGBA | 两个字标由它裁出的原始方形标识。 |

- 这五个文件与兄弟仓库（`ai-passport`、`daodejing-daily`、`qiaopi-quiz`）逐字节相同，
  是共用资产。它们只是文档，不进固件，不占 Flash。
- **本次修订不随仓库提供六屏界面图。** 六屏的几何就是 `main/wd_ui.h` 里的版式预算，
  并且在编译期有断言兜底；若要补截图，请保持 240 x 320 原生分辨率或它的整数倍。
- 图片里不要出现设备的配网二维码、凭据或个人信息。
