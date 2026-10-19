# 英语词典 · ESP-Mosaico 触摸版

一句话：**一台 480×480 方屏触摸机上的离线背单词工具** —— 六本词书、19,513 条词，点一下查词、点卡片翻面看例句、四选一测验；词库和字库全在机器里，不联网也能用。发音第一次要联网，听过的会存进机器，以后离线也能重放。

- 平台：ESP-Mosaico（esp32s31 + CO5300 480×480 正方形触摸屏 + cst9220 触摸 + ES8311 音频），界面用 **LVGL 9.5**
- 词库：初中 / 高中 / 四级 / 六级 / 考研 / 雅思，共 **19,513 条**，编进固件，不插卡
- 发音：单词用有道公开接口，例句用百度 TTS；播放成功后自动存进 NAND，离线可重播
- 操作：**全部靠点按**，没有任何物理按键依赖

> 这是从 ESP32-C3 三键掌机版移植过来的 ESP-Mosaico 版本。屏幕、触摸、音频、配网都按这块板子重做了，交互从「上下 + 确定」变成「直接点」。

---

## 1. 一分钟看懂怎么玩

开机后如果没有保存过 Wi-Fi，屏幕会显示配网页：

1. 手机连上设备热点 **English Dict**；
2. 手机浏览器打开 **192.168.4.1**；
3. 选家里的 Wi-Fi、输入密码，配好后设备自动返回主界面。

配好网之后（其实不配也能查词，只是第一次发音需要联网）：

1. **选书页**：六本词书做成大卡片，点一本就开始学；底部还有「开始测验」和「学习进度」。
2. **词条页**：上面是单词、音标、中文释义；**点卡片本身**可以翻到例句面；底部两行按钮分别是「‹上一个 / 发音 / 读例句」和「看例句（或记住了） / 下一个›」。
3. **测验页**：屏幕给中文释义，下面四个英文单词，点一个就判对错；答对之后点结果区进入下一题。
4. **进度页**：每本书显示已掌握多少词；「重置本册」要点两次才会真的清空。
5. 顶栏左边是「‹返回」，右边常驻「设置」，音量这类常用项不用翻菜单。

**记住单词的两种方式：**

- 在词条页翻到例句面，点「记住了」——这个词就被标记为已掌握，并自动跳到下一个没掌握的；
- 在测验里答对——同样标记为已掌握。

进度按书分别存在 NVS 里，关机重启不会丢。

---

## 2. 六本书，19,513 条

| 词书 | 条数 | 词书 | 条数 |
| --- | ---: | --- | ---: |
| 初中核心词汇 | 1,801 | 考研核心词汇 | 4,533 |
| 高中核心词汇 | 3,668 | 雅思核心词汇 | 3,427 |
| 大学英语四级 | 3,739 | 大学英语六级 | 2,345 |

**总计 19,513 条**，按由易到难排列。

每条词包含：单词、音标、中文释义、英文例句、例句中文翻译。少数词条没有例句，界面把这种情况当常态处理，不会报错。

内容全部来自 `tools/wordbook/words.txt`（唯一事实源），生成一个约 **2.9 MB** 的二进制词库文件，直接编进固件。查词、翻面、测验都不访问网络。

---

## 3. 发音是怎么工作的

### 3.1 两个来源

| 内容 | 来源 | 特点 |
| --- | --- | --- |
| 单词发音 | 有道 `dictvoice?type=0` | 48 kHz 单声道，约 10 KB、1~2 秒 |
| 例句朗读 | 百度翻译 `gettts` | 16 kHz 单声道，整句读出来 |

例句朗读没有用有道：有道的整句接口在 2026-10 实测对整句一律返回 500。百度这个接口能直接返回 MP3，真机验证可用。

### 3.2 为什么 16 kHz 要升到 48 kHz

这块板的 ES8311 在 16 kHz 下播放会明显失真（听起来像尖叫）。所以固件在解码后做 **3 倍线性插值升采样**，统一按 48 kHz 送进 I2S，单词和例句走同一条播放通路。

对应的日志长这样：

```
16000Hz/1ch -> 单声道，3 倍升采到 48000Hz
```

### 3.3 听过的会留在机器里

- 缓存目录：`/nand/wdvoice`（SPI NAND + LittleFS，整卷约 **121.7 MB** 归本应用使用）；
- 文件名：单词/例句的 64 位 FNV-1a 哈希，分 256 个目录，每个分片最多 400 KiB、48 个文件；
- 命中缓存就完全不联网，第一次听某个词/某句话才需要 Wi-Fi；
- 全量预下载放不下：19,513 个词约 **252 MB**，大于整卷容量，所以采用「听过才存」。

**结论：词库完全离线；发音是「第一次联网，之后离线」；从没听过的词，在没网时点发音会提示失败。**

---

## 4. 屏幕与字号

480×480 的方屏，字号做了三档，不是随便定的：

| 档位 | 字号 | 用在哪 | 覆盖字符 |
| --- | ---: | --- | ---: |
| 大字 | 64 px | 单词本身 | 99 个码点 |
| 中字 | 48 px | 中文释义、例句中文翻译、设置标题 | 3,786 个码点 |
| 小字 | 32 px | 英文例句、音标、按钮、状态栏 | 3,786 个码点 |

几个刻意的选择：

- **英文例句用 32 px，中文翻译用 48 px**。英文句子长，32 px 能多放内容；中文翻译短，48 px 更好读。
- **64 px 档一个汉字都没有**。这一档只渲染单词与音标，所以字库只有几十个字形；如果把汉字也塞进来，光这一档就要多花几 MB Flash。
- **主字体缺 18 个音标符号**（`ŋ ɒ ɔ ə ɚ ɛ ɜ ɝ ɪ ʃ ʊ ʌ ʒ ʳ ˈ ˌ ː ˚`），所以每档都配了一份极小的 Noto Sans 补充字库，运行期通过 LVGL 的 `fallback` 回退链接上。没有它，音标会满屏方框。

字库由 `tools/wordbook/gen_font.py` 生成，只使用一条路径编入固件。

---

## 5. 构建与安装

### 5.1 这不是一个能单独打开就编译的工程

本工程是 **ESP-Mosaico 工作区里的一个应用**，路径必须是：

```
esp-mosaico-vibe-main/
├─ submodule/
│  ├─ esp-mosaico-bsp/
│  └─ esp-mosaico-utils/
└─ projects/
   └─ english-dict/     ← 本仓库
```

依赖关系：

- ESP-IDF 固定提交：`7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`，target `esp32s31`；
- BSP 与 ESP-Iris 组件来自工作区的 `submodule/`；
- `esp_littlefs` 使用工作区外的本地 clone（`main/idf_component.yml` 里的 `override_path: ../../../../esp_littlefs`）。如果你的目录不同，需要改这一行。

### 5.2 编译

在**工作区根目录**执行：

```powershell
powershell -ExecutionPolicy Bypass -File projects/english-dict/build_ed.ps1
```

增量编译通常几十秒到几分钟；脚本会自动检查 IDF 提交、处理 ninja 的 `/D:/` 缓存问题。

产物：

```
projects/english-dict/build/english_dict.bin
```

### 5.3 刷进设备

只使用工作区的 `mosaico.py`，不要直接用 `idf.py flash` / `esptool`：

```sh
# 首次安装，或者分区表、外部资源有变化
python mosaico.py iris system-update --project projects/english-dict

# 只是改了代码（分区表不变）
python mosaico.py iris app-update --project projects/english-dict

# 已经构建好，直接复用 build/english_dict.bin
python mosaico.py iris app-update --project projects/english-dict --skip-build
```

> 本机实测：`app-update` 第一次偶尔返回 `outcome_unknown`，再执行一次同样的命令即可；设备实际已经写入。

---

## 6. 数据与字库是怎么生成的

```
kajweb/dict 的六本书 zip
        │  tools/wordbook/extract.py   （下载、清洗、挑例句）
        ▼
tools/wordbook/words.txt             （唯一事实源，制表符分隔）
        │  tools/wordbook/gen_content.py
        ▼
assets/content/wd_content.bin         （2,899,123 字节，编进固件）
        │  tools/wordbook/gen_font.py
        ▼
assets/fonts/wd_font_{64,48,32}.c     （主字体，三档）
assets/fonts/wd_ipa_{64,48,32}.c      （IPA 补充字体）
```

### 6.1 内容

- `tools/wordbook/books.json`：六本书的清单、顺序、下载文件名与期望条数；
- `tools/wordbook/extract.py`：下载 zip，规范音标、清不可见字符、从候选例句里挑最合适的一条；
- `tools/wordbook/gen_content.py`：生成设备端二进制 blob 和宿主测试固定向量；
- `python tools/wordbook/gen_content.py --check`：只校验现有产物与事实源是否同步。

blob 只是「文件头 + 书表 + 偏移表 + 字符串池」，设备端逐字节读取，不做未对齐访问。

### 6.2 字库

- `python tools/wordbook/gen_font.py`：重新生成六份字库；
- `python tools/wordbook/gen_font.py --check`：**硬校验**生成物是否覆盖当前词库与界面文案的全部字符；
- 重新生成需要本机安装 `lv_font_conv`（Node 包）、Noto Sans CJK SC 与 Noto Sans 母字体，路径通过 `WD_MAIN_FONT` / `WD_IPA_FONT` / `LV_FONT_CONV_DIR` 环境变量指定。

平时改界面文案后，跑一次 `--check` 就能知道有没有漏字。

---

## 7. 宿主测试（不用刷机）

纯逻辑层（词库解析、出题、进度序列化、折行、版式）不依赖 ESP-IDF 和 LVGL，可以在电脑上秒级跑完：

```sh
sh tools/run_host_tests.sh
```

当前 6 个套件全部通过，约 **16 万条断言**：

```
t_wd_manifest    词库头/书表解析
t_wd_content     真实 blob 固定向量逐字段比对
t_wd_quiz        四选一状态机与干扰项不变量
t_wd_progress    进度位图与断点序列化
t_wd_wrap        真实词库的中英文折行
t_wd_layout      480×480 三档字号版式预算
```

找不到宿主编译器时，用环境变量指定：

```sh
WD_HOST_CC=/path/to/gcc sh tools/run_host_tests.sh
```

---

## 8. 代码结构

| 文件 | 负责什么 |
| --- | --- |
| `main/main.cc` | 硬件启动：NVS、屏幕、触摸、Wi-Fi、音频、心跳定时器 |
| `main/wd_app.c` | 应用状态机：六屏跳转、学习进度、发音与测验的串联 |
| `main/wd_ui.c` | 六屏绘制与点按事件翻译（只画，不做业务判断） |
| `main/wd_audio.cc` | HTTP → MP3 软件解码 → ES8311 播放，含 16k→48k 升采样 |
| `main/wd_cache.cc` | `/nand/wdvoice` 发音缓存、分片淘汰 |
| `main/wd_display.cc` | 亮度档位、息屏超时与触摸唤醒 |
| `main/wd_store.c` | NVS：每本书的掌握位图、断点、音量、当前书 |
| `main/wd_content.c` | 词库 blob 的只读访问 |
| `main/wd_manifest.c` | 文件头与书表解析 |
| `main/wd_quiz.c` | 四选一出题与判题（纯逻辑，可复现） |
| `main/wd_progress.c` | 进度位图与断点的序列化 |
| `main/wd_wrap.c` | UTF-8 中英文折行 |
| `main/wd_layout.c` | 三档字号与 480×480 版式预算 |
| `main/wd_font.c` | 字库选择与 IPA 回退链 |

---

## 9. 已知限制

- **发音不是全离线**：没听过的词/句第一次必须联网；缓存空间约 100 MiB，按最近使用淘汰。
- **例句 TTS 依赖公开接口**：百度和有道的接口以后可能变；变了只需要改 `main/wd_audio.cc` 里的地址。
- **测验只考一个方向**：看中文释义选英文单词。
- **没有屏上键盘**：配网在手机浏览器完成，这比在方屏上敲密码好用。
- **不支持电池电量显示**：本板没有启用对应的电量计，状态栏只显示 Wi-Fi。
- **仓库里的 `docs/` 是移植前的旧文档**（原 ESP32-C3 三键版），当前行为以本 README 和代码为准。

---

## 10. 许可与来源

- **代码**：MIT License，见 [LICENSE](LICENSE)。
- **词库数据**：来自开源项目 [kajweb/dict](https://github.com/kajweb/dict)，仅用于个人学习。
- **字体**：Noto Sans CJK SC 与 Noto Sans，SIL Open Font License 1.1。
  `assets/fonts/` 里是转换后的 LVGL 点阵字库。
- **发音音频**：运行时从有道 / 百度的公开 TTS 接口获取，**不随仓库分发**；设备本地缓存只存在于用户自己的机器上。
- 界面与移植工作：本项目作者。感谢 [linux.do](https://linux.do) 社区。
