//
// Created by wention on 2026/1/27.
//

#ifndef NVR_NVR_MANAGER_H
#define NVR_NVR_MANAGER_H

#include <string>
#include <unordered_map>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

#include <spdlog/spdlog.h>

#include "ipc_recorder.h"

struct CleanerConfig {
    int max_age_hours;
    int max_disk_usage_gb;
    int check_interval_seconds;
};

class NVRManager {
public:
    NVRManager(const std::string& output_dir, const CleanerConfig& cleaner_config);
    ~NVRManager();

    bool addStream(const std::string& stream_id, const std::string& stream_url);
    bool removeStream(const std::string& stream_id);
    void stopAll();

private:
    void cleanupLoop();
    bool cleanOldFiles();
    bool checkDiskUsage();

    std::string m_output_dir;
    CleanerConfig m_cleaner_config;

    std::unordered_map<std::string, std::unique_ptr<IPCRecorder>> m_recorders;
    std::mutex m_recorders_mutex;

    std::thread m_cleanup_thread;
    std::atomic<bool> m_running;

    std::shared_ptr<spdlog::logger> m_logger;
};

#endif //NVR_NVR_MANAGER_H
