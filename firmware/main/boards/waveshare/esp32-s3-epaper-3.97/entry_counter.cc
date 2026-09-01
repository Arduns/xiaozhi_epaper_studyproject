#include "entry_counter.h"

#include <climits>
#include <esp_log.h>
#include <time.h>

#include "config.h"
#include "sd_card_manager.h"

#define TAG "EntryCounter"

EntryCounter& EntryCounter::GetInstance() {
    static EntryCounter instance;
    return instance;
}

std::string EntryCounter::DataPath() {
    return std::string(SD_STUDY_DIR) + "/entry_counts.txt";
}

int EntryCounter::TodayDate() {
    time_t now = time(nullptr);
    if (now < 1704067200) {
        return 0;
    }
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    return (tm_info.tm_year + 1900) * 10000 + (tm_info.tm_mon + 1) * 100 + tm_info.tm_mday;
}

void EntryCounter::Load() {
    if (loaded_) return;
    counts_.clear();
    auto& sd = SdCardManager::GetInstance();
    std::string text;
    if (!sd.ReadFile(DataPath(), text, 64 * 1024)) {
        ESP_LOGI(TAG, "no previous entry count data, starting fresh");
        loaded_ = true;
        return;
    }
    size_t pos = 0;
    int lines = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos
                                                                     : end - pos);
        pos = (end == std::string::npos) ? text.size() : end + 1;
        if (line.empty()) continue;
        size_t p1 = line.find('|');
        size_t p2 = p1 == std::string::npos ? p1 : line.find('|', p1 + 1);
        if (p1 == std::string::npos) continue;
        std::string key = line.substr(0, p1);
        int count = atoi(line.c_str() + p1 + 1);
        int date = (p2 == std::string::npos) ? 0 : atoi(line.c_str() + p2 + 1);
        if (!key.empty()) {
            CountInfo info;
            info.count = count;
            info.last_date = date;
            counts_[key] = info;
            lines++;
        }
    }
    loaded_ = true;
    ESP_LOGI(TAG, "loaded %d entry count records", lines);
}

void EntryCounter::Save() {
    if (!loaded_) return;
    auto& sd = SdCardManager::GetInstance();
    if (!sd.IsReady()) return;
    std::string text;
    for (const auto& kv : counts_) {
        text += kv.first + "|" + std::to_string(kv.second.count) + "|" +
                std::to_string(kv.second.last_date) + "\n";
    }
    if (!sd.WriteFile(DataPath(), text)) {
        ESP_LOGW(TAG, "save entry counts failed");
    } else {
        ESP_LOGD(TAG, "saved %d entry count records", (int)counts_.size());
    }
}

int EntryCounter::GetCount(const std::string& entry_id) {
    if (entry_id.empty()) return 0;
    if (!loaded_) Load();
    auto it = counts_.find(entry_id);
    if (it != counts_.end()) {
        return it->second.count;
    }
    return 0;
}

void EntryCounter::Record(const std::string& entry_id) {
    if (entry_id.empty()) return;
    if (!loaded_) Load();
    counts_[entry_id].count++;
    counts_[entry_id].last_date = TodayDate();
    Save();
}

std::vector<int> EntryCounter::GetLowCountCandidates(const std::vector<int>& indices,
                                                      int max_extra) {
    if (!loaded_) Load();
    if (indices.empty()) return {};

    struct Candidate {
        int idx;
        int count;
    };
    std::vector<Candidate> candidates;
    int min_count = INT_MAX;

    for (int idx : indices) {
        candidates.push_back({idx, 0});
    }

    if (candidates.empty()) return {};

    for (const auto& c : candidates) {
        if (c.count < min_count) min_count = c.count;
    }

    std::vector<int> result;
    for (const auto& c : candidates) {
        if (c.count <= min_count + max_extra) {
            result.push_back(c.idx);
        }
    }

    if (result.empty()) {
        for (const auto& c : candidates) {
            result.push_back(c.idx);
        }
    }
    return result;
}
