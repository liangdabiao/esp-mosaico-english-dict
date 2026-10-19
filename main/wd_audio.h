// main/wd_audio.h —— 单词与例句朗读（纯 C 接口，实现在 wd_audio.cc）。
//
// 播的是什么：一个词约 1~2 秒、10 KB；一整句例句约 4~10 秒、30~80 KB。都来自
// 有道的公开接口：
//   * 单词：`http://dict.youdao.com/dictvoice?type=0&audio=<word>`（type=0 美音）
//   * 例句：`http://dict.youdao.com/dictvoice?type=2&audio=<sentence>`
//     （type=2 是整句朗读；type=0 对整句返回 HTTP 500 "returned null audio"）
//
// 为什么不做**全量**离线音频：19513 个词，实测平均约 13 KB/词（单词语音
// 9.8~26.7 KB），全量约 252 MB；/nand 可用约 121.7 MB，装不下。折成 8 kHz
// ADPCM 4bit 也要 94 MB，而且音质明显下降。
//
// 所以采用**按需缓存**（见 wd_cache.h）：
//   * 在线播放成功 ⇒ 同一份 MP3 字节写入 /nand/wdvoice/<hash>.mp3；
//   * 再次播放同一个词 ⇒ 先读缓存，命中就完全不联网；
//   * 离线时只能重播已经听过的词 —— 词库本身仍然完全离线可用。
// 缓存按 256 个分片组织，每片限额 400 KiB，满了淘汰该片最旧的条目。
//
// ---------------------------------------------------------------------------
// 与基座 english-podcast 的关系：**只抽走需要的那一段**
// ---------------------------------------------------------------------------
//
// 基座的 radio_player.cc 有 1290 行，绝大部分在解决"边下边播 20 分钟的课程"
// 特有的问题：分段预取、16 KB 环形缓冲、断点续取、段级退避重试、换台抢占……
// 那些机制套在一个 10 KB、1.2 秒的音频上不是"更稳妥"，而是**引入了一堆
// 根本不会发生的故障模式**，还要为它们写代码和维护。
//
// 所以这里保留的只有：
//   * HTTP → esp_audio_simple_dec（纯软件 MP3 解码）→ I2S 这条链路本身
//   * `esp_http_client_is_complete_data_received()` / Content-Length ——
//     区分"播完了"和"断流了"。这是基座用真机踩出来的判据：单看"读返回 0"
//     分不清这两种情况，而它们要走完全不同的后续处理。
//   * **音频产出看门狗**。真机上最难查的故障是"状态显示正在播放，喇叭一声不响"：
//     没有任何一行日志，用户只能听到声音忽然没了。这里的做法是钉住"最后一次
//     成功写进 I2S 的时刻"，超时就把播放判失败并记日志。
//   * "写失败必须打日志"。基座最初的实现是静默 continue —— 声音没了、日志一行没有。
//
// 新增的（基座不需要，这里需要）：
//   * **请求防抖**：同一个词在 400 ms 内的重复请求合并。三颗按键是 ADC 分压共用
//     一个 GPIO，抖动与"用户连点"在软件层看起来是一样的，而每一个请求都是一次
//     完整的 HTTP 往返。
//   * **可取消**：用户翻面 / 返回 / 换词时，正在跑的网络读要能立刻放弃，
//     不能等它自己超时（那会让界面卡住最多 4 秒）。
//   * **剥 ID3v2 头**：有道的 MP3 以 `ID3\x04` 开头（ID3v2.4），解码器直接吃
//     会先失步一段。头部长度写在同步安全整数里，自己剥掉比让解码器试错可靠。
//
// ---------------------------------------------------------------------------
// 状态与调用约定
// ---------------------------------------------------------------------------
// 只有一条播放任务，同一时刻只播一个词。UI 侧：
//   wd_audio_play_word(word)  → 返回 false 表示这次请求被合并掉、既没网
//                               也没有该词的本地缓存
//   wd_audio_state()          → 画"正在加载 / 正在播放"的指示
//   wd_audio_stop()           → 翻面、返回、进配网前调用
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WD_AUDIO_WORD_MAX 32     // 最长单词 18 字符，留余量
#define WD_AUDIO_TEXT_MAX 192    // 例句原文上限（当前词库最长例句 < 120 字符）
#define WD_AUDIO_URL_MAX 704     // 前缀 47 + 百分号编码后的例句（192 x 3 + 余量）

typedef enum {
    WD_AUDIO_IDLE = 0,   // 没在播
    WD_AUDIO_LOADING,    // 已发起请求，还没出第一帧声音
    WD_AUDIO_PLAYING,    // 已经出声
    WD_AUDIO_DONE,       // 本次正常播完
    WD_AUDIO_FAILED,     // 网络 / 解码 / 格式任一出错
} wd_audio_state_t;

// 音频子系统的**唯一入口**。只调一次，做三件事：
//   ① 注册 esp_audio_codec 的默认解码器（不注册的话 MP3 一定开不起来）
//   ② 初始化 ES8311 + I2S（幂等）
//   ③ 创建播放任务
// 幂等，重复调用是安全的。
//
// 缓冲是**静态常驻**的（约 9 KB），不随播放分配释放 —— 发音是本应用的核心操作，
// 一天可能触发几百次，反复 malloc/free 9 KB 会把堆切碎，而这块内存在配网门户期间
// 也不需要让出去（基座让出 16 KB 是因为它的环形缓冲比门户的网页缓冲还大，
// 这里 9 KB 没有那个矛盾）。
void wd_audio_init(void);

// 联网状态。未联网时：
//   * 有该词的缓存 ⇒ 正常离线播放；
//   * 没有缓存     ⇒ play_word 直接返回 false，不去建一个注定失败的连接。
void wd_audio_set_network(bool connected);

// 输出音量 0..100。立即生效。
void wd_audio_set_volume(uint8_t percent);
uint8_t wd_audio_volume(void);

// 播放一个词的发音。
//
// 返回 false 的情形（都不算错误，UI 不必提示）：
//   * 未联网 / word 为空 / word 太长
//   * **同一个词**正在加载或播放中（用户连点两下，合并成一次）
//   * 同一个词在上一次请求的 400 ms 内又被请求（防抖）
//
// 换一个词时**抢占**：上一个词的播放立刻作废，不等它自己结束 ——
// 用户已经翻到下一个词了，再放完上一个词只会让人困惑。
bool wd_audio_play_word(const char *word);

// 朗读一整句例句（有道的 type=2 接口）。规则同 play_word：
// 同一句的连点会合并；换一句会抢占上一句；成功一次后整段 MP3 也会进缓存，
// 缓存键带 "s:" 前缀，不会和单词发音撞。
//
// 返回 false 的情形：为空 / 太长 / 未联网且无缓存 / 同一句正在放或刚放过。
bool wd_audio_play_sentence(const char *text);

// 取消当前播放并回到 IDLE。翻面 / 返回 / 退出 / 进配网之前都要调。
void wd_audio_stop(void);

wd_audio_state_t wd_audio_state(void);

// 等播放结束（成功或失败都算结束），最多等 timeout_ms。
// 返回 true = 已经空闲。进配网门户之前用它把音频通路让出来。
bool wd_audio_wait_idle(uint32_t timeout_ms);

// 最近一次播放的结果，供调试页与日志用。
//  * ok          本次是否成功
//  * bytes       从网络读到的字节数（**含**被剥掉的 ID3 头）
//  * pcm_bytes   真正写进 I2S 的字节数
//  * detail      失败原因（"http 404" / "decode 12" / "no audio" / "stalled"）
typedef struct {
    bool ok;
    uint32_t bytes;
    uint32_t pcm_bytes;
    const char *detail;
} wd_audio_report_t;

wd_audio_report_t wd_audio_last_report(void);

#ifdef __cplusplus
}  // extern "C"
#endif
