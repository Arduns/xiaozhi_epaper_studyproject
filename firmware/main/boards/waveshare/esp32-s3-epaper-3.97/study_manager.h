#ifndef __STUDY_MANAGER_H__
#define __STUDY_MANAGER_H__

#include <string>
#include <vector>

/**
 * 学习管理器：学习计划配置 + 错题本（权重评分机制）
 *
 * 【学习计划配置】/sdcard/study/plan_config.txt（UTF-8，可手动编辑，也可语音修改）：
 *   # 每科每天学习内容条数；all 为统一配置，单科配置优先于 all
 *   all=3
 *   chinese=3
 *   math=3
 *   english=3
 *   history=1    （2026-08-28 起支持：历史）
 *   science=1    （2026-08-28 起支持：科学）
 *
 * 【错题本】/sdcard/study/mistakebook.txt，每行一条：
 *   词条编号或内容|权重|累计答错次数|连续答对次数|最近描述
 * 权重规则：
 *   - 新增错题初始权重 5；再次答错 +2（上限 10）
 *   - 答对一次 -2，连续答对清零答错节奏；连续答对 3 次或权重降到 0 及以下时移出错题本
 *   - 权重越高，复习时越优先
 * 原始错题流水仍追加保存在 /sdcard/study/mistakes.txt（不删除，供回溯）。
 */
class StudyManager {
public:
    static StudyManager& GetInstance();

    /* ---------- 学习计划配置 ---------- */
    // 获取某科每天学习条数（subject: chinese/math/english/history/science 或 语文/数学/英语/历史/科学）
    int GetDailyCount(const std::string& subject);
    // 设置某科（或 all 统一）每天条数并保存到 SD 卡。返回是否成功。
    bool SetDailyCount(const std::string& subject, int count);
    // 当前配置的可读文本（给 MCP 工具返回）
    std::string GetPlanText();

    /* ---------- 错题本 ---------- */
    // 记录答错：新增或加重权重。entry_id 可空（用 description 作键）。
    void AddMistake(const std::string& entry_id, const std::string& description);
    // 记录答对：降权重，连续答对 3 次或权重<=0 时移除。
    // 返回 1=仍在错题本，0=已掌握移除，-1=错题本中未找到该题
    int MarkCorrect(const std::string& entry_id);
    // 错题本内容（按权重从高到低），供复习优先安排
    std::string GetMistakeBookText(int max_items = 20);
    int GetMistakeCount();

    /* ---------- 打卡回顾 ---------- */
    // 最近 days 天打卡记录明细（原文），区别于 ChatLogger 的条数统计
    std::string GetCheckinDetail(int days = 3);

private:
    StudyManager() = default;

    struct MistakeItem {
        std::string key;          // 词条编号或内容键
        int weight = 5;           // 权重 1-10
        int wrong_count = 1;      // 累计答错
        int correct_streak = 0;   // 连续答对
        std::string desc;         // 最近描述
    };

    std::string ConfigPath();
    std::string MistakeBookPath();
    void LoadConfig();
    bool SaveConfig();
    std::vector<MistakeItem> LoadMistakeBook();
    bool SaveMistakeBook(const std::vector<MistakeItem>& items);
    static std::string NormalizeSubject(const std::string& subject);

    bool config_loaded_ = false;
    int count_all_ = 3;
    int count_chinese_ = 3;
    int count_math_ = 3;
    int count_english_ = 3;
    int count_history_ = 1;   // 历史（拓展科目，默认每天 1 条）
    int count_science_ = 1;   // 科学（拓展科目，默认每天 1 条）
};

#endif  // __STUDY_MANAGER_H__
