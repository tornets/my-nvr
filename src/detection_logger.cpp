//
// Created by Claude on 2026/5/28.
// 检测结果持久化日志器实现
//

#include "detection_logger.h"
#include "log.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

DetectionLogger::DetectionLogger(const std::string& output_dir) {
    // 构造函数暂时为空，output_dir 主要用于日志记录
    LOG_DEBUG("DetectionLogger initialized");
}

DetectionLogger::~DetectionLogger() {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 关闭所有打开的文件
    for (auto& [path, writer] : m_writers) {
        if (writer.is_open()) {
            writer.close();
        }
    }
    m_writers.clear();
    m_headers_written.clear();

    LOG_DEBUG("DetectionLogger destroyed");
}

std::string DetectionLogger::createLogFile(const fs::path& video_file_path) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 创建 CSV 文件路径（与视频文件同目录，同名但 .csv 扩展名）
    fs::path csv_file_path = video_file_path;
    csv_file_path.replace_extension(".csv");

    // 确保父目录存在
    ensureParentDirectory(csv_file_path);

    // 检查是否已经存在该日志文件的写入器
    std::string csv_path_str = csv_file_path.string();
    if (m_writers.find(csv_path_str) != m_writers.end()) {
        LOG_WARN("CSV log file already exists: {}", csv_path_str);
        return csv_path_str;
    }

    // 创建新的写入器
    m_writers[csv_path_str] = std::ofstream(csv_file_path, std::ios::out | std::ios::app);
    m_headers_written[csv_path_str] = false;

    if (!m_writers[csv_path_str].is_open()) {
        LOG_ERROR("Failed to create CSV log file: {}", csv_path_str);
        m_writers.erase(csv_path_str);
        m_headers_written.erase(csv_path_str);
        return "";
    }

    LOG_INFO("Created detection log file: {}", csv_path_str);
    return csv_path_str;
}

void DetectionLogger::log(const std::string& log_file_path,
                         int64_t frame_pts,
                         const std::chrono::system_clock::time_point& timestamp,
                         const nvr::detection::DetectionResult& result) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 查找日志文件写入器
    auto it = m_writers.find(log_file_path);
    if (it == m_writers.end() || !it->second.is_open()) {
        LOG_ERROR("CSV log file not opened: {}", log_file_path);
        return;
    }

    std::ofstream& writer = it->second;

    // 写入 CSV 头（如果还没写入）
    if (!m_headers_written[log_file_path]) {
        writer << "frame_pts,timestamp,has_player,player_count,npc_count,boxes_json\n";
        m_headers_written[log_file_path] = true;
    }

    // 格式化时间戳
    std::string timestamp_str = formatTimestamp(timestamp);

    // 统计玩家和 NPC 数量
    int player_count = 0;
    int npc_count = 0;
    for (const auto& box : result.boxes) {
        if (box.class_id == result.player_class_id) {
            player_count++;
        } else if (box.class_id == result.npc_class_id) {
            npc_count++;
        }
    }

    // 序列化边界框为 JSON
    std::string boxes_json = serializeBoxes(result.boxes);

    // 写入 CSV 行
    writer << frame_pts << ","
           << timestamp_str << ","
           << (result.has_player ? "true" : "false") << ","
           << player_count << ","
           << npc_count << ","
           << "\"" << boxes_json << "\"\n";

    writer.flush();  // 确保数据写入磁盘
}

void DetectionLogger::finalizeLogFile(const std::string& log_file_path) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_writers.find(log_file_path);
    if (it != m_writers.end() && it->second.is_open()) {
        it->second.close();
        LOG_DEBUG("Finalized detection log: {}", log_file_path);
    }
}

std::string DetectionLogger::serializeBoxes(const std::vector<nvr::detection::BoundingBox>& boxes) {
    if (boxes.empty()) {
        return "[]";
    }

    std::ostringstream json;
    json << "[";

    for (size_t i = 0; i < boxes.size(); ++i) {
        const auto& box = boxes[i];
        if (i > 0) {
            json << ",";
        }
        json << "{\"class_id\":" << box.class_id
             << ",\"confidence\":" << box.confidence
             << ",\"x\":" << box.x
             << ",\"y\":" << box.y
             << ",\"w\":" << box.width
             << ",\"h\":" << box.height << "}";
    }

    json << "]";
    return json.str();
}

std::string DetectionLogger::formatTimestamp(const std::chrono::system_clock::time_point& timestamp) {
    // 转换为 time_t
    std::time_t time = std::chrono::system_clock::to_time_t(timestamp);

    // 获取毫秒部分
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        timestamp.time_since_epoch()) % 1000;

    // 格式化为 ISO 8601
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&time), "%Y-%m-%dT%H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    ss << "Z";  // UTC 时区标记

    return ss.str();
}

void DetectionLogger::ensureParentDirectory(const fs::path& file_path) {
    fs::path parent_dir = file_path.parent_path();
    if (!parent_dir.empty() && !fs::exists(parent_dir)) {
        try {
            fs::create_directories(parent_dir);
            LOG_DEBUG("Created directory: {}", parent_dir.string());
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to create directory {}: {}", parent_dir.string(), e.what());
        }
    }
}
