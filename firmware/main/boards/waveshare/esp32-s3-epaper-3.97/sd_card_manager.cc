#include "sd_card_manager.h"

#include <dirent.h>
#include <errno.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdmmc_cmd.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"

#define TAG "SdCardManager"

// 微雪 ESP32-S3-ePaper-3.97 官方例程给出的 SDMMC 引脚
#define SD_CLK_PIN GPIO_NUM_16
#define SD_CMD_PIN GPIO_NUM_17
#define SD_D0_PIN  GPIO_NUM_15
#define SD_D1_PIN  GPIO_NUM_7
#define SD_D2_PIN  GPIO_NUM_8
#define SD_D3_PIN  GPIO_NUM_18

SdCardManager& SdCardManager::GetInstance() {
    static SdCardManager instance;
    return instance;
}

bool SdCardManager::TryMount(int bus_width) {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 32 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    // 高速模式(40MHz)对部分卡/接线不稳定，统一用默认 20MHz 提高兼容性
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = bus_width;
    slot_config.clk = SD_CLK_PIN;
    slot_config.cmd = SD_CMD_PIN;
    slot_config.d0 = SD_D0_PIN;
    slot_config.d1 = SD_D1_PIN;
    slot_config.d2 = SD_D2_PIN;
    slot_config.d3 = SD_D3_PIN;

    esp_err_t ret =
        esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card_);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (width=%d): %s", bus_width, esp_err_to_name(ret));
        card_ = nullptr;
        // 释放总线，否则下一次重试会报 ESP_ERR_INVALID_STATE
        sdmmc_host_deinit();
        return false;
    }
    ESP_LOGI(TAG, "SD card mounted at %s (width=%d)", SD_MOUNT_POINT, bus_width);
    return true;
}

bool SdCardManager::Init() {
    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
    }
    // 依次尝试：4线 -> 1线（均已降为默认 20MHz 频率）
    if (!TryMount(4) && !TryMount(1)) {
        ESP_LOGW(TAG, "==========================================");
        ESP_LOGW(TAG, "SD 卡未检测到或挂载失败！请检查：");
        ESP_LOGW(TAG, " 1. SD 卡是否插紧（板载卡槽在屏幕排线旁）");
        ESP_LOGW(TAG, " 2. 卡片是否格式化为 FAT32（exFAT/NTFS 不支持）");
        ESP_LOGW(TAG, "本地知识库已关闭，对话将使用网络知识库。");
        ESP_LOGW(TAG, "==========================================");
        mounted_ = false;
        return false;
    }
    mounted_ = true;
    if (card_ != nullptr) {
        sdmmc_card_print_info(stdout, card_);
    }
    // 启动时打印根目录内容，便于排查目录结构问题
    ESP_LOGI(TAG, "----- SD 卡根目录内容 -----");
    auto root_entries = ListDir(SD_MOUNT_POINT);
    for (const auto& name : root_entries) {
        ESP_LOGI(TAG, "  %s", name.c_str());
    }
    if (root_entries.empty()) {
        ESP_LOGW(TAG, "  (空目录或读取失败)");
    }
    ESP_LOGI(TAG, "---------------------------");
    // 预建工作目录
    EnsureDir(std::string(SD_MOUNT_POINT) + "/knowledge");
    EnsureDir(std::string(SD_MOUNT_POINT) + "/chatlog");
    EnsureDir(std::string(SD_MOUNT_POINT) + "/study");
    return true;
}

void SdCardManager::Unmount() {
    if (mounted_ && card_ != nullptr) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, card_);
        ESP_LOGI(TAG, "SD card unmounted");
    }
    card_ = nullptr;
    mounted_ = false;
}

bool SdCardManager::IsReady() {
    if (!mounted_ || card_ == nullptr) {
        return false;
    }
    return sdmmc_get_status(card_) == ESP_OK;
}

std::string SdCardManager::GetStatusText() {
    if (!mounted_) {
        return "SD卡未挂载（未插卡或初始化失败），本地知识库不可用，将使用网络知识库";
    }
    if (!IsReady()) {
        return "SD卡读写异常，本地知识库暂不可用，将使用网络知识库";
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "SD卡正常，容量 %llu MB，本地知识库已启用",
             (unsigned long long)GetTotalSizeMB());
    return buf;
}

uint64_t SdCardManager::GetTotalSizeMB() {
    if (card_ == nullptr) {
        return 0;
    }
    return ((uint64_t)card_->csd.capacity) * card_->csd.sector_size / (1024 * 1024);
}

bool SdCardManager::EnsureDir(const std::string& path) {
    if (!mounted_) {
        return false;
    }
    // 逐级创建目录
    std::string cur;
    size_t i = 0;
    if (!path.empty() && path[0] == '/') {
        cur = "/";
        i = 1;
    }
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        std::string part = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!part.empty()) {
            if (cur.back() != '/') cur += '/';
            cur += part;
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                ESP_LOGW(TAG, "mkdir failed: %s (errno=%d)", cur.c_str(), errno);
                return false;
            }
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return true;
}

bool SdCardManager::AppendFile(const std::string& path, const std::string& data) {
    if (!IsReady()) {
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    // 确保父目录存在
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        EnsureDir(path.substr(0, slash));
    }
    FILE* f = fopen(path.c_str(), "ab");
    if (f == nullptr) {
        ESP_LOGW(TAG, "append open failed: %s", path.c_str());
        xSemaphoreGive(mutex_);
        return false;
    }
    size_t written = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    xSemaphoreGive(mutex_);
    return written == data.size();
}

bool SdCardManager::WriteFile(const std::string& path, const std::string& data) {
    if (!IsReady()) {
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        EnsureDir(path.substr(0, slash));
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        xSemaphoreGive(mutex_);
        return false;
    }
    size_t written = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    xSemaphoreGive(mutex_);
    return written == data.size();
}

bool SdCardManager::ReadFile(const std::string& path, std::string& out, size_t max_bytes) {
    if (!IsReady()) {
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        xSemaphoreGive(mutex_);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) {
        fclose(f);
        xSemaphoreGive(mutex_);
        return false;
    }
    if ((size_t)len > max_bytes) {
        len = max_bytes;
    }
    out.resize(len);
    size_t read = fread(out.data(), 1, len, f);
    out.resize(read);
    fclose(f);
    xSemaphoreGive(mutex_);
    return true;
}

bool SdCardManager::ReadFileInto(const std::string& path, char* buf, size_t buf_size,
                                 size_t& out_len) {
    out_len = 0;
    if (!IsReady() || buf == nullptr || buf_size == 0) {
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        xSemaphoreGive(mutex_);
        return false;
    }
    out_len = fread(buf, 1, buf_size, f);
    fclose(f);
    xSemaphoreGive(mutex_);
    return true;
}

bool SdCardManager::ReadFileRange(const std::string& path, size_t offset, size_t max_bytes,
                                  std::string& out) {
    out.clear();
    if (!IsReady() || max_bytes == 0) {
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        xSemaphoreGive(mutex_);
        return false;
    }
    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        xSemaphoreGive(mutex_);
        return false;
    }
    out.resize(max_bytes);
    size_t read = fread(out.data(), 1, max_bytes, f);
    out.resize(read);
    fclose(f);
    xSemaphoreGive(mutex_);
    return read > 0;
}

long SdCardManager::GetFileSize(const std::string& path) {
    if (!IsReady()) {
        return -1;
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

std::vector<std::string> SdCardManager::ListDir(const std::string& path) {
    std::vector<std::string> result;
    if (!IsReady()) {
        return result;
    }
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return result;
    }
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        result.emplace_back(entry->d_name);
    }
    closedir(dir);
    return result;
}
