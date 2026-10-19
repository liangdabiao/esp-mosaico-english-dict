idf_component_register(
    SRCS "main.cc"
         # 应用与页面
         "wd_app.c"
         "wd_ui.c"
         # 纯逻辑：与 ESP-IDF/LVGL 解耦，tests/ 里的宿主测试直接编译这些文件。
         # 这一层跑起来是秒级的 —— 不要等到几十秒的编译之后才发现逻辑错。
         "wd_manifest.c"
         "wd_content.c"
         "wd_quiz.c"
         "wd_progress.c"
         "wd_wrap.c"
         "wd_layout.c"
        # 设备服务
        "wd_store.c"
        "wd_audio.cc"
        # 发音缓存：SPI NAND + LittleFS（/nand/wdvoice）。
        "wd_cache.cc"
        # 词库二进制附件的 extern 声明（_binary_wd_content_bin_start）。
        # 曾误叫 wd_audio_blob.c —— 那是从 ancient-poets 的 apo_audio_blob.c
        # 抄形制时带过来的名字，但发音是**流式**的（见 wd_audio.h），
        # 本应用根本没有音频 blob。名字必须说清楚它装的是什么。
        "wd_content_blob.c"
        "wd_font.c"
        "wd_display.cc"
    INCLUDE_DIRS "."
    # BSP（显示/触摸/音频）、LVGL、codec、NVS + 网络栈（Wi-Fi/HTTP/TLS 证书包）。
    # esp_audio_codec（MP3 解码）/ esp-wifi-connect（AP 配网）由 components/ 提供。
    REQUIRES
        esp-mosaico-bsp
        esp_lvgl_adapter
        lvgl
        esp_codec_dev
        esp_mosaico_app_recovery
        nvs_flash
        esp_http_client
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp-tls
        mbedtls
        esp_audio_codec
        esp-wifi-connect
        # 发音缓存：NAND 块设备 + LittleFS，SPI 驱动直接调 spi_master/gpio。
        spi_nand_flash
        esp_blockdev
        esp_littlefs
        driver
)

# ---------------------------------------------------------------------------
# 字库：三档主字体 + 每档一个 IPA 补充字体。
#
# 为什么是两套字体而不是一套：主字体 NotoSansCJKsc 在真实字符集里缺 18 个
# 音标码点（ŋ ɒ ɔ ə ɚ ɛ ɜ ɝ ɪ ʃ ʊ ʌ ʒ ʳ ˈ ˌ ː ˚），而几乎每个词都含 ə/ɪ/ɛ ——
# 只有一套字体的话音标会满屏方框。补充字体由 Noto Sans（SIL OFL）生成，
# 只含主字体缺的那些码点，每档 18 个字形。
#
# 两套字体在运行期由 wd_font.c 用 lv_font_t.fallback 串起来，
# **不需要 fontTools merge**（merge 出来的字体两份 advance 表会打架）。
#
# 32px 档只含 ASCII + IPA + 界面标点（实测 93 个码点），**不含任何汉字**
# —— 它只渲染单词与音标，界面上的汉字一律走 24/16px 档。
# 这一条把大字档从上万字形压到几十个。
#
# 字库由 tools/wordbook/gen_font.py 生成，只用这一条路径编入；
# 不要再把同一个 .c 同时加进 SRCS，也不要被别的 .c 直接 include。
# ---------------------------------------------------------------------------
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/wd_font_64.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/wd_font_48.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/wd_font_32.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/wd_ipa_64.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/wd_ipa_48.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/wd_ipa_32.c"
)

# 词库内容（约 2.8 MB）以二进制附件方式编进固件。
#
# 为什么不生成一个巨大的 .c 数组：2.8 MB 的数组会让编译和链接明显变慢，
# 生成出来的源文件也没法审阅。二进制附件没有这两个问题，字节仍然落在 Flash 的
# rodata 段，读它就是一次内存映射访问。符号名由文件名决定
# （这里是 _binary_wd_content_bin_start），extern 声明只出现在 main/wd_content.c
# 一个文件里，不外露。
target_add_binary_data(${COMPONENT_LIB} "../assets/content/wd_content.bin" BINARY)
