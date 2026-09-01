#ifndef __SD_CARD_MANAGER_H__
#define __SD_CARD_MANAGER_H__

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string>
#include <vector>
#include "driver/sdmmc_host.h"

/**
 * SD 卡管理器（微雪 ESP32-S3-ePaper-3.97 板载 SD 卡槽，SDMMC 接口）
 *
 * 引脚定义来自微雪官方例程（ESP32-S3-ePaper-3.97 / sdcard_bsp）：
 *   CLK=16  CMD=17  D0=15  D1=7  D2=8  D3=18（4 线模式，失败时自动退回 1 线）
 *
 * 使用规则：
 *  - Init() 返回 false 表示未插卡或读写异常，此时所有功能自动回退到网络知识库；
 *  - 所有文件操作前内部都会校验卡状态，操作线程安全。
 */
class SdCardManager {
public:
    static SdCardManager& GetInstance();

    // 挂载 SD 卡。成功返回 true；未插卡/挂载失败返回 false（不影响系统其他功能）。
    bool Init();

    // 卸载 SD 卡（配合 Init 实现重新挂载）
    void Unmount();

    // SD 卡是否可用
    bool IsReady();

    // 状态描述（用于 MCP 工具 / 日志）
    std::string GetStatusText();

    // 追加写入文件（目录不存在会自动创建）。返回是否成功。
    bool AppendFile(const std::string& path, const std::string& data);

    // 读取整个文本文件（最多 max_bytes）。返回是否成功。
    bool ReadFile(const std::string& path, std::string& out, size_t max_bytes = 256 * 1024);

    // 读取文件内容到调用方提供的缓冲区（可传 PSRAM 缓冲，避免大文件占用内部 RAM）。
    // 最多读 buf_size 字节，实际读到的字节数经 out_len 返回。返回是否成功。
    bool ReadFileInto(const std::string& path, char* buf, size_t buf_size, size_t& out_len);

    // 读取文件指定偏移起的一段（知识库词条原文按偏移动态加载用）。
    // 返回是否成功；读到的内容经 out 返回（可能因文件尾而短于 max_bytes）。
    bool ReadFileRange(const std::string& path, size_t offset, size_t max_bytes,
                       std::string& out);

    // 覆盖写入文件。返回是否成功。
    bool WriteFile(const std::string& path, const std::string& data);

    // 列出目录下的条目（不递归）
    std::vector<std::string> ListDir(const std::string& path);

    // 获取文件大小（字节），失败返回 -1
    long GetFileSize(const std::string& path);

    // 确保目录存在（递归创建）
    bool EnsureDir(const std::string& path);

    // 卡容量信息（MB），不可用时返回 0
    uint64_t GetTotalSizeMB();

private:
    SdCardManager() = default;
    ~SdCardManager() = default;
    SdCardManager(const SdCardManager&) = delete;
    SdCardManager& operator=(const SdCardManager&) = delete;

    bool TryMount(int bus_width);

    bool mounted_ = false;
    sdmmc_card_t* card_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;
};

#endif  // __SD_CARD_MANAGER_H__
