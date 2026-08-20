//
// Created by Claude on 2026/5/28.
// 检测结果持久化日志器
//

#ifndef NVR_DETECTION_LOGGER_H
#define NVR_DETECTION_LOGGER_H

#include <string>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <fstream>
#include <chrono>
#include "detection_types.h"

namespace fs = std::filesystem;

// 检测结果日志器 - 将检测结果记录为 CSV 文件
class DetectionLogger {
public:
    // 构造函数：指定输出目录
    explicit DetectionLogger(const std::string& output_dir);

    // 析构函数：关闭所有打开的文件
    ~DetectionLogger();

    // 为新视频文件创建对应的日志文件
    // 返回日志文件路径（与视频文件在同一目录，.csv 扩展名）
    // 参数：video_file_path - 视频文件的完整路径
    std::string createLogFile(const fs::path& video_file_path);

    // 记录检测结果（线程安全）
    // 参数：
    //   log_file_path - 日志文件路径
    //   ss_seconds - 帧相对原始分片开始的相对时间（秒）
    //   result - 检测结果
    void log(const std::string& log_file_path,
             double ss_seconds,
             const nvr::detection::DetectionResult& result);

    // 完成日志文件写入
    void finalizeLogFile(const std::string& log_file_path);

private:
    // 将边界框序列化为 JSON 字符串（坐标归一化到原始帧 [0,1]）
    std::string serializeBoxes(const nvr::detection::DetectionResult& result);

    // 格式化相对时间为 HH:MM:SS.mmm 字符串
    std::string formatRelativeTime(double seconds);

    // 确保日志文件的父目录存在
    void ensureParentDirectory(const fs::path& file_path);

    std::mutex m_mutex;                                          // 线程安全
    std::unordered_map<std::string, std::ofstream> m_writers;   // 每个日志文件路径一个写入器
    std::unordered_map<std::string, bool> m_headers_written;   // 跟踪是否已写入 CSV 头
};

#endif // NVR_DETECTION_LOGGER_H
