// main/wd_cache.h —— 发音的本地缓存（/nand LittleFS，纯 C 接口）。
//
// 设计：**听过一次就在本地留一份**。在线播放时把同一份 MP3 字节流写进
// /nand/wdvoice，下次再点发音先查缓存 —— 命中就不联网，离线也能重播。
//
// 为什么不是预下载整个词库：19,513 个词实测约 252 MB，而 /nand 可用约
// 121.7 MB，全量放不下。按需缓存只占用户真正查过的词；NAND 整卷归本应用
// 使用，按分片限额自动淘汰旧文件。
//
// 文件名用 64 位 FNV-1a 哈希（16 个十六进制字符）+ 前两位做分片目录，
// 避免 19,513 个词直接形成一个超大目录；缓存内容就是原始 MP3 字节
// （含 ID3v2 头），播放时复用网络路径的剥头逻辑。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 挂载 NAND 并建立缓存目录。幂等；失败只影响缓存，在线发音照常工作。
bool wd_cache_init(void);

bool wd_cache_ready(void);

// key（单词原文，或例句的 "s:..." 缓存键）对应的缓存路径。
bool wd_cache_path(const char *key, char *out, size_t capacity);

bool wd_cache_has(const char *key);

// 命中时刷新 mtime，用于分片内按最近使用淘汰。
void wd_cache_touch(const char *path);

void wd_cache_remove(const char *path);

// 一次下载对应一个 writer。begin 创建 <hash>.mp3.tmp；feed 可反复调用；
// commit 只在整段解码成功时调用（fflush + rename）；abort 删除临时文件。
// 任何一步失败都不影响播放，只是这次不进缓存。
bool wd_cache_writer_begin(const char *key);
void wd_cache_writer_feed(const void *data, size_t len);
bool wd_cache_writer_commit(void);
void wd_cache_writer_abort(void);

#ifdef __cplusplus
}
#endif
