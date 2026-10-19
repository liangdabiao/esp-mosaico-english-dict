// main/wd_cache.c —— 发音缓存：SPI NAND + LittleFS（/nand）。
//
// 挂载链与 Recovery 固件（factory_nand_update.c）一致：
//   SPI3 + SIO 40 MHz → spi_nand_flash_init_with_layers（WL 块设备）
//   → esp_vfs_littlefs_register(/nand, blockdev, format_if_mount_failed=false)
//
// 容量：实测卷可用约 121.7 MB（59417 × 2048 B）。缓存按 256 个分片目录
// 组织，每片最多 400 KiB / 48 个文件 ⇒ 总量约 100 MiB，给文件系统元数据
// 和写入留出余量。分片满时按 mtime 删除最旧的条目（命中会 touch）。

#include "wd_cache.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "bsp/esp_mosaico.h"
#include "bsp/nand_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_blockdev.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "spi_nand_flash.h"

namespace {

constexpr char kTag[] = "wd_cache";

constexpr char kMountPath[] = "/nand";
constexpr char kCacheRoot[] = "/nand/wdvoice";
constexpr size_t kPathMax = 96;

// 分片限额：256 片 × 400 KiB ≈ 100 MiB。单文件上限 192 KiB ——
// 单词 MP3 实测最大 27 KiB，整句朗读（type=2，24 kHz）最长约 80 KiB；
// 上限只用于挡住异常响应体。
constexpr size_t kShardBytes = 400 * 1024;
constexpr size_t kShardFiles = 48;
constexpr size_t kMaxFileBytes = 192 * 1024;
constexpr size_t kPruneEntries = 256;

bool s_mounted = false;
bool s_init_done = false;
// 写满一次就停缓存，直到重启。用 ENOSPC 事实驱动，而不是每次播放前
// 查询卷用量 —— 后者在小块 NAND 大卷上要遍历全部 59k 个块（实测约 26 秒），
// 会把播放任务卡在"正在加载"上。
bool s_disk_full = false;

bool s_bus_owned = false;
bool s_device_added = false;
spi_device_handle_t s_spi = nullptr;
esp_blockdev_handle_t s_blockdev = nullptr;

FILE *s_writer = nullptr;
size_t s_writer_bytes = 0;
char s_writer_tmp[kPathMax + 8];
char s_writer_dst[kPathMax];
// 缓存键可以是单词，也可以是带 "s:" 前缀的整句，最长 192+2 字符。
char s_writer_key[256];

typedef struct {
    char name[48];
    time_t mtime;
    off_t size;
} cache_entry_t;

uint64_t fnv1a64(const char *text) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p =
             reinterpret_cast<const unsigned char *>(text);
         *p; ++p) {
        hash ^= static_cast<uint64_t>(*p);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void build_shard(uint64_t hash, char *out, size_t capacity) {
    std::snprintf(out, capacity, "%s/%02x", kCacheRoot,
                  static_cast<unsigned>((hash >> 56) & 0xffu));
}

void build_path(uint64_t hash, char *out, size_t capacity) {
    std::snprintf(out, capacity, "%s/%02x/%016" PRIx64 ".mp3", kCacheRoot,
                  static_cast<unsigned>((hash >> 56) & 0xffu), hash);
}

void build_tmp_path(uint64_t hash, char *out, size_t capacity) {
    std::snprintf(out, capacity, "%s/%02x/%016" PRIx64 ".mp3.tmp", kCacheRoot,
                  static_cast<unsigned>((hash >> 56) & 0xffu), hash);
}

esp_err_t release_control_pin(gpio_num_t pin) {
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << static_cast<unsigned>(pin);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    // HOLD#/WP# 低有效：先把输出锁存器拉高，再开 pad，避免配置瞬间误触发。
    esp_err_t err = gpio_set_level(pin, 1);
    if (err != ESP_OK) return err;
    err = gpio_config(&config);
    if (err != ESP_OK) return err;
    return gpio_set_level(pin, 1);
}

void nand_cleanup_partial(void) {
    if (s_blockdev) {
        s_blockdev->ops->release(s_blockdev);
        s_blockdev = nullptr;
    }
    if (s_device_added && s_spi) {
        (void)spi_bus_remove_device(s_spi);
    }
    s_spi = nullptr;
    s_device_added = false;
    if (s_bus_owned) {
        (void)spi_bus_free(BSP_NAND_SPI_HOST);
    }
    s_bus_owned = false;
}

bool nand_mount(void) {
    ESP_LOGI(kTag, "NAND 挂载开始");
    esp_err_t err = release_control_pin(BSP_NAND_HOLD);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "释放 NAND HOLD 失败: %s", esp_err_to_name(err));
        return false;
    }
    err = release_control_pin(BSP_NAND_WP);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "释放 NAND WP 失败: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "HOLD/WP 已释放");

    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = BSP_NAND_D;
    bus_config.miso_io_num = BSP_NAND_Q;
    bus_config.sclk_io_num = BSP_NAND_CLK;
    bus_config.quadwp_io_num = GPIO_NUM_NC;
    bus_config.quadhd_io_num = GPIO_NUM_NC;
    bus_config.max_transfer_sz = BSP_NAND_FLASH_DEFAULT_MAX_TRANSFER_SZ;
    err = spi_bus_initialize(BSP_NAND_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        // 别的初始化已经把 SPI3 拉起来了：沿用它的总线配置。
        ESP_LOGW(kTag, "SPI3 已初始化，沿用现有总线");
    } else if (err != ESP_OK) {
        ESP_LOGW(kTag, "NAND SPI 总线初始化失败: %s", esp_err_to_name(err));
        return false;
    } else {
        s_bus_owned = true;
    }
    ESP_LOGI(kTag, "SPI3 总线就绪");

    spi_device_interface_config_t device_config = {};
    device_config.clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ;
    device_config.mode = 0;
    device_config.spics_io_num = BSP_NAND_CS;
    device_config.queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE;
    device_config.flags = SPI_DEVICE_HALFDUPLEX;
    err = spi_bus_add_device(BSP_NAND_SPI_HOST, &device_config, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "NAND SPI 设备添加失败: %s", esp_err_to_name(err));
        nand_cleanup_partial();
        return false;
    }
    s_device_added = true;
    ESP_LOGI(kTag, "SPI 设备已添加，开始 NAND 探测");

    // SIO + WL 分层块设备：Recovery 固件同款，兼容性和可写性已在真机验证。
    spi_nand_flash_config_t nand_config = {};
    nand_config.device_handle = s_spi;
    nand_config.gc_factor = 0;
    nand_config.io_mode = SPI_NAND_IO_MODE_SIO;
    nand_config.flags = SPI_DEVICE_HALFDUPLEX;
    err = spi_nand_flash_init_with_layers(&nand_config, &s_blockdev);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "NAND 初始化失败: %s", esp_err_to_name(err));
        nand_cleanup_partial();
        return false;
    }
    ESP_LOGI(kTag, "NAND 块设备就绪，开始挂载 LittleFS");

    esp_vfs_littlefs_conf_t mount_config = {};
    mount_config.base_path = kMountPath;
    mount_config.blockdev = s_blockdev;
    // 绝不自动格式化：卷坏了应该在日志里暴露出来，而不是悄悄清空。
    mount_config.format_if_mount_failed = false;
    mount_config.read_only = false;
    mount_config.dont_mount = false;
    mount_config.grow_on_mount = false;
    err = esp_vfs_littlefs_register(&mount_config);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "LittleFS 挂载失败: %s（发音缓存禁用，仍可在线发音）",
                 esp_err_to_name(err));
        // register 失败时已消费并释放块设备。
        s_blockdev = nullptr;
        nand_cleanup_partial();
        return false;
    }

    if (mkdir(kCacheRoot, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(kTag, "创建缓存目录失败: %s", strerror(errno));
        return false;
    }

    // 这里不查卷用量：esp_littlefs_blockdev_info() 会遍历整卷，
    // 在 121.7 MB / 59417 块上实测要 ~26 秒。挂载本身已经够慢，
    // 再扫一遍只会把缓存可用时间继续往后推。空间不足由写入 ENOSPC 兜底。
    ESP_LOGI(kTag, "NAND LittleFS 已挂载，缓存目录 %s", kCacheRoot);
    return true;
}

int entry_compare(const void *lhs, const void *rhs) {
    const auto *a = static_cast<const cache_entry_t *>(lhs);
    const auto *b = static_cast<const cache_entry_t *>(rhs);
    if (a->mtime != b->mtime) return a->mtime < b->mtime ? -1 : 1;
    return std::strcmp(a->name, b->name);
}

void prune_shard(const char *file_path) {
    char dir[kPathMax];
    std::snprintf(dir, sizeof(dir), "%s", file_path);
    char *slash = std::strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    DIR *handle = opendir(dir);
    if (!handle) return;

    auto *entries = static_cast<cache_entry_t *>(
        std::calloc(kPruneEntries, sizeof(cache_entry_t)));
    if (!entries) {
        closedir(handle);
        return;
    }

    size_t count = 0;
    size_t found = 0;
    size_t bytes = 0;
    struct dirent *item = nullptr;
    while ((item = readdir(handle)) != nullptr) {
        if (item->d_name[0] == '.') continue;
        const size_t name_len = std::strlen(item->d_name);
        if (name_len < 5 || std::strcmp(item->d_name + name_len - 4, ".mp3") != 0) {
            continue;
        }
        char full[kPathMax];
        const int written = std::snprintf(full, sizeof(full), "%s/%s", dir,
                                          item->d_name);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(full)) continue;
        struct stat info = {};
        if (stat(full, &info) != 0) continue;
        ++found;
        bytes += static_cast<size_t>(info.st_size);

        // 只保留最旧的 kPruneEntries 个候选；分片正常只有几十个文件，
        // 这里主要防异常情况下目录失控。
        cache_entry_t candidate = {};
        std::snprintf(candidate.name, sizeof(candidate.name), "%s", item->d_name);
        candidate.mtime = info.st_mtime;
        candidate.size = info.st_size;
        if (count < kPruneEntries) {
            entries[count++] = candidate;
        } else {
            size_t newest = 0;
            for (size_t i = 1; i < count; ++i) {
                if (entries[i].mtime > entries[newest].mtime) newest = i;
            }
            if (candidate.mtime < entries[newest].mtime) entries[newest] = candidate;
        }
    }
    closedir(handle);

    if (bytes <= kShardBytes && found <= kShardFiles) {
        std::free(entries);
        return;
    }

    std::qsort(entries, count, sizeof(cache_entry_t), entry_compare);
    size_t removed = 0;
    for (size_t i = 0; i < count && (bytes > kShardBytes || found > kShardFiles);
         ++i) {
        char full[kPathMax];
        const int written = std::snprintf(full, sizeof(full), "%s/%s", dir,
                                          entries[i].name);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(full)) continue;
        if (unlink(full) == 0) {
            const size_t entry_size = static_cast<size_t>(entries[i].size);
            bytes = entry_size >= bytes ? 0 : bytes - entry_size;
            if (found > 0) --found;
            ++removed;
        }
    }
    std::free(entries);
    if (removed > 0) {
        ESP_LOGI(kTag, "分片 %s 淘汰 %u 个旧缓存（剩 %u B / %u 个）", dir,
                 static_cast<unsigned>(removed), static_cast<unsigned>(bytes),
                 static_cast<unsigned>(found));
    }
}

}  // namespace

extern "C" bool wd_cache_init(void) {
    if (s_init_done) return s_mounted;
    s_init_done = true;
    s_mounted = nand_mount();
    return s_mounted;
}

extern "C" bool wd_cache_ready(void) { return s_mounted; }

extern "C" bool wd_cache_path(const char *key, char *out, size_t capacity) {
    if (!key || !key[0] || !out || capacity < kPathMax) return false;
    build_path(fnv1a64(key), out, capacity);
    return true;
}

extern "C" bool wd_cache_has(const char *key) {
    char path[kPathMax];
    if (!s_mounted || !wd_cache_path(key, path, sizeof(path))) return false;
    struct stat info = {};
    return stat(path, &info) == 0 && info.st_size > 0;
}

extern "C" void wd_cache_touch(const char *path) {
    if (!s_mounted || !path) return;
    (void)utime(path, nullptr);
}

extern "C" void wd_cache_remove(const char *path) {
    if (!s_mounted || !path) return;
    (void)unlink(path);
}

extern "C" bool wd_cache_writer_begin(const char *key) {
    wd_cache_writer_abort();
    if (!s_mounted || !key || !key[0]) return false;
    // 空间统计不能放在这里：见文件头的扫描代价说明。写满由 feed 的
    // ENOSPC 记账（s_disk_full）负责，一旦置位就只影响缓存，不影响播放。
    if (s_disk_full) return false;

    const uint64_t hash = fnv1a64(key);
    char shard[kPathMax];
    build_shard(hash, shard, sizeof(shard));
    if (mkdir(shard, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(kTag, "创建分片目录失败: %s", strerror(errno));
        return false;
    }

    build_path(hash, s_writer_dst, sizeof(s_writer_dst));
    build_tmp_path(hash, s_writer_tmp, sizeof(s_writer_tmp));
    s_writer = std::fopen(s_writer_tmp, "wb");
    if (!s_writer) {
        ESP_LOGW(kTag, "缓存临时文件创建失败: %s", strerror(errno));
        return false;
    }
    std::setvbuf(s_writer, nullptr, _IOFBF, 4096);
    s_writer_bytes = 0;
    std::snprintf(s_writer_key, sizeof(s_writer_key), "%s", key);
    return true;
}

extern "C" void wd_cache_writer_feed(const void *data, size_t len) {
    if (!s_writer || !data || len == 0) return;
    if (s_writer_bytes + len > kMaxFileBytes) {
        ESP_LOGD(kTag, "发音流超过缓存上限，放弃缓存: %s", s_writer_key);
        wd_cache_writer_abort();
        return;
    }
    if (std::fwrite(data, 1, len, s_writer) != len) {
        if (errno == ENOSPC) {
            s_disk_full = true;
            ESP_LOGW(kTag, "NAND 已写满，停止缓存（发音不受影响）");
        } else {
            ESP_LOGW(kTag, "缓存写入失败: %s", strerror(errno));
        }
        wd_cache_writer_abort();
        return;
    }
    s_writer_bytes += len;
}

extern "C" bool wd_cache_writer_commit(void) {
    if (!s_writer) return false;

    bool ok = s_writer_bytes > 0;
    int write_errno = 0;
    if (std::fflush(s_writer) != 0) {
        ok = false;
        write_errno = errno;
    }
    if (std::fclose(s_writer) != 0) {
        ok = false;
        if (!write_errno) write_errno = errno;
    }
    s_writer = nullptr;

    if (!ok) {
        if (write_errno == ENOSPC) {
            s_disk_full = true;
            ESP_LOGW(kTag, "NAND 已写满，停止缓存（发音不受影响）");
        } else if (write_errno != 0) {
            ESP_LOGW(kTag, "缓存提交失败: %s", strerror(write_errno));
        }
        (void)unlink(s_writer_tmp);
        return false;
    }
    if (std::rename(s_writer_tmp, s_writer_dst) != 0) {
        ESP_LOGW(kTag, "缓存提交失败: %s", strerror(errno));
        (void)unlink(s_writer_tmp);
        return false;
    }

    ESP_LOGI(kTag, "已缓存 %s（%u B）", s_writer_key,
             static_cast<unsigned>(s_writer_bytes));
    prune_shard(s_writer_dst);
    return true;
}

extern "C" void wd_cache_writer_abort(void) {
    if (!s_writer) return;
    std::fclose(s_writer);
    s_writer = nullptr;
    (void)unlink(s_writer_tmp);
    s_writer_bytes = 0;
}
