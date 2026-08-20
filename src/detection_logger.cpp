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
                         double ss_seconds,
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
        writer << "ss,has_player,player_count,npc_count,boxes_json\n";
        m_headers_written[log_file_path] = true;
    }

    // 格式化相对时间（相对原始分片开始）
    std::string ss_str = formatRelativeTime(ss_seconds);

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

    // 序列化边界框为 JSON（归一化坐标）
    std::string boxes_json = serializeBoxes(result);

    // 写入 CSV 行
    writer << ss_str << ","
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

    // 回收 map 条目，防止长期运行时 fd 与内存无限增长
    m_writers.erase(log_file_path);
    m_headers_written.erase(log_file_path);
}

std::string DetectionLogger::serializeBoxes(const nvr::detection::DetectionResult& result) {
    const auto& boxes = result.boxes;
    if (boxes.empty()) {
        return "[]";
    }

    // 归一化：原始帧像素坐标 → [0,1]（frame 尺寸未知时退回像素值）
    bool normalized = (result.frame_width > 0 && result.frame_height > 0);
    float inv_w = normalized ? 1.0f / result.frame_width : 1.0f;
    float inv_h = normalized ? 1.0f / result.frame_height : 1.0f;

    std::ostringstream json;
    json << "[";
    json << std::fixed << std::setprecision(6);

    for (size_t i = 0; i < boxes.size(); ++i) {
        const auto& box = boxes[i];
        if (i > 0) {
            json << ",";
        }
        json << "{\"class_id\":" << box.class_id
             << ",\"confidence\":" << box.confidence
             << ",\"x\":" << box.x * inv_w
             << ",\"y\":" << box.y * inv_h
             << ",\"w\":" << box.width * inv_w
             << ",\"h\":" << box.height * inv_h << "}";
    }

    json << "]";
    return json.str();
}

std::string DetectionLogger::formatRelativeTime(double seconds) {
    // 相对时间格式化为 HH:MM:SS.mmm
    if (seconds < 0) {
        seconds = 0;
    }
    int64_t total_ms = static_cast<int64_t>(seconds * 1000.0 + 0.5);
    int ms = static_cast<int>(total_ms % 1000);
    int64_t total_sec = total_ms / 1000;
    int s = static_cast<int>(total_sec % 60);
    int m = static_cast<int>((total_sec / 60) % 60);
    int h = static_cast<int>(total_sec / 3600);

    std::ostringstream ss;
    ss << std::setfill('0')
       << std::setw(2) << h << ":"
       << std::setw(2) << m << ":"
       << std::setw(2) << s << "."
       << std::setw(3) << ms;
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
