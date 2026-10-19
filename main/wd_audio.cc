// main/wd_audio.cc —— 单词发音播放：HTTP → 软件 MP3 解码 → I2S。
//
// 这个文件是从基座 english-podcast 的 radio_player.cc（1290 行）里**抽出来的
// 一段**，不是它的简化版 —— 差别在于去掉了哪些东西，以及为什么。
//
// 去掉的（以及为什么可以去掉）：
//   * 16 KB 环形缓冲 + 网络/播放双任务 + 段级预取
//       基座要边下边播 20 分钟（一段 30 秒 = 120 KB），所以必须一边拉一边放。
//       这里一段只有 10 KB、1.2 秒 —— **根本不存在"供不上"这回事**：
//       在同一个循环里读 1 KB、解开、写 I2S 就够了。
//   * 断点续取（Range 请求）
//       续取是给"播到一半断线、重连后要接着放"用的。一个词的发音断了就重来，
//       续取的复杂度（206/200 分支、偏移记账）换不来任何东西。
//   * 分段重试退避、换台抢占的完整状态机
//       保留"可取消"这一点（用户翻面要立刻停），但不需要重试策略 ——
//       失败就让用户再点一次，比后台偷偷重连 6 次更好理解。
//
// 保留的（每一条都是真机踩出来的，注释里写清了原因）：
//   * `esp_http_client_is_complete_data_received()` / Content-Length
//   * 音频产出看门狗
//   * 写失败必须打日志
//   * 解码器"没进展"的检测（输入缓冲满了却没被消费 = 卡住，与网络无关）
//
// 新增的：请求防抖、可取消、剥 ID3v2 头。见 wd_audio.h。

#include "wd_audio.h"
#include "wd_cache.h"

#include <cstdio>
#include <cstring>

#include "bsp/audio.h"
#include "decoder/esp_audio_dec_default.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

namespace {

constexpr char kTag[] = "wd_audio";

// ---------------------------------------------------------------------------
// 尺寸与超时 —— 全部从"一个词的发音是 1~2 秒、约 10 KB"这个事实反解
// ---------------------------------------------------------------------------

// 输入 1024 / 输出 8192：与基座一致。
//   输入：MP3 一帧最长 1441 字节（320 kbps），1024 略小 —— 但解码器会把没消费完
//         的尾巴留给我们（见 carried），拼起来就够。
//   输出：8192 字节 = 2048 个 16bit 采样 = 48 kHz 下约 42 ms 的声音。
//         再小会让 I2S 写入又碎又频繁，再大只是白占内存。
constexpr std::size_t kInputSize = 1024;
constexpr std::size_t kOutputSize = 8192;

// 一次请求的总时长上界。
// 单词 10 KB @ 一般网络是 200 ms 级；例句最长约 80 KB、播放本身 10 秒级，
// 所以统一给 45 秒。半死连接仍然由下面的停顿看门狗在 5 秒内揪出来。
constexpr uint32_t kTotalBudgetMs = 45000;

// 单次 HTTP 读超时。
constexpr int kHttpTimeoutMs = 4000;

// 音频产出看门狗：连续这么久既没有新的网络字节、也没有新的声音 ⇒ 判失败。
constexpr uint32_t kStallMs = 5000;

// 同一个词的重复请求合并窗口。三颗按键是 ADC 分压共用一个 GPIO，
// "按键抖动"与"用户连点"在软件层看起来完全一样，而每个请求都是一次完整往返。
constexpr uint32_t kDebounceMs = 400;

// 界面显示的"正在加载"上界。例句的整段下载可能比单词慢一个数量级，
// 给到 30 秒；真正的故障（连接失败、断流、解码卡死）都会更早落定。
constexpr uint32_t kLoadingWatchdogMs = 30000;

constexpr uint32_t kIdlePollMs = 15;

// 有道接口。单词 type=0（美音）。
// 整句朗读有道不支持：dictvoice 的 type=0/1/2/3 对整句一律返回
// 500 "returned null audio"（2026-10 实测），fanyivoice 走 688 反爬，不能用。
// 例句改用百度翻译网页版同一个 TTS 接口，HTTPS 直接返回 MP3（16 kHz），
// 证书链是 GlobalSign Root CA - R3，已在证书包里。
constexpr char kWordUrlPrefix[] =
    "http://dict.youdao.com/dictvoice?type=0&audio=";
constexpr char kSentenceUrlPrefix[] =
    "https://fanyi.baidu.com/gettts?lan=en&spd=3&source=web&text=";

// 例句缓存的键前缀。单词与例句共用一套哈希缓存，键必须能区分开。
constexpr char kSentenceKeyPrefix[] = "s:";

// ---------------------------------------------------------------------------
// 常驻状态
// ---------------------------------------------------------------------------

// 缓冲常驻（见头文件：发音是核心操作，反复 malloc/free 9 KB 会把堆切碎）。
uint8_t s_input[kInputSize];
uint8_t s_output[kOutputSize];
// 16 kHz 升采到 48 kHz 的输出缓冲。最大输入 kOutputSize 字节，
// 升 3 倍后是 kOutputSize * 3 字节。
uint8_t s_resample[kOutputSize * 3];

TaskHandle_t s_task = nullptr;

// 取消机制：s_generation 只在"新请求"和"stop"时 +1。
// 运行中的播放每次循环都核对它 —— 不相等就立刻放弃，不再等网络超时。
volatile uint32_t s_generation = 0;
volatile uint32_t s_pending = 0;   // 最后一次 play_word 分配的序号
uint32_t s_served = 0;             // 播放任务已经取走的序号

volatile bool s_connected = false;
volatile uint8_t s_volume = 100;

volatile wd_audio_state_t s_state = WD_AUDIO_IDLE;
// s_state 变成 LOADING 的时刻，供 wd_audio_state() 的超时判定使用。
uint32_t s_loading_since = 0;

// ESP-Mosaico 移植：ES8311 经 esp_codec_dev 出口（与 qiaopi-quiz 同一接法）。
// 句柄懒初始化于首次 set_format —— 播放任务是 I2S 的唯一写入者。
esp_codec_dev_handle_t s_codec = nullptr;

// 待播请求由 UI 任务写、播放任务读（URL 在播放任务里现拼，缓存在
// 播放任务里查）。key = 缓存/防抖用的键（例句带 "s:" 前缀），payload =
// 真正送进 URL 的原文（单词或整句）。
// 任务切换本身是全屏障，所以只需要**编译器屏障**防止"先递增 s_pending
// 再写缓冲"这种重排。写完之后 __sync_synchronize()，任务读到新 s_pending
// 之后再同步一次即可。
char s_pending_key[WD_AUDIO_TEXT_MAX + 2];
char s_pending_payload[WD_AUDIO_TEXT_MAX];
bool s_pending_sentence = false;
char s_last_key[WD_AUDIO_TEXT_MAX + 2];
uint32_t s_last_request_ms = 0;

wd_audio_report_t s_report = {false, 0, 0, "idle"};

uint32_t now_ms() {
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

bool still_current(uint32_t generation) {
    return s_generation == generation;
}

void set_report(bool ok, uint32_t bytes, uint32_t pcm, const char *detail) {
    s_report.ok = ok;
    s_report.bytes = bytes;
    s_report.pcm_bytes = pcm;
    s_report.detail = detail;
}

// ---------------------------------------------------------------------------
// esp_codec_dev 出口（ES8311）。替换 C3 版的 bsp_audio_* 三件套：
// set_format 按流的采样率现配 I2S 时钟，write 的成功判据是 == ESP_CODEC_DEV_OK
// （返回的是状态码不是字节数），音量走档位曲线 + 百分比对数刻度。
// ---------------------------------------------------------------------------

// 音量曲线：在 qiaopi-quiz 那张曲线上整体抬高，最高档给 +6 dB。
// 真机上 ES8311 的 BSP 硬件增益是 -3.6 dB，旧曲线 100% = 0 dB 仍然偏小；
// +6 dB 是 2 倍幅度，既能听出明显提升，又给压缩录音留了不削顶的余量。
// vol 必须升序（esp_codec_dev 的硬约束）。0 是「关」，不在曲线上：
// esp_codec_dev 对 vol==0 直接给 -96 dB。
constexpr int kCurveCount = 6;
constexpr uint8_t kCurveVol[kCurveCount] = {0, 20, 40, 60, 80, 100};
constexpr float kCurveDb[kCurveCount] = {-40.0f, -26.0f, -14.0f,
                                         -6.0f,  0.0f,   6.0f};

// 每个流 open 一次（esp_codec_dev_open 会把 I2S 时钟重配成该流的采样率）。
// 曲线必须在 open **之后**装：verify_codec_setting 对未 open 的句柄回 WRONG_STATE。
bool audio_set_format(uint32_t rate) {
    if (!s_codec) {
        s_codec = bsp_audio_codec_speaker_init();
        if (!s_codec) {
            ESP_LOGE(kTag, "扬声器编解码器初始化失败");
            return false;
        }
    }
    esp_codec_dev_sample_info_t info = {};
    info.sample_rate = (int)rate;
    info.channel = 1;
    info.bits_per_sample = 16;
    if (esp_codec_dev_open(s_codec, &info) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(kTag, "codec open 失败（%lu Hz）", (unsigned long)rate);
        return false;
    }
    esp_codec_dev_vol_map_t map[kCurveCount] = {};
    for (int i = 0; i < kCurveCount; ++i) {
        map[i].vol = kCurveVol[i];
        map[i].db_value = kCurveDb[i];
    }
    esp_codec_dev_vol_curve_t curve = {.vol_map = map, .count = kCurveCount};
    if (esp_codec_dev_set_vol_curve(s_codec, &curve) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(kTag, "音量曲线装不上，本次沿用默认直线");
    }
    esp_codec_dev_set_out_vol(s_codec, (int)s_volume);
    return true;
}

bool audio_write(const void *data, size_t bytes) {
    return s_codec != nullptr &&
           esp_codec_dev_write(s_codec, const_cast<void *>(data), (int)bytes) ==
               ESP_CODEC_DEV_OK;
}

void audio_apply_volume() {
    if (s_codec) esp_codec_dev_set_out_vol(s_codec, (int)s_volume);
}


// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

// MP3 解码器。用 esp_audio_codec 的 simple_dec —— 纯软件解码，
// **不吃 PSRAM、也不吃几百 KB 的堆**（基座的注释里记着全程约 9 KB）。
esp_audio_simple_dec_handle_t open_mp3_decoder() {
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    const esp_audio_err_t error = esp_audio_simple_dec_open(&config, &decoder);
    if (error != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "MP3 解码器打开失败: %d", error);
        return nullptr;
    }
    return decoder;
}

// 百度例句音频是 16 kHz。直接用 16 kHz 打开 codec 时真机输出严重失真
// （听起来像"尖叫"），而单词音频走的 48 kHz 通路完全正常 —— 所以把
// 16 kHz 的 PCM 用 3 倍线性插值升到 48 kHz，两条流统一按 48 kHz 播放。
constexpr uint32_t kSentenceSourceRate = 16000;
constexpr uint32_t kPlaybackRate = 48000;

// 3 倍线性插值升采。输入输出不能重叠（输出缓冲区是 s_resample）。
std::size_t upsample_3x(const uint8_t *pcm, std::size_t bytes, uint8_t *out) {
    const auto *src = reinterpret_cast<const int16_t *>(pcm);
    auto *dst = reinterpret_cast<int16_t *>(out);
    const std::size_t count = bytes / sizeof(int16_t);
    for (std::size_t i = 0; i < count; ++i) {
        const int32_t a = src[i];
        const int32_t b = (i + 1 < count) ? src[i + 1] : a;
        dst[i * 3] = static_cast<int16_t>(a);
        dst[i * 3 + 1] = static_cast<int16_t>((2 * a + b) / 3);
        dst[i * 3 + 2] = static_cast<int16_t>((a + 2 * b) / 3);
    }
    return count * 3 * sizeof(int16_t);
}

// 多声道混成单声道。ES8311 只出单声道，48 kHz 双声道的流直接写会快一倍。
std::size_t downmix_to_mono(uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || channels == 0) return 0;
    if (channels == 1) return bytes & ~static_cast<std::size_t>(1);

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const std::size_t frames = bytes / (sizeof(int16_t) * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        int32_t mixed = 0;
        for (uint8_t channel = 0; channel < channels; ++channel) {
            mixed += samples[frame * channels + channel];
        }
        samples[frame] = static_cast<int16_t>(mixed / channels);
    }
    return frames * sizeof(int16_t);
}

// 判断 ID3v2 头有多少字节，需要跳过。p 至少 10 字节。
//
// 有道的 MP3 以 `ID3\x04` 开头（ID3v2.4）。不剥掉的话解码器要先失步一段 ——
// 表现为开头的爆破音或者干脆不出声。
//
// 头结构：
//   "ID3" | ver_major | ver_rev | flags | size[4]
// size 是 **syncsafe 整数**：每字节只用低 7 位（最高位恒为 0，这样整段头
// 不会出现假的帧同步字）。所以不能当成普通的 big-endian u32 读 ——
// 这是 ID3v2 里最容易写错的一处。
int parse_id3_skip(const uint8_t *p, std::size_t n) {
    if (n < 10) return 0;
    if (p[0] != 'I' || p[1] != 'D' || p[2] != '3') return 0;
    // 版本超过 4 或 revision 是 0xFF（ID3v2.4 规定必须不是）⇒ 这不是一个
    // 能认的头，当成没有头处理，让解码器自己去同步。
    if (p[3] > 4 || p[4] == 0xFF) return 0;
    // size 的四个字节都必须 < 0x80，否则同样说明这不是 syncsafe 整数
    for (int i = 6; i < 10; ++i) {
        if (p[i] & 0x80) return 0;
    }
    const uint32_t size = (static_cast<uint32_t>(p[6] & 0x7f) << 21) |
                          (static_cast<uint32_t>(p[7] & 0x7f) << 14) |
                          (static_cast<uint32_t>(p[8] & 0x7f) << 7) |
                          static_cast<uint32_t>(p[9] & 0x7f);
    uint32_t total = 10 + size;
    // flags bit 4 = 有 footer（再 10 字节）。加上它，否则会从 footer 中间开始解。
    if (p[5] & 0x10) total += 10;
    // 上限：ID3 头不该比整段音频还大。超过 4 KB 说明这个头是坏的。
    if (total > 4096) return 0;
    return static_cast<int>(total);
}

// 拼请求地址。非安全字符做百分号编码 ——
// 词库里有 é ç 这类拉丁扩展字母（UTF-8 两字节），例句里还有空格、逗号、
// 撇号；原样塞进 URL 可能被服务端截断或误解。空格也必须编码成 %20。
bool build_url(const char *prefix_text, const char *text, char *out,
               std::size_t capacity) {
    static const char kHex[] = "0123456789ABCDEF";
    const std::size_t prefix = std::strlen(prefix_text);
    if (capacity < prefix + 4) return false;

    std::memcpy(out, prefix_text, prefix);
    std::size_t n = prefix;

    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        const unsigned char c = *p;
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-';
        if (plain) {
            if (n + 1 >= capacity) return false;
            out[n++] = static_cast<char>(c);
        } else {
            if (n + 3 >= capacity) return false;
            out[n++] = '%';
            out[n++] = kHex[c >> 4];
            out[n++] = kHex[c & 0x0f];
        }
    }
    out[n] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// 一次播放
// ---------------------------------------------------------------------------

// 一次流式播放的结果。
//
// 为什么用返回值而不是 goto cleanup：C++ 的 goto **不能跳过带初始化的变量声明**，
// 而这段逻辑里有十几个局部状态（解码器、格式、计数器、看门狗时刻）。用 goto 就得
// 把它们全部提到函数最前面并手工初始化 —— 那样更容易写出"漏初始化"的 bug，
// 也比返回值难读。所以拆成两层：外层管 client / 解码器的生命周期，
// 内层只管"读→解→写"，每条失败路径直接 return。
struct Outcome {
    bool ok;             // 是否正常播完
    bool cancelled;      // 被 stop 或新请求作废（既不算成功也不算失败）
    uint32_t bytes;      // 从网络/文件读到的字节数（**含**被剥掉的 ID3 头）
    uint32_t pcm_bytes;  // 真正写进 I2S 的字节数
    const char *detail;  // 结果说明，指向**静态字符串**（不能指向局部缓冲区）
};

// 网络与缓存文件共用同一条解码路径：只换读字节的来源。
struct AudioSource {
    esp_http_client_handle_t client;  // 非空 = 网络
    FILE *file;                       // 非空 = /nand 缓存文件
    bool cache_write;                 // 网络字节同时喂给缓存临时文件
};

int source_read(AudioSource *source, char *buffer, int length) {
    int got;
    if (source->file) {
        const size_t n = std::fread(buffer, 1, static_cast<size_t>(length),
                                    source->file);
        if (n > 0) {
            got = static_cast<int>(n);
        } else {
            got = std::ferror(source->file) ? -1 : 0;
        }
    } else {
        got = esp_http_client_read(source->client, buffer, length);
    }
    if (got > 0 && source->cache_write) {
        wd_cache_writer_feed(buffer, static_cast<size_t>(got));
    }
    return got;
}

Outcome stream_and_play(AudioSource *source,
                        esp_audio_simple_dec_handle_t decoder,
                        uint32_t generation) {
    Outcome outcome = {false, false, 0, 0, "unknown"};

    bool format_ready = false;
    uint32_t sample_rate = 0;
    uint8_t source_channels = 1;
    bool upsample = false;
    bool played_audio = false;
    uint32_t write_failures = 0;
    uint32_t decode_failures = 0;

    const uint32_t deadline = now_ms() + kTotalBudgetMs;
    uint32_t last_progress_ms = now_ms();
    std::size_t carried = 0;

    // ---- 剥 ID3v2 头 ----
    //
    // 先把头读出来（最多 10 字节），算出要跳多少；已经进缓冲的部分就地前移，
    // 剩下的边读边丢。跳完之后解码器看到的第一批字节就是 MP3 帧同步字。
    {
        std::size_t head_len = 0;
        while (head_len < 10 && still_current(generation)) {
            const int got = source_read(
                source, reinterpret_cast<char *>(s_input + head_len),
                static_cast<int>(10 - head_len));
            if (got <= 0) break;
            head_len += static_cast<std::size_t>(got);
        }
        outcome.bytes += static_cast<uint32_t>(head_len);

        std::size_t skip =
            (head_len == 10) ? static_cast<std::size_t>(parse_id3_skip(s_input, 10)) : 0;
        if (skip > 0) {
            ESP_LOGD(kTag, "跳过 ID3v2 头 %u 字节", static_cast<unsigned>(skip));
        }
        std::size_t pending = head_len;
        if (skip >= pending) {
            skip -= pending;
            pending = 0;
            uint8_t trash[64];
            while (skip > 0 && still_current(generation)) {
                const std::size_t want = skip < sizeof(trash) ? skip : sizeof(trash);
                const int got = source_read(
                    source, reinterpret_cast<char *>(trash), static_cast<int>(want));
                if (got <= 0) break;
                const std::size_t took = static_cast<std::size_t>(got) < skip
                                             ? static_cast<std::size_t>(got)
                                             : skip;
                skip -= took;
                outcome.bytes += static_cast<uint32_t>(took);
            }
        } else {
            pending -= skip;
            std::memmove(s_input, s_input + skip, pending);
        }
        carried = pending;
    }

    // ---- 主循环：读 → 解 → 写 ----
    bool read_ended = false;
    while (still_current(generation)) {
        const uint32_t now = now_ms();

        if (static_cast<int32_t>(deadline - now) <= 0) {
            ESP_LOGW(kTag, "超过 %u ms 还没播完（已收到 %u 字节）",
                     static_cast<unsigned>(kTotalBudgetMs),
                     static_cast<unsigned>(outcome.bytes));
            outcome.detail = "budget";
            return outcome;
        }
        if (now - last_progress_ms >= kStallMs) {
            ESP_LOGW(kTag, "停顿 %u ms：网络 %u 字节、PCM %u 字节 —— 判失败",
                     static_cast<unsigned>(now - last_progress_ms),
                     static_cast<unsigned>(outcome.bytes),
                     static_cast<unsigned>(outcome.pcm_bytes));
            outcome.detail = played_audio ? "stalled" : "no-audio";
            return outcome;
        }
        if (carried >= kInputSize) {
            // 输入缓冲满了却一个字节没被消费 = 解码器卡住，与网络无关。
            ESP_LOGE(kTag, "解码器无进展（输入缓冲 %u 字节全满）",
                     static_cast<unsigned>(carried));
            outcome.detail = "decoder-stuck";
            return outcome;
        }

        const int got = source_read(source,
                                    reinterpret_cast<char *>(s_input + carried),
                                    static_cast<int>(kInputSize - carried));
        if (got < 0) {
            ESP_LOGW(kTag, "读取失败（已收到 %u 字节）",
                     static_cast<unsigned>(outcome.bytes));
            outcome.detail = "read";
            return outcome;
        }
        if (got == 0) {
            read_ended = true;   // 流结束（正常结束或对端关闭），下面统一判定
        } else {
            carried += static_cast<std::size_t>(got);
            outcome.bytes += static_cast<uint32_t>(got);
            last_progress_ms = now_ms();
        }

        // eos 恒为 false：把"流结束"和"这一批数据"分开处理，
        // 免得解码器在一批不完整的数据上被要求收尾。
        esp_audio_simple_dec_raw_t raw = {
            .buffer = s_input,
            .len = static_cast<uint32_t>(carried),
            .eos = false,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };

        while (raw.len > 0 && still_current(generation)) {
            esp_audio_simple_dec_out_t frame = {
                .buffer = s_output,
                .len = kOutputSize,
                .needed_size = 0,
                .decoded_size = 0,
            };
            const uint32_t before = raw.len;
            const esp_audio_err_t result =
                esp_audio_simple_dec_process(decoder, &raw, &frame);

            if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                // 输出缓冲不够是配置问题（不是流的错），重试也一样。
                ESP_LOGE(kTag, "解码需要 %u 字节，输出缓冲只有 %u",
                         static_cast<unsigned>(frame.needed_size),
                         static_cast<unsigned>(kOutputSize));
                outcome.detail = "decoder-buffer";
                return outcome;
            }
            if (result != ESP_AUDIO_ERR_OK) {
                // 偶发坏帧：放弃这一批数据重新同步，不让整次播放失败。
                // 但坏帧太多说明流本身不对，那时再判失败。
                ++decode_failures;
                if (decode_failures <= 3) {
                    ESP_LOGW(kTag, "MP3 解码失败: %d（第 %u 次）", result,
                             static_cast<unsigned>(decode_failures));
                }
                if (decode_failures > 32) {
                    outcome.detail = "decode";
                    return outcome;
                }
                raw.len = 0;
                break;
            }

            if (raw.consumed > raw.len) raw.consumed = raw.len;
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;

            if (frame.decoded_size > 0) {
                // 第一帧解出来才知道采样率 —— 在这之前没法配 I2S。
                if (!format_ready) {
                    esp_audio_simple_dec_info_t info = {};
                    if (esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                        source_channels =
                            info.channel < 1
                                ? 1
                                : (info.channel > 2 ? 2
                                                    : static_cast<uint8_t>(info.channel));
                        sample_rate = info.sample_rate;
                        upsample = (sample_rate == kSentenceSourceRate);
                        const uint32_t play_rate =
                            upsample ? kPlaybackRate : sample_rate;
                        if (info.bits_per_sample != 16 || info.sample_rate < 8000 ||
                            info.sample_rate > 48000 ||
                            !audio_set_format(play_rate)) {
                            ESP_LOGE(kTag, "不支持的流格式: %luHz/%ubit/%uch",
                                     static_cast<unsigned long>(info.sample_rate),
                                     info.bits_per_sample, source_channels);
                            outcome.detail = "format";
                            return outcome;
                        }
                        format_ready = true;
                        ESP_LOGI(kTag, "%luHz/%uch -> 单声道%s",
                                 static_cast<unsigned long>(sample_rate), source_channels,
                                 upsample ? "，3 倍升采到 48000Hz" : "");
                    }
                }
                if (format_ready) {
                    const std::size_t mono = downmix_to_mono(
                        frame.buffer, frame.decoded_size, source_channels);
                    const uint8_t *play_data = frame.buffer;
                    std::size_t play_bytes = mono;
                    if (upsample) {
                        play_bytes = upsample_3x(frame.buffer, mono, s_resample);
                        play_data = s_resample;
                    }
                    if (!audio_write(play_data, play_bytes)) {
                        // 写不进去一定有原因（I2S 通道不在运行、DMA 排不空）。
                        // 基座最初的实现是**静默** continue —— 声音没了、日志一行没有，
                        // 这是真机上最难查的一类故障。所以这里每次都记数，并周期性出声。
                        ++write_failures;
                        if ((write_failures & 0x07u) == 1) {
                            ESP_LOGW(kTag, "写 I2S 失败 %u 次（本次 %u 字节）",
                                     static_cast<unsigned>(write_failures),
                                     static_cast<unsigned>(play_bytes));
                        }
                        continue;
                    }
                    outcome.pcm_bytes += static_cast<uint32_t>(play_bytes);
                    last_progress_ms = now_ms();
                    if (!played_audio) {
                        played_audio = true;
                        s_state = WD_AUDIO_PLAYING;
                        // 这一行是"有没有真的出声"的分界线：之后若还有
                        // I2S 写失败，日志里能直接对上。
                        ESP_LOGI(kTag, "首帧 PCM 已写入 I2S（%u 字节）",
                                 static_cast<unsigned>(play_bytes));
                    }
                }
            }

            if (raw.len == before || raw.consumed == 0) {
                // 解码器在等更多字节。把这一段留在缓冲里，下一轮接着拼。
                break;
            }
        }

        carried = raw.len;
        if (carried > 0 && raw.buffer != s_input) {
            std::memmove(s_input, raw.buffer, carried);
        }

        if (read_ended) break;
    }

    // ---- 收尾：让解码器把内部残留吐出来 ----
    //
    // 不做这一步，末尾最多丢一帧（约 26 ms）—— 对"单词发音"这种长度，
    // 26 ms 是听得出来的（尾音被切掉）。eos 就是为这个准备的。
    // 用 guard 计数而不是 while(1)：解码器若在 eos 下持续吐数据会变成死循环。
    if (still_current(generation) && format_ready) {
        esp_audio_simple_dec_raw_t tail = {};
        tail.buffer = s_input;
        tail.len = 0;
        tail.eos = true;
        tail.frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE;
        for (unsigned guard = 0; guard < 32 && still_current(generation); ++guard) {
            esp_audio_simple_dec_out_t frame = {};
            frame.buffer = s_output;
            frame.len = kOutputSize;
            const esp_audio_err_t r = esp_audio_simple_dec_process(decoder, &tail, &frame);
            if (r != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) break;
            const std::size_t mono =
                downmix_to_mono(frame.buffer, frame.decoded_size, source_channels);
            if (upsample) {
                const std::size_t up = upsample_3x(frame.buffer, mono, s_resample);
                if (audio_write(s_resample, up)) {
                    outcome.pcm_bytes += static_cast<uint32_t>(up);
                }
            } else if (audio_write(frame.buffer, mono)) {
                outcome.pcm_bytes += static_cast<uint32_t>(mono);
            }
        }
    }

    // ---- 判定这一次的结果 ----
    //
    // 这是整段代码里最要紧的一处判断：
    //   ① 收完整了（Content-Length 对齐，或客户端确认收全） ⇒ 播完
    //   ② 读了半天一个 PCM 都没写出去                    ⇒ 失败
    //   ③ 其余（读到一半断掉）                            ⇒ 失败
    // 把 ③ 误判成 ① 会让界面显示"播完了"而用户什么都没听到；
    // 把 ① 误判成 ③ 会让每一次正常播放都被当成失败。
    if (!still_current(generation)) {
        outcome.cancelled = true;
        outcome.detail = "cancelled";
        return outcome;
    }
    if (!played_audio) {
        ESP_LOGW(kTag, "收到 %u 字节但一个 PCM 都没写出去",
                 static_cast<unsigned>(outcome.bytes));
        outcome.detail = "no-audio";
        return outcome;
    }

    if (source->file) {
        // 本地文件：读到 EOF 且解码出过声音就算完整。文件损坏时
        // 调用方会删掉它，并在联网时回退到网络路径。
        outcome.ok = read_ended;
        outcome.detail = outcome.ok ? "cache" : "cache-truncated";
    } else {
        const int64_t content_length =
            esp_http_client_get_content_length(source->client);
        const bool complete =
            esp_http_client_is_complete_data_received(source->client);
        if (content_length > 0) {
            // 有 Content-Length 时按字节数判 —— 这是最可靠的一条路。
            outcome.ok = complete ||
                         (static_cast<int64_t>(outcome.bytes) >= content_length);
        } else {
            // 没有 Content-Length（chunked 之类）：只能靠客户端"收全"的判定。
            // 打一行日志，以后出问题时能一眼看出走的是哪条路。
            ESP_LOGI(kTag, "无 Content-Length，按客户端收全判定（已收 %u 字节）",
                     static_cast<unsigned>(outcome.bytes));
            outcome.ok = complete;
        }
        outcome.detail = outcome.ok ? "ok" : "truncated";
    }

    ESP_LOGI(kTag, "%s：%lu Hz%s，%s %u 字节，PCM %u 字节，写失败 %u 次，解失败 %u 次",
             outcome.ok ? "播完" : "中止", static_cast<unsigned long>(sample_rate),
             upsample ? "（升采 48 kHz 播放）" : "",
             source->file ? "缓存" : "网络",
             static_cast<unsigned>(outcome.bytes),
             static_cast<unsigned>(outcome.pcm_bytes),
             static_cast<unsigned>(write_failures),
             static_cast<unsigned>(decode_failures));
    return outcome;
}

void run_network_playback(uint32_t generation, const char *url,
                          const char *cache_key) {
    s_state = WD_AUDIO_LOADING;
    ESP_LOGI(kTag, "网络请求: %s", url);

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;
    config.user_agent = "AI-Passport-Dict/1.0.0";
    // 不开 keep-alive：每次播放都新建并销毁一个 client，复用了连接也享受不到
    // keep-alive 的好处，反而会留下一段"以为还活着、其实对端已关"的状态。
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 3;
    // 明文连接下挂着证书包是无害的；万一接口哪天被强制跳到 https，客户端会跟着跳，
    // 那时需要它做证书校验。（这条来自基座，是踩过的坑。）
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(kTag, "HTTP client 分配失败（堆不够？）");
        set_report(false, 0, 0, "no-http-client");
        s_state = WD_AUDIO_FAILED;
        return;
    }

    // 这条 detail 缓冲区是**文件级静态**的：set_report 保存的是指针，
    // 若指向局部数组，函数返回后就是悬垂指针，界面上会显示随机字节。
    static char s_detail[32] = "unknown";

    do {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(kTag, "连接失败: %s", url);
            set_report(false, 0, 0, "connect");
            break;
        }
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        ESP_LOGI(kTag, "HTTP %d，Content-Length=%d", status,
                 static_cast<int>(esp_http_client_get_content_length(client)));
        if (status < 200 || status >= 300) {
            // 404 对发音接口有实际意义：词库里有 1000+ 条词源里就没有音标，
            // 这类词多半也拿不到发音。界面据此静默跳过即可，不必报错。
            ESP_LOGW(kTag, "HTTP %d for %s", status, url);
            if (status == 404) {
                set_report(false, 0, 0, "http 404");
            } else {
                std::snprintf(s_detail, sizeof(s_detail), "http %d", status);
                set_report(false, 0, 0, s_detail);
            }
            break;
        }

        esp_audio_simple_dec_handle_t decoder = open_mp3_decoder();
        if (!decoder) {
            set_report(false, 0, 0, "decoder-open");
            break;
        }

        // 边播边写缓存：整段解码成功才 rename 提交，断流/取消就删临时文件。
        const bool cache_write = wd_cache_writer_begin(cache_key);
        ESP_LOGI(kTag, "缓存写入%s", cache_write ? "已开始" : "未启用（不影响播放）");
        AudioSource source = {client, nullptr, cache_write};
        const Outcome outcome = stream_and_play(&source, decoder, generation);
        esp_audio_simple_dec_close(decoder);
        if (outcome.ok) {
            wd_cache_writer_commit();
        } else {
            wd_cache_writer_abort();
        }

        set_report(outcome.ok, outcome.bytes, outcome.pcm_bytes, outcome.detail);
        if (outcome.cancelled) {
            s_state = WD_AUDIO_IDLE;
        } else {
            s_state = outcome.ok ? WD_AUDIO_DONE : WD_AUDIO_FAILED;
        }
    } while (false);

    // 失败路径也要把状态落定，否则界面会永远停在"正在加载"。
    if (s_state == WD_AUDIO_LOADING) s_state = WD_AUDIO_FAILED;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

void run_cached_playback(uint32_t generation, const char *path) {
    s_state = WD_AUDIO_LOADING;

    FILE *file = std::fopen(path, "rb");
    if (!file) {
        ESP_LOGW(kTag, "缓存文件打不开: %s", path);
        set_report(false, 0, 0, "cache-open");
        s_state = WD_AUDIO_FAILED;
        return;
    }
    esp_audio_simple_dec_handle_t decoder = open_mp3_decoder();
    if (!decoder) {
        std::fclose(file);
        set_report(false, 0, 0, "decoder-open");
        s_state = WD_AUDIO_FAILED;
        return;
    }

    AudioSource source = {nullptr, file, false};
    const Outcome outcome = stream_and_play(&source, decoder, generation);
    esp_audio_simple_dec_close(decoder);
    std::fclose(file);

    set_report(outcome.ok, outcome.bytes, outcome.pcm_bytes, outcome.detail);
    if (outcome.cancelled) {
        s_state = WD_AUDIO_IDLE;
    } else {
        s_state = outcome.ok ? WD_AUDIO_DONE : WD_AUDIO_FAILED;
    }
}

// ---------------------------------------------------------------------------
// 播放任务
// ---------------------------------------------------------------------------

void audio_task(void *) {
    for (;;) {
        if (s_pending == s_served) {
            vTaskDelay(pdMS_TO_TICKS(kIdlePollMs));
            continue;
        }
        const uint32_t generation = s_pending;
        s_served = generation;
        __sync_synchronize();   // 读到 s_pending 之后再读 key / payload

        char key[WD_AUDIO_TEXT_MAX + 2];
        char payload[WD_AUDIO_TEXT_MAX];
        std::memcpy(key, s_pending_key, sizeof(key));
        std::memcpy(payload, s_pending_payload, sizeof(payload));
        const bool sentence = s_pending_sentence;

        ESP_LOGI(kTag, "开始处理%s: %s（在线=%d）", sentence ? "例句" : "发音",
                 payload, s_connected ? 1 : 0);

        // ① 先查本地缓存 —— 命中就完全不联网。
        bool played = false;
        char cache_path[96];
        if (wd_cache_path(key, cache_path, sizeof(cache_path)) &&
            wd_cache_has(key)) {
            ESP_LOGI(kTag, "缓存命中: %s", cache_path);
            wd_cache_touch(cache_path);
            run_cached_playback(generation, cache_path);
            played = s_state == WD_AUDIO_DONE;
            if (!played && still_current(generation)) {
                // 文件损坏/截断：删掉，联网时再按网络路径重来。
                ESP_LOGW(kTag, "缓存不可用，已删除: %s", cache_path);
                wd_cache_remove(cache_path);
            }
        }

        // ② 缓存未命中（或缓存坏了）且在线 → 网络拉取，同时写缓存。
        if (!played && still_current(generation) && s_connected) {
            char url[WD_AUDIO_URL_MAX];
            if (build_url(sentence ? kSentenceUrlPrefix : kWordUrlPrefix, payload,
                          url, sizeof(url))) {
                run_network_playback(generation, url, key);
            } else {
                set_report(false, 0, 0, "url");
                s_state = WD_AUDIO_FAILED;
            }
        } else if (!played && still_current(generation) &&
                   s_state == WD_AUDIO_LOADING) {
            set_report(false, 0, 0, "offline");
            s_state = WD_AUDIO_FAILED;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

extern "C" void wd_audio_init(void) {
    // ---- ① 注册默认解码器（只做一次）----
    //
    // 这一步必须在这里做，而且**顺序不能动**：`esp_audio_simple_dec_open()`
    // 只从已注册的表里找 MP3 解码器，没注册就返回 ESP_AUDIO_ERR_NOT_SUPPORT。
    // 现象是"点了没声，日志只说解码器打开失败" —— 与网络、词库都无关，
    // 很容易被误判成接口挂了。基座在它的播放器 init 里做了同样两件事；
    // 本文件第一版漏了，是交叉编译阶段读基座源码才发现的。
    static bool s_codecs_ready = false;
    if (!s_codecs_ready) {
        const esp_audio_err_t dec_err = esp_audio_dec_register_default();
        const esp_audio_err_t simple_err = esp_audio_simple_dec_register_default();
        if (dec_err != ESP_AUDIO_ERR_OK || simple_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(kTag, "默认解码器注册失败（dec=%d simple=%d）—— 发音一定不出声",
                     dec_err, simple_err);
            return;   // 不建任务：建了也只是每次播放都失败，白占 8 KB 栈
        }
        s_codecs_ready = true;
        ESP_LOGI(kTag, "默认解码器注册完成（dec=%d simple=%d）", dec_err, simple_err);
    }

    // ---- ② I2S 与 ES8311 ----
    //
    // 出口句柄懒初始化：首次 set_format（第一帧解出采样率）时 bsp_audio_codec_speaker_init，
    // 这里不提前建 —— esp_codec_dev_open 按每个流的采样率现配 I2S 时钟，
    // 提前建出来的句柄也得等第一帧才有意义。音量在 open 时与 set_volume 时都会套。

    // ---- ③ 播放任务 ----
    if (s_task) return;
    // 栈 8192：HTTP 客户端、TLS 证书包与解码器虽主要用堆，但调用链上的
    // 临时对象不算小；真机日志表明 6144 偏紧，直接对齐基座。
    if (xTaskCreate(audio_task, "wd_audio", 8192, nullptr, 5, &s_task) != pdPASS) {
        ESP_LOGE(kTag, "播放任务创建失败");
        s_task = nullptr;
        return;
    }
    ESP_LOGI(kTag, "播放任务已创建（栈 8192）");
}

extern "C" void wd_audio_set_network(bool connected) {
    s_connected = connected;
    if (!connected) wd_audio_stop();
}

extern "C" void wd_audio_set_volume(uint8_t percent) {
    s_volume = percent > 100 ? 100 : percent;
    audio_apply_volume();
}

extern "C" uint8_t wd_audio_volume(void) {
    return s_volume;
}

static bool request_playback(const char *key, const char *payload, bool sentence) {
    if (!key || !key[0] || !payload || !payload[0]) return false;
    const std::size_t key_len = std::strlen(key);
    const std::size_t payload_len = std::strlen(payload);
    if (key_len >= sizeof(s_pending_key) ||
        payload_len >= sizeof(s_pending_payload)) {
        return false;
    }

    // 解码器注册失败时 wd_audio_init() 不会建任务；之后每次请求都重试一次，
    // 避免界面永远停在"加载"却没有任何任务会来改状态。
    if (!s_task) {
        ESP_LOGW(kTag, "播放任务缺失，尝试重新初始化");
        wd_audio_init();
        if (!s_task) {
            ESP_LOGE(kTag, "播放任务不可用，请求失败: %s", payload);
            set_report(false, 0, 0, "no-task");
            s_state = WD_AUDIO_FAILED;
            return false;
        }
    }

    if (!s_connected && !wd_cache_has(key)) {
        ESP_LOGD(kTag, "未联网且无缓存，跳过播放：%s", payload);
        return false;
    }

    const uint32_t now = now_ms();

    // 防抖与合并：同一段文字的重复请求（连点）合成一次。
    if (std::strcmp(key, s_last_key) == 0) {
        if (now - s_last_request_ms < kDebounceMs) return false;
        const wd_audio_state_t state = s_state;
        if (state == WD_AUDIO_LOADING || state == WD_AUDIO_PLAYING) {
            return false;   // 已经在放了，不重头再来
        }
    }

    ESP_LOGI(kTag, "请求%s: %s（在线=%d）", sentence ? "例句" : "发音", payload,
             s_connected ? 1 : 0);
    std::memcpy(s_pending_key, key, key_len + 1);
    std::memcpy(s_pending_payload, payload, payload_len + 1);
    s_pending_sentence = sentence;
    std::memcpy(s_last_key, key, key_len + 1);
    s_last_request_ms = now;
    __sync_synchronize();
    // 递增 generation 顺带把**上一段音频**作废 —— 用户已经换词/换句了，
    // 再放完上一段只会让人困惑。
    //
    // 写成 `s_generation = s_generation + 1` 而不是 `++s_generation`：
    // C++20 起对 volatile 用 `++` 已经废弃（-Wvolatile），而这个值本来就该
    // 显式地读一次、写一次 —— 递增的语义正是"让别的任务看到一个新值"。
    s_generation = s_generation + 1;
    s_pending = s_generation;
    s_loading_since = now;
    s_state = WD_AUDIO_LOADING;
    return true;
}

extern "C" bool wd_audio_play_word(const char *word) {
    if (!word || !word[0]) return false;
    if (std::strlen(word) >= WD_AUDIO_WORD_MAX) return false;
    return request_playback(word, word, false);
}

extern "C" bool wd_audio_play_sentence(const char *text) {
    if (!text || !text[0]) return false;
    const std::size_t len = std::strlen(text);
    if (len >= WD_AUDIO_TEXT_MAX) return false;

    // 缓存键加 "s:" 前缀：单词和例句共用一套哈希缓存，不能撞。
    char key[WD_AUDIO_TEXT_MAX + 2];
    std::memcpy(key, kSentenceKeyPrefix, sizeof(kSentenceKeyPrefix) - 1);
    std::memcpy(key + sizeof(kSentenceKeyPrefix) - 1, text, len + 1);
    return request_playback(key, text, true);
}

extern "C" void wd_audio_stop(void) {
    s_generation = s_generation + 1;
    s_state = WD_AUDIO_IDLE;
}

extern "C" wd_audio_state_t wd_audio_state(void) {
    if (s_state == WD_AUDIO_LOADING && s_loading_since != 0 &&
        now_ms() - s_loading_since >= kLoadingWatchdogMs) {
        ESP_LOGE(kTag, "加载超过 %u ms 仍未出声，判定失败",
                 static_cast<unsigned>(kLoadingWatchdogMs));
        set_report(false, 0, 0, "loading-timeout");
        s_state = WD_AUDIO_FAILED;
    }
    return s_state;
}

extern "C" bool wd_audio_wait_idle(uint32_t timeout_ms) {
    const uint32_t deadline = now_ms() + timeout_ms;
    for (;;) {
        const wd_audio_state_t state = s_state;
        if (state == WD_AUDIO_IDLE || state == WD_AUDIO_DONE || state == WD_AUDIO_FAILED) {
            return true;
        }
        if (static_cast<int32_t>(now_ms() - deadline) >= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" wd_audio_report_t wd_audio_last_report(void) {
    return s_report;
}
