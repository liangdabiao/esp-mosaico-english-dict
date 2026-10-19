// main/main.cc —— 英语词典（ESP-Mosaico 移植版）入口。
//
// 硬件：esp32s31 + CO5300 正方形 480×480 触摸屏 + ES8311 音频。
// 内容：词库与字库全部编进固件（约 2.75 MB rodata），查词/翻面/测验不需要
//       网络 —— 发音首次来自有道公开接口（见 wd_audio.h），播放成功后写入
//       /nand 缓存，之后离线也能重播听过的词。
// 配网：手机连上设备热点后在门户网页里完成（esp-wifi-connect 组件的内置
//       httpd），设备侧只显示热点名与指引 —— 触摸交互全在词典六屏里。
//
// 与 ESP32-C3 原版的差异集中在三处：
//   * 显示/触摸：bsp_display_init + bsp_lvgl_init + bsp_button
//     → bsp_display_start() 一个调用（LVGL 9.5 + cst9220 触摸由 BSP 就绪，
//     触摸事件经 BSP indev 自动分发，页面只挂 LV_EVENT_CLICKED —— 见 wd_ui.c）。
//   * 按键：三键 ADC 分压 → 触摸直达。wd_app 的入口从 wd_app_key(btn, ev)
//     换成 wd_app_on_ui_evt(wd_ui_evt_t, int)（见 wd_app.h）。
//   * 电池：CW2017 电量计不随本板初始化（bq27220 路径未启用），状态栏电量
//     恒显 0 —— 接口保留，以后接上充电检测时不必再改 UI。
#include "bsp/display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "iris_ota_support.h"
}
#include "lvgl.h"
#include "nvs_flash.h"

#include "wd_app.h"
#include "wd_audio.h"
#include "wd_cache.h"
#include "wd_display.h"

#include "ssid_manager.h"
#include "wifi_manager.h"

#include <atomic>
#include <string>

static const char *TAG = "english_dict";

namespace {

std::atomic<bool> s_connected{false};

// Wi-Fi 事件（组件事件任务上下文，回调里只注入状态，不碰 LVGL 控件本体 ——
// 与 C3 版同一条纪律）。
void wifi_event(WifiEvent event, const std::string &data) {
    auto &manager = WifiManager::GetInstance();
    switch (event) {
        case WifiEvent::Connected: {
            ESP_LOGI(TAG, "Wi-Fi connected: %s, IP=%s", data.c_str(),
                     manager.GetIpAddress().c_str());
            s_connected.store(true);
            wd_app_set_network(true);
            // 发音是短请求，关省电模式降低首包延迟（C3 版同款）。
            manager.SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
            break;
        }
        case WifiEvent::Disconnected:
            ESP_LOGI(TAG, "Wi-Fi disconnected");
            s_connected.store(false);
            wd_app_set_network(false);
            break;
        case WifiEvent::ConfigModeEnter:
            s_connected.store(false);
            wd_app_set_network(false);
            ESP_LOGI(TAG, "config AP up: %s", manager.GetApSsid().c_str());
            // 非空 SSID ⇒ 显示配网页；空串 ⇒ 回主界面（wd_app.h 的约定）。
            wd_app_set_ap_ssid(manager.GetApSsid().c_str());
            break;
        case WifiEvent::ConfigModeExit:
            wd_app_set_ap_ssid("");
            // 取消配网时把已存网络续上；没有存档就保持离线（用户在设置里随时
            // 可以再开热点 —— 词典离线可用，开门户不是唯一的活路）。
            if (!manager.IsConnected()) manager.StartStation();
            break;
        default:
            break;
    }
}

// 进配网前把发音通路让出来（C3 版 start_config_ap 的同款顺序）：
// 正在播的请求等它落定，门户的 httpd 与发音的 HTTP 客户端不抢堆。
void start_config_ap() {
    wd_audio_stop();
    wd_audio_wait_idle(2000);
    WifiManager::GetInstance().StartConfigAp();
}

// wd_app 的周期心跳（C3 版由按键任务捎带，这里没人捎 —— 单独建一个
// 500 ms 的 esp_timer）：发音状态指示刷新 + 脏断点定期落盘。
// 回调跑在 esp_timer 任务上下文，wd_app_tick 内部自己加锁，不碰这里的量。
void tick_callback(void *) { wd_app_tick(); }

// /nand 的 LittleFS 在大卷上挂载要几十秒（首次更久），放在 UI 起来之后
// 后台挂，避免开机黑屏；挂载完成前发音走在线、不写缓存。
void cache_mount_task(void *) {
    if (!wd_cache_init()) {
        ESP_LOGW(TAG, "pronunciation cache unavailable (NAND mount failed)");
    } else {
        ESP_LOGI(TAG, "cache init done");
    }
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // 屏幕是这个应用的主要出口，点不亮就没得查 —— 直接失败停住，不做降级。
    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    ESP_LOGI(TAG, "display ready");

    // 触摸是唯一输入；indev 没建起来时界面看起来正常但全部点按失效，
    // 必须在开机日志里留一条可核对的证据。
    if (bsp_display_get_input_dev()) {
        ESP_LOGI(TAG, "touch indev ready");
    } else {
        ESP_LOGE(TAG, "touch indev missing —— 界面将无法交互");
    }

    // Retained Recovery 契约：应用启动即向 Recovery 上报健康；OTA 写入器保留在
    // Vibe Mode/Recovery 一侧，应用侧不持有。
    iris_ota_support_start();
    ESP_LOGI(TAG, "iris ota support started");

    // 发音通路（注册解码器 + ES8311 + 播放任务；幂等）。音量等 wd_app_init
    // 从存档恢复后再套（见 wd_app.c 的设置页初始化）。
    wd_audio_init();
    ESP_LOGI(TAG, "audio init done");

    // 屏幕偏好（亮度/息屏超时，NVS 持久化；触摸活动自动恢复亮屏）。
    if (!wd_display_settings_init()) {
        ESP_LOGW(TAG, "display settings init failed");
    }

    // Wi-Fi 先就绪，事件在 wd_app_init 之后才可能真正到达（StartStation/
    // StartConfigAp 在 UI 建完之后才调）。
    auto &manager = WifiManager::GetInstance();
    WifiManagerConfig config;
    config.fixed_ap_ssid = "English Dict";
    config.language = "zh-CN";
    config.station_hostname = "english-dict";
    config.station_scan_min_interval_seconds = 5;
    config.station_scan_max_interval_seconds = 30;
    if (!manager.Initialize(config)) {
        ESP_LOGE(TAG, "wifi init failed");
        return;
    }
    manager.SetEventCallback(wifi_event);

    // 在持有 LVGL 锁的前提下建立 UI（触摸事件由 BSP 的 LVGL indev 自动分发，
    // 页面模块通过 LV_EVENT_CLICKED 接收 —— 见 wd_ui.c 的 build_* 系列）。
    if (!bsp_display_lock(-1)) {
        ESP_LOGE(TAG, "cannot lock LVGL");
        return;
    }
    wd_app_set_config_ap_hook(start_config_ap);
    wd_app_init();
    bsp_display_unlock();

    // 发音缓存（/nand LittleFS）放到后台挂：大卷要几十秒，不能挡住首屏。
    if (xTaskCreate(cache_mount_task, "wd_cache_mount", 8192, nullptr, 4,
                    nullptr) != pdPASS) {
        ESP_LOGW(TAG, "cache mount task create failed");
    }

    // 词库离线可用；发音首次需要联网，听过一次后落盘到 /nand，之后离线可重播。
    if (SsidManager::GetInstance().GetSsidList().empty()) {
        manager.StartConfigAp();
    } else {
        manager.StartStation();
    }


    // 500 ms 心跳：发音指示刷新 + 断点定期落盘（wd_app_tick 自带锁）。
    const esp_timer_create_args_t tick_args = {
        .callback = &tick_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wd_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t tick_timer = nullptr;
    if (esp_timer_create(&tick_args, &tick_timer) == ESP_OK) {
        esp_timer_start_periodic(tick_timer, 500 * 1000);
    } else {
        ESP_LOGW(TAG, "tick timer create failed —— 断点只在返回选书页时落盘");
    }

    ESP_LOGI(TAG, "ready: 480x480 touch");
}
