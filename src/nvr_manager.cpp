//
// Created by wention on 2026/1/27.
//

#include "nvr_manager.h"

#include <chrono>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

NVRManager::NVRManager(const std::string& output_dir, const CleanerConfig& cleaner_config)
    : m_output_dir(output_dir)
    , m_cleaner_config(cleaner_config)
    , m_running(false)
{
    fs::create_directories(m_output_dir);
    m_logger = spdlog::get("manager");
    if (!m_logger) {
        m_logger = spdlog::default_logger()->clone("manager");
    }

    m_logger->info("NVRManager initialized with output_dir: {}", output_dir);
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

    auto recorder = std::make_unique<IPCRecorder>(stream_url, m_output_dir);
    recorder->start();

    m_recorders[stream_id] = std::move(recorder);
    m_logger->info("Added stream: {} -> {}", stream_id, stream_url);

    if (!m_running) {
        m_running = true;
        m_cleanup_thread = std::thread(&NVRManager::cleanupLoop, this);
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
        if (m_cleanup_thread.joinable()) {
            m_cleanup_thread.join();
        }
    }
}

void NVRManager::cleanupLoop() {
    m_logger->info("Cleanup loop started");

    while (m_running) {
        std::this_thread::sleep_for(std::chrono::seconds(m_cleaner_config.check_interval_seconds));

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

bool NVRManager::cleanOldFiles() {
    int max_age_seconds = m_cleaner_config.max_age_hours * 3600;
    auto now = fs::file_time_type::clock::now();
    int deleted_count = 0;

    for (const auto& entry : fs::directory_iterator(m_output_dir)) {
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
    if (m_cleaner_config.max_disk_usage_gb <= 0) {
        return true;
    }

    std::uintmax_t total_size = 0;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(m_output_dir)) {
            if (entry.is_regular_file()) {
                total_size += entry.file_size();
            }
        }
    } catch (const std::exception& e) {
        m_logger->error("Error calculating disk usage: {}", e.what());
        return false;
    }

    double total_gb = static_cast<double>(total_size) / (1024.0 * 1024.0 * 1024.0);

    if (total_gb > m_cleaner_config.max_disk_usage_gb) {
        m_logger->warn("Disk usage {:.2f} GB exceeds limit {} GB", total_gb, m_cleaner_config.max_disk_usage_gb);

        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(m_output_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
                files.push_back(entry.path());
            }
        }

        std::sort(files.begin(), files.end(),
                 [](const fs::path& a, const fs::path& b) {
                     return fs::last_write_time(a) < fs::last_write_time(b);
                 });

        int deleted = 0;
        while (total_gb > m_cleaner_config.max_disk_usage_gb * 0.9 && !files.empty()) {
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
