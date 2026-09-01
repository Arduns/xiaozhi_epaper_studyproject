#include "study_manager.h"

#include <algorithm>
#include <esp_log.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "sd_card_manager.h"

#define TAG "StudyManager"

#define MISTAKE_WEIGHT_INIT 5
#define MISTAKE_WEIGHT_MAX 10
#define MISTAKE_WRONG_PLUS 2
#define MISTAKE_CORRECT_MINUS 2
#define MISTAKE_REMOVE_STREAK 3

StudyManager& StudyManager::GetInstance() {
    static StudyManager instance;
    return instance;
}

std::string StudyManager::ConfigPath() { return std::string(SD_STUDY_DIR) + "/plan_config.txt"; }

std::string StudyManager::MistakeBookPath() {
    return std::string(SD_STUDY_DIR) + "/mistakebook.txt";
}

std::string StudyManager::NormalizeSubject(const std::string& subject) {
    if (subject == "语文" || subject == "chinese" || subject == "语") return "chinese";
    if (subject == "数学" || subject == "math" || subject == "数") return "math";
    if (subject == "英语" || subject == "english" || subject == "英") return "english";
    if (subject == "历史" || subject == "history" || subject == "史") return "history";
    if (subject == "科学" || subject == "science" || subject == "科") return "science";
    if (subject == "全部" || subject == "all" || subject == "统一" || subject == "所有科目") {
        return "all";
    }
    return "";
}

/* ==================== 学习计划配置 ==================== */

void StudyManager::LoadConfig() {
    config_loaded_ = true;
    auto& sd = SdCardManager::GetInstance();
    std::string text;
    if (!sd.ReadFile(ConfigPath(), text, 4096)) {
        // 配置文件不存在：写入默认配置（每天每科 3 条）
        if (sd.IsReady()) {
            SaveConfig();
        }
        return;
    }
    // 解析 key=value，跳过 # 注释行
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos
                                                                     : end - pos);
        pos = (end == std::string::npos) ? text.size() : end + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        int value = atoi(line.c_str() + eq + 1);
        if (value < 1 || value > 9) continue;
        std::string subj = NormalizeSubject(key);
        if (subj == "all") count_all_ = value;
        else if (subj == "chinese") count_chinese_ = value;
        else if (subj == "math") count_math_ = value;
        else if (subj == "english") count_english_ = value;
        else if (subj == "history") count_history_ = value;
        else if (subj == "science") count_science_ = value;
    }
}

bool StudyManager::SaveConfig() {
    std::string text =
        "# 小智学习计划配置（可手动编辑本文件，也可对小智说：把数学改成每天5条）\n"
        "# 每科每天学习内容条数（1-9）；all 为统一配置，单科配置优先于 all\n"
        "all=" + std::to_string(count_all_) + "\n"
        "chinese=" + std::to_string(count_chinese_) + "\n"
        "math=" + std::to_string(count_math_) + "\n"
        "english=" + std::to_string(count_english_) + "\n"
        "history=" + std::to_string(count_history_) + "\n"
        "science=" + std::to_string(count_science_) + "\n";
    return SdCardManager::GetInstance().WriteFile(ConfigPath(), text);
}

int StudyManager::GetDailyCount(const std::string& subject) {
    if (!config_loaded_) LoadConfig();
    std::string subj = NormalizeSubject(subject);
    // 单科配置优先；单科等于默认值且 all 不同（说明用户只改了统一配置）时用 all
    if (subj == "chinese") return count_chinese_;
    if (subj == "math") return count_math_;
    if (subj == "english") return count_english_;
    if (subj == "history") return count_history_;
    if (subj == "science") return count_science_;
    return count_all_;
}

bool StudyManager::SetDailyCount(const std::string& subject, int count) {
    if (!config_loaded_) LoadConfig();
    if (count < 1 || count > 9) return false;
    std::string subj = NormalizeSubject(subject);
    if (subj.empty()) return false;
    if (subj == "all") {
        count_all_ = count;
        count_chinese_ = count;
        count_math_ = count;
        count_english_ = count;
        count_history_ = count;
        count_science_ = count;
    } else if (subj == "chinese") {
        count_chinese_ = count;
    } else if (subj == "math") {
        count_math_ = count;
    } else if (subj == "english") {
        count_english_ = count;
    } else if (subj == "history") {
        count_history_ = count;
    } else if (subj == "science") {
        count_science_ = count;
    }
    bool ok = SaveConfig();
    ESP_LOGI(TAG, "plan updated: %s=%d (%s)", subj.c_str(), count, ok ? "saved" : "save failed");
    return ok;
}

std::string StudyManager::GetPlanText() {
    if (!config_loaded_) LoadConfig();
    return "当前学习计划（每天）：语文 " + std::to_string(count_chinese_) + " 条，数学 " +
           std::to_string(count_math_) + " 条，英语 " + std::to_string(count_english_) +
           " 条，历史 " + std::to_string(count_history_) + " 条，科学 " +
           std::to_string(count_science_) +
           " 条。可通过修改 SD 卡 /study/plan_config.txt 或语音调整。";
}

/* ==================== 错题本（权重评分） ==================== */

std::vector<StudyManager::MistakeItem> StudyManager::LoadMistakeBook() {
    std::vector<MistakeItem> items;
    std::string text;
    if (!SdCardManager::GetInstance().ReadFile(MistakeBookPath(), text, 64 * 1024)) {
        return items;
    }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string line = text.substr(pos, end == std::string::npos ? std::string::npos
                                                                     : end - pos);
        pos = (end == std::string::npos) ? text.size() : end + 1;
        if (line.empty()) continue;
        // 格式：键|权重|累计答错|连续答对|描述
        MistakeItem it;
        size_t p1 = line.find('|');
        size_t p2 = p1 == std::string::npos ? p1 : line.find('|', p1 + 1);
        size_t p3 = p2 == std::string::npos ? p2 : line.find('|', p2 + 1);
        size_t p4 = p3 == std::string::npos ? p3 : line.find('|', p3 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos) {
            continue;
        }
        it.key = line.substr(0, p1);
        it.weight = atoi(line.c_str() + p1 + 1);
        it.wrong_count = atoi(line.c_str() + p2 + 1);
        it.correct_streak = atoi(line.c_str() + p3 + 1);
        it.desc = p4 == std::string::npos ? "" : line.substr(p4 + 1);
        if (!it.key.empty()) items.push_back(std::move(it));
    }
    return items;
}

bool StudyManager::SaveMistakeBook(const std::vector<MistakeItem>& items) {
    std::string text;
    for (const auto& it : items) {
        text += it.key + "|" + std::to_string(it.weight) + "|" +
                std::to_string(it.wrong_count) + "|" + std::to_string(it.correct_streak) +
                "|" + it.desc + "\n";
    }
    return SdCardManager::GetInstance().WriteFile(MistakeBookPath(), text);
}

void StudyManager::AddMistake(const std::string& entry_id, const std::string& description) {
    if (!SdCardManager::GetInstance().IsReady()) return;
    std::string key = entry_id.empty() ? description : entry_id;
    if (key.empty()) return;
    auto items = LoadMistakeBook();
    bool found = false;
    for (auto& it : items) {
        if (it.key == key) {
            it.weight = std::min(MISTAKE_WEIGHT_MAX, it.weight + MISTAKE_WRONG_PLUS);
            it.wrong_count++;
            it.correct_streak = 0;
            if (!description.empty()) it.desc = description;
            found = true;
            break;
        }
    }
    if (!found) {
        MistakeItem it;
        it.key = key;
        it.weight = MISTAKE_WEIGHT_INIT;
        it.wrong_count = 1;
        it.correct_streak = 0;
        it.desc = description;
        items.push_back(std::move(it));
    }
    SaveMistakeBook(items);
    ESP_LOGI(TAG, "mistake added/updated: %s", key.c_str());
}

int StudyManager::MarkCorrect(const std::string& entry_id) {
    if (!SdCardManager::GetInstance().IsReady()) return -1;
    auto items = LoadMistakeBook();
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].key == entry_id) {
            items[i].weight -= MISTAKE_CORRECT_MINUS;
            items[i].correct_streak++;
            if (items[i].correct_streak >= MISTAKE_REMOVE_STREAK || items[i].weight <= 0) {
                ESP_LOGI(TAG, "mistake removed (mastered): %s", entry_id.c_str());
                items.erase(items.begin() + i);
                SaveMistakeBook(items);
                return 0;  // 已移出错题本
            }
            SaveMistakeBook(items);
            return 1;  // 仍在错题本
        }
    }
    return -1;  // 未找到
}

std::string StudyManager::GetMistakeBookText(int max_items) {
    auto items = LoadMistakeBook();
    if (items.empty()) {
        return "错题本是空的，很棒！";
    }
    // 按权重从高到低排序
    std::sort(items.begin(), items.end(),
              [](const MistakeItem& a, const MistakeItem& b) { return a.weight > b.weight; });
    std::string out = "错题本（按复习优先级排序，共 " + std::to_string(items.size()) + " 条）：\n";
    int n = std::min(max_items, (int)items.size());
    for (int i = 0; i < n; i++) {
        const auto& it = items[i];
        out += std::to_string(i + 1) + ". [" + it.key + "] 权重" + std::to_string(it.weight) +
               "，错" + std::to_string(it.wrong_count) + "次：" + it.desc + "\n";
    }
    return out;
}

int StudyManager::GetMistakeCount() { return (int)LoadMistakeBook().size(); }

/* ==================== 打卡明细回顾 ==================== */

std::string StudyManager::GetCheckinDetail(int days) {
    auto& sd = SdCardManager::GetInstance();
    if (!sd.IsReady()) {
        return "";
    }
    std::string out;
    time_t now = time(nullptr);
    if (now < 1704067200) {
        return "";
    }
    for (int d = 0; d < days; d++) {
        time_t t = now - d * 86400;
        struct tm tm_info;
        localtime_r(&t, &tm_info);
        char path[96];
        snprintf(path, sizeof(path), "%s/checkin_%04d%02d%02d.txt", SD_STUDY_DIR,
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);
        std::string content;
        if (sd.ReadFile(path, content, 32 * 1024) && !content.empty()) {
            char date[40];
            snprintf(date, sizeof(date), "【%04d-%02d-%02d】\n", tm_info.tm_year + 1900,
                     tm_info.tm_mon + 1, tm_info.tm_mday);
            out += date;
            out += content;
        }
    }
    return out;
}
