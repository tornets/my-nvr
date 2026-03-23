#include "schedule_utils.h"

bool TimeRange::contains(int hour, int minute) const {
    int current_mins = hour * 60 + minute;
    int start_mins = start_hour * 60 + start_minute;
    int end_mins = end_hour * 60 + end_minute;

    if (start_mins <= end_mins) {
        // 不跨午夜：如 08:00-10:00
        // 注意：结束时间不包含在内（半开区间）
        return current_mins >= start_mins && current_mins < end_mins;
    } else {
        // 跨午夜：如 22:00-02:00
        // current >= 22:00 OR current < 02:00
        return current_mins >= start_mins || current_mins < end_mins;
    }
}

bool ScheduleUtils::isInRecordingTime(const std::vector<TimeRange>& ranges, const std::tm& time) {
    for (const auto& range : ranges) {
        if (range.contains(time.tm_hour, time.tm_min)) {
            return true;
        }
    }
    return false;
}

bool ScheduleUtils::parseTime(const std::string& time_str, int& hour, int& minute) {
    size_t colon_pos = time_str.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }
    try {
        hour = std::stoi(time_str.substr(0, colon_pos));
        minute = std::stoi(time_str.substr(colon_pos + 1));
        return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
    } catch (...) {
        return false;
    }
}