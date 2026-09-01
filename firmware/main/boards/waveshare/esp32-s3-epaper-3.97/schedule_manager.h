#ifndef __SCHEDULE_MANAGER_H__
#define __SCHEDULE_MANAGER_H__

#include <string>
#include <vector>
#include "config.h"

struct ScheduleItem {
    int weekday;
    int period;
    std::string subject;
    std::string note;
};

class ScheduleManager {
public:
    static ScheduleManager& GetInstance();

    void Load();
    std::vector<ScheduleItem> GetDaySchedule(int weekday);
    std::string GetDayScheduleText(int weekday);
    std::string GetTodayScheduleText();
    std::string GetTomorrowPreview();
    std::string GetRecent3DaysText();
    static std::string WeekdayName(int weekday);
    int TodayWeekday();
    bool IsLoaded() const { return loaded_; }
    std::string GetErrorReason() const { return error_reason_; }

private:
    ScheduleManager() = default;
    std::vector<ScheduleItem> items_;
    bool loaded_ = false;
    std::string error_reason_;
    int loaded_weekday_ = 0;
    std::string DataPath() { return std::string(SD_MOUNT_POINT) + "/schedule/schedule.txt"; }
};

#endif
