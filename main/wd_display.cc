#include "wd_display.h"

#include "bsp/display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <atomic>

namespace {

// 命名空间与任务名从基座 english-podcast 的 radio_* 改成了 wd_*：
// 留下 radio_display / radio_screen 这种名字，下一次有人在这份代码里找
// "亮度存在哪"时会去翻一个叫 radio 的东西。改名唯一的影响是同一个设备上
// 从基座固件换成本固件时亮度会回到默认值 —— 默认值本身是能用的，
// 而名字混着电台痕迹的代价是长期的。
constexpr char kTag[] = "wd_display";
constexpr char kNvsNamespace[] = "wd_display";
// 档位表。基座 english-podcast 是 {25, 50, 72, 100}，这里把 72 换成 55：
// 默认亮度就落在 55（见下），少一档更亮的选项换来"开箱不刺眼"。
// ⚠ 改这张表会让**已存的 NVS 值对不上档**（存着 72 就找不到 72 这一档，
//   于是落到 fallback）。这是有意的：fallback 就是新的默认档。
constexpr uint8_t kBrightnessChoices[] = {25, 55, 75, 100};
// 默认档的下标（fallback 与出厂默认必须指向同一个值，否则"没存过"和
// "存了个对不上的值"会得到两个不同的亮度）。
constexpr std::size_t kBrightnessDefaultIndex = 1;   // = 55%
constexpr uint16_t kTimeoutChoices[] = {0, 30, 60, 300, 600};

std::atomic<uint8_t> s_brightness{kBrightnessChoices[kBrightnessDefaultIndex]};
std::atomic<uint16_t> s_timeout_seconds{0};
std::atomic<TickType_t> s_last_activity{0};
std::atomic<bool> s_screen_on{true};
TaskHandle_t s_blank_task;

template <typename T, std::size_t N>
std::size_t choice_index(const T (&choices)[N], T value, std::size_t fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (choices[i] == value) return i;
    }
    return fallback;
}

void save_u8(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u8(handle, key, value) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

void save_u16(const char *key, uint16_t value) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_u16(handle, key, value) == ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}

void blank_task(void *) {
    while (true) {
        const uint16_t timeout = s_timeout_seconds.load(std::memory_order_acquire);
        if (timeout && s_screen_on.load(std::memory_order_acquire)) {
            const TickType_t elapsed =
                xTaskGetTickCount() - s_last_activity.load(std::memory_order_acquire);
            if (elapsed >= pdMS_TO_TICKS(static_cast<uint32_t>(timeout) * 1000U)) {
                s_screen_on.store(false, std::memory_order_release);
                // ESP-Mosaico：背光/面板的电源开关是 bsp_display_off，
                // 亮度档位由 bsp_display_brightness_set 管（0..100）。
                bsp_display_off();
                ESP_LOGI(kTag, "Screen blanked after %u seconds",
                         static_cast<unsigned>(timeout));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

}  // namespace

bool wd_display_settings_init() {
    uint8_t brightness = kBrightnessChoices[kBrightnessDefaultIndex];
    uint16_t timeout = 0;
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "brightness", &brightness);
        nvs_get_u16(handle, "timeout", &timeout);
        nvs_close(handle);
    }

    const std::size_t brightness_index =
        choice_index(kBrightnessChoices, brightness, kBrightnessDefaultIndex);
    const std::size_t timeout_index = choice_index(kTimeoutChoices, timeout, 0);
    s_brightness.store(kBrightnessChoices[brightness_index], std::memory_order_release);
    s_timeout_seconds.store(kTimeoutChoices[timeout_index], std::memory_order_release);
    s_last_activity.store(xTaskGetTickCount(), std::memory_order_release);
    s_screen_on.store(true, std::memory_order_release);
    bsp_display_brightness_set((int)kBrightnessChoices[brightness_index]);

    if (s_blank_task) return true;
    if (xTaskCreate(blank_task, "wd_screen", 2048, nullptr, 3,
                    &s_blank_task) != pdPASS) {
        s_blank_task = nullptr;
        return false;
    }
    return true;
}

bool wd_display_note_activity() {
    s_last_activity.store(xTaskGetTickCount(), std::memory_order_release);
    const bool was_on = s_screen_on.exchange(true, std::memory_order_acq_rel);
    if (!was_on) {
        bsp_display_on();
        bsp_display_brightness_set((int)s_brightness.load(std::memory_order_acquire));
    }
    return was_on;
}

uint8_t wd_display_brightness() {
    return s_brightness.load(std::memory_order_acquire);
}

std::size_t wd_display_brightness_selection() {
    return choice_index(kBrightnessChoices, wd_display_brightness(),
                        kBrightnessDefaultIndex);
}

void wd_display_set_brightness_selection(std::size_t selection) {
    selection %= sizeof(kBrightnessChoices) / sizeof(kBrightnessChoices[0]);
    const uint8_t brightness = kBrightnessChoices[selection];
    s_brightness.store(brightness, std::memory_order_release);
    s_last_activity.store(xTaskGetTickCount(), std::memory_order_release);
    s_screen_on.store(true, std::memory_order_release);
    bsp_display_on();
    bsp_display_brightness_set((int)brightness);
    save_u8("brightness", brightness);
}

uint16_t wd_display_timeout_seconds() {
    return s_timeout_seconds.load(std::memory_order_acquire);
}

std::size_t wd_display_timeout_selection() {
    return choice_index(kTimeoutChoices, wd_display_timeout_seconds(), 0);
}

void wd_display_set_timeout_selection(std::size_t selection) {
    selection %= sizeof(kTimeoutChoices) / sizeof(kTimeoutChoices[0]);
    const uint16_t timeout = kTimeoutChoices[selection];
    s_timeout_seconds.store(timeout, std::memory_order_release);
    s_last_activity.store(xTaskGetTickCount(), std::memory_order_release);
    save_u16("timeout", timeout);
}

// 档数的唯一出口（见头文件里的说明：没有它就算不出"上一档"）。
std::size_t wd_display_brightness_tier_count() {
    return sizeof(kBrightnessChoices) / sizeof(kBrightnessChoices[0]);
}

std::size_t wd_display_timeout_tier_count() {
    return sizeof(kTimeoutChoices) / sizeof(kTimeoutChoices[0]);
}

