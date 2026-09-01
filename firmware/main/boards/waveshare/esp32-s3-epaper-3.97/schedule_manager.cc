#include "schedule_manager.h"
#include "sd_card_manager.h"
#include "config.h"
#include <esp_log.h>
#include <algorithm>
#include <time.h>

#define TAG "ScheduleManager"

ScheduleManager& ScheduleManager::GetInstance() {
    static ScheduleManager instance;
    return instance;
}

int ScheduleManager::TodayWeekday() {
    time_t now = time(nullptr);
    if (now < 1704067200) return 0;
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    int w = tm_info.tm_wday;
    return (w == 0) ? 7 : w;
}

std::string ScheduleManager::WeekdayName(int weekday) {
    const char* names[] = {"", "周一", "周二", "周三", "周四", "周五", "周六", "周日"};
    if (weekday < 1 || weekday > 7) return "未知";
    return names[weekday];
}

void ScheduleManager::Load() {
    items_.clear();
    loaded_ = false;
    error_reason_.clear();
    auto& sd = SdCardManager::GetInstance();
    if (!sd.IsReady()) {
        ESP_LOGW(TAG, "SD not ready, cannot load schedule");
        error_reason_ = "SD卡未就绪";
        return;
    }
    sd.EnsureDir(std::string(SD_MOUNT_POINT) + "/schedule");
    std::string text;
    if (!sd.ReadFile(DataPath(), text, 32 * 1024)) {
        ESP_LOGW(TAG, "schedule file not found at %s", DataPath().c_str());
        error_reason_ = "文件不存在";
        return;
    }
    size_t pos = 0;
    int lines = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t p1 = line.find('|');
        size_t p2 = p1 == std::string::npos ? p1 : line.find('|', p1 + 1);
        size_t p3 = p2 == std::string::npos ? p2 : line.find('|', p2 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) continue;
        ScheduleItem it;
        it.weekday = atoi(line.substr(0, p1).c_str());
        it.period = atoi(line.substr(p1 + 1, p2 - p1 - 1).c_str());
        it.subject = line.substr(p2 + 1, p3 == std::string::npos ? std::string::npos : p3 - p2 - 1);
        if (p3 != std::string::npos) it.note = line.substr(p3 + 1);
        if (it.weekday >= 1 && it.weekday <= 7 && it.period >= 0 && it.period <= 6 && !it.subject.empty()) {
            items_.push_back(std::move(it));
            lines++;
        }
    }
    std::sort(items_.begin(), items_.end(), [](const ScheduleItem& a, const ScheduleItem& b) {
        if (a.weekday != b.weekday) return a.weekday < b.weekday;
        return a.period < b.period;
    });
    loaded_ = true;
    loaded_weekday_ = TodayWeekday();
    ESP_LOGI(TAG, "loaded %d schedule items", lines);
}

std::vector<ScheduleItem> ScheduleManager::GetDaySchedule(int weekday) {
    if (!loaded_ || loaded_weekday_ != TodayWeekday()) Load();
    std::vector<ScheduleItem> res;
    for (const auto& it : items_) {
        if (it.weekday == weekday) res.push_back(it);
    }
    return res;
}

std::string ScheduleManager::GetDayScheduleText(int weekday) {
    if (!loaded_ || loaded_weekday_ != TodayWeekday()) Load();
    auto items = GetDaySchedule(weekday);
    if (items.empty()) {
        if (!error_reason_.empty()) {
            return "无课\n" + error_reason_ + "\n请检查SD卡";
        }
        return "无课\n请检查SD卡\n/schedule/\nschedule.txt";
    }
    std::string out;
    for (const auto& it : items) {
        if (it.period == 0) out += "早读";
        else out += std::to_string(it.period) + ".";
        out += it.subject;
        if (!it.note.empty()) out += "(" + it.note + ")";
        out += "\n";
    }
    return out;
}

std::string ScheduleManager::GetTodayScheduleText() {
    if (!loaded_ || loaded_weekday_ != TodayWeekday()) Load();
    int today = TodayWeekday();
    if (today == 0) return "时间未同步\n请联网后重试";
    if (today > 5) {
        auto mon = GetDaySchedule(1);
        if (mon.empty()) return "周末愉快！\n下周一课程表未配置";
        std::string out = "【下周一】\n";
        for (const auto& it : mon) {
            if (it.period == 0) out += "早读";
            else out += std::to_string(it.period) + "." + it.subject;
            if (!it.note.empty()) out += "(" + it.note + ")";
            out += "\n";
        }
        return out;
    }
    auto items = GetDaySchedule(today);
    if (items.empty()) return WeekdayName(today) + "\n今日无课\n请检查SD卡课程表";
    std::string out = "【" + WeekdayName(today) + "】\n";
    for (const auto& it : items) {
        if (it.period == 0) out += "早读";
        else out += std::to_string(it.period) + "." + it.subject;
        if (!it.note.empty()) out += "(" + it.note + ")";
        out += "\n";
    }
    return out;
}

std::string ScheduleManager::GetTomorrowPreview() {
    if (!loaded_ || loaded_weekday_ != TodayWeekday()) Load();
    int today = TodayWeekday();
    if (today == 0) return "";
    int tomorrow = today + 1;
    if (tomorrow > 5) tomorrow = 1;
    auto items = GetDaySchedule(tomorrow);
    if (items.empty()) return "";
    std::string out = "明日" + WeekdayName(tomorrow) + "：";
    int count = 0;
    for (const auto& it : items) {
        if (it.period >= 1 && it.period <= 4) {
            if (count > 0) out += " ";
            out += it.subject;
            count++;
            if (count >= 4) break;
        }
    }
    return out;
}

std::string ScheduleManager::GetRecent3DaysText() {
    if (!loaded_ || loaded_weekday_ != TodayWeekday()) Load();
    int today = TodayWeekday();
    if (today == 0) return "时间未同步";
    std::string out;
    for (int d = 0; d < 3; d++) {
        int wd = today + d;
        if (wd > 7) wd -= 7;
        if (wd > 5) continue;
        auto day_items = GetDaySchedule(wd);
        if (day_items.empty()) continue;
        out += "【" + WeekdayName(wd);
        if (d == 0) out += "·今天";
        out += "】\n";
        for (const auto& it : day_items) {
            if (it.period == 0) out += "早读";
            else out += std::to_string(it.period) + "." + it.subject;
            if (!it.note.empty()) out += "(" + it.note + ")";
            out += "\n";
        }
        out += "\n";
    }
    return out;
}
