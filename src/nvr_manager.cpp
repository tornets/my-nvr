//
// Created by wention on 2026/1/27.
//

#include "nvr_manager.h"

#include <chrono>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

NVRManager::NVRManager(const Config& config)
    : m_config(config)
    , m_running(false)
{
    fs::create_directories(m_config.record.output_dir);
    m_logger = spdlog::get("manager");
    if (!m_logger) {
        m_logger = spdlog::default_logger()->clone("manager");
    }

    m_logger->info("NVRManager initialized with output_dir: {}", m_config.record.output_dir);
}

NVRManager::~NVRManager() {
    stopAll();
}

bool NVRManager::addStream(const std::string& stream_id, const std::string& stream_url) {
    std::lock_guard<std::mutex> lock(m_recorders_mutex);

    if (m_recorders.find(stream_id) != m_recorders.end()) {
        m_logger->warn("Stream {} already exists", stream_id);
        return false;
    }

    auto recorder = std::make_unique<IPCRecorder>(stream_url, m_config.record.output_dir, m_config.record.segment_duration_seconds);
    recorder->start();

    m_recorders[stream_id] = std::move(recorder);
    m_logger->info("Added stream: {} -> {}", stream_id, stream_url);

    if (!m_running) {
        m_running = true;

        // 如果启用了自动清理，启动清理线程
        if (m_config.autoclean.enabled) {
            m_cleanup_thread = std::thread(&NVRManager::cleanupLoop, this);
        }

        // 如果配置了上传器且启用了上传，启动上传扫描线程
        if (m_uploader && m_uploader->isEnabled()) {
            m_upload_thread = std::thread(&NVRManager::uploadLoop, this);
        }
    }

    return true;
}

bool NVRManager::removeStream(const std::string& stream_id) {
    std::lock_guard<std::mutex> lock(m_recorders_mutex);

    auto it = m_recorders.find(stream_id);
    if (it == m_recorders.end()) {
        m_logger->warn("Stream {} not found", stream_id);
        return false;
    }

    it->second->stop();
    m_recorders.erase(it);
    m_logger->info("Removed stream: {}", stream_id);

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
}

void NVRManager::setUploader(std::shared_ptr<VideoUploader> uploader) {
    m_uploader = uploader;
    if (m_uploader && m_uploader->isEnabled()) {
        m_logger->info("Video uploader configured");
    }
}

void NVRManager::cleanupLoop() {
    m_logger->info("Cleanup loop started");

    while (m_running) {
        std::unique_lock<std::mutex> lock(m_cleanup_mutex);
        // 等待指定时间或直到收到停止信号
        if (m_cleanup_cv.wait_for(lock, std::chrono::seconds(m_config.autoclean.check_interval_seconds),
                                 [this] { return !m_running; })) {
            break;  // 收到停止信号
        }

        if (!m_running) {
            break;
        }

        try {
            cleanOldFiles();
            checkDiskUsage();
        } catch (const std::exception& e) {
            m_logger->error("Cleanup error: {}", e.what());
        }
    }

    m_logger->info("Cleanup loop stopped");
}

void NVRManager::uploadLoop() {
    m_logger->info("Upload scan loop started");

    while (m_running) {
        std::unique_lock<std::mutex> lock(m_upload_mutex);
        // 每10秒扫描一次新文件
        if (m_upload_cv.wait_for(lock, std::chrono::seconds(10),
                                [this] { return !m_running; })) {
            break;  // 收到停止信号
        }

        if (!m_running) {
            break;
        }

        try {
            scanAndUploadNewFiles();
        } catch (const std::exception& e) {
            m_logger->error("Upload scan error: {}", e.what());
        }
    }

    m_logger->info("Upload scan loop stopped");
}

void NVRManager::scanAndUploadNewFiles() {
    if (!m_uploader || !m_uploader->isEnabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_uploaded_files_mutex);

    for (const auto& entry : fs::directory_iterator(m_config.record.output_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mp4") {
            continue;
        }

        std::string file_path = entry.path().string();
        std::string filename = entry.path().filename().string();

        // 检查是否已上传
        if (m_uploaded_files.find(filename) != m_uploaded_files.end()) {
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
        std::string time_str = std::ctime(&time);
        if (!time_str.empty()) {
            time_str.pop_back();  // 移除末尾的换行符
        }

        // 创建上传任务
        UploadTask task;
        task.file_path = file_path;
        task.stream_id = stream_id;
        task.recording_time = time_str;

        m_logger->info("Found new file to upload: {} (stream: {})", filename, stream_id);
        m_uploader->enqueue(task);

        // 标记为已加入上传队列
        m_uploaded_files.insert(filename);
    }
}

bool NVRManager::cleanOldFiles() {
    int max_age_seconds = m_config.autoclean.max_age_hours * 3600;
    auto now = fs::file_time_type::clock::now();
    int deleted_count = 0;

    for (const auto& entry : fs::directory_iterator(m_config.record.output_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
            auto ftime = entry.last_write_time();
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - ftime).count();

            if (age > max_age_seconds) {
                std::error_code ec;
                if (fs::remove(entry.path(), ec)) {
                    m_logger->info("Deleted old file: {} (age: {}h)", entry.path().filename().string(),
                                  age / 3600.0);
                    deleted_count++;
                } else {
                    m_logger->error("Failed to delete {}: {}", entry.path().string(), ec.message());
                }
            }
        }
    }

    if (deleted_count > 0) {
        m_logger->info("Cleaned up {} old files", deleted_count);
    }

    return true;
}

bool NVRManager::checkDiskUsage() {
    if (m_config.autoclean.max_disk_usage_gb <= 0) {
        return true;
    }

    std::uintmax_t total_size = 0;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(m_config.record.output_dir)) {
            if (entry.is_regular_file()) {
                total_size += entry.file_size();
            }
        }
    } catch (const std::exception& e) {
        m_logger->error("Error calculating disk usage: {}", e.what());
        return false;
    }

    double total_gb = static_cast<double>(total_size) / (1024.0 * 1024.0 * 1024.0);

    if (total_gb > m_config.autoclean.max_disk_usage_gb) {
        m_logger->warn("Disk usage {:.2f} GB exceeds limit {} GB", total_gb, m_config.autoclean.max_disk_usage_gb);

        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(m_config.record.output_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
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
            if (fs::remove(files.front(), ec)) {
                total_size -= fs::file_size(files.front());
                total_gb = static_cast<double>(total_size) / (1024.0 * 1024.0 * 1024.0);
                m_logger->info("Deleted file to free space: {}", files.front().filename().string());
                deleted++;
            }
            files.erase(files.begin());
        }

        m_logger->info("Deleted {} files to reduce disk usage", deleted);
    }

    return true;
}
