#ifndef __CHAT_LOGGER_H__
#define __CHAT_LOGGER_H__

#include <string>

/**
 * 对话记录 / 学习打卡记录（保存在 SD 卡）
 *
 * - 对话记录：/sdcard/chatlog/YYYYMMDD.txt
 *     [14:30:05] user: 小智，给我一条今日名言
 *     [14:30:09] assistant: —— 资料库词条 [YW-MR-001] —— ...
 * - 打卡记录：/sdcard/study/checkin_YYYYMMDD.txt（CSV 格式，便于后续统计）
 *     14:30:25,语文,背古诗《宿建德江》,完成
 *
 * SD 卡不存在或读写异常时自动静默跳过（不影响对话功能）。
 */
class ChatLogger {
public:
    static ChatLogger& GetInstance();

    // 记录一条对话。role 为 "user" / "assistant" / "system"。
    // 内部只记录 user 和 assistant，空内容忽略。
    void Log(const char* role, const char* content);

    // 记录一次学习打卡。subject=科目，item=学习内容，result=结果（完成/正确/错误等）
    void LogCheckin(const std::string& subject, const std::string& item,
                    const std::string& result);

    // 记录错误题（用于错题巩固）：/sdcard/study/mistakes.txt
    void LogMistake(const std::string& entry_id, const std::string& description);

    // 最近 n 天的打卡统计摘要（供 MCP 工具返回给 LLM 做学习规划）
    std::string GetCheckinSummary(int days = 7);

    bool IsEnabled();

private:
    ChatLogger() = default;

    std::string TodayLogPath();
    std::string Timestamp();       // "[HH:MM:SS]"，时间未同步时用开机时长
    bool TimeValid();              // SNTP 是否已同步
};

#endif  // __CHAT_LOGGER_H__
