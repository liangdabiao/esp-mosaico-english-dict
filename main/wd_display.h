// main/wd_display.h —— 屏幕设置：亮度档位与自动息屏。
//
// 为什么这一层持有那两张档位表，而 wd_ui 不重复一份：
//   档位表是"设备能力"（面板在 25% 背光下能不能看清、多久该熄屏），
//   不是排版。它只有一份，就在 wd_display.cc，别的模块要值就问它 ——
//   否则改档位要改三处，而漏改的那一处表现为"设置里换了档，屏幕没变"。
//
// 头文件是 **C 兼容**的（extern "C" + size_t，没有 std::）：实现是 .cc
// （用了 std::atomic 与 FreeRTOS），但调用方里有 wd_app.c 这种纯 C 文件。
// 让头文件两边都能 include，比为它写一层 extern "C" 桥接便宜得多，
// 也少一处"改了实现忘了改桥接"的地方。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 屏幕偏好持久化在 NVS 里（命名空间 wd_display）。一次按键如果只是把熄掉的
// 屏幕唤醒，这次按键会被调用方吃掉 —— 否则用户想点亮屏幕，结果顺手改了档位。
bool wd_display_settings_init();
bool wd_display_note_activity();

uint8_t wd_display_brightness();
size_t wd_display_brightness_selection();
void wd_display_set_brightness_selection(size_t selection);

uint16_t wd_display_timeout_seconds();
size_t wd_display_timeout_selection();
void wd_display_set_timeout_selection(size_t selection);

// 档位个数。导出它是因为"上一档"没法靠取模算出来：set_*_selection 收的是
// size_t，`index - 1` 在下标 0 上会先下溢成一个巨大的数，再取模拿到的
// 不是最后一档（4 档时会落到第 2 档）。要往上绕就必须知道档数。
size_t wd_display_brightness_tier_count();
size_t wd_display_timeout_tier_count();

#ifdef __cplusplus
}  // extern "C"
#endif
