#include "chat_logger.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "sd_card_manager.h"

#define TAG "ChatLogger"

ChatLogger& ChatLogger::GetInstance() {
    static ChatLogger instance;
    return instance;
}

bool ChatLogger::TimeValid() {
    time_t now = time(nullptr);
    // 2024-01-01 之后认为时间已同步
    return now > 1704067200;
}

std::string ChatLogger::Timestamp() {
    char buf[48];
    if (TimeValid()) {
        time_t now = time(nullptr);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_info.tm_hour, tm_info.tm_min,
                 tm_info.tm_sec);
    } else {
        // 时间未同步时用开机时长
        int64_t sec = esp_timer_get_time() / 1000000;
        snprintf(buf, sizeof(buf), "boot+%llds", (long long)sec);
    }
    return buf;
}

std::string ChatLogger::TodayLogPath() {
    char buf[96];
    if (TimeValid()) {
        time_t now = time(nullptr);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        snprintf(buf, sizeof(buf), "%s/%04d%02d%02d.txt", SD_CHATLOG_DIR,
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);
    } else {
        snprintf(buf, sizeof(buf), "%s/unknown_date.txt", SD_CHATLOG_DIR);
    }
    return buf;
}

bool ChatLogger::IsEnabled() { return SdCardManager::GetInstance().IsReady(); }

void ChatLogger::Log(const char* role, const char* content) {
    if (role == nullptr || content == nullptr || content[0] == '\0') {
        return;
    }
    // 只记录真实对话，不记录系统提示
    if (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0) {
        return;
    }
    if (!IsEnabled()) {
        return;
    }
    std::string line = "[" + Timestamp() + "] " + role + ": " + content + "\n";
    if (!SdCardManager::GetInstance().AppendFile(TodayLogPath(), line)) {
        ESP_LOGW(TAG, "write chat log failed");
    }
}

void ChatLogger::LogCheckin(const std::string& subject, const std::string& item,
                            const std::string& result) {
    if (!IsEnabled()) {
        return;
    }
    char buf[96];
    if (TimeValid()) {
        time_t now = time(nullptr);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        snprintf(buf, sizeof(buf), "%s/checkin_%04d%02d%02d.txt", SD_STUDY_DIR,
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);
    } else {
        snprintf(buf, sizeof(buf), "%s/checkin_unknown_date.txt", SD_STUDY_DIR);
    }
    std::string line = Timestamp() + "," + subject + "," + item + "," + result + "\n";
    SdCardManager::GetInstance().AppendFile(buf, line);
}

void ChatLogger::LogMistake(const std::string& entry_id, const std::string& description) {
    if (!IsEnabled()) {
        return;
    }
    std::string line = "[" + Timestamp() + "] " + entry_id + ": " + description + "\n";
    SdCardManager::GetInstance().AppendFile(std::string(SD_STUDY_DIR) + "/mistakes.txt", line);
}

std::string ChatLogger::GetCheckinSummary(int days) {
    if (!IsEnabled()) {
        return "";
    }
    auto& sd = SdCardManager::GetInstance();
    std::string summary;
    // 统计最近 days 天的打卡行数
    for (int d = 0; d < days; d++) {
        time_t now = time(nullptr) - d * 86400;
        if (!TimeValid()) break;
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        char path[96];
        snprintf(path, sizeof(path), "%s/checkin_%04d%02d%02d.txt", SD_STUDY_DIR,
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);
        std::string content;
        if (sd.ReadFile(path, content, 32 * 1024)) {
            int lines = 0;
            for (char c : content) {
                if (c == '\n') lines++;
            }
            char date[40];
            snprintf(date, sizeof(date), "%04d-%02d-%02d", tm_info.tm_year + 1900,
                     tm_info.tm_mon + 1, tm_info.tm_mday);
            summary += date;
            summary += ": " + std::to_string(lines) + " 项打卡\n";
        }
    }
    return summary;
}
