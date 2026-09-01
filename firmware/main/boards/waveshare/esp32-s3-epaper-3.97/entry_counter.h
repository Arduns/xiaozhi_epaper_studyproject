#ifndef __ENTRY_COUNTER_H__
#define __ENTRY_COUNTER_H__

#include <map>
#include <string>
#include <vector>

/**
 * 词条出现次数计数器
 *
 * 记录每个词条被推送/出题的次数，用于减少重复率。
 * 数据持久化在 SD 卡 /sdcard/study/entry_counts.txt：
 *   词条编号|出现次数|上次出现日期(YYYYMMDD)
 *
 * 使用方式：
 *   1. 开机时 Load()（可选，首次 GetCount/Record 会自动加载）
 *   2. 出题前用 GetCount() 或 GetLowCountCandidates() 筛选
 *   3. 出题后调用 Record() 增加次数
 */
class EntryCounter {
public:
    static EntryCounter& GetInstance();

    // 从 SD 卡加载计数数据（首次调用自动加载）
    void Load();

    // 保存计数数据到 SD 卡
    void Save();

    // 获取某词条当前出现次数（未记录过返回 0）
    int GetCount(const std::string& entry_id);

    // 记录一次出现（次数+1，更新日期），并自动保存
    void Record(const std::string& entry_id);

    // 从给定索引列表中，筛选出现次数较少的候选（用于加权随机）
    // 返回出现次数 <= min_count + max_extra 的索引列表
    // 如果全部次数相同，返回全部
    std::vector<int> GetLowCountCandidates(const std::vector<int>& indices,
                                            int max_extra = 2);

    // 获取当前记录的总条目数
    int GetTotalTracked() const { return (int)counts_.size(); }

private:
    EntryCounter() = default;

    struct CountInfo {
        int count = 0;
        int last_date = 0;  // YYYYMMDD
    };

    std::map<std::string, CountInfo> counts_;
    bool loaded_ = false;

    std::string DataPath();
    int TodayDate();  // 返回 YYYYMMDD
};

#endif  // __ENTRY_COUNTER_H__
