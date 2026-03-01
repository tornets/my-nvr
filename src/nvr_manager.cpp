//
// Created by wention on 2026/1/27.
//

#include "log.h"
#include "nvr_manager.h"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

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

    // 启动后台服务线程
    m_running = true;

    // 如果启用了自动清理，启动清理线程
    if (m_config.autoclean.enabled) {
        m_cleanup_thread = std::thread(&NVRManager::cleanupLoop, this);
        LOG_INFO("Cleanup thread started");
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
        return false;
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
                                                      stream_name);
    recorder->start();

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

    if (m_running) {
        m_running = false;

        // 通知所有等待的线程
        m_cleanup_cv.notify_all();
        m_upload_cv.notify_all();

        // 只在线程实际启动时 join
        if (m_cleanup_thread.joinable()) {
            m_cleanup_thread.join();
        }
        if (m_upload_thread.joinable()) {
            m_upload_thread.join();
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

    // 获取临时目录的规范路径，用于比较
    fs::path temp_dir_path;
    try {
        temp_dir_path = fs::canonical(m_config.record.temp_dir);
    } catch (...) {
        temp_dir_path = m_config.record.temp_dir;
    }

    // 使用递归扫描，因为文件名模板可能包含多级目录
    for (const auto& entry : fs::recursive_directory_iterator(m_config.record.output_dir)) {
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
        fs::path relative_path = fs::relative(entry.path(), m_config.record.output_dir);
        std::string relative_key = relative_path.string();

        // 使用进度管理器检查是否已上传
        if (m_upload_progress && m_upload_progress->isUploaded(relative_key)) {
            LOG_TRACE("Skipping already uploaded file: {}", relative_key);
            continue;
        }

        // 检查是否正在上传或已入队
        if (m_upload_progress && m_upload_progress->isPendingOrUploading(relative_key)) {
            LOG_TRACE("Skipping file already in upload queue: {}", relative_key);
            continue;
        }

        std::string file_path_str = entry.path().string();
        std::string filename = entry.path().filename().string();

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
    auto now = fs::file_time_type::clock::now();
    int deleted_count = 0;

    // 获取临时目录的规范路径，用于比较
    fs::path temp_dir_path;
    try {
        temp_dir_path = fs::canonical(m_config.record.temp_dir);
    } catch (...) {
        temp_dir_path = m_config.record.temp_dir;
    }

    for (const auto& entry : fs::recursive_directory_iterator(m_config.record.output_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mp4") {
            continue;
        }

        // 跳过临时目录中的文件
        fs::path file_parent = entry.path().parent_path();
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

        auto ftime = entry.last_write_time();
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - ftime).count();

        if (age > max_age_seconds) {
            std::error_code ec;
            if (fs::remove(entry.path(), ec)) {
                LOG_INFO("Deleted old file: {} (age: {}h)", entry.path().string(),
                              age / 3600.0);
                deleted_count++;
            } else {
                LOG_ERROR("Failed to delete {}: {}", entry.path().string(), ec.message());
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
