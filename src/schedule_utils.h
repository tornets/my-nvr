#pragma once

#include <ctime>
#include <string>
#include <vector>
#include "config_loader.h"

class ScheduleUtils {
public:
    // 检查当前时间是否在任意一个录制时间段内
    static bool isInRecordingTime(const std::vector<TimeRange>& ranges, const std::tm& time);

    // 解析时间字符串 "HH:MM" 为小时和分钟
    static bool parseTime(const std::string& time_str, int& hour, int& minute);
};