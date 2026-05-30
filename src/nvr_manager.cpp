//
// Created by wention on 2026/1/27.
//

#include "log.h"
#include "nvr_manager.h"
#include "schedule_utils.h"
#include "video_segment_extractor.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <regex>
#include <unordered_set>
#include <optional>

namespace fs = std::filesystem;

// 前向声明辅助函数
std::optional<std::chrono::system_clock::time_point>
parseRecordingTimeFromFilename(const fs::path& file_path);

bool compareByRecordingTime(const fs::path& a, const fs::path& b);

NVRManager::NVRManager(const Config& config)
    : m_config(config)
    , m_running(false)
{
    fs::create_directories(m_config.record.output_dir);
    fs::create_directories(m_config.record.temp_dir);
    LOG_INFO("NVRManager initialized with output_dir: {}", m_config.record.output_dir);
    LOG_INFO("NVRManager initialized with temp_dir: {}", m_config.record.temp_dir);

    // 清理可能残留的临时文件（例如程序异常退出后）
    cleanTempFiles();

#ifdef ENABLE_RKNN_SMART_RECORDING
    // 创建 NPU 推理池（在添加流之前）
    for (const auto& stream : m_config.streams) {
        if (stream.smart_recording.enabled && stream.smart_recording.rknn.enabled) {
            nvr::detection::DetectionConfig det_config;
            det_config.model_path = stream.smart_recording.rknn.model_path;
            det_config.player_class_id = stream.smart_recording.rknn.player_class_id;
            det_config.npc_class_id = stream.smart_recording.rknn.npc_class_id;
            det_config.confidence_threshold = stream.smart_recording.rknn.confidence_threshold;
            det_config.detection_interval_keyframes = stream.smart_recording.rknn.detection_interval_keyframes;
            det_config.zero_copy_enabled = stream.smart_recording.rknn.zero_copy_enabled;
            det_config.dump_detect = stream.smart_recording.dump_detect;

            m_detection_pool = std::make_unique<nvr::detection::DetectionPool>(
                m_config.detection_pool.workers, det_config, m_config.detection_pool.queue_size);
            if (m_detection_pool->initialize()) {
                LOG_INFO("Detection pool initialized with {} workers", m_detection_pool->getNumWorkers());
            } else {
                LOG_ERROR("Failed to initialize detection pool");
                m_detection_pool.reset();
            }
            break;  // 使用第一个启用的流的配置创建池
        }
    }
#endif

    // 启动后台服务线程
    m_running = true;

    // 如果启用了自动清理，启动清理线程
    if (m_config.autoclean.enabled) {
        m_cleanup_thread = std::thread(&NVRManager::cleanupLoop, this);
        LOG_INFO("Cleanup thread started");
    }

    // 如果启用了事件提取，启动提取线程
    if (m_config.record.enable_extraction) {
        m_extraction_thread = std::thread(&NVRManager::extractionLoop, this);
        LOG_INFO("Event extraction thread started");
    }

    // 如果启用了调度，启动调度线程
    if (m_config.record.schedule.enabled) {
        LOG_INFO("Recording schedule enabled with {} time ranges",
                 m_config.record.schedule.time_ranges.size());
        for (const auto& range : m_config.record.schedule.time_ranges) {
            LOG_INFO("  Schedule: {:02d}:{:02d} - {:02d}:{:02d}",
                     range.start_hour, range.start_minute,
                     range.end_hour, range.end_minute);
        }
        // 启动调度线程
        m_schedule_thread = std::thread(&NVRManager::scheduleLoop, this);
        LOG_INFO("Schedule thread started");
    }
}

NVRManager::~NVRManager() {
    stopAll();
}

bool NVRManager::addStream(const std::string& stream_id, const std::string& stream_url) {
    return addStreamWithConfig(stream_id, stream_url, true, 5, -1, 30, "");
}

bool NVRManager::addStreamWithConfig(const std::string& stream_id, const std::string& stream_url,
                                     bool auto_reconnect, int reconnect_interval_seconds,
                                     int max_reconnect_attempts, int timeout_seconds,
                                     const std::string& stream_name) {
    std::lock_guard<std::mutex> lock(m_recorders_mutex);

    if (m_recorders.find(stream_id) != m_recorders.end()) {
        LOG_WARN("Stream {} already exists", stream_id);
        return true;
    }

    // 查找流的配置（包括智能录制配置）
    const SmartRecordingConfig* smart_recording_config = nullptr;
    for (const auto& stream : m_config.streams) {
        if (stream.id == stream_id) {
            if (stream.smart_recording.enabled) {
                smart_recording_config = &stream.smart_recording;
                LOG_INFO("Smart recording enabled for stream: {}", stream_id);
            }
            break;
        }
    }

    auto recorder = std::make_unique<IPCRecorder>(stream_id, stream_url,
                                                      m_config.record.output_dir,
                                                      m_config.record.temp_dir,
                                                      m_config.record.segment_duration_seconds,
                                                      m_config.record.filename_template,
                                                      m_config.record.enable_audio,
                                                      auto_reconnect,
                                                      reconnect_interval_seconds,
                                                      max_reconnect_attempts,
                                                      timeout_seconds,
                                                      m_config.shop.id,
                                                      stream_name,
                                                      m_config.record.min_segment_duration_seconds
#ifdef ENABLE_RKNN_SMART_RECORDING
                                                      , smart_recording_config
                                                      , m_detection_pool.get()
#endif
    );

    // 检查是否应该启动录制
    bool should_start = true;
    if (m_config.record.schedule.enabled) {
        std::time_t now = std::time(nullptr);
        std::tm local_tm = *std::localtime(&now);
        should_start = ScheduleUtils::isInRecordingTime(
            m_config.record.schedule.time_ranges, local_tm);
        if (!should_start) {
            LOG_INFO("Schedule: Stream {} added but not started (outside recording time)", stream_id);
        }
    }

    if (should_start) {
        recorder->start();
    }

    m_recorders[stream_id] = std::move(recorder);
    LOG_INFO("Added stream: {} -> {} (name={}, auto_reconnect={}, interval={}s, max_attempts={}, timeout={}s)",
                   stream_id, stream_url, stream_name.empty() ? stream_id : stream_name,
                   auto_reconnect, reconnect_interval_seconds,
                   max_reconnect_attempts == -1 ? -1 : max_reconnect_attempts, timeout_seconds);

    return true;
}

bool NVRManager::removeStream(const std::string& stream_id) {
    std::lock_guard<std::mutex> lock(m_recorders_mutex);

    auto it = m_recorders.find(stream_id);
    if (it == m_recorders.end()) {
        LOG_WARN("Stream {} not found", stream_id);
        return false;
    }

    it->second->stop();
    m_recorders.erase(it);
    LOG_INFO("Removed stream: {}", stream_id);

    return true;
}

void NVRManager::stopAll() {
    std::lock_guard<std::mutex> lock(m_recorders_mutex);

    for (auto& pair : m_recorders) {
        pair.second->stop();
    }
    m_recorders.clear();

#ifdef ENABLE_RKNN_SMART_RECORDING
    if (m_detection_pool) {
        m_detection_pool->shutdown();
        m_detection_pool.reset();
    }
#endif

    if (m_running) {
        m_running = false;

        // 通知所有等待的线程
        m_cleanup_cv.notify_all();
        m_upload_cv.notify_all();
        m_schedule_cv.notify_all();
        m_extraction_cv.notify_all();

        // 只在线程实际启动时 join
        if (m_cleanup_thread.joinable()) {
            m_cleanup_thread.join();
        }
        if (m_upload_thread.joinable()) {
            m_upload_thread.join();
        }
        if (m_schedule_thread.joinable()) {
            m_schedule_thread.join();
        }
        if (m_extraction_thread.joinable()) {
            m_extraction_thread.join();
        }
    }

    // 停止上传器（会自动保存进度）
    if (m_uploader) {
        m_uploader->stop();
    }
}

void NVRManager::setUploader(std::shared_ptr<VideoUploader> uploader) {
    m_uploader = uploader;

    if (m_uploader && m_uploader->isEnabled()) {
        LOG_INFO("Video uploader configured");

        // 初始化上传进度管理器
        m_upload_progress = m_uploader->progress();

        // 启动上传扫描线程（如果还未启动）
        if (m_running && !m_upload_thread.joinable()) {
            m_upload_thread = std::thread(&NVRManager::uploadLoop, this);
            LOG_INFO("Upload scan thread started");
        }
    }
}

void NVRManager::cleanupLoop() {
    LOG_INFO("Cleanup loop started");

    while (m_running) {
        try {
            cleanOldFiles();
            checkDiskUsage();
        } catch (const std::exception& e) {
            LOG_ERROR("Cleanup error: {}", e.what());
        }

        std::unique_lock<std::mutex> lock(m_cleanup_mutex);
        // 等待指定时间或直到收到停止信号
        if (m_cleanup_cv.wait_for(lock, std::chrono::seconds(m_config.autoclean.check_interval_seconds),
                                 [this] { return !m_running; })) {
            break;  // 收到停止信号
        }
    }

    LOG_INFO("Cleanup loop stopped");
}

void NVRManager::uploadLoop() {
    LOG_INFO("Upload scan loop started");

    while (m_running) {
        try {
            scanAndUploadNewFiles();
        } catch (const std::exception& e) {
            LOG_ERROR("Upload scan error: {}", e.what());
        }

        std::unique_lock<std::mutex> lock(m_upload_mutex);
        // 每10秒扫描一次新文件
        if (m_upload_cv.wait_for(lock, std::chrono::seconds(10),
                                [this] { return !m_running; })) {
            break;  // 收到停止信号
        }
    }

    LOG_INFO("Upload scan loop stopped");
}

void NVRManager::scanAndUploadNewFiles() {
    if (!m_uploader || !m_uploader->isEnabled()) {
        return;
    }

    // 根据配置决定扫描哪个目录
    fs::path scan_dir = m_config.record.output_dir;

    if (m_config.upload.upload_subdir == "filter") {
        scan_dir = scan_dir / m_config.record.filter_subdir;
        LOG_DEBUG("Upload scan directory: filter ({} )", scan_dir.string());
    } else if (m_config.upload.upload_subdir == "raw") {
        scan_dir = scan_dir / m_config.record.raw_subdir;
        LOG_DEBUG("Upload scan directory: raw ({} )", scan_dir.string());
    } else if (m_config.upload.upload_subdir != "all") {
        LOG_WARN("Unknown upload_subdir: {}, using output_dir", m_config.upload.upload_subdir);
    }

    // 检查扫描目录是否存在
    if (!fs::exists(scan_dir)) {
        return;
    }

    // 获取临时目录的规范路径，用于比较
    fs::path temp_dir_path;
    try {
        temp_dir_path = fs::canonical(m_config.record.temp_dir);
    } catch (...) {
        temp_dir_path = m_config.record.temp_dir;
    }

    // 使用递归扫描，因为文件名模板可能包含多级目录
    for (const auto& entry : fs::recursive_directory_iterator(scan_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mp4") {
            continue;
        }

        // 检查文件是否在临时目录中
        fs::path file_path = entry.path();
        fs::path file_parent = file_path.parent_path();

        // 如果文件的父目录是临时目录（或临时目录的子目录），跳过
        try {
            fs::path file_parent_canonical = fs::canonical(file_parent);
            if (file_parent_canonical == temp_dir_path ||
                file_parent_canonical.string().find(temp_dir_path.string()) == 0) {
                LOG_DEBUG("Skipping temp file: {}", entry.path().string());
                continue;
            }
        } catch (...) {
            // 如果无法获取规范路径，使用字符串比较
            if (file_parent.string().find(m_config.record.temp_dir) != std::string::npos) {
                LOG_DEBUG("Skipping temp file (by path): {}", entry.path().string());
                continue;
            }
        }

        // 使用相对路径作为唯一标识（支持多级目录）
        std::string file_path_str = entry.path().string();
        std::string filename = entry.path().filename().string();

        // 使用进度管理器检查是否已上传
        if (m_upload_progress && m_upload_progress->getRecord(filename)) {
            //LOG_DEBUG("Skipping already scheduled file: {}", relative_key);
            continue;
        }

        // 获取第一个可用的流ID（所有流录制到同一目录）
        std::string stream_id = "default";
        {
            std::lock_guard<std::mutex> rec_lock(m_recorders_mutex);
            if (!m_recorders.empty()) {
                stream_id = m_recorders.begin()->first;
            }
        }

        // 获取文件修改时间作为录制时间
        auto ftime = entry.last_write_time();
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        std::time_t time = std::chrono::system_clock::to_time_t(sctp);

        // 格式化为 YYYY-MM-DD HH:MM:SS 格式（与进度文件格式一致）
        std::tm tm = *std::localtime(&time);
        std::stringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        std::string time_str = ss.str();

        // 创建上传任务
        UploadTask task;
        task.file_path = file_path_str;
        task.stream_id = stream_id;
        task.recording_time = time_str;

        m_uploader->upload(task);
    }
}

bool NVRManager::cleanOldFiles() {
    int max_age_seconds = m_config.autoclean.max_age_hours * 3600;
    auto now = std::chrono::system_clock::now();
    int deleted_count = 0;

    // 获取临时目录的规范路径，用于比较
    fs::path temp_dir_path;
    try {
        temp_dir_path = fs::canonical(m_config.record.temp_dir);
    } catch (...) {
        temp_dir_path = m_config.record.temp_dir;
    }

    // 收集文件并解析录制时间
    std::vector<std::pair<fs::path, std::chrono::system_clock::time_point>> files_with_time;

    for (const auto& entry : fs::recursive_directory_iterator(m_config.record.output_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        // 跳过临时目录中的文件
        fs::path file_path = entry.path();
        fs::path file_parent = file_path.parent_path();
        try {
            fs::path file_parent_canonical = fs::canonical(file_parent);
            if (file_parent_canonical == temp_dir_path ||
                file_parent_canonical.string().find(temp_dir_path.string()) == 0) {
                continue;  // 跳过临时文件
            }
        } catch (...) {
            if (file_parent.string().find(m_config.record.temp_dir) != std::string::npos) {
                continue;  // 跳过临时文件
            }
        }

        // 只处理 MP4 和 CSV 文件
        if (file_path.extension() != ".mp4" && file_path.extension() != ".csv") {
            continue;
        }

        // 尝试从文件名解析录制时间
        auto time_opt = parseRecordingTimeFromFilename(file_path);
        if (time_opt) {
            files_with_time.push_back(std::make_pair(file_path, *time_opt));
        }
    }

    // 按录制时间排序（时间早的在前）
    std::sort(files_with_time.begin(), files_with_time.end(),
             [](const auto& a, const auto& b) {
                 return a.second < b.second;  // 时间早的在前
             });

    // 删除最旧的文件，同时删除关联的 CSV 文件
    std::unordered_set<std::string> deleted_files;
    for (const auto& [file_path, recording_time] : files_with_time) {
        // 计算文件年龄
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - recording_time).count();

        if (age > max_age_seconds) {
            std::error_code ec;
            if (fs::remove(file_path, ec)) {
                deleted_files.insert(file_path.string());
                LOG_INFO("Deleted old file: {} (age: {}h)", file_path.string(), age / 3600.0);
                deleted_count++;

                // 删除关联的 CSV 文件
                fs::path csv_file = file_path;
                csv_file.replace_extension(".csv");
                if (fs::exists(csv_file) && fs::remove(csv_file, ec)) {
                    LOG_INFO("Deleted associated detection log: {}", csv_file.filename().string());
                }

                // 清除空目录
                auto parent_dir = file_path.parent_path();
                while (parent_dir != m_config.record.output_dir && fs::is_empty(parent_dir)) {
                    if (!fs::remove(parent_dir, ec)) {
                        LOG_ERROR("Failed to delete empty directory {}: {}", parent_dir.string(), ec.message());
                        break;
                    }
                    parent_dir = parent_dir.parent_path();
                }
            } else {
                LOG_ERROR("Failed to delete {}: {}", file_path.string(), ec.message());
            }
        }
    }

    if (deleted_count > 0) {
        LOG_INFO("Cleaned up {} old files", deleted_count);
    }

    return true;
}

bool NVRManager::checkDiskUsage() {
    if (m_config.autoclean.max_disk_usage_gb <= 0) {
        return true;
    }

    std::uintmax_t total_size = 0;

    // 获取临时目录的规范路径，用于排除
    fs::path temp_dir_path;
    try {
        temp_dir_path = fs::canonical(m_config.record.temp_dir);
    } catch (...) {
        temp_dir_path = m_config.record.temp_dir;
    }

    try {
        // 递归遍历，但排除临时目录
        for (const auto& entry : fs::recursive_directory_iterator(m_config.record.output_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            // 跳过临时目录中的文件
            fs::path file_path = entry.path();
            fs::path file_parent = file_path.parent_path();
            try {
                fs::path file_parent_canonical = fs::canonical(file_parent);
                if (file_parent_canonical == temp_dir_path ||
                    file_parent_canonical.string().find(temp_dir_path.string()) == 0) {
                    continue;  // 跳过临时文件
                }
            } catch (...) {
                if (file_parent.string().find(m_config.record.temp_dir) != std::string::npos) {
                    continue;  // 跳过临时文件
                }
            }

            total_size += entry.file_size();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Error calculating disk usage: {}", e.what());
        return false;
    }

    double total_gb = static_cast<double>(total_size) / (1024.0 * 1024.0 * 1024.0);

    if (total_gb > m_config.autoclean.max_disk_usage_gb) {
        LOG_WARN("Disk usage {:.2f} GB exceeds limit {} GB", total_gb, m_config.autoclean.max_disk_usage_gb);

        std::vector<fs::path> files;
        // 递归遍历收集所有 MP4 文件（与计算总大小时保持一致）
        for (const auto& entry : fs::recursive_directory_iterator(m_config.record.output_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
                // 跳过临时目录中的文件
                fs::path file_parent = entry.path().parent_path();
                try {
                    fs::path file_parent_canonical = fs::canonical(file_parent);
                    if (file_parent_canonical == temp_dir_path ||
                        file_parent_canonical.string().find(temp_dir_path.string()) == 0) {
                        continue;
                    }
                } catch (...) {
                    if (file_parent.string().find(m_config.record.temp_dir) != std::string::npos) {
                        continue;
                    }
                }

                files.push_back(entry.path());
            }
        }

        std::sort(files.begin(), files.end(),
                 [](const fs::path& a, const fs::path& b) {
                     return fs::last_write_time(a) < fs::last_write_time(b);
                 });

        int deleted = 0;
        while (total_gb > m_config.autoclean.max_disk_usage_gb * 0.9 && !files.empty()) {
            std::error_code ec;
            auto file_size = fs::file_size(files.front());
            if (fs::remove(files.front(), ec)) {
                total_size -= file_size;
                total_gb = static_cast<double>(total_size) / (1024.0 * 1024.0 * 1024.0);
                LOG_INFO("Deleted file to free space: {}", files.front().filename().string());
                deleted++;
            }
            files.erase(files.begin());
        }

        LOG_INFO("Deleted {} files to reduce disk usage", deleted);
    }

    return true;
}

bool NVRManager::cleanTempFiles() {
    // 清理临时目录中可能残留的文件
    int deleted_count = 0;

    try {
        if (!fs::exists(m_config.record.temp_dir)) {
            return true;
        }

        for (const auto& entry : fs::directory_iterator(m_config.record.temp_dir)) {
            if (entry.is_regular_file()) {
                std::error_code ec;
                if (fs::remove(entry.path(), ec)) {
                    LOG_INFO("Cleaned up temp file: {}", entry.path().filename().string());
                    deleted_count++;
                } else {
                    LOG_WARN("Failed to remove temp file {}: {}", entry.path().string(), ec.message());
                }
            }
        }

        if (deleted_count > 0) {
            LOG_INFO("Cleaned up {} residual temp file(s) from previous session", deleted_count);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Error cleaning temp directory: {}", e.what());
        return false;
    }

    return true;
}

// ==================== 调度相关方法 ====================

void NVRManager::scheduleLoop() {
    LOG_INFO("Schedule loop started");

    while (m_running) {
        checkAndUpdateSchedule();

        // 计算下次检查时间
        int sleep_seconds = m_config.record.schedule.check_interval_seconds;
        if (sleep_seconds < 10) sleep_seconds = 10;    // 最小10秒
        if (sleep_seconds > 300) sleep_seconds = 300;  // 最大5分钟

        std::unique_lock<std::mutex> lock(m_schedule_mutex);
        if (m_schedule_cv.wait_for(lock, std::chrono::seconds(sleep_seconds),
                                   [this] { return !m_running; })) {
            break;  // 收到停止信号
        }
    }

    LOG_INFO("Schedule loop stopped");
}

void NVRManager::checkAndUpdateSchedule() {
    // 获取当前时间
    std::time_t now = std::time(nullptr);
    std::tm local_tm = *std::localtime(&now);

    // 检查当前是否应该在录制时间段内
    bool should_record = ScheduleUtils::isInRecordingTime(
        m_config.record.schedule.time_ranges, local_tm);

    LOG_DEBUG("Schedule check: current time {:02d}:{:02d}, should_record={}",
              local_tm.tm_hour, local_tm.tm_min, should_record);

    // 遍历所有配置的流
    for (const auto& stream : m_config.streams) {
        const std::string& stream_id = stream.id;

        // 获取当前录制状态
        bool is_recording = false;
        {
            std::lock_guard<std::mutex> rec_lock(m_recorders_mutex);
            auto it = m_recorders.find(stream_id);
            if (it != m_recorders.end() && it->second) {
                is_recording = it->second->getStatus().is_recording;
            }
        }

        if (should_record && !is_recording) {
            // 进入录制时间段，启动录制
            LOG_INFO("Schedule: Starting recording for stream {}", stream_id);
            startStreamRecording(stream_id);
        } else if (!should_record && is_recording) {
            // 离开录制时间段，停止录制
            LOG_INFO("Schedule: Stopping recording for stream {}", stream_id);
            stopStreamRecording(stream_id);
        }
    }
}

void NVRManager::startStreamRecording(const std::string& stream_id) {
    // 查找流配置
    StreamConfig stream_config;
    bool found = false;
    for (const auto& stream : m_config.streams) {
        if (stream.id == stream_id) {
            stream_config = stream;
            found = true;
            break;
        }
    }
    if (!found) {
        LOG_ERROR("Schedule: Stream {} not found in config", stream_id);
        return;
    }

    std::lock_guard<std::mutex> rec_lock(m_recorders_mutex);

    auto it = m_recorders.find(stream_id);
    if (it != m_recorders.end() && it->second) {
        // 录制器已存在，检查状态
        // 由于 IPCRecorder 没有 getStatus 方法，我们假设需要重新创建
        it->second->stop();
        m_recorders.erase(it);
    }

    // 查找流的配置（包括智能录制配置）
    const SmartRecordingConfig* smart_recording_config = nullptr;
    for (const auto& stream : m_config.streams) {
        if (stream.id == stream_id) {
            if (stream.smart_recording.enabled) {
                smart_recording_config = &stream.smart_recording;
                LOG_INFO("Smart recording enabled for stream: {}", stream_id);
            }
            break;
        }
    }

    // 创建新的录制器
    auto recorder = std::make_unique<IPCRecorder>(
        stream_config.id, stream_config.url,
        m_config.record.output_dir,
        m_config.record.temp_dir,
        m_config.record.segment_duration_seconds,
        m_config.record.filename_template,
        m_config.record.enable_audio,
        stream_config.auto_reconnect,
        stream_config.reconnect_interval_seconds,
        stream_config.max_reconnect_attempts,
        stream_config.timeout_seconds,
        m_config.shop.id,
        stream_config.name,
        m_config.record.min_segment_duration_seconds
#ifdef ENABLE_RKNN_SMART_RECORDING
        , smart_recording_config
        , m_detection_pool.get()
#endif
    );
    recorder->start();

    m_recorders[stream_id] = std::move(recorder);
    LOG_INFO("Schedule: Started recording for stream {}", stream_id);
}

void NVRManager::stopStreamRecording(const std::string& stream_id) {
    std::lock_guard<std::mutex> rec_lock(m_recorders_mutex);

    auto it = m_recorders.find(stream_id);
    if (it != m_recorders.end() && it->second) {
        it->second->stop();
        // 注意：保留录制器对象以便后续恢复，但需要释放 FFmpeg 资源
        // 所以这里选择 erase 来完全清理
        m_recorders.erase(it);
        LOG_INFO("Schedule: Stopped recording for stream {}", stream_id);
    }
}

// ==================== 事件提取相关方法 ====================

void NVRManager::extractionLoop() {
    LOG_INFO("Event extraction loop started");

    while (m_running) {
        try {
            scanAndExtractRawVideos();
        } catch (const std::exception& e) {
            LOG_ERROR("Extraction scan error: {}", e.what());
        }

        std::unique_lock<std::mutex> lock(m_extraction_mutex);
        // 定期扫描已完成的 raw 视频
        if (m_extraction_cv.wait_for(lock, std::chrono::seconds(m_config.record.extraction_scan_interval_seconds),
                                    [this] { return !m_running; })) {
            break;  // 收到停止信号
        }
    }

    LOG_INFO("Event extraction loop stopped");
}

void NVRManager::scanAndExtractRawVideos() {
    if (!m_config.record.enable_extraction) {
        return;
    }

    fs::path raw_dir = fs::path(m_config.record.output_dir) / m_config.record.raw_subdir;

    // 检查 raw 目录是否存在
    if (!fs::exists(raw_dir)) {
        return;
    }

    // 扫描 raw 目录中的 MP4 文件
    int found_count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(raw_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mp4") {
            continue;
        }

        found_count++;
        LOG_TRACE("Found raw video: {}", entry.path().string());

        // 检查是否应该提取该视频
        if (!shouldExtractVideo(entry.path())) {
            LOG_TRACE("Skipping video (shouldExtractVideo=false): {}", entry.path().string());
            continue;
        }

        // 检查对应的 CSV 检测日志是否存在
        fs::path csv_file = entry.path();
        csv_file.replace_extension(".csv");
        if (!fs::exists(csv_file)) {
            LOG_DEBUG("Detection log not found for {}, skipping", entry.path().string());
            continue;
        }

        LOG_INFO("Processing raw video for extraction: {}", entry.path().string());

        try {
            // 创建提取占位符文件（raw 目录下），防止重复提取
            fs::path placeholder = entry.path();
            placeholder.replace_extension(".extracting");
            std::ofstream(placeholder.string()) << "Extracting...";

            LOG_DEBUG("Created extraction placeholder: {}", placeholder.string());

            // 创建视频片段提取器
            VideoSegmentExtractor extractor(m_config);
            extractor.processRawVideo(entry.path(), csv_file);

            // 提取完成，删除占位符，创建完成标记
            fs::remove(placeholder);
            fs::path marker = entry.path();
            marker.replace_extension(".extracted");
            std::ofstream(marker.string()) << "Done";

            // 根据配置决定是否删除原始视频
            if (m_config.record.delete_raw_after_extraction) {
                std::error_code ec;
                fs::remove(entry.path(), ec);
                if (ec) {
                    LOG_WARN("Failed to delete raw video {}: {}", entry.path().string(), ec.message());
                } else {
                    LOG_INFO("Deleted raw video: {}", entry.path().string());
                }
                fs::remove(csv_file, ec);
                fs::remove(marker, ec);
            }

            LOG_INFO("Successfully processed raw video: {}", entry.path().string());
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to extract from {}: {}", entry.path().string(), e.what());

            // 失败时也要删除占位符文件
            fs::path placeholder = entry.path();
            placeholder.replace_extension(".extracting");
            fs::remove(placeholder);
        }
    }

    if (found_count == 0) {
        LOG_DEBUG("Extraction scan: no MP4 files found in {}", raw_dir.string());
    } else {
        LOG_DEBUG("Extraction scan: found {} MP4 files in {}", found_count, raw_dir.string());
    }
}

bool NVRManager::shouldExtractVideo(const std::filesystem::path& raw_video) {
    // 检查 CSV 文件是否存在（视频已完成录制和检测）
    fs::path csv_file = raw_video;
    csv_file.replace_extension(".csv");
    if (!fs::exists(csv_file)) {
        LOG_TRACE("shouldExtractVideo: CSV file not found: {}", csv_file.string());
        return false;  // 视频还在录制或检测未完成
    }

    // 额外检查：确保CSV文件不是临时文件（temp_*.csv）
    if (csv_file.filename().string().find("temp_") != std::string::npos) {
        LOG_TRACE("shouldExtractVideo: CSV file is temporary, skipping: {}", csv_file.string());
        return false;
    }

    // 检查 .extracting 占位符（正在提取中）
    fs::path placeholder = raw_video;
    placeholder.replace_extension(".extracting");
    if (fs::exists(placeholder)) {
        LOG_TRACE("shouldExtractVideo: Extracting placeholder exists: {}", placeholder.string());
        return false;
    }

    // 检查 .extracted 标记文件（已提取完成）
    fs::path marker = raw_video;
    marker.replace_extension(".extracted");
    if (fs::exists(marker)) {
        LOG_TRACE("shouldExtractVideo: Extracted marker exists: {}", marker.string());
        return false;
    }

    LOG_TRACE("shouldExtractVideo: returning TRUE for {}", raw_video.string());
    return true;
}

// ==================== 清理优化辅助函数 ====================

// 从文件名解析录制时间
std::optional<std::chrono::system_clock::time_point>
parseRecordingTimeFromFilename(const fs::path& file_path) {
    std::string filename = file_path.stem().string();  // 去扩展名

    // 示例文件名: camera1_20260528_001407_001417.mp4
    // 使用正则表达式提取时间戳
    std::regex time_regex(R"(\d{8}_\d{6})");
    std::smatch match;

    if (std::regex_search(filename, match, time_regex)) {
        std::string datetime_str = match[0].str();  // "20260528_001407"

        // 解析为时间点
        std::tm tm = {};
        std::istringstream ss(datetime_str);
        ss >> std::get_time(&tm, "%Y%m%d_%H%M%S");
        if (ss.fail()) {
            // 解析失败，返回 nullopt
            return std::nullopt;
        }

        std::time_t time = std::mktime(&tm);
        if (time == -1) {
            return std::nullopt;
        }

        return std::chrono::system_clock::from_time_t(time);
    }

    return std::nullopt;
}

// 按文件名中的录制时间排序
bool compareByRecordingTime(const fs::path& a, const fs::path& b) {
    auto time_a = parseRecordingTimeFromFilename(a);
    auto time_b = parseRecordingTimeFromFilename(b);

    if (time_a && time_b) {
        return *time_a < *time_b;
    } else if (time_a) {
        return true;  // 有时间的排在前面
    } else if (time_b) {
        return false;
    } else {
        // 都没有时间，使用文件修改时间
        return fs::last_write_time(a) < fs::last_write_time(b);
    }
}
